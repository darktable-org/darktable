/*
    This file is part of darktable,
    Copyright (C) 2014-2020 darktable developers.

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

#include <glib.h>
#include <inttypes.h>
#include <lcms2.h>
#include <stddef.h>

int dt_apply_printer_profile(void **in, uint32_t width, uint32_t height, int bpp, cmsHPROFILE hInProfile,
                             cmsHPROFILE hOutProfile, int intent, gboolean black_point_compensation);
// this routines takes as input an image of 8 or 16 bpp but always return a 8 bpp result. It is indeed better to
// apply the profile to a 16bit input but we do not need this for printing.

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
