/*
    This file is part of darktable,
    Copyright (C) 2025 darktable developers.

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

#include "common/opencl.h"

#ifdef HAVE_OPENCL

typedef struct dt_bilinear_cl_global_t
{
  int kernel_bilinear_1c;
  int kernel_bilinear_2c;
  int kernel_bilinear_4c;
} dt_bilinear_cl_global_t;

dt_bilinear_cl_global_t *dt_bilinear_init_cl_global(void);

void dt_bilinear_free_cl_global(dt_bilinear_cl_global_t *g);

/** Bilinear resampling of a grey device buffer holding 1, 2 or 4 channels
 *  per pixel, the GPU counterpart of interpolate_bilinear() in
 *  common/fast_guided_filter.h.
 *
 *  Both buffers are plain arrays of `ch` floats per pixel; input and output
 *  must be distinct. Used to down- and upscale the working buffers of the
 *  guided filters.
 */
cl_int dt_interpolate_bilinear_cl(const int devid,
                                  cl_mem dev_in,
                                  const int width_in,
                                  const int height_in,
                                  cl_mem dev_out,
                                  const int width_out,
                                  const int height_out,
                                  const int ch);
#endif

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
