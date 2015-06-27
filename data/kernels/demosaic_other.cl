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

  write_imagef (out, (int2)(x, y), color);
}
