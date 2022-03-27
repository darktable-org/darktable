/*
    This file is part of darktable,
    Copyright (C) 2016-2021 darktable developers.

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
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "develop/imageop.h" // for dt_iop_roi_t

// Allocate a 64-byte aligned buffer for an image of the given dimensions and channels.
// The return value must be freed with dt_free_align().
static inline float *__restrict__ dt_iop_image_alloc(const size_t width, const size_t height, const size_t ch)
{
  return dt_alloc_align_float(width * height * ch);
}

// Allocate one or more buffers as detailed in the given parameters.  If any allocation fails, free all of them,
// set the module's trouble flag, and return FALSE.
//  The variable arguments take the form  SIZE, PTR-to-floatPTR, SIZE, PTR-to-floatPTR, etc. except that if the SIZE
//  indicates a per-thread allocation, a second pointer is passed: SIZE, PTR-to-floatPTR, PTR-to-size_t, SIZE, etc.
//  SIZE is the number of floats per pixel, ORed with appropriate flags from the list following below
gboolean dt_iop_alloc_image_buffers(struct dt_iop_module_t *const module,
                                    const struct dt_iop_roi_t *const roi_in,
                                    const struct dt_iop_roi_t *const roi_out, ...);
// Optional flags to add to size request.  Default is to allocate N channels per pixel according to
// the dimensions of roi_out
#define DT_IMGSZ_CH_MASK    0x000FFFF  // isolate just the number of floats per pixel

#define DT_IMGSZ_ROI_MASK   0x0100000  // isolate just input/output selection
#define DT_IMGSZ_OUTPUT     0x0000000  // read image dimensions from roi_out
#define DT_IMGSZ_INPUT      0x0100000  // read image dimensions from roi_in

#define DT_IMGSZ_PERTHREAD  0x0200000  // allocate a separate buffer for each thread
#define DT_IMGSZ_CLEARBUF   0x0400000  // zero the allocated buffer

#define DT_IMGSZ_DIM_MASK   0x00F0000  // isolate the requested image dimension(s)
#define DT_IMGSZ_FULL       0x0000000  // full height times width
#define DT_IMGSZ_HEIGHT     0x0010000  // buffer equal to one column of the image
#define DT_IMGSZ_WIDTH      0x0020000  // buffer equal to one row of the image
#define DT_IMGSZ_LONGEST    0x0030000  // buffer equal to larger of one row/one column


__DT_CLONE_TARGETS__
static inline void dt_simd_memcpy(const float *const __restrict__ in,
                                  float *const __restrict__ out,
                                  const size_t num_elem)
{
  // Perform a parallel vectorized memcpy on 64-bits aligned
  // contiguous buffers. This is several times faster than the original memcpy

#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
dt_omp_firstprivate(in, out, num_elem) \
schedule(simd:static) aligned(in, out:64)
#endif
  for(size_t k = 0; k < num_elem; k++)
    out[k] = in[k];
}

// Copy an image buffer, specifying the number of floats it contains.  Use of this function is to be preferred
// over a bare memcpy both because it helps document the purpose of the code and because it gives us a single
// point where we can optimize performance on different architectures.
void dt_iop_image_copy(float *const __restrict__ out, const float *const __restrict__ in, const size_t nfloats);

// Copy an image buffer, specifying its dimensions and number of channels.  Use of this function is to be
// preferred over a bare memcpy both because it helps document the purpose of the code and because it gives us
// a single point where we can optimize performance on different architectures.
static inline void dt_iop_image_copy_by_size(float *const __restrict__ out, const float *const __restrict__ in,
                                             const size_t width, const size_t height, const size_t ch)
{
  dt_iop_image_copy(out, in, width * height * ch);
}

// Copy an image buffer, specifying the region of interest.  The output RoI may be larger than the input RoI,
// in which case the result is optionally padded with zeros.  If the output RoI is smaller than the input RoI,
// only a portion of the input buffer will be copied.
void dt_iop_copy_image_roi(float *const __restrict__ out, const float *const __restrict__ in, const size_t ch,
                           const dt_iop_roi_t *const __restrict__ roi_in,
                           const dt_iop_roi_t *const __restrict__ roi_out, const int zero_pad);

// Copy one image buffer to another, multiplying each element by the specified scale factor
void dt_iop_image_scaled_copy(float *const __restrict__ buf, const float *const __restrict__ src, const float scale,
                              const size_t width, const size_t height, const size_t ch);

// Fill an image buffer with a specified value.
void dt_iop_image_fill(float *const buf, const float fill_value, const size_t width, const size_t height,
                       const size_t ch);

// Add a specified constant value to every element of the image buffer
void dt_iop_image_add_const(float *const buf, const float add_value, const size_t width, const size_t height,
                            const size_t ch);

// Add the contents of another image buffer to the given buffer, element-wise
void dt_iop_image_add_image(float *const buf, const float *const other_buf, const size_t width, const size_t height,
                            const size_t ch);

// Subtract the contents of another image buffer from the given buffer, element-wise
void dt_iop_image_sub_image(float *const buf, const float *const other_buf, const size_t width, const size_t height,
                            const size_t ch);

// Subtract each element of the image buffer from the given constant value
void dt_iop_image_invert(float *const buf, const float max_value, const size_t width, const size_t height,
                         const size_t ch);

// Multiply every element of the image buffer by a specified constant value
void dt_iop_image_mul_const(float *const buf, const float mul_value, const size_t width, const size_t height,
                            const size_t ch);

// Divide every element of the image buffer by a specified constant value
void dt_iop_image_div_const(float *const buf, const float div_value, const size_t width, const size_t height,
                            const size_t ch);

// elementwise: buf = lammda*buf + (1-lambda)*other
void dt_iop_image_linear_blend(float *const __restrict__ buf, const float lambda, const float *const __restrict__ other_buf,
                               const size_t width, const size_t height, const size_t ch);

// perform timings to determine the optimal threshold for switching to parallel operations, as well as the
// maximal number of threads before saturating the memory bus
void dt_iop_image_copy_benchmark();

// load configurable settings from darktablerc
void dt_iop_image_copy_configure();

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

