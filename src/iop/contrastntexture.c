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
- Local contrast in log space based on the eigf surface blur filter.
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
#include "common/fast_guided_filter.h"
#include "common/eigf.h"
#include "common/luminance_mask.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/blend.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "develop/imageop_gui.h"
#include "gui/accelerators.h"
#include "gui/draw.h"
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

DT_MODULE_INTROSPECTION(2, dt_iop_contrastntexture_params_t)

typedef struct dt_iop_contrastntexture_params_t
{
  float gain_coarse;     // $MIN: -1.0 $MAX: 5.0 $DEFAULT: 0.0 $DESCRIPTION: "coarse details"
  float gain_medium;     // $MIN: -1.0 $MAX: 5.0 $DEFAULT: 0.0 $DESCRIPTION: "medium details"
  float gain_fine;       // $MIN: -1.0 $MAX: 5.0 $DEFAULT: 0.0 $DESCRIPTION: "fine details"
  float detail_level;    // $MIN: 1.0 $MAX: 15.0 $DEFAULT: 5.0 $DESCRIPTION: "base detail level"
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
  // coarse, broad, medium, fine, micro
  float gain_details[DT_LC_MASK_LAST];
  float radius_details[MAX_ITERATIONS];
  float scale_details[MAX_ITERATIONS];
  float max_detail_level;
  int   max_used_level;
  float feathering;
  float noise_bias[MAX_ITERATIONS];
  float slope_shadows;
  float slope_highlights;
  float midtones_width;
  float midtones_polynomial[3];
} dt_iop_contrastntexture_data_t;

typedef struct dt_iop_contrastntexture_gui_data_t
{
  // Flags
  dt_iop_contrastntexture_details_display_t details_display;

  // GTK widgets adjustments
  GtkWidget *gain_details[DT_LC_MASK_LAST]; // coarse, medium, and fine
  GtkWidget *gain_shadows;
  GtkWidget *gain_highlights;

  // GTK widgets filter settings
  GtkWidget *detail_level;
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
    (self, _("enhance local contrast by boosting details while preserving edges"),
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
    n->detail_level = o->detail_level;
    n->halo_control = o->edge_protection;
    n->noise_bias = o->noise_bias;
    n->gain_highlights = 0.0f;
    n->gain_shadows = 0.0f;

    *new_params = n;
    *new_params_size = sizeof(dt_iop_contrastntexture_params_t);
    *new_version = 2;
    return 0;
  }
  return 1;
}

// Compute smoothed luminance mask using edge-aware filters
__DT_CLONE_TARGETS__
static inline void compute_luminance(const float *const restrict in,
                                      float *const restrict luminance,
                                      const dt_iop_roi_t *const roi_in,
                                      const float noise_bias)
{
  const size_t width = (size_t)roi_in->width;
  const size_t height = (size_t)roi_in->height;
  const size_t npixels = width * height;

  // First compute pixel-wise luminance (no boost) and add noise bias
  luminance_mask(in, luminance, width, height, DT_TONEEQ_NORM_2, 1.0f, 0.0f, 1.0f);

  DT_OMP_FOR()
  for(size_t k = 0; k < npixels; k++)
  {
    luminance[k] += noise_bias;
  }
}

// Compute smoothed luminance mask using edge-aware filters
__DT_CLONE_TARGETS__
static inline void compute_mask(float *const restrict smoothed_luminance,
                                const dt_iop_roi_t *const roi_in,
                                const dt_iop_contrastntexture_data_t *const d,
                                const int level)
{
  const size_t width = (size_t)roi_in->width;
  const size_t height = (size_t)roi_in->height;

  fast_eigf_surface_blur(smoothed_luminance, width, height,
                         d->radius_details[level], d->feathering, 1,
                         DT_GF_BLENDING_LINEAR, 1.0f,
                         0.0f, NORM_MIN, 4.0f);
}

// Extract logarithmic high pass detail in log space (EV):
// How much brighter/darker is this pixel compared to the smooth version
__DT_CLONE_TARGETS__
static inline float extract_details(const float luminance_pixel,
                                   const float luminance_smoothed,
                                   const float noise_bias)
{
  const float log_pixel = log2f(fmaxf(luminance_pixel, NORM_MIN));
  const float log_smoothed = log2f(fmaxf(luminance_smoothed, NORM_MIN));

  const float noise_power = noise_bias * noise_bias;
  const float combined_power = luminance_smoothed * luminance_smoothed;
  const float weiner_gain = fmaxf(combined_power - noise_power, 0.0f) / fmaxf(combined_power, NORM_MIN);
  return weiner_gain * fmaxf(fminf(log_pixel - log_smoothed, 5.0f), -5.0f);
}

// Squared cosine/sine crossfade weight of a given band (coarse/medium/fine) at position t
static inline float band_weight(const dt_iop_contrastntexture_details_display_t band, const int level, const float max_level)
{
  float t = (float)level / max_level - 0.5f;
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

// Apply shadow and highlight enhancement
// Different slopes for shadows and highlights, with a smooth transition in the midtones
__DT_CLONE_TARGETS__
static inline float apply_shadows_highlights(const float luminance_lowpass,
                                             const dt_iop_contrastntexture_data_t *const d)
{
  const float slope_shadows = d->slope_shadows;
  const float slope_highlights = d->slope_highlights;
  const float midtones_width = d->midtones_width;
  const float a0 = d->midtones_polynomial[0];
  const float a1 = d->midtones_polynomial[1];
  const float a2 = d->midtones_polynomial[2];
  const float noise_bias = d->noise_bias[0]; // Applied on the base level
  const float pivot_offset = log2f(noise_bias + 0.1845f);

  float correction_ev = 0.0f;
  // Low pass correction for shadows and highlights
  const float normalized_ev = log2f(fmaxf(luminance_lowpass, NORM_MIN)) - pivot_offset;
  if(normalized_ev <= -midtones_width)
    correction_ev += slope_shadows * normalized_ev;
  else if(normalized_ev >= midtones_width)
    correction_ev += slope_highlights * normalized_ev;
  else
  {
    const float shifted_ev = normalized_ev + midtones_width; // Shift to [0, 2*midtones_width] for polynomial evaluation
    correction_ev += (a0 + a1 * shifted_ev + a2 * shifted_ev * shifted_ev);
  }
  return correction_ev - normalized_ev; // Its only the difference from the identity line that matters for correction
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
    for_each_channel(c)
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
static inline void display_lowpass_mask(const float *const restrict luminance_lowpass,
                                        float *const restrict out,
                                        const dt_iop_roi_t *const roi_in,
                                        const dt_iop_contrastntexture_data_t *const d)
{
  const size_t npixels = roi_in->width * roi_in->height;
  const float pivot_offset = d->noise_bias[0] + 0.1845f;

  const dt_aligned_pixel_t blue = {0.0f, 0.0f, 1.0f};
  const dt_aligned_pixel_t yellow = {1.0f, 1.0f, 0.0f};

  DT_OMP_FOR()
  for(size_t k = 0; k < npixels; k++)
  {
    // Low contrast sigmoid for the preview.
    const float intensity = luminance_lowpass[k] / (pivot_offset + luminance_lowpass[k]);
    float saturation = (intensity - 0.5f) * 2.0f;
    saturation *= saturation;

    for_each_channel(c)
    {
      if(intensity < 0.5f)
        out[4 * k + c] = intensity * ((1.0f - saturation) + saturation * blue[c]);
      else
        out[4 * k + c] = intensity * ((1.0f - saturation) + saturation * yellow[c]);
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
  float *restrict luminance_highpass = dt_alloc_align_float(npixels);
  float *restrict luminance_lowpass = dt_alloc_align_float(npixels);
  float *restrict corrections = dt_alloc_align_float(npixels);
  if(!luminance_lowpass ||
     !luminance_highpass ||
     !corrections)
  {
    dt_control_log(_("contrast and texture failed to allocate memory, check your RAM settings"));
    dt_free_align(luminance_highpass);
    dt_free_align(luminance_lowpass);
    dt_free_align(corrections);
    return;
  }

  // The actual gain per level is computed in process instead of commit params
  // as its affected by the dispaly mask option.
  float gain_per_level[MAX_ITERATIONS] = { 0.0f };

  // Display output
  bool display_mask = false;
  if(g && g->details_display != DT_LC_MASK_OFF && (piece->pipe->type & DT_DEV_PIXELPIPE_FULL))
  {
    display_mask = true;
    piece->pipe->mask_display = DT_DEV_PIXELPIPE_DISPLAY_PASSTHRU;
    // Preview the displayed band alone, at full strength, across all pyramid levels
    // (the lowpass preview does not use band weights, the final luminance_lowpass suffices)
    if(g->details_display != DT_LC_MASK_LAST)
    {
      for(int level = 0; level <= d->max_used_level; level++)
      {
        gain_per_level[level] = band_weight(g->details_display, level, d->max_detail_level);
      }
    }
  }
  else
  {
    // Smoothly interpolate the 3 sliders (coarse/medium/fine) across all pyramid
    // levels using squared cosine/sine crossfade weights, so neighboring levels blend
    // instead of jumping discretely between the 3 gains.
    for(int level = 0; level <= d->max_used_level; level++)
    {
      for(int band = 0; band < DT_LC_MASK_LAST; band++)
      gain_per_level[level] += d->gain_details[band] * band_weight(band, level, d->max_detail_level);
    }
  }

  const float max_noise_bias = d->noise_bias[d->max_used_level];
  compute_luminance(in, luminance_lowpass, roi_in, max_noise_bias);
  memset(corrections, 0, npixels * sizeof(float));

  for(int level = d->max_used_level; level >= 0; level--)
  {
    memcpy(luminance_highpass, luminance_lowpass, npixels * sizeof(float));
    compute_mask(luminance_lowpass, roi_in, d, level);

    DT_OMP_FOR()
    for(size_t k = 0; k < npixels; k++)
    {
      // Details as the bandpass difference
      corrections[k] += gain_per_level[level] * extract_details(luminance_highpass[k], luminance_lowpass[k], d->noise_bias[level]);

      // Reduce the noise bias to the next level
      if(level > 0)
        luminance_lowpass[k] -= d->noise_bias[level] - d->noise_bias[level - 1];
    }
  }

  if(display_mask)
  {
    if(g->details_display == DT_LC_MASK_LAST)
      display_lowpass_mask(luminance_lowpass, out, roi_in, d);
    else
      display_local_mask(corrections, out, roi_in);
  }
  else
  {
    DT_OMP_FOR()
    for(size_t k = 0; k < npixels; k++)
    {
      // Low pass correction for shadows and highlights
      float lowpass_correction = apply_shadows_highlights(luminance_lowpass[k], d);

      // Apply correction in linear space
      const float multiplier = exp2f(corrections[k] + lowpass_correction);;
      for_each_channel(c)
        out[4 * k + c] = in[4 * k + c] * multiplier;
      out[4 * k + 3] = in[4 * k + 3];
    }
  }

  dt_free_align(luminance_highpass);
  dt_free_align(luminance_lowpass);
  dt_free_align(corrections);
}

void modify_roi_in(dt_iop_module_t *self,
                   dt_dev_pixelpipe_iop_t *piece,
                   const dt_iop_roi_t *roi_out,
                   dt_iop_roi_t *roi_in)
{
  dt_iop_contrastntexture_data_t *const d = piece->data;

  // Get the scaled window radius for the box average
  const float max_size = (float)((piece->iwidth > piece->iheight) ? piece->iwidth : piece->iheight);
  for(int level = 0; level < MAX_ITERATIONS; level++)
  {
    const float base_diameter = max_size * roi_in->scale;
    const float radius = 0.5f * fminf(d->scale_details[level], 0.5f) * base_diameter;
    d->radius_details[level] = radius;
  }

  // Find the smallest level with non zero radius.
  d->max_used_level = MAX_ITERATIONS - 1;
  for(int level = MAX_ITERATIONS - 1; level >= 0; level--)
  {
    // If the radius is <2.0 pixels, the next level will be unused (<1.0)
    if(d->radius_details[level] < 2.0f)
    {
      d->max_used_level = level;
    }
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
  d->max_detail_level = log2f(max_image_size) - p->detail_level - 1.0f;

  // UI contrast scale is inverse logarithmic with 0 as 100% of image width.
  // Convert it to a linear scale for processing. Scales are separated by powers of 2 for each step in the UI.
  // The noise bias is reduced for each level in the pyramid.
  for(int level = 0; level < MAX_ITERATIONS; level++)
  {
    d->scale_details[level] = powf(2.0f, -p->detail_level - (float)level);
    d->noise_bias[level] = p->noise_bias * powf(2.0f, fminf((float)level - d->max_detail_level, 0.0f));
  }

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

static void show_details_callback(GtkWidget *togglebutton, dt_iop_module_t *self)
{
  // early return if blend module is already displaying a mask
  if(self->request_mask_display)
  {
    dt_control_log(_("cannot display masks when the blending mask is displayed"));
    dt_bauhaus_widget_set_quad_active(GTK_WIDGET(togglebutton), FALSE);
    return;
  }

  DT_GUARD_GUI_UPDATE();
  dt_iop_request_focus(self);
  // Activate the module if it wasn't
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->off), TRUE);

  dt_iop_contrastntexture_gui_data_t *g = self->gui_data;
  g->details_display = DT_LC_MASK_OFF;

  const gboolean toggle_is_active = dt_bauhaus_widget_get_quad_active(GTK_WIDGET(togglebutton));
  if(toggle_is_active)
  {
    if(togglebutton == g->detail_level)
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

  for(int i = 0; i < DT_LC_MASK_LAST; i++)
  {
    dt_bauhaus_widget_set_quad_active(GTK_WIDGET(g->gain_details[i]), g->details_display == i);
  }
  dt_bauhaus_widget_set_quad_active(GTK_WIDGET(g->detail_level), g->details_display == DT_LC_MASK_LAST);
  dt_iop_refresh_center(self);
}

void gui_focus(dt_iop_module_t *self, const gboolean in)
{
  if(!in)
  {
    dt_iop_contrastntexture_gui_data_t *g = self->gui_data;
    const gboolean was_mask = g->details_display != DT_LC_MASK_OFF;
    g->details_display = DT_LC_MASK_OFF;
    for(int i = 0; i < DT_LC_MASK_LAST; i++)
      dt_bauhaus_widget_set_quad_active(GTK_WIDGET(g->gain_details[i]), FALSE);
    dt_bauhaus_widget_set_quad_active(GTK_WIDGET(g->detail_level), FALSE);
    if(was_mask)
      dt_iop_refresh_center(self);
  }
}

void gui_init(dt_iop_module_t *self)
{
  dt_iop_contrastntexture_gui_data_t *g = IOP_GUI_ALLOC(contrastntexture);
  g->details_display = DT_LC_MASK_OFF;

  // Main container
  self->widget = dt_gui_vbox();

  g->detail_level = dt_bauhaus_slider_from_params(self, "detail_level");
  dt_bauhaus_slider_set_soft_range(g->detail_level, 1.0, 10.0);
  gtk_widget_set_tooltip_text(g->detail_level,
     _("adjust the detail level used for highlights, shadows, and coarse details.\n"
       "higher = more contrast boost in finer details\n"
       "lower = more contrast boost in coarser details\n"
       "press the mask button to preview the low pass filter result used for shadows/highlights."));
  dt_bauhaus_widget_set_quad(g->detail_level, self, dtgtk_cairo_paint_showmask, TRUE, show_details_callback,
                             _("preview the low pass filter result used for shadows(blue)/highlights(yellow)."));

  // Highlights and shadows sliders
  g->gain_highlights = dt_bauhaus_slider_from_params(self, "gain_highlights");
  dt_bauhaus_slider_set_soft_range(g->gain_highlights, -2.0, 2.0);
  gtk_widget_set_tooltip_text(g->gain_highlights,
    _("adjust highlights at the base detail level size."));
  g->gain_shadows = dt_bauhaus_slider_from_params(self, "gain_shadows");
  dt_bauhaus_slider_set_soft_range(g->gain_shadows, -2.0, 2.0);
  gtk_widget_set_tooltip_text(g->gain_shadows,
    _("adjust shadows at the base detail level size."));

  dt_gui_box_add(self->widget, dt_ui_section_label_new(_("local contrast")));

  // Details boost sliders
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
