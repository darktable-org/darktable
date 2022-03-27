/*
    This file is part of darktable,
    Copyright (C) 2012-2020 darktable developers.

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

#ifdef HAVE_OPENCL
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <CL/cl.h>          // for cl_mem, _cl_mem
#include <stddef.h>         // for size_t

typedef struct dt_bilateral_cl_global_t
{
  int kernel_zero, kernel_splat, kernel_blur_line, kernel_blur_line_z, kernel_slice, kernel_slice2;
} dt_bilateral_cl_global_t;

typedef struct dt_bilateral_cl_t
{
  dt_bilateral_cl_global_t *global;
  int devid;
  size_t size_x, size_y, size_z;
  int width, height;
  size_t blocksizex, blocksizey;
  float sigma_s, sigma_r;
  cl_mem dev_grid;
  cl_mem dev_grid_tmp;
} dt_bilateral_cl_t;

dt_bilateral_cl_global_t *dt_bilateral_init_cl_global();

void dt_bilateral_free_cl(dt_bilateral_cl_t *b);

dt_bilateral_cl_t *dt_bilateral_init_cl(const int devid,
                                        const int width,      // width of input image
                                        const int height,     // height of input image
                                        const float sigma_s,  // spatial sigma (blur pixel coords)
                                        const float sigma_r); // range sigma (blur luma values)

cl_int dt_bilateral_splat_cl(dt_bilateral_cl_t *b, cl_mem in);

cl_int dt_bilateral_blur_cl(dt_bilateral_cl_t *b);

cl_int dt_bilateral_slice_to_output_cl(dt_bilateral_cl_t *b, cl_mem in, cl_mem out, const float detail);

cl_int dt_bilateral_slice_cl(dt_bilateral_cl_t *b, cl_mem in, cl_mem out, const float detail);

void dt_bilateral_free_cl_global(dt_bilateral_cl_global_t *b);

#endif // HAVE_OPENCL

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

