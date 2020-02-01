/*
    This file is part of darktable,
    copyright (c) 2018 Aurélien Pierre.

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

#include "basic.cl"

// In case the OpenCL driver doesn't have a dot method
inline float vdot(const float4 vec1, const float4 vec2)
{
    return vec1.x * vec2.x + vec1.y * vec2.y + vec1.z * vec2.z;
}

typedef enum dt_iop_filmicrgb_methods_type_t
{
  DT_FILMIC_METHOD_NONE = 0,
  DT_FILMIC_METHOD_MAX_RGB = 1,
  DT_FILMIC_METHOD_LUMINANCE = 2,
  DT_FILMIC_METHOD_POWER_NORM = 3
} dt_iop_filmicrgb_methods_type_t;


kernel void
filmic (read_only image2d_t in, write_only image2d_t out, int width, int height,
        const float dynamic_range, const float shadows_range, const float grey,
        read_only image2d_t table, read_only image2d_t diff,
        const float contrast, const float power, const int preserve_color,
        const float saturation)
{
  const unsigned int x = get_global_id(0);
  const unsigned int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 i = read_imagef(in, sampleri, (int2)(x, y));
  const float4 xyz = Lab_to_XYZ(i);
  float4 o = XYZ_to_prophotorgb(xyz);

  const float noise = pow(2.0f, -16.0f);
  const float4 noise4 = noise;
  const float4 dynamic4 = dynamic_range;
  const float4 shadows4 = shadows_range;
  float derivative, luma;

  // Global desaturation
  if (saturation != 1.0f)
  {
    const float4 lum = xyz.y;
    o = lum + (float4)saturation * (o - lum);
  }

  if (preserve_color)
  {

    // Save the ratios
    float maxRGB = max(max(o.x, o.y), o.z);
    const float4 ratios = o / (float4)maxRGB;

    // Log profile
    maxRGB = maxRGB / grey;
    maxRGB = (maxRGB < noise) ? noise : maxRGB;
    maxRGB = (native_log2(maxRGB) - shadows_range) / dynamic_range;
    maxRGB = clamp(maxRGB, 0.0f, 1.0f);

    const float index = maxRGB;

    // Curve S LUT
    maxRGB = lookup(table, (const float)maxRGB);

    // Re-apply the ratios
    o = (float4)maxRGB * ratios;

    // Derivative
    derivative = lookup(diff, (const float)index);
    luma = maxRGB;
  }
  else
  {
    // Log profile
    o = o / grey;
    o = (o < noise) ? noise : o;
    o = (native_log2(o) - shadows4) / dynamic4;
    o = clamp(o, (float4)0.0f, (float4)1.0f);

    const float index = prophotorgb_to_XYZ(o).y;

    // Curve S LUT
    o.x = lookup(table, (const float)o.x);
    o.y = lookup(table, (const float)o.y);
    o.z = lookup(table, (const float)o.z);

    // Get the derivative
    derivative = lookup(diff, (const float)index);
    luma = prophotorgb_to_XYZ(o).y;
  }

  // Desaturate selectively
  o = (float4)luma + (float4)derivative * (o - (float4)luma);
  o = clamp(o, (float4)0.0f, (float4)1.0f);

  // Apply the transfer function of the display
  const float4 power4 = power;
  o = native_powr(o, power4);

  i.xyz = prophotorgb_to_Lab(o).xyz;

  write_imagef(out, (int2)(x, y), i);
}


inline float filmic_desaturate(const float x, const float sigma_toe, const float sigma_shoulder, const float saturation)
{
  const float radius_toe = x;
  const float radius_shoulder = 1.0f - x;

  const float key_toe = native_exp(-0.5f * radius_toe * radius_toe / sigma_toe);
  const float key_shoulder = native_exp(-0.5f * radius_shoulder * radius_shoulder / sigma_shoulder);

  return 1.0f - clamp((key_toe + key_shoulder) / saturation, 0.0f, 1.0f);
}


inline float filmic_spline(const float x,
                           const float4 M1, const float4 M2, const float4 M3, const float4 M4, const float4 M5,
                           const float latitude_min, const float latitude_max)
{
  const int toe = (x < latitude_min);
  const int shoulder = (x > latitude_max);
  const int latitude = (toe == shoulder); // == FALSE
  const float4 mask = { toe, shoulder, latitude, 0 };
  const float4 x4 = x;
  return vdot(mask, M1 + x4 * (M2 + x4 * (M3 + x4 * (M4 + x4 * M5))));
}


inline float4 linear_saturation(const float4 x, const float luminance, const float saturation)
{
  const float4 lum = luminance;
  const float4 sat = saturation;
  float4 result = lum + sat * (x - lum);
  result.w = 0.0f;
  return result;
}


kernel void
filmicrgb_split (read_only image2d_t in, write_only image2d_t out,
                 int width, int height,
                 const float dynamic_range, const float black_exposure, const float grey_value,
                 constant dt_colorspaces_iccprofile_info_cl_t *profile_info,
                 read_only image2d_t lut, const int use_work_profile,
                 const float sigma_toe, const float sigma_shoulder, const float saturation,
                 const float4 M1, const float4 M2, const float4 M3, const float4 M4, const float4 M5,
                 const float latitude_min, const float latitude_max, const float output_power)
{
  const unsigned int x = get_global_id(0);
  const unsigned int y = get_global_id(1);

  if(x >= width || y >= height) return;

  const float4 i = read_imagef(in, sampleri, (int2)(x, y));
  float4 o;

  const float4 noise4 = native_powr(2.0f, -16.0f);
  const float4 dynamic4 = dynamic_range;
  const float4 blacks4 = black_exposure;
  const float4 grey4 = grey_value;

  // Log tonemapping
  o = (i < noise4) ? noise4 : i;
  o = (native_log2(o / grey4) - blacks4) / dynamic4;
  o = clamp(o, noise4, (float4)1.0f);

  // Selective desaturation of extreme luminances
  const float luminance = (use_work_profile) ? get_rgb_matrix_luminance(o, profile_info, profile_info->matrix_in, lut) : dt_camera_rgb_luminance(o);
  const float desaturation = filmic_desaturate(luminance, sigma_toe, sigma_shoulder, saturation);
  o = linear_saturation(o, luminance, desaturation);

  // Filmic spline
  o.x = filmic_spline(o.x, M1, M2, M3, M4, M5, latitude_min, latitude_max);
  o.y = filmic_spline(o.y, M1, M2, M3, M4, M5, latitude_min, latitude_max);
  o.z = filmic_spline(o.z, M1, M2, M3, M4, M5, latitude_min, latitude_max);

  // Output power
  o = native_powr(clamp(o, (float4)0.0f, (float4)1.0f), output_power);

  // Copy alpha layer and save
  o.w = i.w;
  write_imagef(out, (int2)(x, y), o);
}


inline float pixel_rgb_norm_power(const float4 pixel)
{
  // weird norm sort of perceptual. This is black magic really, but it looks good.
  const float4 RGB = fabs(pixel);
  const float4 RGB_square = RGB * RGB;
  const float4 RGB_cubic = RGB_square * RGB;
  return (RGB_cubic.x + RGB_cubic.y + RGB_cubic.z) / fmax(RGB_square.x + RGB_square.y + RGB_square.z, 1e-12f);
}


inline float get_pixel_norm(const float4 pixel, const dt_iop_filmicrgb_methods_type_t variant,
                            constant dt_colorspaces_iccprofile_info_cl_t *const profile_info,
                            read_only image2d_t lut, const int use_work_profile)
{
  switch(variant)
  {
    case DT_FILMIC_METHOD_MAX_RGB:
      return fmax(fmax(pixel.x, pixel.y), pixel.z);

    case DT_FILMIC_METHOD_LUMINANCE:
      return (use_work_profile) ? dt_camera_rgb_luminance(pixel): get_rgb_matrix_luminance(pixel, profile_info, profile_info->matrix_in, lut);

    case DT_FILMIC_METHOD_POWER_NORM:
      return pixel_rgb_norm_power(pixel);

    case DT_FILMIC_METHOD_NONE:
      // path cannot be taken
      return 0.0f;
  }
}


kernel void
filmicrgb_chroma (read_only image2d_t in, write_only image2d_t out,
                 int width, int height,
                 const float dynamic_range, const float black_exposure, const float grey_value,
                 constant dt_colorspaces_iccprofile_info_cl_t *profile_info,
                 read_only image2d_t lut, const int use_work_profile,
                 const float sigma_toe, const float sigma_shoulder, const float saturation,
                 const float4 M1, const float4 M2, const float4 M3, const float4 M4, const float4 M5,
                 const float latitude_min, const float latitude_max, const float output_power,
                 const dt_iop_filmicrgb_methods_type_t variant)
{
  const unsigned int x = get_global_id(0);
  const unsigned int y = get_global_id(1);

  if(x >= width || y >= height) return;

  const float4 i = read_imagef(in, sampleri, (int2)(x, y));

  const float noise = native_powr(2.0f, -16.0f);

  float norm = get_pixel_norm(i, variant, profile_info, lut, use_work_profile);
  norm =  (norm < noise) ? noise : norm;

  // Save the ratios
  float4 o = i / (float4)norm;

  // Sanitize the ratios
  const float min_ratios = fmin(fmin(o.x, o.y), o.z);
  if(min_ratios < 0.0f) o -= (float4)min_ratios;

  // Log tonemapping
  norm = (native_log2(norm / grey_value) - black_exposure) / dynamic_range;
  norm = clamp(norm, noise, 1.0f);

  // Selective desaturation of extreme luminances
  o *= (float4)norm;
  const float luminance = (use_work_profile) ? get_rgb_matrix_luminance(o, profile_info, profile_info->matrix_in, lut) : dt_camera_rgb_luminance(o);
  const float desaturation = filmic_desaturate(norm, sigma_toe, sigma_shoulder, saturation);
  o = linear_saturation(o, luminance, desaturation);
  o /= (float4)norm;

  // Filmic spline
  norm = filmic_spline(norm, M1, M2, M3, M4, M5, latitude_min, latitude_max);

  // Output power
  norm = native_powr(clamp(norm, 0.0f, 1.0f), output_power);

  // Copy alpha layer and save
  o *= norm;
  o.w = i.w;
  write_imagef(out, (int2)(x, y), o);
}
