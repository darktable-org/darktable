/*
    This file is part of darktable,
    copyright (c) 2015

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

#include "common/colorspaces.h"
#include "common/sse.h"
#include <xmmintrin.h>

/* These functions are derived from the non-SSE versions in colorspaces.c */

static inline __m128 lab_f_inv_m(const __m128 x)
{
  const __m128 epsilon = _mm_set1_ps(0.20689655172413796f); // cbrtf(216.0f/24389.0f);
  const __m128 kappa_rcp_x16 = _mm_set1_ps(16.0f * 27.0f / 24389.0f);
  const __m128 kappa_rcp_x116 = _mm_set1_ps(116.0f * 27.0f / 24389.0f);

  // x > epsilon
  const __m128 res_big = _mm_mul_ps(_mm_mul_ps(x, x), x);
  // x <= epsilon
  const __m128 res_small = _mm_sub_ps(_mm_mul_ps(kappa_rcp_x116, x), kappa_rcp_x16);

  // blend results according to whether each component is > epsilon or not
  const __m128 mask = _mm_cmpgt_ps(x, epsilon);
  return _mm_or_ps(_mm_and_ps(mask, res_big), _mm_andnot_ps(mask, res_small));
}

__m128 dt_Lab_to_XYZ_SSE(const __m128 Lab)
{
  const __m128 d50 = _mm_set_ps(0.0f, 0.8249f, 1.0f, 0.9642f);
  const __m128 coef = _mm_set_ps(0.0f, -1.0f / 200.0f, 1.0f / 116.0f, 1.0f / 500.0f);
  const __m128 offset = _mm_set1_ps(0.137931034f);

  // last component ins shuffle taken from 1st component of Lab to make sure it is not nan, so it will become
  // 0.0f in f
  const __m128 f = _mm_mul_ps(_mm_shuffle_ps(Lab, Lab, _MM_SHUFFLE(0, 2, 0, 1)), coef);

  return _mm_mul_ps(
      d50, lab_f_inv_m(_mm_add_ps(_mm_add_ps(f, _mm_shuffle_ps(f, f, _MM_SHUFFLE(1, 1, 3, 1))), offset)));
}

static inline __m128 lab_f_m(const __m128 x)
{
  const __m128 epsilon = _mm_set1_ps(216.0f / 24389.0f);
  const __m128 kappa = _mm_set1_ps(24389.0f / 27.0f);

  // calculate as if x > epsilon : result = cbrtf(x)
  // approximate cbrtf(x):
  const __m128 a = _mm_castsi128_ps(
      _mm_add_epi32(_mm_cvtps_epi32(_mm_div_ps(_mm_cvtepi32_ps(_mm_castps_si128(x)), _mm_set1_ps(3.0f))),
                    _mm_set1_epi32(709921077)));
  const __m128 a3 = _mm_mul_ps(_mm_mul_ps(a, a), a);
  const __m128 res_big
      = _mm_div_ps(_mm_mul_ps(a, _mm_add_ps(a3, _mm_add_ps(x, x))), _mm_add_ps(_mm_add_ps(a3, a3), x));

  // calculate as if x <= epsilon : result = (kappa*x+16)/116
  const __m128 res_small
      = _mm_div_ps(_mm_add_ps(_mm_mul_ps(kappa, x), _mm_set1_ps(16.0f)), _mm_set1_ps(116.0f));

  // blend results according to whether each component is > epsilon or not
  const __m128 mask = _mm_cmpgt_ps(x, epsilon);
  return _mm_or_ps(_mm_and_ps(mask, res_big), _mm_andnot_ps(mask, res_small));
}

__m128 dt_XYZ_to_Lab_SSE(const __m128 XYZ)
{
  const __m128 d50_inv = _mm_set_ps(0.0f, 1.0f / 0.8249f, 1.0f, 1.0f / 0.9642f);
  const __m128 coef = _mm_set_ps(0.0f, 200.0f, 500.0f, 116.0f);
  const __m128 f = lab_f_m(_mm_mul_ps(XYZ, d50_inv));
  // because d50_inv.z is 0.0f, lab_f(0) == 16/116, so Lab[0] = 116*f[0] - 16 equal to 116*(f[0]-f[3])
  return _mm_mul_ps(coef, _mm_sub_ps(_mm_shuffle_ps(f, f, _MM_SHUFFLE(3, 1, 0, 1)),
                                     _mm_shuffle_ps(f, f, _MM_SHUFFLE(3, 2, 1, 3))));
}

// see http://www.brucelindbloom.com/Eqn_RGB_XYZ_Matrix.html for the transformation matrices
__m128 dt_XYZ_to_sRGB_SSE(__m128 XYZ)
{
  // XYZ -> sRGB matrix, D65
  static const __m128 xyz_to_srgb[3] = {
    { 3.1338561, -0.9787684, 0.0719453, 0 },
    { -1.6168667, 1.9161415, -0.2289914, 0 },
    { -0.4906146, 0.0334540, 1.4052427, 0 }
    //     {3.2404542, -1.5371385, -0.4985314},
    //     {-0.9692660,  1.8760108,  0.0415560},
    //     {0.0556434, -0.2040259,  1.0572252}
  };
  __m128 rgb
    = xyz_to_srgb[0] * _mm_shuffle_ps(XYZ, XYZ, _MM_SHUFFLE(0, 0, 0, 0))
    + xyz_to_srgb[1] * _mm_shuffle_ps(XYZ, XYZ, _MM_SHUFFLE(1, 1, 1, 1))
    + xyz_to_srgb[2] * _mm_shuffle_ps(XYZ, XYZ, _MM_SHUFFLE(2, 2, 2, 2));

  // linear sRGB -> gamma corrected sRGB
  __m128 mask = _mm_cmple_ps(rgb, _mm_set1_ps(0.0031308));
  __m128 rgb0 = _mm_set1_ps(12.92) * rgb;
  __m128 rgb1 = _mm_set1_ps(1.0 + 0.055) * _mm_pow_ps1(rgb, 1.0 / 2.4) - _mm_set1_ps(0.055);
  return _mm_or_ps(_mm_and_ps(mask, rgb0), _mm_andnot_ps(mask, rgb1));
}

__m128 dt_sRGB_to_XYZ_SSE(__m128 rgb)
{
  // sRGB -> XYZ matrix, D65
  static const __m128 srgb_to_xyz[3] = {
    { 0.4360747, 0.2225045, 0.0139322, 0 },
    { 0.3850649, 0.7168786, 0.0971045, 0 },
    { 0.1430804, 0.0606169, 0.7141733, 0 }
    //     {0.4124564, 0.3575761, 0.1804375},
    //     {0.2126729, 0.7151522, 0.0721750},
    //     {0.0193339, 0.1191920, 0.9503041}
  };

  // gamma corrected sRGB -> linear sRGB
  __m128 mask = _mm_cmple_ps(rgb, _mm_set1_ps(0.04045));
  __m128 rgb0 = rgb / _mm_set1_ps(12.92);
  __m128 rgb1 = _mm_pow_ps1((rgb + _mm_set1_ps(0.055)) / _mm_set1_ps(1 + 0.055), 2.4);
  rgb = _mm_or_ps(_mm_and_ps(mask, rgb0), _mm_andnot_ps(mask, rgb1));

  __m128 XYZ
    = srgb_to_xyz[0] * _mm_shuffle_ps(rgb, rgb, _MM_SHUFFLE(0, 0, 0, 0))
    + srgb_to_xyz[1] * _mm_shuffle_ps(rgb, rgb, _MM_SHUFFLE(1, 1, 1, 1))
    + srgb_to_xyz[2] * _mm_shuffle_ps(rgb, rgb, _MM_SHUFFLE(2, 2, 2, 2));
  return XYZ;
}
