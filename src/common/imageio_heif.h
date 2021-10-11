/*
 * This file is part of darktable,
 * Copyright (C) 2021 darktable developers.
 *
 *  Copyright (c) 2021      Daniel Vogelbacher
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

#include "common/colorspaces.h"
#include "common/image.h"
#include "common/mipmap_cache.h"

struct heif_color_profile {
    dt_colorspaces_color_profile_type_t type;
    size_t icc_profile_size;
    uint8_t *icc_profile;
};

dt_imageio_retval_t dt_imageio_open_heif(dt_image_t *img,
                                         const char *filename,
                                         dt_mipmap_buffer_t *buf);
int dt_imageio_heif_read_profile(const char *filename,
                                 uint8_t **out,
                                 dt_colorspaces_cicp_t *cicp);

