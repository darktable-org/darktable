/*
    This file is part of darktable,
    Copyright (C) 2009-2020 darktable developers.

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
#include <assert.h>
#include <math.h>
#include <stdlib.h>

#include "common/box_filters.h"
#include "common/darktable.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#if defined(__SSE__)
#include <xmmintrin.h>
#endif

static void blur_horizontal_1ch(float *const restrict buf, const int height, const int width, const int radius,
                                float *const restrict scanlines)
{
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(radius, height, width) \
  dt_omp_sharedconst(buf, scanlines) \
  schedule(static)
#endif
  for(int y = 0; y < height; y++)
  {
    float L = 0;
    int hits = 0;
    const size_t index = (size_t)y * width;
    float *const restrict scanline = scanlines + dt_get_thread_num() * width;
    // add up the left half of the window
    for (int x = 0; x < radius && x < width ; x++)
    {
      L += buf[index+x];
      hits++;
    }
    // process the blur up to the point where we start removing values
    int x;
    for (x = 0; x <= radius && x < width; x++)
    {
      const int np = x + radius;
      if(np < width)
      {
        L += buf[index + np];
        hits++;
      }
      scanline[x] = L / hits;
    }
    // process the blur for the bulk of the scan line
    for(; x + radius < width; x++)
    {
      const int op = x - radius - 1;
      const int np = x + radius;
      L -= buf[index + op];
      L += buf[index + np];
      scanline[x] = L / hits;
    }
    // process the right end where we have no more values to add to the running sum
    for(; x < width; x++)
    {
      const int op = x - radius - 1;
      L -= buf[index + op];
      hits--;
      scanline[x] = L / hits;
    }
    // copy blurred values back to original location in buffer
    for(x = 0; x < width; x++)
      buf[index + x] = scanline[x];
  }
  return;
}

static void blur_horizontal_2ch(float *const restrict buf, const int height, const int width, const int radius,
                                float *const restrict scanlines)
{
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(radius, height, width) \
  dt_omp_sharedconst(buf, scanlines) \
  schedule(static)
#endif
  for(int y = 0; y < height; y++)
  {
    float *const restrict scanline = scanlines + 2 * dt_get_thread_num() * width;
    float L1 = 0.0f, L2 = 0.0f;
    int hits = 0;
    const size_t index = (size_t)2 * y * width;
    // add up the left half of the window
    for (int x = 0; x < radius && x < width ; x++)
    {
      hits++;
      L1 += buf[index + 2*x];
      L2 += buf[index + 2*x + 1];
    }
    // process the blur up to the point where we start removing values
    int x;
    for (x = 0; x <= radius && x < width; x++)
    {
      const int np = x + radius;
      if(np < width)
      {
        hits++;
        L1 += buf[index + 2*np];
        L2 += buf[index + 2*np + 1];
      }
      scanline[2*x] = L1 / hits;
      scanline[2*x+1] = L2 / hits;
    }
    // process the blur for the bulk of the scan line
    for(; x + radius < width; x++)
    {
      const int op = x - radius - 1;
      const int np = x + radius;
      L1 = L1 - buf[index + 2*op] + buf[index + 2*np];
      L2 = L2 - buf[index + 2*op + 1] + buf[index + 2*np + 1];
      scanline[2*x] = L1 / hits;
      scanline[2*x+1] = L2 / hits;
    }
    // process the right end where we have no more values to add to the running sum
    for(; x < width; x++)
    {
      const int op = x - radius - 1;
      hits--;
      L1 -= buf[index + 2*op];
      L2 -= buf[index + 2*op + 1];
      scanline[2*x] = L1 / hits;
      scanline[2*x+1] = L2 / hits;
    }
    // copy blurred values back to original location in buffer
    for(x = 0; x < 2*width; x++)
    {
      buf[index + x] = scanline[x];
    }
  }
  return;
}

static void blur_horizontal_4ch(float *const restrict buf, const int height, const int width, const int radius,
                                float *const restrict scanlines)
{
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(radius, height, width) \
  dt_omp_sharedconst(buf, scanlines) \
  schedule(static)
#endif
  for(int y = 0; y < height; y++)
  {
    float *const restrict scanline = scanlines + 4 * dt_get_thread_num() * width;
    float DT_ALIGNED_PIXEL L[4] = { 0, 0, 0, 0 };
    int hits = 0;
    const size_t index = (size_t)4 * y * width;
    // add up the left half of the window
    for (int x = 0; x < radius && x < width ; x++)
    {
      hits++;
#ifdef _OPENMP
#pragma omp simd aligned(buf)
#endif
      for (int c = 0; c < 4; c++)
        L[c] += buf[index+4*x + c];
    }
    // process the blur up to the point where we start removing values
    int x;
    for (x = 0; x <= radius && x < width; x++)
    {
      const int np = x + radius;
      if(np < width)
      {
        hits++;
#ifdef _OPENMP
#pragma omp simd aligned(buf)
#endif
        for (int c = 0; c < 4; c++)
          L[c] += buf[index + 4*np + c];
      }
#ifdef _OPENMP
#pragma omp simd aligned(scanline)
#endif
      for (int c = 0; c < 4; c++)
        scanline[4*x + c] = L[c] / hits;
    }
    // process the blur for the bulk of the scan line
    for(; x + radius < width; x++)
    {
      const int op = x - radius - 1;
      const int np = x + radius;
#ifdef _OPENMP
#pragma omp simd aligned(buf, scanline)
#endif
      for (int c = 0; c < 4; c++)
      {
        L[c] -= buf[index + 4*op + c];
        L[c] += buf[index + 4*np + c];
        scanline[4*x + c] = L[c] / hits;
      }
    }
    // process the right end where we have no more values to add to the running sum
    for(; x < width; x++)
    {
      const int op = x - radius - 1;
      hits--;
#ifdef _OPENMP
#pragma omp simd aligned(buf, scanline)
#endif
      for (int c = 0; c < 4; c++)
      {
        L[c] -= buf[index + 4*op + c];
        scanline[4*x + c] = L[c] / hits;
      }
    }
    // copy blurred values back to original location in buffer
    for(x = 0; x < width; x++)
    {
#ifdef _OPENMP
#pragma omp simd aligned(buf, scanline)
#endif
      for (int c = 0; c < 4; c++)
        buf[index + 4*x + c] = scanline[4*x + c];
    }
  }
  return;
}

#ifdef __SSE2__
static void blur_vertical_1ch_sse(float *const restrict buf, const int height, const int width, const int radius,
                                  float *const restrict scanline)
{
  __m128 L = { 0, 0, 0, 0 };
  __m128 hits = { 0, 0, 0, 0 };
  const __m128 one = { 1.0f, 1.0f, 1.0f, 1.0f };
  // add up the top half of the window
  for (size_t y = 0; y < radius && y < height; y++)
  {
    size_t index = y * width;
    L += _mm_loadu_ps(buf + index);	// use unaligned load since width is not necessarily a multiple of 4
    hits += one;
  }
  // process the blur up to the point where we start removing values
  for (size_t y = 0; y <= radius && y < height; y++)
  {
    const int np = y + radius;
    if(np < height)
    {
      L += _mm_loadu_ps(buf+np*width);
      hits += one;
    }
    _mm_store_ps(scanline+4*y, L / hits);
  }
  // process the blur for the rest of the scan line
  for (size_t y = radius+1 ; y < height; y++)
  {
    const int op = y - radius - 1;
    const int np = y + radius;
    L -= _mm_loadu_ps(buf+op*width);
    hits -= one;
    if(np < height)
    {
      L += _mm_loadu_ps(buf+np*width);
      hits += one;
    }
    _mm_store_ps(scanline+4*y, L / hits);
  }

  // copy blurred values back to original location in buffer
  for (size_t y = 0; y < height; y++)
  {
    // use unaligned store since width is not necessarily a multiple of four
    // use the faster aligned load since we've ensured that scanline is aligned
    _mm_storeu_ps(buf + y*width, _mm_load_ps(scanline + 4*y));
  }
  return;
}
#endif /* __SSE2__ */

#ifdef __SSE2__
static void blur_vertical_4ch_sse(float *const restrict buf, const int height, const int width, const int radius,
                                  __m128 *const restrict scanline)
{
  __m128 L[4] = { _mm_set1_ps(0), _mm_set1_ps(0), _mm_set1_ps(0), _mm_set1_ps(0) };
  __m128 hits = _mm_set1_ps(0);
  const __m128 one = { 1.0f, 1.0f, 1.0f, 1.0f };
  // add up the top half of the window
  for (size_t y = 0; y < radius && y < height; y++)
  {
    size_t index = y * width;
    for (int c = 0; c < 4; c++)
      L[c] += _mm_loadu_ps(buf + 4 * (index + c)); // use unaligned load since width is not necessarily a multiple of 4
    hits += one;
  }
  // process the blur up to the point where we start removing values
  for (size_t y = 0; y <= radius && y < height; y++)
  {
    const int np = y + radius;
    if(np < height)
    {
      for (int c = 0; c < 4; c++)
        L[c] += _mm_loadu_ps(buf + 4 * (np*width + c));
      hits += one;
    }
    for (int c = 0; c < 4; c++)
      scanline[4*y+c] = L[c] / hits;
  }
  // process the blur for the rest of the scan line
  for (size_t y = radius+1 ; y < height; y++)
  {
    const int op = y - radius - 1;
    const int np = y + radius;
    for (int c = 0; c < 4; c++)
      L[c] -= _mm_loadu_ps(buf + 4 * (op*width + c));
    hits -= one;
    if(np < height)
    {
      for (int c = 0; c < 4; c++)
        L[c] += _mm_loadu_ps(buf + 4 * (np*width + c));
      hits += one;
    }
    for (int c = 0; c < 4; c++)
      scanline[4*y+c] = L[c] / hits;
  }

  // copy blurred values back to original location in buffer
  for (size_t y = 0; y < height; y++)
  {
    // use unaligned store since width is not necessarily a multiple of four
    // use the faster aligned load since we've ensured that scanline is aligned
    for (int c = 0; c < 4; c++)
      _mm_storeu_ps(buf + 4 * (y*width+c), scanline[4*y+c]);
  }
  return;
}
#endif /* __SSE2__ */

// invoked inside an OpenMP parallel for, so no need to parallelize
static void blur_vertical_4wide(float *const restrict buf, const int height, const int width, const int radius,
                                float *const restrict scanline)
{
#ifdef __SSE2__
  if (darktable.codepath.SSE2)
  {
    blur_vertical_1ch_sse(buf, height, width, radius, scanline);
    return;
  }
#endif /* __SSE2__ */

  float DT_ALIGNED_PIXEL L[4] = { 0, 0, 0, 0 };
  int hits = 0;
  // add up the left half of the window
  for (size_t y = 0; y < radius && y < height; y++)
  {
    size_t index = y * width;
    hits++;
#ifdef _OPENMP
#pragma omp simd aligned(buf : 16)
#endif
    for (int c = 0; c < 4; c++)
    {
      L[c] += buf[index+c];
    }
  }
  // process the blur up to the point where we start removing values
  for (size_t y = 0; y <= radius && y < height; y++)
  {
    const int np = y + radius;
    const int npoffset = np * width;
    if(np < height)
    {
      hits++;
#ifdef _OPENMP
#pragma omp simd aligned(buf : 16)
#endif
      for (int c = 0; c < 4; c++)
        L[c] += buf[npoffset+c];
    }
#ifdef _OPENMP
#pragma omp simd aligned(scanline : 16)
#endif
    for (int c = 0; c < 4; c++)
    {
      scanline[4*y + c] = L[c] / hits;
    }
  }
  // process the blur for the rest of the scan line
  for (size_t y = radius+1; y < height; y++)
  {
    const int op = y - radius - 1;
    const int np = y + radius;
    const int opoffset = op * width;
    const int npoffset = np * width;
    if(op >= 0)
    {
      hits--;
#ifdef _OPENMP
#pragma omp simd aligned(buf : 16)
#endif
      for (int c = 0; c < 4; c++)
        L[c] -= buf[opoffset + c];
    }
    if(np < height)
    {
      hits++;
#ifdef _OPENMP
#pragma omp simd aligned(buf : 16)
#endif
      for (int c = 0; c < 4; c++)
        L[c] += buf[npoffset + c];
    }
#ifdef _OPENMP
#pragma omp simd aligned(scanline : 16)
#endif
    for (int c = 0; c < 4; c++)
      scanline[4*y + c] = L[c] / hits;
  }

  // copy blurred values back to original location in buffer
  for (size_t y = 0; y < height; y++)
  {
#ifdef _OPENMP
#pragma omp simd aligned(buf, scanline : 16)
#endif
    for (int c = 0; c < 4; c++)
      buf[y * width + c] = scanline[4*y + c];
  }
  return;
}

static void blur_vertical_1ch(float *const restrict buf, const int height, const int width, const int radius,
                              float *const restrict scanlines)
{
  /* vertical pass on L channel */
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(radius, height, width) \
  dt_omp_sharedconst(buf, scanlines) \
  schedule(static)
#endif
  for(int x = 0; x < (width & ~3); x += 4)
  {
    float *const restrict scanline = scanlines + 4 * dt_get_thread_num() * height;
    blur_vertical_4wide(buf + x, height, width, radius, scanline);
  }
  const int opoffs = -(radius + 1) * width;
  const int npoffs = radius*width;
  for(int x = width & ~3; x < width; x++)
  {
    float L = 0.0f;
    int hits = 0;
    size_t index = (size_t)x - (size_t)radius * width;
    float *const restrict scanline = scanlines;
    for(int y = -radius; y < height; y++)
    {
      const int op = y - radius - 1;
      const int np = y + radius;
      if(op >= 0)
      {
        L -= buf[index + opoffs];
        hits--;
      }
      if(np < height)
      {
        L += buf[index + npoffs];
        hits++;
      }
      if(y >= 0) scanline[y] = L / hits;
      index += width;
    }

    for(int y = 0; y < height; y++)
      buf[(size_t)y * width + x] = scanline[y];
  }
  return;
}

static void dt_box_mean_1ch(float *const buf, const int height, const int width, const int radius,
                            const int iterations)
{
  const int size = MAX(width,height);
  float *const restrict scanlines = dt_alloc_align_float(dt_get_num_threads() * size * 4);

  for(int iteration = 0; iteration < iterations; iteration++)
  {
    blur_horizontal_1ch(buf, height, width, radius, scanlines);
    blur_vertical_1ch(buf, height, width, radius, scanlines);
  }

  dt_free_align(scanlines);
}

static void dt_box_mean_4ch(float *const buf, const int height, const int width, const int radius,
                            const int iterations)
{
  const int size = MAX(width,height);
  float *const restrict scanlines = dt_alloc_align_float(dt_get_num_threads() * size * 4);

  for(int iteration = 0; iteration < iterations; iteration++)
  {
    blur_horizontal_4ch(buf, height, width, radius, scanlines);
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(width, height, radius)  \
  dt_omp_sharedconst(buf, scanlines) \
  schedule(static)
#endif
    for (int col = 0; col < width; col++)
    {
      float *const restrict scanline = scanlines + 4 * dt_get_thread_num() * height;
      // we need to multiply width by 4 to get the correct stride for the vertical blur
      blur_vertical_4wide(buf + 4 * col, height, 4*width, radius, scanline);
    }
  }

  dt_free_align(scanlines);
}

#ifdef __SSE2__
static void dt_box_mean_4ch_sse(float *const buf, const int height, const int width, const int radius,
                                const int iterations)
{
  const int size = MAX(width,height);

  __m128 *const scanline_buf = dt_alloc_align(64, sizeof(__m128) * dt_get_num_threads() * size * 4);

  for(int iteration = 0; iteration < BOX_ITERATIONS; iteration++)
  {
    blur_horizontal_4ch(buf, height, width, radius, (float*)scanline_buf);

    /* vertical pass; start by doing four columns of pixels at a time */
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(width, height, radius)  \
  dt_omp_sharedconst(buf, scanline_buf) \
  schedule(static)
#endif
    for (int col = 0; col < (width & ~3); col += 4)
    {
      __m128 *const restrict scanline = scanline_buf + 4 * dt_get_thread_num() * height;
      blur_vertical_4ch_sse(buf + 4 * col, height, width, radius, scanline);
    }
    // finish up the leftover 0-3 columns of pixels
    const int opoffs = -(radius + 1) * width;
    const int npoffs = (radius)*width;
    for(int x = (width & ~3); x < width; x++)
    {
      __m128 *scanline = scanline_buf + size * dt_get_thread_num();
      __m128 L = _mm_setzero_ps();
      int hits = 0;
      size_t index = (size_t)x - (size_t)radius * width;
      for(int y = -radius; y < height; y++)
      {
        int op = y - radius - 1;
        int np = y + radius;

        if(op >= 0)
        {
          L = L - _mm_load_ps(&buf[(index + opoffs) * 4]);
          hits--;
        }
        if(np < height)
        {
          L = L + _mm_load_ps(&buf[(index + npoffs) * 4]);
          hits++;
        }
        if(y >= 0) scanline[y] = L / _mm_set_ps1(hits);
        index += width;
      }

      for(int y = 0; y < height; y++)
        _mm_store_ps(&buf[((size_t)y * width + x) * 4], scanline[y]);
    }
  }

  dt_free_align(scanline_buf);
}
#endif /* __SSE2__ */

static inline void box_mean_2ch(float *const restrict in, const size_t height, const size_t width,
                                const int radius, const int iterations)
{
  // Compute in-place a box average (filter) on a multi-channel image over a window of size 2*radius + 1
  // We make use of the separable nature of the filter kernel to speed-up the computation
  // by convolving along columns and rows separately (complexity O(2 × radius) instead of O(radius²)).

  const size_t Ndim = MAX(width,height) * 2 * 2;
  float *const restrict temp = dt_alloc_align_float(Ndim * dt_get_num_threads());
  if (temp == NULL) return;

  for (int iteration = 0; iteration < iterations; iteration++)
  {
    blur_horizontal_2ch(in, height, width, radius, temp);
    blur_vertical_1ch(in, height, 2*width, radius, temp);
  }
  dt_free_align(temp);
}

void dt_box_mean(float *const buf, const int height, const int width, const int ch,
                 const int radius, const int iterations)
{
  if (ch == 1)
  {
    dt_box_mean_1ch(buf,height,width,radius,iterations);
  }
  else if (ch == 4)
  {
#ifdef __SSE__
    if (darktable.codepath.SSE2)
    {
      dt_box_mean_4ch_sse(buf,height,width,radius,iterations);
    }
    else
#endif
      dt_box_mean_4ch(buf,height,width,radius,iterations);
  }
  else if (ch == 2) // used by fast_guided_filter.h
  {
    box_mean_2ch(buf,height,width,radius,iterations);
  }
}


// calculate the one-dimensional moving maximum over a window of size 2*w+1
// input array x has stride 1, output array y has stride stride_y
static inline void box_max_1d(int N, const float *const restrict x, float *const restrict y, size_t stride_y, int w)
{
  float m = -(INFINITY);
  for(int i = 0; i < MIN(w + 1, N); i++)
    m = MAX(x[i], m);
  for(int i = 0; i < N; i++)
  {
    // store maximum of current window at center position
    y[i * stride_y] = m;
    // if the earliest member of the current window is the max, we need to
    // rescan the window to determine the new maximum
    if(i - w >= 0 && x[i - w] == m)
    {
      m = -(INFINITY);
      for(int j = i - w + 1; j < MIN(i + w + 2, N); j++)
        m = MAX(x[j], m);
    }
    // if the window has not yet exceeded the end of the row/column, update the maximum value
    if(i + w + 1 < N)
      m = MAX(x[i + w + 1], m);
  }
}

// calculate the one-dimensional moving maximum on four adjacent columns over a window of size 2*w+1
// input array x has stride 16, output array y has stride stride_y and we will write 16 consecutive elements
//  every stride_y elements (thus processing a cache line at a time)
static inline void box_max_vert_16wide(const int N, const float *const restrict x, float *const restrict y,
                                      const int stride_y, const int w)
{
  float DT_ALIGNED_PIXEL m[16] = { -(INFINITY), -(INFINITY), -(INFINITY), -(INFINITY),
                                  -(INFINITY), -(INFINITY), -(INFINITY), -(INFINITY),
                                  -(INFINITY), -(INFINITY), -(INFINITY), -(INFINITY),
                                  -(INFINITY), -(INFINITY), -(INFINITY), -(INFINITY) };
  for(int i = 0; i < MIN(w + 1, N); i++)
#ifdef _OPENMP
#pragma omp simd aligned(m, x)
#endif
    for (int c = 0; c < 16; c++)
      m[c] = MAX(x[16*i + c], m[c]);
  for(int i = 0; i < N; i++)
  {
    // store maximum of current window at center position
#ifdef _OPENMP
#pragma omp simd aligned(m, y)
#endif
    for (int c = 0; c < 16; c++)
      y[i * stride_y + c] = m[c];
    // If the earliest member of the current window is the max, we need to
    // rescan the window to determine the new maximum
    if (i >= w)
    {
#ifdef _OPENMP
#pragma omp simd aligned(m, x)
#endif
      for (int c = 0; c < 16; c++)
      {
//        if(x[16 * (i - w) + c] == m[c]) //prevents vectorization
        {
          m[c] = -(INFINITY);
          for(int j = i - w + 1; j < MIN(i + w + 2, N); j++)
            m[c] = MAX(x[16*j+c], m[c]);
        }
      }
    }
    // if the window has not yet exceeded the end of the row/column, update the maximum value
    if(i + w + 1 < N)
    {
#ifdef _OPENMP
#pragma omp simd aligned(m, x)
#endif
      for (int c = 0; c < 16; c++)
        m[c] = MAX(x[16 * (i + w + 1) + c], m[c]);
    }
  }
}

// calculate the two-dimensional moving maximum over a box of size (2*w+1) x (2*w+1)
// does the calculation in-place if input and output images are identical
static void box_max_1ch(float *const buf, const int height, const int width, const int w)
{
  float *const restrict scratch_buffers = dt_alloc_align_float(dt_get_num_threads() * MAX(width,16*height));
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(w, width, height, buf)    \
  dt_omp_sharedconst(scratch_buffers) \
  schedule(static)
#endif
  for(size_t row = 0; row < height; row++)
  {
    float *const restrict scratch = scratch_buffers + width * dt_get_thread_num();
    memcpy(scratch, buf + row * width, sizeof(float) * width);
    box_max_1d(width, scratch, buf + row * width, 1, w);
  }
#ifdef _OPENMP
#pragma omp parallel for default(none)           \
  dt_omp_firstprivate(w, width, height, buf) \
  dt_omp_sharedconst(scratch_buffers) \
  schedule(static)
#endif
  for(int col = 0; col < (width & ~15); col += 16)
  {
    float *const restrict scratch = scratch_buffers + 16 * height * dt_get_thread_num();
    for (size_t row = 0; row < height; row++)
      for (size_t c = 0; c < 16; c++)
        scratch[16*row+c] = buf[row * width + col + c];
    box_max_vert_16wide(height, scratch, buf + col, width, w);
  }
  for (size_t col = width & ~15 ; col < width; col++)
  {
    float *const restrict scratch = scratch_buffers;
    for(size_t row = 0; row < height; row++)
      scratch[row] = buf[row * width + col];
    box_max_1d(height, scratch, buf + col, width, w);
  }
  dt_free_align(scratch_buffers);
}


// in-place calculate the two-dimensional moving maximum over a box of size (2*radius+1) x (2*radius+1)
void dt_box_max(float *const buf, const int height, const int width, const int ch, const int radius)
{
  if (ch == 1)
    box_max_1ch(buf, height, width, radius);
  //TODO: 4ch version if needed
}

// calculate the one-dimensional moving minimum over a window of size 2*w+1
// input array x has stride 1, output array y has stride stride_y
static inline void box_min_1d(int N, const float *x, float *y, size_t stride_y, int w)
{
  float m = INFINITY;
  for(int i = 0; i < MIN(w + 1, N); i++)
    m = MIN(x[i], m);
  for(int i = 0; i < N; i++)
  {
    y[i * stride_y] = m;
    if(i - w >= 0 && x[i - w] == m)
    {
      m = INFINITY;
      for(int j = i - w + 1; j < MIN(i + w + 2, N); j++)
        m = MIN(x[j], m);
    }
    if(i + w + 1 < N)
      m = MIN(x[i + w + 1], m);
  }
}

// calculate the one-dimensional moving minimum on four adjacent columns over a window of size 2*w+1
// input array x has stride 16, output array y has stride stride_y and we will write 16 consecutive elements
//  every stride_y elements (thus processing a cache line at a time)
static inline void box_min_vert_16wide(const int N, const float *const restrict x, float *const restrict y,
                                      const int stride_y, const int w)
{
  float DT_ALIGNED_PIXEL m[16] = { INFINITY, INFINITY, INFINITY, INFINITY,
                                   INFINITY, INFINITY, INFINITY, INFINITY,
                                   INFINITY, INFINITY, INFINITY, INFINITY,
                                   INFINITY, INFINITY, INFINITY, INFINITY };
  for(int i = 0; i < MIN(w + 1, N); i++)
#ifdef _OPENMP
#pragma omp simd aligned(m, x)
#endif
    for (int c = 0; c < 16; c++)
      m[c] = MIN(x[16*i + c], m[c]);
  for(int i = 0; i < N; i++)
  {
    // store minimum of current window at center position
#ifdef _OPENMP
#pragma omp simd aligned(m, y)
#endif
    for (int c = 0; c < 16; c++)
      y[i * stride_y + c] = m[c];
    // If the earliest member of the current window is the min, we need to
    // rescan the window to determine the new minimum
    if (i >= w)
    {
#ifdef _OPENMP
#pragma omp simd aligned(m, x)
#endif
      for (int c = 0; c < 16; c++)
      {
//        if(x[16 * (i - w) + c] == m[c]) //prevents vectorization
        {
          m[c] = INFINITY;
          for(int j = i - w + 1; j < MIN(i + w + 2, N); j++)
            m[c] = MIN(x[16*j+c], m[c]);
        }
      }
    }
    // if the window has not yet exceeded the end of the row/column, update the minimum value
    if(i + w + 1 < N)
    {
#ifdef _OPENMP
#pragma omp simd aligned(m, x)
#endif
      for (int c = 0; c < 16; c++)
        m[c] = MIN(x[16 * (i + w + 1) + c], m[c]);
    }
  }
}


// calculate the two-dimensional moving minimum over a box of size (2*w+1) x (2*w+1)
// does the calculation in-place if input and output images are identical
static void box_min_1ch(float *const buf, const int height, const int width, const int w)
{
  float *const restrict scratch_buffers = dt_alloc_align_float(dt_get_num_threads() * MAX(width,16*height));
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(w, width, height, buf)    \
  dt_omp_sharedconst(scratch_buffers) \
  schedule(static)
#endif
  for(size_t row = 0; row < height; row++)
  {
    float *const restrict scratch = scratch_buffers + width * dt_get_thread_num();
    memcpy(scratch, buf + row * width, sizeof(float) * width);
    box_min_1d(width, scratch, buf + row * width, 1, w);
  }
#ifdef _OPENMP
#pragma omp parallel for default(none)           \
  dt_omp_firstprivate(w, width, height, buf) \
  dt_omp_sharedconst(scratch_buffers) \
  schedule(static)
#endif
  for(int col = 0; col < (width & ~15); col += 16)
  {
    float *const restrict scratch = scratch_buffers + 16 * height * dt_get_thread_num();
    for (size_t row = 0; row < height; row++)
      for (size_t c = 0; c < 16; c++)
        scratch[16*row+c] = buf[row * width + col + c];
    box_min_vert_16wide(height, scratch, buf + col, width, w);
  }
  for (size_t col = width & ~15 ; col < width; col++)
  {
    float *const restrict scratch = scratch_buffers;
    for(size_t row = 0; row < height; row++)
      scratch[row] = buf[row * width + col];
    box_min_1d(height, scratch, buf + col, width, w);
  }

  dt_free_align(scratch_buffers);
}

void dt_box_min(float *const buf, const int height, const int width, const int ch, const int radius)
{
  if (ch == 1)
    box_min_1ch(buf, height, width, radius);
  //TODO: 4ch version if needed
}
