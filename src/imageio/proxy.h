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

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Compute the proxy path for a raw file: @raw_path + ".proxy.avif".
 * The proxy lives as a sidecar next to the original raw file.
 *
 * @buf / @buflen  output buffer (PATH_MAX recommended).
 * Returns TRUE on success.
 */
gboolean dt_imageio_proxy_path(const char *raw_path, char *buf, size_t buflen);

/*
 * Create a proxy AVIF for the image at @raw_path.
 * The proxy is written to the path returned by dt_imageio_proxy_path().
 *
 * @quality  0-100 (higher = better, default 60).
 *
 * Returns TRUE on success, FALSE on any error.
 * Requires HAVE_LIBRAW; returns FALSE immediately when libraw is absent.
 */
gboolean dt_imageio_create_proxy(const char *raw_path, int quality);

/*
 * Embed minimal camera identification EXIF (Make + Model) into an already-
 * written proxy AVIF.  Without this, darktable cannot look up the camera's
 * color matrix in colorin when the original RAW is absent, causing a visible
 * color mismatch between the proxy and the original render.
 *
 * Called automatically by dt_imageio_create_proxy.  Exposed here for
 * external callers that create or re-export proxy files independently.
 *
 * Returns 1 on success, 0 on error (non-fatal).
 */
int dt_imageio_proxy_write_camera_exif(const char *proxy_path,
                                       const char *make,
                                       const char *model);

#ifdef __cplusplus
}
#endif
