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

#ifndef __FILE_LOCATION_H__
#define __FILE_LOCATION_H__

#include <string.h>
#include <gtk/gtk.h>

/** returns the users home directory */
gchar* dt_loc_get_home_dir(const gchar* user);

/** get systemwide data dir */
void dt_loc_get_datadir(char *datadir, size_t bufsize);
/** get the plugin dir */
void dt_loc_get_plugindir(char *plugindir, size_t bufsize);
/** get user local dir */
void dt_loc_get_user_local_dir(char *localdir, size_t bufsize);
/** get user config dir */
void dt_loc_get_user_config_dir(char *configdir, size_t bufsize);
/** get user cache dir */
void dt_loc_get_user_cache_dir(char *cachedir, size_t bufsize);

#endif
