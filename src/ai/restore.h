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

/*
   restore — reusable AI denoise and upscale processing

   this module provides the core inference, tiling, and detail
   recovery logic for AI-based image restoration. it is part of
   the darktable_ai shared library and has no GUI dependencies.

   consumers:
     - src/libs/neural_restore.c (lighttable batch + preview)

   pixel pipeline:
     input is linear Rec.709 float4 RGBA (from darktable export).
     dt_restore_run_patch() converts linear->sRGB before inference
     and sRGB->linear after. models operate in planar NCHW layout.
     dt_restore_process_tiled() handles interleaved-to-planar
     conversion, mirror padding at boundaries, and overlap blending.

   detail recovery:
     dt_restore_apply_detail_recovery() uses wavelet (DWT)
     decomposition to separate noise from texture in the luminance
     residual (original - denoised). fine bands are thresholded;
     coarser bands are preserved and blended back.
*/

#pragma once

#include <glib.h>

struct _dt_job_t;

/* --- opaque types --- */

typedef struct dt_restore_env_t dt_restore_env_t;
typedef struct dt_restore_context_t dt_restore_context_t;

/* --- environment lifecycle --- */

/**
 * @brief initialize the restore environment
 *
 * wraps dt_ai_env_init(). returns NULL when AI is disabled.
 *
 * @return environment handle, or NULL
 */
dt_restore_env_t *dt_restore_env_init(void);

/**
 * @brief refresh model list after downloads/installs
 * @param env environment handle
 */
void dt_restore_env_refresh(dt_restore_env_t *env);

/**
 * @brief destroy the environment and free resources
 * @param env environment handle (NULL-safe)
 */
void dt_restore_env_destroy(dt_restore_env_t *env);

/* --- model lifecycle --- */

/**
 * @brief load denoise model (scale 1x)
 * @param env environment handle
 * @return context handle, or NULL if no model available
 */
dt_restore_context_t *dt_restore_load_denoise(dt_restore_env_t *env);

/**
 * @brief load upscale model at 2x
 * @param env environment handle
 * @return context handle, or NULL if no model available
 */
dt_restore_context_t *dt_restore_load_upscale_x2(dt_restore_env_t *env);

/**
 * @brief load upscale model at 4x
 * @param env environment handle
 * @return context handle, or NULL if no model available
 */
dt_restore_context_t *dt_restore_load_upscale_x4(dt_restore_env_t *env);

/**
 * @brief unload the ONNX model to free runtime memory
 *
 * the context stays valid. call the matching load function
 * again to reload, or dt_restore_free to release everything.
 *
 * @param ctx context handle (NULL-safe)
 */
void dt_restore_unload(dt_restore_context_t *ctx);

/**
 * @brief reload a previously unloaded context
 *
 * re-loads the same model that was used when the context was
 * created. no-ops if already loaded.
 *
 * @param ctx context handle
 * @return 0 on success, non-zero on error
 */
int dt_restore_reload(dt_restore_context_t *ctx);

/**
 * @brief free the context and all resources
 * @param ctx context handle (NULL-safe)
 */
void dt_restore_free(dt_restore_context_t *ctx);

/**
 * @brief check if a denoise model is available
 * @param env environment handle
 * @return TRUE if a denoise model is configured and present
 */
gboolean dt_restore_denoise_available(dt_restore_env_t *env);

/**
 * @brief check if an upscale model is available
 * @param env environment handle
 * @return TRUE if an upscale model is configured and present
 */
gboolean dt_restore_upscale_available(dt_restore_env_t *env);

/* --- tile size --- */

/**
 * @brief select optimal tile size based on available memory
 * @param scale upscale factor (1 for denoise)
 * @return tile size in pixels
 */
int dt_restore_select_tile_size(int scale);

/**
 * @brief get tile overlap for a given scale factor
 * @param scale upscale factor (1 for denoise)
 * @return overlap in pixels
 */
int dt_restore_get_overlap(int scale);

/* --- inference --- */

/**
 * @brief row writer callback for dt_restore_process_tiled
 *
 * called once per tile-row with 3ch interleaved float scanlines.
 * the callback can write to a buffer, TIFF, or any other sink.
 *
 * @param scanline 3ch interleaved float data (out_w pixels)
 * @param out_w output width in pixels
 * @param y scanline index in the output image
 * @param user_data caller-provided context
 * @return 0 on success, non-zero to abort
 */
typedef int (*dt_restore_row_writer_t)(const float *scanline,
                                       int out_w,
                                       int y,
                                       void *user_data);

/**
 * @brief run a single inference patch with sRGB conversion
 *
 * converts linear RGB input to sRGB, runs ONNX inference,
 * converts output back to linear. input is planar NCHW float.
 *
 * @param ctx loaded restore context
 * @param in_patch input tile (planar RGB, 3 * w * h floats)
 * @param w tile width
 * @param h tile height
 * @param out_patch output buffer (planar RGB, 3 * w*s * h*s)
 * @param scale upscale factor (1 for denoise)
 * @return 0 on success
 */
int dt_restore_run_patch(dt_restore_context_t *ctx,
                         const float *in_patch,
                         int w, int h,
                         float *out_patch,
                         int scale);

/**
 * @brief process an image with tiled inference
 *
 * tiles the input, runs inference on each tile, and delivers
 * completed scanlines via the row_writer callback. input is
 * float4 RGBA interleaved (from dt export).
 *
 * @param ctx loaded restore context
 * @param in_data input pixels (float4 RGBA, width * height)
 * @param width input width
 * @param height input height
 * @param scale upscale factor (1 for denoise)
 * @param row_writer callback receiving 3ch float scanlines
 * @param writer_data user data passed to row_writer
 * @param control_job job handle for progress/cancellation
 * @param tile_size tile size from dt_restore_select_tile_size
 * @return 0 on success
 */
int dt_restore_process_tiled(dt_restore_context_t *ctx,
                             const float *in_data,
                             int width, int height,
                             int scale,
                             dt_restore_row_writer_t row_writer,
                             void *writer_data,
                             struct _dt_job_t *control_job,
                             int tile_size);

/* --- detail recovery --- */

/**
 * @brief apply DWT-based detail recovery after denoising
 *
 * extracts luminance residual, filters noise with wavelet
 * decomposition, and blends preserved texture back.
 * both buffers are float4 RGBA at the same dimensions.
 *
 * @param original_4ch original input pixels (read-only)
 * @param denoised_4ch denoised pixels (modified in-place)
 * @param width image width
 * @param height image height
 * @param alpha blend strength (0 = none, 1 = full)
 */
void dt_restore_apply_detail_recovery(const float *original_4ch,
                                      float *denoised_4ch,
                                      int width, int height,
                                      float alpha);

/**
 * @brief compute DWT-filtered luminance detail from 3ch buffers
 *
 * returns a 1ch float array with wavelet-filtered luminance
 * residual (noise removed, texture preserved). used for
 * preview split visualization.
 *
 * @param before_3ch original image (3ch interleaved float)
 * @param after_3ch processed image (3ch interleaved float)
 * @param width image width
 * @param height image height
 * @return newly allocated 1ch buffer, or NULL. caller frees
 *         with dt_free_align()
 */
float *dt_restore_compute_dwt_detail(const float *before_3ch,
                                     const float *after_3ch,
                                     int width, int height);

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
