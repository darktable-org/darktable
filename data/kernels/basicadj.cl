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

#include "color_conversion.cl"
#include "rgb_norms.h"

float get_gamma(const float x, const float gamma)
{
  return native_powr(x, gamma);
}

float get_lut_gamma(const float x, const float gamma, read_only image2d_t lut)
{
  if(x > 1.0f)
  {
    return get_gamma(x, gamma);
  }
  else
  {
    const int xi = clamp((int)(x * 0x10000ul), 0, 0xffff);
    const int2 p = (int2)((xi & 0xff), (xi >> 8));
    return read_imagef(lut, sampleri, p).x;
  }
}

float get_contrast(const float x, const float contrast, const float middle_grey, const float inv_middle_grey)
{
  return native_powr(x * inv_middle_grey, contrast) * middle_grey;
}

float get_lut_contrast(const float x, const float contrast, const float middle_grey, const float inv_middle_grey, read_only image2d_t lut)
{
  if(x > 1.0f)
  {
    return get_contrast(x, contrast, middle_grey, inv_middle_grey);
  }
  else
  {
    const int xi = clamp((int)(x * 0x10000ul), 0, 0xffff);
    const int2 p = (int2)((xi & 0xff), (xi >> 8));
    return read_imagef(lut, sampleri, p).x;
  }
}

float hlcurve(const float level, const float hlcomp, const float hlrange)
{
  if(hlcomp > 0.0f)
  {
    float val = level + (hlrange - 1.f);

    // to avoid division by zero
    if(val == 0.0f)
    {
      val = 0.000001f;
    }

    float Y = val / hlrange;
    Y *= hlcomp;

    // to avoid log(<=0)
    if(Y <= -1.0f)
    {
      Y = -.999999f;
    }

    float R = hlrange / (val * hlcomp);
    return log1p(Y) * R;
  }
  else
  {
    return 1.f;
  }
}

kernel void
basicadj(read_only image2d_t in, write_only image2d_t out, const int width, const int height,
           read_only image2d_t lut_gamma, read_only image2d_t lut_contrast,
           const float black_point, const float scale,
           const int process_gamma, const float gamma,
           const int plain_contrast, const int preserve_colors, const float contrast,
           const int process_saturation_vibrance, const float saturation, const float vibrance,
           const int process_hlcompr, const float hlcomp, const float hlrange,
           const float middle_grey, const float inv_middle_grey,
           constant dt_colorspaces_iccprofile_info_cl_t *profile_info, read_only image2d_t lut,
           const int use_work_profile)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 pixel = read_imagef(in, sampleri, (int2)(x, y));
  const float w = pixel.w;

  // exposure
  pixel = (pixel - black_point) * scale;

  // highlight compression
  if(process_hlcompr)
  {
    const float lum = (use_work_profile == 0) ? dt_camera_rgb_luminance(pixel): get_rgb_matrix_luminance(pixel, profile_info, profile_info->matrix_in, lut);
    if(lum > 0.f)
    {
      const float ratio = hlcurve(lum, hlcomp, hlrange);

      pixel *= ratio;
    }
  }
  // gamma
  if(process_gamma)
  {
    if(pixel.x > 0.f) pixel.x = get_lut_gamma(pixel.x, gamma, lut_gamma);
    if(pixel.y > 0.f) pixel.y = get_lut_gamma(pixel.y, gamma, lut_gamma);
    if(pixel.z > 0.f) pixel.z = get_lut_gamma(pixel.z, gamma, lut_gamma);
  }

  // contrast
  if(plain_contrast)
  {
    if(pixel.x > 0.f) pixel.x = get_lut_contrast(pixel.x, contrast, middle_grey, inv_middle_grey, lut_contrast);
    if(pixel.y > 0.f) pixel.y = get_lut_contrast(pixel.y, contrast, middle_grey, inv_middle_grey, lut_contrast);
    if(pixel.z > 0.f) pixel.z = get_lut_contrast(pixel.z, contrast, middle_grey, inv_middle_grey, lut_contrast);
  }

  // contrast (with preserve colors)
  if(preserve_colors != DT_RGB_NORM_NONE)
  {
    float ratio = 1.f;
    const float lum = dt_rgb_norm(pixel, preserve_colors, use_work_profile, profile_info, lut);
    if(lum > 0.f)
    {
      const float contrast_lum = native_powr(lum / middle_grey, contrast) * middle_grey;
      ratio = contrast_lum / lum;
    }

    pixel *= ratio;
  }

  // saturation
  if(process_saturation_vibrance)
  {
    const float average = (pixel.x + pixel.y + pixel.z) / 3;
    const float delta = fast_length(pixel - average);
    const float P = vibrance * (1 - native_powr(delta, fabs(vibrance)));
    pixel = average + (saturation + P) * (pixel - average);
  }

  pixel.w = w;

  write_imagef(out, (int2)(x, y), pixel);
}
