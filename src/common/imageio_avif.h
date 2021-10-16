/*
 * This file is part of darktable,
 * Copyright (C) 2019 darktable developers.
 *
 *  darktable is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  darktable is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with darktable.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "common/image.h"
#include "common/mipmap_cache.h"

dt_imageio_retval_t dt_imageio_open_avif(dt_image_t *img,
                                         const char *filename,
                                         dt_mipmap_buffer_t *buf);

int dt_imageio_avif_read_profile(const char *filename, uint8_t **out, dt_colorspaces_cicp_t *cicp);
