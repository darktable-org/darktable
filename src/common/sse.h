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
#ifndef DT_SSE_H
#define DT_SSE_H

#include <xmmintrin.h>

extern __m128 _mm_exp2_ps(__m128 x);
extern __m128 _mm_log2_ps(__m128 x);

static inline __m128 _mm_pow_ps(__m128 x, __m128 y)
{
  return _mm_exp2_ps(_mm_log2_ps(x) * y);
}

static inline __m128 _mm_pow_ps1(__m128 x, float y)
{
  return _mm_exp2_ps(_mm_log2_ps(x) * y);
}

#endif
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
