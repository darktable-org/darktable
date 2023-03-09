/*
    This file is part of darktable,
    Copyright (C) 2009-2023 darktable developers.

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
#include "common/math.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"

#ifdef __GNUC__
#pragma GCC optimize ("finite-math-only")
#endif

#if defined(__SSE__)
#define DT_PREFETCH(addr) _mm_prefetch(addr, _MM_HINT_T2)
#define PREFETCH_NTA(addr) _mm_prefetch(addr, _MM_HINT_NTA)
#elif defined(__GNUC__)
#define DT_PREFETCH(addr) __builtin_prefetch(addr,1,1)
#define PREFETCH_NTA(addr) __builtin_prefetch(addr,1,0)
#else
#define DT_PREFETCH(addr)
#define PREFETCH_NTA(addr)
#endif

static void _blur_horizontal_1ch(float *const restrict buf,
    const int height,
    const int width,
    const int radius,
    float *const restrict scanlines,
    const size_t padded_size)
{
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(radius, height, width, padded_size) \
  dt_omp_sharedconst(buf, scanlines) \
  schedule(static)
#endif
  for(int y = 0; y < height; y++)
  {
    float L = 0;
    int hits = 0;
    const size_t index = (size_t)y * width;
    float *const restrict scanline = dt_get_perthread(scanlines,padded_size);
    // add up the left half of the window
    for(int x = 0; x < MIN(radius,width) ; x++)
    {
      L += buf[index+x];
      hits++;
    }
    // process the blur up to the point where we start removing values
    int x;
    for(x = 0; (x <= radius) && ((x + radius) < width); x++)
    {
      const int np = x + radius;
      L += buf[index + np];
      hits++;
      scanline[x] = L / hits;
    }
    // if radius > width/2, we have pixels for which we can neither add new values (x+radius >= width) nor
    //  remove old values (x-radius < 0)
    for(; x <= radius && x < width; x++)
    {
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

static void _blur_horizontal_2ch(float *const restrict buf,
    const int height,
    const int width,
    const int radius,
    float *const restrict scanlines,
    const size_t padded_size)
{
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(radius, height, width, padded_size)   \
  dt_omp_sharedconst(buf, scanlines) \
  schedule(static)
#endif
  for(int y = 0; y < height; y++)
  {
    float *const restrict scanline = dt_get_perthread(scanlines, padded_size);
    float L1 = 0.0f, L2 = 0.0f;
    int hits = 0;
    const size_t index = (size_t)2 * y * width;
    // add up the left half of the window
    for(int x = 0; x < MIN(radius, width) ; x++)
    {
      hits++;
      L1 += buf[index + 2*x];
      L2 += buf[index + 2*x + 1];
    }
    // process the blur up to the point where we start removing values
    int x;
    for(x = 0; (x <= radius) && ((x + radius) < width); x++)
    {
      const int np = x + radius;
      hits++;
      L1 += buf[index + 2*np];
      L2 += buf[index + 2*np + 1];
      scanline[2*x] = L1 / hits;
      scanline[2*x+1] = L2 / hits;
    }
    // if radius > width/2, we have pixels for which we can neither add new values (x+radius >= width) nor
    //  remove old values (x-radius < 0)
    for(; x <= radius && x < width; x++)
    {
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
static void _load_add_4wide(float *const restrict out,
                            dt_aligned_pixel_t accum,
                            const float *const restrict values)
{
  for_four_channels(c,aligned(accum, out))
  {
    const float v = values[c];
    accum[c] += v;
    out[c] = v;
  }
}

static void _sub_4wide(float *const restrict accum,
                       const dt_aligned_pixel_t values)
{
  for_four_channels(c,aligned(accum))
    accum[c] -= values[c];
}

// Put the to-be-vectorized loop into a function by itself to nudge the compiler into actually vectorizing...
// With optimization enabled, this gets inlined and interleaved with other instructions as though it had been
// written in place, so we get a net win from better vectorization.
static void _load_add_4wide_Kahan(float *const restrict out,
    dt_aligned_pixel_t accum,
    const float *const restrict values,
    float *const restrict comp)
{
  for_four_channels(c,aligned(accum, comp, out))
  {
    const float v = values[c];
    out[c] = v;
    // Kahan (compensated) summation
    const float t1 = v - comp[c];
    const float t2 = accum[c] + t1;
    comp[c] = (t2 - accum[c]) - t1;
    accum[c] = t2;
  }
}

static void _sub_4wide_Kahan(float *const restrict accum,
    const dt_aligned_pixel_t values,
    float *const restrict comp)
{
  for_four_channels(c,aligned(accum,comp,values))
  {
    // Kahan (compensated) summation
    const float t1 = -values[c] - comp[c];
    const float t2 = accum[c] + t1;
    comp[c] = (t2 - accum[c]) - t1;
    accum[c] = t2;
  }
}

static void store_scaled_4wide(float *const restrict out,
                               const dt_aligned_pixel_t in,
                               const float scale)
{
  for_four_channels(c,aligned(in))
    out[c] = in[c] / scale;
}

static void _sub_16wide(float *const restrict accum,
                        const float *const restrict values)
{
#ifdef _OPENMP
#pragma omp simd aligned(accum : 64) aligned(values : 16)
#endif
  for(size_t c = 0; c < 16; c++)
    accum[c] -= values[c];
}

// copy 16 floats from a possibly-unaligned buffer into aligned temporary space, and also add to accumulator
static void _load_add_16wide(float *const restrict out,
    float *const restrict accum,
    const float *const restrict in)
{
#ifdef _OPENMP
#pragma omp simd aligned(accum, out : 64)
#endif
  for(size_t c = 0; c < 16; c++)
  {
    const float v = in[c];
    accum[c] += v;
    out[c] = v;
  }
}

static void _sub_16wide_Kahan(float *const restrict accum,
    const float *const restrict values,
    float *const restrict comp)
{
#ifdef _OPENMP
#pragma omp simd aligned(accum,comp : 64) aligned(values : 16)
#endif
  for(size_t c = 0; c < 16; c++)
  {
    const float v = -values[c];
    // Kahan (compensated) summation
    const float t1 = v - comp[c];
    const float t2 = accum[c] + t1;
    comp[c] = (t2 - accum[c]) - t1;
    accum[c] = t2;
  }
}

// copy 16 floats from a possibly-unaligned buffer into aligned temporary space, and also add to accumulator
static void _load_add_16wide_Kahan(float *const restrict out,
    float *const restrict accum,
    const float *const restrict in,
    float *const restrict comp)
{
#ifdef _OPENMP
#pragma omp simd aligned(accum, comp, out : 64)
#endif
  for(size_t c = 0; c < 16; c++)
  {
    const float v = in[c];
    out[c] = v;
    // Kahan (compensated) summation
    const float t1 = v - comp[c];
    const float t2 = accum[c] + t1;
    comp[c] = (t2 - accum[c]) - t1;
    accum[c] = t2;
  }
}

// copy 16 floats from aligned temporary space back to the possibly-unaligned user buffer
static void _store_16wide(float *const restrict out,
                         const float *const restrict in)
{
#ifdef _OPENMP
#pragma omp simd aligned(in : 64)
#endif
  for(size_t c = 0; c < 16; c++)
    out[c] = in[c];
}

static void store_scaled_16wide(float *const restrict out,
    const float *const restrict in,
    const float scale)
{
#ifdef _OPENMP
#pragma omp simd aligned(in : 64)
#endif
  for(size_t c = 0; c < 16; c++)
    out[c] = in[c] / scale;
}

static void _sub_Nwide_Kahan(const size_t N,
    float *const restrict accum,
    const float *const restrict values,
    float *const restrict comp)
{
#ifdef _OPENMP
#pragma omp simd aligned(accum,comp : 64)
#endif
  for(size_t c = 0; c < N; c++)
  {
    const float v = -values[c];
    // Kahan (compensated) summation
    const float t1 = v - comp[c];
    const float t2 = accum[c] + t1;
    comp[c] = (t2 - accum[c]) - t1;
    accum[c] = t2;
  }
}

// copy N (<=16) floats from a possibly-unaligned buffer into aligned temporary space, and also add to accumulator
static void _load_add_Nwide_Kahan(const size_t N,
    float *const restrict out,
    float *const restrict accum,
    const float *const restrict in,
    float *const restrict comp)
{
#ifdef _OPENMP
#pragma omp simd aligned(accum, comp : 64)
#endif
  for(size_t c = 0; c < N; c++)
  {
    const float v = in[c];
    out[c] = v;
    // Kahan (compensated) summation
    const float t1 = v - comp[c];
    const float t2 = accum[c] + t1;
    comp[c] = (t2 - accum[c]) - t1;
    accum[c] = t2;
  }
}

static void store_scaled_Nwide(const size_t N,
    float *const restrict out,
    const float *const restrict in,
    const float scale)
{
#ifdef _OPENMP
#pragma omp simd aligned(in : 64)
#endif
  for(size_t c = 0; c < N; c++)
    out[c] = in[c] / scale;
}


static void _blur_horizontal_4ch(float *const restrict buf,
    const size_t height,
    const size_t width,
    const size_t radius,
    float *const restrict scanlines,
    const size_t padded_size)
{
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(radius, height, width, padded_size) \
  dt_omp_sharedconst(buf, scanlines) \
  schedule(static)
#endif
  for(int y = 0; y < height; y++)
  {
    float *const restrict scratch = dt_get_perthread(scanlines,padded_size);
    dt_aligned_pixel_t L = { 0, 0, 0, 0 };
    size_t hits = 0;
    const size_t index = (size_t)4 * y * width;
    float *const restrict bufp = buf + index;
    // add up the left half of the window
    for(size_t x = 0; x < MIN(radius,width) ; x++)
    {
      hits++;
      _load_add_4wide(scratch + 4*x, L, bufp + 4*x);
    }
    // process the blur up to the point where we start removing values
    size_t x;
    for(x = 0; (x <= radius) && ((x + radius) < width); x++)
    {
      const int np = x + radius;
      hits++;
      _load_add_4wide(scratch + 4*np, L, bufp + 4*np);
      store_scaled_4wide(bufp + 4*x, L, hits);
    }
    // if radius > width/2, we have pixels for which we can neither add new values (x+radius >= width) nor
    //  remove old values (x-radius < 0)
    for(; x <= radius && x < width; x++)
    {
      store_scaled_4wide(bufp + 4*x, L, hits);
    }
    // process the blur for the bulk of the scan line
    for(; x + radius < width; x++)
    {
      //very strange: if any of the 'op' or 'np' variables in this function are changed to either
      // 'unsigned' or 'size_t', the function runs a fair bit slower....
      const int op = x - radius - 1;
      const int np = x + radius;
      _sub_4wide(L, scratch + 4*op);
      _load_add_4wide(scratch + 4*np, L, bufp + 4*np);
      store_scaled_4wide(bufp + 4*x, L, hits);
    }
    // process the right end where we have no more values to add to the running sum
    for(; x < width; x++)
    {
      const int op = x - radius - 1;
      hits--;
      _sub_4wide(L, scratch + 4*op);
      store_scaled_4wide(bufp + 4*x, L, hits);
    }
  }
  return;
}

// invoked inside an OpenMP parallel for, so no need to parallelize
static void _blur_horizontal_4ch_Kahan(float *const restrict buf,
    const size_t width,
    const size_t radius,
    float *const restrict scratch)
{
  dt_aligned_pixel_t L = { 0, 0, 0, 0 };
  dt_aligned_pixel_t comp = { 0, 0, 0, 0 };
  size_t hits = 0;
  // add up the left half of the window
  for(size_t x = 0; x < MIN(radius,width) ; x++)
  {
    hits++;
    _load_add_4wide_Kahan(scratch + 4*x, L, buf + 4*x, comp);
  }
  // process the blur up to the point where we start removing values from the moving average
  size_t x;
  for(x = 0; (x <= radius) && ((x + radius) < width); x++)
  {
    const int np = x + radius;
    hits++;
    _load_add_4wide_Kahan(scratch + 4*np, L, buf + 4*np, comp);
    store_scaled_4wide(buf + 4*x, L, hits);
  }
  // if radius > width/2, we have pixels for which we can neither add new values (x+radius >= width) nor
  //  remove old values (x-radius < 0)
  for(; x <= radius && x < width; x++)
  {
    store_scaled_4wide(buf + 4*x, L, hits);
  }
  // process the blur for the bulk of the scan line
  for(; x + radius < width; x++)
  {
    const int op = x - radius - 1;
    const int np = x + radius;
    _sub_4wide_Kahan(L, scratch + 4*op, comp);
    _load_add_4wide_Kahan(scratch + 4*np, L, buf + 4*np, comp);
    store_scaled_4wide(buf + 4*x, L, hits);
  }
  // process the right end where we have no more values to add to the running sum
  for(; x < width; x++)
  {
    const int op = x - radius - 1;
    hits--;
    _sub_4wide_Kahan(L, scratch + 4*op, comp);
    store_scaled_4wide(buf + 4*x, L, hits);
  }
  return;
}

// invoked inside an OpenMP parallel for, so no need to parallelize
static void _blur_horizontal_Nch_Kahan(const size_t N,
    float *const restrict buf,
    const size_t width,
    const size_t radius,
    float *const restrict scratch)
{
  if(N > 16) return;
  if(N != 9) return;  // since we only use 9 channels at the moment, give the compiler a big hint

  float DT_ALIGNED_ARRAY L[16] = { 0, 0, 0, 0 };
  float DT_ALIGNED_ARRAY comp[16] = { 0, 0, 0, 0 };
  size_t hits = 0;
  // add up the left half of the window
  for(size_t x = 0; x < MIN(radius,width) ; x++)
  {
    hits++;
    _load_add_Nwide_Kahan(N, scratch + N*x, L, buf + N*x, comp);
  }
  // process the blur up to the point where we start removing values from the moving average
  size_t x;
  for(x = 0; (x <= radius) && ((x + radius) < width); x++)
  {
    const int np = x + radius;
    hits++;
    _load_add_Nwide_Kahan(N, scratch + N*np, L, buf + N*np, comp);
    store_scaled_Nwide(N, buf + N*x, L, hits);
  }
  // if radius > width/2, we have pixels for which we can neither add new values (x+radius >= width) nor
  //  remove old values (x-radius < 0)
  for(; x <= radius && x < width; x++)
  {
    store_scaled_Nwide(N, buf + N*x, L, hits);
  }
  // process the blur for the bulk of the scan line
  for(; x + radius < width; x++)
  {
    const int op = x - radius - 1;
    const int np = x + radius;
    _sub_Nwide_Kahan(N, L, scratch + N*op, comp);
    _load_add_Nwide_Kahan(N, scratch + N*np, L, buf + N*np, comp);
    store_scaled_Nwide(N, buf + N*x, L, hits);
  }
  // process the right end where we have no more values to add to the running sum
  for(; x < width; x++)
  {
    const int op = x - radius - 1;
    hits--;
    _sub_Nwide_Kahan(N, L, scratch + N*op, comp);
    store_scaled_Nwide(N, buf + N*x, L, hits);
  }
  return;
}

// invoked inside an OpenMP parallel for, so no need to parallelize
static void _blur_vertical_1wide(float *const restrict buf,
    const size_t height,
    const size_t width,
    const size_t radius,
    float *const restrict scratch)
{
  // To improve cache hit rates, we copy the final result from the scratch space back to the original
  // location in the buffer as soon as we finish the final read of the buffer.  To reduce the working
  // set and further improve cache hits, we can treat the scratch space as a circular buffer and cycle
  // through it repeatedly.  To use a simple bitmask instead of a division, the size we cycle through
  // needs to be the power of two larger than the window size (2*radius+1).
  size_t mask = 1;
  for(size_t r = (2*radius+1); r > 1 ; r >>= 1) mask = (mask << 1) | 1;

  float L = 0.0f;
  size_t hits = 0;
  // add up the left half of the window
  for(size_t y = 0; y < MIN(radius, height); y++)
  {
    hits++;
    const float v = buf[y*width];
    L += v;
    scratch[y&mask] = v;
  }
  // process up to the point where we start removing values from the moving average
  size_t y;
  for(y = 0; y <= radius && y + radius < height; y++)
  {
    // weirdly, changing any of the 'np' or 'op' variables in this function to 'size_t' yields a substantial slowdown!
    const int np = y + radius;
    hits++;
    const float v = buf[np*width];
    L += v;
    scratch[np&mask] = v;
    buf[y*width] = L / hits;
  }
  // if radius > height/2, we have pixels for which we can neither add new values (y+radius >= height) nor
  //  remove old values (y-radius < 0)
  for(; y <= radius && y < height; y++)
  {
    buf[y*width] = L / hits;
  }
  // process the bulk of the column
  for( ; y + radius < height; y++)
  {
    const int np = y + radius;
    const int op = y - radius - 1;
    L -= scratch[op&mask];
    const float v = buf[np*width];
    L += v;
    scratch[np&mask] = v;
    // update the means
    buf[y*width] = L / hits;
  }
  // process the end of the column, where we don't have any more values to add to the mean
  for( ; y < height; y++)
  {
    const int op = y - radius - 1;
    hits--;
    L -= scratch[op&mask];
    // update the means
    buf[y*width] = L / hits;
  }
  return;
}

// invoked inside an OpenMP parallel for, so no need to parallelize
static void _blur_vertical_1wide_Kahan(float *const restrict buf,
    const size_t height,
    const size_t width,
    const size_t radius,
    float *const restrict scratch)
{
  // To improve cache hit rates, we copy the final result from the scratch space back to the original
  // location in the buffer as soon as we finish the final read of the buffer.  To reduce the working
  // set and further improve cache hits, we can treat the scratch space as a circular buffer and cycle
  // through it repeatedly.  To use a simple bitmask instead of a division, the size we cycle through
  // needs to be the power of two larger than the window size (2*radius+1).
  size_t mask = 1;
  for(size_t r = (2*radius+1); r > 1 ; r >>= 1) mask = (mask << 1) | 1;

  float L = 0.0f;
  float c = 0.0f;
  size_t hits = 0;
  // add up the left half of the window
  for(size_t y = 0; y < MIN(radius, height); y++)
  {
    hits++;
    const float v = buf[y*width];
    L = Kahan_sum(L, &c, v);
    scratch[y&mask] = v;
  }
  // process up to the point where we start removing values from the moving average
  size_t y;
  for(y = 0; y <= radius && y + radius < height; y++)
  {
    // weirdly, changing any of the 'np' or 'op' variables in this function to 'size_t' yields a substantial slowdown!
    const int np = y + radius;
    hits++;
    const float v = buf[np*width];
    L = Kahan_sum(L, &c, v);
    scratch[np&mask] = v;
    buf[y*width] = L / hits;
  }
  // if radius > height/2, we have pixels for which we can neither add new values (y+radius >= height) nor
  //  remove old values (y-radius < 0)
  for(; y <= radius && y < height; y++)
  {
    buf[y*width] = L / hits;
  }
  // process the bulk of the column
  for( ; y + radius < height; y++)
  {
    const int np = y + radius;
    const int op = y - radius - 1;
    L = Kahan_sum(L, &c, -scratch[op&mask]);
    const float v = buf[np*width];
    L = Kahan_sum(L, &c, v);
    scratch[np&mask] = v;
    // update the means
    buf[y*width] = L / hits;
  }
  // process the end of the column, where we don't have any more values to add to the mean
  for( ; y < height; y++)
  {
    const int op = y - radius - 1;
    hits--;
    L = Kahan_sum(L, &c, scratch[op&mask]);
    // update the means
    buf[y*width] = L / hits;
  }
  return;
}

// invoked inside an OpenMP parallel for, so no need to parallelize
static void _blur_vertical_4wide(float *const restrict buf,
    const size_t height,
    const size_t width,
    const size_t radius,
    float *const restrict scratch)
{
  // To improve cache hit rates, we copy the final result from the scratch space back to the original
  // location in the buffer as soon as we finish the final read of the buffer.  To reduce the working
  // set and further improve cache hits, we can treat the scratch space as a circular buffer and cycle
  // through it repeatedly.  To use a simple bitmask instead of a division, the size we cycle through
  // needs to be the power of two larger than the window size (2*radius+1).
  size_t mask = 1;
  for(size_t r = (2*radius+1); r > 1 ; r >>= 1) mask = (mask << 1) | 1;

  dt_aligned_pixel_t L = { 0, 0, 0, 0 };
  size_t hits = 0;
  // add up the left half of the window
  for(size_t y = 0; y < MIN(radius, height); y++)
  {
    hits++;
    _load_add_4wide(scratch + 4*(y&mask), L, buf + y * width);
  }
  // process the blur up to the point where we start removing values
  size_t y;
  for(y = 0; y <= radius && y + radius < height; y++)
  {
    // weirdly, changing any of the 'np' or 'op' variables in this function to 'size_t' yields a substantial slowdown!
    const int np = y + radius;
    hits++;
    _load_add_4wide(scratch + 4*(np&mask), L, buf + np*width);
    store_scaled_4wide(buf + y*width, L, hits);
  }
  // if radius > height/2, we have pixels for which we can neither add new values (y+radius >= height) nor
  //  remove old values (y-radius < 0)
  for(; y <= radius && y < height; y++)
  {
    store_scaled_4wide(buf + y*width, L, hits);
  }
  // process the blur for the bulk of the column
  for( ; y + radius < height; y++)
  {
    const int np = y + radius;
    const int op = y - radius - 1;
    _sub_4wide(L, scratch + 4*(op&mask));
    _load_add_4wide(scratch + 4*(np&mask), L, buf + np*width);
    store_scaled_4wide(buf + y*width, L, hits);
  }
  // process the blur for the end of the scan line, where we don't have any more values to add to the mean
  for( ; y < height; y++)
  {
    const int op = y - radius - 1;
    hits--;
    _sub_4wide(L, scratch + 4*(op&mask));
    store_scaled_4wide(buf + y*width, L, hits);
  }
  return;
}

// invoked inside an OpenMP parallel for, so no need to parallelize
static void _blur_vertical_4wide_Kahan(float *const restrict buf,
    const size_t height,
    const size_t width,
    const size_t radius,
    float *const restrict scratch)
{
  // To improve cache hit rates, we copy the final result from the scratch space back to the original
  // location in the buffer as soon as we finish the final read of the buffer.  To reduce the working
  // set and further improve cache hits, we can treat the scratch space as a circular buffer and cycle
  // through it repeatedly.  To use a simple bitmask instead of a division, the size we cycle through
  // needs to be the power of two larger than the window size (2*radius+1).
  size_t mask = 1;
  for(size_t r = (2*radius+1); r > 1 ; r >>= 1) mask = (mask << 1) | 1;

  dt_aligned_pixel_t L = { 0, 0, 0, 0 };
  dt_aligned_pixel_t comp = { 0, 0, 0, 0 };
  size_t hits = 0;
  // add up the left half of the window
  for(size_t y = 0; y < MIN(radius, height); y++)
  {
    hits++;
    _load_add_4wide_Kahan(scratch + 4*(y&mask), L, buf + y * width, comp);
  }
  // process the blur up to the point where we start removing values
  size_t y;
  for(y = 0; y <= radius && y + radius < height; y++)
  {
    // weirdly, changing any of the 'np' or 'op' variables in this function to 'size_t' yields a substantial slowdown!
    const int np = y + radius;
    hits++;
    _load_add_4wide_Kahan(scratch + 4*(np&mask), L, buf + np*width, comp);
    store_scaled_4wide(buf + y*width, L, hits);
  }
  // if radius > height/2, we have pixels for which we can neither add new values (y+radius >= height) nor
  //  remove old values (y-radius < 0)
  for(; y <= radius && y < height; y++)
  {
    store_scaled_4wide(buf + y*width, L, hits);
  }
  // process the blur for the bulk of the scan line
  for( ; y + radius < height; y++)
  {
    const int np = y + radius;
    const int op = y - radius - 1;
    _sub_4wide_Kahan(L, scratch + 4*(op&mask), comp);
    _load_add_4wide_Kahan(scratch + 4*(np&mask), L, buf + np*width, comp);
    store_scaled_4wide(buf + y*width, L, hits);
  }
  // process the blur for the end of the scan line, where we don't have any more values to add to the mean
  for( ; y < height; y++)
  {
    const int op = y - radius - 1;
    hits--;
    _sub_4wide_Kahan(L, scratch + 4*(op&mask), comp);
    store_scaled_4wide(buf + y*width, L, hits);
  }
  return;
}

// invoked inside an OpenMP parallel for, so no need to parallelize
static void _blur_vertical_16wide(float *const restrict buf,
    const size_t height,
    const size_t width,
    const size_t radius,
    float *const restrict scratch)
{
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
  for(size_t y = 0; y < MIN(radius, height); y++)
  {
    hits++;
    _load_add_16wide(scratch + 16 * (y&mask), L, buf + y*width);
  }
  // process the blur up to the point where we start removing values from the moving average
  size_t y;
  for(y = 0; y <= radius && y + radius < height; y++)
  {
    // weirdly, changing any of the 'np' or 'op' variables in this function to 'size_t' yields a substantial slowdown!
    const int np = y + radius;
    hits++;
    _load_add_16wide(scratch + 16 * (np&mask), L, buf + np*width);
    store_scaled_16wide(buf + y*width, L, hits);
  }
  // if radius > height/2, we have pixels for which we can neither add new values (y+radius >= height) nor
  //  remove old values (y-radius < 0)
  for(; y <= radius && y < height; y++)
  {
    store_scaled_16wide(buf + y*width, L, hits);
  }
  // process the blur for the bulk of the column
  for( ; y + radius < height; y++)
  {
    const int np = y + radius;
    const int op = y - radius - 1;
    _sub_16wide(L, scratch + 16*(op&mask));
    _load_add_16wide(scratch + 16*(np&mask), L, buf + np*width);
    // update the means
    store_scaled_16wide(buf + y*width, L, hits);
  }
  // process the blur for the end of the scan line, where we don't have any more values to add to the mean
  for( ; y < height; y++)
  {
    const int op = y - radius - 1;
    hits--;
    _sub_16wide(L, scratch + 16*(op&mask));
    // update the means
    store_scaled_16wide(buf + y*width, L, hits);
  }
  return;
}

// invoked inside an OpenMP parallel for, so no need to parallelize
static void _blur_vertical_16wide_Kahan(float *const restrict buf,
    const size_t height,
    const size_t width,
    const size_t radius,
    float *const restrict scratch)
{
  // To improve cache hit rates, we copy the final result from the scratch space back to the original
  // location in the buffer as soon as we finish the final read of the buffer.  To reduce the working
  // set and further improve cache hits, we can treat the scratch space as a circular buffer and cycle
  // through it repeatedly.  To use a simple bitmask instead of a division, the size we cycle through
  // needs to be the power of two larger than the window size (2*radius+1).
  size_t mask = 1;
  for(size_t r = (2*radius+1); r > 1 ; r >>= 1) mask = (mask << 1) | 1;

  float DT_ALIGNED_ARRAY L[16] = { 0, 0, 0, 0 };
  float DT_ALIGNED_ARRAY comp[16] = { 0, 0, 0, 0 };
  float hits = 0;
  // add up the left half of the window
  for(size_t y = 0; y < MIN(radius, height); y++)
  {
    hits++;
    _load_add_16wide_Kahan(scratch + 16 * (y&mask), L, buf + y*width, comp);
  }
  // process the blur up to the point where we start removing values from the moving average
  size_t y;
  for(y = 0; y <= radius && y + radius < height; y++)
  {
    // weirdly, changing any of the 'np' or 'op' variables in this function to 'size_t' yields a substantial slowdown!
    const int np = y + radius;
    hits++;
    _load_add_16wide_Kahan(scratch + 16 * (np&mask), L, buf + np*width, comp);
    store_scaled_16wide(buf + y*width, L, hits);
  }
  // if radius > height/2, we have pixels for which we can neither add new values (y+radius >= height) nor
  //  remove old values (y-radius < 0)
  for(; y <= radius && y < height; y++)
  {
    store_scaled_16wide(buf + y*width, L, hits);
  }
  // process the blur for the bulk of the column
  for( ; y + radius < height; y++)
  {
    const int np = y + radius;
    const int op = y - radius - 1;
    _sub_16wide_Kahan(L, scratch + 16*(op&mask), comp);
    _load_add_16wide_Kahan(scratch + 16*(np&mask), L, buf + np*width, comp);
    // update the means
    store_scaled_16wide(buf + y*width, L, hits);
  }
  // process the blur for the end of the scan line, where we don't have any more values to add to the mean
  for( ; y < height; y++)
  {
    const int op = y - radius - 1;
    hits--;
    _sub_16wide_Kahan(L, scratch + 16*(op&mask), comp);
    // update the means
    store_scaled_16wide(buf + y*width, L, hits);
  }
  return;
}

static void _blur_vertical_1ch(float *const restrict buf,
                               const size_t height,
                               const size_t width,
                               const size_t radius,
                               float *const restrict scanlines,
                               const size_t padded_size)
{
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(radius, height, width, padded_size) \
  shared(darktable) \
  dt_omp_sharedconst(buf, scanlines) \
  schedule(static)
#endif
  for(int x = 0; x < width; x += 16)
  {
    float *const restrict scratch = dt_get_perthread(scanlines,padded_size);
    if(x + 16 <= width)
    {
      _blur_vertical_16wide(buf + x, height, width, radius, scratch);
    }
    else
    {
      // handle the leftover 1..15 columns, first in groups of four, then the final 0..3 singly
      int col = x;
      for( ; col < (width & ~3); col += 4)
        _blur_vertical_4wide(buf + col, height, width, radius, scratch);
      for( ; col < width; col++)
        _blur_vertical_1wide(buf + col, height, width, radius, scratch);
    }
  }
  return;
}

// determine the size of the scratch buffer needed for vertical passes of the box-mean filter
// filter_window = 2**ceil(lg2(2*radius+1))
static size_t _compute_effective_height(const size_t height, const size_t radius)
{
  size_t eff_height = 2;
  for(size_t r = (2*radius+1); r > 1 ; r >>= 1) eff_height <<= 1;
  eff_height = MIN(eff_height,height);
  return eff_height;
}

static void dt_box_mean_1ch(float *const buf,
                            const size_t height,
                            const size_t width,
                            const size_t radius,
                            const unsigned iterations)
{
  // scratch space needed per thread:
  //   width floats to store one row during horizontal pass
  //   16*filter_window floats for vertical pass
  const size_t eff_height = _compute_effective_height(height,radius);
  const size_t size = MAX(width,16*eff_height);
  size_t padded_size;
  float *const restrict scanlines = dt_alloc_perthread_float(size, &padded_size);

  for(unsigned iteration = 0; iteration < iterations; iteration++)
  {
    _blur_horizontal_1ch(buf, height, width, radius, scanlines, padded_size);
    _blur_vertical_1ch(buf, height, width, radius, scanlines, padded_size);
  }

  dt_free_align(scanlines);
}

static void dt_box_mean_4ch(float *const buf,
                            const int height,
                            const int width,
                            const int radius,
                            const unsigned iterations)
{
  // scratch space needed per thread:
  //   4*width floats to store one row during horizontal pass
  //   16*filter_window floats for vertical pass
  const size_t eff_height = _compute_effective_height(height,radius);
  const size_t size = MAX(4*width,16*eff_height);
  size_t padded_size;
  float *const restrict scanlines = dt_alloc_perthread_float(size, &padded_size);

  for(unsigned iteration = 0; iteration < iterations; iteration++)
  {
    _blur_horizontal_4ch(buf, height, width, radius, scanlines, padded_size);
    // we need to multiply width by 4 to get the correct stride for the vertical blur
    _blur_vertical_1ch(buf, height, 4*width, radius, scanlines, padded_size);
  }

  dt_free_align(scanlines);
}

static void box_mean_vert_1ch_Kahan(float *const buf,
    const int height,
    const size_t width,
    const size_t radius)
{
  const size_t eff_height = _compute_effective_height(height,radius);
  size_t padded_size;
  float *const restrict scratch_buf = dt_alloc_perthread_float(16*eff_height,&padded_size);

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(width, height, radius, padded_size) \
  dt_omp_sharedconst(buf, scratch_buf) \
  schedule(static)
#endif
  for(size_t col = 0; col < width; col += 16)
  {
    float *const restrict scratch = dt_get_perthread(scratch_buf,padded_size);
    if(col + 16 <= width)
    {
      _blur_vertical_16wide_Kahan(buf + col, height, width, radius, scratch);
    }
    else
    {
      // handle the 1..15 remaining columns
      size_t col_ = col;
      for( ; col_ < (width & ~3); col_ += 4)
        _blur_vertical_4wide_Kahan(buf + col_, height, width, radius, scratch);
      for( ; col_ < width; col_++)
        _blur_vertical_1wide_Kahan(buf + col_, height, width, radius, scratch);
    }
  }

  dt_free_align(scratch_buf);
}

static void dt_box_mean_4ch_Kahan(float *const buf,
    const size_t height,
    const size_t width,
    const int radius,
    const unsigned iterations)
{

  for(unsigned iteration = 0; iteration < iterations; iteration++)
  {
    size_t padded_size;
    float *const restrict scanlines = dt_alloc_perthread_float(4*width,&padded_size);
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(width, height, radius, padded_size) \
  dt_omp_sharedconst(buf, scanlines) \
  schedule(static)
#endif
    for(size_t row = 0; row < height; row++)
    {
      float *const restrict scratch = dt_get_perthread(scanlines,padded_size);
      _blur_horizontal_4ch_Kahan(buf + row * 4 * width, width, radius, scratch);
    }

    dt_free_align(scanlines);

    box_mean_vert_1ch_Kahan(buf, height, 4*width, radius);
  }

}

static inline void box_mean_2ch(float *const restrict in,
    const size_t height,
    const size_t width,
    const int radius,
    const unsigned iterations)
{
  // Compute in-place a box average (filter) on a multi-channel image over a window of size 2*radius + 1
  // We make use of the separable nature of the filter kernel to speed-up the computation
  // by convolving along columns and rows separately (complexity O(2 × radius) instead of O(radius²)).

  const size_t eff_height = _compute_effective_height(height, radius);
  const size_t Ndim = MAX(4*width,16*eff_height);
  size_t padded_size;
  float *const restrict temp = dt_alloc_perthread_float(Ndim, &padded_size);
  if(temp == NULL) return;

  for(unsigned iteration = 0; iteration < iterations; iteration++)
  {
    _blur_horizontal_2ch(in, height, width, radius, temp, padded_size);
    _blur_vertical_1ch(in, height, 2*width, radius, temp, padded_size);
  }
  dt_free_align(temp);
}

void dt_box_mean(float *const buf,
                 const size_t height,
                 const size_t width,
                 const int ch,
                 const int radius,
                 const unsigned iterations)
{
  if(ch == 1)
  {
    dt_box_mean_1ch(buf,height,width,radius,iterations);
  }
  else if(ch == 4)
  {
    dt_box_mean_4ch(buf,height,width,radius,iterations);
  }
  else if(ch == (4|BOXFILTER_KAHAN_SUM))
  {
    dt_box_mean_4ch_Kahan(buf,height,width,radius,iterations);
  }
  else if(ch == 2) // used by fast_guided_filter.h
  {
    box_mean_2ch(buf,height,width,radius,iterations);
  }
  else
    dt_unreachable_codepath();
}

void dt_box_mean_horizontal(float *const restrict buf,
    const size_t width,
    const int ch,
    const int radius,
    float *const restrict user_scratch)
{
  if(ch == (4|BOXFILTER_KAHAN_SUM))
  {
    float *const restrict scratch = user_scratch ? user_scratch : dt_alloc_align_float(4*width);
    if(scratch)
    {
      _blur_horizontal_4ch_Kahan(buf, width, radius, scratch);
      if(!user_scratch)
        dt_free_align(scratch);
    }
    else
      dt_print(DT_DEBUG_ALWAYS,"[box_mean] unable to allocate scratch memory\n");
  }
  else if(ch == (9|BOXFILTER_KAHAN_SUM))
  {
    float *const restrict scratch = user_scratch ? user_scratch : dt_alloc_align_float(9*width);
    if(scratch)
    {
      _blur_horizontal_Nch_Kahan(9, buf, width, radius, scratch);
      if(!user_scratch)
        dt_free_align(scratch);
    }
    else
      dt_print(DT_DEBUG_ALWAYS,"[box_mean] unable to allocate scratch memory\n");
  }
  else
    dt_unreachable_codepath();
}

void dt_box_mean_vertical(float *const buf,
    const size_t height,
    const size_t width,
    const int ch,
    const int radius)
{
  if((ch & BOXFILTER_KAHAN_SUM) && (ch & ~BOXFILTER_KAHAN_SUM) <= 16)
  {
    size_t channels = ch & ~BOXFILTER_KAHAN_SUM;
    box_mean_vert_1ch_Kahan(buf, height, channels*width, radius);
  }
  else
    dt_unreachable_codepath();
}

static inline float window_max(const float *x, int n)
{
  float m = -(FLT_MAX);
#ifdef _OPENMP
#pragma omp simd reduction(max : m)
#endif
  for(int j = 0; j < n; j++)
    m = MAX(m, x[j]);
  return m;
}

// calculate the one-dimensional moving maximum over a window of size 2*w+1
// input array x has stride 1, output array y has stride stride_y
static inline void box_max_1d(const int N,
                              const float *const restrict x,
                              float *const restrict y,
                              const size_t stride_y,
                              const int w)
{
  float m = window_max(x, MIN(w + 1, N));
  for(int i = 0; i < N; i++)
  {
    // store maximum of current window at center position
    y[i * stride_y] = m;
    // if the earliest member of the current window is the max, we need to
    // rescan the window to determine the new maximum
    if(i - w >= 0 && x[i - w] == m)
    {
      const int start = i - w + 1;
      m = window_max(x + start, MIN(i + w + 2, N) - start);
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
  for(size_t c = 0; c < 16; c++)
    out[c] = value;
}

static inline void update_max_16wide(float m[16],
    const float *const restrict base)
{
#ifdef _OPENMP
#pragma omp simd aligned(m, base : 64)
#endif
  for(size_t c = 0; c < 16; c++)
  {
    m[c] = fmaxf(m[c], base[c]);
  }
}

static inline void _load_update_max_16wide(float *const restrict out,
    float m[16],
    const float *const restrict base)
{
#ifdef _OPENMP
#pragma omp simd aligned(out, m : 64)
#endif
  for(size_t c = 0; c < 16; c++)
  {
    const float v = base[c];
    out[c] = v;
    m[c] = fmaxf(m[c], v);
  }
}

// calculate the one-dimensional moving maximum on four adjacent columns over a window of size 2*w+1
// input/output array 'buf' has stride 'stride' and we will write 16 consecutive elements every stride elements
// (thus processing a cache line at a time)
static inline void box_max_vert_16wide(const int N,
    float *const restrict scratch,
    float *const restrict buf,
    const int stride,
    const int w,
    const size_t mask)
{
  float DT_ALIGNED_ARRAY m[16] = { -(FLT_MAX), -(FLT_MAX), -(FLT_MAX), -(FLT_MAX),
                                   -(FLT_MAX), -(FLT_MAX), -(FLT_MAX), -(FLT_MAX),
                                   -(FLT_MAX), -(FLT_MAX), -(FLT_MAX), -(FLT_MAX),
                                   -(FLT_MAX), -(FLT_MAX), -(FLT_MAX), -(FLT_MAX) };
  for(size_t i = 0; i < MIN(w + 1, N); i++)
  {
    PREFETCH_NTA(buf + stride*(i+24));
    _load_update_max_16wide(scratch + 16 * (i&mask),m, buf + stride*i);
  }
  for(size_t i = 0; i < N; i++)
  {
    PREFETCH_NTA(buf + stride*(i+24));
    // store maximum of current window at center position
    _store_16wide(buf + stride * i, m);
    // If the earliest member of the current window is the max, we need to
    // rescan the window to determine the new maximum
    if(i >= w)
    {
      set_16wide(m, -(FLT_MAX));  // reset max values to lowest possible
      for(int j = i - w + 1; j < MIN(i + w + 1, N); j++)
      {
        update_max_16wide(m,scratch + 16*(j&mask));
      }
    }
    // if the window has not yet exceeded the end of the row/column, update the maximum value
    const size_t n = i + w + 1;
    if(n < N)
    {
      _load_update_max_16wide(scratch + 16 * (n&mask), m, buf + stride * n);
    }
  }
}

// calculate the two-dimensional moving maximum over a box of size (2*w+1) x (2*w+1)
// does the calculation in-place if input and output images are identical
static void box_max_1ch(float *const buf,
                        const size_t height,
                        const size_t width,
                        const unsigned w)
{
  const size_t eff_height = _compute_effective_height(height, w);
  const size_t scratch_size = MAX(width,MAX(height,16*eff_height));
  size_t allocsize;
  float *const restrict scratch_buffers = dt_alloc_perthread_float(scratch_size,&allocsize);
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(w, width, height, buf, allocsize) \
  dt_omp_sharedconst(scratch_buffers) \
  schedule(static)
#endif
  for(size_t row = 0; row < height; row++)
  {
    float *const restrict scratch = dt_get_perthread(scratch_buffers,allocsize);
    memcpy(scratch, buf + row * width, sizeof(float) * width);
    box_max_1d(width, scratch, buf + row * width, 1, w);
  }
#ifdef _OPENMP
#pragma omp parallel for default(none)           \
  dt_omp_firstprivate(w, width, height, buf, allocsize, eff_height) \
  dt_omp_sharedconst(scratch_buffers) \
  schedule(static)
#endif
  for(int col = 0; col < (width & ~15); col += 16)
  {
    float *const restrict scratch = dt_get_perthread(scratch_buffers,allocsize);
    box_max_vert_16wide(height, scratch, buf + col, width, w, eff_height-1);
  }
  // handle the leftover 0..15 columns
  for(size_t col = width & ~15 ; col < width; col++)
  {
    float *const restrict scratch = scratch_buffers;
    for(size_t row = 0; row < height; row++)
      scratch[row] = buf[row * width + col];
    box_max_1d(height, scratch, buf + col, width, w);
  }
  dt_free_align(scratch_buffers);
}


// in-place calculate the two-dimensional moving maximum over a box of size (2*radius+1) x (2*radius+1)
void dt_box_max(float *const buf,
                const size_t height,
                const size_t width,
                const int ch,
                const int radius)
{
  if(ch == 1)
    box_max_1ch(buf, height, width, radius);
  else
  //TODO: 4ch version if needed
    dt_unreachable_codepath();
}

static inline float window_min(const float *x, int n)
{
  float m = FLT_MAX;
#ifdef _OPENMP
#pragma omp simd reduction(min : m)
#endif
  for(int j = 0; j < n; j++)
    m = MIN(m, x[j]);
  return m;
}

// calculate the one-dimensional moving minimum over a window of size 2*w+1
// input array x has stride 1, output array y has stride stride_y
static inline void box_min_1d(int N, const float *x, float *y, size_t stride_y, int w)
{
  float m = window_min(x, MIN(w + 1, N));
  for(int i = 0; i < N; i++)
  {
    y[i * stride_y] = m;
    if(i - w >= 0 && x[i - w] == m)
    {
      const int start = (i - w + 1);
      m = window_min(x + start, MIN((i + w + 2), N) - start);
    }
    // if the window has not yet exceeded the end of the row/column, update the minimum value
    if(i + w + 1 < N)
      m = MIN(x[i + w + 1], m);
  }
}

static inline void update_min_16wide(float m[16], const float *const restrict base)
{
#ifdef _OPENMP
#pragma omp simd aligned(m, base : 64)
#endif
  for(size_t c = 0; c < 16; c++)
  {
    m[c] = fminf(m[c], base[c]);
  }
}

static inline void _load_update_min_16wide(float *const restrict out,
    float m[16],
    const float *const restrict base)
{
#ifdef _OPENMP
#pragma omp simd aligned(out, m : 64)
#endif
  for(size_t c = 0; c < 16; c++)
  {
    const float v = base[c];
    out[c] = v;
    m[c] = fminf(m[c], v);
  }
}

// calculate the one-dimensional moving minimum on four adjacent columns over a window of size 2*w+1
// input/output array 'buf' has stride 'stride' and we will write 16 consecutive elements every stride elements
// (thus processing a cache line at a time)
static inline void box_min_vert_16wide(const int N,
    float *const restrict scratch,
    float *const restrict buf,
    const int stride,
    const int w,
    const size_t mask)
{
  float DT_ALIGNED_ARRAY m[16] = { FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX,
                                   FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX,
                                   FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX,
                                   FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX };
  for(size_t i = 0; i < MIN(w + 1, N); i++)
  {
    PREFETCH_NTA(buf + stride*(i+24));
    _load_update_min_16wide(scratch + 16*(i&mask), m, buf + stride*i);
  }
  for(size_t i = 0; i < N; i++)
  {
    PREFETCH_NTA(buf + stride*(i+24));
    // store minimum of current window at center position
    _store_16wide(buf + i * stride, m);
    // If the earliest member of the current window is the min, we need to
    // rescan the window to determine the new minimum
    if(i >= w)
    {
      set_16wide(m, FLT_MAX);  // reset min values to the highest possible
      for(int j = i - w + 1; j < MIN(i + w + 1, N); j++)
      {
        update_min_16wide(m,scratch + 16*(j&mask));
      }
    }
    // if the window has not yet exceeded the end of the row/column, update the minimum value
    const size_t n = i + w + 1;
    if(n < N)
    {
      _load_update_min_16wide(scratch + 16 * (n&mask), m, buf + stride * n);
    }
  }
}


// calculate the two-dimensional moving minimum over a box of size (2*w+1) x (2*w+1)
// does the calculation in-place if input and output images are identical
static void box_min_1ch(float *const buf,
                        const size_t height,
                        const size_t width,
                        const int w)
{
  const size_t eff_height = _compute_effective_height(height, w);
  const size_t scratch_size = MAX(width,MAX(height,16*eff_height));
  size_t allocsize;
  float *const restrict scratch_buffers = dt_alloc_perthread_float(scratch_size,&allocsize);
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(w, width, height, buf, allocsize) \
  dt_omp_sharedconst(scratch_buffers) \
  schedule(static)
#endif
  for(size_t row = 0; row < height; row++)
  {
    float *const restrict scratch = dt_get_perthread(scratch_buffers,allocsize);
    memcpy(scratch, buf + row * width, sizeof(float) * width);
    box_min_1d(width, scratch, buf + row * width, 1, w);
  }
#ifdef _OPENMP
#pragma omp parallel for default(none)           \
  dt_omp_firstprivate(w, width, height, buf,allocsize, eff_height) \
  dt_omp_sharedconst(scratch_buffers) \
  schedule(static)
#endif
  for(size_t col = 0; col < (width & ~15); col += 16)
  {
    float *const restrict scratch = dt_get_perthread(scratch_buffers,allocsize);
    box_min_vert_16wide(height, scratch, buf + col, width, w, eff_height-1);
  }
  // handle the leftover 0..15 columns
  for(size_t col = width & ~15 ; col < width; col++)
  {
    float *const restrict scratch = scratch_buffers;
    for(size_t row = 0; row < height; row++)
      scratch[row] = buf[row * width + col];
    box_min_1d(height, scratch, buf + col, width, w);
  }

  dt_free_align(scratch_buffers);
}

void dt_box_min(float *const buf,
                const size_t height,
                const size_t width,
                const int ch,
                const int radius)
{
  if(ch == 1)
    box_min_1ch(buf, height, width, radius);
  else
  //TODO: 4ch version if needed
    dt_unreachable_codepath();
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
