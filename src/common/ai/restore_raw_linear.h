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

// raw_restore_linear — RawNIND linear-variant denoise pipeline
//
// for sensors the bayer variant can't handle (X-Trans in particular),
// we feed the linear variant of the RawNIND model, which expects a
// 3-channel demosaicked image in lin_rec2020 space at raw scale.
//
// input is produced by running a minimal darktable pipeline:
// rawprepare -> highlights -> demosaic
// while skipping temperature (so we apply our own daylight WB later)
// and every iop after demosaic. this reuses darktable's sensor-aware
// demosaic (AMaZE / VNG / Markesteijn / …) instead of rolling our own,
// which matters for X-Trans quality.
//
// output is a 3ch float RGB buffer at full sensor resolution, in the
// same camRGB + raw ADC range as the source. the neural_restore batch
// path re-mosaics nothing (this sensor type can't be round-tripped
// through a CFA DNG) and writes a LinearRaw DNG via imageio_dng.

#pragma once

#include <glib.h>

#include "common/ai/restore.h"
#include "common/darktable.h"

struct _dt_job_t;
struct dt_image_t;

// @brief Run the RawNIND linear denoise pipeline end-to-end.
//
// Internally:
//   1. builds a minimal darktable pixelpipe (rawprepare + highlights
//      + demosaic, nothing after), disables temperature so no WB is
//      baked in;
//   2. allocates the 3ch float demosaicked output at sensor res;
//   3. applies daylight WB + camRGB -> lin_rec2020 matrix;
//   4. tiles the image and calls dt_restore_run_patch_3ch_raw on each,
//      gain-matching the output per tile;
//   5. inverts the matrix + WB;
//   6. strength-blends with the pre-inference demosaicked buffer.
//
// @param ctx       loaded linear context
//                  (dt_restore_load_rawdenoise_linear)
// @param imgid     image id (pipeline is built per image)
// @param out_rgb   caller-allocated 3ch float buffer,
//                  3 * sensor_w * sensor_h floats (interleaved RGB).
//                  on success contains the denoised image in camRGB
//                  raw-ADC units (same range as the source pre-demosaic
//                  pipeline would produce).
// @param out_w     out: sensor width at which the buffer is filled
// @param out_h     out: sensor height at which the buffer is filled
// @param strength  0..1 blend between the demosaicked source (0) and
//                  the denoised result (1)
// @param control_job job handle for progress/cancellation (NULL-safe)
// @return 0 on success; out_rgb left untouched on failure
int dt_restore_raw_linear(dt_restore_context_t *ctx,
                          const dt_imgid_t imgid,
                          float **out_rgb,
                          int *out_w,
                          int *out_h,
                          float strength,
                          struct _dt_job_t *control_job);

// @brief Once-per-image demosaic + WB + camRGB->lin_rec2020 prep.
//
// Runs the same minimal pipeline as dt_restore_raw_linear (rawprepare +
// highlights + demosaic, no temperature, no post-demosaic modules) and
// returns a 3ch interleaved lin_rec2020 buffer at sensor resolution.
//
// Slow (full-image demosaic via darktable's pipeline). neural_restore
// caches the result across multiple preview refreshes of the same image.
//
// @param ctx       loaded linear context (selects WB mode / colorspace
//                  to match the model the inference + undo paths will
//                  use). may be NULL only if the caller knows defaults
//                  match the downstream consumer.
// @param imgid     image id
// @param out_rgb   caller-frees with dt_free_align. 3ch interleaved
//                  (sensor_w * sensor_h * 3 floats), in lin_rec2020.
// @param out_w     out: sensor width
// @param out_h     out: sensor height
// @return 0 on success
int dt_restore_raw_linear_prepare(const dt_restore_context_t *ctx,
                                  const dt_imgid_t imgid,
                                  float **out_rgb,
                                  int *out_w,
                                  int *out_h);

// @brief Linear preview through darktable's real pixelpipe — "preview =
//        batch" for X-Trans / non-Bayer sensors.
//
// Runs inference on the crop, un-matrix / un-WB / un-boost the denoised
// crop back to raw-ADC space, re-mosaics onto the X-Trans CFA grid at
// the original sensor positions, then runs darktable's full pixelpipe
// twice on the raw-sensor-sized CFA — once on the patched CFA for
// "after", once on the original for "before". The pipe runs natively
// (rawprepare + highlights + X-Trans demosaic + temperature + colorin
// + filmic + output profile), so the output matches what the user sees
// in darkroom.
//
// Expensive: two full pipelined renders per refresh plus a full-sensor
// un-matrix pass. First refresh on a new image also pays one demosaic
// via dt_restore_raw_linear_prepare.
//
// @param ctx         loaded linear context
// @param img         source image metadata (for WB / matrix derivation)
// @param imgid       image id (used by the pixelpipe)
// @param full_rgb    3ch interleaved lin_rec2020 buffer covering the
//                    whole sensor (from dt_restore_raw_linear_prepare)
// @param width       sensor width
// @param height      sensor height
// @param crop_x      displayed crop top-left x
// @param crop_y      displayed crop top-left y
// @param crop_w      displayed crop width (≤ tile_size - 2*OVERLAP_LINEAR)
// @param crop_h      displayed crop height
// @param out_before_rgb    caller-frees with g_free. 3ch interleaved
//                          (*out_w * *out_h * 3 floats), linear Rec.709,
//                          pipe output on the original camRGB raw.
// @param out_denoised_rgb  caller-frees with g_free. same shape, pipe
//                          output on the denoised-patched camRGB raw
//                          at α = 1.
// @param out_w             receives actual rendered width (may differ
//                          from crop_w when user history contains
//                          geometry-modifying modules; both returned
//                          buffers share these dims).
// @param out_h             receives actual rendered height.
// @return 0 on success; both outputs NULL on failure.
int dt_restore_raw_linear_preview_piped(dt_restore_context_t *ctx,
                                        const struct dt_image_t *img,
                                        dt_imgid_t imgid,
                                        const float *full_rgb,
                                        int width,
                                        int height,
                                        int crop_x,
                                        int crop_y,
                                        int crop_w,
                                        int crop_h,
                                        float **out_before_rgb,
                                        float **out_denoised_rgb,
                                        int *out_w,
                                        int *out_h);

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
