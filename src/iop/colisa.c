/*
  This file is part of darktable,
  copyright (c) 2013 ulrich pegelow.

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
#include "common/debug.h"
#include "common/opencl.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "develop/tiling.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "iop/iop_api.h"
#include "common/iop_group.h"
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <gtk/gtk.h>
#include <inttypes.h>

DT_MODULE_INTROSPECTION(1, dt_iop_colisa_params_t)

typedef struct dt_iop_colisa_params_t
{
  float contrast;
  float brightness;
  float saturation;
} dt_iop_colisa_params_t;

typedef struct dt_iop_colisa_gui_data_t
{
  GtkWidget *contrast;
  GtkWidget *brightness;
  GtkWidget *saturation;
} dt_iop_colisa_gui_data_t;

typedef struct dt_iop_colisa_data_t
{
  float contrast;
  float brightness;
  float saturation;
  float ctable[0x10000];      // precomputed look-up table for contrast curve
  float cunbounded_coeffs[3]; // approximation for extrapolation of contrast curve
  float ltable[0x10000];      // precomputed look-up table for brightness curve
  float lunbounded_coeffs[3]; // approximation for extrapolation of brightness curve
} dt_iop_colisa_data_t;

typedef struct dt_iop_colisa_global_data_t
{
  int kernel_colisa;
} dt_iop_colisa_global_data_t;


const char *name()
{
  return _("contrast brightness saturation");
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING;
}

int groups()
{
  return dt_iop_get_group("contrast brightness saturation", IOP_GROUP_BASIC);
}


void init_key_accels(dt_iop_module_so_t *self)
{
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "contrast"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "brightness"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "saturation"));
}

void connect_key_accels(dt_iop_module_t *self)
{
  dt_iop_colisa_gui_data_t *g = (dt_iop_colisa_gui_data_t *)self->gui_data;

  dt_accel_connect_slider_iop(self, "contrast", GTK_WIDGET(g->contrast));
  dt_accel_connect_slider_iop(self, "brightness", GTK_WIDGET(g->brightness));
  dt_accel_connect_slider_iop(self, "saturation", GTK_WIDGET(g->saturation));
}


#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_colisa_data_t *d = (dt_iop_colisa_data_t *)piece->data;
  dt_iop_colisa_global_data_t *gd = (dt_iop_colisa_global_data_t *)self->data;

  cl_int err = -999;
  const int devid = piece->pipe->devid;

  const int width = roi_in->width;
  const int height = roi_in->height;

  const float saturation = d->saturation;

  cl_mem dev_cm = NULL;
  cl_mem dev_ccoeffs = NULL;
  cl_mem dev_lm = NULL;
  cl_mem dev_lcoeffs = NULL;

  dev_cm = dt_opencl_copy_host_to_device(devid, d->ctable, 256, 256, sizeof(float));
  if(dev_cm == NULL) goto error;
  dev_ccoeffs = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * 3, d->cunbounded_coeffs);
  if(dev_ccoeffs == NULL) goto error;
  dev_lm = dt_opencl_copy_host_to_device(devid, d->ltable, 256, 256, sizeof(float));
  if(dev_lm == NULL) goto error;
  dev_lcoeffs = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * 3, d->lunbounded_coeffs);
  if(dev_lcoeffs == NULL) goto error;

  size_t sizes[3];
  sizes[0] = ROUNDUPWD(width);
  sizes[1] = ROUNDUPWD(height);
  sizes[2] = 1;
  dt_opencl_set_kernel_arg(devid, gd->kernel_colisa, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_colisa, 1, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(devid, gd->kernel_colisa, 2, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_colisa, 3, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_colisa, 4, sizeof(float), (void *)&saturation);
  dt_opencl_set_kernel_arg(devid, gd->kernel_colisa, 5, sizeof(cl_mem), (void *)&dev_cm);
  dt_opencl_set_kernel_arg(devid, gd->kernel_colisa, 6, sizeof(cl_mem), (void *)&dev_ccoeffs);
  dt_opencl_set_kernel_arg(devid, gd->kernel_colisa, 7, sizeof(cl_mem), (void *)&dev_lm);
  dt_opencl_set_kernel_arg(devid, gd->kernel_colisa, 8, sizeof(cl_mem), (void *)&dev_lcoeffs);

  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_colisa, sizes);
  if(err != CL_SUCCESS) goto error;

  dt_opencl_release_mem_object(dev_lcoeffs);
  dt_opencl_release_mem_object(dev_lm);
  dt_opencl_release_mem_object(dev_ccoeffs);
  dt_opencl_release_mem_object(dev_cm);
  return TRUE;

error:
  dt_opencl_release_mem_object(dev_lcoeffs);
  dt_opencl_release_mem_object(dev_lm);
  dt_opencl_release_mem_object(dev_ccoeffs);
  dt_opencl_release_mem_object(dev_cm);
  dt_print(DT_DEBUG_OPENCL, "[opencl_colisa] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_colisa_data_t *data = (dt_iop_colisa_data_t *)piece->data;
  float *in = (float *)ivoid;
  float *out = (float *)ovoid;

  const int width = roi_in->width;
  const int height = roi_in->height;
  const int ch = piece->colors;

#ifdef _OPENMP
#pragma omp parallel for default(none) shared(in, out, data) schedule(static)
#endif
  for(size_t k = 0; k < (size_t)width * height; k++)
  {
    float L = (in[k * ch + 0] < 100.0f)
                  ? data->ctable[CLAMP((int)(in[k * ch + 0] / 100.0f * 0x10000ul), 0, 0xffff)]
                  : dt_iop_eval_exp(data->cunbounded_coeffs, in[k * ch + 0] / 100.0f);
    out[k * ch + 0] = (L < 100.0f) ? data->ltable[CLAMP((int)(L / 100.0f * 0x10000ul), 0, 0xffff)]
                                   : dt_iop_eval_exp(data->lunbounded_coeffs, L / 100.0f);
    out[k * ch + 1] = in[k * ch + 1] * data->saturation;
    out[k * ch + 2] = in[k * ch + 2] * data->saturation;
    out[k * ch + 3] = in[k * ch + 3];
  }
}


static void contrast_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_colisa_params_t *p = (dt_iop_colisa_params_t *)self->params;
  p->contrast = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void brightness_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_colisa_params_t *p = (dt_iop_colisa_params_t *)self->params;
  p->brightness = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void saturation_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_colisa_params_t *p = (dt_iop_colisa_params_t *)self->params;
  p->saturation = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}


void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_colisa_params_t *p = (dt_iop_colisa_params_t *)p1;
  dt_iop_colisa_data_t *d = (dt_iop_colisa_data_t *)piece->data;

  d->contrast = p->contrast + 1.0f; // rescale from [-1;+1] to [0;+2] (zero meaning no contrast -> gray plane)
  d->brightness = p->brightness * 2.0f; // rescale from [-1;+1] to [-2;+2]
  d->saturation = p->saturation + 1.0f; // rescale from [-1;+1] to [0;+2] (zero meaning no saturation -> b&w)

  // generate precomputed contrast curve
  if(d->contrast <= 1.0f)
  {
// linear curve for d->contrast below 1
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(d) schedule(static)
#endif
    for(int k = 0; k < 0x10000; k++) d->ctable[k] = d->contrast * (100.0f * k / 0x10000 - 50.0f) + 50.0f;
  }
  else
  {
    // sigmoidal curve for d->contrast above 1
    const float boost = 20.0f;
    const float contrastm1sq = boost * (d->contrast - 1.0f) * (d->contrast - 1.0f);
    const float contrastscale = sqrt(1.0f + contrastm1sq);
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(d) schedule(static)
#endif
    for(int k = 0; k < 0x10000; k++)
    {
      float kx2m1 = 2.0f * (float)k / 0x10000 - 1.0f;
      d->ctable[k] = 50.0f * (contrastscale * kx2m1 / sqrtf(1.0f + contrastm1sq * kx2m1 * kx2m1) + 1.0f);
    }
  }

  // now the extrapolation stuff for the contrast curve:
  const float xc[4] = { 0.7f, 0.8f, 0.9f, 1.0f };
  const float yc[4] = { d->ctable[CLAMP((int)(xc[0] * 0x10000ul), 0, 0xffff)],
                        d->ctable[CLAMP((int)(xc[1] * 0x10000ul), 0, 0xffff)],
                        d->ctable[CLAMP((int)(xc[2] * 0x10000ul), 0, 0xffff)],
                        d->ctable[CLAMP((int)(xc[3] * 0x10000ul), 0, 0xffff)] };
  dt_iop_estimate_exp(xc, yc, 4, d->cunbounded_coeffs);


  // generate precomputed brightness curve
  const float gamma = (d->brightness >= 0.0f) ? 1.0f / (1.0f + d->brightness) : (1.0f - d->brightness);

#ifdef _OPENMP
#pragma omp parallel for default(none) shared(d) schedule(static)
#endif
  for(int k = 0; k < 0x10000; k++)
  {
    d->ltable[k] = 100.0f * powf((float)k / 0x10000, gamma);
  }

  // now the extrapolation stuff for the brightness curve:
  const float xl[4] = { 0.7f, 0.8f, 0.9f, 1.0f };
  const float yl[4] = { d->ltable[CLAMP((int)(xl[0] * 0x10000ul), 0, 0xffff)],
                        d->ltable[CLAMP((int)(xl[1] * 0x10000ul), 0, 0xffff)],
                        d->ltable[CLAMP((int)(xl[2] * 0x10000ul), 0, 0xffff)],
                        d->ltable[CLAMP((int)(xl[3] * 0x10000ul), 0, 0xffff)] };
  dt_iop_estimate_exp(xl, yl, 4, d->lunbounded_coeffs);
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_colisa_data_t *d = (dt_iop_colisa_data_t *)calloc(1, sizeof(dt_iop_colisa_data_t));
  piece->data = (void *)d;
  self->commit_params(self, self->default_params, pipe, piece);
  for(int k = 0; k < 0x10000; k++) d->ctable[k] = d->ltable[k] = 100.0f * k / 0x10000; // identity
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_module_t *module = (dt_iop_module_t *)self;
  dt_iop_colisa_gui_data_t *g = (dt_iop_colisa_gui_data_t *)self->gui_data;
  dt_iop_colisa_params_t *p = (dt_iop_colisa_params_t *)module->params;
  dt_bauhaus_slider_set(g->contrast, p->contrast);
  dt_bauhaus_slider_set(g->brightness, p->brightness);
  dt_bauhaus_slider_set(g->saturation, p->saturation);
}

void init(dt_iop_module_t *module)
{
  module->params = calloc(1, sizeof(dt_iop_colisa_params_t));
  module->default_params = calloc(1, sizeof(dt_iop_colisa_params_t));
  module->default_enabled = 0;
  module->priority = 657; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_colisa_params_t);
  module->gui_data = NULL;
  dt_iop_colisa_params_t tmp = (dt_iop_colisa_params_t){ 0, 0, 0 };
  memcpy(module->params, &tmp, sizeof(dt_iop_colisa_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_colisa_params_t));
}

void init_global(dt_iop_module_so_t *module)
{
  const int program = 2; // basic.cl, from programs.conf
  dt_iop_colisa_global_data_t *gd
      = (dt_iop_colisa_global_data_t *)malloc(sizeof(dt_iop_colisa_global_data_t));
  module->data = gd;
  gd->kernel_colisa = dt_opencl_create_kernel(program, "colisa");
}


void cleanup(dt_iop_module_t *module)
{
  free(module->params);
  module->params = NULL;
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_colisa_global_data_t *gd = (dt_iop_colisa_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_colisa);
  free(module->data);
  module->data = NULL;
}


void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_colisa_gui_data_t));
  dt_iop_colisa_gui_data_t *g = (dt_iop_colisa_gui_data_t *)self->gui_data;
  dt_iop_colisa_params_t *p = (dt_iop_colisa_params_t *)self->params;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);
  dt_gui_add_help_link(self->widget, dt_get_help_url(self->op));

  g->contrast = dt_bauhaus_slider_new_with_range(self, -1.0, 1.0, 0.01, p->contrast, 2);
  g->brightness = dt_bauhaus_slider_new_with_range(self, -1.0, 1.0, 0.01, p->brightness, 2);
  g->saturation = dt_bauhaus_slider_new_with_range(self, -1.0, 1.0, 0.01, p->saturation, 2);

  dt_bauhaus_widget_set_label(g->contrast, NULL, _("contrast"));
  dt_bauhaus_widget_set_label(g->brightness, NULL, _("brightness"));
  dt_bauhaus_widget_set_label(g->saturation, NULL, _("saturation"));

  gtk_box_pack_start(GTK_BOX(self->widget), g->contrast, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->brightness, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->saturation, TRUE, TRUE, 0);

  gtk_widget_set_tooltip_text(g->contrast, _("contrast adjustment"));
  gtk_widget_set_tooltip_text(g->brightness, _("brightness adjustment"));
  gtk_widget_set_tooltip_text(g->saturation, _("color saturation adjustment"));

  g_signal_connect(G_OBJECT(g->contrast), "value-changed", G_CALLBACK(contrast_callback), self);
  g_signal_connect(G_OBJECT(g->brightness), "value-changed", G_CALLBACK(brightness_callback), self);
  g_signal_connect(G_OBJECT(g->saturation), "value-changed", G_CALLBACK(saturation_callback), self);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
