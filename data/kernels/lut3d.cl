/*
    This file is part of darktable,
    copyright (c) 2019 philippe weyland.

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
lut3d_tetrahedral(read_only image2d_t in, write_only image2d_t out, const int width, const int height,
           global float *clut, const int level)
{
  int4 rgbi = (int4)(0);
  float4 rgbd = (float4)(0.0f);
  const int level2 = level * level;
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 input = read_imagef(in, sampleri, (int2)(x, y));
  float4 output = (float4)(0.0f);

  input = clamp(input, (float4)0.0f, (float4)1.0f);

  rgbd = input * (float)(level - 1);
  rgbi = min( max( convert_int4(rgbd), (int4)0), (int4)(level - 2));

// delta r, g, b
  rgbd = rgbd - convert_float4(rgbi);

// indexes of P000 to P111 in clut
  const int color = rgbi.x + rgbi.y * level + rgbi.z * level2;
  const int i000 = color * 3 ;  // P000
  const int i100 = i000 + 3;  // P100
  const int i010 = (int)(color + level) * 3;  // P010
  const int i110 = i010 + 3;  //P110
  const int i001 = (int)(color + level2) * 3;  // P001
  const int i101 = i001 + 3;  // P101
  const int i011 = (int)(color + level + level2) * 3;  // P011
  const int i111 = i011 + 3;  // P111

// float3 are 16 bytes aligned. So no way to set clut as an array of float3.
  const float4 clut000 = (float4)(clut[i000], clut[i000+1], clut[i000+2], 0.0f);
  const float4 clut100 = (float4)(clut[i100], clut[i100+1], clut[i100+2], 0.0f);
  const float4 clut010 = (float4)(clut[i010], clut[i010+1], clut[i010+2], 0.0f);
  const float4 clut110 = (float4)(clut[i110], clut[i110+1], clut[i110+2], 0.0f);
  const float4 clut001 = (float4)(clut[i001], clut[i001+1], clut[i001+2], 0.0f);
  const float4 clut101 = (float4)(clut[i101], clut[i101+1], clut[i101+2], 0.0f);
  const float4 clut011 = (float4)(clut[i011], clut[i011+1], clut[i011+2], 0.0f);
  const float4 clut111 = (float4)(clut[i111], clut[i111+1], clut[i111+2], 0.0f);

  if (rgbd.x > rgbd.y)
  {
    if (rgbd.y > rgbd.z)
    {
      output = (1.0f-rgbd.x)*clut000 + (rgbd.x-rgbd.y)*clut100 + (rgbd.y-rgbd.z)*clut110 + rgbd.z*clut111;
    }
    else if (rgbd.x > rgbd.z)
    {
      output = (1.0f-rgbd.x)*clut000 + (rgbd.x-rgbd.z)*clut100 + (rgbd.z-rgbd.y)*clut101 + rgbd.y*clut111;
    }
    else
    {
      output = (1.0f-rgbd.z)*clut000 + (rgbd.z-rgbd.x)*clut001 + (rgbd.x-rgbd.y)*clut101 + rgbd.y*clut111;
    }
  }
  else
  {
    if (rgbd.z > rgbd.y)
    {
      output = (1.0f-rgbd.z)*clut000 + (rgbd.z-rgbd.y)*clut001 + (rgbd.y-rgbd.x)*clut011 + rgbd.x*clut111;
    }
    else if (rgbd.z > rgbd.x)
    {
      output = (1.0f-rgbd.y)*clut000 + (rgbd.y-rgbd.z)*clut010 + (rgbd.z-rgbd.x)*clut011 + rgbd.x*clut111;
    }
    else
    {
      output = (1.0f-rgbd.y)*clut000 + (rgbd.y-rgbd.x)*clut010 + (rgbd.x-rgbd.z)*clut110 + rgbd.z*clut111;
    }
  }
  output.w = input.w;
  write_imagef(out, (int2)(x, y), output);
}

kernel void
lut3d_trilinear(read_only image2d_t in, write_only image2d_t out, const int width, const int height,
           global float *clut, const uint level)
{
  int4 rgbi = (int4)(0);
  float4 rgbd = (float4)(0.0f);
  const int level2 = level * level;
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  float4 tmp1;
  float4 tmp2;

  if(x >= width || y >= height) return;

  float4 input = read_imagef(in, sampleri, (int2)(x, y));
  float4 output = (float4)(0.0f);

  input = clamp(input, (float4)0.0f, (float4)1.0f);

  rgbd = input * (float)(level - 1);
  rgbi = min( max( convert_int4(rgbd), (int4)0), (int4)(level - 2));

  // delta r, g, b
  rgbd = rgbd - convert_float4(rgbi);

  // indexes of P000 to P111 in clut
  const int color = rgbi.x + rgbi.y * level + rgbi.z * level2;
  const int i000 = color * 3 ;  // P000
  const int i100 = i000 + 3;  // P100
  const int i010 = (int)(color + level) * 3;  // P010
  const int i110 = i010 + 3;  //P110
  const int i001 = (int)(color + level2) * 3;  // P001
  const int i101 = i001 + 3;  // P101
  const int i011 = (int)(color + level + level2) * 3;  // P011
  const int i111 = i011 + 3;  // P111

  // float3 are 16 bytes aligned. So no way to set clut as an array of float3.
  const float4 clut000 = (float4)(clut[i000], clut[i000+1], clut[i000+2], 0.0f);
  const float4 clut100 = (float4)(clut[i100], clut[i100+1], clut[i100+2], 0.0f);
  const float4 clut010 = (float4)(clut[i010], clut[i010+1], clut[i010+2], 0.0f);
  const float4 clut110 = (float4)(clut[i110], clut[i110+1], clut[i110+2], 0.0f);
  const float4 clut001 = (float4)(clut[i001], clut[i001+1], clut[i001+2], 0.0f);
  const float4 clut101 = (float4)(clut[i101], clut[i101+1], clut[i101+2], 0.0f);
  const float4 clut011 = (float4)(clut[i011], clut[i011+1], clut[i011+2], 0.0f);
  const float4 clut111 = (float4)(clut[i111], clut[i111+1], clut[i111+2], 0.0f);

  tmp1 = clut000*(1.0f-rgbd.x) + clut100*rgbd.x;
  tmp2 = clut010*(1.0f-rgbd.x) + clut110*rgbd.x;
  output = tmp1*(1.0f-rgbd.y) + tmp2*rgbd.y;
  tmp1 = clut001*(1.0f-rgbd.x) + clut101*rgbd.x;
  tmp2 = clut011*(1.0f-rgbd.x) + clut111*rgbd.x;
  tmp1 = tmp1*(1.0f-rgbd.y) + tmp2*rgbd.y;
  output = output*(1.0f-rgbd.z) + tmp1*rgbd.z;

  output.w = input.w;
  write_imagef(out, (int2)(x, y), output);
}

kernel void
lut3d_pyramid(read_only image2d_t in, write_only image2d_t out, const int width, const int height,
           global float *clut, const uint level)
{
  int4 rgbi = (int4)(0);
  float4 rgbd = (float4)(0.0f);
  const int level2 = level * level;
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 input = read_imagef(in, sampleri, (int2)(x, y));
  float4 output = (float4)(0.0f);

  input = clamp(input, (float4)0.0f, (float4)1.0f);

  rgbd = input * (float)(level - 1);
  rgbi = min( max( convert_int4(rgbd), (int4)0), (int4)(level - 2));

  // delta r, g, b
  rgbd = rgbd - convert_float4(rgbi);

  // indexes of P000 to P111 in clut
  const int color = rgbi.x + rgbi.y * level + rgbi.z * level2;
  const int i000 = color * 3 ;  // P000
  const int i100 = i000 + 3;  // P100
  const int i010 = (int)(color + level) * 3;  // P010
  const int i110 = i010 + 3;  //P110
  const int i001 = (int)(color + level2) * 3;  // P001
  const int i101 = i001 + 3;  // P101
  const int i011 = (int)(color + level + level2) * 3;  // P011
  const int i111 = i011 + 3;  // P111

  // float3 are 16 bytes aligned. So no way to set clut as an array of float3.
  const float4 clut000 = (float4)(clut[i000], clut[i000+1], clut[i000+2], 0.0f);
  const float4 clut100 = (float4)(clut[i100], clut[i100+1], clut[i100+2], 0.0f);
  const float4 clut010 = (float4)(clut[i010], clut[i010+1], clut[i010+2], 0.0f);
  const float4 clut110 = (float4)(clut[i110], clut[i110+1], clut[i110+2], 0.0f);
  const float4 clut001 = (float4)(clut[i001], clut[i001+1], clut[i001+2], 0.0f);
  const float4 clut101 = (float4)(clut[i101], clut[i101+1], clut[i101+2], 0.0f);
  const float4 clut011 = (float4)(clut[i011], clut[i011+1], clut[i011+2], 0.0f);
  const float4 clut111 = (float4)(clut[i111], clut[i111+1], clut[i111+2], 0.0f);

  if (rgbd.y > rgbd.x && rgbd.z > rgbd.x)
  {
    output = clut000 + (clut111-clut011)*rgbd.x + (clut010-clut000)*rgbd.y + (clut001-clut000)*rgbd.z
      + (clut011-clut001-clut010+clut000)*rgbd.y*rgbd.z;
  }
  else if (rgbd.x > rgbd.y && rgbd.z > rgbd.y)
  {
    output = clut000 + (clut100-clut000)*rgbd.x + (clut111-clut101)*rgbd.y + (clut001-clut000)*rgbd.z
      + (clut101-clut001-clut100+clut000)*rgbd.x*rgbd.z;
  }
  else
  {
    output = clut000 + (clut100-clut000)*rgbd.x + (clut010-clut000)*rgbd.y + (clut111-clut110)*rgbd.z
      + (clut110-clut100-clut010+clut000)*rgbd.x*rgbd.y;
  }
  output.w = input.w;
  write_imagef(out, (int2)(x, y), output);
}

kernel void
lut3d_none(read_only image2d_t in, write_only image2d_t out, const int width, const int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 input = read_imagef(in, sampleri, (int2)(x, y));

  write_imagef(out, (int2)(x, y), input);
}
