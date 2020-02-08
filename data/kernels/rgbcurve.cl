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

kernel void
rgbcurve(read_only image2d_t in, write_only image2d_t out, const int width, const int height,
           read_only image2d_t table_r, read_only image2d_t table_g, read_only image2d_t table_b,
           constant float *coeffs_r, constant float *coeffs_g, constant float *coeffs_b,
           const int autoscale, const int preserve_colors,
           constant dt_colorspaces_iccprofile_info_cl_t *profile_info, read_only image2d_t lut,
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
    if (preserve_colors == DT_RGB_NORM_NONE)
    {
      pixel.x = lookup_unbounded(table_r, pixel.x, coeffs_r);
      pixel.y = lookup_unbounded(table_r, pixel.y, coeffs_r);
      pixel.z = lookup_unbounded(table_r, pixel.z, coeffs_r);
    }
    else
    {
      float ratio = 1.f;
      const float lum = dt_rgb_norm(pixel, preserve_colors, use_work_profile, profile_info, lut);
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
