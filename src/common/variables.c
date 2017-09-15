/*
    This file is part of darktable,
    copyright (c) 2010 henrik andersson.

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
#include "common/variables.h"
#include "common/colorlabels.h"
#include "common/darktable.h"
#include "common/file_location.h"
#include "common/image.h"
#include "common/image_cache.h"
#include "common/metadata.h"
#include "common/utility.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

typedef struct dt_variables_data_t
{
  gchar *source;

  /** expanded result string */
  gchar *result;
  time_t time;
  time_t exif_time;
  guint sequence;
} dt_variables_data_t;

gchar *_string_get_next_variable(gchar *string, gchar *variable, const size_t variable_size)
{
  // initiate empty output variable
  *variable = '\0';
  gchar *end = NULL;
  gchar *start = g_strstr_len(string, -1, "$(");
  if(start)
  {
    end = g_strstr_len(start, variable_size, ")");
    if(end)
    {
      // we want to include ')'
      end++;
      const int length = end - start + 1;
      g_strlcpy(variable, start, MIN(length, variable_size));
    }
  }
  // fprintf(stderr, "_string_get_next_variable: splitted %s in variable %s and remainder %s\n", string, variable,
  // end);
  return end;
}

gboolean _variable_search_replace(const gchar *search,
                                  const gchar *replace,
                                  gchar *value,
                                  size_t value_len)
{
    gchar *replace_value = NULL;

    replace_value = dt_util_str_replace(value, search, replace);
    if (g_utf8_strlen(replace_value, -1) >= value_len) {
        g_free(replace_value);
        return FALSE;
    }

    snprintf(value, value_len, "%s", (char *)replace_value);
    g_free(replace_value);

    return TRUE;
}

gboolean _variable_get_value(dt_variables_params_t *params, gchar *variable, gchar *value, size_t value_len)
{
  const gchar *file_ext = NULL;
  gchar *tmp_variable = NULL;
  gchar **strv_variable = NULL;
  glong length;
  gboolean got_value = FALSE;
  struct tm tim;
  localtime_r(&params->data->time, &tim);

  const gchar *homedir = dt_loc_get_home_dir(NULL);

  gchar *pictures_folder = NULL;

  if(g_get_user_special_dir(G_USER_DIRECTORY_PICTURES) == NULL)
    pictures_folder = g_build_path(G_DIR_SEPARATOR_S, homedir, "Pictures", (char *)NULL);
  else
    pictures_folder = g_strdup(g_get_user_special_dir(G_USER_DIRECTORY_PICTURES));

  // fprintf(stderr, "pictures_folder: %s\n", pictures_folder);
  if(params->filename)
  {
    file_ext = (g_strrstr(params->filename, ".") + 1);
    if(file_ext == (gchar *)1) file_ext = params->filename + strlen(params->filename);
  }

  /* image exif time */
  gboolean have_exif_tm = FALSE;
  int exif_iso = 100;
  char camera_maker[64];
  char camera_alias[64];
  int version = 0;
  int stars = 0;
  struct tm exif_tm = { 0 };
  if(params->imgid)
  {
    const dt_image_t *img = dt_image_cache_get(darktable.image_cache, params->imgid, 'r');
    if(sscanf(img->exif_datetime_taken, "%d:%d:%d %d:%d:%d", &exif_tm.tm_year, &exif_tm.tm_mon,
              &exif_tm.tm_mday, &exif_tm.tm_hour, &exif_tm.tm_min, &exif_tm.tm_sec) == 6)
    {
      exif_tm.tm_year -= 1900;
      exif_tm.tm_mon--;
      have_exif_tm = TRUE;
    }
    exif_iso = img->exif_iso;
    g_strlcpy(camera_maker, img->camera_maker, sizeof(camera_maker));
    g_strlcpy(camera_alias, img->camera_alias, sizeof(camera_alias));
    version = img->version;
    stars = (img->flags & 0x7);
    if(stars == 6) stars = -1;

    dt_image_cache_read_release(darktable.image_cache, img);
  }
  else if (params->data->exif_time) {
      localtime_r(&params->data->exif_time, &exif_tm);
      have_exif_tm = TRUE;
  }

  /* Remove the "$(" and ")" from the variable */
  length = g_utf8_strlen(variable, -1);
  tmp_variable = g_strndup(variable, length - 1);

  /* Split the variable by "/" */
  strv_variable = g_strsplit(tmp_variable + 2, "/", 3);
  g_free(tmp_variable);
  length = g_strv_length(strv_variable);
  if (length == 0) {
      return FALSE;
  }

  variable = strv_variable[0];

  if(g_strcmp0(variable, "YEAR") == 0 && (got_value = TRUE))
    snprintf(value, value_len, "%.4d", tim.tm_year + 1900);
  else if(g_strcmp0(variable, "MONTH") == 0 && (got_value = TRUE))
    snprintf(value, value_len, "%.2d", tim.tm_mon + 1);
  else if(g_strcmp0(variable, "DAY") == 0 && (got_value = TRUE))
    snprintf(value, value_len, "%.2d", tim.tm_mday);
  else if(g_strcmp0(variable, "HOUR") == 0 && (got_value = TRUE))
    snprintf(value, value_len, "%.2d", tim.tm_hour);
  else if(g_strcmp0(variable, "MINUTE") == 0 && (got_value = TRUE))
    snprintf(value, value_len, "%.2d", tim.tm_min);
  else if(g_strcmp0(variable, "SECOND") == 0 && (got_value = TRUE))
    snprintf(value, value_len, "%.2d", tim.tm_sec);

  else if(g_strcmp0(variable, "EXIF_YEAR") == 0 && (got_value = TRUE))
    snprintf(value, value_len, "%.4d", (have_exif_tm ? exif_tm.tm_year : tim.tm_year) + 1900);
  else if(g_strcmp0(variable, "EXIF_MONTH") == 0 && (got_value = TRUE))
    snprintf(value, value_len, "%.2d", (have_exif_tm ? exif_tm.tm_mon : tim.tm_mon) + 1);
  else if(g_strcmp0(variable, "EXIF_DAY") == 0 && (got_value = TRUE))
    snprintf(value, value_len, "%.2d", (have_exif_tm ? exif_tm.tm_mday : tim.tm_mday));
  else if(g_strcmp0(variable, "EXIF_HOUR") == 0 && (got_value = TRUE))
    snprintf(value, value_len, "%.2d", (have_exif_tm ? exif_tm.tm_hour : tim.tm_hour));
  else if(g_strcmp0(variable, "EXIF_MINUTE") == 0 && (got_value = TRUE))
    snprintf(value, value_len, "%.2d", (have_exif_tm ? exif_tm.tm_min : tim.tm_min));
  else if(g_strcmp0(variable, "EXIF_SECOND") == 0 && (got_value = TRUE))
    snprintf(value, value_len, "%.2d", (have_exif_tm ? exif_tm.tm_sec : tim.tm_sec));
  else if(g_strcmp0(variable, "EXIF_ISO") == 0 && (got_value = TRUE))
    snprintf(value, value_len, "%d", exif_iso);
  else if(g_strcmp0(variable, "MAKER") == 0 && (got_value = TRUE))
    snprintf(value, value_len, "%s", camera_maker);
  else if(g_strcmp0(variable, "MODEL") == 0 && (got_value = TRUE))
    snprintf(value, value_len, "%s", camera_alias);
  else if(g_strcmp0(variable, "ID") == 0 && (got_value = TRUE))
    snprintf(value, value_len, "%d", params->imgid);
  else if(g_strcmp0(variable, "VERSION") == 0 && (got_value = TRUE))
    snprintf(value, value_len, "%d", version);
  else if(g_strcmp0(variable, "JOBCODE") == 0 && (got_value = TRUE))
    snprintf(value, value_len, "%s", params->jobcode);
  else if(g_strcmp0(variable, "ROLL_NAME") == 0 && params->filename && (got_value = TRUE))
  {
    gchar *dirname = g_path_get_dirname(params->filename);
    gchar *basename = g_path_get_basename(dirname);
    snprintf(value, value_len, "%s", basename);
    g_free(basename);
    g_free(dirname);
  }
  else if(g_strcmp0(variable, "FILE_DIRECTORY") == 0 && params->filename && (got_value = TRUE))
  {
    gchar *dirname = g_path_get_dirname(params->filename);
    snprintf(value, value_len, "%s", dirname);
    g_free(dirname);
  } // undocumented : backward compatibility
  else if(g_strcmp0(variable, "FILE_FOLDER") == 0 && params->filename && (got_value = TRUE))
  {
    gchar *dirname = g_path_get_dirname(params->filename);
    snprintf(value, value_len, "%s", dirname);
    g_free(dirname);
  }
  else if(g_strcmp0(variable, "FILE_NAME") == 0 && params->filename && (got_value = TRUE))
  {
    gchar *basename = g_path_get_basename(params->filename);
    snprintf(value, value_len, "%s", basename);
    g_free(basename);
    if(g_strrstr(value, ".")) *(g_strrstr(value, ".")) = 0;
  }
  else if(g_strcmp0(variable, "FILE_EXTENSION") == 0 && params->filename && (got_value = TRUE))
    snprintf(value, value_len, "%s", file_ext);
  else if(g_strcmp0(variable, "SEQUENCE") == 0 && (got_value = TRUE))
    snprintf(value, value_len, "%.4d", params->sequence >= 0 ? params->sequence : params->data->sequence);
  else if(g_strcmp0(variable, "USERNAME") == 0 && (got_value = TRUE))
    snprintf(value, value_len, "%s", g_get_user_name());
  else if(g_strcmp0(variable, "HOME_FOLDER") == 0 && (got_value = TRUE))
    snprintf(value, value_len, "%s", homedir); // undocumented : backward compatibility
  else if(g_strcmp0(variable, "HOME") == 0 && (got_value = TRUE))
    snprintf(value, value_len, "%s", homedir);
  else if(g_strcmp0(variable, "PICTURES_FOLDER") == 0 && (got_value = TRUE))
    snprintf(value, value_len, "%s", pictures_folder);
  else if(g_strcmp0(variable, "DESKTOP_FOLDER") == 0 && (got_value = TRUE))
    snprintf(value, value_len, "%s",
             g_get_user_special_dir(G_USER_DIRECTORY_DESKTOP)); // undocumented : backward compatibility
  else if(g_strcmp0(variable, "DESKTOP") == 0 && (got_value = TRUE))
    snprintf(value, value_len, "%s", g_get_user_special_dir(G_USER_DIRECTORY_DESKTOP));
  else if(g_strcmp0(variable, "STARS") == 0 && (got_value = TRUE))
    snprintf(value, value_len, "%d", stars);
  else if(g_strcmp0(variable, "LABELS") == 0 && (got_value = TRUE))
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
      char *str = dt_util_glist_to_str(",", labels);
      g_list_free(labels);
      snprintf(value, value_len, "%s", str);
      g_free(str);
    }
    else
    {
      snprintf(value, value_len, "%s", _("none"));
    }
    g_list_free(res);
  }
  else if(g_strcmp0(variable, "TITLE") == 0 && params->filename && (got_value = TRUE))
  {
    GList *res = dt_metadata_get(params->imgid, "Xmp.dc.title", NULL);
    res = g_list_first(res);
    if(res != NULL)
    {
      snprintf(value, value_len, "%s", (char *)res->data);
    }
    else
    {
      snprintf(value, value_len, "%s", _("none"));
    }
    g_list_free_full(res, &g_free);
  }
  else if(g_strcmp0(variable, "CREATOR") == 0 && params->filename && (got_value = TRUE))
  {
    GList *res = dt_metadata_get(params->imgid, "Xmp.dc.creator", NULL);
    res = g_list_first(res);
    if(res != NULL)
    {
      snprintf(value, value_len, "%s", (char *)res->data);
    }
    else
    {
      snprintf(value, value_len, "%s", _("none"));
    }
    g_list_free_full(res, &g_free);
  }
  else if(g_strcmp0(variable, "PUBLISHER") == 0 && params->filename && (got_value = TRUE))
  {
    GList *res = dt_metadata_get(params->imgid, "Xmp.dc.publisher", NULL);
    res = g_list_first(res);
    if(res != NULL)
    {
      snprintf(value, value_len, "%s", (char *)res->data);
    }
    else
    {
      snprintf(value, value_len, "%s", _("none"));
    }
    g_list_free_full(res, &g_free);
  }
  else if(g_strcmp0(variable, "RIGHTS") == 0 && params->filename && (got_value = TRUE))
  {
    GList *res = dt_metadata_get(params->imgid, "Xmp.dc.rights", NULL);
    res = g_list_first(res);
    if(res != NULL)
    {
      snprintf(value, value_len, "%s", (char *)res->data);
    }
    else
    {
      snprintf(value, value_len, "%s", _("none"));
    }
    g_list_free_full(res, &g_free);
  }

  if (length == 3) {
      gboolean ok;

      ok = _variable_search_replace(strv_variable[1],
                                    strv_variable[2],
                                    value,
                                    value_len);
      if (!ok) {
          dt_print(DT_DEBUG_CONTROL,
                   "Search and replace failed for $(%s): %s",
                   variable, value);
      }
  }

  g_strfreev(strv_variable);
  g_free(pictures_folder);
  g_free((gchar *)homedir);

  return got_value;
}

void dt_variables_params_init(dt_variables_params_t **params)
{
  *params = g_malloc0(sizeof(dt_variables_params_t));
  (*params)->data = g_malloc0(sizeof(dt_variables_data_t));
  (*params)->data->time = time(NULL);
  (*params)->data->exif_time = 0;
  (*params)->sequence = -1;
}

void dt_variables_params_destroy(dt_variables_params_t *params)
{
  g_free(params->data->result);
  g_free(params->data);
  g_free(params);
}

void dt_variables_set_time(dt_variables_params_t *params, time_t time)
{
  params->data->time = time;
}

void dt_variables_set_exif_time(dt_variables_params_t *params, time_t exif_time)
{
  params->data->exif_time = exif_time;
}

gchar *dt_variables_get_result(dt_variables_params_t *params)
{
  return g_strdup(params->data->result);
}

gboolean dt_variables_expand(dt_variables_params_t *params, gchar *string, gboolean iterate)
{
  static const size_t variable_size = 128;
  gchar *variable = g_malloc(variable_size);
  gchar *value = g_malloc_n(1024, sizeof(gchar));

  // Let's free previous expanded result if any...
  g_free(params->data->result);

  if(iterate) params->data->sequence++;

  // Lets expand the string
  params->data->result = params->data->source = string;

  gchar *result = NULL;
  gchar *remainingVariables = params->data->source;
  gboolean success = FALSE;
  while((remainingVariables = _string_get_next_variable(remainingVariables, variable, variable_size)))
  {
    if(_variable_get_value(params, variable, value, 1024 * sizeof(gchar)))
    {
      // fprintf(stderr, "Substitute variable '%s' with value '%s'\n", variable, value);
      result = dt_util_str_replace(params->data->result, variable, value);
      if(result != params->data->result && result != params->data->source)
      {
        // we got a result
        if(params->data->result != params->data->source)
        {
          g_free(params->data->result);
        }
        params->data->result = result;
        success = TRUE;
      }
    }
  }

  if(!success)
  {
    params->data->result = g_strdup(string);
  }

  g_free(variable);
  g_free(value);
  return TRUE;
}

void dt_variables_reset_sequence(dt_variables_params_t *params)
{
  params->data->sequence = 0;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
