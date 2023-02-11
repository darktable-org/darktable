/*
    This file is part of darktable,
    Copyright (C) 2017-2021 darktable developers.

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

#if defined(__SSE__)
#ifdef __PPC64__
#ifdef NO_WARN_X86_INTRINSICS
#include <xmmintrin.h>
#else
#define NO_WARN_X86_INTRINSICS 1
#include <xmmintrin.h>
#undef NO_WARN_X86_INTRINSICS
#endif // NO_WARN_X86_INTRINSICS
#else
#include <xmmintrin.h>
#endif // __PPC64__
#endif

#include "common/darktable.h"
#include "common/opencl.h"

struct dt_iop_roi_t;

// buffer to store single-channel image along with its dimensions
typedef struct gray_image
{
  float *data;
  int width, height;
} gray_image;


// allocate space for 1-component image of size width x height
static inline gray_image new_gray_image(int width, int height)
{
  return (gray_image){ dt_alloc_align(64, sizeof(float) * width * height), width, height };
}


// free space for 1-component image
static inline void free_gray_image(gray_image *img_p)
{
  dt_free_align(img_p->data);
  img_p->data = NULL;
}


// copy 1-component image img1 to img2
static inline void copy_gray_image(gray_image img1, gray_image img2)
{
  memcpy(img2.data, img1.data, sizeof(float) * img1.width * img1.height);
}


// minimum of two integers
static inline int min_i(int a, int b)
{
  return a < b ? a : b;
}


// maximum of two integers
static inline int max_i(int a, int b)
{
  return a > b ? a : b;
}

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
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
