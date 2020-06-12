/*
    This file is part of darktable,
    Copyright (C) 2016-2020 darktable developers.

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

#include "common/bilateral.h"
#include "common/darktable.h" // for CLAMPS, dt_alloc_align, dt_free_align
#include <glib.h>             // for MIN, MAX
#include <math.h>             // for roundf
#include <stdlib.h>           // for size_t, free, malloc, NULL
#include <string.h>           // for memset

// these clamp away insane memory requirements.
// they should reasonably faithfully represent the
// full precision though, so tiling will help reducing memory footprint
// and export will look the same as darkroom mode (only 1mpix there).
#define DT_COMMON_BILATERAL_MAX_RES_S 6000
#define DT_COMMON_BILATERAL_MAX_RES_R 50

#ifndef HAVE_OPENCL
// function definition on opencl path takes precedence
size_t dt_bilateral_memory_use(const int width,     // width of input image
                               const int height,    // height of input image
                               const float sigma_s, // spatial sigma (blur pixel coords)
                               const float sigma_r) // range sigma (blur luma values)
{
  float _x = roundf(width / sigma_s);
  float _y = roundf(height / sigma_s);
  float _z = roundf(100.0f / sigma_r);
  size_t size_x = CLAMPS((int)_x, 4, DT_COMMON_BILATERAL_MAX_RES_S) + 1;
  size_t size_y = CLAMPS((int)_y, 4, DT_COMMON_BILATERAL_MAX_RES_S) + 1;
  size_t size_z = CLAMPS((int)_z, 4, DT_COMMON_BILATERAL_MAX_RES_R) + 1;

  return dt_get_num_threads() * size_x * size_y * size_z * sizeof(float);
}

// for the CPU path this is just an alias as no additional temp buffer is needed
size_t dt_bilateral_memory_use2(const int width,
                                const int height,
                                const float sigma_s,
                                const float sigma_r)
{
  return dt_bilateral_memory_use(width, height, sigma_s, sigma_r);
}

size_t dt_bilateral_singlebuffer_size(const int width,     // width of input image
                                      const int height,    // height of input image
                                      const float sigma_s, // spatial sigma (blur pixel coords)
                                      const float sigma_r) // range sigma (blur luma values)
{
  float _x = roundf(width / sigma_s);
  float _y = roundf(height / sigma_s);
  float _z = roundf(100.0f / sigma_r);
  size_t size_x = CLAMPS((int)_x, 4, DT_COMMON_BILATERAL_MAX_RES_S) + 1;
  size_t size_y = CLAMPS((int)_y, 4, DT_COMMON_BILATERAL_MAX_RES_S) + 1;
  size_t size_z = CLAMPS((int)_z, 4, DT_COMMON_BILATERAL_MAX_RES_R) + 1;

  return dt_get_num_threads() * size_x * size_y * size_z * sizeof(float);
}

// for the CPU path this is just an alias as no additional temp buffer is needed
size_t dt_bilateral_singlebuffer_size2(const int width,
                                       const int height,
                                       const float sigma_s,
                                       const float sigma_r)
{
  return dt_bilateral_singlebuffer_size(width, height, sigma_s, sigma_r);
}
#endif

static void image_to_grid(const dt_bilateral_t *const b, const int i, const int j, const float L, float *x,
                          float *y, float *z)
{
  *x = CLAMPS(i / b->sigma_s, 0, b->size_x - 1);
  *y = CLAMPS(j / b->sigma_s, 0, b->size_y - 1);
  *z = CLAMPS(L / b->sigma_r, 0, b->size_z - 1);
}

dt_bilateral_t *dt_bilateral_init(const int width,     // width of input image
                                  const int height,    // height of input image
                                  const float sigma_s, // spatial sigma (blur pixel coords)
                                  const float sigma_r) // range sigma (blur luma values)
{
  dt_bilateral_t *b = (dt_bilateral_t *)malloc(sizeof(dt_bilateral_t));
  if(!b) return NULL;
  // if(width/sigma_s < 4 || width/sigma_s > 1000) fprintf(stderr, "[bilateral] need to clamp sigma_s!\n");
  // if(height/sigma_s < 4 || height/sigma_s > 1000) fprintf(stderr, "[bilateral] need to clamp sigma_s!\n");
  // if(100.0/sigma_r < 4 || 100.0/sigma_r > 100) fprintf(stderr, "[bilateral] need to clamp sigma_r!\n");
  float _x = roundf(width / sigma_s);
  float _y = roundf(height / sigma_s);
  float _z = roundf(100.0f / sigma_r);
  b->size_x = CLAMPS((int)_x, 4, DT_COMMON_BILATERAL_MAX_RES_S) + 1;
  b->size_y = CLAMPS((int)_y, 4, DT_COMMON_BILATERAL_MAX_RES_S) + 1;
  b->size_z = CLAMPS((int)_z, 4, DT_COMMON_BILATERAL_MAX_RES_R) + 1;
  b->width = width;
  b->height = height;
  b->sigma_s = MAX(height / (b->size_y - 1.0f), width / (b->size_x - 1.0f));
  b->sigma_r = 100.0f / (b->size_z - 1.0f);
  const int nthreads = /*darktable.num_openmp_threads*/ dt_get_num_threads();
  b->buf = dt_alloc_align(64, b->size_x * b->size_y * b->size_z * sizeof(float) * nthreads);

  memset(b->buf, 0, b->size_x * b->size_y * b->size_z * sizeof(float) * nthreads);
#if 0
  fprintf(stderr, "[bilateral] created grid [%d %d %d]"
          " with sigma (%f %f) (%f %f)\n", b->size_x, b->size_y, b->size_z,
          b->sigma_s, sigma_s, b->sigma_r, sigma_r);
#endif
  return b;
}

#ifdef _OPENMP
#pragma omp declare simd aligned(in:64)
#endif
void dt_bilateral_splat(dt_bilateral_t *b, const float *const in)
{
  const int ox = 1;
  const int oy = b->size_x;
  const int oz = b->size_y * b->size_x;
  const float sigma_s = b->sigma_s * b->sigma_s;
  float *const buf = b->buf;
  const int bufsize = b->size_x * b->size_y * b->size_z;

// splat into downsampled grid
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(in, oy, oz, ox, sigma_s, buf, bufsize)    \
  shared(b) \
  collapse(2)
#endif
  for(int j = 0; j < b->height; j++)
  {
    for(int i = 0; i < b->width; i++)
    {
      size_t index = 4 * (j * b->width + i);
      float x, y, z;
      const float L = in[index];
      image_to_grid(b, i, j, L, &x, &y, &z);
      const int xi = MIN((int)x, b->size_x - 2);
      const int yi = MIN((int)y, b->size_y - 2);
      const int zi = MIN((int)z, b->size_z - 2);
      const float xf = x - xi;
      const float yf = y - yi;
      const float zf = z - zi;
      // nearest neighbour splatting:
      const size_t grid_index = xi + b->size_x * (yi + b->size_y * zi) + bufsize * dt_get_thread_num();
      // sum up payload here, doesn't have to be same as edge stopping data
      // for cross bilateral applications.
      // also note that this is not clipped (as L->z is), so potentially hdr/out of gamut
      // should not cause clipping here.
#ifdef _OPENMP
#pragma omp simd aligned(buf:64)
#endif
      for(int k = 0; k < 8; k++)
      {
        const size_t ii = grid_index + ((k & 1) ? ox : 0) + ((k & 2) ? oy : 0) + ((k & 4) ? oz : 0);
        const float contrib = ((k & 1) ? xf : (1.0f - xf)) * ((k & 2) ? yf : (1.0f - yf))
                              * ((k & 4) ? zf : (1.0f - zf)) * 100.0f / sigma_s;
        buf[ii] += contrib;
      }
    }
  }

  // merge the per-thread results into the final result
  const int nthreads = /*darktable.num_openmp_threads*/ dt_get_num_threads();
  for(int index = 0; index < bufsize; index++)
  {
    for(int i = 1; i < nthreads; i++)
    {
      buf[index] += buf[index + i*bufsize];
    }
  }
}

#ifdef _OPENMP
#pragma omp declare simd aligned(buf:64)
#endif
static void blur_line_z(float *buf, const int offset1, const int offset2, const int offset3, const int size1,
                        const int size2, const int size3)
{
  const float w1 = 4.f / 16.f;
  const float w2 = 2.f / 16.f;
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(size1, size2, size3, offset1, offset2, offset3, w1, w2) \
    shared(buf)
#endif
  for(int k = 0; k < size1; k++)
  {
    size_t index = (size_t)k * offset1;
    for(int j = 0; j < size2; j++)
    {
      float tmp1 = buf[index];
      buf[index] = w1 * buf[index + offset3] + w2 * buf[index + 2 * offset3];
      index += offset3;
      float tmp2 = buf[index];
      buf[index] = w1 * (buf[index + offset3] - tmp1) + w2 * buf[index + 2 * offset3];
      index += offset3;
      for(int i = 2; i < size3 - 2; i++)
      {
        const float tmp3 = buf[index];
        buf[index] = +w1 * (buf[index + offset3] - tmp2) + w2 * (buf[index + 2 * offset3] - tmp1);
        index += offset3;
        tmp1 = tmp2;
        tmp2 = tmp3;
      }
      const float tmp3 = buf[index];
      buf[index] = w1 * (buf[index + offset3] - tmp2) - w2 * tmp1;
      index += offset3;
      buf[index] = -w1 * tmp3 - w2 * tmp2;
      index += offset3;
      index += offset2 - offset3 * size3;
    }
  }
}

#ifdef _OPENMP
#pragma omp declare simd aligned(buf:64)
#endif
static void blur_line(float *buf, const int offset1, const int offset2, const int offset3, const int size1,
                      const int size2, const int size3)
{
  const float w0 = 6.f / 16.f;
  const float w1 = 4.f / 16.f;
  const float w2 = 1.f / 16.f;
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(size1, size2, size3, offset1, offset2, offset3, w0, w1, w2) \
    shared(buf)
#endif
  for(int k = 0; k < size1; k++)
  {
    size_t index = (size_t)k * offset1;
    for(int j = 0; j < size2; j++)
    {
      float tmp1 = buf[index];
      buf[index] = buf[index] * w0 + w1 * buf[index + offset3] + w2 * buf[index + 2 * offset3];
      index += offset3;
      float tmp2 = buf[index];
      buf[index] = buf[index] * w0 + w1 * (buf[index + offset3] + tmp1) + w2 * buf[index + 2 * offset3];
      index += offset3;
      for(int i = 2; i < size3 - 2; i++)
      {
        const float tmp3 = buf[index];
        buf[index]
            = buf[index] * w0 + w1 * (buf[index + offset3] + tmp2) + w2 * (buf[index + 2 * offset3] + tmp1);
        index += offset3;
        tmp1 = tmp2;
        tmp2 = tmp3;
      }
      const float tmp3 = buf[index];
      buf[index] = buf[index] * w0 + w1 * (buf[index + offset3] + tmp2) + w2 * tmp1;
      index += offset3;
      buf[index] = buf[index] * w0 + w1 * tmp3 + w2 * tmp2;
      index += offset3;
      index += offset2 - offset3 * size3;
    }
  }
}


void dt_bilateral_blur(dt_bilateral_t *b)
{
  // gaussian up to 3 sigma
  blur_line(b->buf, b->size_x * b->size_y, b->size_x, 1, b->size_z, b->size_y, b->size_x);
  // gaussian up to 3 sigma
  blur_line(b->buf, b->size_x * b->size_y, 1, b->size_x, b->size_z, b->size_x, b->size_y);
  // -2 derivative of the gaussian up to 3 sigma: x*exp(-x*x)
  blur_line_z(b->buf, 1, b->size_x, b->size_x * b->size_y, b->size_x, b->size_y, b->size_z);
}


#ifdef _OPENMP
#pragma omp declare simd aligned(out, in :64)
#endif
void dt_bilateral_slice(const dt_bilateral_t *const b, const float *const in, float *out, const float detail)
{
  // detail: 0 is leave as is, -1 is bilateral filtered, +1 is contrast boost
  const float norm = -detail * b->sigma_r * 0.04f;
  const int ox = 1;
  const int oy = b->size_x;
  const int oz = b->size_y * b->size_x;
  float *const buf = b->buf;
  const int size_x = b->size_x;
  const int size_y = b->size_y;
  const int size_z = b->size_z;
  const int width = b->width;
  const int height = b->height;

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(b, in, norm, ox, oy, oz, size_x, size_y, size_z, height, width, buf) \
    shared(out) collapse(2)
#endif
  for(int j = 0; j < height; j++)
  {
    for(int i = 0; i < width; i++)
    {
      size_t index = 4 * (j * width + i);
      float x, y, z;
      const float L = in[index];
      image_to_grid(b, i, j, L, &x, &y, &z);
      // trilinear lookup:
      const int xi = MIN((int)x, size_x - 2);
      const int yi = MIN((int)y, size_y - 2);
      const int zi = MIN((int)z, size_z - 2);
      const float xf = x - xi;
      const float yf = y - yi;
      const float zf = z - zi;
      const size_t gi = xi + size_x * (yi + size_y * zi);
      const float Lout = L
                         + norm * (buf[gi] * (1.0f - xf) * (1.0f - yf) * (1.0f - zf)
                                   + buf[gi + ox] * (xf) * (1.0f - yf) * (1.0f - zf)
                                   + buf[gi + oy] * (1.0f - xf) * (yf) * (1.0f - zf)
                                   + buf[gi + ox + oy] * (xf) * (yf) * (1.0f - zf)
                                   + buf[gi + oz] * (1.0f - xf) * (1.0f - yf) * (zf)
                                   + buf[gi + ox + oz] * (xf) * (1.0f - yf) * (zf)
                                   + buf[gi + oy + oz] * (1.0f - xf) * (yf) * (zf)
                                   + buf[gi + ox + oy + oz] * (xf) * (yf) * (zf));
      out[index] = Lout;
      // and copy color and mask
      out[index + 1] = in[index + 1];
      out[index + 2] = in[index + 2];
      out[index + 3] = in[index + 3];
    }
  }
}

#ifdef _OPENMP
#pragma omp declare simd aligned(out, in :64)
#endif
void dt_bilateral_slice_to_output(const dt_bilateral_t *const b, const float *const in, float *out,
                                  const float detail)
{
  // detail: 0 is leave as is, -1 is bilateral filtered, +1 is contrast boost
  const float norm = -detail * b->sigma_r * 0.04f;
  const int ox = 1;
  const int oy = b->size_x;
  const int oz = b->size_y * b->size_x;
  float *const buf = b->buf;
  const int size_x = b->size_x;
  const int size_y = b->size_y;
  const int size_z = b->size_z;
  const int width = b->width;
  const int height = b->height;

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(b, in, norm, oy, oz, ox, buf, size_x, size_y, size_z, width, height) \
  shared(out) collapse(2)
#endif
  for(int j = 0; j < height; j++)
  {
    for(int i = 0; i < width; i++)
    {
      size_t index = 4 * (j * width + i);
      float x, y, z;
      const float L = in[index];
      image_to_grid(b, i, j, L, &x, &y, &z);
      // trilinear lookup:
      const int xi = MIN((int)x, size_x - 2);
      const int yi = MIN((int)y, size_y - 2);
      const int zi = MIN((int)z, size_z - 2);
      const float xf = x - xi;
      const float yf = y - yi;
      const float zf = z - zi;
      const size_t gi = xi + size_x * (yi + size_y * zi);
      const float Lout = norm * (buf[gi] * (1.0f - xf) * (1.0f - yf) * (1.0f - zf)
                                 + buf[gi + ox] * (xf) * (1.0f - yf) * (1.0f - zf)
                                 + buf[gi + oy] * (1.0f - xf) * (yf) * (1.0f - zf)
                                 + buf[gi + ox + oy] * (xf) * (yf) * (1.0f - zf)
                                 + buf[gi + oz] * (1.0f - xf) * (1.0f - yf) * (zf)
                                 + buf[gi + ox + oz] * (xf) * (1.0f - yf) * (zf)
                                 + buf[gi + oy + oz] * (1.0f - xf) * (yf) * (zf)
                                 + buf[gi + ox + oy + oz] * (xf) * (yf) * (zf));
      out[index] = MAX(0.0f, out[index] + Lout);
    }
  }
}

void dt_bilateral_free(dt_bilateral_t *b)
{
  if(!b) return;
  dt_free_align(b->buf);
  free(b);
}

#undef DT_COMMON_BILATERAL_MAX_RES_S
#undef DT_COMMON_BILATERAL_MAX_RES_R

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
