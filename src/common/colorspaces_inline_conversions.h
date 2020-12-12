/*
 *    This file is part of darktable,
 *    Copyright (C) 2017-2020 darktable developers.
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
#include "common/darktable.h"

#ifdef __SSE2__
#include "common/sse.h" // also loads darkable.h
#include <xmmintrin.h>
#else
#include "common/darktable.h"
#endif


// When included by a C++ file, restrict qualifiers are not allowed
#ifdef __cplusplus
#define DT_RESTRICT
#else
#define DT_RESTRICT restrict
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
#pragma omp declare simd
#endif
static inline float cbrt_5f(float f)
{
  uint32_t *p = (uint32_t *)&f;
  *p = *p / 3 + 709921077;
  return f;
}

#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline float cbrta_halleyf(const float a, const float R)
{
  const float a3 = a * a * a;
  const float b = a * (a3 + R + R) / (a3 + a3 + R);
  return b;
}

#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline float lab_f(const float x)
{
  const float epsilon = 216.0f / 24389.0f;
  const float kappa = 24389.0f / 27.0f;
  return (x > epsilon) ? cbrta_halleyf(cbrt_5f(x), x) : (kappa * x + 16.0f) / 116.0f;
}

/** uses D50 white point. */
#ifdef _OPENMP
#pragma omp declare simd aligned(Lab, XYZ:16) uniform(Lab, XYZ)
#endif
static inline void dt_XYZ_to_Lab(const float XYZ[3], float Lab[3])
{
  const float d50[3] = { 0.9642f, 1.0f, 0.8249f };
  float f[3] = { 0.0f };
  for(int i = 0; i < 3; i++) f[i] = lab_f(XYZ[i] / d50[i]);
  Lab[0] = 116.0f * f[1] - 16.0f;
  Lab[1] = 500.0f * (f[0] - f[1]);
  Lab[2] = 200.0f * (f[1] - f[2]);
}

#ifdef _OPENMP
#pragma omp declare simd
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
static inline void dt_Lab_to_XYZ(const float Lab[3], float XYZ[3])
{
  const float d50[3] = { 0.9642f, 1.0f, 0.8249f };
  const float fy = (Lab[0] + 16.0f) / 116.0f;
  const float fx = Lab[1] / 500.0f + fy;
  const float fz = fy - Lab[2] / 200.0f;
  const float f[3] = { fx, fy, fz };
  for(int i = 0; i < 3; i++) XYZ[i] = d50[i] * lab_f_inv(f[i]);
}


#ifdef _OPENMP
#pragma omp declare simd aligned(xyY, uvY:16)
#endif
static inline void dt_xyY_to_uvY(const float xyY[3], float uvY[3])
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
static inline void dt_xyY_to_Luv(const float xyY[3], float Luv[3])
{
  // This is the second, non-linear, part of the the 1976 CIE L*u*v* transform.
  // See https://en.wikipedia.org/wiki/CIELUV
  // It is intended to provide perceptual hue-linear-ish controls and settings for more intuitive GUI.
  // Don't ever use it for pixel-processing, it sucks, it's old, it kills kittens and makes your mother cry.
  // Seriously, don't.
  // You need to convert Luv parameters to XYZ or RGB and properly process pixels in RGB or XYZ or related spaces.
  float uvY[3];
  dt_xyY_to_uvY(xyY, uvY);

  // We assume Yn == 1 == peak luminance
  const float threshold = cbf(6.0f / 29.0f);
  Luv[0] = (uvY[2] <= threshold) ? cbf(29.0f / 3.0f) * uvY[2] : 116.0f * cbrtf(uvY[2]) - 16.f;

  const float D50[2] DT_ALIGNED_PIXEL = { 0.20915914598542354f, 0.488075320769787f };
  Luv[1] = 13.f * Luv[0] * (uvY[0] - D50[0]); // u*
  Luv[2] = 13.f * Luv[0] * (uvY[1] - D50[1]); // v*

  // Output is in [0; 100] for all channels
}


static inline void dt_Luv_to_Lch(const float Luv[3], float Lch[3])
{
  Lch[0] = Luv[0];                 // L stays L
  Lch[1] = hypotf(Luv[2], Luv[1]); // chroma radius
  Lch[2] = atan2f(Luv[2], Luv[1]); // hue angle
  Lch[2] = (Lch[2] < 0.f) ? 2.f * M_PI + Lch[2] : Lch[2]; // ensure angle is positive modulo 2 pi
}


static inline void dt_xyY_to_Lch(const float xyY[3], float Lch[3])
{
  float Luv[3];
  dt_xyY_to_Luv(xyY, Luv);
  dt_Luv_to_Lch(Luv, Lch);
}


#ifdef _OPENMP
#pragma omp declare simd aligned(uvY, xyY:16)
#endif
static inline void dt_uvY_to_xyY(const float uvY[3], float xyY[3])
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
static inline void dt_Luv_to_xyY(const float Luv[3], float xyY[3])
{
  // This is the second, non-linear, part of the the 1976 CIE L*u*v* transform.
  // See https://en.wikipedia.org/wiki/CIELUV
  // It is intended to provide perceptual hue-linear-ish controls and settings for more intuitive GUI.
  // Don't ever use it for pixel-processing, it sucks, it's old, it kills kittens and makes your mother cry.
  // Seriously, don't.
  // You need to convert Luv parameters to XYZ or RGB and properly process pixels in RGB or XYZ or related spaces.
  float uvY[3];

  // We assume Yn == 1 == peak luminance
  static const float threshold = 8.0f;
  uvY[2] = (Luv[0] <= threshold) ? Luv[0] * cbf(3.f / 29.f) : cbf((Luv[0] + 16.f) / 116.f);

  static const float D50[2] DT_ALIGNED_PIXEL = { 0.20915914598542354f, 0.488075320769787f };
  uvY[0] = Luv[1] / (Luv[0] * 13.f) + D50[0];  // u' = u* / 13 L + u_n
  uvY[1] = Luv[2] / (Luv[0] * 13.f) + D50[1];  // v' = v* / 13 L + v_n

  dt_uvY_to_xyY(uvY, xyY);
  // Output is normalized for all channels
}

static inline void dt_Lch_to_Luv(const float Lch[3], float Luv[3])
{
  Luv[0] = Lch[0];                // L stays L
  Luv[1] = Lch[1] * cosf(Lch[2]); // radius * cos(angle)
  Luv[2] = Lch[1] * sinf(Lch[2]); // radius * sin(angle)
}

static inline void dt_Lch_to_xyY(const float Lch[3], float xyY[3])
{
  float Luv[3];
  dt_Lch_to_Luv(Lch, Luv);
  dt_Luv_to_xyY(Luv, xyY);
}

/** uses D50 white point. */
#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline void dt_XYZ_to_sRGB(const float *const XYZ, float *const sRGB)
{
  const float xyz_to_srgb_matrix[3][3] = { { 3.1338561, -1.6168667, -0.4906146 },
                                           { -0.9787684, 1.9161415, 0.0334540 },
                                           { 0.0719453, -0.2289914, 1.4052427 } };

  // XYZ -> sRGB
  float rgb[3] = { 0, 0, 0 };
  for(int r = 0; r < 3; r++)
    for(int c = 0; c < 3; c++) rgb[r] += xyz_to_srgb_matrix[r][c] * XYZ[c];
  // linear sRGB -> gamma corrected sRGB
  for(int c = 0; c < 3; c++)
    sRGB[c] = rgb[c] <= 0.0031308 ? 12.92 * rgb[c] : (1.0 + 0.055) * powf(rgb[c], 1.0 / 2.4) - 0.055;
}


/** Uses D50 **/
#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline void dt_XYZ_to_Rec709_D50(const float *const XYZ, float *const sRGB)
{
  // linear sRGB == Rec709 with no gamma
  const float xyz_to_srgb_matrix[3][3] = { {  3.1338561f, -1.6168667f, -0.4906146f },
                                           { -0.9787684f,  1.9161415f,  0.0334540f },
                                           {  0.0719453f, -0.2289914f,  1.4052427f } };

  // XYZ -> sRGB
  float rgb[3] = { 0, 0, 0 };
  for(int r = 0; r < 3; r++)
    for(int c = 0; c < 3; c++) rgb[r] += xyz_to_srgb_matrix[r][c] * XYZ[c];
  for(int r = 0; r < 3; r++) sRGB[r] = rgb[r];
}


/** Uses D65 **/
#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline void dt_XYZ_to_Rec709_D65(const float *const XYZ, float *const sRGB)
{
  // linear sRGB == Rec709 with no gamma
  const float xyz_to_srgb_matrix[3][3] = { {  3.2404542f, -1.5371385f, -0.4985314f },
                                           { -0.9692660f,  1.8760108f,  0.0415560f },
                                           {  0.0556434f, -0.2040259f,  1.0572252f } };

  // XYZ -> sRGB
  float rgb[3] = { 0, 0, 0 };
  for(int r = 0; r < 3; r++)
    for(int c = 0; c < 3; c++) rgb[r] += xyz_to_srgb_matrix[r][c] * XYZ[c];
  for(int r = 0; r < 3; r++) sRGB[r] = rgb[r];
}


/** uses D50 white point and clips the output to [0..1]. */
#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline void dt_XYZ_to_sRGB_clipped(const float *const XYZ, float *const sRGB)
{
  dt_XYZ_to_sRGB(XYZ, sRGB);

#define CLIP(a) ((a) < 0 ? 0 : (a) > 1 ? 1 : (a))

  for(int i = 0; i < 3; i++) sRGB[i] = CLIP(sRGB[i]);

#undef CLIP
}


#ifdef _OPENMP
#pragma omp declare simd aligned(sRGB, XYZ_D50: 16)
#endif
static inline void dt_Rec709_to_XYZ_D50(const float *const DT_RESTRICT sRGB, float *const DT_RESTRICT XYZ_D50)
{
  // Conversion matrix from http://www.brucelindbloom.com/Eqn_RGB_XYZ_Matrix.html
  const float M[3][4] DT_ALIGNED_PIXEL = {
      { 0.4360747f, 0.3850649f, 0.1430804f, 0.0f },
      { 0.2225045f, 0.7168786f, 0.0606169f, 0.0f },
      { 0.0139322f, 0.0971045f, 0.7141733f, 0.0f },
  };

  // sRGB -> XYZ
  for(size_t x = 0; x < 3; x++)
      XYZ_D50[x] = M[x][0] * sRGB[0] + M[x][1] * sRGB[1] + M[x][2] * sRGB[2];
}


#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline void dt_sRGB_to_XYZ(const float *const sRGB, float *const XYZ)
{
  const float srgb_to_xyz[3][3] = { { 0.4360747, 0.3850649, 0.1430804 },
                                    { 0.2225045, 0.7168786, 0.0606169 },
                                    { 0.0139322, 0.0971045, 0.7141733 } };

  // sRGB -> XYZ
  XYZ[0] = XYZ[1] = XYZ[2] = 0.0;
  float rgb[3] = { 0 };
  // gamma corrected sRGB -> linear sRGB
  for(int c = 0; c < 3; c++)
    rgb[c] = sRGB[c] <= 0.04045 ? sRGB[c] / 12.92 : powf((sRGB[c] + 0.055) / (1 + 0.055), 2.4);
  for(int r = 0; r < 3; r++)
    for(int c = 0; c < 3; c++) XYZ[r] += srgb_to_xyz[r][c] * rgb[c];
}

#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline void dt_XYZ_to_prophotorgb(const float *const XYZ, float *const rgb)
{
  const float xyz_to_rgb[3][3] = {
    // prophoto rgb d50
    { 1.3459433f, -0.2556075f, -0.0511118f},
    {-0.5445989f,  1.5081673f,  0.0205351f},
    { 0.0000000f,  0.0000000f,  1.2118128f},
  };
  rgb[0] = rgb[1] = rgb[2] = 0.0f;
  for(int r = 0; r < 3; r++)
    for(int c = 0; c < 3; c++) rgb[r] += xyz_to_rgb[r][c] * XYZ[c];
}

#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline void dt_prophotorgb_to_XYZ(const float *const rgb, float *const XYZ)
{
  const float rgb_to_xyz[3][3] = {
    // prophoto rgb
    {0.7976749f, 0.1351917f, 0.0313534f},
    {0.2880402f, 0.7118741f, 0.0000857f},
    {0.0000000f, 0.0000000f, 0.8252100f},
  };
  XYZ[0] = XYZ[1] = XYZ[2] = 0.0f;
  for(int r = 0; r < 3; r++)
    for(int c = 0; c < 3; c++) XYZ[r] += rgb_to_xyz[r][c] * rgb[c];
}


#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline void dt_Lab_to_prophotorgb(const float *const Lab, float *const rgb)
{
  float XYZ[3] = { 0.0f };
  dt_Lab_to_XYZ(Lab, XYZ);
  dt_XYZ_to_prophotorgb(XYZ, rgb);
}

#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline void dt_prophotorgb_to_Lab(const float *const rgb, float *const Lab)
{
  float XYZ[3] = { 0.0f };
  dt_prophotorgb_to_XYZ(rgb, XYZ);
  dt_XYZ_to_Lab(XYZ, Lab);
}


#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline float _dt_RGB_2_Hue(const float *const DT_RESTRICT RGB, const float max, const float delta)
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
static inline void _dt_Hue_2_RGB(float *const DT_RESTRICT RGB, const float H, const float C, const float min)
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
static inline void dt_RGB_2_HSL(const float *const DT_RESTRICT RGB, float *const DT_RESTRICT HSL)
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
static inline void dt_HSL_2_RGB(const float *const DT_RESTRICT HSL, float *const DT_RESTRICT RGB)
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
static inline void dt_RGB_2_HSV(const float *const DT_RESTRICT RGB, float *const DT_RESTRICT HSV)
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
static inline void dt_HSV_2_RGB(const float *const DT_RESTRICT HSV, float *const DT_RESTRICT RGB)
{
  // almost straight from https://en.wikipedia.org/wiki/HSL_and_HSV
  const float C = HSV[1] * HSV[2];
  const float m = HSV[2] - C;
  _dt_Hue_2_RGB(RGB, HSV[0], C, m);
}


#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline void dt_Lab_2_LCH(const float *const Lab, float *const LCH)
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
static inline void dt_LCH_2_Lab(const float *const LCH, float *const Lab)
{
  Lab[0] = LCH[0];
  Lab[1] = cosf(2.0f * DT_M_PI_F * LCH[2]) * LCH[1];
  Lab[2] = sinf(2.0f * DT_M_PI_F * LCH[2]) * LCH[1];
}

static inline float dt_camera_rgb_luminance(const float *const rgb)
{
  return (rgb[0] * 0.2225045f + rgb[1] * 0.7168786f + rgb[2] * 0.0606169f);
}


#ifdef _OPENMP
#pragma omp declare simd aligned(XYZ_D50, XYZ_D65: 16)
#endif
static inline void dt_XYZ_D50_2_XYZ_D65(const float *const DT_RESTRICT XYZ_D50, float *const DT_RESTRICT XYZ_D65)
{
  // Bradford adaptation matrix from http://www.brucelindbloom.com/index.html?Eqn_ChromAdapt.html
  const float M[3][4] DT_ALIGNED_ARRAY = {
      {  0.9555766f, -0.0230393f,  0.0631636f, 0.0f },
      { -0.0282895f,  1.0099416f,  0.0210077f, 0.0f },
      {  0.0122982f, -0.0204830f,  1.3299098f, 0.0f },
  };
  for(size_t x = 0; x < 3; x++)
    XYZ_D65[x] = M[x][0] * XYZ_D50[0] + M[x][1] * XYZ_D50[1] + M[x][2] * XYZ_D50[2];
}

#ifdef _OPENMP
#pragma omp declare simd aligned(XYZ_D50, XYZ_D65: 16)
#endif
static inline void dt_XYZ_D65_2_XYZ_D50(const float *const DT_RESTRICT XYZ_D65, float *const DT_RESTRICT XYZ_D50)
{
  // Bradford adaptation matrix from http://www.brucelindbloom.com/index.html?Eqn_ChromAdapt.html
  const float M[3][4] DT_ALIGNED_ARRAY = {
      {  1.0478112f,  0.0228866f, -0.0501270f, 0.0f },
      {  0.0295424f,  0.9904844f, -0.0170491f, 0.0f },
      { -0.0092345f,  0.0150436f,  0.7521316f, 0.0f },
  };
  for(size_t x = 0; x < 3; x++)
    XYZ_D50[x] = M[x][0] * XYZ_D65[0] + M[x][1] * XYZ_D65[1] + M[x][2] * XYZ_D65[2];
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
static inline void dt_XYZ_2_JzAzBz(const float *const DT_RESTRICT XYZ_D65, float *const DT_RESTRICT JzAzBz)
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
  const float M[3][4] DT_ALIGNED_ARRAY = {
      { 0.41478972f, 0.579999f, 0.0146480f, 0.0f },
      { -0.2015100f, 1.120649f, 0.0531008f, 0.0f },
      { -0.0166008f, 0.264800f, 0.6684799f, 0.0f },
  };
  const float A[3][4] DT_ALIGNED_ARRAY = {
      { 0.5f,       0.5f,       0.0f,      0.0f },
      { 3.524000f, -4.066708f,  0.542708f, 0.0f },
      { 0.199076f,  1.096799f, -1.295875f, 0.0f },
  };

  float XYZ[4] DT_ALIGNED_PIXEL = { 0.0f, 0.0f, 0.0f, 0.0f };
  float LMS[4] DT_ALIGNED_PIXEL = { 0.0f, 0.0f, 0.0f, 0.0f };

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
#ifdef _OPENMP
#pragma omp simd aligned(LMS, JzAzBz:16) aligned(A:64)
#endif
  for(int i = 0; i < 3; i++) JzAzBz[i] = A[i][0] * LMS[0] + A[i][1] * LMS[1] + A[i][2] * LMS[2];
  // Iz -> Jz
  JzAzBz[0] = ((1.0f + d) * JzAzBz[0]) / (1.0f + d * JzAzBz[0]) - d0;
}

#ifdef _OPENMP
#pragma omp declare simd aligned(JzAzBz, JzCzhz: 16)
#endif
static inline void dt_JzAzBz_2_JzCzhz(const float *const DT_RESTRICT JzAzBz, float *const DT_RESTRICT JzCzhz)
{
  float var_H = atan2f(JzAzBz[2], JzAzBz[1]) / (2.0f * DT_M_PI_F);
  JzCzhz[0] = JzAzBz[0];
  JzCzhz[1] = hypotf(JzAzBz[1], JzAzBz[2]);
  JzCzhz[2] = var_H >= 0.0f ? var_H : 1.0f + var_H;
}

#ifdef _OPENMP
#pragma omp declare simd aligned(JzCzhz, JzAzBz: 16)
#endif
static inline void dt_JzCzhz_2_JzAzBz(const float *const DT_RESTRICT JzCzhz, float *const DT_RESTRICT JzAzBz)
{
  JzAzBz[0] = JzCzhz[0];
  JzAzBz[1] = cosf(2.0f * DT_M_PI_F * JzCzhz[2]) * JzCzhz[1];
  JzAzBz[2] = sinf(2.0f * DT_M_PI_F * JzCzhz[2]) * JzCzhz[1];
}

#ifdef _OPENMP
#pragma omp declare simd aligned(JzAzBz, XYZ_D65: 16)
#endif
static inline void dt_JzAzBz_2_XYZ(const float *const DT_RESTRICT JzAzBz, float *const DT_RESTRICT XYZ_D65)
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
  const float MI[3][4] DT_ALIGNED_ARRAY = {
      {  1.9242264357876067f, -1.0047923125953657f,  0.0376514040306180f, 0.0f },
      {  0.3503167620949991f,  0.7264811939316552f, -0.0653844229480850f, 0.0f },
      { -0.0909828109828475f, -0.3127282905230739f,  1.5227665613052603f, 0.0f },
  };
  const float AI[3][4] DT_ALIGNED_ARRAY = {
      {  1.0f,  0.1386050432715393f,  0.0580473161561189f, 0.0f },
      {  1.0f, -0.1386050432715393f, -0.0580473161561189f, 0.0f },
      {  1.0f, -0.0960192420263190f, -0.8118918960560390f, 0.0f },
  };

  float XYZ[4] DT_ALIGNED_PIXEL = { 0.0f, 0.0f, 0.0f, 0.0f };
  float LMS[4] DT_ALIGNED_PIXEL = { 0.0f, 0.0f, 0.0f, 0.0f };
  float IzAzBz[4] DT_ALIGNED_PIXEL = { 0.0f, 0.0f, 0.0f, 0.0f };

  IzAzBz[0] = JzAzBz[0] + d0;
  IzAzBz[0] = IzAzBz[0] / (1.0f + d - d * IzAzBz[0]);
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

#undef DT_RESTRICT

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
