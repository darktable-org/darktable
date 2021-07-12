#pragma once

#include "common/darktable.h"
#include "common/dwt.h"

// B spline filter
#define BSPLINE_FSIZE 5


#ifdef _OPENMP
#pragma omp declare simd aligned(buf, indices, result:64)
#endif
inline static void sparse_scalar_product(const float *const buf, const size_t indices[BSPLINE_FSIZE], float result[4])
{
  // scalar product of 2 3×5 vectors stored as RGB planes and B-spline filter,
  // e.g. RRRRR - GGGGG - BBBBB

  const float DT_ALIGNED_ARRAY filter[BSPLINE_FSIZE] =
                        { 1.0f / 16.0f, 4.0f / 16.0f, 6.0f / 16.0f, 4.0f / 16.0f, 1.0f / 16.0f };

  #ifdef _OPENMP
  #pragma omp simd
  #endif
  for(size_t c = 0; c < 4; ++c)
  {
    float acc = 0.0f;
    for(size_t k = 0; k < BSPLINE_FSIZE; ++k)
      acc += filter[k] * buf[indices[k] + c];
    result[c] = fmaxf(acc, 0.f);
  }
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
    schedule(simd:static)
  #endif
  for(size_t row = 0; row < height; row++)
  {
    // get a thread-private one-row temporary buffer
    float *const temp = tempbuf + 4 * width * dt_get_thread_num();
    // interleave the order in which we process the rows so that we minimize cache misses
    const size_t i = dwt_interleave_rows(row, height, mult);
    // Convolve B-spline filter over columns: for each pixel in the current row, compute vertical blur
    size_t DT_ALIGNED_ARRAY indices[BSPLINE_FSIZE] = { 0 };
    // Start by computing the array indices of the pixels of interest; the offsets from the current pixel stay
    // unchanged over the entire row, so we can compute once and just offset the base address while iterating
    // over the row
    for(size_t ii = 0; ii < BSPLINE_FSIZE; ++ii)
    {
      const size_t r = CLAMP(mult * (int)(ii - (BSPLINE_FSIZE - 1) / 2) + (int)i, (int)0, (int)height - 1);
      indices[ii] = 4 * r * width;
    }
    for(size_t j = 0; j < width; j++)
    {
      // Compute the vertical blur of the current pixel and store it in the temp buffer for the row
      sparse_scalar_product(in + j * 4, indices, temp + j * 4);
    }
    // Convolve B-spline filter horizontally over current row
    for(size_t j = 0; j < width; j++)
    {
      // Compute the array indices of the pixels of interest; since the offsets will change near the ends of
      // the row, we need to recompute for each pixel
      for(size_t jj = 0; jj < BSPLINE_FSIZE; ++jj)
      {
        const size_t col = CLAMP(mult * (int)(jj - (BSPLINE_FSIZE - 1) / 2) + (int)j, (int)0, (int)width - 1);
        indices[jj] = 4 * col;
      }
      // Compute the horizontal blur of the already vertically-blurred pixel and store the result at the proper
      //  row/column location in the output buffer
      sparse_scalar_product(temp, indices, out + (i * width + j) * 4);
    }
  }
}


inline static void decompose_2D_Bspline(const float *const restrict in, float *const restrict HF,
                                        float *const restrict LF,
                                        const size_t width, const size_t height, const int mult)
{
  size_t padded_size;
  float *tempbuf = dt_alloc_perthread_float(4 * width, &padded_size); //TODO: alloc in caller
  // Blur and compute the decimated wavelet at once
#ifdef _OPENMP
#pragma omp parallel for default(none) \
    dt_omp_firstprivate(width, height, in, LF, HF, mult, tempbuf, padded_size) \
    schedule(simd: static)
#endif
  for(size_t row = 0; row < height; row++)
  {
    // get a thread-private one-row temporary buffer
    float *restrict const temp = dt_get_perthread(tempbuf, padded_size);
    // interleave the order in which we process the rows so that we minimize cache misses
    const size_t i = dwt_interleave_rows(row, height, mult);
    // Convolve B-spline filter over columns: for each pixel in the current row, compute vertical blur
    size_t DT_ALIGNED_ARRAY indices[BSPLINE_FSIZE] = { 0 };
    // Start by computing the array indices of the pixels of interest; the offsets from the current pixel stay
    // unchanged over the entire row, so we can compute once and just offset the base address while iterating
    // over the row
    for(size_t ii = 0; ii < BSPLINE_FSIZE; ++ii)
    {
      const size_t r = CLAMP(mult * (int)(ii - (BSPLINE_FSIZE - 1) / 2) + (int)i, (int)0, (int)height - 1);
      indices[ii] = 4 * r * width;
    }
    for(size_t j = 0; j < width; j++)
    {
      // Compute the vertical blur of the current pixel and store it in the temp buffer for the row
      sparse_scalar_product(in + j * 4, indices, temp + j * 4);
    }
    // Convolve B-spline filter horizontally over current row
    for(size_t j = 0; j < width; j++)
    {
      // Compute the array indices of the pixels of interest; since the offsets will change near the ends of
      // the row, we need to recompute for each pixel
      for(size_t jj = 0; jj < BSPLINE_FSIZE; ++jj)
      {
        const size_t col = CLAMP(mult * (int)(jj - (BSPLINE_FSIZE - 1) / 2) + (int)j, (int)0, (int)width - 1);
        indices[jj] = 4 * col;
      }
      size_t index = 4U * (i * width + j);
      // Compute the horizontal blur of the already vertically-blurred pixel and store the result at the proper
      //  row/column location in the LF output buffer
      sparse_scalar_product(temp, indices, LF + index);
      // compute the HF component by subtracting the LF from the original input
      for_each_channel(c)
        HF[index + c] = in[index + c] - LF[index + c];
    }
  }
  dt_free_align(tempbuf);
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
