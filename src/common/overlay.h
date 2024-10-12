/*
    This file is part of darktable,
    Copyright (C) 2023-2024 darktable developers.

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

G_BEGIN_DECLS

/*
  Record overlay_id as being used as overlay in imgid
*/
void dt_overlay_record(const dt_imgid_t imgid, const dt_imgid_t overlay_id);

/*
  Remove all overlay as being used in imgid
*/
void dt_overlays_remove(const dt_imgid_t imgid);

/*
  Remove overlay_id as being used as overlay in imgid
*/
void dt_overlay_remove(const dt_imgid_t imgid, const dt_imgid_t overlay_id);

/*
  Get list of all images used as overlay in imgid
*/
GList *dt_overlay_get_imgs(const dt_imgid_t imgid);

/*
  Get list of all images where overlay_id is used as overlay.
  If except_self is TRUE do not return the self image references
  (where imgid == overlay_id).
*/
GList *dt_overlay_get_used_in_imgs(const dt_imgid_t overlay_id,
                                   const gboolean except_self);

/* Return TRUE if overlay_id appears in the overlay tree of (is used by) imgid */
gboolean dt_overlay_used_by(const dt_imgid_t imgid, const dt_imgid_t overlay_id);

/* Add overlay references for the imgid history */
void dt_overlay_add_from_history(const dt_imgid_t imgid);

/* Remove overlay reference from history above a specific point in history */
void dt_overlay_remove_from_history(const dt_imgid_t imgid,
                                    const int num);
G_END_DECLS

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
