/*
    This file is part of darktable,
    Copyright (C) 2014-2026 darktable developers.

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
#include <stdlib.h>

#include "common/colorspaces_inline_conversions.h"
#include "common/darktable.h"
#include "common/histogram.h"
#include "common/opencl.h"
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
                                     max_bin / (128.0f * M_SQRT2_F),
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

static void _hist_worker(dt_dev_histogram_collection_params_t *const histogram_params,
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
    *histogram = dt_alloc_aligned(buf_size);
    if(!*histogram) return;
    histogram_stats->buf_size = buf_size;
  }
  // hack to make reduction clause work
  uint32_t DT_ALIGNED_PIXEL *working_hist = *histogram;
  memset(working_hist, 0, buf_size);

  const dt_histogram_roi_t *const roi = histogram_params->roi;

  DT_OMP_FOR(reduction(+:working_hist[:bins_total]))
  for(int j = roi->crop_y; j < roi->height - roi->crop_bottom; j++)
  {
    Worker(histogram_params, pixel, working_hist, j, profile_info);
  }

  histogram_stats->bins_count = histogram_params->bins_count;
  histogram_stats->pixels = (roi->width - roi->crop_right - roi->crop_x)
                            * (roi->height - roi->crop_bottom - roi->crop_y);
}

//------------------------------------------------------------------------------

// the maximum of each channel, if requested
static void _histogram_max(const dt_dev_histogram_stats_t *histogram_stats,
                           const dt_iop_colorspace_type_t cst,
                           const dt_iop_colorspace_type_t cst_to,
                           const uint32_t *const histogram,
                           uint32_t *histogram_max)
{
  DT_ALIGNED_PIXEL uint32_t m[4] = { 0u, 0u, 0u, 0u };
  if(histogram && histogram_max)
  {
    // RGB, Lab, and LCh
    if(cst == IOP_CS_RGB || IOP_CS_LAB)
    {
      const uint32_t *hist = histogram;

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
}

void dt_histogram_helper(dt_dev_histogram_collection_params_t *histogram_params,
                         dt_dev_histogram_stats_t *histogram_stats,
                         const dt_iop_colorspace_type_t cst,
                         const dt_iop_colorspace_type_t cst_to,
                         const void *pixel,
                         uint32_t **histogram,
                         uint32_t *histogram_max,
                         const gboolean compensate_middle_grey,
                         const dt_iop_order_iccprofile_info_t *const profile_info)
{
  dt_times_t start_time = { 0 };
  dt_get_perf_times(&start_time);

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

    case IOP_CS_XYZ:
      histogram_stats->ch = 3u;
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

  _histogram_max(histogram_stats, cst, cst_to, *histogram, histogram_max);

  dt_print(DT_DEBUG_PERF,
            "histogram calculation %u bins %d -> %d"
            " compensate %d %u channels %u pixels took %.3f secs (%.3f CPU)",
            histogram_params->bins_count, cst, cst_to,
            compensate_middle_grey && profile_info, histogram_stats->ch,
            histogram_stats->pixels,
            dt_get_lap_time(&start_time.clock), dt_get_lap_utime(&start_time.user));
}

#ifdef HAVE_OPENCL
dt_histogram_cl_global_t *dt_histogram_init_cl_global(void)
{
  dt_histogram_cl_global_t *g = malloc(sizeof(dt_histogram_cl_global_t));
  if(!g) return NULL;
  const int program = 2; // basic.cl, from programs.conf
  g->kernel_histogram_collect = dt_opencl_create_kernel(program, "histogram_collect");
  return g;
}

void dt_histogram_free_cl_global(dt_histogram_cl_global_t *g)
{
  if(!g) return;
  dt_opencl_free_kernel(g->kernel_histogram_collect);
  free(g);
}

gboolean dt_histogram_helper_cl(const int devid,
                                dt_dev_histogram_collection_params_t *histogram_params,
                                dt_dev_histogram_stats_t *histogram_stats,
                                const dt_iop_colorspace_type_t cst,
                                const dt_iop_colorspace_type_t cst_to,
                                cl_mem img,
                                uint32_t **histogram,
                                uint32_t *histogram_max,
                                const gboolean compensate_middle_grey,
                                const dt_iop_order_iccprofile_info_t *const profile_info)
{
  const dt_histogram_cl_global_t *gd = darktable.opencl->histogram;
  if(!gd) return FALSE;

  // raw histograms count the 16-bit integers of the input
  int mode;
  if(cst == IOP_CS_RGB)
    mode = compensate_middle_grey && profile_info ? 1 : 0;
  else if(cst == IOP_CS_LAB)
    mode = cst_to != IOP_CS_LCH ? 2 : 3;
  else
    return FALSE;

  dt_times_t start_time = { 0 };
  dt_get_perf_times(&start_time);

  const dt_histogram_roi_t *const roi = histogram_params->roi;
  const int x0 = roi->crop_x;
  const int y0 = roi->crop_y;
  const int x1 = roi->width - roi->crop_right;
  const int y1 = roi->height - roi->crop_bottom;
  if(x1 <= x0 || y1 <= y0) return FALSE;
  const int width = x1 - x0;
  const int height = y1 - y0;
  const int bins_count = histogram_params->bins_count;
  const size_t buf_size = (size_t)4 * bins_count * sizeof(uint32_t);

  // a work group counts into local memory first, unless the bins don't
  // fit there, as for the 16384 of levels in automatic mode
  const size_t local_size = (size_t)3 * bins_count * sizeof(uint32_t);
  dt_opencl_local_buffer_t locopt
    = (dt_opencl_local_buffer_t){ .xoffset = 0, .xfactor = 1, .yoffset = 0, .yfactor = 1,
                                  .cellsize = 0, .overhead = local_size,
                                  .sizex = 1 << 4, .sizey = 1 << 4 };
  const int use_local = bins_count <= 1024
    && dt_opencl_local_buffer_opt(devid, gd->kernel_histogram_collect, &locopt) == CL_SUCCESS;

  dt_colorspaces_iccprofile_info_cl_t *profile_info_cl = NULL;
  cl_float *profile_lut_cl = NULL;
  cl_mem dev_profile_info = NULL;
  cl_mem dev_profile_lut = NULL;
  cl_mem dev_bins = NULL;

  cl_int err = dt_ioppr_build_iccprofile_params_cl(mode == 1 ? profile_info : NULL, devid,
                                                   &profile_info_cl, &profile_lut_cl,
                                                   &dev_profile_info, &dev_profile_lut);
  if(err != CL_SUCCESS) goto error;

  // the buffer as in _hist_worker(), which the caller frees
  if(!(*histogram) || histogram_stats->buf_size < buf_size)
  {
    dt_free_align(*histogram);
    *histogram = dt_alloc_aligned(buf_size);
    if(!*histogram)
    {
      err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
      goto error;
    }
    histogram_stats->buf_size = buf_size;
  }
  memset(*histogram, 0, buf_size);

  dev_bins = dt_opencl_alloc_device_buffer(devid, buf_size);
  if(!dev_bins)
  {
    err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
    goto error;
  }
  err = dt_opencl_write_buffer_to_device(devid, *histogram, dev_bins, 0, buf_size, FALSE);
  if(err != CL_SUCCESS) goto error;

  if(use_local)
  {
    const size_t sizes[2] = { ROUNDUP(width, locopt.sizex), ROUNDUP(height, locopt.sizey) };
    const size_t local[2] = { locopt.sizex, locopt.sizey };
    err = dt_opencl_enqueue_kernel_2d_local_args(devid, gd->kernel_histogram_collect, sizes, local,
            CLARG(img), CLARG(x0), CLARG(y0), CLARG(x1), CLARG(y1), CLARG(bins_count), CLARG(mode),
            CLARG(use_local), CLARG(dev_profile_info), CLARG(dev_profile_lut), CLARG(dev_bins),
            CLLOCAL(local_size));
  }
  else
  {
    err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_histogram_collect, width, height,
            CLARG(img), CLARG(x0), CLARG(y0), CLARG(x1), CLARG(y1), CLARG(bins_count), CLARG(mode),
            CLARG(use_local), CLARG(dev_profile_info), CLARG(dev_profile_lut), CLARG(dev_bins),
            CLLOCAL(sizeof(uint32_t)));
  }
  if(err != CL_SUCCESS) goto error;

  err = dt_opencl_read_buffer_from_device(devid, *histogram, dev_bins, 0, buf_size, TRUE);
  if(err != CL_SUCCESS) goto error;

  dt_opencl_release_mem_object(dev_bins);
  dt_ioppr_free_iccprofile_params_cl(&profile_info_cl, &profile_lut_cl, &dev_profile_info, &dev_profile_lut);

  histogram_stats->ch = 3u;
  histogram_stats->bins_count = bins_count;
  histogram_stats->pixels = width * height;
  _histogram_max(histogram_stats, cst, cst_to, *histogram, histogram_max);

  dt_print(DT_DEBUG_PERF,
           "histogram calculation on the OpenCL device %u bins %d -> %d compensate %d %u pixels took %.3f secs",
           bins_count, cst, cst_to, mode == 1, histogram_stats->pixels, dt_get_lap_time(&start_time.clock));
  return TRUE;

error:
  dt_print(DT_DEBUG_OPENCL, "[dt_histogram_helper_cl] couldn't count on the device: %s", cl_errstr(err));
  dt_opencl_release_mem_object(dev_bins);
  dt_ioppr_free_iccprofile_params_cl(&profile_info_cl, &profile_lut_cl, &dev_profile_info, &dev_profile_lut);
  return FALSE;
}
#endif

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
