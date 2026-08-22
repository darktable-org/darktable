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

/*
 * OpenCL port of the "fast scalar guided filter" from common/fast_guided_filter.h
 * (fast_surface_blur() and friends). This mirrors the CPU algorithm exactly:
 *
 *   1. downsample the single-channel image by a factor of 4 (bilinear)
 *   2. per iteration:
 *        - quantize the (downsampled) image into a guide buffer g
 *        - box-blur g, p (=image), g*g, g*p over a window of 2*radius+1
 *        - solve the linear regression p ~= a*g + b per pixel
 *        - box-blur a, b over the same window (spatial smoothing of params)
 *        - if not the last iteration: image = a*image + b
 *   3. upsample a, b back to full resolution (bilinear)
 *   4. final blend: image = a*image + b
 *
 * All buffers are single-channel (CL_R / float) images, addressed the same
 * way as the existing guided_filter_box_mean_x/_y kernels in guided_filter.cl,
 * which this file reuses verbatim for the box-blur passes (see the host-side
 * orchestration that must call them once per buffer, per direction).
 *
 * NOTE: this file only provides the per-pixel kernels. The host-side C code
 * must allocate the intermediate buffers and launch these kernels (plus the
 * existing guided_filter_box_mean_x/_y from guided_filter.cl) in the right
 * order, once per iteration (see src/iop/satcurvergb.c for an implementation).
 */

#define FGF_MIN_FLOAT 0x1p-16f  // exp2(-16.0f), matches MIN_FLOAT in fast_guided_filter.h


// generic bilinear resample of a single-channel image; used both to
// downsample the guide/signal by a factor of ~4 and to upsample the
// solved (a, b) parameter buffers back to full resolution.
kernel void
fastguided_resample(read_only image2d_t in,
                    const int width_in, const int height_in,
                    write_only image2d_t out,
                    const int width_out, const int height_out)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width_out || y >= height_out) return;

  const float x_out = (float)x / (float)width_out;
  const float y_out = (float)y / (float)height_out;

  const float x_in = x_out * (float)width_in;
  const float y_in = y_out * (float)height_in;

  int x_prev = (int)floor(x_in);
  int x_next = x_prev + 1;
  int y_prev = (int)floor(y_in);
  int y_next = y_prev + 1;

  x_prev = clamp(x_prev, 0, width_in - 1);
  x_next = clamp(x_next, 0, width_in - 1);
  y_prev = clamp(y_prev, 0, height_in - 1);
  y_next = clamp(y_next, 0, height_in - 1);

  const float Dy_next = (float)y_next - y_in;
  const float Dy_prev = 1.f - Dy_next;
  const float Dx_next = (float)x_next - x_in;
  const float Dx_prev = 1.f - Dx_next;

  const float Q_NW = Areadsingle(in, x_prev, y_prev);
  const float Q_NE = Areadsingle(in, x_next, y_prev);
  const float Q_SE = Areadsingle(in, x_next, y_next);
  const float Q_SW = Areadsingle(in, x_prev, y_next);

  const float result = Dy_prev * (Q_SW * Dx_next + Q_SE * Dx_prev)
                      + Dy_next * (Q_NW * Dx_next + Q_NE * Dx_prev);

  write_imagef(out, (int2)(x, y), result);
}


// posterize `in` in log2 space; sampling == 0 is a no-op copy, sampling == 1
// is the fast per-stop track, anything else uses the general formula.
// Mirrors quantize() in fast_guided_filter.h.
kernel void
fastguided_quantize(read_only image2d_t in, write_only image2d_t out,
                    const int width, const int height,
                    const float sampling, const float clip_min, const float clip_max)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;

  const float value = Areadsingle(in, x, y);
  float result;

  if(sampling == 0.0f)
    result = value;
  else if(sampling == 1.0f)
    result = clamp(exp2(floor(log2(value))), clip_min, clip_max);
  else
    result = clamp(exp2(floor(log2(value) / sampling) * sampling), clip_min, clip_max);

  write_imagef(out, (int2)(x, y), result);
}


// per-pixel g*g and g*p, ready for box-blurring. Mirrors the packing step in
// variance_analyse() (the guide/mask/guide^2/guide*mask struct), split into
// two single-channel outputs since the box-blur passes are single-channel.
kernel void
fastguided_covar_products(read_only image2d_t guide, read_only image2d_t signal,
                          write_only image2d_t guide_sq, write_only image2d_t guide_signal,
                          const int width, const int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;

  const float g = Areadsingle(guide, x, y);
  const float p = Areadsingle(signal, x, y);

  write_imagef(guide_sq, (int2)(x, y), g * g);
  write_imagef(guide_signal, (int2)(x, y), g * p);
}


// solve the linear blending parameters a, b such that p ~= a*g + b, from the
// box-blurred means of g, p, g*g and g*p. Mirrors the blending step at the
// end of variance_analyse().
kernel void
fastguided_solve_ab(read_only image2d_t mean_g, read_only image2d_t mean_p,
                    read_only image2d_t mean_gg, read_only image2d_t mean_gp,
                    write_only image2d_t a_out, write_only image2d_t b_out,
                    const int width, const int height,
                    const float feathering)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;

  const float g = Areadsingle(mean_g, x, y);
  const float p = Areadsingle(mean_p, x, y);
  const float gg = Areadsingle(mean_gg, x, y);
  const float gp = Areadsingle(mean_gp, x, y);

  const float d = fmax((gg - g * g) + feathering, 1e-15f);
  const float a = (gp - g * p) / d;
  const float b = p - a * g;

  write_imagef(a_out, (int2)(x, y), a);
  write_imagef(b_out, (int2)(x, y), b);
}


// image = max(image * a + b, MIN_FLOAT), in-place capable (no neighbour
// access). Used both for the intermediate per-iteration update of the
// downsampled image and for the final full-resolution blend. Mirrors
// apply_linear_blending() (DT_GF_BLENDING_LINEAR case only -- the geomean
// blending mode is not ported here since satcurvergb only ever requests
// DT_GF_BLENDING_LINEAR).
kernel void
fastguided_apply_blend(read_only image2d_t image_in, write_only image2d_t image_out,
                       read_only image2d_t a_img, read_only image2d_t b_img,
                       const int width, const int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;

  const float value = Areadsingle(image_in, x, y);
  const float a = Areadsingle(a_img, x, y);
  const float b = Areadsingle(b_img, x, y);

  write_imagef(image_out, (int2)(x, y), fmax(value * a + b, FGF_MIN_FLOAT));
}


// final step specific to satcurvergb: blend the already-corrected RGB pixel
// back towards the untouched input RGB, using the filtered scalar mask as
// per-pixel strength. Mirrors the `if(gf_mask)` branch inside the CPU
// process() loop: pixout[c] = rgb[c] + w * (pixout[c] - rgb[c]).
kernel void
satcurve_apply_guided_mask(read_only image2d_t rgb_in, read_only image2d_t corrected,
                           read_only image2d_t mask, write_only image2d_t out,
                           const int width, const int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;

  const float4 rgb = Areadpixel(rgb_in, x, y);
  const float4 pixout = Areadpixel(corrected, x, y);
  const float w = clamp(Areadsingle(mask, x, y), 0.f, 1.f);

  float4 result = rgb + w * (pixout - rgb);
  result.w = pixout.w;

  write_imagef(out, (int2)(x, y), result);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
