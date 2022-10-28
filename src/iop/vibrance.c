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
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"
#include <gtk/gtk.h>
#include <inttypes.h>

DT_MODULE_INTROSPECTION(2, dt_iop_vibrance_params_t)

typedef struct dt_iop_vibrance_params_t
{
  float amount; // $MIN: 0.0 $MAX: 100.0 $DEFAULT: 25.0 $DESCRIPTION: "vibrance"
} dt_iop_vibrance_params_t;

typedef struct dt_iop_vibrance_gui_data_t
{
  GtkWidget *amount_scale;
} dt_iop_vibrance_gui_data_t;

typedef struct dt_iop_vibrance_data_t
{
  float amount;
} dt_iop_vibrance_data_t;

typedef struct dt_iop_vibrance_global_data_t
{
  int kernel_vibrance;
} dt_iop_vibrance_global_data_t;

const char *deprecated_msg()
{
  return _("this module is deprecated. please use the vibrance slider in the color balance rgb module instead.");
}

const char *name()
{
  return _("vibrance");
}

const char *aliases()
{
  return _("saturation");
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING
    | IOP_FLAGS_DEPRECATED;
}

int default_group()
{
  return IOP_GROUP_COLOR | IOP_GROUP_GRADING;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_LAB;
}

const char **description(struct dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("saturate and reduce the lightness of the most saturated pixels\n"
                                        "to make the colors more vivid."),
                                      _("creative"),
                                      _("linear or non-linear, Lab, display-referred"),
                                      _("non-linear, Lab"),
                                      _("non-linear, Lab, display-referred"));
}

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  if(!dt_iop_have_required_input_format(4 /*we need full-color pixels*/, self, piece->colors,
                                         ivoid, ovoid, roi_in, roi_out))
    return; // image has been copied through to output and module's trouble flag has been updated

  const dt_iop_vibrance_data_t *const d = (dt_iop_vibrance_data_t *)piece->data;
  const float *const restrict in = (float *)ivoid;
  float *const restrict out = (float *)ovoid;

  const float amount = (d->amount * 0.01);
  const int npixels = roi_out->height * roi_out->width;

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(amount, npixels) \
  dt_omp_sharedconst(in, out) \
  schedule(static)
#endif
  for(int k = 0; k < 4 * npixels; k += 4)
  {
    /* saturation weight 0 - 1 */
    const float sw = sqrtf((in[k + 1] * in[k + 1]) + (in[k + 2] * in[k + 2])) / 256.0f;
    const float ls = 1.0f - ((amount * sw) * .25f);
    const float ss = 1.0f + (amount * sw);
    const dt_aligned_pixel_t weights = { ls, ss, ss, 1.0f };
#ifdef _OPENMP
#pragma omp simd aligned(in, out : 16)
#endif
    for(int c = 0; c < 4; c++)
    {
      out[k + c] = in[k + c] * weights[c];
    }
  }
}


#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_vibrance_data_t *data = (dt_iop_vibrance_data_t *)piece->data;
  dt_iop_vibrance_global_data_t *gd = (dt_iop_vibrance_global_data_t *)self->global_data;
  cl_int err = DT_OPENCL_DEFAULT_ERROR;

  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;

  const float amount = data->amount * 0.01f;

  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_vibrance, width, height,
    CLARG(dev_in), CLARG(dev_out), CLARG(width), CLARG(height), CLARG(amount));
  if(err != CL_SUCCESS) goto error;

  return TRUE;

error:
  dt_print(DT_DEBUG_OPENCL, "[opencl_vibrance] couldn't enqueue kernel! %s\n", cl_errstr(err));
  return FALSE;
}
#endif



void init_global(dt_iop_module_so_t *module)
{
  const int program = 8; // extended.cl, from programs.conf
  dt_iop_vibrance_global_data_t *gd
      = (dt_iop_vibrance_global_data_t *)malloc(sizeof(dt_iop_vibrance_global_data_t));
  module->data = gd;
  gd->kernel_vibrance = dt_opencl_create_kernel(program, "vibrance");
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_vibrance_global_data_t *gd = (dt_iop_vibrance_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_vibrance);
  free(module->data);
  module->data = NULL;
}


void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_vibrance_params_t *p = (dt_iop_vibrance_params_t *)p1;
  dt_iop_vibrance_data_t *d = (dt_iop_vibrance_data_t *)piece->data;
  d->amount = p->amount;
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1, sizeof(dt_iop_vibrance_data_t));
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_vibrance_gui_data_t *g = (dt_iop_vibrance_gui_data_t *)self->gui_data;
  dt_iop_vibrance_params_t *p = (dt_iop_vibrance_params_t *)self->params;
  dt_bauhaus_slider_set(g->amount_scale, p->amount);
}

void gui_init(struct dt_iop_module_t *self)
{
  dt_iop_vibrance_gui_data_t *g = IOP_GUI_ALLOC(vibrance);

  g->amount_scale = dt_bauhaus_slider_from_params(self, "amount");
  dt_bauhaus_slider_set_format(g->amount_scale, "%");
  gtk_widget_set_tooltip_text(g->amount_scale, _("the amount of vibrance"));
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

