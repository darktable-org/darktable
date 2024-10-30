/*
    This file is part of darktable,
    Copyright (C) 2024 darktable developers.

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

void darktable_splash_screen_create(GtkWindow *parent, gboolean force);
void darktable_splash_screen_set_progress(const char *msg);
void darktable_splash_screen_set_progress_percent(const char *msg, double fraction, double elapsed);
void darktable_splash_screen_destroy();

void darktable_splash_screen_get_geometry(gint *x, gint *y, gint *width, gint *height);

void darktable_exit_screen_create(GtkWindow *parent, gboolean force);
void darktable_exit_screen_destroy();

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
