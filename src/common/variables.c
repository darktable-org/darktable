/*
    This file is part of darktable,
    Copyright (C) 2010-2020 darktable developers.

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
#include "common/debug.h"
#include "common/variables.h"
#include "common/colorlabels.h"
#include "common/darktable.h"
#include "common/file_location.h"
#include "common/image.h"
#include "common/image_cache.h"
#include "common/metadata.h"
#include "common/opencl.h"
#include "common/utility.h"
#include "common/tags.h"
#include "control/conf.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

typedef struct dt_variables_data_t
{
  /** cached values that shouldn't change between variables in the same expansion process */
  struct tm time;
  time_t exif_time;
  guint sequence;

  /* export settings for image maximum width and height taken from GUI */
  int max_width;
  int max_height;

  char *homedir;
  char *pictures_folder;
  const char *file_ext;

  gboolean have_exif_tm;
  int exif_iso;
  char *camera_maker;
  char *camera_alias;
  char *exif_lens;
  int version;
  int stars;
  struct tm exif_tm;

  float exif_exposure;
  float exif_exposure_bias;
  float exif_aperture;
  float exif_focal_length;
  float exif_focus_distance;
  double longitude;
  double latitude;
  double elevation;

  uint32_t tags_flags;

  int flags;

} dt_variables_data_t;

static char *expand(dt_variables_params_t *params, char **source, char extra_stop);

// gather some data that might be used for variable expansion
static void init_expansion(dt_variables_params_t *params, gboolean iterate)
{
  if(iterate) params->data->sequence++;

  params->data->homedir = dt_loc_get_home_dir(NULL);

  if(g_get_user_special_dir(G_USER_DIRECTORY_PICTURES) == NULL)
    params->data->pictures_folder = g_build_path(G_DIR_SEPARATOR_S, params->data->homedir, "Pictures", (char *)NULL);
  else
    params->data->pictures_folder = g_strdup(g_get_user_special_dir(G_USER_DIRECTORY_PICTURES));

  if(params->filename)
  {
    params->data->file_ext = (g_strrstr(params->filename, ".") + 1);
    if(params->data->file_ext == (gchar *)1) params->data->file_ext = params->filename + strlen(params->filename);
  }
  else
    params->data->file_ext = NULL;

  /* image exif time */
  params->data->have_exif_tm = FALSE;
  params->data->exif_iso = 100;
  params->data->camera_maker = NULL;
  params->data->camera_alias = NULL;
  params->data->exif_lens = NULL;
  params->data->version = 0;
  params->data->stars = 0;
  params->data->exif_exposure = 0.0f;
  params->data->exif_exposure_bias = NAN;
  params->data->exif_aperture = 0.0f;
  params->data->exif_focal_length = 0.0f;
  params->data->exif_focus_distance = 0.0f;
  params->data->longitude = 0.0f;
  params->data->latitude = 0.0f;
  params->data->elevation = 0.0f;
  if(params->imgid)
  {
    const dt_image_t *img = dt_image_cache_get(darktable.image_cache, params->imgid, 'r');
    if(sscanf(img->exif_datetime_taken, "%d:%d:%d %d:%d:%d", &params->data->exif_tm.tm_year, &params->data->exif_tm.tm_mon,
      &params->data->exif_tm.tm_mday, &params->data->exif_tm.tm_hour, &params->data->exif_tm.tm_min, &params->data->exif_tm.tm_sec) == 6)
    {
      params->data->exif_tm.tm_year -= 1900;
      params->data->exif_tm.tm_mon--;
      params->data->have_exif_tm = TRUE;
    }
    params->data->exif_iso = img->exif_iso;
    params->data->camera_maker = g_strdup(img->camera_maker);
    params->data->camera_alias = g_strdup(img->camera_alias);
    params->data->exif_lens = g_strdup(img->exif_lens);
    params->data->version = img->version;
    params->data->stars = (img->flags & 0x7);
    if(params->data->stars == 6) params->data->stars = -1;

    params->data->exif_exposure = img->exif_exposure;
    params->data->exif_exposure_bias = img->exif_exposure_bias;
    params->data->exif_aperture = img->exif_aperture;
    params->data->exif_focal_length = img->exif_focal_length;
    if(!isnan(img->exif_focus_distance) && fpclassify(img->exif_focus_distance) != FP_ZERO)
      params->data->exif_focus_distance = img->exif_focus_distance;
    if(!isnan(img->geoloc.longitude)) params->data->longitude = img->geoloc.longitude;
    if(!isnan(img->geoloc.latitude)) params->data->latitude = img->geoloc.latitude;
    if(!isnan(img->geoloc.elevation)) params->data->elevation = img->geoloc.elevation;

    params->data->flags = img->flags;

    dt_image_cache_read_release(darktable.image_cache, img);
  }
  else if (params->data->exif_time) {
    localtime_r(&params->data->exif_time, &params->data->exif_tm);
    params->data->have_exif_tm = TRUE;
  }
}

static void cleanup_expansion(dt_variables_params_t *params)
{
  g_free(params->data->homedir);
  g_free(params->data->pictures_folder);
  g_free(params->data->camera_maker);
  g_free(params->data->camera_alias);
}

static inline gboolean has_prefix(char **str, const char *prefix)
{
  gboolean res = g_str_has_prefix(*str, prefix);
  if(res) *str += strlen(prefix);
  return res;
}

static char *get_base_value(dt_variables_params_t *params, char **variable)
{
  char *result = NULL;
  gboolean escape = TRUE;

  struct tm exif_tm = params->data->have_exif_tm ? params->data->exif_tm : params->data->time;

  if(has_prefix(variable, "YEAR"))
    result = g_strdup_printf("%.4d", params->data->time.tm_year + 1900);
  else if(has_prefix(variable, "MONTH"))
    result = g_strdup_printf("%.2d", params->data->time.tm_mon + 1);
  else if(has_prefix(variable, "DAY"))
    result = g_strdup_printf("%.2d", params->data->time.tm_mday);
  else if(has_prefix(variable, "HOUR"))
    result = g_strdup_printf("%.2d", params->data->time.tm_hour);
  else if(has_prefix(variable, "MINUTE"))
    result = g_strdup_printf("%.2d", params->data->time.tm_min);
  else if(has_prefix(variable, "SECOND"))
    result = g_strdup_printf("%.2d", params->data->time.tm_sec);

  else if(has_prefix(variable, "EXIF_YEAR"))
    result = g_strdup_printf("%.4d", exif_tm.tm_year + 1900);
  else if(has_prefix(variable, "EXIF_MONTH"))
    result = g_strdup_printf("%.2d", exif_tm.tm_mon + 1);
  else if(has_prefix(variable, "EXIF_DAY"))
    result = g_strdup_printf("%.2d", exif_tm.tm_mday);
  else if(has_prefix(variable, "EXIF_HOUR"))
    result = g_strdup_printf("%.2d", exif_tm.tm_hour);
  else if(has_prefix(variable, "EXIF_MINUTE"))
    result = g_strdup_printf("%.2d", exif_tm.tm_min);
  else if(has_prefix(variable, "EXIF_SECOND"))
    result = g_strdup_printf("%.2d", exif_tm.tm_sec);
  else if(has_prefix(variable, "EXIF_ISO"))
    result = g_strdup_printf("%d", params->data->exif_iso);
  else if(has_prefix(variable, "NL") && g_strcmp0(params->jobcode, "infos") == 0)
    result = g_strdup_printf("\n");
  else if(has_prefix(variable, "EXIF_EXPOSURE_BIAS"))
  {
    if(!isnan(params->data->exif_exposure_bias))
      result = g_strdup_printf("%+.2f", params->data->exif_exposure_bias);
  }
  else if(has_prefix(variable, "EXIF_EXPOSURE"))
  {
    result = dt_util_format_exposure(params->data->exif_exposure);
    // for job other than info (export) we strip the slash char
    if(g_strcmp0(params->jobcode, "infos") != 0)
    {
      gchar *res = dt_util_str_replace(result, "/", "_");
      g_free(result);
      result = res;
    }
  }
  else if(has_prefix(variable, "EXIF_APERTURE"))
    result = g_strdup_printf("%.1f", params->data->exif_aperture);
  else if(has_prefix(variable, "EXIF_FOCAL_LENGTH"))
    result = g_strdup_printf("%d", (int)params->data->exif_focal_length);
  else if(has_prefix(variable, "EXIF_FOCUS_DISTANCE"))
    result = g_strdup_printf("%.2f", params->data->exif_focus_distance);
  else if(has_prefix(variable, "LONGITUDE"))
  {
    if(dt_conf_get_bool("plugins/lighttable/metadata_view/pretty_location")
       && g_strcmp0(params->jobcode, "infos") == 0)
    {
      result = dt_util_latitude_str(params->data->longitude);
    }
    else
    {
      gchar NS = params->data->longitude < 0 ? 'W' : 'E';
      result = g_strdup_printf("%c%010.6f", NS, fabs(params->data->longitude));
    }
  }
  else if(has_prefix(variable, "LATITUDE"))
  {
    if(dt_conf_get_bool("plugins/lighttable/metadata_view/pretty_location")
       && g_strcmp0(params->jobcode, "infos") == 0)
    {
      result = dt_util_latitude_str(params->data->latitude);
    }
    else
    {
      gchar NS = params->data->latitude < 0 ? 'S' : 'N';
      result = g_strdup_printf("%c%09.6f", NS, fabs(params->data->latitude));
    }
  }
  else if(has_prefix(variable, "ELEVATION"))
    result = g_strdup_printf("%.2f", params->data->elevation);
  else if(has_prefix(variable, "MAKER"))
    result = g_strdup(params->data->camera_maker);
  else if(has_prefix(variable, "MODEL"))
    result = g_strdup(params->data->camera_alias);
  else if(has_prefix(variable, "LENS"))
    result = g_strdup(params->data->exif_lens);
  else if(has_prefix(variable, "ID"))
    result = g_strdup_printf("%d", params->imgid);
  else if(has_prefix(variable, "VERSION_NAME"))
  {
    GList *res = dt_metadata_get(params->imgid, "Xmp.darktable.version_name", NULL);
    res = g_list_first(res);
    if(res != NULL)
    {
      result = g_strdup((char *)res->data);
    }
    g_list_free_full(res, &g_free);
  }
  else if(has_prefix(variable, "VERSION_IF_MULTI"))
  {
    sqlite3_stmt *stmt;

    // count duplicates
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "SELECT COUNT(1)"
                                " FROM images AS i1"
                                " WHERE EXISTS (SELECT 'y' FROM images AS i2"
                                "               WHERE  i2.id = ?1"
                                "               AND    i1.film_id = i2.film_id"
                                "               AND    i1.filename = i2.filename)",
                                -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, params->imgid);

    if(sqlite3_step(stmt) == SQLITE_ROW)
    {
      const int count = sqlite3_column_int(stmt, 0);
      //only return data if more than one matching image
      if(count > 1)
        result = g_strdup_printf("%d", params->data->version);
    }
    sqlite3_finalize (stmt);
  }
  else if(has_prefix(variable, "VERSION"))
    result = g_strdup_printf("%d", params->data->version);
  else if(has_prefix(variable, "JOBCODE"))
    result = g_strdup(params->jobcode);
  else if(has_prefix(variable, "ROLL_NAME"))
  {
    if(params->filename)
    {
      gchar *dirname = g_path_get_dirname(params->filename);
      result = g_path_get_basename(dirname);
      g_free(dirname);
    }
  }
  else if(has_prefix(variable, "FILE_DIRECTORY"))
  {
    // undocumented : backward compatibility
    if(params->filename)
      result = g_path_get_dirname(params->filename);
  }
  else if(has_prefix(variable, "FILE_FOLDER"))
  {
    if(params->filename)
      result = g_path_get_dirname(params->filename);
  }
  else if(has_prefix(variable, "FILE_NAME"))
  {
    if(params->filename)
    {
      result = g_path_get_basename(params->filename);
      char *dot = g_strrstr(result, ".");
      if(dot) *dot = '\0';
    }
  }
  else if(has_prefix(variable, "FILE_EXTENSION"))
    result = g_strdup(params->data->file_ext);
  else if(has_prefix(variable, "SEQUENCE"))
  {
    uint8_t nb_digit = 4;
    if(g_ascii_isdigit(*variable[0]))
    {
      nb_digit = (uint8_t)*variable[0] & 0b1111;
      (*variable) ++;
    }
    result = g_strdup_printf("%.*d", nb_digit, params->sequence >= 0 ? params->sequence : params->data->sequence);
  }
  else if(has_prefix(variable, "USERNAME"))
    result = g_strdup(g_get_user_name());
  else if(has_prefix(variable, "HOME_FOLDER"))
    result = g_strdup(params->data->homedir); // undocumented : backward compatibility
  else if(has_prefix(variable, "HOME"))
    result = g_strdup(params->data->homedir);
  else if(has_prefix(variable, "PICTURES_FOLDER"))
    result = g_strdup(params->data->pictures_folder);
  else if(has_prefix(variable, "DESKTOP_FOLDER"))
    result = g_strdup(g_get_user_special_dir(G_USER_DIRECTORY_DESKTOP)); // undocumented : backward compatibility
  else if(has_prefix(variable, "DESKTOP"))
    result = g_strdup(g_get_user_special_dir(G_USER_DIRECTORY_DESKTOP));
  else if(has_prefix(variable, "STARS"))
    result = g_strdup_printf("%d", params->data->stars);
  else if(has_prefix(variable, "RATING_ICONS"))
  {
    switch(params->data->stars)
    {
      case -1:
        result = g_strdup("X");
        break;
      case 1:
        result = g_strdup("★");
        break;
      case 2:
        result = g_strdup("★★");
        break;
      case 3:
        result = g_strdup("★★★");
        break;
      case 4:
        result = g_strdup("★★★★");
        break;
      case 5:
        result = g_strdup("★★★★★");
        break;
      default:
        result = g_strdup("");
        break;
    }
  }
  else if(has_prefix(variable, "LABELS_ICONS") && g_strcmp0(params->jobcode, "infos") == 0)
  {
    escape = FALSE;
    GList *res = dt_metadata_get(params->imgid, "Xmp.darktable.colorlabels", NULL);
    res = g_list_first(res);
    if(res != NULL)
    {
      do
      {
        const char *lb = (char *)(dt_colorlabels_to_string(GPOINTER_TO_INT(res->data)));
        if(g_strcmp0(lb, "red") == 0)
        {
          result = dt_util_dstrcat(result, "<span foreground=\"#ee0000\">⚫ </span>");
        }
        else if(g_strcmp0(lb, "yellow") == 0)
        {
          result = dt_util_dstrcat(result, "<span foreground=\"#eeee00\">⚫ </span>");
        }
        else if(g_strcmp0(lb, "green") == 0)
        {
          result = dt_util_dstrcat(result, "<span foreground=\"#00ee00\">⚫ </span>");
        }
        else if(g_strcmp0(lb, "blue") == 0)
        {
          result = dt_util_dstrcat(result, "<span foreground=\"#0000ee\">⚫ </span>");
        }
        else if(g_strcmp0(lb, "purple") == 0)
        {
          result = dt_util_dstrcat(result, "<span foreground=\"#ee00ee\">⚫ </span>");
        }
      } while((res = g_list_next(res)) != NULL);
    }
    g_list_free(res);
  }
  else if(has_prefix(variable, "LABELS_COLORICONS") && g_strcmp0(params->jobcode, "infos") == 0)
  {
    escape = FALSE;
    GList *res = dt_metadata_get(params->imgid, "Xmp.darktable.colorlabels", NULL);
    res = g_list_first(res);
    if(res != NULL)
    {
      do
      {
        const char *lb = (char *)(dt_colorlabels_to_string(GPOINTER_TO_INT(res->data)));
        if(g_strcmp0(lb, "red") == 0)
        {
          result = dt_util_dstrcat(result, "<span foreground=\"#ee0000\">🔴 </span>");
        }
        else if(g_strcmp0(lb, "yellow") == 0)
        {
          result = dt_util_dstrcat(result, "<span foreground=\"#eeee00\">🟡 </span>");
        }
        else if(g_strcmp0(lb, "green") == 0)
        {
          result = dt_util_dstrcat(result, "<span foreground=\"#00ee00\">🟢 </span>");
        }
        else if(g_strcmp0(lb, "blue") == 0)
        {
          result = dt_util_dstrcat(result, "<span foreground=\"#0000ee\">🔵 </span>");
        }
        else if(g_strcmp0(lb, "purple") == 0)
        {
          result = dt_util_dstrcat(result, "<span foreground=\"#ee00ee\">🟣 </span>");
        }
      } while((res = g_list_next(res)) != NULL);
    }
    g_list_free(res);
  }
  else if(has_prefix(variable, "LABELS") || has_prefix(variable, "LABELS_ICONS") || has_prefix(variable, "LABELS_COLORICONS"))
  {
    // TODO: currently we concatenate all the color labels with a ',' as a separator. Maybe it's better to
    // only use the first/last label?
    GList *res = dt_metadata_get(params->imgid, "Xmp.darktable.colorlabels", NULL);
    res = g_list_first(res);
    if(res != NULL)
    {
      GList *labels = NULL;
      do
      {
        labels = g_list_append(labels, (char *)(_(dt_colorlabels_to_string(GPOINTER_TO_INT(res->data)))));
      } while((res = g_list_next(res)) != NULL);
      result = dt_util_glist_to_str(",", labels);
      g_list_free(labels);
    }
    g_list_free(res);
  }
  else if(has_prefix(variable, "TITLE"))
  {
    GList *res = dt_metadata_get(params->imgid, "Xmp.dc.title", NULL);
    res = g_list_first(res);
    if(res != NULL)
    {
      result = g_strdup((char *)res->data);
    }
    g_list_free_full(res, &g_free);
  }
  else if(has_prefix(variable, "DESCRIPTION"))
  {
    GList *res = dt_metadata_get(params->imgid, "Xmp.dc.description", NULL);
    res = g_list_first(res);
    if(res != NULL)
    {
      result = g_strdup((char *)res->data);
    }
    g_list_free_full(res, &g_free);
  }
  else if(has_prefix(variable, "CREATOR"))
  {
    GList *res = dt_metadata_get(params->imgid, "Xmp.dc.creator", NULL);
    res = g_list_first(res);
    if(res != NULL)
    {
      result = g_strdup((char *)res->data);
    }
    g_list_free_full(res, &g_free);
  }
  else if(has_prefix(variable, "PUBLISHER"))
  {
    GList *res = dt_metadata_get(params->imgid, "Xmp.dc.publisher", NULL);
    res = g_list_first(res);
    if(res != NULL)
    {
      result = g_strdup((char *)res->data);
    }
    g_list_free_full(res, &g_free);
  }
  else if(has_prefix(variable, "RIGHTS"))
  {
    GList *res = dt_metadata_get(params->imgid, "Xmp.dc.rights", NULL);
    res = g_list_first(res);
    if(res != NULL)
    {
      result = g_strdup((char *)res->data);
    }
    g_list_free_full(res, &g_free);
  }
  else if(has_prefix(variable, "OPENCL_ACTIVATED"))
  {
    if(dt_opencl_is_enabled())
      result = g_strdup(_("yes"));
    else
      result = g_strdup(_("no"));
  }
  else if(has_prefix(variable, "MAX_WIDTH"))
    result = g_strdup_printf("%d", params->data->max_width);
  else if(has_prefix(variable, "MAX_HEIGHT"))
    result = g_strdup_printf("%d", params->data->max_height);
  else if (has_prefix(variable, "CATEGORY"))
  {
    // CATEGORY should be followed by n [0,9] and "(category)". category can contain 0 or more '|'
    if (g_ascii_isdigit(*variable[0]))
    {
      const uint8_t level = (uint8_t)*variable[0] & 0b1111;
      (*variable) ++;
      if (*variable[0] == '(')
      {
        char *category = g_strdup(*variable + 1);
        char *end = g_strstr_len(category, -1, ")");
        if (end)
        {
          end[0] = '|';
          end[1] = '\0';
          (*variable) += strlen(category) + 1;
          char *tag = dt_tag_get_subtags(params->imgid, category, (int)level);
          if (tag)
          {
            result = g_strdup(tag);
            g_free(tag);
          }
        }
        g_free(category);
      }
    }
  }
  else if (has_prefix(variable, "TAGS"))
  {
    GList *tags_list = dt_tag_get_list_export(params->imgid, params->data->tags_flags);
    char *tags = dt_util_glist_to_str(",", tags_list);
    g_list_free_full(tags_list, g_free);
    result = g_strdup(tags);
    g_free(tags);
  }
  else if(has_prefix(variable, "SIDECAR_TXT") && g_strcmp0(params->jobcode, "infos") == 0
          && (params->data->flags & DT_IMAGE_HAS_TXT))
  {
    char *path = dt_image_get_text_path(params->imgid);
    if(path)
    {
      gchar *txt = NULL;
      if(g_file_get_contents(path, &txt, NULL, NULL))
      {
        result = g_strdup_printf("\n%s", txt);
      }
      g_free(txt);
      g_free(path);
    }
  }
  else
  {
    // go past what looks like an invalid variable. we only expect to see [a-zA-Z]* in a variable name.
    while(g_ascii_isalpha(**variable)) (*variable)++;
  }
  if(!result) result = g_strdup("");

  if(params->escape_markup && escape)
  {
    gchar *e_res = g_markup_escape_text(result, -1);
    g_free(result);
    return e_res;
  }
  return result;
}

// bash style variable manipulation. all patterns are just simple string comparisons!
// See here for bash examples and documentation:
// http://www.tldp.org/LDP/abs/html/parameter-substitution.html
// https://www.gnu.org/software/bash/manual/html_node/Shell-Parameter-Expansion.html
// the descriptions in the comments are referring to the bash behaviour, dt doesn't do it 100% like that!
static char *variable_get_value(dt_variables_params_t *params, char **variable)
{
  // invariant: the variable starts with "$(" which we can skip
  (*variable) += 2;

  // first get the value of the variable
  char *base_value = get_base_value(params, variable); // this is never going to be NULL!
  const size_t base_value_length = strlen(base_value);

  // ... and now see if we have to change it
  const char operation = **variable;
  if(operation != '\0' && operation != ')') (*variable)++;
  switch(operation)
  {
    case '-':
      /*
        $(parameter-default)
          If parameter not set, use default.
      */
      {
        char *replacement = expand(params, variable, ')');
        if(*base_value == '\0')
        {
          g_free(base_value);
          base_value = replacement;
        }
        else
          g_free(replacement);
      }
      break;
    case '+':
      /*
        $(parameter+alt_value)
          If parameter set, use alt_value, else use null string.
      */
      {
        char *replacement = expand(params, variable, ')');
        if(*base_value != '\0')
        {
          g_free(base_value);
          base_value = replacement;
        }
        else
          g_free(replacement);
      }
      break;
    case ':':
      /*
        $(var:offset)
          Variable var expanded, starting from offset.

        $(var:offset:length)
          Expansion to a max of length characters of variable var, from offset.

        If offset evaluates to a number less than zero, the value is used as an offset in characters from the
        end of the value of parameter. If length evaluates to a number less than zero, it is interpreted as an
        offset in characters from the end of the value of parameter rather than a number of characters, and the
        expansion is the characters between offset and that result.
      */
      {
        const glong base_value_utf8_length = g_utf8_strlen(base_value, -1);
        const glong offset = strtol(*variable, variable, 10);

        // find where to start
        char *start; // from where to copy ...
        if(offset >= 0)
          start = g_utf8_offset_to_pointer(base_value, MIN(offset, base_value_utf8_length));
        else
          start = g_utf8_offset_to_pointer(base_value + base_value_length, MAX(offset, -base_value_utf8_length));

        // now find the end if there is a length provided
        char *end = base_value + base_value_length; // ... and until where
        if(start && **variable == ':')
        {
          (*variable)++;
          const size_t start_utf8_length = g_utf8_strlen(start, -1);
          const int length = strtol(*variable, variable, 10);
          if(length >= 0)
            end = g_utf8_offset_to_pointer(start, MIN(length, start_utf8_length));
          else
            end = g_utf8_offset_to_pointer(base_value + base_value_length, MAX(length, -start_utf8_length));
        }

        char *_base_value = g_strndup(start, end - start);
        g_free(base_value);
        base_value = _base_value;
      }
      break;
    case '#':
      /*
        $(var#Pattern)
          Remove from $var the shortest part of $Pattern that matches the front end of $var.
      */
      {
        char *pattern = expand(params, variable, ')');
        const size_t pattern_length = strlen(pattern);
        if(!strncmp(base_value, pattern, pattern_length))
        {
          char *_base_value = g_strdup(base_value + pattern_length);
          g_free(base_value);
          base_value = _base_value;
        }
        g_free(pattern);
      }
      break;
    case '%':
      /*
        $(var%Pattern)
          Remove from $var the shortest part of $Pattern that matches the back end of $var.
      */
      {
        char *pattern = expand(params, variable, ')');
        const size_t pattern_length = strlen(pattern);
        if(!strncmp(base_value + base_value_length - pattern_length, pattern, pattern_length))
          base_value[base_value_length - pattern_length] = '\0';
        g_free(pattern);
      }
      break;
    case '/':
      /*
        replacement. the following cases are possible:

        $(var/Pattern/Replacement)
          First match of Pattern, within var replaced with Replacement.
          If Replacement is omitted, then the first match of Pattern is replaced by nothing, that is, deleted.

        $(var//Pattern/Replacement)
          Global replacement. All matches of Pattern, within var replaced with Replacement.
          As above, if Replacement is omitted, then all occurrences of Pattern are replaced by nothing, that is, deleted.

        $(var/#Pattern/Replacement)
          If prefix of var matches Pattern, then substitute Replacement for Pattern.

        $(var/%Pattern/Replacement)
          If suffix of var matches Pattern, then substitute Replacement for Pattern.
      */
      {
        const char mode = **variable;

        if(mode == '/' || mode == '#' || mode == '%') (*variable)++;
        char *pattern = expand(params, variable, '/');
        const size_t pattern_length = strlen(pattern);
        (*variable)++;
        char *replacement = expand(params, variable, ')');
        const size_t replacement_length = strlen(replacement);

        switch(mode)
        {
          case '/':
          {
            // TODO: write a dt_util_str_replace that can deal with pattern_length ^^
            char *p = g_strndup(pattern, pattern_length);
            char *_base_value = dt_util_str_replace(base_value, p, replacement);
            g_free(p);
            g_free(base_value);
            base_value = _base_value;
            break;
          }
          case '#':
          {
            if(!strncmp(base_value, pattern, pattern_length))
            {
              char *_base_value = g_malloc(base_value_length - pattern_length + replacement_length + 1);
              char *end = g_stpcpy(_base_value, replacement);
              g_stpcpy(end, base_value + pattern_length);
              g_free(base_value);
              base_value = _base_value;
            }
            break;
          }
          case '%':
          {
            if(!strncmp(base_value + base_value_length - pattern_length, pattern, pattern_length))
            {
              char *_base_value = g_malloc(base_value_length - pattern_length + replacement_length + 1);
              base_value[base_value_length - pattern_length] = '\0';
              char *end = g_stpcpy(_base_value, base_value);
              g_stpcpy(end, replacement);
              g_free(base_value);
              base_value = _base_value;
            }
            break;
          }
          default:
          {
            // TODO: is there a strstr_len that limits the length of pattern?
            char *p = g_strndup(pattern, pattern_length);
            gchar *found = g_strstr_len(base_value, -1, p);
            g_free(p);
            if(found)
            {
              *found = '\0';
              char *_base_value = g_malloc(base_value_length - pattern_length + replacement_length + 1);
              char *end = g_stpcpy(_base_value, base_value);
              end = g_stpcpy(end, replacement);
              g_stpcpy(end, found + pattern_length);
              g_free(base_value);
              base_value = _base_value;
            }
            break;
          }
        }
        g_free(pattern);
        g_free(replacement);
      }
      break;
    case '^':
    case ',':
      /*
        changing the case:

        $(parameter^pattern)
        $(parameter^^pattern)
        $(parameter,pattern)
        $(parameter,,pattern)
          This expansion modifies the case of alphabetic characters in parameter.
          The ‘^’ operator converts lowercase letters to uppercase;
          the ‘,’ operator converts uppercase letters to lowercase.
          The ‘^^’ and ‘,,’ expansions convert each character in the expanded value;
          the ‘^’ and ‘,’ expansions convert only the first character in the expanded value.
      */
      {
        const char mode = **variable;
        char *_base_value = NULL;
        if(operation == '^' && mode == '^')
        {
          _base_value = g_utf8_strup (base_value, -1);
          (*variable)++;
        }
        else if(operation == ',' && mode == ',')
        {
          _base_value = g_utf8_strdown(base_value, -1);
          (*variable)++;
        }
        else
        {
          gunichar changed = g_utf8_get_char(base_value);
          changed = operation == '^' ? g_unichar_toupper(changed) : g_unichar_tolower(changed);
          int utf8_length = g_unichar_to_utf8(changed, NULL);
          char *next = g_utf8_next_char(base_value);
          _base_value = g_malloc0(base_value_length - (next - base_value) + utf8_length + 1);
          g_unichar_to_utf8(changed, _base_value);
          g_stpcpy(_base_value + utf8_length, next);
        }
        g_free(base_value);
        base_value = _base_value;
      }
      break;
  }

  if(**variable == ')')
    (*variable)++;
  else
  {
    // error case
    g_free(base_value);
    base_value = NULL;
  }

  return base_value;
}

static void grow_buffer(char **result, char **result_iter, size_t *result_length, size_t extra_space)
{
  const size_t used_length = *result_iter - *result;
  if(used_length + extra_space > *result_length)
  {
    *result_length = used_length + extra_space;
    *result = g_realloc(*result, *result_length + 1);
    *result_iter = *result + used_length;
  }
}

static char *expand(dt_variables_params_t *params, char **source, char extra_stop)
{
  char *result = g_strdup("");
  char *result_iter = result;
  size_t result_length = 0;
  char *source_iter = *source;
  const size_t source_length = strlen(*source);

  while(*source_iter && *source_iter != extra_stop)
  {
    // find start of variable, copying over everything till then
    while(*source_iter && *source_iter != extra_stop)
    {
      char c = *source_iter;
      if(c == '\\' && source_iter[1])
        c = *(++source_iter);
      else if(c == '$' && source_iter[1] == '(')
        break;

      if(result_iter - result >= result_length)
        grow_buffer(&result, &result_iter, &result_length, source_length - (source_iter - *source));
      *result_iter = c;
      result_iter++;
      source_iter++;

    }

    // it seems we have a variable here
    if(*source_iter == '$')
    {
      char *old_source_iter = source_iter;
      char *replacement = variable_get_value(params, &source_iter);
      if(replacement)
      {
        const size_t replacement_length = strlen(replacement);
        grow_buffer(&result, &result_iter, &result_length, replacement_length);
        memcpy(result_iter, replacement, replacement_length);
        result_iter += replacement_length;
        g_free(replacement);
      }
      else
      {
        // the error case of missing closing ')' -- try to recover
        source_iter = old_source_iter;
        grow_buffer(&result, &result_iter, &result_length, source_length - (source_iter - *source));
        *result_iter++ = *source_iter++;
      }
    }
  }

  *result_iter = '\0';
  *source = source_iter;

  return result;
}

char *dt_variables_expand(dt_variables_params_t *params, gchar *source, gboolean iterate)
{
  init_expansion(params, iterate);

  char *result = expand(params, &source, '\0');

  cleanup_expansion(params);

  return result;
}

void dt_variables_params_init(dt_variables_params_t **params)
{
  *params = g_malloc0(sizeof(dt_variables_params_t));
  (*params)->data = g_malloc0(sizeof(dt_variables_data_t));
  time_t now = time(NULL);
  localtime_r(&now, &(*params)->data->time);
  (*params)->data->exif_time = 0;
  (*params)->sequence = -1;
}

void dt_variables_params_destroy(dt_variables_params_t *params)
{
  g_free(params->data);
  g_free(params);
}

void dt_variables_set_max_width_height(dt_variables_params_t *params, int max_width, int max_height)
{
  params->data->max_width = max_width;
  params->data->max_height = max_height;
}

void dt_variables_set_time(dt_variables_params_t *params, time_t time)
{
  localtime_r(&time, &params->data->time);
}

void dt_variables_set_exif_time(dt_variables_params_t *params, time_t exif_time)
{
  params->data->exif_time = exif_time;
}

void dt_variables_reset_sequence(dt_variables_params_t *params)
{
  params->data->sequence = 0;
}

void dt_variables_set_tags_flags(dt_variables_params_t *params, uint32_t flags)
{
  params->data->tags_flags = flags;
}



// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
