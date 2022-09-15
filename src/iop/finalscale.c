/*
    This file is part of darktable,
    Copyright (C) 2015-2021 darktable developers.

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
#include "common/interpolation.h"
#include "common/opencl.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "develop/tiling.h"
#include "iop/iop_api.h"

DT_MODULE_INTROSPECTION(1, dt_iop_finalscale_params_t)

typedef struct dt_iop_finalscale_params_t
{
  int dummy;
} dt_iop_finalscale_params_t;

typedef dt_iop_finalscale_params_t dt_iop_finalscale_data_t;

const char *name()
{
  return C_("modulename", "scale into final size");
}

int flags()
{
  return IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_HIDDEN | IOP_FLAGS_TILING_FULL_ROI | IOP_FLAGS_ONE_INSTANCE | IOP_FLAGS_NO_HISTORY_STACK;
}

int default_group()
{
  return IOP_GROUP_BASIC | IOP_GROUP_TECHNICAL;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

void modify_roi_in(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const dt_iop_roi_t *const roi_out,
                   dt_iop_roi_t *roi_in)
{
  *roi_in = *roi_out;

  roi_in->x /= roi_out->scale;
  roi_in->y /= roi_out->scale;
  // out = in * scale + .5f to more precisely round to user input in export module:
  roi_in->width  = (roi_out->width  - .5f)/roi_out->scale;
  roi_in->height = (roi_out->height - .5f)/roi_out->scale;
  roi_in->scale = 1.0f;
}

void distort_mask(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, const float *const in,
                  float *const out, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const struct dt_interpolation *itor = dt_interpolation_new(DT_INTERPOLATION_USERPREF_WARP);
  dt_interpolation_resample_roi_1c(itor, out, roi_out, roi_out->width * sizeof(float), in, roi_in,
                                   roi_in->width * sizeof(float));
}

#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  if(roi_out->scale >= 1.00001f)
  {
    dt_print(DT_DEBUG_OPENCL,
             "[opencl_finalscale] finalscale with upscaling not yet supported by opencl code\n");
    return FALSE;
  }

  const int devid = piece->pipe->devid;
  cl_int err = -999;

  err = dt_iop_clip_and_zoom_roi_cl(devid, dev_out, dev_in, roi_out, roi_in);
  if(err != CL_SUCCESS) goto error;

  return TRUE;

error:
  dt_print(DT_DEBUG_OPENCL, "[opencl_finalscale] couldn't enqueue kernel! %s\n", cl_errstr(err));
  return FALSE;
}
#endif

void process(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid, void *const ovoid,
             const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_clip_and_zoom_roi(ovoid, ivoid, roi_out, roi_in, roi_out->width, roi_in->width);
}

void commit_params(dt_iop_module_t *self, dt_iop_params_t *params, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  if(piece->pipe->type != DT_DEV_PIXELPIPE_EXPORT) piece->enabled = 0;
}

void init_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1, sizeof(dt_iop_finalscale_data_t));
}

void cleanup_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void init(dt_iop_module_t *self)
{
  self->params = calloc(1, sizeof(dt_iop_finalscale_params_t));
  self->default_params = calloc(1, sizeof(dt_iop_finalscale_params_t));
  self->default_enabled = 1;
  self->hide_enable_button = 1;
  self->params_size = sizeof(dt_iop_finalscale_params_t);
  self->gui_data = NULL;
}

void cleanup(dt_iop_module_t *self)
{
  free(self->params);
  self->params = NULL;
  free(self->default_params);
  self->default_params = NULL;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

