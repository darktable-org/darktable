/*
    This file is part of darktable,
    copyright (c) 2015 LebedevRI.

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

/**
 * Propagate only channel into each of 3 channels, creating monochrome image
 */
__kernel void
passthrough_monochrome (__read_only image2d_t in, __write_only image2d_t out, const int width, const int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 color;
  const float4 pc = read_imagef(in, sampleri, (int2)(x, y));

  color.xyz = pc.x;

  write_imagef (out, (int2)(x, y), fmax(color, 0.0f));
}

__kernel void
passthrough_color (__read_only image2d_t in, __write_only image2d_t out, const int width, const int height, const int rx, const int ry,
                   const unsigned int filters, global const unsigned char (*const xtrans)[6])
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  const float ival = read_imagef(in, sampleri, (int2)(x, y)).x;
  const int c = (filters == 9u) ? FCxtrans(y + ry, x + rx, xtrans) : FC(y + ry, x + rx, filters);

  float4 oval = (float4)(0.0f, 0.0f, 0.0f, 0.0f);
  if(c == 0)       oval.x = ival;
  else if (c == 1) oval.y = ival;
  else             oval.z = ival;

  write_imagef (out, (int2)(x, y), oval);
}

/**
 * downscales and clips a mosaiced buffer (in) to the given region of interest (r_*)
 * and writes it to out in float4 format.
 */
__kernel void
clip_and_zoom_demosaic_passthrough_monochrome(__read_only image2d_t in, __write_only image2d_t out, const int width, const int height,
    const int r_x, const int r_y, const int rin_wd, const int rin_ht, const float r_scale, const unsigned int filters)
{
  // global id is pixel in output image (float4)
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 color = (float4)(0.0f, 0.0f, 0.0f, 0.0f);
  float weight = 0.0f;

  // pixel footprint on input buffer, radius:
  const float px_footprint = 1.0f/r_scale;
  // how many pixels can be sampled inside that area
  const int samples = round(px_footprint);

  const float2 f = (float2)((x + r_x) * px_footprint, (y + r_y) * px_footprint);
  int2 p = (int2)((int)f.x, (int)f.y);
  const float2 d = (float2)(f.x - p.x, f.y - p.y);

  for(int j=0;j<=samples+1;j++) for(int i=0;i<=samples+1;i++)
  {
    const int xx = p.x + i;
    const int yy = p.y + j;

    float xfilter = (i == 0) ? 1.0f - d.x : ((i == samples+1) ? d.x : 1.0f);
    float yfilter = (j == 0) ? 1.0f - d.y : ((j == samples+1) ? d.y : 1.0f);

    float px = read_imagef(in, sampleri, (int2)(xx, yy)).x;
    color += yfilter*xfilter*(float4)(px, px, px, 0.0f);
    weight += yfilter*xfilter;
  }
  color = (weight > 0.0f) ? color/weight : (float4)0.0f;
  write_imagef (out, (int2)(x, y), color);
}
