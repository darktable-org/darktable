/*
    This file is part of darktable,
    copyright (c) 2018-2019 Aurélien Pierre.

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
 * This module aims at relighting the scene by performing an exposure compensation
 * selectively on specified exposures octaves, the same way HiFi audio equalizers allow to set
 * a gain for each octave.
 *
 * It is intended to work in scene-linear camera RGB, to behave as if light was physically added
 * or removed from the scene. As such, it should be put before input profile in the pipe, but preferably
 * after exposure. It also need to be placed after the rotation, perspective and cropping modules
 * for the interactive editing to work properly (so the image buffer overlap perfectly with the
 * image preview).
 *
 * Because it works before camera RGB -> XYZ conversion, the exposure cannot be computed from
 * any human-based perceptual colour model (Y channel), hence why several RGB norms are provided as estimators of
 * the pixel energy to compute a luminance map. None of them is perfect, and I'm still
 * looking forward to a real spectral energy estimator. The best physically-accurate norm should be the euclidian
 * norm, but the best looking is often the power norm, which has no theoritical background.
 * The geometric mean also display interesting properties as it interprets saturated colours
 * as low-lights, allowing to lighten and desaturate them in a realistic way.
 *
 * The exposure correction is computed as a series of each octave's gain weighted by the
 * gaussian of the radial distance between the current pixel exposure and each octave's center.
 * This allows for a smooth and continuous infinite-order interpolation, preserving exposure gradients
 * as best as possible. The radius of the kernel is user-defined and can be tweaked to get
 * a smoother interpolation (possibly generating oscillations), or a more monotonous one
 * (possibly less smooth). The actual factors of the gaussian series are computed by
 * solving the linear system taking the user-input parameters as target exposures compensations.
 *
 * Notice that every pixel operation is performed in linear space. The exposures in log2 (EV)
 * are only used for user-input parameters and for the gaussian weights of the radial distance
 * between pixel exposure and octave's centers.
 *
 * The details preservation modes make use of a fast guided filter optimized to perform
 * an edge-aware surface blur on the luminance mask, in the same spirit as the bilateral
 * filter, but without its classic issues of gradient reversal around sharp edges. This
 * surface blur will allow to perform piece-wise smooth exposure compensation, so local contrast
 * will be preserved inside contiguous regions. Various mask refinements are provided to help
 * the edge-taping of the filter (feathering parameter) while keeping smooth contiguous region
 * (quantization parameter), but also to translate (exposure boost) and dilate (contrast boost)
 * the exposure histogram through the control octaves, to center it on the control view
 * and make maximum use of the available channels.
 *
 * Users should be aware that not all the available octaves will be useful on every pictures.
 * Some automatic options will help them to optimize the luminance mask, performing histogram
 * analyse, mapping the average exposure to -4EV, and mapping the first and last deciles of
 * the histogram on its average ± 4EV. These automatic helpers usually fail on X-Trans sensors,
 * maybe because of bad demosaicing, possibly resulting in outliers\negative RGB values.
 * Since they fail the same way on filmic's auto-tuner, we might need to investigate X-Trans
 * algos at some point.
 *
***/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "bauhaus/bauhaus.h"
#include "common/darktable.h"
#include "common/fast_guided_filter.h"
#include "common/interpolation.h"
#include "common/luminance_mask.h"
#include "common/opencl.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "dtgtk/drawingarea.h"
#include "dtgtk/expander.h"
#include "gui/draw.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "gui/color_picker_proxy.h"
#include "iop/iop_api.h"
#include "iop/choleski.h"
#include "iop/gaussian_elimination.h"
#include "libs/colorpicker.h"
#include "common/iop_group.h"

#ifdef _OPENMP
#include <omp.h>
#endif


DT_MODULE_INTROSPECTION(2, dt_iop_toneequalizer_params_t)


/** Note :
 * we use finite-math-only and fast-math because divisions by zero are manually avoided in the code
 * fp-contract=fast enables hardware-accelerated Fused Multiply-Add
 * the rest is loop reorganization and vectorization optimization
 **/
#if defined(__GNUC__)
#pragma GCC optimize ("unroll-loops", "tree-loop-if-convert", \
                      "tree-loop-distribution", "no-strict-aliasing", \
                      "loop-interchange", "loop-nest-optimize", "tree-loop-im", \
                      "unswitch-loops", "tree-loop-ivcanon", "ira-loop-pressure", \
                      "split-ivs-in-unroller", "variable-expansion-in-unroller", \
                      "split-loops", "ivopts", "predictive-commoning",\
                      "tree-loop-linear", "loop-block", "loop-strip-mine", \
                      "finite-math-only", "fp-contract=fast", "fast-math", \
                      "tree-vectorize")
#endif

#define UI_SAMPLES 256 // 128 is a bit small for 4K resolution
#define CONTRAST_FULCRUM exp2f(-4.0f)
#define MIN_FLOAT exp2f(-16.0f)

/**
 * Build the exposures octaves : 
 * band-pass filters with gaussian windows spaced by 1 EV
**/

#define CHANNELS 9
#define PIXEL_CHAN 8

// radial distances used for pixel ops
const float centers_ops[PIXEL_CHAN] DT_ALIGNED_ARRAY = {-56.0f / 7.0f, // = -8.0f
                                                        -48.0f / 7.0f,
                                                        -40.0f / 7.0f,
                                                        -32.0f / 7.0f,
                                                        -24.0f / 7.0f,
                                                        -16.0f / 7.0f,
                                                        -8.0f / 7.0f,
                                                        0.0f / 7.0f}; // split 8 EV into 7 evenly-spaced channels

const float centers_params[CHANNELS] DT_ALIGNED_ARRAY = { -8.0f, -7.0f, -6.0f, -5.0f,
                                                          -4.0f, -3.0f, -2.0f, -1.0f, 0.0f};


typedef enum dt_iop_toneequalizer_filter_t
{
  DT_TONEEQ_NONE = 0,
  DT_TONEEQ_AVG_GUIDED,
  DT_TONEEQ_GUIDED,
} dt_iop_toneequalizer_filter_t;


typedef struct dt_iop_toneequalizer_params_t
{
  float noise, ultra_deep_blacks, deep_blacks, blacks, shadows, midtones, highlights, whites, speculars;
  float blending, smoothing, feathering, quantization, contrast_boost, exposure_boost;
  dt_iop_toneequalizer_filter_t details;
  dt_iop_luminance_mask_method_t method;
  int iterations;
} dt_iop_toneequalizer_params_t;


typedef struct dt_iop_toneequalizer_data_t
{
  float factors[PIXEL_CHAN] DT_ALIGNED_ARRAY;
  float blending, feathering, contrast_boost, exposure_boost, quantization, smoothing;
  float scale;
  int radius;
  int iterations;
  dt_iop_luminance_mask_method_t method;
  dt_iop_toneequalizer_filter_t details;
} dt_iop_toneequalizer_data_t;


typedef struct dt_iop_toneequalizer_global_data_t
{
  // TODO: put OpenCL kernels here at some point
} dt_iop_toneequalizer_global_data_t;


typedef struct dt_iop_toneequalizer_gui_data_t
{
  // Mem arrays 64-bits aligned - contiguous memory
  float factors[PIXEL_CHAN] DT_ALIGNED_ARRAY;
  float gui_lut[UI_SAMPLES] DT_ALIGNED_ARRAY; // LUT for the UI graph
  float interpolation_matrix[(CHANNELS + 1) * PIXEL_CHAN] DT_ALIGNED_ARRAY;
  int histogram[UI_SAMPLES] DT_ALIGNED_ARRAY; // histogram for the UI graph
  float temp_user_params[CHANNELS] DT_ALIGNED_ARRAY;
  float cursor_exposure; // store the exposure value at current cursor position
  float step; // scrolling step

  // 14 int to pack - contiguous memory
  int mask_display;
  int max_histogram;
  int buf_width;
  int buf_height;
  int cursor_pos_x;
  int cursor_pos_y;
  int cursor_valid;         // TRUE if mouse cursor is over the image
  int pipe_order;
  int interpolation_valid;  // TRUE if the interpolation_matrix is ready
  int luminance_valid;      // TRUE if the luminance cache is ready
  int histogram_valid;      // TRUE if the histogram cache is ready
  int lut_valid;            // TRUE if the gui_lut is ready
  int graph_valid;          // TRUE if the UI graph view is ready
  int histo_stats_valid;    // TRUE if histogram average and deciles are ready
  int scrolling;            // TRUE if scrolling events are being captured
  int scroll_increments;    // Accumulate scrolling events
  int user_param_valid;     // TRUE if users params set in interactive view are in bounds

  // 6 uint64 to pack - contiguous-ish memory
  uint64_t hash;
  uint64_t histogram_hash;
  size_t full_preview_buf_width, full_preview_buf_height;
  size_t thumb_preview_buf_width, thumb_preview_buf_height;

  // Misc stuff, contiguity, length and alignment unknown
  float scale;
  float sigma;
  float histogram_average;
  float histogram_first_decile;
  float histogram_last_decile;
  dt_pthread_mutex_t lock;

  // Heap arrays, 64 bits-aligned, unknown length
  float *thumb_preview_buf;
  float *full_preview_buf;

  // GTK garbage, nobody cares, no SIMD here
  GtkWidget *noise, *ultra_deep_blacks, *deep_blacks, *blacks, *shadows, *midtones, *highlights, *whites, *speculars;
  GtkDrawingArea *area, *bar;
  GtkWidget *colorpicker;
  dt_iop_color_picker_t color_picker;
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

} dt_iop_toneequalizer_gui_data_t;


const char *name()
{
  return _("tone equalizer");
}

int default_group()
{
  return IOP_GROUP_BASIC;
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return iop_cs_rgb;
}

int legacy_params(dt_iop_module_t *self, const void *const old_params, const int old_version, void *new_params,
                  const int new_version)
{
  if(old_version == 1 && new_version == 2)
  {
    typedef struct dt_iop_toneequalizer_params_v1_t
    {
      float noise, ultra_deep_blacks, deep_blacks, blacks, shadows, midtones, highlights, whites, speculars;
      float blending, feathering, contrast_boost, exposure_boost;
      dt_iop_toneequalizer_filter_t details;
      int iterations;
      dt_iop_luminance_mask_method_t method;
    } dt_iop_toneequalizer_params_v1_t;

    dt_iop_toneequalizer_params_v1_t *o = (dt_iop_toneequalizer_params_v1_t *)old_params;
    dt_iop_toneequalizer_params_t *n = (dt_iop_toneequalizer_params_t *)new_params;
    dt_iop_toneequalizer_params_t *d = (dt_iop_toneequalizer_params_t *)self->default_params;

    *n = *d; // start with a fresh copy of default parameters

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
    n->quantization = 0.01f;
    n->smoothing = sqrtf(2.0f);
    return 0;
  }

  return 1;
}

void init_presets(dt_iop_module_so_t *self)
{
  dt_iop_toneequalizer_params_t p;
  memset(&p, 0, sizeof(p));

  p.method = DT_TONEEQ_NORM_POWER;
  p.contrast_boost = 0.0f;
  p.details = DT_TONEEQ_NONE;
  p.exposure_boost = 0.0f;
  p.feathering = 1.0f;
  p.iterations = 1;
  p.smoothing = sqrtf(2.0f);
  p.quantization = 0.0f;

  // Init exposure settings
  p.noise = p.ultra_deep_blacks = p.deep_blacks = p.blacks = p.shadows = p.midtones = p.highlights = p.whites = p. speculars = 0.0f;

  // No blending
  dt_gui_presets_add_generic(_("mask blending : none"), self->op, self->version(), &p, sizeof(p), 1);

  // Simple utils blendings
  p.details = DT_TONEEQ_GUIDED;

  p.blending = 12.5f;
  p.feathering = 5.0f;
  p.iterations = 3;
  dt_gui_presets_add_generic(_("mask blending : landscapes"), self->op, self->version(), &p, sizeof(p), 1);

  p.blending = 17.67f;
  p.feathering = 3.53f;
  p.iterations = 2;
  dt_gui_presets_add_generic(_("mask blending : all purposes"), self->op, self->version(), &p, sizeof(p), 1);

  p.blending = 25.0f;
  p.feathering = 2.5f;
  p.iterations = 1;
  dt_gui_presets_add_generic(_("mask blending : isolated subjects"), self->op, self->version(), &p, sizeof(p), 1);

  // Shadows/highlights presets

  p.blending = 12.5f;
  p.iterations = 2;
  p.quantization = 0.25f;
  p.feathering = 5.0f;
  p.method = DT_TONEEQ_NORM_2;
  p.smoothing = sqrtf(2.0f);

  p.noise = 0.1f;
  p.ultra_deep_blacks = 0.1f;
  p.deep_blacks = 0.35f;
  p.blacks = 0.5f;
  p.shadows = 0.75f;
  p.midtones = 0.5f;
  p.highlights = 0.3f;
  p.whites = -0.3f;
  p.speculars = - 0.6f;

  dt_gui_presets_add_generic(_("shadows/highlights : soft"), self->op, self->version(), &p, sizeof(p), 1);

  p.noise = 0.1f;
  p.ultra_deep_blacks = 0.3f;
  p.deep_blacks = 0.65f;
  p.blacks = 0.85f;
  p.shadows = 0.90f;
  p.midtones = 0.7f;
  p.highlights = 0.2f;
  p.whites = -0.6f;
  p.speculars = - 0.9f;

  dt_gui_presets_add_generic(_("shadows/highlights : medium"), self->op, self->version(), &p, sizeof(p), 1);

}


/**
 * Helper functions
 **/


static void hash_set_get(uint64_t *hash_in, uint64_t *hash_out, dt_pthread_mutex_t *lock)
{
  // Set or get a hash in a struct the thread-safe way
  dt_pthread_mutex_lock(lock);
  *hash_out = *hash_in;
  dt_pthread_mutex_unlock(lock);
}


static void invalidate_luminance_cache(dt_iop_module_t *self)
{
  // Invalidate the private luminance cache and histogram when
  // the luminance mask extraction parameters have changed
  dt_iop_toneequalizer_gui_data_t *g = (dt_iop_toneequalizer_gui_data_t *)self->gui_data;

  dt_pthread_mutex_lock(&g->lock);
  g->hash = 0;
  g->histogram_hash = 0;
  g->max_histogram = 1;
  g->luminance_valid = 0;
  g->histogram_valid = 0;
  g->histo_stats_valid = 0;
  dt_pthread_mutex_unlock(&g->lock);
}


static int sanity_check(dt_iop_module_t *self)
{
  // If tone equalizer is put after flip/orientation module,
  // the pixel buffer will be in landscape orientation even for pictures displayed in portrait orientation
  // so the interactive editing will fail. Disable the module and issue a warning then.

  double position_self = self->iop_order;
  double position_min = dt_ioppr_get_iop_order(self->dev->iop_order_list, "flip");

  if(position_self < position_min && self->enabled)
  {
    dt_control_log(_("tone equalizer need to be after distorsion modules in the pipeline – disabled"));
    self->enabled = 0;

    if(self->dev->gui_attached)
    {
      dt_iop_request_focus(self);
      dt_dev_modules_update_multishow(self->dev);
      dt_dev_add_history_item(self->dev, self, FALSE);
      self->enabled = 0; // ensure stupid module is disabled because dt_dev_add_history_item might re-enable it

      // Repaint the on/off icon
      if(self->off)
      {
        const int reset = darktable.gui->reset;
        darktable.gui->reset = 1;
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->off), self->enabled);
        darktable.gui->reset = reset;
      }
    }
    return 0;
  }

  return 1;
}

__DT_CLONE_TARGETS__
static float get_luminance_from_buffer(const float *const buffer,
                                       const size_t width, const size_t height,
                                       const size_t x, const size_t y)
{
  // Get the weighted average luminance of the 3×3 pixels region centered in (x, y)
  // x and y are ratios in [0, 1] of the width and height

  if(y >= height || x >= width) return NAN;

  const size_t y_abs[3] = { CLAMP(y - 1, 0, height - 1),    // previous line
                            y,                              // center line
                            CLAMP(y + 1, 0, height - 1) };  // next line

  const size_t x_abs[3] = { CLAMP(x - 1, 0, width - 1),     // previous column
                            x,                              // center column
                            CLAMP(x + 1, 0, width - 1) };   // next column

  // gaussian-ish kernel - sum is == 1.0f so we don't care much about actual coeffs
  const float gauss_kernel[3][3] DT_ALIGNED_ARRAY =
                                   { { 0.076555024f, 0.124401914f, 0.076555024f },
                                     { 0.124401914f, 0.196172249f, 0.124401914f },
                                     { 0.076555024f, 0.124401914f, 0.076555024f } };

  float luminance = 0.0f;

  // convolution
  for(int i = 0; i < 3; ++i)
    for(int j = 0; j < 3; ++j)
      luminance += buffer[width * y_abs[i] + x_abs[j]] * gauss_kernel[i][j];

  return luminance;
}


/***
 * Exposure compensation computation
 *
 * Construct the final correction factor by summing the octaves channels gains weighted by
 * the gaussian of the radial distance (pixel exposure - octave center)
 *
 ***/

#ifdef _OPENMP
#pragma omp declare simd
#endif
__DT_CLONE_TARGETS__
static float gaussian_denom(const float sigma)
{
  // Gaussian function denominator such that y = exp(- radius^2 / denominator)
  // this is the constant factor of the exponential, so we don't need to recompute it
  // for every single pixel
  return 2.0f * sigma * sigma;
}


#ifdef _OPENMP
#pragma omp declare simd
#endif
__DT_CLONE_TARGETS__
static float gaussian_func(const float radius, const float denominator)
{
  // Gaussian function without normalization
  // this is the variable part of the exponential
  // the denominator should be evaluated with `gaussian_denom`
  // ahead of the array loop for optimal performance
  return expf(- radius * radius / denominator);
}

__DT_CLONE_TARGETS__
static inline void compute_correction(const float *const restrict luminance,
                                      float *const restrict correction,
                                      const float *const restrict factors,
                                      const float sigma,
                                      const size_t num_elem)
{
  const float gauss_denom = gaussian_denom(sigma);

#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static) \
  dt_omp_firstprivate(correction, num_elem, luminance, factors, centers_ops, gauss_denom)
#endif
  for(size_t k = 0; k < num_elem; ++k)
  {
    // build the correction for the current pixel
    // as the sum of the contribution of each luminance channelcorrection
    float result = 0.0f;
    const float exposure = log2f(luminance[k]);

#ifdef _OPENMP
#pragma omp simd aligned(luminance, centers_ops, factors:64) safelen(PIXEL_CHAN) reduction(+:result)
#endif
    for(int i = 0; i < PIXEL_CHAN; ++i)
      result += gaussian_func(exposure - centers_ops[i], gauss_denom) * factors[i];

    correction[k] = result;
  }
}


__DT_CLONE_TARGETS__
static float pixel_correction(const float exposure,
                              const float *const restrict factors,
                              const float sigma)
{
  // build the correction for the current pixel
  // as the sum of the contribution of each luminance channel
  float result = 0.0f;
  const float gauss_denom = gaussian_denom(sigma);

#ifdef _OPENMP
#pragma omp simd aligned(centers_ops, factors:64) safelen(PIXEL_CHAN) reduction(+:result)
#endif
  for(int i = 0; i < PIXEL_CHAN; ++i)
    result += gaussian_func(exposure - centers_ops[i], gauss_denom) * factors[i];

  return result;
}


__DT_CLONE_TARGETS__
static void compute_lut_correction(struct dt_iop_toneequalizer_gui_data_t *g,
                                   const float offset,
                                   const float scaling)
{
  // Compute the LUT of the exposure corrections in EV,
  // offset and scale it for display in GUI widget graph

  float *const restrict LUT = g->gui_lut;
  const float *const restrict factors = g->factors;
  const float sigma = g->sigma;

#ifdef _OPENMP
#pragma omp parallel for simd schedule(static) default(none) \
  dt_omp_firstprivate(factors, sigma, offset, scaling, LUT) \
  aligned(LUT, factors:64)
#endif
  for(int k = 0; k < UI_SAMPLES; k++)
  {
    // build the inset graph curve LUT
    // the x range is [-14;+2] EV
    const float x = (8.0f * (((float)k) / ((float)(UI_SAMPLES - 1)))) - 8.0f;
    LUT[k] = offset - log2f(pixel_correction(x, factors, sigma)) / scaling;
  }
}


__DT_CLONE_TARGETS__
static inline void compute_luminance_mask(const float *const restrict in, float *const restrict luminance,
                                          const size_t width, const size_t height, const size_t ch,
                                          const dt_iop_toneequalizer_data_t *const d)
{
  switch(d->details)
  {
    case(DT_TONEEQ_NONE):
    {
      // No contrast boost here
      luminance_mask(in, luminance, width, height, ch, d->method, d->exposure_boost, 0.0f, 1.0f);
      break;
    }

    case(DT_TONEEQ_AVG_GUIDED):
    {
      // Still no contrast boost
      luminance_mask(in, luminance, width, height, ch, d->method, d->exposure_boost, 0.0f, 1.0f);
      fast_guided_filter(luminance, width, height, d->radius, d->feathering, d->iterations,
                    DT_GF_BLENDING_GEOMEAN, d->scale, d->quantization, exp2f(-8.0f), 1.0f);
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
      luminance_mask(in, luminance, width, height, ch, d->method, d->exposure_boost,
                      CONTRAST_FULCRUM, d->contrast_boost);
      fast_guided_filter(luminance, width, height, d->radius, d->feathering, d->iterations,
                    DT_GF_BLENDING_LINEAR, d->scale, d->quantization, exp2f(-8.0f), 1.0f);
      break;
    }

    default:
    {
      luminance_mask(in, luminance, width, height, ch, d->method, d->exposure_boost, 0.0f, 1.0f);
      break;
    }
  }
}


/***
 * Histogram computations and stats
 ***/


__DT_CLONE_TARGETS__
static inline void compute_log_histogram(const float *const restrict luminance,
                                          int *const restrict histogram,
                                          const size_t num_elem,
                                          int *max_histogram)
{
  // Compute an histogram of exposures, in log
  int temp_max_histogram = 0;

  // (Re)init the histogram
#ifdef _OPENMP
#pragma omp for simd schedule(simd:static) aligned(histogram:64)
#endif
  for(int k = 0; k < UI_SAMPLES; k++)
    histogram[k] = 0;

  // Split exposure in bins
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(simd:static) \
  dt_omp_firstprivate(luminance, histogram, num_elem) \
  shared(temp_max_histogram)
#endif
  for(size_t k = 0; k < num_elem; k++)
  {
    // the histogram shows bins between [-14; +2] EV remapped between [0 ; UI_SAMPLES[
    const int index = CLAMP((int)(((log2f(luminance[k]) + 8.0f) / 8.0f) * (float)UI_SAMPLES), 0, UI_SAMPLES - 1);
    histogram[index] += 1;

    // store the max numbers of elements in bins for later normalization
    temp_max_histogram = (histogram[index] > temp_max_histogram) ? histogram[index] : temp_max_histogram;
  }

  *max_histogram = temp_max_histogram;
}

__DT_CLONE_TARGETS__
static inline float flat_pseudo_norm(const float *const restrict image, const size_t num_elem)
{
  // 0.5 Pseudo-norm a flat series of data
  // it's better than a simple average to get the "barycenter" of a luminance distribution
  // that is non evenly-spaced around its mean, but has a tail in low-lights
  // don't look for the theoritical background here, there is none.
  float mean = 0.0f;

#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
dt_omp_firstprivate(image, num_elem) \
schedule(static) aligned(image:64) reduction(+:mean)
#endif
    for(size_t k = 0; k < num_elem; k++)
      mean += sqrtf(fmaxf(image[k], 0.0f));

  mean /= (float)num_elem;
  return mean * mean;
}


__DT_CLONE_TARGETS__
static inline float flat_average(const float *const restrict image, const size_t num_elem)
{
  // Simple average of a flat series of data
  float mean = 0.0f;

#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
dt_omp_firstprivate(image, num_elem) \
schedule(static) aligned(image:64) reduction(+:mean)
#endif
    for(size_t k = 0; k < num_elem; k++)
      mean += image[k];

  return mean / (float)num_elem;
}


static void histogram_deciles(const int *const restrict histogram, size_t hist_bins, size_t num_elem,
                              const float hist_span, const float hist_offset,
                              float *first_decile, float *last_decile)
{
  // Browse an histogram of `hist_bins` bins containing a population of `num_elems` elements
  // spanning from `hist_offset` to `hist_offset + hist_span`,
  // looking for the position of the first and last deciles,
  // and return their values scaled in the corresponding span

  const int first = (int)((float)num_elem * 0.1f);
  const int last = (int)((float)num_elem * 0.9f);
  int population = 0;
  int first_pos = 0;
  int last_pos = 0;

  // scout the histogram bins looking for deciles
  for(size_t k = 0; k < hist_bins; ++k)
  {
    const size_t prev_population = population;
    population += histogram[k];
    if(prev_population < first && first <= population) first_pos = k;
    if(prev_population < last && last <= population) last_pos = k;
  }

  // Convert bins positions to exposures
  *first_decile = (hist_span * (((float)first_pos) / ((float)(hist_bins - 1)))) + hist_offset;
  *last_decile = (hist_span * (((float)last_pos) / ((float)(hist_bins - 1)))) + hist_offset;
}


static void update_histogram_stats(struct dt_iop_toneequalizer_gui_data_t *g)
{
  if(g == NULL) return;

  dt_pthread_mutex_lock(&g->lock);
  if(!g->histo_stats_valid && g->histogram_valid)
  {
    const size_t num_elem = g->thumb_preview_buf_height * g->thumb_preview_buf_width;
    histogram_deciles(g->histogram, UI_SAMPLES, num_elem, 8.0f, -8.0f,
                      &g->histogram_first_decile, &g->histogram_last_decile);
    g->histogram_average = log2f(flat_pseudo_norm(g->thumb_preview_buf, num_elem));
    g->histo_stats_valid = 1;
  }
  dt_pthread_mutex_unlock(&g->lock);
}


static void update_histogram(struct dt_iop_toneequalizer_gui_data_t *g)
{
  if(g == NULL) return;

  dt_pthread_mutex_lock(&g->lock);
  if(!g->histogram_valid && g->luminance_valid)
  {
    const size_t num_elem = g->thumb_preview_buf_height * g->thumb_preview_buf_width;
    compute_log_histogram(g->thumb_preview_buf, g->histogram, num_elem, &g->max_histogram);
    g->histogram_valid = 1;
  }
  dt_pthread_mutex_unlock(&g->lock);
}


static void update_curve_lut(struct dt_iop_toneequalizer_gui_data_t *g)
{
  if(g == NULL) return;

  dt_pthread_mutex_lock(&g->lock);
  if(!g->lut_valid && g->interpolation_valid)
  {
    compute_lut_correction(g, 0.5f, 4.0f);
    g->lut_valid = 1;
  }
  dt_pthread_mutex_unlock(&g->lock);
}


/***
 * Actual transfer functions
 **/

__DT_CLONE_TARGETS__
static inline void display_luminance_mask(const float *const restrict luminance,
                                          float *const restrict out,
                                          const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out,
                                          const size_t ch)
{
  const size_t offset_x = (roi_in->x < roi_out->x) ? -roi_in->x + roi_out->x : 0;
  const size_t offset_y = (roi_in->y < roi_out->y) ? -roi_in->y + roi_out->y : 0;

  // The output dimensions need to be smaller or equal to the input ones
  // there is no logical reason they shouldn't, except some weird bug in the pipe
  // in this case, ensure we don't segfault
  const size_t in_width = roi_in->width;
  const size_t out_width = (roi_in->width > roi_out->width) ? roi_out->width : roi_in->width;
  const size_t out_height = (roi_in->height > roi_out->height) ? roi_out->height : roi_in->height;

#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
dt_omp_firstprivate(luminance, out, in_width, out_width, out_height, offset_x, offset_y, ch) \
schedule(static) aligned(luminance, out:64) collapse(3)
#endif
  for(size_t i = 0 ; i < out_height; ++i)
    for(size_t j = 0; j < out_width; ++j)
      for(size_t c = 0; c < ch; ++c)
        out[(i * out_width + j) * ch + c] = luminance[(i + offset_y) * in_width  + (j + offset_x)];
}


__DT_CLONE_TARGETS__
static inline void apply_exposure(const float *const restrict in, float *const restrict out,
                                  const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out,
                                  const size_t ch,
                                  const float *const restrict correction)
{
  const size_t offset_x = (roi_in->x < roi_out->x) ? -roi_in->x + roi_out->x : 0;
  const size_t offset_y = (roi_in->y < roi_out->y) ? -roi_in->y + roi_out->y : 0;

  // The output dimensions need to be smaller or equal to the input ones
  // there is no logical reason they shouldn't, except some weird bug in the pipe
  // in this case, ensure we don't segfault
  const size_t in_width = roi_in->width;
  const size_t out_width = (roi_in->width > roi_out->width) ? roi_out->width : roi_in->width;
  const size_t out_height = (roi_in->height > roi_out->height) ? roi_out->height : roi_in->height;

#ifdef _OPENMP
#pragma omp parallel for simd default(none) schedule(static) \
  dt_omp_firstprivate(in, out, in_width, out_height, out_width, offset_x, offset_y, ch, correction) \
  aligned(in, out, correction:64) collapse(3)
#endif
  for(size_t i = 0 ; i < out_height; ++i)
    for(size_t j = 0; j < out_width; ++j)
      for(size_t c = 0; c < ch; ++c)
        out[(i * out_width + j) * ch + c] = in[((i + offset_y) * in_width + (j + offset_x)) * ch + c] *
                                            correction[(i + offset_y) * in_width + (j + offset_x)];
}


__DT_CLONE_TARGETS__
static inline void apply_toneequalizer(const float *const restrict in,
                                       const float *const restrict luminance,
                                       float *const restrict out,
                                       const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out,
                                       const size_t ch,
                                       const dt_iop_toneequalizer_data_t *const d)
{
  size_t num_elem = roi_in->width * roi_in->height;
  float *const restrict correction = dt_alloc_sse_ps(dt_round_size_sse(num_elem));

  if(correction)
  {
    compute_correction(luminance, correction, d->factors, d->smoothing, num_elem);
    apply_exposure(in, out, roi_in, roi_out, ch, correction);
    dt_free_align(correction);
  }
  else
  {
    dt_control_log(_("tone equalizer failed to allocate memory, check your RAM settings"));
    return;
  }
}


__DT_CLONE_TARGETS__
void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece,
             const void *const restrict ivoid, void *const restrict ovoid,
             const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_toneequalizer_data_t *const d = (const dt_iop_toneequalizer_data_t *const)piece->data;
  dt_iop_toneequalizer_gui_data_t *const g = (dt_iop_toneequalizer_gui_data_t *)self->gui_data;

  const float *const restrict in = __builtin_assume_aligned(dt_check_sse_aligned((float *const)ivoid), 64);
  float *const restrict out = __builtin_assume_aligned(dt_check_sse_aligned((float *const)ovoid), 64);
  float *restrict luminance = NULL;

  if(in == NULL || out == NULL)
  {
    // Pointers are not 64-bits aligned, and SSE code will segfault
    dt_control_log(_("tone equalizer in/out buffer are ill-aligned, please report the bug to the developpers"));
    fprintf(stdout, "tone equalizer in/out buffer are ill-aligned, please report the bug to the developpers\n");
    return;
  }

  const size_t width = roi_in->width;
  const size_t height = roi_in->height;
  const size_t num_elem = width * height;
  const size_t num_elem_aligned = dt_round_size_sse(num_elem);
  const size_t ch = 4;

  // Get the hash of the upstream pipe to track changes
  int position = self->iop_order;
  uint64_t hash = dt_dev_pixelpipe_cache_hash(piece->pipe->image.id, roi_out, piece->pipe, position);

  // Sanity checks
  if(width < 1 || height < 1) return;
  if(width < roi_out->width || height < roi_out->height) return;
  if(piece->colors != 4) return;

  // Init the luminance masks buffers
  int cached = FALSE;

  if(self->dev->gui_attached)
  {
    // If the module instance has changed order in the pipe, invalidate the caches
    if(g->pipe_order != position)
    {
      dt_pthread_mutex_lock(&g->lock);
      g->hash = 0;
      g->histogram_hash = 0;
      g->pipe_order = position;
      g->luminance_valid = 0;
      g->histogram_valid = 0;
      g->histo_stats_valid = 0;
      dt_pthread_mutex_unlock(&g->lock);
    }

    if(piece->pipe->type == DT_DEV_PIXELPIPE_FULL)
    {
      // For DT_DEV_PIXELPIPE_FULL, we cache the luminance mask for performance
      // but it's not accessed from GUI
      // no need for threads lock since no other function is writing/reading that buffer

      // Re-allocate a new buffer if the full preview size has changed
      if(g->full_preview_buf_width != width || g->full_preview_buf_height != height)
      {
        if(g->full_preview_buf) dt_free_align(g->full_preview_buf);
        g->full_preview_buf = dt_alloc_sse_ps(num_elem_aligned);
        g->full_preview_buf_width = width;
        g->full_preview_buf_height = height;
      }

      luminance = g->full_preview_buf;
      cached = TRUE;
    }

    else if(piece->pipe->type == DT_DEV_PIXELPIPE_PREVIEW)
    {
      // For DT_DEV_PIXELPIPE_PREVIEW, we need to cache is too to compute the full image stats
      // upon user request in GUI
      // threads locks are required since GUI reads and writes on that buffer.

      // Re-allocate a new buffer if the thumb preview size has changed
      dt_pthread_mutex_lock(&g->lock);
      if(g->thumb_preview_buf_width != width || g->thumb_preview_buf_height != height)
      {
        if(g->thumb_preview_buf) dt_free_align(g->thumb_preview_buf);
        g->thumb_preview_buf = dt_alloc_sse_ps(num_elem_aligned);
        g->thumb_preview_buf_width = width;
        g->thumb_preview_buf_height = height;
        g->luminance_valid = 0;
      }

      luminance = g->thumb_preview_buf;
      cached = TRUE;

      dt_pthread_mutex_unlock(&g->lock);
    }
    else // just to please GCC
    {
      luminance = dt_alloc_sse_ps(num_elem_aligned);
    }

  }
  else
  {
    // no interactive editing/caching : just allocate a local temp buffer
    luminance = dt_alloc_sse_ps(num_elem_aligned);
  }

  // Check if the luminance buffer exists
  if(!luminance)
  {
    dt_control_log(_("tone equalizer failed to allocate memory, check your RAM settings"));
    return;
  }

  if(!((size_t)luminance & 64))
  {
    // Pointers are not 64-bits aligned, and SSE code will segfault
    dt_control_log(_("tone equalizer luminance buffer is ill-aligned, please report the bug to the developpers"));
    return;
  }

  // Compute the luminance mask
  if(cached)
  {
    // caching path : store the luminance mask for GUI access

    if(piece->pipe->type == DT_DEV_PIXELPIPE_FULL)
    {
      uint64_t saved_hash;
      hash_set_get(&g->hash, &saved_hash, &g->lock);

      dt_pthread_mutex_lock(&g->lock);
      const int luminance_valid = g->luminance_valid;
      dt_pthread_mutex_unlock(&g->lock);

      if(hash != saved_hash || !luminance_valid)
      {
        /* compute only if upstream pipe state has changed */
        compute_luminance_mask(in, luminance, width, height, ch, d);
        hash_set_get(&hash, &g->hash, &g->lock);
      }
    }

    else if(piece->pipe->type == DT_DEV_PIXELPIPE_PREVIEW)
    {
      uint64_t saved_hash;
      hash_set_get(&g->histogram_hash, &saved_hash, &g->lock);

      dt_pthread_mutex_lock(&g->lock);
      const int luminance_valid = g->luminance_valid;
      dt_pthread_mutex_unlock(&g->lock);

      if(saved_hash != hash || !luminance_valid)
      {
        /* compute only if upstream pipe state has changed */
        dt_pthread_mutex_lock(&g->lock);
        g->histogram_hash = hash;
        g->histogram_valid = 0;
        g->histo_stats_valid = 0;
        compute_luminance_mask(in, luminance, width, height, ch, d);
        g->luminance_valid = 1;
        dt_pthread_mutex_unlock(&g->lock);
      }
    }

    else // make it dummy-proof
    {
      compute_luminance_mask(in, luminance, width, height, ch, d);
    }
  }
  else
  {
    // no caching path : compute no matter what
    compute_luminance_mask(in, luminance, width, height, ch, d);
  }

  // Display output
  if(self->dev->gui_attached && piece->pipe->type == DT_DEV_PIXELPIPE_FULL)
  {
    if(g->mask_display)
      display_luminance_mask(luminance, out, roi_in, roi_out, ch);
    else
      apply_toneequalizer(in, luminance, out, roi_in, roi_out, ch, d);
  }
  else
  {
    apply_toneequalizer(in, luminance, out, roi_in, roi_out, ch, d);
  }

  if(!cached) dt_free_align(luminance);

  if(piece->pipe->mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK) dt_iop_alpha_copy(in, out, roi_out->width, roi_out->height);
}



void modify_roi_in(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                   const dt_iop_roi_t *roi_out, dt_iop_roi_t *roi_in)
{
  // Pad the zoomed-in view to avoid weird stuff with local averages at the borders of
  // the preview

  dt_iop_toneequalizer_data_t *const d = (dt_iop_toneequalizer_data_t *const)piece->data;

  // Get the scaled window radius for the box average
  const int max_size = (piece->iwidth > piece->iheight) ? piece->iwidth : piece->iheight;
  const float diameter = d->blending * max_size * roi_in->scale;
  int radius = (int)((diameter - 1.0f) / ( 2.0f));
  d->radius = radius;

  // Enlarge the preview roi with padding if needed
  if(self->dev->gui_attached)
  {
    int roiy = fmaxf(roi_in->y - radius, 0.0f);
    int roix = fmaxf(roi_in->x - radius, 0.0f);
    int roir = fminf(roix + roi_in->width + 2 * radius, piece->buf_in.width * roi_in->scale);
    int roib = fminf(roiy + roi_in->height + 2 * radius, piece->buf_in.height * roi_in->scale);

    // Set the values and check
    roi_in->x = roix;
    roi_in->y = roiy;
    roi_in->width = roir - roi_in->x;
    roi_in->height = roib - roi_in->y;
  }
}



/***
 * Setters and Getters for parameters
 ***/

static void get_channels_factors(float factors[CHANNELS], const dt_iop_toneequalizer_params_t *p)
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

  // Convert from EV offsets to linear factors
#ifdef _OPENMP
#pragma omp simd aligned(factors:64)
#endif
  for(int c = 0; c < CHANNELS; ++c)
    factors[c] = exp2f(factors[c]);
}


__DT_CLONE_TARGETS__
static int compute_channels_factors(const float factors[PIXEL_CHAN], float out[CHANNELS], const float sigma)
{
  // Input factors are the weights for the radial-basis curve approximation of user params
  // Output factors are the gains of the user parameters channels
  // aka the y coordinates of the approximation for x = { CHANNELS }
  assert(PIXEL_CHAN == 8);

  int valid = 1;

  #ifdef _OPENMP
  #pragma omp parallel for simd default(none) schedule(static) \
    aligned(factors, out, centers_params:64) dt_omp_firstprivate(factors, out, sigma, centers_params) shared(valid)
  #endif
  for(int i = 0; i < CHANNELS; ++i)
  {
     // Compute the new channels factors
    out[i] = pixel_correction(centers_params[i], factors, sigma);

    // check they are in [-2, 2] EV and not NAN
    if(out[i] < 0.25f || out[i] > 4.0f || out[i] != out[i]) valid = 0;
  }

  return valid;
}

__DT_CLONE_TARGETS__
static int compute_channels_gains(const float in[CHANNELS], float out[CHANNELS])
{
  assert(PIXEL_CHAN == 8);

  int valid = 1;

  for(int i = 0; i < CHANNELS; ++i)
  {
     // Compute the new channels gains
    out[i] = log2f(in[i]);
  }

  return valid;
}


static int commit_channels_gains(const float factors[CHANNELS], dt_iop_toneequalizer_params_t *p)
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

  return 1;
}


static inline void build_interpolation_matrix(float A[(CHANNELS + 1) * PIXEL_CHAN],
                                              const float sigma)
{
  // Build the symmetrical definite positive part of the augmented matrix
  // of the radial-basis interpolation weights

  const float gauss_denom = gaussian_denom(sigma);

#ifdef _OPENMP
#pragma omp simd aligned(A, centers_ops, centers_params:64) collapse(2)
#endif
  for(int i = 0; i < CHANNELS; ++i)
    for(int j = 0; j < PIXEL_CHAN; ++j)
      A[i * PIXEL_CHAN + j] = gaussian_func(centers_params[i] - centers_ops[j], gauss_denom);

  // Remember there is one free row at the end of A for the radial approximation below
}


__DT_CLONE_TARGETS__
static inline int radial_approximation(float factors[CHANNELS + 1],
                                       const float exposure_correction,
                                       dt_iop_toneequalizer_gui_data_t *g)
{
  // Perform a radial-based approximation of already set user parameters (in *p) and one new value
  // returns in-place the coefficients of each gaussian kernel to fit the input data in factors
  // the valid elements of output factors are from 0 to PIXEL_CHAN

  const float gauss_denom = gaussian_denom(g->sigma);
  int valid = 1;

  // Rebuild the interpolation matrix if needed
  if(!g->interpolation_valid)
  {
    build_interpolation_matrix(g->interpolation_matrix, g->sigma);
    g->lut_valid = 0;
    g->interpolation_valid = 1;
  }

  float *const A DT_ALIGNED_ARRAY = g->interpolation_matrix;
  const float exposure_in = g->cursor_exposure;

#ifdef _OPENMP
#pragma omp parallel sections
#endif
  {
    #ifdef _OPENMP
    #pragma omp section
    #endif
    {
      // Append the current pixel exposures weights to the interpolation matrix
      #ifdef _OPENMP
      #pragma omp simd aligned(A, centers_ops:64)
      #endif
      for(int j = 0; j < PIXEL_CHAN; ++j)
          A[CHANNELS * PIXEL_CHAN + j] = gaussian_func(exposure_in - centers_ops[j], gauss_denom);
    }

    #ifdef _OPENMP
    #pragma omp section
    #endif
    {
      // Build the y augmented vector and append the desired exposure out for current pixel
      dt_simd_memcpy(g->temp_user_params, factors, CHANNELS);
      factors[CHANNELS] = exp2f(exposure_correction);
    }
  }

  // Solve by least-squares the linear system A for the weights of the radial-basis approximation
  // and put them in factors[]
  if(valid) valid = pseudo_solve(A, factors, CHANNELS + 1, PIXEL_CHAN, 1);

  return valid;
}


void init_global(dt_iop_module_so_t *module)
{
  dt_iop_toneequalizer_global_data_t *gd
      = (dt_iop_toneequalizer_global_data_t *)malloc(sizeof(dt_iop_toneequalizer_global_data_t));

  module->data = gd;
}


void cleanup_global(dt_iop_module_so_t *module)
{
  free(module->data);
  module->data = NULL;
}


void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_toneequalizer_params_t *p = (dt_iop_toneequalizer_params_t *)p1;
  dt_iop_toneequalizer_data_t *d = (dt_iop_toneequalizer_data_t *)piece->data;
  dt_iop_toneequalizer_gui_data_t *g = (dt_iop_toneequalizer_gui_data_t *)self->gui_data;

  sanity_check(self);

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

  // UI params are in log2 offsets (EV) : convert to linear factors
  d->contrast_boost = exp2f(p->contrast_boost);
  d->exposure_boost = exp2f(p->exposure_boost);

  // Get the gains for channels parameters and convert them to linear factors
  float factors[CHANNELS] DT_ALIGNED_ARRAY;
  get_channels_factors(factors, p);

  int valid;

  /*
   * Perform a radial-based interpolation using a series gaussian functions
   */
  if(self->dev->gui_attached && g)
  {
    dt_pthread_mutex_lock(&g->lock);
    if(!g->interpolation_valid || g->sigma != p->smoothing)
    {
      // invalidate caches and recompute the interpolation matrice
      g->lut_valid = 0;
      build_interpolation_matrix(g->interpolation_matrix, p->smoothing);
      g->interpolation_valid = 1;
      g->sigma = p->smoothing;
    }

    // copy user params locally for interactive editing
    dt_simd_memcpy(factors, g->temp_user_params, CHANNELS);
    g->user_param_valid = 1;

    // Solve
    valid = pseudo_solve(g->interpolation_matrix, factors, CHANNELS, PIXEL_CHAN, 1);
    dt_pthread_mutex_unlock(&g->lock);
  }
  else
  {
    // No cache : Build / Solve interpolation matrix
    float A[(CHANNELS + 1) * PIXEL_CHAN] DT_ALIGNED_ARRAY;
    build_interpolation_matrix(A, p->smoothing);
    valid = pseudo_solve(A, factors, CHANNELS, PIXEL_CHAN, 1);
  }

  if(valid)
  {
    // solution is valid : accept the factors in pipe
    dt_simd_memcpy(factors, d->factors, PIXEL_CHAN);

    if(self->dev->gui_attached)
    {
      dt_pthread_mutex_lock(&g->lock);
      int factors_changed = 0;
      for(int i = 0; i < PIXEL_CHAN; ++i)
        if(factors[i] != g->factors[i])
          factors_changed = 1;

      if(factors_changed)
      {
        // flush GUI caches if factors changed
        dt_simd_memcpy(factors, g->factors, PIXEL_CHAN);
        g->lut_valid = 0;
      }
      dt_pthread_mutex_unlock(&g->lock);
    }
  }
  else if(self->dev->gui_attached)
  {
    // solution is invalid : input default params
    const float fallback_factors[PIXEL_CHAN] DT_ALIGNED_ARRAY = { 1.0f };
    dt_simd_memcpy(fallback_factors, d->factors, PIXEL_CHAN);
    dt_control_log(_("tone equalizer : the interpolation is unstable, check the smoothing parameter"));
  }
}


void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1, sizeof(dt_iop_toneequalizer_data_t));
}


void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void init(dt_iop_module_t *module)
{
  module->params = calloc(1, sizeof(dt_iop_toneequalizer_params_t));
  module->default_params = calloc(1, sizeof(dt_iop_toneequalizer_params_t));
  module->default_enabled = 0;
  module->params_size = sizeof(dt_iop_toneequalizer_params_t);
  module->gui_data = NULL;

  dt_iop_toneequalizer_params_t tmp = (dt_iop_toneequalizer_params_t){.noise = 0.0f,
                                                                      .ultra_deep_blacks = 0.0f,
                                                                      .deep_blacks = 0.0f,
                                                                      .blacks = 0.0f,
                                                                      .shadows = 0.0f,
                                                                      .midtones = 0.0f,
                                                                      .highlights = 0.0f,
                                                                      .whites = 0.0f,
                                                                      .speculars = 0.0f,
                                                                      .quantization = 0.0f,
                                                                      .smoothing = sqrtf(2.0f),
                                                                      .iterations = 1,
                                                                      .method = DT_TONEEQ_NORM_2,
                                                                      .details = DT_TONEEQ_GUIDED,
                                                                      .blending = 12.5f,
                                                                      .feathering = 5.0f,
                                                                      .contrast_boost = 0.0f,
                                                                      .exposure_boost = 0.0f };
  memcpy(module->params, &tmp, sizeof(dt_iop_toneequalizer_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_toneequalizer_params_t));
}

void cleanup(dt_iop_module_t *module)
{
  free(module->params);
  module->params = NULL;
}

void reload_defaults(struct dt_iop_module_t *self)
{
  dt_iop_toneequalizer_gui_data_t *g = (dt_iop_toneequalizer_gui_data_t *)self->gui_data;

  if(g == NULL) return;

  // Reset all caches for safety
  dt_pthread_mutex_lock(&g->lock);
  g->hash = 0;
  g->histogram_hash = 0;
  g->max_histogram = 1;
  g->scale = 0.0f;
  g->sigma = 0.0f;
  g->interpolation_valid = 0;
  g->histogram_valid = 0;
  g->luminance_valid = 0;
  g->lut_valid = 0;
  g->graph_valid = 0;
  g->histo_stats_valid = 0;
  g->mask_display = 0;
  g->scrolling = 0;
  g->scroll_increments = 0;
  g->user_param_valid = 0;

  g->full_preview_buf = NULL;
  g->full_preview_buf_width = 0;
  g->full_preview_buf_height = 0;

  g->thumb_preview_buf = NULL;
  g->thumb_preview_buf_width = 0;
  g->thumb_preview_buf_height = 0;

  g->desc = NULL;
  g->layout = NULL;
  g->cr = NULL;
  g->cst = NULL;
  g->context = NULL;

  g->pipe_order = 0;
  dt_pthread_mutex_unlock(&g->lock);

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}


void show_guiding_controls(struct dt_iop_module_t *self)
{
  dt_iop_module_t *module = (dt_iop_module_t *)self;
  dt_iop_toneequalizer_gui_data_t *g = (dt_iop_toneequalizer_gui_data_t *)self->gui_data;
  const dt_iop_toneequalizer_params_t *p = (const dt_iop_toneequalizer_params_t *)module->params;

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
    {
      gtk_widget_set_visible(g->blending, TRUE);
      gtk_widget_set_visible(g->feathering, TRUE);
      gtk_widget_set_visible(g->iterations, TRUE);
      gtk_widget_set_visible(g->contrast_boost, FALSE);
      gtk_widget_set_visible(g->quantization, TRUE);
      break;
    }

    case(DT_TONEEQ_GUIDED):
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

static gboolean _init_drawing(GtkWidget *widget, dt_iop_toneequalizer_gui_data_t *g);

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_module_t *module = (dt_iop_module_t *)self;
  dt_iop_toneequalizer_gui_data_t *g = (dt_iop_toneequalizer_gui_data_t *)self->gui_data;
  dt_iop_toneequalizer_params_t *p = (dt_iop_toneequalizer_params_t *)module->params;

  dt_bauhaus_slider_set(g->noise, p->noise);
  dt_bauhaus_slider_set(g->ultra_deep_blacks, p->ultra_deep_blacks);
  dt_bauhaus_slider_set(g->deep_blacks, p->deep_blacks);
  dt_bauhaus_slider_set(g->blacks, p->blacks);
  dt_bauhaus_slider_set(g->shadows, p->shadows);
  dt_bauhaus_slider_set(g->midtones, p->midtones);
  dt_bauhaus_slider_set(g->highlights, p->highlights);
  dt_bauhaus_slider_set(g->whites, p->whites);
  dt_bauhaus_slider_set(g->speculars, p->speculars);

  dt_bauhaus_combobox_set(g->method, p->method);
  dt_bauhaus_combobox_set(g->details, p->details);
  dt_bauhaus_slider_set_soft(g->blending, p->blending);
  dt_bauhaus_slider_set_soft(g->feathering, p->feathering);
  dt_bauhaus_slider_set_soft(g->smoothing, p->smoothing);
  dt_bauhaus_slider_set_soft(g->iterations, p->iterations);
  dt_bauhaus_slider_set_soft(g->quantization, p->quantization);
  dt_bauhaus_slider_set_soft(g->contrast_boost, p->contrast_boost);
  dt_bauhaus_slider_set_soft(g->exposure_boost, p->exposure_boost);

  show_guiding_controls(self);

  dt_pthread_mutex_lock(&g->lock);
  g->histogram_valid = 0;
  g->lut_valid = 0;
  g->graph_valid = 0;
  g->mask_display = 0;
  dt_pthread_mutex_unlock(&g->lock);

  dt_bauhaus_widget_set_quad_active(GTK_WIDGET(g->show_luminance_mask), g->mask_display);

  update_histogram(g);
  update_histogram_stats(g);
  update_curve_lut(g);
  _init_drawing(self->widget, g);
}

static void noise_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;

  dt_iop_toneequalizer_params_t *p = (dt_iop_toneequalizer_params_t *)self->params;
  p->noise = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}


static void ultra_deep_blacks_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_toneequalizer_params_t *p = (dt_iop_toneequalizer_params_t *)self->params;
  p->ultra_deep_blacks = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}


static void deep_blacks_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_toneequalizer_params_t *p = (dt_iop_toneequalizer_params_t *)self->params;
  p->deep_blacks = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}


static void blacks_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_toneequalizer_params_t *p = (dt_iop_toneequalizer_params_t *)self->params;
  p->blacks = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}


static void shadows_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_toneequalizer_params_t *p = (dt_iop_toneequalizer_params_t *)self->params;
  p->shadows = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}


static void midtones_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_toneequalizer_params_t *p = (dt_iop_toneequalizer_params_t *)self->params;
  p->midtones = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}


static void highlights_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_toneequalizer_params_t *p = (dt_iop_toneequalizer_params_t *)self->params;
  p->highlights = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}


static void whites_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_toneequalizer_params_t *p = (dt_iop_toneequalizer_params_t *)self->params;
  p->whites = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}


static void speculars_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_toneequalizer_params_t *p = (dt_iop_toneequalizer_params_t *)self->params;
  p->speculars = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}


static void method_changed(GtkWidget *widget, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_toneequalizer_params_t *p = (dt_iop_toneequalizer_params_t *)self->params;

  p->method = dt_bauhaus_combobox_get(widget);
  invalidate_luminance_cache(self);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}


static void details_changed(GtkWidget *widget, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_toneequalizer_params_t *p = (dt_iop_toneequalizer_params_t *)self->params;

  p->details = dt_bauhaus_combobox_get(widget);
  invalidate_luminance_cache(self);
  show_guiding_controls(self);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void blending_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_toneequalizer_params_t *p = (dt_iop_toneequalizer_params_t *)self->params;

  p->blending = dt_bauhaus_slider_get(slider);
  invalidate_luminance_cache(self);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void feathering_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_toneequalizer_params_t *p = (dt_iop_toneequalizer_params_t *)self->params;

  p->feathering = dt_bauhaus_slider_get(slider);
  invalidate_luminance_cache(self);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void smoothing_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_toneequalizer_params_t *p = (dt_iop_toneequalizer_params_t *)self->params;

  p->smoothing= dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);

  dt_iop_toneequalizer_gui_data_t *g = (dt_iop_toneequalizer_gui_data_t *)self->gui_data;
  gtk_widget_queue_draw(GTK_WIDGET(g->area));
}

static void iterations_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_toneequalizer_params_t *p = (dt_iop_toneequalizer_params_t *)self->params;

  p->iterations = dt_bauhaus_slider_get(slider);
  invalidate_luminance_cache(self);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void quantization_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_toneequalizer_params_t *p = (dt_iop_toneequalizer_params_t *)self->params;

  p->quantization = dt_bauhaus_slider_get(slider);
  invalidate_luminance_cache(self);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void contrast_boost_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_toneequalizer_params_t *p = (dt_iop_toneequalizer_params_t *)self->params;

  p->contrast_boost = dt_bauhaus_slider_get(slider);
  invalidate_luminance_cache(self);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void exposure_boost_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_toneequalizer_params_t *p = (dt_iop_toneequalizer_params_t *)self->params;

  p->exposure_boost = dt_bauhaus_slider_get(slider);
  invalidate_luminance_cache(self);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void auto_adjust_exposure_boost(GtkWidget *quad, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;

  dt_iop_request_focus(self);

  if(!self->enabled)
  {
    // If module disabled, enable and do nothing
    dt_dev_add_history_item(darktable.develop, self, TRUE);
    return;
  }

  dt_iop_toneequalizer_params_t *p = (dt_iop_toneequalizer_params_t *)self->params;
  dt_iop_toneequalizer_gui_data_t *g = (dt_iop_toneequalizer_gui_data_t *)self->gui_data;

  if(!g->luminance_valid || self->dev->pipe->processing)
  {
    dt_control_log(_("wait for the preview to finish recomputing"));
    return;
  }

  // The goal is to get the exposure distribution centered on the equalizer view
  // to spread it over as many nodes as possible for better exposure control.
  // Controls nodes are between -8 and 0 EV,
  // so we aim at centering the exposure distribution on -4 EV
  const float target = log2f(CONTRAST_FULCRUM);
  update_histogram(g);
  update_histogram_stats(g);
  p->exposure_boost += target - g->histogram_average;

  // Update the GUI stuff
  darktable.gui->reset = 1;
  dt_bauhaus_slider_set_soft(g->exposure_boost, p->exposure_boost);
  darktable.gui->reset = 0;
  invalidate_luminance_cache(self);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}


static void auto_adjust_contrast_boost(GtkWidget *quad, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;

  dt_iop_request_focus(self);

  if(!self->enabled)
  {
    // If module disabled, enable and do nothing
    dt_dev_add_history_item(darktable.develop, self, TRUE);
    return;
  }

  dt_iop_toneequalizer_params_t *p = (dt_iop_toneequalizer_params_t *)self->params;
  dt_iop_toneequalizer_gui_data_t *g = (dt_iop_toneequalizer_gui_data_t *)self->gui_data;

  if(p->contrast_boost != 0.0f)
  {
    // Reset the contrast boost and do nothing
    p->contrast_boost = 0.0f;
    dt_bauhaus_slider_set_soft(g->contrast_boost, p->contrast_boost);
    invalidate_luminance_cache(self);
    dt_dev_add_history_item(darktable.develop, self, TRUE);
    return;
  }

  if(!g->luminance_valid || self->dev->pipe->processing)
  {
    dt_control_log(_("wait for the preview to finish recomputing"));
    return;
  }

  // The goal is to spread 80 % of the exposure histogram between average exposure ± 4 EV
  update_histogram(g);
  update_histogram_stats(g);
  const float span_left = fabsf(g->histogram_average - g->histogram_first_decile);
  const float span_right = fabsf(g->histogram_last_decile - g->histogram_average);
  const float origin = fmaxf(span_left, span_right);

  // Compute the correction
  p->contrast_boost = (4.0f - origin) / 2.0f;

  // Update the GUI stuff
  darktable.gui->reset = 1;
  dt_bauhaus_slider_set_soft(g->contrast_boost, p->contrast_boost);
  darktable.gui->reset = 0;
  invalidate_luminance_cache(self);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}


static void show_luminance_mask_callback(GtkWidget *togglebutton, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_request_focus(self);

  if(!self->enabled)
  {
    // If module disabled, enable and do nothing
    dt_dev_add_history_item(darktable.develop, self, TRUE);
    return;
  }

  dt_iop_toneequalizer_gui_data_t *g = (dt_iop_toneequalizer_gui_data_t *)self->gui_data;

  // if blend module is displaying mask do not display it here
  if(self->request_mask_display)
  {
    dt_control_log(_("cannot display masks when the blending mask is displayed"));
    dt_bauhaus_widget_set_quad_active(GTK_WIDGET(g->show_luminance_mask), FALSE);
    g->mask_display = 0;
    return;
  }
  else
    g->mask_display = !g->mask_display;

  dt_bauhaus_widget_set_quad_active(GTK_WIDGET(g->show_luminance_mask), g->mask_display);
  dt_dev_reprocess_center(self->dev);
}


/***
 * GUI Interactivity
 **/

 static void switch_cursors(struct dt_iop_module_t *self)
 {
  dt_iop_toneequalizer_gui_data_t *g = (dt_iop_toneequalizer_gui_data_t *)self->gui_data;
  if(g == NULL) return;

  GtkWidget *widget = dt_ui_main_window(darktable.gui->ui);
  GdkCursor *cursor = gdk_cursor_new_from_name(gdk_display_get_default(), "default");

  if(!dtgtk_expander_get_expanded(DTGTK_EXPANDER(self->expander)) || !self->enabled)
  {
    // if module lost focus or is disabled
    // do nothing and let the app decide
  }
  else if( (self->dev->pipe->processing) ||
          (self->dev->image_status == DT_DEV_PIXELPIPE_DIRTY) ||
          (self->dev->preview_status == DT_DEV_PIXELPIPE_DIRTY) )
  {
    // display waiting cursor while pipe reprocess or will soon
    cursor = gdk_cursor_new_from_name(gdk_display_get_default(), "wait");
    gdk_window_set_cursor(gtk_widget_get_window(widget), cursor);
    dt_control_queue_redraw_center();
  }
  else if(g->cursor_valid && !self->dev->pipe->processing) // seems reduntand but is not
  {
    // hide GTK cursor because we display ours
    dt_control_change_cursor(GDK_BLANK_CURSOR);
    dt_control_queue_redraw_center();
  }
  else
  {
    // display default cursor
    gdk_window_set_cursor(gtk_widget_get_window(widget), cursor);
    dt_control_queue_redraw_center();
  }

  g_object_unref(cursor);

}


int mouse_moved(struct dt_iop_module_t *self, double x, double y, double pressure, int which)
{
  // Whenever the mouse moves over the picture preview, store its coordinates in the GUI struct
  // for later use. This works only if dev->preview_pipe perfectly overlaps with the UI preview
  // meaning all distortions, cropping, rotations etc. are applied before this module in the pipe.

  dt_develop_t *dev = self->dev;
  dt_iop_toneequalizer_gui_data_t *g = (dt_iop_toneequalizer_gui_data_t *)self->gui_data;

  int const wd = dev->preview_pipe->backbuf_width;
  int const ht = dev->preview_pipe->backbuf_height;

  if(g == NULL) return 0;
  if(wd < 1 || ht < 1) return 0;

  float pzx, pzy;
  dt_dev_get_pointer_zoom_pos(dev, x, y, &pzx, &pzy);
  pzx += 0.5f;
  pzy += 0.5f;

  const int x_pointer = pzx * wd;
  const int y_pointer = pzy * ht;

  dt_pthread_mutex_lock(&g->lock);
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
  dt_pthread_mutex_unlock(&g->lock);

  // store the actual exposure too, to spare I/O op
  if(g->cursor_valid && !dev->pipe->processing && g->luminance_valid)
    g->cursor_exposure = log2f(get_luminance_from_buffer(g->thumb_preview_buf,
                                                         g->thumb_preview_buf_width,
                                                         g->thumb_preview_buf_height,
                                                         (size_t)x_pointer, (size_t)y_pointer));


  switch_cursors(self);
  return 1;

}


int mouse_leave(struct dt_iop_module_t *self)
{
  dt_iop_toneequalizer_gui_data_t *g = (dt_iop_toneequalizer_gui_data_t *)self->gui_data;

  if(g == NULL) return 0;

  dt_pthread_mutex_lock(&g->lock);
  g->cursor_valid = FALSE;
  dt_pthread_mutex_unlock(&g->lock);

  // display default cursor
  GtkWidget *widget = dt_ui_main_window(darktable.gui->ui);
  GdkCursor *cursor = gdk_cursor_new_from_name(gdk_display_get_default(), "default");
  gdk_window_set_cursor(gtk_widget_get_window(widget), cursor);
  g_object_unref(cursor);
  dt_control_queue_redraw_center();

  return 1;
}


int scrolled(struct dt_iop_module_t *self, double x, double y, int up, uint32_t state)
{
  dt_develop_t *dev = self->dev;
  dt_iop_toneequalizer_gui_data_t *g = (dt_iop_toneequalizer_gui_data_t *)self->gui_data;
  dt_iop_toneequalizer_params_t *p = (dt_iop_toneequalizer_params_t *)self->params;

  if(self->dt->gui->reset) return 1;
  if(g == NULL) return 0;

  // add an option to allow skip mouse events while editing masks
  if(darktable.develop->darkroom_skip_mouse_events) return 0;

  // if GUI buffers not ready, exit but still handle the cursor
  dt_pthread_mutex_lock(&g->lock);
  const int fail = (!g->cursor_valid || !g->luminance_valid || !g->interpolation_valid || dev->pipe->processing);
  dt_pthread_mutex_unlock(&g->lock);
  if(fail) return 1;
  // Store scrolling step and increment
  dt_pthread_mutex_lock(&g->lock);

  g->scrolling = 1;
  int increment = (up) ? +1 : -1;

  float step;
  if((state & GDK_SHIFT_MASK) == GDK_SHIFT_MASK)
    step = 1.0f;  // coarse
  else if((state & GDK_CONTROL_MASK) == GDK_CONTROL_MASK)
    step = 0.1f;  // fine
  else
    step = 0.25f; // standard

  // Get the desired correction on the selected exposure
  float offset = step * ((float)increment);
  const float correction = log2f(pixel_correction(g->cursor_exposure, g->factors, g->sigma)) + offset;

  // bounds check
  g->user_param_valid = !(correction < -2.0f || correction > 2.0f);
  if(!g->user_param_valid) dt_control_log(_("the maximum correction setting has been reached"));

  // Get the new weights for the radial-basis approximation
  float factors[CHANNELS + 1] DT_ALIGNED_ARRAY;
  if(g->user_param_valid)
  {
    g->user_param_valid = radial_approximation(factors, correction, g);
    if(!g->user_param_valid) dt_control_log(_("the interpolation is unstable"));
  }

  // Compute new user params for channels and store them locally
  if(g->user_param_valid)
  {
    g->user_param_valid = compute_channels_factors(factors, g->temp_user_params, g->sigma);
    if(!g->user_param_valid) dt_control_log(_("some parameters are out-of-bounds"));
  }

  dt_pthread_mutex_unlock(&g->lock);

  if(g->user_param_valid)
  {
    dt_pthread_mutex_lock(&g->lock);
    dt_simd_memcpy(factors, g->factors, PIXEL_CHAN);
    g->lut_valid = 0;
    dt_pthread_mutex_unlock(&g->lock);

    // Update GUI with temporary params
    self->dt->gui->reset = 1;
    dt_bauhaus_slider_set_soft(g->noise, log2f(g->temp_user_params[0]));
    dt_bauhaus_slider_set_soft(g->ultra_deep_blacks, log2f(g->temp_user_params[1]));
    dt_bauhaus_slider_set_soft(g->deep_blacks, log2f(g->temp_user_params[2]));
    dt_bauhaus_slider_set_soft(g->blacks, log2f(g->temp_user_params[3]));
    dt_bauhaus_slider_set_soft(g->shadows, log2f(g->temp_user_params[4]));
    dt_bauhaus_slider_set_soft(g->midtones, log2f(g->temp_user_params[5]));
    dt_bauhaus_slider_set_soft(g->highlights, log2f(g->temp_user_params[6]));
    dt_bauhaus_slider_set_soft(g->whites, log2f(g->temp_user_params[7]));
    dt_bauhaus_slider_set_soft(g->speculars, log2f(g->temp_user_params[8]));
    gtk_widget_queue_draw(GTK_WIDGET(g->area));
    self->dt->gui->reset = 0;

    dt_pthread_mutex_lock(&g->lock);

    // Reset scrolling
    g->scrolling = 0;

    // Convert the linear temp parameters to log gains
    float gains[CHANNELS] DT_ALIGNED_ARRAY;
    compute_channels_gains(g->temp_user_params, gains);

    dt_pthread_mutex_unlock(&g->lock);

    // commit changes and history
    commit_channels_gains(gains, p);
    dt_dev_add_history_item(darktable.develop, self, TRUE);

  }

  return 1;
}


/*
int button_pressed(struct dt_iop_module_t *self, double x, double y, double pressure, int which, int type,
                   uint32_t state)
{
  dt_iop_toneequalizer_gui_data_t *g = (dt_iop_toneequalizer_gui_data_t *)self->gui_data;
  dt_iop_toneequalizer_params_t *p = (dt_iop_toneequalizer_params_t *)self->params;

  fprintf(stdout, "button released started \n");

  if(self->dt->gui->reset) return 0;
  if(g == NULL) return 0;

  dt_pthread_mutex_lock(&g->lock);
  const int fail = (!g->cursor_valid || !g->luminance_valid || !g->scrolling);
  dt_pthread_mutex_unlock(&g->lock);

  if(fail) return 0;

  if(which == 1 && g->user_param_valid)
  {
    dt_pthread_mutex_lock(&g->lock);

    // Reset scrolling
    g->scrolling = 0;

    // Convert the linear temp parameters to log gains
    float gains[CHANNELS] DT_ALIGNED_ARRAY;
    compute_channels_gains(g->temp_user_params, gains);

    dt_pthread_mutex_unlock(&g->lock);

    // commit changes and history
    commit_channels_gains(gains, p);
    dt_dev_add_history_item(darktable.develop, self, TRUE);

    fprintf(stdout, "button released finished \n");
    return 1;
  }
  else if(which == 3)
  {
    // Abort setting
    dt_pthread_mutex_lock(&g->lock);

    // restore user parameters
    float factors[CHANNELS] DT_ALIGNED_ARRAY;
    get_channels_factors(factors, p);
    dt_simd_memcpy(factors, g->temp_user_params, CHANNELS);

    // flush the caches
    g->lut_valid = 0;
    g->scrolling = 0;

    // recompute the approximation weights
    int valid = pseudo_solve(g->interpolation_matrix, factors, CHANNELS, PIXEL_CHAN, 1);
    if(valid) dt_simd_memcpy(factors, g->factors, PIXEL_CHAN);

    dt_pthread_mutex_unlock(&g->lock);

    self->dt->gui->reset = 1;
    dt_bauhaus_slider_set_soft(g->noise, p->noise);
    dt_bauhaus_slider_set_soft(g->ultra_deep_blacks, p->ultra_deep_blacks);
    dt_bauhaus_slider_set_soft(g->deep_blacks, p->deep_blacks);
    dt_bauhaus_slider_set_soft(g->blacks, p->blacks);
    dt_bauhaus_slider_set_soft(g->shadows, p->shadows);
    dt_bauhaus_slider_set_soft(g->midtones, p->midtones);
    dt_bauhaus_slider_set_soft(g->highlights, p->highlights);
    dt_bauhaus_slider_set_soft(g->whites, p->whites);
    dt_bauhaus_slider_set_soft(g->speculars, p->speculars);
    gtk_widget_queue_draw(GTK_WIDGET(g->area));
    self->dt->gui->reset = 0;
    return 1;
  }

  return 0;
}
*/

/***
 * GTK/Cairo drawings and custom widgets
 **/

void cairo_draw_hatches(cairo_t *cr, double center[2], double span[2], int instances, double line_width, double shade)
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

static void dt_get_shade_from_luminance(cairo_t *cr, float luminance, float alpha)
{
  // TODO: fetch screen gamma from ICC display profile
  const float gamma = 1.0f / 2.2f;
  const float shade = powf(luminance, gamma);
  cairo_set_source_rgba(cr, shade, shade, shade, alpha);
}


static void dt_draw_exposure_cursor(cairo_t *cr, double pointerx, double pointery, double radius, float luminance, float zoom_scale, int instances)
{
  // Draw a circle cursor filled with a grey shade corresponding to a luminance value
  // or hatches if the value is above the overexposed threshold

  const double radius_z = radius / zoom_scale;

  dt_get_shade_from_luminance(cr, luminance, 1.0);
  cairo_arc(cr, pointerx, pointery, radius_z, 0, 2 * M_PI);
  cairo_fill_preserve(cr);
  cairo_save(cr);
  cairo_clip(cr);

  if(log2f(luminance) > 0.0f)
  {
    // if overexposed, draw hatches
    double pointer_coord[2] = { pointerx, pointery };
    double span[2] = { radius_z, radius_z };
    cairo_draw_hatches(cr, pointer_coord, span, instances, DT_PIXEL_APPLY_DPI(1. / zoom_scale), 0.3);
  }
  cairo_restore(cr);
}


static void dt_match_color_to_background(cairo_t *cr, float exposure, float alpha)
{
  float shade = 0.0f;
  // TODO: put that as a preference in darktablerc
  const float contrast = 1.0f;

  if(exposure > -2.5f)
    shade = (fminf(exposure * contrast, 0.0f) - 2.5f);
  else
    shade = (fmaxf(exposure / contrast, -5.0f) + 2.5f);

  dt_get_shade_from_luminance(cr, exp2f(shade), alpha);
}


void gui_post_expose(struct dt_iop_module_t *self, cairo_t *cr, int32_t width, int32_t height,
                     int32_t pointerx, int32_t pointery)
{
  // Draw the custom exposure cursor over the image preview

  dt_develop_t *dev = self->dev;
  dt_iop_toneequalizer_gui_data_t *g = (dt_iop_toneequalizer_gui_data_t *)self->gui_data;

  if(!g->cursor_valid || !g->interpolation_valid || !g->luminance_valid || dev->pipe->processing) return;

  float x_pointer = g->cursor_pos_x;
  float y_pointer = g->cursor_pos_y;

  // Rescale and shift Cairo drawing coordinates
  float wd = dev->preview_pipe->backbuf_width;
  float ht = dev->preview_pipe->backbuf_height;
  float zoom_y = dt_control_get_dev_zoom_y();
  float zoom_x = dt_control_get_dev_zoom_x();
  dt_dev_zoom_t zoom = dt_control_get_dev_zoom();
  int closeup = dt_control_get_dev_closeup();
  float zoom_scale = dt_dev_get_zoom_scale(dev, zoom, 1<<closeup, 1);
  cairo_translate(cr, width / 2.0, height / 2.0);
  cairo_scale(cr, zoom_scale, zoom_scale);
  cairo_translate(cr, -.5f * wd - zoom_x * wd, -.5f * ht - zoom_y * ht);

  // Get the corresponding exposure
  const float exposure_in = g->cursor_exposure;
  const float luminance_in = exp2f(exposure_in);

  // Get the corresponding correction and compute resulting exposure
  const float correction = log2f(pixel_correction(exposure_in, g->factors, g->sigma));
  const float exposure_out = exposure_in + correction;
  const float luminance_out = exp2f(exposure_out);

  if(correction < -2.0f || correction > 2.0f || correction != correction || exposure_in != exposure_in) return; // something went wrong

  // set custom cursor dimensions
  const double outer_radius = 16.;
  const double inner_radius = outer_radius / 2.0;
  const double setting_scale = 2. * outer_radius / zoom_scale;
  const double setting_offset_x = (outer_radius + 4. * g->inner_padding) / zoom_scale;

  // setting fill bars
  dt_match_color_to_background(cr, exposure_out, 1.0);
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(6. / zoom_scale));
  cairo_move_to(cr, x_pointer - setting_offset_x, y_pointer);
  cairo_line_to(cr, x_pointer - setting_offset_x, y_pointer - correction * setting_scale);
  cairo_stroke(cr);

  // setting ground level
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.5 / zoom_scale));
  cairo_move_to(cr, x_pointer + (outer_radius + 2. * g->inner_padding) / zoom_scale, y_pointer);
  cairo_line_to(cr, x_pointer + outer_radius / zoom_scale, y_pointer);
  cairo_move_to(cr, x_pointer - outer_radius / zoom_scale, y_pointer);
  cairo_line_to(cr, x_pointer - setting_offset_x - 4.0 * g->inner_padding / zoom_scale, y_pointer);
  cairo_stroke(cr);

  // don't display the setting bullets if we are waiting for a luminance computation to finish
  cairo_arc(cr, x_pointer - setting_offset_x, y_pointer - correction * setting_scale, DT_PIXEL_APPLY_DPI(7. / zoom_scale), 0, 2. * M_PI);
  cairo_fill(cr);

  // draw exposure cursor
  dt_draw_exposure_cursor(cr, x_pointer, y_pointer, outer_radius, luminance_in, zoom_scale, 6);
  dt_draw_exposure_cursor(cr, x_pointer, y_pointer, inner_radius, luminance_out, zoom_scale, 3);

  // Create Pango objects : texts
  char text[256];
  PangoLayout *layout;
  PangoRectangle ink;
  PangoFontDescription *desc = pango_font_description_copy_static(darktable.bauhaus->pango_font_desc);

  // Avoid text resizing based on zoom level
  const int old_size = pango_font_description_get_size(desc);
  pango_font_description_set_size (desc, (int)(old_size / zoom_scale));
  layout = pango_cairo_create_layout(cr);
  pango_layout_set_font_description(layout, desc);

  // Build text object
  snprintf(text, sizeof(text), "%+.1f EV", exposure_in);
  pango_layout_set_text(layout, text, -1);
  pango_layout_get_pixel_extents(layout, &ink, NULL);

  // Draw the text plain blackground
  dt_get_shade_from_luminance(cr, luminance_out, 0.75);
  cairo_rectangle(cr, x_pointer + (outer_radius + 2. * g->inner_padding) / zoom_scale,
                      y_pointer - ink.y - ink.height / 2.0 - g->inner_padding / zoom_scale,
                      ink.width + 2.0 * ink.x + 4. * g->inner_padding / zoom_scale,
                      ink.height + 2.0 * ink.y + 2. * g->inner_padding / zoom_scale);
  cairo_fill(cr);

  // Display the EV reading
  dt_match_color_to_background(cr, exposure_out, 1.0);
  cairo_move_to(cr, x_pointer + (outer_radius + 4. * g->inner_padding) / zoom_scale,
                    y_pointer - ink.y - ink.height / 2.);
  pango_cairo_show_layout(cr, layout);
  cairo_stroke(cr);
}


static gboolean _init_drawing(GtkWidget *widget, dt_iop_toneequalizer_gui_data_t *g)
{
  // Cache the equalizer graph objects to avoid recomputing all the view at each redraw
  gtk_widget_get_allocation(widget, &g->allocation);
  g->cst = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, g->allocation.width, g->allocation.height);
  g->cr = cairo_create(g->cst);
  g->layout = pango_cairo_create_layout(g->cr);
  g->desc = pango_font_description_copy_static(darktable.bauhaus->pango_font_desc);
  pango_layout_set_font_description(g->layout, g->desc);
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
  g->inner_padding = 4; // TODO: INNER_PADDING value as defined in bauhaus.c macros, sync them
  g->inset = g->inner_padding + darktable.bauhaus->quad_width;
  g->graph_width = g->allocation.width - 2.0 * g->inset - 2.0 * g->line_height; // align the right border on sliders
  g->graph_height = g->graph_width; // give room to nodes
  g->gradient_left_limit = 0.0;
  g->gradient_right_limit = g->graph_width;
  g->gradient_top_limit = g->graph_height + 2 * g->inner_padding;
  g->gradient_width = g->gradient_right_limit - g->gradient_left_limit;
  g->legend_top_limit = -0.5 * g->line_height - 2.0 * g->inner_padding;
  g->x_label = g->graph_width + g->sign_width + 3.0 * g->inner_padding;

  gtk_render_background(g->context, g->cr, 0, 0, g->allocation.width, g->allocation.height);

  // set the graph as the origin of the coordinates
  cairo_translate(g->cr, g->line_height + 2 * g->inner_padding, g->line_height + 3 * g->inner_padding);

  // display x-axis and y-axis legends (EV)
  set_color(g->cr, darktable.bauhaus->graph_fg);
  /*
  snprintf(text, sizeof(text), "(EV)");
  pango_layout_set_text(g->layout, text, -1);
  pango_layout_get_pixel_extents(g->layout, &g->ink, NULL);
  cairo_move_to(g->cr, - g->ink.x - g->line_height - 2 * g->inner_padding,
                  g->graph_height + 2 * g->inner_padding - g->ink.y - g->ink.height / 2.0 + g->line_height / 2.0);
  pango_cairo_show_layout(g->cr, g->layout);
  cairo_stroke(g->cr);
  */

  float value = -8.0f;

  for(int k = 0; k < CHANNELS; k++)
  {
    const float xn = (((float)k) / ((float)(CHANNELS - 1))) * g->graph_width - g->sign_width;
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
  grad = cairo_pattern_create_linear(g->gradient_left_limit, 0.0, g->gradient_right_limit, 0.0);
  dt_cairo_perceptual_gradient(grad, 1.0);
  cairo_set_line_width(g->cr, 0.0);
  cairo_rectangle(g->cr, g->gradient_left_limit, g->gradient_top_limit, g->gradient_width, g->line_height);
  cairo_set_source(g->cr, grad);
  cairo_fill(g->cr);

  /** y axis **/
  // Draw the perceptually even gradient
  grad = cairo_pattern_create_linear(0.0, g->graph_height, 0.0, 0.0);
  dt_cairo_perceptual_gradient(grad, 1.0);
  cairo_set_line_width(g->cr, 0.0);
  cairo_rectangle(g->cr, -g->line_height - 2 * g->inner_padding, 0.0, g->line_height, g->graph_height);
  cairo_set_source(g->cr, grad);
  cairo_fill(g->cr);

  cairo_pattern_destroy(grad);

  // Draw frame borders
  cairo_set_line_width(g->cr, DT_PIXEL_APPLY_DPI(0.5));
  set_color(g->cr, darktable.bauhaus->graph_border);
  cairo_rectangle(g->cr, 0, 0, g->graph_width, g->graph_height);
  cairo_stroke_preserve(g->cr);

  // Clip inside frame with some outer margin for nodes bullets
  /*
  cairo_rectangle(g->cr, -2 * g->inner_padding,
                         -2 * g->inner_padding,
                        g->graph_width + 4 * g->inner_padding,
                        g->graph_height + 4 * g->inner_padding);
  */
  //cairo_clip(g->cr);

  // end of caching section, this will not be drawn again

  dt_pthread_mutex_lock(&g->lock);
  g->graph_valid = 1;
  dt_pthread_mutex_unlock(&g->lock);

  return TRUE;
}


static gboolean dt_iop_toneequalizer_draw(GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
  // Draw the widget equalizer view
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_toneequalizer_gui_data_t *g = (dt_iop_toneequalizer_gui_data_t *)self->gui_data;

  // Init or refresh the drawing cache
  //if(!g->graph_valid)
  if(!_init_drawing(self->widget, g)) return FALSE;

  // Refresh cached UI elements
  update_histogram(g);
  update_histogram_stats(g);
  update_curve_lut(g);

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

  if(g->histogram_valid)
  {
    // draw the inset histogram
    set_color(g->cr, darktable.bauhaus->inset_histogram);
    cairo_set_line_width(g->cr, DT_PIXEL_APPLY_DPI(4.0));
    cairo_move_to(g->cr, 0, g->graph_height);

    for(int k = 0; k < UI_SAMPLES; k++)
    {
      // the x range is [-14;+2] EV
      const double x_temp = (16.0 * (double)k / (double)(UI_SAMPLES - 1)) - 14.0;
      const double y_temp = (double)(g->histogram[k]) / (double)(g->max_histogram) * 0.96;
      cairo_line_to(g->cr, (x_temp + 14.0) * g->graph_width / 16.0,
                           (1.0 - y_temp) * g->graph_height );
    }
    cairo_line_to(g->cr, g->graph_width, g->graph_height);
    cairo_close_path(g->cr);
    cairo_fill(g->cr);
  }

  if(g->lut_valid)
  {
    // draw the interpolation curve
    set_color(g->cr, darktable.bauhaus->graph_fg);
    cairo_move_to(g->cr, 0, g->gui_lut[0] * g->graph_height);
    cairo_set_line_width(g->cr, DT_PIXEL_APPLY_DPI(3));

    for(int k = 1; k < UI_SAMPLES; k++)
    {
      // the x range is [-14;+2] EV
      const float x_temp = (16.0f * (((float)k) / ((float)(UI_SAMPLES - 1)))) - 14.0f;
      const float y_temp = g->gui_lut[k];

      cairo_line_to(g->cr, (x_temp + 14.0f) * g->graph_width / 16.0f,
                            y_temp * g->graph_height );
    }
    cairo_stroke(g->cr);
  }

  if(g->user_param_valid)
  {
    // draw nodes positions
    for(int k = 0; k < CHANNELS; k++)
    {
      const float xn = (((float)k) / ((float)(CHANNELS - 1))) * g->graph_width,
                  yn = (0.5 - log2f(g->temp_user_params[k]) / 4.0) * g->graph_height; // assumes factors in [-2 ; 2] EV
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
      set_color(g->cr, darktable.bauhaus->graph_bg);
      cairo_fill(g->cr);
    }
  }

  // clean and exit
  cairo_set_source_surface(cr, g->cst, 0, 0);
  cairo_paint(cr);

  return TRUE;
}

static gboolean dt_iop_toneequalizer_bar_draw(GtkWidget *widget, cairo_t *crf, gpointer user_data)
{
  // Draw the widget equalizer view
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_toneequalizer_gui_data_t *g = (dt_iop_toneequalizer_gui_data_t *)self->gui_data;

  update_histogram(g);
  update_histogram_stats(g);

  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  cairo_surface_t *cst = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, allocation.width, allocation.height);
  cairo_t *cr = cairo_create(cst);

  // draw background
  set_color(cr, darktable.bauhaus->graph_bg);
  cairo_rectangle(cr, 0, 0, allocation.width, allocation.height);
  cairo_fill_preserve(cr);
  cairo_clip(cr);

  if(g->histo_stats_valid)
  {
    // draw histogram span
    const float left = (g->histogram_first_decile + 8.0f) / 8.0f;
    const float right = (g->histogram_last_decile + 8.0f) / 8.0f;
    const float width = (right - left);
    set_color(cr, darktable.bauhaus->inset_histogram);
    cairo_rectangle(cr, left * allocation.width, 0, width * allocation.width, allocation.height);
    cairo_fill(cr);

    // draw average bar
    set_color(cr, darktable.bauhaus->graph_fg);
    cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(3));
    const float average = (g->histogram_average + 8.0f) / 8.0f;
    cairo_move_to(cr, average * allocation.width, 0.0);
    cairo_line_to(cr, average * allocation.width, allocation.height);
    cairo_stroke(cr);

    // draw clipping bars
    cairo_set_source_rgb(cr, 0.75, 0.50, 0);
    cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(6));
    if(left <= 0.0f)
    {
      cairo_move_to(cr, DT_PIXEL_APPLY_DPI(3), 0.0);
      cairo_line_to(cr, DT_PIXEL_APPLY_DPI(3), allocation.height);
      cairo_stroke(cr);
    }
    if(right >= 1.0f)
    {
      cairo_move_to(cr, allocation.width - DT_PIXEL_APPLY_DPI(3), 0.0);
      cairo_line_to(cr, allocation.width - DT_PIXEL_APPLY_DPI(3), allocation.height);
      cairo_stroke(cr);
    }
  }

  cairo_set_source_surface(crf, cst, 0, 0);
  cairo_paint(crf);
  cairo_destroy(cr);
  cairo_surface_destroy(cst);
  return TRUE;
}

static void _develop_ui_pipe_started_callback(gpointer instance, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_toneequalizer_gui_data_t *g = (dt_iop_toneequalizer_gui_data_t *)self->gui_data;
  if(g == NULL) return;
  switch_cursors(self);

  if(!dtgtk_expander_get_expanded(DTGTK_EXPANDER(self->expander)))
  {
    // if module is not active, disable mask preview
    dt_pthread_mutex_lock(&g->lock);
    g->mask_display = 0;
    dt_pthread_mutex_unlock(&g->lock);
  }

  const int reset = darktable.gui->reset;
  darktable.gui->reset = 1;
  dt_pthread_mutex_lock(&g->lock);
  dt_bauhaus_widget_set_quad_active(GTK_WIDGET(g->show_luminance_mask), g->mask_display);
  dt_pthread_mutex_unlock(&g->lock);
  darktable.gui->reset = reset;
}


static void _develop_preview_pipe_finished_callback(gpointer instance, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_toneequalizer_gui_data_t *g = (dt_iop_toneequalizer_gui_data_t *)self->gui_data;
  if(g == NULL) return;
  switch_cursors(self);

  dt_pthread_mutex_lock(&g->lock);
  g->luminance_valid = 1;
  dt_pthread_mutex_unlock(&g->lock);

  const int reset = darktable.gui->reset;
  darktable.gui->reset = 1;
  gtk_widget_queue_draw(GTK_WIDGET(g->area));
  gtk_widget_queue_draw(GTK_WIDGET(g->bar));
  darktable.gui->reset = reset;
}


static void _develop_ui_pipe_finished_callback(gpointer instance, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_toneequalizer_gui_data_t *g = (dt_iop_toneequalizer_gui_data_t *)self->gui_data;
  if(g == NULL) return;
  switch_cursors(self);
  const int reset = darktable.gui->reset;
  darktable.gui->reset = 1;
  gtk_widget_queue_draw(GTK_WIDGET(g->area));
  gtk_widget_queue_draw(GTK_WIDGET(g->bar));
  darktable.gui->reset = reset;
}


void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_toneequalizer_gui_data_t));
  dt_iop_toneequalizer_gui_data_t *g = (dt_iop_toneequalizer_gui_data_t *)self->gui_data;

  dt_pthread_mutex_init(&g->lock, NULL);
  dt_pthread_mutex_lock(&g->lock);
  g->hash = 0;
  g->histogram_hash = 0;
  g->max_histogram = 1;
  g->scale = 0.0f;
  g->sigma = 0.0f;
  g->interpolation_valid = 0;
  g->histogram_valid = 0;
  g->luminance_valid = 0;
  g->lut_valid = 0;
  g->graph_valid = 0;
  g->histo_stats_valid = 0;
  g->mask_display = 0;
  g->scrolling = 0;
  g->scroll_increments = 0;
  g->user_param_valid = 0;

  g->full_preview_buf = NULL;
  g->full_preview_buf_width = 0;
  g->full_preview_buf_height = 0;

  g->thumb_preview_buf = NULL;
  g->thumb_preview_buf_width = 0;
  g->thumb_preview_buf_height = 0;

  g->desc = NULL;
  g->layout = NULL;
  g->cr = NULL;
  g->cst = NULL;
  g->context = NULL;

  g->pipe_order = 0;
  dt_pthread_mutex_unlock(&g->lock);

  // Init GTK notebook
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  g->notebook = GTK_NOTEBOOK(gtk_notebook_new());
  GtkWidget *page1 = GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
  GtkWidget *page2 = GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
  GtkWidget *page3 = GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));

  gtk_notebook_append_page(GTK_NOTEBOOK(g->notebook), page1, gtk_label_new(_("simple")));
  gtk_notebook_append_page(GTK_NOTEBOOK(g->notebook), page2, gtk_label_new(_("advanced")));
  gtk_notebook_append_page(GTK_NOTEBOOK(g->notebook), page3, gtk_label_new(_("masking")));
  gtk_widget_show_all(GTK_WIDGET(gtk_notebook_get_nth_page(g->notebook, 0)));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->notebook), FALSE, FALSE, 0);

  gtk_container_child_set(GTK_CONTAINER(g->notebook), page1, "tab-expand", TRUE, "tab-fill", TRUE, NULL);
  gtk_container_child_set(GTK_CONTAINER(g->notebook), page2, "tab-expand", TRUE, "tab-fill", TRUE, NULL);
  gtk_container_child_set(GTK_CONTAINER(g->notebook), page3, "tab-expand", TRUE, "tab-fill", TRUE, NULL);

  // Simple view
  const float top = 2.0;
  const float bottom = -2.0;

  g->noise = dt_bauhaus_slider_new_with_range(self, bottom, top, 0.1, 0.0, 2);
  dt_bauhaus_slider_set_format(g->noise, "%+.2f EV");
  dt_bauhaus_widget_set_label(g->noise, NULL, _("-8 EV : blacks"));
  gtk_box_pack_start(GTK_BOX(page1), g->noise, FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(g->noise), "value-changed", G_CALLBACK(noise_callback), self);

  g->ultra_deep_blacks = dt_bauhaus_slider_new_with_range(self, bottom, top, 0.1, 0.0, 2);
  dt_bauhaus_slider_set_format(g->ultra_deep_blacks, "%+.2f EV");
  dt_bauhaus_widget_set_label(g->ultra_deep_blacks, NULL, _("-7 EV : deep shadows"));
  gtk_box_pack_start(GTK_BOX(page1), g->ultra_deep_blacks, FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(g->ultra_deep_blacks), "value-changed", G_CALLBACK(ultra_deep_blacks_callback), self);

  g->deep_blacks = dt_bauhaus_slider_new_with_range(self, bottom, top, 0.1, 0.0, 2);
  dt_bauhaus_slider_set_format(g->deep_blacks, "%+.2f EV");
  dt_bauhaus_widget_set_label(g->deep_blacks, NULL, _("-6 EV : shadows"));
  gtk_box_pack_start(GTK_BOX(page1), g->deep_blacks, FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(g->deep_blacks), "value-changed", G_CALLBACK(deep_blacks_callback), self);

  g->blacks = dt_bauhaus_slider_new_with_range(self, bottom, top, 0.1, 0.0, 2);
  dt_bauhaus_slider_set_format(g->blacks, "%+.2f EV");
  dt_bauhaus_widget_set_label(g->blacks, NULL, _("-5 EV : light shadows"));
  gtk_box_pack_start(GTK_BOX(page1), g->blacks, FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(g->blacks), "value-changed", G_CALLBACK(blacks_callback), self);

  g->shadows = dt_bauhaus_slider_new_with_range(self, bottom, top, 0.1, 0.0, 2);
  dt_bauhaus_slider_set_format(g->shadows, "%+.2f EV");
  dt_bauhaus_widget_set_label(g->shadows, NULL, _("-4 EV : midtones"));
  gtk_box_pack_start(GTK_BOX(page1), g->shadows, FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(g->shadows), "value-changed", G_CALLBACK(shadows_callback), self);

  g->midtones = dt_bauhaus_slider_new_with_range(self, bottom, top, 0.1, 0.0, 2);
  dt_bauhaus_slider_set_format(g->midtones, "%+.2f EV");
  dt_bauhaus_widget_set_label(g->midtones, NULL, _("-3 EV : dark highlights"));
  gtk_box_pack_start(GTK_BOX(page1), g->midtones, FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(g->midtones), "value-changed", G_CALLBACK(midtones_callback), self);

  g->highlights = dt_bauhaus_slider_new_with_range(self, bottom, top, 0.1, 0.0, 2);
  dt_bauhaus_slider_set_format(g->highlights, "%+.2f EV");
  dt_bauhaus_widget_set_label(g->highlights, NULL, _("-2 EV : highlights"));
  gtk_box_pack_start(GTK_BOX(page1), g->highlights, FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(g->highlights), "value-changed", G_CALLBACK(highlights_callback), self);

  g->whites = dt_bauhaus_slider_new_with_range(self, bottom, top, 0.1, 0.0, 2);
  dt_bauhaus_slider_set_format(g->whites, "%+.2f EV");
  dt_bauhaus_widget_set_label(g->whites, NULL, _("-1 EV : whites"));
  gtk_box_pack_start(GTK_BOX(page1), g->whites, FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(g->whites), "value-changed", G_CALLBACK(whites_callback), self);

  g->speculars = dt_bauhaus_slider_new_with_range(self, bottom, top, 0.1, 0.0, 2);
  dt_bauhaus_slider_set_format(g->speculars, "%+.2f EV");
  dt_bauhaus_widget_set_label(g->speculars, NULL, _("+0 EV : speculars"));
  gtk_box_pack_start(GTK_BOX(page1), g->speculars, FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(g->speculars), "value-changed", G_CALLBACK(speculars_callback), self);

  // Advanced view
  g->area = GTK_DRAWING_AREA(dtgtk_drawing_area_new_with_aspect_ratio(1.0));
  gtk_box_pack_start(GTK_BOX(page2), GTK_WIDGET(g->area), FALSE, FALSE, 0);

  gtk_widget_add_events(GTK_WIDGET(g->area), GDK_POINTER_MOTION_MASK | GDK_POINTER_MOTION_HINT_MASK
                                                 | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK
                                                 | GDK_LEAVE_NOTIFY_MASK | GDK_SCROLL_MASK
                                                 | darktable.gui->scroll_mask);
  gtk_widget_set_can_focus(GTK_WIDGET(g->area), TRUE);
  g_signal_connect(G_OBJECT(g->area), "draw", G_CALLBACK(dt_iop_toneequalizer_draw), self);
  /*g_signal_connect(G_OBJECT(c->area), "button-press-event", G_CALLBACK(dt_iop_tonecurve_button_press), self);
  g_signal_connect(G_OBJECT(c->area), "motion-notify-event", G_CALLBACK(dt_iop_tonecurve_motion_notify), self);
  g_signal_connect(G_OBJECT(c->area), "leave-notify-event", G_CALLBACK(dt_iop_tonecurve_leave_notify), self);
  g_signal_connect(G_OBJECT(c->area), "enter-notify-event", G_CALLBACK(dt_iop_tonecurve_enter_notify), self);
  g_signal_connect(G_OBJECT(c->area), "configure-event", G_CALLBACK(area_resized), self);
  g_signal_connect(G_OBJECT(tb), "button-press-event", G_CALLBACK(dt_iop_color_picker_callback_button_press), &c->color_picker);
  g_signal_connect(G_OBJECT(c->area), "scroll-event", G_CALLBACK(_scrolled), self);
  g_signal_connect(G_OBJECT(c->area), "key-press-event", G_CALLBACK(dt_iop_tonecurve_key_press), self);*/

  g->smoothing = dt_bauhaus_slider_new_with_range(self, 1.0, 2, 0.1, sqrtf(2.0f), 2);
  dt_bauhaus_slider_enable_soft_boundaries(g->smoothing, 0.01, 2.5);
  dt_bauhaus_widget_set_label(g->smoothing, NULL, _("curve smoothing"));
  g_object_set(G_OBJECT(g->smoothing), "tooltip-text", _("number of passes of guided filter to apply\n"
                                                          "helps diffusing the edges of the filter at the expense of speed"), (char *)NULL);
  gtk_box_pack_start(GTK_BOX(page2), g->smoothing, FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(g->smoothing), "value-changed", G_CALLBACK(smoothing_callback), self);

  // Masking options
  g->method = dt_bauhaus_combobox_new(NULL);
  dt_bauhaus_widget_set_label(g->method, NULL, _("luminance estimator"));
  gtk_box_pack_start(GTK_BOX(page3), g->method, FALSE, FALSE, 0);
  dt_bauhaus_combobox_add(g->method, "RGB average");
  dt_bauhaus_combobox_add(g->method, "HSL lightness");
  dt_bauhaus_combobox_add(g->method, "HSV value / RGB max");
  dt_bauhaus_combobox_add(g->method, "RGB sum");
  dt_bauhaus_combobox_add(g->method, "RGB euclidian norm");
  dt_bauhaus_combobox_add(g->method, "RGB power norm");
  dt_bauhaus_combobox_add(g->method, "RGB geometric mean");
  g_signal_connect(G_OBJECT(g->method), "value-changed", G_CALLBACK(method_changed), self);

  g->details = dt_bauhaus_combobox_new(NULL);
  dt_bauhaus_widget_set_label(g->details, NULL, _("preserve details"));
  gtk_box_pack_start(GTK_BOX(page3), g->details, FALSE, FALSE, 0);
  dt_bauhaus_combobox_add(g->details, "no");
  dt_bauhaus_combobox_add(g->details, "averaged guided filter");
  dt_bauhaus_combobox_add(g->details, "guided filter");
  g_object_set(G_OBJECT(g->details), "tooltip-text", _("'no' affects global and local contrast (safe if you only add contrast)\n"
                                                       "'guided filter' only affects global contrast and tries to preserve local contrast\n"
                                                       "'averaged guided filter' is a geometric mean of both methods"), (char *)NULL);
  g_signal_connect(G_OBJECT(g->details), "value-changed", G_CALLBACK(details_changed), self);

  g->iterations = dt_bauhaus_slider_new_with_range(self, 1, 5, 1, 1, 0);
  dt_bauhaus_slider_enable_soft_boundaries(g->iterations, 1, 20);
  dt_bauhaus_widget_set_label(g->iterations, NULL, _("filter diffusion"));
  g_object_set(G_OBJECT(g->iterations), "tooltip-text", _("number of passes of guided filter to apply\n"
                                                          "helps diffusing the edges of the filter at the expense of speed"), (char *)NULL);
  gtk_box_pack_start(GTK_BOX(page3), g->iterations, FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(g->iterations), "value-changed", G_CALLBACK(iterations_callback), self);

  g->blending = dt_bauhaus_slider_new_with_range(self, 5., 45.0, 1, 12.5, 2);
  dt_bauhaus_slider_enable_soft_boundaries(g->blending, 0.01, 100.0);
  dt_bauhaus_slider_set_format(g->blending, "%.2f %%");
  dt_bauhaus_widget_set_label(g->blending, NULL, _("smoothing diameter"));
  g_object_set(G_OBJECT(g->blending), "tooltip-text", _("diameter of the blur in % of the largest image size"), (char *)NULL);
  gtk_box_pack_start(GTK_BOX(page3), g->blending, FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(g->blending), "value-changed", G_CALLBACK(blending_callback), self);

  g->feathering = dt_bauhaus_slider_new_with_range(self, 1., 10., 0.2, 5., 2);
  dt_bauhaus_slider_enable_soft_boundaries(g->feathering, 0.01, 10000.0);
  dt_bauhaus_widget_set_label(g->feathering, NULL, _("edges refinement/feathering"));
  g_object_set(G_OBJECT(g->feathering), "tooltip-text", _("precision of the feathering :\n"
                                                          "higher values force the mask to follow edges more closely\n"
                                                          "but may void the effect of the smoothing\n"
                                                          "lower values give smoother gradients and better smoothing\n"
                                                          "but may lead to inaccurate edges taping"), (char *)NULL);
  gtk_box_pack_start(GTK_BOX(page3), g->feathering, FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(g->feathering), "value-changed", G_CALLBACK(feathering_callback), self);

  gtk_box_pack_start(GTK_BOX(page3), dt_ui_section_label_new(_("mask post-processing")), FALSE, FALSE, 0);

  g->bar = GTK_DRAWING_AREA(dtgtk_drawing_area_new_with_aspect_ratio(0.05));
  gtk_box_pack_start(GTK_BOX(page3), GTK_WIDGET(g->bar), FALSE, FALSE, 0);
  gtk_widget_set_can_focus(GTK_WIDGET(g->bar), TRUE);
  g_signal_connect(G_OBJECT(g->bar), "draw", G_CALLBACK(dt_iop_toneequalizer_bar_draw), self);
  g_object_set(G_OBJECT(g->bar), "tooltip-text", _("mask histogram span between the first and last deciles.\n"
                                                   "the central line shows the average. Orange bars appear at extrema if clipping occurs."), (char *)NULL);


  g->quantization = dt_bauhaus_slider_new_with_range(self, 0.00, 2., 0.25, 0.0, 2);
  dt_bauhaus_widget_set_label(g->quantization, NULL, _("mask quantization"));
  dt_bauhaus_slider_set_format(g->quantization, "%+.2f EV");
  g_object_set(G_OBJECT(g->quantization), "tooltip-text", _("0 disables the quantization.\n"
                                                            "higher values posterize the luminance mask to help the guiding\n"
                                                            "produce piece-wise smooth areas when using high feathering values"), (char *)NULL);
  gtk_box_pack_start(GTK_BOX(page3), g->quantization, FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(g->quantization), "value-changed", G_CALLBACK(quantization_callback), self);

  g->exposure_boost = dt_bauhaus_slider_new_with_range(self, -4., 4., 0.25, 0., 2);
  dt_bauhaus_slider_enable_soft_boundaries(g->exposure_boost, -16., 16.);
  dt_bauhaus_widget_set_label(g->exposure_boost, NULL, _("mask exposure compensation"));
  dt_bauhaus_slider_set_format(g->exposure_boost, "%+.2f EV");
  g_object_set(G_OBJECT(g->exposure_boost), "tooltip-text", _("use this to slide the mask average exposure along channels\n"
                                                              "for better control of the exposure corrections.\n"
                                                              "the picker will auto-adjust the average exposure at -4EV."), (char *)NULL);
  gtk_box_pack_start(GTK_BOX(page3), g->exposure_boost, FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(g->exposure_boost), "value-changed", G_CALLBACK(exposure_boost_callback), self);

  dt_bauhaus_widget_set_quad_paint(g->exposure_boost, dtgtk_cairo_paint_colorpicker, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
  dt_bauhaus_widget_set_quad_toggle(g->exposure_boost, TRUE);
  g_signal_connect(G_OBJECT(g->exposure_boost), "quad-pressed", G_CALLBACK(auto_adjust_exposure_boost), self);

  g->contrast_boost = dt_bauhaus_slider_new_with_range(self, -4., 4., 0.25, 0., 2);
  dt_bauhaus_slider_enable_soft_boundaries(g->contrast_boost, -16., 16.);
  dt_bauhaus_widget_set_label(g->contrast_boost, NULL, _("mask contrast compensation"));
  dt_bauhaus_slider_set_format(g->contrast_boost, "%+.2f EV");
  g_object_set(G_OBJECT(g->contrast_boost), "tooltip-text", _("use this to dilate the mask contrast around its average exposure\n"
                                                              "this allows to spread the exposure histogram over more channels\n"
                                                              "for better control of the exposure corrections."), (char *)NULL);
  gtk_box_pack_start(GTK_BOX(page3), g->contrast_boost, FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(g->contrast_boost), "value-changed", G_CALLBACK(contrast_boost_callback), self);

  dt_bauhaus_widget_set_quad_paint(g->contrast_boost, dtgtk_cairo_paint_colorpicker, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
  dt_bauhaus_widget_set_quad_toggle(g->contrast_boost, TRUE);
  g_signal_connect(G_OBJECT(g->contrast_boost), "quad-pressed", G_CALLBACK(auto_adjust_contrast_boost), self);


  g->show_luminance_mask = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->show_luminance_mask, NULL, _("display exposure mask"));
  dt_bauhaus_widget_set_quad_paint(g->show_luminance_mask, dtgtk_cairo_paint_showmask,
                                   CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
  dt_bauhaus_widget_set_quad_toggle(g->show_luminance_mask, TRUE);
  g_object_set(G_OBJECT(g->show_luminance_mask), "tooltip-text", _("display exposure mask"), (char *)NULL);
  g_signal_connect(G_OBJECT(g->show_luminance_mask), "quad-pressed", G_CALLBACK(show_luminance_mask_callback), self);
  gtk_box_pack_start(GTK_BOX(self->widget),  g->show_luminance_mask, TRUE, TRUE, 0);

  // Force UI redraws when pipe starts/finishes computing and switch cursors
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_DEVELOP_PREVIEW_PIPE_FINISHED,
                            G_CALLBACK(_develop_preview_pipe_finished_callback), self);
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_DEVELOP_UI_PIPE_FINISHED,
                            G_CALLBACK(_develop_ui_pipe_finished_callback), self);

  dt_control_signal_connect(darktable.signals, DT_SIGNAL_DEVELOP_HISTORY_CHANGE,
                            G_CALLBACK(_develop_ui_pipe_started_callback), self);

  show_guiding_controls(self);
  _init_drawing(self->widget, g);
}


void gui_cleanup(struct dt_iop_module_t *self)
{
  dt_iop_toneequalizer_gui_data_t *g = (dt_iop_toneequalizer_gui_data_t *)self->gui_data;
  self->request_color_pick = DT_REQUEST_COLORPICK_OFF;

  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(_develop_ui_pipe_finished_callback), self);
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(_develop_ui_pipe_started_callback), self);
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(_develop_preview_pipe_finished_callback), self);

  if(g->desc) pango_font_description_free(g->desc);
  if(g->layout) g_object_unref(g->layout);
  if(g->cr) cairo_destroy(g->cr);
  if(g->cst) cairo_surface_destroy(g->cst);

  dt_pthread_mutex_destroy(&g->lock);
  free(self->gui_data);
  self->gui_data = NULL;
}


// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
