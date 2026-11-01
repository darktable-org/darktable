/*
    This file is part of darktable,
    Copyright (C) 2015-2025 darktable developers.

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

static inline gboolean _gui_fullpipe(dt_dev_pixelpipe_iop_t *piece)
{
  return (dt_pipe_is_canvas(piece->pipe) || dt_pipe_is_image(piece->pipe))
     && darktable.develop->late_scaling.enabled;
}

static float _tiling_factor(const dt_iop_roi_t *const roi_in,
                            const dt_iop_roi_t *const roi_out)
{
  const float ioratio
      = (float)(roi_out->width * roi_out->height) / (float)(roi_in->width * roi_in->height);

  // approximate extra requirements for interpolation
  return 1.0f + ioratio + (ioratio != 1.0f ? 0.5f : 0.0f);
}

#ifdef HAVE_OPENCL
// a tiled upscale reads every output tile back to the host, which is no
// faster than the CPU path, and slower over PCIe, so the device only
// upscales what it holds untiled
static gboolean _upscale_fits_device(const dt_dev_pixelpipe_iop_t *piece,
                                     const dt_iop_roi_t *const roi_in,
                                     const dt_iop_roi_t *const roi_out)
{
  const int devid = piece->pipe->devid;
  if(!darktable.opencl->inited || devid < 0) return FALSE;

  const dt_opencl_device_t *dev = &darktable.opencl->dev[devid];
  if(roi_out->width > dev->max_image_width || roi_out->height > dev->max_image_height)
    return FALSE;

  const float needed = 4.0f * sizeof(float) * roi_in->width * roi_in->height
                       * _tiling_factor(roi_in, roi_out);
  return needed < (float)dt_opencl_get_device_available(devid);
}
#endif

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

  // a tile must reach the input row and column under its last output pixel;
  // the truncated x/y above can leave its far edge one short of that, and
  // at the image border no overlap makes up for it
  if(piece->pipe->tiling)
  {
    const int x_end = ceilf((roi_out->x + roi_out->width) / roi_out->scale);
    const int y_end = ceilf((roi_out->y + roi_out->height) / roi_out->scale);
    roi_in->width = MAX(roi_in->width,
                        MIN(x_end, piece->buf_in.width) - roi_in->x);
    roi_in->height = MAX(roi_in->height,
                         MIN(y_end, piece->buf_in.height) - roi_in->y);
  }

  roi_in->scale = 1.0f;

  if(_gui_fullpipe(piece))
  {
    roi_in->x = 0;
    roi_in->y = 0;
    roi_in->width = piece->buf_in.width;
    roi_in->height = piece->buf_in.height;
  }

#ifdef HAVE_OPENCL
  if(roi_out->scale > 1.0f && !piece->pipe->tiling
     && !_upscale_fits_device(piece, roi_in, roi_out))
    piece->process_cl_ready = FALSE;
#endif
}

void tiling_callback(dt_iop_module_t *self,
                     dt_dev_pixelpipe_iop_t *piece,
                     const dt_iop_roi_t *roi_in,
                     const dt_iop_roi_t *roi_out,
                     struct dt_develop_tiling_t *tiling)
{
  tiling->factor = _tiling_factor(roi_in, roi_out);
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
               cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in,
               const dt_iop_roi_t *const roi_out)
{
  const int devid = piece->pipe->devid;
  const gboolean exporting = dt_pipe_is_export(piece->pipe);

  dt_print_pipe(DT_DEBUG_IMAGEIO,
                exporting ? "clip_and_zoom_roi" : "clip_and_zoom",
                piece->pipe, self, piece->pipe->devid, roi_in, roi_out, "device=%i", devid);
  // when tiling, the roi offsets place the tile in the export; shifts are
  // ignored otherwise
  if(exporting && piece->pipe->tiling)
    return dt_iop_clip_and_zoom_tile_cl(devid, dev_out, dev_in, roi_out, roi_in);
  else if(exporting)
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
  const gboolean exporting = dt_pipe_is_export(piece->pipe);
  dt_print_pipe(DT_DEBUG_IMAGEIO,
                exporting ? "clip_and_zoom_roi" : "clip_and_zoom",
                piece->pipe, self, DT_DEVICE_CPU, roi_in, roi_out);

  // see process_cl()
  if(exporting && piece->pipe->tiling)
    dt_iop_clip_and_zoom_tile((float *)ovoid, (float *)ivoid, roi_out, roi_in);
  else if(exporting)
    dt_iop_clip_and_zoom_roi((float *)ovoid, (float *)ivoid, roi_out, roi_in);
  else
    dt_iop_clip_and_zoom((float *)ovoid, (float *)ivoid, roi_out, roi_in, FALSE);
}

void commit_params(dt_iop_module_t *self,
                   dt_iop_params_t *params,
                   dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  piece->enabled = dt_pipe_is_export(piece->pipe)
                  || (dt_pipe_is_image(pipe) && dt_pipe_is_image_final(pipe))
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

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
