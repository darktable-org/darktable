
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

#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "common/darktable.h"
#include "develop/imageop_math.h"


/* NOTE: this code complies with the optimizations in "common/extra_optimizations.h".
 * Consider including that at the beginning of a *.c file where you use this
 * header (provided the rest of the code complies).
 **/


#define MIN_FLOAT exp2f(-16.0f)


typedef enum dt_iop_luminance_mask_method_t
{
  DT_TONEEQ_MEAN = 0,   // $DESCRIPTION: "RGB average"
  DT_TONEEQ_LIGHTNESS,  // $DESCRIPTION: "HSL lightness"
  DT_TONEEQ_VALUE,      // $DESCRIPTION: "HSV value / RGB max"
  DT_TONEEQ_NORM_1,     // $DESCRIPTION: "RGB sum"
  DT_TONEEQ_NORM_2,     // $DESCRIPTION: "RGB euclidean norm")
  DT_TONEEQ_NORM_POWER, // $DESCRIPTION: "RGB power norm"
  DT_TONEEQ_GEOMEAN,    // $DESCRIPTION: "RGB geometric mean"
  DT_TONEEQ_LAST
} dt_iop_luminance_mask_method_t;

/**
 * DOCUMENTATION
 *
 * Lightness map computation
 *
 * Flatten an RGB image into a lightness/luminance map (grey image) using several
 * vector norms, and other pseudo-norms.
 *
 * These functions are all written to be vectorizable, using the base image pointer and
 * the explicit index of the current pixel. They perform exposure and contrast compensation
 * as well, for better cache handling.
 *
 * Buffers need to be 64-bits aligned.
 *
 * The outputs are clipped to avoid negative and close-to-zero results that could
 * backfire in the exposure computations.
 **/

#ifdef _OPENMP
#pragma omp declare simd
#endif
__DT_CLONE_TARGETS__
static float linear_contrast(const float pixel, const float fulcrum, const float contrast)
{
  // Increase the slope of the value around a fulcrum value
  return fmaxf((pixel - fulcrum) * contrast + fulcrum, MIN_FLOAT);
}


#ifdef _OPENMP
#pragma omp declare simd aligned(image, luminance:64) uniform(image, luminance)
#endif
__DT_CLONE_TARGETS__
static void pixel_rgb_mean(const float *const restrict image,
                           float *const restrict luminance,
                           const size_t k, const size_t ch,
                           const float exposure_boost,
                           const float fulcrum, const float contrast_boost)
{
  // mean(RGB) is the intensity

  float lum = 0.0f;

#ifdef _OPENMP
#pragma omp simd reduction(+:lum) aligned(image:64)
#endif
  for(int c = 0; c < 3; ++c)
    lum += image[k + c];

  luminance[k / ch] = linear_contrast(exposure_boost * lum / 3.0f, fulcrum, contrast_boost);
}


#ifdef _OPENMP
#pragma omp declare simd aligned(image, luminance:64) uniform(image, luminance)
#endif
__DT_CLONE_TARGETS__
static void pixel_rgb_value(const float *const restrict image,
                            float *const restrict luminance,
                            const size_t k, const size_t ch,
                            const float exposure_boost,
                            const float fulcrum, const float contrast_boost)
{
  // max(RGB) is equivalent to HSV value

  const float lum = exposure_boost * fmaxf(fmaxf(image[k], image[k + 1]), image[k + 2]);
  luminance[k / ch] = linear_contrast(lum, fulcrum, contrast_boost);
}


#ifdef _OPENMP
#pragma omp declare simd aligned(image, luminance:64) uniform(image, luminance)
#endif
__DT_CLONE_TARGETS__
static void pixel_rgb_lightness(const float *const restrict image,
                                float *const restrict luminance,
                                const size_t k, const size_t ch,
                                const float exposure_boost,
                                const float fulcrum, const float contrast_boost)
{
  // (max(RGB) + min(RGB)) / 2 is equivalent to HSL lightness

  const float max_rgb = fmaxf(fmaxf(image[k], image[k + 1]), image[k + 2]);
  const float min_rgb = fminf(fminf(image[k], image[k + 1]), image[k + 2]);
  luminance[k / ch] = linear_contrast(exposure_boost * (max_rgb + min_rgb) / 2.0f, fulcrum, contrast_boost);
}

#ifdef _OPENMP
#pragma omp declare simd aligned(image, luminance:64) uniform(image, luminance)
#endif
__DT_CLONE_TARGETS__
static void pixel_rgb_norm_1(const float *const restrict image,
                             float *const restrict luminance,
                             const size_t k, const size_t ch,
                             const float exposure_boost,
                             const float fulcrum, const float contrast_boost)
{
  // vector norm L1

  float lum = 0.0f;

  #ifdef _OPENMP
  #pragma omp simd reduction(+:lum) aligned(image:64)
  #endif
    for(int c = 0; c < 3; ++c)
      lum += fabsf(image[k + c]);

  luminance[k / ch] = linear_contrast(exposure_boost * lum, fulcrum, contrast_boost);
}


#ifdef _OPENMP
#pragma omp declare simd aligned(image, luminance:64) uniform(image, luminance)
#endif
__DT_CLONE_TARGETS__
static void pixel_rgb_norm_2(const float *const restrict image,
                             float *const restrict luminance,
                             const size_t k, const size_t ch,
                             const float exposure_boost,
                             const float fulcrum, const float contrast_boost)
{
  // vector norm L2 : euclidean norm

  float result = 0.0f;

#ifdef _OPENMP
#pragma omp simd aligned(image:64) reduction(+: result)
#endif
  for(int c = 0; c < 3; ++c) result += image[k + c] * image[k + c];

  luminance[k / ch] = linear_contrast(exposure_boost * sqrtf(result), fulcrum, contrast_boost);
}


#ifdef _OPENMP
#pragma omp declare simd aligned(image, luminance:64) uniform(image, luminance)
#endif
__DT_CLONE_TARGETS__
static void pixel_rgb_norm_power(const float *const restrict image,
                                 float *const restrict luminance,
                                 const size_t k, const size_t ch,
                                 const float exposure_boost,
                                 const float fulcrum, const float contrast_boost)
{
  // weird norm sort of perceptual. This is black magic really, but it looks good.

  float numerator = 0.0f;
  float denominator = 0.0f;

#ifdef _OPENMP
#pragma omp simd aligned(image:64) reduction(+:numerator, denominator)
#endif
  for(int c = 0; c < 3; ++c)
  {
    const float value = fabsf(image[k + c]);
    const float RGB_square = value * value;
    const float RGB_cubic = RGB_square * value;
    numerator += RGB_cubic;
    denominator += RGB_square;
  }

  luminance[k / ch] = linear_contrast(exposure_boost * numerator / denominator, fulcrum, contrast_boost);
}

#ifdef _OPENMP
#pragma omp declare simd aligned(image, luminance:64) uniform(image, luminance)
#endif
__DT_CLONE_TARGETS__
static void pixel_rgb_geomean(const float *const restrict image,
                              float *const restrict luminance,
                              const size_t k, const size_t ch,
                              const float exposure_boost,
                              const float fulcrum, const float contrast_boost)
{
  // geometric_mean(RGB). Kind of interesting for saturated colours (maps them to shadows)

  float lum = 1.0f;

#ifdef _OPENMP
#pragma omp simd aligned(image:64) reduction(*:lum)
#endif
  for(int c = 0; c < 3; ++c)
  {
    lum *= fabsf(image[k + c]);
  }

  luminance[k / ch] = linear_contrast(exposure_boost * powf(lum, 1.0f / 3.0f), fulcrum, contrast_boost);
}


// Overkill trick to explicitely unswitch loops
// GCC should to it automatically with "funswitch-loops" flag,
// but not sure about Clang
#ifdef _OPENMP
  #define LOOP(fn)                                                        \
    {                                                                     \
      _Pragma ("omp parallel for simd default(none) schedule(static)      \
      dt_omp_firstprivate(num_elem, ch, in, out, exposure_boost, fulcrum, contrast_boost)\
      aligned(in, out:64)" )                                              \
      for(size_t k = 0; k < num_elem; k += ch)                            \
      {                                                                   \
        fn(in, out, k, ch, exposure_boost, fulcrum, contrast_boost);      \
      }                                                                   \
      break;                                                              \
    }
#else
  #define LOOP(fn)                                                        \
    {                                                                     \
      for(size_t k = 0; k < num_elem; k += ch)                            \
      {                                                                   \
        fn(in, out, k, ch, exposure_boost, fulcrum, contrast_boost);      \
      }                                                                   \
      break;                                                              \
    }
#endif


__DT_CLONE_TARGETS__
static inline void luminance_mask(const float *const restrict in, float *const restrict out,
                           const size_t width, const size_t height, const size_t ch,
                           const dt_iop_luminance_mask_method_t method,
                           const float exposure_boost,
                           const float fulcrum, const float contrast_boost)
{
  const size_t num_elem = width * height * ch;
  switch(method)
  {
    case DT_TONEEQ_MEAN:
      LOOP(pixel_rgb_mean);

    case DT_TONEEQ_LIGHTNESS:
      LOOP(pixel_rgb_lightness);

    case DT_TONEEQ_VALUE:
      LOOP(pixel_rgb_value);

    case DT_TONEEQ_NORM_1:
      LOOP(pixel_rgb_norm_1);

    case DT_TONEEQ_NORM_2:
      LOOP(pixel_rgb_norm_2);

    case DT_TONEEQ_NORM_POWER:
      LOOP(pixel_rgb_norm_power);

    case DT_TONEEQ_GEOMEAN:
      LOOP(pixel_rgb_geomean);

    default:
      break;
  }
}


// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

