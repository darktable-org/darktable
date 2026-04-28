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

// restore — generic AI restore environment and model lifecycle.
//
// this module provides the shared scaffolding that all AI restore
// paths sit on top of: environment init, model loading with tile
// ladder selection + OOM retry, reference-counted contexts, tile
// size persistence, and the user-pipe ROI bridge used by raw-
// denoise previews. RGB denoise/upscale inference lives in
// restore_rgb.{c,h}; raw variants live in restore_raw_*.{c,h}.
//
// consumers:
// - src/libs/neural_restore.c (lighttable batch + preview)
// - src/common/ai/restore_rgb.c (RGB denoise + upscale)
// - src/common/ai/restore_raw_bayer.c (RawNIND Bayer)
// - src/common/ai/restore_raw_linear.c (RawNIND linear/X-Trans)

#pragma once

#include <glib.h>

#include "common/image.h"  // for dt_imgid_t

// --- opaque types ---

typedef struct dt_restore_env_t dt_restore_env_t;
typedef struct dt_restore_context_t dt_restore_context_t;

// --- sensor classification ---

// BAYER is any standard 2x2 Bayer (RGGB / BGGR / GRBG / GBRG).
// XTRANS is Fuji's 6x6 pattern (filters == 9u). LINEAR is the
// generic-demosaic fallback used for Foveon, monochrome-with-pattern,
// and anything else without a dedicated pipeline. UNSUPPORTED means
// the image can't be routed to any denoise variant (non-raw, pure
// monochrome, etc.). pick the loader matching the class:
//   BAYER  -> dt_restore_load_rawdenoise_bayer
//   XTRANS -> dt_restore_load_rawdenoise_xtrans
//   LINEAR -> dt_restore_load_rawdenoise_linear
typedef enum
{
  DT_RESTORE_SENSOR_CLASS_BAYER = 0,
  DT_RESTORE_SENSOR_CLASS_XTRANS,
  DT_RESTORE_SENSOR_CLASS_LINEAR,
  DT_RESTORE_SENSOR_CLASS_UNSUPPORTED,
} dt_restore_sensor_class_t;

// classify a raw image by its CFA pattern. pure function of img flags
// and buf_dsc.filters; caller is expected to have a raw-loaded image
// (buf_dsc.filters populated by rawspeed). returns UNSUPPORTED when
// the image isn't a raw darktable can denoise
dt_restore_sensor_class_t dt_restore_classify_sensor(const dt_image_t *img);

// --- environment lifecycle ---

// @brief initialize the restore environment
//
// wraps dt_ai_env_init(). returns NULL when AI is disabled.
//
// @return environment handle, or NULL
dt_restore_env_t *dt_restore_env_init(void);

// @brief refresh model list after downloads/installs
// @param env environment handle
void dt_restore_env_refresh(dt_restore_env_t *env);

// @brief destroy the environment and free resources
// @param env environment handle (NULL-safe)
void dt_restore_env_destroy(dt_restore_env_t *env);

// --- model lifecycle ---

// @brief load denoise model (scale 1x)
// @param env environment handle
// @return context handle, or NULL if no model available
dt_restore_context_t *dt_restore_load_denoise(dt_restore_env_t *env);

// @brief load raw-denoise bayer model (scale 1x)
//
// raw denoise reuses the full scale==1 denoise pipeline (tile size,
// color conversion, shadow boost, wide-gamut pass-through); only the
// model's task string ("rawdenoise") differs. the bayer and linear
// ONNX files ship together in one "rawdenoise" package and the caller
// picks which variant to load.
//
// the filename is read from the model's variants.bayer.onnx attribute;
// a model package that doesn't declare this attribute fails to load
// (no silent fallback).
//
// --- bayer_v1 input contract ---
//
// variants declaring `input_kind: bayer_v1` must satisfy:
//
//   INPUT: NCHW, 4 channels, T×T (packed half-resolution, where the
//          sensor tile is 2T × 2T). channel order: R, G1, G2, B —
//          extraction starts at the CFA's R origin so non-RGGB sensors
//          (BGGR, GRBG, GBRG) get packed as if they were RGGB. this
//          matches RawNIND training, which physically crops non-RGGB
//          sensors to an RGGB origin before tiling. overridable via
//          variants.bayer.bayer_orientation (force_rggb | native);
//          default: force_rggb.
//          values: (raw - black[site]) / range[site] * wb_norm[ch].
//          wb_norm defaults to daylight (D65 derived from the camera
//          adobe_XYZ_to_CAM), overridable via variants.bayer.wb_norm
//          (daylight | as_shot | none).
//          edge tiles that extend past the image bounds are mirror-
//          padded inside the effective-RGGB-cropped rectangle
//          (variants.bayer.edge_pad: mirror_cropped | mirror). default
//          for bayer_v1 is mirror_cropped so corner tiles see the same
//          reflections the model's training did.
//
//   OUTPUT: NCHW, 3 channels, 2T × 2T (model internally demosaics via
//           PixelShuffle). values are camRGB in the same (WB, exposure)
//           frame as the input. output scale is arbitrary unless the
//           variant declares output_scale: absolute — by default the
//           loader applies match_gain (scalar mean-match) before
//           re-mosaicing. input_colorspace and target_mean are ignored
//           on this path.
//
// a declared-but-unknown input_kind (or one that contradicts the slot)
// is a hard error — the loader refuses to open a mis-packaged ONNX.
// manifests predating the contract label (input_kind missing) are
// accepted for back-compat and treated as bayer_v1.
//
// @param env environment handle
// @return context handle, or NULL if no model available / misconfigured
dt_restore_context_t *dt_restore_load_rawdenoise_bayer(dt_restore_env_t *env);

// @brief load raw-denoise X-Trans model (scale 1x)
//
// prefers a dedicated xtrans variant when the manifest declares
// variants.xtrans.onnx; falls back transparently to the linear variant
// otherwise. callers pick this loader for X-Trans sensors so a future
// RawNIND release can swap in a dedicated model via manifest-only
// changes.
//
// --- xtrans_v1 input contract (reserved) ---
//
// variants declaring `input_kind: xtrans_v1` are accepted by the
// loader but the actual preprocessing contract (channel layout, WB
// convention, output-space semantics) is TBD until Benoit's dedicated
// X-Trans model stabilizes. until then this loader's first call
// returns NULL for any manifest lacking a variants.xtrans slot, and
// the fallback path produces a linear_v1 context
//
// @param env environment handle
// @return context handle, or NULL if neither an xtrans nor a linear
//         variant is available.
dt_restore_context_t *dt_restore_load_rawdenoise_xtrans(dt_restore_env_t *env);

// @brief load raw-denoise linear model (scale 1x)
//
// generic-demosaic-based denoise: used for Foveon, monochrome sensors
// with a CFA-ish pattern, and any raw whose CFA pattern doesn't fit
// the bayer or xtrans pipelines. also the fallback pipeline for
// X-Trans sensors (via dt_restore_load_rawdenoise_xtrans) until a
// dedicated xtrans_v1 model is available.
//
// --- linear_v1 input contract ---
//
// variants declaring `input_kind: linear_v1` must satisfy:
//
//   INPUT: NCHW, 3 channels, T × T planar. colorspace is
//          variants.linear.input_colorspace (default lin_rec2020;
//          alternatives: camRGB, srgb_linear). preprocessing applies
//          WB in camRGB first — mode via variants.linear.wb_norm
//          (default as_shot; see _resolve_linear_wb) — then the
//          camRGB → input-space 3×3 matrix derived from
//          adobe_XYZ_to_CAM, then an optional scalar exposure boost
//          to variants.linear.target_mean (default 0.30 for the
//          training distribution; set "null" to disable).
//
//   OUTPUT: NCHW, 3 channels, T × T in the same input-space. output
//           scale is arbitrary unless the variant declares
//           output_scale: absolute — default is per-channel match_gain
//           against the boosted input. the caller then inverts the
//           exposure boost, the matrix, and the WB to recover a raw
//           camRGB DNG that renders identically under the importing
//           pipeline.
//
// same contract-label semantics as the bayer variant: missing label
// accepted as linear_v1, declared-but-mismatched label refuses
// to load with dt_control_log feedback.
//
// @param env environment handle
// @return context handle, or NULL if no model available / misconfigured
dt_restore_context_t *dt_restore_load_rawdenoise_linear(dt_restore_env_t *env);

// @brief load upscale model at 2x
// @param env environment handle
// @return context handle, or NULL if no model available
dt_restore_context_t *dt_restore_load_upscale_x2(dt_restore_env_t *env);

// @brief load upscale model at 4x
// @param env environment handle
// @return context handle, or NULL if no model available
dt_restore_context_t *dt_restore_load_upscale_x4(dt_restore_env_t *env);

// @brief increment the reference count for shared ownership.
//        multiple threads can share the same context for concurrent
//        inference.
// @param ctx context handle
// @return the same pointer (for convenience)
dt_restore_context_t *dt_restore_ref(dt_restore_context_t *ctx);

// @brief decrement the reference count. frees the context and all
//        resources when the count reaches zero.
// @param ctx context handle (NULL-safe)
void dt_restore_unref(dt_restore_context_t *ctx);

// @brief check if a denoise model is available
// @param env environment handle
// @return TRUE if a denoise model is configured and present
gboolean dt_restore_denoise_available(dt_restore_env_t *env);

// @brief check if a raw-denoise model is available
// @param env environment handle
// @return TRUE if a raw-denoise model is configured and present
gboolean dt_restore_rawdenoise_available(dt_restore_env_t *env);

// @brief check if an upscale model is available
// @param env environment handle
// @return TRUE if an upscale model is configured and present
gboolean dt_restore_upscale_available(dt_restore_env_t *env);

// --- tile size ---

// @brief get tile overlap for a given scale factor
// @param scale upscale factor (1 for denoise)
// @return overlap in pixels
int dt_restore_get_overlap(int scale);

// --- inference ---

// @brief run a single RawNIND bayer inference patch
//
// thin wrapper over dt_ai_run for bayer-packed input: NO colorspace
// or gamma conversion, NO WB handling, NO shadow boost. caller is
// responsible for black-subtract / normalize / WB / RGGB pack.
// input is planar 4ch NCHW at packed half-resolution, output is
// planar 3ch at full sensor resolution (model internally upscales
// 2x via PixelShuffle). output is in camRGB — the camera ColorMatrix
// is NOT applied in the graph (training applies it externally for
// loss, so re-mosaic + DNG write works natively).
//
// @param ctx loaded restore context (bayer model)
// @param in_4ch packed input (planar 4ch: R, G1, G2, B; 4 * w * h)
// @param w packed-space tile width (= sensor_w / 2)
// @param h packed-space tile height (= sensor_h / 2)
// @param out_3ch output buffer (planar 3ch at 2w * 2h)
// @return 0 on success
int dt_restore_run_patch_bayer(dt_restore_context_t *ctx,
                               const float *in_4ch,
                               int w, int h,
                               float *out_3ch);

// @brief run a single RawNIND linear inference patch
//
// 3ch in, 3ch out, SAME spatial dims (no internal upscale). like
// _run_patch_bayer: no sRGB / gamma / WP conversion, no shadow boost.
// caller prepares input in the colorspace the linear model was
// trained on (lin_rec2020 per config.json) and gain-matches the
// output afterward (model output is arbitrary-scale camRGB-in-that-
// space, matching the behavior already observed on the bayer variant).
//
// @param ctx loaded restore context (linear model)
// @param in_3ch planar 3ch input (3 * w * h floats, NCHW order)
// @param w tile width
// @param h tile height
// @param out_3ch output buffer (planar 3ch, 3 * w * h floats)
// @return 0 on success
int dt_restore_run_patch_3ch_raw(dt_restore_context_t *ctx,
                                 const float *in_3ch,
                                 int w, int h,
                                 float *out_3ch);

// @brief look up the tile ladder for a restore context
//
// exposes the model-declared (or default) input_sizes list in
// packed-space. used by the bayer pipeline to pick a starting
// tile size that respects the model's declared shapes.
//
// @param ctx loaded restore context
// @param out_count filled with number of entries (may be NULL)
// @return pointer to the ladder (owned by ctx; do not free). NULL
//         if ctx is NULL.
const int *dt_restore_get_tile_ladder(const dt_restore_context_t *ctx,
                                      int *out_count);

// @brief current tile size stored in the loaded session
//
// @param ctx loaded restore context
// @return tile size in packed-space, or 0 if ctx is NULL
int dt_restore_get_tile_size(const dt_restore_context_t *ctx);

// @brief recreate the ORT session for a different tile size
//
// used by the bayer OOM-retry loop to step down the ladder
// when inference fails. keeps the same model/provider; only the
// H/W dim overrides change. the old session is unloaded first
// (avoids VRAM cascade on GPU OOM).
//
// @param ctx loaded restore context
// @param new_tile_size new tile size (must be a ladder member)
// @return TRUE on success
gboolean dt_restore_reload_session(dt_restore_context_t *ctx,
                                   int new_tile_size);

// @brief persist the current tile size to darktablerc
//
// once the bayer pipeline has processed an image end-to-end at
// ctx->tile_size without OOM, call this so the next run skips the
// retry loop and JIT-compiling providers don't pay the compile
// cost again.
//
// @param ctx loaded restore context
void dt_restore_persist_tile_size(const dt_restore_context_t *ctx);

// @brief run darktable's real user pixelpipe on a sensor buffer, ROI-clipped.
//
// Shared bridge for the raw-denoise preview paths. Both Bayer and
// X-Trans previews need to run the user's full iop stack on a
// (possibly neural-denoised and re-mosaiced) raw buffer so the
// displayed before/after pixels match what the user would see after
// batch processing and DNG re-import. The pipe runs natively —
// rawprepare + demosaic + temperature + colorin + filmic + output
// profile — with rawdenoise skipped since the neural denoiser has
// already done its work.
//
// @param imgid        image id (the pipe is built per image)
// @param input_native buffer matching the image's native raw format
//                     (uint16 CFA or 3ch float LinearRaw). pipe only
//                     reads this; caller retains ownership.
// @param iw           buffer width in native samples
// @param ih           buffer height
// @param roi_x        ROI top-left x in sensor (input) coords — same
//                     coordinate system the caller used to patch the
//                     denoised CFA into input_native. the bridge
//                     forward-transforms this through the user's
//                     geometry chain (rawprepare + clipping + ashift +
//                     lens + ...) so the pipe renders the same sensor
//                     area the caller patched.
// @param roi_y        ROI top-left y (sensor coords)
// @param roi_w        ROI width  (sensor coords)
// @param roi_h        ROI height (sensor coords)
// @param out_w        receives actual rendered width (may differ from
//                     roi_w when user history contains geometry-
//                     modifying modules like clipping/ashift/lens, or
//                     when rawprepare trims; NULL to skip)
// @param out_h        receives actual rendered height (as out_w; NULL
//                     to skip)
// @param out_rgb      caller-frees with g_free. 3ch interleaved
//                     (*out_w * *out_h * 3 floats) in linear Rec.709,
//                     ready for sRGB-gamma display. callers must use
//                     *out_w / *out_h (not the requested roi_w/roi_h)
//                     for subsequent indexing.
// @return 0 on success; *out_rgb set to NULL on failure.
int dt_restore_run_user_pipe_roi(dt_imgid_t imgid,
                                 void *input_native,
                                 int iw,
                                 int ih,
                                 int roi_x,
                                 int roi_y,
                                 int roi_w,
                                 int roi_h,
                                 int *out_w,
                                 int *out_h,
                                 float **out_rgb);

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
