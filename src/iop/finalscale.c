/*
    This file is part of darktable,
    Copyright (C) 2015-2024 darktable developers.

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
#include "common/imagebuf.h"
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
  return IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_HIDDEN | IOP_FLAGS_TILING_FULL_ROI
    | IOP_FLAGS_ONE_INSTANCE | IOP_FLAGS_NO_HISTORY_STACK;
}

int default_group()
{
  return IOP_GROUP_BASIC | IOP_GROUP_TECHNICAL;
}

dt_iop_colorspace_type_t default_colorspace(dt_iop_module_t *self,
                                            dt_dev_pixelpipe_t *pipe,
                                            dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

static inline gboolean _gui_fullpipe(dt_dev_pixelpipe_iop_t *piece)
{
  return piece->pipe->type & (DT_DEV_PIXELPIPE_FULL
                              | DT_DEV_PIXELPIPE_PREVIEW2
                              | DT_DEV_PIXELPIPE_IMAGE)
    && darktable.develop->late_scaling.enabled;
}

void modify_roi_in(dt_iop_module_t *self,
                   dt_dev_pixelpipe_iop_t *piece,
                   const dt_iop_roi_t *const roi_out,
                   dt_iop_roi_t *roi_in)
{
  *roi_in = *roi_out;

  roi_in->x /= roi_out->scale;
  roi_in->y /= roi_out->scale;
/*
  Keep <= v4.2 code here as reference

  That lead to rounded-down width&height so if in case of a scale of 1
  both would be one less than roi_out dimensions. This is bad because
  we have to fight the missing data by adopting either scale or size
  in dt_imageio_export_with_flags() leading to either reduced size or
  some slight upscale of the output image.

  // out = in * scale + .5f to more precisely round to user input in
  // export module:

  roi_in->width  = (roi_out->width  - .5f)/roi_out->scale;
  roi_in->height = (roi_out->height - .5f)/roi_out->scale;
*/

  // always avoid
  // - expanding roi_in dimensions to more than what is provided
  // - processing micro-sizes
  roi_in->width  = MAX(16, MIN(ceilf(roi_out->width / roi_out->scale),
                               piece->buf_in.width));
  roi_in->height = MAX(16, MIN(ceilf(roi_out->height / roi_out->scale),
                               piece->buf_in.height));

  // FIXME As long as we don't support upscaling via OpenCL we can & should disable OpenCL
  // here to avoid the costly later fallback to CPU upscaling
  if(roi_in->scale > 1.0f)
    piece->process_cl_ready = FALSE;

  roi_in->scale = 1.0f;

  if(_gui_fullpipe(piece))
  {
    roi_in->x = 0;
    roi_in->y = 0;
    roi_in->width = piece->buf_in.width;
    roi_in->height = piece->buf_in.height;
  }
}

void tiling_callback(dt_iop_module_t *self,
                     dt_dev_pixelpipe_iop_t *piece,
                     const dt_iop_roi_t *roi_in,
                     const dt_iop_roi_t *roi_out,
                     struct dt_develop_tiling_t *tiling)
{
  const float ioratio
      = (float)(roi_out->width * roi_out->height) / (float)(roi_in->width * roi_in->height);

  tiling->factor = 1.0f + ioratio;
  tiling->factor += ioratio != 1.0f ? 0.5f : 0.0f; // approximate extra requirements for interpolation
  tiling->factor_cl = tiling->factor;
  tiling->maxbuf = 1.0f;
  tiling->maxbuf_cl = tiling->maxbuf;
  tiling->overhead = 0;

  tiling->overlap = 4;
  tiling->xalign = 1;
  tiling->yalign = 1;
}

void distort_mask(dt_iop_module_t *self,
                  dt_dev_pixelpipe_iop_t *piece,
                  const float *const in,
                  float *const out,
                  const dt_iop_roi_t *const roi_in,
                  const dt_iop_roi_t *const roi_out)
{
  const struct dt_interpolation *itor =
    dt_interpolation_new(DT_INTERPOLATION_USERPREF);
  dt_interpolation_resample_1c(itor, out, roi_out, in, roi_in);
}

#ifdef HAVE_OPENCL
int process_cl(dt_iop_module_t *self,
               dt_dev_pixelpipe_iop_t *piece,
               cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in,
               const dt_iop_roi_t *const roi_out)
{
  if(roi_out->scale > 1.0f) // trust cl code for 1:1 copy here or downscale
  {
    dt_print(DT_DEBUG_OPENCL,
             "[opencl_finalscale] upscaling not yet supported by opencl code");
    return DT_OPENCL_PROCESS_CL;
  }

  const int devid = piece->pipe->devid;
  const gboolean exporting = piece->pipe->type == DT_DEV_PIXELPIPE_EXPORT;

  dt_print_pipe(DT_DEBUG_IMAGEIO,
                exporting ? "clip_and_zoom_roi" : "clip_and_zoom",
                piece->pipe, self, piece->pipe->devid, roi_in, roi_out, "device=%i", devid);
  if(exporting)
    return dt_iop_clip_and_zoom_roi_cl(devid, dev_out, dev_in, roi_out, roi_in);
  else
    return dt_iop_clip_and_zoom_cl(devid, dev_out, dev_in, roi_out, roi_in);
}
#endif

void process(dt_iop_module_t *self,
             dt_dev_pixelpipe_iop_t *piece,
             const void *const ivoid,
             void *const ovoid,
             const dt_iop_roi_t *const roi_in,
             const dt_iop_roi_t *const roi_out)
{
  const gboolean exporting = piece->pipe->type == DT_DEV_PIXELPIPE_EXPORT;
  dt_print_pipe(DT_DEBUG_IMAGEIO,
                exporting ? "clip_and_zoom_roi" : "clip_and_zoom",
                piece->pipe, self, DT_DEVICE_CPU, roi_in, roi_out);

  if(exporting)
    dt_iop_clip_and_zoom_roi((float *)ovoid, (float *)ivoid, roi_out, roi_in);
  else
    dt_iop_clip_and_zoom((float *)ovoid, (float *)ivoid, roi_out, roi_in);
}

void commit_params(dt_iop_module_t *self,
                   dt_iop_params_t *params,
                   dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  const int use_finalscale = DT_DEV_PIXELPIPE_IMAGE | DT_DEV_PIXELPIPE_IMAGE_FINAL;
  piece->enabled = piece->pipe->type == DT_DEV_PIXELPIPE_EXPORT
                  || (pipe->type & use_finalscale) == use_finalscale
                  || _gui_fullpipe(piece);
}

void init_pipe(dt_iop_module_t *self,
               dt_dev_pixelpipe_t *pipe,
               dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1, sizeof(dt_iop_finalscale_data_t));
}

void cleanup_pipe(dt_iop_module_t *self,
                  dt_dev_pixelpipe_t *pipe,
                  dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void init(dt_iop_module_t *self)
{
  self->params = calloc(1, sizeof(dt_iop_finalscale_params_t));
  self->default_params = calloc(1, sizeof(dt_iop_finalscale_params_t));
  self->default_enabled = TRUE;
  self->hide_enable_button = TRUE;
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
