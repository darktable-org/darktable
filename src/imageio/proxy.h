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
 * Compute the proxy path for a raw file.
 *
 * Proxies are stored in a central directory (config key
 * "plugins/p2p/proxy_dir", default ~/Pictures/dtproxy) under
 * YYYY/MM/DD/ subfolders derived from the raw file's mtime.
 *
 * @raw_path  absolute path to the raw file
 * @buf       output buffer (PATH_MAX recommended)
 * @buflen    size of @buf
 *
 * Returns TRUE when @buf has been filled, FALSE on error.
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
