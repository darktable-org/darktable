/*
    This file is part of darktable,
    copyright (c) 2010-2011 Henrik Andersson, Tobias Ellinghaus.

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

/* getpwnam_r availibility check */
#if defined __APPLE__ || defined _POSIX_C_SOURCE >= 1 || defined _XOPEN_SOURCE || defined _BSD_SOURCE        \
    || defined _SVID_SOURCE || defined _POSIX_SOURCE || defined __DragonFly__ || defined __FreeBSD__         \
    || defined __NetBSD__ || defined __OpenBSD__
#include <pwd.h>
#include <sys/types.h>
#include <unistd.h>
#include "darktable.h"
#endif

#include <sys/stat.h>

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "utility.h"
#include "file_location.h"

gchar *dt_util_dstrcat(gchar *str, const gchar *format, ...)
{
  va_list args;
  gchar *ns;
  va_start(args, format);
  size_t clen = str ? strlen(str) : 0;
  int alen = g_vsnprintf(NULL, 0, format, args);
  int nsize = alen + clen + 1;

  /* realloc for new string */
  ns = g_realloc(str, nsize);
  if(str == NULL) ns[0] = '\0';
  va_end(args);

  /* append string */
  va_start(args, format);
  g_vsnprintf(ns + clen, alen + 1, format, args);
  va_end(args);

  ns[nsize - 1] = '\0';

  return ns;
}

guint dt_util_str_occurence(const gchar *haystack, const gchar *needle)
{
  guint o = 0;
  if(haystack && needle)
  {
    const gchar *p = haystack;
    if((p = g_strstr_len(p, strlen(p), needle)) != NULL)
    {
      do
      {
        o++;
      } while((p = g_strstr_len((p + 1), strlen(p + 1), needle)) != NULL);
    }
  }
  return o;
}

gchar *dt_util_str_replace(const gchar *string, const gchar *pattern, const gchar *substitute)
{
  gint occurences = dt_util_str_occurence(string, pattern);
  gchar *nstring;
  if(occurences)
  {
    nstring = g_malloc_n(strlen(string) + (occurences * strlen(substitute)) + 1, sizeof(gchar));
    const gchar *pend = string + strlen(string);
    const gchar *s = string, *p = string;
    gchar *np = nstring;
    if((s = g_strstr_len(s, strlen(s), pattern)) != NULL)
    {
      do
      {
        memcpy(np, p, s - p);
        np += (s - p);
        memcpy(np, substitute, strlen(substitute));
        np += strlen(substitute);
        p = s + strlen(pattern);
      } while((s = g_strstr_len((s + 1), strlen(s + 1), pattern)) != NULL);
    }
    memcpy(np, p, pend - p);
    np[pend - p] = '\0';
  }
  else
    nstring = g_strdup(string); // otherwise it's a hell to decide whether to free this string later.
  return nstring;
}

gchar *dt_util_glist_to_str(const gchar *separator, GList *items)
{
  if(items == NULL) return NULL;

  const unsigned int count = g_list_length(items);
  gchar *result = NULL;

  // add the entries to an char* array
  items = g_list_first(items);
  gchar **strings = g_malloc0_n(count + 1, sizeof(gchar *));
  if(items != NULL)
  {
    int i = 0;
    do
    {
      strings[i++] = items->data;
    } while((items = g_list_next(items)) != NULL);
  }

  // join them into a single string
  result = g_strjoinv(separator, strings);

  // free the array
  g_free(strings);

  return result;
}

GList *dt_util_glist_uniq(GList *items)
{
  if(!items) return NULL;

  gchar *last = NULL;
  GList *last_item = NULL;

  items = g_list_sort(items, (GCompareFunc)g_strcmp0);
  GList *iter = items;
  while(iter)
  {
    gchar *value = (gchar *)iter->data;
    if(!g_strcmp0(last, value))
    {
      g_free(value);
      items = g_list_delete_link(items, iter);
      iter = last_item;
    }
    else
    {
      last = value;
      last_item = iter;
    }
    iter = g_list_next(iter);
  }
  return items;
}


gchar *dt_util_fix_path(const gchar *path)
{
  if(path == NULL || *path == '\0')
  {
    return NULL;
  }

  gchar *rpath = NULL;

  /* check if path has a prepended tilde */
  if(path[0] == '~')
  {
    size_t len = strlen(path);
    char *user = NULL;
    int off = 1;

    /* if the character after the tilde is not a slash we parse
     * the path until the next slash to extend this part with the
     * home directory of the specified user
     *
     * e.g.: ~foo will be evaluated as the home directory of the
     * user foo */

    if(len > 1 && path[1] != '/')
    {
      while(path[off] != '\0' && path[off] != '/')
      {
        ++off;
      }

      user = g_strndup(path + 1, off - 1);
    }

    gchar *home_path = dt_loc_get_home_dir(user);
    g_free(user);

    if(home_path == NULL)
    {
      return g_strdup(path);
    }

    rpath = g_build_filename(home_path, path + off, NULL);
  }
  else
  {
    rpath = g_strdup(path);
  }

  return rpath;
}

/**
 * dt_utf8_strlcpy:
 * @dest: buffer to fill with characters from @src
 * @src: UTF-8 encoded string
 * @n: size of @dest
 *
 * Like the BSD-standard strlcpy() function, but
 * is careful not to truncate in the middle of a character.
 * The @src string must be valid UTF-8 encoded text.
 * (Use g_utf8_validate() on all text before trying to use UTF-8
 * utility functions with it.)
 *
 * Return value: strlen(src)
 * Implementation by Philip Page, see https://bugzilla.gnome.org/show_bug.cgi?id=520116
 **/
size_t dt_utf8_strlcpy(char *dest, const char *src, size_t n)
{
  register const gchar *s = src;
  while(s - src < n && *s)
  {
    s = g_utf8_next_char(s);
  }

  if(s - src >= n)
  {
    /* We need to truncate; back up one. */
    s = g_utf8_prev_char(s);
    strncpy(dest, src, s - src);
    dest[s - src] = '\0';
    /* Find the full length for return value. */
    while(*s)
    {
      s = g_utf8_next_char(s);
    }
  }
  else
  {
    /* Plenty of room, just copy */
    strncpy(dest, src, s - src);
    dest[s - src] = '\0';
  }
  return s - src;
}

off_t dt_util_get_file_size(const char *filename)
{
#ifdef __WIN32__
  struct _stati64 st;
  if(_stati64(filename, &st) == 0) return st.st_size;
#else
  struct stat st;
  if(stat(filename, &st) == 0) return st.st_size;
#endif

  return -1;
}

gboolean dt_util_is_dir_empty(const char *dirname)
{
  int n = 0;
  GDir *dir = g_dir_open(dirname, 0, NULL);
  if(dir == NULL) // Not a directory or doesn't exist
    return TRUE;
  while(g_dir_read_name(dir) != NULL)
  {
    if(++n > 1) break;
  }
  g_dir_close(dir);
  if(n == 0) // Directory Empty
    return TRUE;
  else
    return FALSE;
}

gchar *dt_util_foo_to_utf8(const char *string)
{
  gchar *tag = NULL;

  if(g_utf8_validate(string, -1, NULL)) // first check if it's utf8 already
    tag = g_strdup(string);
  else
    tag = g_convert(string, -1, "UTF-8", "LATIN1", NULL, NULL, NULL); // let's try latin1

  if(!tag) // hmm, neither utf8 nor latin1, let's fall back to ascii and just remove everything that isn't
  {
    tag = g_strdup(string);
    char *c = tag;
    while(*c)
    {
      if((*c < 0x20) || (*c >= 0x7f)) *c = '?';
      c++;
    }
  }
  return tag;
}

// days are in [1..31], months are in [0..11], see "man localtime"
dt_logo_season_t get_logo_season(void)
{
  time_t now;
  time(&now);
  struct tm lt;
  localtime_r(&now, &lt);
  if((lt.tm_mon == 9 && lt.tm_mday == 31) || (lt.tm_mon == 10 && lt.tm_mday == 1))
    return DT_LOGO_SEASON_HALLOWEEN;
  if(lt.tm_mon == 11 && lt.tm_mday >= 24) return DT_LOGO_SEASON_XMAS;
  return DT_LOGO_SEASON_NONE;
}

// the following two functions (dt_util_latitude_str and dt_util_longitude_str) were taken from libosmgpsmap
// Copyright (C) 2013 John Stowers <john.stowers@gmail.com>
/* these can be overwritten with versions that support
 *   localization */
#define OSD_COORDINATES_CHR_N  "N"
#define OSD_COORDINATES_CHR_S  "S"
#define OSD_COORDINATES_CHR_E  "E"
#define OSD_COORDINATES_CHR_W  "W"

/* this is the classic geocaching notation */
gchar *dt_util_latitude_str(float latitude)
{
  gchar *c = OSD_COORDINATES_CHR_N;
  float integral, fractional;

  if(isnan(latitude)) return NULL;

  if(latitude < 0)
  {
    latitude = fabs(latitude);
    c = OSD_COORDINATES_CHR_S;
  }

  fractional = modff(latitude, &integral);

  return g_strdup_printf("%s %02d° %06.3f'", c, (int)integral, fractional*60.0);
}

gchar *dt_util_longitude_str(float longitude)
{
  gchar *c = OSD_COORDINATES_CHR_E;
  float integral, fractional;

  if(isnan(longitude)) return NULL;

  if(longitude < 0)
  {
    longitude = fabs(longitude);
    c = OSD_COORDINATES_CHR_W;
  }

  fractional = modff(longitude, &integral);

  return g_strdup_printf("%s %03d° %06.3f'", c, (int)integral, fractional*60.0);
}


// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
