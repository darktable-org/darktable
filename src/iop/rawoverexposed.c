/*
   This file is part of darktable,
   copyright (c) 2016 Roman Lebedev.

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

#include "bauhaus/bauhaus.h"      // for dt_bauhaus_slider_get, dt_bauhaus_...
#include "common/darktable.h"     // for darktable_t, dt_print, DT_MODULE_I...
#include "common/image.h"         // for ::DT_IMAGE_RAW, dt_image_t
#include "common/opencl.h"        // for dt_opencl_set_kernel_arg, dt_openc...
#include "develop/develop.h"      // for dt_dev_add_history_item
#include "develop/imageop.h"      // for dt_iop_module_t, dt_iop_roi_t, dt_...
#include "develop/imageop_math.h" // for dt_iop_alpha_copy
#include "develop/pixelpipe.h"    // for dt_dev_pixelpipe_type_t::DT_DEV_PI...
#include "gui/gtk.h"              // for dt_gui_gtk_t
#include "iop/iop_api.h"          // for dt_iop_params_t
#include <glib-object.h>          // for g_type_check_instance_cast, GCallback
#include <glib.h>                 // for TRUE, FALSE
#include <glib/gi18n.h>           // for _
#include <gtk/gtk.h>              // for GtkWidget, gtk_box_get_type, gtk_b...
#include <math.h>                 // for fminf
#include <stdlib.h>               // for free, NULL, size_t, calloc, malloc
#include <string.h>               // for memcpy

DT_MODULE_INTROSPECTION(1, dt_iop_rawoverexposed_params_t)

typedef struct dt_iop_rawoverexposed_params_t
{
  float threshold;
} dt_iop_rawoverexposed_params_t;

typedef struct dt_iop_rawoverexposed_gui_data_t
{
  GtkWidget *threshold;
} dt_iop_rawoverexposed_gui_data_t;

typedef dt_iop_rawoverexposed_params_t dt_iop_rawoverexposed_data_t;

typedef struct dt_iop_rawoverexposed_global_data_t
{
  int kernel_rawoverexposed_1f_mark;
  int kernel_rawoverexposed_4f_mark;
} dt_iop_rawoverexposed_global_data_t;

const char *name()
{
  return _("raw overexposed");
}

int groups()
{
  return IOP_GROUP_BASIC;
}

int flags()
{
  return IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_ONE_INSTANCE;
}

#ifdef HAVE_OPENCL
int process_cl(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_rawoverexposed_data_t *d = piece->data;
  dt_iop_rawoverexposed_global_data_t *gd = self->data;

  cl_int err = -999;
  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;

  size_t sizes[] = { ROUNDUPWD(width), ROUNDUPHT(height), 1 };
  const float threshold
      = d->threshold * fminf(piece->pipe->dsc.processed_maximum[0],
                             fminf(piece->pipe->dsc.processed_maximum[1], piece->pipe->dsc.processed_maximum[2]));

  const int kernel = (!dt_dev_pixelpipe_uses_downsampled_input(piece->pipe) && piece->pipe->dsc.filters)
                         ? gd->kernel_rawoverexposed_1f_mark
                         : gd->kernel_rawoverexposed_4f_mark;

  dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(float), (void *)&threshold);
  err = dt_opencl_enqueue_kernel_2d(devid, kernel, sizes);
  if(err != CL_SUCCESS) goto error;

  return TRUE;

error:
  dt_print(DT_DEBUG_OPENCL, "[opencl_rawoverexposed] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif

void process(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid, void *const ovoid,
             const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_rawoverexposed_data_t *data = piece->data;

  const float threshold
      = data->threshold * fminf(piece->pipe->dsc.processed_maximum[0],
                                fminf(piece->pipe->dsc.processed_maximum[1], piece->pipe->dsc.processed_maximum[2]));

  const float *const in = (const float *const)ivoid;
  float *const out = (float *const)ovoid;

  if(!dt_dev_pixelpipe_uses_downsampled_input(piece->pipe) && piece->pipe->dsc.filters)
  { // raw mosaic
#ifdef _OPENMP
#pragma omp parallel for SIMD() default(none) schedule(static)
#endif
    for(size_t k = 0; k < (size_t)roi_out->width * roi_out->height; k++)
    {
      out[k] = (in[k] >= threshold) ? 0.0 : in[k];
    }
  }
  else
  {
    const int ch = piece->colors;

#ifdef _OPENMP
#pragma omp parallel for SIMD() default(none) schedule(static)
#endif
    for(size_t k = 0; k < (size_t)ch * roi_out->width * roi_out->height; k++)
    {
      out[k] = (in[k] >= threshold) ? 0.0 : in[k];
    }
  }

  if(piece->pipe->mask_display) dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);
}

void commit_params(dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_rawoverexposed_params_t *p = p1;
  dt_iop_rawoverexposed_data_t *d = piece->data;

  *d = *p;

  if(pipe->type != DT_DEV_PIXELPIPE_FULL || /*!self->dev->overexposed.enabled ||*/ !self->dev->gui_attached)
    piece->enabled = 0;
}

void init_global(dt_iop_module_so_t *module)
{
  const int program = 2; // basic.cl, from programs.conf
  module->data = malloc(sizeof(dt_iop_rawoverexposed_global_data_t));
  dt_iop_rawoverexposed_global_data_t *gd = module->data;
  gd->kernel_rawoverexposed_1f_mark = dt_opencl_create_kernel(program, "rawoverexposed_1f_mark");
  gd->kernel_rawoverexposed_4f_mark = dt_opencl_create_kernel(program, "rawoverexposed_4f_mark");
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_rawoverexposed_global_data_t *gd = module->data;
  dt_opencl_free_kernel(gd->kernel_rawoverexposed_4f_mark);
  dt_opencl_free_kernel(gd->kernel_rawoverexposed_1f_mark);
  free(module->data);
  module->data = NULL;
}

void init_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_rawoverexposed_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void reload_defaults(dt_iop_module_t *module)
{
  dt_iop_rawoverexposed_params_t tmp = (dt_iop_rawoverexposed_params_t){.threshold = 1.0 };

  memcpy(module->params, &tmp, sizeof(dt_iop_rawoverexposed_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_rawoverexposed_params_t));
}

void init(dt_iop_module_t *module)
{
  module->params = calloc(1, sizeof(dt_iop_rawoverexposed_params_t));
  module->default_params = calloc(1, sizeof(dt_iop_rawoverexposed_params_t));
  module->priority = 60; // module order created by iop_dependencies.py, do not edit!
  module->default_enabled = 0;
  module->params_size = sizeof(dt_iop_rawoverexposed_params_t);
  module->gui_data = NULL;
}

void cleanup(dt_iop_module_t *module)
{
  free(module->params);
  module->params = NULL;
}

void gui_update(dt_iop_module_t *self)
{
  dt_iop_rawoverexposed_gui_data_t *g = self->gui_data;
  dt_iop_rawoverexposed_params_t *p = self->params;
  dt_bauhaus_slider_set(g->threshold, p->threshold);
}

static void threshold_callback(GtkWidget *slider, dt_iop_module_t *self)
{
  if(self->dt->gui->reset) return;

  dt_iop_rawoverexposed_params_t *p = self->params;
  p->threshold = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

void gui_init(dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_rawoverexposed_gui_data_t));

  dt_iop_rawoverexposed_gui_data_t *g = self->gui_data;
  dt_iop_rawoverexposed_params_t *p = self->params;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  g->threshold = dt_bauhaus_slider_new_with_range(self, 0.0, 2.0, 0.01, p->threshold, 3);
  gtk_widget_set_tooltip_text(g->threshold, _("manually adjust the clipping threshold against "
                                              "magenta highlights (you shouldn't ever need to touch this)"));
  dt_bauhaus_widget_set_label(g->threshold, NULL, _("clipping threshold"));
  g_signal_connect(g->threshold, "value-changed", G_CALLBACK(threshold_callback), self);

  gtk_box_pack_start(GTK_BOX(self->widget), g->threshold, TRUE, TRUE, 0);
}

void gui_cleanup(dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
