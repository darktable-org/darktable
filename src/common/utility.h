/*
    This file is part of darktable,
    copyright (c) 2010 Tobias Ellinghaus.

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

#ifndef __UTILITY_H__
#define __UTILITY_H__

#include <string.h>
#include <gtk/gtk.h>

/** replace all occurences of pattern by substitute. the returned value has to be freed after use. */
gchar* dt_util_str_replace(const gchar* string, const gchar* pattern, const gchar* substitute);
/** count the number of occurences of needle in haystack */
guint dt_util_str_occurence(const gchar *haystack,const gchar *needle);
/** generate a string from the elements of the list, separated by separator. the list is freed, the result has to be freed. */
gchar* dt_util_glist_to_str(const gchar* separator, GList * items, const unsigned int count);
/** returns the users home directory */
gchar* dt_util_get_home_dir(const gchar* user);
/** fixes the given path by replacing a possible tilde with the correct home directory */
gchar* dt_util_fix_path(const gchar* path);

/** get systemwide data dir */
void dt_util_get_datadir(char *datadir, size_t bufsize);
/** get the plugin dir */
void dt_util_get_plugindir(char *plugindir, size_t bufsize);
/** get user local dir */
void dt_util_get_user_local_dir(char *localdir, size_t bufsize);
/** get user config dir */
void dt_util_get_user_config_dir(char *configdir, size_t bufsize);
/** get user cache dir */
void dt_util_get_user_cache_dir(char *cachedir, size_t bufsize);

#endif
