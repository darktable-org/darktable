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
This module performs advanced multi-scale contrast processing in scene-referred linear RGB space, 
designed for wide-gamut workflows in any linear RGB working space.

It builds upon the original proof-of-concept algorithm proposed by WileCoyote:  
https://discuss.pixls.us/t/experiments-with-a-scene-referred-local-contrast-module-proof-of-concept/55402

The module automatically adapts to the native sensor resolution.
A 36 MP sensor is used as reference. 
The contrast scale and spatial edge protection parameters are normalized accordingly, 
the default settings provide a coherent starting point regardless of resolution, from 6 MP to 60 MP.

Architecture
The module decomposes the image into five interdependent frequency scales using edge-aware spatial filtering (EIGF).
Contrast is modeled through three complementary components:

1. Global Contrast  
    Adjusted via a Contrast Sensitivity Function (CSF) centered around middle gray (0.1845), approximating human visual response.
    
2. Multi-scale Local Contrast  
    A harmonic five-layer frequency (micro to coarse), driven by a spatial blending parameter.  
    Due to the nature of spatial decomposition, frequency bands are structurally interdependent.
    
3. Chromatic Contrast  
    _Colorimetric Contrast_: Modulates luminance contrast based on the red/blue channel
    difference, normalized by mean brightness. Space-agnostic — works in any linear RGB
    working space.
    _Colorful Contrast_: Enhances color separation between warm and cool tones while
    preserving overall luminance. The green channel compensation is derived from the
    pipe working profile luminance coefficients.
*/



#include "common/extra_optimizations.h"

#include <assert.h>
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

static inline float dt_smoothstep(const float edge0, const float edge1, const float x)
{
  const float t = CLAMP((fabsf(x) - edge0) / fmaxf(edge1 - edge0, NORM_MIN), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

DT_MODULE_INTROSPECTION(1, dt_iop_contrast_params_t)


typedef struct dt_iop_contrast_params_t
{
  float gain_local_contrast;  // $MIN: 0.0 $MAX: 5.0 $DEFAULT: 1.0  $DESCRIPTION: "local contrast"
  float contrast_scale;       // $MIN: 1.0 $MAX: 2.0 $DEFAULT: 1.2 $DESCRIPTION: "contrast scale"
  float edge_protection;      // $MIN: -10.0 $MAX: 10.0 $DEFAULT: 0.0 $DESCRIPTION: "adjust edge protection"
  int filter_iterations;      // $MIN: 1 $MAX: 20 $DEFAULT: 1 $DESCRIPTION: "filter iterations"  
  float noise_threshold;      // $MIN: 0.0 $MAX: 0.01 $DEFAULT: 0.001 $DESCRIPTION: "noise threshold"
} dt_iop_contrast_params_t;

typedef enum dt_iop_contrast_mask_t
{
  DT_LC_MASK_OFF = -1,
  DT_LC_MASK_LOCAL = 0
} dt_iop_contrast_mask_t;

typedef struct dt_iop_contrast_data_t
{
  float gain_local_contrast;
  float blending;
  float feathering;
  int radius_local;
  int iterations;
  float noise_threshold;
} dt_iop_contrast_data_t;

typedef struct dt_iop_contrast_gui_data_t
{
  // Flags
  dt_iop_contrast_mask_t mask_display;

  int pipe_order;

  // Hash for cache invalidation
  dt_hash_t ui_preview_hash;
  dt_hash_t thumb_preview_hash;
  size_t full_preview_buf_width, full_preview_buf_height;
  size_t thumb_preview_buf_width, thumb_preview_buf_height;

  // Cached luminance buffers
  float *thumb_preview_buf_pixel;     // pixel-wise luminance (no blur)
  float *thumb_preview_buf_smoothed_local;  // smoothed luminance
  float *full_preview_buf_pixel;
  float *full_preview_buf_smoothed_local;

  // Cache validity
  gboolean luminance_valid;

  // GTK widgets
  GtkWidget *gain_local_contrast;
  GtkWidget *contrast_scale;
  GtkWidget *edge_protection;
  GtkWidget *filter_iterations;
  GtkWidget *noise_threshold;

  // New buttons for mask display in expanders
  GtkWidget *f_view_local;
} dt_iop_contrast_gui_data_t;


const char *name()
{
  return _("contrast & texture");
}

const char *aliases()
{
  return _("local contrast|clarity|detail enhancement");
}

const char **description(dt_iop_module_t *self)
{
  return dt_iop_set_description
    (self, _("enhance local contrast by boosting contrast while preserving edges"),
     _("creative"),
     _("linear, RGB, scene-referred"),
     _("linear, RGB"),
     _("linear, RGB, scene-referred"));
}

int default_group()
{
  return IOP_GROUP_TONE | IOP_GROUP_EFFECTS;
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
  return 1;
}

/*
static void hash_set_get(const dt_hash_t *hash_in,
                         dt_hash_t *hash_out,
                         dt_pthread_mutex_t *lock)
{
  dt_pthread_mutex_lock(lock);
  *hash_out = *hash_in;
  dt_pthread_mutex_unlock(lock);
}
*/


static void invalidate_luminance_cache(dt_iop_module_t *const self)
{
  dt_iop_contrast_gui_data_t *const restrict g = self->gui_data;

  dt_iop_gui_enter_critical_section(self);
  g->luminance_valid = FALSE;
  g->thumb_preview_hash = DT_INVALID_HASH;
  g->ui_preview_hash = DT_INVALID_HASH;
  dt_iop_gui_leave_critical_section(self);
  dt_iop_refresh_all(self);
}

// Compute smoothed luminance mask using edge-aware filters

__DT_CLONE_TARGETS__
static inline void compute_luminance_and_mask(const float *const restrict in,
                                                   float *const restrict luminance,
                                                   float *const restrict smoothed_luminance,
                                                   const size_t width,
                                                   const size_t height,
                                                   const dt_iop_contrast_data_t *const d)
{
  // First compute pixel-wise luminance (no boost)
  luminance_mask(in, luminance, width, height, DT_TONEEQ_NORM_2, 1.0f, 0.0f, 1.0f);

  // Then apply the smoothing filter on a copy
  memcpy(smoothed_luminance, luminance, width * height * sizeof(float));

  fast_eigf_surface_blur(smoothed_luminance, width, height,
                         d->radius_local, d->feathering, d->iterations,
                         DT_GF_BLENDING_LINEAR, 1.0f,
                         0.0f, exp2f(-14.0f), 4.0f);
}


// Apply local contrast enhancement
// The detail (local contrast) is the log-space difference between pixel luminance
// and smoothed luminance. Boosting this difference amplifies local details.

__DT_CLONE_TARGETS__
static inline void apply_local_contrast(const float *const restrict in,
                                        const float *const restrict luminance_pixel,
                                        const float *const restrict luminance_smoothed,
                                        float *const restrict out,
                                        const dt_iop_roi_t *const roi_in,
                                        const dt_iop_roi_t *const roi_out,
                                        const dt_iop_contrast_data_t *const d)
{
  const size_t npixels = (size_t)roi_in->width * roi_in->height;
  const float gain_local = d->gain_local_contrast;
  
  DT_OMP_FOR()
  for(size_t k = 0; k < npixels; k++)
  {
    const float lum_pixel = fmaxf(luminance_pixel[k], MIN_FLOAT);
    const float lum_smoothed = fmaxf(luminance_smoothed[k], MIN_FLOAT);

    // Detail in log space (EV): how much brighter/darker is this pixel
    // compared to its local neighborhood
    // detail = log2(pixel_lum / smoothed_lum) = log2(pixel_lum) - log2(smoothed_lum)
    const float local_ev = log2f(lum_pixel / lum_smoothed);

    // The correction is the sum of (gain - 1) * detail for each scale
    float correction_ev = (gain_local - 1.0f) * local_ev;

    // Noise protection (Smoothstep on detail magnitude)
    if (d->noise_threshold > NORM_MIN) {
        const float edge0 = d->noise_threshold * 0.5f;
        const float edge1 = edge0 * 1.5f;
        correction_ev *= dt_smoothstep(edge0, edge1, correction_ev);
    }
    
    // Apply correction in linear space
    const float multiplier = exp2f(correction_ev);

    const float L_final = lum_pixel * multiplier;
    float ratio = L_final / fmaxf(lum_pixel, NORM_MIN);
    ratio = fminf(ratio, 8.0f);

    for_each_channel(c)
        out[4 * k + c] = in[4 * k + c] * ratio;
    out[4 * k + 3] = in[4 * k + 3];
  }
}


/*
 Display the detail mask (difference between pixel and smoothed luminance)
 Output is a grayscale image normalized to [0, 1] where:
 - 0.5 = no local detail (pixel matches neighborhood)
 - < 0.5 = pixel darker than neighborhood
 - > 0.5 = pixel brighter than neighborhood
 */
__DT_CLONE_TARGETS__
static inline void display_local_mask(const float *const restrict luminance_pixel,
                                      const float *const restrict luminance_smoothed,
                                      float *const restrict out,
                                      const size_t width,
                                      const size_t height)
{
  const size_t npixels = width * height;

  DT_OMP_FOR()
  for(size_t k = 0; k < npixels; k++)
  {
    const float lum_pixel = fmaxf(luminance_pixel[k], MIN_FLOAT);
    const float lum_smoothed = fmaxf(luminance_smoothed[k], MIN_FLOAT);

    // Detail in log space, mapped to [0, 1] for display
    // Detail range roughly [-2, +2] EV mapped to [0, 1]
    const float local_ev = log2f(lum_pixel / lum_smoothed);
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


// Main processing function
/*
__DT_CLONE_TARGETS__
static void spatial_contrast_process(dt_iop_module_t *self,
                                       dt_dev_pixelpipe_iop_t *piece,
                                       const void *const restrict ivoid,
                                       void *const restrict ovoid,
                                       const dt_iop_roi_t *const roi_in,
                                       const dt_iop_roi_t *const roi_out)
{
  const dt_iop_contrast_data_t *const d = piece->data;
  dt_iop_contrast_gui_data_t *const g = self->gui_data;

  const float *const restrict in = (float *const)ivoid;
  float *const restrict out = (float *const)ovoid;
  float *restrict luminance_pixel = NULL;
  float *restrict luminance_smoothed_local = NULL;

  const size_t width = roi_in->width;
  const size_t height = roi_in->height;
  const size_t num_elem = width * height;

  // Get the hash of the upstream pipe to track changes
  const dt_hash_t hash = dt_dev_pixelpipe_piece_hash(piece, roi_out, TRUE);

  // Sanity checks
  if(width < 1 || height < 1) return;
  if(roi_in->width < roi_out->width || roi_in->height < roi_out->height) return;
  if(piece->colors != 4) return;

  // Init the luminance mask buffers
  gboolean cached = FALSE;

  if(self->dev->gui_attached)
  {
    // If the module instance has changed order in the pipe, invalidate caches
    if(g->pipe_order != piece->module->iop_order)
    {
      dt_iop_gui_enter_critical_section(self);
      g->ui_preview_hash = DT_INVALID_HASH;
      g->thumb_preview_hash = DT_INVALID_HASH;
      g->pipe_order = piece->module->iop_order;
      g->luminance_valid = FALSE;
      dt_iop_gui_leave_critical_section(self);
    }

    if(piece->pipe->type & DT_DEV_PIXELPIPE_FULL)
    {
      // Re-allocate buffers if size changed
      if(g->full_preview_buf_width != width || g->full_preview_buf_height != height)
      {
        dt_free_align(g->full_preview_buf_pixel);
        dt_free_align(g->full_preview_buf_smoothed_local);
        g->full_preview_buf_pixel = dt_alloc_align_float(num_elem);
        g->full_preview_buf_width = width;
        g->full_preview_buf_height = height;
      }

      luminance_pixel = g->full_preview_buf_pixel;
      luminance_smoothed_local = g->full_preview_buf_smoothed_local;
      cached = TRUE;
    }
    else if(piece->pipe->type & DT_DEV_PIXELPIPE_PREVIEW)
    {
      dt_iop_gui_enter_critical_section(self);
      if(g->thumb_preview_buf_width != width || g->thumb_preview_buf_height != height)
      {
        dt_free_align(g->thumb_preview_buf_pixel);
        dt_free_align(g->thumb_preview_buf_smoothed_local);
        g->thumb_preview_buf_pixel = dt_alloc_align_float(num_elem);
        g->thumb_preview_buf_width = width;
        g->thumb_preview_buf_height = height;
        g->luminance_valid = FALSE;
      }

      luminance_pixel = g->thumb_preview_buf_pixel;
      luminance_smoothed_local = g->thumb_preview_buf_smoothed_local;
      cached = TRUE;
      dt_iop_gui_leave_critical_section(self);
    }
    else
    {
      luminance_pixel = dt_alloc_align_float(num_elem);
      luminance_smoothed_local = dt_alloc_align_float(num_elem);
    }
  }
  else
  {
    // No interactive editing: allocate local temp buffers
    luminance_pixel = dt_alloc_align_float(num_elem);
    luminance_smoothed_local = dt_alloc_align_float(num_elem);
  }

  // Check buffer allocation
  if(!luminance_pixel || !luminance_smoothed_local)
  {
    dt_control_log(_("local contrast failed to allocate memory, check your RAM settings"));
    if(!cached) { dt_free_align(luminance_pixel); dt_free_align(luminance_smoothed_local); }
    return;
  }

  // Compute luminance masks
  // Calculate base epsilon for guided filter: higher f_mult reduces epsilon (stricter filter)
  const float base_eps = d->feathering * d->feathering;
  
  if(cached)
  {
    if(piece->pipe->type & DT_DEV_PIXELPIPE_FULL)
    {
      dt_hash_t saved_hash;
      hash_set_get(&g->ui_preview_hash, &saved_hash, &self->gui_lock);

      dt_iop_gui_enter_critical_section(self);
      const gboolean luminance_valid = g->luminance_valid;
      dt_iop_gui_leave_critical_section(self);

      if(hash != saved_hash || !luminance_valid)
      {
        compute_pixel_luminance_mask(in, luminance_pixel, width, height, d->method);
        if(d->gain_local != 1.0f || g->mask_display == DT_LC_MASK_LOCAL)
          compute_smoothed_luminance_mask(in, luminance_smoothed_local, width, height, d, d->radius_local, base_eps * fmaxf(d->f_mult_local, 0.5f));
        hash_set_get(&hash, &g->ui_preview_hash, &self->gui_lock);
      }
    }
    else if(piece->pipe->type & DT_DEV_PIXELPIPE_PREVIEW)
    {
      dt_hash_t saved_hash;
      hash_set_get(&g->thumb_preview_hash, &saved_hash, &self->gui_lock);

      dt_iop_gui_enter_critical_section(self);
      const gboolean luminance_valid = g->luminance_valid;
      dt_iop_gui_leave_critical_section(self);

      if(saved_hash != hash || !luminance_valid)
      {
        dt_iop_gui_enter_critical_section(self);
        g->thumb_preview_hash = hash;
        compute_pixel_luminance_mask(in, luminance_pixel, width, height, d->method);
        if(d->gain_local != 1.0f || g->mask_display == DT_LC_MASK_LOCAL)
          compute_smoothed_luminance_mask(in, luminance_smoothed_local, width, height, d, d->radius_local, base_eps * fmaxf(d->f_mult_local, 0.5f));
        g->luminance_valid = TRUE;
        dt_iop_gui_leave_critical_section(self);
        dt_dev_pixelpipe_cache_invalidate_later(piece->pipe, self->iop_order);
      }
    }
    else
    {
      compute_pixel_luminance_mask(in, luminance_pixel, width, height, d->method);
      compute_smoothed_luminance_mask(in, luminance_smoothed_local, width, height, d, d->radius_local, base_eps * fmaxf(1.0f, 0.5f));
    }
  }
  else
  {
    compute_pixel_luminance_mask(in, luminance_pixel, width, height, d->method);
    compute_smoothed_luminance_mask(in, luminance_smoothed_local, width, height, d, d->radius_local, base_eps * fmaxf(1.0f, 0.5f));
  }

  // Display output
  if(g && g->mask_display != DT_LC_MASK_OFF)
  {
    display_local_mask(luminance_pixel, luminance_smoothed_local, out, width, height);
    piece->pipe->mask_display = DT_DEV_PIXELPIPE_DISPLAY_PASSTHRU;
  }
  else
  {
    apply_local_contrast(in, luminance_pixel, luminance_smoothed_local, out, roi_in, roi_out, d);
  }

  if(!cached) { dt_free_align(luminance_pixel); dt_free_align(luminance_smoothed_local); }
}
*/

void init_global(dt_iop_module_so_t *self)
{
  self->data = NULL;
}

void cleanup_global(dt_iop_module_so_t *self)
{
  self->data = NULL;
}


void process(dt_iop_module_t *self,
             dt_dev_pixelpipe_iop_t *piece,
             const void *const restrict ivoid,
             void *const restrict ovoid,
             const dt_iop_roi_t *const roi_in,
             const dt_iop_roi_t *const roi_out)
{
  //spatial_contrast_process(self, piece, ivoid, ovoid, roi_in, roi_out);

  const dt_iop_contrast_data_t *const d = piece->data;
  dt_iop_contrast_gui_data_t *const g = self->gui_data;

  const float *const restrict in = (float *const)ivoid;
  float *const restrict out = (float *const)ovoid;
  float *restrict luminance_pixel = NULL;
  float *restrict luminance_smoothed_local = NULL;

  const size_t width = roi_in->width;
  const size_t height = roi_in->height;
  const size_t num_elem = width * height;

  luminance_pixel = dt_alloc_align_float(num_elem);
  luminance_smoothed_local = dt_alloc_align_float(num_elem);

  if(!luminance_pixel || !luminance_smoothed_local)
  {
    dt_control_log(_("local contrast failed to allocate memory, check your RAM settings"));
    dt_free_align(luminance_pixel);
    dt_free_align(luminance_smoothed_local);
    return;
  }

  compute_luminance_and_mask(in, luminance_pixel, luminance_smoothed_local, width, height, d);
  
  // Display output
  if(g && g->mask_display != DT_LC_MASK_OFF)
  {
    display_local_mask(luminance_pixel, luminance_smoothed_local, out, width, height);
    piece->pipe->mask_display = DT_DEV_PIXELPIPE_DISPLAY_PASSTHRU;
  }
  else
  {
    apply_local_contrast(in, luminance_pixel, luminance_smoothed_local, out, roi_in, roi_out, d);
  }

  dt_free_align(luminance_pixel);
  dt_free_align(luminance_smoothed_local);
}


void modify_roi_in(dt_iop_module_t *self,
                   dt_dev_pixelpipe_iop_t *piece,
                   const dt_iop_roi_t *roi_out,
                   dt_iop_roi_t *roi_in)
{
  dt_iop_contrast_data_t *const d = piece->data;

  // Get the scaled window radius for the box average
  const float max_size = (float)((piece->iwidth > piece->iheight) ? piece->iwidth : piece->iheight);
  const float base_diameter = d->blending * max_size * roi_in->scale;

  const float diameter_local = base_diameter;
  d->radius_local = (int)((diameter_local - 1.0f) / 2.0f);
}


void commit_params(dt_iop_module_t *self,
                   dt_iop_params_t *p1,
                   dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  const dt_iop_contrast_params_t *p = (dt_iop_contrast_params_t *)p1;
  dt_iop_contrast_data_t *d = piece->data;

  d->iterations = p->filter_iterations;
  d->gain_local_contrast = p->gain_local_contrast;
  d->noise_threshold = p->noise_threshold;

  // Normalize blending and feathering to the "sensor 3:2 36 MP" as reference.
  // diag_ref = 8848.0f is the diagonal of the "sensor 3:2 36 MP" (7360 x 4912 px).
  // N < 1 for lower resolution sensors, N > 1 for higher resolution sensors.
  const float diag     = sqrtf((float)piece->iwidth  * piece->iwidth
                             + (float)piece->iheight * piece->iheight);
  const float diag_ref = 8848.0f; // sensor 36 MP
  const float N        = diag / diag_ref;

  // UI blending param is squared to increase sensitivity to small values.
  // Scaled by sqrt(N) so larger sensors use proportionally wider radii.
  d->blending = (p->contrast_scale * p->contrast_scale / 100.0f) * sqrtf(N);

  // UI feathering is inverted (higher = stricter edge preservation).
  // Adjust the strength based on the number of iterations to maintain a consistent overall effect regardless of iteration count.
  const float default_feathering = 0.2f;  // Base value based on Christian's experiments for a good balance of edge preservation and contrast boost at default settings.
  d->feathering = default_feathering * powf(2.0, -p->edge_protection) / (p->filter_iterations * p->filter_iterations);
}


void init_pipe(dt_iop_module_t *self,
               dt_dev_pixelpipe_t *pipe,
               dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = dt_calloc1_align_type(dt_iop_contrast_data_t);
}


void cleanup_pipe(dt_iop_module_t *self,
                  dt_dev_pixelpipe_t *pipe,
                  dt_dev_pixelpipe_iop_t *piece)
{
  dt_free_align(piece->data);
  piece->data = NULL;
}


static void gui_cache_init(dt_iop_module_t *self)
{
  dt_iop_contrast_gui_data_t *g = self->gui_data;
  if(g == NULL) return;

  dt_iop_gui_enter_critical_section(self);
  g->ui_preview_hash = DT_INVALID_HASH;
  g->thumb_preview_hash = DT_INVALID_HASH;
  g->mask_display = DT_LC_MASK_OFF;
  g->luminance_valid = FALSE;

  g->full_preview_buf_pixel = NULL;
  g->full_preview_buf_smoothed_local = NULL;

  g->thumb_preview_buf_pixel = NULL;
  g->thumb_preview_buf_smoothed_local = NULL;

  g->pipe_order = 0;
  dt_iop_gui_leave_critical_section(self);
}
static void _update_mask_buttons_state(dt_iop_contrast_gui_data_t *g)
{
  if(darktable.gui->reset) return;
  ++darktable.gui->reset;

  dt_bauhaus_widget_set_quad_active(g->gain_local_contrast, g->mask_display == DT_LC_MASK_LOCAL);

  if(g->f_view_local)
     gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->f_view_local), g->mask_display == DT_LC_MASK_LOCAL);
  --darktable.gui->reset;
}

static void _set_mask_display(dt_iop_module_t *self, dt_iop_contrast_mask_t mask_type)
{
  dt_iop_contrast_gui_data_t *g = self->gui_data;

  if(darktable.gui->reset) return;

  // If blend module is displaying mask, don't display here
  if(self->request_mask_display)
  {
    dt_control_log(_("cannot display masks when the blending mask is displayed"));
    g->mask_display = DT_LC_MASK_OFF;
  }
  else
  {
    // Toggle logic
    if(g->mask_display == mask_type)
    {
      g->mask_display = DT_LC_MASK_OFF;
    }
    else
    {
      g->mask_display = mask_type;
    }
  }

  _update_mask_buttons_state(g);

  invalidate_luminance_cache(self);
}

static void show_guiding_controls(const dt_iop_module_t *self)
{
  const dt_iop_contrast_gui_data_t *g = self->gui_data;

  // All filters need these controls
  gtk_widget_set_visible(g->contrast_scale, TRUE);
  gtk_widget_set_visible(g->edge_protection, TRUE);
}


void gui_changed(dt_iop_module_t *self,
                 GtkWidget *w,
                 void *previous)
{
  const dt_iop_contrast_gui_data_t *g = self->gui_data;

  if(w == g->contrast_scale || w == g->edge_protection)
  {
    invalidate_luminance_cache(self);
  }

}

void gui_update(dt_iop_module_t *self)
{
  dt_iop_contrast_gui_data_t *g = self->gui_data;

  show_guiding_controls(self);
  invalidate_luminance_cache(self);
  _update_mask_buttons_state(g);

  gui_changed(self, NULL, NULL);
}


static void _quad_callback(GtkWidget *quad, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_contrast_gui_data_t *g = self->gui_data;
  dt_iop_contrast_mask_t mask_type = DT_LC_MASK_OFF;

  if(quad == g->gain_local_contrast) mask_type = DT_LC_MASK_LOCAL;

  if(mask_type != DT_LC_MASK_OFF)
  {
    _set_mask_display(self, mask_type);
  }
}


static void _develop_ui_pipe_started_callback(gpointer instance,
                                              dt_iop_module_t *self)
{
  dt_iop_contrast_gui_data_t *g = self->gui_data;
  if(g == NULL) return;

  if(!self->expanded || !self->enabled)
  {
    dt_iop_gui_enter_critical_section(self);
    g->mask_display = DT_LC_MASK_OFF;
    dt_iop_gui_leave_critical_section(self);
  }

  ++darktable.gui->reset;
  dt_iop_gui_enter_critical_section(self);
  _update_mask_buttons_state(g);
  dt_iop_gui_leave_critical_section(self);
  --darktable.gui->reset;
}


void gui_focus(dt_iop_module_t *self, gboolean in)
{
  dt_iop_contrast_gui_data_t *g = self->gui_data;
  if(!in)
  {
    const gboolean mask_was_shown = (g->mask_display != DT_LC_MASK_OFF);
    g->mask_display = DT_LC_MASK_OFF;

    _update_mask_buttons_state(g);
    if(mask_was_shown) dt_dev_reprocess_center(self->dev);
  }
}


void gui_reset(dt_iop_module_t *self)
{
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}


void gui_init(dt_iop_module_t *self)
{
  dt_iop_contrast_gui_data_t *g = IOP_GUI_ALLOC(contrast);

  gui_cache_init(self);

  // Main container
  GtkWidget *main_box = dt_gui_vbox();
  self->widget = main_box;
  
  // Local boost slider
  g->gain_local_contrast = dt_bauhaus_slider_from_params(self, "gain_local_contrast");
  dt_bauhaus_slider_set_soft_range(g->gain_local_contrast, 0.0, 2.0);
  dt_bauhaus_slider_set_digits(g->gain_local_contrast, 2);
  dt_bauhaus_slider_set_format(g->gain_local_contrast, "%");
  dt_bauhaus_slider_set_factor(g->gain_local_contrast, 100.0);
  dt_bauhaus_slider_set_offset(g->gain_local_contrast, -100.0);
  gtk_widget_set_tooltip_text (g->gain_local_contrast,
                              _("amount of local contrast enhancement"));
  dt_bauhaus_widget_set_quad(g->gain_local_contrast, self, dtgtk_cairo_paint_showmask, TRUE, _quad_callback,
                             _("visualize local contrast mask"));

  // filter settings
  GtkWidget *filter_label = dt_ui_section_label_new(C_("section", "filter settings"));
  dt_gui_box_add(main_box, filter_label);
  
  g->contrast_scale = dt_bauhaus_slider_from_params(self, "contrast_scale");
  dt_bauhaus_slider_set_soft_range(g->contrast_scale, 1.0, 4.0);
  gtk_widget_set_tooltip_text
    (g->contrast_scale,
     _("adjusts the scale of details targeted by the sliders in the “spatial contrast” section.\n"
       "higher values target larger features, lower values target finer details."));

  g->edge_protection = dt_bauhaus_slider_from_params(self, "edge_protection");
  dt_bauhaus_slider_set_soft_range(g->edge_protection, -2.0, 2.0);
  dt_bauhaus_slider_set_digits(g->edge_protection, 2);
  dt_bauhaus_slider_set_format(g->edge_protection, "%");
  dt_bauhaus_slider_set_factor(g->edge_protection, 100.0);
  gtk_widget_set_tooltip_text(g->edge_protection, _("adjust the edge sensitivity of the filter\n"
                                                    "higher = more edge preservation\n"
                                                    "lower = smoother transitions, but may lead to halos around edges"));

  g->filter_iterations = dt_bauhaus_slider_from_params(self, "filter_iterations");
  dt_bauhaus_slider_set_soft_range(g->filter_iterations, 1, 5);
  gtk_widget_set_tooltip_text(g->filter_iterations, _("number of passes of the guided filter to apply\n"
       "helps diffusing the edges of the filter at the expense of speed"));

  g->noise_threshold = dt_bauhaus_slider_from_params(self, "noise_threshold");
  dt_bauhaus_slider_set_soft_range(g->noise_threshold, 0.0, 0.01);
  dt_bauhaus_slider_set_hard_min(g->noise_threshold, 0.0);
  dt_bauhaus_slider_set_hard_max(g->noise_threshold, 0.01);
  dt_bauhaus_slider_set_digits(g->noise_threshold, 4);
  dt_bauhaus_slider_set_step(g->noise_threshold, 0.0001);
  gtk_widget_set_tooltip_text(g->noise_threshold, _("noise protection. Only affects dark parts of the image."));

  // Restore main widget
  self->widget = main_box;

  // Connect signals for pipe events
  DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_DEVELOP_HISTORY_CHANGE, _develop_ui_pipe_started_callback);
}


void gui_cleanup(dt_iop_module_t *self)
{
  dt_iop_contrast_gui_data_t *g = self->gui_data;

  dt_free_align(g->thumb_preview_buf_pixel);
  dt_free_align(g->thumb_preview_buf_smoothed_local);
  dt_free_align(g->full_preview_buf_pixel);
  dt_free_align(g->full_preview_buf_smoothed_local);
}


// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on