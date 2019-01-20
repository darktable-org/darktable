/*
 *    This file is part of darktable,
 *    copyright (c) 2009--2017 johannes hanika.
 *    copyright (c) 2011--2017 tobias ellinghaus.
 *    copyright (c) 2015 Bruce Guenter
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

#ifdef __SSE2__
#include "common/sse.h"
#include <xmmintrin.h>

#define d50_sse _mm_set_ps(0.0f, 0.8249f, 1.0f, 0.9642f)
#define d50_inv_sse _mm_set_ps(1.0f, 0.8249f, 1.0f, 0.9642f)

#define coef_Lab_to_XYZ_sse _mm_set_ps(0.0f, -1.0f / 200.0f, 1.0f / 116.0f, 1.0f / 500.0f)
#define offset_Lab_to_XYZ_sse _mm_set1_ps(0.137931034f)
#define epsilon_Lab_to_XYZ_sse _mm_set1_ps(0.20689655172413796f) // cbrtf(216.0f/24389.0f);

#define coef_XYZ_to_Lab_sse _mm_set_ps(0.0f, 200.0f, 500.0f, 116.0f)
#define kappa_sse _mm_set1_ps(24389.0f / 27.0f)
#define kappa_rcp_x16_sse 16.0f / kappa_sse
#define kappa_rcp_x116_sse 116.0f / kappa_sse
#define epsilon_XYZ_to_Lab_sse _mm_set1_ps(216.0f / 24389.0f)

static inline __m128 lab_f_inv_m(const __m128 x)
{
  // x > epsilon
  const __m128 res_big = x * x * x;
  // x <= epsilon
  const __m128 res_small = kappa_rcp_x116_sse * x - kappa_rcp_x16_sse;

  // blend results according to whether each component is > epsilon or not
  const __m128 mask = _mm_cmpgt_ps(x, epsilon_Lab_to_XYZ_sse);
  return _mm_or_ps(_mm_and_ps(mask, res_big), _mm_andnot_ps(mask, res_small));
}

/** uses D50 white point. */
static inline __m128 dt_Lab_to_XYZ_sse2(const __m128 Lab)
{
  // last component ins shuffle taken from 1st component of Lab to make sure it is not nan, so it will become
  // 0.0f in f
  const __m128 f = _mm_shuffle_ps(Lab, Lab, _MM_SHUFFLE(0, 2, 0, 1)) * coef_Lab_to_XYZ_sse;
  return d50_sse * lab_f_inv_m(f + _mm_shuffle_ps(f, f, _MM_SHUFFLE(1, 1, 3, 1)) + offset_Lab_to_XYZ_sse);
}

static inline __m128 lab_f_m_sse2(const __m128 x)
{
  // calculate as if x > epsilon : result = cbrtf(x)
  // approximate cbrtf(x):
  const __m128 a = _mm_castsi128_ps(
      _mm_add_epi32(_mm_cvtps_epi32(_mm_div_ps(_mm_cvtepi32_ps(_mm_castps_si128(x)), _mm_set1_ps(3.0f))),
                    _mm_set1_epi32(709921077)));
  const __m128 a3 = a * a * a;
  const __m128 res_big = a * (a3 + x + x) / (a3 + a3 + x);

  // calculate as if x <= epsilon : result = (kappa*x+16)/116
  const __m128 res_small = (kappa_sse * x + _mm_set1_ps(16.0f)) / _mm_set1_ps(116.0f);

  // blend results according to whether each component is > epsilon or not
  const __m128 mask = _mm_cmpgt_ps(x, epsilon_XYZ_to_Lab_sse);
  return _mm_or_ps(_mm_and_ps(mask, res_big), _mm_andnot_ps(mask, res_small));
}

/** uses D50 white point. */
static inline __m128 dt_XYZ_to_Lab_sse2(const __m128 XYZ)
{
  const __m128 f = lab_f_m_sse2(XYZ / d50_inv_sse);
  // because d50_inv.z is 0.0f, lab_f(0) == 16/116, so Lab[0] = 116*f[0] - 16 equal to 116*(f[0]-f[3])
  return coef_XYZ_to_Lab_sse * (_mm_shuffle_ps(f, f, _MM_SHUFFLE(3, 1, 0, 1)) - _mm_shuffle_ps(f, f, _MM_SHUFFLE(3, 2, 1, 3)));
}

static inline __m128 dt_XYZ_to_xyY_sse2(const __m128 XYZ)    // XYZ  = [  .   Z  |  Y    X   ]
{
  /** The xyY space is essentially a normalized XYZ space, where Y is the linear luminance
   * and (x, y) the normalized chroma coordinates
   *
   xyY    = XYZ    / sum
   --------------------------------------------
   xyY[0] = XYZ[0] / (XYZ[0] + XYZ[1] + XYZ[2])
   xyY[1] = XYZ[1] / (XYZ[0] + XYZ[1] + XYZ[2])
   xyY[2] = XYZ[1]
   xyY[3] = some crap (alpha layer, we don't care)
   * */

  // Horizontal sum of the first 3 elements - SIMD is worthless here,
  // so we let the compiler handle the shuffling
  const float sum = XYZ[0] + XYZ[1] + XYZ[2];

  // Normalize XYZ
  __m128 xyY = XYZ / sum;
  xyY[2] = XYZ[1];
  return xyY;
}

static inline __m128 dt_xyY_to_XYZ_sse2(const __m128 xyY)    // XYZ  = [  .   Y  |  y    x   ]
{
  /**
  XYZ    = sums                     * xyY[2] / shuf
  ---------------------------------------------------
  XYZ[0] = xyY[0]                   * xyY[2] / xyY[1]
  XYZ[1] = xyY[1]                   * xyY[2] / xyY[1]
  XYZ[2] = (1.0f - xyY[0] - xyY[1]) * xyY[2] / xyY[1]
  XYZ[3] = some crap (alpha layer, we don't care)
  **/

  __m128 XYZ = xyY;
  XYZ[2] = 1.0f - xyY[0] - xyY[1];
  XYZ = XYZ * xyY[2] / xyY[1];
  return XYZ;
}

// XYZ -> LCM, assuming equal energy illuminant
// https://en.wikipedia.org/wiki/LMS_color_space
#define xyz_to_lms_0_sse _mm_setr_ps( 0.4002f, -0.22900f, 0.0f, 0.0f)
#define xyz_to_lms_1_sse _mm_setr_ps( 0.7075f, 1.1500f, 0.0f, 0.0f)
#define xyz_to_lms_2_sse _mm_setr_ps(-0.0809f, 0.0612f, 0.9184f, 0.0f)

static inline __m128 dt_XYZ_to_LMS_sse2(const __m128 XYZ)
{
  __m128 lms
      = xyz_to_lms_0_sse * _mm_shuffle_ps(XYZ, XYZ, _MM_SHUFFLE(0, 0, 0, 0)) +
        xyz_to_lms_1_sse * _mm_shuffle_ps(XYZ, XYZ, _MM_SHUFFLE(1, 1, 1, 1)) +
        xyz_to_lms_2_sse * _mm_shuffle_ps(XYZ, XYZ, _MM_SHUFFLE(2, 2, 2, 2));
  return lms;
}

// LCM -> XYZ, assuming equal energy illuminant
// https://en.wikipedia.org/wiki/LMS_color_space
#define lms_to_xyz_0_sse _mm_setr_ps( 1.85924f,  0.36683f, 0.0f, 0.0f)
#define lms_to_xyz_1_sse _mm_setr_ps(-1.13830f,  0.64388f, 0.0f, 0.0f)
#define lms_to_xyz_2_sse _mm_setr_ps( 0.23884f, -0.01059f, 1.08885f, 0.0f)

static inline __m128 dt_LMS_to_XYZ_sse2(const __m128 LMS)
{
  __m128 xyz
      = lms_to_xyz_0_sse * _mm_shuffle_ps(LMS, LMS, _MM_SHUFFLE(0, 0, 0, 0)) +
        lms_to_xyz_1_sse * _mm_shuffle_ps(LMS, LMS, _MM_SHUFFLE(1, 1, 1, 1)) +
        lms_to_xyz_2_sse * _mm_shuffle_ps(LMS, LMS, _MM_SHUFFLE(2, 2, 2, 2));
  return xyz;
}

#define epsilon_LMS_hdr _mm_set1_ps(0.5213250183194932)
#define two_pow_epsilon _mm_pow_ps(_mm_set1_ps(2.0f), epsilon_LMS_hdr)
#define inv_epsilon_LMS_hdr 1.0f / epsilon_LMS_hdr

static inline __m128 dt_Michaelis_Menten(const __m128 LMS)
{
  /** Outputs NaN where LMS < 0
   * */
  // See https://eng.aurelienpierre.com/2019/01/17/derivating-hdr-ipt-direct-and-inverse-transformations/#fixing_ipt-hdr
  const __m128 LMS_pow_epsilon = _mm_pow_ps(LMS, epsilon_LMS_hdr);
  __m128 hdr = 246.06076715f * LMS_pow_epsilon / (LMS_pow_epsilon + two_pow_epsilon);
  return hdr;
}

static inline __m128 dt_Michaelis_Menten_inverse(const __m128 LMShdr)
{
  // See https://eng.aurelienpierre.com/2019/01/17/derivating-hdr-ipt-direct-and-inverse-transformations/#fixing_ipt-hdr
  const __m128 twice_LMShdr = 20000000.0f * LMShdr;
  const __m128 radicand = - (twice_LMShdr * two_pow_epsilon) / (twice_LMShdr - 4921215343.0f);
  return _mm_pow_ps(radicand, inv_epsilon_LMS_hdr);
}

#define zeros_sse _mm_setzero_ps()

static inline __m128 dt_LMS_to_LMShdr(const __m128 LMS)
{
  /** Returns dt_Michaelis_Menten(LMS) where LMS >= 0
   * and - dt_Michaelis_Menten(-LMS) where LMS < 0
   * */
  // See https://eng.aurelienpierre.com/2019/01/17/derivating-hdr-ipt-direct-and-inverse-transformations/#fixing_ipt-hdr
  const __m128 positive = _mm_cmpge_ps(LMS, zeros_sse); // positive or zero
  const __m128 negative = _mm_cmplt_ps(LMS, zeros_sse); // stricly negative
  return _mm_or_ps(_mm_and_ps(positive, dt_Michaelis_Menten(LMS)),
                    _mm_and_ps(negative, -dt_Michaelis_Menten(-LMS)));
}

static inline __m128 dt_LMShdr_to_LMS(const __m128 LMShdr)
{
  /** Returns dt_Michaelis_Menten_inv(LMS) where LMS >= 0
   * and - dt_Michaelis_Menten_inv(-LMS) where LMS < 0
   * */
  // See https://eng.aurelienpierre.com/2019/01/17/derivating-hdr-ipt-direct-and-inverse-transformations/#fixing_ipt-hdr
  const __m128 positive = _mm_cmpge_ps(LMShdr, zeros_sse); // positive or zero
  const __m128 negative = _mm_cmplt_ps(LMShdr, zeros_sse); // stricly negative
  return _mm_or_ps(_mm_and_ps(positive, dt_Michaelis_Menten_inverse(LMShdr)),
                    _mm_and_ps(negative, -dt_Michaelis_Menten_inverse(-LMShdr)));
}

// LMS-HDR -> IPT-HDR
// https://eng.aurelienpierre.com/2019/01/17/derivating-hdr-ipt-direct-and-inverse-transformations/#fixing_ipt-hdr
#define lms_to_ipt_0_sse _mm_setr_ps( 0.40000f,  4.45500f, 0.80560f, 0.0f)
#define lms_to_ipt_1_sse _mm_setr_ps( 0.40000f, -4.85100f, 0.35720f, 0.0f)
#define lms_to_ipt_2_sse _mm_setr_ps( 0.20000f,  0.39600f,-1.16280f, 0.0f)

static inline __m128 dt_LMShdr_to_IPThdr_sse2(const __m128 LMShdr)
{
  __m128 ipthdr
      = lms_to_ipt_0_sse * _mm_shuffle_ps(LMShdr, LMShdr, _MM_SHUFFLE(0, 0, 0, 0)) +
        lms_to_ipt_1_sse * _mm_shuffle_ps(LMShdr, LMShdr, _MM_SHUFFLE(1, 1, 1, 1)) +
        lms_to_ipt_2_sse * _mm_shuffle_ps(LMShdr, LMShdr, _MM_SHUFFLE(2, 2, 2, 2));
  return ipthdr;
}

// IPT-HDR -> LMS-HDR
// https://eng.aurelienpierre.com/2019/01/17/derivating-hdr-ipt-direct-and-inverse-transformations/#fixing_ipt-hdr
#define ipt_to_lms_0_sse _mm_setr_ps( 1.00000f,  1.00000f, 1.00000f, 0.0f)
#define ipt_to_lms_1_sse _mm_setr_ps( 0.09757f, -0.11388f, 0.13322f, 0.0f)
#define ipt_to_lms_2_sse _mm_setr_ps( 0.20523f,  0.13322f,-0.67689f, 0.0f)

static inline __m128 dt_IPThdr_to_LMShdr_sse2(const __m128 IPThdr)
{
  __m128 lmshdr
      = ipt_to_lms_0_sse * _mm_shuffle_ps(IPThdr, IPThdr, _MM_SHUFFLE(0, 0, 0, 0)) +
        ipt_to_lms_1_sse * _mm_shuffle_ps(IPThdr, IPThdr, _MM_SHUFFLE(1, 1, 1, 1)) +
        ipt_to_lms_2_sse * _mm_shuffle_ps(IPThdr, IPThdr, _MM_SHUFFLE(2, 2, 2, 2));
  return lmshdr;
}

static inline __m128 dt_XYZ_to_IPThdr_sse2(const __m128 XYZ)
{
  /** Wrapper function for direct transfer **/
  return dt_LMShdr_to_IPThdr_sse2(dt_LMS_to_LMShdr(dt_XYZ_to_LMS_sse2(XYZ)));
}

static inline __m128 dt_IPThdr_to_XYZ_sse2(const __m128 IPT)
{
  /** Wrapper function for direct transfer **/
  return dt_LMS_to_XYZ_sse2(dt_LMShdr_to_LMS(dt_IPThdr_to_LMShdr_sse2(IPT)));
}

static inline __m128 dt_Lab_to_Lch_sse2(const __m128 Lab)
{
  __m128 Lch = Lab;
  __m128 Lab2 = Lab * Lab;
  Lch[1] = powf((Lab2[1] + Lab2[2]), 0.5f);
  Lch[2] = atan2f(Lab[2], Lab[1]);
  return Lch;
}

static inline __m128 dt_Lch_to_Lab_sse2(const __m128 Lch)
{
  __m128 Lab = Lch;
  Lab[1] = cosf(Lch[2]) * Lch[1];
  Lab[2] = sinf(Lch[2]) * Lch[1];
  return Lab;
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
// XYZ -> prophotoRGB matrix, D50
#define xyz_to_rgb_0_sse _mm_setr_ps( 1.3459433f, -0.5445989f, 0.0000000f, 0.0f)
#define xyz_to_rgb_1_sse _mm_setr_ps(-0.2556075f,  1.5081673f, 0.0000000f, 0.0f)
#define xyz_to_rgb_2_sse _mm_setr_ps(-0.0511118f,  0.0205351f,  1.2118128f, 0.0f)

// see http://www.brucelindbloom.com/Eqn_RGB_XYZ_Matrix.html for the transformation matrices
static inline __m128 dt_XYZ_to_prophotoRGB_sse2(__m128 XYZ)
{
  __m128 rgb
      = xyz_to_rgb_0_sse * _mm_shuffle_ps(XYZ, XYZ, _MM_SHUFFLE(0, 0, 0, 0)) +
        xyz_to_rgb_1_sse * _mm_shuffle_ps(XYZ, XYZ, _MM_SHUFFLE(1, 1, 1, 1)) +
        xyz_to_rgb_2_sse * _mm_shuffle_ps(XYZ, XYZ, _MM_SHUFFLE(2, 2, 2, 2));
  return rgb;
}

// prophotoRGB -> XYZ matrix, D50
#define rgb_to_xyz_0_sse _mm_setr_ps(0.7976749f, 0.2880402f, 0.0000000f, 0.0f)
#define rgb_to_xyz_1_sse _mm_setr_ps(0.1351917f, 0.7118741f, 0.0000000f, 0.0f)
#define rgb_to_xyz_2_sse _mm_setr_ps(0.0313534f, 0.0000857f, 0.8252100f, 0.0f)

/** uses D50 white point. */
// see http://www.brucelindbloom.com/Eqn_RGB_XYZ_Matrix.html for the transformation matrices
static inline __m128 dt_prophotoRGB_to_XYZ_sse2(__m128 rgb)
{

  __m128 XYZ
      = rgb_to_xyz_0_sse * _mm_shuffle_ps(rgb, rgb, _MM_SHUFFLE(0, 0, 0, 0)) +
        rgb_to_xyz_1_sse * _mm_shuffle_ps(rgb, rgb, _MM_SHUFFLE(1, 1, 1, 1)) +
        rgb_to_xyz_2_sse * _mm_shuffle_ps(rgb, rgb, _MM_SHUFFLE(2, 2, 2, 2));
  return XYZ;
}
#endif

static inline float cbrt_5f(float f)
{
  uint32_t *p = (uint32_t *)&f;
  *p = *p / 3 + 709921077;
  return f;
}

static inline float cbrta_halleyf(const float a, const float R)
{
  const float a3 = a * a * a;
  const float b = a * (a3 + R + R) / (a3 + a3 + R);
  return b;
}

static inline float lab_f(const float x)
{
  const float epsilon = 216.0f / 24389.0f;
  const float kappa = 24389.0f / 27.0f;
  if(x > epsilon)
  {
    // approximate cbrtf(x):
    const float a = cbrt_5f(x);
    return cbrta_halleyf(a, x);
  }
  else
    return (kappa * x + 16.0f) / 116.0f;
}

/** uses D50 white point. */
static inline void dt_XYZ_to_Lab(const float *XYZ, float *Lab)
{
  const float d50[3] = { 0.9642f, 1.0f, 0.8249f };
  const float f[3] = { lab_f(XYZ[0] / d50[0]), lab_f(XYZ[1]), lab_f(XYZ[2] / d50[2]) };
  Lab[0] = 116.0f * f[1] - 16.0f;
  Lab[1] = 500.0f * (f[0] - f[1]);
  Lab[2] = 200.0f * (f[1] - f[2]);
}

static inline float lab_f_inv(const float x)
{
  const float epsilon = 0.20689655172413796f; // cbrtf(216.0f/24389.0f);
  const float kappa = 24389.0f / 27.0f;
  if(x > epsilon)
    return x * x * x;
  else
    return (116.0f * x - 16.0f) / kappa;
}

/** uses D50 white point. */
static inline void dt_Lab_to_XYZ(const float *Lab, float *XYZ)
{
  const float d50[3] = { 0.9642f, 1.0f, 0.8249f };
  const float fy = (Lab[0] + 16.0f) / 116.0f;
  const float fx = Lab[1] / 500.0f + fy;
  const float fz = fy - Lab[2] / 200.0f;
  XYZ[0] = d50[0] * lab_f_inv(fx);
  XYZ[1] = lab_f_inv(fy);
  XYZ[2] = d50[2] * lab_f_inv(fz);
}

/** uses D50 white point. */
static inline void dt_XYZ_to_sRGB(const float *const XYZ, float *sRGB)
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
static inline void dt_XYZ_to_sRGB_clipped(const float *const XYZ, float *sRGB)
{
  dt_XYZ_to_sRGB(XYZ, sRGB);

#define CLIP(a) ((a) < 0 ? 0 : (a) > 1 ? 1 : (a))

  for(int i = 0; i < 3; i++) sRGB[i] = CLIP(sRGB[i]);

#undef CLIP
}

static inline void dt_sRGB_to_XYZ(const float *const sRGB, float *XYZ)
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

static inline void dt_XYZ_to_prophotorgb(const float *const XYZ, float *rgb)
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

static inline void dt_prophotorgb_to_XYZ(const float *const rgb, float *XYZ)
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


static inline void dt_Lab_to_prophotorgb(const float *const Lab, float *rgb)
{
  float XYZ[3] = { 0.0f };
  dt_Lab_to_XYZ(Lab, XYZ);
  dt_XYZ_to_prophotorgb(XYZ, rgb);
}

static inline void dt_prophotorgb_to_Lab(const float *const rgb, float *Lab)
{
  float XYZ[3] = { 0.0f };
  dt_prophotorgb_to_XYZ(rgb, XYZ);
  dt_XYZ_to_Lab(XYZ, Lab);
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
