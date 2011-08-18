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


#define ICLAMP(a, mn, mx) ((a) < (mn) ? (mn) : ((a) > (mx) ? (mx) : (a)))

float4
weight(const float4 c1, const float4 c2, const float sharpen)
{
  // native_exp is faster than the cpu floating point aliasing hack:
  const float wc = native_exp(-((c1.y - c2.y)*(c1.y - c2.y) + (c1.z - c2.z)*(c1.z - c2.z)) * sharpen);
  const float wl = native_exp(- (c1.x - c2.x)*(c1.x - c2.x) * sharpen);
  return (float4)(wl, wc, wc, 1.0f);
}


__kernel void
eaw_decompose (__read_only image2d_t in, __write_only image2d_t coarse, __write_only image2d_t detail,
     const int width, const int height, const int scale, const float sharpen)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  const int mult = 1<<scale;
  float filter[5] = {1.0f/16.0f, 4.0f/16.0f, 6.0f/16.0f, 4.0f/16.0f, 1.0f/16.0f};

  float4 pixel = read_imagef(in, sampleri, (int2)(x, y));
  float4 sum = (float4)(0.0f);
  float4 wgt = (float4)(0.0f);
  for(int j=0;j<5;j++) for(int i=0;i<5;i++)
  {
    int xx = x + mult*(i - 2);
    int yy = y + mult*(j - 2);
    /* clamp to region */
    xx = ICLAMP(xx, 0, width - 1);
    yy = ICLAMP(yy, 0, height - 1);

    float4 px = read_imagef(in, sampleri, (int2)(xx, yy));
    float4 w = filter[i]*filter[j]*weight(pixel, px, sharpen);

    sum += w*px;
    wgt += w;
  }
  sum /= wgt;

  write_imagef (detail, (int2)(x, y), pixel - sum);
  write_imagef (coarse, (int2)(x, y), sum);
}


__kernel void
eaw_synthesize (__write_only image2d_t out, __read_only image2d_t coarse, __read_only image2d_t detail,
     const int width, const int height,
     const float t0, const float t1, const float t2, const float t3,
     const float b0, const float b1, const float b2, const float b3)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  const float4 threshold = (float4)(t0, t1, t2, t3);
  const float4 boost     = (float4)(b0, b1, b2, b3);
  float4 c = read_imagef(coarse, sampleri, (int2)(x, y));
  float4 d = read_imagef(detail, sampleri, (int2)(x, y));
  float4 amount = copysign(max((float4)(0.0f), fabs(d) - threshold), d);
  float4 sum = c + boost*amount;
  write_imagef (out, (int2)(x, y), sum);
}

