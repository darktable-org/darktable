/*
    This file is part of darktable,
    Copyright (C) 2017-2023 darktable developers.

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
#include "common/math.h"
#include "control/control.h"     // needed by dwt.h
#include "common/dwt.h"          // for dwt_interleave_rows

static inline void weight(const dt_aligned_pixel_t c1,
                              const dt_aligned_pixel_t c2,
                              const dt_aligned_pixel_t sharpen,
                              dt_aligned_pixel_t weight)
{
/* Computes the vector
 * (wl, wc, wc, 1)
 *
 * where:
 * wl = exp(-sharpen*SQR(c1[0] - c2[0]))
 * wc = exp(-sharpen*(SQR(c1[1] - c2[1]) + SQR(c1[2] - c2[2]))
 */
  dt_aligned_pixel_t square;
  for_each_channel(c) square[c] = c1[c] - c2[c];
  for_each_channel(c) square[c] = square[c] * square[c];	// { d1, d2, d3, ? }
  const dt_aligned_pixel_t square2 = { square[0], square[2], square[1], square[3] }; // { d1, d3, d2, ? }
  dt_aligned_pixel_t added;
  for_each_channel(c)
    added[c] = square[c] + square2[c];				// { d1+d1, d2+d3, d2+d3, ? }
  dt_aligned_pixel_t sharpened;
  for_each_channel(c)
    sharpened[c] = sharpen[c] * added[c];			// { -s*d1,  -s*(d2+d3), -s*(d2+d3), 0 }
  dt_vector_exp(sharpened, weight);				// { wl, wc, wc, 1 }
}

static inline void accumulate(dt_aligned_pixel_t accum,
                              const dt_aligned_pixel_t detail,
                              const dt_aligned_pixel_t thresh,
                              const dt_aligned_pixel_t boostval)
{
  // decrease the absolute magnitude of the detail by the threshold,
  // then add the result to the accumulator; copysignf does not
  // vectorize, but it turns out that just adding up two clamped
  // alternatives gives exactly the same result and DOES vectorize
  //    const float absamt = fmaxf(0.0f, (fabsf(detail[k + c]) - threshold[c]));
  //    const float amount = copysignf(absamt, detail[k + c]);
  // the below code is the vectorization of
  //   amount = MAX(detail - thresh, 0.0f) + MIN(detail + thresh, 0.0f);
  static const dt_aligned_pixel_t zero = { 0.0f, 0.0f, 0.0f, 0.0f };
  dt_aligned_pixel_t sum, diff;
  for_four_channels(c, aligned(sum, diff, detail, thresh))
  {
    sum[c] = detail[c] + thresh[c];
    diff[c] = detail[c] - thresh[c];
  }
  dt_vector_min(sum, sum, zero);
  dt_vector_max(diff, diff, zero);
  dt_aligned_pixel_t amount;
  for_four_channels(c, aligned(amount, sum, diff))
    amount[c] = sum[c] + diff[c];

  for_four_channels(c,aligned(accum,detail,thresh,boostval))
  {
    accum[c] += (boostval[c] * amount[c]);
  }
}

#define SUM_PIXEL_CONTRIBUTION						\
  do                                                                    \
  {                                                                     \
    dt_aligned_pixel_t wp;                                              \
    weight(px + 4*i, px2, vsharpen, wp);                                \
    dt_aligned_pixel_t w;                                               \
    const float f = filter[filter_idx++];                               \
    for_four_channels(c,aligned(px2))                                   \
    {                                                                   \
      w[c] = f * wp[c];                                                 \
      wgt[c] += w[c];                                                   \
      sum[c] += w[c] * px2[c];                                          \
    }                                                                   \
  } while(0)

#define SUM_PIXEL_PROLOGUE                                                                                   \
  dt_aligned_pixel_t sum = { 0.0f, 0.0f, 0.0f, 0.0f };                                                       \
  dt_aligned_pixel_t wgt = { 0.0f, 0.0f, 0.0f, 0.0f };							     \
  size_t filter_idx = 0;

#define SUM_PIXEL_EPILOGUE                                                                                   \
  dt_aligned_pixel_t det;										     \
  for_each_channel(c)      										     \
  {													     \
    sum[c] /= wgt[c];                                                   				     \
    det[c] = (px[4*i+c] - sum[c]);								     	     \
  }                                                                       				     \
  copy_pixel_nontemporal(pcoarse + 4*i,sum);                                  				     \
  accumulate(pdetail + 4*i, det, threshold, boost);							     \

void eaw_decompose_and_synthesize(float *const restrict out,
                                  const float *const restrict in,
                                  float *const restrict accum,
                                  const int scale,
                                  const float sharpen,
                                  const dt_aligned_pixel_t threshold,
                                  const dt_aligned_pixel_t boost,
                                  const ssize_t width,
                                  const ssize_t height)
{
  const int mult = 1 << scale;
  static const float filter[25] =
    {
      1.0f / 256.0f,  4.0f / 256.0f,  6.0f / 256.0f,  4.0f / 256.0f, 1.0f / 256.0f,
      4.0f / 256.0f, 16.0f / 256.0f, 24.0f / 256.0f, 16.0f / 256.0f, 4.0f / 256.0f,
      6.0f / 256.0f, 24.0f / 256.0f, 36.0f / 256.0f, 24.0f / 256.0f, 6.0f / 256.0f,
      4.0f / 256.0f, 16.0f / 256.0f, 24.0f / 256.0f, 16.0f / 256.0f, 4.0f / 256.0f,
      1.0f / 256.0f,  4.0f / 256.0f,  6.0f / 256.0f,  4.0f / 256.0f, 1.0f / 256.0f
    };
  const int boundary = 2 * mult;
  const dt_aligned_pixel_t vsharpen = { -0.5f * sharpen, -sharpen, -sharpen, 0.0f };

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(accum, filter, height, in, vsharpen, threshold, boost, mult, boundary, out, width) \
  schedule(static)
#endif
  for(size_t rowid = 0; rowid < height; rowid++)
  {
    const size_t j = dwt_interleave_rows(rowid, height, mult);
    const float *px = ((float *)in) + (size_t)4 * j * width;
    const float *px2;
    float *pdetail = accum + (size_t)4 * j * width;
    float *pcoarse = out + (size_t)4 * j * width;

    // for the first and last 'boundary' rows, we have to perform boundary tests for the entire row;
    //   for the central bulk, we only need to use those slower versions on the leftmost and rightmost pixels
    const size_t lbound = (j < boundary || j >= height - boundary) ? width-boundary : boundary;

    /* The first "2*mult" pixels need a boundary check because we might try to access past the left edge,
     * which requires nearest pixel interpolation */
    size_t i;
    for(i = 0; i < lbound; i++)
    {
      SUM_PIXEL_PROLOGUE;
      for(ssize_t jj = 0; jj < 5; jj++)
      {
        const ssize_t y = j + mult * (jj-2);
        const ssize_t clamp_y = CLAMP(y,0,height-1);
        for(ssize_t ii = 0; ii < 5; ii++)
        {
          ssize_t x = i + mult * ((ii)-2);
          if(x < 0) x = 0;			// we might be looking past the left edge
          px2 = ((float *)in) + 4 * x + (size_t)4 * clamp_y * width;
          SUM_PIXEL_CONTRIBUTION;
        }
      }
      SUM_PIXEL_EPILOGUE;
    }

    /* For pixels [2*mult, width-2*mult], we don't need to do any boundary checks */
    for( ; i < width - boundary; i++)
    {
      SUM_PIXEL_PROLOGUE;
      px2 = ((float *)in) + (size_t)4 * (i - 2 * mult + (size_t)(j - 2 * mult) * width);
      for(ssize_t jj = 0; jj < 5; jj++)
      {
        for(ssize_t ii = 0; ii < 5; ii++)
        {
          SUM_PIXEL_CONTRIBUTION;
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
      for(ssize_t jj = 0; jj < 5; jj++)
      {
        const ssize_t y = j + mult * (jj-2);
        const ssize_t clamp_y = CLAMP(y,0,height-1);
        for(ssize_t ii = 0; ii < 5; ii++)
        {
          ssize_t x = i + mult * ((ii)-2);
          if(x >= width) x = width - 1;		// we might be looking beyond the right edge
          px2 = ((float *)in) + 4 * x + (size_t)4 * clamp_y * width;
          SUM_PIXEL_CONTRIBUTION;
        }
      }
      SUM_PIXEL_EPILOGUE;
    }
  }
}

void eaw_synthesize(float *const out, const float *const in, const float *const restrict detail,
                    const float *const restrict threshold, const float *const restrict boost,
                    const int32_t width, const int32_t height)
{
  const dt_aligned_pixel_t thresh = { threshold[0], threshold[1], threshold[2], threshold[3] };
  const dt_aligned_pixel_t boostval = { boost[0], boost[1], boost[2], boost[3] };
  const size_t npixels = (size_t)width * height;

#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(in, out, detail, npixels, thresh, boostval)       \
  schedule(simd:static)
#endif
  for(size_t k = 0; k < npixels; k++)
  {
    accumulate(out + 4*k, detail + 4*k, thresh, boostval);
  }
  dt_omploop_sfence();
}

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
  dt_aligned_pixel_t v;
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
#define SUM_PIXEL_CONTRIBUTION	 		                                                             \
  do                                                                                                         \
  {                                                                                                          \
    const float f = filter[filter_idx++];                                                                    \
    const float wp = dn_weight(px, px2, inv_sigma2);                                                         \
    const float w = f * wp;                                                                                  \
    for_each_channel(c,aligned(px2))                                                                         \
    {                                                                                                        \
      wgt[c] += w;                                                                                           \
      sum[c] += w * px2[c];                                                                                  \
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
  static const float filter[25] =
    {
      1.0f / 256.0f,  4.0f / 256.0f,  6.0f / 256.0f,  4.0f / 256.0f, 1.0f / 256.0f,
      4.0f / 256.0f, 16.0f / 256.0f, 24.0f / 256.0f, 16.0f / 256.0f, 4.0f / 256.0f,
      6.0f / 256.0f, 24.0f / 256.0f, 36.0f / 256.0f, 24.0f / 256.0f, 6.0f / 256.0f,
      4.0f / 256.0f, 16.0f / 256.0f, 24.0f / 256.0f, 16.0f / 256.0f, 4.0f / 256.0f,
      1.0f / 256.0f,  4.0f / 256.0f,  6.0f / 256.0f,  4.0f / 256.0f, 1.0f / 256.0f
    };
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
          SUM_PIXEL_CONTRIBUTION;
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
          SUM_PIXEL_CONTRIBUTION;
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
          SUM_PIXEL_CONTRIBUTION;
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

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

