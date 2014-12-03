/*
    This file is part of darktable,
    copyright (c) 2014 LebedevRI.

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
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <xmmintrin.h>
#include <stdlib.h>
#include <assert.h>

#include "common/darktable.h"
#include "develop/imageop.h"
#include "common/histogram.h"

//------------------------------------------------------------------------------

static void inline histogram_helper_cs_RAW_helper_process_pixel_float(
    const dt_dev_histogram_collection_params_t *const histogram_params, const float *pixel,
    uint32_t *histogram)
{
  const uint32_t V
      = CLAMP((float)(histogram_params->bins_count) * *pixel, 0, histogram_params->bins_count - 1);
  histogram[4 * V]++;
}

static void inline histogram_helper_cs_RAW(const dt_dev_histogram_collection_params_t *const histogram_params,
                                           const void *pixel, uint32_t *histogram, int j)
{
  const dt_iop_roi_t *roi = histogram_params->roi;
  const float *input = (float *)pixel + roi->width * j;
  for(int i = roi->x; i < roi->width; i++, input++)
  {
    histogram_helper_cs_RAW_helper_process_pixel_float(histogram_params, input, histogram);
  }
}

//------------------------------------------------------------------------------

static void inline histogram_helper_cs_RAW_helper_process_pixel_uint16(
    const dt_dev_histogram_collection_params_t *const histogram_params, const uint16_t *pixel,
    uint32_t *histogram)
{
  const uint32_t V = CLAMP((float)(histogram_params->bins_count) / (float)UINT16_MAX * *pixel, 0,
                           histogram_params->bins_count - 1);
  histogram[4 * V]++;
}

static void inline histogram_helper_cs_RAW_helper_process_pixel_si128(
    const dt_dev_histogram_collection_params_t *const histogram_params, const __m128i *pixel,
    uint32_t *histogram)
{
  const float scale = (float)(histogram_params->bins_count) / (float)UINT16_MAX;

  const __m128 fscale = _mm_set1_ps(scale);
  const __m128 val_min = _mm_setzero_ps();
  const __m128 val_max = _mm_set1_ps(histogram_params->bins_count - 1);

  assert(dt_is_aligned(pixel, 16));
  const __m128i input = _mm_load_si128(pixel);
  __m128i ilo = _mm_unpacklo_epi16(input, _mm_set1_epi16(0));
  __m128i ihi = _mm_unpackhi_epi16(input, _mm_set1_epi16(0));
  __m128 flo = _mm_cvtepi32_ps(ilo);
  __m128 fhi = _mm_cvtepi32_ps(ihi);

  flo = _mm_mul_ps(flo, fscale);
  fhi = _mm_mul_ps(fhi, fscale);

  flo = _mm_max_ps(_mm_min_ps(flo, val_max), val_min);
  fhi = _mm_max_ps(_mm_min_ps(fhi, val_max), val_min);

  ilo = _mm_cvtps_epi32(flo);
  ihi = _mm_cvtps_epi32(fhi);

  __m128i values[2] __attribute__((aligned(16)));
  _mm_store_si128(&(values[0]), ilo);
  _mm_store_si128(&(values[1]), ihi);

  const uint32_t *valuesi = (uint32_t *)(&values);

  for(int k = 0; k < 8; k++) histogram[4 * valuesi[k]]++;
}

void inline dt_histogram_helper_cs_RAW_uint16(
    const dt_dev_histogram_collection_params_t *const histogram_params, const void *pixel,
    uint32_t *histogram, int j)
{
  const dt_iop_roi_t *roi = histogram_params->roi;
  uint16_t *in = (uint16_t *)pixel + roi->width * j;

  int i = roi->x;
  int alignment = ((8 - (j * roi->width & (8 - 1))) & (8 - 1));

  // process unaligned pixels
  for(; i < alignment; i++, in++)
    histogram_helper_cs_RAW_helper_process_pixel_uint16(histogram_params, in, histogram);

  // process aligned pixels with SSE
  for(; i < roi->width - (8 - 1); i += 8, in += 8)
    histogram_helper_cs_RAW_helper_process_pixel_si128(histogram_params, (__m128i *)in, histogram);

  // process the rest
  for(; i < roi->width; i++, in++)
    histogram_helper_cs_RAW_helper_process_pixel_uint16(histogram_params, in, histogram);
}

//------------------------------------------------------------------------------

static void inline __attribute__((__unused__)) histogram_helper_cs_rgb_helper_process_pixel_float(
    const dt_dev_histogram_collection_params_t *const histogram_params, const float *pixel,
    uint32_t *histogram)
{
  const float Rv = pixel[0];
  const float Gv = pixel[1];
  const float Bv = pixel[2];
  const uint32_t R = CLAMP((float)(histogram_params->bins_count) * Rv, 0, histogram_params->bins_count - 1);
  const uint32_t G = CLAMP((float)(histogram_params->bins_count) * Gv, 0, histogram_params->bins_count - 1);
  const uint32_t B = CLAMP((float)(histogram_params->bins_count) * Bv, 0, histogram_params->bins_count - 1);
  histogram[4 * R]++;
  histogram[4 * G + 1]++;
  histogram[4 * B + 2]++;
}

static void inline histogram_helper_cs_rgb_helper_process_pixel_m128(
    const dt_dev_histogram_collection_params_t *const histogram_params, const float *pixel,
    uint32_t *histogram)
{
  const float fscale = (float)(histogram_params->bins_count);

  const __m128 scale = _mm_set1_ps(fscale);
  const __m128 val_min = _mm_setzero_ps();
  const __m128 val_max = _mm_set1_ps(histogram_params->bins_count - 1);

  assert(dt_is_aligned(pixel, 16));
  const __m128 input = _mm_load_ps(pixel);
  const __m128 scaled = _mm_mul_ps(input, scale);
  const __m128 clamped = _mm_max_ps(_mm_min_ps(scaled, val_max), val_min);

  const __m128i indexes = _mm_cvtps_epi32(clamped);

  __m128i values __attribute__((aligned(16)));
  _mm_store_si128(&values, indexes);

  const uint32_t *valuesi = (uint32_t *)(&values);

  histogram[4 * valuesi[0]]++;
  histogram[4 * valuesi[1] + 1]++;
  histogram[4 * valuesi[2] + 2]++;
}

static void inline histogram_helper_cs_rgb(const dt_dev_histogram_collection_params_t *const histogram_params,
                                           const void *pixel, uint32_t *histogram, int j)
{
  const dt_iop_roi_t *roi = histogram_params->roi;
  float *in = (float *)pixel + 4 * roi->width * j;

  // process aligned pixels with SSE
  for(int i = roi->x; i < roi->width; i++, in += 4)
    histogram_helper_cs_rgb_helper_process_pixel_m128(histogram_params, in, histogram);
}

//------------------------------------------------------------------------------

static void inline __attribute__((__unused__)) histogram_helper_cs_Lab_helper_process_pixel_float(
    const dt_dev_histogram_collection_params_t *const histogram_params, const float *pixel,
    uint32_t *histogram)
{
  const float Lv = pixel[0];
  const float av = pixel[1];
  const float bv = pixel[2];
  const uint32_t L
      = CLAMP((float)(histogram_params->bins_count) / 100.0f * (Lv), 0, histogram_params->bins_count - 1);
  const uint32_t a = CLAMP((float)(histogram_params->bins_count) / 256.0f * (av + 128.0f), 0,
                           histogram_params->bins_count - 1);
  const uint32_t b = CLAMP((float)(histogram_params->bins_count) / 256.0f * (bv + 128.0f), 0,
                           histogram_params->bins_count - 1);
  histogram[4 * L]++;
  histogram[4 * a + 1]++;
  histogram[4 * b + 2]++;
}

static void inline histogram_helper_cs_Lab_helper_process_pixel_m128(
    const dt_dev_histogram_collection_params_t *const histogram_params, const float *pixel,
    uint32_t *histogram)
{
  const float fscale = (float)(histogram_params->bins_count);

  const __m128 shift = _mm_set_ps(0.0f, 128.0f, 128.0f, 0.0f);
  const __m128 scale = _mm_set_ps(fscale / 1.0f, fscale / 256.0f, fscale / 256.0f, fscale / 100.0f);
  const __m128 val_min = _mm_setzero_ps();
  const __m128 val_max = _mm_set1_ps(histogram_params->bins_count - 1);

  assert(dt_is_aligned(pixel, 16));
  const __m128 input = _mm_load_ps(pixel);
  const __m128 shifted = _mm_add_ps(input, shift);
  const __m128 scaled = _mm_mul_ps(shifted, scale);
  const __m128 clamped = _mm_max_ps(_mm_min_ps(scaled, val_max), val_min);

  const __m128i indexes = _mm_cvtps_epi32(clamped);

  __m128i values __attribute__((aligned(16)));
  _mm_store_si128(&values, indexes);

  const uint32_t *valuesi = (uint32_t *)(&values);

  histogram[4 * valuesi[0]]++;
  histogram[4 * valuesi[1] + 1]++;
  histogram[4 * valuesi[2] + 2]++;
}

static void inline histogram_helper_cs_Lab(const dt_dev_histogram_collection_params_t *const histogram_params,
                                           const void *pixel, uint32_t *histogram, int j)
{
  const dt_iop_roi_t *roi = histogram_params->roi;
  float *in = (float *)pixel + 4 * roi->width * j;

  // process aligned pixels with SSE
  for(int i = roi->x; i < roi->width; i++, in += 4)
    histogram_helper_cs_Lab_helper_process_pixel_m128(histogram_params, in, histogram);
}

//==============================================================================

void dt_histogram_worker(const dt_dev_histogram_collection_params_t *const histogram_params,
                         dt_dev_histogram_stats_t *histogram_stats, const void *const pixel,
                         uint32_t **histogram, const dt_worker Worker)
{
  const int nthreads = omp_get_max_threads();

  const size_t bins_total = histogram_params->bins_count * 4;
  const size_t buf_size = bins_total * sizeof(uint32_t);
  void *partial_hists = calloc(nthreads, buf_size);

  const dt_iop_roi_t *const roi = histogram_params->roi;

#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none) shared(partial_hists)
#endif
  for(int j = roi->y; j < roi->height; j++)
  {
    uint32_t *thread_hist = (uint32_t *)partial_hists + bins_total * omp_get_thread_num();
    Worker(histogram_params, pixel, thread_hist, j);
  }

#ifdef _OPENMP
  *histogram = realloc(*histogram, buf_size);
  memset(*histogram, 0, buf_size);
  uint32_t *hist = *histogram;

#pragma omp parallel for schedule(static) default(none) shared(hist, partial_hists)
  for(size_t k = 0; k < bins_total; k++)
  {
    for(size_t n = 0; n < nthreads; n++)
    {
      const uint32_t *thread_hist = (uint32_t *)partial_hists + bins_total * n;
      hist[k] += thread_hist[k];
    }
  }
#else
  *histogram = realloc(*histogram, buf_size);
  memmove(*histogram, partial_hists, buf_size);
#endif
  free(partial_hists);

  histogram_stats->bins_count = histogram_params->bins_count;
  histogram_stats->pixels = (roi->width - roi->x) * (roi->height - roi->y);
}

//------------------------------------------------------------------------------

void dt_histogram_helper(const dt_dev_histogram_collection_params_t *const histogram_params,
                         dt_dev_histogram_stats_t *histogram_stats, dt_iop_colorspace_type_t cst,
                         const void *pixel, uint32_t **histogram)
{
  switch(cst)
  {
    case iop_cs_RAW:
      dt_histogram_worker(histogram_params, histogram_stats, pixel, histogram, histogram_helper_cs_RAW);
      histogram_stats->ch = 1u;
      break;

    case iop_cs_rgb:
      dt_histogram_worker(histogram_params, histogram_stats, pixel, histogram, histogram_helper_cs_rgb);
      histogram_stats->ch = 3u;
      break;

    case iop_cs_Lab:
    default:
      dt_histogram_worker(histogram_params, histogram_stats, pixel, histogram, histogram_helper_cs_Lab);
      histogram_stats->ch = 3u;
      break;
  }
}

void dt_histogram_max_helper(const dt_dev_histogram_stats_t *const histogram_stats,
                             dt_iop_colorspace_type_t cst, uint32_t **histogram, uint32_t *histogram_max)
{
  if(*histogram == NULL) return;
  histogram_max[0] = histogram_max[1] = histogram_max[2] = histogram_max[3] = 0;
  uint32_t *hist = *histogram;
  switch(cst)
  {
    case iop_cs_RAW:
      for(int k = 0; k < 4 * histogram_stats->bins_count; k += 4)
        histogram_max[0] = histogram_max[0] > hist[k] ? histogram_max[0] : hist[k];
      break;

    case iop_cs_rgb:
      // don't count <= 0 pixels
      for(int k = 4; k < 4 * histogram_stats->bins_count; k += 4)
        histogram_max[0] = histogram_max[0] > hist[k] ? histogram_max[0] : hist[k];
      for(int k = 5; k < 4 * histogram_stats->bins_count; k += 4)
        histogram_max[1] = histogram_max[1] > hist[k] ? histogram_max[1] : hist[k];
      for(int k = 6; k < 4 * histogram_stats->bins_count; k += 4)
        histogram_max[2] = histogram_max[2] > hist[k] ? histogram_max[2] : hist[k];
      for(int k = 7; k < 4 * histogram_stats->bins_count; k += 4)
        histogram_max[3] = histogram_max[3] > hist[k] ? histogram_max[3] : hist[k];
      break;

    case iop_cs_Lab:
    default:
      // don't count <= 0 pixels in L
      for(int k = 4; k < 4 * histogram_stats->bins_count; k += 4)
        histogram_max[0] = histogram_max[0] > hist[k] ? histogram_max[0] : hist[k];

      // don't count <= -128 and >= +128 pixels in a and b
      for(int k = 5; k < 4 * (histogram_stats->bins_count - 1); k += 4)
        histogram_max[1] = histogram_max[1] > hist[k] ? histogram_max[1] : hist[k];
      for(int k = 6; k < 4 * (histogram_stats->bins_count - 1); k += 4)
        histogram_max[2] = histogram_max[2] > hist[k] ? histogram_max[2] : hist[k];
      break;
  }
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
