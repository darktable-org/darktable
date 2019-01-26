/*
 *    This file is part of darktable,
 *    copyright (c) 2009--2017 johannes hanika.
 *    copyright (c) 2011--2017 tobias ellinghaus.
 *    copyright (c) 2015 Bruce Guenter
 *    copyright (c) 2019 Aurélien Pierre.
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

#include "common/algebra.h"
#include "common/colorspaces.h"
#include <math.h>

#ifdef __SSE2__
#include "common/sse.h"
#include <xmmintrin.h>
#endif

/***
 * Lab <-> XYZ
 *
 * Lab is non-linear and the conversion is slow, so we use an approximation of the
 * cubic root, which accuracy is subject to caution.
 *
 * Lab is the standard color-space of the pixelpipe between input color-profile and
 * output color-profile IOPs, both of them assume a D50 illuminant.
***/

/** Constants **/
static const float d50[3]              = { 0.9642f, 1.0f, 0.8249f };
static const float epsilon_XYZ_to_Lab  = 216.0f / 24389.0f;
static const float epsilon_Lab_to_XYZ  = 0.206896552f; //cubic root(epsilon_XYZ_to_Lab)
static const float kappa               = 24389.0f / 27.0f;
static const float Lab_coeff[3]        = { 116.0f, 500.0f, 200.0f };

#ifdef __SSE2__
#define d50_sse                        _mm_set_ps(0.0f, d50[2], d50[1], d50[0])
#define d50_inv_sse                    _mm_set_ps(1.0f, d50[2], d50[1], d50[0])
#define epsilon_Lab_to_XYZ_sse         _mm_set1_ps(epsilon_Lab_to_XYZ)
#define kappa_sse                      _mm_set1_ps(kappa)
#define epsilon_XYZ_to_Lab_sse         _mm_set1_ps(epsilon_XYZ_to_Lab)
#define coef_XYZ_to_Lab_sse            _mm_set_ps(0.0f, Lab_coeff[2], Lab_coeff[1], Lab_coeff[0])
#define coef_Lab_to_XYZ_sse            _mm_set_ps(0.0f, -1.0f / Lab_coeff[2], 1.0f / Lab_coeff[0], 1.0f / Lab_coeff[1])
#define offset_Lab_to_XYZ_sse          _mm_set1_ps(0.137931034f)
#define kappa_rcp_x16_sse              _mm_set1_ps(16.0f) / kappa_sse
#define kappa_rcp_x116_sse             _mm_set1_ps(116.0f) / kappa_sse
#endif

/** Conversions **/

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
  if(x > epsilon_XYZ_to_Lab)
  {
    // Cubic root approximation
    return cbrta_halleyf(cbrt_5f(x), x);
  }
  else
    return (kappa * x + 16.0f) / 116.0f;
}

static inline void dt_XYZ_to_Lab(const float *XYZ, float *Lab)
{
  const float f[3] = { lab_f(XYZ[0] / d50[0]),
                       lab_f(XYZ[1] / d50[1]),
                       lab_f(XYZ[2] / d50[2]) };

  Lab[0] = Lab_coeff[0] * f[1] - 16.0f;
  Lab[1] = Lab_coeff[1] * (f[0] - f[1]);
  Lab[2] = Lab_coeff[2] * (f[1] - f[2]);
}

static inline float lab_f_inv(const float x)
{
  if(x > epsilon_Lab_to_XYZ)
    return x * x * x;
  else
    return (116.0f * x - 16.0f) / kappa;
}

static inline void dt_Lab_to_XYZ(const float *Lab, float *XYZ)
{
  const float fy = (Lab[0] + 16.0f) / Lab_coeff[0];
  const float fx = Lab[1] / Lab_coeff[1] + fy;
  const float fz = fy - Lab[2] / Lab_coeff[2];
  XYZ[0] = d50[0] * lab_f_inv(fx);
  XYZ[1] = d50[1] * lab_f_inv(fy);
  XYZ[2] = d50[2] * lab_f_inv(fz);
}

#ifdef __SSE2__
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

static inline __m128 dt_XYZ_to_Lab_sse2(const __m128 XYZ)
{
  const __m128 f = lab_f_m_sse2(XYZ / d50_inv_sse);
  // because d50_inv.z is 0.0f, lab_f(0) == 16/116, so Lab[0] = 116*f[0] - 16 equal to 116*(f[0]-f[3])
  return coef_XYZ_to_Lab_sse * (_mm_shuffle_ps(f, f, _MM_SHUFFLE(3, 1, 0, 1)) - _mm_shuffle_ps(f, f, _MM_SHUFFLE(3, 2, 1, 3)));
}
#endif


/***
 * XYZ <-> xyY
 *
 * xyY is an XYZ space normalized in luminance, so manipulations of Y (the scene-referred
 * luminance) don't affect the scene-referred colors.
 *
 * The pure-C function have a SSE check so, if they are called from within a process() function
 * and SSE is available, they will branch to the fast path. This is usefull for IOPs which
 * don't have a process_sse2() function, like tonecurve.
 ***/


#ifdef __SSE2__
static inline __m128 dt_XYZ_to_xyY_sse2(const __m128 XYZ)
{
  /**
   xyY    = XYZ    / sum
   --------------------------------------------
   xyY[0] = XYZ[0] / (XYZ[0] + XYZ[1] + XYZ[2])
   xyY[1] = XYZ[1] / (XYZ[0] + XYZ[1] + XYZ[2])
   xyY[2] = XYZ[1]
   xyY[3] = some crap (alpha layer, we don't care)
   * */

  __m128 xyY = XYZ /
                ( _mm_shuffle_ps(XYZ, XYZ, _MM_SHUFFLE(3, 0, 0, 0)) +
                  _mm_shuffle_ps(XYZ, XYZ, _MM_SHUFFLE(3, 1, 1, 1)) +
                  _mm_shuffle_ps(XYZ, XYZ, _MM_SHUFFLE(3, 2, 2, 2)));
  xyY[2] = XYZ[1];
  return xyY;
}

static inline __m128 dt_xyY_to_XYZ_sse2(const __m128 xyY)
{
  /**
  XYZ    = sums                     * xyY[2] / shuf
  ---------------------------------------------------
  XYZ[0] = xyY[0]                   * xyY[2] / xyY[1]
  XYZ[1] = xyY[1]                   * xyY[2] / xyY[1] == xyY[2]
  XYZ[2] = (1.0f - xyY[0] - xyY[1]) * xyY[2] / xyY[1]
  XYZ[3] = some crap (alpha layer, we don't care)
  **/

  __m128 XYZ = xyY;
  XYZ[2] = 1.0f - xyY[0] - xyY[1];
  XYZ = XYZ * _mm_shuffle_ps(xyY, xyY, _MM_SHUFFLE(2, 2, 2, 2)) /
          _mm_shuffle_ps(xyY, xyY, _MM_SHUFFLE(2, 1, 1, 1));
  return XYZ;
}
#endif

static inline void dt_XYZ_to_xyY(const float *XYZ, float *const xyY)
{
  const float sum = XYZ[0] + XYZ[1] + XYZ[2];
  xyY[0] = XYZ[0] / sum;
  xyY[1] = XYZ[1] / sum;
  xyY[2] = XYZ[1];
}

static inline void dt_xyY_to_XYZ(const float *xyY, float *const XYZ)
{
  XYZ[0] = xyY[0] * xyY[2] / xyY[1];
  XYZ[1] = xyY[2];
  XYZ[2] = (1.0f - xyY[0] - xyY[1]) * xyY[2] / xyY[1];
}


/***
 * XYZ <-> RGB
 *
 * Simple matrix-vector dot products choosing the right matrix.
 *
 * The RGB matrices assume a D50 illuminant in and out.
 *
 * see http://www.brucelindbloom.com/Eqn_RGB_XYZ_Matrix.html for the transformation matrices
 ***/

/** Matrices **/
static const float xyz_to_prophotorgb[3][3] = {
    { 1.3459433f, -0.2556075f, -0.0511118f},
    {-0.5445989f,  1.5081673f,  0.0205351f},
    { 0.0000000f,  0.0000000f,  1.2118128f},
  };

static const float prophotorgb_to_xyz[3][3] = {
    {0.7976749f, 0.1351917f, 0.0313534f},
    {0.2880402f, 0.7118741f, 0.0000857f},
    {0.0000000f, 0.0000000f, 0.8252100f},
  };

/** Conversions **/

#ifdef __SSE2__
static inline __m128 dt_XYZ_to_prophotoRGB_sse2(__m128 XYZ)
{
  __m128 rgb;
  mat3mulv_sse2(&rgb, xyz_to_prophotorgb, &XYZ);
  return rgb;
}

static inline __m128 dt_prophotoRGB_to_XYZ_sse2(__m128 rgb)
{
  __m128 XYZ;
  mat3mulv_sse2(&XYZ, prophotorgb_to_xyz, &rgb);
  return XYZ;
}
#endif

static inline void dt_XYZ_to_prophotorgb(const float *const XYZ, float *const rgb)
{
  mat3mulv(rgb, (const float *)&xyz_to_prophotorgb[0][0], XYZ);
}

static inline void dt_prophotorgb_to_XYZ(const float *const rgb, float *const XYZ)
{
  mat3mulv(XYZ, (const float *)&prophotorgb_to_xyz[0][0], rgb);
}


#ifdef __SSE2__
/** XYZ -> LCM, assuming XYZ D50, adjusted from D65 values using Bradford transform
* https://en.wikipedia.org/wiki/LMS_color_space
*
*[[ 0.44098236  0.7087099  -0.09297051]
* [-0.20549234  1.13475958  0.03785294]
* [-0.00848096  0.01381604  0.69075766]]
**/
/* D65 coeffs
#define xyz_to_lms_0_sse _mm_setr_ps( 0.4002f, -0.22800f, 0.0f, 0.0f)
#define xyz_to_lms_1_sse _mm_setr_ps( 0.7075f, 1.1500f, 0.0f, 0.0f)
#define xyz_to_lms_2_sse _mm_setr_ps(-0.0809f, 0.0612f, 0.9184f, 0.0f)
*/
#define xyz_to_lms_0_sse _mm_setr_ps( 0.44098236f, -0.20549234f, -0.00848096f, 0.0f)
#define xyz_to_lms_1_sse _mm_setr_ps( 0.7087099f, 1.13475958f, 0.01381604f, 0.0f)
#define xyz_to_lms_2_sse _mm_setr_ps(-0.09297051f, 0.03785294f, 0.69075766f, 0.0f)

static inline __m128 dt_XYZ_to_LMS_sse2(const __m128 XYZ)
{
  __m128 lms
      = xyz_to_lms_0_sse * _mm_shuffle_ps(XYZ, XYZ, _MM_SHUFFLE(0, 0, 0, 0)) +
        xyz_to_lms_1_sse * _mm_shuffle_ps(XYZ, XYZ, _MM_SHUFFLE(1, 1, 1, 1)) +
        xyz_to_lms_2_sse * _mm_shuffle_ps(XYZ, XYZ, _MM_SHUFFLE(2, 2, 2, 2));
  return lms;
}

/** LCM -> XYZ, assuming XYZ D50, adjusted from D65 values using Bradford transform
* https://en.wikipedia.org/wiki/LMS_color_space
*[[ 1.75959743 -1.10256915  0.29724775]
* [ 0.31813513  0.68248784  0.0054187 ]
* [ 0.01524082 -0.02718773  1.45122688]]
* */
/* D65 coeffs
#define lms_to_xyz_0_sse _mm_setr_ps( 1.85924f,  0.36683f, 0.0f, 0.0f)
#define lms_to_xyz_1_sse _mm_setr_ps(-1.13830f,  0.64388f, 0.0f, 0.0f)
#define lms_to_xyz_2_sse _mm_setr_ps( 0.23884f, -0.01059f, 1.08885f, 0.0f)
*/

#define lms_to_xyz_0_sse _mm_setr_ps( 1.75959743f,  0.31813513f, 0.01524082f, 0.0f)
#define lms_to_xyz_1_sse _mm_setr_ps(-1.10256915f,  0.68248784f, -0.02718773f, 0.0f)
#define lms_to_xyz_2_sse _mm_setr_ps( 0.29724775f,  0.0054187f, 1.45122688f, 0.0f)

static inline __m128 dt_LMS_to_XYZ_sse2(const __m128 LMS)
{
  __m128 xyz
      = lms_to_xyz_0_sse * _mm_shuffle_ps(LMS, LMS, _MM_SHUFFLE(0, 0, 0, 0)) +
        lms_to_xyz_1_sse * _mm_shuffle_ps(LMS, LMS, _MM_SHUFFLE(1, 1, 1, 1)) +
        lms_to_xyz_2_sse * _mm_shuffle_ps(LMS, LMS, _MM_SHUFFLE(2, 2, 2, 2));
  return xyz;
}

#define epsilon_LMS_hdr _mm_set1_ps(0.5213250183194932f)
#define two_pow_epsilon _mm_pow_ps(_mm_set1_ps(2.0f), epsilon_LMS_hdr)
#define inv_epsilon_LMS_hdr 1.0f / epsilon_LMS_hdr

static inline __m128 dt_Michaelis_Menten(const __m128 LMS)
{
  /**
   * Outputs NaN where LMS < 0
   **/
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
  /**
   * Returns dt_Michaelis_Menten(LMS) where LMS >= 0
   * and - dt_Michaelis_Menten(-LMS) where LMS < 0
   **/
  // See https://eng.aurelienpierre.com/2019/01/17/derivating-hdr-ipt-direct-and-inverse-transformations/#fixing_ipt-hdr
  const __m128 negative = _mm_cmplt_ps(LMS, zeros_sse); // stricly negative
  return _mm_or_ps(_mm_andnot_ps(negative, dt_Michaelis_Menten(LMS)),
                    _mm_and_ps(negative, -dt_Michaelis_Menten(-LMS)));
}

static inline __m128 dt_LMShdr_to_LMS(const __m128 LMShdr)
{
  /**
   * Returns dt_Michaelis_Menten_inv(LMS) where LMS >= 0
   * and - dt_Michaelis_Menten_inv(-LMS) where LMS < 0
   **/
  // See https://eng.aurelienpierre.com/2019/01/17/derivating-hdr-ipt-direct-and-inverse-transformations/#fixing_ipt-hdr
  /*
  const __m128 negative = _mm_cmplt_ps(LMShdr, zeros_sse); // stricly negative
  return _mm_or_ps(_mm_andnot_ps(negative, dt_Michaelis_Menten_inverse(LMShdr)),
                    _mm_and_ps(negative, -dt_Michaelis_Menten_inverse(-LMShdr)));
  */
  return  dt_Michaelis_Menten_inverse(LMShdr);
}

// LMS-HDR -> IPT-HDR
// https://eng.aurelienpierre.com/2019/01/17/derivating-hdr-ipt-direct-and-inverse-transformations/#fixing_ipt-hdr
/**
 * [[ 0.4     0.4     0.2   ]
    [ 4.455  -4.851   0.396 ]
    [ 0.8056  0.3572 -1.1628]]
 * */
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
/**
 * [[ 1.          0.09756893  0.20522643]
    [ 1.         -0.11387649  0.13321716]
    [ 1.          0.03261511 -0.67688718]]

*/
#define ipt_to_lms_0_sse _mm_setr_ps( 1.00000000f,  1.00000000f, 1.00000000f, 0.0f)
#define ipt_to_lms_1_sse _mm_setr_ps( 0.09756893f, -0.11387649f, 0.03261511f, 0.0f)
#define ipt_to_lms_2_sse _mm_setr_ps( 0.20522643f,  0.13321716f,-0.67688718f, 0.0f)

static inline __m128 dt_IPThdr_to_LMShdr_sse2(const __m128 IPThdr)
{
  __m128 lmshdr
      = _mm_shuffle_ps(IPThdr, IPThdr, _MM_SHUFFLE(0, 0, 0, 0)) +
        ipt_to_lms_1_sse * _mm_shuffle_ps(IPThdr, IPThdr, _MM_SHUFFLE(1, 1, 1, 1)) +
        ipt_to_lms_2_sse * _mm_shuffle_ps(IPThdr, IPThdr, _MM_SHUFFLE(2, 2, 2, 2));
  return lmshdr;
}

static inline __m128 dt_XYZ_to_IPThdr_sse2(const __m128 XYZ)
{
  return dt_LMShdr_to_IPThdr_sse2(dt_LMS_to_LMShdr(dt_XYZ_to_LMS_sse2(XYZ)));
}

static inline __m128 dt_IPThdr_to_XYZ_sse2(const __m128 IPT)
{
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
#endif



static inline void dt_Lab_to_Lch(const float *Lab, float *Lch)
{
  Lch[0] = Lab[0];
  Lch[1] = powf((Lab[1] * Lab[1] + Lab[2] * Lab[2]), 0.5f);
  Lch[2] = atan2f(Lab[2], Lab[1]);
}

static inline void dt_Lch_to_Lab(const float *Lch, float *Lab)
{
  Lab[0] = Lch[0];
  Lab[1] = cosf(Lch[2]) * Lch[1];
  Lab[2] = sinf(Lch[2]) * Lch[1];
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
