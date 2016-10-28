/*
    This file is part of darktable,
    copyright (c) 2010 tobias ellinghaus.

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
#include "gui/gtk.h"
#include "metadata_gen.h"

/** Set metadata for a specific image, or all selected for id == -1. */
void dt_metadata_set(int id, const char *key, const char *value);
/** Get metadata for a specific image, or all selected for id == -1.
    For keys which return a string, the caller has to make sure that it
    is freed after usage. */
GList *dt_metadata_get(int id, const char *key, uint32_t *count);
/** Remove metadata from specific images, or all selected for id == -1. */
void dt_metadata_clear(int id);

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
