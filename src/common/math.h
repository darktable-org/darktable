/*
 *    This file is part of darktable,
 *    Copyright (C) 2018-2023 darktable developers.
 *
 *    darktable is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 *
 *    darktable is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <stddef.h>
#include <math.h>
#include <stdint.h>
#ifdef __SSE__
#include <xmmintrin.h>
#include "common/sse.h"
#endif
#include "common/darktable.h"

#define NORM_MIN 1.52587890625e-05f // norm can't be < to 2^(-16)

// work around missing standard math.h symbols
/** ln(10) */
#ifndef M_LN10
#define M_LN10 2.30258509299404568402
#endif /* !M_LN10 */

/** PI */
#ifndef M_PI
#define M_PI   3.14159265358979323846
#endif /* !M_PI */
#ifndef M_PI_F
#define M_PI_F  3.14159265358979324f
#endif /* !M_PI_F */


#define DT_M_PI_F (3.14159265358979324f)
#define DT_M_PI (3.14159265358979324)

#define DT_M_LN2f (0.6931471805599453f)

// clip channel value to be between 0 and 1
// NaN-safe: NaN compares false and will result in 0.0
// also does not force promotion of floats to doubles, but will use the type of its argument
#define CLIP(x) (((x) >= 0) ? ((x) <= 1 ? (x) : 1) : 0)
#define MM_CLIP_PS(X) (_mm_min_ps(_mm_max_ps((X), _mm_setzero_ps()), _mm_set1_ps(1.0)))

// clip luminance values to be between 0 and 100
#define LCLIP(x) ((x < 0) ? 0.0 : (x > 100.0) ? 100.0 : x)

// clamp value to lie between mn and mx
// Nan-safe: NaN compares false and will result in mn
#define CLAMPF(a, mn, mx) ((a) >= (mn) ? ((a) <= (mx) ? (a) : (mx)) : (mn))
//#define CLAMPF(a, mn, mx) ((a) < (mn) ? (mn) : ((a) > (mx) ? (mx) : (a)))

#if defined(__SSE__)
#define MMCLAMPPS(a, mn, mx) (_mm_min_ps((mx), _mm_max_ps((a), (mn))))
#endif

static inline float clamp_range_f(const float x, const float low, const float high)
{
  return x > high ? high : (x < low ? low : x);
}

// test floats difference smaller than eps
static inline gboolean feqf(const float v1, const float v2, const float eps)
{
  return (fabsf(v1 - v2) < eps);
}

// We don't want to use the SIMD version sqf() in cases we might access unaligned memory
static inline float sqrf(const float a)
{
  return a * a;
}

// taken from rt code: calculate a * b + (1 - a) * c
static inline float interpolatef(const float a, const float b, const float c)
{
  return a * (b - c) + c;
}

// Kahan summation algorithm
#ifdef _OPENMP
#pragma omp declare simd aligned(c)
#endif
static inline float Kahan_sum(const float m, float *const __restrict__ c, const float add)
{
   const float t1 = add - (*c);
   const float t2 = m + t1;
   *c = (t2 - m) - t1;
   return t2;
}

static inline float Log2(const float x)
{
  return (x > 0.0f) ? (logf(x) / DT_M_LN2f) : x;
}

static inline float Log2Thres(const float x, const float Thres)
{
  return logf(x > Thres ? x : Thres) / DT_M_LN2f;
}

// ensure that any changes here are synchronized with data/kernels/extended.cl
static inline float fastlog2(const float x)
{
  union { float f; uint32_t i; } vx = { x };
  union { uint32_t i; float f; } mx = { (vx.i & 0x007FFFFF) | 0x3f000000 };

  float y = vx.i;

  y *= 1.1920928955078125e-7f;

  return y - 124.22551499f
    - 1.498030302f * mx.f
    - 1.72587999f / (0.3520887068f + mx.f);
}

// ensure that any changes here are synchronized with data/kernels/extended.cl
static inline float fastlog (const float x)
{
  return DT_M_LN2f * fastlog2(x);
}

// multiply 3x3 matrix with 3x1 vector
// dest needs to be different from v
#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline void mat3mulv(float *const __restrict__ dest,
                            const float *const mat,
                            const float *const __restrict__ v)
{
  for(int k = 0; k < 3; k++)
  {
    float x = 0.0f;
    for(int i = 0; i < 3; i++)
      x += mat[3 * k + i] * v[i];

    dest[k] = x;
  }
}

// multiply two 3x3 matrices
// dest needs to be different from m1 and m2
// dest = m1 * m2 in this order
#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline void mat3mul(float *const __restrict__ dest,
                           const float *const __restrict__ m1,
                           const float *const __restrict__ m2)
{
  for(int k = 0; k < 3; k++)
  {
    for(int i = 0; i < 3; i++)
    {
      float x = 0.0f;
      for(int j = 0; j < 3; j++)
        x += m1[3 * k + j] * m2[3 * j + i];

      dest[3 * k + i] = x;
    }
  }
}

// multiply two padded 3x3 matrices
// dest needs to be different from m1 and m2
// dest = m1 * m2 in this order
#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline void mat3SSEmul(dt_colormatrix_t dest,
                              const dt_colormatrix_t m1,
                              const dt_colormatrix_t m2)
{
  for(int k = 0; k < 3; k++)
  {
    for(int i = 0; i < 3; i++)
    {
      float x = 0.0f;
      for(int j = 0; j < 3; j++)
        x += m1[k][j] * m2[j][i];

      dest[k][i] = x;
    }
  }
}

#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline void mul_mat_vec_2(const float *m, const float *p, float *o)
{
  o[0] = p[0] * m[0] + p[1] * m[1];
  o[1] = p[0] * m[2] + p[1] * m[3];
}

#ifdef _OPENMP
#pragma omp declare simd uniform(v_2) aligned(v_1, v_2:16)
#endif
static inline float scalar_product(const dt_aligned_pixel_t v_1,
                                   const dt_aligned_pixel_t v_2)
{
  // specialized 3×1 dot products 2 4×1 RGB-alpha pixels.
  // v_2 needs to be uniform along loop increments, e.g. independent from current pixel values
  // we force an order of computation similar to SSE4 _mm_dp_ps() hoping the compiler will get the clue
  float acc = 0.f;

#ifdef _OPENMP
#pragma omp simd aligned(v_1, v_2:16) reduction(+:acc)
#endif
  for(size_t c = 0; c < 3; c++)
    acc += v_1[c] * v_2[c];

  return acc;
}


#ifdef _OPENMP
#pragma omp declare simd uniform(M) aligned(M:64) aligned(v_in, v_out:16)
#endif
static inline void dot_product(const dt_aligned_pixel_t v_in,
                               const dt_colormatrix_t M,
                               dt_aligned_pixel_t v_out)
{
  // specialized 3×4 dot products of 4×1 RGB-alpha pixels
  #ifdef _OPENMP
  #pragma omp simd aligned(M:64) aligned(v_in, v_out:16)
  #endif
  for(size_t i = 0; i < 3; ++i)
    v_out[i] = scalar_product(v_in, M[i]);
}


#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline float sqf(const float x)
{
  return x * x;
}


#ifdef _OPENMP
#pragma omp declare simd aligned(vector:16)
#endif
static inline float euclidean_norm(const dt_aligned_pixel_t vector)
{
  return fmaxf(sqrtf(sqf(vector[0]) + sqf(vector[1]) + sqf(vector[2])), NORM_MIN);
}


#ifdef _OPENMP
#pragma omp declare simd aligned(vector:16)
#endif
static inline void downscale_vector(dt_aligned_pixel_t vector, const float scaling)
{
  // check zero or NaN
  const int valid = (scaling > NORM_MIN) && !isnan(scaling);
  for(size_t c = 0; c < 3; c++)
    vector[c] = (valid) ? vector[c] / (scaling + NORM_MIN) : vector[c] / NORM_MIN;
}


#ifdef _OPENMP
#pragma omp declare simd aligned(vector:16)
#endif
static inline void upscale_vector(dt_aligned_pixel_t vector, const float scaling)
{
  const int valid = (scaling > NORM_MIN) && !isnan(scaling);
  for(size_t c = 0; c < 3; c++)
    vector[c] = (valid) ? vector[c] * (scaling + NORM_MIN) : vector[c] * NORM_MIN;
}


#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline float dt_log2f(const float f)
{
#ifdef __GLIBC__
  return log2f(f);
#else
  return logf(f) / logf(2.0f);
#endif
}

union float_int {
  float f;
  int k;
};

// a faster, vectorizable version of hypotf() when we know that there won't be overflow, NaNs, or infinities
#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline float dt_fast_hypotf(const float x, const float y)
{
  return sqrtf(x * x + y * y);
}

// fast approximation of expf()
/****** if you change this function, you need to make the same change in data/kernels/{basecurve,basic}.cl ***/
#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline float dt_fast_expf(const float x)
{
  // meant for the range [-100.0f, 0.0f]. largest error ~ -0.06 at 0.0f.
  // will get _a_lot_ worse for x > 0.0f (9000 at 10.0f)..
  const int i1 = 0x3f800000u;
  // e^x, the comment would be 2^x
  const int i2 = 0x402DF854u; // 0x40000000u;
  // const int k = CLAMPS(i1 + x * (i2 - i1), 0x0u, 0x7fffffffu);
  // without max clamping (doesn't work for large x, but is faster):
  const int k0 = i1 + x * (i2 - i1);
  union float_int u;
  u.k = k0 > 0 ? k0 : 0;
  return u.f;
}

// fast approximation of 2^-x for 0<x<126
/****** if you change this function, you need to make the same change in data/kernels/{denoiseprofile,nlmeans}.cl ***/
static inline float dt_fast_mexp2f(const float x)
{
  const int i1 = 0x3f800000; // bit representation of 2^0
  const int i2 = 0x3f000000; // bit representation of 2^-1
  const int k0 = i1 + (int)(x * (i2 - i1));
  union {
    float f;
    int i;
  } k;
  k.i = k0 >= 0x800000 ? k0 : 0;
  return k.f;
}

// The below version is incorrect, suffering from reduced precision.
// It is used by the non-local means code in both nlmeans.c and
// denoiseprofile.c, and fixing it would cause a change in output.
static inline float fast_mexp2f(const float x)
{
  const float i1 = (float)0x3f800000u; // 2^0
  const float i2 = (float)0x3f000000u; // 2^-1
  const float k0 = i1 + x * (i2 - i1);
  union {
    float f;
    int i;
  } k;
  k.i = k0 >= (float)0x800000u ? k0 : 0;
  return k.f;
}

/** Compute ceil value of a float
 * @remark Avoid libc ceil for now. Maybe we'll revert to libc later.
 * @param x Value to ceil
 * @return ceil value
 */
static inline float ceil_fast(const float x)
{
  if(x <= 0.f)
  {
    return (float)(int)x;
  }
  else
  {
    return -((float)(int)-x) + 1.f;
  }
}

static inline void dt_vector_log2(const dt_aligned_pixel_t x, dt_aligned_pixel_t res)
{
#ifdef __SSE2__
  *((__m128*)res) = _mm_log2_ps(*((__m128*)x));
#else
  union { float f[4]; uint32_t i[4]; } vx = { .f = { x[0], x[1], x[2], x[3] } };
  union { uint32_t i[4]; float f[4]; } mx;

#ifdef _OPENMP
#pragma omp simd aligned(x, res)
#endif
  for(size_t c = 0; c < 4; c++)
  {
  mx.i[c] = vx.i[c] & 0x007FFFFF;
  }
#ifdef _OPENMP
#pragma omp simd aligned(x, res)
#endif
  for(size_t c = 0; c < 4; c++)
  {
  float y = vx.i[c];
  float f = mx.f[c];
  y *= 1.1920928955078125e-7f;
  res[c] = (y - 124.22551499f - (1.498030302f * f) - 1.72587999f / (0.3520887068f + f));
  }
#endif
}

static inline void dt_vector_exp(const dt_aligned_pixel_t x, dt_aligned_pixel_t result)
{
  // meant for the range [-100.0f, 0.0f]. largest error ~ -0.06 at 0.0f.
  // will get _a_lot_ worse for x > 0.0f (9000 at 10.0f)..
  const int i1 = 0x3f800000u;
  // e^x, the comment would be 2^x
  const int i2 = 0x402DF854u; // 0x40000000u;
  // const int k = CLAMPS(i1 + x * (i2 - i1), 0x0u, 0x7fffffffu);
  // without max clamping (doesn't work for large x, but is faster):
  union float_int u[4];
  for_four_channels(c, aligned(x, result))
  {
    const int k0 = i1 + (int)(x[c] * (i2 - i1));
    u[c].k = k0 > 0 ? k0 : 0;
    result[c] = u[c].f;
  }
}

static inline void dt_vector_exp2(const dt_aligned_pixel_t x, dt_aligned_pixel_t res)
{
#ifdef __SSE2__
  *((__m128*)res) = _mm_exp2_ps(*((__m128*)x));
#else
  //TODO: plain C implementation of _mm_exp2_ps from sse.h
  for_four_channels(c,aligned(x,res))
    res[c] = exp2f(x[c]);
#endif
}

static inline void dt_vector_exp10(const dt_aligned_pixel_t x, dt_aligned_pixel_t res)
{
  // 10^x == 2^(3.3219280948873626 * x)
  dt_aligned_pixel_t scaled;
  for_four_channels(c,aligned(x,scaled))
    scaled[c] = 3.3219280948873626f * x[c];
  dt_vector_exp2(scaled, res);
}

static inline void dt_vector_powf(const dt_aligned_pixel_t input,
                                  const dt_aligned_pixel_t power,
                                  dt_aligned_pixel_t output)
{
#ifdef __SSE__
    *((__m128*)output) = _mm_pow_ps(*((__m128*)input), *((__m128*)power));
#else
    for_four_channels(c)
    {
      // Apply the transfer function of the display
      output[c] = powf(input[c], power[c]);
    }
#endif
}

static inline void dt_vector_min(dt_aligned_pixel_t min,
                                 const dt_aligned_pixel_t v1,
                                 const dt_aligned_pixel_t v2)
{
#ifdef __SSE__
  *((__m128*)min) = _mm_min_ps(*((__m128*)v1), *((__m128*)v2));
#else
  for_each_channel(c)
    min[c] = MIN(v1[c], v2[c]);
#endif
}

static inline void dt_vector_max(dt_aligned_pixel_t max,
                                 const dt_aligned_pixel_t v1,
                                 const dt_aligned_pixel_t v2)
{
#ifdef __SSE__
  *((__m128*)max) = _mm_max_ps(*((__m128*)v1), *((__m128*)v2));
#else
  for_each_channel(c)
    max[c] = MAX(v1[c], v2[c]);
#endif
}

static inline float dt_vector_channel_max(const dt_aligned_pixel_t pixel)
{
  dt_aligned_pixel_t swapRG = { pixel[1], pixel[0], pixel[2], pixel[3] };
  dt_aligned_pixel_t swapRB = { pixel[2], pixel[1], pixel[0], pixel[3] };
  dt_aligned_pixel_t maximum;
  for_each_channel(c)
    maximum[c] = MAX(MAX(pixel[c], swapRG[c]), swapRB[c]);
  return maximum[0];
}

static inline void dt_vector_clip(dt_aligned_pixel_t values)
{
#ifdef __SSE__
  static const __m128 zero = { 0.0f, 0.0f, 0.0f, 0.0f };
  static const __m128 one = { 1.0f, 1.0f, 1.0f, 1.0f };
  *((__m128*)values) = _mm_min_ps(_mm_max_ps(*((__m128*)values), zero), one);
#else
  for_each_channel(c)
    values[c] = CLIP(values[c]);
#endif
}

/** Compute approximate sines, four at a time.
 * This function behaves correctly for the range [-pi pi] only.
 * It has the following properties:
 * <ul>
 *   <li>It has exact values for 0, pi/2, pi, -pi/2, -pi</li>
 *   <li>It has matching derivatives to sine for these same points</li>
 *   <li>Its relative error margin is <= 1% iirc</li>
 *   <li>It computational cost is 5 mults + 3 adds + 2 abs</li>
 * </ul>
 * @param arg: Radian parameters
 * @return sine: guess what
 */
static inline void dt_vector_sin(const dt_aligned_pixel_t arg, dt_aligned_pixel_t sine)
{
  static const dt_aligned_pixel_t pi = { M_PI_F, M_PI_F, M_PI_F, M_PI_F };
  static const dt_aligned_pixel_t a
    = { 4 / (M_PI_F * M_PI_F),
        4 / (M_PI_F * M_PI_F),
        4 / (M_PI_F * M_PI_F),
        4 / (M_PI_F * M_PI_F) };
  static const dt_aligned_pixel_t p = { 0.225f,  0.225f, 0.225f, 0.225f };
  static const dt_aligned_pixel_t one = { 1.0f, 1.0f, 1.0f, 1.0f };

  dt_aligned_pixel_t abs_arg;
  for_four_channels(c)
    abs_arg[c] = (arg[c] < 0.0f) ? -arg[c] : arg[c];
  dt_aligned_pixel_t scaled;
  for_four_channels(c)
    scaled[c] = a[c] * arg[c] * (pi[c] - abs_arg[c]);
  dt_aligned_pixel_t abs_scaled;
  for_four_channels(c)
    abs_scaled[c] = (scaled[c] < 0.0f) ? -scaled[c] : scaled[c];
  for_four_channels(c)
    sine[c] = scaled[c] * (p[c] * (abs_scaled[c] - one[c]) + one[c]);
}


// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
