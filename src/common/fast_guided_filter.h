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

#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "common/darktable.h"

/** Note :
 * we use finite-math-only and fast-math because divisions by zero are manually avoided in the code
 * fp-contract=fast enables hardware-accelerated Fused Multiply-Add
 * the rest is loop reorganization and vectorization optimization
 **/

#if defined(__GNUC__)
#pragma GCC optimize ("unroll-loops", "tree-loop-if-convert", \
                      "tree-loop-distribution", "no-strict-aliasing", \
                      "loop-interchange", "loop-nest-optimize", "tree-loop-im", \
                      "unswitch-loops", "tree-loop-ivcanon", "ira-loop-pressure", \
                      "split-ivs-in-unroller", "variable-expansion-in-unroller", \
                      "split-loops", "ivopts", "predictive-commoning",\
                      "tree-loop-linear", "loop-block", "loop-strip-mine", \
                      "finite-math-only", "fp-contract=fast", "fast-math")
#endif

#define MIN_FLOAT exp2f(-16.0f)


typedef enum dt_iop_guided_filter_blending_t
{
  DT_GF_BLENDING_LINEAR = 0,
  DT_GF_BLENDING_GEOMEAN
} dt_iop_guided_filter_blending_t;


/***
 * DOCUMENTATION
 *
 * Fast Iterative Guided filter for surface blur
 *
 * This is a fast vectorized implementation of guided filter for grey images optimized for
 * the special case where the guiding and the guided image are the same, which is useful
 * for edge-aware surface blur.
 *
 * Since the guided filter is a linear application, we can safely downscale
 * the guiding and the guided image by a factor of 4, using a bilinear interpolation,
 * compute the guidance at this scale, then upscale back to the original size
 * and get a free 10× speed-up.
 *
 * Then, the vectorization adds another substantial speed-up. Overall, it brings a ×50 to ×200
 * speed-up compared to the guided_filter.h lib. Of course, it requires every buffer to be
 * 64-bits aligned.
 *
 * On top of the default guided filter, several pre- and post-processing options are provided :
 *
 *  - mask quantization : perform a posterization of the guiding image in log2 space to
 *    help the guiding to produce smoother areas,
 *
 *  - blending : perform a regular (linear) blending of a and b parameters after the
 *    variance analysis (aka the by-the-book guided filter), or a geometric mean of the filter output (by-the-book)
 *    and the original image, which produces a pleasing trade-off.
 *
 *  - iterations : apply the guided filtering recursively, with kernel size increasing by sqrt(2)
 *    between each iteration, to diffuse the filter and soften edges transitions.
 *
 * Reference : 
 *  Kaiming He, Jian Sun, Microsoft : https://arxiv.org/abs/1505.00996
 **/


 #ifdef _OPENMP
#pragma omp declare simd
#endif
__DT_CLONE_TARGETS__
static inline float fast_clamp(const float value, const float bottom, const float top)
{
  // vectorizable clamping between bottom and top values
  return fmax(fmin(value, top), bottom);
}


__DT_CLONE_TARGETS__
static inline void interpolate_bilinear(const float *const restrict in, const size_t width_in, const size_t height_in,
                                        float *const restrict out, const size_t width_out, const size_t height_out,
                                        const size_t ch)
{
  // Fast vectorized bilinear interpolation on ch channels
#ifdef _OPENMP
#pragma omp parallel for simd collapse(2) default(none) \
  schedule(simd:static) aligned(in, out:64) \
  dt_omp_firstprivate(in, out, width_out, height_out, width_in, height_in, ch)
#endif
  for(size_t i = 0; i < height_out; i++)
  {
    for(size_t j = 0; j < width_out; j++)
    {
      // Relative coordinates of the pixel in output space
      const float x_out = (float)j /(float)width_out;
      const float y_out = (float)i /(float)height_out;

      // Corresponding absolute coordinates of the pixel in input space
      const float x_in = x_out * (float)width_in;
      const float y_in = y_out * (float)height_in;

      // Nearest neighbours coordinates in input space
      size_t x_prev = (size_t)floorf(x_in);
      size_t x_next = x_prev + 1;
      size_t y_prev = (size_t)floorf(y_in);
      size_t y_next = y_prev + 1;

      x_prev = (x_prev < width_in) ? x_prev : width_in - 1;
      x_next = (x_next < width_in) ? x_next : width_in - 1;
      y_prev = (y_prev < height_in) ? y_prev : height_in - 1;
      y_next = (y_next < height_in) ? y_next : height_in - 1;

      // Nearest pixels in input array (nodes in grid)
      const size_t Y_prev = y_prev * width_in;
      const size_t Y_next =  y_next * width_in;
      const float *const Q_NW = (float *)in + (Y_prev + x_prev) * ch;
      const float *const Q_NE = (float *)in + (Y_prev + x_next) * ch;
      const float *const Q_SE = (float *)in + (Y_next + x_next) * ch;
      const float *const Q_SW = (float *)in + (Y_next + x_prev) * ch;

      // Spatial differences between nodes
      const float Dy_next = (float)y_next - y_in;
      const float Dy_prev = 1.f - Dy_next; // because next - prev = 1
      const float Dx_next = (float)x_next - x_in;
      const float Dx_prev = 1.f - Dx_next; // because next - prev = 1

      // Interpolate over ch layers
      float *const pixel_out = (float *)out + (i * width_out + j) * ch;

#pragma unroll
      for(size_t c = 0; c < ch; c++)
      {
        pixel_out[c] = Dy_prev * (Q_SW[c] * Dx_next + Q_SE[c] * Dx_prev) +
                       Dy_next * (Q_NW[c] * Dx_next + Q_NE[c] * Dx_prev);
      }
    }
  }
}


__DT_CLONE_TARGETS__
static inline void variance_analyse(const float *const restrict guide, // I
                                    const float *const restrict mask, //p
                                    float *const restrict ab,
                                    const size_t width, const size_t height,
                                    const int radius, const float feathering)
{
  // Compute a box average (filter) on a grey image over a window of size 2*radius + 1
  // then get the variance of the guide and covariance with its mask
  // output a and b, the linear blending params
  // p, the mask is the quantised guide I

  const size_t Ndim = width * height;
  const size_t Ndimch = Ndim * 4;

  /*
  * input is array of struct : { { guide , mask, guide * guide, guide * mask } }
  * temp is array of struct : { { mean_I, mean_p, corr_I, corr_Ip } } = blur(input)
  */
  float *const restrict temp = dt_alloc_sse_ps(Ndimch);
  float *const restrict input = dt_alloc_sse_ps(Ndimch);

  // Pre-multiply guide and mask and pack all inputs into an array of 4×1 SIMD struct
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(guide, mask, Ndim, radius, input) \
  schedule(simd:static) aligned(guide, mask, input:64)
#endif
  for(size_t k = 0; k < Ndim; k++)
  {
    const size_t index = k * 4;
    input[index] = guide[k];
    input[index + 1] = mask[k];
    input[index + 2] = guide[k] * guide[k];
    input[index + 3] = guide[k] * mask[k];
  }

  /*
  * In the following, input will be blurred horizontally in a single loop
  * as a 4 channels image, to exploit data locality
  */

  // Convolve box average along columns
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(guide, mask, temp, width, height, radius, input) \
  schedule(simd:static) collapse(2)
#endif
  for(size_t i = 0; i < height; i++)
  {
    for(size_t j = 0; j < width; j++)
    {
      const size_t begin_convol = (i < radius) ? 0 : i - radius;
      size_t end_convol = i + radius;
      end_convol = (end_convol < height) ? end_convol : height - 1;
      const float num_elem = 1.0f / ((float)end_convol - (float)begin_convol + 1.0f);
      float tmp[4] DT_ALIGNED_PIXEL = { 0.0f }; // = { w_mean_I, w_mean_p, w_corr_I, w_corr_Ip }

      // convolution
      for(size_t c = begin_convol; c <= end_convol; c++)
      {
        const size_t index = (c * width + j) * 4;

#ifdef _OPENMP
#pragma omp simd reduction(+ : tmp) aligned(tmp : 16) aligned(input : 64)
#endif
        for(size_t k = 0; k < 4; ++k) tmp[k] += input[index + k];
      }

      const size_t index = (i * width + j) * 4;

      // normalization (box blur -> coeffs = 1 / number of elements)
#ifdef _OPENMP
#pragma omp simd aligned(tmp:16) aligned(temp:64)
#endif
      for(size_t c = 0; c < 4; c++)
        temp[index + c] = tmp[c] * num_elem;
    }
  }

  if(input != NULL) dt_free_align(input);

  /*
  * In the following, temp will be blurred vertically in a single loop
  * as a 4 channels image, to exploit data locality
  */

  // Convolve box average along rows and output result
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(ab, temp, width, height, radius, feathering) \
  schedule(simd:static) collapse(2)
#endif
  for(size_t i = 0; i < height; i++)
  {
    for(size_t j = 0; j < width; j++)
    {
      const size_t begin_convol = (j < radius) ? 0 : j - radius;
      size_t end_convol = j + radius;
      end_convol = (end_convol < width) ? end_convol : width - 1;
      const float num_elem = 1.0f / ((float)end_convol - (float)begin_convol + 1.0f);
      float tmp[4] DT_ALIGNED_PIXEL = { 0.0f }; // = { w_mean_I, w_mean_p, w_corr_I, w_corr_Ip }

      // convolution
      for(size_t c = begin_convol; c <= end_convol; c++)
      {
        const size_t index = (i * width + c) * 4;

#ifdef _OPENMP
#pragma omp simd aligned(temp : 64) aligned(tmp : 16) reduction(+ : tmp)
#endif
        for(size_t k = 0; k < 4; ++k) tmp[k] += temp[index + k];
      }

      // normalization (box blur -> coeffs = 1 / number of elements)
#ifdef _OPENMP
#pragma omp simd aligned(tmp:16) reduction(*:tmp)
#endif
      for(size_t c = 0; c < 4; c++)
        tmp[c] *= num_elem;

      // compute blending coeffs and output
      const size_t index = (i * width + j) * 2;
      const float d = fmaxf((tmp[2] - tmp[0] * tmp[0]) + feathering, 1e-15f); // avoid division by 0.
      const float a = (tmp[3] - tmp[0] * tmp[1]) / d;
      const float b = tmp[1] - a * tmp[0];
      const float ab_temp[2] DT_ALIGNED_PIXEL = { a, b };

#ifdef _OPENMP
#pragma omp simd aligned(ab_temp:16) aligned(ab:64)
#endif
      for(size_t c = 0; c < 2; c++)
        ab[index + c] = ab_temp[c];
    }
  }

  if(temp != NULL) dt_free_align(temp);
}


__DT_CLONE_TARGETS__
static inline void box_average(float *const restrict in,
                               const size_t width, const size_t height, const size_t ch,
                               const int radius)
{
  // Compute in-place a box average (filter) on a multi-channel image over a window of size 2*radius + 1
  // We make use of the separable nature of the filter kernel to speed-up the computation
  // by convolving along columns and rows separately (complexity O(2 × radius) instead of O(radius²)).

  assert(ch <= 4);

  const size_t Ndim = width * height * ch;
  float *const restrict temp = dt_alloc_sse_ps(Ndim);

  // Convolve box average along columns
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(in, temp, width, height, ch, radius) \
  schedule(simd:static) collapse(2)
#endif
  for(size_t j = 0; j < width; j++)
  {
    for(size_t i = 0; i < height; i++)
    {
      const size_t begin_convol = (i < radius) ? 0 : i - radius;
      size_t end_convol = i + radius;
      end_convol = (end_convol < height) ? end_convol : height - 1;
      const float num_elem = (float)end_convol - (float)begin_convol + 1.0f;
      const size_t index = (i * width + j) * ch;

      float w[4] DT_ALIGNED_PIXEL = { 0.0f };

      // Convolve
      for(size_t c = begin_convol; c <= end_convol; c++)
      {
        const size_t index_c = (c * width + j) * ch;
#ifdef _OPENMP
#pragma omp simd aligned(in:64) aligned(w:16) reduction(+:w)
#endif
        for(size_t k = 0; k < ch; ++k)
          w[k] += in[index_c + k];
      }

    // Normalize and Save
#ifdef _OPENMP
#pragma omp simd aligned(temp:64) aligned(w:16)
#endif
      for(size_t k = 0; k < ch; ++k)
        temp[index + k] = w[k] / num_elem;
    }
  }

  // Convolve box average along rows and output result
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(in, temp, width, height, ch, radius) \
  schedule(simd:static) collapse(2)
#endif
  for(size_t i = 0; i < height; i++)
  {
    for(size_t j = 0; j < width; j++)
    {
      const size_t begin_convol = (j < radius) ? 0 : j - radius;
      size_t end_convol = j + radius;
      end_convol = (end_convol < width) ? end_convol : width - 1;
      const float num_elem = (float)end_convol - (float)begin_convol + 1.0f;
      const size_t stride = i * width;
      const size_t index = (stride + j) * ch;

      float w[4] DT_ALIGNED_PIXEL = { 0.0f };

      // Convolve
      for(size_t c = begin_convol; c <= end_convol; c++)
      {
        const size_t index_c = (stride + c) * ch;
#ifdef _OPENMP
#pragma omp simd aligned(temp:64) aligned(w:16) reduction(+:w)
#endif
        for(size_t k = 0; k < ch; ++k)
          w[k] += temp[index_c + k];
      }

      // Normalize and Save
#ifdef _OPENMP
#pragma omp simd aligned(w:16) aligned(in:64)
#endif
      for(size_t k = 0; k < ch; ++k)
        in[index + k] = w[k] / num_elem;
    }
  }

  if(temp != NULL) dt_free_align(temp);
}


__DT_CLONE_TARGETS__
static inline void apply_linear_blending(float *const restrict image,
                                         const float *const restrict ab,
                                         const size_t num_elem)
{
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
dt_omp_firstprivate(image, ab, num_elem) \
schedule(simd:static) aligned(image, ab:64)
#endif
  for(size_t k = 0; k < num_elem; k++)
  {
    // Note : image[k] is positive at the outside of the luminance mask
    image[k] = fmaxf(image[k] * ab[k * 2] + ab[k * 2 + 1], MIN_FLOAT);
  }
}


__DT_CLONE_TARGETS__
static inline void apply_linear_blending_w_geomean(float *const restrict image,
                                                   const float *const restrict ab,
                                                   const size_t num_elem)
{
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
dt_omp_firstprivate(image, ab, num_elem) \
schedule(simd:static) aligned(image, ab:64)
#endif
  for(size_t k = 0; k < num_elem; k++)
  {
    // Note : image[k] is positive at the outside of the luminance mask
    image[k] = sqrtf(image[k] * fmaxf(image[k] * ab[k * 2] + ab[k * 2 + 1], MIN_FLOAT));
  }
}


__DT_CLONE_TARGETS__
static inline void quantize(const float *const restrict image,
                            float *const restrict out,
                            const size_t num_elem,
                            const float sampling, const float clip_min, const float clip_max)
{
  // Quantize in exposure levels evenly spaced in log by sampling

  if(sampling == 0.0f)
  {
    // No-op
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
dt_omp_firstprivate(image, out, num_elem, sampling, clip_min, clip_max) \
schedule(simd:static) aligned(image, out:64)
#endif
    for(size_t k = 0; k < num_elem; k++)
      out[k] = image[k];
  }
  else if(sampling == 1.0f)
  {
    // fast track
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
dt_omp_firstprivate(image, out, num_elem, sampling, clip_min, clip_max) \
schedule(simd:static) aligned(image, out:64)
#endif
    for(size_t k = 0; k < num_elem; k++)
      out[k] = fast_clamp(exp2f(floorf(log2f(image[k]))), clip_min, clip_max);
  }

  else
  {
    // slow track
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
dt_omp_firstprivate(image, out, num_elem, sampling, clip_min, clip_max) \
schedule(simd:static) aligned(image, out:64)
#endif
    for(size_t k = 0; k < num_elem; k++)
      out[k] = fast_clamp(exp2f(floorf(log2f(image[k]) / sampling) * sampling), clip_min, clip_max);
  }
}


__DT_CLONE_TARGETS__
static inline void fast_surface_blur(float *const restrict image,
                                      const size_t width, const size_t height,
                                      const int radius, float feathering, const int iterations,
                                      const dt_iop_guided_filter_blending_t filter, const float scale,
                                      const float quantization, const float quantize_min, const float quantize_max)
{
  // Works in-place on a grey image

  // A down-scaling of 4 seems empirically safe and consistent no matter the image zoom level
  // see reference paper above for proof.
  const float scaling = 4.0f;
  int ds_radius = (radius < 4) ? 1 : radius / scaling;

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
    dt_control_log(_("fast guided filter failed to allocate memory, check your RAM settings"));
    goto clean;
  }

  // Downsample the image for speed-up
  interpolate_bilinear(image, width, height, ds_image, ds_width, ds_height, 1);

  // Iterations of filter models the diffusion, sort of
  for(int i = 0; i < iterations; ++i)
  {
    // (Re)build the mask from the quantized image to help guiding
    quantize(ds_image, ds_mask, ds_width * ds_height, quantization, quantize_min, quantize_max);

    // Perform the patch-wise variance analyse to get
    // the a and b parameters for the linear blending s.t. mask = a * I + b
    variance_analyse(ds_mask, ds_image, ds_ab, ds_width, ds_height, ds_radius, feathering);

    // Compute the patch-wise average of parameters a and b
    box_average(ds_ab, ds_width, ds_height, 2, ds_radius);

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
