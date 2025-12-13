/*
 *    This file is part of darktable,
 *    Copyright (C) 2016-2020 darktable developers.
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

/*
  returns dimension and available color channels if pointer is provided.
  parameter planes tells how many color channels the output *float shall have.
*/
float *dt_read_pfm(const char *filename, int *error, int *wd, int *ht, int *ch, const size_t planes);

/*
  we support 3 types of bpp for *data
  bpp=2 1 16bit int
  bpp=4 1 float
  bpp=16 4 floats
*/
void dt_write_pfm(const char *filename, const size_t width, const size_t height, const void *data, const size_t bpp);

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

