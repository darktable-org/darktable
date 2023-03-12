/*
    This file is part of darktable,
    Copyright (C) 2016-2023 darktable developers.

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

// These limits clamp away insane memory requirements.  They should reasonably faithfully represent the full
// precision though, so tiling will help reduce the memory footprint and export will look the same as darkroom
// mode (only 1mpix there).
#define DT_COMMON_BILATERAL_MAX_RES_S 3000
#define DT_COMMON_BILATERAL_MAX_RES_R 50

void dt_bilateral_grid_size(dt_bilateral_t *b,
                            const int width,
                            const int height,
                            const float L_range,
                            float sigma_s,
                            const float sigma_r)
{
  // Callers adjust sigma_s to account for image scaling to make the
  // bilateral filter scale-invariant.  As a result, if the user sets
  // a small enough value for sigma, we can get sigma_s substantially
  // below 1.0.  Values < 1 generate a bilateral grid with spatial
  // dimensions larger than the (scaled) image pixel dimensions; for
  // sigma_s < 0.5, there is at least one unused grid point between
  // any two used points, and thus the gaussian blur will have little
  // effect.  So we force sigma_s to be at least 0.5 to avoid an
  // excessively large grid.
  if(sigma_s < 0.5) sigma_s = 0.5;

  // compute an initial grid size, clamping away insanely large grids
  float _x = CLAMPS((int)roundf(width / sigma_s), 4, DT_COMMON_BILATERAL_MAX_RES_S);
  float _y = CLAMPS((int)roundf(height / sigma_s), 4, DT_COMMON_BILATERAL_MAX_RES_S);
  float _z = CLAMPS((int)roundf(L_range / sigma_r), 4, DT_COMMON_BILATERAL_MAX_RES_R);
  // If we clamped the X or Y dimensions, the sigma_s for that
  // dimension changes.  Since we need to use the same value in both
  // dimensions, compute the effective sigma_s for the grid.
  b->sigma_s = MAX(height / _y, width / _x);
  b->sigma_r = L_range / _z;
  // Compute the grid size in light of the actual adjusted values for
  // sigma_s and sigma_r
  b->size_x = (int)ceilf(width / b->sigma_s) + 1;
  b->size_y = (int)ceilf(height / b->sigma_s) + 1;
  b->size_z = (int)ceilf(L_range / b->sigma_r) + 1;
#if 0
  if(b->sigma_s != sigma_s)
    dt_print(DT_DEBUG_ALWAYS,
             "[bilateral] clamped sigma_s (%g -> %g)!\n",sigma_s,b->sigma_s);
  if(b->sigma_r != sigma_r)
    dt_print(DT_DEBUG_ALWAYS,
             "[bilateral] clamped sigma_r (%g -> %g)!\n",sigma_r,b->sigma_r);
#endif
}

size_t dt_bilateral_memory_use(const int width,     // width of input image
                               const int height,    // height of input image
                               const float sigma_s, // spatial sigma (blur pixel coords)
                               const float sigma_r) // range sigma (blur luma values)
{
  dt_bilateral_t b;
  dt_bilateral_grid_size(&b,width,height,100.0f,sigma_s,sigma_r);
  size_t grid_size = b.size_x * b.size_y * b.size_z;
#ifdef HAVE_OPENCL
  // OpenCL path needs two buffers
  return 2 * grid_size * sizeof(float);
#else
  return (grid_size + 3 * dt_get_num_threads() * b.size_x * b.size_z) * sizeof(float);
#endif /* HAVE_OPENCL */
}

#ifndef HAVE_OPENCL
// for the CPU path this is just an alias as no additional temp buffer is needed
// when compiling with OpenCL, version in bilateralcl.c takes precedence
size_t dt_bilateral_memory_use2(const int width,
                                const int height,
                                const float sigma_s,
                                const float sigma_r)
{
  return dt_bilateral_memory_use(width, height, sigma_s, sigma_r);
}
#endif /* !HAVE_OPENCL */

size_t dt_bilateral_singlebuffer_size
  (const int width,     // width of input image
   const int height,    // height of input image
   const float sigma_s, // spatial sigma (blur pixel coords)
   const float sigma_r) // range sigma (blur luma values)
{
  dt_bilateral_t b;
  dt_bilateral_grid_size(&b,width,height,100.0f,sigma_s,sigma_r);
  size_t grid_size = b.size_x * b.size_y * b.size_z;
  return (grid_size + 3 * dt_get_num_threads() * b.size_x * b.size_z) * sizeof(float);
}

#ifndef HAVE_OPENCL
// for the CPU path this is just an alias as no additional temp buffer is needed
// when compiling with OpenCL, version in bilateralcl.c takes precedence
size_t dt_bilateral_singlebuffer_size2(const int width,
                                       const int height,
                                       const float sigma_s,
                                       const float sigma_r)
{
  return dt_bilateral_singlebuffer_size(width, height, sigma_s, sigma_r);
}
#endif /* !HAVE_OPENCL */

static size_t image_to_grid(const dt_bilateral_t *const b,
                            const int i,
                            const int j,
                            const float L,
                            float *xf,
                            float *yf,
                            float *zf)
{
  float x = CLAMPS(i / b->sigma_s, 0, b->size_x - 1);
  float y = CLAMPS(j / b->sigma_s, 0, b->size_y - 1);
  float z = CLAMPS(L / b->sigma_r, 0, b->size_z - 1);
  const int xi = MIN((int)x, b->size_x - 2);
  const int yi = MIN((int)y, b->size_y - 2);
  const int zi = MIN((int)z, b->size_z - 2);
  *xf = x - xi;
  *yf = y - yi;
  *zf = z - zi;
  return ((xi + yi * b->size_x) * b->size_z) + zi;
}

static size_t image_to_relgrid(const dt_bilateral_t *const b,
                               const int i,
                               const float L,
                               float *xf,
                               float *zf)
{
  float x = CLAMPS(i / b->sigma_s, 0, b->size_x - 1);
  float z = CLAMPS(L / b->sigma_r, 0, b->size_z - 1);
  const int xi = MIN((int)x, b->size_x - 2);
  const int zi = MIN((int)z, b->size_z - 2);
  *xf = x - xi;
  *zf = z - zi;
  return (xi * b->size_z) + zi;
}

dt_bilateral_t *dt_bilateral_init(const int width,     // width of input image
                                  const int height,    // height of input image
                                  const float sigma_s, // spatial sigma (blur pixel coords)
                                  const float sigma_r) // range sigma (blur luma values)
{
  dt_bilateral_t *b = (dt_bilateral_t *)malloc(sizeof(dt_bilateral_t));
  if(!b) return NULL;
  dt_bilateral_grid_size(b,width,height,100.0f,sigma_s,sigma_r);
  b->width = width;
  b->height = height;
  b->numslices = dt_get_num_threads();
  b->sliceheight = (height + b->numslices - 1) / b->numslices;
  b->slicerows = (b->size_y + b->numslices - 1) / b->numslices + 2;
  b->buf = dt_calloc_align_float(b->size_x * b->size_z * b->numslices * b->slicerows);
  if(!b->buf)
  {
    dt_print(DT_DEBUG_ALWAYS,
             "[bilateral] unable to allocate buffer for %zux%zux%zu grid\n",
             b->size_x,b->size_y,b->size_z);
    free(b);
    return NULL;
  }
  dt_print(DT_DEBUG_DEV,
           "[bilateral] created grid [%ld %ld %ld] with sigma (%f %f) (%f %f)\n",
           b->size_x, b->size_y, b->size_z, b->sigma_s, sigma_s, b->sigma_r, sigma_r);
  return b;
}

#ifdef _OPENMP
#pragma omp declare simd aligned(in:64)
#endif
void dt_bilateral_splat(const dt_bilateral_t *b, const float *const in)
{
  const int ox = b->size_z;
  const int oy = b->size_x * b->size_z;
  const int oz = 1;
  const float sigma_s = b->sigma_s * b->sigma_s;
  float *const buf = b->buf;

  if(!buf) return;
  // splat into downsampled grid
  const int nthreads = dt_get_num_threads();
  const size_t offsets[8] =
  {
    0,
    ox,
    oy,
    ox + oy,
    oz,
    oz + ox,
    oz + oy,
    oz + oy + ox
  };

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(in, oy, oz, ox, sigma_s, buf, offsets) \
  shared(b)
#endif
  for(int slice = 0; slice < b->numslices; slice++)
  {
    const int firstrow = slice * b->sliceheight;
    const int lastrow = MIN((slice+1)*b->sliceheight,b->height);
    // compute the first row of the final grid which this slice
    // splats, and subtract that from the first row the current thread
    // should use to get an offset
    const int slice_offset = slice * b->slicerows - (int)(firstrow / b->sigma_s);
    // now iterate over the rows of the current horizontal slice
    for(int j = firstrow; j < lastrow; j++)
    {
      float y = CLAMPS(j / b->sigma_s, 0, b->size_y - 1);
      const int yi = MIN((int)y, b->size_y - 2);
      const float yf = y - yi;
      const size_t base = (size_t)(yi + slice_offset) * oy;
      for(int i = 0; i < b->width; i++)
      {
        size_t index = 4 * (j * b->width + i);
        float xf, zf;
        const float L = in[index];
        // nearest neighbour splatting:
        const size_t grid_index = base + image_to_relgrid(b, i, L, &xf, &zf);
        // sum up payload here
        const dt_aligned_pixel_t contrib =
        {
          // precompute the contributions along the first two dimensions:
          (1.0f - xf) * (1.0f - yf) * 100.0f / sigma_s,
          xf * (1.0f - yf) * 100.0f / sigma_s,
          (1.0f - xf) * yf * 100.0f / sigma_s,
          xf * yf * 100.0f / sigma_s
        };
#ifdef _OPENMP
#pragma omp simd aligned(buf:64)
#endif
        for(int k = 0; k < 4; k++)
        {
          buf[grid_index + offsets[k]] += (contrib[k] * (1.0f - zf));
          buf[grid_index + offsets[k+4]] += (contrib[k] * zf);
        }
      }
    }
  }

  // merge the per-thread results into the final result
  for(int slice = 1 ; slice < nthreads; slice++)
  {
    // compute the first row of the final grid which this slice splats
    const int destrow = (int)(slice * b->sliceheight / b->sigma_s);
    float *dest = buf + destrow * oy;
    // now iterate over the grid rows splatted for this slice
    for(int j = slice * b->slicerows; j < (slice+1)*b->slicerows; j++)
    {
      float *src = buf + j * oy;
      for(int i = 0; i < oy; i++)
      {
        dest[i] += src[i];
      }
      dest += oy;
      // clear elements in the part of the buffer which holds the
      // final result now that we've read the partial result, since
      // we'll be adding to those locations later
      if(j < b->size_y)
        memset(buf + j*oy, '\0', sizeof(float) * oy);
    }
  }
}

#ifdef _OPENMP
#pragma omp declare simd aligned(buf:64)
#endif
static void blur_line_z(float *buf,
                        const int offset1,
                        const int offset2,
                        const int offset3,
                        const int size1,
                        const int size2,
                        const int size3)
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
        buf[index] = +w1 * (buf[index + offset3] - tmp2)
          + w2 * (buf[index + 2 * offset3] - tmp1);
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
static void blur_line(float *buf,
                      const int offset1,
                      const int offset2,
                      const int offset3,
                      const int size1,
                      const int size2,
                      const int size3)
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
      buf[index] = buf[index] * w0 + w1 * buf[index + offset3]
        + w2 * buf[index + 2 * offset3];
      index += offset3;
      float tmp2 = buf[index];
      buf[index] = buf[index] * w0 + w1 * (buf[index + offset3] + tmp1)
        + w2 * buf[index + 2 * offset3];
      index += offset3;
      for(int i = 2; i < size3 - 2; i++)
      {
        const float tmp3 = buf[index];
        buf[index]
            = buf[index] * w0 + w1 * (buf[index + offset3] + tmp2)
          + w2 * (buf[index + 2 * offset3] + tmp1);
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


void dt_bilateral_blur(const dt_bilateral_t *b)
{
  if(!b || !b->buf)
    return;

  const int ox = b->size_z;
  const int oy = b->size_x * b->size_z;
  const int oz = 1;
  // gaussian up to 3 sigma
  blur_line(b->buf, oz, oy, ox, b->size_z, b->size_y, b->size_x);
  // gaussian up to 3 sigma
  blur_line(b->buf, oz, ox, oy, b->size_z, b->size_x, b->size_y);
  // -2 derivative of the gaussian up to 3 sigma: x*exp(-x*x)
  blur_line_z(b->buf, ox, oy, oz, b->size_x, b->size_y, b->size_z);
}


#ifdef _OPENMP
#pragma omp declare simd aligned(out, in :64)
#endif
void dt_bilateral_slice(const dt_bilateral_t *const b,
                        const float *const in,
                        float *out,
                        const float detail)
{
  // detail: 0 is leave as is, -1 is bilateral filtered, +1 is contrast boost
  const float norm = -detail * b->sigma_r * 0.04f;
  const int ox = b->size_z;
  const int oy = b->size_x * b->size_z;
  const int oz = 1;
  float *const buf = b->buf;
  const int width = b->width;
  const int height = b->height;

  if(!buf) return;
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(b, in, out, norm, ox, oy, oz, height, width, buf)  \
  collapse(2)
#endif
  for(int j = 0; j < height; j++)
  {
    for(int i = 0; i < width; i++)
    {
      size_t index = 4 * (j * width + i);
      float xf, yf, zf;
      const float L = in[index];
      // trilinear lookup:
      const size_t gi = image_to_grid(b, i, j, L, &xf, &yf, &zf);
      const float Lout = fmaxf( 0.0f, L
                         + norm * (buf[gi] * (1.0f - xf) * (1.0f - yf) * (1.0f - zf)
                                   + buf[gi + ox] * (xf) * (1.0f - yf) * (1.0f - zf)
                                   + buf[gi + oy] * (1.0f - xf) * (yf) * (1.0f - zf)
                                   + buf[gi + ox + oy] * (xf) * (yf) * (1.0f - zf)
                                   + buf[gi + oz] * (1.0f - xf) * (1.0f - yf) * (zf)
                                   + buf[gi + ox + oz] * (xf) * (1.0f - yf) * (zf)
                                   + buf[gi + oy + oz] * (1.0f - xf) * (yf) * (zf)
                                   + buf[gi + ox + oy + oz] * (xf) * (yf) * (zf)));
      // copy color and mask, then update L
      copy_pixel(out + index, in + index);
      out[index] = Lout;
    }
  }
}

#ifdef _OPENMP
#pragma omp declare simd aligned(out, in :64)
#endif
void dt_bilateral_slice_to_output(const dt_bilateral_t *const b,
                                  const float *const in,
                                  float *out,
                                  const float detail)
{
  // detail: 0 is leave as is, -1 is bilateral filtered, +1 is contrast boost
  const float norm = -detail * b->sigma_r * 0.04f;
  const int ox = b->size_z;
  const int oy = b->size_x * b->size_z;
  const int oz = 1;
  float *const buf = b->buf;
  const int width = b->width;
  const int height = b->height;

  if(!buf) return;
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(b, in, out, norm, oy, oz, ox, buf, width, height)  \
  collapse(2)
#endif
  for(int j = 0; j < height; j++)
  {
    for(int i = 0; i < width; i++)
    {
      size_t index = 4 * (j * width + i);
      float xf, yf, zf;
      const float L = in[index];
      // trilinear lookup:
      const size_t gi = image_to_grid(b, i, j, L, &xf, &yf, &zf);
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

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
