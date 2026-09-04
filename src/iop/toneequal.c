/*
    This file is part of darktable,
    Copyright (C) 2018-2026 darktable developers.

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
#include "common/gdk_event_utils.h"

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
 * has no theoretical background.  The geometric mean also display
 * interesting properties as it interprets saturated colours as
 * low-lights, allowing to lighten and desaturate them in a realistic
 * way.
 *
 * The exposure correction is computed as a series of each octave's
 * gain weighted by the gaussian of the radial distance between the
 * current pixel exposure and each octave's center.  This allows for a
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
 * optimize the luminance mask, performing histogram analyse, mapping
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

#include "bauhaus/bauhaus.h"
#include "common/darktable.h"
#include "common/bilinear.h"
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
#include "develop/preview_data.h"
#include "develop/tiling.h"
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


#define UI_SAMPLES 256 // 128 is a bit small for 4K resolution
#define CONTRAST_FULCRUM exp2f(-4.0f)
#define MIN_FLOAT exp2f(-16.0f)

/**
 * Build the exposures octaves :
 * band-pass filters with gaussian windows spaced by 1 EV
**/

#define CHANNELS 9
#define PIXEL_CHAN 8
#define LUT_RESOLUTION 10000

// radial distances used for pixel ops
static const float centers_ops[PIXEL_CHAN] DT_ALIGNED_ARRAY =
  {-56.0f / 7.0f, // = -8.0f
   -48.0f / 7.0f,
   -40.0f / 7.0f,
   -32.0f / 7.0f,
   -24.0f / 7.0f,
   -16.0f / 7.0f,
   -8.0f / 7.0f,
   0.0f / 7.0f}; // split 8 EV into 7 evenly-spaced channels

static const float centers_params[CHANNELS] DT_ALIGNED_ARRAY =
  { -8.0f, -7.0f, -6.0f, -5.0f,
    -4.0f, -3.0f, -2.0f, -1.0f, 0.0f};


typedef enum dt_iop_toneequalizer_filter_t
{
  DT_TONEEQ_NONE = 0,   // $DESCRIPTION: "no"
  DT_TONEEQ_AVG_GUIDED, // $DESCRIPTION: "averaged guided filter"
  DT_TONEEQ_GUIDED,     // $DESCRIPTION: "guided filter"
  DT_TONEEQ_AVG_EIGF,   // $DESCRIPTION: "averaged EIGF"
  DT_TONEEQ_EIGF        // $DESCRIPTION: "EIGF"
} dt_iop_toneequalizer_filter_t;


typedef struct dt_iop_toneequalizer_params_t
{
  float noise; // $MIN: -2.0 $MAX: 2.0 $DEFAULT: 0.0  $DESCRIPTION: "blacks"
  float ultra_deep_blacks; // $MIN: -2.0 $MAX: 2.0 $DEFAULT: 0.0  $DESCRIPTION: "deep shadows"
  float deep_blacks; // $MIN: -2.0 $MAX: 2.0 $DEFAULT: 0.0  $DESCRIPTION: "shadows"
  float blacks; // $MIN: -2.0 $MAX: 2.0 $DEFAULT: 0.0  $DESCRIPTION: "light shadows"
  float shadows; // $MIN: -2.0 $MAX: 2.0 $DEFAULT: 0.0  $DESCRIPTION: "mid-tones"
  float midtones; // $MIN: -2.0 $MAX: 2.0 $DEFAULT: 0.0  $DESCRIPTION: "dark highlights"
  float highlights; // $MIN: -2.0 $MAX: 2.0 $DEFAULT: 0.0  $DESCRIPTION: "highlights"
  float whites; // $MIN: -2.0 $MAX: 2.0 $DEFAULT: 0.0  $DESCRIPTION: "whites"
  float speculars; // $MIN: -2.0 $MAX: 2.0 $DEFAULT: 0.0  $DESCRIPTION: "speculars"
  float blending; // $MIN: 0.01 $MAX: 100.0 $DEFAULT: 5.0 $DESCRIPTION: "smoothing diameter"
  float smoothing; // $DEFAULT: 1.414213562 sqrtf(2.0f)
  float feathering; // $MIN: 0.01 $MAX: 10000.0 $DEFAULT: 1.0 $DESCRIPTION: "edges refinement/feathering"
  float quantization; // $MIN: 0.0 $MAX: 2.0 $DEFAULT: 0.0 $DESCRIPTION: "mask quantization"
  float contrast_boost; // $MIN: -16.0 $MAX: 16.0 $DEFAULT: 0.0 $DESCRIPTION: "mask contrast compensation"
  float exposure_boost; // $MIN: -16.0 $MAX: 16.0 $DEFAULT: 0.0 $DESCRIPTION: "mask exposure compensation"
  dt_iop_toneequalizer_filter_t details; // $DEFAULT: DT_TONEEQ_EIGF $DESCRIPTION: "preserve details"
  dt_iop_luminance_mask_method_t method; // $DEFAULT: DT_TONEEQ_NORM_2 $DESCRIPTION: "luminance estimator"
  int iterations; // $MIN: 1 $MAX: 20 $DEFAULT: 1 $DESCRIPTION: "filter diffusion"
} dt_iop_toneequalizer_params_t;


typedef struct dt_iop_toneequalizer_data_t
{
  float factors[PIXEL_CHAN] DT_ALIGNED_ARRAY;
  float correction_lut[PIXEL_CHAN * LUT_RESOLUTION + 1] DT_ALIGNED_ARRAY;
  float blending, feathering, contrast_boost, exposure_boost, quantization, smoothing;
  float scale;
  int radius;
  int iterations;
  dt_iop_luminance_mask_method_t method;
  dt_iop_toneequalizer_filter_t details;
} dt_iop_toneequalizer_data_t;


typedef struct dt_iop_toneequalizer_global_data_t
{
  int kernel_toneequal_luminance_mask;
  int kernel_toneequal_apply;
  int kernel_toneequal_display_mask;
  int kernel_toneequal_quantize;
  int kernel_toneequal_box_mean_x_2c;
  int kernel_toneequal_box_mean_y_2c;
  int kernel_toneequal_box_mean_x_4c;
  int kernel_toneequal_box_mean_y_4c;
  int kernel_toneequal_gf_pack;
  int kernel_toneequal_gf_ab;
  int kernel_toneequal_gf_blend;
  int kernel_toneequal_eigf_pack_4c;
  int kernel_toneequal_eigf_finish_4c;
  int kernel_toneequal_eigf_pack_2c;
  int kernel_toneequal_eigf_finish_2c;
  int kernel_toneequal_eigf_blend;
  int kernel_toneequal_eigf_blend_no_mask;
} dt_iop_toneequalizer_global_data_t;


typedef struct dt_iop_toneequalizer_gui_data_t
{
  // Mem arrays 64-bytes aligned - contiguous memory
  float factors[PIXEL_CHAN] DT_ALIGNED_ARRAY;
  float gui_lut[UI_SAMPLES] DT_ALIGNED_ARRAY; // LUT for the UI graph
  float interpolation_matrix[CHANNELS * PIXEL_CHAN] DT_ALIGNED_ARRAY;
  int histogram[UI_SAMPLES] DT_ALIGNED_ARRAY; // histogram for the UI graph
  float temp_user_params[CHANNELS] DT_ALIGNED_ARRAY;
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
  dt_hash_t ui_preview_hash;
  size_t full_preview_buf_width, full_preview_buf_height;

  // shared preview pipe under-cursor data (buffer + freshness hash)
  dt_preview_data_t pd;

  // Misc stuff, contiguity, length and alignment unknown
  float scale;
  float sigma;
  float histogram_average;
  float histogram_first_decile;
  float histogram_last_decile;

  // Heap arrays, 64 bits-aligned, unknown length
  float *full_preview_buf;

  // GTK garbage, nobody cares, no SIMD here
  GtkWidget *noise, *ultra_deep_blacks, *deep_blacks, *blacks, *shadows, *midtones, *highlights, *whites, *speculars;
  GtkDrawingArea *area;
  GtkWidget *blending, *smoothing, *quantization;
  GtkWidget *method;
  GtkWidget *details, *feathering, *contrast_boost, *iterations, *exposure_boost;
  GtkNotebook *notebook;
  GtkWidget *show_luminance_mask;

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
  float nodes_x[CHANNELS] DT_ALIGNED_ARRAY;
  float nodes_y[CHANNELS] DT_ALIGNED_ARRAY;
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
  gboolean interpolation_valid; // TRUE if the interpolation_matrix is ready
  gboolean luminance_valid;     // TRUE if the luminance cache is ready
  gboolean histogram_valid;     // TRUE if the histogram cache and stats are ready
  gboolean lut_valid;           // TRUE if the gui_lut is ready
  gboolean graph_valid;         // TRUE if the UI graph view is ready
  gboolean user_param_valid;    // TRUE if users params set in
                                // interactive view are in bounds
  gboolean factors_valid;       // TRUE if radial-basis coeffs are ready

  gboolean distort_signal_actif;
} dt_iop_toneequalizer_gui_data_t;

/* the signal DT_SIGNAL_DEVELOP_DISTORT is used to refresh the internal
   cached image buffer used for the on-canvas luminance picker. */
static void _set_distort_signal(dt_iop_module_t *self);
static void _unset_distort_signal(dt_iop_module_t *self);

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
  typedef struct dt_iop_toneequalizer_params_v2_t
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
    dt_iop_toneequalizer_filter_t details;
    dt_iop_luminance_mask_method_t method;
    int iterations;
  } dt_iop_toneequalizer_params_v2_t;

  if(old_version == 1)
  {
    typedef struct dt_iop_toneequalizer_params_v1_t
    {
      float noise, ultra_deep_blacks, deep_blacks, blacks;
      float shadows, midtones, highlights, whites, speculars;
      float blending, feathering, contrast_boost, exposure_boost;
      dt_iop_toneequalizer_filter_t details;
      int iterations;
      dt_iop_luminance_mask_method_t method;
    } dt_iop_toneequalizer_params_v1_t;

    const dt_iop_toneequalizer_params_v1_t *o = old_params;
    dt_iop_toneequalizer_params_v2_t *n = malloc(sizeof(dt_iop_toneequalizer_params_v2_t));

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

    n->details = o->details;
    n->iterations = o->iterations;
    n->method = o->method;

    // New params
    n->quantization = 0.0f;
    n->smoothing = M_SQRT2_F;

    *new_params = n;
    *new_params_size = sizeof(dt_iop_toneequalizer_params_v2_t);
    *new_version = 2;
    return 0;
  }
  return 1;
}

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

  p.method = DT_TONEEQ_NORM_POWER;
  p.contrast_boost = 0.0f;
  p.details = DT_TONEEQ_NONE;
  p.exposure_boost = -0.5f;
  p.feathering = 1.0f;
  p.iterations = 1;
  p.smoothing = M_SQRT2_F;
  p.quantization = 0.0f;

  // Init exposure settings
  p.noise = p.ultra_deep_blacks = p.deep_blacks = p.blacks = 0.0f;
  p.shadows = p.midtones = p.highlights = p.whites = p. speculars = 0.0f;

  // No blending
  dt_gui_presets_add_generic
    (_("simple tone curve"), self->op,
     self->version(), &p, sizeof(p), TRUE, DEVELOP_BLEND_CS_RGB_SCENE);

  // Simple utils blendings
  p.details = DT_TONEEQ_EIGF;
  p.method = DT_TONEEQ_NORM_2;

  p.blending = 5.0f;
  p.feathering = 1.0f;
  p.iterations = 1;
  p.quantization = 0.0f;
  p.exposure_boost = 0.0f;
  p.contrast_boost = 0.0f;
  dt_gui_presets_add_generic
    (_("mask blending | all purposes"), self->op,
     self->version(), &p, sizeof(p), TRUE, DEVELOP_BLEND_CS_RGB_SCENE);

  p.blending = 1.0f;
  p.feathering = 10.0f;
  p.iterations = 3;
  dt_gui_presets_add_generic
    (_("mask blending | people with backlight"), self->op,
     self->version(), &p, sizeof(p), TRUE, DEVELOP_BLEND_CS_RGB_SCENE);

  // Shadows/highlights presets
  // move middle-grey to the center of the range
  p.exposure_boost = -1.57f;
  p.contrast_boost = 0.0f;
  p.blending = 2.0f;
  p.feathering = 50.0f;
  p.iterations = 5;
  p.quantization = 0.0f;

  // slight modification to give higher compression
  p.details = DT_TONEEQ_EIGF;
  p.feathering = 20.0f;
  compress_shadows_highlight_preset_set_exposure_params(&p, 0.65f);
  dt_gui_presets_add_generic
    (_("compress shadows/highlights | EIGF | strong"), self->op,
     self->version(), &p, sizeof(p), TRUE, DEVELOP_BLEND_CS_RGB_SCENE);
  p.details = DT_TONEEQ_GUIDED;
  p.feathering = 500.0f;
  dt_gui_presets_add_generic
    (_("compress shadows/highlights | GF | strong"), self->op,
     self->version(), &p, sizeof(p), TRUE, DEVELOP_BLEND_CS_RGB_SCENE);

  p.details = DT_TONEEQ_EIGF;
  p.blending = 3.0f;
  p.feathering = 7.0f;
  p.iterations = 3;
  compress_shadows_highlight_preset_set_exposure_params(&p, 0.45f);
  dt_gui_presets_add_generic
    (_("compress shadows/highlights | EIGF | medium"), self->op,
     self->version(), &p, sizeof(p), TRUE, DEVELOP_BLEND_CS_RGB_SCENE);
  p.details = DT_TONEEQ_GUIDED;
  p.feathering = 500.0f;
  dt_gui_presets_add_generic
    (_("compress shadows/highlights | GF | medium"), self->op,
     self->version(), &p, sizeof(p), TRUE, DEVELOP_BLEND_CS_RGB_SCENE);

  p.details = DT_TONEEQ_EIGF;
  p.blending = 5.0f;
  p.feathering = 1.0f;
  p.iterations = 1;
  compress_shadows_highlight_preset_set_exposure_params(&p, 0.25f);
  dt_gui_presets_add_generic
    (_("compress shadows/highlights | EIGF | soft"), self->op,
     self->version(), &p, sizeof(p), TRUE, DEVELOP_BLEND_CS_RGB_SCENE);
  p.details = DT_TONEEQ_GUIDED;
  p.feathering = 500.0f;
  dt_gui_presets_add_generic
    (_("compress shadows/highlights | GF | soft"), self->op,
     self->version(), &p, sizeof(p), TRUE, DEVELOP_BLEND_CS_RGB_SCENE);

  // build the 1D contrast curves that revert the local compression of
  // contrast above
  p.details = DT_TONEEQ_NONE;
  dilate_shadows_highlight_preset_set_exposure_params(&p, 0.25f);
  dt_gui_presets_add_generic
    (_("contrast tone curve | soft"), self->op,
     self->version(), &p, sizeof(p), TRUE, DEVELOP_BLEND_CS_RGB_SCENE);

  dilate_shadows_highlight_preset_set_exposure_params(&p, 0.45f);
  dt_gui_presets_add_generic
    (_("contrast tone curve | medium"), self->op,
     self->version(), &p, sizeof(p), TRUE, DEVELOP_BLEND_CS_RGB_SCENE);

  dilate_shadows_highlight_preset_set_exposure_params(&p, 0.65f);
  dt_gui_presets_add_generic
    (_("contrast tone curve | strong"), self->op,
     self->version(), &p, sizeof(p), TRUE, DEVELOP_BLEND_CS_RGB_SCENE);

  // relight
  p.details = DT_TONEEQ_EIGF;
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
     self->version(), &p, sizeof(p), TRUE, DEVELOP_BLEND_CS_RGB_SCENE);
}


/**
 * Helper functions
 **/

static gboolean in_mask_editing(const dt_iop_module_t *self)
{
  const dt_develop_t *dev = self->dev;
  return dev->form_gui && dev->form_visible;
}

static void hash_set_get(const dt_hash_t *hash_in,
                         dt_hash_t *hash_out,
                         dt_pthread_mutex_t *lock)
{
  // Set or get a hash in a struct the thread-safe way
  dt_pthread_mutex_lock(lock);
  *hash_out = *hash_in;
  dt_pthread_mutex_unlock(lock);
}


static dt_hash_t _luminance_mask_hash(dt_dev_pixelpipe_iop_t *piece,
                                      const dt_iop_roi_t *const roi_out)
{
  // Freshness key of the cached luminance mask.
  //
  // include = TRUE hashes nodes[0 .. position-1], i.e. our own params too, so
  // a band slider drag recomputed the whole mask (~60 ms/Mpix) and re-copied
  // it from the device.  The mask ignores the band factors and `smoothing`:
  // hash the upstream pipe (include = FALSE, roi folded in) plus the params
  // compute_luminance_mask() reads, the invalidate list of gui_changed().
  const dt_iop_toneequalizer_data_t *const d = piece->data;

  const float mask_floats[] = { d->blending, d->feathering, d->contrast_boost,
                                d->exposure_boost, d->quantization, d->scale };
  const int mask_ints[] = { d->radius, d->iterations,
                            (int)d->method, (int)d->details };

  dt_hash_t hash = dt_dev_pixelpipe_piece_hash(piece, roi_out, FALSE);
  hash = dt_hash(hash, mask_floats, sizeof(mask_floats));
  hash = dt_hash(hash, mask_ints, sizeof(mask_ints));
  return hash;
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
  g->ui_preview_hash = DT_INVALID_HASH;
  dt_iop_gui_leave_critical_section(self);
  dt_preview_data_invalidate(&g->pd);
  dt_iop_refresh_all(self);
}

static void _toneeq_preview_resized(void *const user_data)
{
  // Called under the module GUI lock when the preview buffer has been
  // reallocated: don't let the GUI read it before it has been recomputed.
  dt_iop_module_t *const self = (dt_iop_module_t *)user_data;
  dt_iop_toneequalizer_gui_data_t *const g = self->gui_data;
  if(g) g->luminance_valid = FALSE;
}

// gaussian-ish kernel - sum is == 1.0f so we don't care much about actual coeffs
static const dt_colormatrix_t gauss_kernel =
  { { 0.076555024f, 0.124401914f, 0.076555024f },
    { 0.124401914f, 0.196172249f, 0.124401914f },
    { 0.076555024f, 0.124401914f, 0.076555024f } };

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

static void _get_point(const dt_iop_module_t *self,
                       const int c_x,
                       const int c_y,
                       int *x,
                       int *y)
{
  // TODO: For this to fully work non depending on the place of the module
  //       in the pipe we need a dt_dev_distort_backtransform_plus that
  //       can skip crop only. With the current version if toneequalizer
  //       is moved below rotation & perspective it will fail as we are
  //       then missing all the transform after tone-eq.
  const double crop_order =
    dt_ioppr_get_iop_order(self->dev->iop_order_list, "crop", 0);

  float pts[2] = { c_x, c_y };

  // only a forward backtransform as the buffer already contains all the transforms
  // done before toneequal and we are speaking of on-screen cursor coordinates.
  // also we do transform only after crop as crop does change roi for the whole pipe
  // and so it is already part of the preview buffer cached in this implementation.
  dt_dev_distort_backtransform_plus(darktable.develop, darktable.develop->preview_pipe,
                                    crop_order,
                                    DT_DEV_TRANSFORM_DIR_FORW_EXCL, pts, 1);
  *x = pts[0];
  *y = pts[1];
}

static float _luminance_from_module_buffer(const dt_iop_module_t *self)
{
  const dt_iop_toneequalizer_gui_data_t *g = self->gui_data;

  const size_t c_x = g->cursor_pos_x;
  const size_t c_y = g->cursor_pos_y;

  // get buffer x,y given the cursor position
  int b_x = 0;
  int b_y = 0;

  _get_point(self, c_x, c_y, &b_x, &b_y);

  return get_luminance_from_buffer(g->pd.buf,
                                   g->pd.width,
                                   g->pd.height,
                                   b_x,
                                   b_y);
}

/***
 * Exposure compensation computation
 *
 * Construct the final correction factor by summing the octaves
 * channels gains weighted by the gaussian of the radial distance
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

#define DT_TONEEQ_MIN_EV (-8.0f)
#define DT_TONEEQ_MAX_EV (0.0f)
#define DT_TONEEQ_USE_LUT TRUE
#if DT_TONEEQ_USE_LUT

// this is the version currently used, as using a lut gives a
// big performance speedup on some cpus
__DT_CLONE_TARGETS__
static inline void apply_toneequalizer(const float *const restrict in,
                                       const float *const restrict luminance,
                                       float *const restrict out,
                                       const dt_iop_roi_t *const roi_in,
                                       const dt_iop_roi_t *const roi_out,
                                       const dt_iop_toneequalizer_data_t *const d)
{
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

// we keep this version for further reference; it is the one the OpenCL
// path implements, as evaluating the gaussians is cheaper on a GPU than
// uploading the correction lut
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

    DT_OMP_SIMD(aligned(luminance, centers_ops, factors:64) safelen(PIXEL_CHAN) reduction(+:result))
    for(int i = 0; i < PIXEL_CHAN; ++i)
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
static inline float pixel_correction(const float exposure,
                                     const float *const restrict factors,
                                     const float sigma)
{
  // build the correction for the current pixel
  // as the sum of the contribution of each luminance channel
  float result = 0.0f;
  const float gauss_denom = gaussian_denom(sigma);
  const float expo = fast_clamp(exposure, DT_TONEEQ_MIN_EV, DT_TONEEQ_MAX_EV);

  DT_OMP_SIMD(aligned(centers_ops, factors:64) safelen(PIXEL_CHAN) reduction(+:result))
  for(int i = 0; i < PIXEL_CHAN; ++i)
    result += gaussian_func(expo - centers_ops[i], gauss_denom) * factors[i];

  return fast_clamp(result, 0.25f, 4.0f);
}


__DT_CLONE_TARGETS__
static inline void compute_luminance_mask(const float *const restrict in,
                                          float *const restrict luminance,
                                          const size_t width,
                                          const size_t height,
                                          const dt_iop_toneequalizer_data_t *const d)
{
  switch(d->details)
  {
    case(DT_TONEEQ_NONE):
    {
      // No contrast boost here
      luminance_mask(in, luminance, width, height,
                     d->method, d->exposure_boost, 0.0f, 1.0f);
      break;
    }

    case(DT_TONEEQ_AVG_GUIDED):
    {
      // Still no contrast boost
      luminance_mask(in, luminance, width, height,
                     d->method, d->exposure_boost, 0.0f, 1.0f);
      fast_surface_blur(luminance, width, height, d->radius, d->feathering, d->iterations,
                        DT_GF_BLENDING_GEOMEAN, d->scale, d->quantization,
                        exp2f(-14.0f), 4.0f);
      break;
    }

    case(DT_TONEEQ_GUIDED):
    {
      // Contrast boosting is done around the average luminance of the mask.
      // This is to make exposure corrections easier to control for users, by spreading
      // the dynamic range along all exposure channels, because guided filters
      // tend to flatten the luminance mask a lot around an average ± 2 EV
      // which makes only 2-3 channels usable.
      // we assume the distribution is centered around -4EV, e.g. the center of the nodes
      // the exposure boost should be used to make this assumption true
      luminance_mask(in, luminance, width, height, d->method, d->exposure_boost,
                     CONTRAST_FULCRUM, d->contrast_boost);
      fast_surface_blur(luminance, width, height, d->radius, d->feathering, d->iterations,
                        DT_GF_BLENDING_LINEAR, d->scale, d->quantization,
                        exp2f(-14.0f), 4.0f);
      break;
    }

    case(DT_TONEEQ_AVG_EIGF):
    {
      // Still no contrast boost
      luminance_mask(in, luminance, width, height,
                     d->method, d->exposure_boost, 0.0f, 1.0f);
      fast_eigf_surface_blur(luminance, width, height,
                             d->radius, d->feathering, d->iterations,
                             DT_GF_BLENDING_GEOMEAN, d->scale,
                             d->quantization, exp2f(-14.0f), 4.0f);
      break;
    }

    case(DT_TONEEQ_EIGF):
    {
      luminance_mask(in, luminance, width, height, d->method, d->exposure_boost,
                     CONTRAST_FULCRUM, d->contrast_boost);
      fast_eigf_surface_blur(luminance, width, height,
                             d->radius, d->feathering, d->iterations,
                             DT_GF_BLENDING_LINEAR, d->scale,
                             d->quantization, exp2f(-14.0f), 4.0f);
      break;
    }

    default:
    {
      luminance_mask(in, luminance, width, height,
                     d->method, d->exposure_boost, 0.0f, 1.0f);
      break;
    }
  }
}

/***
 * Actual transfer functions
 **/

__DT_CLONE_TARGETS__
static inline void display_luminance_mask(const float *const restrict in,
                                          const float *const restrict luminance,
                                          float *const restrict out,
                                          const dt_iop_roi_t *const roi_in,
                                          const dt_iop_roi_t *const roi_out)
{
  const size_t offset_x = (roi_in->x < roi_out->x) ? -roi_in->x + roi_out->x : 0;
  const size_t offset_y = (roi_in->y < roi_out->y) ? -roi_in->y + roi_out->y : 0;

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
      const float intensity =
        sqrtf(fminf(
                fmaxf(luminance[(i + offset_y) * in_width  + (j + offset_x)] - 0.00390625f,
                      0.f) / 0.99609375f,
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


__DT_CLONE_TARGETS__
static
void toneeq_process(dt_iop_module_t *self,
                    dt_dev_pixelpipe_iop_t *piece,
                    const void *const restrict ivoid,
                    void *const restrict ovoid,
                    const dt_iop_roi_t *const roi_in,
                    const dt_iop_roi_t *const roi_out)
{
  const dt_iop_toneequalizer_data_t *const d = piece->data;
  dt_iop_toneequalizer_gui_data_t *const g = self->gui_data;

  const float *const restrict in = (float *const)ivoid;
  float *const restrict out = (float *const)ovoid;
  float *restrict luminance = NULL;

  const size_t width = roi_in->width;
  const size_t height = roi_in->height;
  const size_t num_elem = width * height;

  // Freshness key of the luminance mask cache
  const dt_hash_t hash = _luminance_mask_hash(piece, roi_out);

  // Sanity checks
  if(width < 1 || height < 1) return;
  if(roi_in->width < roi_out->width || roi_in->height < roi_out->height)
    return; // input should be at least as large as output
  if(piece->colors != 4) return;  // we need RGB signal

  // Init the luminance masks buffers
  gboolean cached = FALSE;

  if(self->dev->gui_attached)
  {
    // If the module instance has changed order in the pipe, invalidate the caches
    if(g->pipe_order != piece->module->iop_order)
    {
      dt_iop_gui_enter_critical_section(self);
      g->ui_preview_hash = DT_INVALID_HASH;
      g->pipe_order = piece->module->iop_order;
      g->luminance_valid = FALSE;
      g->histogram_valid = FALSE;
      dt_iop_gui_leave_critical_section(self);
      dt_preview_data_invalidate(&g->pd);
    }

    if(dt_pipe_is_full(piece->pipe))
    {
      // For DT_DEV_PIXELPIPE_FULL, we cache the luminance mask for performance
      // but it's not accessed from GUI
      // no need for threads lock since no other function is writing/reading that buffer

      // Re-allocate a new buffer if the full preview size has changed
      if(g->full_preview_buf_width != width || g->full_preview_buf_height != height)
      {
        dt_free_align(g->full_preview_buf);
        g->full_preview_buf = dt_alloc_align_float(num_elem);
        g->full_preview_buf_width = width;
        g->full_preview_buf_height = height;
      }

      luminance = g->full_preview_buf;
      cached = TRUE;
    }
    else if(dt_pipe_is_preview(piece->pipe))
    {
      // For preview pipe we need to cache it too because we have to
      // compute the full image stats upon user request in GUI threads.
      // The shared under-cursor service owns the buffer and its locks.
      // The resize and the luminance_valid invalidation happen under one
      // GUI lock so the GUI never reads a resized, not-yet-recomputed buffer.
      luminance = dt_preview_data_resize(&g->pd, width, height, _toneeq_preview_resized, self);
      cached = TRUE;
    }
    else // just to please GCC
    {
      luminance = dt_alloc_align_float(num_elem);
    }

  }
  else
  {
    // no interactive editing/caching : just allocate a local temp buffer
    luminance = dt_alloc_align_float(num_elem);
  }

  // Check if the luminance buffer exists
  if(!luminance)
  {
    dt_control_log(_("tone equalizer failed to allocate memory, check your RAM settings"));
    return;
  }

  // Compute the luminance mask
  if(cached)
  {
    // caching path : store the luminance mask for GUI access

    if(dt_pipe_is_full(piece->pipe))
    {
      dt_hash_t saved_hash;
      hash_set_get(&g->ui_preview_hash, &saved_hash, &self->gui_lock);

      dt_iop_gui_enter_critical_section(self);
      const gboolean luminance_valid = g->luminance_valid;
      dt_iop_gui_leave_critical_section(self);

      if(hash != saved_hash || !luminance_valid)
      {
        /* compute only if upstream pipe state has changed */
        compute_luminance_mask(in, luminance, width, height, d);
        hash_set_get(&hash, &g->ui_preview_hash, &self->gui_lock);
      }
    }
    else if(dt_pipe_is_preview(piece->pipe))
    {
      const dt_hash_t saved_hash = dt_preview_data_get_hash(&g->pd);

      dt_iop_gui_enter_critical_section(self);
      const gboolean luminance_valid = g->luminance_valid;
      dt_iop_gui_leave_critical_section(self);

      if(saved_hash != hash || !luminance_valid)
      {
        /* compute only if upstream pipe state has changed */
        // Flag the cache as being recomputed so the GUI threads never
        // read a partially filled buffer, then commit hash + validity
        // once the data is ready.
        dt_iop_gui_enter_critical_section(self);
        g->histogram_valid = FALSE;
        g->luminance_valid = FALSE;
        dt_iop_gui_leave_critical_section(self);

        compute_luminance_mask(in, luminance, width, height, d);
        dt_preview_data_set_hash_value(&g->pd, hash);

        dt_iop_gui_enter_critical_section(self);
        g->luminance_valid = TRUE;
        dt_iop_gui_leave_critical_section(self);
        dt_dev_pixelpipe_cache_invalidate_later(piece->pipe, self->iop_order, "toneequal: ");
      }
    }
    else // make it dummy-proof
    {
      compute_luminance_mask(in, luminance, width, height, d);
    }
  }
  else
  {
    // no caching path : compute no matter what
    compute_luminance_mask(in, luminance, width, height, d);
  }

  // Display output
  if(self->dev->gui_attached && dt_pipe_is_full(piece->pipe))
  {
    if(g->mask_display)
    {
      display_luminance_mask(in, luminance, out, roi_in, roi_out);
      piece->pipe->mask_display = DT_DEV_PIXELPIPE_DISPLAY_PASSTHRU;
    }
    else
      apply_toneequalizer(in, luminance, out, roi_in, roi_out, d);
  }
  else
  {
    apply_toneequalizer(in, luminance, out, roi_in, roi_out, d);
  }

  if(!cached) dt_free_align(luminance);
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


#ifdef HAVE_OPENCL

/***
 * OpenCL implementation
 *
 * The GPU path mirrors the CPU code above: extract the luminance mask from the
 * input, optionally refine it with the (exposure independent) guided filter,
 * then correct each pixel exposure from its masked luminance.
 *
 * Every intermediate buffer is a grey image, so we use device buffers rather
 * than images and keep the packing conventions of the CPU code (arrays of 2 or
 * 4 channel structs) to average several terms in a single pass.
 ***/

static cl_int _box_mean_cl(const int devid,
                           const dt_iop_toneequalizer_global_data_t *const gd,
                           cl_mem dev_buf,
                           cl_mem dev_tmp,
                           const int width,
                           const int height,
                           const int ch,
                           const int radius)
{
  // The moving average of the separable box filter reads samples that a
  // parallel in-place pass would already have overwritten, so we bounce
  // through dev_tmp and land back in dev_buf.
  const int kernel_x = (ch == 4)
    ? gd->kernel_toneequal_box_mean_x_4c
    : gd->kernel_toneequal_box_mean_x_2c;
  const int kernel_y = (ch == 4)
    ? gd->kernel_toneequal_box_mean_y_4c
    : gd->kernel_toneequal_box_mean_y_2c;

  const cl_int err = dt_opencl_enqueue_kernel_1d_args(devid, kernel_x, height,
            CLARG(dev_buf), CLARG(dev_tmp),
            CLARG(width), CLARG(height), CLARG(radius));
  if(err != CL_SUCCESS) return err;

  return dt_opencl_enqueue_kernel_1d_args(devid, kernel_y, width,
            CLARG(dev_tmp), CLARG(dev_buf),
            CLARG(width), CLARG(height), CLARG(radius));
}


static cl_int _quantize_cl(const int devid,
                           const dt_iop_toneequalizer_global_data_t *const gd,
                           cl_mem dev_in,
                           cl_mem dev_out,
                           const int width,
                           const int height,
                           const float sampling,
                           const float clip_min,
                           const float clip_max)
{
  return dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_toneequal_quantize,
            width, height,
            CLARG(dev_in), CLARG(dev_out), CLARG(width), CLARG(height),
            CLARG(sampling), CLARG(clip_min), CLARG(clip_max));
}


static cl_int _fast_surface_blur_cl(const int devid,
                                    const dt_iop_toneequalizer_global_data_t *const gd,
                                    cl_mem dev_image,
                                    const int width,
                                    const int height,
                                    const int radius,
                                    const float feathering,
                                    const int iterations,
                                    const dt_iop_guided_filter_blending_t filter,
                                    const float quantization,
                                    const float quantize_min,
                                    const float quantize_max)
{
  // Works in-place on a grey image, see fast_surface_blur()
  // in common/fast_guided_filter.h

  // A down-scaling of 4 seems empirically safe and consistent no matter the
  // image zoom level
  const float scaling = 4.0f;
  const int ds_radius = (radius < 4) ? 1 : (int)((float)radius / scaling);

  // the downscaled dimensions can round down to zero on thumbnails
  const int ds_width = MAX(1, (int)((float)width / scaling));
  const int ds_height = MAX(1, (int)((float)height / scaling));

  const size_t bsize = (size_t)width * height * sizeof(float);
  const size_t ds_bsize = (size_t)ds_width * ds_height * sizeof(float);

  cl_int err = CL_MEM_OBJECT_ALLOCATION_FAILURE;

  cl_mem dev_ab = dt_opencl_alloc_device_buffer(devid, 2 * bsize);
  cl_mem dev_ds_image = dt_opencl_alloc_device_buffer(devid, ds_bsize);
  cl_mem dev_ds_mask = dt_opencl_alloc_device_buffer(devid, ds_bsize);
  cl_mem dev_ds_ab = dt_opencl_alloc_device_buffer(devid, 2 * ds_bsize);
  // array of struct : { { guide, mask, guide * guide, guide * mask } }
  cl_mem dev_packed = dt_opencl_alloc_device_buffer(devid, 4 * ds_bsize);
  // scratch space of the box average, large enough for both channel counts
  cl_mem dev_tmp = dt_opencl_alloc_device_buffer(devid, 4 * ds_bsize);

  if(!dev_ab || !dev_ds_image || !dev_ds_mask || !dev_ds_ab || !dev_packed || !dev_tmp)
    goto error;

  // Downsample the image for speed-up
  err = dt_interpolate_bilinear_cl(devid, dev_image, width, height,
                                   dev_ds_image, ds_width, ds_height, 1);
  if(err != CL_SUCCESS) goto error;

  // Iterations of filter models the diffusion, sort of
  for(int i = 0; i < iterations; ++i)
  {
    // (Re)build the mask from the quantized image to help guiding
    err = _quantize_cl(devid, gd, dev_ds_image, dev_ds_mask, ds_width, ds_height,
                       quantization, quantize_min, quantize_max);
    if(err != CL_SUCCESS) goto error;

    // Perform the patch-wise variance analyse to get the a and b parameters
    // for the linear blending s.t. mask = a * I + b
    err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_toneequal_gf_pack,
            ds_width, ds_height,
            CLARG(dev_ds_mask), CLARG(dev_ds_image), CLARG(dev_packed),
            CLARG(ds_width), CLARG(ds_height));
    if(err != CL_SUCCESS) goto error;

    err = _box_mean_cl(devid, gd, dev_packed, dev_tmp,
                       ds_width, ds_height, 4, ds_radius);
    if(err != CL_SUCCESS) goto error;

    err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_toneequal_gf_ab,
            ds_width, ds_height,
            CLARG(dev_packed), CLARG(dev_ds_ab),
            CLARG(ds_width), CLARG(ds_height), CLARG(feathering));
    if(err != CL_SUCCESS) goto error;

    // Compute the patch-wise average of parameters a and b
    err = _box_mean_cl(devid, gd, dev_ds_ab, dev_tmp,
                       ds_width, ds_height, 2, ds_radius);
    if(err != CL_SUCCESS) goto error;

    if(i != iterations - 1)
    {
      // Process the intermediate filtered image
      const int blending = DT_GF_BLENDING_LINEAR;
      err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_toneequal_gf_blend,
              ds_width, ds_height,
              CLARG(dev_ds_image), CLARG(dev_ds_ab),
              CLARG(ds_width), CLARG(ds_height), CLARG(blending));
      if(err != CL_SUCCESS) goto error;
    }
  }

  // Upsample the blending parameters a and b
  err = dt_interpolate_bilinear_cl(devid, dev_ds_ab, ds_width, ds_height,
                                   dev_ab, width, height, 2);
  if(err != CL_SUCCESS) goto error;

  // Finally, blend the guided image
  {
    const int blending = filter;
    err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_toneequal_gf_blend,
            width, height,
            CLARG(dev_image), CLARG(dev_ab),
            CLARG(width), CLARG(height), CLARG(blending));
  }

error:
  dt_opencl_release_mem_object(dev_tmp);
  dt_opencl_release_mem_object(dev_packed);
  dt_opencl_release_mem_object(dev_ds_ab);
  dt_opencl_release_mem_object(dev_ds_mask);
  dt_opencl_release_mem_object(dev_ds_image);
  dt_opencl_release_mem_object(dev_ab);
  return err;
}


static cl_int _fast_eigf_surface_blur_cl(const int devid,
                                         const dt_iop_toneequalizer_global_data_t *const gd,
                                         cl_mem dev_image,
                                         const int width,
                                         const int height,
                                         const float sigma,
                                         const float feathering,
                                         const int iterations,
                                         const dt_iop_guided_filter_blending_t filter,
                                         const float quantization,
                                         const float quantize_min,
                                         const float quantize_max)
{
  // Works in-place on a grey image, see fast_eigf_surface_blur()
  // in common/eigf.h
  const float scaling = fmaxf(fminf(sigma, 4.0f), 1.0f);
  const float ds_sigma = fmaxf(sigma / scaling, 1.0f);

  // the downscaled dimensions can round down to zero on thumbnails
  const int ds_width = MAX(1, (int)((float)width / scaling));
  const int ds_height = MAX(1, (int)((float)height / scaling));

  const size_t bsize = (size_t)width * height * sizeof(float);
  const size_t ds_bsize = (size_t)ds_width * ds_height * sizeof(float);

  // without quantization the guide is the image itself, which halves the
  // number of terms to blur
  const gboolean use_mask = (quantization != 0.0f);
  const int ch = use_mask ? 4 : 2;

  cl_int err = CL_MEM_OBJECT_ALLOCATION_FAILURE;

  cl_mem dev_mask = use_mask
    ? dt_opencl_alloc_device_buffer(devid, bsize)
    : NULL;
  cl_mem dev_ds_mask = use_mask
    ? dt_opencl_alloc_device_buffer(devid, ds_bsize)
    : NULL;
  cl_mem dev_ds_image = dt_opencl_alloc_device_buffer(devid, ds_bsize);
  // average - variance arrays: store the guide and mask averages and variances
  cl_mem dev_ds_av = dt_opencl_alloc_device_buffer(devid, ch * ds_bsize);
  cl_mem dev_av = dt_opencl_alloc_device_buffer(devid, ch * bsize);

  if(!dev_ds_image || !dev_ds_av || !dev_av
     || (use_mask && (!dev_mask || !dev_ds_mask)))
    goto error;

  // Iterations of filter models the diffusion, sort of
  for(int i = 0; i < iterations; i++)
  {
    // blend linear for all intermediate images, use filter for last iteration
    const int blending = (i == iterations - 1) ? (int)filter : DT_GF_BLENDING_LINEAR;

    err = dt_interpolate_bilinear_cl(devid, dev_image, width, height,
                                     dev_ds_image, ds_width, ds_height, 1);
    if(err != CL_SUCCESS) goto error;

    if(use_mask)
    {
      // (Re)build the mask from the quantized image to help guiding
      err = _quantize_cl(devid, gd, dev_image, dev_mask, width, height,
                         quantization, quantize_min, quantize_max);
      if(err != CL_SUCCESS) goto error;

      // Downsample the mask for speed-up
      err = dt_interpolate_bilinear_cl(devid, dev_mask, width, height,
                                       dev_ds_mask, ds_width, ds_height, 1);
      if(err != CL_SUCCESS) goto error;

      err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_toneequal_eigf_pack_4c,
              ds_width, ds_height,
              CLARG(dev_ds_mask), CLARG(dev_ds_image), CLARG(dev_ds_av),
              CLARG(ds_width), CLARG(ds_height));
      if(err != CL_SUCCESS) goto error;

      err = dt_gaussian_mean_blur_cl(devid, dev_ds_av,
                                     ds_width, ds_height, 4, ds_sigma);
      if(err != CL_SUCCESS) goto error;

      err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_toneequal_eigf_finish_4c,
              ds_width, ds_height,
              CLARG(dev_ds_av), CLARG(ds_width), CLARG(ds_height));
      if(err != CL_SUCCESS) goto error;

      // Upsample the variances and averages
      err = dt_interpolate_bilinear_cl(devid, dev_ds_av, ds_width, ds_height,
                                       dev_av, width, height, 4);
      if(err != CL_SUCCESS) goto error;

      // Blend the guided image
      err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_toneequal_eigf_blend,
              width, height,
              CLARG(dev_image), CLARG(dev_mask), CLARG(dev_av),
              CLARG(width), CLARG(height), CLARG(blending), CLARG(feathering));
      if(err != CL_SUCCESS) goto error;
    }
    else
    {
      // no need to build a mask
      err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_toneequal_eigf_pack_2c,
              ds_width, ds_height,
              CLARG(dev_ds_image), CLARG(dev_ds_av),
              CLARG(ds_width), CLARG(ds_height));
      if(err != CL_SUCCESS) goto error;

      err = dt_gaussian_mean_blur_cl(devid, dev_ds_av,
                                     ds_width, ds_height, 2, ds_sigma);
      if(err != CL_SUCCESS) goto error;

      err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_toneequal_eigf_finish_2c,
              ds_width, ds_height,
              CLARG(dev_ds_av), CLARG(ds_width), CLARG(ds_height));
      if(err != CL_SUCCESS) goto error;

      // Upsample the variances and averages
      err = dt_interpolate_bilinear_cl(devid, dev_ds_av, ds_width, ds_height,
                                       dev_av, width, height, 2);
      if(err != CL_SUCCESS) goto error;

      // Blend the guided image
      err = dt_opencl_enqueue_kernel_2d_args
        (devid, gd->kernel_toneequal_eigf_blend_no_mask, width, height,
         CLARG(dev_image), CLARG(dev_av),
         CLARG(width), CLARG(height), CLARG(blending), CLARG(feathering));
      if(err != CL_SUCCESS) goto error;
    }
  }

error:
  dt_opencl_release_mem_object(dev_av);
  dt_opencl_release_mem_object(dev_ds_av);
  dt_opencl_release_mem_object(dev_ds_image);
  dt_opencl_release_mem_object(dev_ds_mask);
  dt_opencl_release_mem_object(dev_mask);
  return err;
}


static cl_int _compute_luminance_mask_cl(const int devid,
                                         const dt_iop_toneequalizer_global_data_t *const gd,
                                         cl_mem dev_in,
                                         cl_mem dev_luminance,
                                         const int width,
                                         const int height,
                                         const dt_iop_toneequalizer_data_t *const d)
{
  // Contrast boosting is done around the average luminance of the mask for the
  // plain filters only, see compute_luminance_mask() above
  const gboolean boost_contrast = (d->details == DT_TONEEQ_GUIDED)
                               || (d->details == DT_TONEEQ_EIGF);
  const int method = d->method;
  const float exposure_boost = d->exposure_boost;
  const float fulcrum = boost_contrast ? CONTRAST_FULCRUM : 0.0f;
  const float contrast_boost = boost_contrast ? d->contrast_boost : 1.0f;

  const cl_int err =
    dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_toneequal_luminance_mask,
            width, height,
            CLARG(dev_in), CLARG(dev_luminance), CLARG(width), CLARG(height),
            CLARG(method), CLARG(exposure_boost),
            CLARG(fulcrum), CLARG(contrast_boost));
  if(err != CL_SUCCESS) return err;

  switch(d->details)
  {
    case DT_TONEEQ_AVG_GUIDED:
      return _fast_surface_blur_cl(devid, gd, dev_luminance, width, height,
                                   d->radius, d->feathering, d->iterations,
                                   DT_GF_BLENDING_GEOMEAN, d->quantization,
                                   exp2f(-14.0f), 4.0f);

    case DT_TONEEQ_GUIDED:
      return _fast_surface_blur_cl(devid, gd, dev_luminance, width, height,
                                   d->radius, d->feathering, d->iterations,
                                   DT_GF_BLENDING_LINEAR, d->quantization,
                                   exp2f(-14.0f), 4.0f);

    case DT_TONEEQ_AVG_EIGF:
      return _fast_eigf_surface_blur_cl(devid, gd, dev_luminance, width, height,
                                        d->radius, d->feathering, d->iterations,
                                        DT_GF_BLENDING_GEOMEAN, d->quantization,
                                        exp2f(-14.0f), 4.0f);

    case DT_TONEEQ_EIGF:
      return _fast_eigf_surface_blur_cl(devid, gd, dev_luminance, width, height,
                                        d->radius, d->feathering, d->iterations,
                                        DT_GF_BLENDING_LINEAR, d->quantization,
                                        exp2f(-14.0f), 4.0f);

    case DT_TONEEQ_NONE:
    default:
      return CL_SUCCESS;
  }
}


int process_cl(dt_iop_module_t *self,
               dt_dev_pixelpipe_iop_t *piece,
               cl_mem dev_in,
               cl_mem dev_out,
               const dt_iop_roi_t *const roi_in,
               const dt_iop_roi_t *const roi_out)
{
  const dt_iop_toneequalizer_data_t *const d = piece->data;
  const dt_iop_toneequalizer_global_data_t *const gd = self->global_data;
  dt_iop_toneequalizer_gui_data_t *const g = self->gui_data;

  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;
  const size_t num_elem = (size_t)width * height;

  // Freshness key of the luminance mask cache
  const dt_hash_t hash = _luminance_mask_hash(piece, roi_out);

  // Sanity checks
  if(width < 1 || height < 1) return DT_OPENCL_PROCESS_CL;
  if(roi_in->width < roi_out->width || roi_in->height < roi_out->height)
    return DT_OPENCL_PROCESS_CL; // input should be at least as large as output
  if(piece->colors != 4) return DT_OPENCL_PROCESS_CL;  // we need RGB signal

  // Only the preview pipe publishes its luminance mask to the GUI, so only
  // that one is copied back to host memory; see below.
  gboolean cached = FALSE;
  float *luminance = NULL;

  if(self->dev->gui_attached && g)
  {
    // If the module instance has changed order in the pipe, invalidate the caches
    if(g->pipe_order != piece->module->iop_order)
    {
      dt_iop_gui_enter_critical_section(self);
      g->ui_preview_hash = DT_INVALID_HASH;
      g->pipe_order = piece->module->iop_order;
      g->luminance_valid = FALSE;
      g->histogram_valid = FALSE;
      dt_iop_gui_leave_critical_section(self);
      dt_preview_data_invalidate(&g->pd);
    }

    if(dt_pipe_is_full(piece->pipe))
    {
      // The mask stays on the device : the correction is applied there and
      // no GUI code reads g->full_preview_buf.  Both GUI consumers, the
      // histogram and the exposure under the cursor, are fed by the preview
      // pipe buffer instead.  Copying the full pipe mask back would be a
      // blocking multi-megabyte transfer into a buffer nobody reads, and it
      // would happen on every roi or upstream change.
      //
      // toneeq_process() skips compute_luminance_mask() while
      // g->ui_preview_hash still matches, so invalidate it here: should the
      // pipe fall back to the CPU, it has to recompute the mask rather than
      // reuse a host buffer this path never filled.
      const dt_hash_t invalid = DT_INVALID_HASH;
      hash_set_get(&invalid, &g->ui_preview_hash, &self->gui_lock);
    }
    else if(dt_pipe_is_preview(piece->pipe))
    {
      // The shared under-cursor service owns the buffer and its locks.
      // The resize and the luminance_valid invalidation happen under one
      // GUI lock so the GUI never reads a resized, not-yet-recomputed buffer.
      luminance = dt_preview_data_resize(&g->pd, width, height,
                                         _toneeq_preview_resized, self);
      cached = TRUE;
    }

    if(cached && !luminance)
    {
      dt_control_log(_("tone equalizer failed to allocate memory, check your RAM settings"));
      return DT_OPENCL_PROCESS_CL;
    }
  }

  cl_int err = CL_MEM_OBJECT_ALLOCATION_FAILURE;

  cl_mem dev_luminance = dt_opencl_alloc_device_buffer(devid, num_elem * sizeof(float));
  if(!dev_luminance) goto error;

  // Compute the luminance mask
  err = _compute_luminance_mask_cl(devid, gd, dev_in, dev_luminance,
                                   width, height, d);
  if(err != CL_SUCCESS) goto error;

  // Keep the host-side cache in sync so that the GUI can compute the histogram
  // and read the luminance under the cursor
  if(cached)
  {
    const dt_hash_t saved_hash = dt_preview_data_get_hash(&g->pd);

    dt_iop_gui_enter_critical_section(self);
    const gboolean luminance_valid = g->luminance_valid;
    dt_iop_gui_leave_critical_section(self);

    if(saved_hash != hash || !luminance_valid)
    {
      /* copy back only if upstream pipe state has changed */
      // Flag the cache as being recomputed so the GUI threads never
      // read a partially filled buffer, then commit hash + validity
      // once the data is ready.
      dt_iop_gui_enter_critical_section(self);
      g->histogram_valid = FALSE;
      g->luminance_valid = FALSE;
      dt_iop_gui_leave_critical_section(self);

      err = dt_opencl_read_buffer_from_device(devid, luminance, dev_luminance,
                                              0, num_elem * sizeof(float), TRUE);
      if(err != CL_SUCCESS) goto error;
      dt_preview_data_set_hash_value(&g->pd, hash);

      dt_iop_gui_enter_critical_section(self);
      g->luminance_valid = TRUE;
      dt_iop_gui_leave_critical_section(self);
      dt_dev_pixelpipe_cache_invalidate_later(piece->pipe, self->iop_order, "toneequal: ");
    }
  }

  // The output dimensions need to be smaller or equal to the input ones
  const int out_width = MIN(width, roi_out->width);
  const int out_height = MIN(height, roi_out->height);
  const int offset_x = (roi_in->x < roi_out->x) ? roi_out->x - roi_in->x : 0;
  const int offset_y = (roi_in->y < roi_out->y) ? roi_out->y - roi_in->y : 0;

  // Display output
  if(self->dev->gui_attached && g && dt_pipe_is_full(piece->pipe) && g->mask_display)
  {
    err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_toneequal_display_mask,
            out_width, out_height,
            CLARG(dev_in), CLARG(dev_luminance), CLARG(dev_out),
            CLARG(out_width), CLARG(out_height), CLARG(width),
            CLARG(offset_x), CLARG(offset_y));
    if(err != CL_SUCCESS) goto error;

    piece->pipe->mask_display = DT_DEV_PIXELPIPE_DISPLAY_PASSTHRU;
  }
  else
  {
    // the correction is interpolated by a series of gaussians, which is
    // cheaper on a GPU than uploading the correction LUT used by the CPU code
    const float gauss_denom = gaussian_denom(d->smoothing);

    // the 8 octave factors are passed as two float4, so keep the halves in
    // their own pointers : CLFLARRAY() casts before it adds an offset
    const float *const restrict factors_low = d->factors;
    const float *const restrict factors_high = d->factors + PIXEL_CHAN / 2;

    err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_toneequal_apply,
            out_width, out_height,
            CLARG(dev_in), CLARG(dev_luminance), CLARG(dev_out),
            CLARG(out_width), CLARG(out_height), CLARG(width),
            CLARG(offset_x), CLARG(offset_y),
            CLFLARRAY(4, factors_low), CLFLARRAY(4, factors_high),
            CLARG(gauss_denom));
  }

error:
  dt_opencl_release_mem_object(dev_luminance);
  return err;
}

#endif // HAVE_OPENCL


void tiling_callback(dt_iop_module_t *self,
                     dt_dev_pixelpipe_iop_t *piece,
                     const dt_iop_roi_t *roi_in,
                     const dt_iop_roi_t *roi_out,
                     dt_develop_tiling_t *tiling)
{
  const dt_iop_toneequalizer_data_t *const d = piece->data;

  tiling->maxbuf = 1.0f;
  tiling->maxbuf_cl = 1.0f;
  tiling->overhead = 0;
  tiling->overlap = 0;
  tiling->align = 1;

  // in and out buffers plus the full size luminance mask, expressed as a
  // multiple of a 4 channel image buffer
  float factor = 2.25f;

  switch(d->details)
  {
    case DT_TONEEQ_AVG_GUIDED:
    case DT_TONEEQ_GUIDED:
      // full size a and b parameters plus the sixteenth-sized working set
      factor += 0.5f + 0.25f;
      break;

    case DT_TONEEQ_AVG_EIGF:
    case DT_TONEEQ_EIGF:
    {
      // the downscaling factor of the EIGF depends on the filter radius
      const float scaling = fmaxf(fminf((float)d->radius, 4.0f), 1.0f);
      const float ds = 1.0f / (scaling * scaling);
      factor += (d->quantization != 0.0f)
        ? 1.25f + 3.5f * ds  // mask and averages, plus the gaussian buffers
        : 0.5f + 1.75f * ds;
      break;
    }

    case DT_TONEEQ_NONE:
    default:
      break;
  }

  tiling->factor = factor;
  tiling->factor_cl = factor;
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
 * Remember the user params split the [-8; 0] EV range in 9 channels
 * and define a set of (x, y) coordinates, where x are the exposure
 * channels (evenly-spaced by 1 EV in [-8; 0] EV) and y are the
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

static void compute_correction_lut(float* restrict lut,
                                   const float sigma,
                                   const float *const restrict factors)
{
  const float gauss_denom = gaussian_denom(sigma);
  assert(PIXEL_CHAN == 8);

  DT_OMP_FOR(shared(centers_ops))
  for(int j = 0; j <= LUT_RESOLUTION * PIXEL_CHAN; j++)
  {
    // build the correction for each pixel
    // as the sum of the contribution of each luminance channelcorrection
    const float exposure = (float)j / (float)LUT_RESOLUTION + DT_TONEEQ_MIN_EV;
    float result = 0.0f;
    for(int i = 0; i < PIXEL_CHAN; i++)
      result += gaussian_func(exposure - centers_ops[i], gauss_denom) * factors[i];
    // the user-set correction is expected in [-2;+2] EV, so is the interpolated one
    lut[j] = fast_clamp(result, 0.25f, 4.0f);
  }
}

static void get_channels_gains(float factors[CHANNELS],
                               const dt_iop_toneequalizer_params_t *p)
{
  assert(CHANNELS == 9);

  // Get user-set channels gains in EV (log2)
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


static void get_channels_factors(float factors[CHANNELS],
                                 const dt_iop_toneequalizer_params_t *p)
{
  assert(CHANNELS == 9);

  // Get user-set channels gains in EV (log2)
  get_channels_gains(factors, p);

  // Convert from EV offsets to linear factors
  DT_OMP_SIMD(aligned(factors:64))
  for(int c = 0; c < CHANNELS; ++c)
    factors[c] = exp2f(factors[c]);
}


__DT_CLONE_TARGETS__
static gboolean compute_channels_factors(const float factors[PIXEL_CHAN],
                                         float out[CHANNELS],
                                         const float sigma)
{
  // Input factors are the weights for the radial-basis curve
  // approximation of user params Output factors are the gains of the
  // user parameters channels aka the y coordinates of the
  // approximation for x = { CHANNELS }
  assert(PIXEL_CHAN == 8);

  DT_OMP_FOR_SIMD(aligned(factors, out, centers_params:64) firstprivate(centers_params))
  for(int i = 0; i < CHANNELS; ++i)
  {
    // Compute the new channels factors; pixel_correction clamps the factors, so we don't
    // need to check for validity here
    out[i] = pixel_correction(centers_params[i], factors, sigma);
  }
  return TRUE;
}


__DT_CLONE_TARGETS__
static void compute_channels_gains(const float in[CHANNELS],
                                  float out[CHANNELS])
{
  // Helper function to compute the new channels gains (log) from the factors (linear)
  assert(PIXEL_CHAN == 8);

  for(int i = 0; i < CHANNELS; ++i)
    out[i] = log2f(in[i]);
}


static void commit_channels_gains(const float factors[CHANNELS],
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


/***
 * Cache invalidation and initializatiom
 ***/


static void gui_cache_init(dt_iop_module_t *self)
{
  dt_iop_toneequalizer_gui_data_t *g = self->gui_data;
  if(g == NULL) return;

  dt_iop_gui_enter_critical_section(self);
  g->ui_preview_hash = DT_INVALID_HASH;
  dt_preview_data_alloc(&g->pd, self);
  g->max_histogram = 1;
  g->scale = 1.0f;
  g->sigma = M_SQRT2_F;
  g->mask_display = FALSE;

  g->interpolation_valid = FALSE;  // TRUE if the interpolation_matrix is ready
  g->luminance_valid = FALSE;      // TRUE if the luminance cache is ready
  g->histogram_valid = FALSE;      // TRUE if the histogram cache and stats are ready
  g->lut_valid = FALSE;            // TRUE if the gui_lut is ready
  g->graph_valid = FALSE;          // TRUE if the UI graph view is ready
  g->user_param_valid = FALSE;     // TRUE if users params set in interactive view are in bounds
  g->factors_valid = TRUE;         // TRUE if radial-basis coeffs are ready

  g->valid_nodes_x = FALSE;        // TRUE if x coordinates of graph nodes have been inited
  g->valid_nodes_y = FALSE;        // TRUE if y coordinates of graph nodes have been inited
  g->area_cursor_valid = FALSE;    // TRUE if mouse cursor is over the graph area
  g->area_dragging = FALSE;        // TRUE if left-button has been pushed but not released and cursor motion is recorded
  g->cursor_valid = FALSE;         // TRUE if mouse cursor is over the preview image
  g->has_focus = FALSE;            // TRUE if module has focus from GTK

  g->full_preview_buf = NULL;
  g->full_preview_buf_width = 0;
  g->full_preview_buf_height = 0;

  g->desc = NULL;
  g->layout = NULL;
  g->cr = NULL;
  g->cst = NULL;
  g->context = NULL;

  g->pipe_order = 0;
  dt_iop_gui_leave_critical_section(self);
}


static inline void build_interpolation_matrix(float A[CHANNELS * PIXEL_CHAN],
                                              const float sigma)
{
  // Build the symmetrical definite positive part of the augmented matrix
  // of the radial-basis interpolation weights

  const float gauss_denom = gaussian_denom(sigma);

  DT_OMP_SIMD(aligned(A, centers_ops, centers_params:64) collapse(2))
  for(int i = 0; i < CHANNELS; ++i)
    for(int j = 0; j < PIXEL_CHAN; ++j)
      A[i * PIXEL_CHAN + j] =
        gaussian_func(centers_params[i] - centers_ops[j], gauss_denom);
}


__DT_CLONE_TARGETS__
static inline void compute_log_histogram_and_stats(const float *const restrict luminance,
                                                   int histogram[UI_SAMPLES],
                                                   const size_t num_elem,
                                                   int *max_histogram,
                                                   float *first_decile,
                                                   float *last_decile)
{
  // (Re)init the histogram
  memset(histogram, 0, sizeof(int) * UI_SAMPLES);

  // we first calculate an extended histogram for better accuracy
  #define TEMP_SAMPLES 2 * UI_SAMPLES
  int temp_hist[TEMP_SAMPLES];
  memset(temp_hist, 0, sizeof(int) * TEMP_SAMPLES);

  // Split exposure in bins
  DT_OMP_FOR_SIMD(reduction(+:temp_hist[:TEMP_SAMPLES]))
  for(size_t k = 0; k < num_elem; k++)
  {
    // extended histogram bins between [-10; +6] EV remapped between [0 ; 2 * UI_SAMPLES]
    const int index =
      CLAMP((int)(((log2f(luminance[k]) + 10.0f) / 16.0f) * (float)TEMP_SAMPLES),
            0, TEMP_SAMPLES - 1);
    temp_hist[index] += 1;
  }

  const int first = (int)((float)num_elem * 0.05f);
  const int last = (int)((float)num_elem * (1.0f - 0.95f));
  int population = 0;
  int first_pos = 0;
  int last_pos = 0;

  // scout the extended histogram bins looking for deciles
  // these would not be accurate with the regular histogram
  for(int k = 0; k < TEMP_SAMPLES; ++k)
  {
    const size_t prev_population = population;
    population += temp_hist[k];
    if(prev_population < first && first <= population)
    {
      first_pos = k;
      break;
    }
  }
  population = 0;
  for(int k = TEMP_SAMPLES - 1; k >= 0; --k)
  {
    const size_t prev_population = population;
    population += temp_hist[k];
    if(prev_population < last && last <= population)
    {
      last_pos = k;
      break;
    }
  }

  // Convert decile positions to exposures
  *first_decile = 16.0 * (float)first_pos / (float)(TEMP_SAMPLES - 1) - 10.0;
  *last_decile = 16.0 * (float)last_pos / (float)(TEMP_SAMPLES - 1) - 10.0;

  // remap the extended histogram into the normal one
  // bins between [-8; 0] EV remapped between [0 ; UI_SAMPLES]
  for(size_t k = 0; k < TEMP_SAMPLES; ++k)
  {
    const float EV = 16.0 * (float)k / (float)(TEMP_SAMPLES - 1) - 10.0;
    const int i =
      CLAMP((int)(((EV + 8.0f) / 8.0f) * (float)UI_SAMPLES),
            0, UI_SAMPLES - 1);
    histogram[i] += temp_hist[k];

    // store the max numbers of elements in bins for later normalization
    *max_histogram = histogram[i] > *max_histogram ? histogram[i] : *max_histogram;
  }
}

static inline void update_histogram(dt_iop_module_t *const self)
{
  dt_iop_toneequalizer_gui_data_t *const g = self->gui_data;
  if(g == NULL) return;

  dt_iop_gui_enter_critical_section(self);
  if(!g->histogram_valid && g->luminance_valid)
  {
    const size_t num_elem = g->pd.height * g->pd.width;
    compute_log_histogram_and_stats(g->pd.buf, g->histogram, num_elem,
                                    &g->max_histogram,
                                    &g->histogram_first_decile, &g->histogram_last_decile);
    g->histogram_average = (g->histogram_first_decile + g->histogram_last_decile) / 2.0f;
    g->histogram_valid = TRUE;
  }
  dt_iop_gui_leave_critical_section(self);
}


__DT_CLONE_TARGETS__
static inline void compute_lut_correction(dt_iop_toneequalizer_gui_data_t *g,
                                          const float offset,
                                          const float scaling)
{
  // Compute the LUT of the exposure corrections in EV,
  // offset and scale it for display in GUI widget graph

  if(g == NULL) return;

  float *const restrict LUT = g->gui_lut;
  const float *const restrict factors = g->factors;
  const float sigma = g->sigma;

  DT_OMP_FOR_SIMD(aligned(LUT, factors:64))
  for(int k = 0; k < UI_SAMPLES; k++)
  {
    // build the inset graph curve LUT
    // the x range is [-14;+2] EV
    const float x = (8.0f * (((float)k) / ((float)(UI_SAMPLES - 1)))) - 8.0f;
    LUT[k] = offset - log2f(pixel_correction(x, factors, sigma)) / scaling;
  }
}



static inline gboolean update_curve_lut(dt_iop_module_t *self)
{
  const dt_iop_toneequalizer_params_t *p = self->params;
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
    float factors[CHANNELS] DT_ALIGNED_ARRAY;
    get_channels_factors(factors, p);
    dt_simd_memcpy(factors, g->temp_user_params, CHANNELS);
    g->user_param_valid = TRUE;
    g->factors_valid = FALSE;
  }

  if(!g->factors_valid && g->user_param_valid)
  {
    float factors[CHANNELS] DT_ALIGNED_ARRAY;
    dt_simd_memcpy(g->temp_user_params, factors, CHANNELS);
    valid = pseudo_solve(g->interpolation_matrix, factors, CHANNELS, PIXEL_CHAN, FALSE);
    if(valid) dt_simd_memcpy(factors, g->factors, PIXEL_CHAN);

    g->factors_valid = valid;
    g->lut_valid = FALSE;
  }

  if(!g->lut_valid) // && g->factors_valid)
  {
    compute_lut_correction(g, 0.5f, 4.0f);
    g->lut_valid = TRUE;
  }

  dt_iop_gui_leave_critical_section(self);

  return valid;
}


void init_global(dt_iop_module_so_t *self)
{
  const int program = 43; // toneequal.cl, from programs.conf

  dt_iop_toneequalizer_global_data_t *gd = malloc(sizeof(dt_iop_toneequalizer_global_data_t));

  self->data = gd;

  gd->kernel_toneequal_luminance_mask =
    dt_opencl_create_kernel(program, "toneequal_luminance_mask");
  gd->kernel_toneequal_apply =
    dt_opencl_create_kernel(program, "toneequal_apply");
  gd->kernel_toneequal_display_mask =
    dt_opencl_create_kernel(program, "toneequal_display_mask");
  gd->kernel_toneequal_quantize =
    dt_opencl_create_kernel(program, "toneequal_quantize");
  gd->kernel_toneequal_box_mean_x_2c =
    dt_opencl_create_kernel(program, "toneequal_box_mean_x_2c");
  gd->kernel_toneequal_box_mean_y_2c =
    dt_opencl_create_kernel(program, "toneequal_box_mean_y_2c");
  gd->kernel_toneequal_box_mean_x_4c =
    dt_opencl_create_kernel(program, "toneequal_box_mean_x_4c");
  gd->kernel_toneequal_box_mean_y_4c =
    dt_opencl_create_kernel(program, "toneequal_box_mean_y_4c");
  gd->kernel_toneequal_gf_pack =
    dt_opencl_create_kernel(program, "toneequal_gf_pack");
  gd->kernel_toneequal_gf_ab =
    dt_opencl_create_kernel(program, "toneequal_gf_ab");
  gd->kernel_toneequal_gf_blend =
    dt_opencl_create_kernel(program, "toneequal_gf_blend");
  gd->kernel_toneequal_eigf_pack_4c =
    dt_opencl_create_kernel(program, "toneequal_eigf_pack_4c");
  gd->kernel_toneequal_eigf_finish_4c =
    dt_opencl_create_kernel(program, "toneequal_eigf_finish_4c");
  gd->kernel_toneequal_eigf_pack_2c =
    dt_opencl_create_kernel(program, "toneequal_eigf_pack_2c");
  gd->kernel_toneequal_eigf_finish_2c =
    dt_opencl_create_kernel(program, "toneequal_eigf_finish_2c");
  gd->kernel_toneequal_eigf_blend =
    dt_opencl_create_kernel(program, "toneequal_eigf_blend");
  gd->kernel_toneequal_eigf_blend_no_mask =
    dt_opencl_create_kernel(program, "toneequal_eigf_blend_no_mask");
}


void cleanup_global(dt_iop_module_so_t *self)
{
  const dt_iop_toneequalizer_global_data_t *gd = self->data;

  dt_opencl_free_kernel(gd->kernel_toneequal_luminance_mask);
  dt_opencl_free_kernel(gd->kernel_toneequal_apply);
  dt_opencl_free_kernel(gd->kernel_toneequal_display_mask);
  dt_opencl_free_kernel(gd->kernel_toneequal_quantize);
  dt_opencl_free_kernel(gd->kernel_toneequal_box_mean_x_2c);
  dt_opencl_free_kernel(gd->kernel_toneequal_box_mean_y_2c);
  dt_opencl_free_kernel(gd->kernel_toneequal_box_mean_x_4c);
  dt_opencl_free_kernel(gd->kernel_toneequal_box_mean_y_4c);
  dt_opencl_free_kernel(gd->kernel_toneequal_gf_pack);
  dt_opencl_free_kernel(gd->kernel_toneequal_gf_ab);
  dt_opencl_free_kernel(gd->kernel_toneequal_gf_blend);
  dt_opencl_free_kernel(gd->kernel_toneequal_eigf_pack_4c);
  dt_opencl_free_kernel(gd->kernel_toneequal_eigf_finish_4c);
  dt_opencl_free_kernel(gd->kernel_toneequal_eigf_pack_2c);
  dt_opencl_free_kernel(gd->kernel_toneequal_eigf_finish_2c);
  dt_opencl_free_kernel(gd->kernel_toneequal_eigf_blend);
  dt_opencl_free_kernel(gd->kernel_toneequal_eigf_blend_no_mask);

  free(self->data);
  self->data = NULL;
}


void commit_params(dt_iop_module_t *self,
                   dt_iop_params_t *p1,
                   dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  const dt_iop_toneequalizer_params_t *p = (dt_iop_toneequalizer_params_t *)p1;
  dt_iop_toneequalizer_data_t *d = piece->data;
  dt_iop_toneequalizer_gui_data_t *g = self->gui_data;

  // Trivial params passing
  d->method = p->method;
  d->details = p->details;
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

  /*
   * Perform a radial-based interpolation using a series gaussian functions
   */
  if(self->dev->gui_attached && g)
  {
    dt_iop_gui_enter_critical_section(self);
    if(g->sigma != p->smoothing)
      g->interpolation_valid = FALSE;
    g->sigma = p->smoothing;
    g->user_param_valid = FALSE; // force updating channels factors
    dt_iop_gui_leave_critical_section(self);

    update_curve_lut(self);

    dt_iop_gui_enter_critical_section(self);
    dt_simd_memcpy(g->factors, d->factors, PIXEL_CHAN);
    dt_iop_gui_leave_critical_section(self);
  }
  else
  {
    // No cache : Build / Solve interpolation matrix
    float factors[CHANNELS] DT_ALIGNED_ARRAY;
    get_channels_factors(factors, p);

    float A[CHANNELS * PIXEL_CHAN] DT_ALIGNED_ARRAY;
    build_interpolation_matrix(A, p->smoothing);
    pseudo_solve(A, factors, CHANNELS, PIXEL_CHAN, TRUE);

    dt_simd_memcpy(factors, d->factors, PIXEL_CHAN);
  }

  // compute the correction LUT here to spare some time in process
  // when computing several times toneequalizer with same parameters
  compute_correction_lut(d->correction_lut, d->smoothing, d->factors);
}


void init_pipe(dt_iop_module_t *self,
               dt_dev_pixelpipe_t *pipe,
               dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = dt_calloc1_align_type(dt_iop_toneequalizer_data_t);
}


void cleanup_pipe(dt_iop_module_t *self,
                  dt_dev_pixelpipe_t *pipe,
                  dt_dev_pixelpipe_iop_t *piece)
{
  dt_free_align(piece->data);
  piece->data = NULL;
}

static void show_guiding_controls(const dt_iop_module_t *self)
{
  const dt_iop_toneequalizer_gui_data_t *g = self->gui_data;
  const dt_iop_toneequalizer_params_t *p = self->params;

  switch(p->details)
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
}

void update_exposure_sliders(const dt_iop_toneequalizer_gui_data_t *g,
                             const dt_iop_toneequalizer_params_t *p)
{
  DT_ENTER_GUI_UPDATE();
  dt_bauhaus_slider_set(g->noise, p->noise);
  dt_bauhaus_slider_set(g->ultra_deep_blacks, p->ultra_deep_blacks);
  dt_bauhaus_slider_set(g->deep_blacks, p->deep_blacks);
  dt_bauhaus_slider_set(g->blacks, p->blacks);
  dt_bauhaus_slider_set(g->shadows, p->shadows);
  dt_bauhaus_slider_set(g->midtones, p->midtones);
  dt_bauhaus_slider_set(g->highlights, p->highlights);
  dt_bauhaus_slider_set(g->whites, p->whites);
  dt_bauhaus_slider_set(g->speculars, p->speculars);
  DT_LEAVE_GUI_UPDATE();
}


void gui_update(dt_iop_module_t *self)
{
  const dt_iop_toneequalizer_gui_data_t *g = self->gui_data;
  const dt_iop_toneequalizer_params_t *p = self->params;

  dt_bauhaus_slider_set(g->smoothing, logf(p->smoothing) / logf(M_SQRT2_F) - 1.0f);

  show_guiding_controls(self);
  invalidate_luminance_cache(self);

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->show_luminance_mask), g->mask_display);
}

void gui_changed(dt_iop_module_t *self,
                 GtkWidget *w,
                 void *previous)
{
  const dt_iop_toneequalizer_gui_data_t *g = self->gui_data;

  if(w == g->method
     || w == g->blending
     || w == g->feathering
     || w == g->iterations
     || w == g->quantization)
  {
    invalidate_luminance_cache(self);
  }
  else if(w == g->details)
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
}

static void smoothing_callback(GtkWidget *slider, dt_iop_module_t *self)
{
  DT_GUARD_GUI_UPDATE();
  dt_iop_toneequalizer_params_t *p = self->params;
  const dt_iop_toneequalizer_gui_data_t *g = self->gui_data;

  p->smoothing= powf(M_SQRT2_F, 1.0f +  dt_bauhaus_slider_get(slider));

  float factors[CHANNELS] DT_ALIGNED_ARRAY;
  get_channels_factors(factors, p);

  // Solve the interpolation by least-squares to check the validity of the smoothing param
  if(!update_curve_lut(self))
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

  DT_GUARD_GUI_UPDATE();

  dt_iop_request_focus(self);

  if(!self->enabled)
  {
    // activate module and do nothing
    DT_ENTER_GUI_UPDATE();
    dt_bauhaus_slider_set(g->exposure_boost, p->exposure_boost);
    DT_LEAVE_GUI_UPDATE();

    invalidate_luminance_cache(self);
    dt_dev_add_history_item(darktable.develop, self, TRUE);
    return;
  }

  if(!g->luminance_valid || dt_pipe_processing(self->dev->full.pipe) || !g->histogram_valid)
  {
    dt_control_log(_("wait for the preview to finish recomputing"));
    return;
  }

  // The goal is to get the exposure distribution centered on the equalizer view
  // to spread it over as many nodes as possible for better exposure control.
  // Controls nodes are between -8 and 0 EV,
  // so we aim at centering the exposure distribution on -4 EV

  dt_iop_gui_enter_critical_section(self);
  g->histogram_valid = FALSE;
  dt_iop_gui_leave_critical_section(self);

  update_histogram(self);

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
  DT_ENTER_GUI_UPDATE();
  dt_bauhaus_slider_set(g->exposure_boost, p->exposure_boost);
  DT_LEAVE_GUI_UPDATE();
  invalidate_luminance_cache(self);
  dt_dev_add_history_item(darktable.develop, self, TRUE);

  // Unlock the colour picker so we can display our own custom cursor
  dt_iop_color_picker_reset(self, TRUE);
}


static void auto_adjust_contrast_boost(GtkWidget *quad, dt_iop_module_t *self)
{
  dt_iop_toneequalizer_params_t *p = self->params;
  dt_iop_toneequalizer_gui_data_t *g = self->gui_data;

  DT_GUARD_GUI_UPDATE();

  dt_iop_request_focus(self);

  if(!self->enabled)
  {
    // activate module and do nothing
    DT_ENTER_GUI_UPDATE();
    dt_bauhaus_slider_set(g->contrast_boost, p->contrast_boost);
    DT_LEAVE_GUI_UPDATE();

    invalidate_luminance_cache(self);
    dt_dev_add_history_item(darktable.develop, self, TRUE);
    return;
  }

  if(!g->luminance_valid || dt_pipe_processing(self->dev->full.pipe) || !g->histogram_valid)
  {
    dt_control_log(_("wait for the preview to finish recomputing"));
    return;
  }

  // The goal is to spread 90 % of the exposure histogram in the [-7, -1] EV
  dt_iop_gui_enter_critical_section(self);
  g->histogram_valid = FALSE;
  dt_iop_gui_leave_critical_section(self);

  update_histogram(self);

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
  if(p->details == DT_TONEEQ_EIGF && c > 0.0f)
  {
    const float correction = -0.0276f + 0.01823 * p->feathering + (0.7566f - 1.0f) * c;
    if(p->feathering < 5.0f)
      c += correction;
    else if(p->feathering < 10.0f)
      c += correction * (2.0f - p->feathering / 5.0f);
  }
  else if(p->details == DT_TONEEQ_GUIDED && c > 0.0f)
      c = 0.0235f + 1.1225f * c;

  p->contrast_boost += c;

  // Update the GUI stuff
  DT_ENTER_GUI_UPDATE();
  dt_bauhaus_slider_set(g->contrast_boost, p->contrast_boost);
  DT_LEAVE_GUI_UPDATE();
  invalidate_luminance_cache(self);
  dt_dev_add_history_item(darktable.develop, self, TRUE);

  // Unlock the colour picker so we can display our own custom cursor
  dt_iop_color_picker_reset(self, TRUE);
}


static void show_luminance_mask_callback(GtkGestureSingle *gesture,
                                           int n_press,
                                           double x,
                                           double y,
                                           dt_iop_module_t *self)
{
  DT_GUARD_GUI_UPDATE();
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
//  dt_dev_reprocess_center(self->dev, self->iop_order);
  dt_iop_refresh_center(self);

  // Unlock the colour picker so we can display our own custom cursor
  dt_iop_color_picker_reset(self, TRUE);
}


/***
 * GUI Interactivity
 **/

static void switch_cursors(dt_iop_module_t *self)
{
  dt_iop_toneequalizer_gui_data_t *g = self->gui_data;

  if(!g || !self->dev->gui_attached)
    return;

  // if we are editing masks or using colour-pickers, do not display controls
  if(in_mask_editing(self)
     || dt_iop_canvas_not_sensitive(self->dev))
  {
    // display default cursor
    dt_control_change_cursor("default");
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
  else if(g->cursor_valid)
  {
    // if cursor is on the preview, hide GTK cursor because we display
    // our custom one.  We do this whether or not the pipe is still
    // (re)computing, so no busy animation appears while hovering.
    dt_control_change_cursor("none");
    dt_control_hinter_message(_("scroll over image to change tone exposure\n"
                                "shift+scroll for large steps; "
                                "ctrl+scroll for small steps"));

    dt_control_queue_redraw_center();
  }
  else if(!g->cursor_valid)
  {
    // if module is active and opened but cursor is out of the preview,
    // display default cursor
    dt_control_change_cursor("default");
    dt_control_queue_redraw_center();
  }
  else
  {
    // in any other situation where module has focus,
    // reset the cursor but don't launch a redraw
    dt_control_change_cursor("default");
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
  if(g->cursor_valid && !dt_pipe_processing(dev->full.pipe) && g->luminance_valid)
    g->cursor_exposure = log2f(_luminance_from_module_buffer(self));

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
  dt_control_change_cursor("default");
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
  // Apply an exposure offset optimized smoothly over all the exposure channels,
  // taking user instruction to apply exposure_offset EV at control_exposure EV,
  // and commit the new params is the solution is valid.

  // Raise the user params accordingly to control correction and
  // distance from cursor exposure to blend smoothly the desired
  // correction
  const float std = gaussian_denom(blending_sigma);
  if(g->user_param_valid)
  {
    for(int i = 0; i < CHANNELS; ++i)
      g->temp_user_params[i] *=
        exp2f(gaussian_func(centers_params[i] - control_exposure, std) * exposure_offset);
  }

  // Get the new weights for the radial-basis approximation
  float factors[CHANNELS] DT_ALIGNED_ARRAY;
  dt_simd_memcpy(g->temp_user_params, factors, CHANNELS);
  if(g->user_param_valid)
    g->user_param_valid = pseudo_solve(g->interpolation_matrix, factors, CHANNELS, PIXEL_CHAN, FALSE);
  if(!g->user_param_valid)
    dt_control_log(_("the interpolation is unstable, decrease the curve smoothing"));

  // Compute new user params for channels and store them locally
  if(g->user_param_valid)
    g->user_param_valid = compute_channels_factors(factors, g->temp_user_params, g->sigma);
  if(!g->user_param_valid) dt_control_log(_("some parameters are out-of-bounds"));

  const gboolean commit = g->user_param_valid;

  if(commit)
  {
    // Accept the solution
    dt_simd_memcpy(factors, g->factors, PIXEL_CHAN);
    g->lut_valid = FALSE;

    // Convert the linear temp parameters to log gains and commit
    float gains[CHANNELS] DT_ALIGNED_ARRAY;
    compute_channels_gains(g->temp_user_params, gains);
    commit_channels_gains(gains, p);
  }
  else
  {
    // Reset the GUI copy of user params
    get_channels_factors(factors, p);
    dt_simd_memcpy(factors, g->temp_user_params, CHANNELS);
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
  const dt_develop_t *dev = self->dev;
  dt_iop_toneequalizer_gui_data_t *g = self->gui_data;
  dt_iop_toneequalizer_params_t *p = self->params;

  DT_GUARD_GUI_UPDATE(1);
  if(g == NULL) return 0;
  if(!g->has_focus) return 0;

  // turn-on the module if off
  if(!self->enabled)
    if(self->off) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->off), 1);

  if(in_mask_editing(self)) return 0;

  // if GUI buffers not ready, exit but still handle the cursor
  dt_iop_gui_enter_critical_section(self);

  const gboolean fail = !g->cursor_valid
                     || !g->luminance_valid
                     || !g->interpolation_valid
                     || !g->user_param_valid
                     || dt_pipe_processing(dev->full.pipe)
                     || !g->has_focus;

  dt_iop_gui_leave_critical_section(self);
  if(fail) return 1;

  // re-read the exposure in case it has changed
  dt_iop_gui_enter_critical_section(self);
  g->cursor_exposure = log2f(_luminance_from_module_buffer(self));

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

  // Get the desired correction on exposure channels
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

/***
 * GTK/Cairo drawings and custom widgets
 **/

static inline gboolean _init_drawing(dt_iop_module_t *const restrict self,
                                     GtkWidget *widget,
                                     dt_iop_toneequalizer_gui_data_t *const restrict g);


// The on-canvas correction cursor itself (crosshair, wedge, circles, text
// label) is shared with other modules via dt_draw_correction_cursor() in
// gui/draw.h, and its contrasting frame color via dt_draw_backbuf_contrast()
// in the same header; only the exposure-specific grey shades fed into it
// stay here.

static float _shade_from_luminance(const float luminance)
{
  // TODO: fetch screen gamma from ICC display profile
  const float gamma = 1.0f / 2.2f;
  return powf(luminance, gamma);
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

  const dt_develop_t *dev = self->dev;
  dt_iop_toneequalizer_gui_data_t *g = self->gui_data;

  // if we are editing masks, do not display controls
  if(in_mask_editing(self)) return;

  dt_iop_gui_enter_critical_section(self);

  const gboolean fail = !g->cursor_valid
                     || !g->interpolation_valid
                     || !g->has_focus;

  dt_iop_gui_leave_critical_section(self);

  if(fail) return;

  if(!g->graph_valid)
    if(!_init_drawing(self, self->widget, g))
      return;

  // Re-read the exposure in case it has changed.  While the pipe is busy
  // the module buffer may be mid-recompute, so keep the last value and
  // stay drawing the indicator (no blinking cursor during reprocess).
  if(g->luminance_valid && self->enabled && !dt_pipe_processing(dev->full.pipe))
    g->cursor_exposure = log2f(_luminance_from_module_buffer(self));

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

  char text[256];
  if(g->luminance_valid && self->enabled)
    snprintf(text, sizeof(text), _("%+.1f EV"), exposure_in);
  else
    snprintf(text, sizeof(text), "? EV");

  // Sample the display pixel under the cursor from the preview pipe
  // backbuf and pick the contrasting frame color, shared with the
  // color equalizer's cursor via dt_draw_backbuf_contrast() in
  // gui/draw.h.  The circles keep the exposure-specific shades below
  // to convey the before/after luminance.
  float bg_rgb[3];
  float frame_color[3];
  dt_draw_backbuf_contrast(dev, pointerx, pointery, bg_rgb, frame_color,
                           16.0f / (zoom_scale * width));
  const float outer_shade = _shade_from_luminance(luminance_in);
  const float inner_shade = _shade_from_luminance(luminance_out);
  const float outer_color[3] = { outer_shade, outer_shade, outer_shade };
  const float inner_color[3] = { inner_shade, inner_shade, inner_shade };

  // The wedge normalizes the correction to ±1 (full ±90°); tone equalizer
  // corrections are expressed in EV and regularly exceed ±1 EV, so halve
  // the value here: the wedge then reaches its full ±90° at ±2 EV.
  dt_draw_correction_cursor(cr, x_pointer, y_pointer, zoom_scale, 0.5f * correction,
                            frame_color,
                            outer_color, log2f(luminance_in) > 0.0f,
                            inner_color, log2f(luminance_out) > 0.0f,
                            text);

  if(g->luminance_valid && self->enabled)
  {
    // Search for nearest node in graph and highlight it
    const float radius_threshold = 0.45f;
    g->area_active_node = -1;
    if(g->cursor_valid)
      for(int i = 0; i < CHANNELS; ++i)
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
  const dt_iop_toneequalizer_gui_data_t *g = self->gui_data;
  if(g == NULL) return;
  if(!g->distort_signal_actif) return;

  /* disable the distort signal now to avoid recursive call on this signal as we are
     about to reprocess the preview pipe which has some module doing distortion. */

  _unset_distort_signal(self);

  /* we do reprocess the preview to get a new internal image buffer with the proper
     image geometry. */
  if(self->enabled)
    dt_dev_reprocess_preview(darktable.develop, self->iop_order);
}

static void _set_distort_signal(dt_iop_module_t *self)
{
  dt_iop_toneequalizer_gui_data_t *g = self->gui_data;
  if(self->enabled && !g->distort_signal_actif)
  {
    DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_DEVELOP_DISTORT, _develop_distort_callback);
    g->distort_signal_actif = TRUE;
  }
}

static void _unset_distort_signal(dt_iop_module_t *self)
{
  dt_iop_toneequalizer_gui_data_t *g = self->gui_data;
  if(g->distort_signal_actif)
  {
    DT_CONTROL_SIGNAL_DISCONNECT(_develop_distort_callback, self);
    g->distort_signal_actif = FALSE;
  }
}

void gui_focus(dt_iop_module_t *self, const gboolean in)
{
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
      dt_dev_reprocess_center(self->dev, self->iop_order);
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
  g->desc = dt_gui_get_font();

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

  for(int k = 0; k < CHANNELS; k++)
  {
    const float xn =
      (((float)k) / ((float)(CHANNELS - 1))) * g->graph_width - g->sign_width;

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
    for(int i = 0; i < CHANNELS; ++i)
      g->nodes_x[i] = (((float)i) / ((float)(CHANNELS - 1))) * g->graph_width;
    g->valid_nodes_x = TRUE;
  }
}


// must be called while holding self->gui_lock
static inline void init_nodes_y(dt_iop_toneequalizer_gui_data_t *g)
{
  if(g == NULL) return;

  if(g->user_param_valid && g->graph_height > 0)
  {
    for(int i = 0; i < CHANNELS; ++i)
      g->nodes_y[i] = // assumes factors in [-2 ; 2] EV
        (0.5 - log2f(g->temp_user_params[i]) / 4.0) * g->graph_height;
    g->valid_nodes_y = TRUE;
  }
}


static gboolean area_draw(GtkWidget *widget,
                          cairo_t *cr,
                          dt_iop_module_t *self)
{
  // Draw the widget equalizer view
  dt_iop_toneequalizer_gui_data_t *g = self->gui_data;
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
  update_histogram(self);
  update_curve_lut(self);

  // The pipe thread rebuilds the curve cache from commit_params() at
  // any time, so take one coherent snapshot of everything we are about
  // to draw while holding the lock, then paint from the local copies.
  float gui_lut[UI_SAMPLES] DT_ALIGNED_ARRAY;
  float nodes_x[CHANNELS] DT_ALIGNED_ARRAY;
  float nodes_y[CHANNELS] DT_ALIGNED_ARRAY;

  dt_iop_gui_enter_critical_section(self);
  init_nodes_x(g);
  init_nodes_y(g);
  const gboolean lut_valid = g->lut_valid;
  const gboolean factors_valid = g->factors_valid;
  const gboolean user_param_valid = g->user_param_valid;
  const gboolean nodes_valid = g->valid_nodes_x && g->valid_nodes_y;
  const float sigma = g->sigma;
  dt_simd_memcpy(g->gui_lut, gui_lut, UI_SAMPLES);
  dt_simd_memcpy(g->nodes_x, nodes_x, CHANNELS);
  dt_simd_memcpy(g->nodes_y, nodes_y, CHANNELS);
  dt_iop_gui_leave_critical_section(self);

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

  if(g->histogram_valid && self->enabled)
  {
    // draw the inset histogram
    set_color(g->cr, darktable.bauhaus->inset_histogram);
    cairo_set_line_width(g->cr, DT_PIXEL_APPLY_DPI(4.0));
    cairo_move_to(g->cr, 0, g->graph_height);

    for(int k = 0; k < UI_SAMPLES; k++)
    {
      // the x range is [-8;+0] EV
      const float x_temp = (8.0 * (float)k / (float)(UI_SAMPLES - 1)) - 8.0;
      const float y_temp = (float)(g->histogram[k]) / (float)(g->max_histogram) * 0.96;
      cairo_line_to(g->cr, (x_temp + 8.0) * g->graph_width / 8.0,
                           (1.0 - y_temp) * g->graph_height );
    }
    cairo_line_to(g->cr, g->graph_width, g->graph_height);
    cairo_close_path(g->cr);
    cairo_fill(g->cr);

    if(g->histogram_last_decile > -0.1f)
    {
      // histogram overflows controls in highlights : display warning
      cairo_save(g->cr);
      cairo_set_source_rgb(g->cr, 0.75, 0.50, 0.);
      dtgtk_cairo_paint_warning
        (g->cr,
         g->graph_width - 2.5 * g->line_height, 0.5 * g->line_height,
         2.0 * g->line_height, 2.0 * g->line_height, 0, NULL);
      cairo_restore(g->cr);
    }

    if(g->histogram_first_decile < -7.9f)
    {
      // histogram overflows controls in lowlights : display warning
      cairo_save(g->cr);
      cairo_set_source_rgb(g->cr, 0.75, 0.50, 0.);
      dtgtk_cairo_paint_warning
        (g->cr,
         0.5 * g->line_height, 0.5 * g->line_height,
         2.0 * g->line_height, 2.0 * g->line_height, 0, NULL);
      cairo_restore(g->cr);
    }
  }

  if(lut_valid)
  {
    // draw the interpolation curve
    if(factors_valid)
      set_color(g->cr,  darktable.bauhaus->graph_fg);
    else
      cairo_set_source_rgb(g->cr, 0.75, .5, 0.);

    cairo_move_to(g->cr, 0, gui_lut[0] * g->graph_height);
    cairo_set_line_width(g->cr, DT_PIXEL_APPLY_DPI(3));

    for(int k = 1; k < UI_SAMPLES; k++)
    {
      // the x range is [-8;+0] EV
      const float x_temp = (8.0f * (((float)k) / ((float)(UI_SAMPLES - 1)))) - 8.0f;
      const float y_temp = gui_lut[k];

      cairo_line_to(g->cr, (x_temp + 8.0f) * g->graph_width / 8.0f,
                            y_temp * g->graph_height );
    }
    cairo_stroke(g->cr);
  }

  if(user_param_valid && nodes_valid)
  {
    // draw nodes positions
    for(int k = 0; k < CHANNELS; k++)
    {
      const float xn = nodes_x[k];
      const float yn = nodes_y[k];

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
      const float radius = sigma * g->graph_width / 8.0f / M_SQRT2_F;
      cairo_set_line_width(g->cr, DT_PIXEL_APPLY_DPI(1.5));
      const float y =
        gui_lut[(int)CLAMP(((UI_SAMPLES - 1) * g->area_x / g->graph_width),
                           0, UI_SAMPLES - 1)];
      cairo_arc(g->cr, g->area_x, y * g->graph_height, radius, 0, 2. * M_PI);
      set_color(g->cr, darktable.bauhaus->graph_fg);
      cairo_stroke(g->cr);
    }

    if(g->cursor_valid)
    {

      float x_pos = (g->cursor_exposure + 8.0f) / 8.0f * g->graph_width;

      if(x_pos > g->graph_width || x_pos < 0.0f)
      {
        // exposure at current position is outside [-8; 0] EV :
        // bound it in the graph limits and show it in orange
        cairo_set_source_rgb(g->cr, 0.75, 0.50, 0.);
        cairo_set_line_width(g->cr, DT_PIXEL_APPLY_DPI(3));
        x_pos = (x_pos < 0.0f) ? 0.0f : g->graph_width;
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


static void area_leave_notify(GtkEventControllerMotion *controller,
                               dt_iop_module_t *self)
{
  DT_GUARD_GUI_UPDATE();
  if(!self->enabled) return;

  dt_iop_toneequalizer_gui_data_t *g = self->gui_data;

  const dt_iop_toneequalizer_params_t *p = self->params;

  if(g->area_dragging)
  {
    // cursor left area : force commit to avoid glitches
    update_exposure_sliders(g, p);

    dt_dev_add_history_item(darktable.develop, self, FALSE);
  }
  dt_iop_gui_enter_critical_section(self);
  g->area_x = g->inset;
  g->area_y = g->inset;
  g->area_dragging = FALSE;
  g->area_active_node = -1;
  g->area_cursor_valid = FALSE;
  dt_iop_gui_leave_critical_section(self);

  gtk_widget_queue_draw(GTK_WIDGET(g->area));
}


static void area_button_press(GtkGestureSingle *gesture,
                               gint n_press,
                               gdouble x,
                               gdouble y,
                               dt_iop_module_t *self)
{
  DT_GUARD_GUI_UPDATE();

  dt_iop_toneequalizer_gui_data_t *g = self->gui_data;

  dt_iop_request_focus(self);

  if(gtk_gesture_single_get_current_button(gesture) != GDK_BUTTON_PRIMARY)
  {
    // Unlock the colour picker so we can display our own custom cursor
    dt_iop_color_picker_reset(self, TRUE);
    return;
  }

  if(n_press >= 2)
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
    return;
  }

  if(self->enabled)
  {
    g->area_dragging = TRUE;
    gtk_widget_queue_draw(GTK_WIDGET(g->area));
  }
  else
  {
    dt_dev_add_history_item(darktable.develop, self, TRUE);
  }
}


static void area_motion_notify(GtkEventControllerMotion *controller,
                                gdouble x,
                                gdouble y,
                                dt_iop_module_t *self)
{
  DT_GUARD_GUI_UPDATE();
  if(!self->enabled) return;

  dt_iop_toneequalizer_gui_data_t *g = self->gui_data;
  dt_iop_toneequalizer_params_t *p = self->params;

  if(g->area_dragging)
  {
    // vertical distance travelled since button_pressed event
    dt_iop_gui_enter_critical_section(self);
    // graph spans over 4 EV
    const float offset = (-y + g->area_y) / g->graph_height * 4.0f;
    const float cursor_exposure = g->area_x / g->graph_width * 8.0f - 8.0f;

    // Get the desired correction on exposure channels
    g->area_dragging = set_new_params_interactive(cursor_exposure, offset,
                                                  g->sigma * g->sigma / 2.0f, g, p);
    dt_iop_gui_leave_critical_section(self);
  }

  dt_iop_gui_enter_critical_section(self);
  g->area_x = x - g->inset;
  g->area_y = y;
  g->area_cursor_valid = (g->area_x > 0.0f
                          && g->area_x < g->graph_width
                          && g->area_y > 0.0f
                          && g->area_y < g->graph_height);
  g->area_active_node = -1;

  // Search if cursor is close to a node
  if(g->valid_nodes_x)
  {
    const float radius_threshold = fabsf(g->nodes_x[1] - g->nodes_x[0]) * 0.45f;
    for(int i = 0; i < CHANNELS; ++i)
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
}


static void area_button_release(GtkGestureSingle *gesture,
                                 gint n_press,
                                 gdouble x,
                                 gdouble y,
                                 dt_iop_module_t *self)
{
  DT_GUARD_GUI_UPDATE();
  if(!self->enabled) return;

  if(gtk_gesture_single_get_current_button(gesture) != GDK_BUTTON_PRIMARY)
    return;

  dt_iop_toneequalizer_gui_data_t *g = self->gui_data;

  // Give focus to module
  dt_iop_request_focus(self);

  const dt_iop_toneequalizer_params_t *p = self->params;

  if(g->area_dragging)
  {
    // Update GUI with new params
    update_exposure_sliders(g, p);

    dt_dev_add_history_item(darktable.develop, self, FALSE);

    dt_iop_gui_enter_critical_section(self);
    g->area_dragging = FALSE;
    dt_iop_gui_leave_critical_section(self);
  }
}

static void area_scroll(GtkEventControllerScroll *controller,
                         gdouble dx,
                         gdouble dy,
                         dt_iop_module_t *self)
{
  // scroll is already filtered by _scroll_sidebar in the proxy,
  // so only valid scroll events on the area widget reach here.
}

static void notebook_button_press(GtkGestureSingle *gesture,
                                    gint n_press,
                                    gdouble x,
                                    gdouble y,
                                    dt_iop_module_t *self)
{
  // Give focus to module
  dt_iop_request_focus(self);

  // Unlock the colour picker so we can display our own custom cursor
  dt_iop_color_picker_reset(self, TRUE);
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

  DT_ENTER_GUI_UPDATE();
  dt_iop_gui_enter_critical_section(self);

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->show_luminance_mask), g->mask_display);
  dt_iop_gui_leave_critical_section(self);
  DT_LEAVE_GUI_UPDATE();
}


static void _develop_preview_pipe_finished_callback(gpointer instance,
                                                    dt_iop_module_t *self)
{
  const dt_iop_toneequalizer_gui_data_t *g = self->gui_data;
  if(g == NULL) return;

  // now that the preview pipe is termintated, set back the distort signal to catch
  // any new changes from a module doing distortion. this signal has been disconnected
  // at the time the DT_SIGNAL_DEVELOP_DISTORT has been handled (see ) and a full
  // reprocess of the preview has been scheduled.
  _set_distort_signal(self);

  switch_cursors(self);
  gtk_widget_queue_draw(GTK_WIDGET(g->area));
}


static void _develop_ui_pipe_finished_callback(gpointer instance,
                                               dt_iop_module_t *self)
{
  const dt_iop_toneequalizer_gui_data_t *g = self->gui_data;
  if(g == NULL) return;
  switch_cursors(self);
}

void gui_reset(dt_iop_module_t *self)
{
  const dt_iop_toneequalizer_gui_data_t *g = self->gui_data;
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

  // Simple view

  self->widget = dt_ui_notebook_page(g->notebook, N_("simple"), NULL);

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

  // Advanced view

  self->widget = dt_ui_notebook_page(g->notebook, N_("advanced"), NULL);

  g->area = GTK_DRAWING_AREA(gtk_drawing_area_new());
  GtkWidget *wrapper = dt_gui_vbox(g->area);
  g_object_set_data(G_OBJECT(wrapper), "iop-instance", self);
  gtk_widget_set_name(GTK_WIDGET(wrapper), "toneeqgraph");
  dt_action_define_iop(self, NULL, N_("graph"), GTK_WIDGET(wrapper), NULL);
  gtk_widget_set_vexpand(GTK_WIDGET(g->area), TRUE);
  gtk_widget_set_can_focus(GTK_WIDGET(g->area), TRUE);
  g_signal_connect(G_OBJECT(g->area), "draw", G_CALLBACK(area_draw), self);
  gtk_widget_add_events(GTK_WIDGET(g->area),
                        GDK_POINTER_MOTION_MASK | GDK_BUTTON_PRESS_MASK
                        | GDK_BUTTON_RELEASE_MASK
                        | GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK
                        | darktable.gui->scroll_mask);
  dt_gui_connect_click(g->area, area_button_press, area_button_release, self);
  dt_gui_connect_motion(g->area, area_motion_notify, NULL, area_leave_notify, self);
  dt_gui_connect_scroll(g->area, GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES
                                        | GTK_EVENT_CONTROLLER_SCROLL_DISCRETE,
                        area_scroll, self);
  gtk_widget_set_tooltip_text(GTK_WIDGET(g->area), _("double-click to reset the curve"));

  g->smoothing = dt_bauhaus_slider_new_with_range(self, -2.33f, +1.67f, 0, 0.0f, 2);
  dt_bauhaus_slider_set_soft_range(g->smoothing, -1.0f, 1.0f);
  dt_bauhaus_widget_set_label(g->smoothing, NULL, N_("curve smoothing"));
  gtk_widget_set_tooltip_text
    (g->smoothing,
     _("positive values will produce more progressive tone transitions\n"
       "but the curve might become oscillatory in some settings.\n"
       "negative values will avoid oscillations and behave more robustly\n"
       "but may produce brutal tone transitions and damage local contrast."));
  g_signal_connect(G_OBJECT(g->smoothing), "value-changed",
                   G_CALLBACK(smoothing_callback), self);
  dt_gui_box_add(self->widget, wrapper, g->smoothing);

  g->exposure_boost = dt_bauhaus_slider_from_params(self, "exposure_boost");
  dt_bauhaus_slider_set_soft_range(g->exposure_boost, -4.0, 4.0);
  dt_bauhaus_slider_set_format(g->exposure_boost, _(" EV"));
  gtk_widget_set_tooltip_text
    (g->exposure_boost,
    _("use this to slide the mask average exposure along channels\n"
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
      "this allows to spread the exposure histogram over more channels\n"
      "for a better control of the exposure correction."));
  dt_bauhaus_widget_set_quad(g->contrast_boost, self, dtgtk_cairo_paint_wand, FALSE, auto_adjust_contrast_boost,
                            _("auto-adjust the contrast"));

  // Masking options

  self->widget = dt_ui_notebook_page(g->notebook, N_("masking"), NULL);

  g->method = dt_bauhaus_combobox_from_params(self, "method");
  gtk_widget_set_tooltip_text
    (g->method,
     _("preview the mask and chose the estimator that gives you the\n"
       "higher contrast between areas to dodge and areas to burn"));

  g->details = dt_bauhaus_combobox_from_params(self, N_("details"));
  gtk_widget_set_tooltip_text
    (g->details,
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


  g->quantization = dt_bauhaus_slider_from_params(self, "quantization");
  dt_bauhaus_slider_set_format(g->quantization, _(" EV"));
  gtk_widget_set_tooltip_text
    (g->quantization,
     _("0 disables the quantization.\n"
       "higher values posterize the luminance mask to help the guiding\n"
       "produce piece-wise smooth areas when using high feathering values"));

  // start building top level widget
  const int active_page = dt_conf_get_int("plugins/darkroom/toneequal/gui_page");
  gtk_widget_show(gtk_notebook_get_nth_page(g->notebook, active_page));
  gtk_notebook_set_current_page(g->notebook, active_page);

  dt_gui_connect_click(g->notebook, notebook_button_press, NULL, self);

  g->show_luminance_mask = dt_iop_togglebutton_new
    (self, NULL,
     N_("display exposure mask"), NULL, G_CALLBACK(show_luminance_mask_callback),
     FALSE, 0, 0, dtgtk_cairo_paint_showmask, NULL);
  dt_gui_add_class(g->show_luminance_mask, "dt_transparent_background");
  dtgtk_togglebutton_set_paint(DTGTK_TOGGLEBUTTON(g->show_luminance_mask),
                               dtgtk_cairo_paint_showmask, 0, NULL);
  dt_gui_add_class(g->show_luminance_mask, "dt_bauhaus_alignment");
  self->widget = dt_gui_vbox(g->notebook,
                             dt_gui_hbox(dt_gui_expand(dt_ui_label_new(_("display exposure mask"))),
                                         g->show_luminance_mask));

  // Force UI redraws when pipe starts/finishes computing and switch cursors
  DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_DEVELOP_PREVIEW_PIPE_FINISHED, _develop_preview_pipe_finished_callback);
  DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_DEVELOP_UI_PIPE_FINISHED, _develop_ui_pipe_finished_callback);
  DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_DEVELOP_HISTORY_CHANGE, _develop_ui_pipe_started_callback);
}

void gui_cleanup(dt_iop_module_t *self)
{
  const dt_iop_toneequalizer_gui_data_t *g = self->gui_data;
  self->request_color_pick = DT_REQUEST_COLORPICK_OFF;

  dt_conf_set_int("plugins/darkroom/toneequal/gui_page",
                  gtk_notebook_get_current_page (g->notebook));

  dt_preview_data_free((dt_preview_data_t *)&g->pd);
  dt_free_align(g->full_preview_buf);

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
