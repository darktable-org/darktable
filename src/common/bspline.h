#pragma once

#include "common/darktable.h"
#include "common/dwt.h"
#include "develop/openmp_maths.h"

// uncomment the following to use nontemporal writes in the decomposition
// on a 32-core Threadripper, nt writes are 8% slower wiith one thread,
// break even at 8 threads, and are 8% faster with 32 and 64 threads
//#define USE_NONTEMPORAL

// B spline filter
#define BSPLINE_FSIZE 5

// The B spline best approximate a Gaussian of standard deviation :
// see https://eng.aurelienpierre.com/2021/03/rotation-invariant-laplacian-for-2d-grids/
#define B_SPLINE_SIGMA 1.0553651328015339f

// Normalization scaling of the wavelet to approximate a laplacian
// from the function above for sigma = B_SPLINE_SIGMA as a constant
#define B_SPLINE_TO_LAPLACIAN 3.182727439285017f

static inline float equivalent_sigma_at_step(const float sigma, const unsigned int s)
{
  // If we stack several gaussian blurs of standard deviation sigma on top of each other,
  // this is the equivalent standard deviation we get at the end (after s steps)
  // First step is s = 0
  // see
  // https://eng.aurelienpierre.com/2021/03/rotation-invariant-laplacian-for-2d-grids/#Multi-scale-iterative-scheme
  if(s == 0)
    return sigma;
  else
    return sqrtf(sqf(equivalent_sigma_at_step(sigma, s - 1)) + sqf(exp2f((float)s) * sigma));
}

static inline unsigned int num_steps_to_reach_equivalent_sigma(const float sigma_filter, const float sigma_final)
{
  // The inverse of the above : compute the number of scales needed to reach the desired equivalent sigma_final
  // after sequential blurs of constant sigma_filter
  unsigned int s = 0;
  float radius = sigma_filter;
  while(radius < sigma_final)
  {
    ++s;
    radius = sqrtf(sqf(radius) + sqf((float)(1 << s) * sigma_filter));
  }
  return s + 1;
}


#ifdef _OPENMP
#pragma omp declare simd aligned(buf, indices, result:64)
#endif
static inline void sparse_scalar_product(const dt_aligned_pixel_t buf, const size_t indices[BSPLINE_FSIZE],
                                         dt_aligned_pixel_t result, const gboolean clip_negatives)
{
  // scalar product of 2 3×5 vectors stored as RGB planes and B-spline filter,
  // e.g. RRRRR - GGGGG - BBBBB
  static const float filter[BSPLINE_FSIZE] = { 1.0f / 16.0f,
                                               4.0f / 16.0f,
                                               6.0f / 16.0f,
                                               4.0f / 16.0f,
                                               1.0f / 16.0f };

  if(clip_negatives)
  {
    for_each_channel(c, aligned(buf,indices,result))
    {
      result[c] = MAX(0.0f, filter[0] * buf[indices[0] + c] +
                            filter[1] * buf[indices[1] + c] +
                            filter[2] * buf[indices[2] + c] +
                            filter[3] * buf[indices[3] + c] +
                            filter[4] * buf[indices[4] + c]);
    }
  }
  else
  {
    for_each_channel(c, aligned(buf,indices,result))
    {
      result[c] = filter[0] * buf[indices[0] + c] +
                  filter[1] * buf[indices[1] + c] +
                  filter[2] * buf[indices[2] + c] +
                  filter[3] * buf[indices[3] + c] +
                  filter[4] * buf[indices[4] + c];
    }
  }
}

#ifdef _OPENMP
#pragma omp declare simd aligned(in, temp)
#endif
static inline void _bspline_vertical_pass(const float *const restrict in, float *const restrict temp,
                                          size_t row, size_t width, size_t height, int mult, const gboolean clip_negatives)
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
    sparse_scalar_product(in + j * 4, indices, temp + j * 4, clip_negatives);
  }
}

#ifdef _OPENMP
#pragma omp declare simd aligned(temp, out)
#endif
static inline void _bspline_horizontal(const float *const restrict temp, float *const restrict out,
                                       size_t col, size_t width, int mult, const gboolean clip_negatives)
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
  sparse_scalar_product(temp, indices, out, clip_negatives);
}

#ifdef _OPENMP
#pragma omp declare simd aligned(in, out:64) aligned(tempbuf:16)
#endif
static inline void blur_2D_Bspline(const float *const restrict in,
                                   float *const restrict out,
                                   float *const restrict tempbuf,
                                   const size_t width,
                                   const size_t height,
                                   const int mult,
                                   const gboolean clip_negatives)
{
  // À-trous B-spline interpolation/blur shifted by mult
  #ifdef _OPENMP
  #pragma omp parallel for default(none) \
    dt_omp_firstprivate(in, out, tempbuf, width, height, mult, clip_negatives) \
    schedule(static)
  #endif
  for(size_t row = 0; row < height; row++)
  {
    // get a thread-private one-row temporary buffer
    float *const temp = tempbuf + 4 * width * dt_get_thread_num();
    // interleave the order in which we process the rows so that we minimize cache misses
    const size_t i = dwt_interleave_rows(row, height, mult);
    // Convolve B-spline filter over columns: for each pixel in the current row, compute vertical blur
    _bspline_vertical_pass(in, temp, i, width, height, mult, clip_negatives);
    // Convolve B-spline filter horizontally over current row
    for(size_t j = 0; j < width; j++)
    {
#if USE_NONTEMPORAL
      dt_aligned_pixel_t blur;
      _bspline_horizontal(temp, blur, j, width, mult, clip_negatives);
      copy_pixel_nontemporal(out + (i * width + j) * 4, blur);
#else
      _bspline_horizontal(temp, out + (i * width + j) * 4, j, width, mult, clip_negatives);
#endif
    }
  }
  dt_omploop_sfence();  // ensure that nontemporal writes complete before we attempt to read the output
}


#ifdef _OPENMP
#pragma omp declare simd aligned(in, HF, LF:64) aligned(tempbuf:16)
#endif
inline static void decompose_2D_Bspline(const float *const in,
                                        float *const HF,
                                        float *const restrict LF,
                                        const size_t width, const size_t height, const int mult,
                                        float *const tempbuf, size_t padded_size)
{
  // Blur and compute the decimated wavelet at once
#ifdef _OPENMP
#pragma omp parallel for default(none) \
    dt_omp_firstprivate(width, height, mult, padded_size, in, HF, LF, tempbuf) \
    schedule(static)
#endif
  for(size_t row = 0; row < height; row++)
  {
    // get a thread-private one-row temporary buffer
    float *restrict DT_ALIGNED_ARRAY const temp = dt_get_perthread(tempbuf, padded_size);
    // interleave the order in which we process the rows so that we minimize cache misses
    const size_t i = dwt_interleave_rows(row, height, mult);
    // Convolve B-spline filter over columns: for each pixel in the current row, compute vertical blur
    _bspline_vertical_pass(in, temp, i, width, height, mult, TRUE); // always clip negatives
    // Convolve B-spline filter horizontally over current row
    for(size_t j = 0; j < width; j++)
    {
      const size_t index = 4U * (i * width + j);
#if USE_NONTEMPORAL
      dt_aligned_pixel_t blur;
      _bspline_horizontal(temp, blur, j, width, mult, TRUE); // always clip negatives
      copy_pixel_nontemporal(LF + index, blur);
      // compute the HF component by subtracting the LF from the original input
      for_four_channels(c)
        HF[index + c] = in[index + c] - blur[c];
#else
      _bspline_horizontal(temp, LF + index, j, width, mult, TRUE); // always clip negatives
      // compute the HF component by subtracting the LF from the original input
      for_four_channels(c)
        HF[index + c] = in[index + c] - LF[index + c];
#endif
    }
  }
  dt_omploop_sfence();  // ensure that nontemporal writes complete before we attempt to read the output
}

#undef USE_NONTEMPORAL

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
