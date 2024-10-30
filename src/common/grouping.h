/*
    This file is part of darktable,
    Copyright (C) 2011-2024 darktable developers.

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
#include <stdint.h>

#include "common/darktable.h"

/** add an image to a group */
void dt_grouping_add_to_group(const dt_imgid_t group_id,
                              const dt_imgid_t image_id);

/** remove an image from a group. returns the new group_id of the other images. */
dt_imgid_t dt_grouping_remove_from_group(const dt_imgid_t image_id);

/** make an image the representative of the group it is in. returns the new group_id. */
dt_imgid_t dt_grouping_change_representative(const dt_imgid_t image_id);

/** get images of the group */
GList *dt_grouping_get_group_images(const dt_imgid_t imgid);

/** add grouped images to images list */
void dt_grouping_add_grouped_images(GList **images);

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
