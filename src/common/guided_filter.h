/*
    This file is part of darktable,
    copyright (c) 2018 Heiko Bauke.

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

#include "common/opencl.h"

struct dt_iop_roi_t;

void guided_filter(const float * guide, const float * in, float * out,
                   int width, int height, int ch, int w,
                   float sqrt_eps, float min, float max);

#ifdef HAVE_OPENCL
void guided_filter_cl(int devid, cl_mem guide, cl_mem in, cl_mem out,
                      int width, int height, int ch, int w,
                      float sqrt_eps, float min, float max);
#endif
