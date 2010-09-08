/*
    This file is part of darktable,
    copyright (c) 2010 Andrey Kaminsky.

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
#ifndef _LIBRAW_INTERNAL_FUNCS_VCD_H
#define _LIBRAW_INTERNAL_FUNCS_VCD_H
void refinement();
void ahd_interpolate_mod();
void ahd_partial_interpolate(int threshold_value);
void es_median_filter();
void median_filter_new();
#endif
