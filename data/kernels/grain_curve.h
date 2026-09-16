/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

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

/* The grain sampler's density curve lookup, compiled by both the host and the
 * OpenCL kernels.
 *
 * Separate from grain.h because these take pointers into a table, which that
 * header excludes by design. Host and device results must agree bit-for-bit:
 * what these return is the density handed to grain_layer_particle, and
 * grain_poisson's accept/reject loop turns a one-ULP difference there into a
 * whole-integer grain count difference.
 *
 * Both functions are plain scalar arithmetic over correctly-rounded
 * operations, so they reproduce bit-for-bit given the same table.
 */

#pragma once

/* Dialect is DECLARED by the includer, as in grain.h: a .cl defines
   GRAIN_CL before including this, the host leaves it unset. The predefines
   serve only to catch an includer that forgot. */
#if (defined(__OPENCL_VERSION__) || defined(__OPENCL_C_VERSION__)) && !defined(GRAIN_CL)
#error "grain_curve.h: OpenCL translation unit must '#define GRAIN_CL 1' before including this header"
#endif
#if defined(GRAIN_CL) && !defined(__OPENCL_VERSION__) && !defined(__OPENCL_C_VERSION__)
#error "grain_curve.h: GRAIN_CL is set but this is not an OpenCL translation unit"
#endif

/* The tables live in device global memory on the OpenCL side and in ordinary
   host memory otherwise, so the address space qualifier is part of the
   signature and has to vary with the dialect. */
#ifdef GRAIN_CL
#define GRAIN_GMEM __global
#define fabsf fabs
#else
#include <math.h>
#define GRAIN_GMEM
#endif

#ifndef GRAIN_INLINE
#define GRAIN_INLINE static inline
#endif

/* Position along a monotonic curve at which it reaches `target`, as a
   fractional index. Binary search for the bracketing pair, then linear
   interpolation between them; `stride` steps over the interleaved layout the
   caller's table uses. Handles both directions because a density curve may
   be fitted either way round. */
GRAIN_INLINE float grain_curve_inverse(GRAIN_GMEM const float *arr,
                                            int n,
                                            int stride,
                                            float target)
{
  const int increasing = arr[(n - 1) * stride] >= arr[0];
  int lo = 0, hi = n - 1;
  while(hi - lo > 1)
  {
    const int mid = (lo + hi) / 2;
    const float v = arr[mid * stride];
    if((increasing && v <= target) || (!increasing && v >= target)) lo = mid;
    else hi = mid;
  }
  const float v0 = arr[lo * stride], v1 = arr[hi * stride];
  const float denom = v1 - v0;
  float frac = (fabsf(denom) > 1e-9f) ? (target - v0) / denom : 0.0f;
  if(frac < 0.0f) frac = 0.0f;
  if(frac > 1.0f) frac = 1.0f;
  return (float)lo + frac;
}

/* Value of a curve at the fractional index grain_curve_inverse returns. */
GRAIN_INLINE float grain_curve_sample(GRAIN_GMEM const float *arr,
                                           int n,
                                           int stride,
                                           float pos)
{
  int i0 = (int)pos;
  if(i0 < 0) i0 = 0;
  if(i0 > n - 2) i0 = (n - 2 < 0) ? 0 : n - 2;
  const float frac = pos - (float)i0;
  return arr[i0 * stride] * (1.0f - frac) + arr[(i0 + 1) * stride] * frac;
}

#ifdef GRAIN_CL
#undef fabsf
#endif
