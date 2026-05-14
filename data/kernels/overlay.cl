/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

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

// Alpha-blend a Cairo ARGB32 overlay onto a float4 image.
//
// overlay_argb is a flat byte buffer (Cairo ARGB32, little-endian byte order
// [B, G, R, A] at each pixel), with row pitch = stride bytes.
// The blend formula matches the CPU path in overlay.c:
//   alpha  = (s_a / 255) * opacity
//   out_c  = (1 - alpha) * in_c + opacity * s_c / 255
kernel void overlay_blend(read_only  image2d_t in,
                          __global const uchar *overlay_argb,
                          write_only image2d_t out,
                          const int   width,
                          const int   height,
                          const float opacity,
                          const int   stride)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;

  const float4 i   = Areadpixel(in, x, y);
  const int    off = y * stride + x * 4;

  const float b = overlay_argb[off + 0] / 255.f;
  const float g = overlay_argb[off + 1] / 255.f;
  const float r = overlay_argb[off + 2] / 255.f;
  const float a = overlay_argb[off + 3] / 255.f * opacity;

  float4 o;
  o.x = (1.f - a) * i.x + opacity * r;
  o.y = (1.f - a) * i.y + opacity * g;
  o.z = (1.f - a) * i.z + opacity * b;
  o.w = i.w;
  write_imagef(out, (int2)(x, y), o);
}
