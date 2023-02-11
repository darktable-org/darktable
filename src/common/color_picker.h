/*
    This file is part of darktable,
    Copyright (C) 2016-2020 darktable developers.

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

#include "common/iop_profile.h"
#include "libs/colorpicker.h"

struct dt_iop_buffer_dsc_t;
struct dt_iop_roi_t;
enum dt_iop_colorspace_type_t;

void dt_color_picker_helper(const struct dt_iop_buffer_dsc_t *dsc, const float *const pixel,
                            const struct dt_iop_roi_t *roi, const int *const box,
                            const gboolean denoise,
                            lib_colorpicker_stats pick,
                            const enum dt_iop_colorspace_type_t image_cst,
                            const enum dt_iop_colorspace_type_t picker_cst,
                            const dt_iop_order_iccprofile_info_t *const profile);

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
