/*
 *    This file is part of darktable,
 *    Copyright (C) 2018-2024 darktable developers.
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
#include "common/sse.h"		// also includes darktable.h

#define LUT_ELEM 512 // gamut LUT number of elements:

#define NORM_MIN 1.52587890625e-05f // norm can't be < to 2^(-16)

// select speed vs accuracy tradeoff
// supported values for EXP_POLY_DEGREE are 4 and 5
#define EXP_POLY_DEGREE 4
// supported values for LOG_POLY_DEGREE are 5 and 6
#define LOG_POLY_DEGREE 5

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
#define M_PI_F 3.14159265358979323846f
#endif /* !M_PI_F */

#define DT_M_LN2f (0.6931471805599453f)

// clip channel value to be between 0 and 1
// NaN-safe: NaN compares false and will result in 0.0
// also does not force promotion of floats to doubles, but will use the type of its argument
#define CLIP(x) (((x) >= 0) ? ((x) <= 1 ? (x) : 1) : 0)

// clip luminance values to be between 0 and 100
#define LCLIP(x) ((x < 0) ? 0.0 : (x > 100.0) ? 100.0 : x)

// clamp value to lie between mn and mx
// Nan-safe: NaN compares false and will result in mn
#define CLAMPF(a, mn, mx) ((a) >= (mn) ? ((a) <= (mx) ? (a) : (mx)) : (mn))

//*****************
// functions to check for non-finite values
// with -ffinite-math-only, the compiler is free to elide checks based
// on isnan(), isinf(), and isfinite() because it can reason that these
// return constant values since it has been told explicitly that all
// numeric values are finite.  These versions override that directive
// to let use use NAN and INFINITY as flag values.

// Start by telling the compiler that non-finite values may occur.  As
// a side-effect, these functions will not be inlined even though we
// have declared them "inline" (to avoid unused-function warnings)
// when called from code with different optimization flags.
#ifdef __GNUC__
#pragma GCC push_options
#pragma GCC optimize ("-fno-finite-math-only")
#endif

static inline gboolean dt_isnan(const float val)
{
  return isnan(val);
}

static inline gboolean dt_isinf(const float val)
{
  return isinf(val);
}

static inline gboolean dt_isfinite(const float val)
{
  return isfinite(val);
}

static inline gboolean dt_isnormal(const float val)
{
  return isnormal(val);
}

#ifdef __GNUC__
#pragma GCC pop_options
#endif

// end of functions to check for non-finite values
//*****************

// test floats difference smaller than eps
static inline gboolean feqf(const float v1,
                            const float v2,
                            const float eps)
{
  return (fabsf(v1 - v2) < eps);
}

// We don't want to use the SIMD version sqf() in cases we might access unaligned memory
static inline float sqrf(const float a)
{
  return a * a;
}

// taken from rt code: calculate a * b + (1 - a) * c
static inline float interpolatef(const float a,
                                 const float b,
                                 const float c)
{
  return a * (b - c) + c;
}

// Kahan summation algorithm
DT_OMP_DECLARE_SIMD()
static inline float Kahan_sum(const float m,
                              float *const __restrict__ c,
                              const float add)
{
   const float t1 = add - (*c);
   const float t2 = m + t1;
   *c = (t2 - m) - t1;
   return t2;
}

static inline float scharr_gradient(const float *p, const int w)
{
  const float gx = 47.0f / 255.0f * (p[-w-1] - p[-w+1] + p[w-1]  - p[w+1])
                + 162.0f / 255.0f * (p[-1] - p[1]);
  const float gy = 47.0f / 255.0f * (p[-w-1] - p[w-1]  + p[-w+1] - p[w+1])
                + 162.0f / 255.0f * (p[-w] - p[w]);
  return sqrtf(sqrf(gx) + sqrf(gy));
}

static inline float Log2(const float x)
{
  return (x > 0.0f) ? (logf(x) / DT_M_LN2f) : x;
}

static inline float Log2Thres(const float x,
                              const float Thres)
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
static inline float fastlog(const float x)
{
  return DT_M_LN2f * fastlog2(x);
}

// multiply 3x3 matrix with 3x1 vector
// dest needs to be different from v
DT_OMP_DECLARE_SIMD()
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
DT_OMP_DECLARE_SIMD()
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
DT_OMP_DECLARE_SIMD()
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

DT_OMP_DECLARE_SIMD()
static inline void mul_mat_vec_2(const float *m,
                                 const float *p,
                                 float *o)
{
  o[0] = p[0] * m[0] + p[1] * m[1];
  o[1] = p[0] * m[2] + p[1] * m[3];
}

DT_OMP_DECLARE_SIMD(uniform(v_2) aligned(v_1, v_2:16))
static inline float scalar_product(const dt_aligned_pixel_t v_1,
                                   const dt_aligned_pixel_t v_2)
{
  // specialized 3×1 dot products 2 4×1 RGB-alpha pixels.  v_2 needs
  // to be uniform along loop increments, e.g. independent from
  // current pixel values we force an order of computation similar to
  // SSE4 _mm_dp_ps() hoping the compiler will get the clue
  float acc = 0.f;

  DT_OMP_SIMD(aligned(v_1, v_2:16) reduction(+:acc))
  for(size_t c = 0; c < 3; c++)
    acc += v_1[c] * v_2[c];

  return acc;
}


DT_OMP_DECLARE_SIMD(uniform(M) aligned(M:64) aligned(v_in, v_out:16))
static inline void dot_product(const dt_aligned_pixel_t v_in,
                               const dt_colormatrix_t M,
                               dt_aligned_pixel_t v_out)
{
  // specialized 3×4 dot products of 4×1 RGB-alpha pixels
  DT_OMP_SIMD(aligned(M:64) aligned(v_in, v_out:16))
  for(size_t i = 0; i < 3; ++i)
    v_out[i] = scalar_product(v_in, M[i]);
}


DT_OMP_DECLARE_SIMD()
static inline float sqf(const float x)
{
  return x * x;
}


DT_OMP_DECLARE_SIMD(aligned(p:16))
static inline float median9f(const float *p)
{
  float p1 = MIN(p[1], p[2]);
  float p2 = MAX(p[1], p[2]);
  float p4 = MIN(p[4], p[5]);
  float p5 = MAX(p[4], p[5]);
  float p7 = MIN(p[7], p[8]);
  float p8 = MAX(p[7], p[8]);
  float p0 = MIN(p[0], p1);
  float p1a = MAX(p[0], p1);
  float p3 = MIN(p[3], p4);
  float p4a = MAX(p[3], p4);
  float p6 = MIN(p[6], p7);
  float p7a = MAX(p[6], p7);
  p1 = MIN(p1a, p2);
  p2 = MAX(p1a, p2);
  p4 = MIN(p4a, p5);
  p5 = MAX(p4a, p5);
  p7 = MIN(p7a, p8);
  p8 = MAX(p7a, p8);
  p3 = MAX(p0,p3);
  p5 = MIN(p5, p8);
  p7a = MAX(p4, p7);
  p4 = MIN(p4, p7);
  p6 = MAX(p3, p6);
  p4 = MAX(p1, p4);
  p2 = MIN(p2, p5);
  p4a = MIN(p4, p7a);
  p4 = MIN(p4a, p2);
  p2 = MAX(p4a, p2);
  p4 = MAX(p6, p4);
  return MIN(p2,p4);
}

DT_OMP_DECLARE_SIMD(aligned(vector:16))
static inline float euclidean_norm(const dt_aligned_pixel_t vector)
{
  return fmaxf(sqrtf(sqf(vector[0]) + sqf(vector[1]) + sqf(vector[2])), NORM_MIN);
}


DT_OMP_DECLARE_SIMD(aligned(vector:16))
static inline void downscale_vector(dt_aligned_pixel_t vector,
                                    const float scaling)
{
  // check that scaling is positive (NaN produces FALSE)
  const int valid = (scaling > NORM_MIN);
  for(size_t c = 0; c < 3; c++)
    vector[c] = (valid) ? vector[c] / (scaling + NORM_MIN) : vector[c] / NORM_MIN;
}


DT_OMP_DECLARE_SIMD(aligned(vector:16))
static inline void upscale_vector(dt_aligned_pixel_t vector,
                                  const float scaling)
{
  // check that scaling is positive (NaN produces FALSE)
  const int valid = (scaling > NORM_MIN);
  for(size_t c = 0; c < 3; c++)
    vector[c] = (valid) ? vector[c] * (scaling + NORM_MIN) : vector[c] * NORM_MIN;
}


DT_OMP_DECLARE_SIMD()
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

// a faster, vectorizable version of hypotf() when we know that there
// won't be overflow, NaNs, or infinities
DT_OMP_DECLARE_SIMD()
static inline float dt_fast_hypotf(const float x,
                                   const float y)
{
  return sqrtf(x * x + y * y);
}

// fast approximation of expf()
/****** if you change this function, you need to make the same change
 * in data/kernels/{basecurve,basic}.cl ***/
DT_OMP_DECLARE_SIMD()
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
/****** if you change this function, you need to make the same change
 * in data/kernels/{denoiseprofile,nlmeans}.cl ***/
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

static inline void dt_vector_min(dt_aligned_pixel_t min,
                                 const dt_aligned_pixel_t v1,
                                 const dt_aligned_pixel_t v2)
{
#ifdef __SSE__
  *((__m128*)min) = _mm_min_ps(*((__m128*)v1), *((__m128*)v2));
#else
  for_four_channels(c)
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
  for_four_channels(c)
    max[c] = MAX(v1[c], v2[c]);
#endif
}

static inline void dt_vector_max_nan(dt_aligned_pixel_t max,
                                     const dt_aligned_pixel_t v1,
                                     const dt_aligned_pixel_t v2)
{
  for_four_channels(c)
    max[c] = fmaxf(v1[c], v2[c]);
}

static inline void dt_vector_round(const dt_aligned_pixel_t input,
                                   dt_aligned_pixel_t rounded)
{
  // unfortunately, casting to int truncates toward zero, so we need
  // to use an SSE intrinsic to make the compiler convert with
  // rounding instead
#ifdef __SSE2__
  *((__m128*)rounded) = _mm_cvtepi32_ps(_mm_cvtps_epi32(*((__m128*)input)));
#else
  for_four_channels(c)
    rounded[c] = roundf(input[c]);
#endif
}

// plain auto-vectorizing C implementation of _mm_log2_ps from sse.h
// See http://www.devmaster.net/forums/showthread.php?p=43580 for the original
static inline void dt_vector_log2(const dt_aligned_pixel_t x,
                                  dt_aligned_pixel_t res)
{
  // split input value into exponent and mantissa
  union { float f[4]; uint32_t i[4]; } mant, vx = { .f = { x[0], x[1], x[2], x[3] } };
  dt_aligned_pixel_t exp;
  for_four_channels(c)
  {
    mant.i[c] = (vx.i[c] & 0x007FFFFF) | 0x3F800000;
    exp[c] = (float)((vx.i[c] & 0x7F800000) >> 23) - 127;
  }
  // evaluate polynomial fit of log2(x)/(x-1).  Coefficients chosen
  // for minimax fit in [1, 2[ These coefficients can be generated
  // with
  // http://www.boost.org/doc/libs/1_36_0/libs/math/doc/sf_and_dist/html/math_toolkit/toolkit/internals2/minimax.html
  dt_aligned_pixel_t logmant;
  for_four_channels(c)
  {
#if LOG_POLY_DEGREE == 6
    logmant[c] = ((((((-0.0344359067839062357313f)
                      * mant.f[c] + 0.318212422185251071475f)
                     * mant.f[c] - 1.23152682416275988241f)
                    * mant.f[c] + 2.59883907202499966007f)
                   * mant.f[c] - 3.32419399085241980044f)
                  * mant.f[c] + 3.11578814719469302614f);
#elif LOG_POLY_DEGREE == 5
    logmant[c] = (((((0.0596515482674574969533f)
                     * mant.f[c] - 0.465725644288844778798f)
                    * mant.f[c] + 1.48116647521213171641f)
                   * mant.f[c] - 2.52074962577807006663f)
                  * mant.f[c] + 2.8882704548164776201f);
#else
#error Unsupported value for LOG_POLY_DEGREE
#endif
  }
  for_four_channels(c)
    res[c] = (logmant[c] * (mant.f[c] - 1.0f)) + exp[c];
}

static inline void dt_vector_exp(const dt_aligned_pixel_t x,
                                 dt_aligned_pixel_t result)
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

// plain auto-vectorizing C implementation of _mm_exp2_ps from sse.h
// See http://www.devmaster.net/forums/showthread.php?p=43580 for the original
static inline void dt_vector_exp2(const dt_aligned_pixel_t input,
                                  dt_aligned_pixel_t res)
{
  // clamp the exponent to the supported range
  static const dt_aligned_pixel_t lower_bound =
    { -126.99999f, -126.99999f, -126.99999f, -126.99999f };
  static const dt_aligned_pixel_t upper_bound =
    {  129.00000f,  129.00000f,  129.00000f,  129.00000f };
  static const dt_aligned_pixel_t v_half = { 0.5f, 0.5f, 0.5f, 0.5f };
  dt_aligned_pixel_t x;
  dt_vector_min(x, input, upper_bound);
  dt_vector_max(x, x, lower_bound);

  // split the input value into fraction and exponent
  dt_aligned_pixel_t x_adj;
  for_four_channels(c)
    x_adj[c] = x[c] - v_half[c];
  dt_aligned_pixel_t ipart;
  dt_vector_round(x_adj, ipart);
  dt_aligned_pixel_t fpart;
  for_four_channels(c)
    fpart[c] = x[c] - ipart[c];

  // compute the multiplier 2^ipart by directly building the
  // corresponding float value
  union { uint32_t i[4]; float f[4]; } expipart;
  for_four_channels(c)
    expipart.i[c] = (127 + (int)ipart[c]) << 23;

  // evaluate the nth-degree polynomial (coefficients chosen for
  // minimax fit on [-0.5, 0.5[ )
  dt_aligned_pixel_t expfpart;
  for_four_channels(c)
  {
#if EXP_POLY_DEGREE == 5
    expfpart[c] = (((((1.8775767e-3f * fpart[c] + 8.9893397e-3f)
                      * fpart[c] + 5.5826318e-2f)
                     * fpart[c] + 2.4015361e-1f)
                    * fpart[c] + 6.9315308e-1f)
                   * fpart[c] + 9.99999994e-1f);
#elif EXP_POLY_DEGREE == 4
    expfpart[c] = ((((1.3534167e-2f * fpart[c] + 5.2011464e-2f)
                     * fpart[c] + 2.4144275e-1f)
                    * fpart[c] + 6.9300383e-1f)
                   * fpart[c] + 1.0000026f);
#else
#error Unsupported value for EXP_POLY_DEGREE
#endif
  }
  // scale the result using the integer portion of the original input
  for_four_channels(c, aligned(res))
    res[c] = expipart.f[c] * expfpart[c];
}

static inline void dt_vector_exp10(const dt_aligned_pixel_t x,
                                   dt_aligned_pixel_t res)
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
  // x**p == 2^( log2(x) * p)
  dt_aligned_pixel_t log;
  dt_vector_log2(input, log);
  for_four_channels(c)
    log[c] *= power[c];
  dt_vector_exp2(log, output);
}

static inline void dt_vector_pow1(const dt_aligned_pixel_t input,
                                  const float power,
                                  dt_aligned_pixel_t output)
{
  const dt_aligned_pixel_t vpower = { power, power, power, power };
  dt_vector_powf(input, vpower, output);
}

static inline void dt_vector_add(dt_aligned_pixel_t sum,
                                 const dt_aligned_pixel_t v1,
                                 const dt_aligned_pixel_t v2)
{
  for_four_channels(c, aligned(sum,v1,v2))
    sum[c] = v1[c] + v2[c];
}

static inline void dt_vector_sub(dt_aligned_pixel_t diff,
                                 const dt_aligned_pixel_t v1,
                                 const dt_aligned_pixel_t v2)
{
  for_four_channels(c, aligned(diff,v1,v2))
    diff[c] = v1[c] - v2[c];
}

static inline void dt_vector_mul(dt_aligned_pixel_t result,
                                 const dt_aligned_pixel_t v1,
                                 const dt_aligned_pixel_t v2)
{
  for_four_channels(c, aligned(result,v1,v2))
    result[c] = v1[c] * v2[c];
}

static inline void dt_vector_mul1(dt_aligned_pixel_t result,
                                  const dt_aligned_pixel_t in,
                                  const float scale)
{
  for_four_channels(c, aligned(result,in))
    result[c] = in[c] * scale;
}

static inline void dt_vector_div(dt_aligned_pixel_t result,
                                 const dt_aligned_pixel_t v1,
                                 const dt_aligned_pixel_t v2)
{
  for_four_channels(c, aligned(result,v1,v2))
    result[c] = v1[c] / v2[c];
}

static inline void dt_vector_div1(dt_aligned_pixel_t result,
                                  const dt_aligned_pixel_t in,
                                  const float divisor)
{
  for_four_channels(c, aligned(result,in))
    result[c] = in[c] / divisor;
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
  static const dt_aligned_pixel_t zero = { 0.0f, 0.0f, 0.0f, 0.0f };
  static const dt_aligned_pixel_t one = { 1.0f, 1.0f, 1.0f, 1.0f };
  dt_vector_max(values, values, zero);
  dt_vector_min(values, values, one);
}

static inline void dt_vector_clipneg(dt_aligned_pixel_t values)
{
  static const dt_aligned_pixel_t zero = { 0.0f, 0.0f, 0.0f, 0.0f };
  dt_vector_max(values, values, zero);
}

static inline void dt_vector_clipneg_nan(dt_aligned_pixel_t values)
{
  static const dt_aligned_pixel_t zero = { 0.0f, 0.0f, 0.0f, 0.0f };
  dt_vector_max_nan(values, values, zero);
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
static inline void dt_vector_sin(const dt_aligned_pixel_t arg,
                                 dt_aligned_pixel_t sine)
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
