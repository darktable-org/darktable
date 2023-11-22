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
#include "common/darktable.h"
#include "common/dttypes.h"
#include "develop/develop.h"
#include "develop/imageop_math.h"
#include "gui/gtk.h"
#include <bits/stdint-uintn.h>
#include <omp.h>


#include <cairo.h>
#include <stddef.h>
#include <sys/param.h>

void dt_color_finder(const uint8_t *const restrict in, uint8_t *const out, const int width, const int height,
                     const int target_value, const float saturation_adjustment)
{
  const int ch = 4;
#ifdef _OPENMP
#pragma omp parallel for simd default(none)                                                                       \
    dt_omp_sharedconst(in, out, saturation_adjustment, target_value, height, width, ch) schedule(simd             \
                                                                                                 : static)        \
        aligned(in, out : 16)
#endif
  for(size_t i = 0; i < width * height * ch; i += ch)
  {
    // transform input image to HSV
    dt_aligned_pixel_t rgb_in = { in[i], in[i + 1], in[i + 2] };
    dt_aligned_pixel_t hsv_in;
    dt_RGB_2_HSV(rgb_in, hsv_in);

    // apply the operation and transform it back to RGB
    dt_aligned_pixel_t hsv_out = { hsv_in[0], MIN(hsv_in[1] * saturation_adjustment, 1), target_value };
    dt_aligned_pixel_t rgb_out;
    dt_HSV_2_RGB(hsv_out, rgb_out);
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
