/*
    This file is part of darktable,
    Copyright (C) 2009-2020 darktable developers.

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

#include "common/image.h"
#include "common/mipmap_cache.h"

#include <png.h>

typedef struct dt_imageio_png_t
{
  int max_width, max_height;
  int width, height;
  int color_type, bit_depth;
  int bpp;
  FILE *f;
  png_structp png_ptr;
  png_infop info_ptr;
} dt_imageio_png_t;

int read_header(const char *filename, dt_imageio_png_t *png);
int read_image(dt_imageio_png_t *png, void *out);

dt_imageio_retval_t dt_imageio_open_png(dt_image_t *img, const char *filename, dt_mipmap_buffer_t *buf);
int dt_imageio_png_read_profile(const char *filename, uint8_t **out, dt_colorspaces_cicp_t *cicp);

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
