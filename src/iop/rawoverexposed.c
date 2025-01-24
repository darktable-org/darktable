/*
   This file is part of darktable,
   Copyright (C) 2016-2024 darktable developers.

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

#include "common/darktable.h"    // for darktable, darktable_t, dt_alloc_a...
#include "common/image.h"        // for dt_image_t, ::DT_IMAGE_4BAYER
#include "common/imagebuf.h"     // for dt_iop_image_copy_by_size
#include "common/mipmap_cache.h" // for dt_mipmap_buffer_t, dt_mipmap_cach...
#include "common/opencl.h"
#include "control/control.h"      // for dt_control_log
#include "develop/develop.h"      // for dt_develop_t, dt_develop_t::(anony...
#include "develop/imageop.h"      // for dt_iop_module_t, dt_iop_roi_t, dt_...
#include "develop/imageop_math.h" // for FC, FCxtrans
#include "develop/pixelpipe.h"    // for dt_dev_pixelpipe_type_t::DT_DEV_PI...
#include "develop/tiling.h"
#include "iop/iop_api.h"          // for dt_iop_params_t
#include <glib/gi18n.h>           // for _
#include <gtk/gtktypes.h>         // for GtkWidget
#include <stdint.h>               // for uint16_t, uint8_t, uint32_t
#include <stdlib.h>               // for size_t, free, NULL, calloc, malloc
#include <string.h>               // for memcpy

DT_MODULE(1)

typedef struct dt_iop_rawoverexposed_t
{
  int dummy;
} dt_iop_rawoverexposed_t;

static const float dt_iop_rawoverexposed_colors[][4] __attribute__((aligned(64))) = {
  { 1.0f, 0.0f, 0.0f, 1.0f }, // red
  { 0.0f, 1.0f, 0.0f, 1.0f }, // green
  { 0.0f, 0.0f, 1.0f, 1.0f }, // blue
  { 0.0f, 0.0f, 0.0f, 1.0f }  // black
};

typedef struct dt_iop_rawoverexposed_data_t
{
  unsigned int threshold[4];
} dt_iop_rawoverexposed_data_t;

typedef struct dt_iop_rawoverexposed_global_data_t
{
  int kernel_rawoverexposed_mark_cfa;
  int kernel_rawoverexposed_mark_solid;
  int kernel_rawoverexposed_falsecolor;
} dt_iop_rawoverexposed_global_data_t;

const char *name()
{
  return _("raw overexposed");
}

int default_group()
{
  return IOP_GROUP_BASIC | IOP_GROUP_TECHNICAL;
}

int flags()
{
  return IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_HIDDEN | IOP_FLAGS_ONE_INSTANCE | IOP_FLAGS_NO_HISTORY_STACK;
}

dt_iop_colorspace_type_t default_colorspace(dt_iop_module_t *self,
                                            dt_dev_pixelpipe_t *pipe,
                                            dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

static void process_common_setup(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece)
{
  dt_develop_t *dev = self->dev;
  dt_iop_rawoverexposed_data_t *d = piece->data;

  // 4BAYER is not supported by this module yet anyway.
  const int ch = (dev->image_storage.flags & DT_IMAGE_4BAYER) ? 4 : 3;

  // the clipping is detected as (raw value > threshold)
  float threshold = dev->rawoverexposed.threshold;

  for(int k = 0; k < ch; k++)
  {
    // here is our threshold
    float chthr = threshold;

    // "undo" rawprepare iop
    chthr *= piece->pipe->dsc.rawprepare.raw_white_point - piece->pipe->dsc.rawprepare.raw_black_level;
    chthr += piece->pipe->dsc.rawprepare.raw_black_level;

    // and this is that threshold, but in raw input buffer values
    d->threshold[k] = (unsigned int)chthr;
  }
}

void process(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid, void *const ovoid,
             const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_rawoverexposed_data_t *const d = piece->data;

  process_common_setup(self, piece);

  dt_develop_t *dev = self->dev;
  const dt_image_t *const image = &(dev->image_storage);

  const int ch = piece->colors;
  const double iop_order = self->iop_order;

  const dt_dev_rawoverexposed_mode_t mode = dev->rawoverexposed.mode;
  const int colorscheme = dev->rawoverexposed.colorscheme;
  const float *const color = dt_iop_rawoverexposed_colors[colorscheme];

  dt_iop_image_copy_by_size(ovoid, ivoid, roi_out->width, roi_out->height, ch);

  dt_mipmap_buffer_t buf;
  dt_mipmap_cache_get(darktable.mipmap_cache, &buf, image->id, DT_MIPMAP_FULL, DT_MIPMAP_BLOCKING, 'r');
  if(!buf.buf)
  {
    dt_control_log(_("failed to get raw buffer from image `%s'"), image->filename);
    dt_mipmap_cache_release(darktable.mipmap_cache, &buf);
    return;
  }

#if 0
  const float in_scale = roi_in->scale;
  dt_boundingbox_t pts = {(float)(roi_out->x) / in_scale, (float)(roi_out->y) / in_scale,
                          (float)(roi_out->x + roi_out->width) / in_scale, (float)(roi_out->y + roi_out->height) / in_scale};
  printf("in  %f %f %f %f\n", pts[0], pts[1], pts[2], pts[3]);
  dt_dev_distort_backtransform_plus(dev, dev->full.pipe, 0, priority, pts, 2);
  printf("out %f %f %f %f\n\n", pts[0], pts[1], pts[2], pts[3]);
#endif

  const uint16_t *const raw = (const uint16_t *const)buf.buf;
  float *const restrict out = DT_IS_ALIGNED((float *const)ovoid);

  // NOT FROM THE PIPE !!!
  const uint32_t filters = image->buf_dsc.filters;
  const uint8_t(*const xtrans)[6] = (const uint8_t(*const)[6])image->buf_dsc.xtrans;

  // acquire temp memory for distorted pixel coords
  size_t coordbufsize;
  float *const restrict coordbuf = dt_alloc_perthread_float(2*roi_out->width, &coordbufsize);

  DT_OMP_FOR(firstprivate(dt_iop_rawoverexposed_colors))
  for(int j = 0; j < roi_out->height; j++)
  {
    float *const restrict bufptr = dt_get_perthread(coordbuf, coordbufsize);

    // here are all the pixels of this row
    for(int i = 0; i < roi_out->width; i++)
    {
      bufptr[2 * i] = (float)(roi_out->x + i) / roi_in->scale;
      bufptr[2 * i + 1] = (float)(roi_out->y + j) / roi_in->scale;
    }

    // where did they come from?
    dt_dev_distort_backtransform_plus(self->dev, self->dev->full.pipe, iop_order, DT_DEV_TRANSFORM_DIR_BACK_INCL, bufptr, roi_out->width);

    for(int i = 0; i < roi_out->width; i++)
    {
      const size_t pout = (size_t)ch * (j * roi_out->width + i);

      // not sure which float -> int to use here
      const int i_raw = (int)bufptr[2 * i];
      const int j_raw = (int)bufptr[2 * i + 1];

      if(i_raw < 0 || j_raw < 0 || i_raw >= buf.width || j_raw >= buf.height) continue;

      int c;
      if(filters == 9u)
      {
        c = FCxtrans(j_raw, i_raw, NULL, xtrans);
      }
      else // if(filters)
      {
        c = FC(j_raw, i_raw, filters);
      }

      const size_t pin = (size_t)j_raw * buf.width + i_raw;
      const float in = raw[pin];

      // was the raw pixel clipped?
      if(in < d->threshold[c]) continue;

      switch(mode)
      {
        case DT_DEV_RAWOVEREXPOSED_MODE_MARK_CFA:
          memcpy(out + pout, dt_iop_rawoverexposed_colors[c], sizeof(float) * 4);
          break;
        case DT_DEV_RAWOVEREXPOSED_MODE_MARK_SOLID:
          memcpy(out + pout, color, sizeof(float) * 4);
          break;
        case DT_DEV_RAWOVEREXPOSED_MODE_FALSECOLOR:
          out[pout + c] = 0.0;
          break;
      }
    }
  }

  dt_free_align(coordbuf);

  dt_mipmap_cache_release(darktable.mipmap_cache, &buf);

  if(piece->pipe->mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK) dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);
}

#ifdef HAVE_OPENCL
int process_cl(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_rawoverexposed_data_t *const d = piece->data;
  dt_develop_t *dev = self->dev;
  dt_iop_rawoverexposed_global_data_t *gd = self->global_data;

  cl_mem dev_raw = NULL;
  float *coordbuf = NULL;
  cl_mem dev_coord = NULL;
  cl_mem dev_thresholds = NULL;
  cl_mem dev_colors = NULL;
  cl_mem dev_xtrans = NULL;

  cl_int err = DT_OPENCL_DEFAULT_ERROR;

  const dt_image_t *const image = &(dev->image_storage);

  dt_mipmap_buffer_t buf;
  dt_mipmap_cache_get(darktable.mipmap_cache, &buf, image->id, DT_MIPMAP_FULL, DT_MIPMAP_BLOCKING, 'r');
  if(!buf.buf)
  {
    dt_control_log(_("failed to get raw buffer from image `%s'"), image->filename);
    dt_mipmap_cache_release(darktable.mipmap_cache, &buf);
    goto error;
  }

  const int devid = piece->pipe->devid;

  const int width = roi_out->width;
  const int height = roi_out->height;

  size_t origin[] = { 0, 0, 0 };
  size_t region[] = { width, height, 1 };

  process_common_setup(self, piece);

  err = dt_opencl_enqueue_copy_image(devid, dev_in, dev_out, origin, origin, region);
  if(err != CL_SUCCESS) goto error;

  const int colorscheme = dev->rawoverexposed.colorscheme;
  const float *const color = dt_iop_rawoverexposed_colors[colorscheme];

  // NOT FROM THE PIPE !!!
  const uint32_t filters = image->buf_dsc.filters;

  const int raw_width = buf.width;
  const int raw_height = buf.height;

  err = DT_OPENCL_SYSMEM_ALLOCATION;
  dev_raw = dt_opencl_copy_host_to_device(devid, buf.buf, raw_width, raw_height, sizeof(uint16_t));
  if(dev_raw == NULL) goto error;

  const size_t coordbufsize = (size_t)height * width * 2 * sizeof(float);

  coordbuf = dt_alloc_aligned(coordbufsize);
  if(coordbuf == NULL) goto error;

  DT_OMP_FOR()
  for(int j = 0; j < height; j++)
  {
    float *bufptr = ((float *)coordbuf) + (size_t)2 * j * width;

    // here are all the pixels of this row
    for(int i = 0; i < roi_out->width; i++)
    {
      bufptr[2 * i] = (float)(roi_out->x + i) / roi_in->scale;
      bufptr[2 * i + 1] = (float)(roi_out->y + j) / roi_in->scale;
    }

    // where did they come from?
    dt_dev_distort_backtransform_plus(self->dev, self->dev->full.pipe, self->iop_order, DT_DEV_TRANSFORM_DIR_BACK_INCL, bufptr, roi_out->width);
  }

  dev_coord = dt_opencl_alloc_device_buffer(devid, coordbufsize);
  if(dev_coord == NULL) goto error;

  /* _blocking_ memory transfer: host coordbuf buffer -> opencl dev_coordbuf */
  err = dt_opencl_write_buffer_to_device(devid, coordbuf, dev_coord, 0, coordbufsize, CL_TRUE);
  if(err != CL_SUCCESS) goto error;

  int kernel;
  switch(dev->rawoverexposed.mode)
  {
    case DT_DEV_RAWOVEREXPOSED_MODE_MARK_CFA:
      kernel = gd->kernel_rawoverexposed_mark_cfa;

      dev_colors = dt_opencl_alloc_device_buffer(devid, sizeof(dt_iop_rawoverexposed_colors));
      if(dev_colors == NULL) goto error;

      /* _blocking_ memory transfer: host coordbuf buffer -> opencl dev_colors */
      err = dt_opencl_write_buffer_to_device(devid, (void *)dt_iop_rawoverexposed_colors, dev_colors, 0,
                                             sizeof(dt_iop_rawoverexposed_colors), CL_TRUE);
      if(err != CL_SUCCESS) goto error;

      break;
    case DT_DEV_RAWOVEREXPOSED_MODE_MARK_SOLID:
      kernel = gd->kernel_rawoverexposed_mark_solid;
      break;
    case DT_DEV_RAWOVEREXPOSED_MODE_FALSECOLOR:
    default:
      kernel = gd->kernel_rawoverexposed_falsecolor;
      break;
  }

  err = DT_OPENCL_SYSMEM_ALLOCATION;
  if(filters == 9u)
  {
    dev_xtrans
        = dt_opencl_copy_host_to_device_constant(devid, sizeof(image->buf_dsc.xtrans), (void *)image->buf_dsc.xtrans);
    if(dev_xtrans == NULL) goto error;
  }

  dev_thresholds = dt_opencl_copy_host_to_device_constant(devid, sizeof(unsigned int) * 4, (void *)d->threshold);
  if(dev_thresholds == NULL) goto error;

  size_t sizes[2] = { ROUNDUPDWD(width, devid), ROUNDUPDHT(height, devid) };
  dt_opencl_set_kernel_args(devid, kernel, 0, CLARG(dev_in), CLARG(dev_out), CLARG(dev_coord), CLARG(width),
    CLARG(height), CLARG(dev_raw), CLARG(raw_width), CLARG(raw_height), CLARG(filters), CLARG(dev_xtrans),
    CLARG(dev_thresholds));

  if(dev->rawoverexposed.mode == DT_DEV_RAWOVEREXPOSED_MODE_MARK_CFA)
    dt_opencl_set_kernel_args(devid, kernel, 11, CLARG(dev_colors));
  else if(dev->rawoverexposed.mode == DT_DEV_RAWOVEREXPOSED_MODE_MARK_SOLID)
    dt_opencl_set_kernel_args(devid, kernel, 11, CLARRAY(4, color));

  err = dt_opencl_enqueue_kernel_2d(devid, kernel, sizes);

error:
  dt_opencl_release_mem_object(dev_xtrans);
  dt_opencl_release_mem_object(dev_colors);
  dt_opencl_release_mem_object(dev_thresholds);
  dt_opencl_release_mem_object(dev_coord);
  dt_free_align(coordbuf);
  dt_opencl_release_mem_object(dev_raw);
  dt_mipmap_cache_release(darktable.mipmap_cache, &buf);
  return err;
}
#endif

void tiling_callback(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece,
                     const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out,
                     dt_develop_tiling_t *tiling)
{
  dt_develop_t *dev = self->dev;
  const dt_image_t *const image = &(dev->image_storage);

  // the module needs access to the full raw image which adds to the memory footprint
  // on OpenCL devices. We take account in tiling->overhead.

  dt_mipmap_buffer_t buf;
  int raw_width = 0;
  int raw_height = 0;

  dt_mipmap_cache_get(darktable.mipmap_cache, &buf, image->id, DT_MIPMAP_FULL, DT_MIPMAP_BLOCKING, 'r');

  if(buf.buf)
  {
    raw_width = buf.width;
    raw_height = buf.height;
  }

  dt_mipmap_cache_release(darktable.mipmap_cache, &buf);

  tiling->factor = 2.5f;  // in + out + coordinates
  tiling->maxbuf = 1.0f;
  tiling->overhead = (size_t)raw_width * raw_height * sizeof(uint16_t);
  tiling->overlap = 0;
  tiling->xalign = 1;
  tiling->yalign = 1;
  return;
}

void commit_params(dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_develop_t *dev = self->dev;

  const dt_image_t *const image = &(dev->image_storage);
  const gboolean fullpipe = piece->pipe->type & DT_DEV_PIXELPIPE_FULL;
  const gboolean sensorok = (image->flags & DT_IMAGE_4BAYER) == 0;

  piece->enabled = dev->rawoverexposed.enabled && fullpipe && dev->gui_attached && sensorok;

  if(image->buf_dsc.datatype != TYPE_UINT16 || !image->buf_dsc.filters) piece->enabled = FALSE;
}

void init_global(dt_iop_module_so_t *self)
{
  const int program = 2; // basic.cl from programs.conf
  self->data = malloc(sizeof(dt_iop_rawoverexposed_global_data_t));
  dt_iop_rawoverexposed_global_data_t *gd = self->data;
  gd->kernel_rawoverexposed_mark_cfa = dt_opencl_create_kernel(program, "rawoverexposed_mark_cfa");
  gd->kernel_rawoverexposed_mark_solid = dt_opencl_create_kernel(program, "rawoverexposed_mark_solid");
  gd->kernel_rawoverexposed_falsecolor = dt_opencl_create_kernel(program, "rawoverexposed_falsecolor");
}


void cleanup_global(dt_iop_module_so_t *self)
{
  dt_iop_rawoverexposed_global_data_t *gd = self->data;
  dt_opencl_free_kernel(gd->kernel_rawoverexposed_falsecolor);
  dt_opencl_free_kernel(gd->kernel_rawoverexposed_mark_solid);
  dt_opencl_free_kernel(gd->kernel_rawoverexposed_mark_cfa);
  free(self->data);
  self->data = NULL;
}

void init_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_rawoverexposed_data_t));
}

void cleanup_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void init(dt_iop_module_t *self)
{
  self->params = calloc(1, sizeof(dt_iop_rawoverexposed_t));
  self->default_params = calloc(1, sizeof(dt_iop_rawoverexposed_t));
  self->hide_enable_button = TRUE;
  self->default_enabled = TRUE;
  self->params_size = sizeof(dt_iop_rawoverexposed_t);
  self->gui_data = NULL;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

