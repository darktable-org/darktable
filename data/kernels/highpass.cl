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


kernel void 
highpass_invert(read_only image2d_t in, write_only image2d_t out)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  float4 one = (float4)(1.0f);
  float4 pixel = read_imagef(in, sampleri, (int2)(x, y));
  write_imagef (out, (int2)(x, y), clamp(one - pixel, 0.0f, 1.0f));
}


kernel void 
highpass_hblur(read_only image2d_t in, write_only image2d_t out, global float *m, const int rad)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  const int maxx = get_global_size(0);
  const int wd = 2*rad+1;
  
  float4 sum = (float4)(0.0f);
  float weight = 0.0f;
  int xx;

  for (int i=0;i<wd;i++)
  {
    xx = x + (i - rad);
    if (xx < 0 || xx > maxx) continue;
    sum += read_imagef(in, sampleri, (int2)(xx, y)) * m[i];
    weight += m[i];
  }
  write_imagef (out, (int2)(x, y), weight > 0.0f ? sum/weight : (float4)0.0f);
}


kernel void 
highpass_vblur(read_only image2d_t in, write_only image2d_t out, global float *m, const int rad)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  const int maxy = get_global_size(1);
  const int wd = 2*rad+1;
  
  float4 sum = (float4)(0.0f);
  float weight = 0.0f;
  int yy;

  for (int i=0;i<wd;i++)
  {
    yy = y + (i - rad);
    if (yy < 0 || yy > maxy) continue;
    sum += read_imagef(in, sampleri, (int2)(x, yy)) * m[i];
    weight += m[i];
  }
  write_imagef (out, (int2)(x, y), weight > 0.0f ? sum/weight : (float4)0.0f);
}


kernel void 
highpass_mix(read_only image2d_t in_a, read_only image2d_t in_b, write_only image2d_t out, const float contrast_scale)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  
  float4 a = read_imagef(in_a, sampleri, (int2)(x, y));
  float4 b = read_imagef(in_b, sampleri, (int2)(x, y));
  float4 o = 0.5f * a + 0.5f * b;
  o.x = o.y = o.z = (o.x + o.y + o.z)/3.0f;

  write_imagef (out, (int2)(x, y), clamp((float4)0.5f+((o-(float4)0.5f)*contrast_scale), 0.0f, 1.0f));
}

