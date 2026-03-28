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

#include "common/darktable.h"
#include <glib.h>

G_BEGIN_DECLS

/** Library duplicates: same film roll + filename, non-minimum version rows. */
GList *dt_duplicate_review_get_duplicate_ids(void);

/**
 * Burst / spam heuristic: consecutive shots in the same film roll with close
 * capture time (only minimum-version rows per filename). Returns image ids
 * suggested as redundant (not the keeper).
 */
GList *dt_duplicate_review_get_burst_candidate_ids(void);

/** Free a list returned by the functions above (GList of imgid pointers). */
void dt_duplicate_review_free_id_list(GList *list);

G_END_DECLS
