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

#pragma once

typedef struct
{
  const char *make;
  const char *model;
  const char *name;
  int tuning;
  double channels[4];
} dt_wb_data;

/** the number of white balance presets */
int dt_wb_presets_count(void);

//** the k-th wb data on the store */
dt_wb_data *dt_wb_preset(const int k);

/** read the white-balance presets file once on startup */
void dt_wb_presets_init(const char *alternative);

/** interpolate two given wb data, place result in out */
void dt_wb_preset_interpolate
(const dt_wb_data *const p1, // the smaller tuning
 const dt_wb_data *const p2, // the larger tuning (can't be == p1)
 dt_wb_data *out);           // has tuning initialized

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
