/*
    This file is part of darktable,
    Copyright (C) 2009-2026 darktable developers.

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


float4
weight(const float4 c1, const float4 c2, const float sharpen)
{
  const float wc = dtcl_exp(-((c1.y - c2.y)*(c1.y - c2.y) + (c1.z - c2.z)*(c1.z - c2.z)) * sharpen);
  const float wl = dtcl_exp(- (c1.x - c2.x)*(c1.x - c2.x) * sharpen);
  return (float4)(wl, wc, wc, 1.0f);
}


__kernel void
eaw_decompose(__read_only image2d_t in,
              __write_only image2d_t coarse,
              __write_only image2d_t detail,
              const int width,
              const int height,
              const int scale,
              const float sharpen,
              global const float *filter)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  const int mult = 1<<scale;

  const float4 pixel = read_imagef(in, sampleri, (int2)(x, y));
  float4 sum = (float4)(0.0f);
  float4 wgt = (float4)(0.0f);
  for(int j=0;j<5;j++) for(int i=0;i<5;i++)
  {
    const int xx = mad24(mult, i - 2, x);
    const int yy = mad24(mult, j - 2, y);
    const int k  = mad24(j, 5, i);

    const float4 px = read_imagef(in, sampleri, (int2)(xx, yy));
    const float4 w = filter[k]*weight(pixel, px, sharpen);

    sum += w*px;
    wgt += w;
  }
  sum /= wgt;
  sum.w = pixel.w;

  write_imagef (detail, (int2)(x, y), pixel - sum);
  write_imagef (coarse, (int2)(x, y), sum);
}


__kernel void
eaw_synthesize(__write_only image2d_t out,
               __read_only image2d_t coarse,
               __read_only image2d_t detail,
               const int width,
               const int height,
               const float4 threshold,
               const float4 boost)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  const float4 c = read_imagef(coarse, sampleri, (int2)(x, y));
  const float4 d = read_imagef(detail, sampleri, (int2)(x, y));
  const float4 amount = copysign(fmax((float4)(0.0f), fabs(d) - threshold), d);
  float4 sum = c + boost*amount;
  sum.w = c.w;
  write_imagef (out, (int2)(x, y), sum);
}

__kernel void
eaw_zero(__write_only image2d_t out,
         const int width,
         const int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;
  write_imagef(out, (int2)(x, y), (float4)0.0f);
}

__kernel void
eaw_addbuffers(__write_only image2d_t out_out,
               __read_only image2d_t out_in,  
               __read_only image2d_t diff,
               const int width,
               const int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;

  const float4 cs = read_imagef(diff, sampleri, (int2)(x, y));
  const float4 o = read_imagef(out_in, sampleri, (int2)(x, y));
  write_imagef(out_out, (int2)(x, y), (cs + o));  
}