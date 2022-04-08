/*
    This file is part of darktable,
    Copyright (C) 2011-2020 darktable developers.

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

#include "common/darktable.h"
#include <gtk/gtk.h>

#define DT_VIEW_RATINGS_MASK 0x7
// first three bits of dt_view_image_over_t

/** get rating for the specified image */
int dt_ratings_get(const int imgid);

/** apply rating to the specified image */
void dt_ratings_apply_on_image(const int imgid, const int rating, const gboolean single_star_toggle,
                               const gboolean undo_on, const gboolean group_on);

/** apply rating to all images in the list */
void dt_ratings_apply_on_list(const GList *list, const int rating, const gboolean undo_on);

extern const struct dt_action_def_t dt_action_def_rating;

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

