/*
    This file is part of darktable,
    copyright (c) 2014 LebedevRI.

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

#include <stdlib.h>
#include <stdint.h>
#include <xmmintrin.h>

#include "develop/imageop.h"
#include "develop/pixelpipe.h"
#include "common/image.h"
#include "develop/tiling.h"
#include "common/opencl.h"

DT_MODULE(1)

typedef struct dt_iop_letsgofloat_params_t
{
  int keep;
} dt_iop_letsgofloat_params_t;

typedef struct dt_iop_letsgofloat_global_data_t
{
  int kernel_letsgofloat_1ui;
} dt_iop_letsgofloat_global_data_t;

const char *name()
{
  return C_("modulename", "let's go float!");
}

int flags()
{
  return IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_ONE_INSTANCE | IOP_FLAGS_HIDDEN | IOP_FLAGS_NO_HISTORY_STACK;
}

int groups()
{
  return IOP_GROUP_BASIC;
}

int output_bpp(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return sizeof(float);
}

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const float divider = (float)UINT16_MAX;
  const __m128 dividers = _mm_set_ps1(divider);

#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static) shared(ovoid)
#endif
  for(int j = 0; j < roi_out->height; j++)
  {
    const uint16_t *in = ((uint16_t *)ivoid) + (size_t)j * roi_out->width;
    float *out = ((float *)ovoid) + (size_t)j * roi_out->width;

    int i = 0;
    int alignment = ((8 - (j * roi_out->width & (8 - 1))) & (8 - 1));

    // process unaligned pixels
    for(; i < alignment; i++, out++, in++) *out = ((float)(*in)) / divider;

    // process aligned pixels with SSE
    for(; i < roi_out->width - (8 - 1); i += 8, in += 8)
    {
      const __m128i input = _mm_load_si128((__m128i *)in);

      __m128i ilo = _mm_unpacklo_epi16(input, _mm_set1_epi16(0));
      __m128i ihi = _mm_unpackhi_epi16(input, _mm_set1_epi16(0));

      __m128 flo = _mm_cvtepi32_ps(ilo);
      __m128 fhi = _mm_cvtepi32_ps(ihi);

      flo = _mm_div_ps(flo, dividers);
      fhi = _mm_div_ps(fhi, dividers);

      _mm_stream_ps(out, flo);
      out += 4;
      _mm_stream_ps(out, fhi);
      out += 4;
    }

    // process the rest
    for(; i < roi_out->width; i++, out++, in++) *out = ((float)(*in)) / divider;
  }
  _mm_sfence();
}

#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_letsgofloat_global_data_t *gd = (dt_iop_letsgofloat_global_data_t *)self->data;

  cl_int err = -999;

  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;

  size_t sizes[] = { ROUNDUPWD(width), ROUNDUPHT(height), 1 };
  dt_opencl_set_kernel_arg(devid, gd->kernel_letsgofloat_1ui, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_letsgofloat_1ui, 1, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(devid, gd->kernel_letsgofloat_1ui, 2, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_letsgofloat_1ui, 3, sizeof(int), (void *)&height);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_letsgofloat_1ui, sizes);
  if(err != CL_SUCCESS) goto error;

  return TRUE;

error:
  dt_print(DT_DEBUG_OPENCL, "[opencl_letsgofloat] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *params, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  if(!(pipe->image.flags & DT_IMAGE_RAW) || dt_dev_pixelpipe_uses_downsampled_input(pipe)
     || !dt_image_filter(&piece->pipe->image) || piece->pipe->image.bpp != sizeof(uint16_t))
    piece->enabled = 0;
}

void init_global(dt_iop_module_so_t *self)
{
  const int program = 2; // basic.cl, from programs.conf
  self->data = malloc(sizeof(dt_iop_letsgofloat_global_data_t));

  dt_iop_letsgofloat_global_data_t *gd = self->data;
  gd->kernel_letsgofloat_1ui = dt_opencl_create_kernel(program, "letsgofloat_1ui");
}

void init(dt_iop_module_t *self)
{
  self->params = calloc(1, sizeof(dt_iop_letsgofloat_params_t));
  self->default_params = calloc(1, sizeof(dt_iop_letsgofloat_params_t));
  self->hide_enable_button = 1;
  self->default_enabled = 1;
  self->priority = 5; // module order created by iop_dependencies.py, do not edit!
  self->params_size = sizeof(dt_iop_letsgofloat_params_t);
  self->gui_data = NULL;
}

void cleanup(dt_iop_module_t *self)
{
  free(self->params);
  self->params = NULL;
}

void cleanup_global(dt_iop_module_so_t *self)
{
  dt_iop_letsgofloat_global_data_t *gd = (dt_iop_letsgofloat_global_data_t *)self->data;
  dt_opencl_free_kernel(gd->kernel_letsgofloat_1ui);
  free(self->data);
  self->data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
