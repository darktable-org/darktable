/*
    This file is part of darktable,
    Copyright (C) 2012-2020 darktable developers.

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

#pragma once

#include "common/opencl.h"
#include <assert.h>
#include <math.h>

typedef enum dt_gaussian_order_t
{
  DT_IOP_GAUSSIAN_ZERO = 0, // $DESCRIPTION: "order 0"
  DT_IOP_GAUSSIAN_ONE = 1,  // $DESCRIPTION: "order 1"
  DT_IOP_GAUSSIAN_TWO = 2   // $DESCRIPTION: "order 2"
} dt_gaussian_order_t;


typedef struct dt_gaussian_t
{
  int width, height, channels;
  float sigma;
  int order;
  float *max;
  float *min;
  float *buf;
} dt_gaussian_t;

dt_gaussian_t *dt_gaussian_init(const int width, const int height, const int channels, const float *max,
                                const float *min, const float sigma, const int order);

size_t dt_gaussian_memory_use(const int width, const int height, const int channels);
#ifdef HAVE_OPENCL
size_t dt_gaussian_memory_use_cl(const int width, const int height, const int channels);
#endif

size_t dt_gaussian_singlebuffer_size(const int width, const int height, const int channels);

void dt_gaussian_blur(dt_gaussian_t *g, const float *const in, float *const out);

void dt_gaussian_blur_4c(dt_gaussian_t *g, const float *const in, float *const out);

void dt_gaussian_free(dt_gaussian_t *g);
void dt_gaussian_fast_blur(float *in, float *out, const int width, const int height, const float sigma, const float min, const float max, const int channels);

// Convenience in-place Gaussian blur for IOP processing buffers.
// Uses unbounded signal range; works for arbitrary channel counts (1, 2, or 4).
static inline void dt_gaussian_mean_blur(float *const buf, const int width, const int height,
                                          const int ch, const float sigma)
{
  const float range = 1.0e9f;
  const dt_aligned_pixel_t max = { range, range, range, range };
  const dt_aligned_pixel_t min = { -range, -range, -range, -range };
  dt_gaussian_t *g = dt_gaussian_init(width, height, ch, max, min, sigma, DT_IOP_GAUSSIAN_ZERO);
  if(!g) return;
  if(ch == 4)
    dt_gaussian_blur_4c(g, buf, buf);
  else
    dt_gaussian_blur(g, buf, buf);
  dt_gaussian_free(g);
}

#ifdef HAVE_OPENCL
typedef struct dt_gaussian_cl_global_t
{
  int kernel_gaussian_column_4c, kernel_gaussian_transpose_4c;
  int kernel_gaussian_column_2c, kernel_gaussian_transpose_2c;
  int kernel_gaussian_column_1c, kernel_gaussian_transpose_1c;
  int kernel_gaussian_9x9;
} dt_gaussian_cl_global_t;


typedef struct dt_gaussian_cl_t
{
  dt_gaussian_cl_global_t *global;
  int devid;
  int width, height, channels;
  int blocksize;
  size_t bwidth, bheight;
  float sigma;
  int order;
  float *min;
  float *max;
  cl_mem dev_temp1;
  cl_mem dev_temp2;
} dt_gaussian_cl_t;

dt_gaussian_cl_global_t *dt_gaussian_init_cl_global(void);

void dt_gaussian_free_cl_global(dt_gaussian_cl_global_t *g);

dt_gaussian_cl_t *dt_gaussian_init_cl(const int devid, const int width, const int height, const int channels,
                                      const float *max, const float *min, const float sigma, const int order);

cl_int dt_gaussian_blur_cl(dt_gaussian_cl_t *g, cl_mem dev_in, cl_mem dev_out);
cl_int dt_gaussian_blur_cl_buffer(dt_gaussian_cl_t *g, cl_mem dev_in, cl_mem dev_out);
cl_int dt_gaussian_fast_blur_cl_buffer(const int devid, cl_mem dev_in, cl_mem dev_out, const int width, const int height, const float sigma, const int ch, const float min, const float max);

void dt_gaussian_free_cl(dt_gaussian_cl_t *g);

// OpenCL counterpart of dt_gaussian_mean_blur for GPU buffers.
static inline int dt_gaussian_mean_blur_cl(const int devid, cl_mem buf,
                                            const int width, const int height,
                                            const int ch, const float sigma)
{
  const float range = 1.0e9f;
  const dt_aligned_pixel_t max = { range, range, range, range };
  const dt_aligned_pixel_t min = { -range, -range, -range, -range };
  dt_gaussian_cl_t *g = dt_gaussian_init_cl(devid, width, height, ch, max, min, sigma,
                                            DT_IOP_GAUSSIAN_ZERO);
  if(!g) return DT_OPENCL_PROCESS_CL;
  const cl_int err = dt_gaussian_blur_cl_buffer(g, buf, buf);
  dt_gaussian_free_cl(g);
  return err;
}
#endif

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

