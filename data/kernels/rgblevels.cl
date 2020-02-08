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

float rgblevels_1c(const float pixel, global const float *levels, const float inv_gamma, read_only image2d_t lut)
{
  float level;

  if(pixel <= levels[0])
  {
    // Anything below the lower threshold just clips to zero
    level = 0.0f;
  }
  else if(pixel >= levels[2])
  {
    const float percentage = (pixel - levels[0]) / (levels[2] - levels[0]);
    level = native_powr(percentage, inv_gamma);
  }
  else
  {
    // Within the expected input range we can use the lookup table
    const float percentage = (pixel - levels[0]) / (levels[2] - levels[0]);
    level = lookup(lut, percentage);
  }

  return level;
}

kernel void
rgblevels (read_only image2d_t in, write_only image2d_t out, const int width, const int height,
           const int autoscale, const int preserve_colors,
           read_only image2d_t lutr, read_only image2d_t lutg, read_only image2d_t lutb,
           global const float (*const levels)[3], global const float *inv_gamma,
           constant dt_colorspaces_iccprofile_info_cl_t *profile_info, read_only image2d_t lut,
           const int use_work_profile)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 pixel = read_imagef(in, sampleri, (int2)(x, y));

  if(autoscale == 1 || preserve_colors == DT_RGB_NORM_NONE) // autoscale == DT_IOP_RGBLEVELS_INDEPENDENT_CHANNELS
  {
    pixel.x = rgblevels_1c(pixel.x, levels[0], inv_gamma[0], lutr);
    pixel.y = rgblevels_1c(pixel.y, levels[1], inv_gamma[1], lutg);
    pixel.z = rgblevels_1c(pixel.z, levels[2], inv_gamma[2], lutb);
  }
  else
  {
    float ratio = 0.f;
    const float lum = dt_rgb_norm(pixel, preserve_colors, use_work_profile, profile_info, lut);
    if(lum > levels[0][0])
    {
      const float curve_lum = rgblevels_1c(lum, levels[0], inv_gamma[0], lutr);
      ratio = curve_lum / lum;
    }
    pixel.xyz *= ratio;
  }

  write_imagef (out, (int2)(x, y), pixel);
}

