/*
    This file is part of darktable,
    Copyright (C) 2017-2020 darktable developers.

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
#include "control/control.h"     // needed by dwt.h
#include "common/dwt.h"          // for dwt_interleave_rows
#include <math.h>
#if defined(__SSE__)
#include <xmmintrin.h>
#endif

#if defined(__SSE2__)

#define ALIGNED(a) __attribute__((aligned(a)))
#define VEC4(a)                                                                                              \
  {                                                                                                          \
    (a), (a), (a), (a)                                                                                       \
  }

static const __m128 fone ALIGNED(64) = VEC4(0x3f800000u);
static const __m128 femo ALIGNED(64) = VEC4(0x00adf880u);
static const __m128 o111 ALIGNED(64) = { ~0, ~0, ~0, 0 };

/* SSE intrinsics version of dt_fast_expf defined in darktable.h */
static inline __m128 dt_fast_expf_sse2(const __m128 x)
{
  __m128 f = _mm_add_ps(fone, _mm_mul_ps(x, femo)); // f(n) = i1 + x(n)*(i2-i1)
  __m128i i = _mm_cvtps_epi32(f);                   // i(n) = int(f(n))
  __m128i mask = _mm_srai_epi32(i, 31);             // mask(n) = 0xffffffff if i(n) < 0
  i = _mm_andnot_si128(mask, i);                    // i(n) = 0 if i(n) < 0
  return _mm_castsi128_ps(i);                       // return *(float*)&i
}

#endif

static inline void weight(const float *c1, const float *c2, const float sharpen, float *weight)
{
  float square[3];
  for(int c = 0; c < 3; c++) square[c] = c1[c] - c2[c];
  for(int c = 0; c < 3; c++) square[c] = square[c] * square[c];

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
static inline __m128 weight_sse2(const __m128 *c1, const __m128 *c2, const float sharpen)
{
  const __m128 diff = *c1 - *c2;
  __m128 square = diff * diff;                                      // (?, d3, d2, d1)
  __m128 square2 = _mm_shuffle_ps(square, square, _MM_SHUFFLE(3, 1, 2, 0)); // (?, d2, d3, d1)
  __m128 added = square + square2;                                  // (?, d2+d3, d2+d3, 2*d1)
  added = _mm_sub_ss(added, square);                                // (?, d2+d3, d2+d3, d1)
  __m128 sharpened = added * _mm_set1_ps(-sharpen);                 // (?, -s*(d2+d3), -s*(d2+d3), -s*d1)
  sharpened = _mm_and_ps(sharpened,o111);			    // (0, -s*(d2+d3), -s*(d2+d3), -s*d1)
  return dt_fast_expf_sse2(sharpened);                              // (1, wc, wc, wl)
}
#endif

#define SUM_PIXEL_CONTRIBUTION_COMMON(ii, jj)                                                                \
  {                                                                                                          \
    const float f = filter[(ii)] * filter[(jj)];                                                             \
    float wp[4] = { 0.0f, 0.0f, 0.0f, 0.0f };                                                                \
    weight(px, px2, sharpen, wp);                                                                            \
    float w[4] = { 0.0f, 0.0f, 0.0f, 0.0f };                                                                 \
    for(int c = 0; c < 4; c++) w[c] = f * wp[c];                                                             \
    float pd[4] = { 0.0f, 0.0f, 0.0f, 0.0f };                                                                \
    for(int c = 0; c < 4; c++) pd[c] = w[c] * px2[c];                                                        \
    for(int c = 0; c < 4; c++) sum[c] += pd[c];                                                              \
    for(int c = 0; c < 4; c++) wgt[c] += w[c];                                                               \
  }

#if defined(__SSE2__)
#define SUM_PIXEL_CONTRIBUTION_COMMON_SSE2(ii, jj)                                                           \
  {                                                                                                          \
    const __m128 f = _mm_set1_ps(filter[(ii)] * filter[(jj)]);                                               \
    const __m128 wp = weight_sse2(px, px2, sharpen);                                                         \
    const __m128 w = _mm_mul_ps(f, wp);                                                                      \
    const __m128 pd = _mm_mul_ps(w, *px2);                                                                   \
    sum = _mm_add_ps(sum, pd);                                                                               \
    wgt = _mm_add_ps(wgt, w);                                                                                \
  }
#endif

#define SUM_PIXEL_CONTRIBUTION_WITH_TEST(ii, jj)                                                             \
  do                                                                                                         \
  {                                                                                                          \
    const int iii = (ii)-2;                                                                                  \
    const int jjj = (jj)-2;                                                                                  \
    int x = i + mult * iii;                                                                                  \
    int y = j + mult * jjj;                                                                                  \
                                                                                                             \
    if(x < 0) x = 0;                                                                                         \
    if(x >= width) x = width - 1;                                                                            \
    if(y < 0) y = 0;                                                                                         \
    if(y >= height) y = height - 1;                                                                          \
                                                                                                             \
    px2 = ((float *)in) + 4 * x + (size_t)4 * y * width;                                                     \
                                                                                                             \
    SUM_PIXEL_CONTRIBUTION_COMMON(ii, jj);                                                                   \
  } while(0)

#if defined(__SSE2__)
#define SUM_PIXEL_CONTRIBUTION_WITH_TEST_SSE2(ii, jj)                                                        \
  do                                                                                                         \
  {                                                                                                          \
    const int iii = (ii)-2;                                                                                  \
    const int jjj = (jj)-2;                                                                                  \
    int x = i + mult * iii;                                                                                  \
    int y = j + mult * jjj;                                                                                  \
                                                                                                             \
    if(x < 0) x = 0;                                                                                         \
    if(x >= width) x = width - 1;                                                                            \
    if(y < 0) y = 0;                                                                                         \
    if(y >= height) y = height - 1;                                                                          \
                                                                                                             \
    px2 = ((__m128 *)in) + x + (size_t)y * width;                                                            \
                                                                                                             \
    SUM_PIXEL_CONTRIBUTION_COMMON_SSE2(ii, jj);                                                              \
  } while(0)
#endif

#define ROW_PROLOGUE                                                                                         \
  const float *px = ((float *)in) + (size_t)4 * j * width;                                                   \
  const float *px2;                                                                                          \
  float *pdetail = detail + (size_t)4 * j * width;                                                           \
  float *pcoarse = out + (size_t)4 * j * width;

#if defined(__SSE2__)
#define ROW_PROLOGUE_SSE                                                                                     \
  const __m128 *px = ((__m128 *)in) + (size_t)j * width;                                                     \
  const __m128 *px2;                                                                                         \
  float *pdetail = detail + (size_t)4 * j * width;                                                           \
  float *pcoarse = out + (size_t)4 * j * width;
#endif

#define SUM_PIXEL_PROLOGUE                                                                                   \
  float sum[4] = { 0.0f, 0.0f, 0.0f, 0.0f };                                                                 \
  float wgt[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

#if defined(__SSE2__)
#define SUM_PIXEL_PROLOGUE_SSE                                                                               \
  __m128 sum = _mm_setzero_ps();                                                                             \
  __m128 wgt = _mm_setzero_ps();
#endif

#define SUM_PIXEL_EPILOGUE                                                                                   \
  for(int c = 0; c < 4; c++) sum[c] /= wgt[c];                                                               \
                                                                                                             \
  for(int c = 0; c < 4; c++) pdetail[c] = (px[c] - sum[c]);                                                  \
  for(int c = 0; c < 4; c++) pcoarse[c] = sum[c];                                                            \
  px += 4;                                                                                                   \
  pdetail += 4;                                                                                              \
  pcoarse += 4;

#if defined(__SSE2__)
#define SUM_PIXEL_EPILOGUE_SSE                                                                               \
  sum = _mm_mul_ps(sum, _mm_rcp_ps(wgt));                                                                    \
                                                                                                             \
  _mm_stream_ps(pdetail, _mm_sub_ps(*px, sum));                                                              \
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

/* The first "2*mult" lines use the macro with tests because the 5x5 kernel
 * requires nearest pixel interpolation for at least a pixel in the sum */
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(detail, filter, height, in, mult, out, sharpen, width) \
  schedule(static)
#endif
  for(int j = 0; j < 2 * mult; j++)
  {
    ROW_PROLOGUE

    for(int i = 0; i < width; i++)
    {
      SUM_PIXEL_PROLOGUE
      for(int jj = 0; jj < 5; jj++)
      {
        for(int ii = 0; ii < 5; ii++)
        {
          SUM_PIXEL_CONTRIBUTION_WITH_TEST(ii, jj);
        }
      }
      SUM_PIXEL_EPILOGUE
    }
  }

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(detail, filter, height, in, mult, out, sharpen, width) \
  schedule(static)
#endif
  for(int j = 2 * mult; j < height - 2 * mult; j++)
  {
    ROW_PROLOGUE

    /* The first "2*mult" pixels use the macro with tests because the 5x5 kernel
     * requires nearest pixel interpolation for at least a pixel in the sum */
    for(int i = 0; i < 2 * mult; i++)
    {
      SUM_PIXEL_PROLOGUE
      for(int jj = 0; jj < 5; jj++)
      {
        for(int ii = 0; ii < 5; ii++)
        {
          SUM_PIXEL_CONTRIBUTION_WITH_TEST(ii, jj);
        }
      }
      SUM_PIXEL_EPILOGUE
    }

    /* For pixels [2*mult, width-2*mult], we can safely use macro w/o tests
     * to avoid unneeded branching in the inner loops */
    for(int i = 2 * mult; i < width - 2 * mult; i++)
    {
      SUM_PIXEL_PROLOGUE
      px2 = ((float *)in) + (size_t)4 * (i - 2 * mult + (size_t)(j - 2 * mult) * width);
      for(int jj = 0; jj < 5; jj++)
      {
        for(int ii = 0; ii < 5; ii++)
        {
          SUM_PIXEL_CONTRIBUTION_COMMON(ii, jj);
          px2 += (size_t)4 * mult;
        }
        px2 += (size_t)4 * (width - 5) * mult;
      }
      SUM_PIXEL_EPILOGUE
    }

    /* Last two pixels in the row require a slow variant... blablabla */
    for(int i = width - 2 * mult; i < width; i++)
    {
      SUM_PIXEL_PROLOGUE
      for(int jj = 0; jj < 5; jj++)
      {
        for(int ii = 0; ii < 5; ii++)
        {
          SUM_PIXEL_CONTRIBUTION_WITH_TEST(ii, jj);
        }
      }
      SUM_PIXEL_EPILOGUE
    }
  }

/* The last "2*mult" lines use the macro with tests because the 5x5 kernel
 * requires nearest pixel interpolation for at least a pixel in the sum */
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(detail, filter, height, in, mult, out, sharpen, width) \
  schedule(static)
#endif
  for(int j = height - 2 * mult; j < height; j++)
  {
    ROW_PROLOGUE

    for(int i = 0; i < width; i++)
    {
      SUM_PIXEL_PROLOGUE
      for(int jj = 0; jj < 5; jj++)
      {
        for(int ii = 0; ii < 5; ii++)
        {
          SUM_PIXEL_CONTRIBUTION_WITH_TEST(ii, jj);
        }
      }
      SUM_PIXEL_EPILOGUE
    }
  }
}

#undef SUM_PIXEL_CONTRIBUTION_COMMON
#undef SUM_PIXEL_CONTRIBUTION_WITH_TEST
#undef ROW_PROLOGUE
#undef SUM_PIXEL_PROLOGUE
#undef SUM_PIXEL_EPILOGUE


#if defined(__SSE2__)
void eaw_decompose_sse2(float *const restrict out, const float *const restrict in, float *const restrict detail,
                        const int scale, const float sharpen, const int32_t width, const int32_t height)
{
  const int mult = 1 << scale;
  static const float filter[5] = { 1.0f / 16.0f, 4.0f / 16.0f, 6.0f / 16.0f, 4.0f / 16.0f, 1.0f / 16.0f };

/* The first "2*mult" lines use the macro with tests because the 5x5 kernel
 * requires nearest pixel interpolation for at least a pixel in the sum */
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(detail, filter, height, in, mult, out, sharpen, width) \
  schedule(static)
#endif
  for(int j = 0; j < 2 * mult; j++)
  {
    ROW_PROLOGUE_SSE

    for(int i = 0; i < width; i++)
    {
      SUM_PIXEL_PROLOGUE_SSE
      for(int jj = 0; jj < 5; jj++)
      {
        for(int ii = 0; ii < 5; ii++)
        {
          SUM_PIXEL_CONTRIBUTION_WITH_TEST_SSE2(ii, jj);
        }
      }
      SUM_PIXEL_EPILOGUE_SSE
    }
  }

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(detail, filter, height, in, mult, out, sharpen, width) \
  schedule(static)
#endif
  for(int j = 2 * mult; j < height - 2 * mult; j++)
  {
    ROW_PROLOGUE_SSE

    /* The first "2*mult" pixels use the macro with tests because the 5x5 kernel
     * requires nearest pixel interpolation for at least a pixel in the sum */
    for(int i = 0; i < 2 * mult; i++)
    {
      SUM_PIXEL_PROLOGUE_SSE
      for(int jj = 0; jj < 5; jj++)
      {
        for(int ii = 0; ii < 5; ii++)
        {
          SUM_PIXEL_CONTRIBUTION_WITH_TEST_SSE2(ii, jj);
        }
      }
      SUM_PIXEL_EPILOGUE_SSE
    }

    /* For pixels [2*mult, width-2*mult], we can safely use macro w/o tests
     * to avoid unneeded branching in the inner loops */
    for(int i = 2 * mult; i < width - 2 * mult; i++)
    {
      SUM_PIXEL_PROLOGUE_SSE
      px2 = ((__m128 *)in) + i - 2 * mult + (size_t)(j - 2 * mult) * width;
      for(int jj = 0; jj < 5; jj++)
      {
        for(int ii = 0; ii < 5; ii++)
        {
          SUM_PIXEL_CONTRIBUTION_COMMON_SSE2(ii, jj);
          px2 += mult;
        }
        px2 += (width - 5) * mult;
      }
      SUM_PIXEL_EPILOGUE_SSE
    }

    /* Last two pixels in the row require a slow variant... blablabla */
    for(int i = width - 2 * mult; i < width; i++)
    {
      SUM_PIXEL_PROLOGUE_SSE
      for(int jj = 0; jj < 5; jj++)
      {
        for(int ii = 0; ii < 5; ii++)
        {
          SUM_PIXEL_CONTRIBUTION_WITH_TEST_SSE2(ii, jj);
        }
      }
      SUM_PIXEL_EPILOGUE_SSE
    }
  }

/* The last "2*mult" lines use the macro with tests because the 5x5 kernel
 * requires nearest pixel interpolation for at least a pixel in the sum */
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(detail, filter, height, in, mult, out, sharpen, width) \
  schedule(static)
#endif
  for(int j = height - 2 * mult; j < height; j++)
  {
    ROW_PROLOGUE_SSE

    for(int i = 0; i < width; i++)
    {
      SUM_PIXEL_PROLOGUE_SSE
      for(int jj = 0; jj < 5; jj++)
      {
        for(int ii = 0; ii < 5; ii++)
        {
          SUM_PIXEL_CONTRIBUTION_WITH_TEST_SSE2(ii, jj);
        }
      }
      SUM_PIXEL_EPILOGUE_SSE
    }
  }

  _mm_sfence();
}

#undef SUM_PIXEL_CONTRIBUTION_COMMON_SSE2
#undef SUM_PIXEL_CONTRIBUTION_WITH_TEST_SSE2
#undef ROW_PROLOGUE_SSE
#undef SUM_PIXEL_PROLOGUE_SSE
#undef SUM_PIXEL_EPILOGUE_SSE
#endif

void eaw_synthesize(float *const restrict out, const float *const restrict in, const float *const restrict detail,
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
void eaw_synthesize_sse2(float *const restrict out, const float *const restrict in, const float *const restrict detail,
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
  for(size_t j = 0; j < width * height; j++)
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
  float sqr[3];
  for(int c = 0; c < 3; c++)
  {
    float diff = c1[c] - c2[c];
    sqr[c] = diff * diff;
  }
  const float dot = (sqr[0] + sqr[1] + sqr[2]) * inv_sigma2;
  const float var
      = 0.02f; // FIXME: this should ideally depend on the image before noise stabilizing transforms!
  const float off2 = 9.0f; // (3 sigma)^2
  return fast_mexp2f(MAX(0, dot * var - off2));
}

#if defined(__SSE__)
static inline float dn_weight_sse(const __m128 *c1, const __m128 *c2, const float inv_sigma2)
{
  // 3d distance based on color
  __m128 diff = _mm_sub_ps(*c1, *c2);
  __m128 sqr = _mm_mul_ps(diff, diff);
  const float dot = (sqr[0] + sqr[1] + sqr[2]) * inv_sigma2;
  const float var
      = 0.02f; // FIXME: this should ideally depend on the image before noise stabilizing transforms!
  const float off2 = 9.0f; // (3 sigma)^2
  return fast_mexp2f(MAX(0, dot * var - off2));
}
#endif

#ifdef __SSE2__
# define PREFETCH(p) _mm_prefetch(p,_MM_HINT_NTA);
#else
# define PREFETCH(p)
#endif /* __SSE2__ */

#define SUM_PIXEL_CONTRIBUTION(ii, jj) 		                                                             \
  do                                                                                                         \
  {                                                                                                          \
    PREFETCH(px2+8);                                                                                         \
    const float f = filter[(ii)] * filter[(jj)];                                                             \
    const float wp = dn_weight(px, px2, inv_sigma2);                                                         \
    const float w = f * wp;                                                                                  \
    float pd[4];                                                                                             \
    for(int c = 0; c < 4; c++) pd[c] = w * px2[c];                                                           \
    for(int c = 0; c < 4; c++) sum[c] += pd[c];                                                              \
    for(int c = 0; c < 4; c++) wgt[c] += w;                                                                  \
  } while(0)

#if defined(__SSE__)
#define SUM_PIXEL_CONTRIBUTION_SSE(ii, jj)	                                                             \
  do                                                                                                         \
  {                                                                                                          \
    PREFETCH(px2+2);                                                                                         \
    const float f = filter[(ii)] * filter[(jj)];	                                                     \
    const float wp = dn_weight_sse(px, px2, inv_sigma2);                                                     \
    const __m128 w = _mm_set1_ps(f * wp);                                                                    \
    const __m128 pd = *px2 * w;                                                                              \
    sum = sum + pd;                                                                                          \
    wgt = wgt + w;                                                                                           \
  } while(0)
#endif

#define SUM_PIXEL_PROLOGUE                                                                                   \
  float sum[4] = { 0.0f, 0.0f, 0.0f, 0.0f };                                                                 \
  float wgt[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

#if defined(__SSE__)
#define SUM_PIXEL_PROLOGUE_SSE                                                                               \
  __m128 sum = _mm_setzero_ps();                                                                             \
  __m128 wgt = _mm_setzero_ps();
#endif

#define SUM_PIXEL_EPILOGUE                                                                                   \
  for(int c = 0; c < 4; c++)										     \
  {													     \
    sum[c] /= wgt[c];                                                   				     \
    pcoarse[c] = sum[c];                                                                                     \
    float det = (px[c] - sum[c]);									     \
    pdetail[c] = det;    		                                              			     \
    sum_sq[c] += (det*det);					                                             \
  }                                                                       				     \
  px += 4;                                                                                                   \
  pdetail += 4;                                                                                              \
  pcoarse += 4;

#if defined(__SSE__)
#define SUM_PIXEL_EPILOGUE_SSE                                                                               \
  sum = sum / wgt;		                                                                             \
  _mm_stream_ps(pcoarse, sum);                                                                               \
  sum = *px - sum;											     \
  _mm_stream_ps(pdetail, sum);                                                                               \
  sum_sq = sum_sq + sum*sum;					                                             \
  px++;                                                                                                      \
  pdetail += 4;                                                                                              \
  pcoarse += 4;
#endif

void eaw_dn_decompose(float *const restrict out, const float *const restrict in, float *const restrict detail,
                      float sum_squared[4], const int scale, const float inv_sigma2,
                      const int32_t width, const int32_t height)
{
  const int mult = 1u << scale;
  static const float filter[5] = { 1.0f / 16.0f, 4.0f / 16.0f, 6.0f / 16.0f, 4.0f / 16.0f, 1.0f / 16.0f };
  const int boundary = 2 * mult;
  const int nthreads = dt_get_num_threads();
  float *squared_sums = dt_alloc_align(64,3*sizeof(float)*nthreads);
  for(int i = 0; i < 3*nthreads; i++)
    squared_sums[i] = 0.0f;

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(detail, filter, height, in, inv_sigma2, mult, boundary, out, width, squared_sums) \
  schedule(static)
#endif
  for(int rowid = 0; rowid < height; rowid++)
  {
    const size_t j = dwt_interleave_rows(rowid, height, mult);
    const float *px = ((float *)in) + (size_t)4 * j * width;
    const float *px2;
    float *pdetail = detail + (size_t)4 * j * width;
    float *pcoarse = out + (size_t)4 * j * width;
    float sum_sq[4] = { 0 };

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
    int tnum = dt_get_thread_num();
    for(i = 0; i < 3; i++)
      squared_sums[3*tnum+i] += sum_sq[i];
  }
  // reduce the per-thread sums to a single value
  for(int c = 0; c < 3; c++)
  {
    sum_squared[c] = 0.0f;
    for(int i = 0; i < nthreads; i++)
      sum_squared[c] += squared_sums[3*i+c];
  }
  dt_free_align(squared_sums);
}

#undef SUM_PIXEL_CONTRIBUTION
#undef SUM_PIXEL_PROLOGUE
#undef SUM_PIXEL_EPILOGUE


#if defined(__SSE2__)
void eaw_dn_decompose_sse(float *const restrict out, const float *const restrict in, float *const restrict detail,
                                 float sum_squared[4], const int scale, const float inv_sigma2,
                                 const int32_t width, const int32_t height)
{
  const int mult = 1u << scale;
  static const float filter[5] = { 1.0f / 16.0f, 4.0f / 16.0f, 6.0f / 16.0f, 4.0f / 16.0f, 1.0f / 16.0f };
  const int boundary = 2 * mult;
  const int nthreads = dt_get_num_threads();
  __m128 *squared_sums = dt_alloc_align(64,sizeof(__m128)*nthreads);
  for(int i = 0; i < nthreads; i++)
    squared_sums[i] = _mm_setzero_ps();

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(detail, filter, height, in, inv_sigma2, mult, boundary, out, width) \
  shared(squared_sums) \
  schedule(static)
#endif
  for(int rowid = 0; rowid < height; rowid++)
  {
    const size_t j = dwt_interleave_rows(rowid, height, mult);
    const __m128 *px = ((__m128 *)in) + (size_t)j * width;
    const __m128 *px2;
    float *pdetail = detail + (size_t)4 * j * width;
    float *pcoarse = out + (size_t)4 * j * width;
    __m128 sum_sq = _mm_setzero_ps();

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
    squared_sums[dt_get_thread_num()] += sum_sq;
  }
  // reduce the per-thread sums to a single value
  __m128 sum = _mm_setzero_ps();
  for(int i = 0; i < nthreads; i++)
    sum += squared_sums[i];
  dt_free_align(squared_sums);
  _mm_store_ps(sum_squared, sum);
  _mm_sfence();
}

#undef SUM_PIXEL_CONTRIBUTION_SSE
#undef SUM_PIXEL_PROLOGUE_SSE
#undef SUM_PIXEL_EPILOGUE_SSE
#endif

