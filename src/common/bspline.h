#pragma once

#include "common/darktable.h"
#include "common/dwt.h"

// B spline filter
#define BSPLINE_FSIZE 5


#ifdef _OPENMP
#pragma omp declare simd aligned(buf, indices, result:64)
#endif
static inline void sparse_scalar_product(const dt_aligned_pixel_t buf, const size_t indices[BSPLINE_FSIZE],
                                         dt_aligned_pixel_t result)
{
  // scalar product of 2 3×5 vectors stored as RGB planes and B-spline filter,
  // e.g. RRRRR - GGGGG - BBBBB
  for_each_channel(c, aligned(buf,indices,result))
  {
    result[c] = MAX(0.0f, ((buf[indices[0]+c] + 4.0f*(buf[indices[1]+c] + buf[indices[3]+c])
                           + 6.0f*buf[indices[2]+c] + buf[indices[4]+c])
                           /16.0f));
  }
}

#ifdef _OPENMP
#pragma omp declare simd aligned(in, temp)
#endif
static inline void _bspline_vertical_pass(const float *const restrict in, float *const restrict temp,
                                          size_t row, size_t width, size_t height, int mult)
{
  size_t DT_ALIGNED_ARRAY indices[BSPLINE_FSIZE];
  // compute the index offsets of the pixels of interest; since the offsets are the same for the entire row,
  // we only need to do this once and can then process the entire row
  indices[0] = 4 * width * MAX((int)row - 2 * mult, 0);
  indices[1] = 4 * width * MAX((int)row - mult, 0);
  indices[2] = 4 * width * row;
  indices[3] = 4 * width * MIN(row + mult, height-1);
  indices[4] = 4 * width * MIN(row + 2 * mult, height-1);
  for(size_t j = 0; j < width; j++)
  {
    // Compute the vertical blur of the current pixel and store it in the temp buffer for the row
    sparse_scalar_product(in + j * 4, indices, temp + j * 4);
  }
}

#ifdef _OPENMP
#pragma omp declare simd aligned(temp, out)
#endif
static inline void _bspline_horizontal(const float *const restrict temp, float *const restrict out,
                                       size_t col, size_t width, int mult)
{
  // Compute the array indices of the pixels of interest; since the offsets will change near the ends of
  // the row, we need to recompute for each pixel
  size_t DT_ALIGNED_ARRAY indices[BSPLINE_FSIZE];
  indices[0] = 4 * MAX((int)col - 2 * mult, 0);
  indices[1] = 4 * MAX((int)col - mult,  0);
  indices[2] = 4 * col;
  indices[3] = 4 * MIN(col + mult, width-1);
  indices[4] = 4 * MIN(col + 2 * mult, width-1);
  // Compute the horizontal blur of the already vertically-blurred pixel and store the result at the proper
  //  row/column location in the output buffer
  sparse_scalar_product(temp, indices, out);
}

#ifdef _OPENMP
#pragma omp declare simd aligned(in, out:64) aligned(tempbuf:16)
#endif
inline static void blur_2D_Bspline(const float *const restrict in, float *const restrict out,
                                   float *const restrict tempbuf,
                                   const size_t width, const size_t height, const int mult)
{
  // À-trous B-spline interpolation/blur shifted by mult
  #ifdef _OPENMP
  #pragma omp parallel for default(none) \
    dt_omp_firstprivate(width, height, mult)  \
    dt_omp_sharedconst(out, in, tempbuf) \
    schedule(static)
  #endif
  for(size_t row = 0; row < height; row++)
  {
    // get a thread-private one-row temporary buffer
    float *const temp = tempbuf + 4 * width * dt_get_thread_num();
    // interleave the order in which we process the rows so that we minimize cache misses
    const size_t i = dwt_interleave_rows(row, height, mult);
    // Convolve B-spline filter over columns: for each pixel in the current row, compute vertical blur
    _bspline_vertical_pass(in, temp, i, width, height, mult);
    // Convolve B-spline filter horizontally over current row
    for(size_t j = 0; j < width; j++)
    {
      _bspline_horizontal(temp, out + (i * width + j) * 4, j, width, mult);
    }
  }
}


inline static void decompose_2D_Bspline(const float *const DT_ALIGNED_PIXEL restrict in,
                                        float *const DT_ALIGNED_PIXEL restrict HF,
                                        float *const DT_ALIGNED_PIXEL restrict LF,
                                        const size_t width, const size_t height, const int mult,
                                        float *const tempbuf, size_t padded_size)
{
  // Blur and compute the decimated wavelet at once
#ifdef _OPENMP
#pragma omp parallel for default(none) \
    dt_omp_firstprivate(width, height, mult, padded_size) \
    dt_omp_sharedconst(in, HF, LF, tempbuf)  \
    schedule(static)
#endif
  for(size_t row = 0; row < height; row++)
  {
    // get a thread-private one-row temporary buffer
    float *restrict DT_ALIGNED_ARRAY const temp = dt_get_perthread(tempbuf, padded_size);
    // interleave the order in which we process the rows so that we minimize cache misses
    const size_t i = dwt_interleave_rows(row, height, mult);
    // Convolve B-spline filter over columns: for each pixel in the current row, compute vertical blur
    _bspline_vertical_pass(in, temp, i, width, height, mult);
    // Convolve B-spline filter horizontally over current row
    for(size_t j = 0; j < width; j++)
    {
      size_t index = 4U * (i * width + j);
      _bspline_horizontal(temp, LF + index, j, width, mult);
      // compute the HF component by subtracting the LF from the original input
      for_each_channel(c)
        HF[index + c] = in[index + c] - LF[index + c];
    }
  }
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
