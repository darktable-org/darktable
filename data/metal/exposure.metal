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

#include <metal_stdlib>
using namespace metal;

/* kernel for the exposure plugin.
   Equivalent of the OpenCL exposure() kernel in basic.cl.
   Operates on float4 pixel buffers (RGBA).
   Applies: pixel.rgb = (pixel.rgb - black) * scale
   Alpha channel is passed through unchanged. */

kernel void
exposure(device const float4 *input   [[buffer(0)]],
         device float4       *output  [[buffer(1)]],
         constant int        &width   [[buffer(2)]],
         constant int        &height  [[buffer(3)]],
         constant float      &black   [[buffer(4)]],
         constant float      &scale   [[buffer(5)]],
         uint2 gid [[thread_position_in_grid]])
{
  if(gid.x >= (uint)width || gid.y >= (uint)height) return;

  const int idx = gid.y * width + gid.x;
  float4 pixel = input[idx];
  pixel.xyz = (pixel.xyz - black) * scale;
  output[idx] = pixel;
}
