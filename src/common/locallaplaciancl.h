#pragma once
/*
    This file is part of darktable,
    copyright (c) 2016 johannes hanika.

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
#ifdef HAVE_OPENCL_TODO
typedef struct local_laplacian_cl_global_t
{
  int kernel_pad_input;
  int kernel_gauss_expand;
  int kernel_gauss_reduce;
  int kernel_laplacian_assemble;
  int kernel_process_curve;
  int kernel_write_back;
}
local_laplacian_cl_global_t;

typedef struct local_laplacian_cl_t
{
  // pyramid of padded monochrome input buffer
  cl_mem *padded;
  // pyramid of padded output buffer, monochrome, too:
  cl_mem *output;
  // one pyramid of padded monochrome buffers for every value
  // of gamma (curve parameter) that we process:
  cl_mem **processed;
  int devid;
}
local_laplacian_cl_t;

local_laplacian_cl_global_t *local_laplacian_init_cl_global();
void local_laplacian_free_cl(local_laplacian_cl_t *g);
void local_laplacian_free_cl(local_laplacian_cl_t *g);
void local_laplacian_cl();
#endif
