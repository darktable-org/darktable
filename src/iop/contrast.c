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

DT_MODULE_INTROSPECTION(1, dt_iop_contrast_params_t)

typedef struct dt_iop_contrast_params_t
{
  float gain_local_contrast;  // $MIN: 0.0 $MAX: 5.0 $DEFAULT: 1.0  $DESCRIPTION: "local contrast"
  float detail_level;         // $MIN: 0.0 $MAX: 15.0 $DEFAULT: 4.0 $DESCRIPTION: "detail level"
  float edge_protection;      // $MIN: -10.0 $MAX: 10.0 $DEFAULT: 0.0 $DESCRIPTION: "adjust edge protection"
  int filter_iterations;      // $MIN: 1 $MAX: 20 $DEFAULT: 1 $DESCRIPTION: "filter iterations"  
  float noise_bias;           // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.001 $DESCRIPTION: "noise bias"
} dt_iop_contrast_params_t;

typedef enum dt_iop_contrast_mask_t
{
  DT_LC_MASK_OFF = -1,
  DT_LC_MASK_LOCAL = 0,
  DT_LC_MASK_LAST = 1
} dt_iop_contrast_mask_t;

typedef struct dt_iop_contrast_data_t
{
  float gain_local_contrast;
  float contrast_scale;
  float feathering;
  int radius_local;
  int iterations;
  float noise_bias;
} dt_iop_contrast_data_t;

typedef struct dt_iop_contrast_gui_data_t
{
  // Flags
  dt_iop_contrast_mask_t mask_display;

  // GTK widgets
  GtkWidget *gain_local_contrast;
  GtkWidget *detail_level;
  GtkWidget *edge_protection;
  GtkWidget *filter_iterations;
  GtkWidget *noise_bias;
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


void init_global(dt_iop_module_so_t *self)
{
  self->data = NULL;
}

void cleanup_global(dt_iop_module_so_t *self)
{
  self->data = NULL;
}


// Compute smoothed luminance mask using edge-aware filters
__DT_CLONE_TARGETS__
static inline void compute_luminance_and_mask(const float *const restrict in,
                                              float *const restrict luminance,
                                              float *const restrict smoothed_luminance,
                                              const dt_iop_roi_t *const roi_in,
                                              const dt_iop_contrast_data_t *const d)
{
  size_t width = (size_t)roi_in->width;
  size_t height = (size_t)roi_in->height;
  const size_t npixels = width * height;

  // First compute pixel-wise luminance (no boost) and add noise bias
  luminance_mask(in, luminance, width, height, DT_TONEEQ_NORM_2, 1.0f, 0.0f, 1.0f);

  DT_OMP_FOR()
  for(size_t k = 0; k < npixels; k++)
  { 
    luminance[k] += d->noise_bias;
  }

  // Then apply the smoothing filter on a copy
  memcpy(smoothed_luminance, luminance, npixels * sizeof(float));

  fast_eigf_surface_blur(smoothed_luminance, width, height,
                         d->radius_local, d->feathering, d->iterations,
                         DT_GF_BLENDING_LINEAR, 1.0f,
                         0.0f, NORM_MIN, 4.0f);
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
                                        const dt_iop_contrast_data_t *const d)
{
  const size_t npixels = (size_t)roi_in->width * roi_in->height;
  const float gain_local = d->gain_local_contrast;
  
  DT_OMP_FOR()
  for(size_t k = 0; k < npixels; k++)
  {
    const float log_pixel = log2f(fmaxf(luminance_pixel[k], NORM_MIN));
    const float log_smoothed = log2f(fmaxf(luminance_smoothed[k], NORM_MIN));

    // High pass detail in log space (EV):
    // How much brighter/darker is this pixel compared to the smooth version
    const float local_ev = fmaxf(fminf(log_pixel - log_smoothed, 5.0f), -5.0f);

    // Correction as the scaled ev difference
    float correction_ev = (gain_local - 1.0f) * local_ev;
    
    // Apply correction in linear space
    const float multiplier = exp2f(correction_ev);;
    for_each_channel(c)
      out[4 * k + c] = in[4 * k + c] * multiplier;
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
                                      const dt_iop_roi_t *const roi_in)
{
  const size_t npixels = (size_t)roi_in->width * roi_in->height;

  DT_OMP_FOR()
  for(size_t k = 0; k < npixels; k++)
  {
    const float lum_pixel = fmaxf(luminance_pixel[k], NORM_MIN);
    const float lum_smoothed = fmaxf(luminance_smoothed[k], NORM_MIN);

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


void process(dt_iop_module_t *self,
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

  luminance_pixel = dt_alloc_align_float(num_elem);
  luminance_smoothed_local = dt_alloc_align_float(num_elem);

  if(!luminance_pixel || !luminance_smoothed_local)
  {
    dt_control_log(_("local contrast failed to allocate memory, check your RAM settings"));
    dt_free_align(luminance_pixel);
    dt_free_align(luminance_smoothed_local);
    return;
  }

  compute_luminance_and_mask(in, luminance_pixel, luminance_smoothed_local, roi_in, d);
  
  // Display output
  if(g && g->mask_display != DT_LC_MASK_OFF && (piece->pipe->type & DT_DEV_PIXELPIPE_FULL))
  {
    display_local_mask(luminance_pixel, luminance_smoothed_local, out, roi_in);
    piece->pipe->mask_display = DT_DEV_PIXELPIPE_DISPLAY_PASSTHRU;
  }
  else
  {
    apply_local_contrast(in, luminance_pixel, luminance_smoothed_local, out, roi_in, d);
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
  const float base_diameter = d->contrast_scale * max_size * roi_in->scale;

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
  d->noise_bias = p->noise_bias;

  // UI contrast scale is inverse logarithmic with 0 as 100% of image width.
  // Convert it to a linear scale for processing.
  d->contrast_scale = powf(2.0f, -p->detail_level);

  // UI feathering is inverted (higher = stricter edge preservation).
  // Adjust the strength based on the number of iterations to maintain a consistent overall effect regardless of iteration count.
  const float default_feathering = 0.2f;  // Base value based on Christian's experiments for a good balance of edge preservation and contrast boost at default settings.
  d->feathering = default_feathering * powf(2.0f, -p->edge_protection) / (p->filter_iterations * p->filter_iterations);
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


static void _update_mask_buttons_state(dt_iop_contrast_gui_data_t *g)
{
  if(darktable.gui->reset) return;
  ++darktable.gui->reset;

  if(g->gain_local_contrast)
    dt_bauhaus_widget_set_quad_active(g->gain_local_contrast, g->mask_display == DT_LC_MASK_LOCAL);

  --darktable.gui->reset;
}


static void _set_mask_display(dt_iop_module_t *self, dt_iop_contrast_mask_t mask_type)
{
  dt_iop_contrast_gui_data_t *g = self->gui_data;

  if(darktable.gui->reset) return;

  const dt_iop_contrast_mask_t old_mask_display = g->mask_display;

  if(self->request_mask_display)
  {
    dt_control_log(_("cannot display masks when the blending mask is displayed"));
    g->mask_display = DT_LC_MASK_OFF;
  }
  else
  {
    g->mask_display = (g->mask_display == mask_type) ? DT_LC_MASK_OFF : mask_type;
  }

  if(g->mask_display != old_mask_display)
  {
    _update_mask_buttons_state(g);
    if(self->dev) dt_dev_reprocess_center(self->dev);
  }
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


void gui_update(dt_iop_module_t *self)
{
  dt_iop_contrast_gui_data_t *g = self->gui_data;
  _update_mask_buttons_state(g);
}

void gui_focus(dt_iop_module_t *self, gboolean in)
{
  dt_iop_contrast_gui_data_t *g = self->gui_data;
  if(!in)
  {
    // Lost focus, reset mask display to off
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
  g->mask_display = DT_LC_MASK_OFF;
    
  // Main container
  self->widget = dt_gui_vbox();

  // Local boost slider
  g->gain_local_contrast = dt_bauhaus_slider_from_params(self, "gain_local_contrast");
  dt_bauhaus_slider_set_soft_range(g->gain_local_contrast, 0.0, 2.0);
  dt_bauhaus_slider_set_digits(g->gain_local_contrast, 2);
  dt_bauhaus_slider_set_format(g->gain_local_contrast, "%");
  dt_bauhaus_slider_set_factor(g->gain_local_contrast, 100.0);
  dt_bauhaus_slider_set_offset(g->gain_local_contrast, -100.0);
  gtk_widget_set_tooltip_text(g->gain_local_contrast,
                              _("amount of local contrast enhancement"));
  dt_bauhaus_widget_set_quad(g->gain_local_contrast, self, dtgtk_cairo_paint_showmask, TRUE, _quad_callback,
                             _("visualize local contrast mask"));

  // Filter settings section
  GtkWidget *filter_label = dt_ui_section_label_new(C_("section", "filter settings"));
  dt_gui_box_add(self->widget, filter_label);
  
  g->detail_level = dt_bauhaus_slider_from_params(self, "detail_level");
  dt_bauhaus_slider_set_soft_range(g->detail_level, 2.0, 10.0);
  gtk_widget_set_tooltip_text(g->detail_level,
     _("detail level adjusted by the local contrast.\n"
       "higher = more contrast boost in finer details\n"
       "lower = more contrast boost in coarser details"));
  

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

  g->noise_bias = dt_bauhaus_slider_from_params(self, "noise_bias");
  dt_bauhaus_slider_set_soft_range(g->noise_bias, 0.0, 0.2);
  dt_bauhaus_slider_set_digits(g->noise_bias, 4);
  dt_bauhaus_slider_set_step(g->noise_bias, 0.0001);
  gtk_widget_set_tooltip_text(g->noise_bias, _("add bias to reduce shadow noise amplification.\n"
                                               "Only affects dark parts of the image."));
}


// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on