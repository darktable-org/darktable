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

#include "common/darktable.h"     // for darktable, darktable_t, dt_alloc_a...
#include "common/image.h"         // for dt_image_t, ::DT_IMAGE_4BAYER
#include "common/mipmap_cache.h"  // for dt_mipmap_buffer_t, dt_mipmap_cach...
#include "control/control.h"      // for dt_control_log
#include "develop/develop.h"      // for dt_develop_t, dt_develop_t::(anony...
#include "develop/imageop.h"      // for dt_iop_module_t, dt_iop_roi_t, dt_...
#include "develop/imageop_math.h" // for FC, FCxtrans
#include "develop/pixelpipe.h"    // for dt_dev_pixelpipe_type_t::DT_DEV_PI...
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
  return IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_HIDDEN | IOP_FLAGS_ONE_INSTANCE | IOP_FLAGS_NO_HISTORY_STACK;
}

static void process_common_setup(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece)
{
  dt_develop_t *dev = self->dev;
  dt_iop_rawoverexposed_data_t *d = piece->data;

  for(int k = 0; k < 4; k++)
  {
    // the clipping is detected as 1.0 in highlights iop
    float threshold = dev->rawoverexposed.threshold;

    // but we check it on the raw input buffer, so we need backtransform thresholds

    // "undo" temperature iop
    if(piece->pipe->dsc.temperature.enabled) threshold /= piece->pipe->dsc.temperature.coeffs[k];

    // "undo" rawprepare iop
    threshold *= piece->pipe->dsc.rawprepare.raw_white_point - piece->pipe->dsc.rawprepare.raw_black_level;
    threshold += piece->pipe->dsc.rawprepare.raw_black_level;

    // and this is that 1.0 threshold, but in raw input buffer values
    d->threshold[k] = (unsigned int)threshold;
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

  if(piece->pipe->mask_display) dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);
}

void commit_params(dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_develop_t *dev = self->dev;

  if(pipe->type != DT_DEV_PIXELPIPE_FULL || !dev->rawoverexposed.enabled || !dev->gui_attached) piece->enabled = 0;

  const dt_image_t *const image = &(dev->image_storage);

  if(image->flags & DT_IMAGE_4BAYER) piece->enabled = 0;

  if(image->buf_dsc.datatype != TYPE_UINT16 || !image->buf_dsc.filters) piece->enabled = 0;

  piece->process_cl_ready = 0;
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
  module->priority = 924; // module order created by iop_dependencies.py, do not edit!
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
