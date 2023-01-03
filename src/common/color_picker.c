/*
    This file is part of darktable,
    Copyright (C) 2016-2022 darktable developers.

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

#include "common/color_picker.h"
#include "common/bspline.h"
#include "common/darktable.h"
#include "common/colorspaces_inline_conversions.h"
#include "common/iop_profile.h"
#include "develop/format.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"

static inline size_t _box_size(const int *const box)
{
  return (size_t)((box[3] - box[1]) * (box[2] - box[0]));
}

// define custom reduction operations to handle pixel stats/weights
// we can't return an array from a function, so wrap the array type in a struct
// FIXME: use lib_colorpicker_sample_statistics instead? or do we need a locally allocated version of the data for the sake of speed?
typedef struct _stats_pixel {
  dt_aligned_pixel_t acc, min, max;
} _stats_pixel;
typedef struct _count_pixel { DT_ALIGNED_PIXEL uint32_t v[4]; } _count_pixel;

// custom reductions make Xcode 11.3.1 compiler crash?
#if defined(__apple_build_version__) && __apple_build_version__ < 11030000
#define _CUSTOM_REDUCTIONS 0
#else
#define _CUSTOM_REDUCTIONS 1
#endif

#if defined(_OPENMP) && _CUSTOM_REDUCTIONS

static inline _stats_pixel _reduce_stats(_stats_pixel stats, _stats_pixel newval)
{
  for_four_channels(c)
  {
    stats.acc[c] += newval.acc[c];
    stats.min[c] = MIN(stats.min[c], newval.min[c]);
    stats.max[c] = MAX(stats.max[c], newval.max[c]);
  }
  return stats;
}
#pragma omp declare reduction(vstats:_stats_pixel:omp_out=_reduce_stats(omp_out,omp_in)) \
  initializer(omp_priv = omp_orig)

static inline _count_pixel _add_counts(_count_pixel acc, _count_pixel newval)
{
  for_each_channel(c)
    acc.v[c] += newval.v[c];
  return acc;
}
#pragma omp declare reduction(vsum:_count_pixel:omp_out=_add_counts(omp_out,omp_in)) \
  initializer(omp_priv = omp_orig)

#endif

#ifdef _OPENMP
#pragma omp declare simd aligned(rgb, JzCzhz: 16) uniform(profile)
#endif
static inline void rgb_to_JzCzhz(const dt_aligned_pixel_t rgb, dt_aligned_pixel_t JzCzhz,
                                 const dt_iop_order_iccprofile_info_t *const profile)
{
  dt_aligned_pixel_t XYZ_D65 = { 0.0f, 0.0f, 0.0f };
  dt_aligned_pixel_t JzAzBz = { 0.0f, 0.0f, 0.0f };

  if(profile)
  {
    dt_aligned_pixel_t XYZ_D50 = { 0.0f, 0.0f, 0.0f };
    dt_ioppr_rgb_matrix_to_xyz(rgb, XYZ_D50, profile->matrix_in_transposed, profile->lut_in, profile->unbounded_coeffs_in,
                               profile->lutsize, profile->nonlinearlut);
    dt_XYZ_D50_2_XYZ_D65(XYZ_D50, XYZ_D65);
  }
  else
  {
    // This should not happen (we don't know what RGB is), but use this when profile is not defined
    dt_XYZ_D50_2_XYZ_D65(rgb, XYZ_D65);
  }

  dt_XYZ_2_JzAzBz(XYZ_D65, JzAzBz);
  dt_JzAzBz_2_JzCzhz(JzAzBz, JzCzhz);
}

// FIXME: set these up as helpers as in common histogram code?
static inline void _color_picker_rgb_or_lab(_stats_pixel *const stats,
                                            const float *const pixels, const size_t width)
{
  for(size_t i = 0; i < width; i += 4)
    for_each_channel(k, aligned(pixels:16))
    {
      const float v = pixels[i + k];
      stats->acc[k] += v;
      stats->min[k] = MIN(stats->min[k], v);
      stats->max[k] = MAX(stats->max[k], v);
    }
}

static inline void _color_picker_lch(_stats_pixel *const stats,
                                     const float *const pixels, const size_t width)
{
  for(size_t i = 0; i < width; i += 4)
  {
    dt_aligned_pixel_t pick;
    dt_Lab_2_LCH(pixels + i, pick);
    pick[3] = pick[2] < 0.5f ? pick[2] + 0.5f : pick[2] - 0.5f;
    for_four_channels(k, aligned(pixels:16))
    {
      stats->acc[k] += pick[k];
      stats->min[k] = MIN(stats->min[k], pick[k]);
      stats->max[k] = MAX(stats->max[k], pick[k]);
    }
  }
}

static inline void _color_picker_hsl(_stats_pixel *const stats,
                                     const float *const pixels, const size_t width)
{
  for(size_t i = 0; i < width; i += 4)
  {
    dt_aligned_pixel_t pick;
    dt_RGB_2_HSL(pixels + i, pick);
    pick[3] = pick[0] < 0.5f ? pick[0] + 0.5f : pick[0] - 0.5f;
    for_four_channels(k, aligned(pixels:16))
    {
      stats->acc[k] += pick[k];
      stats->min[k] = MIN(stats->min[k], pick[k]);
      stats->max[k] = MAX(stats->max[k], pick[k]);
    }
  }
}

static inline void _color_picker_jzczhz(_stats_pixel *const stats,
                                        const float *const pixels, const size_t width,
                                        const dt_iop_order_iccprofile_info_t *const profile)
{
  for(size_t i = 0; i < width; i += 4)
  {
    dt_aligned_pixel_t pick;
    rgb_to_JzCzhz(pixels + i, pick, profile);
    pick[3] = pick[2] < 0.5f ? pick[2] + 0.5f : pick[2] - 0.5f;
    for_four_channels(k, aligned(pixels:16))
    {
      stats->acc[k] += pick[k];
      stats->min[k] = MIN(stats->min[k], pick[k]);
      stats->max[k] = MAX(stats->max[k], pick[k]);
    }
  }
}

static void color_picker_helper_4ch(const float *const pixel, const dt_iop_roi_t *const roi, const int *const box,
                                    lib_colorpicker_stats pick,
                                    const dt_iop_colorspace_type_t cst_from,
                                    const dt_iop_colorspace_type_t cst_to,
                                    const dt_iop_order_iccprofile_info_t *const profile)
{
  const int width = roi->width;

  const size_t size = _box_size(box);
  const size_t stride = 4 * (size_t)(box[2] - box[0]);
  const size_t off_mul = 4 * width;
  const size_t off_add = 4 * box[0];

  _stats_pixel stats = { .acc = { 0.0f, 0.0f, 0.0f, 0.0f },
                         .min = { FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX },
                         .max = { -FLT_MAX, -FLT_MAX, -FLT_MAX, -FLT_MAX } };

  // cutoffs for using threads depends on # of samples and complexity
  // of the colorspace conversion
  // FIXME: will this run faster if we use collapse(2)? will this take rejiggering of function calls?
  if(cst_from == IOP_CS_LAB && cst_to == IOP_CS_LCH)
  {
#if defined(_OPENMP) && _CUSTOM_REDUCTIONS
#pragma omp parallel for default(none) if (size > 500)                  \
  dt_omp_firstprivate(pixel, stride, off_mul, off_add, box)             \
  reduction(vstats : stats) schedule(static)
#endif
    for(size_t j = box[1]; j < box[3]; j++)
    {
      const size_t offset = j * off_mul + off_add;
      _color_picker_lch(&stats, pixel + offset, stride);
    }
  }
  else if(cst_from == IOP_CS_RGB && cst_to == IOP_CS_HSL)
  {
#if defined(_OPENMP) && _CUSTOM_REDUCTIONS
#pragma omp parallel for default(none) if (size > 250)                  \
  dt_omp_firstprivate(pixel, stride, off_mul, off_add, box)             \
  reduction(vstats : stats) schedule(static)
#endif
    for(size_t j = box[1]; j < box[3]; j++)
    {
      const size_t offset = j * off_mul + off_add;
      _color_picker_hsl(&stats, pixel + offset, stride);
    }
  }
  else if(cst_from == IOP_CS_RGB && cst_to == IOP_CS_JZCZHZ)
  {
#if defined(_OPENMP) && _CUSTOM_REDUCTIONS
#pragma omp parallel for default(none) if (size > 100)                  \
  dt_omp_firstprivate(pixel, stride, off_mul, off_add, box, profile)    \
  reduction(vstats : stats) schedule(static)
#endif
    for(size_t j = box[1]; j < box[3]; j++)
    {
      const size_t offset = j * off_mul + off_add;
      _color_picker_jzczhz(&stats, pixel + offset, stride, profile);
    }
  }
  else
  {
    // fallback, better than crashing as happens with monochromes
    if(cst_from != cst_to && cst_to != IOP_CS_NONE)
      dt_print(DT_DEBUG_DEV, "[color_picker_helper_4ch_parallel] unknown colorspace conversion from %d to %d\n", cst_from, cst_to);

#if defined(_OPENMP) && _CUSTOM_REDUCTIONS
#pragma omp parallel for default(none) if (size > 1000)                 \
  dt_omp_firstprivate(pixel, stride, off_mul, off_add, box)             \
  reduction(vstats : stats) schedule(static)
#endif
    for(size_t j = box[1]; j < box[3]; j++)
    {
      const size_t offset = j * off_mul + off_add;
      _color_picker_rgb_or_lab(&stats, pixel + offset, stride);
    }
  }

  // copy all four channels, as four some colorspaces there may be
  // meaningful data in the fourth pixel
  for_four_channels(c)
  {
    pick[DT_PICK_MEAN][c] = stats.acc[c] / (float)size;
    pick[DT_PICK_MIN][c] = stats.min[c];
    pick[DT_PICK_MAX][c] = stats.max[c];
  }
}

static void color_picker_helper_bayer(const dt_iop_buffer_dsc_t *const dsc, const float *const pixel,
                                      const dt_iop_roi_t *const roi, const int *const box,
                                      lib_colorpicker_stats pick)
{
  const int width = roi->width;
  const uint32_t filters = dsc->filters;

  _count_pixel weights = { { 0u, 0u, 0u, 0u } };
  _stats_pixel stats = { .acc = { 0.0f, 0.0f, 0.0f, 0.0f },
                         .min = { FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX },
                         .max = { -FLT_MAX, -FLT_MAX, -FLT_MAX, -FLT_MAX } };

  // cutoff for using threads depends on # of samples
#if defined(_OPENMP) && _CUSTOM_REDUCTIONS
#pragma omp parallel for default(none) if (_box_size(box) > 25000)      \
  dt_omp_firstprivate(pixel, width, roi, filters, box)                  \
  reduction(vstats : stats) reduction(vsum : weights)                   \
  schedule(static)
#endif
  for(size_t j = box[1]; j < box[3]; j++)
    for(size_t i = box[0]; i < box[2]; i++)
    {
      const int c = FC(j + roi->y, i + roi->x, filters);
      const float px = pixel[width * j + i];
      stats.acc[c] += px;
      stats.min[c] = MIN(stats.min[c], px);
      stats.max[c] = MAX(stats.max[c], px);
      weights.v[c]++;
    }

  copy_pixel(pick[DT_PICK_MIN], stats.min);
  copy_pixel(pick[DT_PICK_MAX], stats.max);
  // and finally normalize data. For bayer, there is twice as much green.
  for_each_channel(c)
    pick[DT_PICK_MEAN][c] =
      weights.v[c] ? (stats.acc[c] / (float)weights.v[c]) : 0.0f;
}

static void color_picker_helper_xtrans(const dt_iop_buffer_dsc_t *const dsc, const float *const pixel,
                                       const dt_iop_roi_t *const roi, const int *const box,
                                       lib_colorpicker_stats pick)
{
  const int width = roi->width;
  const uint8_t(*const xtrans)[6] = (const uint8_t(*const)[6])dsc->xtrans;

  _count_pixel weights = { { 0u, 0u, 0u, 0u } };
  _stats_pixel stats = { .acc = { 0.0f, 0.0f, 0.0f, 0.0f },
                         .min = { FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX },
                         .max = { -FLT_MAX, -FLT_MAX, -FLT_MAX, -FLT_MAX } };

  // cutoff for using threads depends on # of samples
#if defined(_OPENMP) && _CUSTOM_REDUCTIONS
#pragma omp parallel for default(none) if (_box_size(box) > 20000)      \
  dt_omp_firstprivate(pixel, width, roi, xtrans, box)                   \
  reduction(vstats : stats) reduction(vsum : weights)                   \
  schedule(static)
#endif
  for(size_t j = box[1]; j < box[3]; j++)
    for(size_t i = box[0]; i < box[2]; i++)
    {
      const int c = FCxtrans(j, i, roi, xtrans);
      const float px = pixel[width * j + i];
      stats.acc[c] += px;
      stats.min[c] = MIN(stats.min[c], px);
      stats.max[c] = MAX(stats.max[c], px);
      weights.v[c]++;
    }

  copy_pixel(pick[DT_PICK_MIN], stats.min);
  copy_pixel(pick[DT_PICK_MAX], stats.max);
  // and finally normalize data.
  // X-Trans RGB weighting averages to 2:5:2 for each 3x3 cell
  for_each_channel(c)
    pick[DT_PICK_MEAN][c] =
      weights.v[c] ? (stats.acc[c] / (float)weights.v[c]) : 0.0f;
}

// picked_color, picked_color_min and picked_color_max should be aligned
void dt_color_picker_helper(const dt_iop_buffer_dsc_t *dsc, const float *const pixel,
                            const dt_iop_roi_t *roi, const int *const box,
                            lib_colorpicker_stats pick,
                            const dt_iop_colorspace_type_t image_cst,
                            const dt_iop_colorspace_type_t picker_cst,
                            const dt_iop_order_iccprofile_info_t *const profile)
{
  dt_times_t start_time = { 0 }, end_time = { 0 };
  if(darktable.unmuted & DT_DEBUG_PERF) dt_get_times(&start_time);

  if(dsc->channels == 4u)
  {
    // Denoise the image
    size_t padded_size;
    float *const restrict denoised = dt_alloc_align_float(4 * roi->width * roi->height);
    float *const DT_ALIGNED_ARRAY tempbuf = dt_alloc_perthread_float(4 * roi->width, &padded_size); //TODO: alloc in caller

    // blur without clipping negatives because Lab a and b channels can be legitimately negative
    // FIXME: this blurs whole image even when just a bit is sampled
    // FIXME: if multiple samples are made, the blur is called each time -- instead if this is to even happen outside of per-module, do this once
    // FIXME: if this is done in pixelpipe, we should have a spare buffer (output) to write this into, hence can skip the alloc above, and all do this on the input to filmic
    blur_2D_Bspline(pixel, denoised, tempbuf, roi->width, roi->height, 1, FALSE);

    color_picker_helper_4ch(denoised, roi, box, pick, image_cst, picker_cst, profile);

    dt_free_align(denoised);
    dt_free_align(tempbuf);
  }
  else if(dsc->channels == 1u && dsc->filters != 0u && dsc->filters != 9u)
  {
    color_picker_helper_bayer(dsc, pixel, roi, box, pick);
  }
  else if(dsc->channels == 1u && dsc->filters == 9u)
  {
    color_picker_helper_xtrans(dsc, pixel, roi, box, pick);
  }
  else
    dt_unreachable_codepath();

  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_get_times(&end_time);
    fprintf(stderr, "colorpicker stats reading took %.3f secs (%.3f CPU)\n",
        end_time.clock - start_time.clock, end_time.user - start_time.user);
  }
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
