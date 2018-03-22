/*
    This file is part of darktable,
    copyright (c) 2012 Jeremy Rosen

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

/** returns the users home directory */
gchar *dt_loc_get_home_dir(const gchar *user);

/** init systemwide data dir */
void dt_loc_init_datadir(const char *datadir);
/** get the plugin dir */
void dt_loc_init_plugindir(const char *plugindir);
/** init the locale dir */
void dt_loc_init_localedir(const char *localedir);
/** get user local dir */
int dt_loc_init_tmp_dir(const char *tmpdir);
/** get user config dir */
void dt_loc_init_user_config_dir(const char *configdir);
/** get user cache dir */
void dt_loc_init_user_cache_dir(const char *cachedir);

/* temporary backward_compatibility*/
void dt_loc_get_datadir(char *datadir, size_t bufsize);
void dt_loc_get_plugindir(char *plugindir, size_t bufsize);
void dt_loc_get_localedir(char *localedir, size_t bufsize);
void dt_loc_get_tmp_dir(char *tmpdir, size_t bufsize);
void dt_loc_get_user_config_dir(char *configdir, size_t bufsize);
void dt_loc_get_user_cache_dir(char *cachedir, size_t bufsize);

#if defined(__MACH__) || defined(__APPLE__)
char *dt_loc_find_install_dir(const char *suffix, const char *searchname);
#endif

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
