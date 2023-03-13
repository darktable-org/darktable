/*
    This file is part of darktable,
    Copyright (C) 2017-2020 darktable developers.
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
#pragma once

#include <xmmintrin.h>

#include "common/darktable.h"


/**
 * Fast SSE2 implementation of special math functions.
 */

#define POLY0(x, c0) _mm_set1_ps(c0)
#define POLY1(x, c0, c1) _mm_add_ps(_mm_mul_ps(POLY0(x, c1), x), _mm_set1_ps(c0))
#define POLY2(x, c0, c1, c2) _mm_add_ps(_mm_mul_ps(POLY1(x, c1, c2), x), _mm_set1_ps(c0))
#define POLY3(x, c0, c1, c2, c3) _mm_add_ps(_mm_mul_ps(POLY2(x, c1, c2, c3), x), _mm_set1_ps(c0))
#define POLY4(x, c0, c1, c2, c3, c4) _mm_add_ps(_mm_mul_ps(POLY3(x, c1, c2, c3, c4), x), _mm_set1_ps(c0))
#define POLY5(x, c0, c1, c2, c3, c4, c5) _mm_add_ps(_mm_mul_ps(POLY4(x, c1, c2, c3, c4, c5), x), _mm_set1_ps(c0))

#define EXP_POLY_DEGREE 4
#define LOG_POLY_DEGREE 5

/**
 * See http://www.devmaster.net/forums/showthread.php?p=43580
 */
static inline __m128 _mm_exp2_ps(__m128 x)
{
  __m128i ipart;
  __m128 fpart, expipart, expfpart;

  /* clamp the exponent to the suppported range */
  x = _mm_min_ps(x, _mm_set1_ps(129.00000f));
  x = _mm_max_ps(x, _mm_set1_ps(-126.99999f));

  /* ipart = int(x - 0.5) */
  ipart = _mm_cvtps_epi32(x - _mm_set1_ps(0.5f));

  /* fpart = x - ipart */
  fpart = x - _mm_cvtepi32_ps(ipart);

  /* expipart = (float) (1 << ipart) */
  expipart = _mm_castsi128_ps(_mm_slli_epi32(_mm_add_epi32(ipart, _mm_set1_epi32(127)), 23));

/* minimax polynomial fit of 2**x, in range [-0.5, 0.5[ */
#if EXP_POLY_DEGREE == 5
  expfpart
      = POLY5(fpart, 9.9999994e-1f, 6.9315308e-1f, 2.4015361e-1f, 5.5826318e-2f, 8.9893397e-3f, 1.8775767e-3f);
#elif EXP_POLY_DEGREE == 4
  expfpart = POLY4(fpart, 1.0000026f, 6.9300383e-1f, 2.4144275e-1f, 5.2011464e-2f, 1.3534167e-2f);
#elif EXP_POLY_DEGREE == 3
  expfpart = POLY3(fpart, 9.9992520e-1f, 6.9583356e-1f, 2.2606716e-1f, 7.8024521e-2f);
#elif EXP_POLY_DEGREE == 2
  expfpart = POLY2(fpart, 1.0017247f, 6.5763628e-1f, 3.3718944e-1f);
#else
#error
#endif

  return _mm_mul_ps(expipart, expfpart);
}


/**
 * See http://www.devmaster.net/forums/showthread.php?p=43580
 */
static inline __m128 _mm_log2_ps(__m128 x)
{
  __m128i expmask = _mm_set1_epi32(0x7f800000);
  __m128i mantmask = _mm_set1_epi32(0x007fffff);
  __m128 one = _mm_set1_ps(1.0f);

  __m128i i = _mm_castps_si128(x);

  /* exp = (float) exponent(x) */
  __m128 exp = _mm_cvtepi32_ps(_mm_sub_epi32(_mm_srli_epi32(_mm_and_si128(i, expmask), 23), _mm_set1_epi32(127)));

  /* mant = (float) mantissa(x) */
  __m128 mant = _mm_or_ps(_mm_castsi128_ps(_mm_and_si128(i, mantmask)), one);

  __m128 logmant;

/* Minimax polynomial fit of log2(x)/(x - 1), for x in range [1, 2[
 * These coefficients can be generate with
 * http://www.boost.org/doc/libs/1_36_0/libs/math/doc/sf_and_dist/html/math_toolkit/toolkit/internals2/minimax.html
 */
#if LOG_POLY_DEGREE == 6
  logmant = POLY5(mant, 3.11578814719469302614f, -3.32419399085241980044f, 2.59883907202499966007f,
                  -1.23152682416275988241f, 0.318212422185251071475f, -0.0344359067839062357313f);
#elif LOG_POLY_DEGREE == 5
  logmant = POLY4(mant, 2.8882704548164776201f, -2.52074962577807006663f, 1.48116647521213171641f,
                  -0.465725644288844778798f, 0.0596515482674574969533f);
#elif LOG_POLY_DEGREE == 4
  logmant = POLY3(mant, 2.61761038894603480148f, -1.75647175389045657003f, 0.688243882994381274313f,
                  -0.107254423828329604454f);
#elif LOG_POLY_DEGREE == 3
  logmant = POLY2(mant, 2.28330284476918490682f, -1.04913055217340124191f, 0.204446009836232697516f);
#else
#error
#endif

  /* This effectively increases the polynomial degree by one, but ensures that log2(1) == 0*/
  return (logmant * (mant - one)) + exp;
}

static inline __m128 _mm_pow_ps(__m128 x, __m128 y)
{
  return _mm_exp2_ps(_mm_mul_ps(_mm_log2_ps(x), y));
}

static inline __m128 _mm_pow_ps1(__m128 x, float y)
{
  return _mm_exp2_ps(_mm_mul_ps(_mm_log2_ps(x), _mm_set1_ps(y)));
}


/**
 * Allow access of the content of a SSE vector
 **/

static inline float _mm_vectorGetByIndex( __m128 V, unsigned int i)
{
  union {
    __m128 v;
    float a[4];
  } converter;

  converter.v = V;
  return converter.a[i];
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

