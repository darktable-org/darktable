/*
    This file is part of darktable,
    Copyright (C) 2011-2021 darktable developers.

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
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "bauhaus/bauhaus.h"
#include "common/opencl.h"
#include "control/control.h"
#include "develop/imageop_gui.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"
#include <gtk/gtk.h>
#include <inttypes.h>

DT_MODULE_INTROSPECTION(1, dt_iop_vibrancergb_params_t)

typedef struct dt_iop_vibrancergb_params_t
{
  float amount;   // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0
} dt_iop_vibrancergb_params_t;

typedef struct dt_iop_vibrancergb_gui_data_t
{
  GtkWidget *amount_scale;
} dt_iop_vibrancergb_gui_data_t;

typedef struct dt_iop_vibrancergb_data_t
{
  float amount;
} dt_iop_vibrancergb_data_t;

typedef struct dt_iop_vibrancergb_global_data_t
{
  int kernel_vibrancergb;
} dt_iop_vibrancergb_global_data_t;


const char *name()
{
  return _("vibrance rgb");
}

const char *aliases()
{
  return _("saturation");
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING;
}

int default_group()
{
  return IOP_GROUP_COLOR | IOP_GROUP_GRADING;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return iop_cs_rgb;
}

const char *description(struct dt_iop_module_t *self)
{
  return dt_iop_set_description
    (self,
     _("saturate and reduce the lightness of the most saturated pixels\n"
       "to make the colors more vivid."),
     _("creative"),
     _("linear, RGB, scene-referred"),
     _("linear, RGB"),
     _("linear, RGB, scene-referred"));
}

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  if (!dt_iop_have_required_input_format(4 /*we need full-color pixels*/, self, piece->colors,
                                         ivoid, ovoid, roi_in, roi_out))
    return; // image has been copied through to output and module's trouble flag has been updated

  const dt_iop_vibrancergb_data_t *const d = (dt_iop_vibrancergb_data_t *)piece->data;
  const float *const restrict in = (float *)ivoid;
  float *const restrict out = (float *)ovoid;

  const size_t npixels = roi_out->height * roi_out->width;
  const float vibrance = d->amount / 1.4f;

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(vibrance, npixels) \
  dt_omp_sharedconst(in, out) \
  schedule(static)
#endif
  for(size_t k = 0; k < 4 * npixels; k += 4)
  {
    const float average = (in[k] + in[k + 1] + in[k + 2]) / 3.0f;
    const float delta = sqrtf((average - in[k]) * (average - in[k])
                               + (average - in[k + 1]) * (average - in[k + 1])
                               + (average - in[k + 2]) * (average - in[k + 2]));
    const float P = vibrance * (1.0f - powf(delta, fabsf(vibrance)));

    for(size_t c = 0; c < 3; c++)
    {
      out[k + c] = average + (1.0f + P) * (in[k + c] - average);
    }

    out[k + 3] = in[k + 3];
  }
}


#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_vibrancergb_data_t *data = (dt_iop_vibrancergb_data_t *)piece->data;
  dt_iop_vibrancergb_global_data_t *gd = (dt_iop_vibrancergb_global_data_t *)self->global_data;
  cl_int err = -999;

  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;

  const float vibrance = data->amount / 1.4f;

  size_t sizes[] = { ROUNDUPWD(width), ROUNDUPHT(height), 1 };

  dt_opencl_set_kernel_arg(devid, gd->kernel_vibrancergb, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_vibrancergb, 1, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(devid, gd->kernel_vibrancergb, 2, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_vibrancergb, 3, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_vibrancergb, 4, sizeof(float), (void *)&vibrance);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_vibrancergb, sizes);
  if(err != CL_SUCCESS) goto error;

  return TRUE;

error:
  dt_print(DT_DEBUG_OPENCL, "[opencl_vibrancergb] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif



void init_global(dt_iop_module_so_t *module)
{
  const int program = 8; // extended.cl, from programs.conf
  dt_iop_vibrancergb_global_data_t *gd
      = (dt_iop_vibrancergb_global_data_t *)malloc(sizeof(dt_iop_vibrancergb_global_data_t));
  module->data = gd;
  gd->kernel_vibrancergb = dt_opencl_create_kernel(program, "vibrancergb");
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_vibrancergb_global_data_t *gd = (dt_iop_vibrancergb_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_vibrancergb);
  free(module->data);
  module->data = NULL;
}


void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_vibrancergb_params_t *p = (dt_iop_vibrancergb_params_t *)p1;
  dt_iop_vibrancergb_data_t *d = (dt_iop_vibrancergb_data_t *)piece->data;
  d->amount = p->amount;
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1, sizeof(dt_iop_vibrancergb_data_t));
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_vibrancergb_gui_data_t *g = (dt_iop_vibrancergb_gui_data_t *)self->gui_data;
  dt_iop_vibrancergb_params_t *p = (dt_iop_vibrancergb_params_t *)self->params;
  dt_bauhaus_slider_set(g->amount_scale, p->amount);
}

void gui_init(struct dt_iop_module_t *self)
{
  dt_iop_vibrancergb_gui_data_t *g = IOP_GUI_ALLOC(vibrancergb);

  g->amount_scale = dt_bauhaus_slider_from_params(self, "amount");
  gtk_widget_set_tooltip_text(g->amount_scale, _("the amount of vibrance"));
}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
