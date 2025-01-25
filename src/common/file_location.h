/*
    This file is part of darktable,
    Copyright (C) 2012-2025 darktable developers.

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

#include <gtk/gtk.h>
#include <string.h>

G_BEGIN_DECLS

// Returns the users home directory
gchar *dt_loc_get_home_dir(const gchar *user);

// Init all dirs
gboolean dt_loc_init(const char *datadir,
                     const char *moduledir,
                     const char *localedir,
                     const char *configdir,
                     const char *cachedir,
                     const char *tmpdir);

// Init systemwide data dir
void dt_loc_init_datadir(const char *application_directory,
                         const char *datadir);

// Init the plugin dir
void dt_loc_init_plugindir(const char *application_directory,
                           const char *plugindir);

// Init the locale dir
void dt_loc_init_localedir(const char *application_directory,
                           const char *localedir);

// Init share dir
void dt_loc_init_sharedir(const char* application_directory);

// Init user tmp dir
gboolean dt_loc_init_tmp_dir(const char *tmpdir);

// Init user config dir
gboolean dt_loc_init_user_config_dir(const char *configdir);

// Init user cache dir
gboolean dt_loc_init_user_cache_dir(const char *cachedir);

// Init specific dir. Default value is appended to application_directory
// if application_directory is not NULL.
gchar *dt_loc_init_generic(const char *absolute_value,
                           const char *application_directory,
                           const char *default_value);

// Checking if we can open the directory.
gboolean dt_check_opendir(const char* text, const char* directory);

// temporary backward_compatibility
void dt_loc_get_datadir(char *datadir, size_t bufsize);
void dt_loc_get_sharedir(char *sharedir, size_t bufsize);
void dt_loc_get_kerneldir(char *kerneldir, size_t bufsize);
void dt_loc_get_plugindir(char *plugindir, size_t bufsize);
void dt_loc_get_localedir(char *localedir, size_t bufsize);
void dt_loc_get_tmp_dir(char *tmpdir, size_t bufsize);
void dt_loc_get_user_config_dir(char *configdir, size_t bufsize);
void dt_loc_get_user_cache_dir(char *cachedir, size_t bufsize);

G_END_DECLS

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
