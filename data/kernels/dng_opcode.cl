/*
    This file is part of darktable,
    copyright (c) 2026 darktable developers.

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

/* DNG OpcodeList3 GainMap, shared by demosaic and rawprepare.

   The bilinear interpolation is written out by hand rather than using a linear
   sampler so that this stays bit-comparable with dt_dng_gain_map_apply() on the
   CPU; a sampler would be shorter but introduces hardware dependent rounding.
   Any change here must be mirrored there. */
__kernel void
dng_gain_map(read_only image2d_t in,
             write_only image2d_t out,
             global const float *map_gain,
             const int width,
             const int height,
             const int map_w,
             const int map_h,
             const int nplanes,
             const float x0,
             const float y0,
             const float step,
             const float raw_w,
             const float raw_h,
             const float rel_to_map_x,
             const float rel_to_map_y,
             const float map_origin_h,
             const float map_origin_v)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;

  const float rel_y = (y0 + ((float)y + 0.5f) * step) / raw_h;
  const float y_map = clamp((rel_y - map_origin_v) * rel_to_map_y, 0.0f, (float)(map_h - 1));
  const int y_i0 = min((int)y_map, map_h - 1);
  const int y_i1 = min(y_i0 + 1, map_h - 1);
  const float y_frac = y_map - y_i0;

  const float rel_x = (x0 + ((float)x + 0.5f) * step) / raw_w;
  const float x_map = clamp((rel_x - map_origin_h) * rel_to_map_x, 0.0f, (float)(map_w - 1));
  const int x_i0 = min((int)x_map, map_w - 1);
  const int x_i1 = min(x_i0 + 1, map_w - 1);
  const float x_frac = x_map - x_i0;

  float gain[3];
  for(int c = 0; c < 3; c++)
  {
    // "the last gain map plane is used for any remaining planes being modified"
    const int p = min(c, nplanes - 1);
    const float g00 = map_gain[(y_i0 * map_w + x_i0) * nplanes + p];
    const float g01 = map_gain[(y_i0 * map_w + x_i1) * nplanes + p];
    const float g10 = map_gain[(y_i1 * map_w + x_i0) * nplanes + p];
    const float g11 = map_gain[(y_i1 * map_w + x_i1) * nplanes + p];

    const float gain_top    = (1.0f - x_frac) * g00 + x_frac * g01;
    const float gain_bottom = (1.0f - x_frac) * g10 + x_frac * g11;
    gain[c] = (1.0f - y_frac) * gain_top + y_frac * gain_bottom;
  }

  float4 pixel = Areadpixel(in, x, y);
  const float alpha = pixel.w;
  pixel.x *= gain[0];
  pixel.y *= gain[1];
  pixel.z *= gain[2];
  pixel.w = alpha;

  write_imagef(out, (int2)(x, y), pixel);
}
