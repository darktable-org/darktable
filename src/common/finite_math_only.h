/*
    This file is part of darktable,
    Copyright (C) 2022 darktable developers.

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

#include "common/dttypes.h"

#if defined(__GNUC__)
/* Use these pragmas for sections of code which do or do not comply
 * with finite-math-only optimizations. Note that code can't be inlined
 * if the caller code has different flags than the callee.
 *
 * Use DT_BEGIN_FINITE_MATH_ONLY in *.c files (e.g. IOPs) that are meant
 * to handle infinities manually. isnan() and isfinite() are not available.
 * Prefer putting it in the beginning of the *.c file (before includes) so
 * that the included functions get the same optimizations and can be inlined.
 *
 * Use DT_BEGIN_NO_FINITE_MATH_ONLY in *.h files where a section of the code
 * needs to e.g. use isnan() or isfinite() or doesn't otherwise guarantee not
 * producing NaNs or infinities. It can be used like this:
 *
 *   DT_BEGIN_NO_FINITE_MATH_ONLY
 *   static float some_helper_function_in_header(const float x)
 *   {
 *     if(isnan(x))
 *     {
 *       return 0.f;
 *     }
 *     return x * x;
 *   }
 *   DT_END_OPTIMIZATIONS
 */
#define DT_BEGIN_FINITE_MATH_ONLY \
  _DT_Pragma(GCC push_options) \
  _DT_Pragma(GCC optimize ("finite-math-only"))
#define DT_BEGIN_NO_FINITE_MATH_ONLY \
  _DT_Pragma(GCC push_options) \
  _DT_Pragma(GCC optimize ("no-finite-math-only"))
#define DT_END_OPTIMIZATIONS \
  _DT_Pragma(GCC pop_options)
#else
#define DT_BEGIN_FINITE_MATH_ONLY
#define DT_BEGIN_NO_FINITE_MATH_ONLY
#define DT_END_OPTIMIZATIONS
#endif
