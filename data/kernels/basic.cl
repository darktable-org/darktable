/*
    This file is part of darktable,
    copyright (c) 2009--2010 johannes hanika.

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

__kernel void
sharpen (read_only image2d_t in, write_only image2d_t out, constant float *m, const int rad,
    const float sharpen, const float thrs)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  const int wd = 2*rad+1;

  float4 pixel = read_imagef(in, sampleri, (int2)(x, y));
  float sum = 0.0f;
  for(int j=0;j<wd;j++) for(int i=0;i<wd;i++)
  {
    float px = read_imagef(in, sampleri, (float2)(x+(i - rad), y+(j - rad))).x;
    const float w = m[j]*m[i];
    sum += w*px;
  }
  float d = pixel.x - sum;
  float amount = sharpen * copysign(max(0.0f, fabs(d) - thrs), d);
  write_imagef (out, (int2)(x, y), (float4)(max(0.0f, pixel.x + amount), pixel.y, pixel.z, 1.0f));
}


