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

// restore_common — internal shared definitions for the restore_*
// module family (restore.c, restore_rgb.c). NOT a public API:
// consumers must continue to treat dt_restore_context_t /
// dt_restore_env_t as opaque and go through the accessor functions
// declared in restore.h.

#pragma once

#include "ai/backend.h"
#include "common/image.h"

#include <glib.h>
#include <stdint.h>

// --- preprocessing policy enums ---
//
// ctx fields keyed off these enums replace what used to be compile-time
// RawNIND assumptions. Manifest keys (variants.<v>.{input_kind, wb_norm,
// output_scale, input_colorspace, target_mean}) resolve to these values
// at load; defaults reproduce RawNIND v1 behavior so legacy manifests
// keep working. See restore.h for the per-variant contract.

// identifies the full preprocessing contract (layout + WB + scaling +
// training distribution) the ONNX graph was trained against. treated as
// a string match, not a feature set: a model declaring bayer_v1 must
// comply with everything documented for that label in restore.h
typedef enum
{
  DT_RESTORE_INPUT_KIND_UNKNOWN = 0,
  DT_RESTORE_INPUT_KIND_BAYER_V1,
  // reserved for a future dedicated X-Trans denoise model. accepted as
  // a manifest value so packages shipping an xtrans variant can be
  // validated; the actual preprocessing pipeline is TBD and filed in
  // restore_raw_xtrans.c when Benoit's model format stabilizes. until
  // then X-Trans sensors route to the linear pipeline as a fallback
  // via dt_restore_load_rawdenoise_xtrans
  DT_RESTORE_INPUT_KIND_XTRANS_V1,
  DT_RESTORE_INPUT_KIND_LINEAR_V1,
} dt_restore_input_kind_t;

// color space the linear path feeds to the model. bayer path ignores
// this (4ch-packed layout fixes the space to camRGB by construction)
typedef enum
{
  DT_RESTORE_CS_LIN_REC2020 = 0,   // default for linear path
  DT_RESTORE_CS_CAMRGB,
  DT_RESTORE_CS_SRGB_LINEAR,
} dt_restore_colorspace_t;

// how WB is normalized before inference (and inverted after). DAYLIGHT
// uses the D65 coefficients derived from adobe_XYZ_to_CAM; AS_SHOT uses
// the raw's wb_coeffs; NONE leaves camRGB untouched
typedef enum
{
  DT_RESTORE_WB_DAYLIGHT = 0,
  DT_RESTORE_WB_AS_SHOT,
  DT_RESTORE_WB_NONE,
} dt_restore_wb_mode_t;

// post-inference output scale handling. MATCH_GAIN rescales the model
// output so its mean matches the model input mean (compensates for the
// arbitrary output scale RawNIND's L1 loss produces); ABSOLUTE trusts
// the model output as-is
typedef enum
{
  DT_RESTORE_OUT_MATCH_GAIN = 0,
  DT_RESTORE_OUT_ABSOLUTE,
} dt_restore_output_scale_t;

// how the 4-channel packed Bayer input is oriented. FORCE_RGGB extracts
// from the CFA's R origin so channel 0 is always R regardless of the
// sensor pattern — matches RawNIND v1 training, which cropped non-RGGB
// sensors to an RGGB origin before packing. NATIVE packs in the sensor's
// own CFA order (channel 0 at the top-left of each 2x2 block) for models
// that accept any Bayer pattern unchanged
typedef enum
{
  DT_RESTORE_BAYER_FORCE_RGGB = 0,
  DT_RESTORE_BAYER_NATIVE,
} dt_restore_bayer_orientation_t;

// edge handling when a tile extends past the image boundary. MIRROR is
// darktable's historical periodic reflection on absolute sensor coords.
// MIRROR_CROPPED reflects in the effective-cropped frame (post FORCE_RGGB
// shift) so the reflected content matches what a training pipeline that
// physically crops the sensor before tiling would see — required for
// bit-identical corner tiles on non-RGGB sensors under bayer_v1
typedef enum
{
  DT_RESTORE_EDGE_MIRROR_CROPPED = 0,
  DT_RESTORE_EDGE_MIRROR,
} dt_restore_edge_pad_t;

// dt_restore_sensor_class_t and _classify_sensor now live in restore.h
// (part of the public API so callers picking a variant loader can use
// it without pulling in restore_common.h's internal struct layouts)

// --- struct definitions shared across the restore_* module family ---

struct dt_restore_env_t
{
  dt_ai_environment_t *ai_env;
};

struct dt_restore_context_t
{
  dt_ai_context_t *ai_ctx;
  struct dt_restore_env_t *env;
  char *model_id;
  char *model_file;
  char *task;
  char *input_kind; // variant-declared input kind (e.g. "packed_bayer",
                    // "lin_rec2020"); NULL if the model doesn't declare one
  // policy enums resolved from the manifest at load time; see comments
  // on each enum in this file and the per-variant contract in restore.h.
  // defaults (0-init from g_new0) reproduce RawNIND v1 behavior, except
  // target_mean which needs explicit initialization — see _load
  dt_restore_input_kind_t        input_kind_enum;
  dt_restore_colorspace_t        input_colorspace;
  dt_restore_wb_mode_t           wb_mode;
  dt_restore_output_scale_t      output_scale;
  dt_restore_bayer_orientation_t bayer_orientation;
  dt_restore_edge_pad_t          edge_pad;
  float                          target_mean;  // NAN = no exposure boost
  int scale;        // model upscale factor (1 for denoise, 2/4 for upscale)
  int tile_size;    // tile size used to create the current session
  char *dim_h;      // symbolic height dim name used for session overrides
  char *dim_w;      // symbolic width dim name used for session overrides
  // color management (RGB path): convert working profile → sRGB before
  // inference and back after. if has_profile is FALSE, fall back to
  // gamma-only conversion (treats working-profile numbers as if sRGB).
  gboolean has_profile;
  float wp_to_srgb[9];   // working profile RGB -> sRGB linear
  float srgb_to_wp[9];   // sRGB linear -> working profile RGB
  // RGB path: when TRUE (default), out-of-sRGB-gamut pixels pass
  // through unchanged during denoise. when FALSE, every pixel uses
  // the model output and wide-gamut colors get clipped to sRGB.
  gboolean preserve_wide_gamut;
  // RGB path: shadow_boost_capable is set once at load from the
  // model's "shadow_boost" attribute; shadow_boost is re-computed
  // per image inside dt_restore_process_tiled() based on luminance.
  gboolean shadow_boost_capable;
  gboolean shadow_boost;
  // tile ladder candidates from largest to smallest; either the
  // model's "input_sizes" attribute from config.json (when declared)
  // or a copy of the built-in ladder for the model's scale. both the
  // startup budget selector and the runtime OOM retry loop iterate it
  int *tile_ladder;
  int n_tile_ladder;
  uint32_t ep_flags;  // execution provider flags (e.g. CoreML CPU-only)
  gint ref_count;
};

// DWT detail-recovery band count (used by restore_rgb.c)
#define DWT_DETAIL_BANDS 5

// compute per-site black level (4 entries) and raw-ADC range
// (white − black) for this image. prefers the per-site
// raw_black_level_separate when any entry is non-zero, otherwise
// falls back to the single raw_black_level. range entries are
// guarded against non-positive values so callers can divide safely.
// shared by the Bayer prep helper (restore_raw_bayer.c) and by the
// linear path's raw re-mosaic step (restore_raw_linear.c)
static inline void _compute_cfa_black_range(const dt_image_t *img,
                                            float black[4],
                                            float range[4],
                                            float *out_white)
{
  const float white = img->raw_white_point
    ? (float)img->raw_white_point : 65535.0f;
  if(out_white) *out_white = white;

  const gboolean have_separate
    = (img->raw_black_level_separate[0] != 0
       || img->raw_black_level_separate[1] != 0
       || img->raw_black_level_separate[2] != 0
       || img->raw_black_level_separate[3] != 0);
  for(int i = 0; i < 4; i++)
    black[i] = have_separate
      ? (float)img->raw_black_level_separate[i]
      : (float)img->raw_black_level;
  for(int i = 0; i < 4; i++)
  {
    range[i] = white - black[i];
    if(range[i] <= 0.0f) range[i] = 1.0f;
  }
}

// periodic mirror-pad index reflection, shared by every restore_*
// consumer that needs edge padding for tile reads (RGB, raw bayer,
// raw linear). fully periodic: any input index maps into [0, n)
static inline int _mirror(int i, int n)
{
  if(n <= 1) return 0;
  if(i < 0) i = -i;
  const int period = 2 * (n - 1);
  i = i % period;
  if(i < 0) i += period;
  if(i >= n) i = period - i;
  return i;
}

// mirror-pad reflection within an arbitrary sub-range [lo, hi) of the
// underlying 1D array (exclusive hi). used by the Bayer edge-pad mode
// MIRROR_CROPPED so reflections happen inside the RGGB-forced crop
// rectangle rather than the original sensor buffer — matches training
// pipelines that physically crop the sensor to RGGB before tiling
static inline int _mirror_in_range(int i, int lo, int hi)
{
  const int n = hi - lo;
  return lo + _mirror(i - lo, n);
}

// tile overlap blending weights: each tile contributes ax·ay; adjacent
// tiles' ramps sum to 1, so strip accumulators recover the blended value
// with no per-pixel division. seam = 2*sensor_O wide, centered on the
// core boundary; returns 1.0 outside the seam (pure interior)

static inline float _seam_ramp(int d, int sensor_O)
{
  return ((float)d + 0.5f) / (float)(2 * sensor_O);
}

static inline float _seam_ax(int sc,
                             int px_base, int px_end,
                             int sensor_O,
                             gboolean has_left, gboolean has_right)
{
  if(has_left && sc < px_base + sensor_O)
    return _seam_ramp(sc - (px_base - sensor_O), sensor_O);
  if(has_right && sc >= px_end - sensor_O)
    return 1.0f - _seam_ramp(sc - (px_end - sensor_O), sensor_O);
  return 1.0f;
}

static inline float _seam_ay(int sr,
                             int py_base, int py_end,
                             int sensor_O,
                             gboolean has_top, gboolean has_bot)
{
  if(has_top && sr < py_base + sensor_O)
    return _seam_ramp(sr - (py_base - sensor_O), sensor_O);
  if(has_bot && sr >= py_end - sensor_O)
    return 1.0f - _seam_ramp(sr - (py_end - sensor_O), sensor_O);
  return 1.0f;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
