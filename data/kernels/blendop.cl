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

#include "colorspace.h"
#include "color_conversion.h"
#include "common.h"

typedef enum dt_develop_blend_mode_t
{
  DEVELOP_BLEND_DISABLED_OBSOLETE = 0x00, /* same as the new normal */
  DEVELOP_BLEND_NORMAL_OBSOLETE = 0x01, /* obsolete as it did clamping */
  DEVELOP_BLEND_LIGHTEN = 0x02,
  DEVELOP_BLEND_DARKEN = 0x03,
  DEVELOP_BLEND_MULTIPLY = 0x04,
  DEVELOP_BLEND_AVERAGE = 0x05,
  DEVELOP_BLEND_ADD = 0x06,
  DEVELOP_BLEND_SUBTRACT = 0x07,
  DEVELOP_BLEND_DIFFERENCE = 0x08, /* deprecated */
  DEVELOP_BLEND_SCREEN = 0x09,
  DEVELOP_BLEND_OVERLAY = 0x0A,
  DEVELOP_BLEND_SOFTLIGHT = 0x0B,
  DEVELOP_BLEND_HARDLIGHT = 0x0C,
  DEVELOP_BLEND_VIVIDLIGHT = 0x0D,
  DEVELOP_BLEND_LINEARLIGHT = 0x0E,
  DEVELOP_BLEND_PINLIGHT = 0x0F,
  DEVELOP_BLEND_LIGHTNESS = 0x10,
  DEVELOP_BLEND_CHROMA = 0x11,
  DEVELOP_BLEND_HUE = 0x12,
  DEVELOP_BLEND_COLOR = 0x13,
  DEVELOP_BLEND_INVERSE_OBSOLETE = 0x14, /* obsolete */
  DEVELOP_BLEND_UNBOUNDED_OBSOLETE = 0x15, /* obsolete as new normal takes over */
  DEVELOP_BLEND_COLORADJUST = 0x16,
  DEVELOP_BLEND_DIFFERENCE2 = 0x17,
  DEVELOP_BLEND_NORMAL2 = 0x18,
  DEVELOP_BLEND_BOUNDED = 0x19,
  DEVELOP_BLEND_LAB_LIGHTNESS = 0x1A,
  DEVELOP_BLEND_LAB_COLOR = 0x1B,
  DEVELOP_BLEND_HSV_LIGHTNESS = 0x1C,
  DEVELOP_BLEND_HSV_COLOR = 0x1D,
  DEVELOP_BLEND_LAB_L = 0x1E,
  DEVELOP_BLEND_LAB_A = 0x1F,
  DEVELOP_BLEND_LAB_B = 0x20,
  DEVELOP_BLEND_RGB_R = 0x21,
  DEVELOP_BLEND_RGB_G = 0x22,
  DEVELOP_BLEND_RGB_B = 0x23,
  DEVELOP_BLEND_MULTIPLY_REVERSE_OBSOLETE = 0x24, /* obsoleted by MULTIPLY + REVERSE */
  DEVELOP_BLEND_SUBTRACT_INVERSE = 0x25,
  DEVELOP_BLEND_DIVIDE = 0x26,
  DEVELOP_BLEND_DIVIDE_INVERSE = 0x27,
  DEVELOP_BLEND_GEOMETRIC_MEAN = 0x28,
  DEVELOP_BLEND_HARMONIC_MEAN = 0x29,

  DEVELOP_BLEND_REVERSE = 0x80000000,
  DEVELOP_BLEND_MODE_MASK = 0xFF,
} dt_develop_blend_mode_t;

typedef enum dt_develop_mask_mode_t
{
  DEVELOP_MASK_DISABLED = 0x00,
  DEVELOP_MASK_ENABLED = 0x01,
  DEVELOP_MASK_MASK = 0x02,
  DEVELOP_MASK_CONDITIONAL = 0x04,
  DEVELOP_MASK_BOTH = (DEVELOP_MASK_MASK | DEVELOP_MASK_CONDITIONAL)
} dt_develop_mask_mode_t;

typedef enum dt_develop_mask_combine_mode_t
{
  DEVELOP_COMBINE_NORM = 0x00,
  DEVELOP_COMBINE_INV = 0x01,
  DEVELOP_COMBINE_EXCL = 0x00,
  DEVELOP_COMBINE_INCL = 0x02,
  DEVELOP_COMBINE_MASKS_POS = 0x04,
  DEVELOP_COMBINE_NORM_EXCL = (DEVELOP_COMBINE_NORM | DEVELOP_COMBINE_EXCL),
  DEVELOP_COMBINE_NORM_INCL = (DEVELOP_COMBINE_NORM | DEVELOP_COMBINE_INCL),
  DEVELOP_COMBINE_INV_EXCL = (DEVELOP_COMBINE_INV | DEVELOP_COMBINE_EXCL),
  DEVELOP_COMBINE_INV_INCL = (DEVELOP_COMBINE_INV | DEVELOP_COMBINE_INCL)
} dt_develop_mask_combine_mode_t;


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

  DEVELOP_BLENDIF_Jz_in     = 8,
  DEVELOP_BLENDIF_Cz_in     = 9,
  DEVELOP_BLENDIF_hz_in     = 10,

  DEVELOP_BLENDIF_Jz_out    = 12,
  DEVELOP_BLENDIF_Cz_out    = 13,
  DEVELOP_BLENDIF_hz_out    = 14,

  DEVELOP_BLENDIF_MAX       = 14,
  DEVELOP_BLENDIF_unused    = 15,

  DEVELOP_BLENDIF_active    = 31,

  DEVELOP_BLENDIF_SIZE      = 16,

  DEVELOP_BLENDIF_Lab_MASK  = 0x3377,
  DEVELOP_BLENDIF_RGB_MASK  = 0x77FF
} dt_develop_blendif_channels_t;


typedef enum dt_dev_pixelpipe_display_mask_t
{
  DT_DEV_PIXELPIPE_DISPLAY_NONE = 0,
  DT_DEV_PIXELPIPE_DISPLAY_MASK = 1 << 0,
  DT_DEV_PIXELPIPE_DISPLAY_CHANNEL = 1 << 1,
  DT_DEV_PIXELPIPE_DISPLAY_OUTPUT = 1 << 2,
  DT_DEV_PIXELPIPE_DISPLAY_L = 1 << 3,
  DT_DEV_PIXELPIPE_DISPLAY_a = 2 << 3,
  DT_DEV_PIXELPIPE_DISPLAY_b = 3 << 3,
  DT_DEV_PIXELPIPE_DISPLAY_R = 4 << 3,
  DT_DEV_PIXELPIPE_DISPLAY_G = 5 << 3,
  DT_DEV_PIXELPIPE_DISPLAY_B = 6 << 3,
  DT_DEV_PIXELPIPE_DISPLAY_GRAY = 7 << 3,
  DT_DEV_PIXELPIPE_DISPLAY_LCH_C = 8 << 3,
  DT_DEV_PIXELPIPE_DISPLAY_LCH_h = 9 << 3,
  DT_DEV_PIXELPIPE_DISPLAY_HSL_H = 10 << 3,
  DT_DEV_PIXELPIPE_DISPLAY_HSL_S = 11 << 3,
  DT_DEV_PIXELPIPE_DISPLAY_HSL_l = 12 << 3,
  DT_DEV_PIXELPIPE_DISPLAY_JzCzhz_Jz = 13 << 3,
  DT_DEV_PIXELPIPE_DISPLAY_JzCzhz_Cz = 14 << 3,
  DT_DEV_PIXELPIPE_DISPLAY_JzCzhz_hz = 15 << 3,
  DT_DEV_PIXELPIPE_DISPLAY_ANY = 0xff << 2
} dt_dev_pixelpipe_display_mask_t;


float
blendif_factor_Lab(const float4 input, const float4 output,
                   const unsigned int blendif, global const float *parameters,
                   const unsigned int mask_mode, const unsigned int mask_combine)
{
  float scaled[DEVELOP_BLENDIF_SIZE];

  if(!(mask_mode & DEVELOP_MASK_CONDITIONAL)) return (mask_combine & DEVELOP_COMBINE_INCL) ? 0.0f : 1.0f;

  scaled[DEVELOP_BLENDIF_L_in] = input.x / 100.0f,			// L scaled to 0..1
  scaled[DEVELOP_BLENDIF_A_in] = input.y / 256.0f;		// a scaled to -0.5..0.5
  scaled[DEVELOP_BLENDIF_B_in] = input.z / 256.0f;		// b scaled to -0.5..0.5

  scaled[DEVELOP_BLENDIF_L_out] = output.x / 100.0f;			// L scaled to 0..1
  scaled[DEVELOP_BLENDIF_A_out] = output.y / 256.0f;		// a scaled to -0.5..0.5
  scaled[DEVELOP_BLENDIF_B_out] = output.z / 256.0f;		// b scaled to -0.5..0.5


  if((blendif & 0x7f00) != 0)  // do we need to consider LCh ?
  {
    float4 LCH_input = Lab_2_LCH(input);
    float4 LCH_output = Lab_2_LCH(output);

    scaled[DEVELOP_BLENDIF_C_in] = LCH_input.y / (128.0f*sqrt(2.0f));        // C scaled to 0..1
    scaled[DEVELOP_BLENDIF_h_in] = LCH_input.z;		                // h scaled to 0..1

    scaled[DEVELOP_BLENDIF_C_out] = LCH_output.y / (128.0f*sqrt(2.0f));       // C scaled to 0..1
    scaled[DEVELOP_BLENDIF_h_out] = LCH_output.z;		                // h scaled to 0..1
  }

  const unsigned int invert_mask = (blendif >> 16) ^ ((mask_combine & DEVELOP_COMBINE_INCL) ? DEVELOP_BLENDIF_Lab_MASK : 0);

  float result = 1.0f;
  for(int ch=0; ch<=DEVELOP_BLENDIF_MAX; ch++)
  {
    if((DEVELOP_BLENDIF_Lab_MASK & (1<<ch)) == 0) continue;       // skip blendif channels not used in this color space

    float factor;
    if((blendif & (1<<ch)) == 0)                                  // deal with channels where sliders span the whole range
    {
      factor = 1.0f;
    }
    else if(result <= 0.000001f)
    {
      break; // no need to continue if we are already close to or at zero
    }
    else if(scaled[ch] <= parameters[6 * ch + 0])
    {
      factor = 0.0f;
    }
    else if(scaled[ch] < parameters[6 * ch + 1])
    {
      factor = (scaled[ch] - parameters[6 * ch + 0]) * parameters[6 * ch + 4];
    }
    else if(scaled[ch] <= parameters[6 * ch + 2])
    {
      factor = 1.0f;
    }
    else if(scaled[ch] < parameters[6 * ch + 3])
    {
      factor = 1.0f - (scaled[ch] - parameters[6 * ch + 2]) * parameters[6 * ch + 5];
    }
    else
    {
      factor = 0.0f;
    }
    result *= (invert_mask & (1 << ch)) ? 1.0f - factor : factor; // inverted channel?
  }

  return (mask_combine & DEVELOP_COMBINE_INCL) ? 1.0f - result : result;
}


float
blendif_factor_rgb_hsl(const float4 input, const float4 output,
                       const unsigned int blendif, global const float *parameters,
                       const unsigned int mask_mode, const unsigned int mask_combine,
                       constant dt_colorspaces_iccprofile_info_cl_t *profile_info,
                       read_only image2d_t profile_lut, const int use_work_profile)
{
  float scaled[DEVELOP_BLENDIF_SIZE];

  if(!(mask_mode & DEVELOP_MASK_CONDITIONAL)) return (mask_combine & DEVELOP_COMBINE_INCL) ? 0.0f : 1.0f;

  if(use_work_profile == 0)
  {
    scaled[DEVELOP_BLENDIF_GRAY_in]  = 0.3f*input.x + 0.59f*input.y + 0.11f*input.z; // Gray scaled to 0..1
    scaled[DEVELOP_BLENDIF_GRAY_out] = 0.3f*output.x + 0.59f*output.y + 0.11f*output.z; // Gray scaled to 0..1
  }
  else
  {
    scaled[DEVELOP_BLENDIF_GRAY_in]  = get_rgb_matrix_luminance(input, profile_info, profile_info->matrix_in, profile_lut); // Gray scaled to 0..1
    scaled[DEVELOP_BLENDIF_GRAY_out] = get_rgb_matrix_luminance(output, profile_info, profile_info->matrix_in, profile_lut); // Gray scaled to 0..1
  }

  scaled[DEVELOP_BLENDIF_RED_in]   = input.x;						// Red
  scaled[DEVELOP_BLENDIF_GREEN_in] = input.y;						// Green
  scaled[DEVELOP_BLENDIF_BLUE_in]  = input.z;						// Blue

  scaled[DEVELOP_BLENDIF_RED_out]   = output.x;						// Red
  scaled[DEVELOP_BLENDIF_GREEN_out] = output.y;						// Green
  scaled[DEVELOP_BLENDIF_BLUE_out]  = output.z;						// Blue

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

  const unsigned int invert_mask = (blendif >> 16) ^ ((mask_combine & DEVELOP_COMBINE_INCL) ? DEVELOP_BLENDIF_RGB_MASK : 0);

  float result = 1.0f;
  for(int ch=0; ch<=DEVELOP_BLENDIF_MAX; ch++)
  {
    if((DEVELOP_BLENDIF_RGB_MASK & (1<<ch)) == 0) continue;       // skip blendif channels not used in this color space

    float factor;
    if((blendif & (1<<ch)) == 0)                                  // deal with channels where sliders span the whole range
    {
      factor = 1.0f;
    }
    else if(result <= 0.000001f)
    {
      break; // no need to continue if we are already close to or at zero
    }
    else if(scaled[ch] <= parameters[6 * ch + 0])
    {
      factor = 0.0f;
    }
    else if(scaled[ch] < parameters[6 * ch + 1])
    {
      factor = (scaled[ch] - parameters[6 * ch + 0]) * parameters[6 * ch + 4];
    }
    else if(scaled[ch] <= parameters[6 * ch + 2])
    {
      factor = 1.0f;
    }
    else if(scaled[ch] < parameters[6 * ch + 3])
    {
      factor = 1.0f - (scaled[ch] - parameters[6 * ch + 2]) * parameters[6 * ch + 5];
    }
    else
    {
      factor = 0.0f;
    }
    result *= (invert_mask & (1 << ch)) ? 1.0f - factor : factor; // inverted channel?
  }

  return (mask_combine & DEVELOP_COMBINE_INCL) ? 1.0f - result : result;
}


static inline float4 rgb_to_JzCzhz(float4 rgb, constant dt_colorspaces_iccprofile_info_cl_t *profile_info,
                                   read_only image2d_t profile_lut, const int use_work_profile)
{
  float4 xyz = rgb;
  if(use_work_profile != 0)
  {
    // use the matrix_out of the hacked profile for blending to use the
    // conversion from RGB to XYZ D65 (instead of XYZ D50)
    xyz = rgb_matrix_to_xyz(rgb, profile_info, profile_info->matrix_out, profile_lut);
  }
  return JzAzBz_to_JzCzhz(XYZ_to_JzAzBz(xyz));
}

float
blendif_factor_rgb_jzczhz(const float4 input, const float4 output,
                          const unsigned int blendif, global const float *parameters,
                          const unsigned int mask_mode, const unsigned int mask_combine,
                          constant dt_colorspaces_iccprofile_info_cl_t *profile_info,
                          read_only image2d_t profile_lut)
{
  float scaled[DEVELOP_BLENDIF_SIZE];

  if(!(mask_mode & DEVELOP_MASK_CONDITIONAL)) return (mask_combine & DEVELOP_COMBINE_INCL) ? 0.0f : 1.0f;

  scaled[DEVELOP_BLENDIF_GRAY_in]  = get_rgb_matrix_luminance(input, profile_info, profile_info->matrix_in, profile_lut); // Gray scaled to 0..1
  scaled[DEVELOP_BLENDIF_GRAY_out] = get_rgb_matrix_luminance(output, profile_info, profile_info->matrix_in, profile_lut); // Gray scaled to 0..1

  scaled[DEVELOP_BLENDIF_RED_in]   = input.x;           // Red
  scaled[DEVELOP_BLENDIF_GREEN_in] = input.y;           // Green
  scaled[DEVELOP_BLENDIF_BLUE_in]  = input.z;           // Blue

  scaled[DEVELOP_BLENDIF_RED_out]   = output.x;           // Red
  scaled[DEVELOP_BLENDIF_GREEN_out] = output.y;           // Green
  scaled[DEVELOP_BLENDIF_BLUE_out]  = output.z;           // Blue

  if((blendif & 0x7f00) != 0)  // do we need to consider JzCzhz ?
  {
    float4 JzCzhz_input = rgb_to_JzCzhz(input, profile_info, profile_lut, 1);
    float4 JzCzhz_output = rgb_to_JzCzhz(output, profile_info, profile_lut, 1);

    scaled[DEVELOP_BLENDIF_Jz_in] = JzCzhz_input.x;
    scaled[DEVELOP_BLENDIF_Cz_in] = JzCzhz_input.y;
    scaled[DEVELOP_BLENDIF_hz_in] = JzCzhz_input.z;

    scaled[DEVELOP_BLENDIF_Jz_out] = JzCzhz_output.x;
    scaled[DEVELOP_BLENDIF_Cz_out] = JzCzhz_output.y;
    scaled[DEVELOP_BLENDIF_hz_out] = JzCzhz_output.z;
  }

  const unsigned int invert_mask = (blendif >> 16) ^ ((mask_combine & DEVELOP_COMBINE_INCL) ? DEVELOP_BLENDIF_RGB_MASK : 0);

  float result = 1.0f;
  for(int ch=0; ch<=DEVELOP_BLENDIF_MAX; ch++)
  {
    if((DEVELOP_BLENDIF_RGB_MASK & (1<<ch)) == 0) continue;       // skip blendif channels not used in this color space

    float factor;
    if((blendif & (1<<ch)) == 0)                                  // deal with channels where sliders span the whole range
    {
      factor = 1.0f;
    }
    else if(result <= 0.000001f)
    {
      break; // no need to continue if we are already close to or at zero
    }
    else if(scaled[ch] <= parameters[6 * ch + 0])
    {
      factor = 0.0f;
    }
    else if(scaled[ch] < parameters[6 * ch + 1])
    {
      factor = (scaled[ch] - parameters[6 * ch + 0]) * parameters[6 * ch + 4];
    }
    else if(scaled[ch] <= parameters[6 * ch + 2])
    {
      factor = 1.0f;
    }
    else if(scaled[ch] < parameters[6 * ch + 3])
    {
      factor = 1.0f - (scaled[ch] - parameters[6 * ch + 2]) * parameters[6 * ch + 5];
    }
    else
    {
      factor = 0.0f;
    }
    result *= (invert_mask & (1 << ch)) ? 1.0f - factor : factor; // inverted channel?
  }

  return (mask_combine & DEVELOP_COMBINE_INCL) ? 1.0f - result : result;
}


__kernel void
blendop_mask_Lab(__read_only image2d_t in_a, __read_only image2d_t in_b, __read_only image2d_t mask_in, __write_only image2d_t mask, const int width, const int height,
             const float gopacity, const int blendif, global const float *blendif_parameters, const unsigned int mask_mode, const unsigned int mask_combine, const int2 offs,
             constant dt_colorspaces_iccprofile_info_cl_t *profile_info, read_only image2d_t profile_lut, const int use_work_profile)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 a = read_imagef(in_a, sampleri, (int2)(x, y) + offs); // see comment in blend.c:dt_develop_blend_process_cl()
  float4 b = read_imagef(in_b, sampleri, (int2)(x, y));
  float form = read_imagef(mask_in, sampleri, (int2)(x, y)).x;

  float conditional = blendif_factor_Lab(a, b, blendif, blendif_parameters, mask_mode, mask_combine);

  float opacity = (mask_combine & DEVELOP_COMBINE_INCL) ? 1.0f - (1.0f - form) * (1.0f - conditional) : form * conditional ;
  opacity = (mask_combine & DEVELOP_COMBINE_INV) ? 1.0f - opacity : opacity;

  write_imagef(mask, (int2)(x, y), gopacity*opacity);
}

__kernel void
blendop_mask_RAW(__read_only image2d_t in_a, __read_only image2d_t in_b, __read_only image2d_t mask_in, __write_only image2d_t mask, const int width, const int height,
             const float gopacity, const int blendif, global const float *blendif_parameters, const unsigned int mask_mode, const unsigned int mask_combine, const int2 offs,
             constant dt_colorspaces_iccprofile_info_cl_t *profile_info, read_only image2d_t profile_lut, const int use_work_profile)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;
  float form = read_imagef(mask_in, sampleri, (int2)(x, y)).x;

  float opacity = (mask_combine & DEVELOP_COMBINE_INV) ? 1.0f - form : form;

  write_imagef(mask, (int2)(x, y), gopacity*opacity);
}

__kernel void
blendop_mask_rgb_hsl(__read_only image2d_t in_a, __read_only image2d_t in_b, __read_only image2d_t mask_in, __write_only image2d_t mask, const int width, const int height,
             const float gopacity, const int blendif, global const float *blendif_parameters, const unsigned int mask_mode, const unsigned int mask_combine, const int2 offs,
             constant dt_colorspaces_iccprofile_info_cl_t *profile_info, read_only image2d_t profile_lut, const int use_work_profile)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 a = read_imagef(in_a, sampleri, (int2)(x, y) + offs); // see comment in blend.c:dt_develop_blend_process_cl()
  float4 b = read_imagef(in_b, sampleri, (int2)(x, y));
  float form = read_imagef(mask_in, sampleri, (int2)(x, y)).x;

  const unsigned int invert_mask = (blendif >> 16) ^ (mask_combine & DEVELOP_COMBINE_INCL ? 0 : DEVELOP_BLENDIF_RGB_MASK);

  float conditional = blendif_factor_rgb_hsl(a, b, blendif, blendif_parameters, mask_mode, mask_combine, profile_info, profile_lut, use_work_profile);

  float opacity = (mask_combine & DEVELOP_COMBINE_INCL) ? 1.0f - (1.0f - form) * (1.0f - conditional) : form * conditional ;
  opacity = (mask_combine & DEVELOP_COMBINE_INV) ? 1.0f - opacity : opacity;

  write_imagef(mask, (int2)(x, y), gopacity*opacity);
}

__kernel void
blendop_mask_rgb_jzczhz(__read_only image2d_t in_a, __read_only image2d_t in_b, __read_only image2d_t mask_in, __write_only image2d_t mask, const int width, const int height,
             const float gopacity, const int blendif, global const float *blendif_parameters, const unsigned int mask_mode, const unsigned int mask_combine, const int2 offs,
             constant dt_colorspaces_iccprofile_info_cl_t *profile_info, read_only image2d_t profile_lut, const int use_work_profile)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height || use_work_profile == 0) return;

  float4 a = read_imagef(in_a, sampleri, (int2)(x, y) + offs); // see comment in blend.c:dt_develop_blend_process_cl()
  float4 b = read_imagef(in_b, sampleri, (int2)(x, y));
  float form = read_imagef(mask_in, sampleri, (int2)(x, y)).x;

  const unsigned int invert_mask = (blendif >> 16) ^ (mask_combine & DEVELOP_COMBINE_INCL ? 0 : DEVELOP_BLENDIF_RGB_MASK);

  float conditional = blendif_factor_rgb_jzczhz(a, b, blendif, blendif_parameters, mask_mode, mask_combine, profile_info, profile_lut);

  float opacity = (mask_combine & DEVELOP_COMBINE_INCL) ? 1.0f - (1.0f - form) * (1.0f - conditional) : form * conditional ;
  opacity = (mask_combine & DEVELOP_COMBINE_INV) ? 1.0f - opacity : opacity;

  write_imagef(mask, (int2)(x, y), gopacity*opacity);
}


__kernel void
blendop_Lab(__read_only image2d_t in_a, __read_only image2d_t in_b, __read_only image2d_t mask, __write_only image2d_t out, const int width, const int height,
             const int blend_mode, const float blend_parameter, const int2 offs, const int mask_display)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 a, b, o;
  float4 ta, tb, to;
  float d, s;

  if((blend_mode & DEVELOP_BLEND_REVERSE) == DEVELOP_BLEND_REVERSE)
  {
    b = read_imagef(in_a, sampleri, (int2)(x, y) + offs);
    a = read_imagef(in_b, sampleri, (int2)(x, y));
  }
  else
  {
    a = read_imagef(in_a, sampleri, (int2)(x, y) + offs);
    b = read_imagef(in_b, sampleri, (int2)(x, y));
  }
  float opacity = read_imagef(mask, sampleri, (int2)(x, y)).x;

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
  switch(blend_mode & DEVELOP_BLEND_MODE_MASK)
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

    case DEVELOP_BLEND_SUBTRACT:
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

    case DEVELOP_BLEND_BOUNDED:
      o =  clamp((a * (1.0f - opacity)) + (b * opacity), min, max);
      break;

    case DEVELOP_BLEND_LAB_LIGHTNESS:
    case DEVELOP_BLEND_LAB_L:
      o.x = (a.x * (1.0f - opacity)) + (b.x * opacity);
      o.y = a.y;
      o.z = a.z;
      break;

    case DEVELOP_BLEND_LAB_A:
      o.x = a.x;
      o.y = (a.y * (1.0f - opacity)) + (b.y * opacity);
      o.z = a.z;
      break;

    case DEVELOP_BLEND_LAB_B:
      o.x = a.x;
      o.y = a.y;
      o.z = (a.z * (1.0f - opacity)) + (b.z * opacity);
      break;

    case DEVELOP_BLEND_LAB_COLOR:
      o.x = a.x;
      o.y = (a.y * (1.0f - opacity)) + (b.y * opacity);
      o.z = (a.z * (1.0f - opacity)) + (b.z * opacity);
      break;

    case DEVELOP_BLEND_HSV_LIGHTNESS:
    case DEVELOP_BLEND_HSV_COLOR:
    case DEVELOP_BLEND_RGB_R:
    case DEVELOP_BLEND_RGB_G:
    case DEVELOP_BLEND_RGB_B:
      o = a;                            // Noop for Lab (without clamping)
      break;

    /* fallback to normal blend */
    case DEVELOP_BLEND_NORMAL2:
    default:
      o =  (a * (1.0f - opacity)) + (b * opacity);
      break;

  }

  /* scale L back to [0; 100] and a,b to [-128; 128] */
  o *= scale;

  /* we transfer alpha channel of input if mask_display is set, else we save opacity into alpha channel */
  o.w = mask_display ? a.w : opacity;

  write_imagef(out, (int2)(x, y), o);
}



__kernel void
blendop_RAW(__read_only image2d_t in_a, __read_only image2d_t in_b, __read_only image2d_t mask, __write_only image2d_t out, const int width, const int height,
            const int blend_mode, const float blend_parameter, const int2 offs, const int mask_display)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float a, b, o;

  if((blend_mode & DEVELOP_BLEND_REVERSE) == DEVELOP_BLEND_REVERSE)
  {
    b = read_imagef(in_a, sampleri, (int2)(x, y) + offs).x;
    a = read_imagef(in_b, sampleri, (int2)(x, y)).x;
  }
  else
  {
    a = read_imagef(in_a, sampleri, (int2)(x, y) + offs).x;
    b = read_imagef(in_b, sampleri, (int2)(x, y)).x;
  }
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
  switch(blend_mode & DEVELOP_BLEND_MODE_MASK)
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

    case DEVELOP_BLEND_SUBTRACT:
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

    case DEVELOP_BLEND_BOUNDED:
      o =  clamp((a * (1.0f - opacity)) + (b * opacity), min, max);
      break;

    case DEVELOP_BLEND_LAB_LIGHTNESS:
    case DEVELOP_BLEND_LAB_COLOR:
    case DEVELOP_BLEND_LAB_L:
    case DEVELOP_BLEND_LAB_A:
    case DEVELOP_BLEND_LAB_B:
      o = a;                            // Noop for Raw (without clamping)
      break;

    case DEVELOP_BLEND_HSV_LIGHTNESS:
    case DEVELOP_BLEND_HSV_COLOR:
    case DEVELOP_BLEND_RGB_R:
    case DEVELOP_BLEND_RGB_G:
    case DEVELOP_BLEND_RGB_B:
      o = a;                            // Noop for Raw (without clamping)
      break;

    /* fallback to normal blend */
    case DEVELOP_BLEND_NORMAL2:
    default:
      o =  (a * (1.0f - opacity)) + (b * opacity);
      break;

  }

  write_imagef(out, (int2)(x, y), o);
}

__kernel void
blendop_RAW4(__read_only image2d_t in_a, __read_only image2d_t in_b, __read_only image2d_t mask, __write_only image2d_t out, const int width, const int height,
            const int blend_mode, const float blend_parameter, const int2 offs, const int mask_display)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 a, b, o;

  if((blend_mode & DEVELOP_BLEND_REVERSE) == DEVELOP_BLEND_REVERSE)
  {
    b = read_imagef(in_a, sampleri, (int2)(x, y) + offs);
    a = read_imagef(in_b, sampleri, (int2)(x, y));
  }
  else
  {
    a = read_imagef(in_a, sampleri, (int2)(x, y) + offs);
    b = read_imagef(in_b, sampleri, (int2)(x, y));
  }

  float4 opacity = read_imagef(mask, sampleri, (int2)(x, y)).x;

  const float4 min = 0.0f;
  const float4 max = 1.0f;
  const float4 lmin = 0.0f;
  const float4 lmax = 1.0f;        /* max + fabs(min) */
  const float4 halfmax = 0.5f;     /* lmax / 2.0f */
  const float4 doublemax = 2.0f;   /* lmax * 2.0f */
  const float4 opacity2 = opacity*opacity;

  float4 la = clamp(a + fabs(min), lmin, lmax);
  float4 lb = clamp(b + fabs(min), lmin, lmax);


  /* select the blend operator */
  switch(blend_mode & DEVELOP_BLEND_MODE_MASK)
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

    case DEVELOP_BLEND_SUBTRACT:
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
      o = clamp(a, min, max);   // Noop for Raw
      break;

    case DEVELOP_BLEND_CHROMA:
      o = clamp(a, min, max);   // Noop for Raw
      break;

    case DEVELOP_BLEND_HUE:
      o = clamp(a, min, max);   // Noop for Raw
      break;

    case DEVELOP_BLEND_COLOR:
      o = clamp(a, min, max);   // Noop for Raw
      break;

    case DEVELOP_BLEND_COLORADJUST:
      o = clamp(a, min, max);   // Noop for Raw
      break;

    case DEVELOP_BLEND_BOUNDED:
      o =  clamp((a * (1.0f - opacity)) + (b * opacity), min, max);
      break;

    case DEVELOP_BLEND_LAB_LIGHTNESS:
    case DEVELOP_BLEND_LAB_COLOR:
    case DEVELOP_BLEND_LAB_L:
    case DEVELOP_BLEND_LAB_A:
    case DEVELOP_BLEND_LAB_B:
      o = a;                            // Noop for Raw (without clamping)
      break;

    case DEVELOP_BLEND_HSV_LIGHTNESS:
    case DEVELOP_BLEND_HSV_COLOR:
    case DEVELOP_BLEND_RGB_R:
    case DEVELOP_BLEND_RGB_G:
    case DEVELOP_BLEND_RGB_B:
      o = a;                            // Noop for Raw (without clamping)
      break;

    /* fallback to normal blend */
    case DEVELOP_BLEND_NORMAL2:
    default:
      o =  (a * (1.0f - opacity)) + (b * opacity);
      break;

  }

  write_imagef(out, (int2)(x, y), o);
}


__kernel void
blendop_rgb_hsl(__read_only image2d_t in_a, __read_only image2d_t in_b, __read_only image2d_t mask, __write_only image2d_t out, const int width, const int height,
                const int blend_mode, const float blend_parameter, const int2 offs, const int mask_display)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 a, b, o;
  float4 ta, tb, to;
  float d, s;

  if((blend_mode & DEVELOP_BLEND_REVERSE) == DEVELOP_BLEND_REVERSE)
  {
    b = read_imagef(in_a, sampleri, (int2)(x, y) + offs);
    a = read_imagef(in_b, sampleri, (int2)(x, y));
  }
  else
  {
    a = read_imagef(in_a, sampleri, (int2)(x, y) + offs);
    b = read_imagef(in_b, sampleri, (int2)(x, y));
  }
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
  switch(blend_mode & DEVELOP_BLEND_MODE_MASK)
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

    case DEVELOP_BLEND_SUBTRACT:
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

    case DEVELOP_BLEND_BOUNDED:
      o =  clamp((a * (1.0f - opacity)) + (b * opacity), min, max);
      break;

    case DEVELOP_BLEND_LAB_LIGHTNESS:
    case DEVELOP_BLEND_LAB_COLOR:
    case DEVELOP_BLEND_LAB_L:
    case DEVELOP_BLEND_LAB_A:
    case DEVELOP_BLEND_LAB_B:
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

    case DEVELOP_BLEND_RGB_R:
      o.x = (a.x * (1.0f - opacity)) + (b.x * opacity);
      o.y = a.y;
      o.z = a.z;
      break;

    case DEVELOP_BLEND_RGB_G:
      o.x = a.x;
      o.y = (a.y * (1.0f - opacity)) + (b.y * opacity);
      o.z = a.z;
      break;

    case DEVELOP_BLEND_RGB_B:
      o.x = a.x;
      o.y = a.y;
      o.z = (a.z * (1.0f - opacity)) + (b.z * opacity);
      break;


    /* fallback to normal blend */
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
blendop_rgb_jzczhz(__read_only image2d_t in_a, __read_only image2d_t in_b, __read_only image2d_t mask, __write_only image2d_t out, const int width, const int height,
                   const int blend_mode, const float blend_parameter, const int2 offs, const int mask_display)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 a, b, o;

  if((blend_mode & DEVELOP_BLEND_REVERSE) == DEVELOP_BLEND_REVERSE)
  {
    b = read_imagef(in_a, sampleri, (int2)(x, y) + offs);
    a = read_imagef(in_b, sampleri, (int2)(x, y));
  }
  else
  {
    a = read_imagef(in_a, sampleri, (int2)(x, y) + offs);
    b = read_imagef(in_b, sampleri, (int2)(x, y));
  }
  float opacity = read_imagef(mask, sampleri, (int2)(x, y)).x;
  float norm_a;
  float norm_b;

  /* select the blend operator */
  switch (blend_mode & DEVELOP_BLEND_MODE_MASK)
  {
    case DEVELOP_BLEND_MULTIPLY:
      o = a * (1.0f - opacity) + a * b * blend_parameter * opacity;
      break;

    case DEVELOP_BLEND_AVERAGE:
      o = a * (1.0f - opacity) + (a + b)/2.0f * opacity;
      break;

    case DEVELOP_BLEND_ADD:
      o = a * (1.0f - opacity) + (a + blend_parameter * b) * opacity;
      break;

    case DEVELOP_BLEND_SUBTRACT:
      o = a * (1.0f - opacity) + fmax(a - blend_parameter * b, 0.0f) * opacity;
      break;

    case DEVELOP_BLEND_SUBTRACT_INVERSE:
      o = a * (1.0f - opacity) + fmax(b - blend_parameter * a, 0.0f) * opacity;
      break;

    case DEVELOP_BLEND_DIFFERENCE:
    case DEVELOP_BLEND_DIFFERENCE2:
      o = a * (1.0f - opacity) + fabs(a - b) * opacity;
      break;

    case DEVELOP_BLEND_LIGHTNESS:
      norm_a = fmax(sqrt(a.x * a.x + a.y * a.y + a.z * a.z), 1e-6f);
      norm_b = fmax(sqrt(b.x * b.x + b.y * b.y + b.z * b.z), 1e-6f);
      o = a * (1.0f - opacity) + a * norm_b / norm_a * opacity;
      break;

    case DEVELOP_BLEND_CHROMA:
      norm_a = fmax(sqrt(a.x * a.x + a.y * a.y + a.z * a.z), 1e-6f);
      norm_b = fmax(sqrt(b.x * b.x + b.y * b.y + b.z * b.z), 1e-6f);
      o = a * (1.0f - opacity) + b * norm_a / norm_b * opacity;
      break;

    case DEVELOP_BLEND_RGB_R:
      o.x = (a.x * (1.0f - opacity)) + (blend_parameter * b.x * opacity);
      o.y = a.y;
      o.z = a.z;
      break;

    case DEVELOP_BLEND_RGB_G:
      o.x = a.x;
      o.y = (a.y * (1.0f - opacity)) + (blend_parameter * b.y * opacity);
      o.z = a.z;
      break;

    case DEVELOP_BLEND_RGB_B:
      o.x = a.x;
      o.y = a.y;
      o.z = (a.z * (1.0f - opacity)) + (blend_parameter * b.z * opacity);
      break;

    case DEVELOP_BLEND_DIVIDE:
      o = a * (1.0f - opacity) + a / fmax(b * blend_parameter, 1e-6f) * opacity;
      break;

    case DEVELOP_BLEND_DIVIDE_INVERSE:
      o = a * (1.0f - opacity) + b / fmax(a * blend_parameter, 1e-6f) * opacity;
      break;

    case DEVELOP_BLEND_GEOMETRIC_MEAN:
      o = a * (1.0f - opacity) + sqrt(fmax(a * b, 0.0f)) * opacity;
      break;

    case DEVELOP_BLEND_HARMONIC_MEAN:
      o = a * (1.0f - opacity) + 2.0f * a * b / (fmax(a, 5e-7f) + fmax(b, 5e-7f)) * opacity;
      break;

    /* fallback to normal blend */
    case DEVELOP_BLEND_BOUNDED:
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
blendop_mask_tone_curve(__read_only image2d_t mask_in, __write_only image2d_t mask_out,
			const int width, const int height,
			const float e, const float brightness, const float gopacity)
{
  const float mask_epsilon = 16 * FLT_EPSILON;  // empirical mask threshold for fully transparent masks
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if (x >= width || y >= height) return;

  float opacity = read_imagef(mask_in, sampleri, (int2)(x, y)).x;
  float scaled_opacity = (2.f * opacity / gopacity - 1.f);
  if (1.f - brightness <= 0.f)
    scaled_opacity = opacity <= mask_epsilon ? -1.f : 1.f;
  else if (1.f + brightness <= 0.f)
    scaled_opacity = opacity >= 1.f - mask_epsilon ? 1.f : -1.f;
  else if (brightness > 0.f)
  {
    scaled_opacity = (scaled_opacity + brightness) / (1.f - brightness);
    scaled_opacity = fmin(scaled_opacity, 1.f);
  }
  else
  {
    scaled_opacity = (scaled_opacity + brightness) / (1.f + brightness);
    scaled_opacity = fmax(scaled_opacity, -1.f);
  }
  opacity = clamp(
      ((scaled_opacity * e / (1.f + (e - 1.f) * fabs(scaled_opacity))) / 2.f + 0.5f) * gopacity, 0.f, 1.f);
  write_imagef(mask_out, (int2)(x, y), opacity);
}


__kernel void
blendop_set_mask (__write_only image2d_t mask, const int width, const int height, const float value)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  write_imagef(mask, (int2)(x, y), value);
}

#define MININ 1e-5f
__kernel void blendop_highlights_mask
  ( __read_only image2d_t in, __read_only image2d_t out,
    __read_only image2d_t mask_in, __write_only image2d_t mask_out,
    const int width, const int height,
    const int iwidth, const int iheight,
    const int ch, const int2 offs)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  int irow = y + offs.y;
  int icol = x + offs.x;
  if((irow >= iheight) || (icol >= iwidth)) return;

  float r = 0.0f;
  const float m = read_imagef(mask_in, sampleri, (int2)(x, y)).x;

  if(ch == 1)
  {
    const float a = fmax(MININ, read_imagef(in, sampleri, (int2)(icol, irow)).x);
    const float b = read_imagef(out, sampleri, (int2)(x, y)).x / a;
    r = 10.0f * fmax(0.0f, b - 1.0f);
  }
  else
  {
    const float4 a = fmax(MININ, read_imagef(in, sampleri, (int2)(icol, irow)));
    const float4 b = read_imagef(out, sampleri, (int2)(x, y)) / a;
    r = 10.0f * fmax(0.0f, (fmax(b.w, fmax(b.x, b.y)) - 1.0f));
  }

  write_imagef(mask_out, (int2)(x, y), m * clamp(r*r, 0.0f, 2.0f));
}
#undef MININ

__kernel void
blendop_display_channel(__read_only image2d_t in_a, __read_only image2d_t in_b, __read_only image2d_t mask,
                        __write_only image2d_t out, const int width, const int height, const int2 offs,
                        const int mask_display, constant float *boost_factors,
                        constant dt_colorspaces_iccprofile_info_cl_t *profile_info,
                        read_only image2d_t profile_lut, const int use_profile,
                        constant dt_colorspaces_iccprofile_info_cl_t *work_profile_info,
                        read_only image2d_t work_profile_lut, const int use_work_profile)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 a = read_imagef(in_a, sampleri, (int2)(x, y) + offs); // see comment in blend.c:dt_develop_blend_process_cl()
  float4 b = read_imagef(in_b, sampleri, (int2)(x, y));
  float opacity = read_imagef(mask, sampleri, (int2)(x, y)).x;

  float c;
  float4 LCH;
  float4 HSL;
  float4 JzCzhz;
  int is_lab;

  dt_dev_pixelpipe_display_mask_t channel = (dt_dev_pixelpipe_display_mask_t)mask_display;

  switch(channel & DT_DEV_PIXELPIPE_DISPLAY_ANY)
  {
    case DT_DEV_PIXELPIPE_DISPLAY_L:
      c = clamp(a.x / 100.0f / exp2(boost_factors[DEVELOP_BLENDIF_L_in]), 0.0f, 1.0f);
      is_lab = 1;
      break;
    case (DT_DEV_PIXELPIPE_DISPLAY_L | DT_DEV_PIXELPIPE_DISPLAY_OUTPUT):
      c = clamp(b.x / 100.0f / exp2(boost_factors[DEVELOP_BLENDIF_L_out]), 0.0f, 1.0f);
      is_lab = 1;
      break;
    case DT_DEV_PIXELPIPE_DISPLAY_a:
      c = clamp(a.y / 256.0f / exp2(boost_factors[DEVELOP_BLENDIF_A_in]) + 0.5f, 0.0f, 1.0f);
      is_lab = 1;
      break;
    case (DT_DEV_PIXELPIPE_DISPLAY_a | DT_DEV_PIXELPIPE_DISPLAY_OUTPUT):
      c = clamp(b.y / 256.0f / exp2(boost_factors[DEVELOP_BLENDIF_A_out]) + 0.5f, 0.0f, 1.0f);
      is_lab = 1;
      break;
    case DT_DEV_PIXELPIPE_DISPLAY_b:
      c = clamp(a.z / 256.0f / exp2(boost_factors[DEVELOP_BLENDIF_B_in]) + 0.5f, 0.0f, 1.0f);
      is_lab = 1;
      break;
    case (DT_DEV_PIXELPIPE_DISPLAY_b | DT_DEV_PIXELPIPE_DISPLAY_OUTPUT):
      c = clamp(b.z / 256.0f / exp2(boost_factors[DEVELOP_BLENDIF_A_in]) + 0.5f, 0.0f, 1.0f);
      is_lab = 1;
      break;
    case DT_DEV_PIXELPIPE_DISPLAY_R:
      c = clamp(a.x / exp2(boost_factors[DEVELOP_BLENDIF_RED_in]), 0.0f, 1.0f);
      is_lab = 0;
      break;
    case (DT_DEV_PIXELPIPE_DISPLAY_R | DT_DEV_PIXELPIPE_DISPLAY_OUTPUT):
      c = clamp(b.x / exp2(boost_factors[DEVELOP_BLENDIF_RED_out]), 0.0f, 1.0f);
      is_lab = 0;
      break;
    case DT_DEV_PIXELPIPE_DISPLAY_G:
      c = clamp(a.y / exp2(boost_factors[DEVELOP_BLENDIF_GREEN_in]), 0.0f, 1.0f);
      is_lab = 0;
      break;
    case (DT_DEV_PIXELPIPE_DISPLAY_G | DT_DEV_PIXELPIPE_DISPLAY_OUTPUT):
      c = clamp(b.y / exp2(boost_factors[DEVELOP_BLENDIF_GREEN_out]), 0.0f, 1.0f);
      is_lab = 0;
      break;
    case DT_DEV_PIXELPIPE_DISPLAY_B:
      c = clamp(a.z / exp2(boost_factors[DEVELOP_BLENDIF_BLUE_in]), 0.0f, 1.0f);
      is_lab = 0;
      break;
    case (DT_DEV_PIXELPIPE_DISPLAY_B | DT_DEV_PIXELPIPE_DISPLAY_OUTPUT):
      c = clamp(b.z / exp2(boost_factors[DEVELOP_BLENDIF_BLUE_out]), 0.0f, 1.0f);
      is_lab = 0;
      break;
    case DT_DEV_PIXELPIPE_DISPLAY_GRAY:
      if(use_profile == 0)
        c = 0.3f * a.x + 0.59f * a.y + 0.11f * a.z;
      else
        c = get_rgb_matrix_luminance(a, profile_info, profile_info->matrix_in, profile_lut);
      c = clamp(c / exp2(boost_factors[DEVELOP_BLENDIF_GRAY_in]), 0.0f, 1.0f);
      is_lab = 0;
      break;
    case (DT_DEV_PIXELPIPE_DISPLAY_GRAY | DT_DEV_PIXELPIPE_DISPLAY_OUTPUT):
      if(use_profile == 0)
        c = 0.3f * b.x + 0.59f * b.y + 0.11f * b.z;
      else
        c = get_rgb_matrix_luminance(b, profile_info, profile_info->matrix_in, profile_lut);
      c = clamp(c / exp2(boost_factors[DEVELOP_BLENDIF_GRAY_out]), 0.0f, 1.0f);
      is_lab = 0;
      break;
    case DT_DEV_PIXELPIPE_DISPLAY_LCH_C:
      LCH = Lab_2_LCH(a);
      c = clamp(LCH.y / (128.0f * sqrt(2.0f) / exp2(boost_factors[DEVELOP_BLENDIF_C_in])), 0.0f, 1.0f);
      is_lab = 1;
      break;
    case (DT_DEV_PIXELPIPE_DISPLAY_LCH_C | DT_DEV_PIXELPIPE_DISPLAY_OUTPUT):
      LCH = Lab_2_LCH(b);
      c = clamp(LCH.y / (128.0f * sqrt(2.0f)) / exp2(boost_factors[DEVELOP_BLENDIF_C_out]), 0.0f, 1.0f);
      is_lab = 1;
      break;
    case DT_DEV_PIXELPIPE_DISPLAY_LCH_h:
      LCH = Lab_2_LCH(a);
      c = clamp(LCH.z, 0.0f, 1.0f); // no boost for hues
      is_lab = 1;
      break;
    case (DT_DEV_PIXELPIPE_DISPLAY_LCH_h | DT_DEV_PIXELPIPE_DISPLAY_OUTPUT):
      LCH = Lab_2_LCH(b);
      c = clamp(LCH.z, 0.0f, 1.0f); // no boost for hues
      is_lab = 1;
      break;
    case DT_DEV_PIXELPIPE_DISPLAY_HSL_H:
      HSL = RGB_2_HSL(a);
      c = clamp(HSL.x, 0.0f, 1.0f); // no boost for hues
      is_lab = 0;
      break;
    case (DT_DEV_PIXELPIPE_DISPLAY_HSL_H | DT_DEV_PIXELPIPE_DISPLAY_OUTPUT):
      HSL = RGB_2_HSL(b);
      c = clamp(HSL.x, 0.0f, 1.0f); // no boost for hues
      is_lab = 0;
      break;
    case DT_DEV_PIXELPIPE_DISPLAY_HSL_S:
      HSL = RGB_2_HSL(a);
      c = clamp(HSL.y, 0.0f, 1.0f); // no boost for HSL
      is_lab = 0;
      break;
    case (DT_DEV_PIXELPIPE_DISPLAY_HSL_S | DT_DEV_PIXELPIPE_DISPLAY_OUTPUT):
      HSL = RGB_2_HSL(b);
      c = clamp(HSL.y, 0.0f, 1.0f); // no boost for HSL
      is_lab = 0;
      break;
    case DT_DEV_PIXELPIPE_DISPLAY_HSL_l:
      HSL = RGB_2_HSL(a);
      c = clamp(HSL.z, 0.0f, 1.0f); // no boost for HSL
      is_lab = 0;
      break;
    case (DT_DEV_PIXELPIPE_DISPLAY_HSL_l | DT_DEV_PIXELPIPE_DISPLAY_OUTPUT):
      HSL = RGB_2_HSL(b);
      c = clamp(HSL.z, 0.0f, 1.0f); // no boost for HSL
      is_lab = 0;
      break;
    case DT_DEV_PIXELPIPE_DISPLAY_JzCzhz_Jz:
      JzCzhz = rgb_to_JzCzhz(a, profile_info, profile_lut, use_profile);
      c = clamp(JzCzhz.x / exp2(boost_factors[DEVELOP_BLENDIF_Jz_in]), 0.0f, 1.0f);
      is_lab = 0;
      break;
    case (DT_DEV_PIXELPIPE_DISPLAY_JzCzhz_Jz | DT_DEV_PIXELPIPE_DISPLAY_OUTPUT):
      JzCzhz = rgb_to_JzCzhz(b, profile_info, profile_lut, use_profile);
      c = clamp(JzCzhz.x / exp2(boost_factors[DEVELOP_BLENDIF_Jz_out]), 0.0f, 1.0f);
      is_lab = 0;
      break;
    case DT_DEV_PIXELPIPE_DISPLAY_JzCzhz_Cz:
      JzCzhz = rgb_to_JzCzhz(a, profile_info, profile_lut, use_profile);
      c = clamp(JzCzhz.y / exp2(boost_factors[DEVELOP_BLENDIF_Cz_in]), 0.0f, 1.0f);
      is_lab = 0;
      break;
    case (DT_DEV_PIXELPIPE_DISPLAY_JzCzhz_Cz | DT_DEV_PIXELPIPE_DISPLAY_OUTPUT):
      JzCzhz = rgb_to_JzCzhz(b, profile_info, profile_lut, use_profile);
      c = clamp(JzCzhz.y / exp2(boost_factors[DEVELOP_BLENDIF_Cz_out]), 0.0f, 1.0f);
      is_lab = 0;
      break;
    case DT_DEV_PIXELPIPE_DISPLAY_JzCzhz_hz:
      JzCzhz = rgb_to_JzCzhz(a, profile_info, profile_lut, use_profile);
      c = clamp(JzCzhz.z / exp2(boost_factors[DEVELOP_BLENDIF_hz_in]), 0.0f, 1.0f);
      is_lab = 0;
      break;
    case (DT_DEV_PIXELPIPE_DISPLAY_JzCzhz_hz | DT_DEV_PIXELPIPE_DISPLAY_OUTPUT):
      JzCzhz = rgb_to_JzCzhz(b, profile_info, profile_lut, use_profile);
      c = clamp(JzCzhz.z / exp2(boost_factors[DEVELOP_BLENDIF_hz_out]), 0.0f, 1.0f);
      is_lab = 0;
      break;
    default:
      c = 0.0f;
      is_lab = 0;
      break;
  }

  a.x = a.y = a.z = c;
  a.w = opacity;

  if(is_lab)
  {
    float4 xyz;
    if(use_work_profile)
    {
      xyz = rgb_matrix_to_xyz(a, work_profile_info, profile_info->matrix_in, work_profile_lut);
    }
    else
    {
      xyz = sRGB_to_XYZ(a);
    }
    a = XYZ_to_Lab(xyz);
    a.w = opacity;
  }

  write_imagef(out, (int2)(x, y), a);
}
