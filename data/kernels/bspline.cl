/*
    This file is part of darktable,
    copyright (c) 2022 darktable developers.

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


#include "common.h"

// B spline filter
#define FSIZE 5
#define FSTART (FSIZE - 1) / 2


kernel void blur_2D_Bspline_vertical(read_only image2d_t in, write_only image2d_t out,
                                     const int width, const int height, const int mult)
{
  // À-trous B-spline interpolation/blur shifted by mult
  // Convolve B-spline filter over lines
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;

  const float4 filter[FSIZE] = { (float4)1.0f / 16.0f,
                                 (float4)4.0f / 16.0f,
                                 (float4)6.0f / 16.0f,
                                 (float4)4.0f / 16.0f,
                                 (float4)1.0f / 16.0f };

  float4 accumulator = (float4)0.f;

  #pragma unroll
  for(int jj = 0; jj < FSIZE; ++jj)
  {
    const int yy = mult * (jj - FSTART) + y;
    accumulator += filter[jj] * read_imagef(in, samplerA, (int2)(x, clamp(yy, 0, height - 1)));
  }

  write_imagef(out, (int2)(x, y), fmax(accumulator, 0.f));
}


kernel void blur_2D_Bspline_horizontal(read_only image2d_t in, write_only image2d_t out,
                                       const int width, const int height, const int mult)
{
  // À-trous B-spline interpolation/blur shifted by mult
  // Convolve B-spline filter over columns
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;

  const float4 filter[FSIZE] = { (float4)1.0f / 16.0f,
                                 (float4)4.0f / 16.0f,
                                 (float4)6.0f / 16.0f,
                                 (float4)4.0f / 16.0f,
                                 (float4)1.0f / 16.0f };

  float4 accumulator = (float4)0.f;

  #pragma unroll
  for(int ii = 0; ii < FSIZE; ++ii)
  {
    const int xx = mult * (ii - FSTART) + x;
    accumulator += filter[ii] * read_imagef(in, samplerA, (int2)(clamp(xx, 0, width - 1), y));
  }

  write_imagef(out, (int2)(x, y), fmax(accumulator, 0.f));
}


kernel void wavelets_detail_level(read_only image2d_t detail, read_only image2d_t LF,
                                  write_only image2d_t HF, const int width, const int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;

  const float4 d = read_imagef(detail, samplerA, (int2)(x, y));
  const float4 lf = read_imagef(LF, samplerA, (int2)(x, y));

  write_imagef(HF, (int2)(x, y), d - lf);
}
