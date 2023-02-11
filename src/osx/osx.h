/*
 *   This file is part of darktable,
 *   Copyright (C) 2014-2020 darktable developers.
 *
 *   darktable is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   darktable is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with darktable.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <gtk/gtk.h>

#ifdef __cplusplus
extern "C" {
#endif

void dt_osx_autoset_dpi(GtkWidget *widget);
float dt_osx_get_ppd();
void dt_osx_disallow_fullscreen(GtkWidget *widget);
gboolean dt_osx_file_trash(const char *filename, GError **error);
char* dt_osx_get_bundle_res_path();
void dt_osx_prepare_environment();
void dt_osx_focus_window();

#ifdef __cplusplus
}
#endif
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
