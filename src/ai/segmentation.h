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

#include "backend.h"
#include <glib.h>

/**
 * @brief Opaque segmentation context (holds encoder+decoder sessions
 *        and cached image embeddings).
 */
typedef struct dt_seg_context_t dt_seg_context_t;

/**
 * @brief A point prompt for the segmentation decoder.
 */
typedef struct dt_seg_point_t {
  float x, y;  ///< Pixel coordinates in the original image space
  int label;   ///< 1 = foreground (include), 0 = background (exclude)
} dt_seg_point_t;

/**
 * @brief Load a SAM segmentation model from the model registry.
 *        Expects encoder.onnx and decoder.onnx in the model directory.
 *        The execution provider is taken from the environment (read from
 *        the plugins/ai/provider config key at dt_ai_env_init time).
 * @param env AI environment (model registry).
 * @param model_id Model ID to look up in the registry.
 * @return Context, or NULL on error.
 */
dt_seg_context_t *dt_seg_load(dt_ai_environment_t *env,
                              const char *model_id);

/**
 * @brief Encode an image (run the SAM encoder once).
 *        The result is cached — subsequent calls with the same context
 *        skip re-encoding.
 * @param ctx Segmentation context.
 * @param rgb_data RGB uint8 image data (HWC layout, 3 channels).
 * @param width Image width in pixels.
 * @param height Image height in pixels.
 * @return TRUE on success, FALSE on error.
 */
gboolean dt_seg_encode_image(dt_seg_context_t *ctx,
                             const uint8_t *rgb_data,
                             int width, int height);

/**
 * @brief Compute a segmentation mask from point prompts.
 *        Must call dt_seg_encode_image() first.
 *        Uses iterative refinement: the low-resolution mask from the
 *        previous call is fed back as mask_input on subsequent calls.
 * @param ctx Segmentation context (with cached embeddings).
 * @param points Array of point prompts.
 * @param n_points Number of points.
 * @param out_width Set to mask width (same as encoded image width).
 * @param out_height Set to mask height (same as encoded image height).
 * @return Float mask buffer (width*height), caller frees with g_free().
 *         Values are in [0,1] range (sigmoid output). NULL on error.
 */
float *dt_seg_compute_mask(dt_seg_context_t *ctx,
                           const dt_seg_point_t *points, int n_points,
                           int *out_width, int *out_height);

/**
 * @brief Check if the image has been encoded.
 * @param ctx Segmentation context.
 * @return TRUE if image embeddings are cached, FALSE otherwise.
 */
gboolean dt_seg_is_encoded(dt_seg_context_t *ctx);

/**
 * @brief Check if the loaded model supports box prompts.
 *        SAM models support box prompts (label 2/3 corner points).
 *        SegNext models only support point prompts.
 * @param ctx Segmentation context (NULL-safe).
 * @return TRUE if box prompts are supported, FALSE otherwise.
 */
gboolean dt_seg_supports_box(dt_seg_context_t *ctx);

/**
 * @brief Reset cached image encoding (keeps the model loaded).
 *        Call this when the image changes so the next
 *        dt_seg_encode_image() re-encodes.
 * @param ctx Segmentation context (NULL-safe).
 */
void dt_seg_reset_encoding(dt_seg_context_t *ctx);

/**
 * @brief Reset the iterative mask refinement state.
 *        Keeps the image embeddings — only clears the previous low-res
 *        mask so the next dt_seg_compute_mask() starts fresh.
 * @param ctx Segmentation context (NULL-safe).
 */
void dt_seg_reset_prev_mask(dt_seg_context_t *ctx);

/**
 * @brief Free the segmentation context and all cached data.
 * @param ctx Context to free (NULL-safe).
 */
void dt_seg_free(dt_seg_context_t *ctx);

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
