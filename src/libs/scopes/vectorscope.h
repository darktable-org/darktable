/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

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

#include "common/color_harmony.h"
#include "scopes.h"

void dt_vec_get_harmony(dt_scopes_mode_t *mode,
                        dt_color_harmony_guide_t *guide);
void dt_vec_set_harmony(dt_scopes_mode_t *mode,
                        const dt_color_harmony_guide_t *guide);
void dt_vec_set_vectorscope_type(dt_scopes_mode_t *mode,
                                 const int type);
void dt_vec_get_sector_angles(const dt_color_harmony_type_t type,
                              const int rotation,
                              float *angles,
                              int *n);
void dt_vec_set_harmony_changed_callback(dt_scopes_mode_t *mode,
                                         void (*cb)(const dt_color_harmony_guide_t *, void *),
                                         void *user_data);

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
