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

// restore_rgb — public API for the RGB-path AI tasks (denoise + upscale).
//
// consumers:
// - src/libs/neural_restore.c
//
// pixel pipeline:
// input is linear working-profile float4 RGBA (from darktable export).
// dt_restore_run_patch() converts linear→sRGB before inference and
// sRGB→linear after. models operate in planar NCHW layout.
// dt_restore_process_tiled() handles interleaved→planar conversion,
// mirror padding at boundaries, gamut masking, shadow boost, and
// overlap blending.
//
// detail recovery:
// dt_restore_apply_detail_recovery() uses wavelet (DWT) decomposition
// to separate noise from texture in the luminance residual (original
// − denoised). fine bands are thresholded; coarser bands are preserved
// and blended back.

#pragma once

#include "common/ai/restore.h"

#include <glib.h>

struct _dt_job_t;

// --- color management (RGB path) ---

// @brief Set the working color profile for the context.
//
// The AI model was trained on sRGB primaries. If the input pixels are
// in a different working profile (e.g. Rec.2020), we must convert to
// sRGB before inference and back after to avoid hue shifts. Call this
// before running inference on each image that may use a different
// working profile.
//
// If profile is NULL, the pipeline falls back to gamma-only conversion
// (treating working-profile numbers as if they were sRGB), which can
// cause color shifts for wide-gamut working profiles.
//
// Thread-safety: must not be called concurrently with
// dt_restore_run_patch() or dt_restore_process_tiled(). Set the
// profile before dispatching inference on a given image.
//
// @param ctx context handle (NULL-safe)
// @param profile lcms2 cmsHPROFILE handle cast to void*; NULL to disable
void dt_restore_set_profile(dt_restore_context_t *ctx, void *profile);

// @brief Enable/disable wide-gamut pass-through for denoise.
//
// When TRUE (default): pixels that would be out of sRGB gamut pass
// through unchanged, preserving color but not denoising them. When
// FALSE: all pixels use the model output, wide-gamut colors are
// clipped to sRGB but everything gets denoised.
//
// Affects denoise only (scale == 1). Upscale always uses the model
// output because there is no pixel-to-pixel correspondence to
// pass through.
//
// @param ctx context handle (NULL-safe)
// @param preserve TRUE to enable pass-through, FALSE to denoise everything
void dt_restore_set_preserve_wide_gamut(dt_restore_context_t *ctx,
                                        gboolean preserve);

// --- inference ---

// @brief row writer callback for dt_restore_process_tiled
//
// called once per tile-row with 3ch interleaved float scanlines.
// the callback can write to a buffer, TIFF, or any other sink.
//
// @param scanline 3ch interleaved float data (out_w pixels)
// @param out_w output width in pixels
// @param y scanline index in the output image
// @param user_data caller-provided context
// @return 0 on success, non-zero to abort
typedef int (*dt_restore_row_writer_t)(const float *scanline,
                                       int out_w,
                                       int y,
                                       void *user_data);

// @brief run a single inference patch with sRGB conversion
//
// converts linear RGB input to sRGB, runs ONNX inference,
// converts output back to linear. input is planar NCHW float.
//
// @param ctx loaded restore context
// @param in_patch input tile (planar RGB, 3 * w * h floats)
// @param w tile width
// @param h tile height
// @param out_patch output buffer (planar RGB, 3 * w*s * h*s)
// @param scale upscale factor (1 for denoise)
// @return 0 on success
int dt_restore_run_patch(dt_restore_context_t *ctx,
                         const float *in_patch,
                         int w, int h,
                         float *out_patch,
                         int scale);

// @brief process an image with tiled inference
//
// tiles the input, runs inference on each tile, and delivers
// completed scanlines via the row_writer callback. input is
// float4 RGBA interleaved (from dt export).
//
// @param ctx loaded restore context (tile_size is stored in ctx)
// @param in_data input pixels (float4 RGBA, width * height)
// @param width input width
// @param height input height
// @param scale upscale factor (1 for denoise)
// @param row_writer callback receiving 3ch float scanlines
// @param writer_data user data passed to row_writer
// @param control_job job handle for progress/cancellation (NULL-safe)
// @return 0 on success
int dt_restore_process_tiled(dt_restore_context_t *ctx,
                             const float *in_data,
                             int width, int height,
                             int scale,
                             dt_restore_row_writer_t row_writer,
                             void *writer_data,
                             struct _dt_job_t *control_job);

// --- detail recovery ---

// @brief apply DWT-based detail recovery after denoising
//
// extracts luminance residual, filters noise with wavelet
// decomposition, and blends preserved texture back.
// both buffers are float4 RGBA at the same dimensions.
//
// @param original_4ch original input pixels (read-only)
// @param denoised_4ch denoised pixels (modified in-place)
// @param width image width
// @param height image height
// @param alpha blend strength (0 = none, 1 = full)
void dt_restore_apply_detail_recovery(const float *original_4ch,
                                      float *denoised_4ch,
                                      int width, int height,
                                      float alpha);

// @brief compute DWT-filtered luminance detail from 3ch buffers
//
// returns a 1ch float array with wavelet-filtered luminance
// residual (noise removed, texture preserved). used for
// preview split visualization.
//
// @param before_3ch original image (3ch interleaved float)
// @param after_3ch processed image (3ch interleaved float)
// @param width image width
// @param height image height
// @return newly allocated 1ch buffer, or NULL. caller frees
//         with dt_free_align()
float *dt_restore_compute_dwt_detail(const float *before_3ch,
                                     const float *after_3ch,
                                     int width, int height);

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
