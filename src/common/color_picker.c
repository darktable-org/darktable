/*
    This file is part of darktable,
    Copyright (C) 2016-2021 darktable developers.

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

#ifdef _OPENMP
#pragma omp declare simd aligned(avg, min, max, pixels: 16) uniform(width, w)
#endif
static inline void _color_picker_rgb_or_lab(dt_aligned_pixel_t avg, dt_aligned_pixel_t min, dt_aligned_pixel_t max,
                                            const float *const pixels, const float w, const size_t width)
{
  for(size_t i = 0; i < width; i += 4)
  {
    dt_aligned_pixel_t pick = { pixels[i], pixels[i + 1], pixels[i + 2], 0.0f };
    for(size_t k = 0; k < 4; k++)
    {
      avg[k] += w * pick[k];
      min[k] = fminf(min[k], pick[k]);
      max[k] = fmaxf(max[k], pick[k]);
    }
  }
}

#ifdef _OPENMP
#pragma omp declare simd aligned(avg, min, max, pixels: 16) uniform(width, w)
#endif
static inline void _color_picker_lch(dt_aligned_pixel_t avg, dt_aligned_pixel_t min, dt_aligned_pixel_t max,
                                     const float *const pixels, const float w, const size_t width)
{
  for(size_t i = 0; i < width; i += 4)
  {
    dt_aligned_pixel_t pick;
    dt_Lab_2_LCH(pixels + i, pick);
    pick[3] = pick[2] < 0.5f ? pick[2] + 0.5f : pick[2] - 0.5f;
    for(size_t k = 0; k < 4; k++)
    {
      avg[k] += w * pick[k];
      min[k] = fminf(min[k], pick[k]);
      max[k] = fmaxf(max[k], pick[k]);
    }
  }
}

#ifdef _OPENMP
#pragma omp declare simd aligned(avg, min, max, pixels: 16) uniform(width, w)
#endif
static inline void _color_picker_hsl(dt_aligned_pixel_t avg, dt_aligned_pixel_t min, dt_aligned_pixel_t max,
                                     const float *const pixels, const float w, const size_t width)
{
  for(size_t i = 0; i < width; i += 4)
  {
    dt_aligned_pixel_t pick;
    dt_RGB_2_HSL(pixels + i, pick);
    pick[3] = pick[0] < 0.5f ? pick[0] + 0.5f : pick[0] - 0.5f;
    for(size_t k = 0; k < 4; k++)
    {
      avg[k] += w * pick[k];
      min[k] = fminf(min[k], pick[k]);
      max[k] = fmaxf(max[k], pick[k]);
    }
  }
}

#ifdef _OPENMP
#pragma omp declare simd aligned(avg, min, max, pixels: 16) uniform(width, w, profile)
#endif
static inline void _color_picker_jzczhz(dt_aligned_pixel_t avg, dt_aligned_pixel_t min, dt_aligned_pixel_t max,
                                        const float *const pixels, const float w, const size_t width,
                                        const dt_iop_order_iccprofile_info_t *const profile)
{
  for(size_t i = 0; i < width; i += 4)
  {
    dt_aligned_pixel_t pick;
    rgb_to_JzCzhz(pixels + i, pick, profile);
    pick[3] = pick[2] < 0.5f ? pick[2] + 0.5f : pick[2] - 0.5f;
    for(size_t k = 0; k < 4; k++)
    {
      avg[k] += w * pick[k];
      min[k] = fminf(min[k], pick[k]);
      max[k] = fmaxf(max[k], pick[k]);
    }
  }
}

static void color_picker_helper_4ch_seq(const dt_iop_buffer_dsc_t *const dsc, const float *const pixel,
                                        const dt_iop_roi_t *const roi, const int *const box,
                                        dt_aligned_pixel_t picked_color, dt_aligned_pixel_t picked_color_min,
                                        dt_aligned_pixel_t picked_color_max, const dt_iop_colorspace_type_t cst_to,
                                        const dt_iop_order_iccprofile_info_t *const profile)
{
  const int width = roi->width;

  const size_t size = _box_size(box);
  const size_t stride = 4 * (size_t)(box[2] - box[0]);
  const size_t off_mul = 4 * width;
  const size_t off_add = 4 * box[0];

  const float w = 1.0f / (float)size;

  // code path for small region, especially for color picker point mode
  if(cst_to == IOP_CS_LCH)
  {
    for(size_t j = box[1]; j < box[3]; j++)
    {
      const size_t offset = j * off_mul + off_add;
      _color_picker_lch(picked_color, picked_color_min, picked_color_max, pixel + offset, w, stride);
    }
  }
  else if(cst_to == IOP_CS_HSL)
  {
    for(size_t j = box[1]; j < box[3]; j++)
    {
      const size_t offset = j * off_mul + off_add;
      _color_picker_hsl(picked_color, picked_color_min, picked_color_max, pixel + offset, w, stride);
    }
  }
  else if(cst_to == IOP_CS_JZCZHZ)
  {
    for(size_t j = box[1]; j < box[3]; j++)
    {
      const size_t offset = j * off_mul + off_add;
      _color_picker_jzczhz(picked_color, picked_color_min, picked_color_max, pixel + offset, w, stride, profile);
    }
  }
  else
  {
    for(size_t j = box[1]; j < box[3]; j++)
    {
      const size_t offset = j * off_mul + off_add;
      _color_picker_rgb_or_lab(picked_color, picked_color_min, picked_color_max, pixel + offset, w, stride);
    }
  }
}

static void color_picker_helper_4ch_parallel(const dt_iop_buffer_dsc_t *const dsc, const float *const pixel,
                                             const dt_iop_roi_t *const roi, const int *const box,
                                             dt_aligned_pixel_t picked_color, dt_aligned_pixel_t picked_color_min,
                                             dt_aligned_pixel_t picked_color_max, const dt_iop_colorspace_type_t cst_to,
                                             const dt_iop_order_iccprofile_info_t *const profile)
{
  const int width = roi->width;

  const size_t size = _box_size(box);
  const size_t stride = 4 * (size_t)(box[2] - box[0]);
  const size_t off_mul = 4 * width;
  const size_t off_add = 4 * box[0];

  const float w = 1.0f / (float)size;

  const size_t numthreads = dt_get_num_threads();

  size_t allocsize;
  float *const restrict mean = dt_alloc_perthread_float(4, &allocsize);
  float *const restrict mmin = dt_alloc_perthread_float(4, &allocsize);
  float *const restrict mmax = dt_alloc_perthread_float(4, &allocsize);

  for(int n = 0; n < allocsize * numthreads; n++)
  {
    mean[n] = 0.0f;
    mmin[n] = INFINITY;
    mmax[n] = -INFINITY;
  }

  if(cst_to == IOP_CS_LCH)
  {
#ifdef _OPENMP
#pragma omp parallel default(none) \
  dt_omp_firstprivate(w, pixel, width, stride, off_mul, off_add, box, mean, mmin, mmax, allocsize)
#endif
    {
      float *const restrict tmean = dt_get_perthread(mean,allocsize);
      float *const restrict tmmin = dt_get_perthread(mmin,allocsize);
      float *const restrict tmmax = dt_get_perthread(mmax,allocsize);

#ifdef _OPENMP
#pragma omp for schedule(static)
#endif
      for(size_t j = box[1]; j < box[3]; j++)
      {
        const size_t offset = j * off_mul + off_add;
        _color_picker_lch(tmean, tmmin, tmmax, pixel + offset, w, stride);
      }
    }
  }
  else if(cst_to == IOP_CS_HSL)
  {
#ifdef _OPENMP
#pragma omp parallel default(none) \
  dt_omp_firstprivate(w, pixel, width, stride, off_mul, off_add, box, mean, mmin, mmax, allocsize)
#endif
    {
      float *const restrict tmean = dt_get_perthread(mean,allocsize);
      float *const restrict tmmin = dt_get_perthread(mmin,allocsize);
      float *const restrict tmmax = dt_get_perthread(mmax,allocsize);

#ifdef _OPENMP
#pragma omp for schedule(static)
#endif
      for(size_t j = box[1]; j < box[3]; j++)
      {
        const size_t offset = j * off_mul + off_add;
        _color_picker_hsl(tmean, tmmin, tmmax, pixel + offset, w, stride);
      }
    }
  }
  else if(cst_to == IOP_CS_JZCZHZ)
  {
#ifdef _OPENMP
#pragma omp parallel default(none) \
  dt_omp_firstprivate(w, pixel, width, stride, off_mul, off_add, box, mean, mmin, mmax, profile, allocsize)
#endif
    {
      float *const restrict tmean = dt_get_perthread(mean,allocsize);
      float *const restrict tmmin = dt_get_perthread(mmin,allocsize);
      float *const restrict tmmax = dt_get_perthread(mmax,allocsize);

#ifdef _OPENMP
#pragma omp for schedule(static)
#endif
      for(size_t j = box[1]; j < box[3]; j++)
      {
        const size_t offset = j * off_mul + off_add;
        _color_picker_jzczhz(tmean, tmmin, tmmax, pixel + offset, w, stride, profile);
      }
    }
  }
  else
  {
#ifdef _OPENMP
#pragma omp parallel default(none) \
  dt_omp_firstprivate(w, pixel, width, stride, off_mul, off_add, box, mean, mmin, mmax, allocsize)
#endif
    {
      float *const restrict tmean = dt_get_perthread(mean,allocsize);
      float *const restrict tmmin = dt_get_perthread(mmin,allocsize);
      float *const restrict tmmax = dt_get_perthread(mmax,allocsize);

#ifdef _OPENMP
#pragma omp for schedule(static)
#endif
      for(size_t j = box[1]; j < box[3]; j++)
      {
        const size_t offset = j * off_mul + off_add;
        _color_picker_rgb_or_lab(tmean, tmmin, tmmax, pixel + offset, w, stride);
      }
    }
  }

  for(int n = 0; n < numthreads; n++)
  {
    for(int k = 0; k < 4; k++)
    {
      picked_color[k] += mean[allocsize * n + k];
      picked_color_min[k] = fminf(picked_color_min[k], mmin[allocsize * n + k]);
      picked_color_max[k] = fmaxf(picked_color_max[k], mmax[allocsize * n + k]);
    }
  }

  dt_free_align(mmax);
  dt_free_align(mmin);
  dt_free_align(mean);
}

static void color_picker_helper_4ch(const dt_iop_buffer_dsc_t *dsc, const float *const pixel,
                                    const dt_iop_roi_t *roi, const int *const box, dt_aligned_pixel_t picked_color,
                                    dt_aligned_pixel_t picked_color_min, dt_aligned_pixel_t picked_color_max,
                                    const dt_iop_colorspace_type_t cst_to,
                                    const dt_iop_order_iccprofile_info_t *const profile)
{
  const size_t size = _box_size(box);

  if(size > 100) // avoid inefficient multi-threading in case of small region size (arbitrary limit)
    return color_picker_helper_4ch_parallel(dsc, pixel, roi, box, picked_color, picked_color_min,
                                            picked_color_max, cst_to, profile);
  else
    return color_picker_helper_4ch_seq(dsc, pixel, roi, box, picked_color, picked_color_min, picked_color_max,
                                       cst_to, profile);
}

static void color_picker_helper_bayer_seq(const dt_iop_buffer_dsc_t *const dsc, const float *const pixel,
                                          const dt_iop_roi_t *const roi, const int *const box,
                                          dt_aligned_pixel_t picked_color, dt_aligned_pixel_t picked_color_min,
                                          dt_aligned_pixel_t picked_color_max)
{
  const int width = roi->width;
  const uint32_t filters = dsc->filters;

  uint32_t weights[4] = { 0u, 0u, 0u, 0u };

  // code path for small region, especially for color picker point mode
  for(size_t j = box[1]; j < box[3]; j++)
  {
    for(size_t i = box[0]; i < box[2]; i++)
    {
      const int c = FC(j + roi->y, i + roi->x, filters);
      const size_t k = width * j + i;

      const float v = pixel[k];

      picked_color[c] += v;
      picked_color_min[c] = fminf(picked_color_min[c], v);
      picked_color_max[c] = fmaxf(picked_color_max[c], v);
      weights[c]++;
    }
  }

  // and finally normalize data. For bayer, there is twice as much green.
  for(int c = 0; c < 4; c++)
  {
    picked_color[c] = weights[c] ? (picked_color[c] / (float)weights[c]) : 0.0f;
  }
}

static void color_picker_helper_bayer_parallel(const dt_iop_buffer_dsc_t *const dsc, const float *const pixel,
                                               const dt_iop_roi_t *const roi, const int *const box,
                                               dt_aligned_pixel_t picked_color, dt_aligned_pixel_t picked_color_min,
                                               dt_aligned_pixel_t picked_color_max)
{
  const int width = roi->width;
  const uint32_t filters = dsc->filters;

  uint32_t weights[4] = { 0u, 0u, 0u, 0u };

  const size_t numthreads = dt_get_num_threads();

  //TODO: convert to use dt_alloc_perthread
  float *const msum = malloc(sizeof(float) * numthreads * 4);
  float *const mmin = malloc(sizeof(float) * numthreads * 4);
  float *const mmax = malloc(sizeof(float) * numthreads * 4);
  uint32_t *const cnt = malloc(sizeof(uint32_t) * numthreads * 4);

  for(int n = 0; n < 4 * numthreads; n++)
  {
    msum[n] = 0.0f;
    mmin[n] = INFINITY;
    mmax[n] = -INFINITY;
    cnt[n] = 0u;
  }

#ifdef _OPENMP
#pragma omp parallel default(none) \
  dt_omp_firstprivate(pixel, width, roi, filters, box, msum, mmin, mmax, cnt)
#endif
  {
    const int tnum = dt_get_thread_num();

    float *const tsum = msum + 4 * tnum;
    float *const tmmin = mmin + 4 * tnum;
    float *const tmmax = mmax + 4 * tnum;
    uint32_t *const tcnt = cnt + 4 * tnum;

#ifdef _OPENMP
#pragma omp for schedule(static) collapse(2)
#endif
    for(size_t j = box[1]; j < box[3]; j++)
    {
      for(size_t i = box[0]; i < box[2]; i++)
      {
        const int c = FC(j + roi->y, i + roi->x, filters);
        const size_t k = width * j + i;

        const float v = pixel[k];

        tsum[c] += v;
        tmmin[c] = fminf(tmmin[c], v);
        tmmax[c] = fmaxf(tmmax[c], v);
        tcnt[c]++;
      }
    }
  }

  for(int n = 0; n < numthreads; n++)
  {
    for(int c = 0; c < 4; c++)
    {
      picked_color[c] += msum[4 * n + c];
      picked_color_min[c] = fminf(picked_color_min[c], mmin[4 * n + c]);
      picked_color_max[c] = fmaxf(picked_color_max[c], mmax[4 * n + c]);
      weights[c] += cnt[4 * n + c];
    }
  }

  free(cnt);
  free(mmax);
  free(mmin);
  free(msum);

  // and finally normalize data. For bayer, there is twice as much green.
  for(int c = 0; c < 4; c++)
  {
    picked_color[c] = weights[c] ? (picked_color[c] / (float)weights[c]) : 0.0f;
  }
}

static void color_picker_helper_bayer(const dt_iop_buffer_dsc_t *dsc, const float *const pixel,
                                      const dt_iop_roi_t *roi, const int *const box, dt_aligned_pixel_t picked_color,
                                      dt_aligned_pixel_t picked_color_min, dt_aligned_pixel_t picked_color_max)
{
  const size_t size = _box_size(box);

  if(size > 100) // avoid inefficient multi-threading in case of small region size (arbitrary limit)
    return color_picker_helper_bayer_parallel(dsc, pixel, roi, box, picked_color, picked_color_min,
                                              picked_color_max);
  else
    return color_picker_helper_bayer_seq(dsc, pixel, roi, box, picked_color, picked_color_min, picked_color_max);
}

static void color_picker_helper_xtrans_seq(const dt_iop_buffer_dsc_t *const dsc, const float *const pixel,
                                           const dt_iop_roi_t *const roi, const int *const box,
                                           dt_aligned_pixel_t picked_color, dt_aligned_pixel_t picked_color_min,
                                           dt_aligned_pixel_t picked_color_max)
{
  const int width = roi->width;
  const uint8_t(*const xtrans)[6] = (const uint8_t(*const)[6])dsc->xtrans;

  uint32_t weights[3] = { 0u, 0u, 0u };

  // code path for small region, especially for color picker point mode
  for(size_t j = box[1]; j < box[3]; j++)
  {
    for(size_t i = box[0]; i < box[2]; i++)
    {
      const int c = FCxtrans(j, i, roi, xtrans);
      const size_t k = width * j + i;

      const float v = pixel[k];

      picked_color[c] += v;
      picked_color_min[c] = fminf(picked_color_min[c], v);
      picked_color_max[c] = fmaxf(picked_color_max[c], v);
      weights[c]++;
    }
  }

  // and finally normalize data.
  // X-Trans RGB weighting averages to 2:5:2 for each 3x3 cell
  for(int c = 0; c < 3; c++)
  {
    picked_color[c] /= (float)weights[c];
  }
}

static void color_picker_helper_xtrans_parallel(const dt_iop_buffer_dsc_t *const dsc, const float *const pixel,
                                                const dt_iop_roi_t *const roi, const int *const box,
                                                dt_aligned_pixel_t picked_color, dt_aligned_pixel_t picked_color_min,
                                                dt_aligned_pixel_t picked_color_max)
{
  const int width = roi->width;
  const uint8_t(*const xtrans)[6] = (const uint8_t(*const)[6])dsc->xtrans;

  uint32_t weights[3] = { 0u, 0u, 0u };

  const size_t numthreads = dt_get_num_threads();

  //TODO: convert to use dt_alloc_perthread
  float *const mmin = malloc(sizeof(float) * numthreads * 3);
  float *const msum = malloc(sizeof(float) * numthreads * 3);
  float *const mmax = malloc(sizeof(float) * numthreads * 3);
  uint32_t *const cnt = malloc(sizeof(uint32_t) * numthreads * 3);

  for(int n = 0; n < 3 * numthreads; n++)
  {
    msum[n] = 0.0f;
    mmin[n] = INFINITY;
    mmax[n] = -INFINITY;
    cnt[n] = 0u;
  }

#ifdef _OPENMP
#pragma omp parallel default(none) \
  dt_omp_firstprivate(pixel, width, roi, xtrans, box, cnt, msum, mmin, mmax)
#endif
  {
    const int tnum = dt_get_thread_num();

    float *const tsum = msum + 3 * tnum;
    float *const tmmin = mmin + 3 * tnum;
    float *const tmmax = mmax + 3 * tnum;
    uint32_t *const tcnt = cnt + 3 * tnum;

#ifdef _OPENMP
#pragma omp for schedule(static) collapse(2)
#endif
    for(size_t j = box[1]; j < box[3]; j++)
    {
      for(size_t i = box[0]; i < box[2]; i++)
      {
        const int c = FCxtrans(j, i, roi, xtrans);
        const size_t k = width * j + i;

        const float v = pixel[k];

        tsum[c] += v;
        tmmin[c] = fminf(tmmin[c], v);
        tmmax[c] = fmaxf(tmmax[c], v);
        tcnt[c]++;
      }
    }
  }

  for(int n = 0; n < numthreads; n++)
  {
    for(int c = 0; c < 3; c++)
    {
      picked_color[c] += msum[3 * n + c];
      picked_color_min[c] = fminf(picked_color_min[c], mmin[3 * n + c]);
      picked_color_max[c] = fmaxf(picked_color_max[c], mmax[3 * n + c]);
      weights[c] += cnt[3 * n + c];
    }
  }

  free(cnt);
  free(mmax);
  free(mmin);
  free(msum);

  // and finally normalize data.
  // X-Trans RGB weighting averages to 2:5:2 for each 3x3 cell
  for(int c = 0; c < 3; c++)
  {
    picked_color[c] /= (float)weights[c];
  }
}

static void color_picker_helper_xtrans(const dt_iop_buffer_dsc_t *dsc, const float *const pixel,
                                       const dt_iop_roi_t *roi, const int *const box, dt_aligned_pixel_t picked_color,
                                       dt_aligned_pixel_t picked_color_min, dt_aligned_pixel_t picked_color_max)
{
  const size_t size = _box_size(box);

  if(size > 100) // avoid inefficient multi-threading in case of small region size (arbitrary limit)
    return color_picker_helper_xtrans_parallel(dsc, pixel, roi, box, picked_color, picked_color_min,
                                               picked_color_max);
  else
    return color_picker_helper_xtrans_seq(dsc, pixel, roi, box, picked_color, picked_color_min, picked_color_max);
}

// picked_color, picked_color_min and picked_color_max should be aligned
void dt_color_picker_helper(const dt_iop_buffer_dsc_t *dsc, const float *const pixel, const dt_iop_roi_t *roi,
                            const int *const box, dt_aligned_pixel_t picked_color, dt_aligned_pixel_t picked_color_min,
                            dt_aligned_pixel_t picked_color_max, const dt_iop_colorspace_type_t image_cst,
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
    blur_2D_Bspline(pixel, denoised, tempbuf, roi->width, roi->height, 1, FALSE);

    if(((image_cst == picker_cst) || (picker_cst == IOP_CS_NONE)))
      color_picker_helper_4ch(dsc, denoised, roi, box, picked_color, picked_color_min, picked_color_max, picker_cst, profile);
    else if(image_cst == IOP_CS_LAB && picker_cst == IOP_CS_LCH)
      color_picker_helper_4ch(dsc, denoised, roi, box, picked_color, picked_color_min, picked_color_max, picker_cst, profile);
    else if(image_cst == IOP_CS_RGB && (picker_cst == IOP_CS_HSL || picker_cst == IOP_CS_JZCZHZ))
      color_picker_helper_4ch(dsc, denoised, roi, box, picked_color, picked_color_min, picked_color_max, picker_cst, profile);
    else // This is a fallback, better than crashing as happens with monochromes
      color_picker_helper_4ch(dsc, denoised, roi, box, picked_color, picked_color_min, picked_color_max, picker_cst, profile);

    dt_free_align(denoised);
    dt_free_align(tempbuf);
  }
  else if(dsc->channels == 1u && dsc->filters != 0u && dsc->filters != 9u)
    color_picker_helper_bayer(dsc, pixel, roi, box, picked_color, picked_color_min, picked_color_max);
  else if(dsc->channels == 1u && dsc->filters == 9u)
    color_picker_helper_xtrans(dsc, pixel, roi, box, picked_color, picked_color_min, picked_color_max);
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
