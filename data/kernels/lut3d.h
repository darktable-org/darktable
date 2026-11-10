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

#pragma once

/* tetrahedral interpolation in a level^3 color LUT of 3 floats per node,
   the first channel varying fastest. The coordinates in rgb must already
   lie in [0, 1].

   Shared by the lut3d module and the LittleCMS path of color in and color
   out. Keep this file in the include list hashed into every kernel cache
   key in src/common/opencl.c, or an edit here would leave the cached
   binaries of the programs including it stale */
static inline float4 dt_lut3d_tetrahedral(const float4 rgb,
                                          global const float *const clut,
                                          const int level)
{
  const int level2 = level * level;

  float4 rgbd = rgb * (float)(level - 1);
  const int4 rgbi = min(max(convert_int4(rgbd), (int4)0), (int4)(level - 2));

  // delta r, g, b
  rgbd = rgbd - convert_float4(rgbi);

  // indexes of P000 to P111 in clut
  const int color = rgbi.x + rgbi.y * level + rgbi.z * level2;
  const int i000 = color * 3;                          // P000
  const int i100 = i000 + 3;                           // P100
  const int i010 = (color + level) * 3;                // P010
  const int i110 = i010 + 3;                           // P110
  const int i001 = (color + level2) * 3;               // P001
  const int i101 = i001 + 3;                           // P101
  const int i011 = (color + level + level2) * 3;       // P011
  const int i111 = i011 + 3;                           // P111

  // float3 are 16 bytes aligned. So no way to set clut as an array of float3.
  const float4 clut000 = (float4)(clut[i000], clut[i000+1], clut[i000+2], 0.0f);
  const float4 clut100 = (float4)(clut[i100], clut[i100+1], clut[i100+2], 0.0f);
  const float4 clut010 = (float4)(clut[i010], clut[i010+1], clut[i010+2], 0.0f);
  const float4 clut110 = (float4)(clut[i110], clut[i110+1], clut[i110+2], 0.0f);
  const float4 clut001 = (float4)(clut[i001], clut[i001+1], clut[i001+2], 0.0f);
  const float4 clut101 = (float4)(clut[i101], clut[i101+1], clut[i101+2], 0.0f);
  const float4 clut011 = (float4)(clut[i011], clut[i011+1], clut[i011+2], 0.0f);
  const float4 clut111 = (float4)(clut[i111], clut[i111+1], clut[i111+2], 0.0f);

  if(rgbd.x > rgbd.y)
  {
    if(rgbd.y > rgbd.z)
      return (1.0f-rgbd.x)*clut000 + (rgbd.x-rgbd.y)*clut100 + (rgbd.y-rgbd.z)*clut110 + rgbd.z*clut111;
    else if(rgbd.x > rgbd.z)
      return (1.0f-rgbd.x)*clut000 + (rgbd.x-rgbd.z)*clut100 + (rgbd.z-rgbd.y)*clut101 + rgbd.y*clut111;
    else
      return (1.0f-rgbd.z)*clut000 + (rgbd.z-rgbd.x)*clut001 + (rgbd.x-rgbd.y)*clut101 + rgbd.y*clut111;
  }
  else
  {
    if(rgbd.z > rgbd.y)
      return (1.0f-rgbd.z)*clut000 + (rgbd.z-rgbd.y)*clut001 + (rgbd.y-rgbd.x)*clut011 + rgbd.x*clut111;
    else if(rgbd.z > rgbd.x)
      return (1.0f-rgbd.y)*clut000 + (rgbd.y-rgbd.z)*clut010 + (rgbd.z-rgbd.x)*clut011 + rgbd.x*clut111;
    else
      return (1.0f-rgbd.y)*clut000 + (rgbd.y-rgbd.x)*clut010 + (rgbd.x-rgbd.z)*clut110 + rgbd.z*clut111;
  }
}
