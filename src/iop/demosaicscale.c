/*
    This file is part of darktable,
    Copyright (C) 2015-2026 darktable developers.

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
#include "common/interpolation.h"
#include "common/opencl.h"
#include "common/imagebuf.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "develop/tiling.h"
#include "iop/iop_api.h"

DT_MODULE_INTROSPECTION(1, dt_iop_demosaicscale_params_t)

typedef struct dt_iop_demosaicscale_params_t
{
  int dummy;
} dt_iop_demosaicscale_params_t;

typedef dt_iop_demosaicscale_params_t dt_iop_demosaicscale_data_t;

const char *name()
{
  return C_("modulename", "crop and scale demosaicer");
}

int flags()
{
  return IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_HIDDEN
    | IOP_FLAGS_ONE_INSTANCE | IOP_FLAGS_NO_HISTORY_STACK
    | IOP_FLAGS_WRITE_PIPECACHE_IN;
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

void modify_roi_out(dt_iop_module_t *self,
                    dt_dev_pixelpipe_iop_t *piece,
                    dt_iop_roi_t *roi_out,
                    const dt_iop_roi_t *const roi_in)
{
  *roi_out = *roi_in;
  roi_out->x = 0;
  roi_out->y = 0;
}

void modify_roi_in(dt_iop_module_t *self,
                   dt_dev_pixelpipe_iop_t *piece,
                   const dt_iop_roi_t *roi_out,
                   dt_iop_roi_t *roi_in)
{
  *roi_in = *roi_out;
  roi_in->x = 0;
  roi_in->y = 0;

  roi_in->width = piece->buf_in.width;
  roi_in->height = piece->buf_in.height;
  roi_in->scale = 1.0f;
}

void tiling_callback(dt_iop_module_t *self,
                     dt_dev_pixelpipe_iop_t *piece,
                     const dt_iop_roi_t *roi_in,
                     const dt_iop_roi_t *roi_out,
                     struct dt_develop_tiling_t *tiling)
{
  const float ioratio
      = (float)(roi_out->width * roi_out->height) / (float)(roi_in->width * roi_in->height);

  tiling->factor = 1.0f + ioratio + 0.5f;
  tiling->factor_cl = tiling->factor;
  tiling->maxbuf = 1.0f;
  tiling->maxbuf_cl = tiling->maxbuf;
  tiling->overhead = 0;

  tiling->overlap = 4;
  tiling->align = 1;
}

void distort_mask(dt_iop_module_t *self,
                  dt_dev_pixelpipe_iop_t *piece,
                  const float *const in,
                  float *const out,
                  const dt_iop_roi_t *const roi_in,
                  const dt_iop_roi_t *const roi_out)
{
  const dt_interpolation_t *itor = dt_interpolation_new(DT_INTERPOLATION_USERPREF);
  dt_interpolation_resample_mask(itor, out, roi_out, in, roi_in);
}

#ifdef HAVE_OPENCL
int process_cl(dt_iop_module_t *self,
               dt_dev_pixelpipe_iop_t *piece,
               cl_mem dev_in,
               cl_mem dev_out,
               const dt_iop_roi_t *const roi_in,
               const dt_iop_roi_t *const roi_out)
{
  return dt_iop_clip_and_zoom_cl(piece->pipe->devid, dev_out, dev_in, roi_out, roi_in);
}
#endif

void process(dt_iop_module_t *self,
             dt_dev_pixelpipe_iop_t *piece,
             const void *const ivoid,
             void *const ovoid,
             const dt_iop_roi_t *const roi_in,
             const dt_iop_roi_t *const roi_out)
{
  dt_iop_clip_and_zoom((float *)ovoid, (float *)ivoid, roi_out, roi_in, FALSE);
}

void commit_params(dt_iop_module_t *self,
                   dt_iop_params_t *params,
                   dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  const dt_image_t *img = &pipe->image;
  const gboolean demosaiced = dt_image_is_mono_sraw(img) || dt_image_is_raw(img);
  if(!demosaiced)
    piece->enabled = FALSE;
}

void init_pipe(dt_iop_module_t *self,
               dt_dev_pixelpipe_t *pipe,
               dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1, sizeof(dt_iop_demosaicscale_data_t));
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
  self->params = calloc(1, sizeof(dt_iop_demosaicscale_params_t));
  self->default_params = calloc(1, sizeof(dt_iop_demosaicscale_params_t));
  self->default_enabled = TRUE;
  self->hide_enable_button = TRUE;
  self->params_size = sizeof(dt_iop_demosaicscale_params_t);
  self->gui_data = NULL;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
