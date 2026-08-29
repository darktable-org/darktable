/*
    This file is part of darktable,
    Copyright (C) 2025 darktable developers.

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

#include "common.h"

// keep in sync with src/common/luminance_mask.h and src/iop/toneequal.c
#define TONEEQ_MIN_FLOAT 0x1p-16f     // exp2f(-16.0f)
#define TONEEQ_MIN_EV (-8.0f)
#define TONEEQ_MAX_EV (0.0f)
#define TONEEQ_PIXEL_CHAN 8

// dt_iop_luminance_mask_method_t
#define TONEEQ_MEAN 0
#define TONEEQ_LIGHTNESS 1
#define TONEEQ_VALUE 2
#define TONEEQ_NORM_1 3
#define TONEEQ_NORM_2 4
#define TONEEQ_NORM_POWER 5
#define TONEEQ_GEOMEAN 6

// dt_iop_guided_filter_blending_t
#define TONEEQ_BLENDING_LINEAR 0
#define TONEEQ_BLENDING_GEOMEAN 1


static inline float _linear_contrast(const float pixel,
                                     const float fulcrum,
                                     const float contrast)
{
  // Increase the slope of the value around a fulcrum value
  return fmax((pixel - fulcrum) * contrast + fulcrum, TONEEQ_MIN_FLOAT);
}


/* Flatten an RGB image into a luminance map, applying exposure and contrast
   compensation on the fly. Mirrors luminance_mask() from common/luminance_mask.h */
kernel void toneequal_luminance_mask(read_only image2d_t in,
                                     global float *const luminance,
                                     const int width,
                                     const int height,
                                     const int method,
                                     const float exposure_boost,
                                     const float fulcrum,
                                     const float contrast_boost)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;

  const float4 pixel = read_imagef(in, samplerA, (int2)(x, y));
  float lum;

  switch(method)
  {
    case TONEEQ_LIGHTNESS:
    {
      // (max(RGB) + min(RGB)) / 2 is equivalent to HSL lightness
      const float max_rgb = fmax(fmax(pixel.x, pixel.y), pixel.z);
      const float min_rgb = fmin(fmin(pixel.x, pixel.y), pixel.z);
      lum = exposure_boost * (max_rgb + min_rgb) / 2.0f;
      break;
    }

    case TONEEQ_VALUE:
      // max(RGB) is equivalent to HSV value
      lum = exposure_boost * fmax(fmax(pixel.x, pixel.y), pixel.z);
      break;

    case TONEEQ_NORM_1:
      // vector norm L1
      lum = exposure_boost * (fabs(pixel.x) + fabs(pixel.y) + fabs(pixel.z));
      break;

    case TONEEQ_NORM_2:
      // vector norm L2 : euclidean norm
      lum = exposure_boost
        * dtcl_sqrt(pixel.x * pixel.x + pixel.y * pixel.y + pixel.z * pixel.z);
      break;

    case TONEEQ_NORM_POWER:
    {
      // weird norm sort of perceptual. This is black magic really, but it looks good.
      const float4 value = fabs(pixel);
      const float4 RGB_square = value * value;
      const float4 RGB_cubic = RGB_square * value;
      const float numerator = RGB_cubic.x + RGB_cubic.y + RGB_cubic.z;
      const float denominator = RGB_square.x + RGB_square.y + RGB_square.z;
      lum = exposure_boost * numerator / denominator;
      break;
    }

    case TONEEQ_GEOMEAN:
    {
      // geometric_mean(RGB)
      const float product = fabs(pixel.x) * fabs(pixel.y) * fabs(pixel.z);
      lum = exposure_boost * dtcl_pow(product, 1.0f / 3.0f);
      break;
    }

    case TONEEQ_MEAN:
    default:
      // mean(RGB) is the intensity
      lum = exposure_boost * (pixel.x + pixel.y + pixel.z) / 3.0f;
      break;
  }

  luminance[mad24(y, width, x)] = _linear_contrast(lum, fulcrum, contrast_boost);
}


/* Build the pixel correction as the sum of the contribution of each luminance
   channel, weighted by the gaussian of the radial distance in EV.
   This is the non-LUT reference implementation of apply_toneequalizer(): on GPU
   the transcendentals are cheaper than a 320 kB lookup table upload. */
static inline float _pixel_correction(const float exposure,
                                      const float4 factors_low,
                                      const float4 factors_high,
                                      const float gauss_denom)
{
  // radial distances of the exposure octaves, see centers_ops[] in toneequal.c
  const float4 centers_low = { -56.0f / 7.0f, -48.0f / 7.0f,
                               -40.0f / 7.0f, -32.0f / 7.0f };
  const float4 centers_high = { -24.0f / 7.0f, -16.0f / 7.0f,
                                 -8.0f / 7.0f,   0.0f / 7.0f };

  const float4 radius_low = exposure - centers_low;
  const float4 radius_high = exposure - centers_high;

  const float4 gauss_low =
    dtcl_exp(-radius_low * radius_low / gauss_denom) * factors_low;
  const float4 gauss_high =
    dtcl_exp(-radius_high * radius_high / gauss_denom) * factors_high;

  const float4 result = gauss_low + gauss_high;

  // the user-set correction is expected in [-2;+2] EV, so is the interpolated one
  return clamp(result.x + result.y + result.z + result.w, 0.25f, 4.0f);
}


kernel void toneequal_apply(read_only image2d_t in,
                            global const float *const luminance,
                            write_only image2d_t out,
                            const int width,
                            const int height,
                            const int in_width,
                            const int offset_x,
                            const int offset_y,
                            const float4 factors_low,
                            const float4 factors_high,
                            const float gauss_denom)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;

  const int xin = x + offset_x;
  const int yin = y + offset_y;

  // The radial-basis interpolation is valid in [-8; 0] EV and can quickly
  // diverge outside
  const float exposure = clamp(dtcl_log2(luminance[mad24(yin, in_width, xin)]),
                               TONEEQ_MIN_EV, TONEEQ_MAX_EV);
  const float correction =
    _pixel_correction(exposure, factors_low, factors_high, gauss_denom);

  const float4 pixel = read_imagef(in, samplerA, (int2)(xin, yin));
  write_imagef(out, (int2)(x, y), correction * pixel);
}


kernel void toneequal_display_mask(read_only image2d_t in,
                                   global const float *const luminance,
                                   write_only image2d_t out,
                                   const int width,
                                   const int height,
                                   const int in_width,
                                   const int offset_x,
                                   const int offset_y)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;

  const int xin = x + offset_x;
  const int yin = y + offset_y;

  // normalize the mask intensity between -8 EV and 0 EV for clarity,
  // and add a "gamma" 2.0 for better legibility in shadows
  const float lum = luminance[mad24(yin, in_width, xin)];
  const float intensity =
    dtcl_sqrt(fmin(fmax(lum - 0.00390625f, 0.0f) / 0.99609375f, 1.0f));

  // set gray level for the mask, copy alpha channel
  const float4 pixel = read_imagef(in, samplerA, (int2)(xin, yin));
  write_imagef(out, (int2)(x, y),
               (float4)(intensity, intensity, intensity, pixel.w));
}


/* Quantize in exposure levels evenly spaced in log by sampling.
   Mirrors quantize() from common/fast_guided_filter.h */
kernel void toneequal_quantize(global const float *const in,
                               global float *const out,
                               const int width,
                               const int height,
                               const float sampling,
                               const float clip_min,
                               const float clip_max)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;

  const int k = mad24(y, width, x);
  const float value = in[k];

  if(sampling == 0.0f)
    out[k] = value;                      // no-op
  else if(sampling == 1.0f)              // fast track
    out[k] = clamp(dtcl_exp2(floor(dtcl_log2(value))), clip_min, clip_max);
  else                                   // slow track
    out[k] = clamp(dtcl_exp2(floor(dtcl_log2(value) / sampling) * sampling),
                   clip_min, clip_max);
}


/* Separable box average over a window of size 2*radius + 1, mirroring
   dt_box_mean() from common/box_filters.cc. The window is clipped at the
   image borders and the average is taken over the samples actually seen,
   so input and output buffers must be distinct. */
#define TONEEQ_BOX_MEAN_LINE(TYPE, LENGTH, STRIDE)                       \
  TYPE sum = (TYPE)0.0f;                                                 \
  int hits = 0;                                                          \
  int i;                                                                 \
                                                                         \
  /* add up the left half of the window */                               \
  for(i = 0; i < min(radius, LENGTH); i++)                               \
  {                                                                      \
    sum += in[(size_t)i * STRIDE];                                       \
    hits++;                                                              \
  }                                                                      \
                                                                         \
  /* up to the point where we start removing values from the average */  \
  for(i = 0; i <= radius && i + radius < LENGTH; i++)                    \
  {                                                                      \
    sum += in[(size_t)(i + radius) * STRIDE];                            \
    hits++;                                                              \
    out[(size_t)i * STRIDE] = sum / (float)hits;                         \
  }                                                                      \
                                                                         \
  /* if radius > LENGTH/2 we can neither add nor remove values */        \
  for(; i <= radius && i < LENGTH; i++)                                  \
    out[(size_t)i * STRIDE] = sum / (float)hits;                         \
                                                                         \
  /* the bulk of the line */                                             \
  for(; i + radius < LENGTH; i++)                                        \
  {                                                                      \
    sum -= in[(size_t)(i - radius - 1) * STRIDE];                        \
    sum += in[(size_t)(i + radius) * STRIDE];                            \
    out[(size_t)i * STRIDE] = sum / (float)hits;                         \
  }                                                                      \
                                                                         \
  /* the end of the line, no more values to add to the average */        \
  for(; i < LENGTH; i++)                                                 \
  {                                                                      \
    sum -= in[(size_t)(i - radius - 1) * STRIDE];                        \
    hits--;                                                              \
    out[(size_t)i * STRIDE] = sum / (float)hits;                         \
  }

#define TONEEQ_BOX_MEAN_X(TYPE)                                          \
  const int y = get_global_id(0);                                        \
  if(y >= height) return;                                                \
                                                                         \
  global const TYPE *const in = input + (size_t)y * width;               \
  global TYPE *const out = output + (size_t)y * width;                   \
  TONEEQ_BOX_MEAN_LINE(TYPE, width, 1)

#define TONEEQ_BOX_MEAN_Y(TYPE)                                          \
  const int x = get_global_id(0);                                        \
  if(x >= width) return;                                                 \
                                                                         \
  global const TYPE *const in = input + x;                               \
  global TYPE *const out = output + x;                                   \
  TONEEQ_BOX_MEAN_LINE(TYPE, height, width)

kernel void toneequal_box_mean_x_2c(global const float2 *const input,
                                    global float2 *const output,
                                    const int width,
                                    const int height,
                                    const int radius)
{
  TONEEQ_BOX_MEAN_X(float2)
}

kernel void toneequal_box_mean_y_2c(global const float2 *const input,
                                    global float2 *const output,
                                    const int width,
                                    const int height,
                                    const int radius)
{
  TONEEQ_BOX_MEAN_Y(float2)
}

kernel void toneequal_box_mean_x_4c(global const float4 *const input,
                                    global float4 *const output,
                                    const int width,
                                    const int height,
                                    const int radius)
{
  TONEEQ_BOX_MEAN_X(float4)
}

kernel void toneequal_box_mean_y_4c(global const float4 *const input,
                                    global float4 *const output,
                                    const int width,
                                    const int height,
                                    const int radius)
{
  TONEEQ_BOX_MEAN_Y(float4)
}


/* Guided filter: pack guide and mask into an array of 4×1 vectors so that the
   box average of all four terms can be done in a single pass.
   Mirrors the first loop of variance_analyse() in common/fast_guided_filter.h */
kernel void toneequal_gf_pack(global const float *const guide,
                              global const float *const mask,
                              global float4 *const packed,
                              const int width,
                              const int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;

  const int k = mad24(y, width, x);
  const float g = guide[k];
  const float m = mask[k];

  packed[k] = (float4)(g, m, g * g, g * m);
}


/* Guided filter: turn the averaged terms into the linear blending parameters
   a and b such that mask = a * I + b */
kernel void toneequal_gf_ab(global const float4 *const packed,
                            global float2 *const ab,
                            const int width,
                            const int height,
                            const float feathering)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;

  const int k = mad24(y, width, x);
  const float4 avg = packed[k];

  // avoid division by 0
  const float d = fmax((avg.z - avg.x * avg.x) + feathering, 1e-15f);
  const float a = (avg.w - avg.x * avg.y) / d;
  const float b = avg.y - a * avg.x;

  ab[k] = (float2)(a, b);
}


/* Guided filter: blend the guided image with the a and b parameters.
   Mirrors apply_linear_blending[_w_geomean]() in common/fast_guided_filter.h */
kernel void toneequal_gf_blend(global float *const image,
                               global const float2 *const ab,
                               const int width,
                               const int height,
                               const int filter)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;

  const int k = mad24(y, width, x);
  const float pixel = image[k];
  const float2 blend = ab[k];

  // Note : image[k] is positive at the outside of the luminance mask
  const float blended = fmax(pixel * blend.x + blend.y, TONEEQ_MIN_FLOAT);

  image[k] = (filter == TONEEQ_BLENDING_GEOMEAN)
    ? dtcl_sqrt(pixel * blended)
    : blended;
}


/* Exposure independent guided filter: pack guide and mask so that a single
   gaussian blur yields the averages of guide, guide², mask and mask × guide.
   Mirrors eigf_variance_analysis() in common/eigf.h */
kernel void toneequal_eigf_pack_4c(global const float *const guide,
                                   global const float *const mask,
                                   global float4 *const packed,
                                   const int width,
                                   const int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;

  const int k = mad24(y, width, x);
  const float g = guide[k];
  const float m = mask[k];

  packed[k] = (float4)(g, g * g, m, m * g);
}


kernel void toneequal_eigf_finish_4c(global float4 *const av,
                                     const int width,
                                     const int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;

  const int k = mad24(y, width, x);
  const float4 avg = av[k];

  // turn the averages of the squares into variance and covariance
  av[k] = (float4)(avg.x,
                   avg.y - avg.x * avg.x,
                   avg.z,
                   avg.w - avg.x * avg.z);
}


/* same as above, but specialized for the case where guide == mask */
kernel void toneequal_eigf_pack_2c(global const float *const guide,
                                   global float2 *const packed,
                                   const int width,
                                   const int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;

  const int k = mad24(y, width, x);
  const float g = guide[k];

  packed[k] = (float2)(g, g * g);
}


kernel void toneequal_eigf_finish_2c(global float2 *const av,
                                     const int width,
                                     const int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;

  const int k = mad24(y, width, x);
  const float2 avg = av[k];

  av[k] = (float2)(avg.x, avg.y - avg.x * avg.x);
}


/* Exposure independent guided filter: blending step.
   Mirrors eigf_blending() in common/eigf.h */
kernel void toneequal_eigf_blend(global float *const image,
                                 global const float *const mask,
                                 global const float4 *const av,
                                 const int width,
                                 const int height,
                                 const int filter,
                                 const float feathering)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;

  const int k = mad24(y, width, x);
  const float pixel = image[k];
  const float4 avg = av[k];

  const float avg_g = avg.x;
  const float var_g = avg.y;
  const float avg_m = avg.z;
  const float covar_mg = avg.w;

  const float norm_g = fmax(avg_g * pixel, 1E-6f);
  const float norm_m = fmax(avg_m * mask[k], 1E-6f);
  const float normalized_var_guide = var_g / norm_g;
  const float normalized_covar = covar_mg / dtcl_sqrt(norm_g * norm_m);
  const float a = normalized_covar / (normalized_var_guide + feathering);
  const float b = avg_m - a * avg_g;

  const float blended = fmax(pixel * a + b, TONEEQ_MIN_FLOAT);

  image[k] = (filter == TONEEQ_BLENDING_GEOMEAN)
    ? dtcl_sqrt(pixel * blended)
    : blended;
}


/* same as above, but specialized for the case where guide == mask */
kernel void toneequal_eigf_blend_no_mask(global float *const image,
                                         global const float2 *const av,
                                         const int width,
                                         const int height,
                                         const int filter,
                                         const float feathering)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;

  const int k = mad24(y, width, x);
  const float pixel = image[k];
  const float2 avg = av[k];

  const float avg_g = avg.x;
  const float var_g = avg.y;

  const float norm_g = fmax(avg_g * pixel, 1E-6f);
  const float normalized_var_guide = var_g / norm_g;
  const float a = normalized_var_guide / (normalized_var_guide + feathering);
  const float b = avg_g - a * avg_g;

  const float blended = fmax(pixel * a + b, TONEEQ_MIN_FLOAT);

  image[k] = (filter == TONEEQ_BLENDING_GEOMEAN)
    ? dtcl_sqrt(pixel * blended)
    : blended;
}
