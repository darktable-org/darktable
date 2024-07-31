/*
   This file is part of darktable,
   Copyright (C) 2020-2023 - darktable developers.

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


#include <glib.h>            // for inline
#include <math.h>            // for log, logf, powf

#pragma once


#if defined(_OPENMP) && !defined(_WIN32) && (!defined(__GNUC__) || __GNUC__ >= 12)

#ifndef __GNUC__  // GCC 12 compiles but fails at runtime due to missing library function
DT_OMP_DECLARE_SIMD()
extern float fmaxf(const float x, const float y);
#endif

#ifndef __GNUC__  // GCC 12 compiles but fails at runtime due to missing library function
DT_OMP_DECLARE_SIMD()
extern float fminf(const float x, const float y);
#endif

DT_OMP_DECLARE_SIMD()
extern float fabsf(const float x);

DT_OMP_DECLARE_SIMD()
extern float powf(const float x, const float y);

DT_OMP_DECLARE_SIMD()
extern float sqrtf(const float x);

DT_OMP_DECLARE_SIMD()
extern float cbrtf(const float x);

DT_OMP_DECLARE_SIMD()
extern float log2f(const float x);

DT_OMP_DECLARE_SIMD()
extern float exp2f(const float x);

DT_OMP_DECLARE_SIMD()
extern float log10f(const float x);

DT_OMP_DECLARE_SIMD()
extern float expf(const float x);

DT_OMP_DECLARE_SIMD()
extern float logf(const float x);

#endif

/* Bring our own optimized maths functions because Clang makes dumb shit */

DT_OMP_DECLARE_SIMD()
static inline float fast_exp10f(const float x)
{
  // we use the property : 10^x = exp(log(10) * x) = 2^(log(10) * x / log(2))
  // max relative error over x = [0; 4] is 1.5617955706227326e-15
  return exp2f(3.3219280948873626f * x);
}

// Since we are at it, write an optimized expf
DT_OMP_DECLARE_SIMD()
static inline float fast_expf(const float x)
{
  // we use the property : exp(x) = 2^(x / log(2))
  // max relative error over x = [0; 4] is 5.246203046472202e-16
  return exp2f(1.4426950408889634f * x);
}


DT_OMP_DECLARE_SIMD(aligned(vector:16))
static inline float v_maxf(const float vector[3])
{
  // Find the max over an RGB vector
  return fmaxf(fmaxf(vector[0], vector[1]), vector[2]);
}


DT_OMP_DECLARE_SIMD(aligned(vector:16))
static inline float v_minf(const float vector[3])
{
  // Find the min over an RGB vector
  return fminf(fminf(vector[0], vector[1]), vector[2]);
}

DT_OMP_DECLARE_SIMD(aligned(vector:16))
static inline float v_sumf(const float vector[3])
{
  return vector[0] + vector[1] + vector[2];
}


DT_OMP_DECLARE_SIMD()
static inline float fmaxabsf(const float a, const float b)
{
  // Find the max in absolute value and return it with its sign
  return (fabsf(a) > fabsf(b)) ? a : (dt_isnan(b)) ? 0.f : b;
}


DT_OMP_DECLARE_SIMD()
static inline float fminabsf(const float a, const float b)
{
  // Find the min in absolute value and return it with its sign
  return (fabsf(a) < fabsf(b)) ? a : (dt_isnan(b)) ? 0.f : b;
}


DT_OMP_DECLARE_SIMD()
static inline float clamp_simd(const float x)
{
  return fminf(fmaxf(x, 0.0f), 1.0f);
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

