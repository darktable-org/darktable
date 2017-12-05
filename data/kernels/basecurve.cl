/*
    This file is part of darktable,
    copyright (c) 2016 johannes hanika.
    copyright (c) 2016 Ulrich Pegelow.

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

float
lookup_unbounded(read_only image2d_t lut, const float x, global const float *a)
{
  // in case the tone curve is marked as linear, return the fast
  // path to linear unbounded (does not clip x at 1)
  if(a[0] >= 0.0f)
  {
    if(x < 1.0f)
    {
      const int xi = clamp((int)(x * 0x10000ul), 0, 0xffff);
      const int2 p = (int2)((xi & 0xff), (xi >> 8));
      return read_imagef(lut, sampleri, p).x;
    }
    else return a[1] * native_powr(x*a[0], a[2]);
  }
  else return x;
}

/* we use this exp approximation to maintain full identity with cpu path */
float 
fast_expf(const float x)
{
  // meant for the range [-100.0f, 0.0f]. largest error ~ -0.06 at 0.0f.
  // will get _a_lot_ worse for x > 0.0f (9000 at 10.0f)..
  const int i1 = 0x3f800000u;
  // e^x, the comment would be 2^x
  const int i2 = 0x402DF854u;//0x40000000u;
  // const int k = CLAMPS(i1 + x * (i2 - i1), 0x0u, 0x7fffffffu);
  // without max clamping (doesn't work for large x, but is faster):
  const int k0 = i1 + x * (i2 - i1);
  const int k = k0 > 0 ? k0 : 0;
  const float f = *(const float *)&k;
  return f;
}

kernel void
basecurve_lut(read_only image2d_t in, write_only image2d_t out, const int width, const int height,
              read_only image2d_t table, global float *a)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 pixel = read_imagef(in, sampleri, (int2)(x, y));
  // use lut or extrapolation:
  pixel.x = lookup_unbounded(table, pixel.x, a);
  pixel.y = lookup_unbounded(table, pixel.y, a);
  pixel.z = lookup_unbounded(table, pixel.z, a);
  write_imagef (out, (int2)(x, y), pixel);
}


kernel void
basecurve_zero(write_only image2d_t out, const int width, const int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  write_imagef (out, (int2)(x, y), (float4)0.0f);
}

kernel void
basecurve_ev_lut(read_only image2d_t in, write_only image2d_t out, const int width, const int height,
                   const float ev, read_only image2d_t table, global float *a)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 pixel = read_imagef(in, sampleri, (int2)(x, y));
  
  // apply ev multiplier and use lut or extrapolation:
  pixel.x = lookup_unbounded(table, ev * pixel.x, a);
  pixel.y = lookup_unbounded(table, ev * pixel.y, a);
  pixel.z = lookup_unbounded(table, ev * pixel.z, a);
  write_imagef (out, (int2)(x, y), pixel);
}

kernel void
basecurve_compute_features(read_only image2d_t in, write_only image2d_t out, const int width, const int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 value = read_imagef(in, sampleri, (int2)(x, y));
  
  const float ma = max(value.x, max(value.y, value.z));
  const float mi = min(value.x, min(value.y, value.z));
  
  const float sat = 0.1f + 0.1f * (ma - mi) / max(1.0e-4f, ma);
  value.w = sat;

  const float c = 0.54f;
  
  float v = fabs(value.x - c);
  v = max(fabs(value.y - c), v);
  v = max(fabs(value.z - c), v);

  const float var = 0.5f;
  const float e = 0.2f + fast_expf(-v * v / (var * var));

  value.w *= e;
  
  write_imagef (out, (int2)(x, y), value);
}

constant float gw[5] = { 1.0f/16.0f, 4.0f/16.0f, 6.0f/16.0f, 4.0f/16.0f, 1.0f/16.0f };

kernel void 
basecurve_blur_h(read_only image2d_t in, write_only image2d_t out,
                 const int width, const int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  const int rad = 2;
  constant float *w = gw + rad;

  float4 sum = (float4)0.0f;

  for (int i = -rad; i <= rad; i++)
  {
    const int xx = min(max(-x - i, x + i), width - (x + i - width + 1));
    sum += read_imagef(in, sampleri, (int2)(xx, y)) * w[i];
  }

  write_imagef (out, (int2)(x, y), sum);
}


kernel void 
basecurve_blur_v(read_only image2d_t in, write_only image2d_t out, 
                 const int width, const int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);


  if(x >= width || y >= height) return;

  const int rad = 2;
  constant float *w = gw + rad;

  float4 sum = (float4)0.0f;

  for (int i = -rad; i <= rad; i++)
  {
    const int yy = min(max(-y - i, y + i), height - (y + i - height + 1));
    sum += read_imagef(in, sampleri, (int2)(x, yy)) * w[i];
  }

  write_imagef (out, (int2)(x, y), sum);
}

kernel void
basecurve_expand(read_only image2d_t in, write_only image2d_t out, const int width, const int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  // fill numbers in even pixels, zero odd ones
  float4 pixel = (x % 2 == 0 && y % 2 == 0) ? 4.0f * read_imagef(in, sampleri, (int2)(x / 2, y / 2)) : (float4)0.0f;
  
  write_imagef (out, (int2)(x, y), pixel);
}

kernel void
basecurve_reduce(read_only image2d_t in, write_only image2d_t out, const int width, const int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 pixel = read_imagef(in, sampleri, (int2)(2 * x, 2 * y));
  
  write_imagef (out, (int2)(x, y), pixel);
}

kernel void
basecurve_detail(read_only image2d_t in, read_only image2d_t det, write_only image2d_t out, const int width, const int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 input = read_imagef(in, sampleri, (int2)(x, y));
  float4 detail = read_imagef(det, sampleri, (int2)(x, y));
  
  write_imagef (out, (int2)(x, y), input - detail);
}

kernel void
basecurve_adjust_features(read_only image2d_t in, read_only image2d_t det, write_only image2d_t out, const int width, const int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 input = read_imagef(in, sampleri, (int2)(x, y));
  float4 detail = read_imagef(det, sampleri, (int2)(x, y));
  
  input.w *= 0.1f + sqrt(detail.x * detail.x + detail.y * detail.y + detail.z * detail.z);
  
  write_imagef (out, (int2)(x, y), input);
}

kernel void
basecurve_blend_gaussian(read_only image2d_t in, read_only image2d_t col, write_only image2d_t out, const int width, const int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 comb = read_imagef(in, sampleri, (int2)(x, y));
  float4 collect = read_imagef(col, sampleri, (int2)(x, y));
  
  comb.xyz += collect.xyz * collect.w;
  comb.w += collect.w;
    
  write_imagef (out, (int2)(x, y), comb);
}

kernel void
basecurve_blend_laplacian(read_only image2d_t in, read_only image2d_t col, read_only image2d_t tmp, write_only image2d_t out, 
                          const int width, const int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 comb = read_imagef(in, sampleri, (int2)(x, y));
  float4 collect = read_imagef(col, sampleri, (int2)(x, y));
  float4 temp = read_imagef(tmp, sampleri, (int2)(x, y));
  
  comb.xyz += (collect.xyz - temp.xyz) * collect.w;
  comb.w += collect.w;
    
  write_imagef (out, (int2)(x, y), comb);
}

kernel void
basecurve_normalize(read_only image2d_t in, write_only image2d_t out, const int width, const int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 comb = read_imagef(in, sampleri, (int2)(x, y));
  
  comb.xyz /= (comb.w > 1.0e-8f) ? comb.w : 1.0f;
  
  write_imagef (out, (int2)(x, y), comb);
}

kernel void
basecurve_reconstruct(read_only image2d_t in, read_only image2d_t tmp, write_only image2d_t out, const int width, const int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 comb = read_imagef(in, sampleri, (int2)(x, y));
  float4 temp = read_imagef(tmp, sampleri, (int2)(x, y));
  
  comb += temp;
  
  write_imagef (out, (int2)(x, y), comb);
}

kernel void
basecurve_finalize(read_only image2d_t in, read_only image2d_t comb, write_only image2d_t out, const int width, const int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 pixel = read_imagef(comb, sampleri, (int2)(x, y));
  pixel.w = read_imagef(in, sampleri, (int2)(x, y)).w;
  
  write_imagef (out, (int2)(x, y), pixel);
}



