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

#include "common/imagebuf.h"

// Copy an image buffer, specifying the number of floats it contains.  Use of this function is to be preferred
// over a bare memcpy both because it helps document the purpose of the code and because it gives us a single
// point where we can optimize performance on different architectures.
void dt_iop_image_copy(float *const __restrict__ out, const float *const __restrict__ in, const size_t nfloats)
{
#ifdef _OPENMP
  if (nfloats > 500000)	// is the copy big enough to outweigh threading overhead?
  {
    // we can gain a little by using a small number of threads in parallel, but not much since the memory bus
    // quickly saturates (basically, each core can saturate a memory channel, so a system with quad-channel
    // memory won't be able to take advantage of more than four cores).
    const int nthreads = darktable.num_openmp_threads < 4 ? darktable.num_openmp_threads : 4;
#pragma omp parallel for simd default(none) \
    dt_omp_firstprivate(in, out, nfloats) schedule(simd:static) num_threads(nthreads)
    for(size_t k = 0; k < nfloats; k++)
      out[k] = in[k];
    return;
  }
#endif // _OPENMP
  // no OpenMP, or image too small to bother parallelizing
  memcpy(out, in, nfloats * sizeof(float));
}

// Copy an image buffer, specifying the regions of interest.  The output RoI may be larger than the input RoI,
// in which case the result is optionally padded with zeros.  If the output RoI is smaller than the input RoI,
// only a portion of the input buffer will be copied.
void dt_iop_copy_image_roi(float *const __restrict__ out, const float *const __restrict__ in, const size_t ch,
                           const dt_iop_roi_t *const __restrict__ roi_in,
                           const dt_iop_roi_t *const __restrict__ roi_out, const int zero_pad)
{
  if (roi_in->width == roi_out->width && roi_in->height == roi_out->height)
  {
    // fast path, just copy the entire contents of the buffer
    dt_iop_image_copy_by_size(out, in, roi_out->width, roi_out->height, ch);
  }
  else if (roi_in->width <= roi_out->width && roi_in->height <= roi_out->height)
  {
    // output needs padding
    fprintf(stderr,"copy_image_roi with larger output not yet implemented\n");
    //TODO
  }
  else if (roi_in->width >= roi_out->width && roi_in->height >= roi_out->height)
  {
    // copy only a portion of the input
    fprintf(stderr,"copy_image_roi with smaller output not yet implemented\n");
    //TODO
  }
  else
  {
    // inconsistent RoIs!!
    fprintf(stderr,"copy_image_roi called with inconsistent RoI!\n");
    //TODO
  }
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
