/*
    This file is part of darktable,
    copyright (c) 2021 darktable developers.

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

kernel void
convolve(read_only image2d_t in, read_only image2d_t kern, write_only image2d_t out,
         const int width, const int height, const int radius)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 pix_in = read_imagef(in, samplerA, (int2)(x, y));
  float4 acc = 0.f;

  for(int l = -radius; l <= radius; l++)
    for(int m = -radius; m <= radius; m++)
    {
      const int ii = clamp(y + l, 0, height - 1);
      const int jj = clamp(x + m, 0, width - 1);
      const float4 pix = read_imagef(in, samplerA, (int2)(jj, ii));

      const int ik = l + radius;
      const int jk = m + radius;
      const float k = read_imagef(kern, samplerA, (int2)(jk, ik)).x;

      acc += k * pix;
    }

  // copy alpha
  acc.w = pix_in.w;
  write_imagef(out, (int2)(x, y), acc);
}
