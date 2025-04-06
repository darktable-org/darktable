/*
    This file is part of darktable,
    Copyright (C) 2018-2025 darktable developers.

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

/*** DOCUMENTATION
 *
 * This module aims at relighting the scene by performing an exposure
 * compensation selectively on specified exposures octaves, the same
 * way HiFi audio equalizers allow to set a gain for each octave.
 *
 * It is intended to work in scene-linear camera RGB, to behave as if
 * light was physically added or removed from the scene. As such, it
 * should be put before input profile in the pipe, but preferably
 * after exposure. It also need to be placed after the rotation,
 * perspective and cropping modules for the interactive editing to
 * work properly (so the image buffer overlap perfectly with the image
 * preview).
 *
 * Because it works before camera RGB -> XYZ conversion, the exposure
 * cannot be computed from any human-based perceptual colour model (Y
 * channel), hence why several RGB norms are provided as estimators of
 * the pixel energy to compute a luminance map. None of them is
 * perfect, and I'm still looking forward to a real spectral energy
 * estimator. The best physically-accurate norm should be the
 * euclidean norm, but the best looking is often the power norm, which
 * has no theoretical background. The geometric mean also display
 * interesting properties as it interprets saturated colours as
 * low-lights, allowing to lighten and desaturate them in a realistic
 * way.
 *
 * The exposure correction is computed as a series of each octave's
 * gain weighted by the gaussian of the radial distance between the
 * current pixel exposure and each octave's center. This allows for a
 * smooth and continuous infinite-order interpolation, preserving
 * exposure gradients as best as possible. The radius of the kernel is
 * user-defined and can be tweaked to get a smoother interpolation
 * (possibly generating oscillations), or a more monotonous one
 * (possibly less smooth). The actual factors of the gaussian series
 * are computed by solving the linear system taking the user-input
 * parameters as target exposures compensations.
 *
 * Notice that every pixel operation is performed in linear space. The
 * exposures in log2 (EV) are only used for user-input parameters and
 * for the gaussian weights of the radial distance between pixel
 * exposure and octave's centers.
 *
 * The details preservation modes make use of a fast guided filter
 * optimized to perform an edge-aware surface blur on the luminance
 * mask, in the same spirit as the bilateral filter, but without its
 * classic issues of gradient reversal around sharp edges. This
 * surface blur will allow to perform piece-wise smooth exposure
 * compensation, so local contrast will be preserved inside contiguous
 * regions. Various mask refinements are provided to help the
 * edge-taping of the filter (feathering parameter) while keeping
 * smooth contiguous region (quantization parameter), but also to
 * translate (exposure boost) and dilate (contrast boost) the exposure
 * histogram through the control octaves, to center it on the control
 * view and make maximum use of the available channels.
 *
 * Users should be aware that not all the available octaves will be
 * useful on every pictures.  Some automatic options will help them to
 * optimize the luminance mask, performing histogram analys, mapping
 * the average exposure to -4EV, and mapping the first and last
 * deciles of the histogram on its average ± 4EV. These automatic
 * helpers usually fail on X-Trans sensors, maybe because of bad
 * demosaicing, possibly resulting in outliers\negative RGB values.
 * Since they fail the same way on filmic's auto-tuner, we might need
 * to investigate X-Trans algos at some point.
 *
***/

#include "common/extra_optimizations.h"

#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <inttypes.h> // Only needed for debug printf of hashes, TODO remove


#include "bauhaus/bauhaus.h"
#include "common/darktable.h"
#include "common/fast_guided_filter.h"
#include "common/eigf.h"
#include "common/interpolation.h"
#include "common/luminance_mask.h"
#include "common/opencl.h"
#include "common/collection.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/blend.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "develop/imageop_gui.h"
#include "dtgtk/drawingarea.h"
#include "dtgtk/expander.h"
#include "gui/accelerators.h"
#include "gui/color_picker_proxy.h"
#include "gui/draw.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "gui/color_picker_proxy.h"
#include "iop/iop_api.h"
#include "iop/choleski.h"
#include "common/iop_group.h"

#ifdef _OPENMP
#include <omp.h>
#endif


DT_MODULE_INTROSPECTION(2, dt_iop_toneequalizer_params_t)

/****************************************************************************
 *
 * Definition of constants
 *
 ****************************************************************************/

#define UI_HISTO_SAMPLES 256 // 128 is a bit small for 4K resolution
#define HIRES_HISTO_SAMPLES UI_HISTO_SAMPLES * 3 * 16
#define CONTRAST_FULCRUM exp2f(-4.0f)
#define MIN_FLOAT exp2f(-16.0f)

#define DT_TONEEQ_MIN_EV (-8.0f)
#define DT_TONEEQ_MAX_EV (0.0f)
#define DT_TONEEQ_USE_LUT TRUE

#define HIRES_HISTO_MIN_EV -16.0f
#define HIRES_HISTO_MAX_EV 8.0f

/**
 * Build the exposures octaves :
 * band-pass filters with gaussian windows spaced by 1 EV
**/

#define NUM_SLIDERS 9
#define NUM_OCTAVES 8
#define LUT_RESOLUTION 10000

// radial distances used for pixel ops
static const float centers_ops[NUM_OCTAVES] DT_ALIGNED_ARRAY =
  {-56.0f / 7.0f, // = -8.0f
   -48.0f / 7.0f,
   -40.0f / 7.0f,
   -32.0f / 7.0f,
   -24.0f / 7.0f,
   -16.0f / 7.0f,
   -8.0f / 7.0f,
   0.0f / 7.0f}; // split 8 EV into 7 evenly-spaced NUM_SLIDERS

static const float centers_params[NUM_SLIDERS] DT_ALIGNED_ARRAY =
  { -8.0f, -7.0f, -6.0f, -5.0f,
    -4.0f, -3.0f, -2.0f, -1.0f, 0.0f};

// gaussian-ish kernel - sum is == 1.0f so we don't care much about actual coeffs
static const dt_colormatrix_t gauss_kernel =
  { { 0.076555024f, 0.124401914f, 0.076555024f },
    { 0.124401914f, 0.196172249f, 0.124401914f },
    { 0.076555024f, 0.124401914f, 0.076555024f } };


/****************************************************************************
 *
 * Types
 *
 ****************************************************************************/

typedef enum dt_iop_toneequalizer_filter_t
{
  DT_TONEEQ_NONE = 0,   // $DESCRIPTION: "no"
  DT_TONEEQ_AVG_GUIDED, // $DESCRIPTION: "averaged guided filter"
  DT_TONEEQ_GUIDED,     // $DESCRIPTION: "guided filter"
  DT_TONEEQ_AVG_EIGF,   // $DESCRIPTION: "averaged EIGF"
  DT_TONEEQ_EIGF        // $DESCRIPTION: "EIGF"
} dt_iop_toneequalizer_filter_t;

typedef enum dt_iop_toneequalizer_post_auto_align_t
{
  DT_TONEEQ_ALIGN_CUSTOM = 0,   // $DESCRIPTION: "custom"
  DT_TONEEQ_ALIGN_LEFT,         // $DESCRIPTION: "auto-align at shadows"
  DT_TONEEQ_ALIGN_CENTER,       // $DESCRIPTION: "auto-align at mid-tones"
  DT_TONEEQ_ALIGN_RIGHT,        // $DESCRIPTION: "auto-align at highlights"
  DT_TONEEQ_ALIGN_FIT,          // $DESCRIPTION: "fully fit"

} dt_iop_toneequalizer_post_auto_align_t;

typedef struct dt_iop_toneequalizer_params_t
{
  float noise;             // $MIN: -2.0 $MAX: 2.0 $DEFAULT: 0.0  $DESCRIPTION: "blacks"
  float ultra_deep_blacks; // $MIN: -2.0 $MAX: 2.0 $DEFAULT: 0.0  $DESCRIPTION: "deep shadows"
  float deep_blacks;       // $MIN: -2.0 $MAX: 2.0 $DEFAULT: 0.0  $DESCRIPTION: "shadows"
  float blacks;            // $MIN: -2.0 $MAX: 2.0 $DEFAULT: 0.0  $DESCRIPTION: "light shadows"
  float shadows;           // $MIN: -2.0 $MAX: 2.0 $DEFAULT: 0.0  $DESCRIPTION: "mid-tones"
  float midtones;          // $MIN: -2.0 $MAX: 2.0 $DEFAULT: 0.0  $DESCRIPTION: "dark highlights"
  float highlights;        // $MIN: -2.0 $MAX: 2.0 $DEFAULT: 0.0  $DESCRIPTION: "highlights"
  float whites;            // $MIN: -2.0 $MAX: 2.0 $DEFAULT: 0.0  $DESCRIPTION: "whites"
  float speculars;         // $MIN: -2.0 $MAX: 2.0 $DEFAULT: 0.0  $DESCRIPTION: "speculars"
  float blending;          // $MIN: 0.01 $MAX: 100.0 $DEFAULT: 5.0 $DESCRIPTION: "smoothing diameter"
  float smoothing;         // $DEFAULT: 1.414213562 sqrtf(2.0f)
  float feathering;        // $MIN: 0.01 $MAX: 10000.0 $DEFAULT: 1.0 $DESCRIPTION: "edges refinement/feathering"
  float quantization;      // $MIN: 0.0 $MAX: 2.0 $DEFAULT: 0.0 $DESCRIPTION: "mask quantization"
  float contrast_boost;    // $MIN: -16.0 $MAX: 16.0 $DEFAULT: 0.0 $DESCRIPTION: "mask contrast compensation"
  float exposure_boost;    // $MIN: -16.0 $MAX: 16.0 $DEFAULT: 0.0 $DESCRIPTION: "mask exposure compensation"
  dt_iop_toneequalizer_filter_t filter;         // $DEFAULT: DT_TONEEQ_EIGF
  dt_iop_luminance_mask_method_t lum_estimator; // $DEFAULT: DT_TONEEQ_NORM_2 $DESCRIPTION: "luminance estimator"
  int iterations;          // $MIN: 1 $MAX: 20 $DEFAULT: 1 $DESCRIPTION: "filter diffusion"
  float post_scale;        // $MIN: -3.0 $MAX: 3.0 $DEFAULT: 0.0 $DESCRIPTION: "mask contrast / scale histogram"
  float post_shift;        // $MIN: -4.0 $MAX: 4.0 $DEFAULT: 0.0 $DESCRIPTION: "mask brightness / shift histogram"
  dt_iop_toneequalizer_post_auto_align_t post_auto_align; // $DEFAULT: DT_TONEEQ_ALIGN_CUSTOM $DESCRIPTION: "auto align mask exposure"
} dt_iop_toneequalizer_params_t;


typedef struct dt_iop_toneequalizer_data_t
{
  float factors[NUM_OCTAVES] DT_ALIGNED_ARRAY;
  float correction_lut[NUM_OCTAVES * LUT_RESOLUTION + 1] DT_ALIGNED_ARRAY;
  float blending, feathering, contrast_boost, exposure_boost, quantization, smoothing, post_scale, post_shift;
  float scale;
  int radius;
  int iterations;
  dt_iop_luminance_mask_method_t lum_estimator;
  dt_iop_toneequalizer_filter_t filter;
  dt_iop_toneequalizer_post_auto_align_t post_auto_align;
} dt_iop_toneequalizer_data_t;


typedef struct dt_iop_toneequalizer_global_data_t
{
  // TODO: put OpenCL kernels here at some point
} dt_iop_toneequalizer_global_data_t;


typedef struct dt_iop_toneequalizer_gui_data_t
{
  // Mem arrays 64-bytes aligned - contiguous memory
  float factors[NUM_OCTAVES] DT_ALIGNED_ARRAY;
  float gui_curve[UI_HISTO_SAMPLES] DT_ALIGNED_ARRAY;              // LUT for the UI graph
  GdkRGBA gui_curve_colors[UI_HISTO_SAMPLES] DT_ALIGNED_ARRAY;     // color for the UI graph
  float interpolation_matrix[NUM_SLIDERS * NUM_OCTAVES] DT_ALIGNED_ARRAY;
  int histogram[UI_HISTO_SAMPLES] DT_ALIGNED_ARRAY;                // mask histogram for the UI graph
  int hires_histogram[HIRES_HISTO_SAMPLES] DT_ALIGNED_ARRAY;       // hires mask histogram
  int image_histogram[UI_HISTO_SAMPLES] DT_ALIGNED_ARRAY;          // image histogram for UI graph
  int image_hires_histogram[HIRES_HISTO_SAMPLES] DT_ALIGNED_ARRAY; // hires image histogram
  float temp_user_params[NUM_SLIDERS] DT_ALIGNED_ARRAY;
  float cursor_exposure; // store the exposure value at current cursor position
  float step; // scrolling step

  // 14 int to pack - contiguous memory
  gboolean mask_display;
  int max_histogram;
  int buf_width;
  int buf_height;
  int cursor_pos_x;
  int cursor_pos_y;
  int pipe_order;

  // 6 uint64 to pack - contiguous-ish memory
  dt_hash_t full_upstream_hash;
  dt_hash_t preview_upstream_hash;
  dt_hash_t sync_hash;

  size_t preview_buf_width, preview_buf_height;
  size_t full_buf_width, full_buf_height;

  // Heap arrays, 64 bits-aligned, unknown length
  float *preview_buf;  // For performance and to get the mask luminance under the mouse cursor
  float *full_buf;     // For performance and for displaying the mask as greyscale

  // Misc stuff, contiguity, length and alignment unknown
  float scale;
  float sigma;

  // stats for the mask histogram
  float histogram_first_decile;
  float histogram_last_decile;

  // automatic values for post scale/shift from PREVIEW thread
  float post_scale_value;
  float post_shift_value;

  // stats for the image histogram
  float image_histogram_first_decile;
  float image_histogram_last_decile;
  int max_image_histogram;
  float image_EV_per_UI_sample;
  gboolean two_histograms_display;

  // GTK garbage, nobody cares, no SIMD here
  GtkWidget *noise, *ultra_deep_blacks, *deep_blacks, *blacks, *shadows, *midtones, *highlights, *whites, *speculars;
  GtkDrawingArea *area, *bar;
  GtkWidget *blending, *smoothing, *quantization;
  GtkWidget *post_auto_align;
  GtkWidget *lum_estimator;
  GtkWidget *filter, *feathering, *contrast_boost, *iterations, *exposure_boost, *post_scale, *post_shift;
  GtkNotebook *notebook;
  dt_gui_collapsible_section_t sliders_section, advanced_masking_section;
  GtkWidget *show_luminance_mask;
  GtkWidget *show_two_histograms;

  // Cache Pango and Cairo stuff for the equalizer drawing
  float line_height;
  float sign_width;
  float graph_width;
  float graph_height;
  float gradient_left_limit;
  float gradient_right_limit;
  float gradient_top_limit;
  float gradient_width;
  float legend_top_limit;
  float x_label;
  int inset;
  int inner_padding;

  GtkAllocation allocation;
  cairo_surface_t *cst;
  cairo_t *cr;
  PangoLayout *layout;
  PangoRectangle ink;
  PangoFontDescription *desc;
  GtkStyleContext *context;

  // Event for equalizer drawing
  float nodes_x[NUM_SLIDERS] DT_ALIGNED_ARRAY;
  float nodes_y[NUM_SLIDERS] DT_ALIGNED_ARRAY;
  float area_x; // x coordinate of cursor over graph/drawing area
  float area_y; // y coordinate
  int area_active_node;

  // Flags for UI events
  gboolean valid_nodes_x;      // TRUE if x coordinates of graph nodes have been inited
  gboolean valid_nodes_y;      // TRUE if y coordinates of graph nodes have been inited
  gboolean area_cursor_valid;  // TRUE if mouse cursor is over the graph area
  gboolean area_dragging;      // TRUE if left-button has been pushed
                               // but not released and cursor motion
                               // is recorded
  gboolean cursor_valid;       // TRUE if mouse cursor is over the preview image
  gboolean has_focus;          // TRUE if the widget has the focus from GTK

  // Flags for buffer caches invalidation
  gboolean luminance_valid;     // TRUE if the luminance cache is ready,
                                //      hires_histogram and deciles are valid
  gboolean gui_histogram_valid; // TRUE if the histogram cache and stats are ready
  gboolean graph_valid;         // TRUE if the UI graph view is ready

  // For the curve interpolation
  gboolean interpolation_valid; // TRUE if the interpolation_matrix is ready

  gboolean user_param_valid;    // TRUE if users params set in
                                // interactive view are in bounds
  gboolean factors_valid;       // TRUE if radial-basis coeffs are ready
  gboolean gui_curve_valid;     // TRUE if the gui_curve is ready

  gboolean distort_signal_active;
} dt_iop_toneequalizer_gui_data_t;

/* the signal DT_SIGNAL_DEVELOP_DISTORT is used to refresh the internal
   cached image buffer used for the on-canvas luminance picker. */
static void _set_distort_signal(dt_iop_module_t *self);
static void _unset_distort_signal(dt_iop_module_t *self);

/****************************************************************************
 *
 * Darktable housekeeping functions
 *
 ****************************************************************************/

const char *name()
{
  return _("tone equalizer");
}


const char *aliases()
{
  return _("tone curve|tone mapping|relight|background light|shadows highlights");
}


const char **description(dt_iop_module_t *self)
{
  return dt_iop_set_description
    (self, _("relight the scene as if the lighting was done directly on the scene"),
     _("corrective and creative"),
     _("linear, RGB, scene-referred"),
     _("quasi-linear, RGB"),
     _("quasi-linear, RGB, scene-referred"));
}


int default_group()
{
  return IOP_GROUP_BASIC | IOP_GROUP_GRADING;
}


int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING;
}


dt_iop_colorspace_type_t default_colorspace(dt_iop_module_t *self,
                                            dt_dev_pixelpipe_t *pipe,
                                            dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}


int legacy_params(dt_iop_module_t *self,
                  const void *const old_params,
                  const int old_version,
                  void **new_params,
                  int32_t *new_params_size,
                  int *new_version)
{
  printf("legacy_params old_version=%d\n", old_version);

  typedef struct dt_iop_toneequalizer_params_v3_t
  {
    float noise;
    float ultra_deep_blacks;
    float deep_blacks;
    float blacks;
    float shadows;
    float midtones;
    float highlights;
    float whites;
    float speculars;
    float blending;
    float smoothing;
    float feathering;
    float quantization;
    float contrast_boost;
    float exposure_boost;
    dt_iop_toneequalizer_filter_t filter;
    dt_iop_luminance_mask_method_t lum_estimator;
    int iterations;
    float post_scale;
    float post_shift;
    dt_iop_toneequalizer_post_auto_align_t post_auto_align;
  } dt_iop_toneequalizer_params_v3_t;

  if(old_version == 1)
  {
    typedef struct dt_iop_toneequalizer_params_v1_t
    {
      float noise, ultra_deep_blacks, deep_blacks, blacks;
      float shadows, midtones, highlights, whites, speculars;
      float blending, feathering, contrast_boost, exposure_boost;
      dt_iop_toneequalizer_filter_t filter;
      int iterations;
      dt_iop_luminance_mask_method_t lum_estimator;
    } dt_iop_toneequalizer_params_v1_t;

    const dt_iop_toneequalizer_params_v1_t *o = old_params;
    dt_iop_toneequalizer_params_v3_t *n = malloc(sizeof(dt_iop_toneequalizer_params_v3_t));

    // Olds params
    n->noise = o->noise;
    n->ultra_deep_blacks = o->ultra_deep_blacks;
    n->deep_blacks = o->deep_blacks;
    n->blacks = o->blacks;
    n->shadows = o->shadows;
    n->midtones = o->midtones;
    n->highlights = o->highlights;
    n->whites = o->whites;
    n->speculars = o->speculars;

    n->blending = o->blending;
    n->feathering = o->feathering;
    n->contrast_boost = o->contrast_boost;
    n->exposure_boost = o->exposure_boost;

    n->filter = o->filter;
    n->iterations = o->iterations;
    n->lum_estimator = o->lum_estimator;

    // V2 params
    n->quantization = 0.0f;
    n->smoothing = sqrtf(2.0f);

    // V3 params
    n->post_scale = 0.0f;
    n->post_shift = 0.0f;
    n->post_auto_align = DT_TONEEQ_ALIGN_CUSTOM;

    *new_params = n;
    *new_params_size = sizeof(dt_iop_toneequalizer_params_v3_t);
    *new_version = 3;
    return 0;
  }

  if(old_version == 2)
  {
    typedef struct dt_iop_toneequalizer_params_v2_t
    {
      float noise; float ultra_deep_blacks; float deep_blacks; float blacks;
      float shadows; float midtones; float highlights; float whites;
      float speculars; float blending; float smoothing; float feathering;
      float quantization; float contrast_boost; float exposure_boost;
      dt_iop_toneequalizer_filter_t filter;
      dt_iop_luminance_mask_method_t lum_estimator;
      int iterations;
    } dt_iop_toneequalizer_params_v2_t;

    const dt_iop_toneequalizer_params_v2_t *o = old_params;
    dt_iop_toneequalizer_params_v3_t *n = malloc(sizeof(dt_iop_toneequalizer_params_v3_t));

    // V1 params
    n->noise = o->noise;
    n->ultra_deep_blacks = o->ultra_deep_blacks;
    n->deep_blacks = o->deep_blacks;
    n->blacks = o->blacks;
    n->shadows = o->shadows;
    n->midtones = o->midtones;
    n->highlights = o->highlights;
    n->whites = o->whites;
    n->speculars = o->speculars;

    n->blending = o->blending;
    n->feathering = o->feathering;
    n->contrast_boost = o->contrast_boost;
    n->exposure_boost = o->exposure_boost;

    n->filter = o->filter;
    n->iterations = o->iterations;
    n->lum_estimator = o->lum_estimator;

    // V2 params
    n->quantization = o->quantization;
    n->smoothing = o->smoothing;

    // V3 params
    n->post_scale = 0.0f;
    n->post_shift = 0.0f;
    n->post_auto_align = DT_TONEEQ_ALIGN_CUSTOM;

    *new_params = n;
    *new_params_size = sizeof(dt_iop_toneequalizer_params_v3_t);
    *new_version = 3;
    return 0;
  }

  return 1;
}


/****************************************************************************
 *
 * Presets
 *
 ****************************************************************************/
static void compress_shadows_highlight_preset_set_exposure_params
  (dt_iop_toneequalizer_params_t* p,
   const float step)
{
  // this function is used to set the exposure params for the 4 "compress shadows
  // highlights" presets, which use basically the same curve, centered around
  // -4EV with an exposure compensation that puts middle-grey at -4EV.
  p->noise = step;
  p->ultra_deep_blacks = 5.f / 3.f * step;
  p->deep_blacks = 5.f / 3.f * step;
  p->blacks = step;
  p->shadows = 0.0f;
  p->midtones = -step;
  p->highlights = -5.f / 3.f * step;
  p->whites = -5.f / 3.f * step;
  p->speculars = -step;
}


static void dilate_shadows_highlight_preset_set_exposure_params
  (dt_iop_toneequalizer_params_t* p,
   const float step)
{
  // create a tone curve meant to be used without filter (as a flat,
  // non-local, 1D tone curve) that reverts the local settings above.
  p->noise = -15.f / 9.f * step;
  p->ultra_deep_blacks = -14.f / 9.f * step;
  p->deep_blacks = -12.f / 9.f * step;
  p->blacks = -8.f / 9.f * step;
  p->shadows = 0.f;
  p->midtones = 8.f / 9.f * step;
  p->highlights = 12.f / 9.f * step;
  p->whites = 14.f / 9.f * step;
  p->speculars = 15.f / 9.f * step;
}


void init_presets(dt_iop_module_so_t *self)
{
  dt_iop_toneequalizer_params_t p;
  memset(&p, 0, sizeof(p));

  p.lum_estimator = DT_TONEEQ_NORM_POWER;
  p.contrast_boost = 0.0f;
  p.filter = DT_TONEEQ_NONE;
  p.exposure_boost = -0.5f;
  p.feathering = 1.0f;
  p.iterations = 1;
  p.smoothing = sqrtf(2.0f);
  p.quantization = 0.0f;
  p.post_scale = 0.0f;
  p.post_shift = 0.0f;
  p.post_auto_align = DT_TONEEQ_ALIGN_CUSTOM;

  // Init exposure settings
  p.noise = p.ultra_deep_blacks = p.deep_blacks = p.blacks = 0.0f;
  p.shadows = p.midtones = p.highlights = p.whites = p. speculars = 0.0f;

  // No blending
  dt_gui_presets_add_generic
    (_("simple tone curve"), self->op,
     self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_SCENE);

  // Simple utils blendings
  p.filter = DT_TONEEQ_EIGF;
  p.lum_estimator = DT_TONEEQ_NORM_2;

  p.blending = 5.0f;
  p.feathering = 1.0f;
  p.iterations = 1;
  p.quantization = 0.0f;
  p.exposure_boost = 0.0f;
  p.contrast_boost = 0.0f;
  dt_gui_presets_add_generic
    (_("mask blending: all purposes"), self->op,
     self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_SCENE);

  p.blending = 1.0f;
  p.feathering = 10.0f;
  p.iterations = 3;
  dt_gui_presets_add_generic
    (_("mask blending: people with backlight"), self->op,
     self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_SCENE);

  // Shadows/highlights presets
  // move middle-grey to the center of the range
  p.exposure_boost = -1.57f;
  p.contrast_boost = 0.0f;
  p.blending = 2.0f;
  p.feathering = 50.0f;
  p.iterations = 5;
  p.quantization = 0.0f;

  // slight modification to give higher compression
  p.filter = DT_TONEEQ_EIGF;
  p.feathering = 20.0f;
  compress_shadows_highlight_preset_set_exposure_params(&p, 0.65f);
  dt_gui_presets_add_generic
    (_("compress shadows/highlights (EIGF): strong"), self->op,
     self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_SCENE);
  p.filter = DT_TONEEQ_GUIDED;
  p.feathering = 500.0f;
  dt_gui_presets_add_generic
    (_("compress shadows/highlights (GF): strong"), self->op,
     self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_SCENE);

  p.filter = DT_TONEEQ_EIGF;
  p.blending = 3.0f;
  p.feathering = 7.0f;
  p.iterations = 3;
  compress_shadows_highlight_preset_set_exposure_params(&p, 0.45f);
  dt_gui_presets_add_generic
    (_("compress shadows/highlights (EIGF): medium"), self->op,
     self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_SCENE);
  p.filter = DT_TONEEQ_GUIDED;
  p.feathering = 500.0f;
  dt_gui_presets_add_generic
    (_("compress shadows/highlights (GF): medium"), self->op,
     self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_SCENE);

  p.filter = DT_TONEEQ_EIGF;
  p.blending = 5.0f;
  p.feathering = 1.0f;
  p.iterations = 1;
  compress_shadows_highlight_preset_set_exposure_params(&p, 0.25f);
  dt_gui_presets_add_generic
    (_("compress shadows/highlights (EIGF): soft"), self->op,
     self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_SCENE);
  p.filter = DT_TONEEQ_GUIDED;
  p.feathering = 500.0f;
  dt_gui_presets_add_generic
    (_("compress shadows/highlights (GF): soft"), self->op,
     self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_SCENE);

  // build the 1D contrast curves that revert the local compression of
  // contrast above
  p.filter = DT_TONEEQ_NONE;
  dilate_shadows_highlight_preset_set_exposure_params(&p, 0.25f);
  dt_gui_presets_add_generic
    (_("contrast tone curve: soft"), self->op,
     self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_SCENE);

  dilate_shadows_highlight_preset_set_exposure_params(&p, 0.45f);
  dt_gui_presets_add_generic
    (_("contrast tone curve: medium"), self->op,
     self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_SCENE);

  dilate_shadows_highlight_preset_set_exposure_params(&p, 0.65f);
  dt_gui_presets_add_generic
    (_("contrast tone curve: strong"), self->op,
     self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_SCENE);

  // relight
  p.filter = DT_TONEEQ_EIGF;
  p.blending = 5.0f;
  p.feathering = 1.0f;
  p.iterations = 1;
  p.quantization = 0.0f;
  p.exposure_boost = -0.5f;
  p.contrast_boost = 0.0f;

  p.noise = 0.0f;
  p.ultra_deep_blacks = 0.15f;
  p.deep_blacks = 0.6f;
  p.blacks = 1.15f;
  p.shadows = 1.33f;
  p.midtones = 1.15f;
  p.highlights = 0.6f;
  p.whites = 0.15f;
  p.speculars = 0.0f;

  dt_gui_presets_add_generic
    (_("relight: fill-in"), self->op,
     self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_SCENE);
}


/****************************************************************************
 *
 * Functions that are needed by process and therefore
 * are part of worker threads
 *
 ****************************************************************************/
__DT_CLONE_TARGETS__
static inline void compute_hires_histogram_and_stats(const float *const restrict luminance,
                                                     int hires_histogram[HIRES_HISTO_SAMPLES],
                                                     const size_t num_elem,
                                                     float *first_decile,
                                                     float *last_decile,
                                                     dt_dev_pixelpipe_type_t const debug_pipe)
{
  printf("compute_hires_histogram_and_stats pipe=%d num_elem=%ld\n", debug_pipe, num_elem);
  // The GUI histogram comprises 8 EV (UI_HISTO_SAMPLES, -8 to 0).
  // The high resolution histogram extends this to an exta 8 EV before and
  // 8EV after, for a total of 24.
  // Also the resolution is increased to compensate for the fact that the user
  // can scale the histogram.
  const float temp_ev_range = HIRES_HISTO_MAX_EV - HIRES_HISTO_MIN_EV;

  // (Re)init the histogram
  memset(hires_histogram, 0, sizeof(int) * HIRES_HISTO_SAMPLES);

  // Split exposure in bins
  DT_OMP_FOR_SIMD(reduction(+:hires_histogram[:HIRES_HISTO_SAMPLES]))
  for(size_t k = 0; k < num_elem; k++)
  {
    const int index =
      CLAMP((int)(((log2f(luminance[k]) - HIRES_HISTO_MIN_EV) / temp_ev_range) * (float)HIRES_HISTO_SAMPLES),
            0, HIRES_HISTO_SAMPLES - 1);
            hires_histogram[index] += 1;
  }

  // printf("hires_histogram 0: %d 256: %d 512: %d 767: %d\n", hires_histogram[0], hires_histogram[256], hires_histogram[512], hires_histogram[767]);

  const int first_decile_pop = (int)((float)num_elem * 0.05f);
  const int last_decile_pop = (int)((float)num_elem * (1.0f - 0.95f));
  int population = 0;
  int first_decile_pos = 0;
  int last_decile_pos = 0;
  int k;

  // Scout the extended histogram bins looking for the
  // absolute first and last non-zero values and for deciles.
  // These would not be accurate with the gui histogram.

  for(k = 0; k < HIRES_HISTO_SAMPLES; ++k)
  {
    population += hires_histogram[k];
    if (population >= first_decile_pop)
    {
      first_decile_pos = k;
      break;
    }
  }

  population = 0;
  for(k = HIRES_HISTO_SAMPLES - 1; k >= 0; --k)
  {
    population += hires_histogram[k];
    if (population >= last_decile_pop)
    {
      last_decile_pos = k;
      break;
    }
  }
  // printf("First pos: %d, Last pos: %d\n", first_pos, last_pos);

  // Convert decile positions to exposures
  *first_decile = (temp_ev_range * ((float)first_decile_pos / (float)(HIRES_HISTO_SAMPLES - 1))) + HIRES_HISTO_MIN_EV;
  *last_decile = (temp_ev_range * ((float)last_decile_pos / (float)(HIRES_HISTO_SAMPLES - 1))) + HIRES_HISTO_MIN_EV;
}

<<<<<<< HEAD
static void hash_set_get(const dt_hash_t *hash_in,
                         dt_hash_t *hash_out,
                         dt_pthread_mutex_t *lock)
{
  // Set or get a hash in a struct the thread-safe way
  dt_pthread_mutex_lock(lock);
  *hash_out = *hash_in;
  dt_pthread_mutex_unlock(lock);
}


static void invalidate_luminance_cache(dt_iop_module_t *const self)
{
  // Invalidate the private luminance cache and histogram when
  // the luminance mask extraction parameters have changed
  dt_iop_toneequalizer_gui_data_t *const restrict g = self->gui_data;

  dt_iop_gui_enter_critical_section(self);
  g->max_histogram = 1;
  g->luminance_valid = FALSE;
  g->histogram_valid = FALSE;
  g->thumb_preview_hash = DT_INVALID_HASH;
  g->ui_preview_hash = DT_INVALID_HASH;
  dt_iop_gui_leave_critical_section(self);
  dt_iop_refresh_all(self);
}

// gaussian-ish kernel - sum is == 1.0f so we don't care much about actual coeffs
static const dt_colormatrix_t gauss_kernel =
  { { 0.076555024f, 0.124401914f, 0.076555024f },
    { 0.124401914f, 0.196172249f, 0.124401914f },
    { 0.076555024f, 0.124401914f, 0.076555024f } };
=======
>>>>>>> acabda0cbe (2025-04-06 preview version with post-shift, post_scale, auto-align and curve coloring.)

__DT_CLONE_TARGETS__
static inline void compute_luminance_mask(const float *const restrict in,
                                          float *const restrict luminance,
                                          const size_t width,
                                          const size_t height,
                                          const dt_iop_toneequalizer_data_t *const d,
                                          const gboolean compute_image_stats,       // Optionally get the histogram of the image
                                          int hires_histogram[HIRES_HISTO_SAMPLES], // only for compute_image_stats 
                                          float *first_decile,                      // only for compute_image_stats
                                          float *last_decile,                       // only for compute_image_stats
                                          dt_dev_pixelpipe_type_t const debug_pipe)
{
  printf("compute_luminance_mask pipe=%d width=%ld height=%ld first luminance=%f, compute stats=%d\n", debug_pipe, width, height, luminance[0], compute_image_stats);
  const int num_elem = width * height;
  switch(d->filter)
  {
    case(DT_TONEEQ_NONE):
    {
      // No contrast boost here
      luminance_mask(in, luminance, width, height,
                     d->lum_estimator, d->exposure_boost, 0.0f, 1.0f);
      if (compute_image_stats)
          compute_hires_histogram_and_stats(in, hires_histogram, num_elem, first_decile, last_decile, debug_pipe);
      break;
    }

    case(DT_TONEEQ_AVG_GUIDED):
    {
      // Still no contrast boost
      luminance_mask(in, luminance, width, height,
                     d->lum_estimator, d->exposure_boost, 0.0f, 1.0f);
      if (compute_image_stats)
          compute_hires_histogram_and_stats(in, hires_histogram, num_elem, first_decile, last_decile, debug_pipe);
      fast_surface_blur(luminance, width, height, d->radius, d->feathering, d->iterations,
                        DT_GF_BLENDING_GEOMEAN, d->scale, d->quantization,
                        exp2f(-14.0f), 4.0f);
      break;
    }

    case(DT_TONEEQ_GUIDED):
    {
      // Contrast boosting is done around the average luminance of the mask.
      // This is to make exposure corrections easier to control for users, by spreading
      // the dynamic range along all exposure NUM_SLIDERS, because guided filters
      // tend to flatten the luminance mask a lot around an average ± 2 EV
      // which makes only 2-3 NUM_SLIDERS usable.
      // we assume the distribution is centered around -4EV, e.g. the center of the nodes
      // the exposure boost should be used to make this assumption true
      luminance_mask(in, luminance, width, height, d->lum_estimator, d->exposure_boost,
                     CONTRAST_FULCRUM, d->contrast_boost);
      if (compute_image_stats)
          compute_hires_histogram_and_stats(in, hires_histogram, num_elem, first_decile, last_decile, debug_pipe);
      fast_surface_blur(luminance, width, height, d->radius, d->feathering, d->iterations,
                        DT_GF_BLENDING_LINEAR, d->scale, d->quantization,
                        exp2f(-14.0f), 4.0f);
      break;
    }

    case(DT_TONEEQ_AVG_EIGF):
    {
      // Still no contrast boost
      luminance_mask(in, luminance, width, height,
                     d->lum_estimator, d->exposure_boost, 0.0f, 1.0f);
      if (compute_image_stats)
          compute_hires_histogram_and_stats(in, hires_histogram, num_elem, first_decile, last_decile, debug_pipe);
      fast_eigf_surface_blur(luminance, width, height,
                             d->radius, d->feathering, d->iterations,
                             DT_GF_BLENDING_GEOMEAN, d->scale,
                             d->quantization, exp2f(-14.0f), 4.0f);
      break;
    }

    case(DT_TONEEQ_EIGF):
    {
      luminance_mask(in, luminance, width, height, d->lum_estimator, d->exposure_boost,
                     CONTRAST_FULCRUM, d->contrast_boost);
      if (compute_image_stats)
          compute_hires_histogram_and_stats(in, hires_histogram, num_elem, first_decile, last_decile, debug_pipe);
      fast_eigf_surface_blur(luminance, width, height,
                             d->radius, d->feathering, d->iterations,
                             DT_GF_BLENDING_LINEAR, d->scale,
                             d->quantization, exp2f(-14.0f), 4.0f);
      break;
    }

    default:
    {
      luminance_mask(in, luminance, width, height,
                     d->lum_estimator, d->exposure_boost, 0.0f, 1.0f);
      if (compute_image_stats)
        compute_hires_histogram_and_stats(in, hires_histogram, num_elem, first_decile, last_decile, debug_pipe);
      break;
    }
  }
}


// This is similar to exposure/contrast boost.
// However it is applied AFTER the guided filter calculation, so it is much
// easier to control and does not mess with the detail detection of the
// guided filter.
static inline float post_scale_shift(const float v, const float post_scale, const float post_shift)
{
  const float scale_exp = exp2f(post_scale);
  // signifficant range -8..0, centering around the middle
  return (v + 4.0f) * scale_exp - 4.0f + post_shift;
}


// This is similar to the auto-buttons for exposure/contrast boost.
// However it runs automatically in the pipe, so it does not need to be
// triggered by the user each time the upstream exposure changes.
void compute_auto_post_scale_shift(float *post_scale, float *post_shift,
                                   dt_iop_toneequalizer_post_auto_align_t post_auto_align,
                                   float histogram_first_decile,
                                   float histogram_last_decile,
                                   dt_dev_pixelpipe_type_t const debug_pipe
                                  )
{
  const float first_decile_target = -7.0f;
  const float last_decile_target = -1.0f;
  const float pivot = -4.0f; // for scaling

  printf("compute_auto_post_shift_scale: Pipe=%d old post_scale=%f post_shift=%f histogram_first_decile=%f histogram_last_decile=%f\n",
          debug_pipe, *post_scale, *post_shift, histogram_first_decile, histogram_last_decile);

  switch(post_auto_align)
  {
    case(DT_TONEEQ_ALIGN_CUSTOM):
    {
      // fully user-controlled, do not modify
      break;
    }
    case(DT_TONEEQ_ALIGN_LEFT):
    {
      // auto-align at shadows
      // the histogram might be scaled around pivot
      const float scaled_first_decile = (histogram_first_decile - pivot) * exp2f(*post_scale) + pivot;
      *post_shift = first_decile_target - scaled_first_decile;
      break;
    }
    case(DT_TONEEQ_ALIGN_CENTER):
    {
      const float histogram_middle = (histogram_first_decile + histogram_last_decile) / 2.0f;
      const float target_middle = (first_decile_target + last_decile_target) / 2.0f;
      const float scaled_middle = (histogram_middle - pivot) * exp2f(*post_scale) + pivot;
      *post_shift = target_middle - scaled_middle;
      break;
    }
    case(DT_TONEEQ_ALIGN_RIGHT):
    {
      // auto-align at highlights
      const float scaled_last_decile = (histogram_last_decile - pivot) * exp2f(*post_scale) + pivot;
      *post_shift = last_decile_target - scaled_last_decile;
      break;
    }
    case(DT_TONEEQ_ALIGN_FIT):
    {
      // fully fit
      *post_scale = log2f((last_decile_target - first_decile_target) / (histogram_last_decile - histogram_first_decile));
      const float scaled_first_decile = (histogram_first_decile - pivot) * exp2f(*post_scale) + pivot;
      *post_shift = first_decile_target - scaled_first_decile;
      break;
    }
  }
  printf("compute_auto_post_shift_scale: New post_scale=%f post_shift=%f\n", *post_scale, *post_shift);
};


__DT_CLONE_TARGETS__
static inline void display_luminance_mask(const float *const restrict in,
                                          const float *const restrict luminance,
                                          float *const restrict out,
                                          const dt_iop_roi_t *const roi_in,
                                          const dt_iop_roi_t *const roi_out,
                                          const float post_scale,
                                          const float post_shift,
                                          dt_dev_pixelpipe_type_t const debug_pipe)
{
  const size_t offset_x = (roi_in->x < roi_out->x) ? -roi_in->x + roi_out->x : 0;
  const size_t offset_y = (roi_in->y < roi_out->y) ? -roi_in->y + roi_out->y : 0;

  printf("display_luminance_mask pipe=%d offset_x=%ld offset_y=%ld roi_in %d %d %d %d roi_out %d %d %d %d, post_scale=%f, post_shift=%f\n",
         debug_pipe, offset_x, offset_y,
         roi_in->x, roi_in->y, roi_in->width, roi_in->height,
         roi_out->x, roi_out->y, roi_out->width, roi_out->height,
         post_scale, post_shift);

  // The output dimensions need to be smaller or equal to the input ones
  // there is no logical reason they shouldn't, except some weird bug in the pipe
  // in this case, ensure we don't segfault
  const size_t in_width = roi_in->width;
  const size_t out_width = (roi_in->width > roi_out->width)
    ? roi_out->width
    : roi_in->width;

  const size_t out_height = (roi_in->height > roi_out->height)
    ? roi_out->height
    : roi_in->height;

  DT_OMP_FOR(collapse(2))
  for(size_t i = 0 ; i < out_height; ++i)
    for(size_t j = 0; j < out_width; ++j)
    {
      // normalize the mask intensity between -8 EV and 0 EV for clarity,
      // and add a "gamma" 2.0 for better legibility in shadows
      const int lum_index = (i + offset_y) * in_width + (j + offset_x);
      const float lum_log = log2f(luminance[lum_index]);
      const float lum_corrected = post_scale_shift(lum_log, post_scale, post_shift);

      // IMHO it would be fine, to show the log version of the mask to the user.
      // const float intensity =
      //   fminf(fmaxf((lum_corrected + 8.0f) / 8.0f, 0.f), 1.f);
      // However to keep everything identical to before, we go back to linear
      // space and apply the square root/"gamma".
      const float lum_linear = exp2f(lum_corrected);
      const float intensity =
        sqrtf(fminf(
                fmaxf(lum_linear - 0.00390625f, 0.f) / 0.99609375f,
                1.f));

      const size_t index = (i * out_width + j) * 4;
      // set gray level for the mask
      for_each_channel(c,aligned(out))
      {
        out[index + c] = intensity;
      }
      // copy alpha channel
      out[index + 3] = in[((i + offset_y) * in_width + (j + offset_x)) * 4 + 3];
    }
}


/***
 * Exposure compensation computation
 *
 * Construct the final correction factor by summing the octaves
 * NUM_SLIDERS gains weighted by the gaussian of the radial distance
 * (pixel exposure - octave center)
 *
 ***/
DT_OMP_DECLARE_SIMD()
__DT_CLONE_TARGETS__
static float gaussian_denom(const float sigma)
{
  // Gaussian function denominator such that y = exp(- radius^2 / denominator)
  // this is the constant factor of the exponential, so we don't need to recompute it
  // for every single pixel
  return 2.0f * sigma * sigma;
}


DT_OMP_DECLARE_SIMD()
__DT_CLONE_TARGETS__
static float gaussian_func(const float radius, const float denominator)
{
  // Gaussian function without normalization
  // this is the variable part of the exponential
  // the denominator should be evaluated with `gaussian_denom`
  // ahead of the array loop for optimal performance
  return expf(- radius * radius / denominator);
}


static void compute_correction_lut(float *restrict lut, const float sigma,
                                   const float *const restrict factors,
                                   const float post_scale, const float post_shift,
                                   dt_dev_pixelpipe_type_t const debug_pipe)
{
  printf("compute_correction_lut pipe=%d, post_scale=%f, post_shift=%f\n", debug_pipe, post_scale, post_shift);
  const float gauss_denom = gaussian_denom(sigma);
  assert(NUM_OCTAVES == 8);

  // TODO MF: Does the openmp still work here?
  DT_OMP_FOR(shared(centers_ops))
  for(int j = 0; j <= LUT_RESOLUTION * NUM_OCTAVES; j++)
  {
    // build the correction for each pixel
    // as the sum of the contribution of each luminance channelcorrection
    const float exposure_uncorrected = (float)j / (float)LUT_RESOLUTION + DT_TONEEQ_MIN_EV; // [-8...0] EV
    const float exposure = fast_clamp(post_scale_shift(exposure_uncorrected, post_scale, post_shift),
                                      DT_TONEEQ_MIN_EV, DT_TONEEQ_MAX_EV);
    float result = 0.0f;
    for(int i = 0; i < NUM_OCTAVES; i++)
    {
      result += gaussian_func(exposure - centers_ops[i], gauss_denom) * factors[i];
    }
    // the user-set correction is expected in [-2;+2] EV, so is the interpolated one
    lut[j] = fast_clamp(result, 0.25f, 4.0f);
  }
}


#if DT_TONEEQ_USE_LUT
// this is the version currently used, as using a lut gives a
// big performance speedup on some cpus
__DT_CLONE_TARGETS__
static inline void apply_toneequalizer(const float *const restrict in,
                                       const float *const restrict luminance,
                                       float *const restrict out,
                                       const dt_iop_roi_t *const roi_in,
                                       const dt_iop_roi_t *const roi_out,
                                       const dt_iop_toneequalizer_data_t *const d,
                                       dt_dev_pixelpipe_type_t const debug_pipe)
{
  printf("apply_toneequalizer pipe=%d first luminance=%f roi_in %d %d %d %d roi_out %d %d %d %d post_scale=%f, post_shift=%f\n",
    debug_pipe, luminance[0],
    roi_in->x, roi_in->y, roi_in->width, roi_in->height,
    roi_out->x, roi_out->y, roi_out->width, roi_out->height,
    d->post_scale, d->post_shift);
  const size_t npixels = (size_t)roi_in->width * roi_in->height;
  const float* restrict lut = d->correction_lut;
  const float lutres = LUT_RESOLUTION;

  DT_OMP_FOR()
  for(size_t k = 0; k < npixels; k++)
  {
    // The radial-basis interpolation is valid in [-8; 0] EV and can quickly diverge outside.
    // Note: not doing an explicit lut[index] check is safe as long we take care of proper
    // DT_TONEEQ_MIN_EV and DT_TONEEQ_MAX_EV and allocated lut size LUT_RESOLUTION+1
    const float exposure = fast_clamp(log2f(luminance[k]), DT_TONEEQ_MIN_EV, DT_TONEEQ_MAX_EV);
    const float correction = lut[(unsigned)roundf((exposure - DT_TONEEQ_MIN_EV) * lutres)];
    // apply correction
    for_each_channel(c)
      out[4 * k + c] = correction * in[4 * k + c];
  }
}

#else

// we keep this version for further reference (e.g. for implementing
// a gpu version)
// TODO MF: Remove? This is no longer correct anyways.
__DT_CLONE_TARGETS__
static inline void apply_toneequalizer(const float *const restrict in,
                                       const float *const restrict luminance,
                                       float *const restrict out,
                                       const dt_iop_roi_t *const roi_in,
                                       const dt_iop_roi_t *const roi_out,
                                       const dt_iop_toneequalizer_data_t *const d)
{
  const size_t num_elem = roi_in->width * roi_in->height;
  const float *const restrict factors = d->factors;
  const float sigma = d->smoothing;
  const float gauss_denom = gaussian_denom(sigma);

  DT_OMP_FOR(shared(centers_ops))
  for(size_t k = 0; k < num_elem; ++k)
  {
    // build the correction for the current pixel
    // as the sum of the contribution of each luminance channelcorrection
    float result = 0.0f;

    // The radial-basis interpolation is valid in [-8; 0] EV and can
    // quickely diverge outside
    const float exposure = fast_clamp(log2f(luminance[k]), DT_TONEEQ_MIN_EV, DT_TONEEQ_MAX_EV);

    DT_OMP_SIMD(aligned(luminance, centers_ops, factors:64) safelen(NUM_OCTAVES) reduction(+:result))
    for(int i = 0; i < NUM_OCTAVES; ++i)
      result += gaussian_func(exposure - centers_ops[i], gauss_denom) * factors[i];

    // the user-set correction is expected in [-2;+2] EV, so is the interpolated one
    const float correction = fast_clamp(result, 0.25f, 4.0f);

    // apply correction
    for_each_channel(c)
      out[4 * k + c] = correction * in[4 * k + c];
  }
}
#endif // USE_LUT


__DT_CLONE_TARGETS__
static
void toneeq_process(dt_iop_module_t *self,
                    dt_dev_pixelpipe_iop_t *piece,
                    const void *const restrict ivoid,
                    void *const restrict ovoid,
                    const dt_iop_roi_t *const roi_in,
                    const dt_iop_roi_t *const roi_out)
{
  dt_iop_toneequalizer_data_t *const d = piece->data;
  dt_iop_toneequalizer_gui_data_t *const g = self->gui_data;

  const float *const restrict in = (float *const)ivoid;
  float *const restrict out = (float *const)ovoid;

  const size_t width = roi_in->width;
  const size_t height = roi_in->height;
  const size_t num_elem = width * height;

  // Sanity checks
  if(width < 1 || height < 1) return;
  if(roi_in->width < roi_out->width || roi_in->height < roi_out->height)
    return; // input should be at least as large as output
  if(piece->colors != 4) return;  // we need RGB signal

  // This will be local memory or global cache stored in g
  float *restrict luminance = NULL;
  int *hires_histogram = NULL;

  // Remember to free stuff that is allocated here
  gboolean local_luminance = FALSE;
  gboolean local_hires_hist = FALSE;

  printf("toneeq_process, piece type %d, post_align=%d, post_scale=%f, post_shift=%f, roi with=%d, roi_height=%d\n",
    piece->pipe->type, d->post_auto_align, d->post_scale, d->post_shift, roi_in->width, roi_in->height);

  /**************************************************************************
   * Initialization
   **************************************************************************/
  if(self->dev->gui_attached)
  {
    // If the module instance has changed order in the pipe, invalidate the caches
    if(g->pipe_order != piece->module->iop_order)
    {
      dt_iop_gui_enter_critical_section(self);
<<<<<<< HEAD
      g->ui_preview_hash = DT_INVALID_HASH;
      g->thumb_preview_hash = DT_INVALID_HASH;
=======
      g->full_upstream_hash = DT_INVALID_CACHEHASH;
      g->preview_upstream_hash = DT_INVALID_CACHEHASH;
>>>>>>> acabda0cbe (2025-04-06 preview version with post-shift, post_scale, auto-align and curve coloring.)
      g->pipe_order = piece->module->iop_order;
      g->luminance_valid = FALSE;
      g->gui_histogram_valid = FALSE;
      dt_iop_gui_leave_critical_section(self);
    }

    if(piece->pipe->type & DT_DEV_PIXELPIPE_PREVIEW)
    {
      // For DT_DEV_PIXELPIPE_PREVIEW, we need to cache the luminace mask
      // and the hires histogram for the GUI thread.
      // Locks are required since GUI reads and writes on that buffer.

      // TODO MF: Is the above comment correct? Except for gui_cache_init
      // there seems to be no place where the GUI writes.

      // Re-allocate a new buffer if the thumb preview size has changed
      dt_iop_gui_enter_critical_section(self);
      if(g->preview_buf_width != width || g->preview_buf_height != height)
      {
        dt_free_align(g->preview_buf);
        g->preview_buf = dt_alloc_align_float(num_elem);
        g->preview_buf_width = width;
        g->preview_buf_height = height;
        g->luminance_valid = FALSE;
      }

      luminance = g->preview_buf;
      hires_histogram = g->hires_histogram;

      dt_iop_gui_leave_critical_section(self);
    }
    else if (piece->pipe->type & DT_DEV_PIXELPIPE_FULL)
    {
      // For DT_DEV_PIXELPIPE_FULL, we cache the luminance mask for performance
      // but it's not accessed from GUI
      // no need for threads lock since no other function is writing/reading that buffer
      // This is also used to quickly switch between the mask display and the main view.

      // Re-allocate a new buffer if the full preview size has changed
      if(g->full_buf_width != width || g->full_buf_height != height)
      {
        dt_free_align(g->full_buf);
        g->full_buf = dt_alloc_align_float(num_elem);
        g->full_buf_width = width;
        g->full_buf_height = height;
      }

      luminance = g->full_buf;

      hires_histogram = dt_alloc_align_int(HIRES_HISTO_SAMPLES);
      local_hires_hist = TRUE;
    }
    else
    {
      luminance = dt_alloc_align_float(num_elem);
      local_luminance = TRUE;
      hires_histogram = dt_alloc_align_int(HIRES_HISTO_SAMPLES);
      local_hires_hist = TRUE;
    }
  }
  else
  {
    // no interactive editing/caching : just allocate a local temp buffer
    luminance = dt_alloc_align_float(num_elem);
    local_luminance = TRUE;
    hires_histogram = dt_alloc_align_int(HIRES_HISTO_SAMPLES);
    local_hires_hist = TRUE;
  }

  // Check if the luminance buffer exists
  if(!luminance)
  {
    dt_control_log(_("tone equalizer failed to allocate memory, check your RAM settings"));
    return;
  }

  // The values from d are set by the user.
  // If the user requested auto-alignment, these will be changed.
  float post_scale = d->post_scale;
  float post_shift = d->post_shift;

  /**************************************************************************
   * Compute the luminance mask
   **************************************************************************/
  if(self->dev->gui_attached && (piece->pipe->type & DT_DEV_PIXELPIPE_PREVIEW))
  {
    // PREVIEW sees the whole image (at a lower resolution).
    // PREVIEW needs to store the luminance mask, hires_histogram and deciles
    // for GUI access.
    // PREVIEW also computes post_scale and post_shift for FULL.

    // We use the upstream hash to check if the upstream pipe has changed,
    // which requires us to re-compute this pipe's luminance mask.
    const dt_hash_t current_upstream_hash
        = dt_dev_hash_plus(self->dev, self->dev->preview_pipe, self->iop_order, DT_DEV_TRANSFORM_DIR_BACK_EXCL);

    dt_iop_gui_enter_critical_section(self);
    const dt_hash_t saved_upstream_hash = g->preview_upstream_hash;
    const gboolean luminance_valid = g->luminance_valid;
    dt_iop_gui_leave_critical_section(self);

    printf("toneeq_process PIXELPIPE_PREVIEW: hash=%"PRIu64" saved_hash=%"PRIu64" luminance_valid=%d\n", current_upstream_hash, saved_upstream_hash,
           luminance_valid);

    if(saved_upstream_hash != current_upstream_hash || !luminance_valid)
    {
      /* compute only if upstream pipe state has changed */
      dt_iop_gui_enter_critical_section(self);
      g->preview_upstream_hash = current_upstream_hash;
      g->gui_histogram_valid = FALSE;
      compute_luminance_mask(in, luminance, width, height, d,
                             TRUE, // Also compute an image (not mask!) histogram for coloring the curve
                             g->image_hires_histogram,
                             &g->image_histogram_first_decile, &g->image_histogram_last_decile,
                             piece->pipe->type);

      // Histogram and deciles
      compute_hires_histogram_and_stats(luminance, hires_histogram, num_elem,
                                        &g->histogram_first_decile, &g->histogram_last_decile,
                                        piece->pipe->type);

      // GUI can assume that mask, histogram and deciles are valid
      g->luminance_valid = TRUE;

      compute_auto_post_scale_shift(&post_scale, &post_shift,
        d->post_auto_align,
        g->histogram_first_decile, g->histogram_last_decile,
        piece->pipe->type);

      // save for FULL
      g->post_scale_value = post_scale;
      g->post_shift_value = post_shift;

      dt_iop_gui_leave_critical_section(self);
    }
    else
    {
      // No need to re-compute mask, histogram, deciles.
      // We re-use stored deciles for auto alignment.
      dt_iop_gui_enter_critical_section(self);

      compute_auto_post_scale_shift(&post_scale, &post_shift,
        d->post_auto_align,
        g->histogram_first_decile, g->histogram_last_decile,
        piece->pipe->type);

      // for FULL
      g->post_scale_value = post_scale;
      g->post_shift_value = post_shift;

      dt_iop_gui_leave_critical_section(self);
    }

    // TODO MF: Not completely sure in which cases this must be called.
    //          Assumption is once per output image change by this module and
    //          only when the GUI is active.
    dt_dev_pixelpipe_cache_invalidate_later(piece->pipe, self->iop_order);

    // Sync hash to make FULL wait if it needs auto-alignment
    // TODO MF: Is it necessary to do this in two steps to prevent DT getting stuck in a race condition?
    const dt_hash_t sync_hash = dt_dev_hash_plus(self->dev, self->dev->preview_pipe, self->iop_order, DT_DEV_TRANSFORM_DIR_BACK_INCL);
    dt_iop_gui_enter_critical_section(self);
    g->sync_hash = sync_hash;
    dt_iop_gui_leave_critical_section(self);

  }
  else if(self->dev->gui_attached && (piece->pipe->type & DT_DEV_PIXELPIPE_FULL))
  {
    // FULL may only see a part of the image if the user has zoomed in.
    // We need to compute a luminance mask for this pipe and cache it for
    // quick reuse (i.e. if the user only changes the curve).
    // But we can not compute statistics like histograms or deciles here,
    // so we re-use values that PREVIEW has stored in g.

    // We use the upstream hash to check if the upstream pipe has changed,
    // which requires us to re-compute this pipe's luminance mask.
    // TODO: Is this correct? We take the same hash as in PREVIEW, using self->dev->preview_pipe,
    //       assuming that this can also be used in FULL to detect upstream changes.
    const dt_hash_t current_upstream_hash
        = dt_dev_hash_plus(self->dev, self->dev->preview_pipe, self->iop_order, DT_DEV_TRANSFORM_DIR_BACK_EXCL);

    dt_iop_gui_enter_critical_section(self);
    const dt_hash_t saved_upstream_hash = g->full_upstream_hash;
    const gboolean luminance_valid = g->luminance_valid;
    dt_iop_gui_leave_critical_section(self);

    printf("toneeq_process GUI FULL: hash=%"PRIu64" saved_hash=%"PRIu64" luminance_valid=%d\n", current_upstream_hash, saved_upstream_hash,
           luminance_valid);

    // Re-compute if the upstream state has changed
    if(current_upstream_hash != saved_upstream_hash || !luminance_valid)
    {
      /* compute only if upstream pipe state has changed */
      dt_iop_gui_enter_critical_section(self);
      g->full_upstream_hash = current_upstream_hash;
      dt_iop_gui_leave_critical_section(self);

      compute_luminance_mask(in, luminance, width, height, d,
                             FALSE, NULL, NULL, NULL,
                             piece->pipe->type);
    }

    if (d->post_auto_align != DT_TONEEQ_ALIGN_CUSTOM)
    {
      printf("toneeq_process GUI FULL: waiting for sync\n");
      // Wait for PREVIEW to calculate automatic post scale/shift
      if(!dt_dev_sync_pixelpipe_hash(self->dev, piece->pipe, self->iop_order, DT_DEV_TRANSFORM_DIR_BACK_INCL, &self->gui_lock, &g->sync_hash)) {
        dt_control_log(_("inconsistent output"));
        printf("toneeq_process GUI FULL: sync failed\n");
      }
      else
      {
        printf("toneeq_process GUI FULL: synced\n");
      }

      dt_iop_gui_enter_critical_section(self);
      post_scale = g->post_scale_value;
      post_shift = g->post_shift_value;
      dt_iop_gui_leave_critical_section(self);
    }
  } else {
    // no caching path : compute no matter what
    // TODO MF: Post scale/shift are calculated with local data.
    //          Not guraranteed to be identical to what PREVIEW saw.

    compute_luminance_mask(in, luminance, width, height, d,
                           FALSE, NULL, NULL, NULL,
                           piece->pipe->type);

    float histogram_first_decile, histogram_last_decile;
    compute_hires_histogram_and_stats(luminance, hires_histogram, num_elem,
                                      &histogram_first_decile, &histogram_last_decile,
                                      piece->pipe->type);

    compute_auto_post_scale_shift(&post_scale, &post_shift, d->post_auto_align,
                                  histogram_first_decile, histogram_last_decile, piece->pipe->type);
  }

  /**************************************************************************
   * Display output
   **************************************************************************/
  if(self->dev->gui_attached && (piece->pipe->type & DT_DEV_PIXELPIPE_FULL) && g->mask_display)
  {
    display_luminance_mask(in, luminance, out,
                            roi_in, roi_out,
                            post_scale, post_shift,
                            piece->pipe->type);
    piece->pipe->mask_display = DT_DEV_PIXELPIPE_DISPLAY_PASSTHRU;
  }
  else
  {
    compute_correction_lut(d->correction_lut, d->smoothing, d->factors,
                           post_scale, post_shift,
                           piece->pipe->type);
    apply_toneequalizer(in, luminance, out,
                        roi_in, roi_out,
                        d, piece->pipe->type);
  }

  /**************************************************************************
   * Cleanup
   **************************************************************************/
  if(local_luminance) {
    dt_free_align(luminance);
  }
  if(local_hires_hist) {
    dt_free_align(hires_histogram);
  }
}


void process(dt_iop_module_t *self,
             dt_dev_pixelpipe_iop_t *piece,
             const void *const restrict ivoid,
             void *const restrict ovoid,
             const dt_iop_roi_t *const roi_in,
             const dt_iop_roi_t *const roi_out)
{
  toneeq_process(self, piece, ivoid, ovoid, roi_in, roi_out);
}


/****************************************************************************
 *
 * Initialization and Cleanup
 *
 ****************************************************************************/

void init_global(dt_iop_module_so_t *self)
{
  printf("toneequalizer init_global\n");
  dt_iop_toneequalizer_global_data_t *gd = malloc(sizeof(dt_iop_toneequalizer_global_data_t));

  self->data = gd;
}


void cleanup_global(dt_iop_module_so_t *self)
{
  printf("toneequalizer cleanup_global\n");
  free(self->data);
  self->data = NULL;
}


void init_pipe(dt_iop_module_t *self,
               dt_dev_pixelpipe_t *pipe,
               dt_dev_pixelpipe_iop_t *piece)
{
  printf("toneequalizer init_pipe, pipe %d\n", pipe->type);
  piece->data = dt_calloc1_align_type(dt_iop_toneequalizer_data_t);
}


void cleanup_pipe(dt_iop_module_t *self,
                  dt_dev_pixelpipe_t *pipe,
                  dt_dev_pixelpipe_iop_t *piece)
{
  printf("toneequalizer cleanup_pipe, pipe %d\n", pipe->type);
  dt_free_align(piece->data);
  piece->data = NULL;
}


void modify_roi_in(dt_iop_module_t *self,
                   dt_dev_pixelpipe_iop_t *piece,
                   const dt_iop_roi_t *roi_out,
                   dt_iop_roi_t *roi_in)
{
  // Pad the zoomed-in view to avoid weird stuff with local averages
  // at the borders of the preview

  dt_iop_toneequalizer_data_t *const d = piece->data;

  // Get the scaled window radius for the box average
  const int max_size = (piece->iwidth > piece->iheight) ? piece->iwidth : piece->iheight;
  const float diameter = d->blending * max_size * roi_in->scale;
  const int radius = (int)((diameter - 1.0f) / ( 2.0f));
  d->radius = radius;
}


/***
 * Setters and Getters for parameters
 *
 * Remember the user params split the [-8; 0] EV range in 9 NUM_SLIDERS
 * and define a set of (x, y) coordinates, where x are the exposure
 * NUM_SLIDERS (evenly-spaced by 1 EV in [-8; 0] EV) and y are the
 * desired exposure compensation for each channel.
 *
 * This (x, y) set is interpolated by radial-basis function using a
 * series of 8 gaussians.  Losing 1 degree of freedom makes it an
 * approximation rather than an interpolation but helps reducing a bit
 * the oscillations and fills a full AVX vector.
 *
 * The coefficients/factors used in the interpolation/approximation
 * are linear, but keep in mind that users params are expressed as
 * log2 gains, so we always need to do the log2/exp2 flip/flop between
 * both.
 *
 * User params of exposure compensation are expected between [-2 ; +2]
 * EV for practical UI reasons and probably numerical stability
 * reasons, but there is no theoretical obstacle to enlarge this
 * range. The main reason for not allowing it is tone equalizer is
 * mostly intended to do local changes, and these don't look so well
 * if you are too harsh on the changes.  For heavier tonemapping, it
 * should be used in combination with a tone curve or filmic.
 *
 ***/
static void get_channels_gains(float factors[NUM_SLIDERS],
                               const dt_iop_toneequalizer_params_t *p)
{
  assert(NUM_SLIDERS == 9);

  // Get user-set NUM_SLIDERS gains in EV (log2)
  factors[0] = p->noise; // -8 EV
  factors[1] = p->ultra_deep_blacks; // -7 EV
  factors[2] = p->deep_blacks;       // -6 EV
  factors[3] = p->blacks;            // -5 EV
  factors[4] = p->shadows;           // -4 EV
  factors[5] = p->midtones;          // -3 EV
  factors[6] = p->highlights;        // -2 EV
  factors[7] = p->whites;            // -1 EV
  factors[8] = p->speculars;         // +0 EV
}


static void get_channels_factors(float factors[NUM_SLIDERS],
                                 const dt_iop_toneequalizer_params_t *p)
{
  assert(NUM_SLIDERS == 9);

  // Get user-set NUM_SLIDERS gains in EV (log2)
  get_channels_gains(factors, p);

  // Convert from EV offsets to linear factors
  DT_OMP_SIMD(aligned(factors:64))
  for(int c = 0; c < NUM_SLIDERS; ++c)
    factors[c] = exp2f(factors[c]);
}


__DT_CLONE_TARGETS__
static inline float pixel_correction(const float exposure,
                                     const float *const restrict factors,
                                     const float sigma)
{
  // build the correction for the current pixel
  // as the sum of the contribution of each luminance channel
  float result = 0.0f;
  const float gauss_denom = gaussian_denom(sigma);
  const float expo = fast_clamp(exposure, DT_TONEEQ_MIN_EV, DT_TONEEQ_MAX_EV);

  DT_OMP_SIMD(aligned(centers_ops, factors:64) safelen(NUM_OCTAVES) reduction(+:result))
  for(int i = 0; i < NUM_OCTAVES; ++i)
    result += gaussian_func(expo - centers_ops[i], gauss_denom) * factors[i];

  return fast_clamp(result, 0.25f, 4.0f);
}


__DT_CLONE_TARGETS__
static gboolean compute_channels_factors(const float factors[NUM_OCTAVES],
                                         float out[NUM_SLIDERS],
                                         const float sigma)
{
  // Input factors are the weights for the radial-basis curve
  // approximation of user params Output factors are the gains of the
  // user parameters NUM_SLIDERS aka the y coordinates of the
  // approximation for x = { NUM_SLIDERS }
  assert(NUM_OCTAVES == 8);

  DT_OMP_FOR_SIMD(aligned(factors, out, centers_params:64) firstprivate(centers_params))
  for(int i = 0; i < NUM_SLIDERS; ++i)
  {
    // Compute the new NUM_SLIDERS factors; pixel_correction clamps the factors, so we don't
    // need to check for validity here
    out[i] = pixel_correction(centers_params[i], factors, sigma);
  }
  return TRUE;
}


__DT_CLONE_TARGETS__
static void compute_channels_gains(const float in[NUM_SLIDERS],
                                  float out[NUM_SLIDERS])
{
  // Helper function to compute the new NUM_SLIDERS gains (log) from the factors (linear)
  assert(NUM_OCTAVES == 8);

  for(int i = 0; i < NUM_SLIDERS; ++i)
    out[i] = log2f(in[i]);
}


static void commit_channels_gains(const float factors[NUM_SLIDERS],
                                 dt_iop_toneequalizer_params_t *p)
{
  p->noise = factors[0];
  p->ultra_deep_blacks = factors[1];
  p->deep_blacks = factors[2];
  p->blacks = factors[3];
  p->shadows = factors[4];
  p->midtones = factors[5];
  p->highlights = factors[6];
  p->whites = factors[7];
  p->speculars = factors[8];
}


/****************************************************************************
 *
 * Cache invalidation and initialization
 *
 ****************************************************************************/
static void gui_cache_init(dt_iop_module_t *self)
{
  printf("gui_cache_init\n");
  dt_iop_toneequalizer_gui_data_t *g = self->gui_data;
  if(g == NULL) return;

  dt_iop_gui_enter_critical_section(self);
<<<<<<< HEAD
  g->ui_preview_hash = DT_INVALID_HASH;
  g->thumb_preview_hash = DT_INVALID_HASH;
=======
  g->full_upstream_hash = DT_INVALID_CACHEHASH;
  g->preview_upstream_hash = DT_INVALID_CACHEHASH;
>>>>>>> acabda0cbe (2025-04-06 preview version with post-shift, post_scale, auto-align and curve coloring.)
  g->max_histogram = 1;
  g->scale = 1.0f;
  g->sigma = sqrtf(2.0f);
  g->mask_display = FALSE;
  g->image_EV_per_UI_sample = 0.00001; // In case no value is calculated yet, use something small, but not 0

  g->interpolation_valid = FALSE;  // TRUE if the interpolation_matrix is ready
  g->luminance_valid = FALSE;      // TRUE if the luminance cache is ready
  g->gui_histogram_valid = FALSE;  // TRUE if the histogram cache and stats are ready
  g->gui_curve_valid = FALSE;      // TRUE if the gui_curve_lut is ready
  g->graph_valid = FALSE;          // TRUE if the UI graph view is ready
  g->user_param_valid = FALSE;     // TRUE if users params set in interactive view are in bounds
  g->factors_valid = TRUE;         // TRUE if radial-basis coeffs are ready

  g->valid_nodes_x = FALSE;        // TRUE if x coordinates of graph nodes have been inited
  g->valid_nodes_y = FALSE;        // TRUE if y coordinates of graph nodes have been inited
  g->area_cursor_valid = FALSE;    // TRUE if mouse cursor is over the graph area
  g->area_dragging = FALSE;        // TRUE if left-button has been pushed but not released and cursor motion is recorded
  g->cursor_valid = FALSE;         // TRUE if mouse cursor is over the preview image
  g->has_focus = FALSE;            // TRUE if module has focus from GTK

  g->preview_buf = NULL;
  g->preview_buf_width = 0;
  g->preview_buf_height = 0;

  g->full_buf = NULL;
  g->full_buf_width = 0;
  g->full_buf_height = 0;

  g->desc = NULL;
  g->layout = NULL;
  g->cr = NULL;
  g->cst = NULL;
  g->context = NULL;

  g->pipe_order = 0;
  dt_iop_gui_leave_critical_section(self);
}


static void invalidate_luminance_cache(dt_iop_module_t *const self)
{
  printf("invalidate_luminance_cache\n");
  // Invalidate the private luminance cache and histogram when
  // the luminance mask extraction parameters have changed
  dt_iop_toneequalizer_gui_data_t *const restrict g = self->gui_data;

  dt_iop_gui_enter_critical_section(self);
  g->luminance_valid = FALSE;
  g->preview_upstream_hash = DT_INVALID_CACHEHASH;
  g->full_upstream_hash = DT_INVALID_CACHEHASH;

  g->gui_histogram_valid = FALSE;
  g->max_histogram = 1;
  dt_iop_gui_leave_critical_section(self);
  dt_iop_refresh_all(self);
}


static void invalidate_lut_and_histogram(dt_iop_module_t *const self)
{
  printf("invalidate_lut_and_histogram\n");
  dt_iop_toneequalizer_gui_data_t *const restrict g = self->gui_data;

  dt_iop_gui_enter_critical_section(self);
  g->gui_curve_valid = FALSE;
  g->gui_histogram_valid = FALSE;
  g->max_histogram = 1;
  dt_iop_gui_leave_critical_section(self);
  dt_iop_refresh_all(self);
}


/****************************************************************************
 *
 * Curve Interpolation
 *
 ****************************************************************************/
static inline void build_interpolation_matrix(float A[NUM_SLIDERS * NUM_OCTAVES],
                                              const float sigma)
{
  // Build the symmetrical definite positive part of the augmented matrix
  // of the radial-basis interpolation weights

  const float gauss_denom = gaussian_denom(sigma);

  DT_OMP_SIMD(aligned(A, centers_ops, centers_params:64) collapse(2))
  for(int i = 0; i < NUM_SLIDERS; ++i)
    for(int j = 0; j < NUM_OCTAVES; ++j)
      A[i * NUM_OCTAVES + j] =
        gaussian_func(centers_params[i] - centers_ops[j], gauss_denom);
}


__DT_CLONE_TARGETS__
static inline void compute_gui_curve(dt_iop_toneequalizer_gui_data_t *g)
{
  // Compute the curve of the exposure corrections in EV,
  // offset and scale it for display in GUI widget graph

  if(g == NULL) return;

  float *const restrict curve = g->gui_curve;
  const float *const restrict factors = g->factors;
  const float sigma = g->sigma;

  DT_OMP_FOR_SIMD(aligned(curve, factors:64))
  for(int k = 0; k < UI_HISTO_SAMPLES; k++)
  {
    // build the inset graph curve LUT
    // the x range is [-14;+2] EV
    const float x = (8.0f * (((float)k) / ((float)(UI_HISTO_SAMPLES - 1)))) - 8.0f;
    // curve[k] = offset - log2f(pixel_correction(x, factors, sigma)) / scaling;
    curve[k] = log2f(pixel_correction(x, factors, sigma));
  }
}


static inline gboolean curve_interpolation(dt_iop_module_t *self)
{
  dt_iop_toneequalizer_params_t *p = self->params;
  dt_iop_toneequalizer_gui_data_t *g = self->gui_data;

  if(g == NULL) return FALSE;

  gboolean valid = TRUE;

  dt_iop_gui_enter_critical_section(self);

  if(!g->interpolation_valid)
  {
    build_interpolation_matrix(g->interpolation_matrix, g->sigma);
    g->interpolation_valid = TRUE;
    g->factors_valid = FALSE;
  }

  if(!g->user_param_valid)
  {
    float factors[NUM_SLIDERS] DT_ALIGNED_ARRAY;
    get_channels_factors(factors, p);
    dt_simd_memcpy(factors, g->temp_user_params, NUM_SLIDERS);
    g->user_param_valid = TRUE;
    g->factors_valid = FALSE;
  }

  if(!g->factors_valid && g->user_param_valid)
  {
    float factors[NUM_SLIDERS] DT_ALIGNED_ARRAY;
    dt_simd_memcpy(g->temp_user_params, factors, NUM_SLIDERS);
    valid = pseudo_solve(g->interpolation_matrix, factors, NUM_SLIDERS, NUM_OCTAVES, TRUE);
    if(valid) dt_simd_memcpy(factors, g->factors, NUM_OCTAVES);
    else dt_print(DT_DEBUG_PIPE, "tone equalizer pseudo solve problem");
    g->factors_valid = TRUE;
    g->gui_curve_valid = FALSE;
  }

  if(!g->gui_curve_valid && g->factors_valid)
  {
    compute_gui_curve(g);
    g->gui_curve_valid = TRUE;
  }

  dt_iop_gui_leave_critical_section(self);

  return valid;
}


/****************************************************************************
 *
 * Commit Params
 *
 ****************************************************************************/
void commit_params(dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{

  dt_iop_toneequalizer_params_t *p = (dt_iop_toneequalizer_params_t *)p1;
  dt_iop_toneequalizer_data_t *d = piece->data;
  dt_iop_toneequalizer_gui_data_t *g = self->gui_data;

  printf("commit_params pipe=%d align=%d p->post_scale=%f p->post_shift=%f d->post_scale=%f d->post_shift=%f\n",
         pipe->type, p->post_auto_align, p->post_scale, p->post_shift, d->post_scale, d->post_shift);

  // Trivial params passing
  d->lum_estimator = p->lum_estimator;
  d->filter = p->filter;
  d->iterations = p->iterations;
  d->smoothing = p->smoothing;
  d->quantization = p->quantization;

  // UI blending param is set in % of the largest image dimension
  d->blending = p->blending / 100.0f;

  // UI guided filter feathering param increases the edges taping
  // but the actual regularization params applied in guided filter behaves the other way
  d->feathering = 1.f / (p->feathering);

  // UI params are in log2 offsets (EV) : convert to linear factors
  d->contrast_boost = exp2f(p->contrast_boost);
  d->exposure_boost = exp2f(p->exposure_boost);

  // printf("commit_params: post_auto_align %d\n", p->post_auto_align);
  d->post_auto_align = p->post_auto_align;
  d->post_scale = p->post_scale;
  d->post_shift = p->post_shift;

  /*
   * Perform a radial-based interpolation using a series gaussian functions
   */
  if(self->dev->gui_attached && g)
  {
    dt_iop_gui_enter_critical_section(self);

    if(g->sigma != p->smoothing) g->interpolation_valid = FALSE;
    g->sigma = p->smoothing;
    g->user_param_valid = FALSE; // force updating NUM_SLIDERS factors
    dt_iop_gui_leave_critical_section(self);

    curve_interpolation(self);

    dt_iop_gui_enter_critical_section(self);
    dt_simd_memcpy(g->factors, d->factors, NUM_OCTAVES);
    dt_iop_gui_leave_critical_section(self);
  }
  else
  {
    // No cache : Build / Solve interpolation matrix
    float factors[NUM_SLIDERS] DT_ALIGNED_ARRAY;
    get_channels_factors(factors, p);

    float A[NUM_SLIDERS * NUM_OCTAVES] DT_ALIGNED_ARRAY;
    build_interpolation_matrix(A, p->smoothing);
    pseudo_solve(A, factors, NUM_SLIDERS, NUM_OCTAVES, FALSE);

    dt_simd_memcpy(factors, d->factors, NUM_OCTAVES);
  }
}


/****************************************************************************
 *
 * GUI Helpers
 *
 ****************************************************************************/
static inline void compute_gui_histogram(int hires_histogram[HIRES_HISTO_SAMPLES],
  int histogram[UI_HISTO_SAMPLES],
  float histogram_scale,
  float histogram_shift,
  int *max_histogram)
{
  printf("compute_gui_histogram\n");
  // (Re)init the histogram
  memset(histogram, 0, sizeof(int) * UI_HISTO_SAMPLES);

  const float temp_ev_range = HIRES_HISTO_MAX_EV - HIRES_HISTO_MIN_EV;

  // remap the extended histogram into the gui histogram
  // bins between [-8; 0] EV remapped between [0 ; UI_HISTO_SAMPLES]
  for(size_t k = 0; k < HIRES_HISTO_SAMPLES; ++k)
  {
    // from [0...HIRES_HISTO_SAMPLES] to [-16...8EV]
    float EV = temp_ev_range * (float)k / (float)(HIRES_HISTO_SAMPLES - 1) + HIRES_HISTO_MIN_EV;

    // apply shift & scale to the EV value
    const float shift_scaled_EV = post_scale_shift(EV, histogram_scale, histogram_shift);

    // from [-8...0] EV to [0...UI_HISTO_SAMPLES]
    const int i = CLAMP((int)(((shift_scaled_EV + 8.0f) / 8.0f) * (float)UI_HISTO_SAMPLES), 0, UI_HISTO_SAMPLES - 1);

    histogram[i] += hires_histogram[k];
  }

  // store the max numbers of elements in bins for later normalization
  // ignore the first and last value to keep the histogram readable
  *max_histogram = 1; // don't divide by 0 if there are no values at all
  for (int i = 1; i < UI_HISTO_SAMPLES - 1; i++)
  {
    *max_histogram = histogram[i] > *max_histogram ? histogram[i] : *max_histogram;
  }
}


static inline void update_gui_histogram(dt_iop_module_t *const self)
{
  dt_iop_toneequalizer_gui_data_t *const g = self->gui_data;
  dt_iop_toneequalizer_params_t *const p = self->params;
  if(g == NULL) return;

  dt_iop_gui_enter_critical_section(self);
  if(!g->gui_histogram_valid && g->luminance_valid)
  {
    compute_auto_post_scale_shift(&p->post_scale, &p->post_shift, p->post_auto_align, g->histogram_first_decile, g->histogram_last_decile, 999);
    compute_gui_histogram(g->hires_histogram, g->histogram, p->post_scale, p->post_shift, &g->max_histogram);

    // Computation of "image_EV_per_UI_sample"
    // The graph shows 8EV, but when we align the histogram, we consider 6EV [-7; -1] ("target")
    const float target_EV_range = 6.0f;
    const float full_EV_range = 8.0f;
    const float target_to_full = full_EV_range / target_EV_range;

    // What is the real dynamic range of the histogram-part [-7; -1])? We unscale.
    const float mask_EV_of_target = target_EV_range / exp2f(p->post_scale);

    // The histogram shows mask EV, but for evaluating curve steepness, we need image EVs
    const float mask_to_image = (g->image_histogram_last_decile - g->image_histogram_first_decile)
                                / (g->histogram_last_decile - g->histogram_first_decile);

    g->image_EV_per_UI_sample = (mask_EV_of_target * mask_to_image * target_to_full) / (float)UI_HISTO_SAMPLES;

    if (g->show_two_histograms)
      compute_gui_histogram(g->image_hires_histogram, g->image_histogram, p->post_scale, p->post_shift, &g->max_image_histogram);

    g->gui_histogram_valid = TRUE;
  }
  dt_iop_gui_leave_critical_section(self);
}


static void show_guiding_controls(dt_iop_module_t *self)
{
  dt_iop_toneequalizer_gui_data_t *g = self->gui_data;
  const dt_iop_toneequalizer_params_t *p = self->params;

  switch(p->filter)
  {
    case(DT_TONEEQ_NONE):
    {
      gtk_widget_set_visible(g->blending, FALSE);
      gtk_widget_set_visible(g->feathering, FALSE);
      gtk_widget_set_visible(g->iterations, FALSE);
      gtk_widget_set_visible(g->contrast_boost, FALSE);
      gtk_widget_set_visible(g->quantization, FALSE);
      break;
    }

    case(DT_TONEEQ_AVG_GUIDED):
    case(DT_TONEEQ_AVG_EIGF):
    {
      gtk_widget_set_visible(g->blending, TRUE);
      gtk_widget_set_visible(g->feathering, TRUE);
      gtk_widget_set_visible(g->iterations, TRUE);
      gtk_widget_set_visible(g->contrast_boost, FALSE);
      gtk_widget_set_visible(g->quantization, TRUE);
      break;
    }

    case(DT_TONEEQ_GUIDED):
    case(DT_TONEEQ_EIGF):
    {
      gtk_widget_set_visible(g->blending, TRUE);
      gtk_widget_set_visible(g->feathering, TRUE);
      gtk_widget_set_visible(g->iterations, TRUE);
      gtk_widget_set_visible(g->contrast_boost, TRUE);
      gtk_widget_set_visible(g->quantization, TRUE);
      break;
    }
  }

  switch(p->post_auto_align) {
    case(DT_TONEEQ_ALIGN_LEFT):
    case(DT_TONEEQ_ALIGN_CENTER):
    case(DT_TONEEQ_ALIGN_RIGHT):
    {
      gtk_widget_set_visible(g->post_scale, TRUE);
      gtk_widget_set_visible(g->post_shift, FALSE);
      break;
    }
    case(DT_TONEEQ_ALIGN_FIT):
    {
      gtk_widget_set_visible(g->post_scale, FALSE);
      gtk_widget_set_visible(g->post_shift, FALSE);
      break;
    }
    case(DT_TONEEQ_ALIGN_CUSTOM):
    {
      gtk_widget_set_visible(g->post_scale, TRUE);
      gtk_widget_set_visible(g->post_shift, TRUE);
      break;
    }
  }
}


/****************************************************************************
 *
 * GUI Callbacks
 *
 ****************************************************************************/
void gui_update(dt_iop_module_t *self)
{
  printf("gui_update\n");
  dt_iop_toneequalizer_gui_data_t *g = self->gui_data;
  dt_iop_toneequalizer_params_t *p = self->params;

  dt_bauhaus_slider_set(g->smoothing, logf(p->smoothing) / logf(sqrtf(2.0f)) - 1.0f);

  show_guiding_controls(self);

  invalidate_luminance_cache(self);

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->show_luminance_mask), g->mask_display);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->show_two_histograms), g->two_histograms_display);

}


void gui_changed(dt_iop_module_t *self,
                 GtkWidget *w,
                 void *previous)
{
  printf("gui_changed\n");
  dt_iop_toneequalizer_gui_data_t *g = self->gui_data;
  dt_iop_toneequalizer_params_t *p = self->params;

  if(w == g->lum_estimator
     || w == g->blending
     || w == g->feathering
     || w == g->iterations
     || w == g->quantization)
  {
    invalidate_luminance_cache(self);
  }
  else if(w == g->filter)
  {
    invalidate_luminance_cache(self);
    show_guiding_controls(self);
  }
  else if(w == g->contrast_boost
          || w == g->exposure_boost)
  {
    invalidate_luminance_cache(self);
    dt_bauhaus_widget_set_quad_active(w, FALSE);
  }
  else if (w == g->post_scale
           || w == g->post_shift)
  {
    invalidate_lut_and_histogram(self);
  }
  else if (w == g->post_auto_align)
  {
    // We may have switched from a more automatic to a less automatic mode.
    // Copy the automatically determined parameters to the GUI sliders.
    ++darktable.gui->reset;
    dt_bauhaus_slider_set(g->post_scale, p->post_scale);
    dt_bauhaus_slider_set(g->post_shift, p->post_shift);
    --darktable.gui->reset;

    invalidate_lut_and_histogram(self);
    show_guiding_controls(self);
  }
}


static void smoothing_callback(GtkWidget *slider, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_toneequalizer_params_t *p = self->params;
  dt_iop_toneequalizer_gui_data_t *g = self->gui_data;

  p->smoothing= powf(sqrtf(2.0f), 1.0f +  dt_bauhaus_slider_get(slider));

  float factors[NUM_SLIDERS] DT_ALIGNED_ARRAY;
  get_channels_factors(factors, p);

  // Solve the interpolation by least-squares to check the validity of the smoothing param
  if(!curve_interpolation(self))
    dt_control_log
      (_("the interpolation is unstable, decrease the curve smoothing"));

  // Redraw graph before launching computation
  // Don't do this again: update_curve_lut(self);
  gtk_widget_queue_draw(GTK_WIDGET(g->area));
  dt_dev_add_history_item(darktable.develop, self, TRUE);

  // Unlock the colour picker so we can display our own custom cursor
  dt_iop_color_picker_reset(self, TRUE);
}


static void auto_adjust_exposure_boost(GtkWidget *quad, dt_iop_module_t *self)
{
  dt_iop_toneequalizer_params_t *p = self->params;
  dt_iop_toneequalizer_gui_data_t *g = self->gui_data;

  if(darktable.gui->reset) return;

  dt_iop_request_focus(self);

  if(!self->enabled)
  {
    // activate module and do nothing
    ++darktable.gui->reset;
    dt_bauhaus_slider_set(g->exposure_boost, p->exposure_boost);
    --darktable.gui->reset;

    invalidate_luminance_cache(self);
    dt_dev_add_history_item(darktable.develop, self, TRUE);
    return;
  }

  if(!g->luminance_valid || self->dev->full.pipe->processing || !g->gui_histogram_valid)
  {
    dt_control_log(_("wait for the preview to finish recomputing"));
    return;
  }

  // The goal is to get the exposure distribution centered on the equalizer view
  // to spread it over as many nodes as possible for better exposure control.
  // Controls nodes are between -8 and 0 EV,
  // so we aim at centering the exposure distribution on -4 EV
  dt_iop_gui_enter_critical_section(self);
  g->gui_histogram_valid = FALSE;
  dt_iop_gui_leave_critical_section(self);

  update_gui_histogram(self);

  // calculate exposure correction
  const float fd_new = exp2f(g->histogram_first_decile);
  const float ld_new = exp2f(g->histogram_last_decile);
  const float e = exp2f(p->exposure_boost);
  const float c = exp2f(p->contrast_boost);
  // revert current transformation
  const float fd_old = ((fd_new - CONTRAST_FULCRUM) / c + CONTRAST_FULCRUM) / e;
  const float ld_old = ((ld_new - CONTRAST_FULCRUM) / c + CONTRAST_FULCRUM) / e;

  // calculate correction
  const float s1 = CONTRAST_FULCRUM - exp2f(-7.0);
  const float s2 = exp2f(-1.0) - CONTRAST_FULCRUM;
  const float mix = fd_old * s2 +  ld_old * s1;

  p->exposure_boost = log2f(CONTRAST_FULCRUM * (s1 + s2) / mix);

  // Update the GUI stuff
  ++darktable.gui->reset;
  dt_bauhaus_slider_set(g->exposure_boost, p->exposure_boost);
  --darktable.gui->reset;
  invalidate_luminance_cache(self);
  dt_dev_add_history_item(darktable.develop, self, TRUE);

  // Unlock the colour picker so we can display our own custom cursor
  dt_iop_color_picker_reset(self, TRUE);
}


static void auto_adjust_contrast_boost(GtkWidget *quad, dt_iop_module_t *self)
{
  dt_iop_toneequalizer_params_t *p = self->params;
  dt_iop_toneequalizer_gui_data_t *g = self->gui_data;

  if(darktable.gui->reset) return;

  dt_iop_request_focus(self);

  if(!self->enabled)
  {
    // activate module and do nothing
    ++darktable.gui->reset;
    dt_bauhaus_slider_set(g->contrast_boost, p->contrast_boost);
    --darktable.gui->reset;

    invalidate_luminance_cache(self);
    dt_dev_add_history_item(darktable.develop, self, TRUE);
    return;
  }

  if(!g->luminance_valid || self->dev->full.pipe->processing || !g->gui_histogram_valid)
  {
    dt_control_log(_("wait for the preview to finish recomputing"));
    return;
  }

  // The goal is to spread 90 % of the exposure histogram in the [-7, -1] EV
  dt_iop_gui_enter_critical_section(self);
  g->gui_histogram_valid = FALSE;
  dt_iop_gui_leave_critical_section(self);

  update_gui_histogram(self);

  // calculate contrast correction
  const float fd_new = exp2f(g->histogram_first_decile);
  const float ld_new = exp2f(g->histogram_last_decile);
  const float e = exp2f(p->exposure_boost);
  float c = exp2f(p->contrast_boost);
  // revert current transformation
  const float fd_old = ((fd_new - CONTRAST_FULCRUM) / c + CONTRAST_FULCRUM) / e;
  const float ld_old = ((ld_new - CONTRAST_FULCRUM) / c + CONTRAST_FULCRUM) / e;

  // calculate correction
  const float s1 = CONTRAST_FULCRUM - exp2f(-7.0);
  const float s2 = exp2f(-1.0) - CONTRAST_FULCRUM;
  const float mix = fd_old * s2 +  ld_old * s1;

  c = log2f(mix / (CONTRAST_FULCRUM * (ld_old - fd_old)) / c);

  // when adding contrast, blur filters modify the histogram in a way
  // difficult to predict here we implement a heuristic correction
  // based on a set of images and regression analysis
  if(p->filter == DT_TONEEQ_EIGF && c > 0.0f)
  {
    const float correction = -0.0276f + 0.01823 * p->feathering + (0.7566f - 1.0f) * c;
    if(p->feathering < 5.0f)
      c += correction;
    else if(p->feathering < 10.0f)
      c += correction * (2.0f - p->feathering / 5.0f);
  }
  else if(p->filter == DT_TONEEQ_GUIDED && c > 0.0f)
      c = 0.0235f + 1.1225f * c;

  p->contrast_boost += c;

  // Update the GUI stuff
  ++darktable.gui->reset;
  dt_bauhaus_slider_set(g->contrast_boost, p->contrast_boost);
  --darktable.gui->reset;
  invalidate_luminance_cache(self);
  dt_dev_add_history_item(darktable.develop, self, TRUE);

  // Unlock the colour picker so we can display our own custom cursor
  dt_iop_color_picker_reset(self, TRUE);
}


static void show_luminance_mask_callback(GtkWidget *togglebutton,
                                         GdkEventButton *event,
                                         dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_request_focus(self);

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->off), TRUE);

  dt_iop_toneequalizer_gui_data_t *g = self->gui_data;

  // if blend module is displaying mask do not display it here
  if(self->request_mask_display)
  {
    dt_control_log(_("cannot display masks when the blending mask is displayed"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->show_luminance_mask), FALSE);
    g->mask_display = FALSE;
    return;
  }
  else
    g->mask_display = !g->mask_display;

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->show_luminance_mask), g->mask_display);
//  dt_dev_reprocess_center(self->dev);
  dt_iop_refresh_center(self);

  // Unlock the colour picker so we can display our own custom cursor
  dt_iop_color_picker_reset(self, TRUE);
}


// TODO MF: Remove this again? Two histograms are only useful for debugging.
static void show_two_histograms_callback(GtkWidget *togglebutton,
  GdkEventButton *event,
  dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_request_focus(self);

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->off), TRUE);

  dt_iop_toneequalizer_gui_data_t *g = self->gui_data;
  g->two_histograms_display = !g->two_histograms_display;
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->show_two_histograms), g->two_histograms_display);

  //  dt_dev_reprocess_center(self->dev);
  dt_iop_refresh_center(self); // TODO: is this needed?
  // Unlock the colour picker so we can display our own custom cursor
  // dt_iop_color_picker_reset(self, TRUE);
}


/****************************************************************************
 *
 * GUI Interactivity
 *
 ****************************************************************************/
static void _get_point(dt_iop_module_t *self, const int c_x, const int c_y, int *x, int *y)
{
  // TODO: For this to fully work non depending on the place of the module
  //       in the pipe we need a dt_dev_distort_backtransform_plus that
  //       can skip crop only. With the current version if toneequalizer
  //       is moved below rotation & perspective it will fail as we are
  //       then missing all the transform after tone-eq.
  const double crop_order = dt_ioppr_get_iop_order(self->dev->iop_order_list, "crop", 0);

  float pts[2] = { c_x, c_y };

  // only a forward backtransform as the buffer already contains all the transforms
  // done before toneequal and we are speaking of on-screen cursor coordinates.
  // also we do transform only after crop as crop does change roi for the whole pipe
  // and so it is already part of the preview buffer cached in this implementation.
  dt_dev_distort_backtransform_plus(darktable.develop, darktable.develop->preview_pipe, crop_order,
                                    DT_DEV_TRANSFORM_DIR_FORW_EXCL, pts, 1);
  *x = pts[0];
  *y = pts[1];
}


__DT_CLONE_TARGETS__
static float get_luminance_from_buffer(const float *const buffer,
                                       const size_t width,
                                       const size_t height,
                                       const size_t x,
                                       const size_t y)
{
  // Get the weighted average luminance of the 3×3 pixels region centered in (x, y)
  // x and y are ratios in [0, 1] of the width and height

  if(y >= height || x >= width) return NAN;

  const size_t y_abs[4] DT_ALIGNED_PIXEL =
                          { MAX(y, 1) - 1,              // previous line
                            y,                          // center line
                            MIN(y + 1, height - 1),     // next line
                            y };		        // padding for vectorization

  float luminance = 0.0f;
  if(x > 1 && x < width - 2)
  {
    // no clamping needed on x, which allows us to vectorize
    // apply the convolution
    for(int i = 0; i < 3; ++i)
    {
      const size_t y_i = y_abs[i];
      for_each_channel(j)
        luminance += buffer[width * y_i + x-1 + j] * gauss_kernel[i][j];
    }
    return luminance;
  }

  const size_t x_abs[4] DT_ALIGNED_PIXEL =
                          { MAX(x, 1) - 1,              // previous column
                            x,                          // center column
                            MIN(x + 1, width - 1),      // next column
                            x };                        // padding for vectorization

  // convolution
  for(int i = 0; i < 3; ++i)
  {
    const size_t y_i = y_abs[i];
    for_each_channel(j)
      luminance += buffer[width * y_i + x_abs[j]] * gauss_kernel[i][j];
  }
  return luminance;
}


// unify with get_luminance_from_buffer
static float _luminance_from_thumb_preview_buf(dt_iop_module_t *self)
{
  dt_iop_toneequalizer_gui_data_t *g = self->gui_data;

  const size_t c_x = g->cursor_pos_x;
  const size_t c_y = g->cursor_pos_y;

  // get buffer x,y given the cursor position
  int b_x = 0;
  int b_y = 0;

  _get_point(self, c_x, c_y, &b_x, &b_y);

  return get_luminance_from_buffer(g->preview_buf,
    g->preview_buf_width,
    g->preview_buf_height,
    b_x,
    b_y);
}


void update_exposure_sliders(dt_iop_toneequalizer_gui_data_t *g, dt_iop_toneequalizer_params_t *p)
{
  // Params to GUI
  ++darktable.gui->reset;
  dt_bauhaus_slider_set(g->noise, p->noise);
  dt_bauhaus_slider_set(g->ultra_deep_blacks, p->ultra_deep_blacks);
  dt_bauhaus_slider_set(g->deep_blacks, p->deep_blacks);
  dt_bauhaus_slider_set(g->blacks, p->blacks);
  dt_bauhaus_slider_set(g->shadows, p->shadows);
  dt_bauhaus_slider_set(g->midtones, p->midtones);
  dt_bauhaus_slider_set(g->highlights, p->highlights);
  dt_bauhaus_slider_set(g->whites, p->whites);
  dt_bauhaus_slider_set(g->speculars, p->speculars);
  --darktable.gui->reset;
}


static gboolean in_mask_editing(dt_iop_module_t *self)
{
  const dt_develop_t *dev = self->dev;
  return dev->form_gui && dev->form_visible;
}


static void switch_cursors(dt_iop_module_t *self)
{
  dt_iop_toneequalizer_gui_data_t *g = self->gui_data;

  if(!g || !self->dev->gui_attached)
    return;

  GtkWidget *widget = dt_ui_main_window(darktable.gui->ui);

  // if we are editing masks or using colour-pickers, do not display controls
  if(in_mask_editing(self)
     || dt_iop_canvas_not_sensitive(self->dev))
  {
    // display default cursor
    GdkCursor *const cursor =
      gdk_cursor_new_from_name(gdk_display_get_default(), "default");
    gdk_window_set_cursor(gtk_widget_get_window(widget), cursor);
    g_object_unref(cursor);

    return;
  }

  // check if module is expanded
  dt_iop_gui_enter_critical_section(self);
  g->has_focus = self->expanded;
  dt_iop_gui_leave_critical_section(self);

  if(!g->has_focus)
  {
    // if module lost focus or is disabled
    // do nothing and let the app decide
    return;
  }
  else if((self->dev->full.pipe->processing
           || self->dev->full.pipe->status == DT_DEV_PIXELPIPE_DIRTY
           || self->dev->preview_pipe->status == DT_DEV_PIXELPIPE_DIRTY)
          && g->cursor_valid)
  {
    // if pipe is busy or dirty but cursor is on preview,
    // display waiting cursor while pipe reprocesses
    GdkCursor *const cursor = gdk_cursor_new_from_name(gdk_display_get_default(), "wait");
    gdk_window_set_cursor(gtk_widget_get_window(widget), cursor);
    g_object_unref(cursor);

    dt_control_queue_redraw_center();
  }
  else if(g->cursor_valid && !self->dev->full.pipe->processing)
  {
    // if pipe is clean and idle and cursor is on preview,
    // hide GTK cursor because we display our custom one
    dt_control_change_cursor(GDK_BLANK_CURSOR);
    dt_control_hinter_message(_("scroll over image to change tone exposure\n"
                                "shift+scroll for large steps; "
                                "ctrl+scroll for small steps"));

    dt_control_queue_redraw_center();
  }
  else if(!g->cursor_valid)
  {
    // if module is active and opened but cursor is out of the preview,
    // display default cursor
    GdkCursor *const cursor =
      gdk_cursor_new_from_name(gdk_display_get_default(), "default");
    gdk_window_set_cursor(gtk_widget_get_window(widget), cursor);
    g_object_unref(cursor);

    dt_control_queue_redraw_center();
  }
  else
  {
    // in any other situation where module has focus,
    // reset the cursor but don't launch a redraw
    GdkCursor *const cursor =
      gdk_cursor_new_from_name(gdk_display_get_default(), "default");
    gdk_window_set_cursor(gtk_widget_get_window(widget), cursor);
    g_object_unref(cursor);
  }
}


int mouse_moved(dt_iop_module_t *self,
                const float pzx,
                const float pzy,
                const double pressure,
                const int which,
                const float zoom_scale)
{
  // Whenever the mouse moves over the picture preview, store its
  // coordinates in the GUI struct for later use.

  const dt_develop_t *dev = self->dev;
  dt_iop_toneequalizer_gui_data_t *g = self->gui_data;
  dt_iop_toneequalizer_params_t *const p = self->params;

  if(g == NULL) return 0;

  // compute the on-screen point where the mouse cursor is
  float wd, ht;
  if(!dt_dev_get_preview_size(dev, &wd, &ht)) return 0;

  const int x_pointer = pzx * wd;
  const int y_pointer = pzy * ht;

  dt_iop_gui_enter_critical_section(self);
  // Cursor is valid if it's inside the picture frame
  if(x_pointer >= 0 && x_pointer < wd && y_pointer >= 0 && y_pointer < ht)
  {
    g->cursor_valid = TRUE;
    g->cursor_pos_x = x_pointer;
    g->cursor_pos_y = y_pointer;
  }
  else
  {
    g->cursor_valid = FALSE;
    g->cursor_pos_x = 0;
    g->cursor_pos_y = 0;
  }
  dt_iop_gui_leave_critical_section(self);

  // store the actual exposure too, to spare I/O op
  if(g->cursor_valid && !dev->full.pipe->processing && g->luminance_valid) {
    const float lum = log2f(_luminance_from_thumb_preview_buf(self));
    g->cursor_exposure = fast_clamp(post_scale_shift(lum, p->post_scale, p->post_shift), DT_TONEEQ_MIN_EV, DT_TONEEQ_MAX_EV);
  }

  switch_cursors(self);

  return 1;
}


int mouse_leave(dt_iop_module_t *self)
{
  dt_iop_toneequalizer_gui_data_t *g = self->gui_data;

  if(g == NULL) return 0;

  dt_iop_gui_enter_critical_section(self);
  g->cursor_valid = FALSE;
  g->area_active_node = -1;
  dt_iop_gui_leave_critical_section(self);

  // display default cursor
  GtkWidget *widget = dt_ui_main_window(darktable.gui->ui);
  GdkCursor *cursor = gdk_cursor_new_from_name(gdk_display_get_default(), "default");
  gdk_window_set_cursor(gtk_widget_get_window(widget), cursor);
  g_object_unref(cursor);
  dt_control_queue_redraw_center();
  gtk_widget_queue_draw(GTK_WIDGET(g->area));

  return 1;
}


static inline gboolean set_new_params_interactive(const float control_exposure,
                                                  const float exposure_offset,
                                                  const float blending_sigma,
                                                  dt_iop_toneequalizer_gui_data_t *g,
                                                  dt_iop_toneequalizer_params_t *p)
{
  // Apply an exposure offset optimized smoothly over all the exposure NUM_SLIDERS,
  // taking user instruction to apply exposure_offset EV at control_exposure EV,
  // and commit the new params is the solution is valid.

  // Raise the user params accordingly to control correction and
  // distance from cursor exposure to blend smoothly the desired
  // correction
  const float std = gaussian_denom(blending_sigma);
  if(g->user_param_valid)
  {
    for(int i = 0; i < NUM_SLIDERS; ++i)
      g->temp_user_params[i] *=
        exp2f(gaussian_func(centers_params[i] - control_exposure, std) * exposure_offset);
  }

  // Get the new weights for the radial-basis approximation
  float factors[NUM_SLIDERS] DT_ALIGNED_ARRAY;
  dt_simd_memcpy(g->temp_user_params, factors, NUM_SLIDERS);
  if(g->user_param_valid)
    g->user_param_valid = pseudo_solve(g->interpolation_matrix, factors, NUM_SLIDERS, NUM_OCTAVES, TRUE);
  if(!g->user_param_valid)
    dt_control_log(_("the interpolation is unstable, decrease the curve smoothing"));

  // Compute new user params for NUM_SLIDERS and store them locally
  if(g->user_param_valid)
    g->user_param_valid = compute_channels_factors(factors, g->temp_user_params, g->sigma);
  if(!g->user_param_valid) dt_control_log(_("some parameters are out-of-bounds"));

  const gboolean commit = g->user_param_valid;

  if(commit)
  {
    // Accept the solution
    dt_simd_memcpy(factors, g->factors, NUM_OCTAVES);
    g->gui_curve_valid = FALSE;

    // Convert the linear temp parameters to log gains and commit
    float gains[NUM_SLIDERS] DT_ALIGNED_ARRAY;
    compute_channels_gains(g->temp_user_params, gains);
    commit_channels_gains(gains, p);
  }
  else
  {
    // Reset the GUI copy of user params
    get_channels_factors(factors, p);
    dt_simd_memcpy(factors, g->temp_user_params, NUM_SLIDERS);
    g->user_param_valid = TRUE;
  }

  return commit;
}


int scrolled(dt_iop_module_t *self,
             const float x,
             const float y,
             const int up,
             const uint32_t state)
{
  dt_develop_t *dev = self->dev;
  dt_iop_toneequalizer_gui_data_t *g = self->gui_data;
  dt_iop_toneequalizer_params_t *p = self->params;

  if(darktable.gui->reset) return 1;
  if(g == NULL) return 0;
  if(!g->has_focus) return 0;

  // turn-on the module if off
  if(!self->enabled)
    if(self->off) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->off), 1);

  if(in_mask_editing(self)) return 0;

  // ALT/Option should work for zooming
  if (dt_modifier_is(state, GDK_MOD1_MASK)) return 0;

  // if GUI buffers not ready, exit but still handle the cursor
  dt_iop_gui_enter_critical_section(self);

  const gboolean fail = !g->cursor_valid
                     || !g->luminance_valid
                     || !g->interpolation_valid
                     || !g->user_param_valid
                     || dev->full.pipe->processing
                     || !g->has_focus;

  dt_iop_gui_leave_critical_section(self);
  if(fail) return 1;

  // re-read the exposure in case it has changed
  dt_iop_gui_enter_critical_section(self);

  const float lum = log2f(_luminance_from_thumb_preview_buf(self));
  g->cursor_exposure = fast_clamp(post_scale_shift(lum, p->post_scale, p->post_shift), DT_TONEEQ_MIN_EV, DT_TONEEQ_MAX_EV);

  dt_iop_gui_leave_critical_section(self);

  // Set the correction from mouse scroll input
  const float increment = (up) ? +1.0f : -1.0f;

  float step;
  if(dt_modifier_is(state, GDK_SHIFT_MASK))
    step = 1.0f;  // coarse
  else if(dt_modifier_is(state, GDK_CONTROL_MASK))
    step = 0.1f;  // fine
  else
    step = 0.25f; // standard

  const float offset = step * ((float)increment);

  // Get the desired correction on exposure NUM_SLIDERS
  dt_iop_gui_enter_critical_section(self);
  const gboolean commit = set_new_params_interactive(g->cursor_exposure, offset,
                                                g->sigma * g->sigma / 2.0f, g, p);
  dt_iop_gui_leave_critical_section(self);

  gtk_widget_queue_draw(GTK_WIDGET(g->area));

  if(commit)
  {
    // Update GUI with new params
    update_exposure_sliders(g, p);

    dt_dev_add_history_item(darktable.develop, self, FALSE);
  }

  return 1;
}


 /****************************************************************************
 *
 * GTK/Cairo drawings and custom widgets
 *
 ****************************************************************************/
static inline gboolean _init_drawing(dt_iop_module_t *const restrict self,
                                     GtkWidget *widget,
                                     dt_iop_toneequalizer_gui_data_t *const restrict g);


void cairo_draw_hatches(cairo_t *cr,
                        double center[2],
                        double span[2],
                        const int instances,
                        const double line_width,
                        const double shade)
{
  // center is the (x, y) coordinates of the region to draw
  // span is the distance of the region's bounds to the center, over (x, y) axes

  // Get the coordinates of the corners of the bounding box of the region
  double C0[2] = { center[0] - span[0], center[1] - span[1] };
  double C2[2] = { center[0] + span[0], center[1] + span[1] };

  double delta[2] = { 2.0 * span[0] / (double)instances,
                      2.0 * span[1] / (double)instances };

  cairo_set_line_width(cr, line_width);
  cairo_set_source_rgb(cr, shade, shade, shade);

  for(int i = -instances / 2 - 1; i <= instances / 2 + 1; i++)
  {
    cairo_move_to(cr, C0[0] + (double)i * delta[0], C0[1]);
    cairo_line_to(cr, C2[0] + (double)i * delta[0], C2[1]);
    cairo_stroke(cr);
  }
}


static void get_shade_from_luminance(cairo_t *cr,
                                     const float luminance,
                                     const float alpha)
{
  // TODO: fetch screen gamma from ICC display profile
  const float gamma = 1.0f / 2.2f;
  const float shade = powf(luminance, gamma);
  cairo_set_source_rgba(cr, shade, shade, shade, alpha);
}


static void draw_exposure_cursor(cairo_t *cr,
                                 const double pointerx,
                                 const double pointery,
                                 const double radius,
                                 const float luminance,
                                 const float zoom_scale,
                                 const int instances,
                                 const float alpha)
{
  // Draw a circle cursor filled with a grey shade corresponding to a luminance value
  // or hatches if the value is above the overexposed threshold

  const double radius_z = radius / zoom_scale;

  get_shade_from_luminance(cr, luminance, alpha);
  cairo_arc(cr, pointerx, pointery, radius_z, 0, 2 * M_PI);
  cairo_fill_preserve(cr);
  cairo_save(cr);
  cairo_clip(cr);

  if(log2f(luminance) > 0.0f)
  {
    // if overexposed, draw hatches
    double pointer_coord[2] = { pointerx, pointery };
    double span[2] = { radius_z, radius_z };
    cairo_draw_hatches(cr, pointer_coord, span, instances,
                       DT_PIXEL_APPLY_DPI(1. / zoom_scale), 0.3);
  }
  cairo_restore(cr);
}


static void match_color_to_background(cairo_t *cr,
                                      const float exposure,
                                      const float alpha)
{
  float shade = 0.0f;
  // TODO: put that as a preference in darktablerc
  const float contrast = 1.0f;

  if(exposure > -2.5f)
    shade = (fminf(exposure * contrast, 0.0f) - 2.5f);
  else
    shade = (fmaxf(exposure / contrast, -5.0f) + 2.5f);

  get_shade_from_luminance(cr, exp2f(shade), alpha);
}


void gui_post_expose(dt_iop_module_t *self,
                     cairo_t *cr,
                     const float width,
                     const float height,
                     const float pointerx,
                     const float pointery,
                     const float zoom_scale)
{
  // Draw the custom exposure cursor over the image preview

  dt_develop_t *dev = self->dev;
  dt_iop_toneequalizer_gui_data_t *g = self->gui_data;
  dt_iop_toneequalizer_params_t *p = self->params;

  // if we are editing masks, do not display controls
  if(in_mask_editing(self)) return;

  dt_iop_gui_enter_critical_section(self);

  const gboolean fail = !g->cursor_valid
                     || !g->interpolation_valid
                     || dev->full.pipe->processing
                     || !g->has_focus;

  dt_iop_gui_leave_critical_section(self);

  if(fail) return;

  if(!g->graph_valid)
    if(!_init_drawing(self, self->widget, g))
      return;

  // re-read the exposure in case it has changed
  if(g->luminance_valid && self->enabled) {
    const float lum = log2f(_luminance_from_thumb_preview_buf(self));
    g->cursor_exposure = fast_clamp(post_scale_shift(lum, p->post_scale, p->post_shift), DT_TONEEQ_MIN_EV, DT_TONEEQ_MAX_EV);
  }

  dt_iop_gui_enter_critical_section(self);

  // Get coordinates
  const float x_pointer = g->cursor_pos_x;
  const float y_pointer = g->cursor_pos_y;

  float exposure_in = 0.0f;
  float luminance_in = 0.0f;
  float correction = 0.0f;
  float exposure_out = 0.0f;
  float luminance_out = 0.0f;
  if(g->luminance_valid && self->enabled)
  {
    // Get the corresponding exposure
    exposure_in = g->cursor_exposure;
    luminance_in = exp2f(exposure_in);

    // Get the corresponding correction and compute resulting exposure
    correction = log2f(pixel_correction(exposure_in, g->factors, g->sigma));
    exposure_out = exposure_in + correction;
    luminance_out = exp2f(exposure_out);
  }

  dt_iop_gui_leave_critical_section(self);

  if(dt_isnan(exposure_in)) return; // something went wrong

  // set custom cursor dimensions
  const double outer_radius = 16.;
  const double inner_radius = outer_radius / 2.0;
  const double setting_offset_x = (outer_radius + 4. * g->inner_padding) / zoom_scale;
  const double fill_width = DT_PIXEL_APPLY_DPI(4. / zoom_scale);

  // setting fill bars
  match_color_to_background(cr, exposure_out, 1.0);
  cairo_set_line_width(cr, 2.0 * fill_width);
  cairo_move_to(cr, x_pointer - setting_offset_x, y_pointer);

  if(correction > 0.0f)
    cairo_arc(cr, x_pointer, y_pointer, setting_offset_x,
              M_PI, M_PI + correction * M_PI / 4.0);
  else
    cairo_arc_negative(cr, x_pointer, y_pointer, setting_offset_x,
                       M_PI, M_PI + correction * M_PI / 4.0);

  cairo_stroke(cr);

  // setting ground level
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.5 / zoom_scale));
  cairo_move_to(cr, x_pointer + (outer_radius + 2. * g->inner_padding) / zoom_scale,
                y_pointer);
  cairo_line_to(cr, x_pointer + outer_radius / zoom_scale, y_pointer);
  cairo_move_to(cr, x_pointer - outer_radius / zoom_scale, y_pointer);
  cairo_line_to(cr, x_pointer - setting_offset_x - 4.0 * g->inner_padding / zoom_scale,
                y_pointer);
  cairo_stroke(cr);

  // setting cursor cross hair
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.5 / zoom_scale));
  cairo_move_to(cr, x_pointer, y_pointer + setting_offset_x + fill_width);
  cairo_line_to(cr, x_pointer, y_pointer + outer_radius / zoom_scale);
  cairo_move_to(cr, x_pointer, y_pointer - outer_radius / zoom_scale);
  cairo_line_to(cr, x_pointer, y_pointer - setting_offset_x - fill_width);
  cairo_stroke(cr);

  // draw exposure cursor
  draw_exposure_cursor(cr, x_pointer, y_pointer, outer_radius,
                       luminance_in, zoom_scale, 6, .9);
  draw_exposure_cursor(cr, x_pointer, y_pointer, inner_radius,
                       luminance_out, zoom_scale, 3, .9);

  // Create Pango objects : texts
  char text[256];
  PangoLayout *layout;
  PangoRectangle ink;
  PangoFontDescription *desc =
    pango_font_description_copy_static(darktable.bauhaus->pango_font_desc);

  // Avoid text resizing based on zoom level
  const int old_size = pango_font_description_get_size(desc);
  pango_font_description_set_size (desc, (int)(old_size / zoom_scale));
  layout = pango_cairo_create_layout(cr);
  pango_layout_set_font_description(layout, desc);
  pango_cairo_context_set_resolution(pango_layout_get_context(layout), darktable.gui->dpi);

  // Build text object
  if(g->luminance_valid && self->enabled)
    snprintf(text, sizeof(text), _("%+.1f EV"), exposure_in);
  else
    snprintf(text, sizeof(text), "? EV");
  pango_layout_set_text(layout, text, -1);
  pango_layout_get_pixel_extents(layout, &ink, NULL);

  // Draw the text plain blackground
  get_shade_from_luminance(cr, luminance_out, 0.75);
  cairo_rectangle(cr,
                  x_pointer + (outer_radius + 2. * g->inner_padding) / zoom_scale,
                  y_pointer - ink.y - ink.height / 2.0 - g->inner_padding / zoom_scale,
                  ink.width + 2.0 * ink.x + 4. * g->inner_padding / zoom_scale,
                  ink.height + 2.0 * ink.y + 2. * g->inner_padding / zoom_scale);
  cairo_fill(cr);

  // Display the EV reading
  match_color_to_background(cr, exposure_out, 1.0);
  cairo_move_to(cr, x_pointer + (outer_radius + 4. * g->inner_padding) / zoom_scale,
                    y_pointer - ink.y - ink.height / 2.);
  pango_cairo_show_layout(cr, layout);

  cairo_stroke(cr);

  pango_font_description_free(desc);
  g_object_unref(layout);

  if(g->luminance_valid && self->enabled)
  {
    // Search for nearest node in graph and highlight it
    const float radius_threshold = 0.45f;
    g->area_active_node = -1;
    if(g->cursor_valid)
      for(int i = 0; i < NUM_SLIDERS; ++i)
      {
        const float delta_x = fabsf(g->cursor_exposure - centers_params[i]);
        if(delta_x < radius_threshold)
          g->area_active_node = i;
      }

    gtk_widget_queue_draw(GTK_WIDGET(g->area));
  }
}


static void _develop_distort_callback(gpointer instance,
                                      dt_iop_module_t *self)
{
  dt_iop_toneequalizer_gui_data_t *g = self->gui_data;
  if(g == NULL) return;
  if(!g->distort_signal_active) return;

  /* disable the distort signal now to avoid recursive call on this signal as we are
     about to reprocess the preview pipe which has some module doing distortion. */

  _unset_distort_signal(self);

  /* we do reprocess the preview to get a new internal image buffer with the proper
     image geometry. */
  if(self->enabled)
    dt_dev_reprocess_preview(darktable.develop);
}


static void _set_distort_signal(dt_iop_module_t *self)
{
  dt_iop_toneequalizer_gui_data_t *g = self->gui_data;
  if(self->enabled && !g->distort_signal_active)
  {
    DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_DEVELOP_DISTORT, _develop_distort_callback);
    g->distort_signal_active = TRUE;
  }
}


static void _unset_distort_signal(dt_iop_module_t *self)
{
  dt_iop_toneequalizer_gui_data_t *g = self->gui_data;
  if(g->distort_signal_active)
  {
    DT_CONTROL_SIGNAL_DISCONNECT(_develop_distort_callback, self);
    g->distort_signal_active = FALSE;
  }
}


void gui_focus(dt_iop_module_t *self, gboolean in)
{
  printf("gui_focus %d\n", in);
  dt_iop_toneequalizer_gui_data_t *g = self->gui_data;
  dt_iop_gui_enter_critical_section(self);
  g->has_focus = in;
  dt_iop_gui_leave_critical_section(self);
  switch_cursors(self);
  if(!in)
  {
    //lost focus - stop showing mask
    const gboolean was_mask = g->mask_display;
    g->mask_display = FALSE;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->show_luminance_mask), FALSE);
    if(was_mask)
      dt_dev_reprocess_center(self->dev);
    dt_collection_hint_message(darktable.collection);

    // no need for the distort signal anymore
    _unset_distort_signal(self);
  }
  else
  {
    dt_control_hinter_message(_("scroll over image to change tone exposure\n"
                                "shift+scroll for large steps; "
                                "ctrl+scroll for small steps"));
    // listen to distort change again
    _set_distort_signal(self);
  }
}


static inline gboolean _init_drawing(dt_iop_module_t *const restrict self,
                                     GtkWidget *widget,
                                     dt_iop_toneequalizer_gui_data_t *const restrict g)
{
  // Cache the equalizer graph objects to avoid recomputing all the view at each redraw
  gtk_widget_get_allocation(widget, &g->allocation);

  if(g->cst)
    cairo_surface_destroy(g->cst);

  g->allocation.height -= DT_RESIZE_HANDLE_SIZE;
  g->cst = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                         g->allocation.width, g->allocation.height);

  if(g->cr)
    cairo_destroy(g->cr);
  g->cr = cairo_create(g->cst);

  if(g->layout)
    g_object_unref(g->layout);
  g->layout = pango_cairo_create_layout(g->cr);

  if(g->desc)
    pango_font_description_free(g->desc);
  g->desc = pango_font_description_copy_static(darktable.bauhaus->pango_font_desc);

  pango_layout_set_font_description(g->layout, g->desc);
  pango_cairo_context_set_resolution
    (pango_layout_get_context(g->layout), darktable.gui->dpi);
  g->context = gtk_widget_get_style_context(widget);

  char text[256];

  // Get the text line height for spacing
  snprintf(text, sizeof(text), "X");
  pango_layout_set_text(g->layout, text, -1);
  pango_layout_get_pixel_extents(g->layout, &g->ink, NULL);
  g->line_height = g->ink.height;

  // Get the width of a minus sign for legend labels spacing
  snprintf(text, sizeof(text), "-");
  pango_layout_set_text(g->layout, text, -1);
  pango_layout_get_pixel_extents(g->layout, &g->ink, NULL);
  g->sign_width = g->ink.width / 2.0;

  // Set the sizes, margins and paddings
  g->inner_padding = 4; // TODO: INNER_PADDING value as defined in
                        // bauhaus.c macros, sync them
  g->inset = g->inner_padding + darktable.bauhaus->quad_width;
  // align the right border on sliders:
  g->graph_width = g->allocation.width - g->inset - 2.0 * g->line_height;
   // give room to nodes:
  g->graph_height = g->allocation.height - g->inset - 2.0 * g->line_height;
  g->gradient_left_limit = 0.0;
  g->gradient_right_limit = g->graph_width;
  g->gradient_top_limit = g->graph_height + 2 * g->inner_padding;
  g->gradient_width = g->gradient_right_limit - g->gradient_left_limit;
  g->legend_top_limit = -0.5 * g->line_height - 2.0 * g->inner_padding;
  g->x_label = g->graph_width + g->sign_width + 3.0 * g->inner_padding;

  gtk_render_background(g->context, g->cr, 0, 0, g->allocation.width, g->allocation.height);

  // set the graph as the origin of the coordinates
  cairo_translate(g->cr, g->line_height + 2 * g->inner_padding,
                  g->line_height + 3 * g->inner_padding);

  // display x-axis and y-axis legends (EV)
  set_color(g->cr, darktable.bauhaus->graph_fg);

  float value = -8.0f;

  for(int k = 0; k < NUM_SLIDERS; k++)
  {
    const float xn =
      (((float)k) / ((float)(NUM_SLIDERS - 1))) * g->graph_width - g->sign_width;

    snprintf(text, sizeof(text), "%+.0f", value);
    pango_layout_set_text(g->layout, text, -1);
    pango_layout_get_pixel_extents(g->layout, &g->ink, NULL);
    cairo_move_to(g->cr, xn - 0.5 * g->ink.width - g->ink.x,
                         g->legend_top_limit - 0.5 * g->ink.height - g->ink.y);
    pango_cairo_show_layout(g->cr, g->layout);
    cairo_stroke(g->cr);

    value += 1.0;
  }

  value = 2.0f;

  for(int k = 0; k < 5; k++)
  {
    const float yn = (k / 4.0f) * g->graph_height;
    snprintf(text, sizeof(text), "%+.0f", value);
    pango_layout_set_text(g->layout, text, -1);
    pango_layout_get_pixel_extents(g->layout, &g->ink, NULL);
    cairo_move_to(g->cr, g->x_label - 0.5 * g->ink.width - g->ink.x,
                yn - 0.5 * g->ink.height - g->ink.y);
    pango_cairo_show_layout(g->cr, g->layout);
    cairo_stroke(g->cr);

    value -= 1.0;
  }

  /** x axis **/
  // Draw the perceptually even gradient
  cairo_pattern_t *grad;
  grad = cairo_pattern_create_linear(g->gradient_left_limit, 0.0,
                                     g->gradient_right_limit, 0.0);
  dt_cairo_perceptual_gradient(grad, 1.0);
  cairo_set_line_width(g->cr, 0.0);
  cairo_rectangle(g->cr, g->gradient_left_limit, g->gradient_top_limit,
                  g->gradient_width, g->line_height);
  cairo_set_source(g->cr, grad);
  cairo_fill(g->cr);
  cairo_pattern_destroy(grad);

  /** y axis **/
  // Draw the perceptually even gradient
  grad = cairo_pattern_create_linear(0.0, g->graph_height, 0.0, 0.0);
  dt_cairo_perceptual_gradient(grad, 1.0);
  cairo_set_line_width(g->cr, 0.0);
  cairo_rectangle(g->cr, -g->line_height - 2 * g->inner_padding, 0.0,
                  g->line_height, g->graph_height);
  cairo_set_source(g->cr, grad);
  cairo_fill(g->cr);

  cairo_pattern_destroy(grad);

  // Draw frame borders
  cairo_set_line_width(g->cr, DT_PIXEL_APPLY_DPI(0.5));
  set_color(g->cr, darktable.bauhaus->graph_border);
  cairo_rectangle(g->cr, 0, 0, g->graph_width, g->graph_height);
  cairo_stroke_preserve(g->cr);

  // end of caching section, this will not be drawn again

  dt_iop_gui_enter_critical_section(self);
  g->graph_valid = TRUE;
  dt_iop_gui_leave_critical_section(self);

  return TRUE;
}


// must be called while holding self->gui_lock
static inline void init_nodes_x(dt_iop_toneequalizer_gui_data_t *g)
{
  if(g == NULL) return;

  if(!g->valid_nodes_x && g->graph_width > 0)
  {
    for(int i = 0; i < NUM_SLIDERS; ++i)
      g->nodes_x[i] = (((float)i) / ((float)(NUM_SLIDERS - 1))) * g->graph_width;
    g->valid_nodes_x = TRUE;
  }
}


// must be called while holding self->gui_lock
static inline void init_nodes_y(dt_iop_toneequalizer_gui_data_t *g)
{
  if(g == NULL) return;

  if(g->user_param_valid && g->graph_height > 0)
  {
    for(int i = 0; i < NUM_SLIDERS; ++i)
      g->nodes_y[i] = // assumes factors in [-2 ; 2] EV
        (0.5 - log2f(g->temp_user_params[i]) / 4.0) * g->graph_height;
    g->valid_nodes_y = TRUE;
  }
}


static inline void interpolate_gui_color(GdkRGBA a, GdkRGBA b, float t, GdkRGBA *out)
{
  float t_clamp = fast_clamp(t, 0.0f, 1.0f);
  out->red = a.red + t_clamp * (b.red - a.red);
  out->green = a.green + t_clamp * (b.green - a.green);
  out->blue = a.blue + t_clamp * (b.blue - a.blue);
  out->alpha = a.alpha + t_clamp * (b.alpha - a.alpha);
}


static inline void compute_gui_curve_colors(dt_iop_module_t *self)
{
  dt_iop_toneequalizer_gui_data_t *g = self->gui_data;
  dt_iop_toneequalizer_params_t *p = self->params;

  const float *const restrict curve = g->gui_curve;
  GdkRGBA *const restrict colors = g->gui_curve_colors;
  const gboolean filter_active = (p->filter != DT_TONEEQ_NONE);
  const float ev_dx = g->image_EV_per_UI_sample;

  const GdkRGBA standard = darktable.bauhaus->graph_fg;
  const GdkRGBA warning = {0.75, 0.5, 0.0, 1.0};
  const GdkRGBA error = {1.0, 0.0, 0.0, 1.0};
  GdkRGBA temp_color = {0.0, 0.0, 0.0, 1.0};

  const int shadows_limit = (int)UI_HISTO_SAMPLES * 0.3333;
  const int highlights_limit = (int)UI_HISTO_SAMPLES * 0.6666;

  // printf("ev_dx=%f filter_active=%d filter=%d\n", ev_dx, filter_active, p->filter);

  if (!g->gui_histogram_valid || !g->gui_curve_valid) {
    // the module is not completely initialized, set all colors to standard
    for(int k = 0; k < UI_HISTO_SAMPLES; k++)
      colors[k] = standard;
    return;
  };

  colors[0] = standard;
  for(int k = 1; k < UI_HISTO_SAMPLES; k++)
  {
    float ev_dy = (curve[k] - curve[k - 1]);
    float steepness = ev_dy / ev_dx;
    colors[k] = standard;

    if(filter_active && k < shadows_limit && curve[k] < 0.0f)
    {
      // Lower shadows with filter active, this does not provide the local
      // contrast that the user probably expects.
      const float x_dist = ((float)(shadows_limit - k) / (float)UI_HISTO_SAMPLES) * 8.0f;
      const float color_dist = fminf(x_dist, -curve[k]);
      interpolate_gui_color(standard, warning, color_dist, &temp_color);
      colors[k] = temp_color;
    }
    else if(filter_active && k > highlights_limit && curve[k] > 0.0f)
    {
      // Raise highlights with filter active, this does not provide the local
      // contrast that the user probably expects.
      const float x_dist = ((float)(k - highlights_limit) / (float)UI_HISTO_SAMPLES) * 8.0f;
      const float color_dist = fminf(x_dist, curve[k]);
      interpolate_gui_color(standard, warning, color_dist, &temp_color);
      colors[k] = temp_color;
    }
    else if(!filter_active && k < shadows_limit && curve[k] > 0.0f)
    {
      // Raise shadows without filter, this leads to a loss of contrast.
      const float x_dist = ((float)(shadows_limit - k) / (float)UI_HISTO_SAMPLES) * 8.0f;
      const float color_dist = fminf(x_dist, curve[k]);
      interpolate_gui_color(standard, warning, color_dist, &temp_color);
      colors[k] = temp_color;
    }
    else if(!filter_active && k > highlights_limit && curve[k] < 0.0f)
    {
      // Lower highlights without filter, this leads to a loss of contrast.
      const float x_dist = ((float)(k - highlights_limit) / (float)UI_HISTO_SAMPLES) * 8.0f;
      const float color_dist = fminf(x_dist, -curve[k]);
      interpolate_gui_color(standard, warning, color_dist, &temp_color);
      colors[k] = temp_color;
    }

    // Too steep downward slopes.
    // These warnings take precedence, even if the segment was already
    // colored, we overwrite the colors here.
    if(steepness < -0.5f && steepness > -1.0f)
    {
      colors[k] = warning;
    }
    else if(steepness <= -1.0f)
    {
      colors[k] = error;
    }

    // printf("curve[%d]=%f ev_dx=%f ev_dy=%f steepness=%f colors[%d]=%f\n", k, curve[k], ev_dx, ev_dy, steepness, k, colors[k].red);
  }
}


static gboolean area_draw(GtkWidget *widget,
                          cairo_t *cr,
                          dt_iop_module_t *self)
{
  // Draw the widget equalizer view
  dt_iop_toneequalizer_gui_data_t *g = self->gui_data;
  dt_iop_toneequalizer_params_t *p = self->params;

  if(g == NULL) return FALSE;

  // Init or refresh the drawing cache
  //if(!g->graph_valid)

  // this can be cached and drawn just once, but too lazy to debug a
  // cache invalidation for Cairo objects
  if(!_init_drawing(self, widget, g))
    return FALSE;

  // since the widget sizes are not cached and invalidated properly
  // above (yet…)  force the invalidation of the nodes coordinates to
  // account for possible widget resizing
  dt_iop_gui_enter_critical_section(self);
  g->valid_nodes_x = FALSE;
  g->valid_nodes_y = FALSE;
  dt_iop_gui_leave_critical_section(self);

  // Refresh cached UI elements
  update_gui_histogram(self);
  curve_interpolation(self);

  // The colors depend on the histogram and the curve
  compute_gui_curve_colors(self);

  // Draw graph background
  cairo_set_line_width(g->cr, DT_PIXEL_APPLY_DPI(0.5));
  cairo_rectangle(g->cr, 0, 0, g->graph_width, g->graph_height);
  set_color(g->cr, darktable.bauhaus->graph_bg);
  cairo_fill(g->cr);

  // draw grid
  cairo_set_line_width(g->cr, DT_PIXEL_APPLY_DPI(0.5));
  set_color(g->cr, darktable.bauhaus->graph_border);
  dt_draw_grid(g->cr, 8, 0, 0, g->graph_width, g->graph_height);

  // draw ground level
  set_color(g->cr, darktable.bauhaus->graph_fg);
  cairo_set_line_width(g->cr, DT_PIXEL_APPLY_DPI(1));
  cairo_move_to(g->cr, 0, 0.5 * g->graph_height);
  cairo_line_to(g->cr, g->graph_width, 0.5 * g->graph_height);
  cairo_stroke(g->cr);

  if(g->gui_histogram_valid && self->enabled)
  {
    float histo_height;
    if (g->two_histograms_display)
      histo_height = 0.5 * g->graph_height;
    else
      histo_height = g->graph_height;

    // draw the mask histogram background
    set_color(g->cr, darktable.bauhaus->inset_histogram);
    cairo_set_line_width(g->cr, DT_PIXEL_APPLY_DPI(4.0));
    cairo_move_to(g->cr, 0, g->graph_height);

    for(int k = 0; k < UI_HISTO_SAMPLES; k++)
    {
      // the x range is [-8;+0] EV
      const float x_temp = (8.0 * (float)k / (float)(UI_HISTO_SAMPLES - 1)) - 8.0;
      const float y_temp = fast_clamp((float)(g->histogram[k]) / (float)(g->max_histogram), -1.0f, 1.0f) * 0.96;
      cairo_line_to(g->cr, (x_temp + 8.0) * g->graph_width / 8.0,
                           (g->graph_height - y_temp * histo_height));
    }
    cairo_line_to(g->cr, g->graph_width, g->graph_height);
    cairo_close_path(g->cr);
    cairo_fill(g->cr);

    // optionally draw the image histogram upside-down in the top half
    if (g->two_histograms_display)
    {
      cairo_move_to(g->cr, 0, 0);

      for(int k = 0; k < UI_HISTO_SAMPLES; k++)
      {
        // the x range is [-8;+0] EV
        const float x_temp = (8.0 * (float)k / (float)(UI_HISTO_SAMPLES - 1)) - 8.0;
        const float y_temp = fast_clamp((float)(g->image_histogram[k]) / (float)(g->max_image_histogram), -1.0f, 1.0f) * 0.96;
        cairo_line_to(g->cr, (x_temp + 8.0) * g->graph_width / 8.0,
                             y_temp * histo_height);
        //if (k % 5 == 0)
        // printf("g->image_histogram[%d] = %d, max_image_histogram = %d\n", k, g->image_histogram[k], g->max_image_histogram);
      }
      cairo_line_to(g->cr, g->graph_width, 0);
      cairo_close_path(g->cr);
      cairo_fill(g->cr);
    }

    if(post_scale_shift(g->histogram_last_decile, p->post_scale, p->post_shift) > -0.1f)
    {
      // histogram overflows controls in highlights : display warning
      cairo_save(g->cr);
      cairo_set_source_rgb(g->cr, 0.75, 0.50, 0.);
      dtgtk_cairo_paint_gamut_check
        (g->cr,
         g->graph_width - 2.5 * g->line_height, 0.5 * g->line_height,
         2.0 * g->line_height, 2.0 * g->line_height, 0, NULL);
      cairo_restore(g->cr);
    }

    if(post_scale_shift(g->histogram_first_decile, p->post_scale, p->post_shift) < -7.9f)
    {
      // histogram overflows controls in lowlights : display warning
      cairo_save(g->cr);
      cairo_set_source_rgb(g->cr, 0.75, 0.50, 0.);
      dtgtk_cairo_paint_gamut_check
        (g->cr,
         0.5 * g->line_height, 0.5 * g->line_height,
         2.0 * g->line_height, 2.0 * g->line_height, 0, NULL);
      cairo_restore(g->cr);
    }
  }

  if(g->gui_curve_valid)
  {
    // draw the interpolation curve

    cairo_set_line_width(g->cr, DT_PIXEL_APPLY_DPI(3));
    float x_draw, y_draw;

    // The coloring of the curve makes it necessary to draw it as individual segments.
    // However this led to aliasing artifacts, therefore we draw overlapping segments
    // from k-1 to k+1.
    for(int k = 1; k < UI_HISTO_SAMPLES - 1; k++)
    {
      set_color(g->cr, g->gui_curve_colors[k]);

      // Map [0;UI_HISTO_SAMPLES] to [0;1] and then to g->graph_width.
      x_draw = ((float)(k - 1) / (float)(UI_HISTO_SAMPLES - 1)) * g->graph_width;
      // Map [-2;+2] EV to graph_height, with graph pixel 0 at the top
      y_draw = (0.5f - g->gui_curve[k - 1] / 4.0f) * g->graph_height;

      cairo_move_to(g->cr, x_draw, y_draw);

      // Map [0;UI_HISTO_SAMPLES] to [0;1] and then to g->graph_width.
      x_draw = ((float)(k+1) / (float)(UI_HISTO_SAMPLES - 1)) * g->graph_width;
      // Map [-2;+2] EV to graph_height, with graph pixel 0 at the top
      y_draw = (0.5f - g->gui_curve[k+1] / 4.0f) * g->graph_height;

      cairo_line_to(g->cr, x_draw, y_draw);
      cairo_stroke(g->cr);
    }

  }

  dt_iop_gui_enter_critical_section(self);
  init_nodes_x(g);
  dt_iop_gui_leave_critical_section(self);

  dt_iop_gui_enter_critical_section(self);
  init_nodes_y(g);
  dt_iop_gui_leave_critical_section(self);

  if(g->user_param_valid)
  {
    // draw nodes positions
    for(int k = 0; k < NUM_SLIDERS; k++)
    {
      const float xn = g->nodes_x[k];
      const float yn = g->nodes_y[k];

      // fill bars
      cairo_set_line_width(g->cr, DT_PIXEL_APPLY_DPI(6));
      set_color(g->cr, darktable.bauhaus->color_fill);
      cairo_move_to(g->cr, xn, 0.5 * g->graph_height);
      cairo_line_to(g->cr, xn, yn);
      cairo_stroke(g->cr);

      // bullets
      cairo_set_line_width(g->cr, DT_PIXEL_APPLY_DPI(3));
      cairo_arc(g->cr, xn, yn, DT_PIXEL_APPLY_DPI(4), 0, 2. * M_PI);
      set_color(g->cr, darktable.bauhaus->graph_fg);
      cairo_stroke_preserve(g->cr);

      if(g->area_active_node == k)
        set_color(g->cr, darktable.bauhaus->graph_fg);
      else
        set_color(g->cr, darktable.bauhaus->graph_bg);

      cairo_fill(g->cr);
    }
  }

  if(self->enabled)
  {
    if(g->area_cursor_valid)
    {
      const float radius = g->sigma * g->graph_width / 8.0f / sqrtf(2.0f);
      cairo_set_line_width(g->cr, DT_PIXEL_APPLY_DPI(1.5));
      const float y = 0.5f -
        g->gui_curve[(int)CLAMP(((UI_HISTO_SAMPLES - 1) * g->area_x / g->graph_width),
                              0, UI_HISTO_SAMPLES - 1)] / 4.0f;
      cairo_arc(g->cr, g->area_x, y * g->graph_height, radius, 0, 2. * M_PI);
      set_color(g->cr, darktable.bauhaus->graph_fg);
      cairo_stroke(g->cr);
    }

    if(g->cursor_valid)
    {

      float x_pos = (g->cursor_exposure + 8.0f) / 8.0f * g->graph_width;

      if(x_pos >= g->graph_width || x_pos <= 0.0f)
      {
        // exposure at current position is outside [-8; 0] EV :
        // bound it in the graph limits and show it in orange
        cairo_set_source_rgb(g->cr, 0.75, 0.50, 0.);
        cairo_set_line_width(g->cr, DT_PIXEL_APPLY_DPI(3));
        x_pos = (x_pos <= 0.0f) ? 0.0f : g->graph_width;
      }
      else
      {
        set_color(g->cr, darktable.bauhaus->graph_fg);
        cairo_set_line_width(g->cr, DT_PIXEL_APPLY_DPI(1.5));
      }

      cairo_move_to(g->cr, x_pos, 0.0);
      cairo_line_to(g->cr, x_pos, g->graph_height);
      cairo_stroke(g->cr);
    }
  }

  // clean and exit
  cairo_set_source_surface(cr, g->cst, 0, 0);
  cairo_paint(cr);

  return TRUE;
}


static gboolean _toneequalizer_bar_draw(GtkWidget *widget,
                                        cairo_t *crf,
                                        dt_iop_module_t *self)
{
  // Draw the widget equalizer view
  dt_iop_toneequalizer_gui_data_t *g = self->gui_data;

  update_gui_histogram(self);

  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  cairo_surface_t *cst = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                                       allocation.width, allocation.height);
  cairo_t *cr = cairo_create(cst);

  // draw background
  set_color(cr, darktable.bauhaus->graph_bg);
  cairo_rectangle(cr, 0, 0, allocation.width, allocation.height);
  cairo_fill_preserve(cr);
  cairo_clip(cr);

  dt_iop_gui_enter_critical_section(self);

  if(g->gui_histogram_valid)
  {
    // draw histogram span
    const float left = (g->histogram_first_decile + 8.0f) / 8.0f;
    const float right = (g->histogram_last_decile + 8.0f) / 8.0f;
    const float width = (right - left);
    set_color(cr, darktable.bauhaus->inset_histogram);
    cairo_rectangle(cr, left * allocation.width, 0,
                    width * allocation.width, allocation.height);
    cairo_fill(cr);

    // draw average bar
    set_color(cr, darktable.bauhaus->graph_fg);
    cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(3));
    const float average = ((g->histogram_first_decile + g->histogram_last_decile) / 2.0f + 8.0f) / 8.0f;
    cairo_move_to(cr, average * allocation.width, 0.0);
    cairo_line_to(cr, average * allocation.width, allocation.height);
    cairo_stroke(cr);

    // draw clipping bars
    cairo_set_source_rgb(cr, 0.75, 0.50, 0);
    cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(6));
    if(g->histogram_first_decile < -7.9f)
    {
      cairo_move_to(cr, DT_PIXEL_APPLY_DPI(3), 0.0);
      cairo_line_to(cr, DT_PIXEL_APPLY_DPI(3), allocation.height);
      cairo_stroke(cr);
    }
    if(g->histogram_last_decile > - 0.1f)
    {
      cairo_move_to(cr, allocation.width - DT_PIXEL_APPLY_DPI(3), 0.0);
      cairo_line_to(cr, allocation.width - DT_PIXEL_APPLY_DPI(3), allocation.height);
      cairo_stroke(cr);
    }
  }

  dt_iop_gui_leave_critical_section(self);

  cairo_set_source_surface(crf, cst, 0, 0);
  cairo_paint(crf);
  cairo_destroy(cr);
  cairo_surface_destroy(cst);
  return TRUE;
}


static gboolean area_enter_leave_notify(GtkWidget *widget,
                                        GdkEventCrossing *event,
                                        dt_iop_module_t *self)
{
  if(darktable.gui->reset) return TRUE;
  if(!self->enabled) return FALSE;

  dt_iop_toneequalizer_gui_data_t *g = self->gui_data;

  dt_iop_toneequalizer_params_t *p = self->params;

  if(g->area_dragging)
  {
    // cursor left area : force commit to avoid glitches
    update_exposure_sliders(g, p);

    dt_dev_add_history_item(darktable.develop, self, FALSE);
  }
  dt_iop_gui_enter_critical_section(self);
  g->area_x = (event->x - g->inset);
  g->area_y = (event->y - g->inset);
  g->area_dragging = FALSE;
  g->area_active_node = -1;
  g->area_cursor_valid = (g->area_x > 0.0f
                          && g->area_x < g->graph_width
                          && g->area_y > 0.0f
                          && g->area_y < g->graph_height);
  dt_iop_gui_leave_critical_section(self);

  gtk_widget_queue_draw(GTK_WIDGET(g->area));
  return FALSE;
}


static gboolean area_button_press(GtkWidget *widget,
                                  GdkEventButton *event,
                                  dt_iop_module_t *self)
{

  if(darktable.gui->reset) return TRUE;

  dt_iop_toneequalizer_gui_data_t *g = self->gui_data;

  dt_iop_request_focus(self);

  if(event->button == GDK_BUTTON_PRIMARY && event->type == GDK_2BUTTON_PRESS)
  {
    dt_iop_toneequalizer_params_t *p = self->params;
    const dt_iop_toneequalizer_params_t *const d = self->default_params;

    // reset nodes params
    p->noise = d->noise;
    p->ultra_deep_blacks = d->ultra_deep_blacks;
    p->deep_blacks = d->deep_blacks;
    p->blacks = d->blacks;
    p->shadows = d->shadows;
    p->midtones = d->midtones;
    p->highlights = d->highlights;
    p->whites = d->whites;
    p->speculars = d->speculars;

    // update UI sliders
    update_exposure_sliders(g, p);

    // Redraw graph
    gtk_widget_queue_draw(GTK_WIDGET(g->area));
    dt_dev_add_history_item(darktable.develop, self, TRUE);
    return TRUE;
  }
  else if(event->button == GDK_BUTTON_PRIMARY)
  {
    if(self->enabled)
    {
      g->area_dragging = TRUE;
      gtk_widget_queue_draw(GTK_WIDGET(g->area));
    }
    else
    {
      dt_dev_add_history_item(darktable.develop, self, TRUE);
    }
    return TRUE;
  }

  // Unlock the colour picker so we can display our own custom cursor
  dt_iop_color_picker_reset(self, TRUE);

  return FALSE;
}


static gboolean area_motion_notify(GtkWidget *widget,
                                   GdkEventMotion *event,
                                   dt_iop_module_t *self)
{
  if(darktable.gui->reset) return TRUE;
  if(!self->enabled) return FALSE;

  dt_iop_toneequalizer_gui_data_t *g = self->gui_data;
  dt_iop_toneequalizer_params_t *p = self->params;

  if(g->area_dragging)
  {
    // vertical distance travelled since button_pressed event
    dt_iop_gui_enter_critical_section(self);
    // graph spans over 4 EV
    const float offset = (-event->y + g->area_y) / g->graph_height * 4.0f;
    const float cursor_exposure = g->area_x / g->graph_width * 8.0f - 8.0f;

    // Get the desired correction on exposure NUM_SLIDERS
    g->area_dragging = set_new_params_interactive(cursor_exposure, offset,
                                                  g->sigma * g->sigma / 2.0f, g, p);
    dt_iop_gui_leave_critical_section(self);
  }

  dt_iop_gui_enter_critical_section(self);
  g->area_x = event->x - g->inset;
  g->area_y = event->y;
  g->area_cursor_valid = (g->area_x > 0.0f
                          && g->area_x < g->graph_width
                          && g->area_y > 0.0f
                          && g->area_y < g->graph_height);
  g->area_active_node = -1;

  // Search if cursor is close to a node
  if(g->valid_nodes_x)
  {
    const float radius_threshold = fabsf(g->nodes_x[1] - g->nodes_x[0]) * 0.45f;
    for(int i = 0; i < NUM_SLIDERS; ++i)
    {
      const float delta_x = fabsf(g->area_x - g->nodes_x[i]);
      if(delta_x < radius_threshold)
      {
        g->area_active_node = i;
        g->area_cursor_valid = TRUE;
      }
    }
  }
  dt_iop_gui_leave_critical_section(self);

  gtk_widget_queue_draw(GTK_WIDGET(g->area));
  return TRUE;
}


static gboolean area_button_release(GtkWidget *widget,
                                    GdkEventButton *event,
                                    dt_iop_module_t *self)
{
  if(darktable.gui->reset) return TRUE;
  if(!self->enabled) return FALSE;

  dt_iop_toneequalizer_gui_data_t *g = self->gui_data;

  // Give focus to module
  dt_iop_request_focus(self);

  if(event->button == GDK_BUTTON_PRIMARY)
  {
    dt_iop_toneequalizer_params_t *p = self->params;

    if(g->area_dragging)
    {
      // Update GUI with new params
      update_exposure_sliders(g, p);

      dt_dev_add_history_item(darktable.develop, self, FALSE);

      dt_iop_gui_enter_critical_section(self);
      g->area_dragging = FALSE;
      dt_iop_gui_leave_critical_section(self);

      return TRUE;
    }
  }
  return FALSE;
}


static gboolean area_scroll(GtkWidget *widget,
                            GdkEventScroll *event,
                            gpointer user_data)
{
  // do not propagate to tab bar unless scrolling sidebar
  return !dt_gui_ignore_scroll(event);
}

static gboolean notebook_button_press(GtkWidget *widget,
                                      GdkEventButton *event,
                                      dt_iop_module_t *self)
{
  if(darktable.gui->reset) return TRUE;

  // Give focus to module
  dt_iop_request_focus(self);

  // Unlock the colour picker so we can display our own custom cursor
  dt_iop_color_picker_reset(self, TRUE);

  return FALSE;
}


GSList *mouse_actions(dt_iop_module_t *self)
{
  GSList *lm = NULL;
  lm = dt_mouse_action_create_format
    (lm, DT_MOUSE_ACTION_SCROLL, 0,
     _("[%s over image] change tone exposure"), self->name());
  lm = dt_mouse_action_create_format
    (lm, DT_MOUSE_ACTION_SCROLL, GDK_SHIFT_MASK,
     _("[%s over image] change tone exposure in large steps"), self->name());
  lm = dt_mouse_action_create_format
    (lm, DT_MOUSE_ACTION_SCROLL, GDK_CONTROL_MASK,
     _("[%s over image] change tone exposure in small steps"), self->name());
  return lm;
}


/**
 * Post pipe events
 **/
static void _develop_ui_pipe_started_callback(gpointer instance,
                                              dt_iop_module_t *self)
{
  printf("ui pipe started callback\n");
  dt_iop_toneequalizer_gui_data_t *g = self->gui_data;
  if(g == NULL) return;
  switch_cursors(self);

  if(!self->expanded || !self->enabled)
  {
    // if module is not active, disable mask preview
    dt_iop_gui_enter_critical_section(self);
    g->mask_display = FALSE;
    dt_iop_gui_leave_critical_section(self);
  }

  ++darktable.gui->reset;
  dt_iop_gui_enter_critical_section(self);

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->show_luminance_mask), g->mask_display);
  dt_iop_gui_leave_critical_section(self);
  --darktable.gui->reset;
}


static void _develop_preview_pipe_finished_callback(gpointer instance,
                                                    dt_iop_module_t *self)
{
  printf("preview pipe finished callback\n");
  dt_iop_toneequalizer_gui_data_t *g = self->gui_data;
  if(g == NULL) return;

  // now that the preview pipe is termintated, set back the distort signal to catch
  // any new changes from a module doing distortion. this signal has been disconnected
  // at the time the DT_SIGNAL_DEVELOP_DISTORT has been handled (see ) and a full
  // reprocess of the preview has been scheduled.
  _set_distort_signal(self);

  switch_cursors(self);
  gtk_widget_queue_draw(GTK_WIDGET(g->area));
  gtk_widget_queue_draw(GTK_WIDGET(g->bar));
}


static void _develop_ui_pipe_finished_callback(gpointer instance,
                                               dt_iop_module_t *self)
{
  printf("ui pipe finished callback\n");
  dt_iop_toneequalizer_gui_data_t *g = self->gui_data;
  if(g == NULL) return;
  switch_cursors(self);
}


void gui_reset(dt_iop_module_t *self)
{
  dt_iop_toneequalizer_gui_data_t *g = self->gui_data;
  if(g == NULL) return;
  dt_iop_request_focus(self);
  dt_bauhaus_widget_set_quad_active(g->exposure_boost, FALSE);
  dt_bauhaus_widget_set_quad_active(g->contrast_boost, FALSE);
  dt_dev_add_history_item(darktable.develop, self, TRUE);

  // Redraw graph
  gtk_widget_queue_draw(GTK_WIDGET(g->area));
}


void gui_init(dt_iop_module_t *self)
{
  dt_iop_toneequalizer_gui_data_t *g = IOP_GUI_ALLOC(toneequalizer);

  gui_cache_init(self);

  static dt_action_def_t notebook_def = { };
  g->notebook = dt_ui_notebook_new(&notebook_def);
  dt_action_define_iop(self, NULL, N_("page"), GTK_WIDGET(g->notebook), &notebook_def);

  // Curve view (former "advanced" page)
  self->widget = dt_ui_notebook_page(g->notebook, N_("curve"), NULL);
  gtk_widget_set_vexpand(GTK_WIDGET(self->widget), TRUE);

  // g->area = GTK_DRAWING_AREA(gtk_drawing_area_new());

  g->area = GTK_DRAWING_AREA(dt_ui_resize_wrap(NULL,
    0,
    "plugins/darkroom/toneequal/graphheight"));

  GtkWidget *wrapper = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0); // for CSS size
  gtk_box_pack_start(GTK_BOX(wrapper), GTK_WIDGET(g->area), TRUE, TRUE, 0);
  g_object_set_data(G_OBJECT(wrapper), "iop-instance", self);
  gtk_widget_set_name(GTK_WIDGET(wrapper), "toneeqgraph");
  dt_action_define_iop(self, NULL, N_("graph"), GTK_WIDGET(wrapper), NULL);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(wrapper), TRUE, TRUE, 0);
  gtk_widget_add_events(GTK_WIDGET(g->area),
                        GDK_POINTER_MOTION_MASK | darktable.gui->scroll_mask
                        | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK
                        | GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK);
  gtk_widget_set_can_focus(GTK_WIDGET(g->area), TRUE);
  g_signal_connect(G_OBJECT(g->area), "draw", G_CALLBACK(area_draw), self);
  g_signal_connect(G_OBJECT(g->area), "button-press-event",
                   G_CALLBACK(area_button_press), self);
  g_signal_connect(G_OBJECT(g->area), "button-release-event",
                   G_CALLBACK(area_button_release), self);
  g_signal_connect(G_OBJECT(g->area), "leave-notify-event",
                   G_CALLBACK(area_enter_leave_notify), self);
  g_signal_connect(G_OBJECT(g->area), "enter-notify-event",
                   G_CALLBACK(area_enter_leave_notify), self);
  g_signal_connect(G_OBJECT(g->area), "motion-notify-event",
                   G_CALLBACK(area_motion_notify), self);
  g_signal_connect(G_OBJECT(g->area), "scroll-event",
                   G_CALLBACK(area_scroll), self);
  gtk_widget_set_tooltip_text(GTK_WIDGET(g->area), _("double-click to reset the curve"));

  g->post_auto_align = dt_bauhaus_combobox_from_params(self, "post_auto_align");
  gtk_widget_set_tooltip_text(g->post_auto_align, _("automatically set the mask exposure/contrast"));

  g->post_scale = dt_bauhaus_slider_from_params(self, "post_scale");
  dt_bauhaus_slider_set_soft_range(g->post_scale, -2.0, 2.0);
  gtk_widget_set_tooltip_text
    (g->post_scale,
    _("set the mask contrast / scale the histogram"));

  g->post_shift = dt_bauhaus_slider_from_params(self, "post_shift");
  dt_bauhaus_slider_set_soft_range(g->post_shift, -4.0, 4.0);
  gtk_widget_set_tooltip_text
    (g->post_shift,
    _("set the mask exposure / shift the histogram"));

  g->smoothing = dt_bauhaus_slider_new_with_range(self, -2.33f, +1.67f, 0, 0.0f, 2);
  dt_bauhaus_slider_set_soft_range(g->smoothing, -1.0f, 1.0f);
  dt_bauhaus_widget_set_label(g->smoothing, NULL, N_("curve smoothing"));
  gtk_widget_set_tooltip_text(g->smoothing,
                              _("positive values will produce more progressive tone transitions\n"
                                "but the curve might become oscillatory in some settings.\n"
                                "negative values will avoid oscillations and behave more robustly\n"
                                "but may produce brutal tone transitions and damage local contrast."));
  gtk_box_pack_start(GTK_BOX(self->widget), g->smoothing, FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(g->smoothing), "value-changed", G_CALLBACK(smoothing_callback), self);


  // sliders section (former "simple" page)
  dt_gui_new_collapsible_section(&g->sliders_section, "plugins/darkroom/toneequal/expand_sliders", _("sliders"),
                                 GTK_BOX(self->widget), DT_ACTION(self));
  gtk_widget_set_tooltip_text(g->sliders_section.expander, _("sliders"));

  self->widget = GTK_WIDGET(g->sliders_section.container);

  g->noise = dt_bauhaus_slider_from_params(self, "noise");
  dt_bauhaus_slider_set_format(g->noise, _(" EV"));

  g->ultra_deep_blacks = dt_bauhaus_slider_from_params(self, "ultra_deep_blacks");
  dt_bauhaus_slider_set_format(g->ultra_deep_blacks, _(" EV"));

  g->deep_blacks = dt_bauhaus_slider_from_params(self, "deep_blacks");
  dt_bauhaus_slider_set_format(g->deep_blacks, _(" EV"));

  g->blacks = dt_bauhaus_slider_from_params(self, "blacks");
  dt_bauhaus_slider_set_format(g->blacks, _(" EV"));

  g->shadows = dt_bauhaus_slider_from_params(self, "shadows");
  dt_bauhaus_slider_set_format(g->shadows, _(" EV"));

  g->midtones = dt_bauhaus_slider_from_params(self, "midtones");
  dt_bauhaus_slider_set_format(g->midtones, _(" EV"));

  g->highlights = dt_bauhaus_slider_from_params(self, "highlights");
  dt_bauhaus_slider_set_format(g->highlights, _(" EV"));

  g->whites = dt_bauhaus_slider_from_params(self, "whites");
  dt_bauhaus_slider_set_format(g->whites, _(" EV"));

  g->speculars = dt_bauhaus_slider_from_params(self, "speculars");
  dt_bauhaus_slider_set_format(g->speculars, _(" EV"));

  dt_bauhaus_widget_set_label(g->noise, N_("simple"), N_("-8 EV"));
  dt_bauhaus_widget_set_label(g->ultra_deep_blacks, N_("simple"), N_("-7 EV"));
  dt_bauhaus_widget_set_label(g->deep_blacks, N_("simple"), N_("-6 EV"));
  dt_bauhaus_widget_set_label(g->blacks, N_("simple"), N_("-5 EV"));
  dt_bauhaus_widget_set_label(g->shadows, N_("simple"), N_("-4 EV"));
  dt_bauhaus_widget_set_label(g->midtones, N_("simple"), N_("-3 EV"));
  dt_bauhaus_widget_set_label(g->highlights, N_("simple"), N_("-2 EV"));
  dt_bauhaus_widget_set_label(g->whites, N_("simple"), N_("-1 EV"));
  dt_bauhaus_widget_set_label(g->speculars, N_("simple"), N_("+0 EV"));


  // Masking options
  self->widget = dt_ui_notebook_page(g->notebook, N_("masking"), NULL);

  g->lum_estimator = dt_bauhaus_combobox_from_params(self, "lum_estimator");
  gtk_widget_set_tooltip_text
    (g->lum_estimator,
     _("preview the mask and chose the estimator that gives you the\n"
       "higher contrast between areas to dodge and areas to burn"));

  g->filter = dt_bauhaus_combobox_from_params(self, N_("filter"));
  dt_bauhaus_widget_set_label(g->filter, NULL, N_("preserve details"));
  gtk_widget_set_tooltip_text
    (g->filter,
     _("'no' affects global and local contrast (safe if you only add contrast)\n"
       "'guided filter' only affects global contrast and tries to preserve local contrast\n"
       "'averaged guided filter' is a geometric mean of 'no' and 'guided filter' methods\n"
       "'EIGF' (exposure-independent guided filter) is a guided filter that is"
       " exposure-independent, it smooths shadows and highlights the same way"
       " (contrary to guided filter which smooths less the highlights)\n"
       "'averaged EIGF' is a geometric mean of 'no' and 'exposure-independent"
       " guided filter' methods"));

  g->iterations = dt_bauhaus_slider_from_params(self, "iterations");
  dt_bauhaus_slider_set_soft_max(g->iterations, 5);
  gtk_widget_set_tooltip_text
    (g->iterations,
     _("number of passes of guided filter to apply\n"
       "helps diffusing the edges of the filter at the expense of speed"));

  g->blending = dt_bauhaus_slider_from_params(self, "blending");
  dt_bauhaus_slider_set_soft_range(g->blending, 1.0, 45.0);
  dt_bauhaus_slider_set_format(g->blending, "%");
  gtk_widget_set_tooltip_text
    (g->blending,
     _("diameter of the blur in percent of the largest image size\n"
       "warning: big values of this parameter can make the darkroom\n"
       "preview much slower if denoise profiled is used."));

  g->feathering = dt_bauhaus_slider_from_params(self, "feathering");
  dt_bauhaus_slider_set_soft_range(g->feathering, 0.1, 50.0);
  gtk_widget_set_tooltip_text
    (g->feathering,
     _("precision of the feathering:\n"
       "higher values force the mask to follow edges more closely\n"
       "but may void the effect of the smoothing\n"
       "lower values give smoother gradients and better smoothing\n"
       "but may lead to inaccurate edges taping and halos"));

  // gtk_box_pack_start(GTK_BOX(self->widget),
  //                    dt_ui_section_label_new(C_("section", "mask post-processing")),
  //                    FALSE, FALSE, 0);
  dt_gui_new_collapsible_section(&g->advanced_masking_section, "plugins/darkroom/toneequal/expand_advanced_masking",
    _("mask pre-processing"), GTK_BOX(self->widget), DT_ACTION(self));
  gtk_widget_set_tooltip_text(g->advanced_masking_section.expander, _("advanced masking"));

  self->widget = GTK_WIDGET(g->advanced_masking_section.container);

  g->bar = GTK_DRAWING_AREA(gtk_drawing_area_new());
  gtk_widget_set_size_request(GTK_WIDGET(g->bar), -1, 40);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->bar), TRUE, TRUE, 0);
  gtk_widget_set_can_focus(GTK_WIDGET(g->bar), TRUE);
  g_signal_connect(G_OBJECT(g->bar), "draw",
                   G_CALLBACK(_toneequalizer_bar_draw), self);
  gtk_widget_set_tooltip_text
    (GTK_WIDGET(g->bar),
     _("mask histogram span between the first and last deciles.\n"
       "the central line shows the average. orange bars appear at extrema"
       " if clipping occurs."));

  g->quantization = dt_bauhaus_slider_from_params(self, "quantization");
  dt_bauhaus_slider_set_format(g->quantization, _(" EV"));
  gtk_widget_set_tooltip_text
    (g->quantization,
     _("0 disables the quantization.\n"
       "higher values posterize the luminance mask to help the guiding\n"
       "produce piece-wise smooth areas when using high feathering values"));

  g->exposure_boost = dt_bauhaus_slider_from_params(self, "exposure_boost");
  dt_bauhaus_slider_set_soft_range(g->exposure_boost, -4.0, 4.0);
  dt_bauhaus_slider_set_format(g->exposure_boost, _(" EV"));
  gtk_widget_set_tooltip_text
    (g->exposure_boost,
     _("use this to slide the mask average exposure along NUM_SLIDERS\n"
       "for a better control of the exposure correction with the available nodes."));
  dt_bauhaus_widget_set_quad(g->exposure_boost, self, dtgtk_cairo_paint_wand, FALSE, auto_adjust_exposure_boost,
                             _("auto-adjust the average exposure"));

  g->contrast_boost = dt_bauhaus_slider_from_params(self, "contrast_boost");
  dt_bauhaus_slider_set_soft_range(g->contrast_boost, -2.0, 2.0);
  dt_bauhaus_slider_set_format(g->contrast_boost, _(" EV"));
  gtk_widget_set_tooltip_text
    (g->contrast_boost,
     _("use this to counter the averaging effect of the guided filter\n"
       "and dilate the mask contrast around -4EV\n"
       "this allows to spread the exposure histogram over more NUM_SLIDERS\n"
       "for a better control of the exposure correction."));
  dt_bauhaus_widget_set_quad(g->contrast_boost, self, dtgtk_cairo_paint_wand, FALSE, auto_adjust_contrast_boost,
                             _("auto-adjust the contrast"));

  GtkWidget *histo_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_start(GTK_BOX(histo_box),
                    dt_ui_label_new(_("show image histogram in graph")), TRUE, TRUE, 0);
  g->show_two_histograms = dt_iop_togglebutton_new
    (self, NULL,
    N_("display the image histogram together with mask histogram"), NULL, G_CALLBACK(show_two_histograms_callback),
    FALSE, 0, 0, dtgtk_cairo_paint_showmask, histo_box);
  dt_gui_add_class(g->show_two_histograms, "dt_transparent_background");
  dtgtk_togglebutton_set_paint(DTGTK_TOGGLEBUTTON(g->show_two_histograms),
                              dtgtk_cairo_paint_showmask, 0, NULL);
  dt_gui_add_class(g->show_two_histograms, "dt_bauhaus_alignment");
  gtk_box_pack_start(GTK_BOX(self->widget), histo_box, FALSE, FALSE, 0);


  // start building top level widget
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  const int active_page = dt_conf_get_int("plugins/darkroom/toneequal/gui_page");
  gtk_widget_show(gtk_notebook_get_nth_page(g->notebook, active_page));
  gtk_notebook_set_current_page(g->notebook, active_page);

  g_signal_connect(G_OBJECT(g->notebook), "button-press-event",
                   G_CALLBACK(notebook_button_press), self);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->notebook), FALSE, FALSE, 0);

  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_start(GTK_BOX(hbox),
                     dt_ui_label_new(_("display exposure mask")), TRUE, TRUE, 0);
  g->show_luminance_mask = dt_iop_togglebutton_new
    (self, NULL,
     N_("display exposure mask"), NULL, G_CALLBACK(show_luminance_mask_callback),
     FALSE, 0, 0, dtgtk_cairo_paint_showmask, hbox);
  dt_gui_add_class(g->show_luminance_mask, "dt_transparent_background");
  dtgtk_togglebutton_set_paint(DTGTK_TOGGLEBUTTON(g->show_luminance_mask),
                               dtgtk_cairo_paint_showmask, 0, NULL);
  dt_gui_add_class(g->show_luminance_mask, "dt_bauhaus_alignment");
  gtk_box_pack_start(GTK_BOX(self->widget), hbox, FALSE, FALSE, 0);

  // Force UI redraws when pipe starts/finishes computing and switch cursors
  DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_DEVELOP_PREVIEW_PIPE_FINISHED, _develop_preview_pipe_finished_callback);
  DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_DEVELOP_UI_PIPE_FINISHED, _develop_ui_pipe_finished_callback);
  DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_DEVELOP_HISTORY_CHANGE, _develop_ui_pipe_started_callback);
}


void gui_cleanup(dt_iop_module_t *self)
{
  dt_iop_toneequalizer_gui_data_t *g = self->gui_data;
  self->request_color_pick = DT_REQUEST_COLORPICK_OFF;

  dt_conf_set_int("plugins/darkroom/toneequal/gui_page",
                  gtk_notebook_get_current_page (g->notebook));

  dt_free_align(g->preview_buf);
  dt_free_align(g->full_buf);

  if(g->desc)   pango_font_description_free(g->desc);
  if(g->layout) g_object_unref(g->layout);
  if(g->cr)     cairo_destroy(g->cr);
  if(g->cst)    cairo_surface_destroy(g->cst);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
