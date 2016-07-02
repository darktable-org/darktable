/*
    This file is part of darktable,
    copyright (c) 2011--2013 ulrich pegelow.

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
#include "colorspace.cl"


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
#define DEVELOP_BLEND_LIGHTNESS				0x10
#define DEVELOP_BLEND_CHROMA				0x11
#define DEVELOP_BLEND_HUE				0x12
#define DEVELOP_BLEND_COLOR				0x13
#define DEVELOP_BLEND_INVERSE				0x14
#define DEVELOP_BLEND_UNBOUNDED                         0x15
#define DEVELOP_BLEND_COLORADJUST                       0x16
#define DEVELOP_BLEND_DIFFERENCE2                       0x17
#define DEVELOP_BLEND_NORMAL2                           0x18
#define DEVELOP_BLEND_BOUNDED                           0x19
#define DEVELOP_BLEND_LAB_LIGHTNESS                     0x1A
#define DEVELOP_BLEND_LAB_COLOR                         0x1B
#define DEVELOP_BLEND_HSV_LIGHTNESS                     0x1C
#define DEVELOP_BLEND_HSV_COLOR                         0x1D


#define DEVELOP_MASK_DISABLED       0x00
#define DEVELOP_MASK_ENABLED        0x01
#define DEVELOP_MASK_MASK           0x02
#define DEVELOP_MASK_CONDITIONAL    0x04
#define DEVELOP_MASK_BOTH           (DEVELOP_MASK_MASK | DEVELOP_MASK_CONDITIONAL)

#define DEVELOP_COMBINE_NORM        0x00
#define DEVELOP_COMBINE_INV         0x01
#define DEVELOP_COMBINE_EXCL        0x00
#define DEVELOP_COMBINE_INCL        0x02
#define DEVELOP_COMBINE_MASKS_POS   0x04
#define DEVELOP_COMBINE_NORM_EXCL   (DEVELOP_COMBINE_NORM | DEVELOP_COMBINE_EXCL)
#define DEVELOP_COMBINE_NORM_INCL   (DEVELOP_COMBINE_NORM | DEVELOP_COMBINE_INCL)
#define DEVELOP_COMBINE_INV_EXCL    (DEVELOP_COMBINE_INV | DEVELOP_COMBINE_EXCL)
#define DEVELOP_COMBINE_INV_INCL    (DEVELOP_COMBINE_INV | DEVELOP_COMBINE_INCL)

#define BLEND_ONLY_LIGHTNESS				8

typedef enum iop_cs_t 
{
  iop_cs_Lab, 
  iop_cs_rgb, 
  iop_cs_RAW
} iop_cs_t;


typedef enum dt_develop_blendif_channels_t
{
  DEVELOP_BLENDIF_L_in      = 0,
  DEVELOP_BLENDIF_A_in      = 1,
  DEVELOP_BLENDIF_B_in      = 2,

  DEVELOP_BLENDIF_L_out     = 4,
  DEVELOP_BLENDIF_A_out     = 5,
  DEVELOP_BLENDIF_B_out     = 6,

  DEVELOP_BLENDIF_GRAY_in   = 0,
  DEVELOP_BLENDIF_RED_in    = 1,
  DEVELOP_BLENDIF_GREEN_in  = 2,
  DEVELOP_BLENDIF_BLUE_in   = 3,

  DEVELOP_BLENDIF_GRAY_out  = 4,
  DEVELOP_BLENDIF_RED_out   = 5,
  DEVELOP_BLENDIF_GREEN_out = 6,
  DEVELOP_BLENDIF_BLUE_out  = 7,

  DEVELOP_BLENDIF_C_in      = 8,
  DEVELOP_BLENDIF_h_in      = 9,

  DEVELOP_BLENDIF_C_out     = 12,
  DEVELOP_BLENDIF_h_out     = 13,

  DEVELOP_BLENDIF_H_in      = 8,
  DEVELOP_BLENDIF_S_in      = 9,
  DEVELOP_BLENDIF_l_in      = 10,

  DEVELOP_BLENDIF_H_out     = 12,
  DEVELOP_BLENDIF_S_out     = 13,
  DEVELOP_BLENDIF_l_out     = 14,

  DEVELOP_BLENDIF_MAX       = 14,
  DEVELOP_BLENDIF_unused    = 15,

  DEVELOP_BLENDIF_active    = 31,

  DEVELOP_BLENDIF_SIZE      = 16,

  DEVELOP_BLENDIF_Lab_MASK  = 0x3377,
  DEVELOP_BLENDIF_RGB_MASK  = 0x77FF
}
dt_develop_blendif_channels_t;



float
blendif_factor_Lab(const float4 input, const float4 output, const unsigned int blendif, global const float *parameters, const unsigned int mask_mode, const unsigned int mask_combine)
{
  float result = 1.0f;
  float scaled[DEVELOP_BLENDIF_SIZE];

  if(!(mask_mode & DEVELOP_MASK_CONDITIONAL)) return (mask_combine & DEVELOP_COMBINE_INCL) ? 0.0f : 1.0f;

  scaled[DEVELOP_BLENDIF_L_in] = clamp(input.x / 100.0f, 0.0f, 1.0f);			// L scaled to 0..1
  scaled[DEVELOP_BLENDIF_A_in] = clamp((input.y + 128.0f)/256.0f, 0.0f, 1.0f);		// a scaled to 0..1
  scaled[DEVELOP_BLENDIF_B_in] = clamp((input.z + 128.0f)/256.0f, 0.0f, 1.0f);		// b scaled to 0..1

  scaled[DEVELOP_BLENDIF_L_out] = clamp(output.x / 100.0f, 0.0f, 1.0f);			// L scaled to 0..1
  scaled[DEVELOP_BLENDIF_A_out] = clamp((output.y + 128.0f)/256.0f, 0.0f, 1.0f);		// a scaled to 0..1
  scaled[DEVELOP_BLENDIF_B_out] = clamp((output.z + 128.0f)/256.0f, 0.0f, 1.0f);		// b scaled to 0..1


  if((blendif & 0x7f00) != 0)  // do we need to consider LCh ?
  {
    float4 LCH_input = Lab_2_LCH(input);
    float4 LCH_output = Lab_2_LCH(output);

    scaled[DEVELOP_BLENDIF_C_in] = clamp(LCH_input.y / (128.0f*sqrt(2.0f)), 0.0f, 1.0f);        // C scaled to 0..1
    scaled[DEVELOP_BLENDIF_h_in] = clamp(LCH_input.z, 0.0f, 1.0f);		                // h scaled to 0..1

    scaled[DEVELOP_BLENDIF_C_out] = clamp(LCH_output.y / (128.0f*sqrt(2.0f)), 0.0f, 1.0f);       // C scaled to 0..1
    scaled[DEVELOP_BLENDIF_h_out] = clamp(LCH_output.z, 0.0f, 1.0f);		                // h scaled to 0..1
  }


  for(int ch=0; ch<=DEVELOP_BLENDIF_MAX; ch++)
  {
    if((DEVELOP_BLENDIF_Lab_MASK & (1<<ch)) == 0) continue;       // skip blendif channels not used in this color space

    if((blendif & (1<<ch)) == 0)                                  // deal with channels where sliders span the whole range
    {
      result *= (!(blendif & (1<<(ch+16)))) == (!(mask_combine & DEVELOP_COMBINE_INCL)) ? 1.0f : 0.0f;
      continue;
    }

    if(result <= 0.000001f) break;				// no need to continue if we are already close to or at zero

    float factor;

    if      (scaled[ch] >= parameters[4*ch+1] && scaled[ch] <= parameters[4*ch+2])
    {
      factor = 1.0f;
    }
    else if (scaled[ch] >  parameters[4*ch+0] && scaled[ch] <  parameters[4*ch+1])
    {
      factor = (scaled[ch] - parameters[4*ch+0])/fmax(0.01f, parameters[4*ch+1]-parameters[4*ch+0]);
    }
    else if (scaled[ch] >  parameters[4*ch+2] && scaled[ch] <  parameters[4*ch+3])
    {
      factor = 1.0f - (scaled[ch] - parameters[4*ch+2])/fmax(0.01f, parameters[4*ch+3]-parameters[4*ch+2]);
    }
    else factor = 0.0f;

    if((blendif & (1<<(ch+16))) != 0) factor = 1.0f - factor;  // inverted channel

    result *= ((mask_combine & DEVELOP_COMBINE_INCL) ? 1.0f - factor : factor);
  }

  return (mask_combine & DEVELOP_COMBINE_INCL) ? 1.0f - result : result;
}


float
blendif_factor_rgb(const float4 input, const float4 output, const unsigned int blendif, global const float *parameters, const unsigned int mask_mode, const unsigned int mask_combine)
{
  float result = 1.0f;
  float scaled[DEVELOP_BLENDIF_SIZE];

  if(!(mask_mode & DEVELOP_MASK_CONDITIONAL)) return (mask_combine & DEVELOP_COMBINE_INCL) ? 0.0f : 1.0f;

  scaled[DEVELOP_BLENDIF_GRAY_in]  = clamp(0.3f*input.x + 0.59f*input.y + 0.11f*input.z, 0.0f, 1.0f);	// Gray scaled to 0..1
  scaled[DEVELOP_BLENDIF_RED_in]   = clamp(input.x, 0.0f, 1.0f);						// Red
  scaled[DEVELOP_BLENDIF_GREEN_in] = clamp(input.y, 0.0f, 1.0f);						// Green
  scaled[DEVELOP_BLENDIF_BLUE_in]  = clamp(input.z, 0.0f, 1.0f);						// Blue
  scaled[DEVELOP_BLENDIF_GRAY_out]  = clamp(0.3f*output.x + 0.59f*output.y + 0.11f*output.z, 0.0f, 1.0f);	// Gray scaled to 0..1
  scaled[DEVELOP_BLENDIF_RED_out]   = clamp(output.x, 0.0f, 1.0f);						// Red
  scaled[DEVELOP_BLENDIF_GREEN_out] = clamp(output.y, 0.0f, 1.0f);						// Green
  scaled[DEVELOP_BLENDIF_BLUE_out]  = clamp(output.z, 0.0f, 1.0f);						// Blue

  if((blendif & 0x7f00) != 0)  // do we need to consider HSL ?
  {
    float4 HSL_input = RGB_2_HSL(input);
    float4 HSL_output = RGB_2_HSL(output);

    scaled[DEVELOP_BLENDIF_H_in] = clamp(HSL_input.x, 0.0f, 1.0f);			        // H scaled to 0..1
    scaled[DEVELOP_BLENDIF_S_in] = clamp(HSL_input.y, 0.0f, 1.0f);		                // S scaled to 0..1
    scaled[DEVELOP_BLENDIF_l_in] = clamp(HSL_input.z, 0.0f, 1.0f);		                // L scaled to 0..1

    scaled[DEVELOP_BLENDIF_H_out] = clamp(HSL_output.x, 0.0f, 1.0f);			        // H scaled to 0..1
    scaled[DEVELOP_BLENDIF_S_out] = clamp(HSL_output.y, 0.0f, 1.0f);		                // S scaled to 0..1
    scaled[DEVELOP_BLENDIF_l_out] = clamp(HSL_output.z, 0.0f, 1.0f);		                // L scaled to 0..1
  }


  for(int ch=0; ch<=DEVELOP_BLENDIF_MAX; ch++)
  {
    if((DEVELOP_BLENDIF_RGB_MASK & (1<<ch)) == 0) continue;       // skip blendif channels not used in this color space

    if((blendif & (1<<ch)) == 0)                                  // deal with channels where sliders span the whole range
    {
      result *= (!(blendif & (1<<(ch+16)))) == (!(mask_combine & DEVELOP_COMBINE_INCL)) ? 1.0f : 0.0f;
      continue;
    }

    if(result <= 0.000001f) break;				// no need to continue if we are already close to or at zero

    float factor;
    if      (scaled[ch] >= parameters[4*ch+1] && scaled[ch] <= parameters[4*ch+2])
    {
      factor = 1.0f;
    }
    else if (scaled[ch] >  parameters[4*ch+0] && scaled[ch] <  parameters[4*ch+1])
    {
      factor = (scaled[ch] - parameters[4*ch+0])/fmax(0.01f, parameters[4*ch+1]-parameters[4*ch+0]);
    }
    else if (scaled[ch] >  parameters[4*ch+2] && scaled[ch] <  parameters[4*ch+3])
    {
      factor = 1.0f - (scaled[ch] - parameters[4*ch+2])/fmax(0.01f, parameters[4*ch+3]-parameters[4*ch+2]);
    }
    else factor = 0.0f;

    if((blendif & (1<<(ch+16))) != 0) factor = 1.0f - factor;  // inverted channel

    result *= ((mask_combine & DEVELOP_COMBINE_INCL) ? 1.0f - factor : factor);
  }

  return (mask_combine & DEVELOP_COMBINE_INCL) ? 1.0f - result : result;
}

__kernel void
blendop_mask_Lab (__read_only image2d_t in_a, __read_only image2d_t in_b, __read_only image2d_t mask_in, __write_only image2d_t mask, const int width, const int height, 
             const float gopacity, const int blendif, global const float *blendif_parameters, const unsigned int mask_mode, const unsigned int mask_combine, const int2 offs)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 a = read_imagef(in_a, sampleri, (int2)(x, y) + offs);
  float4 b = read_imagef(in_b, sampleri, (int2)(x, y));
  float form = read_imagef(mask_in, sampleri, (int2)(x, y)).x;

  float conditional = blendif_factor_Lab(a, b, blendif, blendif_parameters, mask_mode, mask_combine);
  
  float opacity = (mask_combine & DEVELOP_COMBINE_INCL) ? 1.0f - (1.0f - form) * (1.0f - conditional) : form * conditional ;
  opacity = (mask_combine & DEVELOP_COMBINE_INV) ? 1.0f - opacity : opacity;

  write_imagef(mask, (int2)(x, y), gopacity*opacity);
}

#if 0
__kernel void
blendop_mask_RAW (__read_only image2d_t in_a, __read_only image2d_t in_b, __read_only image2d_t mask_in, __write_only image2d_t mask, const int width, const int height, 
             const float gopacity, const int blendif, global const float *blendif_parameters, const unsigned int mask_mode, const unsigned int mask_combine, const int2 offs)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;
  float form = read_imagef(mask_in, sampleri, (int2)(x, y)).x;

  float opacity = (mask_combine & DEVELOP_COMBINE_INV) ? 1.0f - form : form;
    
  write_imagef(mask, (int2)(x, y), gopacity*opacity);
}
#else
// the following is a workaround for a current bug (as of Nov. 2012) in NVIDIA's OpenCL compiler, affecting GeForce GT6xx gpus.
// the above original kernel would simply not compile. the code below is functionally equivalent, a bit slower, and complicated enough
// to trick the compiler.
// thanks to Jens Fendler for finding this workaround.
// TODO: review after some time (May 2013) if this is still needed.
__kernel void
blendop_mask_RAW (__read_only image2d_t in_a, __read_only image2d_t in_b, __read_only image2d_t mask_in, __write_only image2d_t mask, const int width, const int height, 
             const float gopacity, const int blendif, global const float *blendif_parameters, const unsigned int mask_mode, const unsigned int mask_combine, const int2 offs)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 a = read_imagef(in_a, sampleri, (int2)(x, y) + offs);
  float4 b = read_imagef(in_b, sampleri, (int2)(x, y));
  float form = read_imagef(mask_in, sampleri, (int2)(x, y)).x;
  
  float bif = blendif_factor_Lab(a, b, blendif, blendif_parameters, DEVELOP_MASK_DISABLED, DEVELOP_COMBINE_EXCL);

  float opacity = ((mask_combine & DEVELOP_COMBINE_INV) ? 1.0f - form : form)*bif;
  opacity /= bif;

  write_imagef(mask, (int2)(x, y), gopacity*opacity);
}
#endif

__kernel void
blendop_mask_rgb (__read_only image2d_t in_a, __read_only image2d_t in_b, __read_only image2d_t mask_in, __write_only image2d_t mask, const int width, const int height, 
             const float gopacity, const int blendif, global const float *blendif_parameters, const unsigned int mask_mode, const unsigned int mask_combine, const int2 offs)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 a = read_imagef(in_a, sampleri, (int2)(x, y) + offs);
  float4 b = read_imagef(in_b, sampleri, (int2)(x, y));
  float form = read_imagef(mask_in, sampleri, (int2)(x, y)).x;

  float conditional = blendif_factor_rgb(a, b, blendif, blendif_parameters, mask_mode, mask_combine);
  
  float opacity = (mask_combine & DEVELOP_COMBINE_INCL) ? 1.0f - (1.0f - form) * (1.0f - conditional) : form * conditional ;
  opacity = (mask_combine & DEVELOP_COMBINE_INV) ? 1.0f - opacity : opacity;
  
  write_imagef(mask, (int2)(x, y), gopacity*opacity);
}

__kernel void
blendop_Lab (__read_only image2d_t in_a, __read_only image2d_t in_b, __read_only image2d_t mask, __write_only image2d_t out, const int width, const int height, 
             const int blend_mode, const int blendflag, const int2 offs, const int mask_display)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 o;
  float4 ta, tb, to;
  float d, s;

  float4 a = read_imagef(in_a, sampleri, (int2)(x, y) + offs);
  float4 b = read_imagef(in_b, sampleri, (int2)(x, y));
  float opacity = read_imagef(mask, sampleri, (int2)(x, y)).x;

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
  switch (blend_mode)
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

    case DEVELOP_BLEND_DIFFERENCE2:
      to = fabs(a - b) / fabs(max - min);
      to.x = fmax(to.x, fmax(to.y, to.z));
      o = clamp(la * (1.0f - opacity) + to * opacity, lmin, lmax);
      o.y = o.z = 0.0f;
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
      o = clamp(la * (1.0f - opacity2) + (lb > halfmax ? (lb >= lmax ? lmax : la / (doublemax * (lmax - lb))) : (lb <= lmin ? lmin : lmax - (lmax - la)/(doublemax * lb))) * opacity2, lmin, lmax) - fabs(min);
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
      o.y = clamp(a.y, min.y, max.y);
      o.z = clamp(a.z, min.z, max.z);
      break;

    case DEVELOP_BLEND_LIGHTNESS:
      // no need to transfer to LCH as we only work on L, which is the same as in Lab
      o.x = clamp((a.x * (1.0f - opacity)) + (b.x * opacity), min.x, max.x);
      o.y = clamp(a.y, min.y, max.y);
      o.z = clamp(a.z, min.z, max.z);
      break;

    case DEVELOP_BLEND_CHROMA:
      ta = Lab_2_LCH(clamp(a, min, max));
      tb = Lab_2_LCH(clamp(b, min, max));
      to.x = ta.x;
      to.y = (ta.y * (1.0f - opacity)) + (tb.y * opacity);
      to.z = ta.z;
      o = clamp(LCH_2_Lab(to), min, max);;
      break;

    case DEVELOP_BLEND_HUE:
      ta = Lab_2_LCH(clamp(a, min, max));
      tb = Lab_2_LCH(clamp(b, min, max));
      to.x = ta.x;
      to.y = ta.y;
      d = fabs(ta.z - tb.z);
      s = d > 0.5f ? -opacity*(1.0f - d) / d : opacity;
      to.z = fmod((ta.z * (1.0f - s)) + (tb.z * s) + 1.0f, 1.0f);
      o = clamp(LCH_2_Lab(to), min, max);
      break;

    case DEVELOP_BLEND_COLOR:
      ta = Lab_2_LCH(clamp(a, min, max));
      tb = Lab_2_LCH(clamp(b, min, max));
      to.x = ta.x;
      to.y = (ta.y * (1.0f - opacity)) + (tb.y * opacity);
      d = fabs(ta.z - tb.z);
      s = d > 0.5f ? -opacity*(1.0f - d) / d : opacity;
      to.z = fmod((ta.z * (1.0f - s)) + (tb.z * s) + 1.0f, 1.0f);
      o = clamp(LCH_2_Lab(to), min, max);
      break;

    case DEVELOP_BLEND_COLORADJUST:
      ta = Lab_2_LCH(clamp(a, min, max));
      tb = Lab_2_LCH(clamp(b, min, max));
      to.x = tb.x;
      to.y = (ta.y * (1.0f - opacity)) + (tb.y * opacity);
      d = fabs(ta.z - tb.z);
      s = d > 0.5f ? -opacity*(1.0f - d) / d : opacity;
      to.z = fmod((ta.z * (1.0f - s)) + (tb.z * s) + 1.0f, 1.0f);
      o = clamp(LCH_2_Lab(to), min, max);
      break;

    case DEVELOP_BLEND_INVERSE:
      o =  clamp((a * opacity) + (b * (1.0f - opacity)), min, max);
      break;

    case DEVELOP_BLEND_BOUNDED:
    case DEVELOP_BLEND_NORMAL:
      o =  clamp((a * (1.0f - opacity)) + (b * opacity), min, max);
      break;

    case DEVELOP_BLEND_LAB_LIGHTNESS:
      o.x = (a.x * (1.0f - opacity)) + (b.x * opacity);
      o.y = a.y;
      o.z = a.z;
      break;

    case DEVELOP_BLEND_LAB_COLOR:
      o.x = a.x;
      o.y = (a.y * (1.0f - opacity)) + (b.y * opacity);
      o.z = (a.z * (1.0f - opacity)) + (b.z * opacity);
      break;

    case DEVELOP_BLEND_HSV_LIGHTNESS:
    case DEVELOP_BLEND_HSV_COLOR:
      o = a;                            // Noop for Lab (without clamping)
      break;

    /* fallback to normal blend */
    case DEVELOP_BLEND_UNBOUNDED:
    case DEVELOP_BLEND_NORMAL2:
    default:
      o =  (a * (1.0f - opacity)) + (b * opacity);
      break;

  }

  /* scale L back to [0; 100] and a,b to [-128; 128] */
  o *= scale;

  /* we transfer alpha channel of input if mask_display is set, else we save opacity into alpha channel */
  o.w = mask_display ? a.w : opacity;

  /* if module wants to blend only lightness, set a and b to values of input image (saved before scaling) */
  if (blendflag & BLEND_ONLY_LIGHTNESS)
  {
    o.y = ay;
    o.z = az;
  }

  write_imagef(out, (int2)(x, y), o);
}



__kernel void
blendop_RAW (__read_only image2d_t in_a, __read_only image2d_t in_b, __read_only image2d_t mask, __write_only image2d_t out, const int width, const int height, 
             const int blend_mode, const int blendflag, const int2 offs, const int mask_display)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float o;

  float a = read_imagef(in_a, sampleri, (int2)(x, y) + offs).x;
  float b = read_imagef(in_b, sampleri, (int2)(x, y)).x;
  float opacity = read_imagef(mask, sampleri, (int2)(x, y)).x;

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
  switch (blend_mode)
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
    case DEVELOP_BLEND_DIFFERENCE2:
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
      o = clamp(la * (1.0f - opacity2) + (lb > halfmax ? la / (lb >= lmax ? lmax : (doublemax * (lmax - lb))) : (lb <= lmin ? lmin : lmax - (lmax - la)/(doublemax * lb))) * opacity2, lmin, lmax) - fabs(min);
      break;

    case DEVELOP_BLEND_LINEARLIGHT:
      o = clamp(la * (1.0f - opacity2) + (la + doublemax * lb - lmax) * opacity2, lmin, lmax) - fabs(min);
      break;

    case DEVELOP_BLEND_PINLIGHT:
      o = clamp(la * (1.0f - opacity2) + (lb > halfmax ? fmax(la, doublemax * (lb - halfmax)) : fmin(la, doublemax * lb)) * opacity2, lmin, lmax) - fabs(min);
      break;

    case DEVELOP_BLEND_LIGHTNESS:
      o = clamp(a, min, max);		// Noop for Raw
      break;

    case DEVELOP_BLEND_CHROMA:
      o = clamp(a, min, max);		// Noop for Raw
      break;

    case DEVELOP_BLEND_HUE:
      o = clamp(a, min, max);		// Noop for Raw
      break;

    case DEVELOP_BLEND_COLOR:
      o = clamp(a, min, max);		// Noop for Raw
      break;

    case DEVELOP_BLEND_COLORADJUST:
      o = clamp(a, min, max);		// Noop for Raw
      break;

    case DEVELOP_BLEND_INVERSE:
      o =  clamp((a * opacity) + (b * (1.0f - opacity)), min, max);
      break;

    case DEVELOP_BLEND_BOUNDED:
    case DEVELOP_BLEND_NORMAL:
      o =  clamp((a * (1.0f - opacity)) + (b * opacity), min, max);
      break;

    case DEVELOP_BLEND_LAB_LIGHTNESS:
    case DEVELOP_BLEND_LAB_COLOR:
      o = a;                            // Noop for Raw (without clamping)
      break;

    case DEVELOP_BLEND_HSV_LIGHTNESS:
    case DEVELOP_BLEND_HSV_COLOR:
      o = a;                            // Noop for Raw (without clamping)
      break;

    /* fallback to normal blend */
    case DEVELOP_BLEND_UNBOUNDED:
    case DEVELOP_BLEND_NORMAL2:
    default:
      o =  (a * (1.0f - opacity)) + (b * opacity);
      break;

  }

  write_imagef(out, (int2)(x, y), (float4)(o, 0.0f, 0.0f, 0.0f));
}


__kernel void
blendop_rgb (__read_only image2d_t in_a, __read_only image2d_t in_b, __read_only image2d_t mask, __write_only image2d_t out, const int width, const int height, 
             const int blend_mode, const int blendflag, const int2 offs, const int mask_display)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 o;
  float4 ta, tb, to;
  float d, s;

  float4 a = read_imagef(in_a, sampleri, (int2)(x, y) + offs);
  float4 b = read_imagef(in_b, sampleri, (int2)(x, y));
  float opacity = read_imagef(mask, sampleri, (int2)(x, y)).x;

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
  switch (blend_mode)
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
    case DEVELOP_BLEND_DIFFERENCE2:
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
      o = clamp(la * (1.0f - opacity2) + (lb > halfmax ? (lb >= lmax ? lmax : la / (doublemax * (lmax - lb))) : (lb <= lmin ? lmin : lmax - (lmax - la)/(doublemax * lb))) * opacity2, lmin, lmax) - fabs(min);
      break;

    case DEVELOP_BLEND_LINEARLIGHT:
      o = clamp(la * (1.0f - opacity2) + (la + doublemax * lb - lmax) * opacity2, lmin, lmax) - fabs(min);
      break;

    case DEVELOP_BLEND_PINLIGHT:
      o = clamp(la * (1.0f - opacity2) + (lb > halfmax ? fmax(la, doublemax * (lb - halfmax)) : fmin(la, doublemax * lb)) * opacity2, lmin, lmax) - fabs(min);
      break;

    case DEVELOP_BLEND_LIGHTNESS:
      ta = RGB_2_HSL(clamp(a, min, max));
      tb = RGB_2_HSL(clamp(b, min, max));
      to.x = ta.x;
      to.y = ta.y;
      to.z = (ta.z * (1.0f - opacity)) + (tb.z * opacity);
      o = clamp(HSL_2_RGB(to), min, max);
      break;

    case DEVELOP_BLEND_CHROMA:
      ta = RGB_2_HSL(clamp(a, min, max));
      tb = RGB_2_HSL(clamp(b, min, max));
      to.x = ta.x;
      to.y = (ta.y * (1.0f - opacity)) + (tb.y * opacity);
      to.z = ta.z;
      o = clamp(HSL_2_RGB(to), min, max);
      break;

    case DEVELOP_BLEND_HUE:
      ta = RGB_2_HSL(clamp(a, min, max));
      tb = RGB_2_HSL(clamp(b, min, max));
      d = fabs(ta.x - tb.x);
      s = d > 0.5f ? -opacity*(1.0f - d) / d : opacity;
      to.x = fmod((ta.x * (1.0f - s)) + (tb.x * s) + 1.0f, 1.0f);
      to.y = ta.y;
      to.z = ta.z;
      o = clamp(HSL_2_RGB(to), min, max);;
      break;

    case DEVELOP_BLEND_COLOR:
      ta = RGB_2_HSL(clamp(a, min, max));
      tb = RGB_2_HSL(clamp(b, min, max));
      d = fabs(ta.x - tb.x);
      s = d > 0.5f ? -opacity*(1.0f - d) / d : opacity;
      to.x = fmod((ta.x * (1.0f - s)) + (tb.x * s) + 1.0f, 1.0f);
      to.y = (ta.y * (1.0f - opacity)) + (tb.y * opacity);
      to.z = ta.z;
      o = clamp(HSL_2_RGB(to), min, max);
      break;

    case DEVELOP_BLEND_COLORADJUST:
      ta = RGB_2_HSL(clamp(a, min, max));
      tb = RGB_2_HSL(clamp(b, min, max));
      d = fabs(ta.x - tb.x);
      s = d > 0.5f ? -opacity*(1.0f - d) / d : opacity;
      to.x = fmod((ta.x * (1.0f - s)) + (tb.x * s) + 1.0f, 1.0f);
      to.y = (ta.y * (1.0f - opacity)) + (tb.y * opacity);
      to.z = tb.z;
      o = clamp(HSL_2_RGB(to), min, max);
      break;

    case DEVELOP_BLEND_INVERSE:
      o =  clamp((a * opacity) + (b * (1.0f - opacity)), min, max);
      break;

    case DEVELOP_BLEND_BOUNDED:
    case DEVELOP_BLEND_NORMAL:
      o =  clamp((a * (1.0f - opacity)) + (b * opacity), min, max);
      break;

    case DEVELOP_BLEND_LAB_LIGHTNESS:
    case DEVELOP_BLEND_LAB_COLOR:
      o = a;                            // Noop for RGB (without clamping)
      break;

    case DEVELOP_BLEND_HSV_LIGHTNESS:
      ta = RGB_2_HSV(a);
      tb = RGB_2_HSV(b);
      to.x = ta.x;
      to.y = ta.y;
      to.z = (ta.z * (1.0f - opacity)) + (tb.z * opacity);
      o = HSV_2_RGB(to);
      break;

    case DEVELOP_BLEND_HSV_COLOR:
      ta = RGB_2_HSV(a);
      tb = RGB_2_HSV(b);
      // blend color vectors of input and output
      d = ta.y*cos(2.0f*M_PI_F*ta.x) * (1.0f - opacity) + tb.y*cos(2.0f*M_PI_F*tb.x) * opacity;
      s = ta.y*sin(2.0f*M_PI_F*ta.x) * (1.0f - opacity) + tb.y*sin(2.0f*M_PI_F*tb.x) * opacity;
      to.x = fmod(atan2(s, d)/(2.0f*M_PI_F)+1.0f, 1.0f);
      to.y = sqrt(s*s + d*d);
      to.z = ta.z;
      o = HSV_2_RGB(to);
      break;

    /* fallback to normal blend */
    case DEVELOP_BLEND_UNBOUNDED:
    case DEVELOP_BLEND_NORMAL2:
    default:
      o =  (a * (1.0f - opacity)) + (b * opacity);
      break;

  }

  /* we transfer alpha channel of input if mask_display is set, else we save opacity into alpha channel */
  o.w = mask_display ? a.w : opacity;

  write_imagef(out, (int2)(x, y), o);
}

__kernel void
blendop_set_mask (__write_only image2d_t mask, const int width, const int height, const float value)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  write_imagef(mask, (int2)(x, y), value);
}



