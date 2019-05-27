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

kernel void
rgbcurve(read_only image2d_t in, write_only image2d_t out, const int width, const int height,
           read_only image2d_t table_r, read_only image2d_t table_g, read_only image2d_t table_b,
           global float *coeffs_r, global float *coeffs_g, global float *coeffs_b,
           const int autoscale, const int preserve_colors,
           global const dt_colorspaces_iccprofile_info_cl_t *profile_info, read_only image2d_t lut,
           const int use_work_profile)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 pixel = read_imagef(in, sampleri, (int2)(x, y));

  if(autoscale == 1) // DT_S_SCALE_MANUAL_RGB
  {
    pixel.x = lookup_unbounded(table_r, pixel.x, coeffs_r);
    pixel.y = lookup_unbounded(table_g, pixel.y, coeffs_g);
    pixel.z = lookup_unbounded(table_b, pixel.z, coeffs_b);
  }
  else if(autoscale == 0) // DT_S_SCALE_AUTOMATIC_RGB
  {
    if (preserve_colors == 0) // DT_RGBCURVE_PRESERVE_NONE
    {
      pixel.x = lookup_unbounded(table_r, pixel.x, coeffs_r);
      pixel.y = lookup_unbounded(table_r, pixel.y, coeffs_r);
      pixel.z = lookup_unbounded(table_r, pixel.z, coeffs_r);
    }
    else
    {
      float ratio = 1.f;
      float lum = 0.0f;
      if(preserve_colors == 1) // DT_RGBCURVE_PRESERVE_LUMINANCE
      {
        lum = (use_work_profile == 0) ? dt_camera_rgb_luminance(pixel): get_rgb_matrix_luminance(pixel, profile_info, lut);
      }
      if (preserve_colors == 2) // DT_RGBCURVE_PRESERVE_LMAX
      {
        lum = max(pixel.x, max(pixel.y, pixel.z));
      }
      else if (preserve_colors == 3) // DT_RGBCURVE_PRESERVE_LAVG
      {
        lum = (pixel.x + pixel.y + pixel.z) / 3.0f;
      }
      else if (preserve_colors == 4) // DT_RGBCURVE_PRESERVE_LSUM
      {
        lum = pixel.x + pixel.y + pixel.z;
      }
      else if (preserve_colors == 5) // DT_RGBCURVE_PRESERVE_LNORM
      {
        lum = native_powr(pixel.x * pixel.x + pixel.y * pixel.y + pixel.z * pixel.z, 0.5f);
      }
      else if (preserve_colors == 6) // DT_RGBCURVE_PRESERVE_LBP
      {
        float R, G, B;
        R = pixel.x * pixel.x;
        G = pixel.y * pixel.y;
        B = pixel.z * pixel.z;
        lum = (pixel.x * R + pixel.y * G + pixel.z * B) / (R + G + B);
      }
      if(lum > 0.f)
      {
        const float curve_lum = lookup_unbounded(table_r, lum, coeffs_r);
        ratio = curve_lum / lum;
      }
      pixel.xyz *= ratio;
    }
  }

  write_imagef(out, (int2)(x, y), pixel);
}
