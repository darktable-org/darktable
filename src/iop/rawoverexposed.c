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

#include "common/darktable.h"    // for darktable, darktable_t, dt_alloc_a...
#include "common/image.h"        // for dt_image_t, ::DT_IMAGE_4BAYER
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

static const float dt_iop_rawoverexposed_colors[][4] __attribute__((aligned(16))) = {
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

int groups()
{
  return IOP_GROUP_BASIC;
}

int flags()
{
  return IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_HIDDEN | IOP_FLAGS_ONE_INSTANCE | IOP_FLAGS_NO_HISTORY_STACK;
}

static void process_common_setup(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece)
{
  dt_develop_t *dev = self->dev;
  dt_iop_rawoverexposed_data_t *d = piece->data;

  // 4BAYER is not supported by this module yet anyway.
  const int ch = (dev->image_storage.flags & DT_IMAGE_4BAYER) ? 4 : 3;

  float threshold;

  // the clipping is detected as >1.0 after white level normalization

  /*
   * yes, technically, sensor clipping needs to be detected not accounting
   * for white balance.
   *
   * but we are not after technical sensor clipping.
   *
   * pick some image that is overexposed, disable highlight clipping, apply
   * negative exposure compensation. you'll see magenta highlight.
   * if comment-out that ^ wb division, the module would not mark that
   * area with magenta highlights as clipped, because technically
   * the channels are not clipped, even though the colour is wrong.
   *
   * but we do want to see those magenta highlights marked...
   */

  if(piece->pipe->dsc.temperature.enabled)
  {
    threshold = FLT_MAX;

    // so to detect the color clipping, we need to take white balance into account.
    for(int k = 0; k < ch; k++) threshold = fminf(threshold, piece->pipe->dsc.temperature.coeffs[k]);
  }
  else
  {
    threshold = 1.0f;
  }

  threshold *= dev->rawoverexposed.threshold;

  for(int k = 0; k < ch; k++)
  {
    // here is our threshold
    float chthr = threshold;

    // but we check it on the raw input buffer, so we need backtransform threshold

    // "undo" temperature iop
    if(piece->pipe->dsc.temperature.enabled) chthr /= piece->pipe->dsc.temperature.coeffs[k];

    // "undo" rawprepare iop
    chthr *= piece->pipe->dsc.rawprepare.raw_white_point - piece->pipe->dsc.rawprepare.raw_black_level;
    chthr += piece->pipe->dsc.rawprepare.raw_black_level;

    // and this is that threshold, but in raw input buffer values
    d->threshold[k] = (unsigned int)chthr;
  }

  // printf("d->threshold[] = { %i, %i, %i, %i }\n", d->threshold[0], d->threshold[1], d->threshold[2],
  // d->threshold[3]);
}

void process(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid, void *const ovoid,
             const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_rawoverexposed_data_t *const d = piece->data;

  process_common_setup(self, piece);

  dt_develop_t *dev = self->dev;
  const dt_image_t *const image = &(dev->image_storage);

  const int ch = piece->colors;
  const int priority = self->priority;

  const dt_dev_rawoverexposed_mode_t mode = dev->rawoverexposed.mode;
  const int colorscheme = dev->rawoverexposed.colorscheme;
  const float *const color = dt_iop_rawoverexposed_colors[colorscheme];

  memcpy(ovoid, ivoid, (size_t)ch * roi_out->width * roi_out->height * sizeof(float));

  dt_mipmap_buffer_t buf;
  dt_mipmap_cache_get(darktable.mipmap_cache, &buf, image->id, DT_MIPMAP_FULL, DT_MIPMAP_BLOCKING, 'r');
  if(!buf.buf)
  {
    dt_control_log(_("failed to get raw buffer from image `%s'"), image->filename);
    dt_mipmap_cache_release(darktable.mipmap_cache, &buf);
    return;
  }

#if 0
  float pts[4] = {(float)(roi_out->x) / roi_in->scale, (float)(roi_out->y) / roi_in->scale, (float)(roi_out->x + roi_out->width) / roi_in->scale, (float)(roi_out->y + roi_out->height) / roi_in->scale};
  printf("in  %f %f %f %f\n", pts[0], pts[1], pts[2], pts[3]);
  dt_dev_distort_backtransform_plus(dev, dev->pipe, 0, priority, pts, 2);
  printf("out %f %f %f %f\n\n", pts[0], pts[1], pts[2], pts[3]);
#endif

  const uint16_t *const raw = (const uint16_t *const)buf.buf;
  float *const out = (float *const)ovoid;

  // NOT FROM THE PIPE !!!
  const uint32_t filters = image->buf_dsc.filters;
  const uint8_t(*const xtrans)[6] = (const uint8_t(*const)[6])image->buf_dsc.xtrans;

  // acquire temp memory for distorted pixel coords
  const size_t coordbufsize = (size_t)roi_out->width * 2;
  void *coordbuf = dt_alloc_align(16, coordbufsize * sizeof(float) * dt_get_num_threads());

#ifdef _OPENMP
#pragma omp parallel for SIMD() default(none) shared(self, coordbuf, buf) schedule(static)
#endif
  for(int j = 0; j < roi_out->height; j++)
  {
    float *bufptr = ((float *)coordbuf) + (size_t)coordbufsize * dt_get_thread_num();

    // here are all the pixels of this row
    for(int i = 0; i < roi_out->width; i++)
    {
      bufptr[2 * i] = (float)(roi_out->x + i) / roi_in->scale;
      bufptr[2 * i + 1] = (float)(roi_out->y + j) / roi_in->scale;
    }

    // where did they come from?
    dt_dev_distort_backtransform_plus(self->dev, self->dev->pipe, 0, priority, bufptr, roi_out->width);

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
          memcpy(out + pout, dt_iop_rawoverexposed_colors[c], 4 * sizeof(float));
          break;
        case DT_DEV_RAWOVEREXPOSED_MODE_MARK_SOLID:
          memcpy(out + pout, color, 4 * sizeof(float));
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
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_rawoverexposed_data_t *const d = piece->data;
  dt_develop_t *dev = self->dev;
  dt_iop_rawoverexposed_global_data_t *gd = (dt_iop_rawoverexposed_global_data_t *)self->data;

  cl_mem dev_raw = NULL;
  float *coordbuf = NULL;
  cl_mem dev_coord = NULL;
  cl_mem dev_thresholds = NULL;
  cl_mem dev_colors = NULL;
  cl_mem dev_xtrans = NULL;

  cl_int err = -999;

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

  dev_raw = dt_opencl_copy_host_to_device(devid, buf.buf, raw_width, raw_height, sizeof(uint16_t));
  if(dev_raw == NULL) goto error;

  const size_t coordbufsize = (size_t)height * width * 2 * sizeof(float);

  coordbuf = dt_alloc_align(16, coordbufsize);
  if(coordbuf == NULL) goto error;

#ifdef _OPENMP
#pragma omp parallel for SIMD() default(none) shared(self, coordbuf, buf) schedule(static)
#endif
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
    dt_dev_distort_backtransform_plus(self->dev, self->dev->pipe, 0, self->priority, bufptr, roi_out->width);
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

  if(filters == 9u)
  {
    dev_xtrans
        = dt_opencl_copy_host_to_device_constant(devid, sizeof(image->buf_dsc.xtrans), (void *)image->buf_dsc.xtrans);
    if(dev_xtrans == NULL) goto error;
  }

  dev_thresholds = dt_opencl_copy_host_to_device_constant(devid, sizeof(unsigned int) * 4, (void *)d->threshold);
  if(dev_thresholds == NULL) goto error;

  size_t sizes[2] = { ROUNDUPWD(width), ROUNDUPHT(height) };
  dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &dev_in);
  dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &dev_out);
  dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &dev_coord);
  dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), &width);
  dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), &height);
  dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(cl_mem), &dev_raw);
  dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(int), &raw_width);
  dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(int), &raw_height);
  dt_opencl_set_kernel_arg(devid, kernel, 8, sizeof(uint32_t), &filters);
  dt_opencl_set_kernel_arg(devid, kernel, 9, sizeof(cl_mem), &dev_xtrans);
  dt_opencl_set_kernel_arg(devid, kernel, 10, sizeof(cl_mem), &dev_thresholds);

  if(dev->rawoverexposed.mode == DT_DEV_RAWOVEREXPOSED_MODE_MARK_CFA)
    dt_opencl_set_kernel_arg(devid, kernel, 11, sizeof(cl_mem), &dev_colors);
  else if(dev->rawoverexposed.mode == DT_DEV_RAWOVEREXPOSED_MODE_MARK_SOLID)
    dt_opencl_set_kernel_arg(devid, kernel, 11, 4 * sizeof(float), color);

  err = dt_opencl_enqueue_kernel_2d(devid, kernel, sizes);
  if(err != CL_SUCCESS) goto error;

  dt_opencl_release_mem_object(dev_xtrans);
  dt_opencl_release_mem_object(dev_colors);
  dt_opencl_release_mem_object(dev_thresholds);
  dt_opencl_release_mem_object(dev_coord);
  dt_free_align(coordbuf);
  dt_opencl_release_mem_object(dev_raw);
  dt_mipmap_cache_release(darktable.mipmap_cache, &buf);

  return TRUE;

error:
  dt_opencl_release_mem_object(dev_xtrans);
  dt_opencl_release_mem_object(dev_colors);
  dt_opencl_release_mem_object(dev_thresholds);
  dt_opencl_release_mem_object(dev_coord);
  dt_free_align(coordbuf);
  dt_opencl_release_mem_object(dev_raw);
  dt_mipmap_cache_release(darktable.mipmap_cache, &buf);
  dt_print(DT_DEBUG_OPENCL, "[opencl_rawoverexposed] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif

void tiling_callback(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                     const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out,
                     struct dt_develop_tiling_t *tiling)
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

  if(pipe->type != DT_DEV_PIXELPIPE_FULL || !dev->rawoverexposed.enabled || !dev->gui_attached) piece->enabled = 0;

  const dt_image_t *const image = &(dev->image_storage);

  if(image->flags & DT_IMAGE_4BAYER) piece->enabled = 0;

  if(image->buf_dsc.datatype != TYPE_UINT16 || !image->buf_dsc.filters) piece->enabled = 0;
}

void init_global(dt_iop_module_so_t *module)
{
  const int program = 2; // basic.cl from programs.conf
  module->data = malloc(sizeof(dt_iop_rawoverexposed_global_data_t));
  dt_iop_rawoverexposed_global_data_t *gd = module->data;
  gd->kernel_rawoverexposed_mark_cfa = dt_opencl_create_kernel(program, "rawoverexposed_mark_cfa");
  gd->kernel_rawoverexposed_mark_solid = dt_opencl_create_kernel(program, "rawoverexposed_mark_solid");
  gd->kernel_rawoverexposed_falsecolor = dt_opencl_create_kernel(program, "rawoverexposed_falsecolor");
}


void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_rawoverexposed_global_data_t *gd = (dt_iop_rawoverexposed_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_rawoverexposed_falsecolor);
  dt_opencl_free_kernel(gd->kernel_rawoverexposed_mark_solid);
  dt_opencl_free_kernel(gd->kernel_rawoverexposed_mark_cfa);
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

void init(dt_iop_module_t *module)
{
  module->params = calloc(1, sizeof(dt_iop_rawoverexposed_t));
  module->default_params = calloc(1, sizeof(dt_iop_rawoverexposed_t));
  module->hide_enable_button = 1;
  module->default_enabled = 1;
  module->priority = 942; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_rawoverexposed_t);
  module->gui_data = NULL;
}

void cleanup(dt_iop_module_t *module)
{
  free(module->params);
  module->params = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
