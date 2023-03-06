/*
    This file is part of darktable,
    Copyright (C) 2012-2020 darktable developers.

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

constant sampler_t sampleri =  CLK_NORMALIZED_COORDS_FALSE | CLK_ADDRESS_CLAMP_TO_EDGE | CLK_FILTER_NEAREST;

constant sampler_t samplerf =  CLK_NORMALIZED_COORDS_FALSE | CLK_ADDRESS_CLAMP_TO_EDGE | CLK_FILTER_LINEAR;

constant sampler_t samplerc =  CLK_NORMALIZED_COORDS_FALSE | CLK_ADDRESS_CLAMP         | CLK_FILTER_NEAREST;

// sampler for when the bound checks are already done manually
constant sampler_t samplerA = CLK_NORMALIZED_COORDS_FALSE | CLK_ADDRESS_NONE            | CLK_FILTER_NEAREST;


#ifndef M_PI_F
#define M_PI_F           3.14159265358979323846  // should be defined by the OpenCL compiler acc. to standard
#endif

#define ICLAMP(a, mn, mx) ((a) < (mn) ? (mn) : ((a) > (mx) ? (mx) : (a)))
#define RED 0
#define GREEN 1
#define BLUE 2
#define ALPHA 3


static inline int
FC(const int row, const int col, const unsigned int filters)
{
  return filters >> ((((row) << 1 & 14) + ((col) & 1)) << 1) & 3;
}


static inline int
FCxtrans(const int row, const int col, global const unsigned char (*const xtrans)[6])
{
  return xtrans[row % 6][col % 6];
}


static inline float
dt_fast_hypot(const float x, const float y)
{
  return native_sqrt(x * x + y * y);
}

/* we use this exp approximation to maintain full identity with cpu path */
static inline float
dt_fast_expf(const float x)
{
  // meant for the range [-100.0f, 0.0f]. largest error ~ -0.06 at 0.0f.
  // will get _a_lot_ worse for x > 0.0f (9000 at 10.0f)..
  const int i1 = 0x3f800000u;
  // e^x, the comment would be 2^x
  const int i2 = 0x402DF854u;//0x40000000u;
  // const int k = CLAMPS(i1 + x * (i2 - i1), 0x0u, 0x7fffffffu);
  // without max clamping (doesn't work for large x, but is faster):
  const int k0 = i1 + x * (i2 - i1);
  union {
      float f;
      int k;
  } u;
  u.k = k0 > 0 ? k0 : 0;
  return u.f;
}

// Allow the compiler to convert a * b + c to fused multiply-add to use hardware acceleration
// on compatible platforms
#pragma OPENCL FP_CONTRACT ON
