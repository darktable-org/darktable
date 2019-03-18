/*
    This file is part of darktable,
    copyright (c) 2019 edgardo hoszowski.

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

// must be in synch with dt_iop_colorspace_type_t in imageop.h
typedef enum dt_iop_colorspace_type_t
{
  iop_cs_NONE = -1,
  iop_cs_RAW = 0,
  iop_cs_Lab = 1,
  iop_cs_rgb = 2,
  iop_cs_LCh = 3,
  iop_cs_HSL = 4
} dt_iop_colorspace_type_t;

// must be in synch with dt_colorspaces_iccprofile_info_cl_t
typedef struct dt_colorspaces_iccprofile_info_cl_t
{
  float matrix_in[9];
  float matrix_out[9];
  int lutsize;
  float unbounded_coeffs_in[3][3];
  float unbounded_coeffs_out[3][3];
  int nonlinearlut;
  float grey;
} dt_colorspaces_iccprofile_info_cl_t;

float lerp_lookup_unbounded(const float x, read_only image2d_t lut, global const float *const unbounded_coeffs, const int n_lut, const int lutsize)
{
  // in case the tone curve is marked as linear, return the fast
  // path to linear unbounded (does not clip x at 1)
  if(unbounded_coeffs[0] >= 0.0f)
  {
    if(x < 1.0f)
    {
      const float ft = clamp(x * (float)(lutsize - 1), 0.0f, (float)(lutsize - 1));
      const int t = ft < lutsize - 2 ? ft : lutsize - 2;
      const float f = ft - t;
      const int2 p1 = (int2)((t & 0xff), (t >> 8) + n_lut * 256);
      const int2 p2 = (int2)(((t + 1) & 0xff), ((t + 1) >> 8) + n_lut * 256);
      const float l1 = read_imagef(lut, sampleri, p1).x;
      const float l2 = read_imagef(lut, sampleri, p2).x;
      return l1 * (1.0f - f) + l2 * f;
    }
    else return unbounded_coeffs[1] * native_powr(x*unbounded_coeffs[0], unbounded_coeffs[2]);
  }
  else return x;
}

float lookup_unbounded(read_only image2d_t lut, const float x, global const float *a)
{
  // in case the tone curve is marked as linear, return the fast
  // path to linear unbounded (does not clip x at 1)
  if(a[0] >= 0.0f)
  {
    if(x < 1.0f)
    {
      const int xi = clamp((int)(x * 0x10000ul), 0, 0xffff);
      const int2 p = (int2)((xi & 0xff), (xi >> 8));
      return read_imagef(lut, sampleri, p).x;
    }
    else return a[1] * native_powr(x*a[0], a[2]);
  }
  else return x;
}

float4 apply_trc_in(const float4 rgb_in, global const dt_colorspaces_iccprofile_info_cl_t *profile_info, read_only image2d_t lut)
{
  float4 rgb_out;
  
  rgb_out.x = lerp_lookup_unbounded(rgb_in.x, lut, profile_info->unbounded_coeffs_in[0], 0, profile_info->lutsize);
  rgb_out.y = lerp_lookup_unbounded(rgb_in.y, lut, profile_info->unbounded_coeffs_in[1], 1, profile_info->lutsize);
  rgb_out.z = lerp_lookup_unbounded(rgb_in.z, lut, profile_info->unbounded_coeffs_in[2], 2, profile_info->lutsize);
  rgb_out.w = rgb_in.w;
  
  return rgb_out;
}

float4 apply_trc_out(const float4 rgb_in, global const dt_colorspaces_iccprofile_info_cl_t *profile_info, read_only image2d_t lut)
{
  float4 rgb_out;
  
  rgb_out.x = lerp_lookup_unbounded(rgb_in.x, lut, profile_info->unbounded_coeffs_out[0], 3, profile_info->lutsize);
  rgb_out.y = lerp_lookup_unbounded(rgb_in.y, lut, profile_info->unbounded_coeffs_out[1], 4, profile_info->lutsize);
  rgb_out.z = lerp_lookup_unbounded(rgb_in.z, lut, profile_info->unbounded_coeffs_out[2], 5, profile_info->lutsize);
  rgb_out.w = rgb_in.w;
  
  return rgb_out;
}

float4 linear_rgb_matrix_to_xyz(const float4 rgb, global const dt_colorspaces_iccprofile_info_cl_t *profile_info)
{
  float XYZ[3], RGB[3];
  RGB[0] = rgb.x;
  RGB[1] = rgb.y;
  RGB[2] = rgb.z;
  
  for(int c = 0; c < 3; c++)
  {
    XYZ[c] = 0.0f;
    for(int i = 0; i < 3; i++)
    {
      XYZ[c] += profile_info->matrix_in[3 * c + i] * RGB[i];
    }
  }

  return (float4)(XYZ[0], XYZ[1], XYZ[2], rgb.w);
}

float4 xyz_to_linear_rgb_matrix(const float4 xyz, global const dt_colorspaces_iccprofile_info_cl_t *profile_info)
{
  float XYZ[3], RGB[3];
  XYZ[0] = xyz.x;
  XYZ[1] = xyz.y;
  XYZ[2] = xyz.z;
  
  for(int c = 0; c < 3; c++)
  {
    RGB[c] = 0.0f;
    for(int i = 0; i < 3; i++)
    {
      RGB[c] += profile_info->matrix_out[3 * c + i] * XYZ[i];
    }
  }

  return (float4)(RGB[0], RGB[1], RGB[2], xyz.w);
}

float get_rgb_matrix_luminance(const float4 rgb, global const dt_colorspaces_iccprofile_info_cl_t *profile_info, read_only image2d_t lut)
{
  float luminance = 0.f;
  
  if(profile_info->nonlinearlut)
  {
    float4 linear_rgb;

    linear_rgb = apply_trc_in(rgb, profile_info, lut);
    luminance = profile_info->matrix_in[3] * linear_rgb.x + profile_info->matrix_in[4] * linear_rgb.y + profile_info->matrix_in[5] * linear_rgb.z;
  }
  else
    luminance = profile_info->matrix_in[3] * rgb.x + profile_info->matrix_in[4] * rgb.y + profile_info->matrix_in[5] * rgb.z;
  
  return luminance;
}

float dt_camera_rgb_luminance(const float4 rgb)
{
  return (rgb.x * 0.2225045f + rgb.y * 0.7168786f + rgb.z * 0.0606169f);
}
