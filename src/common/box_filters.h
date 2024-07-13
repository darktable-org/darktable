/*
    This file is part of darktable,
    Copyright (C) 2009-2024 darktable developers.

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

#pragma once

#include <stdlib.h>
#include <inttypes.h>

// default number of iterations to run for dt_box_mean
#define BOX_ITERATIONS 8

// flag to add to number of channels to request the slower but more accurate version using Kahan (compensated)
// summation
#define BOXFILTER_KAHAN_SUM 0x1000000

#ifdef __cplusplus
extern "C" {
#endif
  
// ch = number of channels per pixel.  Supported values: 1, 2, 4, and 4|Kahan
void dt_box_mean(float *const buf, const size_t height, const size_t width, const uint32_t ch,
                 const size_t radius, const uint32_t interations);
// run a single iteration horizonally over a single row.  Supported values for ch: 4|Kahan
// 'scratch' must point at a buffer large enough to hold ch*width floats, or be NULL
void dt_box_mean_horizontal(float *const buf, const size_t width, const uint32_t ch, const size_t radius,
                            float *const scratch);
// run a single iteration vertically over the entire image.  Supported values for ch: 4|Kahan
void dt_box_mean_vertical(float *const buf, const size_t height, const size_t width, const uint32_t ch, const size_t radius);

void dt_box_min(float *const buf, const size_t height, const size_t width, const uint32_t ch, const size_t radius);
void dt_box_max(float *const buf, const size_t height, const size_t width, const uint32_t ch, const size_t radius);

#ifdef __cplusplus
}
#endif

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

