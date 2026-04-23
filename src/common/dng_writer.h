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

// dng_writer — minimal DNG CFA writer
//
// writes a single-plane uint16 Bayer mosaic plus enough DNG metadata
// to let a raw processor (darktable, adobe, etc.) re-import the file
// and run the normal raw pipeline: black/white level, CFA pattern,
// AsShotNeutral (white balance), ColorMatrix1 (camRGB -> CIE XYZ),
// camera make/model, and a pass-through of the source EXIF blob.
//
// this writer is intentionally narrow in scope:
// - bayer only (no X-Trans, no LinearRaw demosaiced DNG)
// - uncompressed strip layout
// - single IFD (no embedded JPEG preview or thumbnails)
// - 16-bit integer data only
//
// the consumer of the DNG (darktable itself) does not need more than
// this for the neural restore round-trip.

#pragma once

#include <stdint.h>
#include <glib.h>

struct dt_image_t;

// @brief Write a Bayer CFA mosaic as a DNG file.
//
// The output file contains a single IFD with PhotometricInterpretation=CFA.
// All DNG metadata required for darktable re-import is sourced from @p img:
//   - BlackLevel[4]                from img->raw_black_level_separate
//   - WhiteLevel                   from img->raw_white_point
//   - CFAPattern / CFARepeatDim    from img->buf_dsc.filters (dcraw format)
//   - AsShotNeutral                from img->wb_coeffs (inverted)
//   - ColorMatrix1                 from img->adobe_XYZ_to_CAM
//   - Make / Model / UniqueModel   from img->camera_maker / camera_model
//
// @param filename output path (UTF-8)
// @param cfa      Bayer mosaic (uint16, width * height samples, row-major)
// @param width    image width in pixels (CFA samples per row)
// @param height   image height in rows
// @param img      source image, for DNG metadata
// @param exif_blob optional Exif blob to embed (NULL = skip)
// @param exif_len  size of exif_blob in bytes
// @return 0 on success, non-zero on failure (file is removed on failure)
int dt_dng_write_cfa_bayer(const char *filename,
                           const uint16_t *cfa,
                           int width,
                           int height,
                           const struct dt_image_t *img,
                           const void *exif_blob,
                           int exif_len);

// @brief Write a demosaicked 3-channel linear DNG.
//
// Used for sensors the bayer DNG round-trip can't handle (X-Trans,
// Foveon-like, pre-demosaicked raws). The output file has
// PhotometricInterpretation=LinearRaw, SamplesPerPixel=3, and carries
// the camera's ColorMatrix1 / AsShotNeutral / BlackLevel / WhiteLevel
// so darktable re-imports it as a raw-origin image and skips its own
// demosaic stage.
//
// Pixel data is interpreted as float-normalized camRGB in [0, ~1+]
// (1.0 = source sensor white point after black subtract). The writer
// scales that to uint16 using black = img->raw_black_level,
// white = img->raw_white_point, so the encoding matches what the
// corresponding raw CFA data would be in ADC units.
//
// @param filename output path (UTF-8)
// @param rgb      interleaved 3ch float RGB, width*height*3 samples
// @param width    image width in pixels
// @param height   image height in pixels
// @param img      source image, for DNG metadata + encoding range
// @param exif_blob optional Exif blob to embed (NULL = skip)
// @param exif_len  size of exif_blob in bytes
// @return 0 on success, non-zero on failure (file removed on failure)
int dt_dng_write_linear(const char *filename,
                        const float *rgb,
                        int width,
                        int height,
                        const struct dt_image_t *img,
                        const void *exif_blob,
                        int exif_len);

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
