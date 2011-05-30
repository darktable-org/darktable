/*
    This file is part of darktable,
    copyright (c) 2011 ulrich pegelow.

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

const sampler_t sampleri =  CLK_NORMALIZED_COORDS_FALSE | CLK_ADDRESS_CLAMP_TO_EDGE | CLK_FILTER_NEAREST;
const sampler_t samplerf =  CLK_NORMALIZED_COORDS_FALSE | CLK_ADDRESS_CLAMP_TO_EDGE | CLK_FILTER_LINEAR;


/* This is highpass for Lab space. We only do invert/blur/mix on L and desaturate a and b */

kernel void 
highpass_invert(read_only image2d_t in, write_only image2d_t out)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  float4 pixel = read_imagef(in, sampleri, (int2)(x, y));
  pixel.x = clamp(100.0f - pixel.x, 0.0f, 100.0f);
  write_imagef (out, (int2)(x, y), pixel);
}


kernel void 
highpass_hblur(read_only image2d_t in, write_only image2d_t out, constant float *m, const int rad)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  const int maxx = get_global_size(0);
  const int wd = 2*rad+1;
  
  float sum = 0.0f;
  float weight = 0.0f;
  float4 ipixel;
  float4 opixel = 0.0f;
  int xx;

  for (int i=0;i<wd;i++)
  {
    xx = x + (i - rad);
    if (xx < 0 || xx > maxx) continue;
    ipixel = read_imagef(in, sampleri, (int2)(xx, y));
    sum += ipixel.x * m[i];
    weight += m[i];
    if (i == rad) opixel = ipixel;
  }
  opixel.x = weight > 0.0f ? sum/weight : 0.0f;
  write_imagef (out, (int2)(x, y), opixel);
}


kernel void 
highpass_vblur(read_only image2d_t in, write_only image2d_t out, constant float *m, const int rad)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  const int maxy = get_global_size(1);
  const int wd = 2*rad+1;
  
  float sum = 0.0f;
  float weight = 0.0f;
  float4 ipixel;
  float4 opixel = 0.0f;
  int yy;

  for (int i=0;i<wd;i++)
  {
    yy = y + (i - rad);
    if (yy < 0 || yy > maxy) continue;
    ipixel = read_imagef(in, sampleri, (int2)(x, yy));
    sum += ipixel.x * m[i];
    weight += m[i];
    if (i == rad) opixel = ipixel;
  }
  opixel.x = weight > 0.0f ? sum/weight : 0.0f;
  write_imagef (out, (int2)(x, y), opixel);
}


kernel void 
highpass_mix(read_only image2d_t in_a, read_only image2d_t in_b, write_only image2d_t out, const float contrast_scale)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);


  float4 o = 0.0f;
  float4 a = read_imagef(in_a, sampleri, (int2)(x, y));
  float4 b = read_imagef(in_b, sampleri, (int2)(x, y));
  float4 min = (float4)(0.0f, -128.0f, -128.0f, 0.0f);
  float4 max = (float4)(100.0f, 128.0f, 128.0f, 1.0f);

  o.x = 50.0f+((0.5f * a.x + 0.5f * b.x) - 50.0f)*contrast_scale;
  o.y = 0.0f;
  o.z = 0.0f;

  write_imagef (out, (int2)(x, y), clamp(o, min, max));
}

