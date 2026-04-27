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

// raw_restore — RawNIND bayer-denoise pipeline
//
// wraps a loaded RawNIND bayer model and runs the whole raw->raw
// denoise pipeline: preprocessing (black level, normalize, per-channel
// WB, 2x2 pack), tiled inference with overlap blending, postprocessing
// (un-WB, un-normalize), and re-mosaic back to the original CFA
// pattern. produces a uint16 sensor-sized mosaic that is written to
// DNG by dt_imageio_dng_write_cfa_bayer().
//
// this is kept separate from the RGB denoise/upscale path in restore.c
// because:
// - input is single-channel CFA, not RGB; no sRGB gamma, no wide-gamut
// - preprocessing is raw-specific (per-channel black, WB normalization)
// - output is re-mosaiced to a CFA, not scanlines of interleaved RGB
// - tile dims are in packed half-res space, not sensor resolution

#pragma once

#include <glib.h>
#include <stdint.h>

#include "common/ai/restore.h"
#include "common/image.h"  // for dt_imgid_t

struct dt_image_t;
struct _dt_job_t;

// @brief Run the RawNIND bayer denoise pipeline end-to-end.
//
// @param ctx      loaded bayer context (dt_restore_load_rawdenoise_bayer)
// @param img      source image (metadata for preprocessing + re-mosaic)
// @param cfa_in   sensor CFA as float (full sensor resolution, row-major,
//                 unnormalized: values in raw ADC units, no black
//                 subtracted). This is what rawspeed delivers in
//                 DT_MIPMAP_FULL for raw images.
// @param width    sensor width (img->width)
// @param height   sensor height (img->height)
// @param cfa_out  caller-allocated uint16 buffer of width*height samples.
//                 On success, contains the denoised mosaic in the same
//                 CFA layout and raw ADC range as the input.
// @param strength linear blend between original and denoised CFA in
//                 [0, 1]. 0 = pass-through the source CFA, 1 = full
//                 model output. Applied per sample at the end of the
//                 tile postprocess so tile boundaries stay seamless.
// @param control_job job handle for progress/cancellation (NULL-safe)
// @return 0 on success
int dt_restore_raw_bayer(dt_restore_context_t *ctx,
                         const struct dt_image_t *img,
                         const float *cfa_in,
                         int width,
                         int height,
                         uint16_t *cfa_out,
                         float strength,
                         struct _dt_job_t *control_job);

// @brief Bayer preview through darktable's real pixelpipe — "preview = batch".
//
// Runs model inference on the displayed crop, re-mosaics the output back
// to CFA (same un-WB / un-normalise / clip logic as dt_restore_raw_bayer),
// patches it into a full-sensor copy of the source, then runs darktable's
// full pixelpipe TWICE — once on the patched (denoised) CFA for the
// "after" view, once on the original CFA for the "before" view. Both
// results go through the image's complete history stack (including
// temperature / filmic / output profile), so the displayed preview
// matches what the user will see after Process + re-import.
//
// The strength slider should blend out_before_rgb and out_denoised_rgb
// at display time; this entry always returns the "strength = 1" denoised
// result.
//
// Expensive: two full pipelined renders per refresh on top of the model
// inference. Typically 2–5 seconds depending on sensor size and iop stack
// complexity. Use dt_restore_raw_bayer_preview for cheaper (but
// colour-approximate) previews.
//
// @param ctx        loaded bayer context
// @param img        source image metadata
// @param imgid      image id (used by the pixelpipe)
// @param cfa_full   full-sensor CFA as float (cache in neural_restore)
// @param width      sensor width
// @param height     sensor height
// @param crop_x     displayed crop top-left x (sensor coords, snapped mod 2)
// @param crop_y     displayed crop top-left y (snapped mod 2)
// @param crop_w     displayed crop width (snapped mod 2)
// @param crop_h     displayed crop height (snapped mod 2)
// @param out_before_rgb    caller-frees with g_free. 3ch interleaved
//                          (*out_w * *out_h * 3 floats), linear Rec.709,
//                          pipe output for the original CFA.
// @param out_denoised_rgb  caller-frees with g_free. same shape, pipe
//                          output for the denoised-patched CFA at α=1.
// @param out_w             receives actual rendered width (may differ
//                          from crop_w when user history contains
//                          geometry-modifying modules; both returned
//                          buffers share these dims).
// @param out_h             receives actual rendered height.
// @return 0 on success; both outputs NULL on failure.
int dt_restore_raw_bayer_preview_piped(dt_restore_context_t *ctx,
                                       const struct dt_image_t *img,
                                       dt_imgid_t imgid,
                                       const float *cfa_full,
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
