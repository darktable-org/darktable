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

#include "common/eaw.h"
#include "common/darktable.h"
#include "common/math.h"
#include "control/control.h"     // needed by dwt.h
#include "common/dwt.h"          // for dwt_interleave_rows
#include <math.h>
#if defined(__SSE__)
#include <xmmintrin.h>
#endif

static inline void weight(const float *c1, const float *c2, const float sharpen, dt_aligned_pixel_t weight)
{
  dt_aligned_pixel_t square;
  for_each_channel(c) square[c] = c1[c] - c2[c];
  for_each_channel(c) square[c] = square[c] * square[c];

  const float wl = dt_fast_expf(-sharpen * square[0]);
  const float wc = dt_fast_expf(-sharpen * (square[1] + square[2]));

  weight[0] = wl;
  weight[1] = wc;
  weight[2] = wc;
  weight[3] = 1.0f;
}

#if defined(__SSE2__)
/* Computes the vector
 * (wl, wc, wc, 1)
 *
 * where:
 * wl = exp(-sharpen*SQR(c1[0] - c2[0]))
 *    = exp(-s*d1) (as noted in code comments below)
 * wc = exp(-sharpen*(SQR(c1[1] - c2[1]) + SQR(c1[2] - c2[2]))
 *    = exp(-s*(d2+d3)) (as noted in code comments below)
 */
static const __m128 o111 DT_ALIGNED_ARRAY = { ~0, ~0, ~0, 0 };
static inline __m128 weight_sse2(const __m128 *c1, const __m128 *c2, const float sharpen)
{
  const __m128 diff = *c1 - *c2;
  const __m128 square = diff * diff;                                // (?, d3, d2, d1)
  const __m128 square2 = _mm_shuffle_ps(square, square, _MM_SHUFFLE(3, 1, 2, 0)); // (?, d2, d3, d1)
  __m128 added = square + square2;                                  // (?, d2+d3, d2+d3, 2*d1)
  added = _mm_sub_ss(added, square);                                // (?, d2+d3, d2+d3, d1)
  __m128 sharpened = added * _mm_set1_ps(-sharpen);                 // (?, -s*(d2+d3), -s*(d2+d3), -s*d1)
  sharpened = _mm_and_ps(sharpened,o111);			    // (0, -s*(d2+d3), -s*(d2+d3), -s*d1)
  return dt_fast_expf_sse2(sharpened);                              // (1, wc, wc, wl)
}
#endif

#define SUM_PIXEL_CONTRIBUTION(ii, jj) 		                                                             \
  do                                                                                                         \
  {                                                                                                          \
    const float f = filter[(ii)] * filter[(jj)];                                                             \
    dt_aligned_pixel_t wp;                                                                                   \
    weight(px, px2, sharpen, wp);                                                                            \
    dt_aligned_pixel_t w;                                                                                    \
    dt_aligned_pixel_t pd;                                                                                   \
    for_four_channels(c,aligned(px2))                                                                        \
    {                                                                                                        \
      w[c] = f * wp[c];                                                                                      \
      wgt[c] += w[c];                                                                                        \
      pd[c] = w[c] * px2[c];                                                                                 \
      sum[c] += pd[c];                                                                                       \
    }                                                                                                        \
  } while(0)

#if defined(__SSE__)
#define SUM_PIXEL_CONTRIBUTION_SSE(ii, jj)	                                                             \
  do                                                                                                         \
  {                                                                                                          \
    const float f = filter[(ii)] * filter[(jj)];	                                                     \
    const __m128 wp = weight_sse2(px, px2, sharpen);                                                         \
    const __m128 w = f * wp;                                                                                 \
    const __m128 pd = *px2 * w;                                                                              \
    sum = sum + pd;                                                                                          \
    wgt = wgt + w;                                                                                           \
  } while(0)
#endif

#define SUM_PIXEL_PROLOGUE                                                                                   \
  dt_aligned_pixel_t sum = { 0.0f, 0.0f, 0.0f, 0.0f };                                                       \
  dt_aligned_pixel_t wgt = { 0.0f, 0.0f, 0.0f, 0.0f };

#if defined(__SSE2__)
#define SUM_PIXEL_PROLOGUE_SSE                                                                               \
  __m128 sum = _mm_setzero_ps();                                                                             \
  __m128 wgt = _mm_setzero_ps();
#endif

#define SUM_PIXEL_EPILOGUE                                                                                   \
  for_each_channel(c)      										     \
  {													     \
    sum[c] /= wgt[c];                                                   				     \
    pcoarse[c] = sum[c];                                                                                     \
    const float det = (px[c] - sum[c]);									     \
    pdetail[c] = det;    		                                              			     \
  }                                                                       				     \
  px += 4;                                                                                                   \
  pdetail += 4;                                                                                              \
  pcoarse += 4;

#if defined(__SSE2__)
#define SUM_PIXEL_EPILOGUE_SSE                                                                               \
  sum = sum / wgt;                                                                                           \
                                                                                                             \
  _mm_stream_ps(pdetail, *px - sum);                                                                         \
  _mm_stream_ps(pcoarse, sum);                                                                               \
  px++;                                                                                                      \
  pdetail += 4;                                                                                              \
  pcoarse += 4;
#endif

void eaw_decompose(float *const restrict out, const float *const restrict in, float *const restrict detail,
                   const int scale, const float sharpen, const int32_t width, const int32_t height)
{
  const int mult = 1 << scale;
  static const float filter[5] = { 1.0f / 16.0f, 4.0f / 16.0f, 6.0f / 16.0f, 4.0f / 16.0f, 1.0f / 16.0f };
  const int boundary = 2 * mult;

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(detail, filter, height, in, sharpen, mult, boundary, out, width) \
  schedule(static)
#endif
  for(int rowid = 0; rowid < height; rowid++)
  {
    const size_t j = dwt_interleave_rows(rowid, height, mult);
    const float *px = ((float *)in) + (size_t)4 * j * width;
    const float *px2;
    float *pdetail = detail + (size_t)4 * j * width;
    float *pcoarse = out + (size_t)4 * j * width;

    // for the first and last 'boundary' rows, we have to perform boundary tests for the entire row;
    //   for the central bulk, we only need to use those slower versions on the leftmost and rightmost pixels
    const int lbound = (j < boundary || j >= height - boundary) ? width-boundary : boundary;

    /* The first "2*mult" pixels need a boundary check because we might try to access past the left edge,
     * which requires nearest pixel interpolation */
    int i;
    for(i = 0; i < lbound; i++)
    {
      SUM_PIXEL_PROLOGUE;
      for(int jj = 0; jj < 5; jj++)
      {
        const int y = j + mult * (jj-2);
        const int clamp_y = CLAMP(y,0,height-1);
        for(int ii = 0; ii < 5; ii++)
        {
          int x = i + mult * ((ii)-2);
          if(x < 0) x = 0;			// we might be looking past the left edge
          px2 = ((float *)in) + 4 * x + (size_t)4 * clamp_y * width;
          SUM_PIXEL_CONTRIBUTION(ii, jj);
        }
      }
      SUM_PIXEL_EPILOGUE;
    }

    /* For pixels [2*mult, width-2*mult], we don't need to do any boundary checks */
    for( ; i < width - boundary; i++)
    {
      SUM_PIXEL_PROLOGUE;
      px2 = ((float *)in) + (size_t)4 * (i - 2 * mult + (size_t)(j - 2 * mult) * width);
      for(int jj = 0; jj < 5; jj++)
      {
        for(int ii = 0; ii < 5; ii++)
        {
          SUM_PIXEL_CONTRIBUTION(ii, jj);
          px2 += (size_t)4 * mult;
        }
        px2 += (size_t)4 * (width - 5) * mult;
      }
      SUM_PIXEL_EPILOGUE;
    }

    /* Last 2*mult pixels in the row require the boundary check again */
    for( ; i < width; i++)
    {
      SUM_PIXEL_PROLOGUE;
      for(int jj = 0; jj < 5; jj++)
      {
        const int y = j + mult * (jj-2);
        const int clamp_y = CLAMP(y,0,height-1);
        for(int ii = 0; ii < 5; ii++)
        {
          int x = i + mult * ((ii)-2);
          if(x >= width) x = width - 1;		// we might be looking beyond the right edge
          px2 = ((float *)in) + 4 * x + (size_t)4 * clamp_y * width;
          SUM_PIXEL_CONTRIBUTION(ii, jj);
        }
      }
      SUM_PIXEL_EPILOGUE;
    }
  }
}

#if defined(__SSE2__)
void eaw_decompose_sse2(float *const restrict out, const float *const restrict in, float *const restrict detail,
                        const int scale, const float sharpen, const int32_t width, const int32_t height)
{
  const int mult = 1 << scale;
  static const float filter[5] = { 1.0f / 16.0f, 4.0f / 16.0f, 6.0f / 16.0f, 4.0f / 16.0f, 1.0f / 16.0f };
  const int boundary = 2 * mult;

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(detail, filter, height, in, sharpen, mult, boundary, out, width) \
  schedule(static)
#endif
  for(int rowid = 0; rowid < height; rowid++)
  {
    const size_t j = dwt_interleave_rows(rowid, height, mult);
    const __m128 *px = ((__m128 *)in) + (size_t)j * width;
    const __m128 *px2;
    float *pdetail = detail + (size_t)4 * j * width;
    float *pcoarse = out + (size_t)4 * j * width;

    // for the first and last 'boundary' rows, we have to use the macros with tests for the entire row;
    //   for the central bulk, we only need to use those slower versions on the leftmost and rightmost pixels
    const int lbound = (j < boundary || j >= height - boundary) ? width-boundary : boundary;

    /* The first "2*mult" pixels need a boundary check because we might try to access past the left edge,
     * which requires nearest pixel interpolation */
    int i;
    for(i = 0; i < lbound; i++)
    {
      SUM_PIXEL_PROLOGUE_SSE;
      for(int jj = 0; jj < 5; jj++)
      {
        const int y = j + mult * (jj-2);
        const int clamp_y = CLAMP(y,0,height-1);
        for(int ii = 0; ii < 5; ii++)
        {
          int x = i + mult * ((ii)-2);
          if(x < 0) x = 0;			// we might be looking beyond the left edge
          px2 = ((__m128 *)in) + x + (size_t)clamp_y * width;
          SUM_PIXEL_CONTRIBUTION_SSE(ii, jj);
        }
      }
      SUM_PIXEL_EPILOGUE_SSE;
    }

    /* For pixels [2*mult, width-2*mult], we don't need to do any boundary checks */
    for( ; i < width - boundary; i++)
    {
      SUM_PIXEL_PROLOGUE_SSE;
      px2 = ((__m128 *)in) + i - 2 * mult + (size_t)(j - 2 * mult) * width;
      for(int jj = 0; jj < 5; jj++)
      {
        for(int ii = 0; ii < 5; ii++)
        {
          SUM_PIXEL_CONTRIBUTION_SSE(ii, jj);
          px2 += mult;
        }
        px2 += (width - 5) * mult;
      }
      SUM_PIXEL_EPILOGUE_SSE;
    }

    /* Last 2*mult pixels in the row require the boundary check again */
    for( ; i < width; i++)
    {
      SUM_PIXEL_PROLOGUE_SSE;
      for(int jj = 0; jj < 5; jj++)
      {
        const int y = j + mult * (jj-2);
        const int clamp_y = CLAMP(y,0,height-1);
        for(int ii = 0; ii < 5; ii++)
        {
          int x = i + mult * ((ii)-2);
          if(x >= width) x = width-1;		// we might be looking beyond the right edge
          px2 = ((__m128 *)in) + x + (size_t)clamp_y * width;
          SUM_PIXEL_CONTRIBUTION_SSE(ii, jj);
        }
      }
      SUM_PIXEL_EPILOGUE_SSE;
    }
  }
  _mm_sfence();
}
#endif

void eaw_synthesize(float *const out, const float *const in, const float *const restrict detail,
                    const float *const restrict threshold, const float *const restrict boost,
                    const int32_t width, const int32_t height)
{
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(height, width) \
  dt_omp_sharedconst(in, out, detail, threshold, boost) \
  schedule(simd:static)
#endif
  for(size_t k = 0; k < (size_t)width * height; k++)
  {
#ifdef _OPENMP
#pragma omp simd simdlen(4) aligned(detail, in, out, threshold, boost)
#endif
    for(size_t c = 0; c < 4; c++)
    {
      // decrease the absolute magnitude of the detail by the threshold; copysignf does not vectorize, but it
      // turns out that just adding up two clamped alternatives gives exactly the same result and DOES vectorize
      //const float absamt = fmaxf(0.0f, (fabsf(detail[k + c]) - threshold[c]));
      //const float amount = copysignf(absamt, detail[k + c]);
      const float amount = MAX(detail[4*k+c] - threshold[c], 0.0f) + MIN(detail[4*k+c] + threshold[c], 0.0f);
      out[4*k + c] = in[4*k + c] + (boost[c] * amount);
    }
  }
}

#if defined(__SSE2__)
void eaw_synthesize_sse2(float *const out, const float *const in, const float *const restrict detail,
                         const float *const restrict thrsf, const float *const restrict boostf,
                         const int32_t width, const int32_t height)
{
  const __m128 threshold = _mm_load_ps(thrsf);
  const __m128 boost = _mm_load_ps(boostf);
  const __m128i maski = _mm_set1_epi32(0x80000000u);
  const __m128 *mask = (__m128 *)&maski;

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(boost, detail, height, in, out, threshold, width, maski, mask) \
  schedule(static)
#endif
  for(size_t j = 0; j < (size_t)width * height; j++)
  {
    const __m128 *pin = (__m128 *)in + j;
    const __m128 *pdetail = (__m128 *)detail + j;
    const __m128 absamt = _mm_max_ps(_mm_setzero_ps(), _mm_andnot_ps(*mask, *pdetail) - threshold);
    const __m128 amount = _mm_or_ps(_mm_and_ps(*pdetail, *mask), absamt);
    _mm_stream_ps(out + 4*j, *pin + boost * amount);
  }
  _mm_sfence();
}
#endif

// =====================================================================================
// begin wavelet code from denoiseprofile.c
// =====================================================================================

static inline float dn_weight(const float *c1, const float *c2, const float inv_sigma2)
{
  // 3d distance based on color
  dt_aligned_pixel_t sqr;
  for_each_channel(c)
  {
    const float diff = c1[c] - c2[c];
    sqr[c] = diff * diff;
  }
  const float dot = (sqr[0] + sqr[1] + sqr[2]) * inv_sigma2;
  const float var
      = 0.02f; // FIXME: this should ideally depend on the image before noise stabilizing transforms!
  const float off2 = 9.0f; // (3 sigma)^2
  return fast_mexp2f(MAX(0, dot * var - off2));
}

typedef struct _aligned_pixel {
  union {
    dt_aligned_pixel_t v;
#ifdef __SSE2__
    __m128 sse;
#endif
  };
} _aligned_pixel;
#ifdef _OPENMP
static inline _aligned_pixel add_float4(_aligned_pixel acc, _aligned_pixel newval)
{
  for_four_channels(c) acc.v[c] += newval.v[c];
  return acc;
}
#pragma omp declare reduction(vsum:_aligned_pixel:omp_out=add_float4(omp_out,omp_in)) \
  initializer(omp_priv = { .v = { 0.0f, 0.0f, 0.0f, 0.0f } })
#endif

#undef SUM_PIXEL_CONTRIBUTION
#define SUM_PIXEL_CONTRIBUTION(ii, jj) 		                                                             \
  do                                                                                                         \
  {                                                                                                          \
    const float f = filter[(ii)] * filter[(jj)];                                                             \
    const float wp = dn_weight(px, px2, inv_sigma2);                                                         \
    const float w = f * wp;                                                                                  \
    dt_aligned_pixel_t pd;                                                                                   \
    for_each_channel(c,aligned(px2))                                                                         \
    {                                                                                                        \
      pd[c] = w * px2[c];                                                                                    \
      wgt[c] += w;                                                                                           \
      sum[c] += pd[c];                                                                                       \
    }                                                                                                        \
  } while(0)

#undef SUM_PIXEL_EPILOGUE
#define SUM_PIXEL_EPILOGUE                                                                                   \
  for_each_channel(c)      										     \
  {													     \
    sum[c] /= wgt[c];                                                   				     \
    pcoarse[c] = sum[c];                                                                                     \
    const float det = (px[c] - sum[c]);									     \
    pdetail[c] = det;    		                                              			     \
    sum_sq.v[c] += (det*det);					                                             \
  }                                                                       				     \
  px += 4;                                                                                                   \
  pdetail += 4;                                                                                              \
  pcoarse += 4;

void eaw_dn_decompose(float *const restrict out, const float *const restrict in, float *const restrict detail,
                      dt_aligned_pixel_t sum_squared, const int scale, const float inv_sigma2,
                      const int32_t width, const int32_t height)
{
  const int mult = 1u << scale;
  static const float filter[5] = { 1.0f / 16.0f, 4.0f / 16.0f, 6.0f / 16.0f, 4.0f / 16.0f, 1.0f / 16.0f };
  const int boundary = 2 * mult;

  _aligned_pixel sum_sq = { .v = { 0.0f } };

#if !(defined(__apple_build_version__) && __apple_build_version__ < 11030000) //makes Xcode 11.3.1 compiler crash
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(detail, filter, height, in, inv_sigma2, mult, boundary, out, width) \
  reduction(vsum: sum_sq) \
  schedule(static)
#endif
#endif
  for(int rowid = 0; rowid < height; rowid++)
  {
    const size_t j = dwt_interleave_rows(rowid, height, mult);
    const float *px = ((float *)in) + (size_t)4 * j * width;
    const float *px2;
    float *pdetail = detail + (size_t)4 * j * width;
    float *pcoarse = out + (size_t)4 * j * width;

    // for the first and last 'boundary' rows, we have to perform boundary tests for the entire row;
    //   for the central bulk, we only need to use those slower versions on the leftmost and rightmost pixels
    const int lbound = (j < boundary || j >= height - boundary) ? width-boundary : boundary;

    /* The first "2*mult" pixels need a boundary check because we might try to access past the left edge,
     * which requires nearest pixel interpolation */
    int i;
    for(i = 0; i < lbound; i++)
    {
      SUM_PIXEL_PROLOGUE;
      for(int jj = 0; jj < 5; jj++)
      {
        const int y = j + mult * (jj-2);
        const int clamp_y = CLAMP(y,0,height-1);
        for(int ii = 0; ii < 5; ii++)
        {
          int x = i + mult * ((ii)-2);
          if(x < 0) x = 0;			// we might be looking past the left edge
          px2 = ((float *)in) + 4 * x + (size_t)4 * clamp_y * width;
          SUM_PIXEL_CONTRIBUTION(ii, jj);
        }
      }
      SUM_PIXEL_EPILOGUE;
    }

    /* For pixels [2*mult, width-2*mult], we don't need to do any boundary checks */
    for( ; i < width - boundary; i++)
    {
      SUM_PIXEL_PROLOGUE;
      px2 = ((float *)in) + (size_t)4 * (i - 2 * mult + (size_t)(j - 2 * mult) * width);
      for(int jj = 0; jj < 5; jj++)
      {
        for(int ii = 0; ii < 5; ii++)
        {
          SUM_PIXEL_CONTRIBUTION(ii, jj);
          px2 += (size_t)4 * mult;
        }
        px2 += (size_t)4 * (width - 5) * mult;
      }
      SUM_PIXEL_EPILOGUE;
    }

    /* Last 2*mult pixels in the row require the boundary check again */
    for( ; i < width; i++)
    {
      SUM_PIXEL_PROLOGUE;
      for(int jj = 0; jj < 5; jj++)
      {
        const int y = j + mult * (jj-2);
        const int clamp_y = CLAMP(y,0,height-1);
        for(int ii = 0; ii < 5; ii++)
        {
          int x = i + mult * ((ii)-2);
          if(x >= width) x = width - 1;		// we might be looking past the right edge
          px2 = ((float *)in) + 4 * x + (size_t)4 * clamp_y * width;
          SUM_PIXEL_CONTRIBUTION(ii, jj);
        }
      }
      SUM_PIXEL_EPILOGUE;
    }
  }
  for_each_channel(c)
    sum_squared[c] = sum_sq.v[c];
}

#undef SUM_PIXEL_CONTRIBUTION
#undef SUM_PIXEL_PROLOGUE
#undef SUM_PIXEL_EPILOGUE


#undef SUM_PIXEL_CONTRIBUTION_SSE
#undef SUM_PIXEL_PROLOGUE_SSE
#undef SUM_PIXEL_EPILOGUE_SSE

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

