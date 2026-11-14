/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

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

/*
DOCUMENTATION
This module implements a scene-referred local contrast enhancement algorithm,
designed to enhance local details while preserving edges and avoiding artifacts.

It builds upon the original proof-of-concept algorithm proposed by WileCoyote:
https://discuss.pixls.us/t/experiments-with-a-scene-referred-local-contrast-module-proof-of-concept/55402

And then further explored and optimized by Christian Bouhon
https://discuss.pixls.us/t/contrast-management-rgb-a-new-scene-referred-approach-poc/56004

Current status as implemented by Jandren:
- Local contrast based on the gaussian guided filter surface blur in log space.
- The local contrast is extracted and manipulated in multiple pyramid levels.
  The filtering is done top to bottom and each new level is filtering the
  filtered result of the one above, extracted details are the log difference between the two levels.
- The user controls the size of the pyramid's base and the amount in three
  interpolated segments along the pyramid, fine to coarse (top to bottom).
- The final very smooth filtered image at the base level is used as a base
  for highlights and shadows sliders which alter the contrast above and below middle grey.

TERMINOLOGY:
- detail level: absolute scale, log2(image size / detail size), 0 == full image size.
  The detail size is in pixels, the filter gets a gaussian sigma of 0.375 x detail size.
- pyramid level: index of a filter instance, 0 == the base detail level slider, +1 == half size.
  detail level = base detail level + pyramid level
- max pyramid level: where the "fine" band reaches 100%.
- max used pyramid level: last level filtered at the current preview scale.
*/

/* Work around a compiler bug, see
   https://github.com/darktable-org/darktable/issues/21801
*/
#if __GNUC__ == 16
#pragma GCC optimize ("no-ipa-cp")
#endif

#include "common/extra_optimizations.h"

#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "bauhaus/bauhaus.h"
#include "common/darktable.h"
#include "common/fast_gaussian_guided_filter.h"
#include "common/opencl.h"
#include "common/gaussian.h"
#include "develop/tiling.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/blend.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "develop/imageop_gui.h"
#include "gui/accelerators.h"
#include "gui/draw.h"
#include "dtgtk/drawingarea.h"
#include "dtgtk/paint.h"
#include "dtgtk/togglebutton.h"
#include "dtgtk/expander.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "iop/iop_api.h"
#include "common/iop_group.h"

#ifdef _OPENMP
#include <omp.h>
#endif

DT_MODULE_INTROSPECTION(3, dt_iop_contrastntexture_params_t)

// Standard scene-referred midtone pivot.
#define MIDDLE_GREY 0.1845f
// Detail sizes in pixels for the low pass filter, a size of 1 px is the nyquist limit "half period".
// Detail size 1 px barely extracts any feature, 2 px is therefore where the max_pyramid_level is placed.
#define NYQUIST_SIZE 1.0f
#define MINIMUM_SIZE (2.0f * NYQUIST_SIZE)
// Gaussian sigma that gives a -6 dB response at the Nyquist frequency
#define SIZE_TO_SIGMA 0.375f

typedef struct dt_iop_contrastntexture_params_t
{
  float gain_coarse;     // $MIN: -1.0 $MAX: 5.0 $DEFAULT: 0.0 $DESCRIPTION: "coarse details"
  float gain_medium;     // $MIN: -1.0 $MAX: 5.0 $DEFAULT: 0.0 $DESCRIPTION: "medium details"
  float gain_fine;       // $MIN: -1.0 $MAX: 5.0 $DEFAULT: 0.0 $DESCRIPTION: "fine details"
  float base_detail_level; // $MIN: 1.0 $MAX: 15.0 $DEFAULT: 5.0 $DESCRIPTION: "base detail level"
  float halo_control;    // $MIN: -10.0 $MAX: 10.0 $DEFAULT: 0.0 $DESCRIPTION: "halo control"
  float noise_bias;      // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.001 $DESCRIPTION: "noise bias"
  float gain_shadows;    // $MIN: -5.0 $MAX: 5.0 $DEFAULT: 0.0 $DESCRIPTION: "shadows"
  float gain_highlights; // $MIN: -5.0 $MAX: 5.0 $DEFAULT: 0.0 $DESCRIPTION: "highlights"
} dt_iop_contrastntexture_params_t;

typedef enum dt_iop_contrastntexture_details_display_t
{
  DT_LC_MASK_OFF = -1,
  DT_LC_MASK_COARSE = 0,
  DT_LC_MASK_MEDIUM = 1,
  DT_LC_MASK_FINE = 2,
  DT_LC_MASK_LAST = 3
} dt_iop_contrastntexture_details_display_t;

#define MAX_ITERATIONS 20

typedef struct dt_iop_contrastntexture_data_t
{
  float gain_details[DT_LC_MASK_LAST];
  // indexed by pyramid level
  float size_details[MAX_ITERATIONS];
  float scale_details[MAX_ITERATIONS];
  float noise_bias[MAX_ITERATIONS];
  float max_pyramid_level;
  int   max_used_pyramid_level;
  float feathering;
  float slope_shadows;
  float slope_highlights;
  float midtones_pivot;
  float midtones_width;
  float midtones_polynomial[3];
} dt_iop_contrastntexture_data_t;

typedef struct dt_iop_contrastntexture_global_data_t
{
  int kernel_luminance;
  int kernel_details;
  int kernel_apply;
  int kernel_display_details;
  int kernel_display_lowpass;
  int kernel_guided_filter_pack;
} dt_iop_contrastntexture_global_data_t;

typedef struct dt_iop_contrastntexture_gui_data_t
{
  // Preview widgets and data for the GUI
  // Read-only graph of the total gain per detail size
  dt_iop_contrastntexture_details_display_t details_display;
  GtkWidget *gain_graph;
  dt_gui_collapsible_section_t graph_section;

  // GTK widgets adjustments
  GtkWidget *gain_details[DT_LC_MASK_LAST]; // coarse, medium, and fine
  GtkWidget *gain_shadows;
  GtkWidget *gain_highlights;

  // GTK widgets filter settings
  GtkWidget *base_detail_level;
  GtkWidget *halo_control;
  GtkWidget *noise_bias;
} dt_iop_contrastntexture_gui_data_t;


const char *name()
{
  return _("contrast & texture");
}

const char *aliases()
{
  return _("local contrast|texture|clarity|detail enhancement|highlights and shadows");
}

const char **description(dt_iop_module_t *self)
{
  return dt_iop_set_description
    (self, _("enhance local contrast and handle highlights and shadows"),
     _("creative"),
     _("linear, RGB, scene-referred"),
     _("linear, RGB"),
     _("linear, RGB, scene-referred"));
}

int default_group()
{
  return IOP_GROUP_EFFECTS;
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
  if(old_version == 1)
  {
    typedef struct dt_iop_contrastntexture_params_v1_t
    {
      float gain_local_contrast;
      float detail_level;
      float edge_protection;
      int filter_iterations;
      float noise_bias;
    } dt_iop_contrastntexture_params_v1_t;

    dt_iop_contrastntexture_params_v1_t *o = (dt_iop_contrastntexture_params_v1_t *)old_params;
    dt_iop_contrastntexture_params_t *n = malloc(sizeof(dt_iop_contrastntexture_params_t));
    n->gain_coarse = o->gain_local_contrast - 1.0f;
    n->gain_medium = o->gain_local_contrast - 1.0f;
    n->gain_fine = o->gain_local_contrast - 1.0f;
    n->base_detail_level = o->detail_level;
    n->halo_control = o->edge_protection;
    n->noise_bias = o->noise_bias;
    n->gain_highlights = 0.0f;
    n->gain_shadows = 0.0f;

    *new_params = n;
    *new_params_size = sizeof(dt_iop_contrastntexture_params_t);
    *new_version = 2;
    return 0;
  }
  if(old_version == 2)
  {
    // v3 changed the filter sigma from 0.5 x detail size to SIZE_TO_SIGMA x detail size.
    // Lowering the base detail level by log2(0.5 / SIZE_TO_SIGMA) keeps the sigma of every filter
    // instance as it was. Exact except where the hard minimum of the slider clamps it.
    dt_iop_contrastntexture_params_t *n = calloc(1, sizeof(dt_iop_contrastntexture_params_t));
    memcpy(n, old_params, sizeof(dt_iop_contrastntexture_params_t));
    n->base_detail_level = fmaxf(n->base_detail_level - log2f(0.5f / SIZE_TO_SIGMA), 1.0f);

    *new_params = n;
    *new_params_size = sizeof(dt_iop_contrastntexture_params_t);
    *new_version = 3;
    return 0;
  }
  return 1;
}

// Compute the greyscale log luminance from linear rgb input
__DT_CLONE_TARGETS__
static inline void compute_luminance(const float *const restrict linear_rgb,
                                      float *const restrict log_luminance,
                                      const dt_iop_roi_t *const roi_in,
                                      const float noise_bias)
{
  const size_t width = (size_t)roi_in->width;
  const size_t height = (size_t)roi_in->height;
  const size_t npixels = width * height;

  const float normalize_luminance = 1.0f / sqrtf(3.0f);
  DT_OMP_FOR()
  for(size_t k = 0; k < npixels; k++)
  {
    float rgb_square_sum = 0.0f;
    for_three_channels(c)
    {
      const float channel_value = linear_rgb[4 * k + c];
      rgb_square_sum += channel_value * channel_value;
    }
    const float rgb_norm = normalize_luminance * sqrtf(rgb_square_sum) + noise_bias;
    log_luminance[k] = log2f(fmaxf(rgb_norm, NORM_MIN));
  }
}

// Compute smoothed luminance mask using edge-aware filters
__DT_CLONE_TARGETS__
static inline void compute_mask(float *const restrict log_smoothed_luminance,
                                const dt_iop_roi_t *const roi_in,
                                const dt_iop_contrastntexture_data_t *const d,
                                const int pyramid_level)
{
  const size_t width = (size_t)roi_in->width;
  const size_t height = (size_t)roi_in->height;
  const float sigma = SIZE_TO_SIGMA * d->size_details[pyramid_level];

  fast_gaussian_guided_filter(log_smoothed_luminance, width, height,
                              sigma, d->feathering);
}

// Extract logarithmic high pass detail in log space (EV):
// How much brighter/darker is this pixel compared to the smooth version.
// The caller supplies the smoothed luminance in linear space too, as it needs it as well.
__DT_CLONE_TARGETS__
static inline float extract_details(const float log_luminance_pixel,
                                    const float log_luminance_smoothed,
                                    const float smoothed_luminance,
                                    const float noise_power)
{
  const float combined_power = smoothed_luminance * smoothed_luminance;
  const float wiener_gain = fmaxf(combined_power - noise_power, 0.0f) / fmaxf(combined_power, NORM_MIN);
  return wiener_gain * fmaxf(fminf(log_luminance_pixel - log_luminance_smoothed, 5.0f), -5.0f);
}

// Squared cosine/sine crossfade weight of a given band (coarse/medium/fine) at a pyramid level
static inline float band_weight(const dt_iop_contrastntexture_details_display_t band,
                                const float pyramid_level,
                                const float max_pyramid_level)
{
  // with no room for a crossfade (max_pyramid_level <= 0) every level is at or beyond
  // the fine end, so avoid the division and give the fine band full weight
  float t = max_pyramid_level > 0.0f ? pyramid_level / max_pyramid_level - 0.5f : 0.5f;
  t = fmaxf(fminf(t, 0.5f), -0.5f);

  float weight = 0.0f;
  switch(band)
  {
    case DT_LC_MASK_COARSE: weight = t < 0.0f ? sinf(M_PI_F * t) : 0.0f; break;
    case DT_LC_MASK_MEDIUM: weight = cosf(M_PI_F * t); break;
    case DT_LC_MASK_FINE: weight = t >= 0.0f ? sinf(M_PI_F * t) : 0.0f; break;
    default: weight = 0.0f; break;
  }
  return weight * weight;
}

// The pyramid level where the detail size == MINIMUM_SIZE for a full resolution image, i.e. where the
// "fine" band reaches 100%. It is independent of the preview scale, so the band weights match
// between the preview and the full pipe.
static inline float max_pyramid_level_for(const float max_image_size,
                                          const float base_detail_level)
{
  return log2f(max_image_size) - log2f(MINIMUM_SIZE) - base_detail_level;
}

// Apply shadow and highlight enhancement
// Different slopes for shadows and highlights, with a smooth transition in the midtones
__DT_CLONE_TARGETS__
static inline float apply_shadows_highlights(const float log_luminance_lowpass,
                                             const dt_iop_contrastntexture_data_t *const d)
{
  const float slope_shadows = d->slope_shadows;
  const float slope_highlights = d->slope_highlights;
  const float midtones_width = d->midtones_width;
  const float a0 = d->midtones_polynomial[0];
  const float a1 = d->midtones_polynomial[1];
  const float a2 = d->midtones_polynomial[2];
  const float pivot_offset = d->midtones_pivot;

  float correction_ev = 0.0f;
  // Low pass correction for shadows and highlights
  const float normalized_ev = log_luminance_lowpass - pivot_offset;
  if(normalized_ev <= -midtones_width)
    correction_ev += slope_shadows * normalized_ev;
  else if(normalized_ev >= midtones_width)
    correction_ev += slope_highlights * normalized_ev;
  else
  {
    const float shifted_ev = normalized_ev + midtones_width; // Shift to [0, 2*midtones_width] for polynomial evaluation
    correction_ev += (a0 + a1 * shifted_ev + a2 * shifted_ev * shifted_ev);
  }
  return correction_ev - normalized_ev; // It is the difference from the identity line that matters for correction
}

/*
 Display the detail mask (difference between pixel and smoothed luminance)
 Output is a grayscale image normalized to [0, 1] where:
 - 0.5 = no local detail (pixel matches neighborhood)
 - < 0.5 = pixel darker than neighborhood
 - > 0.5 = pixel brighter than neighborhood
 */
__DT_CLONE_TARGETS__
static inline void display_local_mask(const float *const restrict corrections,
                                      float *const restrict out,
                                      const dt_iop_roi_t *const roi_in)
{
  const size_t npixels = roi_in->width * roi_in->height;

  DT_OMP_FOR()
  for(size_t k = 0; k < npixels; k++)
  {
    const float local_ev = corrections[k];

    // Detail in log space, mapped to [0, 1] for display
    // Detail range roughly [-2, +2] EV mapped to [0, 1]
    const float intensity = local_ev / sqrtf(local_ev * local_ev + 1.0f) * 0.5f + 0.5f; // Smooth mapping to [0, 1]

    // Set all RGB channels to the same intensity (grayscale)
    for_three_channels(c)
    {
      out[4 * k + c] = intensity;
    }
    // Full opacity
    out[4 * k + 3] = 1.0f;
  }
}

/*
 Display the final low pass filter result with a tint, i.e. the value fed into the
 shadows (blue) and highlights (yellow) control, mapped to linear [0, 1].
 */
__DT_CLONE_TARGETS__
static inline void display_lowpass_mask(const float *const restrict log_luminance_lowpass,
                                        float *const restrict out,
                                        const dt_iop_roi_t *const roi_in,
                                        const dt_iop_contrastntexture_data_t *const d)
{
  const size_t npixels = roi_in->width * roi_in->height;
  const float pivot_offset = d->noise_bias[0] + MIDDLE_GREY;

  const dt_aligned_pixel_t blue = {0.0f, 0.0f, 1.0f};
  const dt_aligned_pixel_t yellow = {1.0f, 1.0f, 0.0f};

  DT_OMP_FOR()
  for(size_t k = 0; k < npixels; k++)
  {
    // Low contrast sigmoid for the preview.
    const float linear_luminance = exp2f(log_luminance_lowpass[k]);
    const float display_intensity = linear_luminance / (pivot_offset + linear_luminance);
    float display_saturation = (display_intensity - 0.5f) * 2.0f;
    display_saturation *= display_saturation;

    for_three_channels(c)
    {
      if(display_intensity < 0.5f)
        out[4 * k + c] = display_intensity * ((1.0f - display_saturation) + display_saturation * blue[c]);
      else
        out[4 * k + c] = display_intensity * ((1.0f - display_saturation) + display_saturation * yellow[c]);
    }
    out[4 * k + 3] = 1.0f;
  }
}

void process(dt_iop_module_t *self,
             dt_dev_pixelpipe_iop_t *piece,
             const void *const restrict ivoid,
             void *const restrict ovoid,
             const dt_iop_roi_t *const roi_in,
             const dt_iop_roi_t *const roi_out)
{
  const dt_iop_contrastntexture_data_t *const d = piece->data;
  dt_iop_contrastntexture_gui_data_t *const g = self->gui_data;

  // Validate input format
  if(!dt_iop_have_required_input_format(4, self, piece->colors,
                                        ivoid, ovoid, roi_in, roi_out))
    return;

  const float *const restrict in = (float *const)ivoid;
  float *const restrict out = (float *const)ovoid;

  const size_t npixels = roi_in->width * roi_in->height;
  float *restrict log_luminance_highpass = dt_alloc_align_float(npixels);
  float *restrict log_luminance_lowpass = dt_alloc_align_float(npixels);
  float *restrict corrections = dt_alloc_align_float(npixels);
    if(!log_luminance_lowpass ||
      !log_luminance_highpass ||
     !corrections)
  {
    dt_control_log(_("contrast and texture failed to allocate memory, check your RAM settings"));
    dt_free_align(log_luminance_highpass);
    dt_free_align(log_luminance_lowpass);
    dt_free_align(corrections);
    return;
  }

  // The actual gain per pyramid level is computed here in process() instead of
  // in commit params as it is affected by the display mask option.
  float gain_per_pyramid_level[MAX_ITERATIONS] = { 0.0f };

  // Display output
  dt_iop_contrastntexture_details_display_t details_display = DT_LC_MASK_OFF;
  gboolean display_mask = FALSE;
  if(self->dev->gui_attached && g && dt_pipe_is_full(piece->pipe))
  {
    dt_iop_gui_enter_critical_section(self);
    details_display = g->details_display;
    display_mask = g->details_display != DT_LC_MASK_OFF
                   && dt_pipe_is_full(piece->pipe);
    dt_iop_gui_leave_critical_section(self);
  }

  if(display_mask)
  {
    piece->pipe->mask_display = DT_DEV_PIXELPIPE_DISPLAY_PASSTHRU;
    // Preview the displayed band alone, at full strength, across all pyramid levels
    // (the lowpass preview does not use band weights, the final log_luminance_lowpass suffices)
    if(details_display != DT_LC_MASK_LAST)
    {
      for(int pyramid_level = 0; pyramid_level <= d->max_used_pyramid_level; pyramid_level++)
      {
        gain_per_pyramid_level[pyramid_level]
          = band_weight(details_display, pyramid_level, d->max_pyramid_level);
      }
    }
  }
  else
  {
    // Smoothly interpolate the 3 sliders (coarse/medium/fine) across all pyramid
    // levels using squared cosine/sine crossfade weights, so neighboring levels blend
    // instead of jumping discretely between the 3 gains.
    for(int pyramid_level = 0; pyramid_level <= d->max_used_pyramid_level; pyramid_level++)
    {
      for(int band = 0; band < DT_LC_MASK_LAST; band++)
      {
        gain_per_pyramid_level[pyramid_level]
          += d->gain_details[band] * band_weight(band, pyramid_level, d->max_pyramid_level);
      }
    }
  }

  const float max_noise_bias = d->noise_bias[d->max_used_pyramid_level];
  compute_luminance(in, log_luminance_lowpass, roi_in, max_noise_bias);
  memset(corrections, 0, npixels * sizeof(float));

  for(int pyramid_level = d->max_used_pyramid_level; pyramid_level >= 0; pyramid_level--)
  {
    memcpy(log_luminance_highpass, log_luminance_lowpass, npixels * sizeof(float));
    compute_mask(log_luminance_lowpass, roi_in, d, pyramid_level);

    const float level_gain = gain_per_pyramid_level[pyramid_level];
    const float noise_power = d->noise_bias[pyramid_level] * d->noise_bias[pyramid_level];
    const float bias_diff = pyramid_level > 0
                            ? d->noise_bias[pyramid_level] - d->noise_bias[pyramid_level - 1]
                            : 0.0f;

    DT_OMP_FOR()
    for(size_t k = 0; k < npixels; k++)
    {
      const float smoothed_luminance = exp2f(log_luminance_lowpass[k]);

      // Details as the bandpass difference
      corrections[k] += level_gain * extract_details(log_luminance_highpass[k],
                                                     log_luminance_lowpass[k],
                                                     smoothed_luminance,
                                                     noise_power);

      // Reduce the noise bias to the next pyramid level
      if(pyramid_level > 0)
      {
        const float linear_luminance = smoothed_luminance - bias_diff;
        log_luminance_lowpass[k] = log2f(fmaxf(linear_luminance, NORM_MIN));
      }
    }
  }

  if(display_mask)
  {
    if(details_display == DT_LC_MASK_LAST)
      display_lowpass_mask(log_luminance_lowpass, out, roi_in, d);
    else
      display_local_mask(corrections, out, roi_in);
  }
  else
  {
    DT_OMP_FOR()
    for(size_t k = 0; k < npixels; k++)
    {
      // Low pass correction for shadows and highlights
      const float lowpass_correction = apply_shadows_highlights(log_luminance_lowpass[k], d);

      // Apply correction in linear space
      const float multiplier = exp2f(corrections[k] + lowpass_correction);
      for_three_channels(c)
        out[4 * k + c] = in[4 * k + c] * multiplier;
      out[4 * k + 3] = in[4 * k + 3];
    }
  }

  dt_free_align(log_luminance_highpass);
  dt_free_align(log_luminance_lowpass);
  dt_free_align(corrections);
}

void modify_roi_in(dt_iop_module_t *self,
                   dt_dev_pixelpipe_iop_t *piece,
                   const dt_iop_roi_t *roi_out,
                   dt_iop_roi_t *roi_in)
{
  dt_iop_contrastntexture_data_t *const d = piece->data;

  // Detail size in pixels of each pyramid level at the current scale
  const float max_size = (float)((piece->iwidth > piece->iheight) ? piece->iwidth : piece->iheight);
  for(int pyramid_level = 0; pyramid_level < MAX_ITERATIONS; pyramid_level++)
  {
    const float base_diameter = max_size * roi_in->scale;
    d->size_details[pyramid_level] = base_diameter * d->scale_details[pyramid_level];
  }

  // The last level, i.e. the top of the pyramid that still has a usable size, level 0 is always processed.
  d->max_used_pyramid_level = 0;
  for(int pyramid_level = 1; pyramid_level < MAX_ITERATIONS; pyramid_level++)
  {
    if(d->size_details[pyramid_level] < NYQUIST_SIZE) break;
    d->max_used_pyramid_level = pyramid_level;
  }
}

void commit_params(dt_iop_module_t *self,
                   dt_iop_params_t *p1,
                   dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  const dt_iop_contrastntexture_params_t *p = (dt_iop_contrastntexture_params_t *)p1;
  dt_iop_contrastntexture_data_t *d = piece->data;

  d->gain_details[DT_LC_MASK_COARSE] = p->gain_coarse;
  d->gain_details[DT_LC_MASK_MEDIUM] = p->gain_medium;
  d->gain_details[DT_LC_MASK_FINE] = p->gain_fine;

  // Log slope of shadows and highlights, .i.e. the power the modify them with.
  d->slope_shadows = powf(2.0f, -p->gain_shadows);
  d->slope_highlights = powf(2.0f, p->gain_highlights);

  // Quadratic polynomial for the midtones pivot transition.
  // First order smooth with aligned slopes at [0, 2*width]
  d->midtones_width = 0.3f;
  d->midtones_polynomial[0] = -d->slope_shadows * d->midtones_width;
  d->midtones_polynomial[1] = d->slope_shadows;
  d->midtones_polynomial[2] = (d->slope_highlights - d->slope_shadows) / (4.0f * d->midtones_width);

  const float max_piece_size = (float)((piece->iwidth > piece->iheight) ? piece->iwidth : piece->iheight);
  const float max_image_size = max_piece_size * piece->iscale;
  d->max_pyramid_level = max_pyramid_level_for(max_image_size, p->base_detail_level);

  // UI contrast scale is inverse logarithmic with 0 as 100% of image width.
  // Convert it to a linear scale for processing. Scales are separated by powers of 2 for each step in the UI.
  // The noise bias is reduced for each pyramid level.
  for(int pyramid_level = 0; pyramid_level < MAX_ITERATIONS; pyramid_level++)
  {
    d->scale_details[pyramid_level] = powf(2.0f, -p->base_detail_level - (float)pyramid_level);
    d->noise_bias[pyramid_level]
      = p->noise_bias * powf(2.0f, fminf((float)pyramid_level - d->max_pyramid_level, 0.0f));
  }
  // Pre-compute the midtones pivot based on the noise bias at the base pyramid level and the middle grey reference.
  d->midtones_pivot = log2f(d->noise_bias[0] + MIDDLE_GREY);

  // UI feathering is inverted (higher = stricter halo control).
  const float default_feathering = 0.2f;  // Base value based on Christian's experiments for a good balance of halo control and contrast boost at default settings.
  d->feathering = default_feathering * powf(2.0f, -p->halo_control);
}

// The default implementation sizes piece->data by params_size, which is only correct while the data struct
// is no larger than the params struct. Ours is not: dt_iop_contrast_data_t carries contrast_scale, feathering and
// radius_local in place of the params' detail_level and edge_protection, and one field more besides, so noise_bias
// would land past the end of the allocation, written by commit_params and then read back by process().
void init_pipe(dt_iop_module_t *self,
               dt_dev_pixelpipe_t *pipe,
               dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = dt_calloc1_align_type(dt_iop_contrastntexture_data_t);
}

void cleanup_pipe(dt_iop_module_t *self,
                  dt_dev_pixelpipe_t *pipe,
                  dt_dev_pixelpipe_iop_t *piece)
{
  dt_free_align(piece->data);
  piece->data = NULL;
}

void tiling_callback(dt_iop_module_t *self,
                     dt_dev_pixelpipe_iop_t *piece,
                     const dt_iop_roi_t *roi_in,
                     const dt_iop_roi_t *roi_out,
                     dt_develop_tiling_t *tiling)
{
  // in and out plus the three grey full size buffers of process(). On the CPU,
  // fast_eigf_surface_blur() adds a full size mask and 4 channel averages. On
  // the GPU, the two grey buffers of process_cl() add the two channel moments
  // and the two temporary two channel buffers of their gaussian
  tiling->factor = 2.0f + 3.0f / 4.0f + 1.0f / 4.0f + 1.0f;
  tiling->factor_cl = 2.0f + 2.0f / 4.0f + 2.0f / 4.0f + 4.0f / 4.0f;
  tiling->maxbuf = 1.0f;
  tiling->maxbuf_cl = 1.0f;
  tiling->overhead = 0;
  tiling->overlap = 0;
  tiling->align = 1;
}

#ifdef HAVE_OPENCL
// fast_gaussian_guided_filter() of common/fast_gaussian_guided_filter.h, with
// the value and its square blurred as the two channels of one gaussian. The
// details kernel finishes the filter, so this leaves the blurred moments in
// dev_moments and dev_image untouched
static cl_int _gaussian_guided_filter_moments_cl(const int devid,
                                                 const dt_iop_contrastntexture_global_data_t *const gd,
                                                 cl_mem dev_image,
                                                 cl_mem dev_moments,
                                                 const int width,
                                                 const int height,
                                                 const float sigma)
{
  cl_int err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_guided_filter_pack, width, height,
                 CLARG(dev_image), CLARG(dev_moments), CLARG(width), CLARG(height));
  if(err != CL_SUCCESS) return err;

  // the CPU bounds the gaussian by the range of the values. A blur stays
  // within that range up to rounding, so wide bounds save a min/max
  // reduction here. The square keeps its lower bound of 0
  const float range = 1.0e9f;
  const float max[2] = { range, range };
  const float min[2] = { -range, 0.0f };
  dt_gaussian_cl_t *g = dt_gaussian_init_cl(devid, width, height, 2, max, min, sigma, 0);
  if(!g) return CL_MEM_OBJECT_ALLOCATION_FAILURE;
  err = dt_gaussian_blur_cl_buffer(g, dev_moments, dev_moments);
  dt_gaussian_free_cl(g);
  return err;
}

int process_cl(dt_iop_module_t *self,
               dt_dev_pixelpipe_iop_t *piece,
               cl_mem dev_in,
               cl_mem dev_out,
               const dt_iop_roi_t *const roi_in,
               const dt_iop_roi_t *const roi_out)
{
  const dt_iop_contrastntexture_data_t *const d = piece->data;
  const dt_iop_contrastntexture_global_data_t *const gd = self->global_data;
  dt_iop_contrastntexture_gui_data_t *const g = self->gui_data;

  if(piece->colors != 4) return DT_OPENCL_PROCESS_CL;

  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;
  const size_t bsize = (size_t)width * height * sizeof(float);

  // the gains per pyramid level and the mask display, as in process()
  float gain_per_pyramid_level[MAX_ITERATIONS] = { 0.0f };
  dt_iop_contrastntexture_details_display_t details_display = DT_LC_MASK_OFF;
  gboolean display_mask = FALSE;
  if(self->dev->gui_attached && g && dt_pipe_is_full(piece->pipe))
  {
    dt_iop_gui_enter_critical_section(self);
    details_display = g->details_display;
    display_mask = g->details_display != DT_LC_MASK_OFF;
    dt_iop_gui_leave_critical_section(self);
  }

  if(display_mask)
  {
    piece->pipe->mask_display = DT_DEV_PIXELPIPE_DISPLAY_PASSTHRU;
    if(details_display != DT_LC_MASK_LAST)
      for(int pyramid_level = 0; pyramid_level <= d->max_used_pyramid_level; pyramid_level++)
        gain_per_pyramid_level[pyramid_level]
          = band_weight(details_display, pyramid_level, d->max_pyramid_level);
  }
  else
  {
    for(int pyramid_level = 0; pyramid_level <= d->max_used_pyramid_level; pyramid_level++)
      for(int band = 0; band < DT_LC_MASK_LAST; band++)
        gain_per_pyramid_level[pyramid_level]
          += d->gain_details[band] * band_weight(band, pyramid_level, d->max_pyramid_level);
  }

  cl_int err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
  cl_mem dev_lowpass = dt_opencl_alloc_device_buffer(devid, bsize);
  cl_mem dev_moments = dt_opencl_alloc_device_buffer(devid, 2 * bsize);
  cl_mem dev_corrections = dt_opencl_alloc_device_buffer(devid, bsize);
  if(!dev_lowpass || !dev_moments || !dev_corrections) goto error;

  const float max_noise_bias = d->noise_bias[d->max_used_pyramid_level];
  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_luminance, width, height,
          CLARG(dev_in), CLARG(dev_lowpass), CLARG(width), CLARG(height), CLARG(max_noise_bias));
  if(err != CL_SUCCESS) goto error;

  for(int pyramid_level = d->max_used_pyramid_level; pyramid_level >= 0; pyramid_level--)
  {
    const float sigma = SIZE_TO_SIGMA * d->size_details[pyramid_level];
    err = _gaussian_guided_filter_moments_cl(devid, gd, dev_lowpass, dev_moments, width, height, sigma);
    if(err != CL_SUCCESS) goto error;

    const float gain = gain_per_pyramid_level[pyramid_level];
    const float noise_bias = d->noise_bias[pyramid_level];
    const float next_bias_step = pyramid_level > 0
                                 ? d->noise_bias[pyramid_level] - d->noise_bias[pyramid_level - 1]
                                 : 0.0f;
    // the first level starts the sum, so the corrections need no clearing
    const int accumulate = pyramid_level < d->max_used_pyramid_level;
    const int last_level = pyramid_level == 0;
    err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_details, width, height,
            CLARG(dev_lowpass), CLARG(dev_moments), CLARG(dev_corrections),
            CLARG(width), CLARG(height), CLARG(d->feathering), CLARG(gain), CLARG(noise_bias),
            CLARG(next_bias_step), CLARG(accumulate), CLARG(last_level));
    if(err != CL_SUCCESS) goto error;
  }

  if(display_mask && details_display == DT_LC_MASK_LAST)
  {
    const float pivot_offset = d->noise_bias[0] + MIDDLE_GREY;
    err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_display_lowpass, width, height,
            CLARG(dev_out), CLARG(dev_lowpass), CLARG(width), CLARG(height), CLARG(pivot_offset));
  }
  else if(display_mask)
  {
    err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_display_details, width, height,
            CLARG(dev_out), CLARG(dev_corrections), CLARG(width), CLARG(height));
  }
  else
  {
    const float polynomial[4] = { d->midtones_polynomial[0], d->midtones_polynomial[1],
                                  d->midtones_polynomial[2], 0.0f };
    err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_apply, width, height,
            CLARG(dev_in), CLARG(dev_out), CLARG(dev_lowpass), CLARG(dev_corrections),
            CLARG(width), CLARG(height), CLARG(d->slope_shadows), CLARG(d->slope_highlights),
            CLARG(d->midtones_width), CLFLARRAY(4, polynomial), CLARG(d->midtones_pivot));
  }

error:
  dt_opencl_release_mem_object(dev_corrections);
  dt_opencl_release_mem_object(dev_moments);
  dt_opencl_release_mem_object(dev_lowpass);
  return err;
}
#endif

void init_global(dt_iop_module_so_t *self)
{
  const int program = 46; // contrastntexture.cl, from programs.conf
  dt_iop_contrastntexture_global_data_t *gd = malloc(sizeof(dt_iop_contrastntexture_global_data_t));
  self->data = gd;
  gd->kernel_luminance = dt_opencl_create_kernel(program, "contrastntexture_luminance");
  gd->kernel_details = dt_opencl_create_kernel(program, "contrastntexture_details");
  gd->kernel_apply = dt_opencl_create_kernel(program, "contrastntexture_apply");
  gd->kernel_display_details = dt_opencl_create_kernel(program, "contrastntexture_display_details");
  gd->kernel_display_lowpass = dt_opencl_create_kernel(program, "contrastntexture_display_lowpass");
  gd->kernel_guided_filter_pack = dt_opencl_create_kernel(program, "contrastntexture_guided_filter_pack");
}

void cleanup_global(dt_iop_module_so_t *self)
{
  const dt_iop_contrastntexture_global_data_t *gd = self->data;
  dt_opencl_free_kernel(gd->kernel_luminance);
  dt_opencl_free_kernel(gd->kernel_details);
  dt_opencl_free_kernel(gd->kernel_apply);
  dt_opencl_free_kernel(gd->kernel_display_details);
  dt_opencl_free_kernel(gd->kernel_display_lowpass);
  dt_opencl_free_kernel(gd->kernel_guided_filter_pack);
  free(self->data);
  self->data = NULL;
}

static void show_details_callback(GtkWidget *togglebutton, dt_iop_module_t *self)
{
  DT_GUARD_GUI_UPDATE();
  // early return if blend module is already displaying a mask
  if(self->request_mask_display)
  {
    dt_control_log(_("cannot display masks when the blending mask is displayed"));
    dt_bauhaus_widget_set_quad_active(GTK_WIDGET(togglebutton), FALSE);
    return;
  }

  dt_iop_request_focus(self);
  // Activate the module if it wasn't
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->off), TRUE);

  dt_iop_contrastntexture_gui_data_t *g = self->gui_data;

  const gboolean toggle_is_active = dt_bauhaus_widget_get_quad_active(GTK_WIDGET(togglebutton));

  dt_iop_gui_enter_critical_section(self);
  g->details_display = DT_LC_MASK_OFF;
  if(toggle_is_active)
  {
    if(togglebutton == g->base_detail_level)
    {
      g->details_display = DT_LC_MASK_LAST;
    }
    else
    {
      for(int i = 0; i < DT_LC_MASK_LAST; i++)
      {
        if(togglebutton == g->gain_details[i])
        {
          g->details_display = i;
          break;
        }
      }
    }
  }
  dt_iop_gui_leave_critical_section(self);

  for(int i = 0; i < DT_LC_MASK_LAST; i++)
  {
    dt_bauhaus_widget_set_quad_active(GTK_WIDGET(g->gain_details[i]), g->details_display == i);
  }
  dt_bauhaus_widget_set_quad_active(GTK_WIDGET(g->base_detail_level), g->details_display == DT_LC_MASK_LAST);
  gtk_widget_queue_draw(g->gain_graph);
  dt_iop_refresh_center(self);
}

void gui_focus(dt_iop_module_t *self, const gboolean in)
{
  DT_GUARD_GUI_UPDATE();
  if(!in)
  {
    dt_iop_contrastntexture_gui_data_t *g = self->gui_data;
    const gboolean was_mask = g->details_display != DT_LC_MASK_OFF;
    g->details_display = DT_LC_MASK_OFF;
    for(int i = 0; i < DT_LC_MASK_LAST; i++)
      dt_bauhaus_widget_set_quad_active(GTK_WIDGET(g->gain_details[i]), FALSE);
    dt_bauhaus_widget_set_quad_active(GTK_WIDGET(g->base_detail_level), FALSE);
    gtk_widget_queue_draw(g->gain_graph);
    if(was_mask)
      dt_iop_refresh_center(self);
  }
}

static void draw_label(cairo_t *cr,
                       PangoLayout *layout,
                       const char *text,
                       const double x,
                       const double y,
                       const double ax,
                       const double ay)
{
  PangoRectangle ink;
  pango_layout_set_text(layout, text, -1);
  pango_layout_get_pixel_extents(layout, &ink, NULL);
  cairo_move_to(cr, x - ax * (ink.x + ink.width), y - ay * (ink.y + ink.height));
  pango_cairo_show_layout(cr, layout);
}

// the image dimensions are unknown until the image is loaded
static float gui_max_image_size(const dt_iop_module_t *self)
{
  const dt_image_t *img = &self->dev->image_storage;
  const float image_size = (float)MAX(img->width, img->height);
  return image_size > 1.0f ? image_size : 4096.0f;
}

static gboolean gain_graph_draw(GtkWidget *widget,
                                cairo_t *cr,
                                dt_iop_module_t *self)
{
  const dt_iop_contrastntexture_params_t *p = self->params;
  const dt_iop_contrastntexture_gui_data_t *g = self->gui_data;

  const float max_image_size = gui_max_image_size(self);

  const float x_min = 1.0f;
  const float x_max = fmaxf(max_pyramid_level_for(max_image_size, 0.0f) + 1.0f, x_min);
  const float max_pyramid_level = max_pyramid_level_for(max_image_size, p->base_detail_level);
  const float band_gains[DT_LC_MASK_LAST] = { p->gain_coarse, p->gain_medium, p->gain_fine };

  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);

  PangoFontDescription *desc = dt_gui_get_font();
  PangoLayout *layout = pango_cairo_create_layout(cr);
  pango_layout_set_font_description(layout, desc);
  pango_cairo_context_set_resolution(pango_layout_get_context(layout), darktable.gui->dpi);
  PangoRectangle ink;
  pango_layout_set_text(layout, "-100%", -1);
  pango_layout_get_pixel_extents(layout, &ink, NULL);

  const double margin_left = ink.width + DT_PIXEL_APPLY_DPI(6);
  const double margin_bottom = DT_PIXEL_APPLY_DPI(4);
  const double margin_top = DT_PIXEL_APPLY_DPI(4);
  const double margin_right = DT_PIXEL_APPLY_DPI(4);
  const double width = allocation.width - margin_left - margin_right;
  const double height = allocation.height - margin_top - margin_bottom;
  if(width < 10.0 || height < 10.0)
  {
    pango_font_description_free(desc);
    g_object_unref(layout);
    return FALSE;
  }

  // Determine whether a single band is being previewed, which affects the y-axis range and the total gain calculation.
  const gboolean band_preview = g->details_display != DT_LC_MASK_OFF && g->details_display != DT_LC_MASK_LAST;

  // The band weights sum to 1 at every level, so the total gain is a weighted average of the band gains
  // and never leaves their range. The y-range can therefore be set up front. A previewed band is
  // shown alone at full strength, i.e. 0 to 100%.
  float y_min = -0.5f, y_max = 0.5f;
  if(band_preview)
  {
    y_max = 1.0f;
  }
  else
  {
    for(int band = 0; band < DT_LC_MASK_LAST; band++)
    {
      y_min = fminf(y_min, band_gains[band]);
      y_max = fmaxf(y_max, band_gains[band]);
    }
  }
  const float y_pad = 0.05f * (y_max - y_min);
  y_min -= y_pad;
  y_max += y_pad;

  // The curve is a stair function to reflect the underlying detail structure:
  // it only changes at whole pyramid levels, so one value per pyramid level is enough.
  const int level_count = CLAMP((int)floorf(x_max - p->base_detail_level) + 1, 1, MAX_ITERATIONS);
  float total_gain_per_level[MAX_ITERATIONS];
  for(int pyramid_level = 0; pyramid_level < level_count; pyramid_level++)
  {
    float total_gain = 0.0f;
    if(band_preview)
      total_gain = band_weight(g->details_display, pyramid_level, max_pyramid_level);
    else
    {
      for(int band = 0; band < DT_LC_MASK_LAST; band++)
      {
        total_gain += band_gains[band] * band_weight(band, pyramid_level, max_pyramid_level);
      }
    }
    total_gain_per_level[pyramid_level] = total_gain;
  }

  cairo_translate(cr, margin_left, margin_top);
  set_color(cr, darktable.bauhaus->graph_bg);
  cairo_rectangle(cr, 0, 0, width, height);
  cairo_fill(cr);

  // y(value) and x(value) to pixel coordinates
#define GRAPH_X(v) (((v) - x_min) / (x_max - x_min) * width)
#define GRAPH_Y(v) ((1.0 - ((v) - y_min) / (y_max - y_min)) * height)

  // filter instances in use, one per pyramid level starting at the base detail level
  int max_used_pyramid_level = 0;
  for(int pyramid_level = 0; pyramid_level < MAX_ITERATIONS; pyramid_level++)
  {
    const float scale = powf(2.0f, -p->base_detail_level - (float)pyramid_level);
    const float detail_size = max_image_size * scale;
    if(pyramid_level > 0 && detail_size < NYQUIST_SIZE) break;
    max_used_pyramid_level = pyramid_level;
  }
  cairo_save(cr);
  cairo_rectangle(cr, 0, 0, width, height);
  cairo_clip(cr);
  const gboolean lowpass_preview = g->details_display == DT_LC_MASK_LAST;
  const double dashes[] = { DT_PIXEL_APPLY_DPI(5), DT_PIXEL_APPLY_DPI(5) };
  cairo_set_dash(cr, dashes, 2, 0);
  set_color(cr, darktable.bauhaus->graph_overlay);
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.5));
  for(int pyramid_level = 0; pyramid_level <= max_used_pyramid_level; pyramid_level++)
  {
    const float detail = p->base_detail_level + (float)pyramid_level;
    if(detail < x_min || detail > x_max) continue;
    // the base detail level is solid and brighter than the other filter instances
    if(pyramid_level == 0)
    {
      cairo_set_dash(cr, NULL, 0, 0);
      // highlighted while the base detail level preview shows the low pass of that level
      set_color(cr, lowpass_preview ? darktable.bauhaus->graph_fg_active : darktable.bauhaus->graph_fg);
      if(lowpass_preview) cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(3.0));
    }
    cairo_move_to(cr, GRAPH_X(detail), 0);
    cairo_line_to(cr, GRAPH_X(detail), height);
    cairo_stroke(cr);
    if(pyramid_level == 0)
    {
      cairo_set_dash(cr, dashes, 2, 0);
      set_color(cr, darktable.bauhaus->graph_overlay);
      cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.5));
    }
  }
  cairo_restore(cr);

  // y zero line
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.5));
  set_color(cr, darktable.bauhaus->graph_overlay);
  cairo_move_to(cr, 0, GRAPH_Y(0.0f));
  cairo_line_to(cr, width, GRAPH_Y(0.0f));
  cairo_stroke(cr);

  // total gain curve
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.5));
  set_color(cr, darktable.bauhaus->graph_fg_active);
  // detail levels finer than the base detail level are not processed, so the curve starts there
  for(int pyramid_level = 0; pyramid_level < level_count; pyramid_level++)
  {
    const float detail_start = p->base_detail_level + (float)pyramid_level;
    const float detail_end = fminf(detail_start + 1.0f, x_max);
    const double pixel_y = GRAPH_Y(total_gain_per_level[pyramid_level]);
    if(pyramid_level == 0) cairo_move_to(cr, GRAPH_X(detail_start), pixel_y);
    else cairo_line_to(cr, GRAPH_X(detail_start), pixel_y);
    cairo_line_to(cr, GRAPH_X(detail_end), pixel_y);
  }
  cairo_stroke(cr);

  // labels
  set_color(cr, darktable.bauhaus->graph_fg);
  char text[32];
  const float y_values[3] = { y_min + y_pad, 0.0f, y_max - y_pad };
  for(int i = 0; i < 3; i++)
  {
    snprintf(text, sizeof(text), "%.0f%%", 100.0f * y_values[i]);
    draw_label(cr, layout, text, -DT_PIXEL_APPLY_DPI(3), GRAPH_Y(y_values[i]), 1.0, 0.5);
  }
#undef GRAPH_X
#undef GRAPH_Y

  pango_font_description_free(desc);
  g_object_unref(layout);
  return FALSE;
}

void gui_changed(dt_iop_module_t *self,
                 GtkWidget *w,
                 void *previous)
{
  dt_iop_contrastntexture_gui_data_t *g = self->gui_data;
  gtk_widget_queue_draw(g->gain_graph);
}

void gui_update(dt_iop_module_t *self)
{
  dt_iop_contrastntexture_gui_data_t *g = self->gui_data;

  // at the maximum, coarse, medium and fine each sit on exactly one pyramid level
  const float hard_min = dt_bauhaus_slider_get_hard_min(g->base_detail_level);
  const float hard_max = fmaxf(max_pyramid_level_for(gui_max_image_size(self), 2.0f), hard_min);
  dt_bauhaus_slider_set_hard_max(g->base_detail_level, hard_max);

  gui_changed(self, NULL, NULL);
}

void gui_init(dt_iop_module_t *self)
{
  dt_iop_contrastntexture_gui_data_t *g = IOP_GUI_ALLOC(contrastntexture);
  dt_iop_gui_enter_critical_section(self);
  g->details_display = DT_LC_MASK_OFF;
  dt_iop_gui_leave_critical_section(self);

  // Main container
  self->widget = dt_gui_vbox();

  dt_gui_new_collapsible_section(&g->graph_section, "plugins/darkroom/contrastntexture/expand_gain_graph",
                                 _("show gain graph"), GTK_BOX(self->widget), DT_ACTION(self));
  g->gain_graph = dtgtk_drawing_area_new_with_aspect_ratio(0.5f);
  gtk_widget_set_tooltip_text(g->gain_graph,
     _("total gain applied per detail size.\n"
       "solid vertical line: base detail level\n"
       "dashed vertical lines: filter instances (pyramid levels) in use"));
  g_signal_connect(G_OBJECT(g->gain_graph), "draw", G_CALLBACK(gain_graph_draw), self);
  dt_gui_box_add(g->graph_section.container, g->gain_graph);

  g->base_detail_level = dt_bauhaus_slider_from_params(self, "base_detail_level");
  gtk_widget_set_tooltip_text(g->base_detail_level,
     _("adjust the detail level used for highlights, shadows, and coarse details.\n"
       "higher = more contrast boost in finer details\n"
       "lower = more contrast boost in coarser details\n"
       "press the mask button to preview the low pass filter result used for shadows/highlights."));
  dt_bauhaus_widget_set_quad(g->base_detail_level, self, dtgtk_cairo_paint_showmask, TRUE, show_details_callback,
                             _("preview the low pass filter result used for shadows(blue)/highlights(yellow)."));

  // Details boost sliders
  dt_gui_box_add(self->widget, dt_ui_section_label_new(_("texture")));
  const char *labels[DT_LC_MASK_LAST] = {"gain_coarse", "gain_medium", "gain_fine"};
  for(int i = 0; i < DT_LC_MASK_LAST; i++)
  {
    g->gain_details[i] = dt_bauhaus_slider_from_params(self, labels[i]);
    dt_bauhaus_slider_set_soft_range(g->gain_details[i], -1.0, 1.5);
    dt_bauhaus_slider_set_digits(g->gain_details[i], 2);
    dt_bauhaus_slider_set_format(g->gain_details[i], "%");
    dt_bauhaus_slider_set_factor(g->gain_details[i], 100.0);
  }
  gtk_widget_set_tooltip_text(g->gain_details[DT_LC_MASK_COARSE],
                              _("adjust coarse, low frequency content.\n"
                                "press the mask button to preview the effect."));
  dt_bauhaus_widget_set_quad(g->gain_details[DT_LC_MASK_COARSE], self, dtgtk_cairo_paint_showmask, TRUE, show_details_callback,
                             _("preview the size of the coarse details to adjust."));

  gtk_widget_set_tooltip_text(g->gain_details[DT_LC_MASK_MEDIUM],
                              _("adjust medium, frequency content between coarse and fine.\n"
                                "press the mask button to preview the effect."));
  dt_bauhaus_widget_set_quad(g->gain_details[DT_LC_MASK_MEDIUM], self, dtgtk_cairo_paint_showmask, TRUE, show_details_callback,
                             _("preview the size of the medium details to adjust."));

  gtk_widget_set_tooltip_text(g->gain_details[DT_LC_MASK_FINE],
                              _("adjust fine, high frequency content.\n"
                                "press the mask button to preview the effect."));
  dt_bauhaus_widget_set_quad(g->gain_details[DT_LC_MASK_FINE], self, dtgtk_cairo_paint_showmask, TRUE, show_details_callback,
                             _("preview the size of the fine details to adjust."));

  // Highlights and shadows sliders
  dt_gui_box_add(self->widget, dt_ui_section_label_new(_("contrast")));
  g->gain_highlights = dt_bauhaus_slider_from_params(self, "gain_highlights");
  dt_bauhaus_slider_set_soft_range(g->gain_highlights, -2.0, 2.0);
  gtk_widget_set_tooltip_text(g->gain_highlights,
    _("adjust highlights at the base detail level size."));
  g->gain_shadows = dt_bauhaus_slider_from_params(self, "gain_shadows");
  dt_bauhaus_slider_set_soft_range(g->gain_shadows, -2.0, 2.0);
  gtk_widget_set_tooltip_text(g->gain_shadows,
    _("adjust shadows at the base detail level size."));

  // Filter settings section
  dt_gui_box_add(self->widget, dt_ui_section_label_new(C_("section", "filter settings")));

  g->halo_control = dt_bauhaus_slider_from_params(self, "halo_control");
  dt_bauhaus_slider_set_soft_range(g->halo_control, -5.0, 5.0);
  dt_bauhaus_slider_set_digits(g->halo_control, 2);
  dt_bauhaus_slider_set_format(g->halo_control, "%");
  dt_bauhaus_slider_set_factor(g->halo_control, 100.0);
  gtk_widget_set_tooltip_text(g->halo_control, _("adjust the halo control of the filter.\n"
                                                 "higher = suppress halos at the expense of details around edges.\n"
                                                 "lower = allow more halos to get more local contrast and details."));

  g->noise_bias = dt_bauhaus_slider_from_params(self, "noise_bias");
  dt_bauhaus_slider_set_soft_range(g->noise_bias, 0.0, 0.2);
  dt_bauhaus_slider_set_digits(g->noise_bias, 4);
  dt_bauhaus_slider_set_step(g->noise_bias, 0.0001);
  gtk_widget_set_tooltip_text(g->noise_bias, _("add bias to reduce shadow noise amplification.\n"
                                               "only affects dark parts of the image."));
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
