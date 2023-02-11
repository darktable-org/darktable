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

#include <locale.h>

#include "common/darktable.h"
#include "common/file_location.h"
#include "common/grealpath.h"
#include "common/utility.h"
#include "control/conf.h"
#include "gui/gtk.h"

/* getpwnam_r availability check */
#if defined __APPLE__ || defined _POSIX_C_SOURCE >= 1 || defined _XOPEN_SOURCE || defined _BSD_SOURCE        \
    || defined _SVID_SOURCE || defined _POSIX_SOURCE || defined __DragonFly__ || defined __FreeBSD__         \
    || defined __NetBSD__ || defined __OpenBSD__
  #include <pwd.h>
  #include <sys/types.h>
  #include <unistd.h>
#endif

#ifdef _WIN32
  #include <Windows.h>
  #include <WinBase.h>
  #include <FileAPI.h>
#endif

#include <math.h>
#include <glib/gi18n.h>

#include <sys/stat.h>
#include <ctype.h>

#ifdef HAVE_CONFIG_H
  #include <config.h>
#endif

#include <librsvg/rsvg.h>
// ugh, ugly hack. why do people break stuff all the time?
#ifndef RSVG_CAIRO_H
#include <librsvg/rsvg-cairo.h>
#endif

gchar *dt_util_dstrcat(gchar *str, const gchar *format, ...)
{
  va_list args;
  gchar *ns;
  va_start(args, format);
  const size_t clen = str ? strlen(str) : 0;
  const int alen = g_vsnprintf(NULL, 0, format, args);
  const int nsize = alen + clen + 1;

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
  const gint occurrences = dt_util_str_occurence(string, pattern);
  gchar *nstring = NULL;

  if(occurrences)
  {
    nstring = g_malloc_n(strlen(string) + (occurrences * strlen(substitute)) + 1, sizeof(gchar));
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
  gchar **strings = g_malloc0_n(count + 1, sizeof(gchar *));
  if(items != NULL)
  {
    int i = 0;
    for(; items; items = g_list_next(items))
    {
      strings[i++] = items->data;
    }
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
    const size_t len = strlen(path);
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
    g_free(home_path);
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

gboolean dt_util_test_image_file(const char *filename)
{
  if(g_access(filename, R_OK)) return FALSE;
#ifdef _WIN32
  struct _stati64 stats;

  // the code this replaced used utf8 paths with no problem
  // utf8 paths will not work in this context for no reason
  // that I can figure out, but converting utf8 to utf16 works
  // fine.

  wchar_t *wfilename = g_utf8_to_utf16(filename, -1, NULL, NULL, NULL);
  const int result = _wstati64(wfilename, &stats);
  g_free(wfilename);
  if(result) return FALSE; // there was an error
 #else
  struct stat stats;
  if(stat(filename, &stats)) return FALSE;
#endif

  const gboolean regular = (S_ISREG(stats.st_mode)) != 0;
  const gboolean size_ok = stats.st_size > 0;
  return regular && size_ok;
}

gboolean dt_util_test_writable_dir(const char *path)
{
  if(path == NULL) return FALSE;
#ifdef _WIN32
  struct _stati64 stats;

  wchar_t *wpath = g_utf8_to_utf16(path, -1, NULL, NULL, NULL);
  const int result = _wstati64(wpath, &stats);
  g_free(wpath);

  if(result)
  { // error while testing path:
    return FALSE;
  }
#else
  struct stat stats;
  if(stat(path, &stats)) return FALSE;
#endif
  if(S_ISDIR(stats.st_mode) == 0) return FALSE;
  if(g_access(path, W_OK | X_OK) != 0) return FALSE;
  return TRUE;
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

// get easter sunday (in the western world)
static void easter(int Y, int* month, int *day)
{
  const int a  = Y % 19;
  const int b  = Y / 100;
  const int c  = Y % 100;
  const int d  = b / 4;
  const int e  = b % 4;
  const int f  = (b + 8) / 25;
  const int g  = (b - f + 1) / 3;
  const int h  = (19*a + b - d - g + 15) % 30;
  const int i  = c / 4;
  const int k  = c % 4;
  const int L  = (32 + 2*e + 2*i - h - k) % 7;
  const int m  = (a + 11*h + 22*L) / 451;
  *month = (h + L - 7*m + 114) / 31;
  *day   = ((h + L - 7*m + 114) % 31) + 1;
}

// days are in [1..31], months are in [0..11], see "man localtime"
dt_logo_season_t dt_util_get_logo_season(void)
{
  time_t now;
  time(&now);
  struct tm lt;
  localtime_r(&now, &lt);

  // Halloween is active on 31.10. and 01.11.
  if((lt.tm_mon == 9 && lt.tm_mday == 31) || (lt.tm_mon == 10 && lt.tm_mday == 1))
    return DT_LOGO_SEASON_HALLOWEEN;

  // Xmas is active from 24.12. until the end of the year
  if(lt.tm_mon == 11 && lt.tm_mday >= 24) return DT_LOGO_SEASON_XMAS;

  // Easter is active from 2 days before Easter Sunday until 1 day after
  {
    struct tm easter_sunday = lt;
    easter(lt.tm_year+1900, &easter_sunday.tm_mon, &easter_sunday.tm_mday);
    easter_sunday.tm_mon--;
    easter_sunday.tm_hour = easter_sunday.tm_min = easter_sunday.tm_sec = 0;
    easter_sunday.tm_isdst = -1;
    time_t easter_sunday_sec = mktime(&easter_sunday);
    // we start at midnight, so it's basically +- 2 days
    if(llabs(easter_sunday_sec - now) <= 2 * 24 * 60 * 60) return DT_LOGO_SEASON_EASTER;
  }

  return DT_LOGO_SEASON_NONE;
}

static cairo_surface_t *_util_get_svg_img(gchar *logo, const float size)
{
  GError *error = NULL;
  cairo_surface_t *surface = NULL;
  char datadir[PATH_MAX] = { 0 };

  dt_loc_get_datadir(datadir, sizeof(datadir));
  char *dtlogo = g_build_filename(datadir, "pixmaps", logo, NULL);
  RsvgHandle *svg = rsvg_handle_new_from_file(dtlogo, &error);
  if(svg)
  {
    RsvgDimensionData dimension;
    dimension = dt_get_svg_dimension(svg);

    const float ppd = darktable.gui ? darktable.gui->ppd : 1.0;

    const float svg_size = MAX(dimension.width, dimension.height);
    const float factor = size > 0.0 ? size / svg_size : -1.0 * size;
    const float final_width = dimension.width * factor * ppd,
                final_height = dimension.height * factor * ppd;
    const int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, final_width);

    guint8 *image_buffer = (guint8 *)calloc(stride * final_height, sizeof(guint8));
    if(darktable.gui)
      surface = dt_cairo_image_surface_create_for_data(image_buffer, CAIRO_FORMAT_ARGB32, final_width,
                                                      final_height, stride);
    else // during startup we don't know ppd yet and darktable.gui isn't initialized yet.
      surface = cairo_image_surface_create_for_data(image_buffer, CAIRO_FORMAT_ARGB32, final_width,
                                                       final_height, stride);
    if(cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS)
    {
      fprintf(stderr, "warning: can't load darktable logo from SVG file `%s'\n", dtlogo);
      cairo_surface_destroy(surface);
      free(image_buffer);
      image_buffer = NULL;
      surface = NULL;
    }
    else
    {
      cairo_t *cr = cairo_create(surface);
      cairo_scale(cr, factor, factor);
      dt_render_svg(svg, cr, dimension.width, dimension.height, 0, 0);
      cairo_destroy(cr);
      cairo_surface_flush(surface);
    }
    g_object_unref(svg);
  }
  else
  {
    fprintf(stderr, "warning: can't load darktable logo from SVG file `%s'\n%s\n", dtlogo, error->message);
    g_error_free(error);
  }

  g_free(logo);
  g_free(dtlogo);

  return surface;
}

cairo_surface_t *dt_util_get_logo(const float size)
{
  char *logo;
  const dt_logo_season_t season = dt_util_get_logo_season();
  if(season != DT_LOGO_SEASON_NONE)
    logo = g_strdup_printf("idbutton-%d.svg", (int)season);
  else
    logo = g_strdup("idbutton.svg");

  return _util_get_svg_img(logo, size);
}

cairo_surface_t *dt_util_get_logo_text(const float size)
{
  return _util_get_svg_img(g_strdup("dt_text.svg"), size);
}

// the following two functions (dt_util_latitude_str and dt_util_longitude_str) were taken from libosmgpsmap
// Copyright (C) 2013 John Stowers <john.stowers@gmail.com>
/* these can be overwritten with versions that support
 *   localization */
#define OSD_COORDINATES_CHR_N  "N"
#define OSD_COORDINATES_CHR_S  "S"
#define OSD_COORDINATES_CHR_E  "E"
#define OSD_COORDINATES_CHR_W  "W"

static const char *OSD_ELEVATION_ASL = N_("Above sea level");
static const char *OSD_ELEVATION_BSL = N_("Below sea level");

/* this is the classic geocaching notation */
gchar *dt_util_latitude_str(float latitude)
{
  gchar *c = OSD_COORDINATES_CHR_N;
  float integral, fractional;

  if(isnan(latitude)) return NULL;

  if(latitude < 0)
  {
    latitude = fabsf(latitude);
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
    longitude = fabsf(longitude);
    c = OSD_COORDINATES_CHR_W;
  }

  fractional = modff(longitude, &integral);

  return g_strdup_printf("%s %03d° %06.3f'", c, (int)integral, fractional*60.0);
}

gchar *dt_util_elevation_str(float elevation)
{
  const gchar *c = OSD_ELEVATION_ASL;

  if(isnan(elevation)) return NULL;

  if(elevation < 0)
  {
    elevation = fabsf(elevation);
    c = OSD_ELEVATION_BSL;
  }

  return g_strdup_printf("%.2f %s %s", elevation, _("M"), _(c));
}

/* a few helper functions inspired by
 *  https://projects.kde.org/projects/kde/kdegraphics/libs/libkexiv2/repository/revisions/master/entry/libkexiv2/kexiv2gps.cpp
 */

double dt_util_gps_string_to_number(const gchar *input)
{
  double res = NAN;
  gchar dir = toupper(input[strlen(input) - 1]);
  gchar **list = g_strsplit(input, ",", 0);
  if(list)
  {
    if(list[2] == NULL) // format DDD,MM.mm{N|S}
      res = g_ascii_strtoll(list[0], NULL, 10) + (g_ascii_strtod(list[1], NULL) / 60.0);
    else if(list[3] == NULL) // format DDD,MM,SS{N|S}
      res = g_ascii_strtoll(list[0], NULL, 10) + (g_ascii_strtoll(list[1], NULL, 10) / 60.0)
            + (g_ascii_strtoll(list[2], NULL, 10) / 3600.0);
    if(dir == 'S' || dir == 'W') res *= -1.0;
  }
  g_strfreev(list);
  return res;
}

gboolean dt_util_gps_rationale_to_number(const double r0_1, const double r0_2, const double r1_1,
                                         const double r1_2, const double r2_1, const double r2_2, char sign,
                                         double *result)
{
  if(!result) return FALSE;
  double res = 0.0;
  // Latitude decoding from Exif.
  double num, den, min, sec;
  num = r0_1;
  den = r0_2;
  if(den == 0) return FALSE;
  res = num / den;

  num = r1_1;
  den = r1_2;
  if(den == 0) return FALSE;
  min = num / den;
  if(min != -1.0) res += min / 60.0;

  num = r2_1;
  den = r2_2;
  if(den == 0)
  {
    // be relaxed and accept 0/0 seconds. See #246077.
    if(num == 0)
      den = 1;
    else
      return FALSE;
  }
  sec = num / den;
  if(sec != -1.0) res += sec / 3600.0;

  if(sign == 'S' || sign == 'W') res *= -1.0;

  *result = res;
  return TRUE;
}

gboolean dt_util_gps_elevation_to_number(const double r_1, const double r_2, char sign, double *result)
{
  if(!result) return FALSE;
  double res = 0.0;
  // Altitude decoding from Exif.
  const double num = r_1;
  const double den = r_2;
  if(den == 0) return FALSE;
  res = num / den;

  if(sign != '0') res *= -1.0;

  *result = res;
  return TRUE;
}


// make paths absolute and try to normalize on Windows. also deal with character encoding on Windows.
gchar *dt_util_normalize_path(const gchar *_input)
{
#ifdef _WIN32
  gchar *input;
  if(g_utf8_validate(_input, -1, NULL))
    input = g_strdup(_input);
  else
  {
    input = g_locale_to_utf8(_input, -1, NULL, NULL, NULL);
    if(!input) return NULL;
  }
#else
  const gchar *input = _input;
#endif

  gchar *filename = g_filename_from_uri(input, NULL, NULL);

  if(!filename)
  {
    if(g_str_has_prefix(input, "file://")) // in this case we should take care of %XX encodings in the string
                                           // (for example %20 = ' ')
    {
      input += strlen("file://");
      filename = g_uri_unescape_string(input, NULL);
    }
    else
      filename = g_strdup(input);
  }

#ifdef _WIN32
  g_free(input);
#endif

  if(g_path_is_absolute(filename) == FALSE)
  {
    char *current_dir = g_get_current_dir();
    char *tmp_filename = g_build_filename(current_dir, filename, NULL);
    g_free(filename);
    filename = g_realpath(tmp_filename);
    if(filename == NULL)
    {
      g_free(current_dir);
      g_free(tmp_filename);
      g_free(filename);
      return NULL;
    }
    g_free(current_dir);
    g_free(tmp_filename);
  }

#ifdef _WIN32
  // on Windows filenames are case insensitive, so we can end up with an arbitrary number of different spellings for the same file.
  // another problem is that path separators can either be / or \ leading to even more problems.

  // TODO:
  // this handles filenames in the formats <drive letter>:\path\to\file or \\host-name\share-name\file
  // some other formats like \Device\... are not supported

  // the Windows api expects wide chars and not utf8 :(
  wchar_t *wfilename = g_utf8_to_utf16(filename, -1, NULL, NULL, NULL);
  g_free(filename);
  if(!wfilename)
    return NULL;

  wchar_t LongPath[MAX_PATH] = {0};
  const DWORD size = GetLongPathNameW(wfilename, LongPath, MAX_PATH);
  g_free(wfilename);
  if(size == 0 || size > MAX_PATH)
    return NULL;

  // back to utf8!
  filename = g_utf16_to_utf8(LongPath, -1, NULL, NULL, NULL);
  if(!filename)
    return NULL;

  GFile *gfile = g_file_new_for_path(filename);
  g_free(filename);
  if(!gfile)
    return NULL;
  filename = g_file_get_path(gfile);
  g_object_unref(gfile);
  if(!filename)
    return NULL;

  const char first = g_ascii_toupper(filename[0]);
  if(first >= 'A' && first <= 'Z' && filename[1] == ':') // path format is <drive letter>:\path\to\file
  {
    filename[0] = first;
    return filename;
  }
  else if(first == '\\' && filename[1] == '\\') // path format is \\host-name\share-name\file
    return filename;
  else
  {
    g_free(filename);
    return NULL;
  }
#endif

  return filename;
}

#ifdef WIN32
// returns TRUE if the path is a Windows UNC (\\server\share\...\file)
const gboolean dt_util_path_is_UNC(const gchar *filename)
{
  return filename[0] == G_DIR_SEPARATOR && filename[1] == G_DIR_SEPARATOR;
}
#endif

// gets the directory components of a file name, like g_path_get_dirname(), but works also with Windows networks paths (\\hostname\share\file)
gchar *dt_util_path_get_dirname(const gchar *filename)
{
  gchar *dirname = g_path_get_dirname(filename);

  /* Remove trailing slash, as g_path_get_dirname() leaves it for Windows UNC and this messes up film roll name */
  if(dirname[0])
  {
    int last = strlen(dirname) - 1;
    if(G_IS_DIR_SEPARATOR(dirname[last]))
      dirname[last] = '\0';
  }
  return dirname;
}

guint dt_util_string_count_char(const char *text, const char needle)
{
  guint count = 0;
  while(text[0])
  {
    if(text[0] == needle) count ++;
    text ++;
  }
  return count;
}

void dt_util_str_to_loc_numbers_format(char *data)
{
  const struct lconv *currentLocalConv = localeconv();
  const gchar loc_decimal_point = currentLocalConv->decimal_point[0];
  const gchar *en_decimal_point = ".";
  g_strdelimit(data, en_decimal_point, loc_decimal_point);
}

GList *dt_util_str_to_glist(const gchar *separator, const gchar *text)
{
  if(text == NULL) return NULL;
  GList *list = NULL;
  gchar *item = NULL;
  gchar *entry = g_strdup(text);
  gchar *prev = entry;
  int len = strlen(prev);
  while(len)
  {
    gchar *next = g_strstr_len(prev, -1, separator);
    if(next)
    {
      const gchar c = next[0];
      next[0] = '\0';
      item = g_strdup(prev);
      next[0] = c;
      prev = next + strlen(separator);
      len = strlen(prev);
      list = g_list_prepend(list, item);
      if(!len) list = g_list_prepend(list, g_strdup(""));
    }
    else
    {
      item = g_strdup(prev);
      len = 0;
      list = g_list_prepend(list, item);
    }
  }
  list = g_list_reverse(list);
  g_free(entry);
  return list;
}

// format exposure time given in seconds to a string in a unified way
char *dt_util_format_exposure(const float exposuretime)
{
  char *result = NULL;
  if(exposuretime >= 1.0f)
  {
    if(nearbyintf(exposuretime) == exposuretime)
      result = g_strdup_printf("%.0f″", exposuretime);
    else
      result = g_strdup_printf("%.1f″", exposuretime);
  }
  /* want to catch everything below 0.3 seconds */
  else if(exposuretime < 0.29f)
    result = g_strdup_printf("1/%.0f", 1.0 / exposuretime);

  /* catch 1/2, 1/3 */
  else if(nearbyintf(1.0f / exposuretime) == 1.0f / exposuretime)
    result = g_strdup_printf("1/%.0f", 1.0 / exposuretime);

  /* catch 1/1.3, 1/1.6, etc. */
  else if(10 * nearbyintf(10.0f / exposuretime) == nearbyintf(100.0f / exposuretime))
    result = g_strdup_printf("1/%.1f", 1.0 / exposuretime);

  else
    result = g_strdup_printf("%.1f″", exposuretime);

  return result;
}

char *dt_read_file(const char *const filename, size_t *filesize)
{
  if(filesize) *filesize = 0;
  FILE *fd = g_fopen(filename, "rb");
  if(!fd) return NULL;

  fseek(fd, 0, SEEK_END);
  const size_t end = ftell(fd);
  rewind(fd);

  char *content = (char *)malloc(sizeof(char) * end);
  if(!content) return NULL;

  const size_t count = fread(content, sizeof(char), end, fd);
  fclose(fd);
  if(count == end)
  {
    if(filesize) *filesize = end;
    return content;
  }
  free(content);
  return NULL;
}

void dt_copy_file(const char *const sourcefile, const char *dst)
{
  char *content = NULL;
  FILE *fin = g_fopen(sourcefile, "rb");
  FILE *fout = g_fopen(dst, "wb");

  if(fin && fout)
  {
    fseek(fin, 0, SEEK_END);
    const size_t end = ftell(fin);
    rewind(fin);
    content = (char *)g_malloc_n(end, sizeof(char));
    if(content == NULL) goto END;
    if(fread(content, sizeof(char), end, fin) != end) goto END;
    if(fwrite(content, sizeof(char), end, fout) != end) goto END;
  }

END:
  if(fout != NULL) fclose(fout);
  if(fin != NULL) fclose(fin);

  g_free(content);
}

void dt_copy_resource_file(const char *src, const char *dst)
{
  char share[PATH_MAX] = { 0 };
  dt_loc_get_datadir(share, sizeof(share));
  gchar *sourcefile = g_build_filename(share, src, NULL);
  dt_copy_file(sourcefile, dst);
  g_free(sourcefile);
}

RsvgDimensionData dt_get_svg_dimension(RsvgHandle *svg)
{
  RsvgDimensionData dimension;
  // rsvg_handle_get_dimensions has been deprecated in librsvg 2.52
  #if LIBRSVG_CHECK_VERSION(2,52,0)
    double width;
    double height;
    if(rsvg_handle_get_intrinsic_size_in_pixels(svg, &width, &height)) //only works if SVG document has size specified
    {
      dimension.width = lround(width);
      dimension.height = lround(height);
    }
    else
    {
#define VIEWPORT_SIZE 32767 //use maximum cairo surface size to have enough precision when size is converted to int
      const RsvgRectangle viewport = {
        .x = 0,
        .y = 0,
        .width = VIEWPORT_SIZE,
        .height = VIEWPORT_SIZE,
      };
#undef VIEWPORT_SIZE
      RsvgRectangle rectangle;
      rsvg_handle_get_geometry_for_layer(svg, NULL, &viewport, NULL, &rectangle, NULL);
      dimension.width = lround(rectangle.width);
      dimension.height = lround(rectangle.height);
    }
  #else
    rsvg_handle_get_dimensions(svg, &dimension);
  #endif
  return dimension;
}

void dt_render_svg(RsvgHandle *svg, cairo_t *cr, double width, double height, double offset_x, double offset_y)
{
  // rsvg_handle_render_cairo has been deprecated in librsvg 2.52
  #if LIBRSVG_CHECK_VERSION(2,52,0)
    RsvgRectangle viewport = {
      .x = offset_x,
      .y = offset_y,
      .width = width,
      .height = height,
    };
    rsvg_handle_render_document(svg, cr, &viewport, NULL);
  #else
    rsvg_handle_render_cairo(svg, cr);
  #endif
}

// check if the path + basenames are the same (<=> only differ by the extension)
gboolean dt_has_same_path_basename(const char *filename1, const char *filename2)
{
  // assume both filenames have an extension
  if(!filename1 || !filename2) return FALSE;
  const char *dot1 = strrchr(filename1, '.');
  if(!dot1) return FALSE;
  const char *dot2 = strrchr(filename2, '.');
  if(!dot2) return FALSE;
  const int length1 = dot1 - filename1;
  const int length2 = dot2 - filename2;
  if(length1 != length2)
    return FALSE;
  for(int i = length1 - 1; i > 0; i--)
    if(filename1[i] != filename2[i])
      return FALSE;
  return TRUE;
}

// set the filename2 extension to filename1 - return NULL if fails - result should be freed
char *dt_copy_filename_extension(const char *filename1, const char *filename2)
{
  // assume both filenames have an extension
  if(!filename1 || !filename2) return NULL;
  const char *dot1 = strrchr(filename1, '.');
  if(!dot1) return NULL;
  const char *dot2 = strrchr(filename2, '.');
  if(!dot2) return NULL;
  const int name_lgth = dot1 - filename1;
  const int ext_lgth = strlen(dot2);
  char *output = g_malloc(name_lgth + ext_lgth + 1);
  if(output)
  {
    memcpy(output, filename1, name_lgth);
    memcpy(&output[name_lgth], &filename2[strlen(filename2) - ext_lgth], ext_lgth + 1);
  }
  return output;
}

// replaces all occurences of a substring in a string
gchar *dt_str_replace(const char *string, const char *search, const char *replace)
{
  gchar **split = g_strsplit(string, search, -1);
  gchar *res = g_strjoinv(replace, split);
  g_strfreev(split);
  return res;
}

gboolean dt_is_scene_referred(void)
{
  return dt_conf_is_equal("plugins/darkroom/workflow", "scene-referred (filmic)")
    || dt_conf_is_equal("plugins/darkroom/workflow", "scene-referred (sigmoid)");
}

gboolean dt_is_display_referred(void)
{
  return dt_conf_is_equal("plugins/darkroom/workflow", "display-referred (legacy)");
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
