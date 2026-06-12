/*
    This file is part of darktable,
    Copyright (C) 2024 darktable developers.

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

#include "common/darktable.h"

/*
 * Proxy media: half-resolution AVIF sidecars (IMG.CR2.proxy.avif) that
 * preserve camera-native linear sensor data without demosaicing, white
 * balance, or colour-space conversion.  darktable loads them transparently
 * when the original raw file is absent.
 *
 * The AVIF is stored with:
 *   - 12-bit depth, YUV 4:4:4
 *   - AVIF_MATRIX_COEFFICIENTS_IDENTITY   (Y=R, Cb=G, Cr=B, no rotation)
 *   - AVIF_COLOR_PRIMARIES_UNSPECIFIED    (camera native)
 *   - AVIF_TRANSFER_CHARACTERISTICS_LINEAR (linear light)
 *   - Full range
 */

/*
 * Create a proxy AVIF for the image at @raw_path.
 * The proxy is written to @raw_path + ".proxy.avif".
 *
 * @quality  0-100 (higher = better, default 60).
 *           Mapped to the appropriate libavif quantizer range.
 *
 * Returns TRUE on success, FALSE on any error.
 * Requires HAVE_LIBRAW; returns FALSE immediately when libraw is absent.
 */
gboolean dt_imageio_create_proxy(const char *raw_path, int quality);
