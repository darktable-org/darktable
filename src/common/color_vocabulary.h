/*
    This file is part of darktable,
    Copyright (C) 2022 darktable developers.

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

#include "common/colorspaces_inline_conversions.h"

#include <glib.h>


// declare a model average ± standard deviation
typedef struct gaussian_stats_t
{
  float avg;
  float std;
} gaussian_stats_t;


// bounds of the range
typedef struct range_t
{
  float bottom;
  float top;
} range_t;


// ID keys for the ethnicities database
typedef enum ethnicities_t
{
  ETHNIE_CHINESE = 0,
  ETHNIE_THAI = 1,
  ETHNIE_KURDISH = 2,
  ETHNIE_CAUCASIAN = 3,
  ETHNIE_AFRICAN_AM = 4,
  ETHNIE_MEXICAN = 5,
  ETHNIE_END = 6
} ethnicities_t;


// Translatable names for ethnicities
typedef struct ethnicity_t
{
  char *name;
  ethnicities_t ethnicity;
} ethnicity_t;


// Database entry for skin parts color of an ethnicity
typedef struct skin_color_t
{
  char *name;
  ethnicities_t ethnicity;
  gaussian_stats_t L;
  gaussian_stats_t a;
  gaussian_stats_t b;
} skin_color_t;


#define SKINS 16

// returns a color name for color
const char *Lch_to_color_name(dt_aligned_pixel_t color);
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
