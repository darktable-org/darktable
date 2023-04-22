/*
    This file is part of darktable,
    Copyright (C) 2010-2023 darktable developers.

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

#include <math.h>
#include <gtk/gtk.h>
#include <string.h>
#include <librsvg/rsvg.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/** dynamically allocate and concatenate string */
gchar *dt_util_dstrcat(gchar *str, const gchar *format, ...) __attribute__((format(printf, 2, 3)));

/** replace all occurrences of pattern by substitute. the returned value has to be freed after use. */
gchar *dt_util_str_replace(const gchar *string, const gchar *pattern, const gchar *substitute);
/** count the number of occurrences of needle in haystack */
guint dt_util_str_occurence(const gchar *haystack, const gchar *needle);
/** generate a string from the elements of the list, separated by separator. the result has to be freed. */
gchar *dt_util_glist_to_str(const gchar *separator, GList *items);
/** generate a GList from the elements of a string, separated by separator. the result has to be freed. */
GList *dt_util_str_to_glist(const gchar *separator, const gchar *text);
/** take a list of strings and remove all duplicates. the result will be sorted. */
GList *dt_util_glist_uniq(GList *items);
/** fixes the given path by replacing a possible tilde with the correct home directory */
gchar *dt_util_fix_path(const gchar *path);
size_t dt_utf8_strlcpy(char *dest, const char *src, size_t n);
/** returns true if a file is regular, has read access and a filesize > 0 */
gboolean dt_util_test_image_file(const char *filename);
/** returns true if the path represents a directory with write access */
gboolean dt_util_test_writable_dir(const char *path);
/** returns true if dirname is empty */
gboolean dt_util_is_dir_empty(const char *dirname);
/** returns a valid UTF-8 string for the given char array. has to be freed with g_free(). */
gchar *dt_util_foo_to_utf8(const char *string);
/** returns the number of occurence of character in a text. */
guint dt_util_string_count_char(const char *text, const char needle);
/* helper function to convert en float numbers to local based numbers for scanf */
void dt_util_str_to_loc_numbers_format(char *data);
/* helper function to search for a string in a comma seperated string */
gboolean dt_str_commasubstring(const char *list, const char *search);

typedef enum dt_logo_season_t
{
  DT_LOGO_SEASON_NONE = 0,
  DT_LOGO_SEASON_HALLOWEEN = 1,
  DT_LOGO_SEASON_XMAS = 2,
  DT_LOGO_SEASON_EASTER = 3
} dt_logo_season_t;

/** returns the dt logo season to use right now */
dt_logo_season_t dt_util_get_logo_season(void);

cairo_surface_t *dt_util_get_logo(const float size);
cairo_surface_t *dt_util_get_logo_text(const float size);

/** special value to indicate an invalid or unitialized coordinate (replaces */
/** former use of NAN and isnan() by the most negative float) **/
#define DT_INVALID_GPS_COORDINATE (-FLT_MAX)
static inline gboolean dt_valid_gps_coordinate(float value)
{
  return value > DT_INVALID_GPS_COORDINATE;
}
/** we keep the value NAN in the database and .XMP for backward compatibility,
 ** so provide functions to convert back and forth **/
static inline float dt_gps_convert_sql_to_img(float value)
{
  return dt_valid_gps_coordinate(value) ? value : DT_INVALID_GPS_COORDINATE;
}
static inline float dt_gps_convert_img_to_sql(float value)
{
  return dt_valid_gps_coordinate(value) ? value : NAN;
}

gchar *dt_util_latitude_str(float latitude);
gchar *dt_util_longitude_str(float longitude);
gchar *dt_util_elevation_str(float elevation);
double dt_util_gps_string_to_number(const gchar *input);
gboolean dt_util_gps_rationale_to_number(const double r0_1, const double r0_2, const double r1_1,
                                         const double r1_2, const double r2_1, const double r2_2, char sign,
                                         double *result);
gboolean dt_util_gps_elevation_to_number(const double r_1, const double r_2, char sign, double *result);

// make paths absolute and try to normalize on Windows. also deal with character encoding on Windows.
gchar *dt_util_normalize_path(const gchar *input);

#ifdef WIN32
// returns TRUE if the path is a Windows UNC (\\server\share\...\file)
const gboolean dt_util_path_is_UNC(const gchar *filename);
#endif

// gets the directory components of a file name, like g_path_get_dirname(), but works also with Windows networks paths (\\hostname\share\file)
gchar *dt_util_path_get_dirname(const gchar *filename);

// format exposure time string
gchar *dt_util_format_exposure(const float exposuretime);

// read the contents of the given file into a malloc'ed buffer
// returns NULL if unable to read file or alloc memory; sets filesize to the number of bytes returned
char *dt_read_file(const char *filename, size_t *filesize);

// copy the contents of the given file to a new file
void dt_copy_file(const char *src, const char *dst);

// copy the contents of a file in dt's data directory to a new file
void dt_copy_resource_file(const char *src, const char *dst);

// returns the RsvgDimensionData of a supplied RsvgHandle
RsvgDimensionData dt_get_svg_dimension(RsvgHandle *svg);

// renders svg data
void dt_render_svg(RsvgHandle *svg, cairo_t *cr, double width, double height, double offset_x, double offset_y);

// check if the path + basenames are the same (<=> only differ by the extension)
gboolean dt_has_same_path_basename(const char *filename1, const char *filename2);

// set the filename2 extension to filename1 - return NULL if fails - result should be freed
char *dt_copy_filename_extension(const char *filename1, const char *filename2);

// replaces all occurences of a substring in a string
gchar *dt_str_replace(const char *string, const char *search, const char *replace);

// returns true if current settings is scene-referred
gboolean dt_is_scene_referred(void);

// returns true if current settings is display-referred
gboolean dt_is_display_referred(void);

#ifdef __cplusplus
} // extern "C"
#endif /* __cplusplus */

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
