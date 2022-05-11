/*
    This file is part of darktable,
    Copyright (C) 2011-2021 darktable developers.

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

#include <stdint.h>
#include "image.h"

typedef struct dt_dng_gain_map_t
{
  uint32_t top;
  uint32_t left;
  uint32_t bottom;
  uint32_t right;
  uint32_t plane;
  uint32_t planes;
  uint32_t row_pitch;
  uint32_t col_pitch;
  uint32_t map_points_v;
  uint32_t map_points_h;
  double map_spacing_v;
  double map_spacing_h;
  double map_origin_v;
  double map_origin_h;
  uint32_t map_planes;
  float map_gain[];
} dt_dng_gain_map_t;

void dt_dng_opcode_process_opcode_list_2(uint8_t *buf, uint32_t size, dt_image_t *img);
