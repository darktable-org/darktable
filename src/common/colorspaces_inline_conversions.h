/*
 *    This file is part of darktable,
 *    Copyright (C) 2017-2023 darktable developers.
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

#include "common/math.h"

#ifdef __SSE2__
#include "common/sse.h" // also loads darkable.h
#include <xmmintrin.h>
#else
#include "common/darktable.h"
#endif


#ifdef __SSE2__
static inline __m128 lab_f_inv_m(const __m128 x)
{
  const __m128 epsilon = _mm_set1_ps(0.20689655172413796f); // cbrtf(216.0f/24389.0f);
  const __m128 kappa_rcp_x16 = _mm_set1_ps(16.0f * 27.0f / 24389.0f);
  const __m128 kappa_rcp_x116 = _mm_set1_ps(116.0f * 27.0f / 24389.0f);

  // x > epsilon
  const __m128 res_big = x * x * x;
  // x <= epsilon
  const __m128 res_small = kappa_rcp_x116 * x - kappa_rcp_x16;

  // blend results according to whether each component is > epsilon or not
  const __m128 mask = _mm_cmpgt_ps(x, epsilon);
  return _mm_or_ps(_mm_and_ps(mask, res_big), _mm_andnot_ps(mask, res_small));
}

/** uses D50 white point. */
static inline __m128 dt_Lab_to_XYZ_sse2(const __m128 Lab)
{
  const __m128 d50 = _mm_set_ps(0.0f, 0.8249f, 1.0f, 0.9642f);
  const __m128 coef = _mm_set_ps(0.0f, -1.0f / 200.0f, 1.0f / 116.0f, 1.0f / 500.0f);
  const __m128 offset = _mm_set1_ps(0.137931034f);

  // last component ins shuffle taken from 1st component of Lab to make sure it is not nan, so it will become
  // 0.0f in f
  const __m128 f = _mm_shuffle_ps(Lab, Lab, _MM_SHUFFLE(0, 2, 0, 1)) * coef;

  return d50 * lab_f_inv_m(f + _mm_shuffle_ps(f, f, _MM_SHUFFLE(1, 1, 3, 1)) + offset);
}

static inline __m128 lab_f_m_sse2(const __m128 x)
{
  const __m128 epsilon = _mm_set1_ps(216.0f / 24389.0f);
  const __m128 kappa = _mm_set1_ps(24389.0f / 27.0f);

  // calculate as if x > epsilon : result = cbrtf(x)
  // approximate cbrtf(x):
  const __m128 a = _mm_castsi128_ps(
      _mm_add_epi32(_mm_cvtps_epi32(_mm_div_ps(_mm_cvtepi32_ps(_mm_castps_si128(x)), _mm_set1_ps(3.0f))),
                    _mm_set1_epi32(709921077)));
  const __m128 a3 = a * a * a;
  const __m128 res_big = a * (a3 + x + x) / (a3 + a3 + x);

  // calculate as if x <= epsilon : result = (kappa*x+16)/116
  const __m128 res_small = (kappa * x + _mm_set1_ps(16.0f)) / _mm_set1_ps(116.0f);

  // blend results according to whether each component is > epsilon or not
  const __m128 mask = _mm_cmpgt_ps(x, epsilon);
  return _mm_or_ps(_mm_and_ps(mask, res_big), _mm_andnot_ps(mask, res_small));
}

/** uses D50 white point. */
static inline __m128 dt_XYZ_to_Lab_sse2(const __m128 XYZ)
{
  const __m128 d50_inv = _mm_set_ps(1.0f, 0.8249f, 1.0f, 0.9642f);
  const __m128 coef = _mm_set_ps(0.0f, 200.0f, 500.0f, 116.0f);
  const __m128 f = lab_f_m_sse2(XYZ / d50_inv);
  // because d50_inv.z is 0.0f, lab_f(0) == 16/116, so Lab[0] = 116*f[0] - 16 equal to 116*(f[0]-f[3])
  return coef * (_mm_shuffle_ps(f, f, _MM_SHUFFLE(3, 1, 0, 1)) - _mm_shuffle_ps(f, f, _MM_SHUFFLE(3, 2, 1, 3)));
}

/** uses D50 white point. */
// see http://www.brucelindbloom.com/Eqn_RGB_XYZ_Matrix.html for the transformation matrices
static inline __m128 dt_XYZ_to_sRGB_sse2(__m128 XYZ)
{
  // XYZ -> sRGB matrix, D65
  const __m128 xyz_to_srgb_0 = _mm_setr_ps(3.1338561f, -0.9787684f, 0.0719453f, 0.0f);
  const __m128 xyz_to_srgb_1 = _mm_setr_ps(-1.6168667f, 1.9161415f, -0.2289914f, 0.0f);
  const __m128 xyz_to_srgb_2 = _mm_setr_ps(-0.4906146f, 0.0334540f, 1.4052427f, 0.0f);

  __m128 rgb
      = xyz_to_srgb_0 * _mm_shuffle_ps(XYZ, XYZ, _MM_SHUFFLE(0, 0, 0, 0)) +
        xyz_to_srgb_1 * _mm_shuffle_ps(XYZ, XYZ, _MM_SHUFFLE(1, 1, 1, 1)) +
        xyz_to_srgb_2 * _mm_shuffle_ps(XYZ, XYZ, _MM_SHUFFLE(2, 2, 2, 2));

  // linear sRGB -> gamma corrected sRGB
  __m128 mask = _mm_cmple_ps(rgb, _mm_set1_ps(0.0031308));
  __m128 rgb0 = _mm_set1_ps(12.92) * rgb;
  __m128 rgb1 = _mm_set1_ps(1.0 + 0.055) * _mm_pow_ps1(rgb, 1.0 / 2.4) - _mm_set1_ps(0.055);
  return _mm_or_ps(_mm_and_ps(mask, rgb0), _mm_andnot_ps(mask, rgb1));
}

static inline __m128 dt_sRGB_to_XYZ_sse2(__m128 rgb)
{
  // sRGB -> XYZ matrix, D65
  const __m128 srgb_to_xyz_0 = _mm_setr_ps(0.4360747f, 0.2225045f, 0.0139322f, 0.0f);
  const __m128 srgb_to_xyz_1 = _mm_setr_ps(0.3850649f, 0.7168786f, 0.0971045f, 0.0f);
  const __m128 srgb_to_xyz_2 = _mm_setr_ps(0.1430804f, 0.0606169f, 0.7141733f, 0.0f);

  // gamma corrected sRGB -> linear sRGB
  __m128 mask = _mm_cmple_ps(rgb, _mm_set1_ps(0.04045));
  __m128 rgb0 = _mm_div_ps(rgb, _mm_set1_ps(12.92));
  __m128 rgb1 = _mm_pow_ps1(_mm_div_ps(_mm_add_ps(rgb, _mm_set1_ps(0.055)), _mm_set1_ps(1 + 0.055)), 2.4);
  rgb = _mm_or_ps(_mm_and_ps(mask, rgb0), _mm_andnot_ps(mask, rgb1));

  __m128 XYZ
      = srgb_to_xyz_0 * _mm_shuffle_ps(rgb, rgb, _MM_SHUFFLE(0, 0, 0, 0)) +
        srgb_to_xyz_1 * _mm_shuffle_ps(rgb, rgb, _MM_SHUFFLE(1, 1, 1, 1)) +
        srgb_to_xyz_2 * _mm_shuffle_ps(rgb, rgb, _MM_SHUFFLE(2, 2, 2, 2));
  return XYZ;
}

/** uses D50 white point. */
// see http://www.brucelindbloom.com/Eqn_RGB_XYZ_Matrix.html for the transformation matrices
static inline __m128 dt_XYZ_to_prophotoRGB_sse2(__m128 XYZ)
{
  // XYZ -> prophotoRGB matrix, D50
  const __m128 xyz_to_rgb_0 = _mm_setr_ps( 1.3459433f, -0.5445989f, 0.0000000f, 0.0f);
  const __m128 xyz_to_rgb_1 = _mm_setr_ps(-0.2556075f,  1.5081673f, 0.0000000f, 0.0f);
  const __m128 xyz_to_rgb_2 = _mm_setr_ps(-0.0511118f,  0.0205351f,  1.2118128f, 0.0f);

  __m128 rgb
      = xyz_to_rgb_0 * _mm_shuffle_ps(XYZ, XYZ, _MM_SHUFFLE(0, 0, 0, 0)) +
        xyz_to_rgb_1 * _mm_shuffle_ps(XYZ, XYZ, _MM_SHUFFLE(1, 1, 1, 1)) +
        xyz_to_rgb_2 * _mm_shuffle_ps(XYZ, XYZ, _MM_SHUFFLE(2, 2, 2, 2));
  return rgb;
}

/** uses D50 white point. */
// see http://www.brucelindbloom.com/Eqn_RGB_XYZ_Matrix.html for the transformation matrices
static inline __m128 dt_prophotoRGB_to_XYZ_sse2(__m128 rgb)
{
  // prophotoRGB -> XYZ matrix, D50
  const __m128 rgb_to_xyz_0 = _mm_setr_ps(0.7976749f, 0.2880402f, 0.0000000f, 0.0f);
  const __m128 rgb_to_xyz_1 = _mm_setr_ps(0.1351917f, 0.7118741f, 0.0000000f, 0.0f);
  const __m128 rgb_to_xyz_2 = _mm_setr_ps(0.0313534f, 0.0000857f, 0.8252100f, 0.0f);

  __m128 XYZ
      = rgb_to_xyz_0 * _mm_shuffle_ps(rgb, rgb, _MM_SHUFFLE(0, 0, 0, 0)) +
        rgb_to_xyz_1 * _mm_shuffle_ps(rgb, rgb, _MM_SHUFFLE(1, 1, 1, 1)) +
        rgb_to_xyz_2 * _mm_shuffle_ps(rgb, rgb, _MM_SHUFFLE(2, 2, 2, 2));
  return XYZ;
}
#endif

#ifdef _OPENMP
#pragma omp declare simd aligned(in,out)
#endif
static inline void dt_apply_transposed_color_matrix(const dt_aligned_pixel_t in, const dt_colormatrix_t matrix,
                                                    dt_aligned_pixel_t out)
{
  // using dt_aligned_pixel_t instead of float* for the function parameters gives GCC enough info to vectorize
  // and eliminate intermediate memory writes without going through major contortions
  for_each_channel(r)
    out[r] = matrix[0][r] * in[0] + matrix[1][r] * in[1] + matrix[2][r] * in[2];
}

#ifdef _OPENMP
#pragma omp declare simd simdlen(4)
#endif
static inline float cbrt_5f(float f)
{
  uint32_t * const p = (uint32_t *)&f;
  *p = *p / 3 + 709921077;
  return f;
}

#ifdef _OPENMP
#pragma omp declare simd simdlen(4)
#endif
static inline float cbrta_halleyf(const float a, const float R)
{
  const float a3 = a * a * a;
  const float b = a * (a3 + R + R) / (a3 + a3 + R);
  return b;
}

#ifdef _OPENMP
#pragma omp declare simd simdlen(4)
#endif
static inline float lab_f(const float x)
{
  const float epsilon = 216.0f / 24389.0f;
  const float kappa = 24389.0f / 27.0f;
  return (x > epsilon) ? cbrta_halleyf(cbrt_5f(x), x) : (kappa * x + 16.0f) / 116.0f;
}

/** uses D50 white point. */
static const dt_aligned_pixel_t d50 = { 0.9642f, 1.0f, 0.8249f };
static const dt_aligned_pixel_t d50_inv = { 1.0f/0.9642f, 1.0f, 1.0f/0.8249f };

#ifdef _OPENMP
#pragma omp declare simd aligned(Lab, XYZ:16) uniform(Lab, XYZ)
#endif
static inline void dt_XYZ_to_Lab(const dt_aligned_pixel_t XYZ, dt_aligned_pixel_t Lab)
{
  dt_aligned_pixel_t f;
  for_each_channel(i)
    f[i] = lab_f(XYZ[i] * d50_inv[i]);
//  Lab[0] = 116.0f * f[1] - 16.0f;
//  Lab[1] = 500.0f * (f[0] - f[1]);
//  Lab[2] = -200.0f * (f[2] - f[1]);
  static const dt_aligned_pixel_t coeff = { 116.0f, 500.0f, -200.0f, 0.0f };
  static const dt_aligned_pixel_t offset = { 16.0f, 0.0f, 0.0f, 0.0f };
  static const dt_aligned_pixel_t zero = { 0.0f, 0.0f, 0.0f, 0.0f };
  // compiler uses permute instruction if we copy ALL elements
  // unused fourth element will be zeroed by the multiplication
  const dt_aligned_pixel_t tmp1 = { f[1], f[0], f[2], f[3] };
#if defined(__GNUC__) && !defined(__clang__)
  // convince the compiler to use shuffle/permute instructions instead
  // of building the result in RAM
  typedef float v4sf __attribute__((vector_size(16)));
  typedef int v4si __attribute__((vector_size(16)));
  const v4si mask = { 5, 1, 1, 5 }; // zero[0], f[1], f[1], zero[0]
  dt_aligned_pixel_t tmp2;
  *((v4sf*)tmp2) =  __builtin_shuffle(*((v4sf*)f), *((v4sf*)zero), mask);
#else
  const dt_aligned_pixel_t tmp2 = { zero[0], f[1], f[1], zero[0] };
#endif
  for_each_channel(c)
    Lab[c] = (coeff[c] * (tmp1[c] - tmp2[c])) - offset[c];
}

#ifdef _OPENMP
#pragma omp declare simd simdlen(4)
#endif
static inline float lab_f_inv(const float x)
{
  const float epsilon = 0.20689655172413796f; // cbrtf(216.0f/24389.0f);
  const float kappa = 24389.0f / 27.0f;
  return (x > epsilon) ? x * x * x : (116.0f * x - 16.0f) / kappa;
}

/** uses D50 white point. */
#ifdef _OPENMP
#pragma omp declare simd aligned(Lab, XYZ:16) uniform(Lab, XYZ)
#endif
static inline void dt_Lab_to_XYZ(const dt_aligned_pixel_t Lab, dt_aligned_pixel_t XYZ)
{
  dt_aligned_pixel_t f = { Lab[1], Lab[0], Lab[2], Lab[3] };
  static const dt_aligned_pixel_t offset = { 0.0f, 16.0f, 0.0f, 0.0f };
  static const dt_aligned_pixel_t coeff = { 1.0f/500.0f, 1.0f/116.0f, -1.0f/200.0f, 0.0f };
  static const dt_aligned_pixel_t add_coeff = { 1.0f, 0.0f, 1.0f, 0.0f };
  dt_aligned_pixel_t scaled;
  for_each_channel(c,aligned(Lab,coeff,f))
    scaled[c] = (f[c] + offset[c]) * coeff[c];
  dt_aligned_pixel_t inv;
  for_each_channel(c)
    inv[c] = lab_f_inv(scaled[c] + scaled[1] * add_coeff[c]);
  for_each_channel(c,aligned(d50,f,add_coeff,XYZ))
    XYZ[c] = d50[c] * inv[c];
}

/** uses D50 white point. */
#ifdef _OPENMP
#pragma omp declare simd aligned(Lab, RGB:16) uniform(Lab, RGB)
#endif
static inline void dt_Lab_to_linearRGB(
	const dt_aligned_pixel_t Lab,
        const dt_aligned_pixel_t cmatrix_row0,
        const dt_aligned_pixel_t cmatrix_row1,
        const dt_aligned_pixel_t cmatrix_row2,
        dt_aligned_pixel_t RGB)
{
  // we need to duplicate the code from dt_Lab_to_XYZ and
  // dt_apply_transposed_color_matrix here in order to make the
  // compiler optimize away memory accesses for intermediates
  static const dt_aligned_pixel_t coeff = { 1.0f/500.0f, 1.0f/116.0f, -1.0f/200.0f, 1.0f };
  static const dt_aligned_pixel_t add = { 0.0f, 16.0f, 0.0f, 0.0f };
  static const dt_aligned_pixel_t add_coeff = { 1.0f, 0.0f, 1.0f, 0.0f };
  const dt_aligned_pixel_t f = { Lab[1], Lab[0], Lab[2], Lab[3] };
  dt_aligned_pixel_t scaled;
  for_each_channel(c,aligned(Lab,coeff,f))
    scaled[c] = (f[c] + add[c]) * coeff[c];
  dt_aligned_pixel_t inv;
  for_each_channel(c)
    inv[c] = lab_f_inv(scaled[c] + scaled[1] * add_coeff[c]);
  dt_aligned_pixel_t XYZ;
  for_each_channel(c,aligned(d50,f,add_coeff,XYZ))
    XYZ[c] = d50[c] * inv[c];
  // the following loop is dt_apply_transposed_color_matrix(XYZ, cmatrix, RGB);
  for_each_channel(r)
    RGB[r] = cmatrix_row0[r] * XYZ[0] + cmatrix_row1[r] * XYZ[1] + cmatrix_row2[r] * XYZ[2];
}

#ifdef _OPENMP
#pragma omp declare simd aligned(xyY, XYZ:16)
#endif
static inline void dt_XYZ_to_xyY(const dt_aligned_pixel_t XYZ, dt_aligned_pixel_t xyY)
{
  const float sum = XYZ[0] + XYZ[1] + XYZ[2];
  xyY[0] = XYZ[0] / sum;
  xyY[1] = XYZ[1] / sum;
  xyY[2] = XYZ[1];
}


#ifdef _OPENMP
#pragma omp declare simd aligned(xyY, XYZ:16)
#endif
static inline void dt_xyY_to_XYZ(const dt_aligned_pixel_t xyY, dt_aligned_pixel_t XYZ)
{
  XYZ[0] = xyY[2] * xyY[0] / xyY[1];
  XYZ[1] = xyY[2];
  XYZ[2] = xyY[2] * (1.f - xyY[0] - xyY[1]) / xyY[1];
}


#ifdef _OPENMP
#pragma omp declare simd aligned(xyY, uvY:16)
#endif
static inline void dt_xyY_to_uvY(const dt_aligned_pixel_t xyY, dt_aligned_pixel_t uvY)
{
  // This is the linear part of the chromaticity transform from CIE L*u*v* e.g. u'v'.
  // See https://en.wikipedia.org/wiki/CIELUV
  // It rescales the chromaticity diagram xyY in a more perceptual way,
  // but it is still not hue-linear and not perfectly perceptual.
  // As such, it is the only radiometricly-accurate representation of hue non-linearity in human vision system.
  // Use it for "hue preserving" (as much as possible) gamut mapping in scene-referred space
  const float denominator = -2.f * xyY[0] + 12.f * xyY[1] + 3.f;
  uvY[0] = 4.f * xyY[0] / denominator; // u'
  uvY[1] = 9.f * xyY[1] / denominator; // v'
  uvY[2] = xyY[2];                     // Y
}

#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline float cbf(const float x)
{
  return x * x * x;
}


#ifdef _OPENMP
#pragma omp declare simd aligned(xyY, Luv:16)
#endif
static inline void dt_xyY_to_Luv(const dt_aligned_pixel_t xyY, dt_aligned_pixel_t Luv)
{
  // This is the second, non-linear, part of the the 1976 CIE L*u*v* transform.
  // See https://en.wikipedia.org/wiki/CIELUV
  // It is intended to provide perceptual hue-linear-ish controls and settings for more intuitive GUI.
  // Don't ever use it for pixel-processing, it sucks, it's old, it kills kittens and makes your mother cry.
  // Seriously, don't.
  // You need to convert Luv parameters to XYZ or RGB and properly process pixels in RGB or XYZ or related spaces.
  dt_aligned_pixel_t uvY;
  dt_xyY_to_uvY(xyY, uvY);

  // We assume Yn == 1 == peak luminance
  const float threshold = cbf(6.0f / 29.0f);
  Luv[0] = (uvY[2] <= threshold) ? cbf(29.0f / 3.0f) * uvY[2] : 116.0f * cbrtf(uvY[2]) - 16.f;

  const float D50[2] DT_ALIGNED_PIXEL = { 0.20915914598542354f, 0.488075320769787f };
  Luv[1] = 13.f * Luv[0] * (uvY[0] - D50[0]); // u*
  Luv[2] = 13.f * Luv[0] * (uvY[1] - D50[1]); // v*

  // Output is in [0; 100] for all channels
}


static inline void dt_Luv_to_Lch(const dt_aligned_pixel_t Luv, dt_aligned_pixel_t Lch)
{
  Lch[0] = Luv[0];                 // L stays L
  Lch[1] = hypotf(Luv[2], Luv[1]); // chroma radius
  Lch[2] = atan2f(Luv[2], Luv[1]); // hue angle
  Lch[2] = (Lch[2] < 0.f) ? 2.f * M_PI + Lch[2] : Lch[2]; // ensure angle is positive modulo 2 pi
}


static inline void dt_xyY_to_Lch(const dt_aligned_pixel_t xyY, dt_aligned_pixel_t Lch)
{
  dt_aligned_pixel_t Luv;
  dt_xyY_to_Luv(xyY, Luv);
  dt_Luv_to_Lch(Luv, Lch);
}


#ifdef _OPENMP
#pragma omp declare simd aligned(uvY, xyY:16)
#endif
static inline void dt_uvY_to_xyY(const dt_aligned_pixel_t uvY, dt_aligned_pixel_t xyY)
{
  // This is the linear part of chromaticity transform from CIE L*u*v* e.g. u'v'.
  // See https://en.wikipedia.org/wiki/CIELUV
  // It rescales the chromaticity diagram xyY in a more perceptual way,
  // but it is still not hue-linear and not perfectly perceptual.
  // As such, it is the only radiometricly-accurate representation of hue non-linearity in human vision system.
  // Use it for "hue preserving" (as much as possible) gamut mapping in scene-referred space
  const float denominator = 6.0f * uvY[0] - 16.f * uvY[1] + 12.0f;
  xyY[0] = 9.f * uvY[0] / denominator; // x
  xyY[1] = 4.f * uvY[1] / denominator; // y
  xyY[2] = uvY[2];                     // Y
}


#ifdef _OPENMP
#pragma omp declare simd aligned(xyY, Luv:16)
#endif
static inline void dt_Luv_to_xyY(const dt_aligned_pixel_t Luv, dt_aligned_pixel_t xyY)
{
  // This is the second, non-linear, part of the the 1976 CIE L*u*v* transform.
  // See https://en.wikipedia.org/wiki/CIELUV
  // It is intended to provide perceptual hue-linear-ish controls and settings for more intuitive GUI.
  // Don't ever use it for pixel-processing, it sucks, it's old, it kills kittens and makes your mother cry.
  // Seriously, don't.
  // You need to convert Luv parameters to XYZ or RGB and properly process pixels in RGB or XYZ or related spaces.
  dt_aligned_pixel_t uvY;

  // We assume Yn == 1 == peak luminance
  static const float threshold = 8.0f;
  uvY[2] = (Luv[0] <= threshold) ? Luv[0] * cbf(3.f / 29.f) : cbf((Luv[0] + 16.f) / 116.f);

  static const float D50[2] DT_ALIGNED_PIXEL = { 0.20915914598542354f, 0.488075320769787f };
  uvY[0] = Luv[1] / (Luv[0] * 13.f) + D50[0];  // u' = u* / 13 L + u_n
  uvY[1] = Luv[2] / (Luv[0] * 13.f) + D50[1];  // v' = v* / 13 L + v_n

  dt_uvY_to_xyY(uvY, xyY);
  // Output is normalized for all channels
}

static inline void dt_Lch_to_Luv(const dt_aligned_pixel_t Lch, dt_aligned_pixel_t Luv)
{
  Luv[0] = Lch[0];                // L stays L
  Luv[1] = Lch[1] * cosf(Lch[2]); // radius * cos(angle)
  Luv[2] = Lch[1] * sinf(Lch[2]); // radius * sin(angle)
}

static inline void dt_Lch_to_xyY(const dt_aligned_pixel_t Lch, dt_aligned_pixel_t xyY)
{
  dt_aligned_pixel_t Luv;
  dt_Lch_to_Luv(Lch, Luv);
  dt_Luv_to_xyY(Luv, xyY);
}


/** Uses D65 **/
#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline void dt_XYZ_to_Rec709_D65(const dt_aligned_pixel_t XYZ, dt_aligned_pixel_t sRGB)
{
  // linear sRGB == Rec709 with no gamma
  // transpose and pad the conversion matrix to enable vectorization
  static const dt_colormatrix_t xyz_to_srgb_transposed = {
    {  3.2404542f, -0.9692660f,  0.0556434f, 0.0f },
    { -1.5371385f,  1.8760108f, -0.2040259f, 0.0f },
    { -0.4985314f,  0.0415560f,  1.0572252f, 0.0f },
  };
  // XYZ -> linear sRGB
  dt_apply_transposed_color_matrix(XYZ, xyz_to_srgb_transposed, sRGB);
}

#ifdef _OPENMP
#pragma omp declare simd aligned(sRGB, RGB)
#endif
static inline void dt_sRGB_to_linear_sRGB(const dt_aligned_pixel_t sRGB, dt_aligned_pixel_t RGB)
{
  // gamma corrected sRGB -> linear sRGB
  for(int c = 0; c < 3; c++)
    RGB[c] = sRGB[c] <= 0.04045f ? sRGB[c] / 12.92f : powf((sRGB[c] + 0.055f) / (1.0f + 0.055f), 2.4f);
}

#ifdef _OPENMP
#pragma omp declare simd aligned(XYZ,rgb)
#endif
static inline void dt_XYZ_to_prophotorgb(const dt_aligned_pixel_t XYZ, dt_aligned_pixel_t rgb)
{
  // transpose and pad the conversion matrix to enable vectorization
  static const dt_colormatrix_t xyz_to_prophotorgb_transpose = {
    {  1.3459433f, -0.5445989f, 0.0000000f, 0.0f },
    { -0.2556075f,  1.5081673f, 0.0000000f, 0.0f },
    { -0.0511118f,  0.0205351f, 1.2118128f, 0.0f }
  };
  dt_apply_transposed_color_matrix(XYZ,xyz_to_prophotorgb_transpose,rgb);
}

// transpose and pad the conversion matrix to enable vectorization
static const dt_colormatrix_t prophotorgb_to_xyz_transpose = {
  // prophoto rgb
  { 0.7976749f, 0.2880402f, 0.0000000f, 0.0f },
  { 0.1351917f, 0.7118741f, 0.0000000f, 0.0f },
  { 0.0313534f, 0.0000857f, 0.8252100f, 0.0f }
};

#ifdef _OPENMP
#pragma omp declare simd aligned(rgb, XYZ)
#endif
static inline void dt_prophotorgb_to_XYZ(const dt_aligned_pixel_t rgb, dt_aligned_pixel_t XYZ)
{
  dt_apply_transposed_color_matrix(rgb,prophotorgb_to_xyz_transpose,XYZ);
}

static inline float dt_prophotorgb_to_XYZ_luma(const dt_aligned_pixel_t rgb)
{
  return (prophotorgb_to_xyz_transpose[0][1] * rgb[0]
          + prophotorgb_to_xyz_transpose[1][1] * rgb[1]
          + prophotorgb_to_xyz_transpose[2][1] * rgb[2]);
}

// Conversion matrix from http://www.brucelindbloom.com/Eqn_RGB_XYZ_Matrix.html
// (transpose and pad the conversion matrix to enable vectorization)
static const dt_colormatrix_t sRGB_to_xyz_transposed =
  { { 0.4360747f, 0.2225045f, 0.0139322f },
    { 0.3850649f, 0.7168786f, 0.0971045f },
    { 0.1430804f, 0.0606169f, 0.7141733f } };

static const dt_colormatrix_t xyz_to_srgb_transposed =
  { {  3.1338561f, -0.9787684f,  0.0719453f },
    { -1.6168667f,  1.9161415f, -0.2289914f },
    { -0.4906146f,  0.0334540f,  1.4052427f } };

#ifdef __SSE2__
/** uses D50 white point. */
// see http://www.brucelindbloom.com/Eqn_RGB_XYZ_Matrix.html for the transformation matrices
static inline __m128 dt_XYZ_to_RGB_sse2(__m128 XYZ)
{
  // XYZ -> sRGB matrix, D65
  const __m128 xyz_to_srgb_0 = _mm_load_ps(xyz_to_srgb_transposed[0]);
  const __m128 xyz_to_srgb_1 = _mm_load_ps(xyz_to_srgb_transposed[1]);
  const __m128 xyz_to_srgb_2 = _mm_load_ps(xyz_to_srgb_transposed[2]);

  __m128 rgb = ((xyz_to_srgb_0 * _mm_shuffle_ps(XYZ, XYZ, _MM_SHUFFLE(0, 0, 0, 0))) +
                (xyz_to_srgb_1 * _mm_shuffle_ps(XYZ, XYZ, _MM_SHUFFLE(1, 1, 1, 1))) +
                (xyz_to_srgb_2 * _mm_shuffle_ps(XYZ, XYZ, _MM_SHUFFLE(2, 2, 2, 2))));
  return rgb;
}

static inline __m128 dt_RGB_to_XYZ_sse2(__m128 rgb)
{
  // sRGB -> XYZ matrix, D65
  const __m128 srgb_to_xyz_0 = _mm_load_ps(sRGB_to_xyz_transposed[0]);
  const __m128 srgb_to_xyz_1 = _mm_load_ps(sRGB_to_xyz_transposed[1]);
  const __m128 srgb_to_xyz_2 = _mm_load_ps(sRGB_to_xyz_transposed[2]);

  __m128 XYZ
    = ((srgb_to_xyz_0 * _mm_shuffle_ps(rgb, rgb, _MM_SHUFFLE(0, 0, 0, 0)))
       + (srgb_to_xyz_1 * _mm_shuffle_ps(rgb, rgb, _MM_SHUFFLE(1, 1, 1, 1)))
       + (srgb_to_xyz_2 * _mm_shuffle_ps(rgb, rgb, _MM_SHUFFLE(2, 2, 2, 2))));
  return XYZ;
}
#endif

static inline void dt_linearRGB_to_XYZ(const dt_aligned_pixel_t linearRGB, dt_aligned_pixel_t XYZ)
{
  dt_apply_transposed_color_matrix(linearRGB, sRGB_to_xyz_transposed, XYZ);
}

#ifdef _OPENMP
#pragma omp declare simd aligned(sRGB, XYZ_D50: 16)
#endif
static inline void dt_Rec709_to_XYZ_D50(const dt_aligned_pixel_t sRGB, dt_aligned_pixel_t XYZ_D50)
{
  dt_apply_transposed_color_matrix(sRGB, sRGB_to_xyz_transposed, XYZ_D50);
}

#ifdef _OPENMP
#pragma omp declare simd aligned(sRGB, XYZ)
#endif
static inline void dt_sRGB_to_XYZ(const dt_aligned_pixel_t sRGB, dt_aligned_pixel_t XYZ)
{
  dt_aligned_pixel_t rgb = { 0 };
  dt_sRGB_to_linear_sRGB(sRGB, rgb);
  // linear sRGB -> XYZ
  dt_Rec709_to_XYZ_D50(rgb, XYZ);
}

static inline void dt_XYZ_to_linearRGB(const dt_aligned_pixel_t XYZ, dt_aligned_pixel_t linearRGB)
{
  dt_apply_transposed_color_matrix(XYZ, xyz_to_srgb_transposed, linearRGB);
}

/** Uses D50 **/
#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline void dt_XYZ_to_Rec709_D50(const dt_aligned_pixel_t XYZ, dt_aligned_pixel_t sRGB)
{
  // XYZ -> linear sRGB
  dt_apply_transposed_color_matrix(XYZ, xyz_to_srgb_transposed, sRGB);
}

/** uses D50 white point. */
#ifdef _OPENMP
#pragma omp declare simd aligned(XYZ, sRGB)
#endif
static inline void dt_XYZ_to_sRGB(const dt_aligned_pixel_t XYZ, dt_aligned_pixel_t sRGB)
{
  // XYZ -> linear sRGB
  dt_aligned_pixel_t rgb;
  dt_XYZ_to_Rec709_D50(XYZ, rgb);
  // linear sRGB -> gamma corrected sRGB
  for(size_t c = 0; c < 3; c++)
    sRGB[c] = rgb[c] <= 0.0031308f ? 12.92f * rgb[c] : (1.0f + 0.055f) * powf(rgb[c], 1.0f / 2.4f) - 0.055f;
}

/** uses D50 white point and clips the output to [0..1]. */
#ifdef _OPENMP
#pragma omp declare simd aligned(XYZ, sRGB)
#endif
static inline void dt_XYZ_to_sRGB_clipped(const dt_aligned_pixel_t XYZ, dt_aligned_pixel_t sRGB)
{
  dt_aligned_pixel_t result;
  dt_XYZ_to_sRGB(XYZ, result);

  for_each_channel(c)
    sRGB[c] = CLIP(result[c]);
}

#ifdef _OPENMP
#pragma omp declare simd aligned(Lab, rgb)
#endif
static inline void dt_Lab_to_prophotorgb(const dt_aligned_pixel_t Lab, dt_aligned_pixel_t rgb)
{
  dt_aligned_pixel_t XYZ = { 0.0f };
  dt_Lab_to_XYZ(Lab, XYZ);
  dt_XYZ_to_prophotorgb(XYZ, rgb);
}

#ifdef _OPENMP
#pragma omp declare simd aligned(rgb, Lab)
#endif
static inline void dt_prophotorgb_to_Lab(const dt_aligned_pixel_t rgb, dt_aligned_pixel_t Lab)
{
  dt_aligned_pixel_t XYZ = { 0.0f };
  dt_prophotorgb_to_XYZ(rgb, XYZ);
  dt_XYZ_to_Lab(XYZ, Lab);
}


#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline float _dt_RGB_2_Hue(const dt_aligned_pixel_t RGB, const float max, const float delta)
{
  float hue;
  if(RGB[0] == max)
    hue = (RGB[1] - RGB[2]) / delta;
  else if(RGB[1] == max)
    hue = 2.0f + (RGB[2] - RGB[0]) / delta;
  else
    hue = 4.0f + (RGB[0] - RGB[1]) / delta;

  hue /= 6.0f;
  if(hue < 0.0f) hue += 1.0f;
  if(hue > 1.0f) hue -= 1.0f;
  return hue;
}

#ifdef _OPENMP
#pragma omp declare simd aligned(RGB: 16)
#endif
static inline void _dt_Hue_2_RGB(dt_aligned_pixel_t RGB, const float H, const float C, const float min)
{
  const float h = H * 6.0f;
  const float i = floorf(h);
  const float f = h - i;
  const float fc = f * C;
  const float top = C + min;
  const float inc = fc + min;
  const float dec = top - fc;
  const size_t i_idx = (size_t)i;
  if(i_idx == 0)
  {
    RGB[0] = top;
    RGB[1] = inc;
    RGB[2] = min;
  }
  else if(i_idx == 1)
  {
    RGB[0] = dec;
    RGB[1] = top;
    RGB[2] = min;
  }
  else if(i_idx == 2)
  {
    RGB[0] = min;
    RGB[1] = top;
    RGB[2] = inc;
  }
  else if(i_idx == 3)
  {
    RGB[0] = min;
    RGB[1] = dec;
    RGB[2] = top;
  }
  else if(i_idx == 4)
  {
    RGB[0] = inc;
    RGB[1] = min;
    RGB[2] = top;
  }
  else
  {
    RGB[0] = top;
    RGB[1] = min;
    RGB[2] = dec;
  }
}


#ifdef _OPENMP
#pragma omp declare simd aligned(RGB, HSL: 16)
#endif
static inline void dt_RGB_2_HSL(const dt_aligned_pixel_t RGB, dt_aligned_pixel_t HSL)
{
  const float min = fminf(RGB[0], fminf(RGB[1], RGB[2]));
  const float max = fmaxf(RGB[0], fmaxf(RGB[1], RGB[2]));
  const float delta = max - min;

  const float L = (max + min) / 2.0f;
  float H, S;

  if(fabsf(max) > 1e-6f && fabsf(delta) > 1e-6f)
  {
    if(L < 0.5f)
      S = delta / (max + min);
    else
      S = delta / (2.0f - max - min);
    H = _dt_RGB_2_Hue(RGB, max, delta);
  }
  else
  {
    H = 0.0f;
    S = 0.0f;
  }

  HSL[0] = H;
  HSL[1] = S;
  HSL[2] = L;
}

#ifdef _OPENMP
#pragma omp declare simd aligned(HSL, RGB: 16)
#endif
static inline void dt_HSL_2_RGB(const dt_aligned_pixel_t HSL, dt_aligned_pixel_t RGB)
{
  // almost straight from https://en.wikipedia.org/wiki/HSL_and_HSV
  const float L = HSL[2];
  float C;
  if(L < 0.5f)
    C = L * HSL[1];
  else
    C = (1.0f - L) * HSL[1];
  const float m = L - C;
  _dt_Hue_2_RGB(RGB, HSL[0], 2.0f * C, m);
}


#ifdef _OPENMP
#pragma omp declare simd aligned(RGB, HSV: 16)
#endif
static inline void dt_RGB_2_HSV(const dt_aligned_pixel_t RGB, dt_aligned_pixel_t HSV)
{
  const float min = fminf(RGB[0], fminf(RGB[1], RGB[2]));
  const float max = fmaxf(RGB[0], fmaxf(RGB[1], RGB[2]));
  const float delta = max - min;

  const float V = max;
  float S, H;

  if(fabsf(max) > 1e-6f && fabsf(delta) > 1e-6f)
  {
    S = delta / max;
    H = _dt_RGB_2_Hue(RGB, max, delta);
  }
  else
  {
    S = 0.0f;
    H = 0.0f;
  }

  HSV[0] = H;
  HSV[1] = S;
  HSV[2] = V;
}

#ifdef _OPENMP
#pragma omp declare simd aligned(HSV, RGB: 16)
#endif
static inline void dt_HSV_2_RGB(const dt_aligned_pixel_t HSV, dt_aligned_pixel_t RGB)
{
  // almost straight from https://en.wikipedia.org/wiki/HSL_and_HSV
  const float C = HSV[1] * HSV[2];
  const float m = HSV[2] - C;
  _dt_Hue_2_RGB(RGB, HSV[0], C, m);
}


#ifdef _OPENMP
#pragma omp declare simd aligned(RGB, HCV: 16)
#endif
static inline void dt_RGB_2_HCV(const dt_aligned_pixel_t RGB, dt_aligned_pixel_t HCV)
{
  const float min = fminf(RGB[0], fminf(RGB[1], RGB[2]));
  const float max = fmaxf(RGB[0], fmaxf(RGB[1], RGB[2]));
  const float delta = max - min;

  const float V = max;
  float C, H;

  if(fabsf(max) > 1e-6f && fabsf(delta) > 1e-6f)
  {
    C = delta;
    H = _dt_RGB_2_Hue(RGB, max, delta);
  }
  else
  {
    C = 0.0f;
    H = 0.0f;
  }

  HCV[0] = H;
  HCV[1] = C;
  HCV[2] = V;
}

#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline void dt_Lab_2_LCH(const dt_aligned_pixel_t Lab, dt_aligned_pixel_t LCH)
{
  float var_H = atan2f(Lab[2], Lab[1]);

  if(var_H > 0.0f)
    var_H = var_H / (2.0f * DT_M_PI_F);
  else
    var_H = 1.0f - fabsf(var_H) / (2.0f * DT_M_PI_F);

  LCH[0] = Lab[0];
  LCH[1] = hypotf(Lab[1], Lab[2]);
  LCH[2] = var_H;
}


#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline void dt_LCH_2_Lab(const dt_aligned_pixel_t LCH, dt_aligned_pixel_t Lab)
{
  Lab[0] = LCH[0];
  Lab[1] = cosf(2.0f * DT_M_PI_F * LCH[2]) * LCH[1];
  Lab[2] = sinf(2.0f * DT_M_PI_F * LCH[2]) * LCH[1];
}

static inline float dt_camera_rgb_luminance(const dt_aligned_pixel_t rgb)
{
  return (rgb[0] * 0.2225045f + rgb[1] * 0.7168786f + rgb[2] * 0.0606169f);
}


#ifdef _OPENMP
#pragma omp declare simd aligned(XYZ_D50, XYZ_D65: 16)
#endif
static inline void dt_XYZ_D50_2_XYZ_D65(const dt_aligned_pixel_t XYZ_D50, dt_aligned_pixel_t XYZ_D65)
{
  // Bradford adaptation matrix from http://www.brucelindbloom.com/index.html?Eqn_ChromAdapt.html
  static const dt_colormatrix_t M_transposed = {
      {  0.9555766f, -0.0282895f,  0.0122982f, 0.0f },
      { -0.0230393f,  1.0099416f, -0.0204830f, 0.0f },
      {  0.0631636f,  0.0210077f,  1.3299098f, 0.0f },
  };

  for_each_channel(x)
    XYZ_D65[x] = M_transposed[0][x] * XYZ_D50[0] + M_transposed[1][x] * XYZ_D50[1] + M_transposed[2][x] * XYZ_D50[2];
}

#ifdef _OPENMP
#pragma omp declare simd aligned(XYZ_D50, XYZ_D65: 16)
#endif
static inline void dt_XYZ_D65_2_XYZ_D50(const dt_aligned_pixel_t XYZ_D65, dt_aligned_pixel_t XYZ_D50)
{
  // Bradford adaptation matrix from http://www.brucelindbloom.com/index.html?Eqn_ChromAdapt.html
  static const dt_colormatrix_t M_transposed = {
      {  1.0478112f,  0.0295424f, -0.0092345f, 0.0f },
      {  0.0228866f,  0.9904844f,  0.0150436f, 0.0f },
      { -0.0501270f, -0.0170491f,  0.7521316f, 0.0f },
  };

  for_each_channel(x)
    XYZ_D50[x] = M_transposed[0][x] * XYZ_D65[0] + M_transposed[1][x] * XYZ_D65[1] + M_transposed[2][x] * XYZ_D65[2];
}


/**
 * Conversion algorithms between XYZ and JzAzBz and JzCzhz are described in the following paper:
 *
 *  Perceptually uniform color space for image signals including high dynamic range and wide gamut
 *  https://www.osapublishing.org/oe/fulltext.cfm?uri=oe-25-13-15131&id=368272
 */

#ifdef _OPENMP
#pragma omp declare simd aligned(XYZ_D65, JzAzBz: 16)
#endif
static inline void dt_XYZ_2_JzAzBz(const dt_aligned_pixel_t XYZ_D65, dt_aligned_pixel_t JzAzBz)
{
  const float b = 1.15f;
  const float g = 0.66f;
  const float c1 = 0.8359375f; // 3424 / 2^12
  const float c2 = 18.8515625f; // 2413 / 2^7
  const float c3 = 18.6875f; // 2392 / 2^7
  const float n = 0.159301758f; // 2610 / 2^14
  const float p = 134.034375f; // 1.7 x 2523 / 2^5
  const float d = -0.56f;
  const float d0 = 1.6295499532821566e-11f;
  static const dt_colormatrix_t M = {
      { 0.41478972f, 0.579999f, 0.0146480f, 0.0f },
      { -0.2015100f, 1.120649f, 0.0531008f, 0.0f },
      { -0.0166008f, 0.264800f, 0.6684799f, 0.0f },
  };
  static const dt_colormatrix_t A_transposed = {
      { 0.5f,       3.524000f,  0.199076f, 0.0f },
      { 0.5f,      -4.066708f,  1.096799f, 0.0f },
      { 0.0f,       0.542708f, -1.295875f, 0.0f },
  };

  dt_aligned_pixel_t XYZ = { 0.0f, 0.0f, 0.0f, 0.0f };
  dt_aligned_pixel_t LMS = { 0.0f, 0.0f, 0.0f, 0.0f };

  // XYZ -> X'Y'Z
  XYZ[0] = b * XYZ_D65[0] - (b - 1.0f) * XYZ_D65[2];
  XYZ[1] = g * XYZ_D65[1] - (g - 1.0f) * XYZ_D65[0];
  XYZ[2] = XYZ_D65[2];

  // X'Y'Z -> L'M'S'
#ifdef _OPENMP
#pragma omp simd aligned(LMS, XYZ:16) aligned(M:64)
#endif
  for(int i = 0; i < 3; i++)
  {
    LMS[i] = M[i][0] * XYZ[0] + M[i][1] * XYZ[1] + M[i][2] * XYZ[2];
    LMS[i] = powf(fmaxf(LMS[i] / 10000.f, 0.0f), n);
    LMS[i] = powf((c1 + c2 * LMS[i]) / (1.0f + c3 * LMS[i]), p);
  }

  // L'M'S' -> Izazbz
  for_each_channel(c)
    JzAzBz[c] = A_transposed[0][c] * LMS[0] + A_transposed[1][c] * LMS[1] + A_transposed[2][c] * LMS[2];
  // Iz -> Jz
  JzAzBz[0] = fmaxf(((1.0f + d) * JzAzBz[0]) / (1.0f + d * JzAzBz[0]) - d0, 0.f);
}

#ifdef _OPENMP
#pragma omp declare simd aligned(JzAzBz, JzCzhz: 16)
#endif
static inline void dt_JzAzBz_2_JzCzhz(const dt_aligned_pixel_t JzAzBz, dt_aligned_pixel_t JzCzhz)
{
  float var_H = atan2f(JzAzBz[2], JzAzBz[1]) / (2.0f * DT_M_PI_F);
  JzCzhz[0] = JzAzBz[0];
  JzCzhz[1] = hypotf(JzAzBz[1], JzAzBz[2]);
  JzCzhz[2] = var_H >= 0.0f ? var_H : 1.0f + var_H;
}

#ifdef _OPENMP
#pragma omp declare simd aligned(JzCzhz, JzAzBz: 16)
#endif
static inline void dt_JzCzhz_2_JzAzBz(const dt_aligned_pixel_t JzCzhz, dt_aligned_pixel_t JzAzBz)
{
  JzAzBz[0] = JzCzhz[0];
  JzAzBz[1] = cosf(2.0f * DT_M_PI_F * JzCzhz[2]) * JzCzhz[1];
  JzAzBz[2] = sinf(2.0f * DT_M_PI_F * JzCzhz[2]) * JzCzhz[1];
}

#ifdef _OPENMP
#pragma omp declare simd aligned(JzAzBz, XYZ_D65: 16)
#endif
static inline void dt_JzAzBz_2_XYZ(const dt_aligned_pixel_t JzAzBz, dt_aligned_pixel_t XYZ_D65)
{
  const float b = 1.15f;
  const float g = 0.66f;
  const float c1 = 0.8359375f; // 3424 / 2^12
  const float c2 = 18.8515625f; // 2413 / 2^7
  const float c3 = 18.6875f; // 2392 / 2^7
  const float n_inv = 1.0f / 0.159301758f; // 2610 / 2^14
  const float p_inv = 1.0f / 134.034375f; // 1.7 x 2523 / 2^5
  const float d = -0.56f;
  const float d0 = 1.6295499532821566e-11f;
  const dt_colormatrix_t MI = {
      {  1.9242264357876067f, -1.0047923125953657f,  0.0376514040306180f, 0.0f },
      {  0.3503167620949991f,  0.7264811939316552f, -0.0653844229480850f, 0.0f },
      { -0.0909828109828475f, -0.3127282905230739f,  1.5227665613052603f, 0.0f },
  };
  const dt_colormatrix_t AI = {
      {  1.0f,  0.1386050432715393f,  0.0580473161561189f, 0.0f },
      {  1.0f, -0.1386050432715393f, -0.0580473161561189f, 0.0f },
      {  1.0f, -0.0960192420263190f, -0.8118918960560390f, 0.0f },
  };

  dt_aligned_pixel_t XYZ = { 0.0f, 0.0f, 0.0f, 0.0f };
  dt_aligned_pixel_t LMS = { 0.0f, 0.0f, 0.0f, 0.0f };
  dt_aligned_pixel_t IzAzBz = { 0.0f, 0.0f, 0.0f, 0.0f };

  IzAzBz[0] = JzAzBz[0] + d0;
  IzAzBz[0] = fmaxf(IzAzBz[0] / (1.0f + d - d * IzAzBz[0]), 0.f);
  IzAzBz[1] = JzAzBz[1];
  IzAzBz[2] = JzAzBz[2];

  // IzAzBz -> LMS
#ifdef _OPENMP
#pragma omp simd aligned(LMS, IzAzBz:16) aligned(AI:64)
#endif
  for(int i = 0; i < 3; i++)
  {
    LMS[i] = AI[i][0] * IzAzBz[0] + AI[i][1] * IzAzBz[1] + AI[i][2] * IzAzBz[2];
    LMS[i] = powf(fmaxf(LMS[i], 0.0f), p_inv);
    LMS[i] = 10000.f * powf(fmaxf((c1 - LMS[i]) / (c3 * LMS[i] - c2), 0.0f), n_inv);
  }

  // LMS -> X'Y'Z
#ifdef _OPENMP
#pragma omp simd aligned(LMS, XYZ:16) aligned(MI:64)
#endif
  for(int i = 0; i < 3; i++) XYZ[i] = MI[i][0] * LMS[0] + MI[i][1] * LMS[1] + MI[i][2] * LMS[2];

  // X'Y'Z -> XYZ_D65
  XYZ_D65[0] = (XYZ[0] + (b - 1.0f) * XYZ[2]) / b;
  XYZ_D65[1] = (XYZ[1] + (g - 1.0f) * XYZ_D65[0]) / g;
  XYZ_D65[2] = XYZ[2];
}

// Convert CIE 1931 2° XYZ D65 to CIE 2006 LMS D65 (cone space)
/*
* The CIE 1931 XYZ 2° observer D65 is converted to CIE 2006 LMS D65 using the approximation by
* Richard A. Kirk, Chromaticity coordinates for graphic arts based on CIE 2006 LMS
* with even spacing of Munsell colours
* https://doi.org/10.2352/issn.2169-2629.2019.27.38
*/

static const dt_colormatrix_t XYZ_D65_to_LMS_2006_D65
    = { { 0.257085f, 0.859943f, -0.031061f, 0.f },
        { -0.394427f, 1.175800f, 0.106423f, 0.f },
        { 0.064856f, -0.076250f, 0.559067f, 0.f } };

static const dt_colormatrix_t LMS_2006_D65_to_XYZ_D65
    = { { 1.80794659f, -1.29971660f, 0.34785879f, 0.f },
        { 0.61783960f, 0.39595453f, -0.04104687f, 0.f },
        { -0.12546960f, 0.20478038f, 1.74274183f, 0.f } };


#ifdef _OPENMP
#pragma omp declare simd aligned(LMS, XYZ: 16)
#endif
static inline void XYZ_to_LMS(const dt_aligned_pixel_t XYZ, dt_aligned_pixel_t LMS)
{
  dot_product(XYZ, XYZ_D65_to_LMS_2006_D65, LMS);
}

#ifdef _OPENMP
#pragma omp declare simd aligned(XYZ, LMS: 16)
#endif
static inline void LMS_to_XYZ(const dt_aligned_pixel_t LMS, dt_aligned_pixel_t XYZ)
{
  dot_product(LMS, LMS_2006_D65_to_XYZ_D65, XYZ);
}

/*
* Convert from CIE 2006 LMS D65 to Filmlight RGB defined in
* Richard A. Kirk, Chromaticity coordinates for graphic arts based on CIE 2006 LMS
* with even spacing of Munsell colours
* https://doi.org/10.2352/issn.2169-2629.2019.27.38
*/

static const dt_colormatrix_t filmlightRGB_D65_to_LMS_D65
    = { { 0.95f, 0.38f, 0.00f, 0.f },
        { 0.05f, 0.62f, 0.03f, 0.f },
        { 0.00f, 0.00f, 0.97f, 0.f } };

static const dt_colormatrix_t LMS_D65_to_filmlightRGB_D65
    = { {  1.0877193f, -0.66666667f,  0.02061856f, 0.f },
        { -0.0877193f,  1.66666667f, -0.05154639f, 0.f },
        {         0.f,          0.f,  1.03092784f, 0.f } };

#ifdef _OPENMP
#pragma omp declare simd aligned(LMS, RGB: 16)
#endif
static inline void gradingRGB_to_LMS(const dt_aligned_pixel_t RGB, dt_aligned_pixel_t LMS)
{
  dot_product(RGB, filmlightRGB_D65_to_LMS_D65, LMS);
}

#ifdef _OPENMP
#pragma omp declare simd aligned(LMS, RGB: 16)
#endif
static inline void LMS_to_gradingRGB(const dt_aligned_pixel_t LMS, dt_aligned_pixel_t RGB)
{
  dot_product(LMS, LMS_D65_to_filmlightRGB_D65, RGB);
}


/*
* Re-express the Filmlight RGB triplet as Yrg luminance/chromacity coordinates
*/

#ifdef _OPENMP
#pragma omp declare simd aligned(LMS, Yrg: 16)
#endif
static inline void LMS_to_Yrg(const dt_aligned_pixel_t LMS, dt_aligned_pixel_t Yrg)
{
  // compute luminance
  const float Y = 0.68990272f * LMS[0] + 0.34832189f * LMS[1];

  // normalize LMS
  const float a = LMS[0] + LMS[1] + LMS[2];
  dt_aligned_pixel_t lms = { 0.f };
  for_four_channels(c, aligned(LMS, lms : 16)) lms[c] = (a == 0.f) ? 0.f : LMS[c] / a;

  // convert to Filmlight rgb (normalized)
  dt_aligned_pixel_t rgb = { 0.f };
  LMS_to_gradingRGB(lms, rgb);

  Yrg[0] = Y;
  Yrg[1] = rgb[0];
  Yrg[2] = rgb[1];
}

#ifdef _OPENMP
#pragma omp declare simd aligned(Yrg, LMS: 16)
#endif
static inline void Yrg_to_LMS(const dt_aligned_pixel_t Yrg, dt_aligned_pixel_t LMS)
{
  const float Y = Yrg[0];

  // reform rgb (normalized) from chroma
  const float r = Yrg[1];
  const float g = Yrg[2];
  const float b = 1.f - r - g;
  const dt_aligned_pixel_t rgb = { r, g, b, 0.f };

  // convert to lms (normalized)
  dt_aligned_pixel_t lms = { 0.f };
  gradingRGB_to_LMS(rgb, lms);

  // denormalize to LMS
  const float denom = (0.68990272f * lms[0] + 0.34832189f * lms[1]);
  const float a = (denom == 0.f) ? 0.f : Y / denom;
  for_four_channels(c, aligned(lms, LMS:16)) LMS[c] = lms[c] * a;
}

/*
 * Re-express Filmlight Yrg in polar coordinates Ych.
 *
 * Note that we don't explicitly store the hue angle
 * but rather just the cosine and sine of the angle.
 * This is because we don't need the hue angle anywhere
 * and this way we can avoid calculating expensive
 * trigonometric functions.
 */

#ifdef _OPENMP
#pragma omp declare simd aligned(Ych, Yrg: 16)
#endif
static inline void Yrg_to_Ych(const dt_aligned_pixel_t Yrg, dt_aligned_pixel_t Ych)
{
  const float Y = Yrg[0];
  // Subtract white point. These are the r, g coordinates of
  // sRGB (D50 adapted) (1, 1, 1) taken through
  // XYZ D50 -> CAT16 D50->D65 adaptation -> LMS 2006
  // -> grading RGB conversion.
  const float r = Yrg[1] - 0.21902143f;
  const float g = Yrg[2] - 0.54371398f;
  const float c = dt_fast_hypotf(g, r);
  const float cos_h = c != 0.f ? r / c : 1.f;
  const float sin_h = c != 0.f ? g / c : 0.f;
  Ych[0] = Y;
  Ych[1] = c;
  Ych[2] = cos_h;
  Ych[3] = sin_h;
}

#ifdef _OPENMP
#pragma omp declare simd aligned(Ych, Yrg: 16)
#endif
static inline void Ych_to_Yrg(const dt_aligned_pixel_t Ych, dt_aligned_pixel_t Yrg)
{
  const float Y = Ych[0];
  const float c = Ych[1];
  const float cos_h = Ych[2];
  const float sin_h = Ych[3];
  const float r = c * cos_h + 0.21902143f;
  const float g = c * sin_h + 0.54371398f;
  Yrg[0] = Y;
  Yrg[1] = r;
  Yrg[2] = g;
}

#ifdef _OPENMP
#pragma omp declare simd aligned(Ych: 16)
#endif
static inline void make_Ych(const float Y, const float c, const float h, dt_aligned_pixel_t Ych)
{
  Ych[0] = Y;
  Ych[1] = c;
  Ych[2] = cosf(h);
  Ych[3] = sinf(h);
}

#ifdef _OPENMP
#pragma omp declare simd aligned(Ych: 16)
#endif
static inline float get_hue_angle_from_Ych(const dt_aligned_pixel_t Ych)
{
  return atan2f(Ych[3], Ych[2]);
}

/*
* Filmlight RGB utils functions
*/

#ifdef _OPENMP
#pragma omp declare simd aligned(Ych, RGB: 16)
#endif
static inline void Ych_to_gradingRGB(const dt_aligned_pixel_t Ych, dt_aligned_pixel_t RGB)
{
  dt_aligned_pixel_t Yrg = { 0.f };
  dt_aligned_pixel_t LMS = { 0.f };
  Ych_to_Yrg(Ych, Yrg);
  Yrg_to_LMS(Yrg, LMS);
  LMS_to_gradingRGB(LMS, RGB);
}


#ifdef _OPENMP
#pragma omp declare simd aligned(Ych, RGB: 16)
#endif
static inline void gradingRGB_to_Ych(const dt_aligned_pixel_t RGB, dt_aligned_pixel_t Ych)
{
  dt_aligned_pixel_t Yrg = { 0.f };
  dt_aligned_pixel_t LMS = { 0.f };
  gradingRGB_to_LMS(RGB, LMS);
  LMS_to_Yrg(LMS, Yrg);
  Yrg_to_Ych(Yrg, Ych);
}


#ifdef _OPENMP
#pragma omp declare simd aligned(Ych, XYZ: 16)
#endif
static inline void XYZ_to_Ych(const dt_aligned_pixel_t XYZ, dt_aligned_pixel_t Ych)
{
  // WARNING: XYZ needs to be chroma-adapted to D65 before
  dt_aligned_pixel_t Yrg = { 0.f };
  dt_aligned_pixel_t LMS = { 0.f };
  XYZ_to_LMS(XYZ, LMS);
  LMS_to_Yrg(LMS, Yrg);
  Yrg_to_Ych(Yrg, Ych);
}


#ifdef _OPENMP
#pragma omp declare simd aligned(Ych, XYZ: 16)
#endif
static inline void Ych_to_XYZ(const dt_aligned_pixel_t Ych, dt_aligned_pixel_t XYZ)
{
  // WARNING: XYZ is output in D65
  dt_aligned_pixel_t Yrg = { 0.f };
  dt_aligned_pixel_t LMS = { 0.f };
  Ych_to_Yrg(Ych, Yrg);
  Yrg_to_LMS(Yrg, LMS);
  LMS_to_XYZ(LMS, XYZ);
}


static inline void gamut_check_Yrg(dt_aligned_pixel_t Ych)
{
  // Check if the color fits in Yrg and LMS cone space
  // clip chroma at constant hue and luminance otherwise

  // Do a test conversion to Yrg
  dt_aligned_pixel_t Yrg = { 0.f };
  Ych_to_Yrg(Ych, Yrg);

  // Gamut-clip chroma in Yrg at constant hue and luminance
  // e.g. find the max chroma value that fits in gamut at the current hue
  // taken from colorbalancergb.c
  const float D65_r = 0.21902143f;
  const float D65_g = 0.54371398f;

  float max_c = Ych[1];
  const float cos_h = Ych[2];
  const float sin_h = Ych[3];

  if(Yrg[1] < 0.f)
  {
    max_c = fminf(-D65_r / cos_h, max_c);
  }
  if(Yrg[2] < 0.f)
  {
    max_c = fminf(-D65_g / sin_h, max_c);
  }
  if(Yrg[1] + Yrg[2] > 1.f)
  {
    max_c = fminf((1.f - D65_r - D65_g) / (cos_h + sin_h), max_c);
  }

  // Overwrite chroma with the sanitized value
  Ych[1] = max_c;
}

/** The following is darktable Uniform Color Space 2022
 * © Aurélien Pierre
 * https://eng.aurelienpierre.com/2022/02/color-saturation-control-for-the-21th-century/
 *
 * Use this space for color-grading in a perceptual framework.
 * The CAM terms have been removed for performance.
 **/

static inline float Y_to_dt_UCS_L_star(const float Y)
{
  // WARNING: L_star needs to be < 2.098883786377, meaning Y needs to be < 3.875766378407574e+19
  const float Y_hat = powf(Y, 0.631651345306265f);
  return 2.098883786377f * Y_hat / (Y_hat + 1.12426773749357f);
}

static inline float dt_UCS_L_star_to_Y(const float L_star)
{
  // WARNING: L_star needs to be < 2.098883786377, meaning Y needs to be < 3.875766378407574e+19
  return powf((1.12426773749357f * L_star / (2.098883786377f - L_star)), 1.5831518565279648f);
}


// L_star upper limit is 2.098883786377 truncated to 32-bit float and last decimal removed.
// By clipping L_star to this limit, we ensure dt_UCS_L_star_to_Y() doesn't divide by zero.
static const float DT_UCS_L_STAR_UPPER_LIMIT = 2.098883f;
// Y upper limit is calculated from the above L star upper limit.
static const float DT_UCS_Y_UPPER_LIMIT = 13237757000.f;


#ifdef _OPENMP
#pragma omp declare simd aligned(xyY: 16)
#endif
static inline void xyY_to_dt_UCS_UV(const dt_aligned_pixel_t xyY, float UV_star_prime[2])
{

  const dt_aligned_pixel_t x_factors = { -0.783941002840055f,  0.745273540913283f, 0.318707282433486f, 0.f };
  const dt_aligned_pixel_t y_factors = {  0.277512987809202f, -0.205375866083878f, 2.16743692732158f,  0.f };
  const dt_aligned_pixel_t offsets   = {  0.153836578598858f, -0.165478376301988f, 0.291320554395942f, 0.f };

  dt_aligned_pixel_t UVD = { 0.f };
  for_each_channel(c, aligned(xyY, UVD, x_factors, y_factors, offsets))
    UVD[c] = x_factors[c] * xyY[0] + y_factors[c] * xyY[1] + offsets[c];

  UVD[0] /= UVD[2];
  UVD[1] /= UVD[2];

  float UV_star[2] = { 0.f };
  const float factors[2]     = { 1.39656225667f, 1.4513954287f };
  const float half_values[2] = { 1.49217352929f, 1.52488637914f };
  for(int c = 0; c < 2; c++)
    UV_star[c] = factors[c] * UVD[c] / (fabsf(UVD[c]) + half_values[c]);

  // The following is equivalent to a 2D matrix product
  UV_star_prime[0] = -1.124983854323892f * UV_star[0] - 0.980483721769325f * UV_star[1];
  UV_star_prime[1] =  1.86323315098672f  * UV_star[0] + 1.971853092390862f * UV_star[1];

}


#ifdef _OPENMP
#pragma omp declare simd aligned(xyY, JCH: 16)
#endif
static inline void xyY_to_dt_UCS_JCH(const dt_aligned_pixel_t xyY, const float L_white, dt_aligned_pixel_t JCH)
{
  /*
    input :
      * xyY in normalized CIE XYZ for the 2° 1931 observer adapted for D65
      * L_white the lightness of white as dt UCS L* lightness
      * cz = 1 for standard pre-print proofing conditions with average surround and n = 20 %
              (background = middle grey, white = perfect diffuse white)
    range : xy in [0; 1], Y normalized for perfect diffuse white = 1
  */

  float UV_star_prime[2];
  xyY_to_dt_UCS_UV(xyY, UV_star_prime);

  // L_star must be clipped to the valid range of dt UCS
  const float L_star = Y_to_dt_UCS_L_star(CLAMPF(xyY[2], 0.f, DT_UCS_Y_UPPER_LIMIT));
  const float M2 = UV_star_prime[0] * UV_star_prime[0] + UV_star_prime[1] * UV_star_prime[1]; // square of colorfulness M

  // should be JCH[0] = powf(L_star / L_white), cz) but we treat only the case where cz = 1
  JCH[0] = L_star / L_white;
  JCH[1] = 15.932993652962535f * powf(L_star, 0.6523997524738018f) * powf(M2, 0.6007557017508491f) / L_white;
  JCH[2] = atan2f(UV_star_prime[1], UV_star_prime[0]);
}


#ifdef _OPENMP
#pragma omp declare simd aligned(xyY, JCH: 16)
#endif
static inline void dt_UCS_JCH_to_xyY(const dt_aligned_pixel_t JCH, const float L_white, dt_aligned_pixel_t xyY)
{
  /*
    input :
      * xyY in normalized CIE XYZ for the 2° 1931 observer adapted for D65
      * L_white the lightness of white as dt UCS L* lightness
      * cz = 1 for standard pre-print proofing conditions with average surround and n = 20 %
              (background = middle grey, white = perfect diffuse white)
    range : xy in [0; 1], Y normalized for perfect diffuse white = 1
  */

  // should be L_star = powf(JCH[0], 1.f / cz) * L_white but we treat only the case where cz = 1
  // L_star must be clipped to the valid range of dt UCS
  const float L_star = CLAMPF(JCH[0] * L_white, 0.f, DT_UCS_L_STAR_UPPER_LIMIT);
  const float M = L_star != 0.f
    ? powf(JCH[1] * L_white / (15.932993652962535f * powf(L_star, 0.6523997524738018f)), 0.8322850678616855f)
    : 0.f;

  const float U_star_prime = M * cosf(JCH[2]);
  const float V_star_prime = M * sinf(JCH[2]);

  // The following is equivalent to a 2D matrix product
  const float UV_star[2] = { -5.037522385190711f * U_star_prime - 2.504856328185843f * V_star_prime,
                              4.760029407436461f * U_star_prime + 2.874012963239247f * V_star_prime };

  float UV[2] = { 0.f };
  const float factors[2]     = { 1.39656225667f, 1.4513954287f };
  const float half_values[2] = { 1.49217352929f, 1.52488637914f };
  for(int c = 0; c < 2; c++)
    UV[c] = -half_values[c] * UV_star[c] / (fabsf(UV_star[c]) - factors[c]);

  const dt_aligned_pixel_t U_factors = {  0.167171472114775f,   -0.150959086409163f,    0.940254742367256f,  0.f };
  const dt_aligned_pixel_t V_factors = {  0.141299802443708f,   -0.155185060382272f,    1.000000000000000f,  0.f };
  const dt_aligned_pixel_t offsets   = { -0.00801531300850582f, -0.00843312433578007f, -0.0256325967652889f, 0.f };

  dt_aligned_pixel_t xyD = { 0.f };
  for_each_channel(c, aligned(xyD, UV, U_factors, V_factors, offsets))
    xyD[c] = U_factors[c] * UV[0] + V_factors[c] * UV[1] + offsets[c];

  xyY[0] = xyD[0] / xyD[2];
  xyY[1] = xyD[1] / xyD[2];
  xyY[2] = dt_UCS_L_star_to_Y(L_star);
}


static inline void dt_UCS_JCH_to_HSB(const dt_aligned_pixel_t JCH, dt_aligned_pixel_t HSB)
{
  HSB[2] = JCH[0] * (powf(JCH[1], 1.33654221029386f) + 1.f);
  HSB[1] = (HSB[2] > 0.f) ? JCH[1] / HSB[2] : 0.f;
  HSB[0] = JCH[2];
}


static inline void dt_UCS_HSB_to_JCH(const dt_aligned_pixel_t HSB, dt_aligned_pixel_t JCH)
{
  JCH[2] = HSB[0];
  JCH[1] = HSB[1] * HSB[2];
  JCH[0] = HSB[2] / (powf(JCH[1], 1.33654221029386f) + 1.f);
}


static inline void dt_UCS_JCH_to_HCB(const dt_aligned_pixel_t JCH, dt_aligned_pixel_t HCB)
{
  HCB[2] = JCH[0] * (powf(JCH[1], 1.33654221029386f) + 1.f);
  HCB[1] = JCH[1];
  HCB[0] = JCH[2];
}


static inline void dt_UCS_HCB_to_JCH(const dt_aligned_pixel_t HCB, dt_aligned_pixel_t JCH)
{
  JCH[2] = HCB[0];
  JCH[1] = HCB[1];
  JCH[0] = HCB[2] / (powf(HCB[1], 1.33654221029386f) + 1.f);
}


static inline void dt_UCS_HSB_to_HPW(const dt_aligned_pixel_t HSB, dt_aligned_pixel_t HPW)
{
  HPW[2] = sqrtf(HSB[1] * HSB[1] + HSB[2] * HSB[2]);
  HPW[1] = (HPW[2] > 0.f) ? HSB[1] / HPW[2] : 0.f;
  HPW[0] = HSB[0];
}


static inline void dt_UCS_HPW_to_HSB(const dt_aligned_pixel_t HPW, dt_aligned_pixel_t HSB)
{
  HSB[0] = HPW[0];
  HSB[1] = HPW[1] * HPW[2];
  HSB[2] = fmaxf(sqrtf(HPW[2] * HPW[2] - HSB[1] * HSB[1]), 0.f);
}

#undef DT_RESTRICT

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
