/*
    This file is part of darktable,
    Copyright (C) 2017-2020 darktable developers.

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

struct dt_iop_roi_t;

void guided_filter(const float *guide, const float *in, float *out, int width, int height, int ch, int w,
                   float sqrt_eps, float guide_weight, float min, float max);

#ifdef HAVE_OPENCL

typedef struct dt_guided_filter_cl_global_t
{
  int kernel_guided_filter_split_rgb;
  int kernel_guided_filter_box_mean_x;
  int kernel_guided_filter_box_mean_y;
  int kernel_guided_filter_guided_filter_covariances;
  int kernel_guided_filter_guided_filter_variances;
  int kernel_guided_filter_update_covariance;
  int kernel_guided_filter_solve;
  int kernel_guided_filter_generate_result;
} dt_guided_filter_cl_global_t;


dt_guided_filter_cl_global_t *dt_guided_filter_init_cl_global();

void dt_guided_filter_free_cl_global(dt_guided_filter_cl_global_t *g);

void guided_filter_cl(int devid, cl_mem guide, cl_mem in, cl_mem out, int width, int height, int ch, int w,
                      float sqrt_eps, float guide_weight, float min, float max);

#endif
