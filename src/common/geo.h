/*
 *    This file is part of darktable,
 *    Copyright (C) 2016-2021 darktable developers.
 *
 *    darktable is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 *
 *    darktable is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

typedef enum dt_geo_map_display_t
{
  MAP_DISPLAY_NONE,
  MAP_DISPLAY_POINT,
  MAP_DISPLAY_TRACK,
  MAP_DISPLAY_POLYGON,
  MAP_DISPLAY_THUMB
} dt_geo_map_display_t;

typedef struct dt_geo_map_display_point_t
{
  float lat, lon;
} dt_geo_map_display_point_t;

typedef struct dt_map_box_t
{
  float lon1;
  float lat1;
  float lon2;
  float lat2;
} dt_map_box_t;

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

