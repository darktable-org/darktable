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

inline float lerp_lookup_unbounded(const float x, read_only image2d_t lut, constant float *const unbounded_coeffs, const int n_lut, const int lutsize)
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

inline float lookup(read_only image2d_t lut, const float x)
{
  const int xi = clamp((int)(x * 0x10000ul), 0, 0xffff);
  const int2 p = (int2)((xi & 0xff), (xi >> 8));
  return read_imagef(lut, sampleri, p).x;
}

inline float lookup_unbounded(read_only image2d_t lut, const float x, constant float *a)
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

inline float4 apply_trc_in(const float4 rgb_in, constant dt_colorspaces_iccprofile_info_cl_t *profile_info, read_only image2d_t lut)
{
  float4 rgb_out;

  rgb_out.x = lerp_lookup_unbounded(rgb_in.x, lut, profile_info->unbounded_coeffs_in[0], 0, profile_info->lutsize);
  rgb_out.y = lerp_lookup_unbounded(rgb_in.y, lut, profile_info->unbounded_coeffs_in[1], 1, profile_info->lutsize);
  rgb_out.z = lerp_lookup_unbounded(rgb_in.z, lut, profile_info->unbounded_coeffs_in[2], 2, profile_info->lutsize);
  rgb_out.w = rgb_in.w;

  return rgb_out;
}

inline float4 apply_trc_out(const float4 rgb_in, constant dt_colorspaces_iccprofile_info_cl_t *profile_info, read_only image2d_t lut)
{
  float4 rgb_out;

  rgb_out.x = lerp_lookup_unbounded(rgb_in.x, lut, profile_info->unbounded_coeffs_out[0], 3, profile_info->lutsize);
  rgb_out.y = lerp_lookup_unbounded(rgb_in.y, lut, profile_info->unbounded_coeffs_out[1], 4, profile_info->lutsize);
  rgb_out.z = lerp_lookup_unbounded(rgb_in.z, lut, profile_info->unbounded_coeffs_out[2], 5, profile_info->lutsize);
  rgb_out.w = rgb_in.w;

  return rgb_out;
}

inline float4 matrix_product(const float4 xyz, constant float *matrix)
{
  float4 output = 0.0f;
  output.x = matrix[0] * xyz.x + matrix[1] * xyz.y + matrix[2] * xyz.z;
  output.y = matrix[3] * xyz.x + matrix[4] * xyz.y + matrix[5] * xyz.z;
  output.z = matrix[6] * xyz.x + matrix[7] * xyz.y + matrix[8] * xyz.z;
  output.w = xyz.w;
  return output;
}

inline float get_rgb_matrix_luminance(const float4 rgb, constant dt_colorspaces_iccprofile_info_cl_t *profile_info, constant float *matrix, read_only image2d_t lut)
{
  float luminance = 0.f;

  if(profile_info->nonlinearlut)
  {
    float4 linear_rgb;

    linear_rgb = apply_trc_in(rgb, profile_info, lut);
    luminance = matrix[3] * linear_rgb.x + matrix[4] * linear_rgb.y + matrix[5] * linear_rgb.z;
  }
  else
    luminance = matrix[3] * rgb.x + matrix[4] * rgb.y + matrix[5] * rgb.z;

  return luminance;
}

inline float dt_camera_rgb_luminance(const float4 rgb)
{
  const float4 coeffs = { 0.2225045f, 0.7168786f, 0.0606169f, 0.0f };
  return dot(rgb, coeffs);
}
