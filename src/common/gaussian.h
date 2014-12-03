/*
    This file is part of darktable,
    copyright (c) 2012 Ulrich Pegelow.

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

#ifndef DT_COMMON_GAUSSIAN_H
#define DT_COMMON_GAUSSIAN_H

#include <math.h>
#include <assert.h>
#include <xmmintrin.h>
#include "common/opencl.h"

typedef enum dt_gaussian_order_t
{
  DT_IOP_GAUSSIAN_ZERO = 0,
  DT_IOP_GAUSSIAN_ONE = 1,
  DT_IOP_GAUSSIAN_TWO = 2
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

size_t dt_gaussian_singlebuffer_size(const int width, const int height, const int channels);

void dt_gaussian_blur(dt_gaussian_t *g, float *in, float *out);

void dt_gaussian_blur_4c(dt_gaussian_t *g, float *in, float *out);

void dt_gaussian_free(dt_gaussian_t *g);


#ifdef HAVE_OPENCL
typedef struct dt_gaussian_cl_global_t
{
  int kernel_gaussian_column_4c, kernel_gaussian_transpose_4c;
  int kernel_gaussian_column_1c, kernel_gaussian_transpose_1c;
} dt_gaussian_cl_global_t;


typedef struct dt_gaussian_cl_t
{
  dt_gaussian_cl_global_t *global;
  int devid;
  int width, height, channels;
  int blocksize, blockwd, blockht;
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

void dt_gaussian_free_cl(dt_gaussian_cl_t *g);
#endif

#endif
