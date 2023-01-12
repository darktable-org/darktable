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

typedef DT_ALIGNED_PIXEL uint32_t dt_aligned_uint32_t[4];

static inline uint32_t bin(const float v,
                           const dt_dev_histogram_collection_params_t *const params)
{
  const float scaled = params->mul * v;
  return CLAMP((uint32_t)scaled, 0, params->bins_count - 1);
}

static inline void clamp_and_bin(const dt_aligned_pixel_t vals, uint32_t *histogram,
                                 // FIXME: does it matterif this is uint32_t?
                                 const uint32_t max_bin)
{
  dt_aligned_uint32_t bnum;
  for_each_channel(k,aligned(vals,bnum:16))
    bnum[k] = CLAMP((uint32_t)vals[k], 0, max_bin);
  histogram[4 * bnum[0]]++;
  histogram[4 * bnum[1] + 1]++;
  histogram[4 * bnum[2] + 2]++;
}

//------------------------------------------------------------------------------

// FIXME: do we ever need histograms of float raw files?
inline static void histogram_helper_cs_RAW(const dt_dev_histogram_collection_params_t *const histogram_params,
                                           const void *pixel, uint32_t *histogram, int j,
                                           const dt_iop_order_iccprofile_info_t *const profile_info)
{
  const dt_histogram_roi_t *roi = histogram_params->roi;
  const float *input = (float *)pixel + roi->width * j + roi->crop_x;
  for(int i = 0; i < roi->width - roi->crop_width - roi->crop_x; i++, input++)
    histogram[4 * bin(*input, histogram_params)]++;
}

//------------------------------------------------------------------------------

inline void dt_histogram_helper_cs_RAW_uint16(const dt_dev_histogram_collection_params_t *const histogram_params,
                                              const void *pixel, uint32_t *histogram, int j,
                                              const dt_iop_order_iccprofile_info_t *const profile_info)
{
  const dt_histogram_roi_t *roi = histogram_params->roi;
  uint16_t *in = (uint16_t *)pixel + roi->width * j + roi->crop_x;

  // process pixels
  for(int i = 0; i < roi->width - roi->crop_width - roi->crop_x; i++, in++)
  {
    // WARNING: you must ensure that bins_count is big enough
    const uint16_t binned = MIN(*in, histogram_params->bins_count - 1);
    histogram[4 * binned]++;
  }
}

//------------------------------------------------------------------------------

inline static void histogram_helper_cs_rgb(const dt_dev_histogram_collection_params_t *const histogram_params,
                                           const void *pixel, uint32_t *histogram, int j,
                                           const dt_iop_order_iccprofile_info_t *const profile_info)
{
  const dt_histogram_roi_t *roi = histogram_params->roi;
  float *in = (float *)pixel + 4 * (roi->width * j + roi->crop_x);

  for(int i = 0; i < roi->width - roi->crop_width - roi->crop_x; i++)
  {
    dt_aligned_pixel_t b;
    for_each_channel(k,aligned(in,b:16))
      b[k] = histogram_params->mul * in[i*4+k];
    clamp_and_bin(b, histogram, histogram_params->bins_count - 1);
  }
}

inline static void histogram_helper_cs_rgb_compensated(const dt_dev_histogram_collection_params_t *const histogram_params,
                                           const void *pixel, uint32_t *histogram, int j,
                                           const dt_iop_order_iccprofile_info_t *const profile_info)
{
  const dt_histogram_roi_t *roi = histogram_params->roi;
  float *in = (float *)pixel + 4 * (roi->width * j + roi->crop_x);

  for(int i = 0; i < roi->width - roi->crop_width - roi->crop_x; i++)
  {
    dt_aligned_pixel_t b;
    for_each_channel(k,aligned(in,b:16))
      b[k] = histogram_params->mul *
        dt_ioppr_compensate_middle_grey(in[i*4+k], profile_info);
    clamp_and_bin(b, histogram, histogram_params->bins_count - 1);
  }
}

//------------------------------------------------------------------------------

static inline void histogram_helper_cs_Lab(const dt_dev_histogram_collection_params_t *const histogram_params,
                                           const void *pixel, uint32_t *histogram, int j,
                                           const dt_iop_order_iccprofile_info_t *const profile_info)
{
  const dt_histogram_roi_t *roi = histogram_params->roi;
  float *in = (float *)pixel + 4 * (roi->width * j + roi->crop_x);
  const dt_aligned_pixel_t scale = { histogram_params->mul / 100.0f,
                                     histogram_params->mul / 256.0f,
                                     histogram_params->mul / 256.0f, 0.0f };
  const dt_aligned_pixel_t shift = { 0.0f, 128.0f, 128.0f, 0.0f };

  for(int i = 0; i < roi->width - roi->crop_width - roi->crop_x; i++)
  {
    dt_aligned_pixel_t b;
    for_each_channel(k,aligned(in,b,scale,shift:16))
      b[k] = scale[k] * (in[i*4+k] + shift[k]);
    clamp_and_bin(b, histogram, histogram_params->bins_count - 1);
  }
}

static inline void histogram_helper_cs_Lab_LCh(const dt_dev_histogram_collection_params_t *const histogram_params,
                                               const void *pixel, uint32_t *histogram, int j,
                                               const dt_iop_order_iccprofile_info_t *const profile_info)
{
  const dt_histogram_roi_t *roi = histogram_params->roi;
  float *in = (float *)pixel + 4 * (roi->width * j + roi->crop_x);
  const dt_aligned_pixel_t scale = { histogram_params->mul / 100.0f,
                                     histogram_params->mul / (128.0f * sqrtf(2.0f)),
                                     histogram_params->mul, 0.0f };

  for(int i = 0; i < roi->width - roi->crop_width - roi->crop_x; i++)
  {
    dt_aligned_pixel_t LCh, b;
    dt_Lab_2_LCH(in + i*4, LCh);
    for_each_channel(k,aligned(LCh,b,scale:16))
      b[k] = scale[k] * LCh[k];
    clamp_and_bin(b, histogram, histogram_params->bins_count - 1);
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
  // hack to make histogram buffer always aligned
  // FIXME: should remember its size and only realloc if it has cahnged
  dt_free_align(*histogram);
  *histogram = dt_alloc_align(16, buf_size);
  // hack to make reduction clause work
  uint32_t *working_hist = *histogram;
  memset(working_hist, 0, buf_size);

  if(histogram_params->mul == 0)
    histogram_params->mul = (double)(histogram_params->bins_count - 1);

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
    fprintf(stderr, "histogram calculation %d bins %d -> %d compensate %d %d channels %d pixels took %.3f secs (%.3f CPU)\n",
            histogram_params->bins_count, cst, cst_to, compensate_middle_grey && profile_info, histogram_stats->ch, histogram_stats->pixels,
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

