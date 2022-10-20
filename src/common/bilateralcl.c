/*
    This file is part of darktable,
    Copyright (C) 2016-2020 darktable developers.

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

#ifdef HAVE_OPENCL

#include "common/bilateral.h"
#include "common/bilateralcl.h"
#include "common/darktable.h" // for CLAMPS, dt_print, darktable, darktable_t
#include "common/opencl.h"    // for dt_opencl_set_kernel_arg, dt_opencl_cr...
#include <glib.h>             // for MAX
#include <math.h>             // for roundf
#include <stdlib.h>           // for free, malloc

dt_bilateral_cl_global_t *dt_bilateral_init_cl_global()
{
  dt_bilateral_cl_global_t *b = (dt_bilateral_cl_global_t *)malloc(sizeof(dt_bilateral_cl_global_t));

  const int program = 10; // bilateral.cl, from programs.conf
  b->kernel_zero = dt_opencl_create_kernel(program, "zero");
  b->kernel_splat = dt_opencl_create_kernel(program, "splat");
  b->kernel_blur_line = dt_opencl_create_kernel(program, "blur_line");
  b->kernel_blur_line_z = dt_opencl_create_kernel(program, "blur_line_z");
  b->kernel_slice = dt_opencl_create_kernel(program, "slice");
  b->kernel_slice2 = dt_opencl_create_kernel(program, "slice_to_output");
  return b;
}

void dt_bilateral_free_cl(dt_bilateral_cl_t *b)
{
  if(!b) return;
  // be sure we're done with the memory:
  dt_opencl_finish(b->devid);
  // free device mem
  dt_opencl_release_mem_object(b->dev_grid);
  dt_opencl_release_mem_object(b->dev_grid_tmp);
  free(b);
}


// modules that want to use dt_bilateral_slice_to_output_cl() ought to take this one;
// takes account of an additional temp buffer needed in the OpenCL code path
size_t dt_bilateral_memory_use2(const int width,
                                const int height,
                                const float sigma_s,
                                const float sigma_r)
{
  return dt_bilateral_memory_use(width, height, sigma_s, sigma_r) + sizeof(float) * 4 * width * height;
}

// modules that want to use dt_bilateral_slice_to_output_cl() ought to take this one;
// takes account of an additional temp buffer needed in the OpenCL code path
size_t dt_bilateral_singlebuffer_size2(const int width,
                                       const int height,
                                       const float sigma_s,
                                       const float sigma_r)
{
  return MAX(dt_bilateral_singlebuffer_size(width, height, sigma_s, sigma_r), sizeof(float) * 4 * width * height);
}


dt_bilateral_cl_t *dt_bilateral_init_cl(const int devid,
                                        const int width,     // width of input image
                                        const int height,    // height of input image
                                        const float sigma_s, // spatial sigma (blur pixel coords)
                                        const float sigma_r) // range sigma (blur luma values)
{
  dt_opencl_local_buffer_t locopt
    = (dt_opencl_local_buffer_t){ .xoffset = 0, .xfactor = 1, .yoffset = 0, .yfactor = 1,
                                  .cellsize = 8 * sizeof(float) + sizeof(int), .overhead = 0,
                                  .sizex = 1 << 6, .sizey = 1 << 6 };

  if(!dt_opencl_local_buffer_opt(devid, darktable.opencl->bilateral->kernel_splat, &locopt))
  {
    dt_print(DT_DEBUG_OPENCL,
             "[opencl_bilateral] can not identify resource limits for device %d in bilateral grid\n", devid);
    return NULL;
  }

  if(locopt.sizex * locopt.sizey < 16 * 16)
  {
    dt_print(DT_DEBUG_OPENCL,
             "[opencl_bilateral] device %d does not offer sufficient resources to run bilateral grid\n",
             devid);
    return NULL;
  }

  dt_bilateral_cl_t *b = (dt_bilateral_cl_t *)malloc(sizeof(dt_bilateral_cl_t));
  if(!b) return NULL;

  b->global = darktable.opencl->bilateral;
  b->width = width;
  b->height = height;
  b->blocksizex = locopt.sizex;
  b->blocksizey = locopt.sizey;
  b->devid = devid;
  b->dev_grid = NULL;
  b->dev_grid_tmp = NULL;
  dt_bilateral_t b2;
  dt_bilateral_grid_size(&b2,width,height,100.0f,sigma_s,sigma_r);
  b->size_x = b2.size_x;
  b->size_y = b2.size_y;
  b->size_z = b2.size_z;
  b->sigma_s = b2.sigma_s;
  b->sigma_r = b2.sigma_r;

  // alloc grid buffer:
  b->dev_grid
      = dt_opencl_alloc_device_buffer(b->devid, sizeof(float) * b->size_x * b->size_y * b->size_z);
  if(!b->dev_grid)
  {
    dt_bilateral_free_cl(b);
    return NULL;
  }

  // alloc temporary grid buffer
  b->dev_grid_tmp
      = dt_opencl_alloc_device_buffer(b->devid, sizeof(float) * b->size_x * b->size_y * b->size_z);
  if(!b->dev_grid_tmp)
  {
    dt_bilateral_free_cl(b);
    return NULL;
  }

  // zero out grid
  int wd = b->size_x, ht = b->size_y * b->size_z;
  cl_int err = dt_opencl_enqueue_kernel_2d_args(b->devid, b->global->kernel_zero, wd, ht,
    CLARG(b->dev_grid), CLARG(wd), CLARG(ht));
  if(err != CL_SUCCESS)
  {
    dt_bilateral_free_cl(b);
    return NULL;
  }

#if 0
  fprintf(stderr, "[bilateral] created grid [%d %d %d]"
          " with sigma (%f %f) (%f %f)\n", b->size_x, b->size_y, b->size_z,
          b->sigma_s, sigma_s, b->sigma_r, sigma_r);
#endif
  return b;
}

cl_int dt_bilateral_splat_cl(dt_bilateral_cl_t *b, cl_mem in)
{
  cl_int err = -666;
  size_t sizes[] = { ROUNDUP(b->width, b->blocksizex), ROUNDUP(b->height, b->blocksizey), 1 };
  size_t local[] = { b->blocksizex, b->blocksizey, 1 };
  dt_opencl_set_kernel_args(b->devid, b->global->kernel_splat, 0, CLARG(in), CLARG(b->dev_grid), CLARG(b->width), CLARG(b->height), CLARG(b->size_x), CLARG(b->size_y), CLARG(b->size_z), CLARG(b->sigma_s), CLARG(b->sigma_r), CLLOCAL(b->blocksizex * b->blocksizey * sizeof(int)), CLLOCAL(b->blocksizex * b->blocksizey * 8 * sizeof(float)));
  err = dt_opencl_enqueue_kernel_2d_with_local(b->devid, b->global->kernel_splat, sizes, local);
  return err;
}

cl_int dt_bilateral_blur_cl(dt_bilateral_cl_t *b)
{
  cl_int err = -666;
  size_t sizes[3] = { 0, 0, 1 };

  err = dt_opencl_enqueue_copy_buffer_to_buffer(b->devid, b->dev_grid, b->dev_grid_tmp, 0, 0,
                                                b->size_x * b->size_y * b->size_z * sizeof(float));
  if(err != CL_SUCCESS) return err;

  sizes[0] = ROUNDUPDWD(b->size_z, b->devid);
  sizes[1] = ROUNDUPDHT(b->size_y, b->devid);
  int stride1, stride2, stride3;
  stride1 = b->size_x * b->size_y;
  stride2 = b->size_x;
  stride3 = 1;
  dt_opencl_set_kernel_args(b->devid, b->global->kernel_blur_line, 0, CLARG(b->dev_grid_tmp), CLARG(b->dev_grid), CLARG(stride1), CLARG(stride2), CLARG(stride3), CLARG(b->size_z), CLARG(b->size_y), CLARG(b->size_x));
  err = dt_opencl_enqueue_kernel_2d(b->devid, b->global->kernel_blur_line, sizes);
  if(err != CL_SUCCESS) return err;

  stride1 = b->size_x * b->size_y;
  stride2 = 1;
  stride3 = b->size_x;
  sizes[0] = ROUNDUPDWD(b->size_z, b->devid);
  sizes[1] = ROUNDUPDHT(b->size_x, b->devid);
  dt_opencl_set_kernel_args(b->devid, b->global->kernel_blur_line, 0, CLARG(b->dev_grid), CLARG(b->dev_grid_tmp), CLARG(stride1), CLARG(stride2), CLARG(stride3), CLARG(b->size_z), CLARG(b->size_x), CLARG(b->size_y));
  err = dt_opencl_enqueue_kernel_2d(b->devid, b->global->kernel_blur_line, sizes);
  if(err != CL_SUCCESS) return err;

  stride1 = 1;
  stride2 = b->size_x;
  stride3 = b->size_x * b->size_y;
  sizes[0] = ROUNDUPDWD(b->size_x, b->devid);
  sizes[1] = ROUNDUPDHT(b->size_y, b->devid);
  dt_opencl_set_kernel_args(b->devid, b->global->kernel_blur_line_z, 0, CLARG(b->dev_grid_tmp), CLARG(b->dev_grid), CLARG(stride1), CLARG(stride2), CLARG(stride3), CLARG(b->size_x), CLARG(b->size_y), CLARG(b->size_z));
  err = dt_opencl_enqueue_kernel_2d(b->devid, b->global->kernel_blur_line_z, sizes);
  return err;
}

cl_int dt_bilateral_slice_to_output_cl(dt_bilateral_cl_t *b, cl_mem in, cl_mem out, const float detail)
{
  cl_int err = -666;
  cl_mem tmp = NULL;

  tmp = dt_opencl_alloc_device(b->devid, b->width, b->height, sizeof(float) * 4);
  if(tmp == NULL) goto error;

  size_t origin[] = { 0, 0, 0 };
  size_t region[] = { b->width, b->height, 1 };
  err = dt_opencl_enqueue_copy_image(b->devid, out, tmp, origin, origin, region);
  if(err != CL_SUCCESS) goto error;

  err = dt_opencl_enqueue_kernel_2d_args(b->devid, b->global->kernel_slice2, b->width, b->height,
    CLARG(in), CLARG(tmp), CLARG(out), CLARG(b->dev_grid), CLARG(b->width), CLARG(b->height), CLARG(b->size_x), CLARG(b->size_y), CLARG(b->size_z), CLARG(b->sigma_s), CLARG(b->sigma_r), CLARG(detail));

  dt_opencl_release_mem_object(tmp);
  return err;

error:
  dt_opencl_release_mem_object(tmp);
  return err;
}

cl_int dt_bilateral_slice_cl(dt_bilateral_cl_t *b, cl_mem in, cl_mem out, const float detail)
{
  cl_int err = -666;
  err = dt_opencl_enqueue_kernel_2d_args(b->devid, b->global->kernel_slice, b->width, b->height,
    CLARG(in), CLARG(out), CLARG(b->dev_grid), CLARG(b->width), CLARG(b->height), CLARG(b->size_x), CLARG(b->size_y), CLARG(b->size_z), CLARG(b->sigma_s), CLARG(b->sigma_r), CLARG(detail));
  return err;
}

void dt_bilateral_free_cl_global(dt_bilateral_cl_global_t *b)
{
  if(!b) return;
  // destroy kernels
  dt_opencl_free_kernel(b->kernel_zero);
  dt_opencl_free_kernel(b->kernel_splat);
  dt_opencl_free_kernel(b->kernel_blur_line);
  dt_opencl_free_kernel(b->kernel_blur_line_z);
  dt_opencl_free_kernel(b->kernel_slice);
  dt_opencl_free_kernel(b->kernel_slice2);
  free(b);
}

#endif

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

