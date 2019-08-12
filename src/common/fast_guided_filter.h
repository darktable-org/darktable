/*
    This file is part of darktable,
    copyright (c) 2019 Aurélien Pierre.

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

#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "common/darktable.h"
#include "common/sse.h"


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

__DT_CLONE_TARGETS__
static inline void interpolate_bilinear(const float *const restrict in, const size_t width_in, const size_t height_in,
                         float *const restrict out, const size_t width_out, const size_t height_out,
                         const size_t ch)
{
  // Fast vectorized bilinear interpolation on ch channels
#ifdef _OPENMP
#pragma omp parallel for simd collapse(2) default(none) schedule(static) aligned(in, out:64) \
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
                                    float *const restrict a, float *const restrict b,
                                    const size_t width, const size_t height,
                                    const int radius, const float feathering)
{
  // Compute a box average (filter) on a grey image over a window of size 2*radius + 1
  // then get the variance of the guide and covariance with its mask
  // output a and b, the linear blending params
  // p, the mask is the quantised guide I

  float *const mean_I DT_ALIGNED_ARRAY = dt_alloc_sse_ps(width * height);
  float *const mean_p DT_ALIGNED_ARRAY = dt_alloc_sse_ps(width * height);
  float *const corr_I DT_ALIGNED_ARRAY = dt_alloc_sse_ps(width * height);
  float *const corr_Ip DT_ALIGNED_ARRAY = dt_alloc_sse_ps(width * height);

  // Convolve box average along columns
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(guide, mask, mean_I, mean_p, corr_I, corr_Ip, width, height, radius) \
  schedule(static) collapse(2)
#endif
  for(size_t i = 0; i < height; i++)
  {
    for(size_t j = 0; j < width; j++)
    {
      const size_t begin_convol = (i < radius) ? 0 : i - radius;
      size_t end_convol = i + radius;
      end_convol = (end_convol < height) ? end_convol : height - 1;
      const float num_elem = (float)end_convol - (float)begin_convol + 1.0f;

      float w_mean_I = 0.0f;
      float w_mean_p = 0.0f;
      float w_corr_I = 0.0f;
      float w_corr_Ip = 0.0f;

#ifdef _OPENMP
#pragma omp simd aligned(guide, mask, mean_I, mean_p, corr_I, corr_Ip:64) reduction(+:w_mean_I, w_mean_p, w_corr_I, w_corr_Ip)
#endif
      for(size_t c = begin_convol; c <= end_convol; c++)
      {
        w_mean_I += guide[c * width + j];
        w_mean_p += mask[c * width + j];
        w_corr_I += guide[c * width + j] * guide[c * width + j];
        w_corr_Ip += mask[c * width + j] * guide[c * width + j];
      }

      mean_I[i * width + j] = w_mean_I / num_elem;
      mean_p[i * width + j] = w_mean_p / num_elem;
      corr_I[i * width + j] = w_corr_I / num_elem;
      corr_Ip[i * width + j] = w_corr_Ip / num_elem;
    }
  }

  // Convolve box average along rows and output result
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(a, b, mean_I, mean_p, corr_I, corr_Ip, width, height, radius, feathering) \
  schedule(static) collapse(2)
#endif
  for(size_t i = 0; i < height; i++)
  {
    for(size_t j = 0; j < width; j++)
    {
      const size_t begin_convol = (j < radius) ? 0 : j - radius;
      size_t end_convol = j + radius;
      end_convol = (end_convol < width) ? end_convol : width - 1;
      const float num_elem = (float)end_convol - (float)begin_convol + 1.0f;

      float w_mean_I = 0.0f;
      float w_mean_p = 0.0f;
      float w_corr_I = 0.0f;
      float w_corr_Ip = 0.0f;

#ifdef _OPENMP
#pragma omp simd aligned(a, b, mean_I, mean_p, corr_I, corr_Ip:64) reduction(+:w_mean_I, w_mean_p, w_corr_I, w_corr_Ip)
#endif
      for(size_t c = begin_convol; c <= end_convol; c++)
      {
        w_mean_I += mean_I[i * width + c];
        w_mean_p += mean_p[i * width + c];
        w_corr_I += corr_I[i * width + c];
        w_corr_Ip += corr_Ip[i * width + c];
      }

      w_mean_I /= num_elem;
      w_mean_p /= num_elem;
      w_corr_I /= num_elem;
      w_corr_Ip /= num_elem;

      float var_I = w_corr_I - w_mean_I * w_mean_I;
      float cov_Ip = w_corr_Ip - w_mean_I * w_mean_p;

      a[i * width + j] = cov_Ip / (var_I + feathering);
      b[i * width + j] = w_mean_p - a[i * width + j] * w_mean_I;
    }
  }

  dt_free_align(mean_I);
  dt_free_align(mean_p);
  dt_free_align(corr_I);
  dt_free_align(corr_Ip);
}


__DT_CLONE_TARGETS__
static inline void box_average(float *const restrict in,
                               const size_t width, const size_t height,
                               const int radius)
{
  // Compute in-place a box average (filter) on a grey image over a window of size 2*radius + 1
  // We make use of the separable nature of the filter kernel to speed-up the computation
  // by convolving along columns and rows separately (complexity O(2 × radius) instead of O(radius²)).

  float *const temp DT_ALIGNED_ARRAY = dt_alloc_sse_ps(width * height);

  // Convolve box average along columns
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(in, temp, width, height, radius) \
  schedule(static) collapse(2)
#endif
  for(size_t i = 0; i < height; i++)
  {
    for(size_t j = 0; j < width; j++)
    {
      const size_t begin_convol = (i < radius) ? 0 : i - radius;
      size_t end_convol = i + radius;
      end_convol = (end_convol < height) ? end_convol : height - 1;
      const float num_elem = (float)end_convol - (float)begin_convol + 1.0f;

      float w = 0.0f;

#ifdef _OPENMP
#pragma omp simd aligned(in, temp:64) reduction(+:w)
#endif
      for(size_t c = begin_convol; c <= end_convol; c++)
      {
        w += in[c * width + j];
      }

      temp[i * width + j] = w / num_elem;
    }
  }

  // Convolve box average along rows and output result
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(in, temp, width, height, radius) \
  schedule(static) collapse(2)
#endif
  for(size_t i = 0; i < height; i++)
  {
    for(size_t j = 0; j < width; j++)
    {
      const size_t begin_convol = (j < radius) ? 0 : j - radius;
      size_t end_convol = j + radius;
      end_convol = (end_convol < width) ? end_convol : width - 1;
      const float num_elem = (float)end_convol - (float)begin_convol + 1.0f;

      float w = 0.0f;

#ifdef _OPENMP
#pragma omp simd aligned(in, temp:64) reduction(+:w)
#endif
      for(size_t c = begin_convol; c <= end_convol; c++)
      {
        w += temp[i * width + c];
      }

      in[i * width + j] = w / num_elem;
    }
  }

  dt_free_align(temp);
}


__DT_CLONE_TARGETS__
static inline void apply_linear_blending(float *const restrict image,
                                    const float *const restrict a,
                                    const float *const restrict b,
                                    const size_t num_elem)
{
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
dt_omp_firstprivate(image, a, b, num_elem) \
schedule(static) aligned(image, a, b:64)
#endif
  for(size_t k = 0; k < num_elem; k++)
  {
    // Note : image[k] is positive at the outside of the luminance mask
    image[k] = fmaxf(image[k] * a[k] + b[k], MIN_FLOAT);
  }
}


__DT_CLONE_TARGETS__
static inline void apply_linear_blending_w_geomean(float *const restrict image,
                                                   const float *const restrict a,
                                                   const float *const restrict b,
                                                   const size_t num_elem)
{
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
dt_omp_firstprivate(image, a, b, num_elem) \
schedule(static) aligned(image, a, b:64)
#endif
  for(size_t k = 0; k < num_elem; k++)
  {
    // Note : image[k] is positive at the outside of the luminance mask
    image[k] = sqrtf(image[k] * fmaxf(image[k] * a[k] + b[k], MIN_FLOAT));
  }
}


__DT_CLONE_TARGETS__
static inline void quantize(const float *const restrict image,
                            float *const restrict out,
                            const size_t num_elem,
                            const float sampling)
{
  // Quantize in exposure levels evenly spaced in log by sampling

  if(sampling == 0.0f)
  {
    // No-op
    dt_simd_memcpy(image, out, num_elem);
  }

  else if(sampling == 1.0f)
  {
    // fast track
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
dt_omp_firstprivate(image, out, num_elem, sampling) \
schedule(static) aligned(image, out:64)
#endif
    for(size_t k = 0; k < num_elem; k++)
      out[k] = exp2f(floorf(log2f(image[k])));
  }

  else
  {
    // slow track
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
dt_omp_firstprivate(image, out, num_elem, sampling) \
schedule(static) aligned(image, out:64)
#endif
    for(size_t k = 0; k < num_elem; k++)
      out[k] = exp2f(floorf(log2f(image[k]) / sampling) * sampling);
  }
}


__DT_CLONE_TARGETS__
static inline void fast_guided_filter(float *const restrict image,
                                 const size_t width, const size_t height,
                                 const int radius, float feathering, const int iterations,
                                 const dt_iop_guided_filter_blending_t filter, const float scale,
                                 const float quantization)
{
  // Works in-place on a grey image

  // A down-scaling of 4 seems empirically safe and consistent no matter the image zoom level
  // see reference paper above for proof.
  const float scaling = 4.0f;
  const size_t ds_height = height / scaling;
  const size_t ds_width = width / scaling;
  int ds_radius = (radius < 4) ? 1 : radius / scaling;

  // Downsample the image for speed-up
  float *ds_image DT_ALIGNED_ARRAY = dt_alloc_sse_ps(ds_width * ds_height);
  interpolate_bilinear(image, width, height, ds_image, ds_width, ds_height, 1);

  float *ds_mask DT_ALIGNED_ARRAY = dt_alloc_sse_ps(ds_width * ds_height);
  float *ds_a DT_ALIGNED_ARRAY = dt_alloc_sse_ps(ds_width * ds_height);
  float *ds_b DT_ALIGNED_ARRAY = dt_alloc_sse_ps(ds_width * ds_height);

  // Iterations of filter models the diffusion, sort of
  for(int i = 0; i < iterations; ++i)
  {
    // (Re)build the mask from the quantized image to help guiding
    quantize(ds_image, ds_mask, ds_width * ds_height, quantization);

    // Perform the patch-wise variance analyse to get
    // the a and b parameters for the linear blending s.t. mask = a * I + b
    variance_analyse(ds_image, ds_mask, ds_a, ds_b, ds_width, ds_height, ds_radius, feathering);

    // Compute the patch-wise average of parameters a and b
    box_average(ds_a, ds_width, ds_height, ds_radius);
    box_average(ds_b, ds_width, ds_height, ds_radius);

    if(i != iterations - 1)
    {
      // Process the intermediate filtered image
      apply_linear_blending(ds_image, ds_a, ds_b, ds_width * ds_height);
    }
    else
    {
      // Increase the radius for the next iteration
      ds_radius *= sqrtf(2.0f);
    }
  }
  dt_free_align(ds_mask);
  dt_free_align(ds_image);

  // Upsample the blending parameters a and b
  const size_t num_elem_2 = width * height;
  float *a DT_ALIGNED_ARRAY = dt_alloc_sse_ps(num_elem_2);
  float *b DT_ALIGNED_ARRAY = dt_alloc_sse_ps(num_elem_2);
  interpolate_bilinear(ds_a, ds_width, ds_height, a, width, height, 1);
  interpolate_bilinear(ds_b, ds_width, ds_height, b, width, height, 1);
  dt_free_align(ds_a);
  dt_free_align(ds_b);

  // Finally, blend the guided image
  if(filter == DT_GF_BLENDING_LINEAR)
    apply_linear_blending(image, a, b, num_elem_2);
  else if(filter == DT_GF_BLENDING_GEOMEAN)
    apply_linear_blending_w_geomean(image, a, b, num_elem_2);

  dt_free_align(a);
  dt_free_align(b);
}
