/*
    This file is part of darktable,
    Copyright (C) 2019-2020 darktable developers.

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

#pragma once

#include "common/fast_guided_filter.h"
#include "common/gaussian.h"

/***
 * DOCUMENTATION
 *
 * Exposure-Independent Guided Filter (EIGF)
 *
 * This filter is a modification of guided filter to make it exposure independent
 * As variance depends on the exposure, the original guided filter preserves
 * much better the edges in the highlights than in the shadows.
 * In particular doing:
 * (1) increase exposure by 1EV
 * (2) guided filtering
 * (3) decrease exposure by 1EV
 * is NOT equivalent to doing the guided filtering only.
 *
 * To overcome this, instead of using variance directly to determine "a",
 * we use a ratio:
 * variance / (pixel_value)^2
 * we tried also the following ratios:
 * - variance / average^2
 * - variance / (pixel_value * average)
 * we kept variance / (pixel_value)^2 as it seemed to behave a bit better than
 * the other (dividing by average^2 smoothed too much dark details surrounded
 * by bright pixels).
 *
 * This modification makes the filter exposure-independent.
 * However, due to the fact that the average advantages the bright pixels
 * compared to dark pixels if we consider that human eye sees in log,
 * we get strong bright halos.
 * These are due to the spatial averaging of "a" and "b" that is performed at
 * the end of the filter, especially due to the spatial averaging of "b".
 * We decided to remove this final spatial averaging, as it is very hard
 * to keep it without having either large unsmoothed regions or halos.
 * Although the filter may blur a bit less without it, it remains sufficiently
 * good at smoothing the image, and there are much less halos.
 *
 * The implementation EIGF uses downscaling to speed-up the filtering,
 * just like what is done in fast_guided_filter.h
**/

/* computes average and variance of guide and mask, and put them in out.
 * out has 4 channels:
 * - average of guide
 * - variance of guide
 * - average of mask
 * - covariance of mask and guide. */
static inline void eigf_variance_analysis(const float *const restrict guide, // I
                                    const float *const restrict mask, //p
                                    float *const restrict out,
                                    const size_t width, const size_t height,
                                    const float sigma)
{
  // We also use gaussian blurs instead of the square blurs of the guided filter
  const size_t Ndim = width * height;
  float *const restrict in = dt_alloc_sse_ps(Ndim * 4);

  float ming = 10000000.0f;
  float maxg = 0.0f;
  float minm = 10000000.0f;
  float maxm = 0.0f;
  float ming2 = 10000000.0f;
  float maxg2 = 0.0f;
  float minmg = 10000000.0f;
  float maxmg = 0.0f;
#ifdef _OPENMP
#pragma omp parallel for default(none) \
dt_omp_firstprivate(guide, mask, in, Ndim) \
  schedule(simd:static) \
  reduction(max:maxg, maxm, maxg2, maxmg)\
  reduction(min:ming, minm, ming2, minmg)
#endif
  for(size_t k = 0; k < Ndim; k++)
  {
    const float pixelg = guide[k];
    const float pixelm = mask[k];
    const float pixelg2 = pixelg * pixelg;
    const float pixelmg = pixelm * pixelg;
    in[k * 4] = pixelg;
    in[k * 4 + 1] = pixelg2;
    in[k * 4 + 2] = pixelm;
    in[k * 4 + 3] = pixelmg;
    ming = MIN(ming,pixelg);
    maxg = MAX(maxg,pixelg);
    minm = MIN(minm,pixelm);
    maxm = MAX(maxm,pixelm);
    ming2 = MIN(ming2,pixelg2);
    maxg2 = MAX(maxg2,pixelg2);
    minmg = MIN(minmg,pixelmg);
    maxmg = MAX(maxmg,pixelmg);
  }

  dt_aligned_pixel_t max = {maxg, maxg2, maxm, maxmg};
  dt_aligned_pixel_t min = {ming, ming2, minm, minmg};
  dt_gaussian_t *g = dt_gaussian_init(width, height, 4, max, min, sigma, 0);
  if(!g) return;
  dt_gaussian_blur_4c(g, in, out);
  dt_gaussian_free(g);

#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
dt_omp_firstprivate(out, Ndim) \
  schedule(simd:static) aligned(out:64)
#endif
  for(size_t k = 0; k < Ndim; k++)
  {
    out[4 * k + 1] -= out[4 * k] * out[4 * k];
    out[4 * k + 3] -= out[4 * k] * out[4 * k + 2];
  }

  dt_free_align(in);
}

// same function as above, but specialized for the case where guide == mask
// for increased performance
static inline void eigf_variance_analysis_no_mask(const float *const restrict guide, // I
                                    float *const restrict out,
                                    const size_t width, const size_t height,
                                    const float sigma)
{
  // We also use gaussian blurs instead of the square blurs of the guided filter
  const size_t Ndim = width * height;
  float *const restrict in = dt_alloc_sse_ps(Ndim * 2);

  float ming = 10000000.0f;
  float maxg = 0.0f;
  float ming2 = 10000000.0f;
  float maxg2 = 0.0f;
#ifdef _OPENMP
#pragma omp parallel for default(none) \
dt_omp_firstprivate(guide, in, Ndim) \
  schedule(simd:static) \
  reduction(max:maxg, maxg2)\
  reduction(min:ming, ming2)
#endif
  for(size_t k = 0; k < Ndim; k++)
  {
    const float pixelg = guide[k];
    const float pixelg2 = pixelg * pixelg;
    in[2 * k] = pixelg;
    in[2 * k + 1] = pixelg2;
    ming = MIN(ming,pixelg);
    maxg = MAX(maxg,pixelg);
    ming2 = MIN(ming2,pixelg2);
    maxg2 = MAX(maxg2,pixelg2);
  }

  float max[2] = {maxg, maxg2};
  float min[2] = {ming, ming2};
  dt_gaussian_t *g = dt_gaussian_init(width, height, 2, max, min, sigma, 0);
  if(!g) return;
  dt_gaussian_blur(g, in, out);
  dt_gaussian_free(g);

#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
dt_omp_firstprivate(out, Ndim) \
  schedule(simd:static) aligned(out:64)
#endif
  for(size_t k = 0; k < Ndim; k++)
  {
    const float avg = out[2 * k];
    out[2 * k + 1] -= avg * avg;
  }

  dt_free_align(in);
}

void eigf_blending(float *const restrict image, const float *const restrict mask,
                  const float *const restrict av, const size_t Ndim,
                  const dt_iop_guided_filter_blending_t filter,
                  const float feathering)
{
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(image, mask, av, Ndim, feathering, filter) \
  schedule(simd:static) aligned(image, mask, av:64)
#endif
  for(size_t k = 0; k < Ndim; k++)
  {
    const float avg_g = av[k * 4];
    const float avg_m = av[k * 4 + 2];
    const float var_g = av[k * 4 + 1];
    const float covar_mg = av[k * 4 + 3];
    const float norm_g = fmaxf(avg_g * image[k], 1E-6);
    const float norm_m = fmaxf(avg_m * mask[k], 1E-6);
    const float normalized_var_guide = var_g / norm_g;
    const float normalized_covar = covar_mg / sqrtf(norm_g * norm_m);
    const float a = normalized_covar / (normalized_var_guide + feathering);
    const float b = avg_m - a * avg_g;
    if(filter == DT_GF_BLENDING_LINEAR)
    {
      image[k] = fmaxf(image[k] * a + b, MIN_FLOAT);
    }
    else
    {
      // filter == DT_GF_BLENDING_GEOMEAN
      image[k] *= fmaxf(image[k] * a + b, MIN_FLOAT);
      image[k] = sqrtf(image[k]);
    }
  }
}

// same function as above, but specialized for the case where guide == mask
// for increased performance
void eigf_blending_no_mask(float *const restrict image,
                  const float *const restrict av, const size_t Ndim,
                  const dt_iop_guided_filter_blending_t filter,
                  const float feathering)
{
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(image, av, Ndim, feathering, filter) \
  schedule(simd:static) aligned(image, av:64)
#endif
  for(size_t k = 0; k < Ndim; k++)
  {
    const float avg_g = av[k * 2];
    const float var_g = av[k * 2 + 1];
    const float norm_g = fmaxf(avg_g * image[k], 1E-6);
    const float normalized_var_guide = var_g / norm_g;
    const float a = normalized_var_guide / (normalized_var_guide + feathering);
    const float b = avg_g - a * avg_g;
    if(filter == DT_GF_BLENDING_LINEAR)
    {
      image[k] = fmaxf(image[k] * a + b, MIN_FLOAT);
    }
    else
    {
      // filter == DT_GF_BLENDING_GEOMEAN
      image[k] *= fmaxf(image[k] * a + b, MIN_FLOAT);
      image[k] = sqrtf(image[k]);
    }
  }
}

__DT_CLONE_TARGETS__
static inline void fast_eigf_surface_blur(float *const restrict image,
                                      const size_t width, const size_t height,
                                      const float sigma, float feathering, const int iterations,
                                      const dt_iop_guided_filter_blending_t filter, const float scale,
                                      const float quantization, const float quantize_min, const float quantize_max)
{
  // Works in-place on a grey image
  // mostly similar with fast_surface_blur from fast_guided_filter.h

  // A down-scaling of 4 seems empirically safe and consistent no matter the image zoom level
  // see reference paper above for proof.
  const float scaling = fmaxf(fminf(sigma, 4.0f), 1.0f);
  const float ds_sigma = fmaxf(sigma / scaling, 1.0f);

  const size_t ds_height = height / scaling;
  const size_t ds_width = width / scaling;

  const size_t num_elem_ds = ds_width * ds_height;
  const size_t num_elem = width * height;

  float *const restrict mask = dt_alloc_sse_ps(dt_round_size_sse(num_elem));
  float *const restrict ds_image = dt_alloc_sse_ps(dt_round_size_sse(num_elem_ds));
  float *const restrict ds_mask = dt_alloc_sse_ps(dt_round_size_sse(num_elem_ds));
  // average - variance arrays: store the guide and mask averages and variances
  float *const restrict ds_av = dt_alloc_sse_ps(dt_round_size_sse(num_elem_ds * 4));
  float *const restrict av = dt_alloc_sse_ps(dt_round_size_sse(num_elem * 4));

  if(!ds_image || !ds_mask || !ds_av || !av)
  {
    dt_control_log(_("Fast exposure independent guided filter failed to allocate memory, check your RAM settings"));
    goto clean;
  }

  // Iterations of filter models the diffusion, sort of
  for(int i = 0; i < iterations; i++)
  {
    // blend linear for all intermediate images
    dt_iop_guided_filter_blending_t blend = DT_GF_BLENDING_LINEAR;
    // use filter for last iteration
    if(i == iterations - 1)
      blend = filter;

    interpolate_bilinear(image, width, height, ds_image, ds_width, ds_height, 1);
    if(quantization != 0.0f)
    {
      // (Re)build the mask from the quantized image to help guiding
      quantize(image, mask, width * height, quantization, quantize_min, quantize_max);
      // Downsample the image for speed-up
      interpolate_bilinear(mask, width, height, ds_mask, ds_width, ds_height, 1);
      eigf_variance_analysis(ds_mask, ds_image, ds_av, ds_width, ds_height, ds_sigma);
      // Upsample the variances and averages
      interpolate_bilinear(ds_av, ds_width, ds_height, av, width, height, 4);
      // Blend the guided image
      eigf_blending(image, mask, av, num_elem, blend, feathering);
    }
    else
    {
      // no need to build a mask.
      eigf_variance_analysis_no_mask(ds_image, ds_av, ds_width, ds_height, ds_sigma);
      // Upsample the variances and averages
      interpolate_bilinear(ds_av, ds_width, ds_height, av, width, height, 2);
      // Blend the guided image
      eigf_blending_no_mask(image, av, num_elem, blend, feathering);
    }
  }

clean:
  if(av) dt_free_align(av);
  if(ds_av) dt_free_align(ds_av);
  if(ds_mask) dt_free_align(ds_mask);
  if(ds_image) dt_free_align(ds_image);
  if(mask) dt_free_align(mask);
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

