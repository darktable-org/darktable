/*
    This file is part of darktable,
    Copyright (C) 2010-2021 darktable developers.

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

/** shows the preferences dialog and blocks until it's closed. */
void dt_gui_preferences_show();

// return the widget for a given preference key
GtkWidget *dt_gui_preferences_bool(GtkGrid *grid, const char *key, const guint col,
                                   const guint line, const gboolean swap);
GtkWidget *dt_gui_preferences_int(GtkGrid *grid, const char *key, const guint col,
                                  const guint line);
GtkWidget *dt_gui_preferences_enum(GtkGrid *grid, const char *key, const guint col,
                                   const guint line);
GtkWidget *dt_gui_preferences_string(GtkGrid *grid, const char *key, const guint col,
                                     const guint line);

// update widget with current preference
void dt_gui_preferences_bool_update(GtkWidget *widget);
void dt_gui_preferences_int_update(GtkWidget *widget);
void dt_gui_preferences_enum_update(GtkWidget *widget);
void dt_gui_preferences_string_update(GtkWidget *widget);

// reset widget to default value
void dt_gui_preferences_bool_reset(GtkWidget *widget);
void dt_gui_preferences_int_reset(GtkWidget *widget);
void dt_gui_preferences_enum_reset(GtkWidget *widget);
void dt_gui_preferences_string_reset(GtkWidget *widget);

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

