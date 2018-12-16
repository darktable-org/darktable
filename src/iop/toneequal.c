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
 * maybe because of bad demosaicing, possibly resulting in outliers/negative RGB values.
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

#include "bauhaus/bauhaus.h"
#include "common/darktable.h"
#include "common/interpolation.h"
#include "common/opencl.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "dtgtk/drawingarea.h"
#include "gui/draw.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "gui/color_picker_proxy.h"
#include "iop/iop_api.h"
#include "iop/gaussian_elimination.h"
#include "libs/colorpicker.h"
#include "common/iop_group.h"

#define CHANNELS 9
#define PIXEL_CHAN 16 // Only 12 is mandatory, but 16 makes full SSE/AVX vectors

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
                      "finite-math-only", "fp-contract=fast", "fast-math")
#endif

#define UI_SAMPLES 256 // 128 is a bit small for 4K resolution
#define CONTRAST_FULCRUM exp2f(-4.0f)
#define MIN_FLOAT exp2f(-16.0f)

/**
 * Build the exposures octaves : 
 * band-pass filters with gaussian windows spaced by 2 EV
 *
 * Notice that the [-22 ; -16] and [+4 ; +8] octaves copy the values respectively from the
 * - 14 and +2 octaves, for a more stable boundary behaviour, and also to guaranty that
 * the series of gaussian weights == 1.0 on the whole [-14 ; +2] EV range
**/
const float centers[PIXEL_CHAN] DT_ALIGNED_ARRAY = {-22.0f, -20.0f, -18.0f, -16.0f, // padding
                                                    -14.0f, -12.0f, -10.0f,  -8.0f, // data
                                                    -6.0f,  -4.0f,  -2.0f,   0.0f,  // data
                                                     2.0f,   4.0f,   6.0f,   8.0f}; // padding


DT_MODULE_INTROSPECTION(2, dt_iop_toneequalizer_params_t)


typedef enum dt_iop_toneequalizer_method_t
{
  DT_TONEEQ_MEAN = 0,
  DT_TONEEQ_LIGHTNESS,
  DT_TONEEQ_VALUE,
  DT_TONEEQ_NORM_1,
  DT_TONEEQ_NORM_2,
  DT_TONEEQ_NORM_POWER,
  DT_TONEEQ_GEOMEAN,
  DT_TONEEQ_LAST
} dt_iop_toneequalizer_method_t;


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
  dt_iop_toneequalizer_method_t method;
  int iterations;
} dt_iop_toneequalizer_params_t;


typedef struct dt_iop_toneequalizer_data_t
{
  float factors[PIXEL_CHAN] DT_ALIGNED_ARRAY;
  float blending, feathering, contrast_boost, exposure_boost, quantization, smoothing;
  float scale;
  int radius;
  int iterations;
  dt_iop_toneequalizer_method_t method;
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
  int histogram[UI_SAMPLES] DT_ALIGNED_ARRAY; // histogram for the UI graph

  // 8 int to pack - contiguous memory
  int mask_display;
  int max_histogram;
  int buf_width;
  int buf_height;
  int cursor_pos_x;
  int cursor_pos_y;
  int valid_image_cursor; // TRUE if mouse is over the image
  int pipe_order;

  // 9 uint64 to pack - contiguous-ish memory
  uint64_t hash;
  uint64_t histogram_hash;
  size_t image_cursor_x, image_cursor_y; // coordinates of the mouse in the exposure buffer plan
  size_t full_preview_buf_width, full_preview_buf_height;
  size_t thumb_preview_buf_width, thumb_preview_buf_height;
  size_t histogram_num_elem;

  // Misc stuff, contiguity, length and alignment unknown
  float scale;
  float sigma;
  dt_pthread_mutex_t lock;

  // Heap arrays, 64 bits-aligned, unknown length
  float *thumb_preview_buf DT_ALIGNED_ARRAY;
  float *full_preview_buf DT_ALIGNED_ARRAY;

  // GTK garbage, nobody cares, no SIMD here
  GtkWidget *noise, *ultra_deep_blacks, *deep_blacks, *blacks, *shadows, *midtones, *highlights, *whites, *speculars;
  GtkDrawingArea *area;
  GtkWidget *colorpicker;
  dt_iop_color_picker_t color_picker;
  GtkWidget *blending, *smoothing, *quantization;
  GtkWidget *method;
  GtkWidget *details, *feathering, *contrast_boost, *iterations, *exposure_boost;
  GtkNotebook *notebook;
  GtkWidget *show_luminance_mask;

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
      dt_iop_toneequalizer_method_t method;
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
  dt_pthread_mutex_unlock(&g->lock);
  dt_dev_reprocess_all(self->dev);
  gtk_widget_queue_draw(self->widget);
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
    dt_control_log(_("disabling tone equalizer: move it after distorsion modules in the pipeline"));
    self->enabled = 0;

    if(self->dev->gui_attached)
    {
      dt_iop_request_focus(self);
      dt_dev_modules_update_multishow(self->dev);
      dt_dev_add_history_item(self->dev, self, FALSE);
      self->enabled = 0; // ensure stupid module is disabled because dt_dev_ædd_history_item might re-enable it

      // Repaint the on/off icon
      if(self->off)
      {
        darktable.gui->reset = 1;
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->off), self->enabled);
        darktable.gui->reset = 0;
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


__DT_CLONE_TARGETS__
static inline void simd_memcpy(const float *const restrict in,
                            float *const restrict out,
                            const size_t num_elem)
{
  // Perform a parallel vectorized memcpy on 64-bits aligned
  // contiguous buffers. This is several times faster than the original memcpy

#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
dt_omp_firstprivate(in, out, num_elem) \
schedule(static) aligned(in, out:64)
#endif
  for(size_t k = 0; k < num_elem; k++)
    out[k] = in[k];
}


/**
 *
 * Lightness map computation
 *
 * These functions are all written to be vectorizable, using the base image pointer and
 * the explicit index of the current pixel. They perform exposure and contrast compensation
 * as well, for better cache handling.
 *
 * The outputs are clipped to avoid negative and close-to-zero results that could
 * backfire in the exposure computations.
 **/

#ifdef _OPENMP
#pragma omp declare simd
#endif
__DT_CLONE_TARGETS__
static float linear_contrast(const float pixel, const float fulcrum, const float contrast)
{
  return fmaxf((pixel - fulcrum) * contrast + fulcrum, MIN_FLOAT);
}


#ifdef _OPENMP
#pragma omp declare simd aligned(image, luminance:64) uniform(image, luminance)
#endif
__DT_CLONE_TARGETS__
static void pixel_rgb_mean(const float *const restrict image,
                           float *const restrict luminance,
                           const size_t k, const size_t ch,
                           const float exposure_boost,
                           const float fulcrum, const float contrast_boost)
{
  float lum = 0.0f;

#ifdef _OPENMP
#pragma omp simd reduction(+:lum) aligned(image:64)
#endif
  for(int c = 0; c < 3; ++c)
    lum += image[k + c];

  luminance[k / ch] = linear_contrast(exposure_boost * lum / 3.0f, fulcrum, contrast_boost);
}


#ifdef _OPENMP
#pragma omp declare simd aligned(image, luminance:64) uniform(image, luminance)
#endif
__DT_CLONE_TARGETS__
static void pixel_rgb_value(const float *const restrict image,
                            float *const restrict luminance,
                            const size_t k, const size_t ch,
                            const float exposure_boost,
                            const float fulcrum, const float contrast_boost)
{
  float lum = exposure_boost * fmaxf(fmaxf(image[k], image[k + 1]), image[k + 2]);
  luminance[k / ch] = linear_contrast(lum, fulcrum, contrast_boost);
}


#ifdef _OPENMP
#pragma omp declare simd aligned(image, luminance:64) uniform(image, luminance)
#endif
__DT_CLONE_TARGETS__
static void pixel_rgb_lightness(const float *const restrict image,
                                float *const restrict luminance,
                                const size_t k, const size_t ch,
                                const float exposure_boost,
                                const float fulcrum, const float contrast_boost)
{
  const float max_rgb = fmaxf(fmaxf(image[k], image[k + 1]), image[k + 2]);
  const float min_rgb = fminf(fminf(image[k], image[k + 1]), image[k + 2]);
  luminance[k / ch] = linear_contrast(exposure_boost * (max_rgb + min_rgb) / 2.0f, fulcrum, contrast_boost);
}

#ifdef _OPENMP
#pragma omp declare simd aligned(image, luminance:64) uniform(image, luminance)
#endif
__DT_CLONE_TARGETS__
static void pixel_rgb_norm_1(const float *const restrict image,
                             float *const restrict luminance,
                             const size_t k, const size_t ch,
                             const float exposure_boost,
                             const float fulcrum, const float contrast_boost)
{
  float lum = 0.0f;

  #ifdef _OPENMP
  #pragma omp simd reduction(+:lum) aligned(image:64)
  #endif
    for(int c = 0; c < 3; ++c)
      lum += fabsf(image[k + c]);

  luminance[k / ch] = linear_contrast(exposure_boost * lum, fulcrum, contrast_boost);
}


#ifdef _OPENMP
#pragma omp declare simd aligned(image, luminance:64) uniform(image, luminance)
#endif
__DT_CLONE_TARGETS__
static void pixel_rgb_norm_2(const float *const restrict image,
                             float *const restrict luminance,
                             const size_t k, const size_t ch,
                             const float exposure_boost,
                             const float fulcrum, const float contrast_boost)
{
  float result = 0.0f;

#ifdef _OPENMP
#pragma omp simd aligned(image:64) reduction(+: result)
#endif
  for(int c = 0; c < 3; ++c) result += image[k + c] * image[k + c];

  luminance[k / ch] = linear_contrast(exposure_boost * sqrtf(result), fulcrum, contrast_boost);
}


#ifdef _OPENMP
#pragma omp declare simd aligned(image, luminance:64) uniform(image, luminance)
#endif
__DT_CLONE_TARGETS__
static void pixel_rgb_norm_power(const float *const restrict image,
                                 float *const restrict luminance,
                                 const size_t k, const size_t ch,
                                 const float exposure_boost,
                                 const float fulcrum, const float contrast_boost)
{
  float numerator = 0.0f;
  float denominator = 0.0f;

#ifdef _OPENMP
#pragma omp simd aligned(image:64) reduction(+:numerator, denominator)
#endif
  for(int c = 0; c < 3; ++c)
  {
    const float value = image[k + c];
    const float RGB_square = value * value;
    const float RGB_cubic = RGB_square * value;
    numerator += RGB_cubic;
    denominator += RGB_square;
  }

  luminance[k / ch] = linear_contrast(exposure_boost * numerator / denominator, fulcrum, contrast_boost);
}

#ifdef _OPENMP
#pragma omp declare simd aligned(image, luminance:64) uniform(image, luminance)
#endif
__DT_CLONE_TARGETS__
static void pixel_rgb_geomean(const float *const restrict image,
                              float *const restrict luminance,
                              const size_t k, const size_t ch,
                              const float exposure_boost,
                              const float fulcrum, const float contrast_boost)
{
  float lum = 0.0f;

#ifdef _OPENMP
#pragma omp simd aligned(image:64) reduction(*:lum)
#endif
  for(int c = 0; c < 3; ++c)
  {
    lum *= image[k + c];
  }

  luminance[k / ch] = linear_contrast(exposure_boost * powf(lum, 1.0f / 3.0f), fulcrum, contrast_boost);
}


// Overkill trick to explicitely unswitch loops
// GCC should to it automatically with "funswitch-loops" flag,
// but not sure about Clang
#ifdef _OPENMP
  #define LOOP(fn)                                                        \
    {                                                                     \
      _Pragma ("omp parallel for simd default(none) schedule(static)      \
      dt_omp_firstprivate(num_elem, ch, in, out, exposure_boost, fulcrum, contrast_boost)\
      aligned(in, out:64)" )                                              \
      for(size_t k = 0; k < num_elem; k += ch)                            \
      {                                                                   \
        fn(in, out, k, ch, exposure_boost, fulcrum, contrast_boost);      \
      }                                                                   \
      break;                                                              \
    }
#else
  #define LOOP(fn)                                                        \
    {                                                                     \
      for(size_t k = 0; k < num_elem; k += ch)                            \
      {                                                                   \
        fn(in, out, k, ch, exposure_boost, fulcrum, contrast_boost);      \
      }                                                                   \
      break;                                                              \
    }
#endif


__DT_CLONE_TARGETS__
static void luminance_mask(const float *const restrict in, float *const restrict out,
                           const size_t width, const size_t height, const size_t ch,
                           const dt_iop_toneequalizer_method_t method,
                           const float exposure_boost,
                           const float fulcrum, const float contrast_boost)
{
  const size_t num_elem = width * height * ch;
  switch(method)
  {
    case DT_TONEEQ_MEAN:
      LOOP(pixel_rgb_mean);

    case DT_TONEEQ_LIGHTNESS:
      LOOP(pixel_rgb_lightness);

    case DT_TONEEQ_VALUE:
      LOOP(pixel_rgb_value);

    case DT_TONEEQ_NORM_1:
      LOOP(pixel_rgb_norm_1);

    case DT_TONEEQ_NORM_2:
      LOOP(pixel_rgb_norm_2);

    case DT_TONEEQ_NORM_POWER:
      LOOP(pixel_rgb_norm_power);

    case DT_TONEEQ_GEOMEAN:
      LOOP(pixel_rgb_geomean);

    default:
      break;
  }
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
  dt_omp_firstprivate(correction, num_elem, luminance, factors, centers, gauss_denom)
#endif
  for(size_t k = 0; k < num_elem; ++k)
  {
    // build the correction for the current pixel
    // as the sum of the contribution of each luminance channelcorrection
    float result = 0.0f;
    const float exposure = log2f(luminance[k]);

#ifdef _OPENMP
#pragma omp simd aligned(luminance, centers, factors:64) safelen(PIXEL_CHAN) reduction(+:result)
#endif
    for(int i = 0; i < PIXEL_CHAN; ++i)
      result += gaussian_func(exposure - centers[i], gauss_denom) * factors[i];

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
#pragma omp simd aligned(centers, factors:64) safelen(PIXEL_CHAN) reduction(+:result)
#endif
  for(int i = 0; i < PIXEL_CHAN; ++i)
    result += gaussian_func(exposure - centers[i], gauss_denom) * factors[i];

  return result;
}


__DT_CLONE_TARGETS__
static void compute_lut_correction(float *const restrict LUT,
                                   const float *const restrict factors,
                                   const float sigma,
                                   const float offset,
                                   const float scaling)
{
  // Compute the LUT of the exposure corrections in EV,
  // offset and scale it for display in GUI wuidget
#ifdef _OPENMP
#pragma omp parallel for simd schedule(static) default(none) \
  dt_omp_firstprivate(factors, sigma, offset, scaling) \
  shared(LUT) aligned(LUT, factors:64)
#endif
  for(int k = 0; k < UI_SAMPLES; k++)
  {
    // build the inset graph curve LUT
    // the x range is [-14;+2] EV
    const float x = (16.0f * (((float)k) / ((float)(UI_SAMPLES - 1)))) - 14.0f;
    LUT[k] = offset - log2f(pixel_correction(x, factors, sigma)) / scaling;
  }
}


/***
 * Fast Guided filter computation
 *
 * Since the guided filter is a linear application, we can safely downscale
 * the guiding and the guided image by a factor of 4, using a bilinear interpolation,
 * compute the guidance at this scale, then upscale back to the original size
 * and get a "free" 10× speed-up.
 *
 * Reference : 
 * Kaiming He, Jian Sun, Microsoft : https://arxiv.org/abs/1505.00996
 **/

__DT_CLONE_TARGETS__
static inline void interpolate_bilinear(const float *const restrict in, const size_t width_in, const size_t height_in,
                         float *const restrict out, const size_t width_out, const size_t height_out,
                         const size_t ch)
{
#ifdef _OPENMP
#pragma omp parallel for simd collapse(2) default(none) schedule(static) aligned(in, out:64) \
  dt_omp_firstprivate(in, out, width_out, height_out, width_in, height_in, ch)
#endif
  for(size_t i = 0; i < height_out; i++)
  {
    for(size_t j = 0; j < width_out; j++)
    {
      // Relative coordinates of the pixel in output space
      const float x_out = (float)j /(float)width_out;
      const float y_out = (float)i /(float)height_out;

      // Corresponding absolute coordinates of the pixel in input space
      const float x_in = x_out * (float)width_in;
      const float y_in = y_out * (float)height_in;

      // Nearest neighbours coordinates in input space
      size_t x_prev = (size_t)floorf(x_in);
      size_t x_next = x_prev + 1;
      size_t y_prev = (size_t)floorf(y_in);
      size_t y_next = y_prev + 1;

      x_prev = (x_prev < width_in) ? x_prev : width_in - 1;
      x_next = (x_next < width_in) ? x_next : width_in - 1;
      y_prev = (y_prev < height_in) ? y_prev : height_in - 1;
      y_next = (y_next < height_in) ? y_next : height_in - 1;

      // Nearest pixels in input array (nodes in grid)
      const size_t Y_prev = y_prev * width_in;
      const size_t Y_next =  y_next * width_in;
      const float *const Q_NW = (float *)in + (Y_prev + x_prev) * ch;
      const float *const Q_NE = (float *)in + (Y_prev + x_next) * ch;
      const float *const Q_SE = (float *)in + (Y_next + x_next) * ch;
      const float *const Q_SW = (float *)in + (Y_next + x_prev) * ch;

      // Spatial differences between nodes
      const float Dy_next = (float)y_next - y_in;
      const float Dy_prev = 1.f - Dy_next; // because next - prev = 1
      const float Dx_next = (float)x_next - x_in;
      const float Dx_prev = 1.f - Dx_next; // because next - prev = 1

      // Interpolate over ch layers
      float *const pixel_out = (float *)out + (i * width_out + j) * ch;
      for(size_t c = 0; c < ch; c++)
      {
        pixel_out[c] = Dy_prev * (Q_SW[c] * Dx_next + Q_SE[c] * Dx_prev) +
                       Dy_next * (Q_NW[c] * Dx_next + Q_NE[c] * Dx_prev);
      }
    }
  }
}


__DT_CLONE_TARGETS__
static inline void variance_analyse(const float *const restrict guide, // I
                                    const float *const restrict mask, //p
                                    float *const restrict a, float *const restrict b,
                                    const size_t width, const size_t height,
                                    const int radius, const float feathering)
{
  // Compute a box average (filter) on a grey image over a window of size 2*radius + 1
  // then get the variance of the guide and covariance with its mask
  // output a and b, the linear blending params
  // p, the mask is the quantised guide I

  float *const mean_I = dt_alloc_align(64, width * height * sizeof(float));
  float *const mean_p = dt_alloc_align(64, width * height * sizeof(float));
  float *const corr_I = dt_alloc_align(64, width * height * sizeof(float));
  float *const corr_Ip = dt_alloc_align(64, width * height * sizeof(float));

  // Convolve box average along columns
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(guide, mask, mean_I, mean_p, corr_I, corr_Ip, width, height, radius) \
  schedule(static) collapse(2)
#endif
  for(size_t i = 0; i < height; i++)
  {
    for(size_t j = 0; j < width; j++)
    {
      const size_t begin_convol = (i < radius) ? 0 : i - radius;
      size_t end_convol = i + radius;
      end_convol = (end_convol < height) ? end_convol : height - 1;
      const float num_elem = (float)end_convol - (float)begin_convol + 1.0f;

      float w_mean_I = 0.0f;
      float w_mean_p = 0.0f;
      float w_corr_I = 0.0f;
      float w_corr_Ip = 0.0f;

#ifdef _OPENMP
#pragma omp simd aligned(guide, mask, mean_I, mean_p, corr_I, corr_Ip:64) reduction(+:w_mean_I, w_mean_p, w_corr_I, w_corr_Ip)
#endif
      for(size_t c = begin_convol; c <= end_convol; c++)
      {
        w_mean_I += guide[c * width + j];
        w_mean_p += mask[c * width + j];
        w_corr_I += guide[c * width + j] * guide[c * width + j];
        w_corr_Ip += mask[c * width + j] * guide[c * width + j];
      }

      mean_I[i * width + j] = w_mean_I / num_elem;
      mean_p[i * width + j] = w_mean_p / num_elem;
      corr_I[i * width + j] = w_corr_I / num_elem;
      corr_Ip[i * width + j] = w_corr_Ip / num_elem;
    }
  }

  // Convolve box average along rows and output result
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(a, b, mean_I, mean_p, corr_I, corr_Ip, width, height, radius, feathering) \
  schedule(static) collapse(2)
#endif
  for(size_t i = 0; i < height; i++)
  {
    for(size_t j = 0; j < width; j++)
    {
      const size_t begin_convol = (j < radius) ? 0 : j - radius;
      size_t end_convol = j + radius;
      end_convol = (end_convol < width) ? end_convol : width - 1;
      const float num_elem = (float)end_convol - (float)begin_convol + 1.0f;

      float w_mean_I = 0.0f;
      float w_mean_p = 0.0f;
      float w_corr_I = 0.0f;
      float w_corr_Ip = 0.0f;

#ifdef _OPENMP
#pragma omp simd aligned(a, b, mean_I, mean_p, corr_I, corr_Ip:64) reduction(+:w_mean_I, w_mean_p, w_corr_I, w_corr_Ip)
#endif
      for(size_t c = begin_convol; c <= end_convol; c++)
      {
        w_mean_I += mean_I[i * width + c];
        w_mean_p += mean_p[i * width + c];
        w_corr_I += corr_I[i * width + c];
        w_corr_Ip += corr_Ip[i * width + c];
      }

      w_mean_I /= num_elem;
      w_mean_p /= num_elem;
      w_corr_I /= num_elem;
      w_corr_Ip /= num_elem;

      float var_I = w_corr_I - w_mean_I * w_mean_I;
      float cov_Ip = w_corr_Ip - w_mean_I * w_mean_p;

      a[i * width + j] = cov_Ip / (var_I + feathering);
      b[i * width + j] = w_mean_p - a[i * width + j] * w_mean_I;
    }
  }

  dt_free_align(mean_I);
  dt_free_align(mean_p);
  dt_free_align(corr_I);
  dt_free_align(corr_Ip);
}


__DT_CLONE_TARGETS__
static inline void box_average(float *const restrict in,
                               const size_t width, const size_t height,
                               const int radius)
{
  // Compute in-place a box average (filter) on a grey image over a window of size 2*radius + 1
  // We make use of the separable nature of the filter kernel to speed-up the computation
  // by convolving along columns and rows separately (complexity O(2 × radius) instead of O(radius²)).

  float *const temp = dt_alloc_align(64, width * height * sizeof(float));

  // Convolve box average along columns
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(in, temp, width, height, radius) \
  schedule(static) collapse(2)
#endif
  for(size_t i = 0; i < height; i++)
  {
    for(size_t j = 0; j < width; j++)
    {
      const size_t begin_convol = (i < radius) ? 0 : i - radius;
      size_t end_convol = i + radius;
      end_convol = (end_convol < height) ? end_convol : height - 1;
      const float num_elem = (float)end_convol - (float)begin_convol + 1.0f;

      float w = 0.0f;

#ifdef _OPENMP
#pragma omp simd aligned(in, temp:64) reduction(+:w)
#endif
      for(size_t c = begin_convol; c <= end_convol; c++)
      {
        w += in[c * width + j];
      }

      temp[i * width + j] = w / num_elem;
    }
  }

  // Convolve box average along rows and output result
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(in, temp, width, height, radius) \
  schedule(static) collapse(2)
#endif
  for(size_t i = 0; i < height; i++)
  {
    for(size_t j = 0; j < width; j++)
    {
      const size_t begin_convol = (j < radius) ? 0 : j - radius;
      size_t end_convol = j + radius;
      end_convol = (end_convol < width) ? end_convol : width - 1;
      const float num_elem = (float)end_convol - (float)begin_convol + 1.0f;

      float w = 0.0f;

#ifdef _OPENMP
#pragma omp simd aligned(in, temp:64) reduction(+:w)
#endif
      for(size_t c = begin_convol; c <= end_convol; c++)
      {
        w += temp[i * width + c];
      }

      in[i * width + j] = w / num_elem;
    }
  }

  dt_free_align(temp);
}


__DT_CLONE_TARGETS__
static inline void apply_linear_blending(float *const restrict image,
                                    const float *const restrict a,
                                    const float *const restrict b,
                                    const size_t num_elem)
{
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
dt_omp_firstprivate(image, a, b, num_elem) \
schedule(static) aligned(image, a, b:64)
#endif
  for(size_t k = 0; k < num_elem; k++)
  {
    // Note : image[k] is positive at the outside of the luminance mask
    image[k] = fmaxf(image[k] * a[k] + b[k], MIN_FLOAT);
  }
}


__DT_CLONE_TARGETS__
static inline void apply_linear_blending_w_geomean(float *const restrict image,
                                                   const float *const restrict a,
                                                   const float *const restrict b,
                                                   const size_t num_elem)
{
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
dt_omp_firstprivate(image, a, b, num_elem) \
schedule(static) aligned(image, a, b:64)
#endif
  for(size_t k = 0; k < num_elem; k++)
  {
    // Note : image[k] is positive at the outside of the luminance mask
    image[k] = sqrtf(image[k] * fmaxf(image[k] * a[k] + b[k], MIN_FLOAT));
  }
}


__DT_CLONE_TARGETS__
static inline void quantize(const float *const restrict image,
                            float *const restrict out,
                            const size_t num_elem,
                            const float sampling)
{
  // Quantize in exposure levels evenly spaced in log by sampling

  if(sampling == 0.0f)
  {
    // No-op
    simd_memcpy(image, out, num_elem);
  }

  else if(sampling == 1.0f)
  {
    // fast track
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
dt_omp_firstprivate(image, out, num_elem, sampling) \
schedule(static) aligned(image, out:64)
#endif
    for(size_t k = 0; k < num_elem; k++)
    {
      out[k] = exp2f(floorf(log2f(image[k])));
    }
  }

  else
  {
    // slow track
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
dt_omp_firstprivate(image, out, num_elem, sampling) \
schedule(static) aligned(image, out:64)
#endif
    for(size_t k = 0; k < num_elem; k++)
    {
      out[k] = exp2f(floorf(log2f(image[k]) / sampling) * sampling);
    }
  }
}


__DT_CLONE_TARGETS__
static inline void guided_filter(float *const restrict image,
                                 const size_t width, const size_t height,
                                 const int radius, float feathering, const int iterations,
                                 const dt_iop_toneequalizer_filter_t filter, const float scale,
                                 const float quantization)
{
  // Works in-place on a grey image

  // A down-scaling of 4 seems empirically safe and consistent no matter the image zoom level
  const float scaling = 4.0f;
  const size_t ds_height = height / scaling;
  const size_t ds_width = width / scaling;
  int ds_radius = (radius < 4) ? 1 : radius / scaling;

  // Downsample the image for speed-up
  float *ds_image = dt_alloc_align(64, ds_width * ds_height * sizeof(float));
  interpolate_bilinear(image, width, height, ds_image, ds_width, ds_height, 1);

  float *ds_mask = dt_alloc_align(64, ds_width * ds_height * sizeof(float));
  float *ds_a = dt_alloc_align(64, ds_width * ds_height * sizeof(float));
  float *ds_b = dt_alloc_align(64, ds_width * ds_height * sizeof(float));

  // Iterations of filter model diffusion, sort of
  for(int i = 0; i < iterations; ++i)
  {
    // (Re)build the mask from the quantized image to help guiding
    quantize(ds_image, ds_mask, ds_width * ds_height, quantization);

    // Perform the patch-wise variance analyse to get
    // the a and b parameters for the linear blending s.t. mask = a * I + b
    variance_analyse(ds_image, ds_mask, ds_a, ds_b, ds_width, ds_height, ds_radius, feathering);

    // Compute the patch-wise average of parameters a and b
    box_average(ds_a, ds_width, ds_height, ds_radius);
    box_average(ds_b, ds_width, ds_height, ds_radius);

    if(i != iterations - 1)
    {
      // Process the intermediate filtered image
      apply_linear_blending(ds_image, ds_a, ds_b, ds_width * ds_height);
    }
    else
    {
      // Increase the radius for the next iteration
      ds_radius *= sqrtf(2.0f);
      //feathering *= sqrtf(2.0f);
    }
  }
  dt_free_align(ds_mask);
  dt_free_align(ds_image);

  // Upsample the blending parameters a and b
  const size_t num_elem_2 = width * height;
  float *a = dt_alloc_align(64, num_elem_2 * sizeof(float));
  float *b = dt_alloc_align(64, num_elem_2 * sizeof(float));
  interpolate_bilinear(ds_a, ds_width, ds_height, a, width, height, 1);
  interpolate_bilinear(ds_b, ds_width, ds_height, b, width, height, 1);
  dt_free_align(ds_a);
  dt_free_align(ds_b);

  // Finally, blend the guided image and boost contrast
  if(filter == DT_TONEEQ_GUIDED)
  {
    apply_linear_blending(image, a, b, num_elem_2);
  }
  else if(filter == DT_TONEEQ_AVG_GUIDED)
  {
    apply_linear_blending_w_geomean(image, a, b, num_elem_2);
  }

  dt_free_align(a);
  dt_free_align(b);
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
      guided_filter(luminance, width, height, d->radius, d->feathering, d->iterations,
                    d->details, d->scale, d->quantization);
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
      guided_filter(luminance, width, height, d->radius, d->feathering, d->iterations,
                    d->details, d->scale, d->quantization);
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
    const int index = CLAMP((int)(((log2f(luminance[k]) + 14.0f) / 16.0f) * (float)UI_SAMPLES), 0, UI_SAMPLES - 1);
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
        out[(i * out_width + j) * ch + c] = powf(luminance[(i + offset_y) * in_width  + (j + offset_x)], 0.5f);
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
  float *correction = dt_alloc_align(64, num_elem * sizeof(float));
  compute_correction(luminance, correction, d->factors, d->smoothing, num_elem);
  apply_exposure(in, out, roi_in, roi_out, ch, correction);
  dt_free_align(correction);
}


__DT_CLONE_TARGETS__
void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_toneequalizer_data_t *const d = (const dt_iop_toneequalizer_data_t *const)piece->data;
  dt_iop_toneequalizer_gui_data_t *const g = (dt_iop_toneequalizer_gui_data_t *)self->gui_data;

  const float *const in  = (float *const)ivoid;
  float *const out  = (float *const)ovoid;
  float *luminance;

  const size_t width = roi_in->width;
  const size_t height = roi_in->height;
  const size_t num_elem = width * height;
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
      dt_pthread_mutex_unlock(&g->lock);
    }

    if(piece->pipe->type == DT_DEV_PIXELPIPE_FULL)
    {
      // For DT_DEV_PIXELPIPE_FULL, we cache the luminance mask for performance
      // no need for threads lock since no other function is writing/reading that buffer

      // Re-allocate a new buffer if the full preview size has changed
      if(g->full_preview_buf_width != width || g->full_preview_buf_height != height)
      {
        if(g->full_preview_buf) dt_free_align(g->full_preview_buf);
        g->full_preview_buf = dt_alloc_align(64, num_elem * sizeof(float));
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
      // threads locks are required since GUI reads and write on that buffer.

      // Re-allocate a new buffer if the thumb preview size has changed
      dt_pthread_mutex_lock(&g->lock);
      if(g->thumb_preview_buf_width != width || g->thumb_preview_buf_height != height)
      {
        if(g->thumb_preview_buf) dt_free_align(g->thumb_preview_buf);
        g->thumb_preview_buf = dt_alloc_align(64, num_elem * sizeof(float));
        g->thumb_preview_buf_width = width;
        g->thumb_preview_buf_height = height;
      }

      luminance = g->thumb_preview_buf;
      cached = TRUE;

      dt_pthread_mutex_unlock(&g->lock);
    }
    else // just to please GCC
    {
      luminance = dt_alloc_align(64, num_elem * sizeof(float));
    }

  }
  else
  {
    // no interactive editing/caching : just allocate a local temp buffer
    luminance = dt_alloc_align(64, num_elem * sizeof(float));
  }

  // Compute the luminance mask
  if(cached)
  {
    // caching path : store the luminance mask for GUI access

    if(piece->pipe->type == DT_DEV_PIXELPIPE_FULL)
    {
      uint64_t saved_hash;
      hash_set_get(&g->hash, &saved_hash, &g->lock);

      if(hash != saved_hash)
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

      if(saved_hash != hash)
      {
        /* compute only if upstream pipe state has changed */
        dt_pthread_mutex_lock(&g->lock);
        compute_luminance_mask(in, luminance, width, height, ch, d);
        compute_log_histogram(luminance, g->histogram, num_elem, &g->max_histogram);
        g->histogram_num_elem = num_elem;
        g->histogram_hash = hash;
        dt_pthread_mutex_unlock(&g->lock);
        gtk_widget_queue_draw(self->widget);
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

  // Enlarge the roi with padding if needed
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
  const float sigma = d->smoothing;

  // UI blending param is set in % of the largest image dimension
  d->blending = p->blending / 100.0f;

  // UI guided filter feathering param increases the edges taping
  // but the actual regularization params applied in guided filter behaves the other way
  d->feathering = 1.f / (p->feathering);

  // UI params are in log2 offsets (EV) : convert to linear factors
  d->contrast_boost = exp2f(p->contrast_boost);
  d->exposure_boost = exp2f(p->exposure_boost);

  // BIG stuff : gains for channels
  float factors[PIXEL_CHAN]  DT_ALIGNED_ARRAY = {
                                0.0, p->noise, p->noise, p->noise, // padding
                                p->noise,             // -14 EV
                                p->ultra_deep_blacks, // -12 EV
                                p->deep_blacks,       // -10 EV
                                p->blacks,            //  -8 EV
                                p->shadows,           //  -6 EV
                                p->midtones,          //  -4 EV
                                p->highlights,        //  -2 EV
                                p->whites,            //   0 EV
                                p->speculars,         //  +2 EV
                                p->speculars, p->speculars, p->speculars}; // padding

  // Convert the user-set channels gains from log offsets (EV) to linear coefficients
#ifdef _OPENMP
#pragma omp simd aligned(factors:64)
#endif
  for(int c = 0; c < PIXEL_CHAN; ++c)
    factors[c] = exp2f(factors[c]);

  // Get the actual interpolation factors to match exactly the user params
  float A[PIXEL_CHAN * PIXEL_CHAN] DT_ALIGNED_ARRAY;
  const float gauss_denom = gaussian_denom(sigma);

  // Build the symmetrical definite positive matrix of the radial-based interpolation weights
#ifdef _OPENMP
#pragma omp parallel for simd schedule(static) default(none) \
  dt_omp_firstprivate(centers, gauss_denom) shared(A) aligned(A, centers:64) collapse(2)
#endif
  for(int i = 0; i < PIXEL_CHAN; ++i)
    for(int j = 0; j < PIXEL_CHAN; ++j)
      A[i * PIXEL_CHAN + j] = gaussian_func(centers[i] - centers[j], gauss_denom);

  // Solve the linear system
  // TODO: this is by design a sparse with 1 on the diagonal, so faster methods than Gauss-Jordan are available
  gauss_solve_f(A, factors, PIXEL_CHAN);
  simd_memcpy(factors, d->factors, PIXEL_CHAN);

  // Build the GUI elements
  if(self->dev->gui_attached && piece->pipe->type == DT_DEV_PIXELPIPE_PREVIEW)
  {
    dt_pthread_mutex_lock(&g->lock);
    g->sigma = sigma;
    simd_memcpy(factors, g->factors, PIXEL_CHAN);
    compute_lut_correction(g->gui_lut, g->factors, g->sigma, 0.5f, 4.0f);
    dt_pthread_mutex_unlock(&g->lock);
    gtk_widget_queue_draw(self->widget);
  }

  fprintf(stdout, "pipe type %i\n", piece->pipe->type);
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
                                                                      .quantization = 0.1f,
                                                                      .smoothing = 2.0f,
                                                                      .iterations = 2,
                                                                      .method = DT_TONEEQ_NORM_POWER,
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

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->show_luminance_mask), FALSE);
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
  show_guiding_controls(self);

  invalidate_luminance_cache(self);
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
  gtk_widget_queue_draw(self->widget);
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

  // The goal is to get the exposure distribution centered on the equalizer view
  // to spread it over as many nodes as possible for better exposure control.

  dt_pthread_mutex_lock(&g->lock);
  if(g->thumb_preview_buf)
  {
    const size_t num_elem = g->thumb_preview_buf_height * g->thumb_preview_buf_width;

    // Controls nodes are between -12 and 0 EV,
    // with 2 extra nodes at +2 and -14 EV for boundary conditions
    // so we aim at centering the exposure distribution on -6 EV
    const float target = log2f(CONTRAST_FULCRUM);
    const float origin = log2f(flat_pseudo_norm(g->thumb_preview_buf, num_elem));
    p->exposure_boost += target - origin;

    fprintf(stdout, "average is %f, correction is %f\n", origin, target - origin);
  }
  dt_pthread_mutex_unlock(&g->lock);

  // Update the GUI stuff
  dt_bauhaus_slider_set_soft(g->exposure_boost, p->exposure_boost);
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

  // The goal is to spread 80 % of the exposure histogram between average exposure ± 4 EV

  float first_quartile, last_quartile, average;

  if(g->thumb_preview_buf && g->histogram)
  {
    // Fetch the quartiles
    dt_pthread_mutex_lock(&g->lock);
    histogram_deciles(g->histogram, UI_SAMPLES, g->histogram_num_elem,
                      16.0f, // histogram spans over 16 EV
                      -14.0f, // histogram begins at -14 EV
                      &first_quartile, &last_quartile);

    // Get the average
    const size_t num_elem = g->thumb_preview_buf_height * g->thumb_preview_buf_width;
    average = log2f(flat_pseudo_norm(g->thumb_preview_buf, num_elem));
    dt_pthread_mutex_unlock(&g->lock);

    const float span_left = fabsf(average - first_quartile);
    const float span_right = fabsf(last_quartile - average);
    const float origin = fmaxf(span_left, span_right);

    // Compute the correction
    p->contrast_boost = (4.0f - origin) / 2.0f;
    fprintf(stdout, "span: %f, average: %f, correction:%f, new value: %f\n", origin, average, 4.0f - origin, p->contrast_boost);

    // Update the GUI stuff
    dt_bauhaus_slider_set_soft(g->contrast_boost, p->contrast_boost);
    invalidate_luminance_cache(self);
    dt_dev_add_history_item(darktable.develop, self, TRUE);
  }
}


static void show_luminance_mask_callback(GtkToggleButton *togglebutton, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;

  dt_iop_toneequalizer_gui_data_t *g = (dt_iop_toneequalizer_gui_data_t *)self->gui_data;

  // if blend module is displaying mask do not display it here
  if(self->request_mask_display && !g->mask_display)
  {
    dt_control_log(_("cannot display masks when the blending mask is displayed"));

    const int reset = darktable.gui->reset;
    darktable.gui->reset = 1;
    gtk_toggle_button_set_active(togglebutton, FALSE);
    darktable.gui->reset = reset;
    return;
  }

  g->mask_display = gtk_toggle_button_get_active(togglebutton);

  if(self->off) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->off), 1);

  invalidate_luminance_cache(self);
  dt_iop_request_focus(self);
  dt_dev_reprocess_center(self->dev);
}


/***
 * GUI Interactivity
 **/


int mouse_moved(struct dt_iop_module_t *self, double x, double y, double pressure, int which)
{
  // Whenever the mouse moves over the picture preview, store its coordinates in the GUI struct
  // for later use. This works only if dev->preview_pipe perfectly overlaps with the UI preview
  // meaning all distortions, cropping, rotations etc. are applied before this module in the pipe.

  dt_develop_t *dev = self->dev;
  dt_iop_toneequalizer_gui_data_t *g = (dt_iop_toneequalizer_gui_data_t *)self->gui_data;

  if(!dev->preview_pipe) return 1;
  if(self->dt->gui->reset) return 1;

  int *wd = &dev->preview_pipe->backbuf_width;
  int *ht = &dev->preview_pipe->backbuf_height;
  if(*wd < 1 || *ht < 1) return 1;

  float pzx, pzy;
  dt_dev_get_pointer_zoom_pos(dev, x, y, &pzx, &pzy);
  pzx += 0.5f;
  pzy += 0.5f;

  const int x_pointer = pzx * *wd;
  const int y_pointer = pzy * *ht;

  dt_pthread_mutex_lock(&g->lock);

  int previous_image_cursor = g->valid_image_cursor;

  // Cursor is valid if it's inside the picture frame
  if(x_pointer >= 0 && x_pointer < *wd &&
     y_pointer >= 0 && y_pointer < *ht)
    g->valid_image_cursor = TRUE;
  else
    g->valid_image_cursor = FALSE;

  if(g->valid_image_cursor)
  {
    // store the cursor position in buffer
    g->cursor_pos_x = x_pointer;
    g->cursor_pos_y = y_pointer;
    dt_control_change_cursor(GDK_BLANK_CURSOR);
  }
  else if(previous_image_cursor)
  {
    // if the cursor was in image previously but just left, reset cursor
    dt_control_change_cursor(GDK_LEFT_PTR);
  }

  if(g->valid_image_cursor || previous_image_cursor)
    dt_control_queue_redraw_center();

  dt_pthread_mutex_unlock(&g->lock);

  return 0;
}


/*
int scrolled(struct dt_iop_module_t *self, double x, double y, int up, uint32_t state)
{
  dt_develop_t *dev = self->dev;
  dt_iop_toneequalizer_global_data_t *gd = (dt_iop_toneequalizer_global_data_t *)self->global_data;
  dt_iop_toneequalizer_gui_data_t *g = (dt_iop_toneequalizer_gui_data_t *)self->gui_data;

  if(!dev->preview_pipe) return 1;
  if(self->dt->gui->reset) return 1;

  dt_pthread_mutex_lock(&g->lock);
  int valid_cursor = g->valid_image_cursor;
  dt_pthread_mutex_unlock(&g->lock);

  if(gd->full_preview_buf && valid_cursor)
  {
    // If we have an exposure mask and pointer is inside the image borders
    size_t bufferx, buffery;
    dt_pthread_mutex_lock(&g->lock);
    bufferx = g->cursor_pos_x;
    buffery = g->cursor_pos_y;
    dt_pthread_mutex_unlock(&g->lock);

    // Get the corresponding exposure
    dt_pthread_mutex_lock(&gd->lock);
    const float luminance_in = get_luminance_from_buffer(gd->full_preview_buf,
                                                    gd->full_preview_buf_width,
                                                    gd->full_preview_buf_height,
                                                    bufferx, buffery);
    const float exposure_in = log2f(luminance_in);
    dt_pthread_mutex_unlock(&gd->lock);

    // Get the corresponding correction and compute resulting exposure
    dt_pthread_mutex_lock(&g->lock);
    float correction = log2f(pixel_correction(exposure_in, g->factors));
    float exposure_out = exposure_in + correction;
    float luminance_out = exp2f(exposure_out);
    dt_pthread_mutex_unlock(&g->lock);

    // Apply the correction
    // TODO: set the step in preferences
    correction = up ? correction + 0.1f : correction - 0.1f;

    dt_control_queue_redraw_center();
  }

  return FALSE;
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

  if(!dev->preview_pipe) return;
  if(!g->thumb_preview_buf) return;

  dt_pthread_mutex_lock(&g->lock);
  int valid_cursor = g->valid_image_cursor;
  dt_pthread_mutex_unlock(&g->lock);

  if(!valid_cursor) return;

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

  // Get cursor coordinates
  float x_pointer = g->cursor_pos_x;
  float y_pointer = g->cursor_pos_y;

  dt_pthread_mutex_lock(&g->lock);

  // Get the corresponding exposure
  const float luminance_in = get_luminance_from_buffer(g->thumb_preview_buf,
                                                        g->thumb_preview_buf_width,
                                                        g->thumb_preview_buf_height,
                                                        (size_t)x_pointer, (size_t)y_pointer);
  const float exposure_in = log2f(luminance_in);

  // Get the corresponding correction and compute resulting exposure
  const float correction = log2f(pixel_correction(exposure_in, g->factors, g->sigma));
  const float exposure_out = exposure_in + correction;
  const float luminance_out = exp2f(exposure_out);

  dt_pthread_mutex_unlock(&g->lock);

  if(correction < -4.0f || correction > 4.0f) return;

  // set custom cursor dimensions
  const double outer_radius = 16.;
  const double inner_radius = outer_radius / 2.0;
  const int INNER_PADDING = 4; // TODO: INNER_PADDING value as defined in bauhaus.c macros, sync them
  const double setting_scale = 2. * outer_radius / zoom_scale;
  const double setting_offset_x = (outer_radius + 4. * INNER_PADDING) / zoom_scale;

  // setting fill bars
  if(luminance_in > 0.0f)
  {
    dt_match_color_to_background(cr, exposure_out, 1.0);
    cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(6. / zoom_scale));
    cairo_move_to(cr, x_pointer - setting_offset_x, y_pointer);
    cairo_line_to(cr, x_pointer - setting_offset_x, y_pointer - correction * setting_scale);
    cairo_stroke(cr);

    // setting ground level
    cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.5 / zoom_scale));
    cairo_move_to(cr, x_pointer + (outer_radius + 2. * INNER_PADDING) / zoom_scale, y_pointer);
    cairo_line_to(cr, x_pointer + outer_radius / zoom_scale, y_pointer);
    cairo_move_to(cr, x_pointer - outer_radius / zoom_scale, y_pointer);
    cairo_line_to(cr, x_pointer - setting_offset_x - 4.0 * INNER_PADDING / zoom_scale, y_pointer);
    cairo_stroke(cr);

    // setting bullets
    cairo_arc(cr, x_pointer - setting_offset_x, y_pointer - correction * setting_scale, DT_PIXEL_APPLY_DPI(7. / zoom_scale), 0, 2. * M_PI);
    cairo_fill(cr);
  }

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
  cairo_rectangle(cr, x_pointer + (outer_radius + 2. * INNER_PADDING) / zoom_scale,
                      y_pointer - ink.y - ink.height / 2.0 - INNER_PADDING / zoom_scale,
                      ink.width + 2.0 * ink.x + 4. * INNER_PADDING / zoom_scale,
                      ink.height + 2.0 * ink.y + 2. * INNER_PADDING / zoom_scale);
  cairo_fill(cr);

  // Display the EV reading
  dt_match_color_to_background(cr, exposure_out, 1.0);
  cairo_move_to(cr, x_pointer + (outer_radius + 4. * INNER_PADDING) / zoom_scale,
                    y_pointer - ink.y - ink.height / 2.);
  pango_cairo_show_layout(cr, layout);
  cairo_stroke(cr);
}


static gboolean dt_iop_toneequalizer_draw(GtkWidget *widget, cairo_t *crf, gpointer user_data)
{
  // Draw the widget equalizer view

  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_toneequalizer_gui_data_t *g = (dt_iop_toneequalizer_gui_data_t *)self->gui_data;
  dt_iop_toneequalizer_params_t *p = (dt_iop_toneequalizer_params_t *)self->params;

  // Store channels nodes
  const int nodes = 9;
  const float channels[9]  DT_ALIGNED_ARRAY = {
                              p->noise,             // -14 EV
                              p->ultra_deep_blacks, // -12 EV
                              p->deep_blacks,       // -10 EV
                              p->blacks,            //  -8 EV
                              p->shadows,           //  -6 EV
                              p->midtones,          //  -4 EV
                              p->highlights,        //  -2 EV
                              p->whites,            //   0 EV
                              p->speculars };       //  +2EV


  // Create Cairo objects : drawings
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  const int width = allocation.width , height = allocation.height;
  //int offset = height - graph_height;
  cairo_surface_t *cst = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);

  // Create Pango objects : texts
  char text[256];
  PangoLayout *layout;
  PangoRectangle ink;
  PangoFontDescription *desc = pango_font_description_copy_static(darktable.bauhaus->pango_font_desc);
  layout = pango_cairo_create_layout(cr);
  pango_layout_set_font_description(layout, desc);

  // Get the text line height
  snprintf(text, sizeof(text), "X");
  pango_layout_set_text(layout, text, -1);
  pango_layout_get_pixel_extents(layout, &ink, NULL);
  const float line_height = ink.height;

  // Get the width of a minus sign
  snprintf(text, sizeof(text), "-");
  pango_layout_set_text(layout, text, -1);
  pango_layout_get_pixel_extents(layout, &ink, NULL);
  const float sign_width = ink.width / 2.0;

  // Set the sizes
  const int INNER_PADDING = 4; // TODO: INNER_PADDING value as defined in bauhaus.c macros, sync them
  const int inset = INNER_PADDING + darktable.bauhaus->quad_width;

  const float graph_width = width - 2 * inset; // align the right border on sliders
  const float graph_height = graph_width; // give room to nodes

  const float gradient_left_limit = graph_width / ((float) (nodes - 1));
  const float gradient_right_limit = ((float)(nodes - 2)) * graph_width / ((float) (nodes - 1));
  const float gradient_top_limit = graph_height + 2 * INNER_PADDING;
  const float gradient_width = gradient_right_limit - gradient_left_limit;

  const double legend_top_limit = - 0.5 * line_height - 2.0 * INNER_PADDING;

  cairo_translate(cr, line_height + 2 * INNER_PADDING, line_height + 3 * INNER_PADDING); // set the graph as the origin of the coordinates


  // display x-axis and y-axis legends (EV)
  set_color(cr, darktable.bauhaus->graph_fg);

  snprintf(text, sizeof(text), "(EV)");
  pango_layout_set_text(layout, text, -1);
  pango_layout_get_pixel_extents(layout, &ink, NULL);
  cairo_move_to(cr, - ink.x - line_height - 2 * INNER_PADDING,
                  graph_height + 2 * INNER_PADDING - ink.y - ink.height / 2.0 + line_height / 2.0);
  pango_cairo_show_layout(cr, layout);
  cairo_stroke(cr);

  float value = -14.0f;

  for(int k = 0; k < nodes; k++)
  {
    const float xn = (((float)k) / ((float)(nodes - 1))) * graph_width - sign_width;
    snprintf(text, sizeof(text), "%+.0f", value);
    pango_layout_set_text(layout, text, -1);
    pango_layout_get_pixel_extents(layout, &ink, NULL);
    cairo_move_to(cr, xn - 0.5 * ink.width - ink.x,
                legend_top_limit - 0.5 * ink.height - ink.y);
    pango_cairo_show_layout(cr, layout);
    cairo_stroke(cr);

    value += 2.0;
  }

  value = 2.0f;
  const float x_labels = graph_width + sign_width + 3.0 * INNER_PADDING;

  for(int k = 0; k < 5; k++)
  {
    const float yn = k / 4.0f * graph_height;
    snprintf(text, sizeof(text), "%+.0f", value);
    pango_layout_set_text(layout, text, -1);
    pango_layout_get_pixel_extents(layout, &ink, NULL);
    cairo_move_to(cr, x_labels - 0.5 * ink.width - ink.x,
                yn - 0.5 * ink.height - ink.y);
    pango_cairo_show_layout(cr, layout);
    cairo_stroke(cr);

    value -= 1.0;
  }

  // Draw frame borders
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(0.5));
  set_color(cr, darktable.bauhaus->graph_border);
  cairo_rectangle(cr, 0, 0, graph_width, graph_height);
  cairo_stroke_preserve(cr);
  set_color(cr, darktable.bauhaus->graph_bg);
  cairo_fill(cr);

  // draw grid
  set_color(cr, darktable.bauhaus->graph_border);
  dt_draw_grid(cr, 8, 0, 0, graph_width, graph_height);

  // draw ground level
  set_color(cr, darktable.bauhaus->graph_fg);
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1));
  cairo_move_to(cr, 0, 0.5 * graph_height);
  cairo_line_to(cr, graph_width, 0.5 * graph_height);
  cairo_stroke(cr);

  dt_pthread_mutex_lock(&g->lock);
  if(g->max_histogram != 0)
  {
    // draw the inset histogram
    set_color(cr, darktable.bauhaus->inset_histogram);
    cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(4.0));
    cairo_move_to(cr, 0, graph_height);

    for(int k = 0; k < UI_SAMPLES; k++) // computation loop - accelerated
    {
      // the x range is [-14;+2] EV
      const double x_temp = (16.0 * (double)k / (double)(UI_SAMPLES - 1)) - 14.0;
      const double y_temp = (double)(g->histogram[k]) / (double)(g->max_histogram) * 0.96;
      cairo_line_to(cr, (x_temp + 14.0) * graph_width / 16.0,
                         (1.0 - y_temp) * graph_height );
    }
    cairo_line_to(cr, graph_width, graph_height);
    cairo_close_path(cr);
    cairo_fill(cr);

    // draw the interpolation curve
    set_color(cr, darktable.bauhaus->graph_fg);
    cairo_move_to(cr, 0, g->gui_lut[0] * graph_height);
    cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(3));

    for(int k = 1; k < UI_SAMPLES; k++)
    {
      // the x range is [-14;+2] EV
      const float x_temp = (16.0f * (((float)k) / ((float)(UI_SAMPLES - 1)))) - 14.0f;
      const float y_temp = g->gui_lut[k];

      cairo_line_to(cr, (x_temp + 14.0f) * graph_width / 16.0f,
                         y_temp * graph_height );
    }
    cairo_stroke(cr);
  }
  dt_pthread_mutex_unlock(&g->lock);

  // draw nodes positions
  for(int k = 0; k < nodes; k++)
  {
    const float xn = (((float)k) / ((float)(nodes - 1))) * graph_width,
                yn = (0.5 - channels[k] / 4.0) * graph_height; // assumes factors in [-2 ; 2] EV
    // fill bars
    cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(6));
    set_color(cr, darktable.bauhaus->color_fill);
    cairo_move_to(cr, xn, 0.5 * graph_height);
    cairo_line_to(cr, xn, yn);
    cairo_stroke(cr);

    // bullets
    cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(3));
    cairo_arc(cr, xn, yn, DT_PIXEL_APPLY_DPI(4), 0, 2. * M_PI);
    set_color(cr, darktable.bauhaus->graph_fg);
    cairo_stroke_preserve(cr);
    set_color(cr, darktable.bauhaus->graph_bg);
    cairo_fill(cr);
  }

  /** x axis **/
  // Draw the perceptually even gradient
  cairo_pattern_t *grad;
  grad = cairo_pattern_create_linear(gradient_left_limit, 0.0, gradient_right_limit, 0.0);
  dt_cairo_perceptual_gradient(grad, 1.0);
  cairo_set_line_width(cr, 0.0);
  cairo_rectangle(cr, gradient_left_limit, gradient_top_limit, gradient_width, line_height);
  cairo_set_source(cr, grad);
  cairo_fill(cr);

  // draw the specular highlights icon (little sun at the right)
  cairo_save(cr);
  set_color(cr, darktable.bauhaus->graph_fg);
  float s = line_height / 2.0;
  cairo_translate(cr, gradient_right_limit + (graph_width - gradient_right_limit) / 2.0, gradient_top_limit + (s / 2.0));
  cairo_scale(cr, s, s);

  cairo_set_line_width(cr, .05);
  cairo_arc(cr, 0.5, 0.5, 0.5, 0., 2.0f * M_PI);
  cairo_fill(cr);

  const double radius = 1.25;
  const double perimeter = 2.0f * M_PI * radius;
  const double sector = perimeter / 12.0; // 12 sun rays -> 12 perimeter sectors
  double dashes[2] = {  sector * 0.15,   // length of ON dashes
                        sector * 0.85 }; // length of OFF dashes
  cairo_set_line_width(cr, .5);
  cairo_set_dash(cr, &dashes[0], 2, 0.0);
  cairo_arc(cr, 0.5, 0.5, radius, 0., 2.0 * 11.5/12.0 * M_PI); // don't draw the first ray twice
  cairo_stroke(cr);
  cairo_restore(cr);

  // draw the noise icon (matrice of random values at the left)
  cairo_save(cr);
  cairo_translate(cr, gradient_left_limit / 2.0 - s, gradient_top_limit - s / 4.0);
  cairo_scale(cr, 2.0 * s, 2.0 * s);
  // todo
  cairo_restore(cr);

  /** y axis **/
  // Draw the perceptually even gradient
  grad = cairo_pattern_create_linear(0.0, graph_height, 0.0, 0.0);
  dt_cairo_perceptual_gradient(grad, 1.0);
  cairo_set_line_width(cr, 0.0);
  cairo_rectangle(cr, -line_height - 2 * INNER_PADDING, 0.0, line_height, graph_height);
  cairo_set_source(cr, grad);
  cairo_fill(cr);

  // clean and exit
  cairo_pattern_destroy(grad);
  pango_font_description_free(desc);
  g_object_unref(layout);
  cairo_destroy(cr);
  cairo_set_source_surface(crf, cst, 0, 0);
  cairo_paint(crf);
  cairo_surface_destroy(cst);
  return TRUE;
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

  g->full_preview_buf = NULL;
  g->full_preview_buf_width = 0;
  g->full_preview_buf_height = 0;

  g->thumb_preview_buf = NULL;
  g->thumb_preview_buf_width = 0;
  g->thumb_preview_buf_height = 0;

  g->pipe_order = 0;
  dt_pthread_mutex_unlock(&g->lock);

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);


  // Init the GUI LUT to avoid weird plots before thumbnail is processed
  float *const LUT = g->gui_lut;
  int *const histogram = g->histogram;

#ifdef _OPENMP
#pragma omp parallel for simd schedule(static) default(none) shared(LUT, histogram) aligned(LUT, histogram:64)
#endif
  for(int k = 0; k < UI_SAMPLES; k++)
  {
    LUT[k] = 0.5f;
    histogram[k] = 0;
  }


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
  dt_bauhaus_widget_set_label(g->noise, NULL, _("-14 EV : noise"));
  gtk_box_pack_start(GTK_BOX(page1), g->noise, FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(g->noise), "value-changed", G_CALLBACK(noise_callback), self);

  g->ultra_deep_blacks = dt_bauhaus_slider_new_with_range(self, bottom, top, 0.1, 0.0, 2);
  dt_bauhaus_slider_set_format(g->ultra_deep_blacks, "%+.2f EV");
  dt_bauhaus_widget_set_label(g->ultra_deep_blacks, NULL, _("-12 EV : HDR deep blacks"));
  gtk_box_pack_start(GTK_BOX(page1), g->ultra_deep_blacks, FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(g->ultra_deep_blacks), "value-changed", G_CALLBACK(ultra_deep_blacks_callback), self);

  g->deep_blacks = dt_bauhaus_slider_new_with_range(self, bottom, top, 0.1, 0.0, 2);
  dt_bauhaus_slider_set_format(g->deep_blacks, "%+.2f EV");
  dt_bauhaus_widget_set_label(g->deep_blacks, NULL, _("-10 EV : HDR blacks"));
  gtk_box_pack_start(GTK_BOX(page1), g->deep_blacks, FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(g->deep_blacks), "value-changed", G_CALLBACK(deep_blacks_callback), self);

  g->blacks = dt_bauhaus_slider_new_with_range(self, bottom, top, 0.1, 0.0, 2);
  dt_bauhaus_slider_set_format(g->blacks, "%+.2f EV");
  dt_bauhaus_widget_set_label(g->blacks, NULL, _("-08 EV : blacks"));
  gtk_box_pack_start(GTK_BOX(page1), g->blacks, FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(g->blacks), "value-changed", G_CALLBACK(blacks_callback), self);

  g->shadows = dt_bauhaus_slider_new_with_range(self, bottom, top, 0.1, 0.0, 2);
  dt_bauhaus_slider_set_format(g->shadows, "%+.2f EV");
  dt_bauhaus_widget_set_label(g->shadows, NULL, _("-06 EV : shadows"));
  gtk_box_pack_start(GTK_BOX(page1), g->shadows, FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(g->shadows), "value-changed", G_CALLBACK(shadows_callback), self);

  g->midtones = dt_bauhaus_slider_new_with_range(self, bottom, top, 0.1, 0.0, 2);
  dt_bauhaus_slider_set_format(g->midtones, "%+.2f EV");
  dt_bauhaus_widget_set_label(g->midtones, NULL, _("-04 EV : midtones"));
  gtk_box_pack_start(GTK_BOX(page1), g->midtones, FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(g->midtones), "value-changed", G_CALLBACK(midtones_callback), self);

  g->highlights = dt_bauhaus_slider_new_with_range(self, bottom, top, 0.1, 0.0, 2);
  dt_bauhaus_slider_set_format(g->highlights, "%+.2f EV");
  dt_bauhaus_widget_set_label(g->highlights, NULL, _("-02 EV : highlights"));
  gtk_box_pack_start(GTK_BOX(page1), g->highlights, FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(g->highlights), "value-changed", G_CALLBACK(highlights_callback), self);

  g->whites = dt_bauhaus_slider_new_with_range(self, bottom, top, 0.1, 0.0, 2);
  dt_bauhaus_slider_set_format(g->whites, "%+.2f EV");
  dt_bauhaus_widget_set_label(g->whites, NULL, _("-00 EV : whites"));
  gtk_box_pack_start(GTK_BOX(page1), g->whites, FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(g->whites), "value-changed", G_CALLBACK(whites_callback), self);

  g->speculars = dt_bauhaus_slider_new_with_range(self, bottom, top, 0.1, 0.0, 2);
  dt_bauhaus_slider_set_format(g->speculars, "%+.2f EV");
  dt_bauhaus_widget_set_label(g->speculars, NULL, _("+02 EV : speculars"));
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

  g->smoothing = dt_bauhaus_slider_new_with_range(self, 1.0, 3, 0.1, 2.0, 2);
  dt_bauhaus_slider_enable_soft_boundaries(g->smoothing, 0.01, 16);
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

  g->iterations = dt_bauhaus_slider_new_with_range(self, 1, 5, 1, 2, 0);
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

  g->quantization = dt_bauhaus_slider_new_with_range(self, 0.00, 2., 0.1, 0.0, 2);
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

  g->show_luminance_mask
      = dtgtk_togglebutton_new(dtgtk_cairo_paint_showmask, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
  g_object_set(G_OBJECT(g->show_luminance_mask), "tooltip-text", _("display luminance mask"), (char *)NULL);
  g_signal_connect(G_OBJECT(g->show_luminance_mask), "toggled", G_CALLBACK(show_luminance_mask_callback), self);
  gtk_box_pack_end(GTK_BOX(page3), g->show_luminance_mask, FALSE, FALSE, 0);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->show_luminance_mask), FALSE);
  g->mask_display = FALSE;

  show_guiding_controls(self);
}


void gui_cleanup(struct dt_iop_module_t *self)
{
  dt_iop_toneequalizer_gui_data_t *g = (dt_iop_toneequalizer_gui_data_t *)self->gui_data;
  self->request_color_pick = DT_REQUEST_COLORPICK_OFF;
  dt_pthread_mutex_destroy(&g->lock);
  free(self->gui_data);
  self->gui_data = NULL;
}


// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
