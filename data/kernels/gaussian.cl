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


/* This is gaussian blur in Lab space. Please mind: in contrast to most of DT's other openCL kernels,
   the kernels in this package expect part/all of their input/output buffers in form of vectors. This
   is needed to have read-write access to some buffers which openCL does not offer for image object. */



kernel void 
gaussian_column(read_only image2d_t in, global float4 *out, unsigned int width, unsigned int height,
                  const float a0, const float a1, const float a2, const float a3, const float b1, const float b2,
                  const float coefp, const float coefn)
{
  const int x = get_global_id(0);

  float4 xp = (float4)0.0f;
  float4 yb = (float4)0.0f;
  float4 yp = (float4)0.0f;
  float4 xc = (float4)0.0f;
  float4 yc = (float4)0.0f;
  float4 xn = (float4)0.0f;
  float4 xa = (float4)0.0f;
  float4 yn = (float4)0.0f;
  float4 ya = (float4)0.0f;

  // forward filter
  xp = read_imagef(in, sampleri, (int2)(x, 0));
  yb = xp * coefp;
  yp = yb;

 
  for(int y=0; y<height; y++)
  {
    xc = read_imagef(in, sampleri, (int2)(x, y));
    yc = (a0 * xc) + (a1 * xp) - (b1 * yp) - (b2 * yb);

    xp = xc;
    yb = yp;
    yp = yc;

    out[x + y*width] = yc;

  }

  // backward filter
  xn = read_imagef(in, sampleri, (int2)(x, height-1));
  xa = xn;
  yn = xn * coefn;
  ya = yn;


  for(int y=height-1; y>-1; y--)
  {
    xc = read_imagef(in, sampleri, (int2)(x, y));
    yc = (a2 * xn) + (a3 * xa) - (b1 * yn) - (b2 * ya);

    xa = xn; 
    xn = xc; 
    ya = yn; 
    yn = yc;

    out[x + y*width] += yc;

  }
}



kernel void 
gaussian_row(read_only image2d_t in, global float4 *out, unsigned int width, unsigned int height,
                const float a0, const float a1, const float a2, const float a3, const float b1, const float b2,
                const float coefp, const float coefn)
{
  const int y = get_global_id(1);

  float4 xp = (float4)0.0f;
  float4 yb = (float4)0.0f;
  float4 yp = (float4)0.0f;
  float4 xc = (float4)0.0f;
  float4 yc = (float4)0.0f;
  float4 xn = (float4)0.0f;
  float4 xa = (float4)0.0f;
  float4 yn = (float4)0.0f;
  float4 ya = (float4)0.0f;

  // forward filter
  xp = read_imagef(in, sampleri, (int2)(0, y));
  yb = xp * coefp;
  yp = yb;

 
  for(int x=0; x<width; x++)
  {
    xc = read_imagef(in, sampleri, (int2)(x, y));
    yc = (a0 * xc) + (a1 * xp) - (b1 * yp) - (b2 * yb);

    xp = xc;
    yb = yp;
    yp = yc;

    out[x + y*width] = yc;

  }

  // backward filter
  xn = read_imagef(in, sampleri, (int2)(width-1, y));
  xa = xn;
  yn = xn * coefn;
  ya = yn;


  for(int x=width-1; x>-1; x--)
  {
    xc = read_imagef(in, sampleri, (int2)(x, y));
    yc = (a2 * xn) + (a3 * xa) - (b1 * yn) - (b2 * ya);

    xa = xn; 
    xn = xc; 
    ya = yn; 
    yn = yc;

    out[x + y*width] += yc;

  }
}


kernel void 
lowpass_mix(read_only image2d_t in, write_only image2d_t out, const float contrast, const float saturation)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  float4 i = read_imagef(in, sampleri, (int2)(x, y));
  float4 o;

  const float4 min = (float4)(0.0f, -128.0f, -128.0f, 0.0f);
  const float4 max = (float4)(100.0f, 128.0f, 128.0f, 1.0f);

  o.x = i.x*contrast + 50.0f * (1.0f - contrast);
  o.y = i.y*saturation;
  o.z = i.z*saturation;

  write_imagef(out, (int2)(x, y), clamp(o, min, max));
}

