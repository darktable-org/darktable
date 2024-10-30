/*
    This file is part of darktable,
    Copyright (C) 2016-2024 darktable developers.

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

#include <stdarg.h>
#include "common/imagebuf.h"

static size_t parallel_imgop_minimum = 500000;
static size_t parallel_imgop_maxthreads = 4;

// Allocate one or more buffers as detailed in the given parameters.
// If any allocation fails, free all of them, set the module's trouble
// flag, and return FALSE.
gboolean dt_iop_alloc_image_buffers(struct dt_iop_module_t *const module,
                                    const struct dt_iop_roi_t *const roi_in,
                                    const struct dt_iop_roi_t *const roi_out, ...)
{
  gboolean success = TRUE;
  va_list args;
  // first pass: zero out all of the given buffer pointers
  va_start(args,roi_out);
  while(TRUE)
  {
    const int size = va_arg(args,int);
    float **bufptr = va_arg(args,float**);
    if(size & DT_IMGSZ_PERTHREAD)
      (void)va_arg(args,size_t*);    // skip the extra pointer for per-thread allocations
    if(size == 0 || !bufptr)        // end of arg list?
      break;
    *bufptr = NULL;
  }
  va_end(args);

  // second pass: attempt to allocate the requested buffers
  va_start(args,roi_out);
  while(success)
  {
    const int size = va_arg(args,int);
    float **bufptr = va_arg(args,float**);
    size_t *paddedsize = (size & DT_IMGSZ_PERTHREAD) ? va_arg(args,size_t*) : NULL;
    if(size == 0 || !bufptr)
      break;
    const size_t channels = size & DT_IMGSZ_CH_MASK;
    size_t nfloats;
    switch(size & (DT_IMGSZ_ROI_MASK | DT_IMGSZ_DIM_MASK))
    {
    case DT_IMGSZ_OUTPUT | DT_IMGSZ_FULL:
      nfloats = channels * roi_out->width * roi_out->height;
      break;
    case DT_IMGSZ_OUTPUT | DT_IMGSZ_HEIGHT:
      nfloats = channels * roi_out->height;
      break;
    case DT_IMGSZ_OUTPUT | DT_IMGSZ_WIDTH:
      nfloats = channels * roi_out->width;
      break;
    case DT_IMGSZ_OUTPUT | DT_IMGSZ_LONGEST:
      nfloats = channels * MAX(roi_out->width, roi_out->height);
      break;
    case DT_IMGSZ_INPUT | DT_IMGSZ_FULL:
      nfloats = channels * roi_in->width * roi_in->height;
      break;
    case DT_IMGSZ_INPUT | DT_IMGSZ_HEIGHT:
      nfloats = channels * roi_in->height;
      break;
    case DT_IMGSZ_INPUT | DT_IMGSZ_WIDTH:
      nfloats = channels * roi_in->width;
      break;
    case DT_IMGSZ_INPUT | DT_IMGSZ_LONGEST:
      nfloats = channels * MAX(roi_in->width, roi_in->height);
      break;
    default:
      nfloats = 0;
      break;
    }
    if(size & DT_IMGSZ_PERTHREAD)
    {
      *bufptr = dt_alloc_perthread_float(nfloats,paddedsize);
      if((size & DT_IMGSZ_CLEARBUF) && *bufptr)
        memset(*bufptr, 0, *paddedsize * dt_get_num_threads() * sizeof(float));
    }
    else
    {
      *bufptr = dt_alloc_align_float(nfloats);
      if((size & DT_IMGSZ_CLEARBUF) && *bufptr)
        memset(*bufptr, 0, nfloats * sizeof(float));
    }
    if(!*bufptr)
    {
      success = FALSE;
      break;
    }
  }
  va_end(args);

  // finally, check whether successful and clean up if something went wrong
  if(success)
  {
    if(module)
      dt_iop_set_module_trouble_message(module, NULL, NULL, NULL);
  }
  else
  {
    va_start(args,roi_out);
    while(TRUE)
    {
      const int size = va_arg(args,int);
      float **bufptr = va_arg(args,float**);
      if(size & DT_IMGSZ_PERTHREAD)
        (void)va_arg(args,size_t*);  // skip the extra pointer for per-thread allocations
      if(size == 0 || !bufptr || !*bufptr)
        break;  // end of arg list or this attempted allocation failed
      dt_free_align(*bufptr);
      *bufptr = NULL;
    }
    va_end(args);
    // set the module's trouble flag
    if(module)
      dt_iop_set_module_trouble_message(module, _("insufficient memory"),
                                        _("this module was unable to allocate\n"
                                          "all of the memory required to process\n"
                                          "the image.  some or all processing\n"
                                          "has been skipped."),
                                        "unable to allocate working memory");
  }
  return success;
}


// Copy an image buffer, specifying the number of floats it contains.
// Use of this function is to be preferred over a bare memcpy both
// because it helps document the purpose of the code and because it
// gives us a single point where we can optimize performance on
// different architectures.
void dt_iop_image_copy(float *const __restrict__ out,
                       const float *const __restrict__ in,
                       const size_t nfloats)
{
#ifdef _OPENMP
  if(nfloats > parallel_imgop_minimum)	// is the copy big enough to outweigh threading overhead?
  {
    float *const outv __attribute__((aligned(16))) = out;
    const float *const inv __attribute__((aligned(16))) = in;
    // we can gain a little by using a small number of threads in
    // parallel, but not much since the memory bus quickly saturates
    // (basically, each core can saturate a memory channel, so a
    // system with quad-channel memory won't be able to take advantage
    // of more than four cores).
    const int nthreads = MIN(dt_get_num_threads(), parallel_imgop_maxthreads);
    // determine the number of 4-float vectors to be processed by each thread
    const size_t chunksize = (((nfloats + nthreads - 1) / nthreads) + 3) / 4;
    DT_OMP_FOR(num_threads(nthreads))
    for(size_t chunk = 0; chunk < nthreads; chunk++)
    {
      const size_t limit = MIN(4*(chunk+1)*chunksize, nfloats);
      const size_t limit4 = limit & ~3;
      for(size_t k = 4 * chunk * chunksize; k < limit4; k += 4)
        copy_pixel_nontemporal(outv + k, inv + k);
      // handle any leftover pixels in the final slice
      for(size_t k = 0; k < (limit & 3); k++)
        outv[k + limit4] = inv[k + limit4];
    }
    return;
  }
#endif // _OPENMP
  // no OpenMP, or image too small to bother parallelizing
  memcpy(out, in, nfloats * sizeof(float));
}

// Copy an image buffer, specifying the regions of interest.  The
// output RoI may be larger than the input RoI, in which case the
// result is padded with zeros.  If the output RoI is
// smaller than the input RoI, only a portion of the input buffer will
// be copied.
void dt_iop_copy_image_roi(float *const __restrict__ out,
                           const float *const __restrict__ in,
                           const size_t ch,
                           const dt_iop_roi_t *const __restrict__ roi_in,
                           const dt_iop_roi_t *const __restrict__ roi_out)
{
  if(roi_in->width == roi_out->width
     && roi_in->height == roi_out->height)
  {
    // fast path, just copy the entire contents of the buffer
    dt_iop_image_copy_by_size(out, in, roi_out->width, roi_out->height, ch);
    return;
  }

  const int dy = roi_out->y - roi_in->y;
  const int dx = roi_out->x - roi_in->x;

  // we may copy data per line if data for roi_out are fully available in roi_in
  if((roi_in->width - dx >= roi_out->width)
      && (roi_in->height - dy >= roi_out->height))
  {
    const size_t lwidth = sizeof(float) * roi_out->width * ch;
    DT_OMP_FOR()
    for(size_t row = 0; row < roi_out->height; row++)
    {
      float *o = out + (size_t)(ch * row * roi_out->width);
      const float *i = in + (size_t)(ch * (roi_in->width * (row + dy) + dx));
      memcpy(o, i, lwidth);
    }
    return;
  }

  // the RoI are inconsistant so we do a copy per location and fill by zero if
  // not available in RoI-in
  DT_OMP_FOR(collapse(2))
  for(int row = 0; row < roi_out->height; row++)
  {
    for(int col = 0; col < roi_out->width; col++)
    {
      const int irow = row + dy;
      const int icol = col + dx;
      const size_t ox = (size_t)ch * (row * roi_out->width + col);
      const size_t ix = (size_t)ch * (irow * roi_in->width + icol);
      const gboolean avail = irow >= 0 && irow < roi_in->height
                          && icol >= 0 && icol < roi_in->width;

      for(int c = 0; c < ch; c++)
        out[ox+c] = avail ? in[ix+c] : 0.0f;
    }
  }
}

void dt_iop_image_scaled_copy(float *const restrict buf,
                              const float *const restrict src,
                              const float scale,
                              const size_t width,
                              const size_t height,
                              const size_t ch)
{
  const size_t nfloats = width * height * ch;
#ifdef _OPENMP
  if(nfloats > parallel_imgop_minimum)	// is the copy big enough to outweigh threading overhead?
  {
    // we can gain a little by using a small number of threads in
    // parallel, but not much since the memory bus quickly saturates
    // (basically, each core can saturate a memory channel, so a
    // system with quad-channel memory won't be able to take advantage
    // of more than four cores).
    const int nthreads = MIN(dt_get_num_threads(), parallel_imgop_maxthreads);
    DT_OMP_FOR_SIMD(num_threads(nthreads) aligned(buf, src : 16))
    for(size_t k = 0; k < nfloats; k++)
      buf[k] = scale * src[k];
    return;
  }
#endif // _OPENMP
  // no OpenMP, or image too small to bother parallelizing
  DT_OMP_SIMD(aligned(buf, src : 16))
  for(size_t k = 0; k < nfloats; k++)
    buf[k] = scale * src[k];
}

void dt_iop_image_fill(float *const buf,
                       const float fill_value,
                       const size_t width,
                       const size_t height,
                       const size_t ch)
{
  const size_t nfloats = width * height * ch;
#ifdef _OPENMP
  if(nfloats > parallel_imgop_minimum)	// is the copy big enough to outweigh threading overhead?
  {
    const size_t nthreads = MIN(16, dt_get_num_threads());
    // determine the number of 4-float vectors to be processed by each thread
    const size_t chunksize = (((nfloats + nthreads - 1) / nthreads) + 3) / 4;
    DT_OMP_FOR(num_threads(nthreads))
    for(size_t chunk = 0; chunk < nthreads; chunk++)
    {
      size_t limit = MIN(4*(chunk+1)*chunksize, nfloats);
      size_t limit4 = limit & ~3;
      dt_aligned_pixel_t pix = { fill_value, fill_value,fill_value, fill_value };
      for(size_t k = 4 * chunk * chunksize; k < limit4; k += 4)
        copy_pixel_nontemporal(buf + k, pix);
      // handle any leftover pixels in the final slice
      for(size_t k  = 0; k < (limit & 3); k++)
        buf[k + limit4] = fill_value;
    }
    return;
  }
#endif // _OPENMP
  // no OpenMP, or image too small to bother parallelizing
  if(fill_value == 0.0f)
  {
    // take advantage of compiler intrinsic which is hopefully highly optimized
    memset(buf, 0, sizeof(float) * nfloats);
  }
  else
  {
    DT_OMP_SIMD(aligned(buf:16))
    for(size_t k = 0; k < nfloats; k++)
      buf[k] = fill_value;
  }
}

void dt_iop_image_add_const(float *const buf,
                            const float add_value,
                            const size_t width,
                            const size_t height,
                            const size_t ch)
{
  const size_t nfloats = width * height * ch;
#ifdef _OPENMP
  if(nfloats > parallel_imgop_minimum)	// is the copy big enough to outweigh threading overhead?
  {
    // we can gain a little by using a small number of threads in
    // parallel, but not much since the memory bus quickly saturates
    // (basically, each core can saturate a memory channel, so a
    // system with quad-channel memory won't be able to take advantage
    // of more than four cores).
    const int nthreads = MIN(dt_get_num_threads(), parallel_imgop_maxthreads);
    DT_OMP_FOR_SIMD(num_threads(nthreads))
    for(size_t k = 0; k < nfloats; k++)
      buf[k] += add_value;
    return;
  }
#endif // _OPENMP
  // no OpenMP, or image too small to bother parallelizing
  DT_OMP_SIMD(aligned(buf:16))
  for(size_t k = 0; k < nfloats; k++)
    buf[k] += add_value;
}

void dt_iop_image_add_image(float *const buf,
                            const float* const other_image,
                            const size_t width,
                            const size_t height,
                            const size_t ch)
{
  const size_t nfloats = width * height * ch;
#ifdef _OPENMP
  if(nfloats > parallel_imgop_minimum)	// is the copy big enough to outweigh threading overhead?
  {
    // we can gain a little by using a small number of threads in
    // parallel, but not much since the memory bus quickly saturates
    // (basically, each core can saturate a memory channel, so a
    // system with quad-channel memory won't be able to take advantage
    // of more than four cores).
    const int nthreads = MIN(dt_get_num_threads(), parallel_imgop_maxthreads);
    DT_OMP_FOR_SIMD(num_threads(nthreads) aligned(buf, other_image : 16))
    for(size_t k = 0; k < nfloats; k++)
      buf[k] += other_image[k];
    return;
  }
#endif // _OPENMP
  // no OpenMP, or image too small to bother parallelizing
  DT_OMP_SIMD(aligned(buf, other_image : 16))
  for(size_t k = 0; k < nfloats; k++)
    buf[k] += other_image[k];
}

void dt_iop_image_sub_image(float *const buf,
                            const float* const other_image,
                            const size_t width,
                            const size_t height,
                            const size_t ch)
{
  const size_t nfloats = width * height * ch;
#ifdef _OPENMP
  if(nfloats > parallel_imgop_minimum)	// is the copy big enough to outweigh threading overhead?
  {
    // we can gain a little by using a small number of threads in
    // parallel, but not much since the memory bus quickly saturates
    // (basically, each core can saturate a memory channel, so a
    // system with quad-channel memory won't be able to take advantage
    // of more than four cores).
    const int nthreads = MIN(dt_get_num_threads(), parallel_imgop_maxthreads);
    DT_OMP_FOR_SIMD(num_threads(nthreads) aligned(buf, other_image : 16))
    for(size_t k = 0; k < nfloats; k++)
      buf[k] -= other_image[k];
    return;
  }
#endif // _OPENMP
  // no OpenMP, or image too small to bother parallelizing
  DT_OMP_SIMD(aligned(buf, other_image : 16))
  for(size_t k = 0; k < nfloats; k++)
    buf[k] -= other_image[k];
}

void dt_iop_image_invert(float *const buf,
                         const float max_value,
                         const size_t width,
                         const size_t height,
                         const size_t ch)
{
  const size_t nfloats = width * height * ch;
#ifdef _OPENMP
  if(nfloats > parallel_imgop_minimum)	// is the copy big enough to outweigh threading overhead?
  {
    // we can gain a little by using a small number of threads in
    // parallel, but not much since the memory bus quickly saturates
    // (basically, each core can saturate a memory channel, so a
    // system with quad-channel memory won't be able to take advantage
    // of more than four cores).
    const int nthreads = MIN(dt_get_num_threads(), parallel_imgop_maxthreads);
    DT_OMP_FOR_SIMD(num_threads(nthreads) aligned(buf:16))
    for(size_t k = 0; k < nfloats; k++)
      buf[k] = max_value - buf[k];
    return;
  }
#endif // _OPENMP
  // no OpenMP, or image too small to bother parallelizing
  DT_OMP_SIMD(aligned(buf:16))
  for(size_t k = 0; k < nfloats; k++)
    buf[k] = max_value - buf[k];
}

void dt_iop_image_mul_const(float *const buf,
                            const float mul_value,
                            const size_t width,
                            const size_t height,
                            const size_t ch)
{
  const size_t nfloats = width * height * ch;
#ifdef _OPENMP
  if(nfloats > parallel_imgop_minimum)	// is the copy big enough to outweigh threading overhead?
  {
    // we can gain a little by using a small number of threads in
    // parallel, but not much since the memory bus quickly saturates
    // (basically, each core can saturate a memory channel, so a
    // system with quad-channel memory won't be able to take advantage
    // of more than four cores).
    const int nthreads = MIN(dt_get_num_threads(), parallel_imgop_maxthreads);
    DT_OMP_FOR_SIMD(num_threads(nthreads) aligned(buf:16))
    for(size_t k = 0; k < nfloats; k++)
      buf[k] *= mul_value;
    return;
  }
#endif // _OPENMP
  // no OpenMP, or image too small to bother parallelizing
  DT_OMP_SIMD(aligned(buf:16))
  for(size_t k = 0; k < nfloats; k++)
    buf[k] *= mul_value;
}

void dt_iop_image_div_const(float *const buf,
                            const float div_value,
                            const size_t width,
                            const size_t height,
                            const size_t ch)
{
  const size_t nfloats = width * height * ch;
#ifdef _OPENMP
  if(nfloats > parallel_imgop_minimum)	// is the copy big enough to outweigh threading overhead?
  {
    // we can gain a little by using a small number of threads in
    // parallel, but not much since the memory bus quickly saturates
    // (basically, each core can saturate a memory channel, so a
    // system with quad-channel memory won't be able to take advantage
    // of more than four cores).
    const int nthreads = MIN(darktable.num_openmp_threads,parallel_imgop_maxthreads);
    DT_OMP_FOR_SIMD(num_threads(nthreads) aligned(buf:16))
    for(size_t k = 0; k < nfloats; k++)
      buf[k] /= div_value;
    return;
  }
#endif // _OPENMP
  // no OpenMP, or image too small to bother parallelizing
  DT_OMP_SIMD(aligned(buf:16))
  for(size_t k = 0; k < nfloats; k++)
    buf[k] /= div_value;
}

// elementwise: buf = lammda*buf + (1-lambda)*other
void dt_iop_image_linear_blend(float *const restrict buf,
                               const float lambda,
                               const float *const restrict other,
                               const size_t width,
                               const size_t height,
                               const size_t ch)
{
  const size_t nfloats = width * height * ch;
  const float lambda_1 = 1.0f - lambda;
#ifdef _OPENMP
  if(nfloats > parallel_imgop_minimum/2) // is the task big enough to outweigh threading overhead?
  {
    // we can gain a little by using a small number of threads in
    // parallel, but not much since the memory bus quickly saturates
    // (basically, each core can saturate a memory channel, so a
    // system with quad-channel memory won't be able to take advantage
    // of more than four cores).
    const int nthreads = MIN(dt_get_num_threads(), parallel_imgop_maxthreads);
    DT_OMP_FOR_SIMD(num_threads(nthreads) aligned(buf:16))
    for(size_t k = 0; k < nfloats; k++)
      buf[k] = lambda*buf[k] + lambda_1*other[k];
    return;
  }
#endif // _OPENMP
  // no OpenMP, or image too small to bother parallelizing
  DT_OMP_SIMD(aligned(buf:16))
  for(size_t k = 0; k < nfloats; k++)
    buf[k] = lambda*buf[k] + lambda_1*other[k];
}

// perform timings to determine the optimal threshold for switching to
// parallel operations, as well as the maximal number of threads
// before saturating the memory bus
void dt_iop_image_copy_benchmark()
{
  ///TODO
}

void dt_iop_image_copy_configure()
{
  int thresh = dt_conf_get_int("memcpy_parallel_threshold");
  if(thresh > 0)
    parallel_imgop_minimum = thresh;
  int threads = dt_conf_get_int("memcpy_parallel_maxthreads");
  if(threads > 0)
    parallel_imgop_maxthreads = threads;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
