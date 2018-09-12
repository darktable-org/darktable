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
#include "bauhaus/bauhaus.h"
#include "common/darktable.h"
#include "common/opencl.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop_math.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

DT_MODULE_INTROSPECTION(2, dt_iop_profilegamma_params_t)

typedef struct dt_iop_profilegamma_params_t
{
  float offset;
  float power;
  float slope;
} dt_iop_profilegamma_params_t;

typedef struct dt_iop_profilegamma_gui_data_t
{
  GtkWidget *slope;
  GtkWidget *offset;
  GtkWidget *power;
} dt_iop_profilegamma_gui_data_t;

typedef struct dt_iop_profilegamma_data_t
{
  float offset;
  float power;
  float slope;
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
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "slope"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "offset"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "power"));
}

void connect_key_accels(dt_iop_module_t *self)
{
  dt_iop_profilegamma_gui_data_t *g = (dt_iop_profilegamma_gui_data_t *)self->gui_data;
  dt_accel_connect_slider_iop(self, "slope", GTK_WIDGET(g->slope));
  dt_accel_connect_slider_iop(self, "offset", GTK_WIDGET(g->offset));
  dt_accel_connect_slider_iop(self, "power", GTK_WIDGET(g->power));
}

#ifdef HAVE_OPENCL
int process_cl(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_profilegamma_data_t *d = (dt_iop_profilegamma_data_t *)piece->data;
  dt_iop_profilegamma_global_data_t *gd = (dt_iop_profilegamma_global_data_t *)self->data;

  cl_int err = -999;
  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;
  
  size_t sizes[] = { ROUNDUPWD(width) , ROUNDUPWD(height), 1 };
  dt_opencl_set_kernel_arg(devid, gd->kernel_profilegamma, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_profilegamma, 1, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(devid, gd->kernel_profilegamma, 2, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_profilegamma, 3, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_profilegamma, 4, sizeof(float), (void *)&(d->slope));
  dt_opencl_set_kernel_arg(devid, gd->kernel_profilegamma, 5, sizeof(float), (void *)&(d->offset));
  dt_opencl_set_kernel_arg(devid, gd->kernel_profilegamma, 6, sizeof(float), (void *)&(d->power));
    
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_profilegamma, sizes);
  if(err != CL_SUCCESS) goto error;

  return TRUE;

error:
  dt_print(DT_DEBUG_OPENCL, "[opencl_profilegamma] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif

void process(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid, void *const ovoid,
             const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_profilegamma_data_t *data = (dt_iop_profilegamma_data_t *)piece->data;

  const int ch = piece->colors;

#ifdef _OPENMP
#pragma omp parallel for default(none) shared(data) schedule(static)
#endif
      for(size_t k = 1; k < (size_t)ch * roi_out->width * roi_out->height; k++)
      {
        // https://en.wikipedia.org/wiki/ASC_CDL
        float pixel = ((float *)ivoid)[k] * data->slope + data->offset;
        
        // ensure we dont take apply power < 1 on negative values
        if (pixel >= 0.)
        {
          ((float *)ovoid)[k] = powf(pixel, data->power);
        }
        else
        {
          ((float *)ovoid)[k] = pixel * fabsf(data->offset);
        }
      }
 
  if(piece->pipe->mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK) dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);
  
}

static void power_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_profilegamma_params_t *p = (dt_iop_profilegamma_params_t *)self->params;
  p->power = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void offset_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_profilegamma_params_t *p = (dt_iop_profilegamma_params_t *)self->params;
  p->offset = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void slope_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_profilegamma_params_t *p = (dt_iop_profilegamma_params_t *)self->params;
  p->slope = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}


void commit_params(dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_profilegamma_params_t *p = (dt_iop_profilegamma_params_t *)p1;
  dt_iop_profilegamma_data_t *d = (dt_iop_profilegamma_data_t *)piece->data;
  
  d->offset = p->offset;
  d->slope = p->slope;
  d->power = p->power;
  
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
  dt_bauhaus_slider_set(g->offset, p->offset);
  dt_bauhaus_slider_set(g->slope, p->slope);
  dt_bauhaus_slider_set(g->power, p->power);
}

void init(dt_iop_module_t *module)
{
  module->params = calloc(1, sizeof(dt_iop_profilegamma_params_t));
  module->default_params = calloc(1, sizeof(dt_iop_profilegamma_params_t));
  module->default_enabled = 0;
  module->priority = 323; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_profilegamma_params_t);
  module->gui_data = NULL;
  dt_iop_profilegamma_params_t tmp = (dt_iop_profilegamma_params_t){ 0., 1., 1.};
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
  g->offset = dt_bauhaus_slider_new_with_range(self, -0.002, 0.002, 0.0001, p->offset, 4);
  dt_bauhaus_slider_enable_soft_boundaries(g->offset, -1., 1.);
  g->slope = dt_bauhaus_slider_new_with_range(self, 0.001, 2.0, 0.005, p->slope, 3);
  dt_bauhaus_slider_enable_soft_boundaries(g->slope, 0., 4.0);
  g->power = dt_bauhaus_slider_new_with_range(self, 0.0, 2.0, 0.005, p->power, 3);
  dt_bauhaus_slider_enable_soft_boundaries(g->power, 0., 2.0);

  dt_bauhaus_widget_set_label(g->offset, NULL, _("offset"));
  dt_bauhaus_widget_set_label(g->slope, NULL, _("slope"));
  dt_bauhaus_widget_set_label(g->power, NULL, _("power"));

  gtk_box_pack_start(GTK_BOX(self->widget), g->offset, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->power, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->slope, TRUE, TRUE, 0);

  gtk_widget_set_tooltip_text(g->offset, _("constant offset, for dark tones"));
  gtk_widget_set_tooltip_text(g->slope, _("proportionnal slope, for highlights"));
  gtk_widget_set_tooltip_text(g->power, _("gamma power, for midtones"));

  g_signal_connect(G_OBJECT(g->offset), "value-changed", G_CALLBACK(offset_callback), self);
  g_signal_connect(G_OBJECT(g->slope), "value-changed", G_CALLBACK(slope_callback), self);
  g_signal_connect(G_OBJECT(g->power), "value-changed", G_CALLBACK(power_callback), self);
  
}

void gui_cleanup(dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
