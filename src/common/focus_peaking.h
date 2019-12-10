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

#pragma once

#include "common/fast_guided_filter.h"

#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline float uint8_to_float(const uint8_t i)
{
  return (float)i / 255.0f;
}

#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline uint8_t float_to_uint8(const float i)
{
  return (uint8_t)(i * 255.0f);
}

#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline float sqf(const float x)
{
  // square
  return x * x;
}

#ifdef _OPENMP
#pragma omp declare simd aligned(image:64) uniform(image)
#endif
static inline float laplacian(const float *const image, const size_t index[8])
{
  // Compute the magnitude of the gradient over the principal directions,
  // then again over the diagonal directions, and average both.
  const float l1 = hypotf(image[index[4]] - image[index[3]], image[index[6]] - image[index[1]]);
  const float l2 = hypotf(image[index[7]] - image[index[0]], image[index[5]] - image[index[2]]);
  const float div = fabsf(image[index[3]] + image[index[4]] + image[index[1]] + image[index[6]] - 4.0f * image[index[3] + 1]) + 5e-1;
  return logf(1.0f + (l1 + l2) / (2.0f * div));
}

#ifdef _OPENMP
#pragma omp declare simd
#endif
static inline void get_indices(const size_t i, const size_t j, const size_t width, const size_t height, size_t index[8])
{
  const size_t upper_line = (i - 1) * width;
  const size_t center_line = upper_line + width;
  const size_t lower_line = center_line + width;
  const size_t left_row = j - 1;
  const size_t right_row = j + 1;

  index[0] = upper_line + left_row;       // north west
  index[1] = upper_line + j;              // north
  index[2] = upper_line + right_row;      // north east
  index[3] = center_line + left_row;      // west
  index[4] = center_line + right_row;     // east
  index[5] = lower_line + left_row;       // south west
  index[6] = lower_line + j;              // south
  index[7] = lower_line + right_row;      // south east
}

static inline void dt_focuspeaking(cairo_t *cr, int width, int height,
                                   uint8_t *const restrict image,
                                   const int buf_width, const int buf_height)
{
  float *const restrict luma =  dt_alloc_sse_ps(buf_width * buf_height);
  uint8_t *const restrict focus_peaking = dt_alloc_align(64, 4 * buf_width * buf_height * sizeof(uint8_t));

  // Create a luma buffer as the euclidian norm of RGB channels
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
dt_omp_firstprivate(image, luma, buf_height, buf_width) \
schedule(static) collapse(2) aligned(image, luma:64)
#endif
  for(size_t j = 0; j < buf_height; j++)
    for(size_t i = 0; i < buf_width; i++)
    {
      const size_t index = j * buf_width + i;
      const size_t index_RGB = index * 4;

      // remove gamma 2.2 and take the square is equivalent to this:
      const float exponent = 2.0f / 2.2f;

      luma[index] = sqrtf( powf(uint8_to_float(image[index_RGB]), exponent) +
                           powf(uint8_to_float(image[index_RGB + 1]), exponent) +
                           powf(uint8_to_float(image[index_RGB + 2]), exponent) );
    }

  // Downscale image
  const size_t buf_width_ds = buf_width / 2;
  const size_t buf_height_ds = buf_height / 2;
  float *const restrict luma_ds =  dt_alloc_sse_ps(buf_width * buf_height);
  box_average(luma, buf_width, buf_height, 1, 1);
  interpolate_bilinear(luma, buf_width, buf_height, luma_ds, buf_width_ds, buf_height_ds, 1);

  // Prefilter noise
  fast_surface_blur(luma_ds, buf_width_ds, buf_height_ds, 8, 0.001f, 4, DT_GF_BLENDING_LINEAR, 1, 0.0f, exp2f(-8.0f), 0.0f);

  // Compute the gradients magnitudes
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
dt_omp_firstprivate(luma, luma_ds, buf_height_ds, buf_width_ds) \
schedule(static) collapse(2) aligned(luma_ds, luma:64)
#endif
  for(size_t i = 1; i < buf_height_ds - 1; ++i)
    for(size_t j = 1; j < buf_width_ds - 1; ++j)
    {
      size_t index[8];
      get_indices(i, j, buf_width_ds, buf_height_ds, index);
      luma[i * buf_width_ds + j] = laplacian(luma_ds, index);
    }

  // Postfilter to join isolated dots and draw lines
  fast_surface_blur(luma, buf_width_ds, buf_height_ds, 1, 0.001f, 1, DT_GF_BLENDING_LINEAR, 1, 0.0f, exp2f(-8.0f), 0.0f);

  // Compute the gradient mean over the picture
  float TV_sum = 0.0f;

#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
dt_omp_firstprivate(luma, buf_height_ds, buf_width_ds) \
schedule(static) collapse(2) aligned(luma:64) reduction(+:TV_sum)
#endif
  for(size_t i = 1; i < buf_height_ds - 1; ++i)
    for(size_t j = 1; j < buf_width_ds - 1; ++j)
      TV_sum += luma[i * buf_width_ds + j];

  TV_sum /= (float)(buf_height_ds - 2) * (float)(buf_width_ds - 2);

  // Compute the gradient standard deviation
  float sigma = 0.0f;

#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
dt_omp_firstprivate(focus_peaking, luma, luma_ds, buf_height_ds, buf_width_ds, TV_sum) \
schedule(static) collapse(2) aligned(focus_peaking, luma_ds, luma:64) reduction(+:sigma)
#endif
  for(size_t i = 2; i < buf_height_ds - 2; ++i)
    for(size_t j = 2; j < buf_width_ds - 2; ++j)
       sigma += sqf(luma[i * buf_width_ds + j] - TV_sum);

  sigma = sqrtf(sigma / ((float)(buf_height_ds - 4) * (float)(buf_width_ds - 4)));

  // Upscale focus peaking mask
  interpolate_bilinear(luma, buf_width_ds, buf_height_ds, luma_ds, buf_width, buf_height, 1);

  // Set the sharpness thresholds
  const float six_sigma = TV_sum + 6.0f * sigma;
  const float four_sigma = TV_sum + 4.0f * sigma;
  const float two_sigma = TV_sum + 2.0f * sigma;

  sigma *= sigma;

  // Prepare the focus-peaking image overlay
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
dt_omp_firstprivate(focus_peaking, luma_ds, buf_height, buf_width, six_sigma, four_sigma, two_sigma, sigma) \
schedule(static) collapse(2) aligned(focus_peaking, luma_ds:64)
#endif
  for(size_t i = 2; i < buf_height - 2; ++i)
    for(size_t j = 2; j < buf_width - 2; ++j)
    {
      const size_t index = (i * buf_width + j) * 4;
      const float TV = luma_ds[index / 4];

      if(TV > six_sigma)
      {
        // Very sharp : paint yellow, BGR = (0, 255, 255)
        focus_peaking[index + 0] = 0;
        focus_peaking[index + 1] = 255;
        focus_peaking[index + 2] = 255;

        // alpha channel
        focus_peaking[index + 3] = 255;
      }
      else if(TV > four_sigma)
      {
        // Mediun sharp : paint green, BGR = (0, 255, 0)
        focus_peaking[index + 0] = 0;
        focus_peaking[index + 1] = 255;
        focus_peaking[index + 2] = 0;

        // alpha channel
        focus_peaking[index + 3] = 255;
      }
      else if(TV > two_sigma)
      {
        // Little sharp : paint blue, BGR = (255, 0, 0)
        focus_peaking[index + 0] = 255;
        focus_peaking[index + 1] = 0;
        focus_peaking[index + 2] = 0;

        // alpha channel
        focus_peaking[index + 3] = 255;
      }
      else
      {
        // Not sharp enough : paint 0
        focus_peaking[index + 3] = focus_peaking[index + 2] = focus_peaking[index + 1] = focus_peaking[index] = 0;
      }
    }

  // deal with image borders : top rows
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
dt_omp_firstprivate(focus_peaking, buf_height, buf_width) \
schedule(static) collapse(2) aligned(focus_peaking:64)
#endif
  for(size_t i = 0; i < 4; ++i)
    for(size_t j = 2; j < buf_width - 2; ++j)
    {
      const size_t index = (i * buf_width + j) * 4;
      focus_peaking[index + 3] = focus_peaking[index + 2] = focus_peaking[index + 1] = focus_peaking[index] = 0;
    }

  // deal with image borders : left columns
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
dt_omp_firstprivate(focus_peaking, buf_height, buf_width) \
schedule(static) collapse(2) aligned(focus_peaking:64)
#endif
  for(size_t i = 0; i < buf_height; ++i)
    for(size_t j = 0; j < 4; ++j)
    {
      const size_t index = (i * buf_width + j) * 4;
      focus_peaking[index + 3] = focus_peaking[index + 2] = focus_peaking[index + 1] = focus_peaking[index] = 0;
    }

  // deal with image borders : right columns
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
dt_omp_firstprivate(focus_peaking, buf_height, buf_width) \
schedule(static) collapse(2) aligned(focus_peaking:64)
#endif
  for(size_t i = 0; i < buf_height; ++i)
    for(size_t j = buf_width - 5; j < buf_width; ++j)
    {
      const size_t index = (i * buf_width + j) * 4;
      focus_peaking[index + 3] = focus_peaking[index + 2] = focus_peaking[index + 1] = focus_peaking[index] = 0;
    }

  // deal with image borders : bottom rows
#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
dt_omp_firstprivate(focus_peaking, buf_height, buf_width) \
schedule(static) collapse(2) aligned(focus_peaking:64)
#endif
  for(size_t i = buf_height - 5; i < buf_height; ++i)
    for(size_t j = 0; j < buf_width; ++j)
    {
      const size_t index = (i * buf_width + j) * 4;
      focus_peaking[index + 3] = focus_peaking[index + 2] = focus_peaking[index + 1] = focus_peaking[index] = 0;
    }

  // draw the focus peaking overlay
  cairo_save(cr);
  cairo_rectangle(cr, 0, 0, buf_width, buf_height);
  cairo_surface_t *surface = cairo_image_surface_create_for_data((unsigned char *)focus_peaking,
                                                                 CAIRO_FORMAT_ARGB32,
                                                                 buf_width, buf_height,
                                                                 cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, buf_width));
  cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
  cairo_set_source_surface(cr, surface, 0.0, 0.0);
  cairo_pattern_set_filter(cairo_get_source (cr), CAIRO_FILTER_FAST);
  cairo_fill(cr);
  cairo_restore(cr);

  // cleanup
  cairo_surface_destroy(surface);
  dt_free_align(luma);
  dt_free_align(luma_ds);
  dt_free_align(focus_peaking);
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
