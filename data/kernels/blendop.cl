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


#define DEVELOP_BLEND_MASK_FLAG				0x80
#define DEVELOP_BLEND_DISABLED				0x00
#define DEVELOP_BLEND_NORMAL				0x01
#define DEVELOP_BLEND_LIGHTEN				0x02
#define DEVELOP_BLEND_DARKEN				0x03
#define DEVELOP_BLEND_MULTIPLY				0x04
#define DEVELOP_BLEND_AVERAGE				0x05
#define DEVELOP_BLEND_ADD				0x06
#define DEVELOP_BLEND_SUBSTRACT				0x07
#define DEVELOP_BLEND_DIFFERENCE			0x08
#define DEVELOP_BLEND_SCREEN				0x09
#define DEVELOP_BLEND_OVERLAY				0x0A
#define DEVELOP_BLEND_SOFTLIGHT				0x0B
#define DEVELOP_BLEND_HARDLIGHT				0x0C
#define DEVELOP_BLEND_VIVIDLIGHT			0x0D
#define DEVELOP_BLEND_LINEARLIGHT			0x0E
#define DEVELOP_BLEND_PINLIGHT				0x0F

#define BLEND_ONLY_LIGHTNESS				8


const sampler_t sampleri =  CLK_NORMALIZED_COORDS_FALSE | CLK_ADDRESS_CLAMP_TO_EDGE | CLK_FILTER_NEAREST;


__kernel void
blendop_Lab (__read_only image2d_t in_a, __read_only image2d_t in_b, __write_only image2d_t out, const int width, const int height, const int mode, const float opacity, const int blendflag)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 o;

  float4 a = read_imagef(in_a, sampleri, (int2)(x, y));
  float4 b = read_imagef(in_b, sampleri, (int2)(x, y));

  /* save before scaling (for later use) */
  float ay = a.y;
  float az = a.z;

  /* scale L down to [0; 1] and a,b to [-1; 1] */
  const float4 scale = (float4)(100.0f, 128.0f, 128.0f, 1.0f);
  a /= scale;
  b /= scale;

  const float4 min = (float4)(0.0f, -1.0f, -1.0f, 0.0f);
  const float4 max = (float4)(1.0f, 1.0f, 1.0f, 1.0f);
  const float4 lmin = (float4)(0.0f, 0.0f, 0.0f, 1.0f);
  const float4 lmax = (float4)(1.0f, 2.0f, 2.0f, 1.0f);       /* max + fabs(min) */
  const float4 halfmax = (float4)(0.5f, 1.0f, 1.0f, 0.5f);    /* lmax / 2.0f */
  const float4 doublemax = (float4)(2.0f, 4.0f, 4.0f, 2.0f);  /* lmax * 2.0f */
  const float opacity2 = opacity*opacity;

  float4 la = clamp(a + fabs(min), lmin, lmax);
  float4 lb = clamp(b + fabs(min), lmin, lmax);



  /* select the blend operator */
  switch (mode)
  {
    case DEVELOP_BLEND_LIGHTEN:
      o = clamp(a * (1.0f - opacity) + (a > b ? a : b) * opacity, min, max);
      o.y = clamp(a.y * (1.0f - fabs(o.x - a.x)) + 0.5f * (a.y + b.y) * fabs(o.x - a.x), min.y, max.y);
      o.z = clamp(a.z * (1.0f - fabs(o.x - a.x)) + 0.5f * (a.z + b.z) * fabs(o.x - a.x), min.z, max.z);
      break;

    case DEVELOP_BLEND_DARKEN:
      o = clamp(a * (1.0f - opacity) + (a < b ? a : b) * opacity, min, max);
      o.y = clamp(a.y * (1.0f - fabs(o.x - a.x)) + 0.5f * (a.y + b.y) * fabs(o.x - a.x), min.y, max.y);
      o.z = clamp(a.z * (1.0f - fabs(o.x - a.x)) + 0.5f * (a.z + b.z) * fabs(o.x - a.x), min.z, max.z);
      break;

    case DEVELOP_BLEND_MULTIPLY:
      o = clamp(a * (1.0f - opacity) + a * b * opacity, min, max);
      if (a.x > 0.01f)
      {
        o.y = clamp(a.y * (1.0f - opacity) + (a.y + b.y) * o.x/a.x * opacity, min.y, max.y);
        o.z = clamp(a.z * (1.0f - opacity) + (a.z + b.z) * o.x/a.x * opacity, min.z, max.z);
      }
      else
      { 
        o.y = clamp(a.y * (1.0f - opacity) + (a.y + b.y) * o.x/0.01f * opacity, min.y, max.y);
        o.z = clamp(a.z * (1.0f - opacity) + (a.z + b.z) * o.x/0.01f * opacity, min.z, max.z);
      }
      break;

    case DEVELOP_BLEND_AVERAGE:
      o =  clamp(a * (1.0f - opacity) + (a + b)/2.0f * opacity, min, max);
      break;

    case DEVELOP_BLEND_ADD:
      o =  clamp(a * (1.0f - opacity) +  (a + b) * opacity, min, max);
      break;

    case DEVELOP_BLEND_SUBSTRACT:
      o =  clamp(a * (1.0f - opacity) +  (b + a - fabs(min + max)) * opacity, min, max);
      break;

    case DEVELOP_BLEND_DIFFERENCE:
      o = clamp(la * (1.0f - opacity) + fabs(la - lb) * opacity, lmin, lmax) - fabs(min);
      break;

    case DEVELOP_BLEND_SCREEN:
      o = clamp(la * (1.0f - opacity) + (lmax - (lmax - la) * (lmax - lb)) * opacity, lmin, lmax) - fabs(min);
      if (a.x > 0.01f)
      {
        o.y = clamp(a.y * (1.0f - opacity) + 0.5f * (a.y + b.y) * o.x/a.x * opacity, min.y, max.y);
        o.z = clamp(a.z * (1.0f - opacity) + 0.5f * (a.z + b.z) * o.x/a.x * opacity, min.z, max.z);
      }
      else
      { 
        o.y = clamp(a.y * (1.0f - opacity) + 0.5f * (a.y + b.y) * o.x/0.01f * opacity, min.y, max.y);
        o.z = clamp(a.z * (1.0f - opacity) + 0.5f * (a.z + b.z) * o.x/0.01f * opacity, min.z, max.z);
      }
      break;

    case DEVELOP_BLEND_OVERLAY:
      o = clamp(la * (1.0f - opacity2) + (la > halfmax ? lmax - (lmax - doublemax * (la - halfmax)) * (lmax-lb) : doublemax * la * lb) * opacity2, lmin, lmax) - fabs(min);
      if (a.x > 0.01f)
      {
        o.y = clamp(a.y * (1.0f - opacity2) + (a.y + b.y) * o.x/a.x * opacity2, min.y, max.y);
        o.z = clamp(a.z * (1.0f - opacity2) + (a.z + b.z) * o.x/a.x * opacity2, min.z, max.z);
      }
      else
      { 
        o.y = clamp(a.y * (1.0f - opacity2) + (a.y + b.y) * o.x/0.01f * opacity2, min.y, max.y);
        o.z = clamp(a.z * (1.0f - opacity2) + (a.z + b.z) * o.x/0.01f * opacity2, min.z, max.z);
      }
      break;

    case DEVELOP_BLEND_SOFTLIGHT:
      o = clamp(la * (1.0f - opacity2) + (lb > halfmax ? lmax - (lmax - la)  * (lmax - (lb - halfmax)) : la * (lb + halfmax)) * opacity2, lmin, lmax) - fabs(min);
      if (a.x > 0.01f)
      {
        o.y = clamp(a.y * (1.0f - opacity2) + (a.y + b.y) * o.x/a.x * opacity2, min.y, max.y);
        o.z = clamp(a.z * (1.0f - opacity2) + (a.z + b.z) * o.x/a.x * opacity2, min.z, max.z);
      }
      else
      { 
        o.y = clamp(a.y * (1.0f - opacity2) + (a.y + b.y) * o.x/0.01f * opacity2, min.y, max.y);
        o.z = clamp(a.z * (1.0f - opacity2) + (a.z + b.z) * o.x/0.01f * opacity2, min.z, max.z);
      }
      break;

    case DEVELOP_BLEND_HARDLIGHT:
      o = clamp(la * (1.0f - opacity2) + (lb > halfmax ? lmax - (lmax - doublemax * (la - halfmax)) * (lmax-lb) : doublemax * la * lb) * opacity2, lmin, lmax) - fabs(min);
      if (a.x > 0.01f)
      {
        o.y = clamp(a.y * (1.0f - opacity2) + (a.y + b.y) * o.x/a.x * opacity2, min.y, max.y);
        o.z = clamp(a.z * (1.0f - opacity2) + (a.z + b.z) * o.x/a.x * opacity2, min.z, max.z);
      }
      else
      { 
        o.y = clamp(a.y * (1.0f - opacity2) + (a.y + b.y) * o.x/0.01f * opacity2, min.y, max.y);
        o.z = clamp(a.z * (1.0f - opacity2) + (a.z + b.z) * o.x/0.01f * opacity2, min.z, max.z);
      }
      break;

    case DEVELOP_BLEND_VIVIDLIGHT:
      o = clamp(la * (1.0f - opacity2) + (lb > halfmax ? la / (doublemax * (lmax - lb)) : lmax - (lmax - la)/(doublemax * lb)) * opacity2, lmin, lmax) - fabs(min);
      if (a.x > 0.01f)
      {
        o.y = clamp(a.y * (1.0f - opacity2) + (a.y + b.y) * o.x/a.x * opacity2, min.y, max.y);
        o.z = clamp(a.z * (1.0f - opacity2) + (a.z + b.z) * o.x/a.x * opacity2, min.z, max.z);
      }
      else
      { 
        o.y = clamp(a.y * (1.0f - opacity2) + (a.y + b.y) * o.x/0.01f * opacity2, min.y, max.y);
        o.z = clamp(a.z * (1.0f - opacity2) + (a.z + b.z) * o.x/0.01f * opacity2, min.z, max.z);
      }
      break;

    case DEVELOP_BLEND_LINEARLIGHT:
      o = clamp(la * (1.0f - opacity2) + (la + doublemax * lb - lmax) * opacity2, lmin, lmax) - fabs(min);
      if (a.x > 0.01f)
      {
        o.y = clamp(a.y * (1.0f - opacity2) + (a.y + b.y) * o.x/a.x * opacity2, min.y, max.y);
        o.z = clamp(a.z * (1.0f - opacity2) + (a.z + b.z) * o.x/a.x * opacity2, min.z, max.z);
      }
      else
      { 
        o.y = clamp(a.y * (1.0f - opacity2) + (a.y + b.y) * o.x/0.01f * opacity2, min.y, max.y);
        o.z = clamp(a.z * (1.0f - opacity2) + (a.z + b.z) * o.x/0.01f * opacity2, min.z, max.z);
      }
      break;

    case DEVELOP_BLEND_PINLIGHT:
      o = clamp(la * (1.0f - opacity2) + (lb > halfmax ? fmax(la, doublemax * (lb - halfmax)) : fmin(la, doublemax * lb)) * opacity2, lmin, lmax) - fabs(min);
      o.y = a.y;
      o.z = a.z;
      break;


    /* fallback to normal blend */
    case DEVELOP_BLEND_NORMAL:
    default:
      o =  (a * (1.0f - opacity)) + (b * opacity);
      break;
  }

  /* scale L back to [0; 100] and a,b to [-128; 128] */
  o *= scale;

  /* if module wants to blend only lightness, set a and b to values of input image (saved before scaling) */
  if (blendflag & BLEND_ONLY_LIGHTNESS)
  {
    o.y = ay;
    o.z = az;
  }

  write_imagef(out, (int2)(x, y), o);
}



__kernel void
blendop_RAW (__read_only image2d_t in_a, __read_only image2d_t in_b, __write_only image2d_t out, const int mode, const float opacity, const int blendflag)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  float o;

  float a = read_imagef(in_a, sampleri, (int2)(x, y)).x;
  float b = read_imagef(in_b, sampleri, (int2)(x, y)).x;

  const float min = 0.0f;
  const float max = 1.0f;
  const float lmin = 0.0f;
  const float lmax = 1.0f;        /* max + fabs(min) */
  const float halfmax = 0.5f;     /* lmax / 2.0f */
  const float doublemax = 2.0f;   /* lmax * 2.0f */
  const float opacity2 = opacity*opacity;

  float la = clamp(a + fabs(min), lmin, lmax);
  float lb = clamp(b + fabs(min), lmin, lmax);


  /* select the blend operator */
  switch (mode)
  {
    case DEVELOP_BLEND_LIGHTEN:
      o = clamp(a * (1.0f - opacity) + fmax(a, b) * opacity, min, max);
      break;

    case DEVELOP_BLEND_DARKEN:
      o = clamp(a * (1.0f - opacity) + fmin(a, b) * opacity, min, max);
      break;

    case DEVELOP_BLEND_MULTIPLY:
      o = clamp(a * (1.0f - opacity) + a * b * opacity, min, max);
      break;

    case DEVELOP_BLEND_AVERAGE:
      o = clamp(a * (1.0f - opacity) + (a + b)/2.0f * opacity, min, max);
      break;

    case DEVELOP_BLEND_ADD:
      o =  clamp(a * (1.0f - opacity) +  (a + b) * opacity, min, max);
      break;

    case DEVELOP_BLEND_SUBSTRACT:
      o =  clamp(a * (1.0f - opacity) +  (b + a - fabs(min + max)) * opacity, min, max);
      break;

    case DEVELOP_BLEND_DIFFERENCE:
      o = clamp(la * (1.0f - opacity) + fabs(la - lb) * opacity, lmin, lmax) - fabs(min);
      break;

    case DEVELOP_BLEND_SCREEN:
      o = clamp(la * (1.0f - opacity) + (lmax - (lmax - la) * (lmax - lb)) * opacity, lmin, lmax) - fabs(min);
      break;

    case DEVELOP_BLEND_OVERLAY:
      o = clamp(la * (1.0f - opacity2) + (la > halfmax ? lmax - (lmax - doublemax * (la - halfmax)) * (lmax-lb) : doublemax * la * lb) * opacity2, lmin, lmax) - fabs(min);
      break;

    case DEVELOP_BLEND_SOFTLIGHT:
      o = clamp(la * (1.0f - opacity2) + (lb > halfmax ? lmax - (lmax - la)  * (lmax - (lb - halfmax)) : la * (lb + halfmax)) * opacity2, lmin, lmax) - fabs(min);
      break;

    case DEVELOP_BLEND_HARDLIGHT:
      o = clamp(la * (1.0f - opacity2) + (lb > halfmax ? lmax - (lmax - doublemax * (la - halfmax)) * (lmax-lb) : doublemax * la * lb) * opacity2, lmin, lmax) - fabs(min);
      break;

    case DEVELOP_BLEND_VIVIDLIGHT:
      o = clamp(la * (1.0f - opacity2) + (lb > halfmax ? la / (doublemax * (lmax - lb)) : lmax - (lmax - la)/(doublemax * lb)) * opacity2, lmin, lmax) - fabs(min);
      break;

    case DEVELOP_BLEND_LINEARLIGHT:
      o = clamp(la * (1.0f - opacity2) + (la + doublemax * lb - lmax) * opacity2, lmin, lmax) - fabs(min);
      break;

    case DEVELOP_BLEND_PINLIGHT:
      o = clamp(la * (1.0f - opacity2) + (lb > halfmax ? fmax(la, doublemax * (lb - halfmax)) : fmin(la, doublemax * lb)) * opacity2, lmin, lmax) - fabs(min);
      break;


    /* fallback to normal blend */
    case DEVELOP_BLEND_NORMAL:
    default:
      o =  (a * (1.0f - opacity)) + (b * opacity);
      break;
  }

  write_imagef(out, (int2)(x, y), (float4)(o, 0.0f, 0.0f, 0.0f));
}


__kernel void
blendop_rgb (__read_only image2d_t in_a, __read_only image2d_t in_b, __write_only image2d_t out, const int mode, const float opacity, const int blendflag)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  float4 o;

  float4 a = read_imagef(in_a, sampleri, (int2)(x, y));
  float4 b = read_imagef(in_b, sampleri, (int2)(x, y));

  const float4 min = (float4)(0.0f, 0.0f, 0.0f, 1.0f);
  const float4 max = (float4)(1.0f, 1.0f, 1.0f, 1.0f);
  const float4 lmin = (float4)(0.0f, 0.0f, 0.0f, 1.0f);
  const float4 lmax = (float4)(1.0f, 1.0f, 1.0f, 1.0f);       /* max + fabs(min) */
  const float4 halfmax = (float4)(0.5f, 0.5f, 0.5f, 1.0f);    /* lmax / 2.0f */
  const float4 doublemax = (float4)(2.0f, 2.0f, 2.0f, 1.0f);  /* lmax * 2.0f */
  const float opacity2 = opacity*opacity;

  float4 la = clamp(a + fabs(min), lmin, lmax);
  float4 lb = clamp(b + fabs(min), lmin, lmax);


  /* select the blend operator */
  switch (mode)
  {
    case DEVELOP_BLEND_LIGHTEN:
      o =  clamp(a * (1.0f - opacity) + fmax(a, b) * opacity, min, max);
      break;

    case DEVELOP_BLEND_DARKEN:
      o =  clamp(a * (1.0f - opacity) + fmin(a, b) * opacity, min, max);
      break;

    case DEVELOP_BLEND_MULTIPLY:
      o = clamp(a * (1.0f - opacity) + a * b * opacity, min, max);
      break;

    case DEVELOP_BLEND_AVERAGE:
      o =  clamp(a * (1.0f - opacity) + (a + b)/2.0f * opacity, min, max);
      break;

    case DEVELOP_BLEND_ADD:
      o =  clamp(a * (1.0f - opacity) +  (a + b) * opacity, min, max);
      break;

    case DEVELOP_BLEND_SUBSTRACT:
      o =  clamp(a * (1.0f - opacity) +  (b + a - fabs(min + max)) * opacity, min, max);
      break;

    case DEVELOP_BLEND_DIFFERENCE:
      o = clamp(la * (1.0f - opacity) + fabs(la - lb) * opacity, lmin, lmax) - fabs(min);
      break;

    case DEVELOP_BLEND_SCREEN:
      o = clamp(la * (1.0f - opacity) + (lmax - (lmax - la) * (lmax - lb)) * opacity, lmin, lmax) - fabs(min);
      break;

    case DEVELOP_BLEND_OVERLAY:
      o = clamp(la * (1.0f - opacity2) + (la > halfmax ? lmax - (lmax - doublemax * (la - halfmax)) * (lmax-lb) : doublemax * la * lb) * opacity2, lmin, lmax) - fabs(min);
      break;

    case DEVELOP_BLEND_SOFTLIGHT:
      o = clamp(la * (1.0f - opacity2) + (lb > halfmax ? lmax - (lmax - la)  * (lmax - (lb - halfmax)) : la * (lb + halfmax)) * opacity2, lmin, lmax) - fabs(min);
      break;

    case DEVELOP_BLEND_HARDLIGHT:
      o = clamp(la * (1.0f - opacity2) + (lb > halfmax ? lmax - (lmax - doublemax * (la - halfmax)) * (lmax-lb) : doublemax * la * lb) * opacity2, lmin, lmax) - fabs(min);
      break;

    case DEVELOP_BLEND_VIVIDLIGHT:
      o = clamp(la * (1.0f - opacity2) + (lb > halfmax ? la / (doublemax * (lmax - lb)) : lmax - (lmax - la)/(doublemax * lb)) * opacity2, lmin, lmax) - fabs(min);
      break;

    case DEVELOP_BLEND_LINEARLIGHT:
      o = clamp(la * (1.0f - opacity2) + (la + doublemax * lb - lmax) * opacity2, lmin, lmax) - fabs(min);
      break;

    case DEVELOP_BLEND_PINLIGHT:
      o = clamp(la * (1.0f - opacity2) + (lb > halfmax ? fmax(la, doublemax * (lb - halfmax)) : fmin(la, doublemax * lb)) * opacity2, lmin, lmax) - fabs(min);
      break;


    /* fallback to normal blend */
    case DEVELOP_BLEND_NORMAL:
    default:
      o =  (a * (1.0f - opacity)) + (b * opacity);
      break;
  }

  write_imagef(out, (int2)(x, y), o);
}


