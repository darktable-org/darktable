/*
    This file is part of darktable,
    Copyright (C) 2011-2026 darktable developers.

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

// imageio_dng — DNG writers
//
// Three entry points for three different DNG flavors:
//   - dt_imageio_dng_write_float      32-bit float CFA (HDR merge)
//   - dt_imageio_dng_write_cfa_bayer  16-bit uint Bayer CFA (raw round-trip)
//   - dt_imageio_dng_write_linear     16-bit uint LinearRaw 3ch (demosaicked)
//
// The float writer is hand-rolled byte assembly into a small TIFF
// header buffer; the two uint16 writers use libtiff.

#pragma once

#include <stdint.h>
#include <glib.h>

#include "common/dttypes.h"  // for dt_aligned_pixel_t

struct dt_image_t;

// optional embedded JPEG preview for the uint16 DNG writers. when
// non-NULL, the writer uses the canonical Adobe layout (IFD0 = JPEG
// preview, SubIFD0 = raw payload) so library browsers (Finder,
// Photomator, Lightroom) can render thumbnails without decoding the
// raw. when NULL, falls back to the historical single-IFD layout
typedef struct dt_imageio_dng_preview_t
{
  const uint8_t *data;     // pre-encoded JPEG bytes, 8-bit YCbCr
  int            len;      // length of @p data in bytes
  int            width;    // declared image width
  int            height;   // declared image height
} dt_imageio_dng_preview_t;

// @brief Write a 32-bit float CFA DNG (Bayer or X-Trans).
//
// Used by HDR merge: pixel data is float pre-normalized to
// [0, whitelevel], so values may exceed any single sensor's white
// point. The writer doesn't emit BlackLevel or ACTIVEAREA — the
// importer assumes black=0 and the buffer is at the dimensions you
// want displayed.
//
// @param filename output path (UTF-8)
// @param pixel    float CFA, wd*ht samples, row-major
// @param wd       image width in pixels
// @param ht       image height in pixels
// @param exif     optional Exif blob to embed (NULL = skip)
// @param exif_len size of @p exif in bytes
// @param filter   dcraw 2x2 CFA filters word, or 9u for X-Trans
// @param xtrans   X-Trans 6x6 pattern (used iff filter == 9u)
// @param whitelevel pre-normalized white level (typically 1.0f for HDR)
// @param wb_coeffs camera-RGB raw-to-white multipliers
// @param adobe_XYZ_to_CAM XYZ->cameraRGB matrix (4x3, only first 3 rows used)
void dt_imageio_dng_write_float(const char *filename,
                                const float *pixel,
                                int wd,
                                int ht,
                                void *exif,
                                int exif_len,
                                uint32_t filter,
                                const uint8_t xtrans[6][6],
                                float whitelevel,
                                const dt_aligned_pixel_t wb_coeffs,
                                const float adobe_XYZ_to_CAM[4][3]);

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
int dt_imageio_dng_write_cfa_bayer(const char *filename,
                                   const uint16_t *cfa,
                                   int width,
                                   int height,
                                   const struct dt_image_t *img,
                                   const void *exif_blob,
                                   int exif_len,
                                   const dt_imageio_dng_preview_t *preview);

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
int dt_imageio_dng_write_linear(const char *filename,
                                const float *rgb,
                                int width,
                                int height,
                                const struct dt_image_t *img,
                                const void *exif_blob,
                                int exif_len,
                                const dt_imageio_dng_preview_t *preview);

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
