/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

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

#include <math.h>
#include <float.h>  // FLT_MAX

#include "common/gaussian.h"           // dt_gaussian_*

// Guided filter surface gaussian blurring with edge preservation.
// supports negative values for log-luminance data (any sign).
// This implementation avoids downscaling (as done in eigf) and works directly on the full-resolution image,
// ensuring higher accuracy at the cost of a roughly ~2x increased computational load.

__DT_CLONE_TARGETS__
static inline void fast_gaussian_guided_filter(float *const restrict image,
                                               const size_t width,
                                               const size_t height,
                                               const float sigma,
                                               const float feathering)
{
  const size_t num_elem = width * height;
  // holds the squared image, then its blur (the gaussian supports in-place use)
  float *const restrict blurred_image = dt_alloc_align_float(num_elem);
  float *const restrict blurred_squared_image = dt_alloc_align_float(num_elem);

  if(!blurred_image || !blurred_squared_image)
  {
    dt_control_log(_("fast gaussian guided filter failed to allocate memory, check your RAM settings"));
    goto clean;
  }

  // channels: value and squared value, with ranges for the gaussian
  float ming = FLT_MAX;
  float maxg = -FLT_MAX;
  float maxg2 = 0.0f;
  DT_OMP_FOR(reduction(max:maxg, maxg2) reduction(min:ming))
  for(size_t k = 0; k < num_elem; k++)
  {
    const float pixel_value = image[k];
    blurred_squared_image[k] = pixel_value * pixel_value;
    ming = fminf(ming, pixel_value);
    maxg = fmaxf(maxg, pixel_value);
    maxg2 = fmaxf(maxg2, pixel_value * pixel_value);
  }
  // squares of values spanning zero have a minimum of 0
  const float ming2 = 0.0f;

  // Blur the image to obtain the local average
  const float image_max[1] = { maxg };
  const float image_min[1] = { ming };
  dt_gaussian_t *g = dt_gaussian_init(width, height, 1, image_max, image_min, sigma, 0);
  if(!g)
  {
    dt_control_log(_("fast gaussian guided filter failed to initialize gaussian structure"));
    goto clean;
  }
  dt_gaussian_blur(g, image, blurred_image);
  dt_gaussian_free(g);

  // Blur the squared image to obtain the local variance
  const float squared_max[1] = { maxg2 };
  const float squared_min[1] = { ming2 };
  g = dt_gaussian_init(width, height, 1, squared_max, squared_min, sigma, 0);
  if(!g)
  {
    dt_control_log(_("fast gaussian guided filter failed to initialize gaussian structure for squared image"));
    goto clean;
  }
  dt_gaussian_blur(g, blurred_squared_image, blurred_squared_image);
  dt_gaussian_free(g);

  DT_OMP_FOR()
  for(size_t k = 0; k < num_elem; k++)
  {
    const float average = blurred_image[k];
    const float variance = fmaxf(blurred_squared_image[k] - average * average, 0.0f);
    const float a = variance / (variance + feathering);
    image[k] = a * image[k] + (1.0f - a) * average;
  }

  clean:
  dt_free_align(blurred_image);
  dt_free_align(blurred_squared_image);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on