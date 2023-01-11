/*
    This file is part of darktable,
    Copyright (C) 2014-2020 darktable developers.

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
#if defined(__SSE__)
#include <xmmintrin.h>
#endif
#include <assert.h>
#include <stdlib.h>

#include "common/colorspaces_inline_conversions.h"
#include "common/darktable.h"
#include "common/histogram.h"
#include "develop/imageop.h"

#define S(V, params) ((params->mul) * ((float)V))
#define P(V, params) (CLAMP((V), 0, (params->bins_count - 1)))
#define PU(V, params) (MIN((V), (params->bins_count - 1)))
#define PS(V, params) (P(S(V, params), params))

//------------------------------------------------------------------------------

inline static void histogram_helper_cs_RAW_helper_process_pixel_float(
    const dt_dev_histogram_collection_params_t *const histogram_params, const float *pixel, uint32_t *histogram)
{
  const uint32_t i = PS(*pixel, histogram_params);
  histogram[4 * i]++;
}

inline static void histogram_helper_cs_RAW(const dt_dev_histogram_collection_params_t *const histogram_params,
                                           const void *pixel, uint32_t *histogram, int j,
                                           const dt_iop_order_iccprofile_info_t *const profile_info)
{
  const dt_histogram_roi_t *roi = histogram_params->roi;
  const float *input = (float *)pixel + roi->width * j + roi->crop_x;
  for(int i = 0; i < roi->width - roi->crop_width - roi->crop_x; i++, input++)
  {
    histogram_helper_cs_RAW_helper_process_pixel_float(histogram_params, input, histogram);
  }
}

//------------------------------------------------------------------------------

// WARNING: you must ensure that bins_count is big enough
inline static void histogram_helper_cs_RAW_helper_process_pixel_uint16(
    const dt_dev_histogram_collection_params_t *const histogram_params, const uint16_t *pixel, uint32_t *histogram)
{
  const uint16_t i = PU(*pixel, histogram_params);
  histogram[4 * i]++;
}

inline void dt_histogram_helper_cs_RAW_uint16(const dt_dev_histogram_collection_params_t *const histogram_params,
                                              const void *pixel, uint32_t *histogram, int j,
                                              const dt_iop_order_iccprofile_info_t *const profile_info)
{
  const dt_histogram_roi_t *roi = histogram_params->roi;
  uint16_t *in = (uint16_t *)pixel + roi->width * j + roi->crop_x;

  // process pixels
  for(int i = 0; i < roi->width - roi->crop_width - roi->crop_x; i++, in++)
    histogram_helper_cs_RAW_helper_process_pixel_uint16(histogram_params, in, histogram);
}

//------------------------------------------------------------------------------

inline static void __attribute__((__unused__)) histogram_helper_cs_rgb_helper_process_pixel_float(
    const dt_dev_histogram_collection_params_t *const histogram_params, const float *pixel, uint32_t *histogram)
{
  const uint32_t R = PS(pixel[0], histogram_params);
  const uint32_t G = PS(pixel[1], histogram_params);
  const uint32_t B = PS(pixel[2], histogram_params);
  histogram[4 * R]++;
  histogram[4 * G + 1]++;
  histogram[4 * B + 2]++;
}

inline static void __attribute__((__unused__)) histogram_helper_cs_rgb_helper_process_pixel_float_compensated(
    const dt_dev_histogram_collection_params_t *const histogram_params, const float *pixel, uint32_t *histogram,
    const dt_iop_order_iccprofile_info_t *const profile_info)
{
  const dt_aligned_pixel_t rgb = { dt_ioppr_compensate_middle_grey(pixel[0], profile_info),
                                   dt_ioppr_compensate_middle_grey(pixel[1], profile_info),
                                   dt_ioppr_compensate_middle_grey(pixel[2], profile_info) };
  const uint32_t R = PS(rgb[0], histogram_params);
  const uint32_t G = PS(rgb[1], histogram_params);
  const uint32_t B = PS(rgb[2], histogram_params);
  histogram[4 * R]++;
  histogram[4 * G + 1]++;
  histogram[4 * B + 2]++;
}

#if defined(__SSE2__)
inline static void histogram_helper_cs_rgb_helper_process_pixel_m128(
    const dt_dev_histogram_collection_params_t *const histogram_params, const float *pixel, uint32_t *histogram)
{
  const __m128 scale = _mm_set1_ps(histogram_params->mul);
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

inline static void histogram_helper_cs_rgb_helper_process_pixel_m128_compensated(
    const dt_dev_histogram_collection_params_t *const histogram_params, const float *pixel, uint32_t *histogram,
    const dt_iop_order_iccprofile_info_t *const profile_info)
{
  const __m128 rgb = { dt_ioppr_compensate_middle_grey(pixel[0], profile_info),
      dt_ioppr_compensate_middle_grey(pixel[1], profile_info),
      dt_ioppr_compensate_middle_grey(pixel[2], profile_info), 1.f };
  const __m128 scale = _mm_set1_ps(histogram_params->mul);
  const __m128 val_min = _mm_setzero_ps();
  const __m128 val_max = _mm_set1_ps(histogram_params->bins_count - 1);

  assert(dt_is_aligned(pixel, 16));
  const __m128 input = rgb;
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
#endif

inline static void histogram_helper_cs_rgb(const dt_dev_histogram_collection_params_t *const histogram_params,
                                           const void *pixel, uint32_t *histogram, int j,
                                           const dt_iop_order_iccprofile_info_t *const profile_info)
{
  const dt_histogram_roi_t *roi = histogram_params->roi;
  float *in = (float *)pixel + 4 * (roi->width * j + roi->crop_x);

  // process aligned pixels with SSE
  for(int i = 0; i < roi->width - roi->crop_width - roi->crop_x; i++, in += 4)
  {
    if(darktable.codepath.OPENMP_SIMD)
      histogram_helper_cs_rgb_helper_process_pixel_float(histogram_params, in, histogram);
#if defined(__SSE2__)
    else if(darktable.codepath.SSE2)
      histogram_helper_cs_rgb_helper_process_pixel_m128(histogram_params, in, histogram);
#endif
    else
      dt_unreachable_codepath();
  }
}

inline static void histogram_helper_cs_rgb_compensated(const dt_dev_histogram_collection_params_t *const histogram_params,
                                           const void *pixel, uint32_t *histogram, int j,
                                           const dt_iop_order_iccprofile_info_t *const profile_info)
{
  const dt_histogram_roi_t *roi = histogram_params->roi;
  float *in = (float *)pixel + 4 * (roi->width * j + roi->crop_x);

  // process aligned pixels with SSE
  for(int i = 0; i < roi->width - roi->crop_width - roi->crop_x; i++, in += 4)
  {
    if(darktable.codepath.OPENMP_SIMD)
      histogram_helper_cs_rgb_helper_process_pixel_float_compensated(histogram_params, in, histogram, profile_info);
#if defined(__SSE2__)
    else if(darktable.codepath.SSE2)
      histogram_helper_cs_rgb_helper_process_pixel_m128_compensated(histogram_params, in, histogram, profile_info);
#endif
    else
      dt_unreachable_codepath();
  }
}

//------------------------------------------------------------------------------

inline static void __attribute__((__unused__)) histogram_helper_cs_Lab_helper_process_pixel_float(
    const dt_dev_histogram_collection_params_t *const histogram_params, const float *pixel, uint32_t *histogram)
{
  const float Lv = pixel[0];
  const float av = pixel[1];
  const float bv = pixel[2];
  const float max = histogram_params->bins_count - 1;
  const uint32_t L = CLAMP(histogram_params->mul / 100.0f * (Lv), 0, max);
  const uint32_t a = CLAMP(histogram_params->mul / 256.0f * (av + 128.0f), 0, max);
  const uint32_t b = CLAMP(histogram_params->mul / 256.0f * (bv + 128.0f), 0, max);
  histogram[4 * L]++;
  histogram[4 * a + 1]++;
  histogram[4 * b + 2]++;
}

#if defined(__SSE2__)
inline static void histogram_helper_cs_Lab_helper_process_pixel_m128(
    const dt_dev_histogram_collection_params_t *const histogram_params, const float *pixel, uint32_t *histogram)
{
  const float fscale = histogram_params->mul;

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
#endif

inline static void histogram_helper_cs_Lab(const dt_dev_histogram_collection_params_t *const histogram_params,
                                           const void *pixel, uint32_t *histogram, int j,
                                           const dt_iop_order_iccprofile_info_t *const profile_info)
{
  const dt_histogram_roi_t *roi = histogram_params->roi;
  float *in = (float *)pixel + 4 * (roi->width * j + roi->crop_x);

  // process aligned pixels with SSE
  for(int i = 0; i < roi->width - roi->crop_width - roi->crop_x; i++, in += 4)
  {
    if(darktable.codepath.OPENMP_SIMD)
      histogram_helper_cs_Lab_helper_process_pixel_float(histogram_params, in, histogram);
#if defined(__SSE2__)
    else if(darktable.codepath.SSE2)
      histogram_helper_cs_Lab_helper_process_pixel_m128(histogram_params, in, histogram);
#endif
    else
      dt_unreachable_codepath();
  }
}

inline static void __attribute__((__unused__)) histogram_helper_cs_Lab_LCh_helper_process_pixel_float(
    const dt_dev_histogram_collection_params_t *const histogram_params, const float *pixel, uint32_t *histogram)
{
  dt_aligned_pixel_t LCh;
  dt_Lab_2_LCH(pixel, LCh);
  const uint32_t L = PS((LCh[0] / 100.f), histogram_params);
  const uint32_t C = PS((LCh[1] / (128.0f * sqrtf(2.0f))), histogram_params);
  const uint32_t h = PS(LCh[2], histogram_params);
  histogram[4 * L]++;
  histogram[4 * C + 1]++;
  histogram[4 * h + 2]++;
}

inline static void histogram_helper_cs_Lab_LCh(const dt_dev_histogram_collection_params_t *const histogram_params,
                                               const void *pixel, uint32_t *histogram, int j,
                                               const dt_iop_order_iccprofile_info_t *const profile_info)
{
  const dt_histogram_roi_t *roi = histogram_params->roi;
  float *in = (float *)pixel + 4 * (roi->width * j + roi->crop_x);

  // TODO: process aligned pixels with SSE
  for(int i = 0; i < roi->width - roi->crop_width - roi->crop_x; i++, in += 4)
  {
    //    if(darktable.codepath.OPENMP_SIMD)
    histogram_helper_cs_Lab_LCh_helper_process_pixel_float(histogram_params, in, histogram);
    //#if defined(__SSE2__)
    //    else if(darktable.codepath.SSE2)
    //      histogram_helper_cs_Lab_helper_process_pixel_m128(histogram_params, in, histogram);
    //#endif
    //    else
    //      dt_unreachable_codepath();
  }
}

//==============================================================================

void dt_histogram_worker(dt_dev_histogram_collection_params_t *const histogram_params,
                         dt_dev_histogram_stats_t *histogram_stats, const void *const pixel,
                         uint32_t **histogram, const dt_worker Worker,
                         const dt_iop_order_iccprofile_info_t *const profile_info)
{
  const size_t bins_total = (size_t)4 * histogram_params->bins_count;
  const size_t buf_size = bins_total * sizeof(uint32_t);
  *histogram = realloc(*histogram, buf_size);
  // hack to make reduction clause work
  uint32_t *working_hist = *histogram;
  memset(working_hist, 0, buf_size);

  if(histogram_params->mul == 0) histogram_params->mul = (double)(histogram_params->bins_count - 1);

  const dt_histogram_roi_t *const roi = histogram_params->roi;

#ifdef _OPENMP
#pragma omp parallel for default(none)                                  \
  dt_omp_firstprivate(histogram_params, pixel, Worker, profile_info, roi) \
  reduction(+:working_hist[:bins_total])                                \
  schedule(static)
#endif
  for(int j = roi->crop_y; j < roi->height - roi->crop_height; j++)
  {
    Worker(histogram_params, pixel, working_hist, j, profile_info);
  }

  histogram_stats->bins_count = histogram_params->bins_count;
  histogram_stats->pixels = (roi->width - roi->crop_width - roi->crop_x)
                            * (roi->height - roi->crop_height - roi->crop_y);
}

//------------------------------------------------------------------------------

void dt_histogram_helper(dt_dev_histogram_collection_params_t *histogram_params,
    dt_dev_histogram_stats_t *histogram_stats, const dt_iop_colorspace_type_t cst,
    const dt_iop_colorspace_type_t cst_to, const void *pixel, uint32_t **histogram,
    const int compensate_middle_grey, const dt_iop_order_iccprofile_info_t *const profile_info)
{
  dt_times_t start_time = { 0 }, end_time = { 0 };
  if(darktable.unmuted & DT_DEBUG_PERF) dt_get_times(&start_time);

  switch(cst)
  {
    case IOP_CS_RAW:
      dt_histogram_worker(histogram_params, histogram_stats, pixel, histogram, histogram_helper_cs_RAW, profile_info);
      histogram_stats->ch = 1u;
      break;

    case IOP_CS_RGB:
      if(compensate_middle_grey && profile_info)
        dt_histogram_worker(histogram_params, histogram_stats, pixel, histogram, histogram_helper_cs_rgb_compensated, profile_info);
      else
        dt_histogram_worker(histogram_params, histogram_stats, pixel, histogram, histogram_helper_cs_rgb, profile_info);
      histogram_stats->ch = 3u;
      break;

    case IOP_CS_LAB:
    default:
      if(cst_to != IOP_CS_LCH)
        dt_histogram_worker(histogram_params, histogram_stats, pixel, histogram, histogram_helper_cs_Lab, profile_info);
      else
        dt_histogram_worker(histogram_params, histogram_stats, pixel, histogram, histogram_helper_cs_Lab_LCh, profile_info);
      histogram_stats->ch = 3u;
      break;
  }

  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_get_times(&end_time);
    fprintf(stderr, "histogram calculation %d bins %d -> %d %d channels %d pixels took %.3f secs (%.3f CPU)\n",
            histogram_params->bins_count, cst, cst_to, histogram_stats->ch, histogram_stats->pixels,
            end_time.clock - start_time.clock, end_time.user - start_time.user);
  }
}

void dt_histogram_max_helper(const dt_dev_histogram_stats_t *const histogram_stats,
                             const dt_iop_colorspace_type_t cst, const dt_iop_colorspace_type_t cst_to,
                             uint32_t **histogram, uint32_t *histogram_max)
{
  if(*histogram == NULL) return;

  dt_times_t start_time = { 0 }, end_time = { 0 };
  if(darktable.unmuted & DT_DEBUG_PERF) dt_get_times(&start_time);

  histogram_max[0] = histogram_max[1] = histogram_max[2] = histogram_max[3] = 0;
  uint32_t *hist = *histogram;
  switch(cst)
  {
    case IOP_CS_RAW:
      for(int k = 0; k < 4 * histogram_stats->bins_count; k += 4)
        histogram_max[0] = histogram_max[0] > hist[k] ? histogram_max[0] : hist[k];
      break;

    case IOP_CS_RGB:
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

    case IOP_CS_LAB:
    default:
      if(cst_to == IOP_CS_LCH)
      {
        // don't count <= 0 pixels
        for(int k = 4; k < 4 * histogram_stats->bins_count; k += 4)
          histogram_max[0] = histogram_max[0] > hist[k] ? histogram_max[0] : hist[k];
        for(int k = 5; k < 4 * histogram_stats->bins_count; k += 4)
          histogram_max[1] = histogram_max[1] > hist[k] ? histogram_max[1] : hist[k];
        for(int k = 6; k < 4 * histogram_stats->bins_count; k += 4)
          histogram_max[2] = histogram_max[2] > hist[k] ? histogram_max[2] : hist[k];
        for(int k = 7; k < 4 * histogram_stats->bins_count; k += 4)
          histogram_max[3] = histogram_max[3] > hist[k] ? histogram_max[3] : hist[k];
      }
      else
      {
        // don't count <= 0 pixels in L
        for(int k = 4; k < 4 * histogram_stats->bins_count; k += 4)
          histogram_max[0] = histogram_max[0] > hist[k] ? histogram_max[0] : hist[k];

        // don't count <= -128 and >= +128 pixels in a and b
        for(int k = 5; k < 4 * (histogram_stats->bins_count - 1); k += 4)
          histogram_max[1] = histogram_max[1] > hist[k] ? histogram_max[1] : hist[k];
        for(int k = 6; k < 4 * (histogram_stats->bins_count - 1); k += 4)
          histogram_max[2] = histogram_max[2] > hist[k] ? histogram_max[2] : hist[k];
      }
      break;
  }

  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_get_times(&end_time);
    fprintf(stderr, "histogram max calc took %.3f secs (%.3f CPU)\n",
        end_time.clock - start_time.clock, end_time.user - start_time.user);
  }
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

