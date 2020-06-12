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
static inline void dt_RGB_2_HSL(const float *const RGB, float *const HSL)
{
  float H, S, L;

  float R = RGB[0];
  float G = RGB[1];
  float B = RGB[2];

  float var_Min = fminf(R, fminf(G, B));
  float var_Max = fmaxf(R, fmaxf(G, B));
  float del_Max = var_Max - var_Min;

  L = (var_Max + var_Min) / 2.0f;

  if(del_Max == 0.0f)
  {
    H = 0.0f;
    S = 0.0f;
  }
  else
  {
    if(L < 0.5f)
      S = del_Max / (var_Max + var_Min);
    else
      S = del_Max / (2.0f - var_Max - var_Min);

    float del_R = (((var_Max - R) / 6.0f) + (del_Max / 2.0f)) / del_Max;
    float del_G = (((var_Max - G) / 6.0f) + (del_Max / 2.0f)) / del_Max;
    float del_B = (((var_Max - B) / 6.0f) + (del_Max / 2.0f)) / del_Max;

    if(R == var_Max)
      H = del_B - del_G;
    else if(G == var_Max)
      H = (1.0f / 3.0f) + del_R - del_B;
    else if(B == var_Max)
      H = (2.0f / 3.0f) + del_G - del_R;
    else
      H = 0.0f; // make GCC happy

    if(H < 0.0f) H += 1.0f;
    if(H > 1.0f) H -= 1.0f;
  }

  HSL[0] = H;
  HSL[1] = S;
  HSL[2] = L;
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
    var_H = 1.0f - fabs(var_H) / (2.0f * DT_M_PI_F);

  LCH[0] = Lab[0];
  LCH[1] = sqrtf(Lab[1] * Lab[1] + Lab[2] * Lab[2]);
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

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
