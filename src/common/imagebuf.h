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

#pragma once
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "develop/imageop.h" // for dt_iop_roi_t

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

// Allocate a 64-byte aligned buffer for an image of the given dimensions and channels.
// The return value must be freed with dt_free_align().
static inline float *__restrict__ dt_iop_image_alloc(const size_t width, const size_t height, const size_t ch)
{
  return dt_alloc_align_float(width * height * ch);
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

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
