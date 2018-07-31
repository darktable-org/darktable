/*
    This file is part of darktable,
    copyright (c) 2011 Henrik Andersson.

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

struct dt_selection_t;

struct dt_selection_t *dt_selection_new();
void dt_selection_free(struct dt_selection_t *selection);

/** inverts the current selection */
void dt_selection_invert(struct dt_selection_t *selection);
/** clears the selection */
void dt_selection_clear(struct dt_selection_t *selection);
/** adds imgid to the current selection */
void dt_selection_select(struct dt_selection_t *selection, uint32_t imgid);
/** removes imgid from the current selection */
void dt_selection_deselect(struct dt_selection_t *selection, uint32_t imgid);
/** clears current selection and adds imgid */
void dt_selection_select_single(struct dt_selection_t *selection, uint32_t imgid);
/** toggles selection of image in the current selection */
void dt_selection_toggle(struct dt_selection_t *selection, uint32_t imgid);
/** selects images range last_single_id to imgid */
void dt_selection_select_range(struct dt_selection_t *selection, uint32_t imgid);
/** selects all images from current collection */
void dt_selection_select_all(struct dt_selection_t *selection);
/** selects all images from filmroll of last single selected image */
void dt_selection_select_filmroll(struct dt_selection_t *selection);
/** selects all unaltered images in the current collection */
void dt_selection_select_unaltered(struct dt_selection_t *selection);
/** selects a set of images from a list. the list is unaltered */
void dt_selection_select_list(struct dt_selection_t *selection, GList *list);

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
