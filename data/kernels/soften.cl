/*
    This file is part of darktable,
    copyright (c) 2012 ulrich pegelow.

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
#include "colorspace.h"


/* first step for soften module: generate overexposed image */
kernel void
soften_overexposed(read_only image2d_t in, write_only image2d_t out, const int width, const int height,
                  const float saturation, const float brightness)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 pixel = read_imagef(in, sampleri, (int2)(x, y));

  float4 hsl = RGB_2_HSL(pixel);

  hsl.y = clamp(hsl.y * saturation, 0.0f, 1.0f);
  hsl.z = clamp(hsl.z * brightness, 0.0f, 1.0f);

  pixel = HSL_2_RGB(hsl);

  write_imagef (out, (int2)(x, y), pixel);
}

/* horizontal gaussian blur */
kernel void 
soften_hblur(read_only image2d_t in, write_only image2d_t out, global const float *m, const int rad,
      const int width, const int height, const int blocksize, local float4 *buffer)
{
  const int lid = get_local_id(0);
  const int lsz = get_local_size(0);
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  float4 pixel = (float4)0.0f;

  /* read pixel and fill center part of buffer */
  pixel = read_imagef(in, sampleri, (int2)(x, y));
  buffer[rad + lid] = pixel;

  /* left wing of buffer */
  for(int n=0; n <= rad/lsz; n++)
  {
    const int l = mad24(n, lsz, lid + 1);
    if(l > rad) continue;
    const int xx = mad24((int)get_group_id(0), lsz, -l);
    buffer[rad - l] = read_imagef(in, sampleri, (int2)(xx, y));
  }
    
  /* right wing of buffer */
  for(int n=0; n <= rad/lsz; n++)
  {
    const int r = mad24(n, lsz, lsz - lid);
    if(r > rad) continue;
    const int xx = mad24((int)get_group_id(0), lsz, lsz - 1 + r);
    buffer[rad + lsz - 1 + r] = read_imagef(in, sampleri, (int2)(xx, y));
  }

  barrier(CLK_LOCAL_MEM_FENCE);

  if(x >= width || y >= height) return;

  buffer += lid + rad;
  m += rad;

  float4 sum = (float4)0.0f;

  for (int i=-rad; i<=rad; i++)
  {
    sum += buffer[i] * m[i];
  }

  pixel = sum;
  write_imagef (out, (int2)(x, y), pixel);
}


/* vertical gaussian blur */
kernel void 
soften_vblur(read_only image2d_t in, write_only image2d_t out, global const float *m, const int rad,
      const int width, const int height, const int blocksize, local float4 *buffer)
{
  const int lid = get_local_id(1);
  const int lsz = get_local_size(1);
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  float4 pixel = (float4)0.0f;

  /* read pixel and fill center part of buffer */
  pixel = read_imagef(in, sampleri, (int2)(x, y));
  buffer[rad + lid] = pixel;

  /* left wing of buffer */
  for(int n=0; n <= rad/lsz; n++)
  {
    const int l = mad24(n, lsz, lid + 1);
    if(l > rad) continue;
    const int yy = mad24((int)get_group_id(1), lsz, -l);
    buffer[rad - l] = read_imagef(in, sampleri, (int2)(x, yy));
  }
    
  /* right wing of buffer */
  for(int n=0; n <= rad/lsz; n++)
  {
    const int r = mad24(n, lsz, lsz - lid);
    if(r > rad) continue;
    const int yy = mad24((int)get_group_id(1), lsz, lsz - 1 + r);
    buffer[rad + lsz - 1 + r] = read_imagef(in, sampleri, (int2)(x, yy));
  }

  barrier(CLK_LOCAL_MEM_FENCE);

  if(x >= width || y >= height) return;

  buffer += lid + rad;
  m += rad;

  float4 sum = (float4)0.0f;

  for (int i=-rad; i<=rad; i++)
  {
    sum += buffer[i] * m[i];
  }

  pixel = sum;
  write_imagef (out, (int2)(x, y), pixel);
}




/* final step for soften module */
kernel void
soften_mix(read_only image2d_t in_a, read_only image2d_t in_b, write_only image2d_t out, const int width, const int height,
                  const float amount)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 original  = read_imagef(in_a, sampleri, (int2)(x, y));
  float4 processed = read_imagef(in_b, sampleri, (int2)(x, y));

  float4 pixel = original * (1.0f - amount) + clamp(processed, (float4)0.0f, (float4)1.0f) * amount;
  pixel.w = original.w;

  write_imagef (out, (int2)(x, y), pixel);
}


