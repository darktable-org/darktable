/*
    This file is part of darktable,
    Copyright (C) 2020-2024 darktable developers.

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


kernel void
filminversion (read_only image2d_t in, write_only image2d_t out, int width, int height,
               const float4 Dmin, const float4 wb_high, const float4 offset,
               const float contrast, const float exposure)
{
  const unsigned int x = get_global_id(0);
  const unsigned int y = get_global_id(1);

  if(x >= width || y >= height) return;

  const float4 i = read_imagef(in, sampleri, (int2)(x, y));

  // Step 1: density ratio Dmin / clamped_pixel
  const float4 density = Dmin / fmax(i, (float4)2.3283064365386963e-10f);

  // Step 2: -log10(density) via native_log10
  const float4 log_density = -native_log10(density);

  // Step 3: corrected density = wb_high * (-log10(density)) + offset
  const float4 corrected_de = fma(wb_high, log_density, offset);

  // Step 4: convert to linear light
  const float4 print_linear = fmax((float4)1.0f - native_exp10(corrected_de), (float4)0.0f);

  // Step 5: apply contrast power curve and exposure
  float4 o = (float4)exposure * native_powr(fmax(print_linear, (float4)0.0f), (float4)contrast);

  // Preserve alpha channel
  o.w = i.w;

  write_imagef(out, (int2)(x, y), o);
}
