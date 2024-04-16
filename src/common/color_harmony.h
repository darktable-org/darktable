/*
    This file is part of darktable,
    Copyright (C) 2023 darktable developers.

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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "common/darktable.h"

G_BEGIN_DECLS

typedef enum _color_harmony_type_t
{
  DT_COLOR_HARMONY_NONE = 0,
  DT_COLOR_HARMONY_MONOCHROMATIC,
  DT_COLOR_HARMONY_ANALOGOUS,
  DT_COLOR_HARMONY_ANALOGOUS_COMPLEMENTARY,
  DT_COLOR_HARMONY_COMPLEMENTARY,
  DT_COLOR_HARMONY_SPLIT_COMPLEMENTARY,
  DT_COLOR_HARMONY_DYAD,
  DT_COLOR_HARMONY_TRIAD,
  DT_COLOR_HARMONY_TETRAD,
  DT_COLOR_HARMONY_SQUARE,
  DT_COLOR_HARMONY_N // needs to be the last one
} dt_color_harmony_type_t;

typedef enum dt_color_harmony_width_t
{
  DT_COLOR_HARMONY_WIDTH_NORMAL = 0,
  DT_COLOR_HARMONY_WIDTH_LARGE,
  DT_COLOR_HARMONY_WIDTH_NARROW,
  DT_COLOR_HARMONY_WIDTH_LINE,
  DT_COLOR_HARMONY_WIDTH_N // needs to be the last one
} dt_color_harmony_width_t;

typedef struct _color_harmony_t
{
  dt_color_harmony_type_t type;
  int rotation;
  dt_color_harmony_width_t width;
} dt_color_harmony_guide_t;

typedef int32_t dt_harmony_guide_id_t;

// init layout to default value
void dt_color_harmony_init(dt_color_harmony_guide_t *layout);

// record color harmony in db for imgid
void dt_color_harmony_set(const dt_imgid_t imgid,
                          const dt_color_harmony_guide_t layout);

// get harmony id for the given image, -1 if there is no harmony recorded
dt_harmony_guide_id_t dt_color_harmony_get_id(const dt_imgid_t imgid);

// get color harmony for imgid, return FALSE if not found in db
gboolean dt_color_harmony_get(const dt_imgid_t imgid,
                              dt_color_harmony_guide_t *layout);

G_END_DECLS

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
