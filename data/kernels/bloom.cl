/*
    This file is part of darktable,
    copyright (c) 2014 ulrich pegelow.

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
#include "colorspace.cl"


/* first step for bloom module: get the thresholded lights into buffer */
kernel void
bloom_threshold(read_only image2d_t in, write_only image2d_t out, const int width, const int height,
                  const float scale, const float threshold)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 pixel = read_imagef(in, sampleri, (int2)(x, y));

  float L = pixel.x*scale;

  L = L > threshold ? L : 0.0f;

  write_imagef (out, (int2)(x, y), L);
}


/* horizontal box blur */
kernel void 
bloom_hblur(read_only image2d_t in, write_only image2d_t out, const int rad,
      const int width, const int height, const int blocksize, local float *buffer)
{
  const int lid = get_local_id(0);
  const int lsz = get_local_size(0);
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  float pixel = 0.0f;

  /* read pixel and fill center part of buffer */
  pixel = read_imagef(in, sampleri, (int2)(x, y)).x;
  buffer[rad + lid] = pixel;

  /* left wing of buffer */
  for(int n=0; n <= rad/lsz; n++)
  {
    const int l = mad24(n, lsz, lid + 1);
    if(l > rad) continue;
    const int xx = mad24((int)get_group_id(0), lsz, -l);
    buffer[rad - l] = read_imagef(in, sampleri, (int2)(xx, y)).x;
  }
    
  /* right wing of buffer */
  for(int n=0; n <= rad/lsz; n++)
  {
    const int r = mad24(n, lsz, lsz - lid);
    if(r > rad) continue;
    const int xx = mad24((int)get_group_id(0), lsz, lsz - 1 + r);
    buffer[rad + lsz - 1 + r] = read_imagef(in, sampleri, (int2)(xx, y)).x;
  }

  barrier(CLK_LOCAL_MEM_FENCE);

  if(x >= width || y >= height) return;

  buffer += lid + rad;

  float sum = 0.0f;

  for (int i=-rad; i<=rad; i++)
  {
    sum += buffer[i];
  }

  pixel = sum/(2*rad+1);
  write_imagef (out, (int2)(x, y), pixel);
}


/* vertical box blur */
kernel void 
bloom_vblur(read_only image2d_t in, write_only image2d_t out, const int rad,
      const int width, const int height, const int blocksize, local float *buffer)
{
  const int lid = get_local_id(1);
  const int lsz = get_local_size(1);
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  float pixel = 0.0f;

  /* read pixel and fill center part of buffer */
  pixel = read_imagef(in, sampleri, (int2)(x, y)).x;
  buffer[rad + lid] = pixel;

  /* left wing of buffer */
  for(int n=0; n <= rad/lsz; n++)
  {
    const int l = mad24(n, lsz, lid + 1);
    if(l > rad) continue;
    const int yy = mad24((int)get_group_id(1), lsz, -l);
    buffer[rad - l] = read_imagef(in, sampleri, (int2)(x, yy)).x;
  }
    
  /* right wing of buffer */
  for(int n=0; n <= rad/lsz; n++)
  {
    const int r = mad24(n, lsz, lsz - lid);
    if(r > rad) continue;
    const int yy = mad24((int)get_group_id(1), lsz, lsz - 1 + r);
    buffer[rad + lsz - 1 + r] = read_imagef(in, sampleri, (int2)(x, yy)).x;
  }

  barrier(CLK_LOCAL_MEM_FENCE);

  if(x >= width || y >= height) return;

  buffer += lid + rad;

  float sum = 0.0f;

  for (int i=-rad; i<=rad; i++)
  {
    sum += buffer[i];
  }

  pixel = sum / (2*rad+1);
  write_imagef (out, (int2)(x, y), pixel);
}


/* final step for bloom module */
kernel void
bloom_mix(read_only image2d_t in_a, read_only image2d_t in_b, write_only image2d_t out, const int width, const int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 pixel  = read_imagef(in_a, sampleri, (int2)(x, y));
  float processed = read_imagef(in_b, sampleri, (int2)(x, y)).x;

  pixel.x = 100.0f-(((100.0f-pixel.x)*(100.0f-processed))/100.0f); // Screen blend

  write_imagef (out, (int2)(x, y), pixel);
}

