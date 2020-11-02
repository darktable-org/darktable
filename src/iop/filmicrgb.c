/*
   This file is part of darktable,
   Copyright (C) 2019-2020 darktable developers.

   darktable is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   darktable is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "bauhaus/bauhaus.h"
#include "common/colorspaces_inline_conversions.h"
#include "common/darktable.h"
#include "common/image.h"
#include "common/iop_profile.h"
#include "common/opencl.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop_gui.h"
#include "develop/imageop_math.h"
#include "dtgtk/button.h"
#include "dtgtk/drawingarea.h"
#include "dtgtk/expander.h"
#include "dtgtk/paint.h"
#include "gui/accelerators.h"
#include "gui/color_picker_proxy.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "iop/gaussian_elimination.h"
#include "iop/iop_api.h"


#include "develop/imageop.h"
#include "gui/draw.h"

#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>


#define NORM_MIN 1.52587890625e-05f // norm can't be < to 2^(-16)

#define DT_GUI_CURVE_EDITOR_INSET DT_PIXEL_APPLY_DPI(1)


DT_MODULE_INTROSPECTION(4, dt_iop_filmicrgb_params_t)

/**
 * DOCUMENTATION
 *
 * This code ports :
 * 1. Troy Sobotka's filmic curves for Blender (and other softs)
 *      https://github.com/sobotka/OpenAgX/blob/master/lib/agx_colour.py
 * 2. ACES camera logarithmic encoding
 *        https://github.com/ampas/aces-dev/blob/master/transforms/ctl/utilities/ACESutil.Lin_to_Log2_param.ctl
 *
 * The ACES log implementation is taken from the profile_gamma.c IOP
 * where it works in camera RGB space. Here, it works on an arbitrary RGB
 * space. ProPhotoRGB has been chosen for its wide gamut coverage and
 * for conveniency because it's already in darktable's libs. Any other
 * RGB working space could work. This chouice could (should) also be
 * exposed to the user.
 *
 * The filmic curves are tonecurves intended to simulate the luminance
 * transfer function of film with "S" curves. These could be reproduced in
 * the tonecurve.c IOP, however what we offer here is a parametric
 * interface useful to remap accurately and promptly the middle grey
 * to any arbitrary value chosen accordingly to the destination space.
 *
 * The combined use of both define a modern way to deal with large
 * dynamic range photographs by remapping the values with a comprehensive
 * interface avoiding many of the back and forth adjustments darktable
 * is prone to enforce.
 *
 * */


/** Note :
 * we use finite-math-only and fast-math because divisions by zero are manually avoided in the code
 * fp-contract=fast enables hardware-accelerated Fused Multiply-Add
 * the rest is loop reorganization and vectorization optimization
 **/
#if defined(__GNUC__)
#pragma GCC optimize("unroll-loops", "tree-loop-if-convert", "tree-loop-distribution", "no-strict-aliasing",      \
                     "loop-interchange", "loop-nest-optimize", "tree-loop-im", "unswitch-loops",                  \
                     "tree-loop-ivcanon", "ira-loop-pressure", "split-ivs-in-unroller",                           \
                     "variable-expansion-in-unroller", "split-loops", "ivopts", "predictive-commoning",           \
                     "tree-loop-linear", "loop-block", "loop-strip-mine", "finite-math-only", "fp-contract=fast", \
                     "fast-math", "no-math-errno")
#endif

typedef enum dt_iop_filmicrgb_methods_type_t
{
  DT_FILMIC_METHOD_NONE = 0,          // $DESCRIPTION: "no"
  DT_FILMIC_METHOD_MAX_RGB = 1,       // $DESCRIPTION: "max RGB"
  DT_FILMIC_METHOD_LUMINANCE = 2,     // $DESCRIPTION: "luminance Y"
  DT_FILMIC_METHOD_POWER_NORM = 3,    // $DESCRIPTION: "RGB power norm"
  DT_FILMIC_METHOD_EUCLIDEAN_NORM = 4 // $DESCRIPTION: "RGB euclidean norm"
} dt_iop_filmicrgb_methods_type_t;


typedef enum dt_iop_filmicrgb_curve_type_t
{
  DT_FILMIC_CURVE_POLY_4 = 0, // $DESCRIPTION: "hard"
  DT_FILMIC_CURVE_POLY_3 = 1  // $DESCRIPTION: "soft"
} dt_iop_filmicrgb_curve_type_t;


typedef enum dt_iop_filmicrgb_colorscience_type_t
{
  DT_FILMIC_COLORSCIENCE_V1 = 0, // $DESCRIPTION: "v3 (2019)"
  DT_FILMIC_COLORSCIENCE_V2 = 1, // $DESCRIPTION: "v4 (2020)"
} dt_iop_filmicrgb_colorscience_type_t;


typedef enum dt_iop_filmicrgb_reconstruction_type_t
{
  DT_FILMIC_RECONSTRUCT_RGB = 0,
  DT_FILMIC_RECONSTRUCT_RATIOS = 1,
} dt_iop_filmicrgb_reconstruction_type_t;


typedef struct dt_iop_filmic_rgb_spline_t
{
  float DT_ALIGNED_PIXEL M1[4], M2[4], M3[4], M4[4], M5[4]; // factors for the interpolation polynom
  float latitude_min, latitude_max;                         // bounds of the latitude == linear part by design
  float y[5];                                               // controls nodes
  float x[5];                                               // controls nodes
} dt_iop_filmic_rgb_spline_t;


typedef enum dt_iop_filmic_rgb_gui_mode_t
{
  DT_FILMIC_GUI_LOOK = 0,      // default GUI, showing only the contrast curve in a log/gamma space
  DT_FILMIC_GUI_BASECURVE = 1, // basecurve-like GUI, showing the contrast and brightness curves, in lin/lin space
  DT_FILMIC_GUI_BASECURVE_LOG = 2, // same as previous, but log-scaled
  DT_FILMIC_GUI_RANGES = 3,        // zone-system-like GUI, showing the range to range mapping
  DT_FILMIC_GUI_LAST
} dt_iop_filmic_rgb_gui_mode_t;

// clang-format off
typedef struct dt_iop_filmicrgb_params_t
{
  float grey_point_source;     // $MIN: 0 $MAX: 100 $DEFAULT: 18.45 $DESCRIPTION: "middle grey luminance"
  float black_point_source;    // $MIN: -16 $MAX: -0.1 $DEFAULT: -8.0 $DESCRIPTION: "black relative exposure"
  float white_point_source;    // $MIN: 0 $MAX: 16 $DEFAULT: 4.0 $DESCRIPTION: "white relative exposure"
  float reconstruct_threshold; // $MIN: -6.0 $MAX: 6.0 $DEFAULT: +3.0 $DESCRIPTION: "threshold"
  float reconstruct_feather;   // $MIN: 0.25 $MAX: 6.0 $DEFAULT: 3.0 $DESCRIPTION: "transition"
  float reconstruct_bloom_vs_details; // $MIN: -100.0 $MAX: 100.0 $DEFAULT: 100.0 $DESCRIPTION: "bloom/reconstruct"
  float reconstruct_grey_vs_color; // $MIN: -100.0 $MAX: 100.0 $DEFAULT: 100.0 $DESCRIPTION: "grey/colorful details"
  float reconstruct_structure_vs_texture; // $MIN: -100.0 $MAX: 100.0 $DEFAULT: 0.0 $DESCRIPTION: "structure/texture"
  float security_factor;                  // $MIN: -50 $MAX: 200 $DEFAULT: 0 $DESCRIPTION: "dynamic range scaling"
  float grey_point_target;                // $MIN: 1 $MAX: 50 $DEFAULT: 18.45 $DESCRIPTION: "target middle grey"
  float black_point_target; // $MIN: 0.000 $MAX: 20.000 $DEFAULT: 0.01517634 $DESCRIPTION: "target black luminance"
  float white_point_target; // $MIN: 0 $MAX: 1600 $DEFAULT: 100 $DESCRIPTION: "target white luminance"
  float output_power;       // $MIN: 1 $MAX: 10 $DEFAULT: 4.0 $DESCRIPTION: "hardness"
  float latitude;           // $MIN: 0.01 $MAX: 100 $DEFAULT: 25.0
  float contrast;           // $MIN: 0 $MAX: 5 $DEFAULT: 1.35
  float saturation;         // $MIN: -50 $MAX: 200 $DEFAULT: 10 $DESCRIPTION: "extreme luminance saturation"
  float balance;            // $MIN: -50 $MAX: 50 $DEFAULT: 0.0 $DESCRIPTION: "shadows/highlights balance"
  float noise_level;        // $MIN: 0.0 $MAX: 6.0 $DEFAULT: 0.1f $DESCRIPTION: "add noise in highlights"
  dt_iop_filmicrgb_methods_type_t preserve_color; // $DEFAULT: DT_FILMIC_METHOD_POWER_NORM $DESCRIPTION: "preserve chrominance"
  dt_iop_filmicrgb_colorscience_type_t version; // $DEFAULT: DT_FILMIC_COLORSCIENCE_V2 $DESCRIPTION: "color science"
  gboolean auto_hardness;                       // $DEFAULT: TRUE $DESCRIPTION: "auto adjust hardness"
  gboolean custom_grey;                         // $DEFAULT: FALSE $DESCRIPTION: "use custom middle-grey values"
  int high_quality_reconstruction;       // $MIN: 0 $MAX: 10 $DEFAULT: 1 $DESCRIPTION: "iterations of high-quality reconstruction"
  int noise_distribution;                // $DEFAULT: DT_NOISE_POISSONIAN $DESCRIPTION: "type of noise"
  dt_iop_filmicrgb_curve_type_t shadows; // $DEFAULT: DT_FILMIC_CURVE_POLY_4 $DESCRIPTION: "contrast in shadows"
  dt_iop_filmicrgb_curve_type_t highlights; // $DEFAULT: DT_FILMIC_CURVE_POLY_4 $DESCRIPTION: "contrast in highlights"
  gboolean compensate_icc_black; // $DEFAULT: FALSE $DESCRIPTION: "compensate output ICC profile black point"
  gint internal_version;         // $DEFAULT: 2020 $DESCRIPTION: "version of the spline generator"
} dt_iop_filmicrgb_params_t;
// clang-format on


// custom buttons in graph views
typedef enum dt_iop_filmicrgb_gui_button_t
{
  DT_FILMIC_GUI_BUTTON_TYPE = 0,
  DT_FILMIC_GUI_BUTTON_LABELS = 1,
  DT_FILMIC_GUI_BUTTON_LAST
} dt_iop_filmicrgb_gui_button_t;

// custom buttons in graph views - data
typedef struct dt_iop_filmicrgb_gui_button_data_t
{
  // coordinates in GUI - compute them only in the drawing function
  float left;
  float right;
  float top;
  float bottom;
  float w;
  float h;

  // properties
  gint mouse_hover; // whether it should be acted on / mouse is over it
  GtkStateFlags state;

  // icon drawing, function as set in dtgtk/paint.h
  DTGTKCairoPaintIconFunc icon;

} dt_iop_filmicrgb_gui_button_data_t;


typedef struct dt_iop_filmicrgb_gui_data_t
{
  GtkWidget *white_point_source;
  GtkWidget *grey_point_source;
  GtkWidget *black_point_source;
  GtkWidget *reconstruct_threshold, *reconstruct_bloom_vs_details, *reconstruct_grey_vs_color,
      *reconstruct_structure_vs_texture, *reconstruct_feather;
  GtkWidget *show_highlight_mask;
  GtkWidget *security_factor;
  GtkWidget *auto_button;
  GtkWidget *grey_point_target;
  GtkWidget *white_point_target;
  GtkWidget *black_point_target;
  GtkWidget *output_power;
  GtkWidget *latitude;
  GtkWidget *contrast;
  GtkWidget *saturation;
  GtkWidget *balance;
  GtkWidget *preserve_color;
  GtkWidget *autoset_display_gamma;
  GtkWidget *shadows, *highlights;
  GtkWidget *version;
  GtkWidget *auto_hardness;
  GtkWidget *custom_grey;
  GtkWidget *high_quality_reconstruction;
  GtkWidget *noise_level, *noise_distribution;
  GtkWidget *compensate_icc_black;
  GtkNotebook *notebook;
  GtkDrawingArea *area;
  struct dt_iop_filmic_rgb_spline_t spline DT_ALIGNED_ARRAY;
  gint show_mask;
  dt_iop_filmic_rgb_gui_mode_t gui_mode; // graph display mode
  gint gui_show_labels;
  gint gui_hover;
  gint gui_sizes_inited;
  dt_iop_filmicrgb_gui_button_t active_button; // ID of the button under cursor
  dt_iop_filmicrgb_gui_button_data_t buttons[DT_FILMIC_GUI_BUTTON_LAST];

  // Cache Pango and Cairo stuff for the equalizer drawing
  float line_height;
  float sign_width;
  float zero_width;
  float graph_width;
  float graph_height;
  int inset;
  int inner_padding;

  GtkAllocation allocation;
  cairo_surface_t *cst;
  cairo_t *cr;
  PangoLayout *layout;
  PangoRectangle ink;
  PangoFontDescription *desc;
  GtkStyleContext *context;
} dt_iop_filmicrgb_gui_data_t;

typedef struct dt_iop_filmicrgb_data_t
{
  float max_grad;
  float white_source;
  float grey_source;
  float black_source;
  float reconstruct_threshold;
  float reconstruct_feather;
  float reconstruct_bloom_vs_details;
  float reconstruct_grey_vs_color;
  float reconstruct_structure_vs_texture;
  float dynamic_range;
  float saturation;
  float output_power;
  float contrast;
  float sigma_toe, sigma_shoulder;
  float noise_level;
  int preserve_color;
  int version;
  int high_quality_reconstruction;
  struct dt_iop_filmic_rgb_spline_t spline DT_ALIGNED_ARRAY;
  dt_noise_distribution_t noise_distribution;
} dt_iop_filmicrgb_data_t;


typedef struct dt_iop_filmicrgb_global_data_t
{
  int kernel_filmic_rgb_split;
  int kernel_filmic_rgb_chroma;
} dt_iop_filmicrgb_global_data_t;


const char *name()
{
  return _("filmic rgb");
}

int default_group()
{
  return IOP_GROUP_TONE | IOP_GROUP_TECHNICAL;
}

int flags()
{
  return IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return iop_cs_rgb;
}

int legacy_params(dt_iop_module_t *self, const void *const old_params, const int old_version, void *new_params,
                  const int new_version)
{
  if(old_version == 1 && new_version == 4)
  {
    typedef struct dt_iop_filmicrgb_params_v1_t
    {
      float grey_point_source;
      float black_point_source;
      float white_point_source;
      float security_factor;
      float grey_point_target;
      float black_point_target;
      float white_point_target;
      float output_power;
      float latitude;
      float contrast;
      float saturation;
      float balance;
      int preserve_color;
    } dt_iop_filmicrgb_params_v1_t;

    dt_iop_filmicrgb_params_v1_t *o = (dt_iop_filmicrgb_params_v1_t *)old_params;
    dt_iop_filmicrgb_params_t *n = (dt_iop_filmicrgb_params_t *)new_params;
    dt_iop_filmicrgb_params_t *d = (dt_iop_filmicrgb_params_t *)self->default_params;

    *n = *d; // start with a fresh copy of default parameters

    n->grey_point_source = o->grey_point_source;
    n->white_point_source = o->white_point_source;
    n->black_point_source = o->black_point_source;
    n->security_factor = o->security_factor;
    n->grey_point_target = o->grey_point_target;
    n->black_point_target = o->black_point_target;
    n->white_point_target = o->white_point_target;
    n->output_power = o->output_power;
    n->latitude = o->latitude;
    n->contrast = o->contrast;
    n->saturation = o->saturation;
    n->balance = o->balance;
    n->preserve_color = o->preserve_color;
    n->shadows = DT_FILMIC_CURVE_POLY_4;
    n->highlights = DT_FILMIC_CURVE_POLY_3;
    n->reconstruct_threshold
        = 6.0f; // for old edits, this ensures clipping threshold >> white level, so it's a no-op
    n->reconstruct_bloom_vs_details = d->reconstruct_bloom_vs_details;
    n->reconstruct_grey_vs_color = d->reconstruct_grey_vs_color;
    n->reconstruct_structure_vs_texture = d->reconstruct_structure_vs_texture;
    n->reconstruct_feather = 3.0f;
    n->version = DT_FILMIC_COLORSCIENCE_V1;
    n->auto_hardness = TRUE;
    n->custom_grey = TRUE;
    n->high_quality_reconstruction = 0;
    n->noise_distribution = d->noise_distribution;
    n->noise_level = 0.f;
    n->internal_version = 2019;
    n->compensate_icc_black = FALSE;
    return 0;
  }
  if(old_version == 2 && new_version == 4)
  {
    typedef struct dt_iop_filmicrgb_params_v2_t
    {
      float grey_point_source;
      float black_point_source;
      float white_point_source;
      float reconstruct_threshold;
      float reconstruct_feather;
      float reconstruct_bloom_vs_details;
      float reconstruct_grey_vs_color;
      float reconstruct_structure_vs_texture;
      float security_factor;
      float grey_point_target;
      float black_point_target;
      float white_point_target;
      float output_power;
      float latitude;
      float contrast;
      float saturation;
      float balance;
      int preserve_color;
      int version;
      int auto_hardness;
      int custom_grey;
      int high_quality_reconstruction;
      dt_iop_filmicrgb_curve_type_t shadows;
      dt_iop_filmicrgb_curve_type_t highlights;
    } dt_iop_filmicrgb_params_v2_t;

    dt_iop_filmicrgb_params_v2_t *o = (dt_iop_filmicrgb_params_v2_t *)old_params;
    dt_iop_filmicrgb_params_t *n = (dt_iop_filmicrgb_params_t *)new_params;
    dt_iop_filmicrgb_params_t *d = (dt_iop_filmicrgb_params_t *)self->default_params;

    *n = *d; // start with a fresh copy of default parameters

    n->grey_point_source = o->grey_point_source;
    n->white_point_source = o->white_point_source;
    n->black_point_source = o->black_point_source;
    n->security_factor = o->security_factor;
    n->grey_point_target = o->grey_point_target;
    n->black_point_target = o->black_point_target;
    n->white_point_target = o->white_point_target;
    n->output_power = o->output_power;
    n->latitude = o->latitude;
    n->contrast = o->contrast;
    n->saturation = o->saturation;
    n->balance = o->balance;
    n->preserve_color = o->preserve_color;
    n->shadows = o->shadows;
    n->highlights = o->highlights;
    n->reconstruct_threshold = o->reconstruct_threshold;
    n->reconstruct_bloom_vs_details = o->reconstruct_bloom_vs_details;
    n->reconstruct_grey_vs_color = o->reconstruct_grey_vs_color;
    n->reconstruct_structure_vs_texture = o->reconstruct_structure_vs_texture;
    n->reconstruct_feather = o->reconstruct_feather;
    n->version = o->version;
    n->auto_hardness = o->auto_hardness;
    n->custom_grey = o->custom_grey;
    n->high_quality_reconstruction = o->high_quality_reconstruction;
    n->noise_level = d->noise_level;
    n->noise_distribution = d->noise_distribution;
    n->noise_level = 0.f;
    n->internal_version = 2019;
    n->compensate_icc_black = FALSE;
    return 0;
  }
  if(old_version == 3 && new_version == 4)
  {
    typedef struct dt_iop_filmicrgb_params_v3_t
    {
      float grey_point_source;     // $MIN: 0 $MAX: 100 $DEFAULT: 18.45 $DESCRIPTION: "middle grey luminance"
      float black_point_source;    // $MIN: -16 $MAX: -0.1 $DEFAULT: -8.0 $DESCRIPTION: "black relative exposure"
      float white_point_source;    // $MIN: 0 $MAX: 16 $DEFAULT: 4.0 $DESCRIPTION: "white relative exposure"
      float reconstruct_threshold; // $MIN: -6.0 $MAX: 6.0 $DEFAULT: +3.0 $DESCRIPTION: "threshold"
      float reconstruct_feather;   // $MIN: 0.25 $MAX: 6.0 $DEFAULT: 3.0 $DESCRIPTION: "transition"
      float reconstruct_bloom_vs_details; // $MIN: -100.0 $MAX: 100.0 $DEFAULT: 100.0 $DESCRIPTION:
                                          // "bloom/reconstruct"
      float reconstruct_grey_vs_color;    // $MIN: -100.0 $MAX: 100.0 $DEFAULT: 100.0 $DESCRIPTION: "grey/colorful
                                          // details"
      float reconstruct_structure_vs_texture; // $MIN: -100.0 $MAX: 100.0 $DEFAULT: 0.0 $DESCRIPTION:
                                              // "structure/texture"
      float security_factor;    // $MIN: -50 $MAX: 200 $DEFAULT: 0 $DESCRIPTION: "dynamic range scaling"
      float grey_point_target;  // $MIN: 1 $MAX: 50 $DEFAULT: 18.45 $DESCRIPTION: "target middle grey"
      float black_point_target; // $MIN: 0 $MAX: 20 $DEFAULT: 0 $DESCRIPTION: "target black luminance"
      float white_point_target; // $MIN: 0 $MAX: 1600 $DEFAULT: 100 $DESCRIPTION: "target white luminance"
      float output_power;       // $MIN: 1 $MAX: 10 $DEFAULT: 4.0 $DESCRIPTION: "hardness"
      float latitude;           // $MIN: 0.01 $MAX: 100 $DEFAULT: 33.0
      float contrast;           // $MIN: 0 $MAX: 5 $DEFAULT: 1.50
      float saturation;         // $MIN: -50 $MAX: 200 $DEFAULT: 10 $DESCRIPTION: "extreme luminance saturation"
      float balance;            // $MIN: -50 $MAX: 50 $DEFAULT: 0.0 $DESCRIPTION: "shadows/highlights balance"
      float noise_level;        // $MIN: 0.0 $MAX: 6.0 $DEFAULT: 0.1f $DESCRIPTION: "add noise in highlights"
      dt_iop_filmicrgb_methods_type_t preserve_color; // $DEFAULT: DT_FILMIC_METHOD_POWER_NORM $DESCRIPTION:
                                                      // "preserve chrominance"
      dt_iop_filmicrgb_colorscience_type_t version;   // $DEFAULT: DT_FILMIC_COLORSCIENCE_V2 $DESCRIPTION: "color
                                                      // science"
      gboolean auto_hardness;                         // $DEFAULT: TRUE $DESCRIPTION: "auto adjust hardness"
      gboolean custom_grey;            // $DEFAULT: FALSE $DESCRIPTION: "use custom middle-grey values"
      int high_quality_reconstruction; // $MIN: 0 $MAX: 10 $DEFAULT: 1 $DESCRIPTION: "iterations of high-quality
                                       // reconstruction"
      int noise_distribution;          // $DEFAULT: DT_NOISE_POISSONIAN $DESCRIPTION: "type of noise"
      dt_iop_filmicrgb_curve_type_t shadows; // $DEFAULT: DT_FILMIC_CURVE_POLY_4 $DESCRIPTION: "contrast in shadows"
      dt_iop_filmicrgb_curve_type_t highlights; // $DEFAULT: DT_FILMIC_CURVE_POLY_4 $DESCRIPTION: "contrast in
                                                // highlights"
    } dt_iop_filmicrgb_params_v3_t;

    dt_iop_filmicrgb_params_v3_t *o = (dt_iop_filmicrgb_params_v3_t *)old_params;
    dt_iop_filmicrgb_params_t *n = (dt_iop_filmicrgb_params_t *)new_params;
    dt_iop_filmicrgb_params_t *d = (dt_iop_filmicrgb_params_t *)self->default_params;

    *n = *d; // start with a fresh copy of default parameters

    n->grey_point_source = o->grey_point_source;
    n->white_point_source = o->white_point_source;
    n->black_point_source = o->black_point_source;
    n->security_factor = o->security_factor;
    n->grey_point_target = o->grey_point_target;
    n->black_point_target = o->black_point_target;
    n->white_point_target = o->white_point_target;
    n->output_power = o->output_power;
    n->latitude = o->latitude;
    n->contrast = o->contrast;
    n->saturation = o->saturation;
    n->balance = o->balance;
    n->preserve_color = o->preserve_color;
    n->shadows = o->shadows;
    n->highlights = o->highlights;
    n->reconstruct_threshold = o->reconstruct_threshold;
    n->reconstruct_bloom_vs_details = o->reconstruct_bloom_vs_details;
    n->reconstruct_grey_vs_color = o->reconstruct_grey_vs_color;
    n->reconstruct_structure_vs_texture = o->reconstruct_structure_vs_texture;
    n->reconstruct_feather = o->reconstruct_feather;
    n->version = o->version;
    n->auto_hardness = o->auto_hardness;
    n->custom_grey = o->custom_grey;
    n->high_quality_reconstruction = o->high_quality_reconstruction;
    n->noise_level = d->noise_level;
    n->noise_distribution = d->noise_distribution;
    n->noise_level = d->noise_level;
    n->internal_version = 2019;
    n->compensate_icc_black = FALSE;
    return 0;
  }
  return 1;
}


#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline float clamp_simd(const float x)
{
  return fminf(fmaxf(x, 0.0f), 1.0f);
}


#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline float sqf(const float x)
{
  return x * x;
}


#ifdef _OPENMP
#pragma omp declare simd aligned(pixel : 16)
#endif
static inline float pixel_rgb_norm_power(const float pixel[4])
{
  // weird norm sort of perceptual. This is black magic really, but it looks good.
  // the full norm is (R^3 + G^3 + B^3) / (R^2 + G^2 + B^2) and it should be in ]0; +infinity[

  float numerator = 0.0f;
  float denominator = 0.0f;

  for(int c = 0; c < 3; c++)
  {
    const float value = fabsf(pixel[c]);
    const float RGB_square = value * value;
    const float RGB_cubic = RGB_square * value;
    numerator += RGB_cubic;
    denominator += RGB_square;
  }

  return numerator / fmaxf(denominator, 1e-12f); // prevent from division-by-0 (note: (1e-6)^2 = 1e-12
}


#ifdef _OPENMP
#pragma omp declare simd aligned(pixel : 16) uniform(variant, work_profile)
#endif
static inline float get_pixel_norm(const float pixel[4], const dt_iop_filmicrgb_methods_type_t variant,
                                   const dt_iop_order_iccprofile_info_t *const work_profile)
{
  switch(variant)
  {
    case(DT_FILMIC_METHOD_MAX_RGB):
      return fmaxf(fmaxf(pixel[0], pixel[1]), pixel[2]);

    case(DT_FILMIC_METHOD_LUMINANCE):
      return (work_profile)
                 ? dt_ioppr_get_rgb_matrix_luminance(pixel, work_profile->matrix_in, work_profile->lut_in,
                                                     work_profile->unbounded_coeffs_in, work_profile->lutsize,
                                                     work_profile->nonlinearlut)
                 : dt_camera_rgb_luminance(pixel);

    case(DT_FILMIC_METHOD_POWER_NORM):
      return pixel_rgb_norm_power(pixel);

    case(DT_FILMIC_METHOD_EUCLIDEAN_NORM):
      return sqrtf(sqf(pixel[0]) + sqf(pixel[1]) + sqf(pixel[2]));

    default:
      return (work_profile)
                 ? dt_ioppr_get_rgb_matrix_luminance(pixel, work_profile->matrix_in, work_profile->lut_in,
                                                     work_profile->unbounded_coeffs_in, work_profile->lutsize,
                                                     work_profile->nonlinearlut)
                 : dt_camera_rgb_luminance(pixel);
  }
}


#ifdef _OPENMP
#pragma omp declare simd uniform(grey, black, dynamic_range)
#endif
static inline float log_tonemapping_v1(const float x, const float grey, const float black,
                                       const float dynamic_range)
{
  const float temp = (log2f(x / grey) - black) / dynamic_range;
  return fmaxf(fminf(temp, 1.0f), 1.52587890625e-05f);
}


#ifdef _OPENMP
#pragma omp declare simd uniform(grey, black, dynamic_range)
#endif
static inline float log_tonemapping_v2(const float x, const float grey, const float black,
                                       const float dynamic_range)
{
  return clamp_simd((log2f(x / grey) - black) / dynamic_range);
}

#ifdef _OPENMP
#pragma omp declare simd uniform(grey, black, dynamic_range)
#endif
static inline float exp_tonemapping_v2(const float x, const float grey, const float black,
                                       const float dynamic_range)
{
  // inverse of log_tonemapping
  return grey * exp2f(dynamic_range * x + black);
}


#ifdef _OPENMP
#pragma omp declare simd aligned(M1, M2, M3, M4 : 16) uniform(M1, M2, M3, M4, M5, latitude_min, latitude_max)
#endif
static inline float filmic_spline(const float x, const float M1[4], const float M2[4], const float M3[4],
                                  const float M4[4], const float M5[4], const float latitude_min,
                                  const float latitude_max)
{
  return (x < latitude_min) ? M1[0] + x * (M2[0] + x * (M3[0] + x * (M4[0] + x * M5[0]))) :     // toe
             (x > latitude_max) ? M1[1] + x * (M2[1] + x * (M3[1] + x * (M4[1] + x * M5[1]))) : // shoulder
                 M1[2] + x * (M2[2] + x * (M3[2] + x * (M4[2] + x * M5[2])));                   // latitude
}


#ifdef _OPENMP
#pragma omp declare simd uniform(sigma_toe, sigma_shoulder)
#endif
static inline float filmic_desaturate_v1(const float x, const float sigma_toe, const float sigma_shoulder,
                                         const float saturation)
{
  const float radius_toe = x;
  const float radius_shoulder = 1.0f - x;

  const float key_toe = expf(-0.5f * radius_toe * radius_toe / sigma_toe);
  const float key_shoulder = expf(-0.5f * radius_shoulder * radius_shoulder / sigma_shoulder);

  return 1.0f - clamp_simd((key_toe + key_shoulder) / saturation);
}


#ifdef _OPENMP
#pragma omp declare simd uniform(sigma_toe, sigma_shoulder)
#endif
static inline float filmic_desaturate_v2(const float x, const float sigma_toe, const float sigma_shoulder,
                                         const float saturation)
{
  const float radius_toe = x;
  const float radius_shoulder = 1.0f - x;
  const float sat2 = 0.5f / sqrtf(saturation);
  const float key_toe = expf(-radius_toe * radius_toe / sigma_toe * sat2);
  const float key_shoulder = expf(-radius_shoulder * radius_shoulder / sigma_shoulder * sat2);

  return (saturation - (key_toe + key_shoulder) * (saturation));
}


#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline float linear_saturation(const float x, const float luminance, const float saturation)
{
  return luminance + saturation * (x - luminance);
}


#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline float fmaxabsf(const float a, const float b)
{
  const float abs_a = fabsf(a);
  const float abs_b = fabsf(b);
  return (abs_a > abs_b) ? a : b;
}

#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline float fminabsf(const float a, const float b)
{
  const float abs_a = fabsf(a);
  const float abs_b = fabsf(b);
  return (abs_a < abs_b) ? a : b;
}


#define MAX_NUM_SCALES 12


#ifdef _OPENMP
#pragma omp declare simd aligned(in, mask : 64) uniform(feathering, normalize, width, height, ch)
#endif
static inline gint mask_clipped_pixels(const float *const restrict in, float *const restrict mask,
                                       const float normalize, const float feathering, const size_t width,
                                       const size_t height, const size_t ch)
{
  /* 1. Detect if pixels are clipped and count them,
   * 2. assign them a weight in [0. ; 1.] depending on how close from clipping they are. The weights are defined
   *    by a sigmoid centered in `reconstruct_threshold` so the transition is soft and symmetrical
   */

  int clipped = 0;

#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(in, mask, normalize, feathering, width, height, ch) \
  schedule(simd:static) aligned(mask, in:64) reduction(+:clipped)
#endif
  for(size_t k = 0; k < height * width * ch; k += ch)
  {
    const float pix_max = sqrtf(sqf(in[k]) + sqf(in[k + 1]) + sqf(in[k + 2]));
    const float argument = -pix_max * normalize + feathering;
    const float weight = 1.0f / (1.0f + exp2f(argument));
    mask[k / ch] = weight;

    // at x = 4, the sigmoid produces opacity = 5.882 %.
    // any x > 4 will produce negligible changes over the image,
    // especially since we have reduced visual sensitivity in highlights.
    // so we discard pixels for argument > 4. for they are not worth computing.
    clipped += (4.f > argument);
  }

  // If clipped area is < 9 pixels, recovery is not worth the computational cost, so skip it.
  return (clipped > 9);
}


#ifdef _OPENMP
#pragma omp declare simd aligned(in, mask, inpainted : 64)                                                        \
    uniform(num_elem, ch, noise_level, noise_distribution, threshold)
#endif
inline static void inpaint_noise(const float *const in, const float *const mask, float *const inpainted,
                                 const float noise_level, const float threshold,
                                 const dt_noise_distribution_t noise_distribution, const size_t num_elem,
                                 const size_t ch)
{
  // add statistical noise in highlights to fill-in texture
  // this creates "particules" in highlights, that will help the implicit partial derivative equation
  // solver used in wavelets reconstruction to generate texture

  // Init random number generator
  uint64_t DT_ALIGNED_ARRAY state[4] = { 0 };
  xoshiro256_init(1, state);

#ifdef _OPENMP
#pragma omp parallel for simd default(none)                                                                       \
    dt_omp_firstprivate(in, mask, inpainted, num_elem, ch, state, noise_level, noise_distribution, threshold)     \
        schedule(simd                                                                                             \
                 : static) aligned(mask, in, inpainted, state : 64)
#endif
  for(size_t k = 0; k < num_elem; k += ch)
  {
    // get the mask value in [0 ; 1]
    const float weight = mask[k / ch];

    for(size_t c = 0; c < 3; c++)
    {
      // create statistical noise
      const float input = in[k + c];
      const float noise
          = dt_noise_generator(noise_distribution, input, input * noise_level / threshold, (c % 2) == 0.f, state);

      // add noise to input
      inpainted[k + c] = input * (1.0f - weight) + weight * noise;
    }
  }
}


// B spline filter
#define FSIZE 5


#ifdef _OPENMP
#pragma omp declare simd aligned(in, out : 64) uniform(mult, bound_left, bound_right, ch, width, height)
#endif
inline static void blur_2D_Bspline_vertical(const float *const restrict in, float *const restrict out,
                                            const size_t width, const size_t height, const size_t ch,
                                            const size_t mult, const int bound_left, const int bound_right)
{
// À-trous B-spline interpolation/blur shifted by mult
// Convolve B-spline filter over lines
#ifdef _OPENMP
#pragma omp parallel for default(none)                                                                            \
    dt_omp_firstprivate(in, out, width, height, ch, bound_left, bound_right, mult) schedule(simd                  \
                                                                                            : static) collapse(2)
#endif
  for(size_t i = 0; i < height; i++)
    for(size_t j = 0; j < width; j++)
    {
      const size_t index_out = (i * width + j) * ch;
      float DT_ALIGNED_PIXEL accumulator[4] = { 0.0f };

      // Are we in the boundary zone that needs bound checking ?
      const int check = !((j > 2 * mult) && (j < width - 2 * mult));

      // -funswitch-loops should compile 2 loops, for each check outcome
      if(check)
      {
#ifdef _OPENMP
#pragma omp simd aligned(in : 64) aligned(accumulator : 16) reduction(+ : accumulator)
#endif
        for(size_t jj = 0; jj < FSIZE; ++jj)
          for(size_t c = 0; c < 3; ++c)
          {
            int index_x = mult * (jj - (FSIZE - 1) / 2) + j;
            index_x = (index_x < bound_left) ? bound_left : (index_x > bound_right) ? bound_right : index_x;

            static const float DT_ALIGNED_ARRAY filter[FSIZE]
                = { 1.0f / 16.0f, 4.0f / 16.0f, 6.0f / 16.0f, 4.0f / 16.0f, 1.0f / 16.0f };

            accumulator[c] += filter[jj] * in[(i * width + index_x) * ch + c];
          }
      }
      else // fast-track
      {
#ifdef _OPENMP
#pragma omp simd aligned(in : 64) aligned(accumulator : 16) reduction(+ : accumulator)
#endif
        for(size_t jj = 0; jj < FSIZE; ++jj)
          for(size_t c = 0; c < 3; ++c)
          {
            const size_t index_x = mult * (jj - (FSIZE - 1) / 2) + j;
            static const float DT_ALIGNED_ARRAY filter[FSIZE]
                = { 1.0f / 16.0f, 4.0f / 16.0f, 6.0f / 16.0f, 4.0f / 16.0f, 1.0f / 16.0f };
            accumulator[c] += filter[jj] * in[(i * width + index_x) * ch + c];
          }
      }

#ifdef _OPENMP
#pragma omp simd aligned(out : 64) aligned(accumulator : 16)
#endif
      for(size_t c = 0; c < 3; ++c) out[index_out + c] = accumulator[c];
    }
}


#ifdef _OPENMP
#pragma omp declare simd aligned(in, out : 64) uniform(mult, bound_top, bound_bot, ch, width, height)
#endif
inline static void blur_2D_Bspline_horizontal(const float *const restrict in, float *const restrict out,
                                              const size_t width, const size_t height, const size_t ch,
                                              const size_t mult, const int bound_top, const int bound_bot)
{
// À-trous B-spline interpolation/blur shifted by mult
// Convolve B-spline filter over columns
#ifdef _OPENMP
#pragma omp parallel for default(none)                                                                            \
    dt_omp_firstprivate(out, in, width, height, ch, bound_bot, bound_top, mult) schedule(simd                     \
                                                                                         : static) collapse(2)
#endif
  for(size_t i = 0; i < height; i++)
    for(size_t j = 0; j < width; j++)
    {
      const size_t index_out = (i * width + j) * ch;
      float DT_ALIGNED_PIXEL accumulator[4] = { 0.0f };

      // Are we in the boundary zone that needs bound checking ?
      const int check = !((i > 2 * mult) && (i < height - 2 * mult));

      // -funswitch-loops should compile 2 loops, for each check outcome
      if(check)
      {
#ifdef _OPENMP
#pragma omp simd aligned(in : 64) aligned(accumulator : 16) reduction(+ : accumulator)
#endif
        for(size_t ii = 0; ii < FSIZE; ++ii)
          for(size_t c = 0; c < 3; ++c)
          {
            int index_y = mult * (ii - (FSIZE - 1) / 2) + i;
            index_y = (index_y < bound_top) ? bound_top : (index_y > bound_bot) ? bound_bot : index_y;

            static const float DT_ALIGNED_ARRAY filter[FSIZE]
                = { 1.0f / 16.0f, 4.0f / 16.0f, 6.0f / 16.0f, 4.0f / 16.0f, 1.0f / 16.0f };

            accumulator[c] += filter[ii] * in[(index_y * width + j) * ch + c];
          }
      }
      else // fast-track
      {
        for(size_t ii = 0; ii < FSIZE; ++ii)
        {
          const size_t index_y = mult * (ii - (FSIZE - 1) / 2) + i;

#ifdef _OPENMP
#pragma omp simd aligned(in : 64) aligned(accumulator : 16) reduction(+ : accumulator)
#endif
          for(size_t c = 0; c < 3; ++c)
          {
            static const float DT_ALIGNED_ARRAY filter[FSIZE]
                = { 1.0f / 16.0f, 4.0f / 16.0f, 6.0f / 16.0f, 4.0f / 16.0f, 1.0f / 16.0f };
            accumulator[c] += filter[ii] * in[(index_y * width + j) * ch + c];
          }
        }
      }

#ifdef _OPENMP
#pragma omp simd aligned(out : 64) aligned(accumulator : 16)
#endif
      for(size_t c = 0; c < ch; ++c) out[index_out + c] = accumulator[c];
    }
}


#ifdef _OPENMP
#pragma omp declare simd aligned(LF, HF, texture, mask, reconstructed : 64)                                       \
    uniform(ch, width, height, gamma, gamma_comp, beta, beta_comp, delta, s, scales)
#endif
inline static void wavelets_reconstruct_RGB(const float *const restrict HF, const float *const restrict LF,
                                            const float *const restrict texture, const float *const restrict mask,
                                            float *const restrict reconstructed, const size_t width,
                                            const size_t height, const size_t ch, const float gamma,
                                            const float gamma_comp, const float beta, const float beta_comp,
                                            const float delta, const size_t s, const size_t scales)
{
#ifdef _OPENMP
#pragma omp parallel for simd default(none)                                                                       \
    dt_omp_firstprivate(width, height, ch, HF, LF, texture, mask, reconstructed, gamma, gamma_comp, beta,         \
                        beta_comp, delta, s, scales) schedule(simd                                                \
                                                              : static)                                           \
        aligned(HF, LF, texture, mask, reconstructed : 64)
#endif
  for(size_t k = 0; k < height * width * ch; k += ch)
  {
    const float alpha = mask[k / ch];

    // cache RGB wavelets scales just to be sure the compiler doesn't reload them
    const float DT_ALIGNED_ARRAY HF_c[4] = { HF[k], HF[k + 1], HF[k + 2], HF[k + 3] };
    const float DT_ALIGNED_ARRAY LF_c[4] = { LF[k], LF[k + 1], LF[k + 2], LF[k + 3] };

    // synthesize the max of all RGB channels texture as a flat texture term for the whole pixel
    // this is useful if only 1 or 2 channels are clipped, so we transfer the valid/sharpest texture on the other
    // channels
    const float grey_texture = gamma * texture[k / ch];

    // synthesize the max of all interpolated/inpainted RGB channels as a flat details term for the whole pixel
    // this is smoother than grey_texture and will fill holes smoothly in details layers if grey_texture ~= 0.f
    const float grey_details = fmaxabsf(fmaxabsf(HF_c[0], HF_c[1]), HF_c[2]);

    // synthesize both terms with weighting
    // when beta_comp ~= 1.0, we force the reconstruction to be achromatic, which may help with gamut issues or
    // magenta highlights.
    const float grey_HF = beta_comp * (gamma_comp * grey_details + grey_texture);

    // synthesize the min of all low-frequency RGB channels as a flat structure term for the whole pixel
    // when beta_comp ~= 1.0, we force the reconstruction to be achromatic, which may help with gamut issues or
    // magenta highlights.
    const float grey_residual = beta_comp * fminf(fminf(LF_c[0], LF_c[1]), LF_c[2]);

    for(size_t c = 0; c < 3; c++)
    {
      // synthesize interpolated/inpainted RGB channels color details residuals and weigh them
      // this brings back some color on top of the grey_residual
      const float color_residual = LF_c[c] * beta;

      // synthesize interpolated/inpainted RGB channels color details and weigh them
      // this brings back some color on top of the grey_details
      const float color_details
          = (HF_c[c] * gamma_comp + fminf(fabsf(HF_c[c] / grey_details), 1.f) * grey_texture) * beta;

      // reconstruction
      reconstructed[k + c]
          += alpha * (delta * (grey_HF + color_details) + (grey_residual + color_residual) / (float)scales);
    }
  }
}


#ifdef _OPENMP
#pragma omp declare simd aligned(LF, HF, texture, mask, reconstructed : 64)                                       \
    uniform(ch, width, height, gamma, gamma_comp, beta, beta_comp, delta, s, scales)
#endif
inline static void wavelets_reconstruct_ratios(const float *const restrict HF, const float *const restrict LF,
                                               const float *const restrict texture,
                                               const float *const restrict mask,
                                               float *const restrict reconstructed, const size_t width,
                                               const size_t height, const size_t ch, const float gamma,
                                               const float gamma_comp, const float beta, const float beta_comp,
                                               const float delta, const size_t s, const size_t scales)
{
/*
 * This is the adapted version of the RGB reconstruction
 * RGB contain high frequencies that we try to recover, so we favor them in the reconstruction.
 * The ratios represent the chromaticity in image and contain low frequencies in the absence of noise or
 * aberrations, so, here, we favor them instead.
 *
 * Consequences : 
 *  1. use min of interpolated channels details instead of max, to get smoother details
 *  4. use the max of low frequency channels instead of min, to favor achromatic solution.
 *
 * Note : ratios close to 1 mean higher spectral purity (more white). Ratios close to 0 mean lower spectral purity
 * (more colorful)
 */
#ifdef _OPENMP
#pragma omp parallel for simd default(none)                                                                       \
    dt_omp_firstprivate(width, height, ch, HF, LF, texture, mask, reconstructed, gamma, gamma_comp, beta,         \
                        beta_comp, delta, s, scales) schedule(simd                                                \
                                                              : static)                                           \
        aligned(HF, LF, texture, mask, reconstructed : 64)
#endif
  for(size_t k = 0; k < height * width * ch; k += ch)
  {
    const float alpha = mask[k / ch];

    // cache RGB wavelets scales just to be sure the compiler doesn't reload them
    const float DT_ALIGNED_ARRAY HF_c[4] = { HF[k], HF[k + 1], HF[k + 2], HF[k + 3] };
    const float DT_ALIGNED_ARRAY LF_c[4] = { LF[k], LF[k + 1], LF[k + 2], LF[k + 3] };

    // synthesize the max of all RGB channels texture as a flat texture term for the whole pixel
    // this is useful if only 1 or 2 channels are clipped, so we transfer the valid/sharpest texture on the other
    // channels
    const float grey_texture = gamma * texture[k / ch];

    // synthesize the max of all interpolated/inpainted RGB channels as a flat details term for the whole pixel
    // this is smoother than grey_texture and will fill holes smoothly in details layers if grey_texture ~= 0.f
    const float grey_details = fmaxabsf(fmaxabsf(HF_c[0], HF_c[1]), HF_c[2]);

    // synthesize both terms with weighting
    // when beta_comp ~= 1.0, we force the reconstruction to be achromatic, which may help with gamut issues or
    // magenta highlights.
    const float grey_HF = beta_comp * (gamma_comp * grey_details + grey_texture);

    // synthesize the min of all low-frequency RGB channels as a flat structure term for the whole pixel
    // when beta_comp ~= 1.0, we force the reconstruction to be achromatic, which may help with gamut issues or
    // magenta highlights.
    const float grey_residual = beta_comp * fmaxf(fmaxf(LF_c[0], LF_c[1]), LF_c[2]);

    for(size_t c = 0; c < 3; c++)
    {
      // synthesize interpolated/inpainted RGB channels color details residuals and weigh them
      // this brings back some color on top of the grey_residual
      const float color_residual = LF_c[c] * beta;

      // synthesize interpolated/inpainted RGB channels color details and weigh them
      // this brings back some color on top of the grey_details
      const float color_details
          = (HF_c[c] * gamma_comp - 0.5f * fminf(fabsf(HF_c[c] / grey_details), 1.f) * grey_texture) * beta;

      // reconstruction
      reconstructed[k + c]
          += alpha * (delta * (grey_HF + color_details) + (grey_residual + color_residual) / (float)scales);
    }
  }
}


#ifdef _OPENMP
#pragma omp declare simd aligned(in, mask, reconstructed : 64) uniform(ch, width, height)
#endif
static inline void init_reconstruct(const float *const restrict in, const float *const restrict mask,
                                    float *const restrict reconstructed, const size_t width, const size_t height,
                                    const size_t ch)
{
// init the reconstructed buffer with non-clipped and partially clipped pixels
// Note : it's a simple multiplied alpha blending where mask = alpha weight
#ifdef _OPENMP
#pragma omp parallel for simd default(none) dt_omp_firstprivate(in, mask, reconstructed, width, height, ch)       \
    schedule(simd                                                                                                 \
             : static) aligned(in, mask, reconstructed : 64)
#endif
  for(size_t k = 0; k < height * width * ch; k++)
  {
    reconstructed[k] = in[k] * (1.f - mask[k / ch]);
  }
}


#ifdef _OPENMP
#pragma omp declare simd aligned(detail, LF, HF, texture : 64) uniform(ch, width, height)
#endif
static inline void wavelets_detail_level_RGB(const float *const restrict detail, const float *const restrict LF,
                                             float *const restrict HF, float *const restrict texture,
                                             const size_t width, const size_t height, const size_t ch)
{
#ifdef _OPENMP
#pragma omp parallel for simd default(none) dt_omp_firstprivate(width, height, ch, HF, LF, detail, texture)       \
    schedule(simd                                                                                                 \
             : static) aligned(HF, LF, detail, texture : 64)
#endif
  for(size_t k = 0; k < height * width * ch; k += ch)
  {
    for(size_t c = 0; c < 3; ++c) HF[k + c] = detail[k + c] - LF[k + c];
    texture[k / ch] = fmaxabsf(fmaxabsf(HF[k], HF[k + 1]), HF[k + 2]);
  }
}


#ifdef _OPENMP
#pragma omp declare simd aligned(detail, LF, HF, texture : 64) uniform(ch, width, height)
#endif
static inline void wavelets_detail_level_ratios(const float *const restrict detail, const float *const restrict LF,
                                                float *const restrict HF, float *const restrict texture,
                                                const size_t width, const size_t height, const size_t ch)
{
#ifdef _OPENMP
#pragma omp parallel for simd default(none) dt_omp_firstprivate(width, height, ch, HF, LF, detail, texture)       \
    schedule(simd                                                                                                 \
             : static) aligned(HF, LF, detail, texture : 64)
#endif
  for(size_t k = 0; k < height * width * ch; k += ch)
  {
    for(size_t c = 0; c < 3; ++c) HF[k + c] = detail[k + c] - LF[k + c];
    texture[k / ch] = fminabsf(fminabsf(HF[k], HF[k + 1]), HF[k + 2]);
  }
}


static int get_scales(const dt_iop_roi_t *roi_in, const dt_dev_pixelpipe_iop_t *const piece)
{
  /* How many wavelets scales do we need to compute at current zoom level ?
   * 0. To get the same preview no matter the zoom scale, the relative image coverage ratio of the filter at
   * the coarsest wavelet level should always stay constant.
   * 1. The image coverage of each B spline filter of size `FSIZE` is `2^(level) * (FSIZE - 1) / 2 + 1` pixels
   * 2. The coarsest level filter at full resolution should cover `1/FSIZE` of the largest image dimension.
   * 3. The coarsest level filter at current zoom level should cover `scale/FSIZE` of the largest image dimension.
   *
   * So we compute the level that solves 1. subject to 3. Of course, integer rounding doesn't make that 1:1
   * accurate.
   */
  const float scale = roi_in->scale / piece->iscale;
  const size_t size = MAX(piece->buf_in.height * piece->iscale, piece->buf_in.width * piece->iscale);
  const int scales = floorf(log2f((2.0f * size * scale / ((FSIZE - 1) * FSIZE)) - 1.0f));
  return CLAMP(scales, 1, MAX_NUM_SCALES);
}


#ifdef _OPENMP
#pragma omp declare simd aligned(in, mask, reconstructed : 64) uniform(ch, variant, piece, roi_in, roi_out)
#endif
static inline gint reconstruct_highlights(const float *const restrict in, const float *const restrict mask,
                                          float *const restrict reconstructed,
                                          const dt_iop_filmicrgb_reconstruction_type_t variant, const size_t ch,
                                          const dt_iop_filmicrgb_data_t *const data, dt_dev_pixelpipe_iop_t *piece,
                                          const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  gint success = TRUE;

  // wavelets scales
  const int scales = get_scales(roi_in, piece);

  // wavelets scales buffers
  float *const restrict LF_even = dt_alloc_sse_ps(roi_out->width * roi_out->height * ch); // low-frequencies RGB
  float *const restrict LF_odd = dt_alloc_sse_ps(roi_out->width * roi_out->height * ch);  // low-frequencies RGB
  float *const restrict HF_RGB = dt_alloc_sse_ps(roi_out->width * roi_out->height * ch);  // high-frequencies RGB
  float *const restrict HF_grey
      = dt_alloc_sse_ps(roi_out->width * roi_out->height); // max(high-frequencies RGB) grey

  // alloc a permanent reusable buffer for intermediate computations - avoid multiple alloc/free
  float *const restrict temp = dt_alloc_sse_ps(roi_out->width * roi_out->height * ch);

  if(!LF_even || !LF_odd || !HF_RGB || !HF_grey || !temp)
  {
    dt_control_log(_("filmic highlights reconstruction failed to allocate memory, check your RAM settings"));
    success = FALSE;
    goto error;
  }

  // Init reconstructed with valid parts of image
  init_reconstruct(in, mask, reconstructed, roi_out->width, roi_out->height, ch);

  // structure inpainting vs. texture duplicating weight
  const float gamma = (data->reconstruct_structure_vs_texture);
  const float gamma_comp = 1.0f - data->reconstruct_structure_vs_texture;

  // colorful vs. grey weight
  const float beta = data->reconstruct_grey_vs_color;
  const float beta_comp = 1.f - data->reconstruct_grey_vs_color;

  // bloom vs reconstruct weight
  const float delta = data->reconstruct_bloom_vs_details;

  // boundary conditions
  const int bound_left = 0;
  const int bound_right = roi_out->width - 1;
  const int bound_top = 0;
  const int bound_bot = roi_out->height - 1;

  // À trous wavelet decompose
  // there is a paper from a guy we know that explains it : https://jo.dreggn.org/home/2010_atrous.pdf
  // the wavelets decomposition here is the same as the equalizer/atrous module,
  // but simplified because we don't need the edge-aware term, so we can seperate the convolution kernel
  // with a vertical and horizontal blur, wich is 10 multiply-add instead of 25 by pixel.
  for(int s = 0; s < scales; ++s)
  {
    const float *restrict detail;
    float *restrict LF;

    // swap buffers so we only need 2 LF buffers : the LF at scale (s-1) and the one at current scale (s)
    if(s == 0)
    {
      detail = in;
      LF = LF_odd;
    }
    else if(s % 2 != 0)
    {
      detail = LF_odd;
      LF = LF_even;
    }
    else
    {
      detail = LF_even;
      LF = LF_odd;
    }

    const int mult = 1 << s; // fancy-pants C notation for 2^s with integer type, don't be afraid

    // Compute wavelets low-frequency scales
    blur_2D_Bspline_vertical(detail, temp, roi_out->width, roi_out->height, ch, mult, bound_left, bound_right);
    blur_2D_Bspline_horizontal(temp, LF, roi_out->width, roi_out->height, ch, mult, bound_top, bound_bot);

    // Compute wavelets high-frequency scales and save the maximum of texture over the RGB channels
    // Note : HF_RGB = detail - LF, HF_grey = max(HF_RGB)
    if(variant == DT_FILMIC_RECONSTRUCT_RGB)
      wavelets_detail_level_RGB(detail, LF, HF_RGB, HF_grey, roi_out->width, roi_out->height, ch);
    else if(variant == DT_FILMIC_RECONSTRUCT_RATIOS)
      wavelets_detail_level_ratios(detail, LF, HF_RGB, HF_grey, roi_out->width, roi_out->height, ch);

    // interpolate/blur/inpaint (same thing) the RGB high-frequency to fill holes
    blur_2D_Bspline_vertical(HF_RGB, temp, roi_out->width, roi_out->height, ch, mult, bound_left, bound_right);
    blur_2D_Bspline_horizontal(temp, HF_RGB, roi_out->width, roi_out->height, ch, mult, bound_top, bound_bot);

    // Reconstruct clipped parts
    if(variant == DT_FILMIC_RECONSTRUCT_RGB)
      wavelets_reconstruct_RGB(HF_RGB, LF, HF_grey, mask, reconstructed, roi_out->width, roi_out->height, ch,
                               gamma, gamma_comp, beta, beta_comp, delta, s, scales);
    else if(variant == DT_FILMIC_RECONSTRUCT_RATIOS)
      wavelets_reconstruct_ratios(HF_RGB, LF, HF_grey, mask, reconstructed, roi_out->width, roi_out->height, ch,
                                  gamma, gamma_comp, beta, beta_comp, delta, s, scales);
  }

error:
  if(temp) dt_free_align(temp);
  if(LF_even) dt_free_align(LF_even);
  if(LF_odd) dt_free_align(LF_odd);
  if(HF_RGB) dt_free_align(HF_RGB);
  if(HF_grey) dt_free_align(HF_grey);
  return success;
}


#ifdef _OPENMP
#pragma omp declare simd aligned(in, out : 64) uniform(ch, width, height, work_profile, data, spline)
#endif
static inline void filmic_split_v1(const float *const restrict in, float *const restrict out,
                                   const dt_iop_order_iccprofile_info_t *const work_profile,
                                   const dt_iop_filmicrgb_data_t *const data,
                                   const dt_iop_filmic_rgb_spline_t spline, const size_t width,
                                   const size_t height, const size_t ch)
{
#ifdef _OPENMP
#pragma omp parallel for simd default(none) dt_omp_firstprivate(width, height, ch, data, in, out, work_profile,   \
                                                                spline) schedule(simd                             \
                                                                                 : static) aligned(in, out : 64)
#endif
  for(size_t k = 0; k < height * width * ch; k += ch)
  {
    const float *const restrict pix_in = in + k;
    float *const restrict pix_out = out + k;
    float DT_ALIGNED_PIXEL temp[4];

    // Log tone-mapping
    for(int c = 0; c < 3; c++)
      temp[c] = log_tonemapping_v1(fmaxf(pix_in[c], NORM_MIN), data->grey_source, data->black_source,
                                   data->dynamic_range);

    // Get the desaturation coeff based on the log value
    const float lum = (work_profile)
                          ? dt_ioppr_get_rgb_matrix_luminance(temp, work_profile->matrix_in, work_profile->lut_in,
                                                              work_profile->unbounded_coeffs_in,
                                                              work_profile->lutsize, work_profile->nonlinearlut)
                          : dt_camera_rgb_luminance(temp);
    const float desaturation = filmic_desaturate_v1(lum, data->sigma_toe, data->sigma_shoulder, data->saturation);

    // Desaturate on the non-linear parts of the curve
    // Filmic S curve on the max RGB
    // Apply the transfer function of the display
    for(int c = 0; c < 3; c++)
      pix_out[c] = powf(
          clamp_simd(filmic_spline(linear_saturation(temp[c], lum, desaturation), spline.M1, spline.M2, spline.M3,
                                   spline.M4, spline.M5, spline.latitude_min, spline.latitude_max)),
          data->output_power);
  }
}


#ifdef _OPENMP
#pragma omp declare simd aligned(in, out : 64) uniform(ch, width, height, work_profile, data, spline)
#endif
static inline void filmic_split_v2(const float *const restrict in, float *const restrict out,
                                   const dt_iop_order_iccprofile_info_t *const work_profile,
                                   const dt_iop_filmicrgb_data_t *const data,
                                   const dt_iop_filmic_rgb_spline_t spline, const size_t width,
                                   const size_t height, const size_t ch)
{
#ifdef _OPENMP
#pragma omp parallel for simd default(none) dt_omp_firstprivate(width, height, ch, data, in, out, work_profile,   \
                                                                spline) schedule(simd                             \
                                                                                 : static) aligned(in, out : 64)
#endif
  for(size_t k = 0; k < height * width * ch; k += ch)
  {
    const float *const restrict pix_in = in + k;
    float *const restrict pix_out = out + k;
    float DT_ALIGNED_PIXEL temp[4];

    // Log tone-mapping
    for(int c = 0; c < 3; c++)
      temp[c] = log_tonemapping_v2(fmaxf(pix_in[c], NORM_MIN), data->grey_source, data->black_source,
                                   data->dynamic_range);

    // Get the desaturation coeff based on the log value
    const float lum = (work_profile)
                          ? dt_ioppr_get_rgb_matrix_luminance(temp, work_profile->matrix_in, work_profile->lut_in,
                                                              work_profile->unbounded_coeffs_in,
                                                              work_profile->lutsize, work_profile->nonlinearlut)
                          : dt_camera_rgb_luminance(temp);
    const float desaturation = filmic_desaturate_v2(lum, data->sigma_toe, data->sigma_shoulder, data->saturation);

    // Desaturate on the non-linear parts of the curve
    // Filmic S curve on the max RGB
    // Apply the transfer function of the display
    for(int c = 0; c < 3; c++)
      pix_out[c] = powf(
          clamp_simd(filmic_spline(linear_saturation(temp[c], lum, desaturation), spline.M1, spline.M2, spline.M3,
                                   spline.M4, spline.M5, spline.latitude_min, spline.latitude_max)),
          data->output_power);
  }
}


#ifdef _OPENMP
#pragma omp declare simd aligned(in, out : 64) uniform(ch, width, height, work_profile, data, spline, variant)
#endif
static inline void filmic_chroma_v1(const float *const restrict in, float *const restrict out,
                                    const dt_iop_order_iccprofile_info_t *const work_profile,
                                    const dt_iop_filmicrgb_data_t *const data,
                                    const dt_iop_filmic_rgb_spline_t spline, const int variant, const size_t width,
                                    const size_t height, const size_t ch)
{
#ifdef _OPENMP
#pragma omp parallel for simd default(none)                                                                       \
    dt_omp_firstprivate(width, height, ch, data, in, out, work_profile, variant, spline) schedule(simd            \
                                                                                                  : static)       \
        aligned(in, out : 64)
#endif
  for(size_t k = 0; k < height * width * ch; k += ch)
  {
    const float *const restrict pix_in = in + k;
    float *const restrict pix_out = out + k;

    float DT_ALIGNED_PIXEL ratios[4] = { 0.0f };
    float norm = fmaxf(get_pixel_norm(pix_in, variant, work_profile), NORM_MIN);

    // Save the ratios
    for(int c = 0; c < 3; c++) ratios[c] = pix_in[c] / norm;

    // Sanitize the ratios
    const float min_ratios = fminf(fminf(ratios[0], ratios[1]), ratios[2]);
    if(min_ratios < 0.0f)
      for(int c = 0; c < 3; c++) ratios[c] -= min_ratios;

    // Log tone-mapping
    norm = log_tonemapping_v1(norm, data->grey_source, data->black_source, data->dynamic_range);

    // Get the desaturation value based on the log value
    const float desaturation = filmic_desaturate_v1(norm, data->sigma_toe, data->sigma_shoulder, data->saturation);

    for(int c = 0; c < 3; c++) ratios[c] *= norm;

    const float lum = (work_profile) ? dt_ioppr_get_rgb_matrix_luminance(
                          ratios, work_profile->matrix_in, work_profile->lut_in, work_profile->unbounded_coeffs_in,
                          work_profile->lutsize, work_profile->nonlinearlut)
                                     : dt_camera_rgb_luminance(ratios);

    // Desaturate on the non-linear parts of the curve and save ratios
    for(int c = 0; c < 3; c++) ratios[c] = linear_saturation(ratios[c], lum, desaturation) / norm;

    // Filmic S curve on the max RGB
    // Apply the transfer function of the display
    norm = powf(clamp_simd(filmic_spline(norm, spline.M1, spline.M2, spline.M3, spline.M4, spline.M5,
                                         spline.latitude_min, spline.latitude_max)),
                data->output_power);

    // Re-apply ratios
    for(int c = 0; c < 3; c++) pix_out[c] = ratios[c] * norm;
  }
}


#ifdef _OPENMP
#pragma omp declare simd aligned(in, out : 64) uniform(ch, width, height, work_profile, data, spline, variant)
#endif
static inline void filmic_chroma_v2(const float *const restrict in, float *const restrict out,
                                    const dt_iop_order_iccprofile_info_t *const work_profile,
                                    const dt_iop_filmicrgb_data_t *const data,
                                    const dt_iop_filmic_rgb_spline_t spline, const int variant, const size_t width,
                                    const size_t height, const size_t ch)
{

#ifdef _OPENMP
#pragma omp parallel for simd default(none)                                                                       \
    dt_omp_firstprivate(width, height, ch, data, in, out, work_profile, variant, spline) schedule(simd            \
                                                                                                  : static)       \
        aligned(in, out : 64)
#endif
  for(size_t k = 0; k < height * width * ch; k += ch)
  {
    const float *const restrict pix_in = in + k;
    float *const restrict pix_out = out + k;

    float norm = fmaxf(get_pixel_norm(pix_in, variant, work_profile), NORM_MIN);

    // Save the ratios
    float DT_ALIGNED_PIXEL ratios[4] = { 0.0f };
    for(int c = 0; c < 3; c++) ratios[c] = pix_in[c] / norm;

    // Sanitize the ratios
    const float min_ratios = fminf(fminf(ratios[0], ratios[1]), ratios[2]);
    const int sanitize = (min_ratios < 0.0f);

    if(sanitize)
      for(int c = 0; c < 3; c++) ratios[c] -= min_ratios;

    // Log tone-mapping
    norm = log_tonemapping_v2(norm, data->grey_source, data->black_source, data->dynamic_range);

    // Get the desaturation value based on the log value
    const float desaturation = filmic_desaturate_v2(norm, data->sigma_toe, data->sigma_shoulder, data->saturation);

    // Filmic S curve on the max RGB
    // Apply the transfer function of the display
    norm = powf(clamp_simd(filmic_spline(norm, spline.M1, spline.M2, spline.M3, spline.M4, spline.M5,
                                         spline.latitude_min, spline.latitude_max)),
                data->output_power);

    // Re-apply ratios with saturation change
    for(int c = 0; c < 3; c++)
    {
      ratios[c] = fmaxf(ratios[c] + (1.0f - ratios[c]) * (1.0f - desaturation), 0.0f);
      pix_out[c] = ratios[c] * norm;
    }

    // Gamut mapping
    const float max_pix = fmaxf(fmaxf(pix_out[0], pix_out[1]), pix_out[2]);
    const int penalize = (max_pix > 1.0f);

    // Penalize the ratios by the amount of clipping
    if(penalize)
    {
      for(int c = 0; c < 3; c++)
      {
        ratios[c] = fmaxf(ratios[c] + (1.0f - max_pix), 0.0f);
        pix_out[c] = clamp_simd(ratios[c] * norm);
      }
    }
  }
}


#ifdef _OPENMP
#pragma omp declare simd aligned(mask, out : 64) uniform(ch, width, height)
#endif
static inline void display_mask(const float *const restrict mask, float *const restrict out, const size_t width,
                                const size_t height, const size_t ch)
{
#ifdef _OPENMP
#pragma omp parallel for simd default(none) dt_omp_firstprivate(width, height, ch, out, mask) schedule(simd       \
                                                                                                       : static)  \
    aligned(mask, out : 64)
#endif
  for(size_t k = 0; k < height * width * ch; k++) out[k] = mask[k / ch];
}


#ifdef _OPENMP
#pragma omp declare simd aligned(in, norms, ratios : 64) uniform(ch, width, height, work_profile, variant)
#endif
static inline void compute_ratios(const float *const restrict in, float *const restrict norms,
                                  float *const restrict ratios,
                                  const dt_iop_order_iccprofile_info_t *const work_profile, const int variant,
                                  const size_t width, const size_t height, const size_t ch)
{
#ifdef _OPENMP
#pragma omp parallel for simd default(none)                                                                       \
    dt_omp_firstprivate(ch, width, height, norms, ratios, in, work_profile, variant) schedule(simd                \
                                                                                              : static)           \
        aligned(norms, ratios, in : 64)
#endif
  for(size_t k = 0; k < height * width * ch; k += ch)
  {
    const float norm = fmaxf(get_pixel_norm(in + k, variant, work_profile), NORM_MIN);
    norms[k / ch] = norm;

    for(size_t c = 0; c < 3; c++) ratios[k + c] = in[k + c] / norm;
  }
}


#ifdef _OPENMP
#pragma omp declare simd aligned(norms, ratios : 64) uniform(ch, width, height)
#endif
static inline void restore_ratios(float *const restrict ratios, const float *const restrict norms,
                                  const size_t width, const size_t height, const size_t ch)
{
#ifdef _OPENMP
#pragma omp parallel for simd default(none) dt_omp_firstprivate(width, height, ch, norms, ratios)                 \
    schedule(simd                                                                                                 \
             : static) aligned(norms, ratios : 64)
#endif
  for(size_t k = 0; k < height * width * ch; k += ch)
  {
    for(size_t c = 0; c < 3; c++) ratios[k + c] *= norms[k / ch];
  }
}


#ifdef _OPENMP
#pragma omp declare simd aligned(ivoid, ovoid : 64) uniform(self, piece, roi_in, roi_out)
#endif
void process(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const restrict ivoid,
             void *const restrict ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_filmicrgb_data_t *const data = (dt_iop_filmicrgb_data_t *)piece->data;
  const dt_iop_order_iccprofile_info_t *const work_profile = dt_ioppr_get_pipe_work_profile_info(piece->pipe);

  if(piece->colors != 4)
  {
    dt_control_log(_("filmic works only on RGB input"));
    return;
  }

  const size_t ch = 4;

  /** The log2(x) -> -INF when x -> 0
   * thus very low values (noise) will get even lower, resulting in noise negative amplification,
   * which leads to pepper noise in shadows. To avoid that, we need to clip values that are noise for sure.
   * Using 16 bits RAW data, the black value (known by rawspeed for every manufacturer) could be used as a
   * threshold. However, at this point of the pixelpipe, the RAW levels have already been corrected and everything
   * can happen with black levels in the exposure module. So we define the threshold as the first non-null 16 bit
   * integer
   */

  float *restrict in = (float *)ivoid;
  float *const restrict out = (float *)ovoid;
  float *const restrict mask = dt_alloc_sse_ps(roi_out->width * roi_out->height);

  // used to adjuste noise level depending on size. Don't amplify noise if magnified > 100%
  const float scale = fmaxf(piece->iscale / roi_in->scale, 1.f);

  // build a mask of clipped pixels
  const float normalize = data->reconstruct_feather / data->reconstruct_threshold;
  const int recover_highlights
      = mask_clipped_pixels(in, mask, normalize, data->reconstruct_feather, roi_out->width, roi_out->height, 4);

  // display mask and exit
  if(self->dev->gui_attached && (piece->pipe->type & DT_DEV_PIXELPIPE_FULL) == DT_DEV_PIXELPIPE_FULL && mask)
  {
    dt_iop_filmicrgb_gui_data_t *g = (dt_iop_filmicrgb_gui_data_t *)self->gui_data;

    if(g->show_mask)
    {
      display_mask(mask, out, roi_out->width, roi_out->height, ch);
      dt_free_align(mask);
      return;
    }
  }

  float *const restrict reconstructed = dt_alloc_sse_ps(roi_out->width * roi_out->height * ch);

  const gboolean run_fast = (piece->pipe->type & DT_DEV_PIXELPIPE_FAST) == DT_DEV_PIXELPIPE_FAST;

  if(!run_fast && recover_highlights && mask && reconstructed)
  {
    float *const restrict inpainted = dt_alloc_sse_ps(roi_out->width * roi_out->height * ch);
    inpaint_noise(in, mask, inpainted, data->noise_level / scale, data->reconstruct_threshold,
                  data->noise_distribution, roi_out->width * roi_out->height * ch, ch);
    const gint success_1 = reconstruct_highlights(inpainted, mask, reconstructed, DT_FILMIC_RECONSTRUCT_RGB, ch,
                                                  data, piece, roi_in, roi_out);
    gint success_2 = TRUE;

    dt_free_align(inpainted);

    if(data->high_quality_reconstruction > 0 && success_1)
    {
      float *const restrict norms = dt_alloc_sse_ps(roi_out->width * roi_out->height);
      float *const restrict ratios = dt_alloc_sse_ps(roi_out->width * roi_out->height * ch);

      // reconstruct highlights PASS 2 on ratios
      if(norms && ratios)
      {
        for(int i = 0; i < data->high_quality_reconstruction; i++)
        {
          compute_ratios(reconstructed, norms, ratios, work_profile, DT_FILMIC_METHOD_EUCLIDEAN_NORM,
                         roi_out->width, roi_out->height, ch);
          success_2 = success_2
                      && reconstruct_highlights(ratios, mask, reconstructed, DT_FILMIC_RECONSTRUCT_RATIOS, ch,
                                                data, piece, roi_in, roi_out);
          restore_ratios(reconstructed, norms, roi_out->width, roi_out->height, ch);
        }
      }

      if(norms) dt_free_align(norms);
      if(ratios) dt_free_align(ratios);
    }

    if(success_1 && success_2) in = reconstructed; // use reconstructed buffer as tonemapping input
  }

  if(mask) dt_free_align(mask);

  if(data->preserve_color == DT_FILMIC_METHOD_NONE)
  {
    // no chroma preservation
    if(data->version == DT_FILMIC_COLORSCIENCE_V1)
      filmic_split_v1(in, out, work_profile, data, data->spline, roi_out->width, roi_in->height, ch);
    else if(data->version == DT_FILMIC_COLORSCIENCE_V2)
      filmic_split_v2(in, out, work_profile, data, data->spline, roi_out->width, roi_in->height, ch);
  }
  else
  {
    // chroma preservation
    if(data->version == DT_FILMIC_COLORSCIENCE_V1)
      filmic_chroma_v1(in, out, work_profile, data, data->spline, data->preserve_color, roi_out->width,
                       roi_out->height, ch);
    else if(data->version == DT_FILMIC_COLORSCIENCE_V2)
      filmic_chroma_v2(in, out, work_profile, data, data->spline, data->preserve_color, roi_out->width,
                       roi_out->height, ch);
  }

  if(reconstructed) dt_free_align(reconstructed);

  if(piece->pipe->mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK)
    dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);
}

#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_filmicrgb_data_t *const d = (dt_iop_filmicrgb_data_t *)piece->data;
  const dt_iop_order_iccprofile_info_t *const work_profile = dt_ioppr_get_pipe_work_profile_info(piece->pipe);
  dt_iop_filmicrgb_global_data_t *const gd = (dt_iop_filmicrgb_global_data_t *)self->global_data;
  const dt_iop_filmic_rgb_spline_t spline = (dt_iop_filmic_rgb_spline_t)d->spline;

  cl_int err = -999;

  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;
  const int use_work_profile = (work_profile == NULL) ? 0 : 1;

  cl_mem dev_profile_info = NULL;
  cl_mem dev_profile_lut = NULL;
  dt_colorspaces_iccprofile_info_cl_t *profile_info_cl;
  cl_float *profile_lut_cl = NULL;

  err = dt_ioppr_build_iccprofile_params_cl(work_profile, devid, &profile_info_cl, &profile_lut_cl,
                                            &dev_profile_info, &dev_profile_lut);
  if(err != CL_SUCCESS) goto error;

  size_t sizes[] = { ROUNDUPWD(width), ROUNDUPHT(height), 1 };

  if(d->preserve_color == DT_FILMIC_METHOD_NONE)
  {
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 0, sizeof(cl_mem), (void *)&dev_in);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 1, sizeof(cl_mem), (void *)&dev_out);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 2, sizeof(int), (void *)&width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 3, sizeof(int), (void *)&height);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 4, sizeof(float), (void *)&d->dynamic_range);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 5, sizeof(float), (void *)&d->black_source);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 6, sizeof(float), (void *)&d->grey_source);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 7, sizeof(cl_mem), (void *)&dev_profile_info);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 8, sizeof(cl_mem), (void *)&dev_profile_lut);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 9, sizeof(int), (void *)&use_work_profile);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 10, sizeof(float), (void *)&d->sigma_toe);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 11, sizeof(float), (void *)&d->sigma_shoulder);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 12, sizeof(float), (void *)&d->saturation);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 13, 4 * sizeof(float), (void *)&spline.M1);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 14, 4 * sizeof(float), (void *)&spline.M2);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 15, 4 * sizeof(float), (void *)&spline.M3);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 16, 4 * sizeof(float), (void *)&spline.M4);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 17, 4 * sizeof(float), (void *)&spline.M5);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 18, sizeof(float), (void *)&spline.latitude_min);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 19, sizeof(float), (void *)&spline.latitude_max);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_split, 20, sizeof(float), (void *)&d->output_power);

    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_filmic_rgb_split, sizes);
    dt_ioppr_free_iccprofile_params_cl(&profile_info_cl, &profile_lut_cl, &dev_profile_info, &dev_profile_lut);
    if(err != CL_SUCCESS) goto error;
    return TRUE;
  }
  else
  {
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 0, sizeof(cl_mem), (void *)&dev_in);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 1, sizeof(cl_mem), (void *)&dev_out);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 2, sizeof(int), (void *)&width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 3, sizeof(int), (void *)&height);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 4, sizeof(float), (void *)&d->dynamic_range);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 5, sizeof(float), (void *)&d->black_source);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 6, sizeof(float), (void *)&d->grey_source);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 7, sizeof(cl_mem), (void *)&dev_profile_info);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 8, sizeof(cl_mem), (void *)&dev_profile_lut);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 9, sizeof(int), (void *)&use_work_profile);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 10, sizeof(float), (void *)&d->sigma_toe);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 11, sizeof(float), (void *)&d->sigma_shoulder);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 12, sizeof(float), (void *)&d->saturation);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 13, 4 * sizeof(float), (void *)&spline.M1);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 14, 4 * sizeof(float), (void *)&spline.M2);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 15, 4 * sizeof(float), (void *)&spline.M3);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 16, 4 * sizeof(float), (void *)&spline.M4);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 17, 4 * sizeof(float), (void *)&spline.M5);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 18, sizeof(float), (void *)&spline.latitude_min);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 19, sizeof(float), (void *)&spline.latitude_max);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 20, sizeof(float), (void *)&d->output_power);
    dt_opencl_set_kernel_arg(devid, gd->kernel_filmic_rgb_chroma, 21, sizeof(int), (void *)&d->preserve_color);

    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_filmic_rgb_chroma, sizes);
    dt_ioppr_free_iccprofile_params_cl(&profile_info_cl, &profile_lut_cl, &dev_profile_info, &dev_profile_lut);
    if(err != CL_SUCCESS) goto error;
    return TRUE;
  }
error:
  dt_print(DT_DEBUG_OPENCL, "[opencl_filmicrgb] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif


static void apply_auto_grey(dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_filmicrgb_params_t *p = (dt_iop_filmicrgb_params_t *)self->params;
  dt_iop_filmicrgb_gui_data_t *g = (dt_iop_filmicrgb_gui_data_t *)self->gui_data;

  const dt_iop_order_iccprofile_info_t *const work_profile
      = dt_ioppr_get_iop_work_profile_info(self, self->dev->iop);
  const float grey = get_pixel_norm(self->picked_color, p->preserve_color, work_profile) / 2.0f;

  const float prev_grey = p->grey_point_source;
  p->grey_point_source = CLAMP(100.f * grey, 0.001f, 100.0f);
  const float grey_var = log2f(prev_grey / p->grey_point_source);
  p->black_point_source = p->black_point_source - grey_var;
  p->white_point_source = p->white_point_source + grey_var;
  p->output_power = logf(p->grey_point_target / 100.0f)
                    / logf(-p->black_point_source / (p->white_point_source - p->black_point_source));

  ++darktable.gui->reset;
  dt_bauhaus_slider_set_soft(g->grey_point_source, p->grey_point_source);
  dt_bauhaus_slider_set_soft(g->black_point_source, p->black_point_source);
  dt_bauhaus_slider_set_soft(g->white_point_source, p->white_point_source);
  dt_bauhaus_slider_set_soft(g->output_power, p->output_power);
  --darktable.gui->reset;

  gtk_widget_queue_draw(self->widget);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void apply_auto_black(dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_filmicrgb_params_t *p = (dt_iop_filmicrgb_params_t *)self->params;
  dt_iop_filmicrgb_gui_data_t *g = (dt_iop_filmicrgb_gui_data_t *)self->gui_data;

  // Black
  const dt_iop_order_iccprofile_info_t *const work_profile
      = dt_ioppr_get_iop_work_profile_info(self, self->dev->iop);
  const float black = get_pixel_norm(self->picked_color_min, DT_FILMIC_METHOD_MAX_RGB, work_profile);

  float EVmin = CLAMP(log2f(black / (p->grey_point_source / 100.0f)), -16.0f, -1.0f);
  EVmin *= (1.0f + p->security_factor / 100.0f);

  p->black_point_source = fmaxf(EVmin, -16.0f);
  p->output_power = logf(p->grey_point_target / 100.0f)
                    / logf(-p->black_point_source / (p->white_point_source - p->black_point_source));

  ++darktable.gui->reset;
  dt_bauhaus_slider_set_soft(g->black_point_source, p->black_point_source);
  dt_bauhaus_slider_set_soft(g->output_power, p->output_power);
  --darktable.gui->reset;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
  gtk_widget_queue_draw(self->widget);
}


static void apply_auto_white_point_source(dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_filmicrgb_params_t *p = (dt_iop_filmicrgb_params_t *)self->params;
  dt_iop_filmicrgb_gui_data_t *g = (dt_iop_filmicrgb_gui_data_t *)self->gui_data;

  // White
  const dt_iop_order_iccprofile_info_t *const work_profile
      = dt_ioppr_get_iop_work_profile_info(self, self->dev->iop);
  const float white = get_pixel_norm(self->picked_color_max, DT_FILMIC_METHOD_MAX_RGB, work_profile);

  float EVmax = CLAMP(log2f(white / (p->grey_point_source / 100.0f)), 1.0f, 16.0f);
  EVmax *= (1.0f + p->security_factor / 100.0f);

  p->white_point_source = EVmax;
  p->output_power = logf(p->grey_point_target / 100.0f)
                    / logf(-p->black_point_source / (p->white_point_source - p->black_point_source));

  ++darktable.gui->reset;
  dt_bauhaus_slider_set_soft(g->white_point_source, p->white_point_source);
  dt_bauhaus_slider_set_soft(g->output_power, p->output_power);
  --darktable.gui->reset;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
  gtk_widget_queue_draw(self->widget);
}

static void apply_autotune(dt_iop_module_t *self)
{
  dt_iop_filmicrgb_gui_data_t *g = (dt_iop_filmicrgb_gui_data_t *)self->gui_data;
  dt_iop_filmicrgb_params_t *p = (dt_iop_filmicrgb_params_t *)self->params;
  const dt_iop_order_iccprofile_info_t *const work_profile
      = dt_ioppr_get_iop_work_profile_info(self, self->dev->iop);

  // Grey
  if(p->custom_grey)
  {
    const float grey = get_pixel_norm(self->picked_color, p->preserve_color, work_profile) / 2.0f;
    p->grey_point_source = CLAMP(100.f * grey, 0.001f, 100.0f);
  }

  // White
  const float white = get_pixel_norm(self->picked_color_max, DT_FILMIC_METHOD_MAX_RGB, work_profile);
  float EVmax = CLAMP(log2f(white / (p->grey_point_source / 100.0f)), 1.0f, 16.0f);
  EVmax *= (1.0f + p->security_factor / 100.0f);

  // Black
  const float black = get_pixel_norm(self->picked_color_min, DT_FILMIC_METHOD_MAX_RGB, work_profile);
  float EVmin = CLAMP(log2f(black / (p->grey_point_source / 100.0f)), -16.0f, -1.0f);
  EVmin *= (1.0f + p->security_factor / 100.0f);

  p->black_point_source = fmaxf(EVmin, -16.0f);
  p->white_point_source = EVmax;
  p->output_power = logf(p->grey_point_target / 100.0f)
                    / logf(-p->black_point_source / (p->white_point_source - p->black_point_source));

  ++darktable.gui->reset;
  dt_bauhaus_slider_set_soft(g->grey_point_source, p->grey_point_source);
  dt_bauhaus_slider_set_soft(g->black_point_source, p->black_point_source);
  dt_bauhaus_slider_set_soft(g->white_point_source, p->white_point_source);
  dt_bauhaus_slider_set_soft(g->output_power, p->output_power);
  --darktable.gui->reset;

  gtk_widget_queue_draw(self->widget);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

void color_picker_apply(dt_iop_module_t *self, GtkWidget *picker, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_filmicrgb_gui_data_t *g = (dt_iop_filmicrgb_gui_data_t *)self->gui_data;

  if(picker == g->grey_point_source)
    apply_auto_grey(self);
  else if(picker == g->black_point_source)
    apply_auto_black(self);
  else if(picker == g->white_point_source)
    apply_auto_white_point_source(self);
  else if(picker == g->auto_button)
    apply_autotune(self);
}

static void show_mask_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(darktable.gui->reset) return;
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->off), TRUE);
  dt_iop_filmicrgb_gui_data_t *g = (dt_iop_filmicrgb_gui_data_t *)self->gui_data;
  g->show_mask = !(g->show_mask);
  dt_bauhaus_widget_set_quad_active(g->show_highlight_mask, g->show_mask);
  dt_bauhaus_widget_set_quad_toggle(g->show_highlight_mask, g->show_mask);
  dt_dev_reprocess_center(self->dev);
}


#define ORDER_4 5
#define ORDER_3 4


inline static void dt_iop_filmic_rgb_compute_spline(const dt_iop_filmicrgb_params_t *const p,
                                                    struct dt_iop_filmic_rgb_spline_t *const spline)
{
  float grey_display = 0.4638f;

  if(p->custom_grey)
  {
    // user set a custom value
    grey_display = powf(CLAMP(p->grey_point_target, p->black_point_target, p->white_point_target) / 100.0f,
                        1.0f / (p->output_power));
  }
  else
  {
    // use 18.45% grey and don't bother
    grey_display = powf(0.1845f, 1.0f / (p->output_power));
  }

  const float white_source = p->white_point_source;
  const float black_source = p->black_point_source;
  const float dynamic_range = white_source - black_source;

  // luminance after log encoding
  const float black_log = 0.0f; // assumes user set log as in the autotuner
  const float grey_log = fabsf(p->black_point_source) / dynamic_range;
  const float white_log = 1.0f; // assumes user set log as in the autotuner

  // target luminance desired after filmic curve
  float black_display, white_display;

  if(p->internal_version == 2019)
  {
    // this is a buggy version that doesn't take the output power function into account
    // it was silent because black and white display were set to 0 and 1 and users were advised to not touch them.
    // (since 0^x = 0 and 1^x = 1). It's not silent anymore if black display > 0,
    // for example if compensating ICC black level for target medium
    black_display = CLAMP(p->black_point_target, 0.0f, p->grey_point_target) / 100.0f; // in %
    white_display = fmaxf(p->white_point_target, p->grey_point_target) / 100.0f;       // in %
  }
  else //(p->internal_version == 2020)
  {
    // this is the fixed version
    black_display = powf(CLAMP(p->black_point_target, 0.0f, p->grey_point_target) / 100.0f,
                         1.0f / (p->output_power)); // in %
    white_display
        = powf(fmaxf(p->white_point_target, p->grey_point_target) / 100.0f, 1.0f / (p->output_power)); // in %
  }

  float latitude = CLAMP(p->latitude, 0.0f, 100.0f) / 100.0f * dynamic_range; // in % of dynamic range
  float balance = CLAMP(p->balance, -50.0f, 50.0f) / 100.0f;                  // in %
  float contrast = CLAMP(p->contrast, 0.1f, 2.0f);

  // nodes for mapping from log encoding to desired target luminance
  // X coordinates
  float toe_log = grey_log - latitude / dynamic_range * fabsf(black_source / dynamic_range);
  float shoulder_log = grey_log + latitude / dynamic_range * fabsf(white_source / dynamic_range);

  // interception
  float linear_intercept = grey_display - (contrast * grey_log);

  // y coordinates
  float toe_display = (toe_log * contrast + linear_intercept);
  float shoulder_display = (shoulder_log * contrast + linear_intercept);

  // Apply the highlights/shadows balance as a shift along the contrast slope
  const float norm = sqrtf(contrast * contrast + 1.0f);

  // negative values drag to the left and compress the shadows, on the UI negative is the inverse
  const float coeff = -((2.0f * latitude) / dynamic_range) * balance;

  toe_display += coeff * contrast / norm;
  shoulder_display += coeff * contrast / norm;
  toe_log += coeff / norm;
  shoulder_log += coeff / norm;

  /**
   * Now we have 3 segments :
   *  - x = [0.0 ; toe_log], curved part
   *  - x = [toe_log ; grey_log ; shoulder_log], linear part
   *  - x = [shoulder_log ; 1.0] curved part
   *
   * BUT : in case some nodes overlap, we need to remove them to avoid
   * degenerating of the curve
   **/

  // Build the curve from the nodes
  spline->x[0] = black_log;
  spline->x[1] = toe_log;
  spline->x[2] = grey_log;
  spline->x[3] = shoulder_log;
  spline->x[4] = white_log;

  spline->y[0] = black_display;
  spline->y[1] = toe_display;
  spline->y[2] = grey_display;
  spline->y[3] = shoulder_display;
  spline->y[4] = white_display;

  spline->latitude_min = spline->x[1];
  spline->latitude_max = spline->x[3];

  /**
   * For background and details, see :
   * https://eng.aurelienpierre.com/2018/11/30/filmic-darktable-and-the-quest-of-the-hdr-tone-mapping/#filmic_s_curve
   *
   **/
  const double Tl = spline->x[1];
  const double Tl2 = Tl * Tl;
  const double Tl3 = Tl2 * Tl;
  const double Tl4 = Tl3 * Tl;

  const double Sl = spline->x[3];
  const double Sl2 = Sl * Sl;
  const double Sl3 = Sl2 * Sl;
  const double Sl4 = Sl3 * Sl;

  // solve the linear central part - affine function
  spline->M2[2] = contrast;                                    // * x¹ (slope)
  spline->M1[2] = spline->y[1] - spline->M2[2] * spline->x[1]; // * x⁰ (offset)
  spline->M3[2] = 0.f;                                         // * x²
  spline->M4[2] = 0.f;                                         // * x³
  spline->M5[2] = 0.f;                                         // * x⁴

  // solve the toe part
  if(p->shadows == DT_FILMIC_CURVE_POLY_4)
  {
    // fourth order polynom - only mode in darktable 3.0.0
    double A0[ORDER_4 * ORDER_4] = { 0.,        0.,       0.,      0., 1.,   // position in 0
                                     0.,        0.,       0.,      1., 0.,   // first derivative in 0
                                     Tl4,       Tl3,      Tl2,     Tl, 1.,   // position at toe node
                                     4. * Tl3,  3. * Tl2, 2. * Tl, 1., 0.,   // first derivative at toe node
                                     12. * Tl2, 6. * Tl,  2.,      0., 0. }; // second derivative at toe node

    double b0[ORDER_4] = { spline->y[0], 0., spline->y[1], spline->M2[2], 0. };

    gauss_solve(A0, b0, ORDER_4);

    spline->M5[0] = b0[0]; // * x⁴
    spline->M4[0] = b0[1]; // * x³
    spline->M3[0] = b0[2]; // * x²
    spline->M2[0] = b0[3]; // * x¹
    spline->M1[0] = b0[4]; // * x⁰
  }
  else
  {
    // third order polynom
    double A0[ORDER_3 * ORDER_3] = { 0.,       0.,      0., 1.,   // position in 0
                                     Tl3,      Tl2,     Tl, 1.,   // position at toe node
                                     3. * Tl2, 2. * Tl, 1., 0.,   // first derivative at toe node
                                     6. * Tl,  2.,      0., 0. }; // second derivative at toe node

    double b0[ORDER_3] = { spline->y[0], spline->y[1], spline->M2[2], 0. };

    gauss_solve(A0, b0, ORDER_3);

    spline->M5[0] = 0.0f;  // * x⁴
    spline->M4[0] = b0[0]; // * x³
    spline->M3[0] = b0[1]; // * x²
    spline->M2[0] = b0[2]; // * x¹
    spline->M1[0] = b0[3]; // * x⁰
  }

  // solve the shoulder part
  if(p->highlights == DT_FILMIC_CURVE_POLY_3)
  {
    // 3rd order polynom - only mode in darktable 3.0.0
    double A1[ORDER_3 * ORDER_3] = { 1.,       1.,      1., 1.,   // position in 1
                                     Sl3,      Sl2,     Sl, 1.,   // position at shoulder node
                                     3. * Sl2, 2. * Sl, 1., 0.,   // first derivative at shoulder node
                                     6. * Sl,  2.,      0., 0. }; // second derivative at shoulder node

    double b1[ORDER_3] = { spline->y[4], spline->y[3], spline->M2[2], 0. };

    gauss_solve(A1, b1, ORDER_3);

    spline->M5[1] = 0.0f;  // * x⁴
    spline->M4[1] = b1[0]; // * x³
    spline->M3[1] = b1[1]; // * x²
    spline->M2[1] = b1[2]; // * x¹
    spline->M1[1] = b1[3]; // * x⁰
  }
  else
  {
    // 4th order polynom
    double A1[ORDER_4 * ORDER_4] = { 1.,        1.,       1.,      1., 1.,   // position in 1
                                     4.,        3.,       2.,      1., 0.,   // first derivative in 1
                                     Sl4,       Sl3,      Sl2,     Sl, 1.,   // position at shoulder node
                                     4. * Sl3,  3. * Sl2, 2. * Sl, 1., 0.,   // first derivative at shoulder node
                                     12. * Sl2, 6. * Sl,  2.,      0., 0. }; // second derivative at shoulder node

    double b1[ORDER_4] = { spline->y[4], 0., spline->y[3], spline->M2[2], 0. };

    gauss_solve(A1, b1, ORDER_4);

    spline->M5[1] = b1[0]; // * x⁴
    spline->M4[1] = b1[1]; // * x³
    spline->M3[1] = b1[2]; // * x²
    spline->M2[1] = b1[3]; // * x¹
    spline->M1[1] = b1[4]; // * x⁰
  }
}

void commit_params(dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_filmicrgb_params_t *p = (dt_iop_filmicrgb_params_t *)p1;
  dt_iop_filmicrgb_data_t *d = (dt_iop_filmicrgb_data_t *)piece->data;

  // source and display greys
  float grey_source = 0.1845f, grey_display = 0.4638f;
  if(p->custom_grey)
  {
    // user set a custom value
    grey_source = p->grey_point_source / 100.0f; // in %
    grey_display = powf(p->grey_point_target / 100.0f, 1.0f / (p->output_power));
  }
  else
  {
    // use 18.45% grey and don't bother
    grey_source = 0.1845f; // in %
    grey_display = powf(0.1845f, 1.0f / (p->output_power));
  }

  // source luminance - Used only in the log encoding
  const float white_source = p->white_point_source;
  const float black_source = p->black_point_source;
  const float dynamic_range = white_source - black_source;

  // luminance after log encoding
  const float grey_log = fabsf(p->black_point_source) / dynamic_range;


  float contrast = p->contrast;
  if(contrast < grey_display / grey_log)
  {
    // We need grey_display - (contrast * grey_log) <= 0.0
    contrast = 1.0001f * grey_display / grey_log;
  }

  // commit
  d->dynamic_range = dynamic_range;
  d->black_source = black_source;
  d->grey_source = grey_source;
  d->output_power = p->output_power;
  d->contrast = contrast;
  d->version = p->version;
  d->preserve_color = p->preserve_color;
  d->high_quality_reconstruction = p->high_quality_reconstruction;
  d->noise_level = p->noise_level;
  d->noise_distribution = p->noise_distribution;

  // TODO: write OpenCL for v2
  piece->process_cl_ready = FALSE;


  // compute the curves and their LUT
  dt_iop_filmic_rgb_compute_spline(p, &d->spline);

  d->saturation = (2.0f * p->saturation / 100.0f + 1.0f);
  d->sigma_toe = powf(d->spline.latitude_min / 3.0f, 2.0f);
  d->sigma_shoulder = powf((1.0f - d->spline.latitude_max) / 3.0f, 2.0f);

  d->reconstruct_threshold = powf(2.0f, white_source + p->reconstruct_threshold) * grey_source;
  d->reconstruct_feather = exp2f(12.f / p->reconstruct_feather);

  // offset and rescale user param to alpha blending so 0 -> 50% and 1 -> 100%
  d->reconstruct_structure_vs_texture = (p->reconstruct_structure_vs_texture / 100.0f + 1.f) / 2.f;
  d->reconstruct_bloom_vs_details = (p->reconstruct_bloom_vs_details / 100.0f + 1.f) / 2.f;
  d->reconstruct_grey_vs_color = (p->reconstruct_grey_vs_color / 100.0f + 1.f) / 2.f;
}

void gui_focus(struct dt_iop_module_t *self, gboolean in)
{
  dt_iop_filmicrgb_gui_data_t *g = (dt_iop_filmicrgb_gui_data_t *)self->gui_data;

  if(!in)
  {
    // lost focus - hide the mask
    g->show_mask = FALSE;
    dt_bauhaus_widget_set_quad_toggle(g->show_highlight_mask, FALSE);
    dt_bauhaus_widget_set_quad_active(g->show_highlight_mask, FALSE);
    dt_dev_reprocess_center(self->dev);
  }
}
void init_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1, sizeof(dt_iop_filmicrgb_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void gui_update(dt_iop_module_t *self)
{
  dt_iop_filmicrgb_gui_data_t *g = (dt_iop_filmicrgb_gui_data_t *)self->gui_data;
  dt_iop_filmicrgb_params_t *p = (dt_iop_filmicrgb_params_t *)self->params;

  dt_iop_color_picker_reset(self, TRUE);

  g->show_mask = FALSE;
  g->gui_mode = dt_conf_get_int("plugins/darkroom/filmicrgb/graph_view");
  g->gui_show_labels = dt_conf_get_int("plugins/darkroom/filmicrgb/graph_show_labels");
  g->gui_hover = FALSE;
  g->gui_sizes_inited = FALSE;

  // fetch last view in dartablerc


  self->color_picker_box[0] = self->color_picker_box[1] = .25f;
  self->color_picker_box[2] = self->color_picker_box[3] = .50f;
  self->color_picker_point[0] = self->color_picker_point[1] = 0.5f;

  dt_bauhaus_slider_set_soft(g->white_point_source, p->white_point_source);
  dt_bauhaus_slider_set_soft(g->grey_point_source, p->grey_point_source);
  dt_bauhaus_slider_set_soft(g->black_point_source, p->black_point_source);
  dt_bauhaus_slider_set_soft(g->security_factor, p->security_factor);
  dt_bauhaus_slider_set_soft(g->reconstruct_threshold, p->reconstruct_threshold);
  dt_bauhaus_slider_set_soft(g->reconstruct_feather, p->reconstruct_feather);
  dt_bauhaus_slider_set_soft(g->reconstruct_bloom_vs_details, p->reconstruct_bloom_vs_details);
  dt_bauhaus_slider_set_soft(g->reconstruct_grey_vs_color, p->reconstruct_grey_vs_color);
  dt_bauhaus_slider_set_soft(g->reconstruct_structure_vs_texture, p->reconstruct_structure_vs_texture);
  dt_bauhaus_slider_set_soft(g->white_point_target, p->white_point_target);
  dt_bauhaus_slider_set_soft(g->grey_point_target, p->grey_point_target);
  dt_bauhaus_slider_set_soft(g->black_point_target, p->black_point_target);
  dt_bauhaus_slider_set_soft(g->output_power, p->output_power);
  dt_bauhaus_slider_set_soft(g->latitude, p->latitude);
  dt_bauhaus_slider_set_soft(g->contrast, p->contrast);
  dt_bauhaus_slider_set_soft(g->saturation, p->saturation);
  dt_bauhaus_slider_set_soft(g->balance, p->balance);

  dt_bauhaus_combobox_set(g->version, p->version);
  dt_bauhaus_combobox_set(g->preserve_color, p->preserve_color);
  dt_bauhaus_combobox_set(g->shadows, p->shadows);
  dt_bauhaus_combobox_set(g->highlights, p->highlights);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->auto_hardness), p->auto_hardness);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->custom_grey), p->custom_grey);

  dt_bauhaus_slider_set_soft(g->high_quality_reconstruction, p->high_quality_reconstruction);
  dt_bauhaus_slider_set_soft(g->noise_level, p->noise_level);
  dt_bauhaus_combobox_set(g->noise_distribution, p->noise_distribution);

  gui_changed(self, NULL, NULL);

  gtk_widget_queue_draw(self->widget);
}

void reload_defaults(dt_iop_module_t *module)
{
  dt_iop_filmicrgb_params_t *d = module->default_params;

  d->black_point_source = module->so->get_f("black_point_source")->Float.Default;
  d->white_point_source = module->so->get_f("white_point_source")->Float.Default;
  d->output_power = module->so->get_f("output_power")->Float.Default;

  module->default_enabled = FALSE;

  gchar *workflow = dt_conf_get_string("plugins/darkroom/workflow");
  const gboolean is_scene_referred = strcmp(workflow, "scene-referred") == 0;
  g_free(workflow);

  if(dt_image_is_matrix_correction_supported(&module->dev->image_storage) && is_scene_referred)
  {
    // For scene-referred workflow, auto-enable and adjust based on exposure
    // TODO: fetch actual exposure in module, don't assume 1.
    const float exposure = 0.5f - dt_image_get_exposure_bias(&module->dev->image_storage);

    // As global exposure increases, white exposure increases faster than black
    // this is probably because raw black/white points offsets the lower bound of the dynamic range to 0
    // so exposure compensation actually increases the dynamic range too (stretches only white).
    d->black_point_source += 0.5f * exposure;
    d->white_point_source += 0.8f * exposure;
    d->output_power = logf(d->grey_point_target / 100.0f)
                      / logf(-d->black_point_source / (d->white_point_source - d->black_point_source));
  }
}


void init_global(dt_iop_module_so_t *module)
{
  const int program = 22; // filmic.cl, from programs.conf
  dt_iop_filmicrgb_global_data_t *gd
      = (dt_iop_filmicrgb_global_data_t *)malloc(sizeof(dt_iop_filmicrgb_global_data_t));

  module->data = gd;
  gd->kernel_filmic_rgb_split = dt_opencl_create_kernel(program, "filmicrgb_split");
  gd->kernel_filmic_rgb_chroma = dt_opencl_create_kernel(program, "filmicrgb_chroma");
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_filmicrgb_global_data_t *gd = (dt_iop_filmicrgb_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_filmic_rgb_split);
  dt_opencl_free_kernel(gd->kernel_filmic_rgb_chroma);
  free(module->data);
  module->data = NULL;
}


void gui_reset(dt_iop_module_t *self)
{
  dt_iop_color_picker_reset(self, TRUE);
}

#define LOGBASE 20.f

static inline void dt_cairo_draw_arrow(cairo_t *cr, double origin_x, double origin_y, double destination_x,
                                       double destination_y, gboolean show_head)
{
  cairo_move_to(cr, origin_x, origin_y);
  cairo_line_to(cr, destination_x, destination_y);
  cairo_stroke(cr);

  if(show_head)
  {
    // arrow head is hard set to 45° - convert to radians
    const float angle_arrow = 45.f / 360.f * M_PI;
    const float angle_trunk = atan2f((destination_y - origin_y), (destination_x - origin_x));
    const float radius = DT_PIXEL_APPLY_DPI(3);

    const float x_1 = destination_x + radius / sinf(angle_arrow + angle_trunk);
    const float y_1 = destination_y + radius / cosf(angle_arrow + angle_trunk);

    const float x_2 = destination_x - radius / sinf(-angle_arrow + angle_trunk);
    const float y_2 = destination_y - radius / cosf(-angle_arrow + angle_trunk);

    cairo_move_to(cr, x_1, y_1);
    cairo_line_to(cr, destination_x, destination_y);
    cairo_line_to(cr, x_2, y_2);
    cairo_stroke(cr);
  }
}

void filmic_gui_draw_icon(cairo_t *cr, struct dt_iop_filmicrgb_gui_button_data_t *button,
                          struct dt_iop_filmicrgb_gui_data_t *g)
{
  if(!g->gui_sizes_inited) return;

  cairo_save(cr);

  GdkRGBA color;

  // copy color
  color.red = darktable.bauhaus->graph_fg.red;
  color.green = darktable.bauhaus->graph_fg.green;
  color.blue = darktable.bauhaus->graph_fg.blue;
  color.alpha = darktable.bauhaus->graph_fg.alpha;

  if(button->mouse_hover)
  {
    // use graph_fg color as-is if mouse hover
    cairo_set_source_rgba(cr, color.red, color.green, color.blue, color.alpha);
  }
  else
  {
    // use graph_fg color with transparency else
    cairo_set_source_rgba(cr, color.red, color.green, color.blue, color.alpha * 0.5);
  }

  cairo_rectangle(cr, button->left, button->top, button->w - DT_PIXEL_APPLY_DPI(0.5),
                  button->h - DT_PIXEL_APPLY_DPI(0.5));
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.));
  cairo_stroke(cr);
  cairo_translate(cr, button->left + button->w / 2. - DT_PIXEL_APPLY_DPI(0.25),
                  button->top + button->h / 2. - DT_PIXEL_APPLY_DPI(0.25));

  const float scale = 0.85;
  cairo_scale(cr, scale, scale);
  button->icon(cr, -scale * button->w / 2., -scale * button->h / 2., scale * button->w, scale * button->h,
               CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
  cairo_restore(cr);
}


static gboolean dt_iop_tonecurve_draw(GtkWidget *widget, cairo_t *crf, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_filmicrgb_params_t *p = (dt_iop_filmicrgb_params_t *)self->params;
  dt_iop_filmicrgb_gui_data_t *g = (dt_iop_filmicrgb_gui_data_t *)self->gui_data;
  dt_iop_filmic_rgb_compute_spline(p, &g->spline);

  // Cache the graph objects to avoid recomputing all the view at each redraw
  gtk_widget_get_allocation(widget, &g->allocation);
  g->cst = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, g->allocation.width, g->allocation.height);
  g->cr = cairo_create(g->cst);
  g->layout = pango_cairo_create_layout(g->cr);
  g->desc = pango_font_description_copy_static(darktable.bauhaus->pango_font_desc);
  pango_layout_set_font_description(g->layout, g->desc);
  pango_cairo_context_set_resolution(pango_layout_get_context(g->layout), darktable.gui->dpi);
  g->context = gtk_widget_get_style_context(widget);

  char text[256];

  // reduce a bit the font size
  const gint font_size = pango_font_description_get_size(g->desc);
  pango_font_description_set_size(g->desc, 0.95 * font_size);
  pango_layout_set_font_description(g->layout, g->desc);

  // Get the text line height for spacing
  g_strlcpy(text, "X", sizeof(text));
  pango_layout_set_text(g->layout, text, -1);
  pango_layout_get_pixel_extents(g->layout, &g->ink, NULL);
  g->line_height = g->ink.height;

  // Get the width of a minus sign for legend labels spacing
  g_strlcpy(text, "-", sizeof(text));
  pango_layout_set_text(g->layout, text, -1);
  pango_layout_get_pixel_extents(g->layout, &g->ink, NULL);
  g->sign_width = g->ink.width / 2.0;

  // Get the width of a zero for legend labels spacing
  g_strlcpy(text, "0", sizeof(text));
  pango_layout_set_text(g->layout, text, -1);
  pango_layout_get_pixel_extents(g->layout, &g->ink, NULL);
  g->zero_width = g->ink.width;

  // Set the sizes, margins and paddings
  g->inner_padding = DT_PIXEL_APPLY_DPI(4); // TODO: INNER_PADDING value as defined in bauhaus.c macros, sync them
  g->inset = g->inner_padding;

  float margin_left;
  float margin_bottom;
  if(g->gui_show_labels)
  {
    // leave room for labels
    margin_left = 3. * g->zero_width + 2. * g->inset;
    margin_bottom = 2. * g->line_height + 4. * g->inset;
  }
  else
  {
    margin_left = g->inset;
    margin_bottom = g->inset;
  }

  const float margin_top = 2. * g->line_height + g->inset;
  const float margin_right = darktable.bauhaus->quad_width + 2. * g->inset;

  g->graph_width = g->allocation.width - margin_right - margin_left;   // align the right border on sliders
  g->graph_height = g->allocation.height - margin_bottom - margin_top; // give room to nodes

  gtk_render_background(g->context, g->cr, 0, 0, g->allocation.width, g->allocation.height);

  // Init icons bounds and cache them for mouse events
  for(int i = 0; i < DT_FILMIC_GUI_BUTTON_LAST; i++)
  {
    // put the buttons in the right margin and increment vertical position
    g->buttons[i].right = g->allocation.width;
    g->buttons[i].left = g->buttons[i].right - darktable.bauhaus->quad_width;
    g->buttons[i].top = margin_top + i * (g->inset + darktable.bauhaus->quad_width);
    g->buttons[i].bottom = g->buttons[i].top + darktable.bauhaus->quad_width;
    g->buttons[i].w = g->buttons[i].right - g->buttons[i].left;
    g->buttons[i].h = g->buttons[i].bottom - g->buttons[i].top;
    g->buttons[i].state = GTK_STATE_FLAG_NORMAL;
  }

  g->gui_sizes_inited = TRUE;

  g->buttons[0].icon = dtgtk_cairo_paint_refresh;
  g->buttons[1].icon = dtgtk_cairo_paint_text_label;

  if(g->gui_hover)
  {
    for(int i = 0; i < DT_FILMIC_GUI_BUTTON_LAST; i++) filmic_gui_draw_icon(g->cr, &g->buttons[i], g);
  }

  const float grey = p->grey_point_source / 100.f;
  const float DR = p->white_point_source - p->black_point_source;

  // set the graph as the origin of the coordinates
  cairo_translate(g->cr, margin_left, margin_top);

  cairo_set_line_cap(g->cr, CAIRO_LINE_CAP_ROUND);

  // write the graph legend at GUI default size
  pango_font_description_set_size(g->desc, font_size);
  pango_layout_set_font_description(g->layout, g->desc);
  if(g->gui_mode == DT_FILMIC_GUI_LOOK)
    g_strlcpy(text, _("look only"), sizeof(text));
  else if(g->gui_mode == DT_FILMIC_GUI_BASECURVE)
    g_strlcpy(text, _("look + mapping (lin)"), sizeof(text));
  else if(g->gui_mode == DT_FILMIC_GUI_BASECURVE_LOG)
    g_strlcpy(text, _("look + mapping (log)"), sizeof(text));
  else if(g->gui_mode == DT_FILMIC_GUI_RANGES)
    g_strlcpy(text, _("dynamic range mapping"), sizeof(text));

  pango_layout_set_text(g->layout, text, -1);
  pango_layout_get_pixel_extents(g->layout, &g->ink, NULL);

  // legend background
  set_color(g->cr, darktable.bauhaus->graph_bg);
  cairo_rectangle(g->cr, g->allocation.width - margin_left - g->ink.width - g->ink.x - 2. * g->inset,
                  -g->line_height - g->inset - 0.5 * g->ink.height - g->ink.y - g->inset,
                  g->ink.width + 3. * g->inset, g->ink.height + 2. * g->inset);
  cairo_fill(g->cr);

  // legend text
  set_color(g->cr, darktable.bauhaus->graph_fg);
  cairo_move_to(g->cr, g->allocation.width - margin_left - g->ink.width - g->ink.x - g->inset,
                -g->line_height - g->inset - 0.5 * g->ink.height - g->ink.y);
  pango_cairo_show_layout(g->cr, g->layout);
  cairo_stroke(g->cr);

  // reduce font size for the rest of the graph
  pango_font_description_set_size(g->desc, 0.95 * font_size);
  pango_layout_set_font_description(g->layout, g->desc);

  if(g->gui_mode != DT_FILMIC_GUI_RANGES)
  {
    // Draw graph background then border
    cairo_set_line_width(g->cr, DT_PIXEL_APPLY_DPI(0.5));
    cairo_rectangle(g->cr, 0, 0, g->graph_width, g->graph_height);
    set_color(g->cr, darktable.bauhaus->graph_bg);
    cairo_fill_preserve(g->cr);
    set_color(g->cr, darktable.bauhaus->graph_border);
    cairo_stroke(g->cr);

    // draw grid
    cairo_set_line_width(g->cr, DT_PIXEL_APPLY_DPI(0.5));
    set_color(g->cr, darktable.bauhaus->graph_border);

    // we need to tweak the coordinates system to match dt_draw_grid expectations
    cairo_save(g->cr);
    cairo_scale(g->cr, 1., -1.);
    cairo_translate(g->cr, 0., -g->graph_height);

    if(g->gui_mode == DT_FILMIC_GUI_LOOK || g->gui_mode == DT_FILMIC_GUI_BASECURVE)
      dt_draw_grid(g->cr, 4, 0, 0, g->graph_width, g->graph_height);
    else if(g->gui_mode == DT_FILMIC_GUI_BASECURVE_LOG)
      dt_draw_loglog_grid(g->cr, 4, 0, 0, g->graph_width, g->graph_height, LOGBASE);

    // reset coordinates
    cairo_restore(g->cr);

    // draw identity line
    cairo_move_to(g->cr, 0, g->graph_height);
    cairo_line_to(g->cr, g->graph_width, 0);
    cairo_stroke(g->cr);

    cairo_set_line_width(g->cr, DT_PIXEL_APPLY_DPI(2.));

    // Draw the saturation curve
    const float saturation = (2.0f * p->saturation / 100.0f + 1.0f);
    const float sigma_toe = powf(g->spline.latitude_min / 3.0f, 2.0f);
    const float sigma_shoulder = powf((1.0f - g->spline.latitude_max) / 3.0f, 2.0f);

    cairo_set_source_rgb(g->cr, .5, .5, .5);

    // prevent graph overflowing
    cairo_save(g->cr);
    cairo_rectangle(g->cr, -DT_PIXEL_APPLY_DPI(2.), -DT_PIXEL_APPLY_DPI(2.),
                    g->graph_width + 2. * DT_PIXEL_APPLY_DPI(2.), g->graph_height + 2. * DT_PIXEL_APPLY_DPI(2.));
    cairo_clip(g->cr);

    if(p->version == DT_FILMIC_COLORSCIENCE_V1)
    {
      cairo_move_to(g->cr, 0,
                    g->graph_height * (1.0 - filmic_desaturate_v1(0.0f, sigma_toe, sigma_shoulder, saturation)));
      for(int k = 1; k < 256; k++)
      {
        float x = k / 255.0;
        const float y = filmic_desaturate_v1(x, sigma_toe, sigma_shoulder, saturation);

        if(g->gui_mode == DT_FILMIC_GUI_BASECURVE)
          x = exp_tonemapping_v2(x, grey, p->black_point_source, DR);
        else if(g->gui_mode == DT_FILMIC_GUI_BASECURVE_LOG)
          x = dt_log_scale_axis(exp_tonemapping_v2(x, grey, p->black_point_source, DR), LOGBASE);

        cairo_line_to(g->cr, x * g->graph_width, g->graph_height * (1.0 - y));
      }
    }
    else if(p->version == DT_FILMIC_COLORSCIENCE_V2)
    {
      cairo_move_to(g->cr, 0,
                    g->graph_height * (1.0 - filmic_desaturate_v2(0.0f, sigma_toe, sigma_shoulder, saturation)));
      for(int k = 1; k < 256; k++)
      {
        float x = k / 255.0;
        const float y = filmic_desaturate_v2(x, sigma_toe, sigma_shoulder, saturation);

        if(g->gui_mode == DT_FILMIC_GUI_BASECURVE)
          x = exp_tonemapping_v2(x, grey, p->black_point_source, DR);
        else if(g->gui_mode == DT_FILMIC_GUI_BASECURVE_LOG)
          x = dt_log_scale_axis(exp_tonemapping_v2(x, grey, p->black_point_source, DR), LOGBASE);

        cairo_line_to(g->cr, x * g->graph_width, g->graph_height * (1.0 - y));
      }
    }
    cairo_stroke(g->cr);

    // draw the tone curve
    float x_start = 0.f;
    if(g->gui_mode == DT_FILMIC_GUI_BASECURVE || g->gui_mode == DT_FILMIC_GUI_BASECURVE_LOG)
      x_start = log_tonemapping_v2(x_start, grey, p->black_point_source, DR);

    if(g->gui_mode == DT_FILMIC_GUI_BASECURVE_LOG) x_start = dt_log_scale_axis(x_start, LOGBASE);

    float y_start = clamp_simd(filmic_spline(x_start, g->spline.M1, g->spline.M2, g->spline.M3, g->spline.M4,
                                             g->spline.M5, g->spline.latitude_min, g->spline.latitude_max));

    if(g->gui_mode == DT_FILMIC_GUI_BASECURVE)
      y_start = powf(y_start, p->output_power);
    else if(g->gui_mode == DT_FILMIC_GUI_BASECURVE_LOG)
      y_start = dt_log_scale_axis(powf(y_start, p->output_power), LOGBASE);

    cairo_move_to(g->cr, 0, g->graph_height * (1.0 - y_start));

    for(int k = 1; k < 256; k++)
    {
      // k / 255 step defines a linearly scaled space. This might produce large gaps in lowlights when using log
      // GUI scaling so we non-linearly rescale that step to get more points in lowlights
      float x = powf(k / 255.0f, 2.4f);
      float value = x;

      if(g->gui_mode == DT_FILMIC_GUI_BASECURVE || g->gui_mode == DT_FILMIC_GUI_BASECURVE_LOG)
        value = log_tonemapping_v2(x, grey, p->black_point_source, DR);

      if(g->gui_mode == DT_FILMIC_GUI_BASECURVE_LOG) x = dt_log_scale_axis(x, LOGBASE);

      float y = filmic_spline(value, g->spline.M1, g->spline.M2, g->spline.M3, g->spline.M4, g->spline.M5,
                              g->spline.latitude_min, g->spline.latitude_max);

      if(y > g->spline.y[4])
      {
        y = fminf(y, 1.0f);
        cairo_set_source_rgb(g->cr, 0.75, .5, 0.);
      }
      else if(y < g->spline.y[0])
      {
        y = fmaxf(y, 0.f);
        cairo_set_source_rgb(g->cr, 0.75, .5, 0.);
      }
      else
      {
        set_color(g->cr, darktable.bauhaus->graph_fg);
      }

      if(g->gui_mode == DT_FILMIC_GUI_BASECURVE)
        y = powf(y, p->output_power);
      else if(g->gui_mode == DT_FILMIC_GUI_BASECURVE_LOG)
        y = dt_log_scale_axis(powf(y, p->output_power), LOGBASE);

      cairo_line_to(g->cr, x * g->graph_width, g->graph_height * (1.0 - y));
      cairo_stroke(g->cr);
      cairo_move_to(g->cr, x * g->graph_width, g->graph_height * (1.0 - y));
    }

    cairo_restore(g->cr);

    // draw nodes

    // special case for the grey node
    cairo_save(g->cr);
    cairo_rectangle(g->cr, -DT_PIXEL_APPLY_DPI(4.), -DT_PIXEL_APPLY_DPI(4.),
                    g->graph_width + 2. * DT_PIXEL_APPLY_DPI(4.), g->graph_height + 2. * DT_PIXEL_APPLY_DPI(4.));
    cairo_clip(g->cr);
    float x_grey = g->spline.x[2];
    float y_grey = g->spline.y[2];

    if(g->gui_mode == DT_FILMIC_GUI_BASECURVE)
    {
      x_grey = exp_tonemapping_v2(x_grey, grey, p->black_point_source, DR);
      y_grey = powf(y_grey, p->output_power);
    }
    else if(g->gui_mode == DT_FILMIC_GUI_BASECURVE_LOG)
    {
      x_grey = dt_log_scale_axis(exp_tonemapping_v2(x_grey, grey, p->black_point_source, DR), LOGBASE);
      y_grey = dt_log_scale_axis(powf(y_grey, p->output_power), LOGBASE);
    }

    cairo_set_source_rgb(g->cr, 0.75, 0.5, 0.0);
    cairo_arc(g->cr, x_grey * g->graph_width, (1.0 - y_grey) * g->graph_height, DT_PIXEL_APPLY_DPI(6), 0,
              2. * M_PI);
    cairo_fill(g->cr);
    cairo_stroke(g->cr);

    // latitude nodes
    float x_black = 0.f;
    float y_black = 0.f;

    float x_white = 1.f;
    float y_white = 1.f;

    set_color(g->cr, darktable.bauhaus->graph_fg);
    for(int k = 0; k < 5; k++)
    {
      if(k != 2) // k == 2 : grey point, already processed above
      {
        float x = g->spline.x[k];
        float y = g->spline.y[k];

        if(g->gui_mode == DT_FILMIC_GUI_BASECURVE)
        {
          x = exp_tonemapping_v2(x, grey, p->black_point_source, DR);
          y = powf(y, p->output_power);
        }
        else if(g->gui_mode == DT_FILMIC_GUI_BASECURVE_LOG)
        {
          x = dt_log_scale_axis(exp_tonemapping_v2(x, grey, p->black_point_source, DR), LOGBASE);
          y = dt_log_scale_axis(powf(y, p->output_power), LOGBASE);
        }

        // save the bounds of the curve to mark the axis graduation
        if(k == 0) // black point
        {
          x_black = x;
          y_black = y;
        }
        else if(k == 4) // white point
        {
          x_white = x;
          y_white = y;
        }

        // draw bullet
        cairo_arc(g->cr, x * g->graph_width, (1.0 - y) * g->graph_height, DT_PIXEL_APPLY_DPI(4), 0, 2. * M_PI);
        cairo_fill(g->cr);
        cairo_stroke(g->cr);
      }
    }
    cairo_restore(g->cr);

    if(g->gui_show_labels)
    {
      // position of the upper bound of x axis labels
      const float x_legend_top = g->graph_height + 0.5 * g->line_height;

      // mark the y axis graduation at grey spot
      set_color(g->cr, darktable.bauhaus->graph_fg);
      snprintf(text, sizeof(text), "%.0f", p->grey_point_target);
      pango_layout_set_text(g->layout, text, -1);
      pango_layout_get_pixel_extents(g->layout, &g->ink, NULL);
      cairo_move_to(g->cr, -2. * g->inset - g->ink.width - g->ink.x,
                    (1.0 - y_grey) * g->graph_height - 0.5 * g->ink.height - g->ink.y);
      pango_cairo_show_layout(g->cr, g->layout);
      cairo_stroke(g->cr);

      // mark the x axis graduation at grey spot
      set_color(g->cr, darktable.bauhaus->graph_fg);
      if(g->gui_mode == DT_FILMIC_GUI_LOOK)
        snprintf(text, sizeof(text), "%+.1f", 0.f);
      else if(g->gui_mode == DT_FILMIC_GUI_BASECURVE || g->gui_mode == DT_FILMIC_GUI_BASECURVE_LOG)
        snprintf(text, sizeof(text), "%.0f", p->grey_point_source);

      pango_layout_set_text(g->layout, text, -1);
      pango_layout_get_pixel_extents(g->layout, &g->ink, NULL);
      cairo_move_to(g->cr, x_grey * g->graph_width - 0.5 * g->ink.width - g->ink.x, x_legend_top);
      pango_cairo_show_layout(g->cr, g->layout);
      cairo_stroke(g->cr);

      // mark the y axis graduation at black spot
      set_color(g->cr, darktable.bauhaus->graph_fg);
      snprintf(text, sizeof(text), "%.0f", p->black_point_target);
      pango_layout_set_text(g->layout, text, -1);
      pango_layout_get_pixel_extents(g->layout, &g->ink, NULL);
      cairo_move_to(g->cr, -2. * g->inset - g->ink.width - g->ink.x,
                    (1.0 - y_black) * g->graph_height - 0.5 * g->ink.height - g->ink.y);
      pango_cairo_show_layout(g->cr, g->layout);
      cairo_stroke(g->cr);

      // mark the y axis graduation at black spot
      set_color(g->cr, darktable.bauhaus->graph_fg);
      snprintf(text, sizeof(text), "%.0f", p->white_point_target);
      pango_layout_set_text(g->layout, text, -1);
      pango_layout_get_pixel_extents(g->layout, &g->ink, NULL);
      cairo_move_to(g->cr, -2. * g->inset - g->ink.width - g->ink.x,
                    (1.0 - y_white) * g->graph_height - 0.5 * g->ink.height - g->ink.y);
      pango_cairo_show_layout(g->cr, g->layout);
      cairo_stroke(g->cr);

      // mark the x axis graduation at black spot
      set_color(g->cr, darktable.bauhaus->graph_fg);
      if(g->gui_mode == DT_FILMIC_GUI_LOOK)
        snprintf(text, sizeof(text), "%+.1f", p->black_point_source);
      else if(g->gui_mode == DT_FILMIC_GUI_BASECURVE || g->gui_mode == DT_FILMIC_GUI_BASECURVE_LOG)
        snprintf(text, sizeof(text), "%.0f", exp2f(p->black_point_source) * p->grey_point_source);

      pango_layout_set_text(g->layout, text, -1);
      pango_layout_get_pixel_extents(g->layout, &g->ink, NULL);
      cairo_move_to(g->cr, x_black * g->graph_width - 0.5 * g->ink.width - g->ink.x, x_legend_top);
      pango_cairo_show_layout(g->cr, g->layout);
      cairo_stroke(g->cr);

      // mark the x axis graduation at white spot
      set_color(g->cr, darktable.bauhaus->graph_fg);
      if(g->gui_mode == DT_FILMIC_GUI_LOOK)
        snprintf(text, sizeof(text), "%+.1f", p->white_point_source);
      else if(g->gui_mode == DT_FILMIC_GUI_BASECURVE || g->gui_mode == DT_FILMIC_GUI_BASECURVE_LOG)
      {
        if(x_white > 1.f)
          snprintf(text, sizeof(text), "%.0f →", 100.f); // this marks the bound of the graph, not the actual white
        else
          snprintf(text, sizeof(text), "%.0f", exp2f(p->white_point_source) * p->grey_point_source);
      }

      pango_layout_set_text(g->layout, text, -1);
      pango_layout_get_pixel_extents(g->layout, &g->ink, NULL);
      cairo_move_to(g->cr,
                    fminf(x_white, 1.f) * g->graph_width - 0.5 * g->ink.width - g->ink.x
                        + 2. * (x_white > 1.f) * g->sign_width,
                    x_legend_top);
      pango_cairo_show_layout(g->cr, g->layout);
      cairo_stroke(g->cr);

      // handle the case where white > 100 %, so the node is out of the graph.
      // we still want to display the value to get a hint.
      set_color(g->cr, darktable.bauhaus->graph_fg);
      if((g->gui_mode == DT_FILMIC_GUI_BASECURVE || g->gui_mode == DT_FILMIC_GUI_BASECURVE_LOG) && (x_white > 1.f))
      {
        // set to italic font
        PangoStyle backup = pango_font_description_get_style(g->desc);
        pango_font_description_set_style(g->desc, PANGO_STYLE_ITALIC);
        pango_layout_set_font_description(g->layout, g->desc);

        snprintf(text, sizeof(text), _("(%.0f %%)"), exp2f(p->white_point_source) * p->grey_point_source);
        pango_layout_set_text(g->layout, text, -1);
        pango_layout_get_pixel_extents(g->layout, &g->ink, NULL);
        cairo_move_to(g->cr, g->allocation.width - g->ink.width - g->ink.x - margin_left,
                      g->graph_height + 3. * g->inset + g->line_height - g->ink.y);
        pango_cairo_show_layout(g->cr, g->layout);
        cairo_stroke(g->cr);

        // restore font
        pango_font_description_set_style(g->desc, backup);
        pango_layout_set_font_description(g->layout, g->desc);
      }

      // mark the y axis legend
      set_color(g->cr, darktable.bauhaus->graph_fg);
      /* xgettext:no-c-format */
      g_strlcpy(text, _("% display"), sizeof(text));
      pango_layout_set_text(g->layout, text, -1);
      pango_layout_get_pixel_extents(g->layout, &g->ink, NULL);
      cairo_move_to(g->cr, -2. * g->inset - g->zero_width - g->ink.x,
                    -g->line_height - g->inset - 0.5 * g->ink.height - g->ink.y);
      pango_cairo_show_layout(g->cr, g->layout);
      cairo_stroke(g->cr);

      // mark the x axis legend
      set_color(g->cr, darktable.bauhaus->graph_fg);
      if(g->gui_mode == DT_FILMIC_GUI_LOOK)
        g_strlcpy(text, _("EV scene"), sizeof(text));
      else if(g->gui_mode == DT_FILMIC_GUI_BASECURVE || g->gui_mode == DT_FILMIC_GUI_BASECURVE_LOG)
      {
        /* xgettext:no-c-format */
        g_strlcpy(text, _("% camera"), sizeof(text));
      }
      pango_layout_set_text(g->layout, text, -1);
      pango_layout_get_pixel_extents(g->layout, &g->ink, NULL);
      cairo_move_to(g->cr, 0.5 * g->graph_width - 0.5 * g->ink.width - g->ink.x,
                    g->graph_height + 3. * g->inset + g->line_height - g->ink.y);
      pango_cairo_show_layout(g->cr, g->layout);
      cairo_stroke(g->cr);
    }
  }
  else
  {
    // mode ranges
    cairo_identity_matrix(g->cr); // reset coordinates

    // draw the dynamic range of display
    // if white = 100%, assume -11.69 EV because of uint8 output + sRGB OETF.
    // for uint10 output, white should be set to 400%, so anything above 100% increases DR
    // FIXME : if darktable becomes HDR-10bits compatible (for output), this needs to be updated
    const float display_DR = 12.f + log2f(p->white_point_target / 100.f);

    const float y_display = g->allocation.height / 3.f + g->line_height;
    const float y_scene = 2. * g->allocation.height / 3.f + g->line_height;

    const float display_top = y_display - g->line_height / 2;
    const float display_bottom = display_top + g->line_height;

    const float scene_top = y_scene - g->line_height / 2;
    const float scene_bottom = scene_top + g->line_height;

    float column_left;

    if(g->gui_show_labels)
    {
      // labels
      set_color(g->cr, darktable.bauhaus->graph_fg);
      g_strlcpy(text, _("display"), sizeof(text));
      pango_layout_set_text(g->layout, text, -1);
      pango_layout_get_pixel_extents(g->layout, &g->ink, NULL);
      cairo_move_to(g->cr, 0., y_display - 0.5 * g->ink.height - g->ink.y);
      pango_cairo_show_layout(g->cr, g->layout);
      cairo_stroke(g->cr);
      const float display_label_width = g->ink.width;

      // axis legend
      g_strlcpy(text, _("(%)"), sizeof(text));
      pango_layout_set_text(g->layout, text, -1);
      pango_layout_get_pixel_extents(g->layout, &g->ink, NULL);
      cairo_move_to(g->cr, 0.5 * display_label_width - 0.5 * g->ink.width - g->ink.x,
                    display_top - 4. * g->inset - g->ink.height - g->ink.y);
      pango_cairo_show_layout(g->cr, g->layout);
      cairo_stroke(g->cr);

      set_color(g->cr, darktable.bauhaus->graph_fg);
      g_strlcpy(text, _("scene"), sizeof(text));
      pango_layout_set_text(g->layout, text, -1);
      pango_layout_get_pixel_extents(g->layout, &g->ink, NULL);
      cairo_move_to(g->cr, 0., y_scene - 0.5 * g->ink.height - g->ink.y);
      pango_cairo_show_layout(g->cr, g->layout);
      cairo_stroke(g->cr);
      const float scene_label_width = g->ink.width;

      // axis legend
      g_strlcpy(text, _("(EV)"), sizeof(text));
      pango_layout_set_text(g->layout, text, -1);
      pango_layout_get_pixel_extents(g->layout, &g->ink, NULL);
      cairo_move_to(g->cr, 0.5 * scene_label_width - 0.5 * g->ink.width - g->ink.x,
                    scene_bottom + 2. * g->inset + 0. * g->ink.height + g->ink.y);
      pango_cairo_show_layout(g->cr, g->layout);
      cairo_stroke(g->cr);

      // arrow between labels
      cairo_set_line_width(g->cr, DT_PIXEL_APPLY_DPI(1.));
      dt_cairo_draw_arrow(g->cr, fminf(scene_label_width, display_label_width) / 2.f, y_scene - g->line_height,
                          fminf(scene_label_width, display_label_width) / 2.f,
                          y_display + g->line_height + g->inset, TRUE);

      column_left = fmaxf(display_label_width, scene_label_width) + g->inset;
    }
    else
      column_left = darktable.bauhaus->quad_width;

    const float column_right = g->allocation.width - column_left - darktable.bauhaus->quad_width;

    // compute dynamic ranges left and right to middle grey
    const float display_HL_EV = -log2f(p->grey_point_target / p->white_point_target); // compared to white EV
    const float display_LL_EV = display_DR - display_HL_EV;                           // compared to black EV
    const float display_real_black_EV
        = -fmaxf(log2f(p->black_point_target / p->grey_point_target),
                 -11.685887601778058f + display_HL_EV - log2f(p->white_point_target / 100.f));
    const float scene_HL_EV = p->white_point_source;  // compared to white EV
    const float scene_LL_EV = -p->black_point_source; // compared to black EV

    // compute the max width needed to fit both dynamic ranges and derivate the unit size of a GUI EV
    const float max_DR = ceilf(fmaxf(display_HL_EV, scene_HL_EV)) + ceilf(fmaxf(display_LL_EV, scene_LL_EV));
    const float EV = (column_right) / max_DR;

    // all greys are aligned vertically in GUI since they are the fulcrum of the transform
    // so, get their coordinates
    const float grey_EV = fmaxf(ceilf(display_HL_EV), ceilf(scene_HL_EV));
    const float grey_x = g->allocation.width - (grey_EV)*EV - darktable.bauhaus->quad_width;

    // similarly, get black/white coordinates from grey point
    const float display_black_x = grey_x - display_real_black_EV * EV;
    const float display_DR_start_x = grey_x - display_LL_EV * EV;
    const float display_white_x = grey_x + display_HL_EV * EV;

    const float scene_black_x = grey_x - scene_LL_EV * EV;
    const float scene_white_x = grey_x + scene_HL_EV * EV;
    const float scene_lat_bottom = grey_x + (g->spline.x[1] - g->spline.x[2]) * EV * DR;
    const float scene_lat_top = grey_x + (g->spline.x[3] - g->spline.x[2]) * EV * DR;

    // show EV zones for display - zones are aligned on 0% and 100%
    cairo_set_line_width(g->cr, DT_PIXEL_APPLY_DPI(1.));

    // latitude bounds - show contrast expansion

    // Compute usual filmic  mapping
    float display_lat_bottom = filmic_spline(g->spline.latitude_min, g->spline.M1, g->spline.M2, g->spline.M3, g->spline.M4,
                                  g->spline.M5, g->spline.latitude_min, g->spline.latitude_max);
    display_lat_bottom = powf(fmaxf(display_lat_bottom, NORM_MIN), p->output_power); // clamp at -16 EV

    // rescale output to log scale
    display_lat_bottom = log2f(display_lat_bottom/ (p->grey_point_target / 100.f));

    // take clamping into account
    if(display_lat_bottom < 0.f) // clamp to - 8 EV (black)
      display_lat_bottom = fmaxf(display_lat_bottom, -display_real_black_EV);
    else if(display_lat_bottom > 0.f) // clamp to 0 EV (white)
      display_lat_bottom = fminf(display_lat_bottom, display_HL_EV);

    // get destination coordinate
    display_lat_bottom = grey_x + display_lat_bottom * EV;

    // Compute usual filmic  mapping
    float display_lat_top = filmic_spline(g->spline.latitude_max, g->spline.M1, g->spline.M2, g->spline.M3, g->spline.M4,
                                  g->spline.M5, g->spline.latitude_min, g->spline.latitude_max);
    display_lat_top = powf(fmaxf(display_lat_top, NORM_MIN), p->output_power); // clamp at -16 EV

    // rescale output to log scale
    display_lat_top = log2f(display_lat_top / (p->grey_point_target / 100.f));

    // take clamping into account
    if(display_lat_top < 0.f) // clamp to - 8 EV (black)
      display_lat_top = fmaxf(display_lat_top, -display_real_black_EV);
    else if(display_lat_top > 0.f) // clamp to 0 EV (white)
      display_lat_top = fminf(display_lat_top, display_HL_EV);

    // get destination coordinate and draw
    display_lat_top = grey_x + display_lat_top * EV;

    cairo_move_to(g->cr, scene_lat_bottom, scene_top);
    cairo_line_to(g->cr, scene_lat_top, scene_top);
    cairo_line_to(g->cr, display_lat_top, display_bottom);
    cairo_line_to(g->cr, display_lat_bottom, display_bottom);
    cairo_line_to(g->cr, scene_lat_bottom, scene_top);
    set_color(g->cr, darktable.bauhaus->graph_bg);
    cairo_fill(g->cr);

    for(int i = 0; i < (int)ceilf(display_DR); i++)
    {
      // content
      const float shade = powf(exp2f(-11.f + (float)i), 1.f / 2.4f);
      cairo_set_source_rgb(g->cr, shade, shade, shade);
      cairo_rectangle(g->cr, display_DR_start_x + i * EV, display_top, EV, g->line_height);
      cairo_fill_preserve(g->cr);

      // borders
      cairo_set_source_rgb(g->cr, 0.75, .5, 0.);
      cairo_stroke(g->cr);
    }

    // middle grey display
    cairo_set_line_width(g->cr, DT_PIXEL_APPLY_DPI(2.));
    cairo_move_to(g->cr, grey_x, display_bottom + 2. * g->inset);
    cairo_line_to(g->cr, grey_x, display_top - 2. * g->inset);
    cairo_stroke(g->cr);

    // show EV zones for scene - zones are aligned on grey

    for(int i = floorf(p->black_point_source); i < ceilf(p->white_point_source); i++)
    {
      // content
      cairo_set_line_width(g->cr, DT_PIXEL_APPLY_DPI(1.));
      const float shade = powf(0.1845f * exp2f((float)i), 1.f / 2.4f);
      const float x_temp = grey_x + i * EV;
      cairo_set_source_rgb(g->cr, shade, shade, shade);
      cairo_rectangle(g->cr, x_temp, scene_top, EV, g->line_height);
      cairo_fill_preserve(g->cr);

      // borders
      cairo_set_source_rgb(g->cr, 0.75, .5, 0.);
      cairo_stroke(g->cr);

      // arrows
      if(i == 0)
        cairo_set_line_width(g->cr, DT_PIXEL_APPLY_DPI(2.));
      else
        cairo_set_line_width(g->cr, DT_PIXEL_APPLY_DPI(1.));

      if((float)i > p->black_point_source && (float)i < p->white_point_source)
      {
        // Compute usual filmic  mapping
        const float normal_value = ((float)i - p->black_point_source) / DR;
        float y_temp = filmic_spline(normal_value, g->spline.M1, g->spline.M2, g->spline.M3, g->spline.M4,
                                     g->spline.M5, g->spline.latitude_min, g->spline.latitude_max);
        y_temp = powf(fmaxf(y_temp, NORM_MIN), p->output_power); // clamp at -16 EV

        // rescale output to log scale
        y_temp = log2f(y_temp / (p->grey_point_target / 100.f));

        // take clamping into account
        if(y_temp < 0.f) // clamp to - 8 EV (black)
          y_temp = fmaxf(y_temp, -display_real_black_EV);
        else if(y_temp > 0.f) // clamp to 0 EV (white)
          y_temp = fminf(y_temp, display_HL_EV);

        // get destination coordinate and draw
        y_temp = grey_x + y_temp * EV;
        dt_cairo_draw_arrow(g->cr, x_temp, scene_top, y_temp, display_bottom, FALSE);
      }
    }

    cairo_set_line_width(g->cr, DT_PIXEL_APPLY_DPI(2.));

    // arrows for black and white
    float x_temp = grey_x + p->black_point_source * EV;
    float y_temp = grey_x - display_real_black_EV * EV;
    dt_cairo_draw_arrow(g->cr, x_temp, scene_top, y_temp, display_bottom, FALSE);

    x_temp = grey_x + p->white_point_source * EV;
    y_temp = grey_x + display_HL_EV * EV;
    dt_cairo_draw_arrow(g->cr, x_temp, scene_top, y_temp, display_bottom, FALSE);

    // draw white - grey - black ticks

    // black display
    cairo_move_to(g->cr, display_black_x, display_bottom);
    cairo_line_to(g->cr, display_black_x, display_top - 2. * g->inset);
    cairo_stroke(g->cr);

    // middle grey display
    cairo_move_to(g->cr, grey_x, display_bottom);
    cairo_line_to(g->cr, grey_x, display_top - 2. * g->inset);
    cairo_stroke(g->cr);

    // white display
    cairo_move_to(g->cr, display_white_x, display_bottom);
    cairo_line_to(g->cr, display_white_x, display_top - 2. * g->inset);
    cairo_stroke(g->cr);

    // black scene
    cairo_move_to(g->cr, scene_black_x, scene_bottom + 2. * g->inset);
    cairo_line_to(g->cr, scene_black_x, scene_top);
    cairo_stroke(g->cr);

    // middle grey scene
    cairo_move_to(g->cr, grey_x, scene_bottom + 2. * g->inset);
    cairo_line_to(g->cr, grey_x, scene_top);
    cairo_stroke(g->cr);

    // white scene
    cairo_move_to(g->cr, scene_white_x, scene_bottom + 2. * g->inset);
    cairo_line_to(g->cr, scene_white_x, scene_top);
    cairo_stroke(g->cr);

    // legends
    set_color(g->cr, darktable.bauhaus->graph_fg);

    // black scene legend
    snprintf(text, sizeof(text), "%+.1f", p->black_point_source);
    pango_layout_set_text(g->layout, text, -1);
    pango_layout_get_pixel_extents(g->layout, &g->ink, NULL);
    cairo_move_to(g->cr, scene_black_x - 0.5 * g->ink.width - g->ink.x,
                  scene_bottom + 2. * g->inset + 0. * g->ink.height + g->ink.y);
    pango_cairo_show_layout(g->cr, g->layout);
    cairo_stroke(g->cr);

    // grey scene legend
    snprintf(text, sizeof(text), "%+.1f", 0.f);
    pango_layout_set_text(g->layout, text, -1);
    pango_layout_get_pixel_extents(g->layout, &g->ink, NULL);
    cairo_move_to(g->cr, grey_x - 0.5 * g->ink.width - g->ink.x,
                  scene_bottom + 2. * g->inset + 0. * g->ink.height + g->ink.y);
    pango_cairo_show_layout(g->cr, g->layout);
    cairo_stroke(g->cr);

    // white scene legend
    snprintf(text, sizeof(text), "%+.1f", p->white_point_source);
    pango_layout_set_text(g->layout, text, -1);
    pango_layout_get_pixel_extents(g->layout, &g->ink, NULL);
    cairo_move_to(g->cr, scene_white_x - 0.5 * g->ink.width - g->ink.x,
                  scene_bottom + 2. * g->inset + 0. * g->ink.height + g->ink.y);
    pango_cairo_show_layout(g->cr, g->layout);
    cairo_stroke(g->cr);

    // black scene legend
    snprintf(text, sizeof(text), "%.0f", p->black_point_target);
    pango_layout_set_text(g->layout, text, -1);
    pango_layout_get_pixel_extents(g->layout, &g->ink, NULL);
    cairo_move_to(g->cr, display_black_x - 0.5 * g->ink.width - g->ink.x,
                  display_top - 4. * g->inset - g->ink.height - g->ink.y);
    pango_cairo_show_layout(g->cr, g->layout);
    cairo_stroke(g->cr);

    // grey scene legend
    snprintf(text, sizeof(text), "%.0f", p->grey_point_target);
    pango_layout_set_text(g->layout, text, -1);
    pango_layout_get_pixel_extents(g->layout, &g->ink, NULL);
    cairo_move_to(g->cr, grey_x - 0.5 * g->ink.width - g->ink.x,
                  display_top - 4. * g->inset - g->ink.height - g->ink.y);
    pango_cairo_show_layout(g->cr, g->layout);
    cairo_stroke(g->cr);

    // white scene legend
    snprintf(text, sizeof(text), "%.0f", p->white_point_target);
    pango_layout_set_text(g->layout, text, -1);
    pango_layout_get_pixel_extents(g->layout, &g->ink, NULL);
    cairo_move_to(g->cr, display_white_x - 0.5 * g->ink.width - g->ink.x,
                  display_top - 4. * g->inset - g->ink.height - g->ink.y);
    pango_cairo_show_layout(g->cr, g->layout);
    cairo_stroke(g->cr);
  }

  // restore font size
  pango_font_description_set_size(g->desc, font_size);
  pango_layout_set_font_description(g->layout, g->desc);

  cairo_destroy(g->cr);
  cairo_set_source_surface(crf, g->cst, 0, 0);
  cairo_paint(crf);
  cairo_surface_destroy(g->cst);
  return TRUE;
}

static gboolean area_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(darktable.gui->reset) return TRUE;

  dt_iop_filmicrgb_gui_data_t *g = (dt_iop_filmicrgb_gui_data_t *)self->gui_data;

  dt_iop_request_focus(self);

  if(g->active_button != DT_FILMIC_GUI_BUTTON_LAST)
  {

    if(event->button == 1 && event->type == GDK_2BUTTON_PRESS)
    {
      // double click resets view
      if(g->active_button == DT_FILMIC_GUI_BUTTON_TYPE)
      {
        g->gui_mode = DT_FILMIC_GUI_LOOK;
        gtk_widget_queue_draw(GTK_WIDGET(g->area));
        dt_conf_set_int("plugins/darkroom/filmicrgb/graph_view", g->gui_mode);
        return TRUE;
      }
      else
      {
        return FALSE;
      }
    }
    else if(event->button == 1)
    {
      // simple left click cycles through modes in positive direction
      if(g->active_button == DT_FILMIC_GUI_BUTTON_TYPE)
      {
        // cycle type of graph
        if(g->gui_mode == DT_FILMIC_GUI_RANGES)
          g->gui_mode = DT_FILMIC_GUI_LOOK;
        else
          g->gui_mode++;

        gtk_widget_queue_draw(GTK_WIDGET(g->area));
        dt_conf_set_int("plugins/darkroom/filmicrgb/graph_view", g->gui_mode);
        return TRUE;
      }
      else if(g->active_button == DT_FILMIC_GUI_BUTTON_LABELS)
      {
        g->gui_show_labels = !g->gui_show_labels;
        gtk_widget_queue_draw(GTK_WIDGET(g->area));
        dt_conf_set_int("plugins/darkroom/filmicrgb/graph_show_labels", g->gui_show_labels);
        return TRUE;
      }
      else
      {
        // we should never get there since (g->active_button != DT_FILMIC_GUI_BUTTON_LAST)
        // and any other case has been processed above.
        return FALSE;
      }
    }
    else if(event->button == 3)
    {
      // simple right click cycles through modes in negative direction
      if(g->active_button == DT_FILMIC_GUI_BUTTON_TYPE)
      {
        if(g->gui_mode == DT_FILMIC_GUI_LOOK)
          g->gui_mode = DT_FILMIC_GUI_RANGES;
        else
          g->gui_mode--;

        gtk_widget_queue_draw(GTK_WIDGET(g->area));
        dt_conf_set_int("plugins/darkroom/filmicrgb/graph_view", g->gui_mode);
        return TRUE;
      }
      else if(g->active_button == DT_FILMIC_GUI_BUTTON_LABELS)
      {
        g->gui_show_labels = !g->gui_show_labels;
        gtk_widget_queue_draw(GTK_WIDGET(g->area));
        dt_conf_set_int("plugins/darkroom/filmicrgb/graph_show_labels", g->gui_show_labels);
        return TRUE;
      }
      else
      {
        return FALSE;
      }
    }
  }

  return FALSE;
}

static gboolean area_enter_notify(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(darktable.gui->reset) return 1;
  if(!self->enabled) return 0;

  dt_iop_filmicrgb_gui_data_t *g = (dt_iop_filmicrgb_gui_data_t *)self->gui_data;
  g->gui_hover = TRUE;
  gtk_widget_queue_draw(GTK_WIDGET(g->area));
  return TRUE;
}


static gboolean area_leave_notify(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(darktable.gui->reset) return 1;
  if(!self->enabled) return 0;

  dt_iop_filmicrgb_gui_data_t *g = (dt_iop_filmicrgb_gui_data_t *)self->gui_data;
  g->gui_hover = FALSE;
  gtk_widget_queue_draw(GTK_WIDGET(g->area));
  return TRUE;
}

static gboolean area_motion_notify(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(darktable.gui->reset) return 1;

  dt_iop_filmicrgb_gui_data_t *g = (dt_iop_filmicrgb_gui_data_t *)self->gui_data;
  if(!g->gui_sizes_inited) return FALSE;

  // get in-widget coordinates
  const float y = event->y;
  const float x = event->x;

  if(x > 0. && x < g->allocation.width && y > 0. && y < g->allocation.height) g->gui_hover = TRUE;

  gint save_active_button = g->active_button;

  if(g->gui_hover)
  {
    // find out which button is under the mouse
    gint found_something = FALSE;
    for(int i = 0; i < DT_FILMIC_GUI_BUTTON_LAST; i++)
    {
      // check if mouse in in the button's bounds
      if(x > g->buttons[i].left && x < g->buttons[i].right && y > g->buttons[i].top && y < g->buttons[i].bottom)
      {
        // yeah, mouse is over that button
        g->buttons[i].mouse_hover = TRUE;
        g->active_button = i;
        found_something = TRUE;
      }
      else
      {
        // no luck with this button
        g->buttons[i].mouse_hover = FALSE;
      }
    }

    if(!found_something) g->active_button = DT_FILMIC_GUI_BUTTON_LAST; // mouse is over no known button

    // update the tooltips
    if(g->active_button == DT_FILMIC_GUI_BUTTON_LAST && x < g->buttons[0].left)
    {
      // we are over the graph area
      gtk_widget_set_tooltip_text(GTK_WIDGET(g->area), _("use the parameters below to set the nodes.\n"
                                                         "the bright curve is the filmic tone mapping curve\n"
                                                         "the dark curve is the desaturation curve."));
    }
    else if(g->active_button == DT_FILMIC_GUI_BUTTON_LABELS)
    {
      gtk_widget_set_tooltip_text(GTK_WIDGET(g->area), _("toggle axis labels and values display."));
    }
    else if(g->active_button == DT_FILMIC_GUI_BUTTON_TYPE)
    {
      gtk_widget_set_tooltip_text(GTK_WIDGET(g->area), _("cycle through graph views.\n"
                                                         "left click: cycle forward.\n"
                                                         "right click: cycle backward.\n"
                                                         "double click: reset to look view."));
    }
    else
    {
      gtk_widget_set_tooltip_text(GTK_WIDGET(g->area), "");
    }

    if(save_active_button != g->active_button) gtk_widget_queue_draw(GTK_WIDGET(g->area));
    return TRUE;
  }
  else
  {
    g->active_button = DT_FILMIC_GUI_BUTTON_LAST;
    if(save_active_button != g->active_button) (GTK_WIDGET(g->area));
    return FALSE;
  }
}


void gui_init(dt_iop_module_t *self)
{
  dt_iop_filmicrgb_gui_data_t *g = IOP_GUI_ALLOC(filmicrgb);

  g->show_mask = FALSE;
  g->gui_mode = DT_FILMIC_GUI_LOOK;
  g->gui_show_labels = TRUE;
  g->gui_hover = FALSE;
  g->gui_sizes_inited = FALSE;

  // don't make the area square to safe some vertical space -- it's not interactive anyway
  g->area = GTK_DRAWING_AREA(dtgtk_drawing_area_new_with_aspect_ratio(0.75));
  gtk_widget_set_can_focus(GTK_WIDGET(g->area), TRUE);
  gtk_widget_add_events(GTK_WIDGET(g->area), GDK_BUTTON_PRESS_MASK | GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK
                                                 | GDK_POINTER_MOTION_MASK);
  g_signal_connect(G_OBJECT(g->area), "draw", G_CALLBACK(dt_iop_tonecurve_draw), self);
  g_signal_connect(G_OBJECT(g->area), "button-press-event", G_CALLBACK(area_button_press), self);
  g_signal_connect(G_OBJECT(g->area), "leave-notify-event", G_CALLBACK(area_leave_notify), self);
  g_signal_connect(G_OBJECT(g->area), "enter-notify-event", G_CALLBACK(area_enter_notify), self);
  g_signal_connect(G_OBJECT(g->area), "motion-notify-event", G_CALLBACK(area_motion_notify), self);

  // Init GTK notebook
  g->notebook = GTK_NOTEBOOK(gtk_notebook_new());

  // Page SCENE
  self->widget = dt_ui_notebook_page(g->notebook, _("scene"), NULL);

  g->grey_point_source
      = dt_color_picker_new(self, DT_COLOR_PICKER_AREA, dt_bauhaus_slider_from_params(self, "grey_point_source"));
  dt_bauhaus_slider_set_soft_range(g->grey_point_source, .1, 36.0);
  dt_bauhaus_slider_set_format(g->grey_point_source, "%.2f %%");
  gtk_widget_set_tooltip_text(g->grey_point_source,
                              _("adjust to match the average luminance of the image's subject.\n"
                                "the value entered here will then be remapped to 18.45%.\n"
                                "decrease the value to increase the overall brightness."));

  // White slider
  g->white_point_source
      = dt_color_picker_new(self, DT_COLOR_PICKER_AREA, dt_bauhaus_slider_from_params(self, "white_point_source"));
  dt_bauhaus_slider_set_soft_range(g->white_point_source, 2.0, 8.0);
  dt_bauhaus_slider_set_format(g->white_point_source, _("%+.2f EV"));
  gtk_widget_set_tooltip_text(g->white_point_source,
                              _("number of stops between middle grey and pure white.\n"
                                "this is a reading a lightmeter would give you on the scene.\n"
                                "adjust so highlights clipping is avoided"));

  // Black slider
  g->black_point_source
      = dt_color_picker_new(self, DT_COLOR_PICKER_AREA, dt_bauhaus_slider_from_params(self, "black_point_source"));
  dt_bauhaus_slider_set_soft_range(g->black_point_source, -14.0, -3);
  dt_bauhaus_slider_set_format(g->black_point_source, _("%+.2f EV"));
  gtk_widget_set_tooltip_text(
      g->black_point_source, _("number of stops between middle grey and pure black.\n"
                               "this is a reading a lightmeter would give you on the scene.\n"
                               "increase to get more contrast.\ndecrease to recover more details in low-lights."));

  // Dynamic range scaling
  g->security_factor = dt_bauhaus_slider_from_params(self, "security_factor");
  dt_bauhaus_slider_set_soft_max(g->security_factor, 50);
  dt_bauhaus_slider_set_format(g->security_factor, "%+.2f %%");
  gtk_widget_set_tooltip_text(g->security_factor, _("symmetrically enlarge or shrink the computed dynamic range.\n"
                                                    "useful to give a safety margin to extreme luminances."));

  // Auto tune slider
  g->auto_button = dt_color_picker_new(self, DT_COLOR_PICKER_AREA, dt_bauhaus_combobox_new(self));
  dt_bauhaus_widget_set_label(g->auto_button, NULL, N_("auto tune levels"));
  gtk_widget_set_tooltip_text(g->auto_button, _("try to optimize the settings with some statistical assumptions.\n"
                                                "this will fit the luminance range inside the histogram bounds.\n"
                                                "works better for landscapes and evenly-lit pictures\n"
                                                "but fails for high-keys, low-keys and high-ISO pictures.\n"
                                                "this is not an artificial intelligence, but a simple guess.\n"
                                                "ensure you understand its assumptions before using it."));
  gtk_box_pack_start(GTK_BOX(self->widget), g->auto_button, FALSE, FALSE, 0);

  // Page RECONSTRUCT
  self->widget = dt_ui_notebook_page(g->notebook, _("reconstruct"), NULL);

  GtkWidget *label = dt_ui_section_label_new(_("highlights clipping"));
  GtkStyleContext *context = gtk_widget_get_style_context(GTK_WIDGET(label));
  gtk_style_context_add_class(context, "section_label_top");
  gtk_box_pack_start(GTK_BOX(self->widget), label, FALSE, FALSE, 0);

  g->reconstruct_threshold = dt_bauhaus_slider_from_params(self, "reconstruct_threshold");
  dt_bauhaus_slider_set_format(g->reconstruct_threshold, _("%+.2f EV"));
  gtk_widget_set_tooltip_text(g->reconstruct_threshold,
                              _("set the exposure threshold upon which\n"
                                "clipped highlights get reconstructed.\n"
                                "values are relative to the scene white point.\n"
                                "0 EV means the threshold is the same as the scene white point.\n"
                                "decrease to include more areas,\n"
                                "increase to exclude more areas."));

  g->reconstruct_feather = dt_bauhaus_slider_from_params(self, "reconstruct_feather");
  dt_bauhaus_slider_set_format(g->reconstruct_feather, _("%+.2f EV"));
  gtk_widget_set_tooltip_text(g->reconstruct_feather,
                              _("soften the transition between clipped highlights and valid pixels.\n"
                                "decrease to make the transition harder and sharper,\n"
                                "increase to make the transition softer and blurrier."));

  // Highlight Reconstruction Mask
  g->show_highlight_mask = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->show_highlight_mask, NULL, N_("display highlight reconstruction mask"));
  dt_bauhaus_widget_set_quad_paint(g->show_highlight_mask, dtgtk_cairo_paint_showmask,
                                   CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
  dt_bauhaus_widget_set_quad_toggle(g->show_highlight_mask, TRUE);
  g_signal_connect(G_OBJECT(g->show_highlight_mask), "quad-pressed", G_CALLBACK(show_mask_callback), self);
  gtk_box_pack_start(GTK_BOX(self->widget), g->show_highlight_mask, FALSE, FALSE, 0);

  label = dt_ui_section_label_new(_("balance"));
  gtk_box_pack_start(GTK_BOX(self->widget), label, FALSE, FALSE, 0);

  g->reconstruct_structure_vs_texture = dt_bauhaus_slider_from_params(self, "reconstruct_structure_vs_texture");
  dt_bauhaus_slider_set_step(g->reconstruct_structure_vs_texture, 0.1);
  dt_bauhaus_slider_set_format(g->reconstruct_structure_vs_texture, "%.2f %%");
  gtk_widget_set_tooltip_text(g->reconstruct_structure_vs_texture,
                              /* xgettext:no-c-format */
                              _("decide which reconstruction strategy to favor,\n"
                                "between inpainting a smooth color gradient,\n"
                                "or trying to recover the textured details.\n"
                                "0% is an equal mix of both.\n"
                                "increase if at least one RGB channel is not clipped.\n"
                                "decrease if all RGB channels are clipped over large areas."));

  g->reconstruct_bloom_vs_details = dt_bauhaus_slider_from_params(self, "reconstruct_bloom_vs_details");
  dt_bauhaus_slider_set_step(g->reconstruct_bloom_vs_details, 0.1);
  dt_bauhaus_slider_set_format(g->reconstruct_bloom_vs_details, "%.2f %%");
  gtk_widget_set_tooltip_text(g->reconstruct_bloom_vs_details,
                              /* xgettext:no-c-format */
                              _("decide which reconstruction strategy to favor,\n"
                                "between blooming highlights like film does,\n"
                                "or trying to recover sharp details.\n"
                                "0% is an equal mix of both.\n"
                                "increase if you want more details.\n"
                                "decrease if you want more blur."));

  // Bloom threshold
  g->reconstruct_grey_vs_color = dt_bauhaus_slider_from_params(self, "reconstruct_grey_vs_color");
  dt_bauhaus_slider_set_step(g->reconstruct_grey_vs_color, 0.1);
  dt_bauhaus_slider_set_format(g->reconstruct_grey_vs_color, "%.2f %%");
  gtk_widget_set_tooltip_text(g->reconstruct_grey_vs_color,
                              /* xgettext:no-c-format */
                              _("decide which reconstruction strategy to favor,\n"
                                "between recovering monochromatic highlights,\n"
                                "or trying to recover colorful highlights.\n"
                                "0% is an equal mix of both.\n"
                                "increase if you want more color.\n"
                                "decrease if you see magenta or out-of-gamut highlights."));

  // Page LOOK
  self->widget = dt_ui_notebook_page(g->notebook, _("look"), NULL);

  g->contrast = dt_bauhaus_slider_from_params(self, N_("contrast"));
  dt_bauhaus_slider_set_soft_range(g->contrast, 1.0, 2.0);
  dt_bauhaus_slider_set_digits(g->contrast, 3);
  dt_bauhaus_slider_set_step(g->contrast, .01);
  gtk_widget_set_tooltip_text(g->contrast, _("slope of the linear part of the curve\n"
                                             "affects mostly the mid-tones"));

  // brightness slider
  g->output_power = dt_bauhaus_slider_from_params(self, "output_power");
  gtk_widget_set_tooltip_text(g->output_power, _("equivalent to paper grade in analog.\n"
                                                 "increase to make highlights brighter and less compressed.\n"
                                                 "decrease to mute highlights."));

  g->latitude = dt_bauhaus_slider_from_params(self, N_("latitude"));
  dt_bauhaus_slider_set_soft_range(g->latitude, 5.0, 50.0);
  dt_bauhaus_slider_set_format(g->latitude, "%.2f %%");
  gtk_widget_set_tooltip_text(g->latitude,
                              _("width of the linear domain in the middle of the curve,\n"
                                "in percent of the dynamic range (white exposure - black exposure).\n"
                                "increase to get more contrast and less desaturation at extreme luminances,\n"
                                "decrease otherwise. no desaturation happens in the latitude range.\n"
                                "this has no effect on mid-tones."));

  g->balance = dt_bauhaus_slider_from_params(self, "balance");
  dt_bauhaus_slider_set_format(g->balance, "%.2f %%");
  gtk_widget_set_tooltip_text(g->balance, _("slides the latitude along the slope\n"
                                            "to give more room to shadows or highlights.\n"
                                            "use it if you need to protect the details\n"
                                            "at one extremity of the histogram."));

  g->saturation = dt_bauhaus_slider_from_params(self, "saturation");
  dt_bauhaus_slider_set_soft_max(g->saturation, 50.0);
  dt_bauhaus_slider_set_format(g->saturation, "%.2f %%");
  gtk_widget_set_tooltip_text(g->saturation, _("desaturates the output of the module\n"
                                               "specifically at extreme luminances.\n"
                                               "increase if shadows and/or highlights are under-saturated."));

  // Page DISPLAY
  self->widget = dt_ui_notebook_page(g->notebook, _("display"), NULL);

  // Black slider
  g->black_point_target = dt_bauhaus_slider_from_params(self, "black_point_target");
  dt_bauhaus_slider_set_step(g->black_point_target, .001);
  dt_bauhaus_slider_set_digits(g->black_point_target, 4);
  dt_bauhaus_slider_set_format(g->black_point_target, "%.4f %%");
  gtk_widget_set_tooltip_text(g->black_point_target, _("luminance of output pure black, "
                                                       "this should be 0%\nexcept if you want a faded look"));

  g->grey_point_target = dt_bauhaus_slider_from_params(self, "grey_point_target");
  dt_bauhaus_slider_set_step(g->grey_point_target, .01);
  dt_bauhaus_slider_set_digits(g->grey_point_target, 4);
  dt_bauhaus_slider_set_format(g->grey_point_target, "%.4f %%");
  gtk_widget_set_tooltip_text(g->grey_point_target,
                              _("midde grey value of the target display or color space.\n"
                                "you should never touch that unless you know what you are doing."));

  g->white_point_target = dt_bauhaus_slider_from_params(self, "white_point_target");
  dt_bauhaus_slider_set_soft_max(g->white_point_target, 100.0);
  dt_bauhaus_slider_set_step(g->white_point_target, .01);
  dt_bauhaus_slider_set_digits(g->white_point_target, 4);
  dt_bauhaus_slider_set_format(g->white_point_target, "%.4f %%");
  gtk_widget_set_tooltip_text(g->white_point_target, _("luminance of output pure white, "
                                                       "this should be 100%\nexcept if you want a faded look"));

  // Page OPTIONS
  self->widget = dt_ui_notebook_page(g->notebook, _("options"), NULL);

  // Color science
  g->version = dt_bauhaus_combobox_from_params(self, "version");
  gtk_widget_set_tooltip_text(g->version,
                              _("v3 is darktable 3.0 desaturation method, same as color balance.\n"
                                "v4 is a newer desaturation method, based on spectral purity of light."));

  g->preserve_color = dt_bauhaus_combobox_from_params(self, "preserve_color");
  gtk_widget_set_tooltip_text(g->preserve_color, _("ensure the original color are preserved.\n"
                                                   "may reinforce chromatic aberrations and chroma noise,\n"
                                                   "so ensure they are properly corrected elsewhere.\n"));

  // Curve type
  g->highlights = dt_bauhaus_combobox_from_params(self, "highlights");
  gtk_widget_set_tooltip_text(g->highlights, _("choose the desired curvature of the filmic spline in highlights.\n"
                                               "hard uses a high curvature resulting in more tonal compression.\n"
                                               "soft uses a low curvature resulting in less tonal compression."));

  g->shadows = dt_bauhaus_combobox_from_params(self, "shadows");
  gtk_widget_set_tooltip_text(g->shadows, _("choose the desired curvature of the filmic spline in shadows.\n"
                                            "hard uses a high curvature resulting in more tonal compression.\n"
                                            "soft uses a low curvature resulting in less tonal compression."));

  g->custom_grey = dt_bauhaus_toggle_from_params(self, "custom_grey");
  gtk_widget_set_tooltip_text(g->custom_grey, _("enable to input custom middle-grey values.\n"
                                                "this is not recommended in general.\n"
                                                "fix the global exposure in the exposure module instead.\n"
                                                "disable to use standard 18.45 %% middle grey."));

  g->auto_hardness = dt_bauhaus_toggle_from_params(self, "auto_hardness");
  gtk_widget_set_tooltip_text(
      g->auto_hardness, _("enable to auto-set the look hardness depending on the scene white and black points.\n"
                          "this keeps the middle grey on the identity line and improves fast tuning.\n"
                          "disable if you want a manual control."));

  g->high_quality_reconstruction = dt_bauhaus_slider_from_params(self, "high_quality_reconstruction");
  gtk_widget_set_tooltip_text(g->high_quality_reconstruction,
                              _("run extra passes of chromaticity reconstruction.\n"
                                "more iterations means more color propagation from neighbourhood.\n"
                                "this will be slower but will yield more neutral highlights.\n"
                                "it also helps with difficult cases of magenta highlights."));

  // Highlight noise
  g->noise_level = dt_bauhaus_slider_from_params(self, "noise_level");
  gtk_widget_set_tooltip_text(g->noise_level, _("add statistical noise in reconstructed highlights.\n"
                                                "this avoids highlights to look too smooth\n"
                                                "when the picture is noisy overall,\n"
                                                "so they blend with the rest of the picture."));

  // Noise distribution
  g->noise_distribution = dt_bauhaus_combobox_from_params(self, "noise_distribution");
  dt_bauhaus_combobox_add(g->noise_distribution, _("uniform"));
  dt_bauhaus_combobox_add(g->noise_distribution, _("gaussian"));
  dt_bauhaus_combobox_add(g->noise_distribution, _("poissonian"));
  gtk_widget_set_tooltip_text(g->noise_distribution, _("choose the statistical distribution of noise.\n"
                                                       "this is useful to match natural sensor noise pattern.\n"));

  // start building top level widget
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->area), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->notebook), FALSE, FALSE, 0);
}

void gui_changed(dt_iop_module_t *self, GtkWidget *w, void *previous)
{
  dt_iop_filmicrgb_params_t *p = (dt_iop_filmicrgb_params_t *)self->params;
  dt_iop_filmicrgb_gui_data_t *g = (dt_iop_filmicrgb_gui_data_t *)self->gui_data;

  if(!w || w == g->auto_hardness || w == g->security_factor || w == g->grey_point_source
     || w == g->black_point_source || w == g->white_point_source)
  {
    ++darktable.gui->reset;

    if(w == g->security_factor || w == g->grey_point_source)
    {
      float prev = *(float *)previous;
      if(w == g->security_factor)
      {
        float ratio = (p->security_factor - prev) / (prev + 100.0f);

        float EVmin = p->black_point_source;
        EVmin = EVmin + ratio * EVmin;

        float EVmax = p->white_point_source;
        EVmax = EVmax + ratio * EVmax;

        p->white_point_source = EVmax;
        p->black_point_source = EVmin;
      }
      else
      {
        float grey_var = log2f(prev / p->grey_point_source);
        p->black_point_source = p->black_point_source - grey_var;
        p->white_point_source = p->white_point_source + grey_var;
      }

      dt_bauhaus_slider_set_soft(g->white_point_source, p->white_point_source);
      dt_bauhaus_slider_set_soft(g->black_point_source, p->black_point_source);
    }

    if(p->auto_hardness)
      p->output_power = logf(p->grey_point_target / 100.0f)
                        / logf(-p->black_point_source / (p->white_point_source - p->black_point_source));

    gtk_widget_set_visible(GTK_WIDGET(g->output_power), !p->auto_hardness);
    dt_bauhaus_slider_set_soft(g->output_power, p->output_power);

    --darktable.gui->reset;
  }

  if(!w || w == g->version)
  {
    if(p->version == DT_FILMIC_COLORSCIENCE_V1)
      dt_bauhaus_widget_set_label(g->saturation, NULL, N_("extreme luminance saturation"));
    else if(p->version == DT_FILMIC_COLORSCIENCE_V2)
      dt_bauhaus_widget_set_label(g->saturation, NULL, N_("middle tones saturation"));
  }

  if(!w || w == g->reconstruct_bloom_vs_details)
  {
    if(p->reconstruct_bloom_vs_details == -100.f)
    {
      // user disabled the reconstruction in favor of full blooming
      // so the structure vs. texture setting doesn't make any difference
      // make it insensitive to not confuse users
      gtk_widget_set_sensitive(g->reconstruct_structure_vs_texture, FALSE);
    }
    else
    {
      gtk_widget_set_sensitive(g->reconstruct_structure_vs_texture, TRUE);
    }
  }

  if(!w || w == g->custom_grey)
  {
    gtk_widget_set_visible(g->grey_point_source, p->custom_grey);
    gtk_widget_set_visible(g->grey_point_target, p->custom_grey);
  }

  gtk_widget_queue_draw(self->widget);
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
