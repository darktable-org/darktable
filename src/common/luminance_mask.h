
/*
    This file is part of darktable,
    Copyright (C) 2019-2023 darktable developers.

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
  DT_TONEEQ_NORM_2,     // $DESCRIPTION: "RGB euclidean norm"
  DT_TONEEQ_NORM_POWER, // $DESCRIPTION: "RGB power norm"
  DT_TONEEQ_GEOMEAN,    // $DESCRIPTION: "RGB geometric mean"
  DT_TONEEQ_REC709W,    // $DESCRIPTION: "Rec. 709 weights"
  DT_TONEEQ_LAST,
  DT_TONEEQ_CUSTOM,     // $DESCRIPTION: "Custom"
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

DT_OMP_DECLARE_SIMD()
__DT_CLONE_TARGETS__
static float linear_contrast(const float pixel, const float fulcrum, const float contrast)
{
  // Increase the slope of the value around a fulcrum value
  return MAX((pixel - fulcrum) * contrast + fulcrum, MIN_FLOAT);
}


DT_OMP_DECLARE_SIMD(aligned(image, luminance:64) uniform(image, luminance))
__DT_CLONE_TARGETS__
static float pixel_rgb_mean(const float *const restrict image,
                           float *const restrict luminance,
                           const size_t k)
{
  // mean(RGB) is the intensity

  float lum = 0.0f;

  DT_OMP_SIMD(reduction(+:lum) aligned(image:64))
  for(int c = 0; c < 3; ++c)
    lum += image[k + c];

  return lum / 3.0f;

  //luminance[k / 4] = linear_contrast(exposure_boost * lum / 3.0f, fulcrum, contrast_boost);
}


DT_OMP_DECLARE_SIMD(aligned(image, luminance:64) uniform(image, luminance))
__DT_CLONE_TARGETS__
static float pixel_rgb_value(const float *const restrict image,
                            float *const restrict luminance,
                            const size_t k)
{
  // max(RGB) is equivalent to HSV value

  const float lum = MAX(MAX(image[k], image[k + 1]), image[k + 2]);
  return lum;

  //luminance[k / 4] = linear_contrast(lum, fulcrum, contrast_boost);
}


DT_OMP_DECLARE_SIMD(aligned(image, luminance:64) uniform(image, luminance))
__DT_CLONE_TARGETS__
static float pixel_rgb_lightness(const float *const restrict image,
                                float *const restrict luminance,
                                const size_t k)
{
  // (max(RGB) + min(RGB)) / 2 is equivalent to HSL lightness

  const float max_rgb = MAX(MAX(image[k], image[k + 1]), image[k + 2]);
  const float min_rgb = MIN(MIN(image[k], image[k + 1]), image[k + 2]);
  return (max_rgb + min_rgb) / 2.0f;

  //luminance[k / 4] = linear_contrast(exposure_boost * (max_rgb + min_rgb) / 2.0f, fulcrum, contrast_boost);
}

DT_OMP_DECLARE_SIMD(aligned(image, luminance:64) uniform(image, luminance))
__DT_CLONE_TARGETS__
static float pixel_rgb_norm_1(const float *const restrict image,
                             float *const restrict luminance,
                             const size_t k)
{
  // vector norm L1

  float lum = 0.0f;

  DT_OMP_SIMD(reduction(+:lum) aligned(image:64))
  for(int c = 0; c < 3; ++c)
    lum += fabsf(image[k + c]);

  return lum;

  // luminance[k / 4] = linear_contrast(exposure_boost * lum, fulcrum, contrast_boost);
}


DT_OMP_DECLARE_SIMD(aligned(image, luminance:64) uniform(image, luminance))
__DT_CLONE_TARGETS__
static float pixel_rgb_norm_2(const float *const restrict image,
                             float *const restrict luminance,
                             const size_t k)
{
  // vector norm L2 : euclidean norm

  float result = 0.0f;

  DT_OMP_SIMD(aligned(image:64) reduction(+: result))
  for(int c = 0; c < 3; ++c)
    result += image[k + c] * image[k + c];

  return sqrtf(result);

  // luminance[k / 4] = linear_contrast(exposure_boost * sqrtf(result), fulcrum, contrast_boost);
}


DT_OMP_DECLARE_SIMD(aligned(image, luminance:64) uniform(image, luminance))
__DT_CLONE_TARGETS__
static float pixel_rgb_norm_power(const float *const restrict image,
                                 float *const restrict luminance,
                                 const size_t k)
{
  // weird norm sort of perceptual. This is black magic really, but it looks good.

  float numerator = 0.0f;
  float denominator = 0.0f;

  DT_OMP_SIMD(aligned(image:64) reduction(+:numerator, denominator))
  for(int c = 0; c < 3; ++c)
  {
    const float value = fabsf(image[k + c]);
    const float RGB_square = value * value;
    const float RGB_cubic = RGB_square * value;
    numerator += RGB_cubic;
    denominator += RGB_square;
  }

  return numerator / denominator;

  // luminance[k / 4] = linear_contrast(exposure_boost * numerator / denominator, fulcrum, contrast_boost);
}

DT_OMP_DECLARE_SIMD(aligned(image, luminance:64) uniform(image, luminance))
__DT_CLONE_TARGETS__
static float pixel_rgb_geomean(const float *const restrict image,
                              float *const restrict luminance,
                              const size_t k)
{
  // geometric_mean(RGB). Kind of interesting for saturated colours (maps them to shadows)

  float lum = 1.0f;

  DT_OMP_SIMD(aligned(image:64) reduction(*:lum))
  for(int c = 0; c < 3; ++c)
  {
    lum *= fabsf(image[k + c]);
  }

  return powf(lum, 1.0f / 3.0f);

  // luminance[k / 4] = linear_contrast(exposure_boost * powf(lum, 1.0f / 3.0f), fulcrum, contrast_boost);
}

DT_OMP_DECLARE_SIMD(aligned(image, luminance:64) uniform(image, luminance))
__DT_CLONE_TARGETS__
static float pixel_rgb_r709w(const float *const restrict image,
                            float *const restrict luminance,
                            const size_t k)
{
  const float lum = 0.2126 * image[k] + 0.7152 * image[k + 1] + 0.0722 * image[k + 2];
  return lum;
  // luminance[k / 4] = linear_contrast(lum, fulcrum, contrast_boost);
}


DT_OMP_DECLARE_SIMD(aligned(image, luminance:64) uniform(image, luminance))
__DT_CLONE_TARGETS__
static float pixel_rgb_custom(const float *const restrict image,
                            float *const restrict luminance,
                            const size_t k,
                            const float r_weight,
                            const float g_weight,
                            const float b_weight)
{
  const float lum = MAX(MIN_FLOAT, r_weight * image[k] + g_weight * image[k + 1] + b_weight * image[k + 2]);
  return lum;
  // luminance[k / 4] = linear_contrast(lum, fulcrum, contrast_boost);
}


// Overkill trick to explicitely unswitch loops
// GCC should to it automatically with "funswitch-loops" flag,
// but not sure about Clang
#ifdef _OPENMP
  #define LOOP(fn)                                                        \
    {                                                                     \
      _Pragma ("omp parallel for simd default(none) schedule(static)      \
      dt_omp_firstprivate(num_elem, in, out, exposure_boost, fulcrum, contrast_boost, r_weight, g_weight, b_weight)\
      reduction(min:min_lum) reduction(max:max_lum)                       \
      aligned(in, out:64)" )                                              \
      for(size_t k = 0; k < num_elem; k += 4)                             \
      {                                                                   \
        const float lum = COMPUTE_LUM(fn);                                \
        const float lum_boosted = linear_contrast(exposure_boost * lum, fulcrum, contrast_boost); \
        min_lum = MIN(min_lum, lum_boosted);                              \
        max_lum = MAX(max_lum, lum_boosted);                              \
        out[k / 4] = lum_boosted;                                         \
      }                                                                   \
      break;                                                              \
    }
#else
  #define LOOP(fn)                                                        \
    {                                                                     \
      for(size_t k = 0; k < num_elem; k += 4)                             \
      {                                                                   \
        const float lum = COMPUTE_LUM(fn);                                \
        const float lum_boosted = linear_contrast(exposure_boost * lum, fulcrum, contrast_boost); \
        min_lum = MIN(min_lum, lum_boosted);                              \
        max_lum = MAX(max_lum, lum_boosted);                              \
        out[k / 4] = lum_boosted;                                         \
      }                                                                   \
      break;                                                              \
    }
#endif


__DT_CLONE_TARGETS__
static inline void luminance_mask(const float *const restrict in,
                                  float *const restrict out,
                                  const size_t width,
                                  const size_t height,
                                  const dt_iop_luminance_mask_method_t method,
                                  const float exposure_boost,
                                  const float fulcrum,
                                  const float contrast_boost,
                                  const float r_weight,
                                  const float g_weight,
                                  const float b_weight,
                                  float *image_min_ev,
                                  float *image_max_ev)
{
  const size_t num_elem = width * height * 4;
  float min_lum = INFINITY;
  float max_lum = -INFINITY;

  printf("luminance_mask: method=%d, exposure_boost=%f, fulcrum=%f, contrast_boost=%f, r_weight=%f, g_weight=%f, b_weight=%f\n",
         method, exposure_boost, fulcrum, contrast_boost, r_weight, g_weight, b_weight);

  switch(method)
  {
    #define COMPUTE_LUM(fn) fn(in, out, k)
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

    case DT_TONEEQ_REC709W:
      LOOP(pixel_rgb_r709w);

    #undef COMPUTE_LUM
    #define COMPUTE_LUM(fn) fn(in, out, k, r_weight, g_weight, b_weight)
    case DT_TONEEQ_CUSTOM:
      LOOP(pixel_rgb_custom);

    default:
      break;
  }

  *image_min_ev = log2f(min_lum);
  *image_max_ev = log2f(max_lum);
}




// __DT_CLONE_TARGETS__
// static inline void luminance_mask2(const float *const restrict in,
//                                   float *const restrict out,
//                                   const size_t width,
//                                   const size_t height,
//                                   const dt_iop_luminance_mask_method_t method,
//                                   const float exposure_boost,
//                                   const float fulcrum,
//                                   const float contrast_boost
//                                   const float r_weight,
//                                   const float g_weight,
//                                   const float b_weight)
// {

//   const size_t num_elem = width * height * 4;
//   switch(method)
//   {
//     case DT_TONEEQ_MEAN:
//       LOOP(pixel_rgb_mean);

//     case DT_TONEEQ_LIGHTNESS:
//       LOOP(pixel_rgb_lightness);

//     case DT_TONEEQ_VALUE:
//       LOOP(pixel_rgb_value);

//     case DT_TONEEQ_NORM_1:
//       LOOP(pixel_rgb_norm_1);

//     case DT_TONEEQ_NORM_2:
//       LOOP(pixel_rgb_norm_2);

//     case DT_TONEEQ_NORM_POWER:
//       LOOP(pixel_rgb_norm_power);

//     case DT_TONEEQ_GEOMEAN:
//       LOOP(pixel_rgb_geomean);

//     case DT_TONEEQ_REC709W:
//       LOOP(pixel_rgb_r709w);

//     default:
//       break;
//   }
// }

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
