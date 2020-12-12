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

// default number of iterations to run for dt_box_mean
#define BOX_ITERATIONS 8

void dt_box_mean(float *const buf, const int height, const int width, const int ch,
                 const int radius, const int interations);

void dt_box_min(float *const buf, const int height, const int width, const int ch, const int radius);
void dt_box_max(float *const buf, const int height, const int width, const int ch, const int radius);

