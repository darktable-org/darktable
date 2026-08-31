/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

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

#include "common/bilinear.h"

#ifdef HAVE_OPENCL

#include <stdlib.h>

dt_bilinear_cl_global_t *dt_bilinear_init_cl_global(void)
{
  dt_bilinear_cl_global_t *g = malloc(sizeof(dt_bilinear_cl_global_t));

  const int program = 42; // bilinear.cl, from programs.conf
  g->kernel_bilinear_1c = dt_opencl_create_kernel(program, "bilinear1");
  g->kernel_bilinear_2c = dt_opencl_create_kernel(program, "bilinear2");
  g->kernel_bilinear_4c = dt_opencl_create_kernel(program, "bilinear4");
  g->kernel_bilinear_image = dt_opencl_create_kernel(program, "bilinear_image");
  return g;
}


void dt_bilinear_free_cl_global(dt_bilinear_cl_global_t *g)
{
  if(!g) return;

  dt_opencl_free_kernel(g->kernel_bilinear_1c);
  dt_opencl_free_kernel(g->kernel_bilinear_2c);
  dt_opencl_free_kernel(g->kernel_bilinear_4c);
  dt_opencl_free_kernel(g->kernel_bilinear_image);

  free(g);
}


cl_int dt_interpolate_bilinear_cl(const int devid,
                                  cl_mem dev_in,
                                  const int width_in,
                                  const int height_in,
                                  cl_mem dev_out,
                                  const int width_out,
                                  const int height_out,
                                  const int ch)
{
  const dt_bilinear_cl_global_t *const g = darktable.opencl->bilinear;
  int kernel;

  switch(ch)
  {
    case 1:
      kernel = g->kernel_bilinear_1c;
      break;
    case 2:
      kernel = g->kernel_bilinear_2c;
      break;
    case 4:
      kernel = g->kernel_bilinear_4c;
      break;
    default:
      return DT_OPENCL_PROCESS_CL;
  }

  return dt_opencl_enqueue_kernel_2d_args(devid, kernel, width_out, height_out,
            CLARG(dev_in), CLARG(width_in), CLARG(height_in),
            CLARG(dev_out), CLARG(width_out), CLARG(height_out));
}


cl_int dt_interpolate_bilinear_image_cl(const int devid,
                                        cl_mem dev_in,
                                        const int width_in,
                                        const int height_in,
                                        cl_mem dev_out,
                                        const int width_out,
                                        const int height_out)
{
  const dt_bilinear_cl_global_t *const g = darktable.opencl->bilinear;

  return dt_opencl_enqueue_kernel_2d_args(devid, g->kernel_bilinear_image,
            width_out, height_out,
            CLARG(dev_in), CLARG(width_in), CLARG(height_in),
            CLARG(dev_out), CLARG(width_out), CLARG(height_out));
}

#endif // HAVE_OPENCL

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
