/*
    This file is part of darktable,
    Copyright (C) 2012-2024 darktable developers.

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
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>

#include "common/metadata.h"

/** initialize entry with the variables table */
void dt_gtkentry_setup_variables_completion(GtkEntry *entry);

/** add a metadata to the variables substitution */
void dt_gtkentry_variables_add_metadata(dt_metadata_t *metadata);

/** remove a metadata from the variables substitution */
void dt_gtkentry_variables_remove_metadata(dt_metadata_t *metadata);


// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

