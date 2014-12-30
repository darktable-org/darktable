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

/** dynamically allocate and concatenate string */
gchar *dt_util_dstrcat(gchar *str, const gchar *format, ...);

/** replace all occurrences of pattern by substitute. the returned value has to be freed after use. */
gchar *dt_util_str_replace(const gchar *string, const gchar *pattern, const gchar *substitute);
/** count the number of occurrences of needle in haystack */
guint dt_util_str_occurence(const gchar *haystack, const gchar *needle);
/** generate a string from the elements of the list, separated by separator. the result has to be freed. */
gchar *dt_util_glist_to_str(const gchar *separator, GList *items);
/** take a list of strings and remove all duplicates. the result will be sorted. */
GList *dt_util_glist_uniq(GList *items);
/** fixes the given path by replacing a possible tilde with the correct home directory */
gchar *dt_util_fix_path(const gchar *path);
size_t dt_utf8_strlcpy(char *dest, const char *src, size_t n);
/** get the size of a file in bytes */
off_t dt_util_get_file_size(const char *filename);
/** returns true if dirname is empty */
gboolean dt_util_is_dir_empty(const char *dirname);
/** returns a valid UTF-8 string for the given char array. has to be freed with g_free(). */
gchar *dt_util_foo_to_utf8(const char *string);

typedef enum dt_logo_season_t
{
  DT_LOGO_SEASON_NONE = 0,
  DT_LOGO_SEASON_HALLOWEEN = 1,
  DT_LOGO_SEASON_XMAS = 2,
} dt_logo_season_t;

/** returns the dt logo season to use right now */
dt_logo_season_t get_logo_season(void);

gchar *dt_util_latitude_str(float latitude);
gchar *dt_util_longitude_str(float longitude);

#endif
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
