/*
    This file is part of darktable,
    copyright (c) 2009--2012 Ulrich Pegelow

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

#ifndef M_PI
#define M_PI           3.14159265358979323846  // should be defined by the OpenCL compiler acc. to standard
#endif


__kernel void
graduatedndp (read_only image2d_t in, write_only image2d_t out, const int width, const int height, const float4 color,
              const float density, const float length_base, const float length_inc_x, const float length_inc_y)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 pixel = read_imagef(in, sampleri, (int2)(x, y));
  
  const float len = length_base + y*length_inc_y + x*length_inc_x;

  const float t = 0.693147181f * (density * clamp(0.5f+len, 0.0f, 1.0f)/8.0f);
  const float d1 = t * t * 0.5f;
  const float d2 = d1 * t * 0.333333333f;
  const float d3 = d2 * t * 0.25f;
  float dens = 1.0f + t + d1 + d2 + d3;
  dens *= dens;
  dens *= dens;
  dens *= dens;

  pixel = fmax((float4)0.0f, pixel / (color + ((float4)1.0f - color) * (float4)dens));
      
  write_imagef (out, (int2)(x, y), pixel); 
}


__kernel void
graduatedndm (read_only image2d_t in, write_only image2d_t out, const int width, const int height, const float4 color,
              const float density, const float length_base, const float length_inc_x, const float length_inc_y)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 pixel = read_imagef(in, sampleri, (int2)(x, y));
  
  const float len = length_base + y*length_inc_y + x*length_inc_x;

  const float t = 0.693147181f * (-density * clamp(0.5f-len, 0.0f, 1.0f)/8.0f);
  const float d1 = t * t * 0.5f;
  const float d2 = d1 * t * 0.333333333f;
  const float d3 = d2 * t * 0.25f;
  float dens = 1.0f + t + d1 + d2 + d3;
  dens *= dens;
  dens *= dens;
  dens *= dens;

  pixel = fmax((float4)0.0f, pixel * (color + ((float4)1.0f - color) * (float4)dens));
      
  write_imagef (out, (int2)(x, y), pixel); 
}

__kernel void
colorize (read_only image2d_t in, write_only image2d_t out, const int width, const int height,
          const float mix, const float L, const float a, const float b)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 pixel = read_imagef(in, sampleri, (int2)(x, y));

  pixel.x = pixel.x * mix + L - 50.0f * mix;
  pixel.y = a;
  pixel.z = b;

  write_imagef (out, (int2)(x, y), pixel); 
}


float 
GAUSS(float center, float wings, float x)
{
  const float b = -1.0f + center * 2.0f;
  const float c = (wings / 10.0f) / 2.0f;
  return exp(-(x-b)*(x-b)/(c*c));
}


__kernel void
relight (read_only image2d_t in, write_only image2d_t out, const int width, const int height,
         const float center, const float wings, const float ev)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 pixel = read_imagef(in, sampleri, (int2)(x, y));

  const float lightness = pixel.x/100.0f;
  const float value = -1.0f+(lightness*2.0f);
  float gauss = GAUSS(center, wings, value);

  if(isnan(gauss) || isinf(gauss))
    gauss = 0.0f;

  float relight = 1.0f / exp2(-ev * clamp(gauss, 0.0f, 1.0f));

  if(isnan(relight) || isinf(relight))
    relight = 1.0f;

  pixel.x = 100.0f * clamp(lightness*relight, 0.0f, 1.0f);

  write_imagef (out, (int2)(x, y), pixel); 
}









