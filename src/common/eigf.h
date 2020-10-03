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

static inline void exposure_independent_guided_filter(const float *const restrict guide, // I
                                    const float *const restrict mask, //p
                                    float *const restrict ab,
                                    const size_t width, const size_t height,
                                    const float sigma, const float feathering)
{
  // We also use gaussian blurs instead of the square blurs of the guided filter
  const size_t Ndim = width * height;
  float *const restrict blurred_guide = dt_alloc_sse_ps(Ndim);
  // guide_x_guide = (guide - blurred_guide)^2
  float *const restrict guide_x_guide = dt_alloc_sse_ps(Ndim);
  // guide_variance = blur(guide_x_guide)
  float *const restrict guide_variance = dt_alloc_sse_ps(Ndim);
  float *const restrict guide_x_mask = dt_alloc_sse_ps(Ndim);
  // guide_mask_covariance = blur(guide_x_mask)
  float *const restrict guide_mask_covariance = dt_alloc_sse_ps(Ndim);
  float *const restrict blurred_mask = dt_alloc_sse_ps(Ndim);

  float ming = 10000000.0f;
  float maxg = 0.0f;
  float minm = 10000000.0f;
  float maxm = 0.0f;
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
dt_omp_firstprivate(guide, mask, Ndim) \
  schedule(simd:static) aligned(guide, mask:64) \
  reduction(max:maxg, maxm)\
  reduction(min:ming, minm)
#endif
  for(size_t k = 0; k < Ndim; k++)
  {
    const float pixelg = guide[k];
    const float pixelm = mask[k];
    if(pixelg < ming) ming = pixelg;
    if(pixelg > maxg) maxg = pixelg;
    if(pixelm < minm) minm = pixelm;
    if(pixelm > maxm) maxm = pixelm;
  }

  dt_gaussian_t *g = dt_gaussian_init(width, height, 1, &maxg, &ming, sigma, 0);
  if(!g) return;
  dt_gaussian_blur(g, guide, blurred_guide);
  dt_gaussian_free(g);

  g = dt_gaussian_init(width, height, 1, &maxm, &minm, sigma, 0);
  if(!g) return;
  dt_gaussian_blur(g, mask, blurred_mask);
  dt_gaussian_free(g);

  float mingg = 10000000.0f;
  float maxgg = 0.0f;
  float mingm = 10000000.0f;
  float maxgm = 0.0f;
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(guide, mask, blurred_guide, blurred_mask, guide_x_guide, guide_x_mask, Ndim) \
  schedule(simd:static) aligned(guide, mask, blurred_guide, blurred_mask, guide_x_guide, guide_x_mask:64) \
  reduction(max:maxgg, maxgm)\
  reduction(min:mingg, mingm)
#endif
  for(size_t k = 0; k < Ndim; k++)
  {
    const float deviation = guide[k] - blurred_guide[k];
    const float squared_deviation = deviation * deviation;
    guide_x_guide[k] = squared_deviation;
    if(squared_deviation < mingg) mingg = squared_deviation;
    if(squared_deviation > maxgg) maxgg = squared_deviation;
    const float cov = (guide[k] - blurred_guide[k]) * (mask[k] - blurred_mask[k]);
    guide_x_mask[k] = cov;
    if(cov < mingm) mingm = cov;
    if(cov > maxgm) maxgm = cov;
  }

  g = dt_gaussian_init(width, height, 1, &maxgg, &mingg, sigma, 0);
  if(!g) return;
  dt_gaussian_blur(g, guide_x_guide, guide_variance);
  dt_gaussian_free(g);

  g = dt_gaussian_init(width, height, 1, &maxgm, &mingm, sigma, 0);
  if(!g) return;
  dt_gaussian_blur(g, guide_x_mask, guide_mask_covariance);
  dt_gaussian_free(g);

#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(guide, mask, blurred_guide, guide_variance, blurred_mask, guide_mask_covariance, ab, Ndim, feathering) \
  schedule(simd:static) aligned(guide, mask, blurred_guide, guide_variance, blurred_mask, guide_mask_covariance, ab:64)
#endif
  for(size_t k = 0; k < Ndim; k++)
  {
    const float normg = fmaxf(blurred_guide[k] * guide[k], 1E-6);
    const float normm = fmaxf(blurred_mask[k] * mask[k], 1E-6);
    const float normalized_var_guide = guide_variance[k] / normg;
    const float normalized_covar = guide_mask_covariance[k] / sqrtf(normg * normm);
    ab[2 * k] = normalized_covar / (normalized_var_guide + feathering);
    ab[2 * k + 1] = blurred_mask[k] - ab[2 * k] * blurred_guide[k];
  }

  dt_free_align(blurred_guide);
  dt_free_align(guide_x_guide);
  dt_free_align(guide_variance);
  dt_free_align(guide_x_mask);
  dt_free_align(guide_mask_covariance);
  dt_free_align(blurred_mask);
}

__DT_CLONE_TARGETS__
static inline void fast_eigf_surface_blur(float *const restrict image,
                                      const size_t width, const size_t height,
                                      const int radius, float feathering, const int iterations,
                                      const dt_iop_guided_filter_blending_t filter, const float scale,
                                      const float quantization, const float quantize_min, const float quantize_max)
{
  // Works in-place on a grey image
  // mostly similar with fast_surface_blur from fast_guided_filter.h

  // A down-scaling of 4 seems empirically safe and consistent no matter the image zoom level
  // see reference paper above for proof.
  const float scaling = 1.0f;
  const float ds_sigma = fmaxf((float)radius / scaling, 1.0f);

  const size_t ds_height = height / scaling;
  const size_t ds_width = width / scaling;

  const size_t num_elem_ds = ds_width * ds_height;
  const size_t num_elem = width * height;

  float *const restrict ds_image = dt_alloc_sse_ps(dt_round_size_sse(num_elem_ds));
  float *const restrict ds_mask = dt_alloc_sse_ps(dt_round_size_sse(num_elem_ds));
  float *const restrict ds_ab = dt_alloc_sse_ps(dt_round_size_sse(num_elem_ds * 2));
  float *const restrict ab = dt_alloc_sse_ps(dt_round_size_sse(num_elem * 2));

  if(!ds_image || !ds_mask || !ds_ab || !ab)
  {
    dt_control_log(_("fast exposure independent guided filter failed to allocate memory, check your RAM settings"));
    goto clean;
  }

  // Downsample the image for speed-up
  interpolate_bilinear(image, width, height, ds_image, ds_width, ds_height, 1);

  // empirical formula to have consistent smoothing when increasing the radius
  const float adapted_feathering = feathering * radius * sqrt(radius) / 40.0f;
  // Iterations of filter models the diffusion, sort of
  for(int i = 0; i < iterations; i++)
  {
    // (Re)build the mask from the quantized image to help guiding
    quantize(ds_image, ds_mask, ds_width * ds_height, quantization, quantize_min, quantize_max);
    exposure_independent_guided_filter(ds_mask, ds_image, ds_ab, ds_width, ds_height, ds_sigma, adapted_feathering);

    if(i != iterations - 1)
    {
      // Process the intermediate filtered image
      apply_linear_blending(ds_image, ds_ab, num_elem_ds);
    }
  }

  // Upsample the blending parameters a and b
  interpolate_bilinear(ds_ab, ds_width, ds_height, ab, width, height, 2);

  // Finally, blend the guided image
  if(filter == DT_GF_BLENDING_LINEAR)
    apply_linear_blending(image, ab, num_elem);
  else if(filter == DT_GF_BLENDING_GEOMEAN)
    apply_linear_blending_w_geomean(image, ab, num_elem);

clean:
  if(ab) dt_free_align(ab);
  if(ds_ab) dt_free_align(ds_ab);
  if(ds_mask) dt_free_align(ds_mask);
  if(ds_image) dt_free_align(ds_image);
}
