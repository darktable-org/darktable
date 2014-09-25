/*
    This file is part of darktable,
    copyright (c) 2009--2010 johannes hanika.
    copyright (c) 2014 LebedevRI.

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
#include <stdlib.h>
#include <math.h>
#include <assert.h>
#include <string.h>
#include "develop/develop.h"
#include "control/control.h"
#include "bauhaus/bauhaus.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"

DT_MODULE_INTROSPECTION(1, dt_iop_profilegamma_params_t)

typedef struct dt_iop_profilegamma_params_t
{
  float linear;
  float gamma;
} dt_iop_profilegamma_params_t;

typedef struct dt_iop_profilegamma_gui_data_t
{
  GtkWidget *linear;
  GtkWidget *gamma;
} dt_iop_profilegamma_gui_data_t;

typedef struct dt_iop_profilegamma_data_t
{
  float linear;
  float gamma;
  float table[0x10000];      // precomputed look-up table
  float unbounded_coeffs[3]; // approximation for extrapolation of curve
} dt_iop_profilegamma_data_t;

typedef struct dt_iop_profilegamma_global_data_t
{
  int kernel_profilegamma;
} dt_iop_profilegamma_global_data_t;

const char *name()
{
  return _("unbreak input profile");
}

int groups()
{
  return IOP_GROUP_COLOR;
}

int flags()
{
  return IOP_FLAGS_ONE_INSTANCE | IOP_FLAGS_ALLOW_TILING;
}

void init_key_accels(dt_iop_module_so_t *self)
{
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "linear"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "gamma"));
}

void connect_key_accels(dt_iop_module_t *self)
{
  dt_iop_profilegamma_gui_data_t *g = (dt_iop_profilegamma_gui_data_t *)self->gui_data;

  dt_accel_connect_slider_iop(self, "linear", GTK_WIDGET(g->linear));
  dt_accel_connect_slider_iop(self, "gamma", GTK_WIDGET(g->gamma));
}

#ifdef HAVE_OPENCL
int process_cl(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_profilegamma_data_t *d = (dt_iop_profilegamma_data_t *)piece->data;
  dt_iop_profilegamma_global_data_t *gd = (dt_iop_profilegamma_global_data_t *)self->data;

  cl_int err = -999;
  const int devid = piece->pipe->devid;

  const int width = roi_in->width;
  const int height = roi_in->height;

  cl_mem dev_table = NULL;
  cl_mem dev_coeffs = NULL;

  dev_table = dt_opencl_copy_host_to_device(devid, d->table, 256, 256, sizeof(float));
  if(dev_table == NULL) goto error;

  dev_coeffs = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * 3, d->unbounded_coeffs);
  if(dev_coeffs == NULL) goto error;

  size_t sizes[3];
  sizes[0] = ROUNDUPWD(width);
  sizes[1] = ROUNDUPWD(height);
  sizes[2] = 1;
  dt_opencl_set_kernel_arg(devid, gd->kernel_profilegamma, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_profilegamma, 1, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(devid, gd->kernel_profilegamma, 2, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_profilegamma, 3, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_profilegamma, 4, sizeof(cl_mem), (void *)&dev_table);
  dt_opencl_set_kernel_arg(devid, gd->kernel_profilegamma, 5, sizeof(cl_mem), (void *)&dev_coeffs);

  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_profilegamma, sizes);
  if(err != CL_SUCCESS) goto error;

  if(dev_table != NULL) dt_opencl_release_mem_object(dev_table);
  if(dev_coeffs != NULL) dt_opencl_release_mem_object(dev_coeffs);
  return TRUE;

error:
  if(dev_table != NULL) dt_opencl_release_mem_object(dev_table);
  if(dev_coeffs != NULL) dt_opencl_release_mem_object(dev_coeffs);
  dt_print(DT_DEBUG_OPENCL, "[opencl_profilegamma] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif

void process(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *ivoid, void *ovoid,
             const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_profilegamma_data_t *data = (dt_iop_profilegamma_data_t *)piece->data;

  const int ch = piece->colors;

#ifdef _OPENMP
#pragma omp parallel for default(none) shared(roi_out, ivoid, ovoid, data) schedule(static)
#endif
  for(int k = 0; k < roi_out->height; k++)
  {
    const float *in = ((float *)ivoid) + (size_t)ch * k * roi_out->width;
    float *out = ((float *)ovoid) + (size_t)ch * k * roi_out->width;

    for(int j = 0; j < roi_out->width; j++, in += ch, out += ch)
    {
      for(int i = 0; i < 3; i++)
      {
        // use base curve for values < 1, else use extrapolation.
        if(in[i] < 1.0f)
          out[i] = data->table[CLAMP((int)(in[i] * 0x10000ul), 0, 0xffff)];
        else
          out[i] = dt_iop_eval_exp(data->unbounded_coeffs, in[i]);
      }
    }
  }

  if(piece->pipe->mask_display) dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);
}

static void linear_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_profilegamma_params_t *p = (dt_iop_profilegamma_params_t *)self->params;
  p->linear = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void gamma_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_profilegamma_params_t *p = (dt_iop_profilegamma_params_t *)self->params;
  p->gamma = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

void commit_params(dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_profilegamma_params_t *p = (dt_iop_profilegamma_params_t *)p1;
  dt_iop_profilegamma_data_t *d = (dt_iop_profilegamma_data_t *)piece->data;

  const float linear = p->linear;
  const float gamma = p->gamma;

  d->linear = p->linear;
  d->gamma = p->gamma;

  float a, b, c, g;
  if(gamma == 1.0)
  {
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(d) schedule(static)
#endif
    for(int k = 0; k < 0x10000; k++) d->table[k] = 1.0 * k / 0x10000;
  }
  else
  {
    if(linear == 0.0)
    {
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(d) schedule(static)
#endif
      for(int k = 0; k < 0x10000; k++) d->table[k] = powf(1.00 * k / 0x10000, gamma);
    }
    else
    {
      if(linear < 1.0)
      {
        g = gamma * (1.0 - linear) / (1.0 - gamma * linear);
        a = 1.0 / (1.0 + linear * (g - 1));
        b = linear * (g - 1) * a;
        c = powf(a * linear + b, g) / linear;
      }
      else
      {
        a = b = g = 0.0;
        c = 1.0;
      }
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(d, a, b, c, g) schedule(static)
#endif
      for(int k = 0; k < 0x10000; k++)
      {
        float tmp;
        if(k < 0x10000 * linear)
          tmp = c * k / 0x10000;
        else
          tmp = powf(a * k / 0x10000 + b, g);
        d->table[k] = tmp;
      }
    }
  }

  // now the extrapolation stuff:
  const float x[4] = { 0.7f, 0.8f, 0.9f, 1.0f };
  const float y[4] = { d->table[CLAMP((int)(x[0] * 0x10000ul), 0, 0xffff)],
                       d->table[CLAMP((int)(x[1] * 0x10000ul), 0, 0xffff)],
                       d->table[CLAMP((int)(x[2] * 0x10000ul), 0, 0xffff)],
                       d->table[CLAMP((int)(x[3] * 0x10000ul), 0, 0xffff)] };
  dt_iop_estimate_exp(x, y, 4, d->unbounded_coeffs);
}

void init_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1, sizeof(dt_iop_profilegamma_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void gui_update(dt_iop_module_t *self)
{
  dt_iop_module_t *module = (dt_iop_module_t *)self;
  dt_iop_profilegamma_gui_data_t *g = (dt_iop_profilegamma_gui_data_t *)self->gui_data;
  dt_iop_profilegamma_params_t *p = (dt_iop_profilegamma_params_t *)module->params;
  dt_bauhaus_slider_set(g->linear, p->linear);
  dt_bauhaus_slider_set(g->gamma, p->gamma);
}

void init(dt_iop_module_t *module)
{
  module->params = malloc(sizeof(dt_iop_profilegamma_params_t));
  module->default_params = malloc(sizeof(dt_iop_profilegamma_params_t));
  module->default_enabled = 0;
  module->priority = 333; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_profilegamma_params_t);
  module->gui_data = NULL;
  dt_iop_profilegamma_params_t tmp = (dt_iop_profilegamma_params_t){ 0.1, 0.45 };
  memcpy(module->params, &tmp, sizeof(dt_iop_profilegamma_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_profilegamma_params_t));
}

void init_global(dt_iop_module_so_t *module)
{
  const int program = 2; // basic.cl, from programs.conf
  dt_iop_profilegamma_global_data_t *gd
      = (dt_iop_profilegamma_global_data_t *)malloc(sizeof(dt_iop_profilegamma_global_data_t));
  module->data = gd;
  gd->kernel_profilegamma = dt_opencl_create_kernel(program, "profilegamma");
}

void cleanup(dt_iop_module_t *module)
{
  free(module->gui_data);
  module->gui_data = NULL;
  free(module->params);
  module->params = NULL;
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_profilegamma_global_data_t *gd = (dt_iop_profilegamma_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_profilegamma);
  free(module->data);
  module->data = NULL;
}
void gui_init(dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_profilegamma_gui_data_t));
  dt_iop_profilegamma_gui_data_t *g = (dt_iop_profilegamma_gui_data_t *)self->gui_data;
  dt_iop_profilegamma_params_t *p = (dt_iop_profilegamma_params_t *)self->params;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  g->linear = dt_bauhaus_slider_new_with_range(self, 0.0, 1.0, 0.0001, p->linear, 4);
  g->gamma = dt_bauhaus_slider_new_with_range(self, 0.0, 1.0, 0.0001, p->gamma, 4);

  dt_bauhaus_widget_set_label(g->linear, NULL, _("linear"));
  dt_bauhaus_widget_set_label(g->gamma, NULL, _("gamma"));

  gtk_box_pack_start(GTK_BOX(self->widget), g->linear, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->gamma, TRUE, TRUE, 0);

  g_object_set(g->linear, "tooltip-text", _("linear part"), (char *)NULL);
  g_object_set(g->gamma, "tooltip-text", _("gamma exponential factor"), (char *)NULL);

  g_signal_connect(G_OBJECT(g->linear), "value-changed", G_CALLBACK(linear_callback), self);
  g_signal_connect(G_OBJECT(g->gamma), "value-changed", G_CALLBACK(gamma_callback), self);
}

void gui_cleanup(dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
