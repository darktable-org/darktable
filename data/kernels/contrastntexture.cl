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

#include "common.h"

#define INVERSE_SQRT_3 0.5773502691896258f

// see compute_luminance(): the log luminance, input of the first level
kernel void contrastntexture_luminance(read_only image2d_t in,
                                       global float *const log_luminance,
                                       const int width,
                                       const int height,
                                       const float noise_bias)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;

  const float4 pixel = Areadpixel(in, x, y);
  const float norm = INVERSE_SQRT_3 * dtcl_sqrt(pixel.x * pixel.x + pixel.y * pixel.y + pixel.z * pixel.z)
                     + noise_bias;
  log_luminance[mad24(y, width, x)] = dtcl_log2(fmax(norm, NORM_MIN));
}

// the value relative to center and its square, blurred together as the two
// channels of one gaussian to give the local mean and variance of
// fast_gaussian_guided_filter()
kernel void contrastntexture_guided_filter_pack(global const float *const log_luminance,
                                                global float2 *const moments,
                                                const int width,
                                                const int height,
                                                const float center)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;

  const int k = mad24(y, width, x);
  const float v = log_luminance[k] - center;
  moments[k] = (float2)(v, v * v);
}

// one pyramid level: the guided filter output from the blurred moments, then
// the band pass between the log luminance before and after it, see
// extract_details(). The smoothed luminance, less the noise bias step,
// replaces the input as the next level's input
kernel void contrastntexture_details(global float *const log_luminance,
                                     global const float2 *const moments,
                                     global float *const corrections,
                                     const int width,
                                     const int height,
                                     const float feathering,
                                     const float gain,
                                     const float noise_bias,
                                     const float next_bias_step,
                                     const int accumulate,
                                     const int last_level,
                                     const float center)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;

  const int k = mad24(y, width, x);
  const float log_pixel = log_luminance[k];
  const float2 m = moments[k];
  const float variance = fmax(m.y - m.x * m.x, 0.0f);
  const float a = fp32_div(variance, variance + feathering);
  const float log_smoothed = a * log_pixel + (1.0f - a) * (m.x + center);
  const float smoothed = dtcl_exp2(log_smoothed);

  const float noise_power = noise_bias * noise_bias;
  const float combined_power = smoothed * smoothed;
  const float wiener_gain = fp32_div(fmax(combined_power - noise_power, 0.0f), fmax(combined_power, NORM_MIN));
  const float detail = gain * (wiener_gain * clamp(log_pixel - log_smoothed, -5.0f, 5.0f));
  corrections[k] = accumulate ? corrections[k] + detail : detail;

  log_luminance[k] = last_level ? log_smoothed
                                : dtcl_log2(fmax(smoothed - next_bias_step, NORM_MIN));
}

// see apply_shadows_highlights()
static inline float _shadows_highlights(const float log_luminance_lowpass,
                                        const float slope_shadows,
                                        const float slope_highlights,
                                        const float midtones_width,
                                        const float4 polynomial,
                                        const float pivot_offset)
{
  const float normalized_ev = log_luminance_lowpass - pivot_offset;
  float correction_ev;
  if(normalized_ev <= -midtones_width)
    correction_ev = slope_shadows * normalized_ev;
  else if(normalized_ev >= midtones_width)
    correction_ev = slope_highlights * normalized_ev;
  else
  {
    const float shifted_ev = normalized_ev + midtones_width;
    correction_ev = polynomial.x + polynomial.y * shifted_ev + polynomial.z * shifted_ev * shifted_ev;
  }
  return correction_ev - normalized_ev;
}

kernel void contrastntexture_apply(read_only image2d_t in,
                                   write_only image2d_t out,
                                   global const float *const lowpass,
                                   global const float *const corrections,
                                   const int width,
                                   const int height,
                                   const float slope_shadows,
                                   const float slope_highlights,
                                   const float midtones_width,
                                   const float4 polynomial,
                                   const float pivot_offset)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;

  const int k = mad24(y, width, x);
  const float4 pixel = Areadpixel(in, x, y);
  const float lowpass_correction = _shadows_highlights(lowpass[k], slope_shadows, slope_highlights,
                                                       midtones_width, polynomial, pivot_offset);
  const float multiplier = dtcl_exp2(corrections[k] + lowpass_correction);
  float4 o = pixel * multiplier;
  o.w = pixel.w;
  write_imagef(out, (int2)(x, y), o);
}

// see display_local_mask()
kernel void contrastntexture_display_details(write_only image2d_t out,
                                             global const float *const corrections,
                                             const int width,
                                             const int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;

  const float local_ev = corrections[mad24(y, width, x)];
  const float intensity = local_ev / dtcl_sqrt(local_ev * local_ev + 1.0f) * 0.5f + 0.5f;
  write_imagef(out, (int2)(x, y), (float4)(intensity, intensity, intensity, 1.0f));
}

// see display_lowpass_mask(): blue below the pivot, yellow above
kernel void contrastntexture_display_lowpass(write_only image2d_t out,
                                             global const float *const lowpass,
                                             const int width,
                                             const int height,
                                             const float pivot_offset)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;

  const float l = dtcl_exp2(lowpass[mad24(y, width, x)]);
  const float intensity = l / (pivot_offset + l);
  float saturation = (intensity - 0.5f) * 2.0f;
  saturation *= saturation;
  const float4 tint = intensity < 0.5f ? (float4)(0.0f, 0.0f, 1.0f, 0.0f)
                                       : (float4)(1.0f, 1.0f, 0.0f, 0.0f);
  float4 o = intensity * ((1.0f - saturation) + saturation * tint);
  o.w = 1.0f;
  write_imagef(out, (int2)(x, y), o);
}
