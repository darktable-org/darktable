/*
    This file is part of darktable,
    Copyright (C) 2009-2024 darktable developers.

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

#ifdef __GNUC__
#pragma GCC optimize ("finite-math-only", "no-math-errno", "fp-contract=fast", "fast-math")
#endif

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

// specify the maximum number of channels (floats) to process at once in vectorizing.
// depends on the number and size of vector registers, and snould ideally be large
// enough to cover a full cache line
// IMPORTANT: must be a power of two!
// TODO: check if Apple silicon has the registers to handle 32 without spilling to
// memory during operations, since is has a larger cache line than x86
//#if defined(__APPLE__) && defined(__aarch64)
//#define MAX_VECT 32
//#else
#define MAX_VECT 16
//#endif

// Put the to-be-vectorized loop into a function by itself to nudge the compiler into actually vectorizing...
// With optimization enabled, this gets inlined and interleaved with other instructions as though it had been
// written in place, so we get a net win from better vectorization.
template <size_t N, bool compensated = false>
static void _load_add(float *const __restrict__ out,
                      float *const __restrict__ accum,
                      const float *const __restrict__ values,
                      float *const __restrict__ comp = nullptr)
{
  if(compensated)
  {
    DT_OMP_SIMD(aligned(accum, comp : 64))
    for(size_t c = 0; c < N; c++)
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
  else
  {
    DT_OMP_SIMD(aligned(accum : 64))
    for(size_t c = 0; c < N; c++)
    {
      const float v = values[c];
      out[c] = v;
      accum[c] += v;
    }
  }
}

template <size_t N, bool compensated = false>
static void _sub(float *const __restrict__ accum,
                 const float *const __restrict__ values,
                 float *const __restrict__ comp = nullptr)
{
  if(compensated)
  {
    DT_OMP_SIMD(aligned(accum,comp : 64))
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
  else
  {
    DT_OMP_SIMD(aligned(accum : 64))
    for(size_t c = 0; c < N; c++)
      accum[c] -= values[c];
  }
}

template <size_t N>
static void _set(float *const __restrict__ out, const float value)
{
  DT_OMP_SIMD(aligned(out : 64))
  for(size_t c = 0; c < N; c++)
    out[c] = value;
}

// copy N floats from aligned temporary space back to the possibly-unaligned user buffer
template <size_t N>
static void _store(float *const __restrict__ out,
                   const float *const __restrict__ in)
{
  DT_OMP_SIMD(aligned(in : 64))
  for(size_t c = 0; c < N; c++)
    out[c] = in[c];
}

template <size_t N>
static void _store_scaled(float *const __restrict__ out,
                          const float *const __restrict__ in,
                          const float scale)
{
  DT_OMP_SIMD(aligned(in : 64))
  for(size_t c = 0; c < N; c++)
    out[c] = in[c] / scale;
}

template<size_t N>
static inline void _update_max(float m[N],
                               const float *const __restrict__ base)
{
  DT_OMP_SIMD(aligned(m : 64))
  for(size_t c = 0; c < N; c++)
  {
    m[c] = fmaxf(m[c], base[c]);
  }
}

template<size_t N>
static inline void _load_update_max(float *const __restrict__ out,
                                    float m[N],
                                    const float *const __restrict__ base)
{
  DT_OMP_SIMD(aligned(m : 64))
  for(size_t c = 0; c < N; c++)
  {
    const float v = base[c];
    out[c] = v;
    m[c] = fmaxf(m[c], v);
  }
}

template <size_t N>
static inline void _update_min(float m[N], const float *const __restrict__ base)
{
  DT_OMP_SIMD(aligned(m : 64))
  for(size_t c = 0; c < N; c++)
  {
    m[c] = fminf(m[c], base[c]);
  }
}

template <size_t N>
static inline void _load_update_min(float *const __restrict__ out,
                                    float m[N],
                                    const float *const __restrict__ base)
{
  DT_OMP_SIMD(aligned(m : 64))
  for(size_t c = 0; c < N; c++)
  {
    const float v = base[c];
    out[c] = v;
    m[c] = fminf(m[c], v);
  }
}

// invoked inside an OpenMP parallel for, so no need to parallelize
template <size_t N, bool compensated = false>
static void _blur_horizontal(float *const __restrict__ buf,
                             const size_t width,
                             const size_t radius,
                             float *const __restrict__ scratch)
{
   float DT_ALIGNED_ARRAY L[N];
   float DT_ALIGNED_ARRAY comp[N];
   for(size_t i = 0; i < N; i++)
   {
     L[i] = 0.0f;
     comp[i] = 0.0f;
   }
   size_t hits = 0;
   // add up the left half of the window
   for(size_t x = 0; x < MIN(radius,width) ; x++)
   {
     hits++;
     _load_add<N,compensated>(scratch + N*x, L, buf + N*x, comp);
   }
   // process the blur up to the point where we start removing values from the moving average
   size_t x;
   for(x = 0; (x <= radius) && ((x + radius) < width); x++)
   {
     const int np = x + radius;
     hits++;
     _load_add<N,compensated>(scratch + N*np, L, buf + N*np, comp);
     _store_scaled<N>(buf + N*x, L, hits);
   }
   // if radius > width/2, we have pixels for which we can neither add new values (x+radius >= width) nor
   //  remove old values (x-radius < 0)
   for(; x <= radius && x < width; x++)
   {
     _store_scaled<N>(buf + N*x, L, hits);
   }
   // process the blur for the bulk of the scan line
   for(; x + radius < width; x++)
   {
     //very strange: if any of the 'op' or 'np' variables in this function are changed to either
     // 'unsigned' or 'size_t', the function runs a fair bit slower....
     const int op = x - radius - 1;
     const int np = x + radius;
     _sub<N,compensated>(L, scratch + N*op, comp);
     _load_add<N,compensated>(scratch + N*np, L, buf + N*np, comp);
     _store_scaled<N>(buf + N*x, L, hits);
   }
   // process the right end where we have no more values to add to the running sum
   for(; x < width; x++)
   {
     const int op = x - radius - 1;
     hits--;
     _sub<N,compensated>(L, scratch + N*op, comp);
     _store_scaled<N>(buf + N*x, L, hits);
   }
  return;
}

// invoked inside an OpenMP parallel for, so no need to parallelize
template <size_t N, bool compensated = false>
static void _blur_vertical(float *const __restrict__ buf,
    const size_t height,
    const size_t width,
    const size_t radius,
    float *const __restrict__ scratch)
{
  // To improve cache hit rates, we copy the final result from the scratch space back to the original
  // location in the buffer as soon as we finish the final read of the buffer.  To reduce the working
  // set and further improve cache hits, we can treat the scratch space as a circular buffer and cycle
  // through it repeatedly.  To use a simple bitmask instead of a division, the size we cycle through
  // needs to be the power of two larger than the window size (2*radius+1).
  size_t mask = 1;
  for(size_t r = (2*radius+1); r > 1 ; r >>= 1) mask = (mask << 1) | 1;

  float DT_ALIGNED_ARRAY L[N];
  float DT_ALIGNED_ARRAY comp[N];
  for(size_t i = 0; i < N; i++)
  {
    L[i] = 0.0f;
    comp[i] = 0.0f;
  }
  size_t hits = 0;
  // add up the left half of the window
  for(size_t y = 0; y < MIN(radius, height); y++)
  {
    hits++;
    _load_add<N,compensated>(scratch + N*(y&mask), L, buf + y * width, comp);
  }
  // process the blur up to the point where we start removing values from the moving average
  size_t y;
  for(y = 0; y <= radius && y + radius < height; y++)
  {
    // weirdly, changing any of the 'np' or 'op' variables in this function to 'size_t' yields a substantial slowdown!
    const int np = y + radius;
    hits++;
    _load_add<N,compensated>(scratch + N*(np&mask), L, buf + np*width, comp);
    _store_scaled<N>(buf + y*width, L, hits);
  }
  // if radius > height/2, we have pixels for which we can neither add new values (y+radius >= height) nor
  //  remove old values (y-radius < 0)
  for(; y <= radius && y < height; y++)
  {
    _store_scaled<N>(buf + y*width, L, hits);
  }
  // process the blur for the bulk of the column
  for( ; y + radius < height; y++)
  {
    const int np = y + radius;
    const int op = y - radius - 1;
    _sub<N,compensated>(L, scratch + N*(op&mask), comp);
    _load_add<N,compensated>(scratch + N*(np&mask), L, buf + np*width, comp);
    _store_scaled<N>(buf + y*width, L, hits);
  }
  // process the blur for the end of the scan line, where we don't have any more values to add to the mean
  for( ; y < height; y++)
  {
    const int op = y - radius - 1;
    hits--;
    _sub<N,compensated>(L, scratch + N*(op&mask), comp);
    _store_scaled<N>(buf + y*width, L, hits);
  }
  return;
}

template<bool compensated>
static void _blur_vertical_1ch(float *const __restrict__ buf,
                               const size_t height,
                               const size_t width,
                               const size_t radius,
                               float *const __restrict__ scanlines,
                               const size_t padded_size)
{
  DT_OMP_FOR()
  for(size_t x = 0; x < width; x += MAX_VECT)
  {
    float *const __restrict__ scratch = (float*)dt_get_perthread(scanlines,padded_size);
    if(x + MAX_VECT <= width)
    {
      _blur_vertical<MAX_VECT,compensated>(buf + x, height, width, radius, scratch);
    }
    else
    {
      // handle the leftover 1..(MAX_VECT-1) columns, first in groups of four, then the final 0..3 singly
      size_t col = x;
      for( ; col < (width & ~3); col += 4)
	_blur_vertical<4,compensated>(buf + col, height, width, radius, scratch);
      for( ; col < width; col++)
	_blur_vertical<1,compensated>(buf + col, height, width, radius, scratch);
    }
  }
  return;
}

// determine the size of the scratch buffer needed for vertical passes of the box-mean filter
// filter_window = 2**ceil(lg2(2*radius+1))
static inline size_t _compute_effective_height(const size_t height, const size_t radius)
{
  size_t eff_height = 2;
  for(size_t r = (2*radius+1); r > 1 ; r >>= 1) eff_height <<= 1;
  eff_height = MIN(eff_height,height);
  return eff_height;
}

static float *_alloc_scratch_space(const size_t N,
   const size_t height,
   const size_t width,
   const size_t radius,
   size_t *padded_size)
{
  // scratch space needed per thread:
  //   N*width floats to store one row during horizontal pass
  //   MAX_VECT*filter_window floats for vertical pass
  const size_t eff_height = _compute_effective_height(height,radius);
  const size_t size = MAX(N*width,MAX(height,MAX_VECT*eff_height));
  return dt_alloc_perthread_float(size, padded_size);
}

template <size_t N, bool compensated = false>
static void _box_mean(float *const buf,
                      const size_t height,
                      const size_t width,
                      const size_t radius,
                      const uint32_t iterations)
{
  // Compute in-place a box average (filter) on a multi-channel image over a window of size 2*radius + 1
  // We make use of the separable nature of the filter kernel to speed-up the computation
  // by convolving along columns and rows separately (complexity O(2 × radius) instead of O(radius²)).

  size_t padded_size;
  float *const __restrict__ scanlines = _alloc_scratch_space(N, height, width, radius, &padded_size);
  if(scanlines == NULL) return;

  for(uint32_t iteration = 0; iteration < iterations; iteration++)
  {
    DT_OMP_FOR()
    for(size_t row = 0; row < height; row++)
    {
      float *const __restrict__ scratch = (float*)dt_get_perthread(scanlines,padded_size);
      _blur_horizontal<N,compensated>(buf + row * N * width, width, radius, scratch);
    }
    // we need to multiply width by N to get the correct stride for the vertical blur
    _blur_vertical_1ch<compensated>(buf, height, N*width, radius, scanlines, padded_size);
  }
  dt_free_align(scanlines);
}

static inline float _window_max(const float *x, int n)
{
  float m = -(FLT_MAX);
  for(int j = 0; j < n; j++)
    m = MAX(m, x[j]);
  return m;
}

// calculate the one-dimensional moving maximum over a window of size 2*w+1
static inline void box_max_1d(const int N,
                              const float *const __restrict__ x,
                              float *const __restrict__ y,
                              const int w)
{
  float m = _window_max(x, MIN(w + 1, N));
  for(int i = 0; i < N; i++)
  {
    // store maximum of current window at center position
    y[i] = m;
    // if the earliest member of the current window is the max, we need to
    // rescan the window to determine the new maximum
    if(i - w >= 0 && x[i - w] == m)
    {
      const int start = i - w + 1;
      m = _window_max(x + start, MIN(i + w + 2, N) - start);
    }
    // if the window has not yet exceeded the end of the row/column, update the maximum value
    if(i + w + 1 < N)
      m = MAX(x[i + w + 1], m);
  }
}

// calculate the one-dimensional moving maximum on four adjacent columns over a window of size 2*w+1
// input/output array 'buf' has stride 'stride' and we will write N consecutive elements every stride elements
// (thus processing a cache line at a time if N==MAX_VECT)
template <size_t N>
static inline void _box_max_vert(const unsigned height,
    float *const __restrict__ scratch,
    float *const __restrict__ buf,
    const size_t stride,
    const unsigned w,
    const size_t mask)
{
  float DT_ALIGNED_ARRAY m[N];
  for(size_t i = 0; i < N; i++)
     m[i] = -(FLT_MAX);
  for(size_t i = 0; i < MIN(w + 1, height); i++)
  {
    PREFETCH_NTA(buf + stride*(i+24));
    _load_update_max<N>(scratch + N * (i&mask),m, buf + stride*i);
  }
  for(size_t i = 0; i < height; i++)
  {
    PREFETCH_NTA(buf + stride*(i+24));
    // store maximum of current window at center position
    _store<N>(buf + stride * i, m);
    // If the earliest member of the current window is the max, we need to
    // rescan the window to determine the new maximum
    if(i >= w)
    {
      _set<N>(m, -(FLT_MAX));  // reset max values to lowest possible
      for(size_t j = i - w + 1; j < MIN(i + w + 1, height); j++)
      {
        _update_max<N>(m,scratch + N*(j&mask));
      }
    }
    // if the window has not yet exceeded the end of the row/column, update the maximum value
    const size_t n = i + w + 1;
    if(n < height)
    {
      _load_update_max<N>(scratch + N * (n&mask), m, buf + stride * n);
    }
  }
}

// calculate the two-dimensional moving maximum over a box of size (2*w+1) x (2*w+1)
// does the calculation in-place if input and output images are identical
static void _box_max_1ch(float *const buf,
                        const size_t height,
                        const size_t width,
                        const unsigned w)
{
  const size_t eff_height = _compute_effective_height(height, w);
  const size_t scratch_size = MAX(width,MAX(height,MAX_VECT*eff_height));
  size_t allocsize;
  float *const __restrict__ scratch_buffers = dt_alloc_perthread_float(scratch_size, &allocsize);
  if(scratch_buffers == NULL) return;

  DT_OMP_FOR()
  for(size_t row = 0; row < height; row++)
  {
    float *const __restrict__ scratch = (float*)dt_get_perthread(scratch_buffers,allocsize);
    memcpy(scratch, buf + row * width, sizeof(float) * width);
    box_max_1d(width, scratch, buf + row * width, w);
  }
  DT_OMP_FOR()
  for(size_t col = 0; col < (width & ~(MAX_VECT-1)); col += MAX_VECT)
  {
    float *const __restrict__ scratch = (float*)dt_get_perthread(scratch_buffers,allocsize);
    _box_max_vert<MAX_VECT>(height, scratch, buf + col, width, w, eff_height-1);
  }
  // handle the leftover 0..(MAX_VECT-1) columns, first in groups of four, then the final 0..3 singly
  size_t col = width & ~(MAX_VECT-1);
  for( ; col < (width & ~3); col += 4)
    _box_max_vert<4>(height, scratch_buffers, buf + col, width, w, eff_height-1);
  for( ; col < width; col++)
    _box_max_vert<1>(height, scratch_buffers, buf + col, width, w, eff_height-1);
  dt_free_align(scratch_buffers);
}

static inline float _window_min(const float *x, int n)
{
  float m = FLT_MAX;
  for(int j = 0; j < n; j++)
    m = MIN(m, x[j]);
  return m;
}

// calculate the one-dimensional moving minimum over a window of size 2*w+1
static inline void _box_min_1d(int N, const float *x, float *y, int w)
{
  float m = _window_min(x, MIN(w + 1, N));
  for(int i = 0; i < N; i++)
  {
    y[i] = m;
    if(i - w >= 0 && x[i - w] == m)
    {
      const int start = (i - w + 1);
      m = _window_min(x + start, MIN((i + w + 2), N) - start);
    }
    // if the window has not yet exceeded the end of the row/column, update the minimum value
    if(i + w + 1 < N)
      m = MIN(x[i + w + 1], m);
  }
}

// calculate the one-dimensional moving minimum on four adjacent columns over a window of size 2*w+1
// input/output array 'buf' has stride 'stride' and we will write N consecutive elements every stride elements
// (thus processing a cache line at a time when N == MAX_VECT)
template <size_t N>
static inline void _box_min_vert(const unsigned height,
    float *const __restrict__ scratch,
    float *const __restrict__ buf,
    const int stride,
    const unsigned w,
    const size_t mask)
{
  float DT_ALIGNED_ARRAY m[N];
  for(size_t i = 0; i < N; i++)
     m[i] = FLT_MAX;
  for(size_t i = 0; i < MIN(w + 1, height); i++)
  {
    PREFETCH_NTA(buf + stride*(i+24));
    _load_update_min<N>(scratch + N*(i&mask), m, buf + stride*i);
  }
  for(size_t i = 0; i < height; i++)
  {
    PREFETCH_NTA(buf + stride*(i+24));
    // store minimum of current window at center position
    _store<N>(buf + i * stride, m);
    // If the earliest member of the current window is the min, we need to
    // rescan the window to determine the new minimum
    if(i >= w)
    {
      _set<N>(m, FLT_MAX);  // reset min values to the highest possible
      for(size_t j = i - w + 1; j < MIN(i + w + 1, height); j++)
      {
        _update_min<N>(m,scratch + N*(j&mask));
      }
    }
    // if the window has not yet exceeded the end of the row/column, update the minimum value
    const size_t n = i + w + 1;
    if(n < height)
    {
      _load_update_min<N>(scratch + N * (n&mask), m, buf + stride * n);
    }
  }
}

// calculate the two-dimensional moving minimum over a box of size (2*w+1) x (2*w+1)
// does the calculation in-place if input and output images are identical
static void _box_min_1ch(float *const buf,
                        const size_t height,
                        const size_t width,
                        const unsigned w)
{
  const size_t eff_height = _compute_effective_height(height, w);
  const size_t scratch_size = MAX(width,MAX(height,MAX_VECT*eff_height));
  size_t allocsize;
  float *const __restrict__ scratch_buffers = dt_alloc_perthread_float(scratch_size,&allocsize);
  if(scratch_buffers == NULL) return;

  DT_OMP_FOR()
  for(size_t row = 0; row < height; row++)
  {
    float *const __restrict__ scratch = (float*)dt_get_perthread(scratch_buffers,allocsize);
    memcpy(scratch, buf + row * width, sizeof(float) * width);
    _box_min_1d(width, scratch, buf + row * width, w);
  }
  DT_OMP_FOR()
  for(size_t col = 0; col < (width & ~(MAX_VECT-1)); col += MAX_VECT)
  {
    float *const __restrict__ scratch = (float*)dt_get_perthread(scratch_buffers,allocsize);
    _box_min_vert<MAX_VECT>(height, scratch, buf + col, width, w, eff_height-1);
  }
  // handle the leftover 0..(MAX_VECT-1) columns, first in groups of four, then the final 0..3 singly
  size_t col = width & ~(MAX_VECT-1);
  for( ; col < (width & ~3); col += 4)
    _box_min_vert<4>(height, scratch_buffers, buf + col, width, w, eff_height-1);
  for( ; col < width; col++)
    _box_min_vert<1>(height, scratch_buffers, buf + col, width, w, eff_height-1);
  dt_free_align(scratch_buffers);
}

void dt_box_mean(float *const buf,
                 const size_t height,
                 const size_t width,
                 const uint32_t ch,
                 const size_t radius,
                 const uint32_t iterations)
{
  if(ch == 1)
  {
    _box_mean<1>(buf,height,width,radius,iterations);
  }
  else if(ch == 2) // used by fast_guided_filter.h
  {
    _box_mean<2>(buf,height,width,radius,iterations);
  }
  else if(ch == 4)
  {
    _box_mean<4>(buf,height,width,radius,iterations);
  }
  else if(ch == (2|BOXFILTER_KAHAN_SUM))
  {
    _box_mean<2,true>(buf,height,width,radius,iterations);
  }
  else if(ch == (4|BOXFILTER_KAHAN_SUM))
  {
    _box_mean<4,true>(buf,height,width,radius,iterations);
  }
  else
    dt_unreachable_codepath();
}

void dt_box_mean_horizontal(float *const __restrict__ buf,
    const size_t width,
    const uint32_t ch,
    const size_t radius,
    float *const __restrict__ user_scratch)
{
  if(ch == (4|BOXFILTER_KAHAN_SUM))
  {
    float *const __restrict__ scratch
       = user_scratch ? user_scratch : dt_alloc_align_float(4 * dt_round_size(width, MAX_VECT));
    if(scratch)
    {
      _blur_horizontal<4,true>(buf, width, radius, scratch);
      if(!user_scratch)
        dt_free_align(scratch);
    }
    else
      dt_print(DT_DEBUG_ALWAYS, "[box_mean] unable to allocate scratch memory");
  }
  else if(ch == (9|BOXFILTER_KAHAN_SUM))
  {
    float *const __restrict__ scratch
       = user_scratch ? user_scratch : dt_alloc_align_float(9 * dt_round_size(width, MAX_VECT));
    if(scratch)
    {
      _blur_horizontal<9,true>(buf, width, radius, scratch);
      if(!user_scratch)
        dt_free_align(scratch);
    }
    else
      dt_print(DT_DEBUG_ALWAYS, "[box_mean] unable to allocate scratch memory");
  }
  else
    dt_unreachable_codepath();
}

void dt_box_mean_vertical(float *const buf,
    const size_t height,
    const size_t width,
    const uint32_t ch,
    const size_t radius)
{
  if((ch & BOXFILTER_KAHAN_SUM) && (ch & ~BOXFILTER_KAHAN_SUM) <= 16)
  {
    size_t channels = ch & ~BOXFILTER_KAHAN_SUM;
    size_t padded_size;
    float *const __restrict__ scratch_buf = _alloc_scratch_space(channels, height, width, radius, &padded_size);
    if(scratch_buf == NULL) return;

    _blur_vertical_1ch<true>(buf, height, channels*width, radius, scratch_buf, padded_size);
    dt_free_align(scratch_buf);
  }
  else
    dt_unreachable_codepath();
}

// in-place calculate the two-dimensional moving minimum over a box of size (2*radius+1) x (2*radius+1)
void dt_box_min(float *const buf,
                const size_t height,
                const size_t width,
                const uint32_t ch,
                const size_t radius)
{
  if(ch == 1)
    _box_min_1ch(buf, height, width, radius);
  else
  //TODO: 4ch version if needed
    dt_unreachable_codepath();
}

// in-place calculate the two-dimensional moving maximum over a box of size (2*radius+1) x (2*radius+1)
void dt_box_max(float *const buf,
                const size_t height,
                const size_t width,
                const uint32_t ch,
                const size_t radius)
{
  if(ch == 1)
    _box_max_1ch(buf, height, width, radius);
  else
  //TODO: 4ch version if needed
    dt_unreachable_codepath();
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
