/*
    This file is part of darktable,
    Copyright (C) 2019-2021 darktable developers.
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

#include "common/color_finder.h"
#include "common/colorspaces_inline_conversions.h"

void dt_color_finder(const uint8_t *const restrict in, uint8_t *const restrict out, const int width,
                     const int height, const int target_value, const float saturation_adjustment)
{
  const int ch = 4;

#ifdef _OPENMP
#pragma omp parallel for simd default(none)                                                                       \
    dt_omp_sharedconst(in, out, saturation_adjustment, target_value, height, width, ch) schedule(simd             \
                                                                                                 : static)        \
        aligned(in, out : 64)
#endif
  for(size_t i = 0; i < width * height * ch; i += ch)
  {
    float s = saturation_adjustment;
    dt_aligned_pixel_t rgb_in = { in[i], in[i + 1], in[i + 2] };
    dt_aligned_pixel_t yuv_in, rgb_out;

    dt_RGB_to_YCbCr(rgb_in, yuv_in);

    dt_aligned_pixel_t yuv_out = { target_value, yuv_in[1] * s, yuv_in[2] * s };

    // do a test conversion back
    // for the default target_value=128 and saturation_adjustment=1 this always
    // works
    dt_YCbCr_to_RGB(yuv_out, rgb_out);

    // for non default values the colors might get push outside the valid RGB
    // space so check them
    if(rgb_out[0] > 255 || rgb_out[0] < 0 || rgb_out[1] > 255 || rgb_out[1] < 0 || rgb_out[2] > 255
       || rgb_out[2] < 0)
    {
      // for invalid colors determine the maximum possible saturation given
      // Cb, Cr, the target value and the transformation paramters inside
      // dt_YCbCr_to_RGB() in colorspaces_inline_conversions.h
      const float s_r_max = (yuv_in[2] == 0)  ? 0
                            : (yuv_in[2] > 0) ? (254 - target_value) / (1.140f * yuv_in[2])
                                              : (1 - target_value) / (1.140f * yuv_in[2]);
      const float s_b_max = (yuv_in[1] == 0)  ? 0
                            : (yuv_in[1] > 0) ? (254 - target_value) / (2.028f * yuv_in[1])
                                              : (1 - target_value) / (2.028f * yuv_in[1]);
      const float alpha = 0.394f * yuv_in[1] + 0.581f * yuv_in[2];
      const float s_g_max = (alpha > 0) ? (target_value - 1) / alpha : (target_value - 254) / alpha;

      // apply the max saturation and redo the YCbCr -> RGB conversion
      s = MIN(MIN(s_r_max, s_b_max), s_g_max);

      yuv_out[1] = yuv_in[1] * s;
      yuv_out[2] = yuv_in[2] * s;

      dt_YCbCr_to_RGB(yuv_out, rgb_out);
    }

    rgb_out[3] = 255;

    // set the data
    for_four_channels(c) out[i + c] = rgb_out[c];
  }
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
