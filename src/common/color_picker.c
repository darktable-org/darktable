/*
    This file is part of darktable,
    Copyright (C) 2016-2023 darktable developers.

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

static inline void _update_stats_1ch(_stats_pixel *const stats,
                                     const int ch, const float pick)
{
  stats->acc[ch] += pick;
  stats->min[ch] = MIN(stats->min[ch], pick);
  stats->max[ch] = MAX(stats->max[ch], pick);
}

static inline void _update_stats_4ch(_stats_pixel *const stats,
                                     const dt_aligned_pixel_t pick)
{
  // FIXME: if non Lab/RGB only use three channels, can use for_each_channel()
  for_four_channels(k)
    _update_stats_1ch(stats, k, pick[k]);
}

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

typedef void((*picker_worker_4ch)(_stats_pixel *const stats,
                                  const float *const pixels, const size_t width,
                                  // FIXME: could be gconstpointer
                                  const void *const data));
typedef void((*picker_worker_1ch)(_stats_pixel *const stats,
                                  _count_pixel *const weights,
                                  const float *const pixels,
                                  const size_t j,
                                  const dt_iop_roi_t *const roi,
                                  const int *const box,
                                  // FIXME: could be gconstpointer
                                  const void *const data));

static inline void _color_picker_rgb_or_lab(_stats_pixel *const stats,
                                            const float *const pixels, const size_t width,
                                            const void *const data)
{
  for(size_t i = 0; i < width; i += 4)
    for_each_channel(k, aligned(pixels:16))
      _update_stats_1ch(stats, k, pixels[i + k]);
}

static inline void _color_picker_lch(_stats_pixel *const stats,
                                     const float *const pixels, const size_t width,
                                     const void *const data)
{
  for(size_t i = 0; i < width; i += 4)
  {
    dt_aligned_pixel_t pick;
    dt_Lab_2_LCH(pixels + i, pick);
    // FIXME: is this used?
    pick[3] = pick[2] < 0.5f ? pick[2] + 0.5f : pick[2] - 0.5f;
    _update_stats_4ch(stats, pick);
  }
}

static inline void _color_picker_hsl(_stats_pixel *const stats,
                                     const float *const pixels, const size_t width,
                                     const void *const data)
{
  for(size_t i = 0; i < width; i += 4)
  {
    dt_aligned_pixel_t pick;
    dt_RGB_2_HSL(pixels + i, pick);
    // FIXME: is this used?
    pick[3] = pick[0] < 0.5f ? pick[0] + 0.5f : pick[0] - 0.5f;
    _update_stats_4ch(stats, pick);
  }
}

static inline void _color_picker_jzczhz(_stats_pixel *const stats,
                                        const float *const pixels, const size_t width,
                                        const void *const data)
{
  const dt_iop_order_iccprofile_info_t *const profile = data;
  for(size_t i = 0; i < width; i += 4)
  {
    dt_aligned_pixel_t pick;
    rgb_to_JzCzhz(pixels + i, pick, profile);
    // FIXME: is this used?
    pick[3] = pick[2] < 0.5f ? pick[2] + 0.5f : pick[2] - 0.5f;
    _update_stats_4ch(stats, pick);
  }
}

static inline void _color_picker_bayer(_stats_pixel *const stats,
                                       _count_pixel *const weights,
                                       const float *const pixels,
                                       const size_t j,
                                       const dt_iop_roi_t *const roi,
                                       const int *const box,
                                       const void *const data)
{
  const uint32_t filters = GPOINTER_TO_UINT(data);
  for(size_t i = box[0]; i < box[2]; i++)
  {
    const int c = FC(j + roi->y, i + roi->x, filters);
    _update_stats_1ch(stats, c, pixels[i]);
    weights->v[c]++;
  }
}

static inline void _color_picker_xtrans(_stats_pixel *const stats,
                                       _count_pixel *const weights,
                                       const float *const pixels,
                                       const size_t j,
                                       const dt_iop_roi_t *const roi,
                                       const int *const box,
                                       const void *const data)
{
  const uint8_t(*const xtrans)[6] = (const uint8_t(*const)[6])data;
  for(size_t i = box[0]; i < box[2]; i++)
  {
    const int c = FCxtrans(j, i, roi, xtrans);
    _update_stats_1ch(stats, c, pixels[i]);
    weights->v[c]++;
  }
}

static void _color_picker_work_4ch(const float *const pixel,
                                   const dt_iop_roi_t *const roi, const int *const box,
                                   lib_colorpicker_stats pick,
                                   const void *const data,
                                   const picker_worker_4ch worker,
                                   const size_t min_for_threads)
{
  const int width = roi->width;
  const size_t size = _box_size(box);
  const size_t stride = 4 * (size_t)(box[2] - box[0]);
  const size_t off_mul = 4 * width;
  const size_t off_add = 4 * box[0];

  _stats_pixel stats = { .acc = { 0.0f, 0.0f, 0.0f, 0.0f },
                         .min = { FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX },
                         .max = { -FLT_MAX, -FLT_MAX, -FLT_MAX, -FLT_MAX } };

  // FIXME: will this run faster if we use collapse(2)? will this take rejiggering of function calls?
  // cutoffs for using threads depends on # of samples and complexity
  // of the colorspace conversion
#if defined(_OPENMP) && _CUSTOM_REDUCTIONS
#pragma omp parallel for default(none) if (size > min_for_threads)        \
  dt_omp_firstprivate(worker, pixel, stride, off_mul, off_add, box, data) \
  reduction(vstats : stats) schedule(static)
#endif
  for(size_t j = box[1]; j < box[3]; j++)
  {
    const size_t offset = j * off_mul + off_add;
    worker(&stats, pixel + offset, stride, data);
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

static void _color_picker_work_1ch(const float *const pixel,
                                   const dt_iop_roi_t *const roi, const int *const box,
                                   lib_colorpicker_stats pick,
                                   const void *const data,
                                   const picker_worker_1ch worker,
                                   const size_t min_for_threads)
{
  const int width = roi->width;
  _count_pixel weights = { { 0u, 0u, 0u, 0u } };
  _stats_pixel stats = { .acc = { 0.0f, 0.0f, 0.0f, 0.0f },
                         .min = { FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX },
                         .max = { -FLT_MAX, -FLT_MAX, -FLT_MAX, -FLT_MAX } };
  const size_t size = _box_size(box);

  // worker logic is slightly different from 4-channel as we need to
  // keep track of position in the mosiac
#if defined(_OPENMP) && _CUSTOM_REDUCTIONS
#pragma omp parallel for default(none) if (size > min_for_threads)      \
  dt_omp_firstprivate(worker, pixel, width, roi, box, data)             \
  reduction(vstats : stats) reduction(vsum : weights)                   \
  schedule(static)
#endif
  for(size_t j = box[1]; j < box[3]; j++)
  {
    worker(&stats, &weights, pixel + width * j, j, roi, box, data);
  }

  copy_pixel(pick[DT_PICK_MIN], stats.min);
  copy_pixel(pick[DT_PICK_MAX], stats.max);
  // and finally normalize data. For bayer, there is twice as much green.
  // X-Trans RGB weighting averages to 2:5:2 for each 3x3 cell
  for_each_channel(c)
    pick[DT_PICK_MEAN][c] =
    weights.v[c] ? (stats.acc[c] / (float)weights.v[c]) : 0.0f;
}

void dt_color_picker_helper(const dt_iop_buffer_dsc_t *dsc, const float *const pixel,
                            const dt_iop_roi_t *roi, const int *const box,
                            const gboolean denoise,
                            lib_colorpicker_stats pick,
                            const dt_iop_colorspace_type_t image_cst,
                            const dt_iop_colorspace_type_t picker_cst,
                            const dt_iop_order_iccprofile_info_t *const profile)
{
  dt_times_t start_time = { 0 }, end_time = { 0 };
  if(darktable.unmuted & DT_DEBUG_PERF) dt_get_times(&start_time);

  if(dsc->channels == 4u)
  {
    float *restrict denoised = NULL;
    const float *source = pixel;
    if(denoise)
    {
      // Denoise the image
      size_t padded_size;
      denoised = dt_alloc_align_float(4 * roi->width * roi->height);
      float *const DT_ALIGNED_ARRAY tempbuf = dt_alloc_perthread_float(4 * roi->width, &padded_size); //TODO: alloc in caller

      // blur without clipping negatives because Lab a and b channels can be legitimately negative
      // FIXME: this blurs whole image even when just a bit is sampled
      // FIXME: if this is done in pixelpipe, we should have a spare buffer (output) to write this into, hence can skip the alloc above, and all do this on the input to filmic
      blur_2D_Bspline(pixel, denoised, tempbuf, roi->width, roi->height, 1, FALSE);
      dt_free_align(tempbuf);
      source = denoised;
    }

    // 4-channel raw images are monochrome, can be read as RGB
    const dt_iop_colorspace_type_t effective_cst =
      image_cst == IOP_CS_RAW ? IOP_CS_RGB : image_cst;

    if(effective_cst == IOP_CS_LAB && picker_cst == IOP_CS_LCH)
    {
      // blending for Lab modules (e.g. color zones and tone curve)
      _color_picker_work_4ch(source, roi, box, pick, NULL, _color_picker_lch, 500);
    }
    else if(effective_cst == IOP_CS_RGB && picker_cst == IOP_CS_HSL)
    {
      // display-referred blending for RGB mdoules
      _color_picker_work_4ch(source, roi, box, pick, NULL, _color_picker_hsl, 250);
    }
    else if(effective_cst == IOP_CS_RGB && picker_cst == IOP_CS_JZCZHZ)
    {
      // scene-referred blending for RGB mdoules
      _color_picker_work_4ch(source, roi, box, pick, profile, _color_picker_jzczhz, 100);
    }
    else if(effective_cst == picker_cst)
    {
      // most iop pickers and the global picker
      _color_picker_work_4ch(source, roi, box, pick, NULL, _color_picker_rgb_or_lab, 1000);
    }
    else if(picker_cst == IOP_CS_NONE)
    {
      // temperature IOP when correcting non-RAW
      _color_picker_work_4ch(source, roi, box, pick, NULL, _color_picker_rgb_or_lab, 1000);
    }
    else
    {
      // fallback, but this shouldn't happen
      fprintf(stderr, "[colorpicker] unknown colorspace conversion from %d to %d\n", image_cst, picker_cst);
      _color_picker_work_4ch(source, roi, box, pick, NULL, _color_picker_rgb_or_lab, 1000);
    }

    if(denoised) dt_free_align(denoised);
  }
  else if(dsc->channels == 1u && dsc->filters != 0u && dsc->filters != 9u)
  {
    _color_picker_work_1ch(pixel, roi, box, pick, GUINT_TO_POINTER(dsc->filters),
                           _color_picker_bayer, 25000);
  }
  else if(dsc->channels == 1u && dsc->filters == 9u)
  {
    _color_picker_work_1ch(pixel, roi, box, pick, dsc->xtrans,
                           _color_picker_xtrans, 20000);
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
