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

#ifdef __GNUC__
#pragma GCC optimize ("finite-math-only")
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

// Put the to-be-vectorized loop into a function by itself to nudge the compiler into actually vectorizing...
// With optimization enabled, this gets inlined and interleaved with other instructions as though it had been
// written in place, so we get a net win from better vectorization.
static void add_4wide(float *const restrict accum, const float *const restrict values)
{
  for_four_channels(c,aligned(accum,values))
    accum[c] += values[c];
}

// Put the to-be-vectorized loop into a function by itself to nudge the compiler into actually vectorizing...
// With optimization enabled, this gets inlined and interleaved with other instructions as though it had been
// written in place, so we get a net win from better vectorization.
static void add_16wide(float *const restrict accum, const float *const restrict values)
{
#ifdef _OPENMP
#pragma omp simd aligned(accum : 64) aligned(values : 16)
#endif
  for(size_t c = 0; c < 16; c++)
    accum[c] += values[c];
}

static void sub_4wide(float *const restrict accum, const float *const restrict values)
{
  for_four_channels(c,aligned(accum,values))
    accum[c] -= values[c];
}

static void sub_16wide(float *const restrict accum, const float *const restrict values)
{
#ifdef _OPENMP
#pragma omp simd aligned(accum : 64) aligned(values : 16)
#endif
  for(size_t c = 0; c < 16; c++)
    accum[c] -= values[c];
}

// Copy the result back to the original buffer.  We don't declare 'out' to be aligned because this function
// can be used for a one-channel image whose width is not a multiple of 4, and makihg the compiler use an aligned
// vector store will result in crashes.
static void store_4wide(float *const restrict out, const float *const restrict in)
{
  for_four_channels(c,aligned(in : 16))
    out[c] = in[c];
}

// copy 16 floats from aligned temporary space back to the possibly-unaligned user buffer
static void store_16wide(float *const restrict out, const float *const restrict in)
{
#ifdef _OPENMP
#pragma omp simd aligned(in : 64)
#endif
  for (size_t c = 0; c < 16; c++)
    out[c] = in[c];
}

static void store_scaled_4wide(float *const restrict out, const float *const restrict in, const float scale)
{
  for_four_channels(c,aligned(in,out))
    out[c] = in[c] / scale;
}

static void store_scaled_16wide(float *const restrict out, const float *const restrict in, const float scale)
{
#ifdef _OPENMP
#pragma omp simd aligned(in,out : 64)
#endif
  for(size_t c = 0; c < 16; c++)
    out[c] = in[c] / scale;
}

static void blur_horizontal_4ch(float *const restrict buf, const size_t height, const size_t width, const size_t radius,
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
    size_t hits = 0;
    const size_t index = (size_t)4 * y * width;
    // add up the left half of the window
    for (size_t x = 0; x < MIN(radius,width) ; x++)
    {
      hits++;
      for_four_channels(c,aligned(buf))
        L[c] += buf[index+4*x + c];
    }
    // process the blur up to the point where we start removing values
    size_t x;
    for (x = 0; x <= MIN(radius, width-1); x++)
    {
      const int np = x + radius;
      if(np < width)
      {
        hits++;
        for_four_channels(c,aligned(buf))
          L[c] += buf[index + 4*np + c];
      }
      for_four_channels(c,aligned(scanline))
        scanline[4*x + c] = L[c] / hits;
    }
    // process the blur for the bulk of the scan line
    for(; x + radius < width; x++)
    {
      //very strange: if any of the 'op' or 'np' variables in this function are changed to either
      // 'unsigned' or 'size_t', the function runs a fair bit slower....
      const int op = x - radius - 1;
      const int np = x + radius;
      for_four_channels(c,aligned(buf, scanline))
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
      for_four_channels(c,aligned(buf, scanline))
      {
        L[c] -= buf[index + 4*op + c];
        scanline[4*x + c] = L[c] / hits;
      }
    }
    // copy blurred values back to original location in buffer
    for(x = 0; x < width; x++)
    {
      for_four_channels(c,aligned(buf, scanline))
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
  size_t y;
  for (y = 0; y <= MIN(radius, height-1); y++)
  {
    const int np = y + radius;
    hits += one;
    for (int c = 0; c < 4; c++)
      L[c] += _mm_loadu_ps(buf + 4 * (np*width + c));
    for (int c = 0; c < 4; c++)
      scanline[4*y+c] = L[c] / hits;
  }
  // process the blur for the bulk of the scan line
  for ( ; y < height-radius; y++)
  {
    const int op = y - radius - 1;
    const int np = y + radius;
    for (int c = 0; c < 4; c++)
    {
      L[c] -= _mm_loadu_ps(buf + 4 * (op*width + c));
      // we're now done with the orig value, so store the final result while this line is still in cache
      _mm_storeu_ps(buf + 4 * (op*width + c), scanline[4*op+c]);
      L[c] += _mm_loadu_ps(buf + 4 * (np*width + c));
      scanline[4*y+c] = L[c] / hits;
    }
  }
  // process the blur for the end of the scan line, where we don't have any more values to add to the mean
  for ( ; y < height; y++)
  {
    const int op = y - radius - 1;
    hits -= one;
    for (int c = 0; c < 4; c++)
    {
      L[c] -= _mm_loadu_ps(buf + 4 * (op*width + c));
      // we're now done with the orig value, so store the final result while this line is still in cache
      _mm_storeu_ps(buf + 4 * (op*width + c), scanline[4*op+c]);
    }
    for (int c = 0; c < 4; c++)
      scanline[4*y+c] = L[c] / hits;
  }

  // copy remaining blurred values back to original location in buffer
  for (y = (height>radius)?(height-radius-1):0; y < height; y++)
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
static void blur_vertical_4wide(float *const restrict buf, const size_t height, const size_t width, const size_t radius,
                                float *const restrict scratch)
{
#ifdef __SSE2__
  if (darktable.codepath.SSE2)
  {
    blur_vertical_1ch_sse(buf, height, width, radius, scratch);
    return;
  }
#endif /* __SSE2__ */

  // To improve cache hit rates, we copy the final result from the scratch space back to the original
  // location in the buffer as soon as we finish the final read of the buffer.  To reduce the working
  // set and further improve cache hits, we can treat the scratch space as a circular buffer and cycle
  // through it repeatedly.  To use a simple bitmask instead of a division, the size we cycle through
  // needs to be the power of two larger than the window size (2*radius+1).
  size_t mask = 1;
  for(size_t r = (2*radius+1); r > 1 ; r >>= 1) mask = (mask << 1) | 1;

  float DT_ALIGNED_PIXEL L[4] = { 0, 0, 0, 0 };
  size_t hits = 0;
  // add up the left half of the window
  for (size_t y = 0; y < MIN(radius, height); y++)
  {
    hits++;
    add_4wide(L, buf + y * width);
  }
  // process the blur up to the point where we start removing values
  size_t y;
  for (y = 0; y <= MIN(radius, height-radius-1); y++)
  {
    // weirdly, changing any of the 'np' or 'op' variables in this function to 'size_t' yields a substantial slowdown!
    const int np = y + radius;
    hits++;
    add_4wide(L, buf + np*width);
    store_scaled_4wide(scratch + 4*(y&mask), L, hits);
  }
  // process the blur for the bulk of the scan line
  for ( ; y < height-radius; y++)
  {
    const int op = y - radius - 1;
    const int np = y + radius;
    sub_4wide(L, buf + op*width);
    // we're now done with the orig value, so store the final result while this line is still in cache
    store_4wide(buf + op*width, scratch + 4*(op&mask));
    add_4wide(L, buf + np*width);
    store_scaled_4wide(scratch + 4*(y&mask), L, hits);
  }
  // process the blur for the end of the scan line, where we don't have any more values to add to the mean
  for ( ; y < height; y++)
  {
    const int op = y - radius - 1;
    hits--;
    sub_4wide(L, buf + op*width);
    // we're now done with the orig value, so store the final result while this line is still in cache
    store_4wide(buf + op*width, scratch + 4*(op&mask));
    store_scaled_4wide(scratch + 4*(y&mask), L, hits);
  }

  // copy remaining blurred values back to original location in buffer
  for (y = (height>radius)?(height-radius-1):0; y < height; y++)
  {
    store_4wide(buf + y*width, scratch + 4*(y&mask));
  }
  return;
}

// invoked inside an OpenMP parallel for, so no need to parallelize
static void blur_vertical_16wide(float *const restrict buf, const size_t height, const size_t width,
                                 const size_t radius, float *const restrict scratch)
{
#ifdef __SSE2__
  if (darktable.codepath.SSE2)
  {
    blur_vertical_4ch_sse(buf, height, width, radius, (__m128*)scratch);
    return;
  }
#endif /* __SSE2__ */

  // To improve cache hit rates, we copy the final result from the scratch space back to the original
  // location in the buffer as soon as we finish the final read of the buffer.  To reduce the working
  // set and further improve cache hits, we can treat the scratch space as a circular buffer and cycle
  // through it repeatedly.  To use a simple bitmask instead of a division, the size we cycle through
  // needs to be the power of two larger than the window size (2*radius+1).
  size_t mask = 1;
  for(size_t r = (2*radius+1); r > 1 ; r >>= 1) mask = (mask << 1) | 1;

  float DT_ALIGNED_ARRAY L[16] = { 0, 0, 0, 0 };
  float hits = 0;
  // add up the left half of the window
  for (size_t y = 0; y < MIN(radius, height); y++)
  {
    hits++;
    add_16wide(L, buf + y * width);
  }
  // process the blur up to the point where we start removing values
  size_t y;
  for (y = 0; y <= MIN(radius, height-radius-1); y++)
  {
    const size_t np = y + radius;
    hits++;
    add_16wide(L, buf + np*width);
    store_scaled_16wide(scratch + 16*(y&mask), L, hits);
  }
  // process the blur for the bulk of the scan line
  for ( ; y < height-radius; y++)
  {
    const size_t op = y - radius - 1;
    const size_t np = y + radius;
    sub_16wide(L, buf + op*width);
    // we're now done with the orig value, so store the final result while this line is still in cache
    store_16wide(buf + op*width, scratch + 16*(op&mask));
    add_16wide(L, buf + np*width);
    // update the means
    store_scaled_16wide(scratch + 16*(y&mask), L, hits);
  }
  // process the blur for the end of the scan line, where we don't have any more values to add to the mean
  for ( ; y < height; y++)
  {
    const size_t op = y - radius - 1;
    hits--;
    sub_16wide(L, buf + op*width);
    // we're now done with the orig value, so store the final result while this line is still in cache
    store_16wide(buf + op*width, scratch + 16*(op&mask));
    // update the means
    store_scaled_16wide(scratch + 16*(y&mask), L, hits);
  }

  // copy remaining blurred values back to original location in buffer
  for (y = (height>radius)?(height-radius-1):0; y < height; y++)
  {
    store_16wide(buf + y*width, scratch + 16*(y&mask));
  }
  return;
}

static void blur_vertical_1ch(float *const restrict buf, const size_t height, const size_t width, const size_t radius,
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
  // handle the 0..3 remaining columns
  const int opoffs = -(radius + 1) * width;
  const int npoffs = radius*width;
  for(int x = width & ~3; x < width; x++)
  {
    float L = 0.0f;
    size_t hits = 0;
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

static void dt_box_mean_1ch(float *const buf, const size_t height, const size_t width, const size_t radius,
                            const unsigned iterations)
{
  const size_t size = MAX(width,height);
  float *const restrict scanlines = dt_alloc_align_float(dt_get_num_threads() * size * 4);

  for(unsigned iteration = 0; iteration < iterations; iteration++)
  {
    blur_horizontal_1ch(buf, height, width, radius, scanlines);
    blur_vertical_1ch(buf, height, width, radius, scanlines);
  }

  dt_free_align(scanlines);
}

static void dt_box_mean_4ch(float *const buf, const int height, const int width, const int radius,
                            const unsigned iterations)
{
  // scratch space needed per thread:
  //   4*width floats to store one row during horizontal pass
  //   16*filter_window floats for vertical pass (where filter_window = 2**ceil(lg2(2*radius+1))
  //   16*height for vertical pass using SSE codepath
  int eff_height = height;
#ifdef __SSE2__
  if (!darktable.codepath.SSE2)
#endif
  {
    eff_height = 2;
    for(size_t r = (2*radius+1); r > 1 ; r >>= 1) eff_height <<= 1;
    eff_height = MIN(eff_height,height);
  }
  const size_t size = MAX(4*width,16*eff_height);
  float *const restrict scanlines = dt_alloc_align_float(dt_get_num_threads() * size);

  for(unsigned iteration = 0; iteration < iterations; iteration++)
  {
    blur_horizontal_4ch(buf, height, width, radius, scanlines);
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(width, height, radius)  \
  dt_omp_sharedconst(buf, scanlines) \
  schedule(static)
#endif
    for (size_t col = 0; col < (width & ~3); col += 4)
    {
      float *const restrict scanline = scanlines + 16 * dt_get_thread_num() * height;
      // we need to multiply width by 4 to get the correct stride for the vertical blur
      blur_vertical_16wide(buf + 4 * col, height, 4*width, radius, scanline);
    }
    // handle the 0..3 remaining columns
    for (size_t col = (width & ~3); col < width; col++)
    {
      // we need to multiply width by 4 to get the correct stride for the vertical blur
      blur_vertical_4wide(buf + 4 * col, height, 4*width, radius, scanlines);
    }
  }

  dt_free_align(scanlines);
}

#ifdef __SSE2__
static void dt_box_mean_4ch_sse(float *const buf, const int height, const int width, const int radius,
                                const unsigned iterations)
{
  const int size = MAX(width,height);

  __m128 *const scanline_buf = dt_alloc_align(64, sizeof(__m128) * dt_get_num_threads() * size * 4);

  for(unsigned iteration = 0; iteration < iterations; iteration++)
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
                                const int radius, const unsigned iterations)
{
  // Compute in-place a box average (filter) on a multi-channel image over a window of size 2*radius + 1
  // We make use of the separable nature of the filter kernel to speed-up the computation
  // by convolving along columns and rows separately (complexity O(2 × radius) instead of O(radius²)).

  const size_t Ndim = MAX(width,height) * 2 * 2;
  float *const restrict temp = dt_alloc_align_float(Ndim * dt_get_num_threads());
  if (temp == NULL) return;

  for (unsigned iteration = 0; iteration < iterations; iteration++)
  {
    blur_horizontal_2ch(in, height, width, radius, temp);
    blur_vertical_1ch(in, height, 2*width, radius, temp);
  }
  dt_free_align(temp);
}

void dt_box_mean(float *const buf, const size_t height, const size_t width, const int ch,
                 const int radius, const unsigned iterations)
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
  else
    dt_unreachable_codepath();
}


// calculate the one-dimensional moving maximum over a window of size 2*w+1
// input array x has stride 1, output array y has stride stride_y
static inline void box_max_1d(int N, const float *const restrict x, float *const restrict y, size_t stride_y, int w)
{
  float m = -(FLT_MAX);
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
      m = -(FLT_MAX);
#ifdef _OPENMP
#pragma omp simd aligned(x) reduction(max : m)
#endif
      for(int j = i - w + 1; j < MIN(i + w + 2, N); j++)
        m = MAX(x[j], m);
    }
    // if the window has not yet exceeded the end of the row/column, update the maximum value
    if(i + w + 1 < N)
      m = MAX(x[i + w + 1], m);
  }
}

static void set_16wide(float *const restrict out, const float value)
{
#ifdef _OPENMP
#pragma omp simd aligned(out : 64)
#endif
  for (size_t c = 0; c < 16; c++)
    out[c] = value;
}

// copy 16 floats from a possibly-unaligned buffer into aligned temporary space
static void load_16wide(float *const restrict out, const float *const restrict in)
{
#ifdef _OPENMP
#pragma omp simd aligned(out : 64)
#endif
  for (size_t c = 0; c < 16; c++)
    out[c] = in[c];
}

static inline void update_max_16wide(float m[16], const float *const restrict base)
{
#ifdef _OPENMP
#pragma omp simd aligned(m, base)
#endif
  for (size_t c = 0; c < 16; c++)
  {
    m[c] = fmaxf(m[c], base[c]);
  }
}

// calculate the one-dimensional moving maximum on four adjacent columns over a window of size 2*w+1
// input array x has stride 16, output array y has stride stride_y and we will write 16 consecutive elements
//  every stride_y elements (thus processing a cache line at a time)
static inline void box_max_vert_16wide(const int N, const float *const restrict x, float *const restrict y,
                                      const int stride_y, const int w)
{
  float DT_ALIGNED_ARRAY m[16] = { -(FLT_MAX), -(FLT_MAX), -(FLT_MAX), -(FLT_MAX),
                                   -(FLT_MAX), -(FLT_MAX), -(FLT_MAX), -(FLT_MAX),
                                   -(FLT_MAX), -(FLT_MAX), -(FLT_MAX), -(FLT_MAX),
                                   -(FLT_MAX), -(FLT_MAX), -(FLT_MAX), -(FLT_MAX) };
  for(size_t i = 0; i < MIN(w + 1, N); i++)
  {
    update_max_16wide(m,x + 16 * i);
  }
  for(size_t i = 0; i < N; i++)
  {
    // store maximum of current window at center position
    store_16wide(y + i * stride_y, m);
    // If the earliest member of the current window is the max, we need to
    // rescan the window to determine the new maximum
    if (i >= w)
    {
      set_16wide(m, -(FLT_MAX));  // reset max values to lowest possible
      for(int j = i - w + 1; j < MIN(i + w + 2, N); j++)
      {
        update_max_16wide(m,x + 16*j);
      }
    }
    // if the window has not yet exceeded the end of the row/column, update the maximum value
    if(i + w + 1 < N)
    {
      update_max_16wide(m, x + (16 * (i + w + 1)));
    }
  }
}

// calculate the two-dimensional moving maximum over a box of size (2*w+1) x (2*w+1)
// does the calculation in-place if input and output images are identical
static void box_max_1ch(float *const buf, const size_t height, const size_t width, const unsigned w)
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
    {
      load_16wide(scratch + 16*row, buf + row*width + col);
    }
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
void dt_box_max(float *const buf, const size_t height, const size_t width, const int ch, const int radius)
{
  if (ch == 1)
    box_max_1ch(buf, height, width, radius);
  else
  //TODO: 4ch version if needed
    dt_unreachable_codepath();
}

// calculate the one-dimensional moving minimum over a window of size 2*w+1
// input array x has stride 1, output array y has stride stride_y
static inline void box_min_1d(int N, const float *x, float *y, size_t stride_y, int w)
{
  float m = FLT_MAX;
  for(int i = 0; i < MIN(w + 1, N); i++)
    m = MIN(x[i], m);
  for(int i = 0; i < N; i++)
  {
    y[i * stride_y] = m;
    if(i - w >= 0 && x[i - w] == m)
    {
      m = FLT_MAX;
#ifdef _OPENMP
#pragma omp simd aligned(x) reduction(min : m)
#endif
      for(int j = i - w + 1; j < MIN(i + w + 2, N); j++)
        m = MIN(x[j], m);
    }
    // if the window has not yet exceeded the end of the row/column, update the minimum value
    if(i + w + 1 < N)
      m = MIN(x[i + w + 1], m);
  }
}

static inline void update_min_16wide(float m[16], const float *const restrict base)
{
#ifdef _OPENMP
#pragma omp simd aligned(m, base)
#endif
  for (size_t c = 0; c < 16; c++)
  {
    m[c] = fminf(m[c], base[c]);
  }
}

// calculate the one-dimensional moving minimum on four adjacent columns over a window of size 2*w+1
// input array x has stride 16, output array y has stride stride_y and we will write 16 consecutive elements
//  every stride_y elements (thus processing a cache line at a time)
static inline void box_min_vert_16wide(const int N, const float *const restrict x, float *const restrict y,
                                      const int stride_y, const int w)
{
  float DT_ALIGNED_ARRAY m[16] = { FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX,
                                   FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX,
                                   FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX,
                                   FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX };
  for(size_t i = 0; i < MIN(w + 1, N); i++)
  {
    update_min_16wide(m, x + 16*i);
  }
  for(size_t i = 0; i < N; i++)
  {
    // store minimum of current window at center position
    store_16wide(y + i * stride_y, m);
    // If the earliest member of the current window is the min, we need to
    // rescan the window to determine the new minimum
    if (i >= w)
    {
      set_16wide(m, FLT_MAX);  // reset min values to the highest possible
      for(int j = i - w + 1; j < MIN(i + w + 2, N); j++)
      {
        update_min_16wide(m,x + 16*j);
      }
    }
    // if the window has not yet exceeded the end of the row/column, update the minimum value
    if(i + w + 1 < N)
    {
      update_min_16wide(m, x  + (16 * (i + w + 1)));
    }
  }
}


// calculate the two-dimensional moving minimum over a box of size (2*w+1) x (2*w+1)
// does the calculation in-place if input and output images are identical
static void box_min_1ch(float *const buf, const size_t height, const size_t width, const int w)
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
  for(size_t col = 0; col < (width & ~15); col += 16)
  {
    float *const restrict scratch = scratch_buffers + 16 * height * dt_get_thread_num();
    for (size_t row = 0; row < height; row++)
    {
      load_16wide(scratch + 16*row, buf + row*width + col);
    }
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

void dt_box_min(float *const buf, const size_t height, const size_t width, const int ch, const int radius)
{
  if (ch == 1)
    box_min_1ch(buf, height, width, radius);
  else
  //TODO: 4ch version if needed
    dt_unreachable_codepath();
}
