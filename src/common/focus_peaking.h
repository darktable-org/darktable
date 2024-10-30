/*
    This file is part of darktable,
    Copyright (C) 2019-2024 darktable developers.

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

#include "common/box_filters.h"
#include "common/fast_guided_filter.h"
#include "develop/openmp_maths.h"

/* NOTE: this code complies with the optimizations in "common/extra_optimizations.h".
 * Consider including that at the beginning of a *.c file where you use this
 * header (provided the rest of the code complies).
 **/

DT_OMP_DECLARE_SIMD()
static inline float _uint8_to_float(const uint8_t i)
{
  return (float)i / 255.0f;
}

DT_OMP_DECLARE_SIMD(aligned(image, index:64) uniform(image))
static inline float _laplacian(const float *const image, const size_t index[8])
{
  // Compute the magnitude of the gradient over the principal directions,
  // then again over the diagonal directions, and average both.
  const float l1 = dt_fast_hypotf(image[index[4]] - image[index[3]], image[index[6]] - image[index[1]]);
  const float l2 = dt_fast_hypotf(image[index[7]] - image[index[0]], image[index[5]] - image[index[2]]);
  //const float div = fabsf(image[index[3]] + image[index[4]] + image[index[1]] + image[index[6]] - 4.0f * image[index[3] + 1]) + 1.0f;

  // we assume the gradients follow an hyper-laplacian distributions in natural images,
  // which is baked by some examples the literature, but is still very hacky
  // https://www.sciencedirect.com/science/article/pii/S0165168415004168
  // http://citeseerx.ist.psu.edu/viewdoc/download?doi=10.1.1.154.539&rep=rep1&type=pdf
  return (l1 + l2) / 2.0f;
}

DT_OMP_DECLARE_SIMD()
static inline void _get_indices(const size_t i,
                               const size_t j,
                               const size_t width,
                               const size_t height,
                               const size_t delta,
                               size_t index[8])
{
  const size_t upper_line = (i - delta) * width;
  const size_t center_line = i * width;
  const size_t lower_line = (i + delta) * width;
  const size_t left_row = j - delta;
  const size_t right_row = j + delta;

  index[0] = upper_line + left_row;       // north west
  index[1] = upper_line + j;              // north
  index[2] = upper_line + right_row;      // north east
  index[3] = center_line + left_row;      // west
  index[4] = center_line + right_row;     // east
  index[5] = lower_line + left_row;       // south west
  index[6] = lower_line + j;              // south
  index[7] = lower_line + right_row;      // south east
}

static inline void dt_focuspeaking(cairo_t *cr,
                                   const int buf_width,
                                   const int buf_height,
                                   uint8_t *const restrict image)
{
  float *const restrict luma = dt_alloc_align_float((size_t)buf_width * buf_height);
  uint8_t *const restrict focus_peaking = dt_alloc_align_uint8(buf_width * buf_height * 4);

  const size_t npixels = (size_t)buf_height * buf_width;
  // Create a luma buffer as the euclidian norm of RGB channels
  DT_OMP_FOR_SIMD(aligned(image, luma:64))
  for(size_t index = 0; index < npixels; index++)
    {
      const size_t index_RGB = index * 4;

      // remove gamma 2.2 and take the square is equivalent to this:
      const float exponent = 2.0f * 2.2f;

      luma[index] = sqrtf( powf(_uint8_to_float(image[index_RGB]), exponent) +
                           powf(_uint8_to_float(image[index_RGB + 1]), exponent) +
                           powf(_uint8_to_float(image[index_RGB + 2]), exponent) );
    }

  // Prefilter noise
  fast_surface_blur(luma, buf_width, buf_height, 12, 0.00001f, 4, DT_GF_BLENDING_LINEAR, 1, 0.0f, exp2f(-8.0f), 1.0f);

  // Compute the gradients magnitudes
  float *const restrict luma_ds =  dt_alloc_align_float((size_t)buf_width * buf_height);
  DT_OMP_FOR(collapse(2))
  for(size_t i = 0; i < buf_height; ++i)
    for(size_t j = 0; j < buf_width; ++j)
    {
      size_t index = i * buf_width + j;
      if (i < 2 || i >= buf_height - 2 || j < 2 || j > buf_width -2)
        // ensure defined value for borders
        luma_ds[index] = 0.0f;
      else
      {
        size_t DT_ALIGNED_ARRAY index_close[8];
        _get_indices(i, j, buf_width, buf_height, 1, index_close);

        size_t DT_ALIGNED_ARRAY index_far[8];
        _get_indices(i, j, buf_width, buf_height, 2, index_far);

        // Computing the gradient on the closest neighbours gives us the rate of variation, but doesn't say if we are
        // looking at local contrast or optical sharpness.
        // so we compute again the gradient on neighbours a bit further.
        // if both gradients have the same magnitude, it means we have no sharpness but just a big step in intensity,
        // aka local contrast. If the closest is higher than the farthest, is means we have indeed a sharp something,
        // either noise or edge. To mitigate that, we just subtract half the farthest gradient but add a noise threshold
        luma_ds[index] = _laplacian(luma, index_close) - 0.67f * (_laplacian(luma, index_far) - 0.00390625f);
      }
    }

  // Anti-aliasing
  dt_box_mean(luma_ds, buf_height, buf_width, 1, 2, 1);

  // Compute the gradient mean over the picture
  float TV_sum = 0.0f;

  DT_OMP_FOR_SIMD(collapse(2) aligned(luma_ds:64) reduction(+:TV_sum))
  for(size_t i = 2; i < buf_height - 2; ++i)
    for(size_t j = 2; j < buf_width - 2; ++j)
      TV_sum += luma_ds[i * buf_width + j];

  TV_sum /= (float)(buf_height - 4) * (float)(buf_width - 4);

  // Compute the predicator of the hyper-laplacian distribution
  // (similar to the standard deviation if we had a gaussian distribution)
  float sigma = 0.0f;

  DT_OMP_FOR_SIMD(collapse(2) aligned(focus_peaking, luma_ds:64) reduction(+:sigma))
  for(size_t i = 2; i < buf_height - 2; ++i)
    for(size_t j = 2; j < buf_width - 2; ++j)
       sigma += fabsf(luma_ds[i * buf_width + j] - TV_sum);

  sigma /= (float)(buf_height - 4) * (float)(buf_width - 4);

  // Set the sharpness thresholds
  const float six_sigma = TV_sum + 10.0f * sigma;
  const float four_sigma = TV_sum + 5.0f * sigma;
  const float two_sigma = TV_sum + 2.5f * sigma;

  // Postfilter to connect isolated dots and draw lines
  fast_surface_blur(luma_ds, buf_width, buf_height, 12, 0.00001f, 4, DT_GF_BLENDING_LINEAR, 1, 0.0f, exp2f(-8.0f), 1.0f);

  // Prepare the focus-peaking image overlay
  DT_OMP_FOR(collapse(2))
  for(size_t i = 0; i < buf_height; ++i)
    for(size_t j = 0; j < buf_width; ++j)
    {
      static const uint8_t yellow[4] = { 0/*B*/, 255/*G*/, 255/*R*/, 255/*alpha*/ };
      static const uint8_t green[4] = { 0, 255, 0, 255 };
      static const uint8_t blue[4] = { 255, 0, 0, 255 };

      const size_t index = (i * buf_width + j) * 4;
      const float TV = luma_ds[index / 4];

      if(TV > six_sigma)
      {
        // Very sharp : paint yellow, BGR = (0, 255, 255)
        for_four_channels(c) focus_peaking[index + c] = yellow[c];
      }
      else if(TV > four_sigma)
      {
        // Mediun sharp : paint green, BGR = (0, 255, 0)
        for_four_channels(c) focus_peaking[index + c] = green[c];
      }
      else if(TV > two_sigma)
      {
        // Little sharp : paint blue, BGR = (255, 0, 0)
        for_four_channels(c) focus_peaking[index + c] = blue[c];
      }
      else
      {
        // Not sharp enough : paint 0
        for_four_channels(c) focus_peaking[index + c] = 0;
      }
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
  cairo_pattern_set_filter(cairo_get_source (cr), darktable.gui->filter_image);
  cairo_fill(cr);
  cairo_restore(cr);

  // cleanup
  cairo_surface_destroy(surface);
  dt_free_align(luma);
  dt_free_align(luma_ds);
  dt_free_align(focus_peaking);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
