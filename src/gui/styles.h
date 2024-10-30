/*
    This file is part of darktable,
    Copyright (C) 2010-2024 darktable developers.

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

#include <cairo.h>

/** extract the style's name from the XML in the given file */
/** returns NULL if the contents are not valid XML or do not name the style */
gchar *dt_get_style_name(const char *filename);

/** import styles stored in the shared directory if they are not already in our database */
void dt_import_default_styles(const char *sharedir);

/** shows a dialog for creating a new style */
void dt_gui_styles_dialog_new(const dt_imgid_t imgid);

/** shows a dialog for editing existing style */
void dt_gui_styles_dialog_edit(const char *name, char **new_name);

cairo_surface_t *dt_gui_get_style_preview(const dt_imgid_t imgid, const char *name);

GtkWidget *dt_gui_style_content_dialog(char *name, const dt_imgid_t imgid);

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
