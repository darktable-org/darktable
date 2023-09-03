/*
 *    This file is part of darktable,
 *    Copyright (C) 2023 darktable developers.
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

#include "common/iop_profile.h"
#include "dttypes.h"

// Make a set of custom primaries starting from the primaries of the given profile.
// Rotate the chromaticity angles about the white point,
// along the profile gamut footprint boundary by the given amounts.
// Then scale them by the given amounts.
void dt_rotate_and_scale_primary(const dt_iop_order_iccprofile_info_t *const profile,
                                 const float scaling,
                                 const float rotation,
                                 const size_t primary_index,
                                 float new_xy[2]);
