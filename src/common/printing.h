/*
    This file is part of darktable,
    Copyright (C) 2021 darktable developers.

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

#include <glib.h>
#include <inttypes.h>

#define MAX_IMAGE_PER_PAGE 20

typedef struct _imgage_pos
{
  float x, y, width, height;
} dt_image_pos;

typedef struct _image_box
{
  int32_t imgid;
  int32_t max_width, max_height;
  int32_t exp_width, exp_height;
  dt_image_pos pos;              // relative pos (in per 10000)
  dt_image_pos screen;           // current screen pos (in pixels)
  dt_image_pos print;            // current print pos (in mm) depending on paper size + DPI
  uint16_t *buf;
} dt_image_box;

typedef struct dt_images_box
{
  gboolean auto_fit;
  int count;
  dt_image_box box[MAX_IMAGE_PER_PAGE];
  dt_image_pos screen_page;      // this is for reference and is the box of the white page (in mm)
                                 // in print module. it is the full page.
  dt_image_pos screen_page_area; // this is for reference and is the box of the white page (in mm)
                                 // in print module. it is the page area (without margins).
} dt_images_box;

// return the box index or -1 if (x, y) coordinate is not over an image
int dt_printing_get_image_box(dt_images_box *imgs, const int x, const int y);

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
