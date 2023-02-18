/*
    This file is part of darktable,
    Copyright (C) 2014-2023 darktable developers.

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
#include <assert.h>
#include <stdlib.h>

#include "common/colorspaces_inline_conversions.h"
#include "common/darktable.h"
#include "common/histogram.h"
#include "develop/imageop.h"

typedef void((*_histogram_worker)(const dt_dev_histogram_collection_params_t *const params,
                                  const void *const restrict pixel,
                                  uint32_t *const restrict histogram,
                                  const int j,
                                  const dt_iop_order_iccprofile_info_t *const profile));

static inline void _clamp_bin(const dt_aligned_pixel_t vals,
                              uint32_t *const restrict histogram,
                              const float max_bin)
{
  DT_ALIGNED_PIXEL size_t bin[4];
  for_each_channel(k,aligned(vals,bin:16))
  {
    // must be signed before clamping as value may be negative
    bin[k] = CLAMP(vals[k], 0.0f, max_bin);
  }

  histogram[bin[0]*4]++;
  histogram[bin[1]*4+1]++;
  histogram[bin[2]*4+2]++;
}

//------------------------------------------------------------------------------

static inline void _bin_raw(const dt_dev_histogram_collection_params_t *const params,
                            const void *pixel,
                            uint32_t *histogram,
                            const int j,
                            const dt_iop_order_iccprofile_info_t *const profile)
{
  const dt_histogram_roi_t *roi = params->roi;
  uint16_t *in = (uint16_t *)pixel + roi->width * j + roi->crop_x;
  const size_t max_bin = params->bins_count - 1;

  for(int i = 0; i < roi->width - roi->crop_right - roi->crop_x; i++)
  {
    // WARNING: you must ensure that bins_count is big enough
    // e.g. 2^16 if you expect 16 bit raw files
    histogram[MIN(in[i], max_bin)]++;
  }
}

//------------------------------------------------------------------------------

static inline void _bin_rgb(const dt_dev_histogram_collection_params_t *const params,
                            const void *const restrict pixel,
                            uint32_t *const restrict histogram,
                            const int j,
                            const dt_iop_order_iccprofile_info_t *const profile)
{
  const dt_histogram_roi_t *roi = params->roi;
  float *in = (float *)pixel + 4 * (roi->width * j + roi->crop_x);
  const float max_bin = params->bins_count - 1;

  for(int i = 0; i < roi->width - roi->crop_right - roi->crop_x; i++)
  {
    dt_aligned_pixel_t b;
    for_each_channel(k,aligned(in,b:16))
      b[k] = max_bin * in[i*4+k];
    _clamp_bin(b, histogram, max_bin);
  }
}

static inline void _bin_rgb_compensated
  (const dt_dev_histogram_collection_params_t *const params,
   const void *const pixel,
   uint32_t *const restrict histogram,
   const int j,
   const dt_iop_order_iccprofile_info_t *const profile)
{
  const dt_histogram_roi_t *roi = params->roi;
  float *in = (float *)pixel + 4 * (roi->width * j + roi->crop_x);
  const float max_bin = params->bins_count - 1;

  for(int i = 0; i < roi->width - roi->crop_right - roi->crop_x; i++)
  {
    dt_aligned_pixel_t b;
    for_each_channel(k,aligned(in,b:16))
      b[k] = max_bin * dt_ioppr_compensate_middle_grey(in[i*4+k], profile);
    _clamp_bin(b, histogram, max_bin);
  }
}

//------------------------------------------------------------------------------

static inline void _bin_Lab(const dt_dev_histogram_collection_params_t *const params,
                            const void *const restrict pixel,
                            uint32_t *const restrict histogram,
                            const int j,
                            const dt_iop_order_iccprofile_info_t *const profile)
{
  const dt_histogram_roi_t *roi = params->roi;
  float *in = (float *)pixel + 4 * (roi->width * j + roi->crop_x);
  const float max_bin = params->bins_count - 1;
  const dt_aligned_pixel_t scale = { max_bin / 100.0f,
                                     max_bin / 256.0f,
                                     max_bin / 256.0f, 0.0f };
  const dt_aligned_pixel_t shift = { 0.0f, 128.0f, 128.0f, 0.0f };

  for(int i = 0; i < roi->width - roi->crop_right - roi->crop_x; i++)
  {
    dt_aligned_pixel_t b;
    for_each_channel(k,aligned(in,b,scale,shift:16))
      b[k] = scale[k] * (in[i*4+k] + shift[k]);
    _clamp_bin(b, histogram, max_bin);
  }
}

static inline void _bin_Lab_LCh(const dt_dev_histogram_collection_params_t *const params,
                                const void *const restrict pixel,
                                uint32_t *const restrict histogram,
                                const int j,
                                const dt_iop_order_iccprofile_info_t *const profile)
{
  const dt_histogram_roi_t *roi = params->roi;
  float *in = (float *)pixel + 4 * (roi->width * j + roi->crop_x);
  const float max_bin = params->bins_count - 1;
  const dt_aligned_pixel_t scale = { max_bin / 100.0f,
                                     max_bin / (128.0f * sqrtf(2.0f)),
                                     max_bin, 0.0f };

  for(int i = 0; i < roi->width - roi->crop_right - roi->crop_x; i++)
  {
    dt_aligned_pixel_t LCh = { 0.0f, 0.0f, 0.0f };
    dt_aligned_pixel_t b;
    dt_Lab_2_LCH(in + i*4, LCh);
    for_each_channel(k,aligned(LCh,b,scale:16))
      b[k] = scale[k] * LCh[k];
    _clamp_bin(b, histogram, max_bin);
  }
}

//==============================================================================

void _hist_worker(dt_dev_histogram_collection_params_t *const histogram_params,
                  dt_dev_histogram_stats_t *histogram_stats,
                  const void *const pixel,
                  uint32_t **histogram,
                  const _histogram_worker Worker,
                  const dt_iop_order_iccprofile_info_t *const profile_info)
{
  const size_t bins_total = (size_t)(histogram_stats->ch == 1 ? 1 : 4)
    * histogram_params->bins_count;
  const size_t buf_size = bins_total * sizeof(uint32_t);
  // we allocate an aligned buffer, the caller must free it, and if
  // the caller has increased the buffer size, we hackily realloc it
  if(!(*histogram) || histogram_stats->buf_size < buf_size)
  {
    if(*histogram)
      dt_free_align(*histogram);
    *histogram = dt_alloc_align(64, buf_size);
    if(!*histogram) return;
    histogram_stats->buf_size = buf_size;
  }
  // hack to make reduction clause work
  uint32_t DT_ALIGNED_PIXEL *working_hist = *histogram;
  memset(working_hist, 0, buf_size);

  const dt_histogram_roi_t *const roi = histogram_params->roi;

#ifdef _OPENMP
#pragma omp parallel for default(none)                                  \
  dt_omp_firstprivate(histogram_params, pixel, Worker, profile_info,    \
                      roi, bins_total)                                  \
  reduction(+:working_hist[:bins_total])                                \
  schedule(static)
#endif
  for(int j = roi->crop_y; j < roi->height - roi->crop_bottom; j++)
  {
    Worker(histogram_params, pixel, working_hist, j, profile_info);
  }

  histogram_stats->bins_count = histogram_params->bins_count;
  histogram_stats->pixels = (roi->width - roi->crop_right - roi->crop_x)
                            * (roi->height - roi->crop_bottom - roi->crop_y);
}

//------------------------------------------------------------------------------

void dt_histogram_helper(dt_dev_histogram_collection_params_t *histogram_params,
                         dt_dev_histogram_stats_t *histogram_stats,
                         const dt_iop_colorspace_type_t cst,
                         const dt_iop_colorspace_type_t cst_to,
                         const void *pixel,
                         uint32_t **histogram, uint32_t *histogram_max,
                         const gboolean compensate_middle_grey,
                         const dt_iop_order_iccprofile_info_t *const profile_info)
{
  dt_times_t start_time = { 0 }, end_time = { 0 };
  if(darktable.unmuted & DT_DEBUG_PERF) dt_get_times(&start_time);

  // all use 256 bins excepting:
  // levels in automatic mode which uses 16384
  // exposure deflicker uses 65536 (assumes maximum raw bit depth is 16)
  switch(cst)
  {
    case IOP_CS_RAW:
      histogram_stats->ch = 1u;
      // for exposure auto/deflicker of 16-bit int raws
      _hist_worker(histogram_params, histogram_stats, pixel, histogram,
                   _bin_raw, profile_info);
      break;

    case IOP_CS_RGB:
      histogram_stats->ch = 3u;
      if(compensate_middle_grey && profile_info)
        // for rgbcurve (compensated)
        _hist_worker(histogram_params, histogram_stats, pixel, histogram,
                     _bin_rgb_compensated, profile_info);
      else
        // used by levels, rgbcurve (uncompensated), rgblevels
        _hist_worker(histogram_params, histogram_stats, pixel, histogram,
                     _bin_rgb, profile_info);
      break;

    case IOP_CS_LAB:
      histogram_stats->ch = 3u;
      if(cst_to != IOP_CS_LCH)
        // for tonecurve
        _hist_worker(histogram_params, histogram_stats, pixel, histogram,
                     _bin_Lab, profile_info);
      else
        // for colorzones
        _hist_worker(histogram_params, histogram_stats, pixel, histogram,
                     _bin_Lab_LCh, profile_info);
      break;

    default:
      dt_unreachable_codepath();
  }

  // now, if requested, calculate maximum of each channel
  DT_ALIGNED_PIXEL uint32_t m[4] = { 0u, 0u, 0u, 0u };
  if(*histogram && histogram_max)
  {
    // RGB, Lab, and LCh
    if(cst == IOP_CS_RGB || IOP_CS_LAB)
    {
      uint32_t *hist = *histogram;

      // don't count <= 0 pixels related to lightness (RGB, L from
      // Lab, C from LCh) but we're fine counting zero chroma values
      // (ab from Lab, h from LCh)
      if(cst == IOP_CS_LAB)
      {
        if(cst_to != IOP_CS_LCH)
          m[1] = hist[1];
        m[2] = hist[2];
      }

      for(int k = 4; k < 4 * histogram_stats->bins_count; k += 4)
        for_each_channel(ch,aligned(hist:64) aligned(m:16))
          m[ch] = MAX(m[ch], hist[k+ch]);
    }
    else
      // raw max not implemented, as is only seen in exposure
      // deflicker, and in that case we don't use maximums
      dt_unreachable_codepath();
  }

  if(histogram_max)
    for_each_channel(ch,aligned(m:16))
      histogram_max[ch] = m[ch];

  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_get_times(&end_time);
    fprintf(stderr,
            "histogram calculation %u bins %d -> %d"
            " compensate %d %u channels %u pixels took %.3f secs (%.3f CPU)\n",
            histogram_params->bins_count, cst, cst_to,
            compensate_middle_grey && profile_info, histogram_stats->ch,
            histogram_stats->pixels,
            end_time.clock - start_time.clock, end_time.user - start_time.user);
  }
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
