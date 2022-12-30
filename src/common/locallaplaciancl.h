#pragma once
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
typedef struct dt_local_laplacian_cl_global_t
{
  int kernel_pad_input;
  int kernel_gauss_expand;
  int kernel_gauss_reduce;
  int kernel_laplacian_assemble;
  int kernel_process_curve;
  int kernel_write_back;
}
dt_local_laplacian_cl_global_t;

typedef struct dt_local_laplacian_cl_t
{
  int devid;
  dt_local_laplacian_cl_global_t *global;

  int width, height;
  int num_levels;
  float sigma, highlights, shadows, clarity;
  int blocksize, blockwd, blockht;
  int max_supp;
  int bwidth, bheight;

  // pyramid of padded monochrome input buffer
  cl_mem *dev_padded;
  // pyramid of padded output buffer, monochrome, too:
  cl_mem *dev_output;
  // one pyramid of padded monochrome buffers for every value
  // of gamma (curve parameter) that we process:
  cl_mem **dev_processed;
}
dt_local_laplacian_cl_t;

dt_local_laplacian_cl_global_t *dt_local_laplacian_init_cl_global();
dt_local_laplacian_cl_t *dt_local_laplacian_init_cl(
    const int devid,
    const int width,            // width of input image
    const int height,           // height of input image
    const float sigma,          // user param: separate shadows/mid-tones/highlights
    const float shadows,        // user param: lift shadows
    const float highlights,     // user param: compress highlights
    const float clarity);       // user param: increase clarity/local contrast
void dt_local_laplacian_free_cl(dt_local_laplacian_cl_t *g);
cl_int dt_local_laplacian_cl(dt_local_laplacian_cl_t *g, cl_mem input, cl_mem output);
#endif
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

