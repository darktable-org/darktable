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

#include "common/debug.h"
#include "common/darktable.h"
#include "bauhaus/bauhaus.h"
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
#include "common/datetime.h"
#include "control/conf.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

typedef struct dt_variables_data_t
{
  /** cached values that shouldn't change between variables in the same expansion process */
  // session data - do not come from image but are set by the application (import mainly)
  GDateTime *time;
  GDateTime *exif_time;
  char *exif_maker;
  char *exif_model;
  guint sequence;

  // max image size taken from export module GUI, can be zero
  int max_width;
  int max_height;

  // total sensor size, before RAW crop
  int sensor_width;
  int sensor_height;

  // max RAW file size, after the raw crop
  int raw_width;
  int raw_height;

  // image size after crop, but before export resize
  int crop_width;
  int crop_height;

  // image export size after crop and export resize
  int export_width;
  int export_height;

  // upscale allowed on export
  gboolean upscale;

  char *homedir;
  char *pictures_folder;
  const char *file_ext;

  gboolean have_exif_dt;
  gboolean show_msec;
  int exif_iso;
  char *camera_maker;
  char *camera_alias;
  char *exif_lens;
  int version;
  int stars;
  GDateTime *datetime;

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

static char *_expand_source(dt_variables_params_t *params, char **source, char extra_stop);

// gather some data that might be used for variable expansion
static void _init_expansion(dt_variables_params_t *params, gboolean iterate)
{
  if(iterate) params->data->sequence++;

  params->data->homedir = dt_loc_get_home_dir(NULL);

  if(g_get_user_special_dir(G_USER_DIRECTORY_PICTURES) == NULL)
    params->data->pictures_folder =
      g_build_path(G_DIR_SEPARATOR_S, params->data->homedir, "Pictures", (char *)NULL);
  else
    params->data->pictures_folder =
      g_strdup(g_get_user_special_dir(G_USER_DIRECTORY_PICTURES));

  if(params->filename)
  {
    params->data->file_ext = (g_strrstr(params->filename, ".") + 1);
    if(params->data->file_ext == (gchar *)1)
      params->data->file_ext = params->filename + strlen(params->filename);
  }
  else
    params->data->file_ext = NULL;

  /* image exif time */
  params->data->have_exif_dt = FALSE;
  params->data->exif_iso = 100;
  params->data->exif_lens = NULL;
  params->data->version = 0;
  params->data->stars = 0;
  params->data->exif_exposure = 0.0f;
  params->data->exif_exposure_bias = NAN;
  params->data->exif_aperture = 0.0f;
  params->data->exif_focal_length = 0.0f;
  params->data->exif_focus_distance = 0.0f;
  params->data->longitude = NAN;
  params->data->latitude = NAN;
  params->data->elevation = NAN;
  params->data->show_msec = dt_conf_get_bool("lighttable/ui/milliseconds");
  if(params->imgid)
  {
    params->data->camera_maker = NULL;
    params->data->camera_alias = NULL;
    const dt_image_t *img = params->img
      ? (dt_image_t *)params->img
      : dt_image_cache_get(darktable.image_cache, params->imgid, 'r');

    params->data->datetime = dt_datetime_img_to_gdatetime(img, darktable.utc_tz);
    if(params->data->datetime)
      params->data->have_exif_dt = TRUE;
    params->data->exif_iso = img->exif_iso;
    params->data->camera_maker = g_strdup(img->camera_maker);
    params->data->camera_alias = g_strdup(img->camera_alias);
    params->data->exif_lens = g_strdup(img->exif_lens);
    params->data->version = img->version;
    params->data->stars = (img->flags & 0x7);
    if(params->data->stars == 6 || (img->flags & DT_IMAGE_REJECTED) != 0)
      params->data->stars = -1;

    params->data->exif_exposure = img->exif_exposure;
    params->data->exif_exposure_bias = img->exif_exposure_bias;
    params->data->exif_aperture = img->exif_aperture;
    params->data->exif_focal_length = img->exif_focal_length;
    if(!isnan(img->exif_focus_distance) && fpclassify(img->exif_focus_distance) != FP_ZERO)
      params->data->exif_focus_distance = img->exif_focus_distance;
    params->data->longitude = img->geoloc.longitude;
    params->data->latitude = img->geoloc.latitude;
    params->data->elevation = img->geoloc.elevation;

    params->data->flags = img->flags;

    params->data->raw_height = img->p_height;
    params->data->raw_width = img->p_width;
    params->data->sensor_height = img->height;
    params->data->sensor_width = img->width;
    params->data->crop_height = img->final_height;
    params->data->crop_width = img->final_width;

    // for export size, assume initially no export scaling
    params->data->export_height = img->final_height;
    params->data->export_width = img->final_width;

    if(params->data->max_height || params->data->max_width)
    {
      // export scaling occurs, calculate the resize
      const int mh = params->data->max_height ? params->data->max_height : INT_MAX;
      const int mw = params->data->max_width ? params->data->max_width : INT_MAX;
      const float scale = fminf((float)mh / img->final_height,
                                (float)mw / img->final_width);
      if(scale < 1.0f || params->data->upscale)
      {
        // export scaling
        params->data->export_height = roundf(img->final_height * scale);
        params->data->export_width = roundf(img->final_width * scale);
      }
    }

    if(params->img == NULL) dt_image_cache_read_release(darktable.image_cache, img);
  }
  else
  { // session data
    params->data->datetime = params->data->exif_time;
    if(params->data->datetime)
      params->data->have_exif_dt = TRUE;
    params->data->camera_maker = params->data->exif_maker;
    params->data->camera_alias = params->data->exif_model;
  }
}

static void _cleanup_expansion(dt_variables_params_t *params)
{
  if(params->imgid)
  {
    if(params->data->datetime)
    {
      g_date_time_unref(params->data->datetime);
      params->data->datetime = NULL;
    }
    g_free(params->data->camera_maker);
    g_free(params->data->camera_alias);
  }
  g_free(params->data->homedir);
  g_free(params->data->pictures_folder);
}

static inline gboolean _has_prefix(char **str, const char *prefix)
{
  const gboolean res = g_str_has_prefix(*str, prefix);
  if(res) *str += strlen(prefix);
  return res;
}

static char *_variables_get_longitude(dt_variables_params_t *params)
{
  if(isnan(params->data->longitude))
    return g_strdup("");
  if(dt_conf_get_bool("plugins/lighttable/metadata_view/pretty_location")
     && g_strcmp0(params->jobcode, "infos") == 0)
  {
    return dt_util_longitude_str(params->data->longitude);
  }
  else
  {
    const gchar NS = params->data->longitude < 0 ? 'W' : 'E';
    return g_strdup_printf("%c%010.6f", NS, fabs(params->data->longitude));
  }
}

static char *_variables_get_latitude(dt_variables_params_t *params)
{
  if(isnan(params->data->latitude))
    return g_strdup("");
  if(dt_conf_get_bool("plugins/lighttable/metadata_view/pretty_location")
     && g_strcmp0(params->jobcode, "infos") == 0)
  {
    return dt_util_latitude_str(params->data->latitude);
  }
  else
  {
    const gchar NS = params->data->latitude < 0 ? 'S' : 'N';
    return g_strdup_printf("%c%09.6f", NS, fabs(params->data->latitude));
  }
}

static char *_get_base_value(dt_variables_params_t *params, char **variable)
{
  char *result = NULL;
  gboolean escape = TRUE;

  char exif_datetime[DT_DATETIME_LENGTH];
  GDateTime *datetime = params->data->have_exif_dt
    ? params->data->datetime
    : params->data->time;

  if(_has_prefix(variable, "YEAR.SHORT")
     || _has_prefix(variable, "SHORT_YEAR")
     || _has_prefix(variable, "DATE.SHORT_YEAR"))
    result = g_date_time_format(params->data->time, "%y");
  else if(_has_prefix(variable, "YEAR") || _has_prefix(variable, "DATE.LONG_YEAR"))
    result = g_date_time_format(params->data->time, "%Y");
  else if(_has_prefix(variable, "MONTH.SHORT") || _has_prefix(variable, "DATE.SHORT_MONTH"))
    result = g_date_time_format(params->data->time, "%b");
  else if(_has_prefix(variable, "MONTH.LONG") || _has_prefix(variable, "DATE.LONG_MONTH"))
    result = g_date_time_format(params->data->time, "%B");
  else if(_has_prefix(variable, "MONTH") || _has_prefix(variable, "DATE.MONTH"))
    result = g_date_time_format(params->data->time, "%m");
  else if(_has_prefix(variable, "DAY") || _has_prefix(variable, "DATE.DAY"))
    result = g_date_time_format(params->data->time, "%d");
  else if(_has_prefix(variable, "HOUR.AMPM") || _has_prefix(variable, "DATE.HOUR_AMPM"))
    result = g_date_time_format(params->data->time, "%I %p");
  else if(_has_prefix(variable, "HOUR") || _has_prefix(variable, "DATE.HOUR"))
    result = g_date_time_format(params->data->time, "%H");
  else if(_has_prefix(variable, "MINUTE") || _has_prefix(variable, "DATE.MINUTE"))
    result = g_date_time_format(params->data->time, "%M");
  else if(_has_prefix(variable, "SECOND") || _has_prefix(variable, "DATE.SECOND"))
    result = g_date_time_format(params->data->time, "%S");
  else if(_has_prefix(variable, "MSEC"))
  {
    result = g_date_time_format(params->data->time, "%f");
    result[3] = '\0';
  }
  // for watermark backward compatibility
  else if(_has_prefix(variable, "DATE"))
  {
    dt_datetime_gdatetime_to_exif(exif_datetime,
                                  params->data->show_msec
                                  ? DT_DATETIME_LENGTH
                                  : DT_DATETIME_EXIF_LENGTH, params->data->time);
    result = g_strdup(exif_datetime);
  }

  else if(_has_prefix(variable, "EXIF.DATE.REGIONAL"))
    result = g_date_time_format(datetime, "%x");

  else if(_has_prefix(variable, "EXIF.TIME.REGIONAL"))
    result = g_date_time_format(datetime, "%X");

  else if(_has_prefix(variable, "EXIF.YEAR.SHORT")
          || _has_prefix(variable, "EXIF.DATE.SHORT_YEAR"))
    result = g_date_time_format(datetime, "%y");

  else if(_has_prefix(variable, "EXIF.YEAR")
          || _has_prefix(variable, "EXIF_YEAR")
          || _has_prefix(variable, "EXIF.DATE.LONG_YEAR"))
    result = g_date_time_format(datetime, "%Y");

  else if(_has_prefix(variable, "EXIF.MONTH.SHORT")
          || _has_prefix(variable, "EXIF.DATE.SHORT_MONTH"))
    result = g_date_time_format(datetime, "%b");

  else if(_has_prefix(variable, "EXIF.MONTH.LONG")
          || _has_prefix(variable, "EXIF.DATE.LONG_MONTH"))
    result = g_date_time_format(datetime, "%B");

  else if(_has_prefix(variable, "EXIF.MONTH")
          || _has_prefix(variable, "EXIF_MONTH")
          || _has_prefix(variable, "EXIF.DATE.MONTH"))
    result = g_date_time_format(datetime, "%m");

  else if(_has_prefix(variable, "EXIF.DAY")
          || _has_prefix(variable, "EXIF_DAY")
          || _has_prefix(variable, "EXIF.DATE.DAY"))
    result = g_date_time_format(datetime, "%d");

  else if(_has_prefix(variable, "EXIF.HOUR.AMPM")
          || _has_prefix(variable, "EXIF.DATE.HOUR_AMPM"))
    result = g_date_time_format(datetime, "%I %p");

  else if(_has_prefix(variable, "EXIF.HOUR")
          || _has_prefix(variable, "EXIF_HOUR")
          || _has_prefix(variable, "EXIF.DATE.HOUR"))
    result = g_date_time_format(datetime, "%H");

  else if(_has_prefix(variable, "EXIF.MINUTE")
          || _has_prefix(variable, "EXIF_MINUTE")
          || _has_prefix(variable, "EXIF.DATE.MINUTE"))
    result = g_date_time_format(datetime, "%M");

  else if(_has_prefix(variable, "EXIF.SECOND")
          || _has_prefix(variable, "EXIF_SECOND")
          || _has_prefix(variable, "EXIF.DATE.SECOND"))
    result = g_date_time_format(datetime, "%S");

  else if(_has_prefix(variable, "EXIF.MSEC")
          || _has_prefix(variable, "EXIF_MSEC"))
  {
    result = g_date_time_format(datetime, "%f");
    result[3] = '\0';
  }
  // for watermark backward compatibility
  else if(_has_prefix(variable, "EXIF.DATE"))
  {
    dt_datetime_gdatetime_to_exif(exif_datetime,
                                  params->data->show_msec
                                  ? DT_DATETIME_LENGTH
                                  : DT_DATETIME_EXIF_LENGTH, datetime);
    result = g_strdup(exif_datetime);
  }
  else if(_has_prefix(variable, "EXIF.ISO") || _has_prefix(variable, "EXIF_ISO"))
    result = g_strdup_printf("%d", params->data->exif_iso);
  else if(_has_prefix(variable, "NL") && g_strcmp0(params->jobcode, "infos") == 0)
    result = g_strdup_printf("\n");
  else if(_has_prefix(variable, "EXIF.EXPOSURE.BIAS")
          || _has_prefix(variable, "EXIF_EXPOSURE_BIAS"))
  {
    if(!isnan(params->data->exif_exposure_bias))
      result = g_strdup_printf("%+.2f", params->data->exif_exposure_bias);
  }
  else if(_has_prefix(variable, "EXIF.EXPOSURE")
          || _has_prefix(variable, "EXIF_EXPOSURE"))
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
  else if(_has_prefix(variable, "EXIF.APERTURE")
          || _has_prefix(variable, "EXIF_APERTURE"))
    result = g_strdup_printf("%.1f", params->data->exif_aperture);

  else if(_has_prefix(variable, "EXIF.FOCAL.LENGTH")
          || _has_prefix(variable, "EXIF_FOCAL_LENGTH"))
    result = g_strdup_printf("%d", (int)params->data->exif_focal_length);

  else if(_has_prefix(variable, "EXIF.FOCUS.DISTANCE")
          || _has_prefix(variable, "EXIF_FOCUS_DISTANCE"))
    result = g_strdup_printf("%.2f", params->data->exif_focus_distance);

  else if(_has_prefix(variable, "LONGITUDE")
          || _has_prefix(variable, "GPS.LONGITUDE"))
    result = _variables_get_longitude(params);

  else if(_has_prefix(variable, "LATITUDE")
          || _has_prefix(variable, "GPS.LATITUDE"))
    result = _variables_get_latitude(params);

  else if(_has_prefix(variable, "ELEVATION")
          || _has_prefix(variable, "GPS.ELEVATION"))
    result = g_strdup_printf("%.2f", params->data->elevation);

  // for watermark backward compatibility
  else if(_has_prefix(variable, "GPS.LOCATION"))
  {
    gchar *parts[4] = { 0 };
    int i = 0;
    if(!isnan(params->data->latitude))
      parts[i++] = _variables_get_latitude(params);
    if(!isnan(params->data->longitude))
      parts[i++] = _variables_get_longitude(params);
    if(!isnan(params->data->elevation))
      parts[i++] = g_strdup_printf("%.2f", params->data->elevation);

    result = g_strjoinv(", ", parts);
    for(int j = 0; j < i; j++)
      g_free(parts[j]);
  }
  else if(_has_prefix(variable, "EXIF.MAKER") || _has_prefix(variable, "MAKER"))
    result = g_strdup(params->data->camera_maker);
  else if(_has_prefix(variable, "EXIF.MODEL") || _has_prefix(variable, "MODEL"))
    result = g_strdup(params->data->camera_alias);
  else if(_has_prefix(variable, "EXIF.LENS") || _has_prefix(variable, "LENS"))
    result = g_strdup(params->data->exif_lens);
  else if(_has_prefix(variable, "ID") || _has_prefix(variable, "IMAGE.ID"))
    result = g_strdup_printf("%u", params->imgid);
  else if(_has_prefix(variable, "IMAGE.EXIF"))
  {
    gchar buffer[1024];
    const dt_image_t *img = params->img
      ? (dt_image_t *)params->img
      : dt_image_cache_get(darktable.image_cache, params->imgid, 'r');

    dt_image_print_exif(img, buffer, sizeof(buffer));
    if(params->img == NULL)
      dt_image_cache_read_release(darktable.image_cache, img);
    result = g_strdup(buffer);
  }
  else if(_has_prefix(variable, "VERSION.NAME")
          || _has_prefix(variable, "VERSION_NAME"))
  {
    GList *res = dt_metadata_get(params->imgid, "Xmp.darktable.version_name", NULL);
    if(res != NULL)
    {
      result = g_strdup((char *)res->data);
    }
    g_list_free_full(res, &g_free);
  }
  else if(_has_prefix(variable, "VERSION.IF_MULTI")
          || _has_prefix(variable, "VERSION_IF_MULTI"))
  {
    sqlite3_stmt *stmt;

    // count duplicates
    // clang-format off
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "SELECT COUNT(1)"
                                " FROM images AS i1"
                                " WHERE EXISTS (SELECT 'y' FROM images AS i2"
                                "               WHERE  i2.id = ?1"
                                "               AND    i1.film_id = i2.film_id"
                                "               AND    i1.filename = i2.filename)",
                                -1, &stmt, NULL);
    // clang-format on
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
  else if(_has_prefix(variable, "VERSION"))
    result = g_strdup_printf("%d", params->data->version);
  else if(_has_prefix(variable, "JOBCODE"))
    result = g_strdup(params->jobcode);
  else if(_has_prefix(variable, "ROLL.NAME")
          || _has_prefix(variable, "ROLL_NAME"))
  {
    if(params->filename)
    {
      gchar *dirname = g_path_get_dirname(params->filename);
      result = g_path_get_basename(dirname);
      g_free(dirname);
    }
  }
  else if(_has_prefix(variable, "FILE.DIRECTORY")
          || _has_prefix(variable, "FILE_DIRECTORY"))
  {
    // undocumented : backward compatibility
    if(params->filename)
      result = g_path_get_dirname(params->filename);
  }
  else if(_has_prefix(variable, "FILE.FOLDER")
          || _has_prefix(variable, "FILE_FOLDER"))
  {
    if(params->filename)
      result = g_path_get_dirname(params->filename);
  }
  // for watermark backward compatibility
  else if(_has_prefix(variable, "IMAGE.FILENAME"))
  {
    if(params->filename)
      result = g_strdup(params->filename);
  }
  else if(_has_prefix(variable, "FILE.NAME")
          || _has_prefix(variable, "FILE_NAME")
          || _has_prefix(variable, "IMAGE.BASENAME"))
  {
    if(params->filename)
    {
      result = g_path_get_basename(params->filename);
      char *dot = g_strrstr(result, ".");
      if(dot) *dot = '\0';
    }
  }
  else if(_has_prefix(variable, "FILE.EXTENSION")
          || _has_prefix(variable, "FILE_EXTENSION"))
    result = g_strdup(params->data->file_ext);
  else if(_has_prefix(variable, "SEQUENCE"))
  {
    uint8_t nb_digit = 4;
    if(g_ascii_isdigit(*variable[0]))
    {
      nb_digit = (uint8_t)*variable[0] & 0b1111;
      (*variable) ++;
    }
    result = g_strdup_printf("%.*u", nb_digit,
                             params->sequence >= 0
                             ? params->sequence
                             : params->data->sequence);
  }
  else if(_has_prefix(variable, "USERNAME"))
    result = g_strdup(g_get_user_name());
  else if(_has_prefix(variable, "FOLDER.HOME")
          || _has_prefix(variable, "HOME_FOLDER")
          || _has_prefix(variable, "HOME"))
    result = g_strdup(params->data->homedir);

  else if(_has_prefix(variable, "FOLDER.PICTURES")
          || _has_prefix(variable, "PICTURES_FOLDER"))
    result = g_strdup(params->data->pictures_folder);

  else if(_has_prefix(variable, "FOLDER.DESKTOP")
          || _has_prefix(variable, "DESKTOP_FOLDER"))
    result = g_strdup(g_get_user_special_dir(G_USER_DIRECTORY_DESKTOP)); // undocumented : backward compatibility

  else if(_has_prefix(variable, "DESKTOP"))
    result = g_strdup(g_get_user_special_dir(G_USER_DIRECTORY_DESKTOP));

  else if(_has_prefix(variable, "STARS"))
    result = g_strdup_printf("%d", params->data->stars);

  else if(_has_prefix(variable, "RATING.ICONS")
          || _has_prefix(variable, "RATING_ICONS")
          || _has_prefix(variable, "Xmp.xmp.Rating"))
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
  else if((_has_prefix(variable, "LABELS.ICONS")
           || _has_prefix(variable, "LABELS_ICONS")
           || _has_prefix(variable, "LABELS.COLORICONS")
           || _has_prefix(variable, "LABELS_COLORICONS"))
          && g_strcmp0(params->jobcode, "infos") == 0)
  {
    escape = FALSE;
    GList *res = dt_metadata_get(params->imgid, "Xmp.darktable.colorlabels", NULL);
    for(GList *res_iter = res; res_iter; res_iter = g_list_next(res_iter))
    {
      const int dot_index = GPOINTER_TO_INT(res_iter->data);
      const GdkRGBA c = darktable.bauhaus->colorlabels[dot_index];
      result = dt_util_dstrcat
        (result,
         "<span foreground='#%02x%02x%02x'>⬤ </span>",
         (guint)(c.red*255), (guint)(c.green*255), (guint)(c.blue*255));
    }
    g_list_free(res);
  }
  else if(_has_prefix(variable, "LABELS"))
  {
    // TODO: currently we concatenate all the color labels with a ','
    // as a separator. Maybe it's better to only use the first/last
    // label?
    GList *res = dt_metadata_get(params->imgid, "Xmp.darktable.colorlabels", NULL);
    if(res != NULL)
    {
      GList *labels = NULL;
      for(GList *res_iter = res; res_iter; res_iter = g_list_next(res_iter))
      {
        labels = g_list_prepend
          (labels,
           (char *)(_(dt_colorlabels_to_string(GPOINTER_TO_INT(res_iter->data)))));
      }
      labels = g_list_reverse(labels);  // list was built in reverse order, so un-reverse it
      result = dt_util_glist_to_str(",", labels);
      g_list_free(labels);
    }
    g_list_free(res);
  }
  else if(_has_prefix(variable, "TITLE")
          || _has_prefix(variable, "Xmp.dc.title"))
  {
    GList *res = dt_metadata_get(params->imgid, "Xmp.dc.title", NULL);
    if(res != NULL)
    {
      result = g_strdup((char *)res->data);
    }
    g_list_free_full(res, &g_free);
  }
  else if(_has_prefix(variable, "DESCRIPTION")
          || _has_prefix(variable, "Xmp.dc.description"))
  {
    GList *res = dt_metadata_get(params->imgid, "Xmp.dc.description", NULL);
    if(res != NULL)
    {
      result = g_strdup((char *)res->data);
    }
    g_list_free_full(res, &g_free);
  }
  else if(_has_prefix(variable, "CREATOR")
          || _has_prefix(variable, "Xmp.dc.creator"))
  {
    GList *res = dt_metadata_get(params->imgid, "Xmp.dc.creator", NULL);
    if(res != NULL)
    {
      result = g_strdup((char *)res->data);
    }
    g_list_free_full(res, &g_free);
  }
  else if(_has_prefix(variable, "PUBLISHER")
          || _has_prefix(variable, "Xmp.dc.publisher"))
  {
    GList *res = dt_metadata_get(params->imgid, "Xmp.dc.publisher", NULL);
    if(res != NULL)
    {
      result = g_strdup((char *)res->data);
    }
    g_list_free_full(res, &g_free);
  }
  else if(_has_prefix(variable, "RIGHTS")
          || _has_prefix(variable, "Xmp.dc.rights"))
  {
    GList *res = dt_metadata_get(params->imgid, "Xmp.dc.rights", NULL);
    if(res != NULL)
    {
      result = g_strdup((char *)res->data);
    }
    g_list_free_full(res, &g_free);
  }
  else if(_has_prefix(variable, "OPENCL.ACTIVATED")
          || _has_prefix(variable, "OPENCL_ACTIVATED"))
  {
    if(dt_opencl_is_enabled())
      result = g_strdup(_("yes"));
    else
      result = g_strdup(_("no"));
  }
  else if(_has_prefix(variable, "WIDTH.MAX")
          || _has_prefix(variable, "MAX_WIDTH"))
    result = g_strdup_printf("%d", params->data->max_width);

  else if(_has_prefix(variable, "WIDTH.SENSOR")
          || _has_prefix(variable, "SENSOR_WIDTH"))
    result = g_strdup_printf("%d", params->data->sensor_width);

  else if(_has_prefix(variable, "WIDTH.RAW")
          || _has_prefix(variable, "RAW_WIDTH"))
    result = g_strdup_printf("%d", params->data->raw_width);

  else if(_has_prefix(variable, "WIDTH.CROP")
          || _has_prefix(variable, "CROP_WIDTH"))
    result = g_strdup_printf("%d", params->data->crop_width);

  else if(_has_prefix(variable, "WIDTH.EXPORT")
          || _has_prefix(variable, "EXPORT_WIDTH"))
    result = g_strdup_printf("%d", params->data->export_width);

  else if(_has_prefix(variable, "HEIGHT.MAX")
          || _has_prefix(variable, "MAX_HEIGHT"))
    result = g_strdup_printf("%d", params->data->max_height);

  else if(_has_prefix(variable, "HEIGHT.SENSOR")
          || _has_prefix(variable, "SENSOR_HEIGHT"))
    result = g_strdup_printf("%d", params->data->sensor_height);

  else if(_has_prefix(variable, "HEIGHT.RAW")
          || _has_prefix(variable, "RAW_HEIGHT"))
    result = g_strdup_printf("%d", params->data->raw_height);

  else if(_has_prefix(variable, "HEIGHT.CROP")
          || _has_prefix(variable, "CROP_HEIGHT"))
    result = g_strdup_printf("%d", params->data->crop_height);

  else if(_has_prefix(variable, "HEIGHT.EXPORT")
          || _has_prefix(variable, "EXPORT_HEIGHT"))
    result = g_strdup_printf("%d", params->data->export_height);

  else if(_has_prefix(variable, "CATEGORY"))
  {
    // CATEGORY should be followed by n [0,9] and
    // "(category)". category can contain 0 or more '|'
    if(g_ascii_isdigit(*variable[0]))
    {
      const uint8_t level = (uint8_t)*variable[0] & 0b1111;
      (*variable) ++;
      if(*variable[0] == '(')
      {
        char *category = g_strdup(*variable + 1);
        char *end = g_strstr_len(category, -1, ")");
        if(end)
        {
          end[0] = '|';
          end[1] = '\0';
          (*variable) += strlen(category) + 1;
          char *tag = dt_tag_get_subtags(params->imgid, category, (int)level);
          if(tag)
          {
            result = g_strdup(tag);
            g_free(tag);
          }
        }
        g_free(category);
      }
    }
  }
  else if(_has_prefix(variable, "TAGS") || _has_prefix(variable, "IMAGE.TAGS"))
  {
    GList *tags_list = dt_tag_get_list_export(params->imgid, params->data->tags_flags);
    char *tags = dt_util_glist_to_str(", ", tags_list);
    g_list_free_full(tags_list, g_free);
    result = g_strdup(tags);
    g_free(tags);
  }
  else if(_has_prefix(variable, "SIDECAR_TXT") && g_strcmp0(params->jobcode, "infos") == 0
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
  else if(_has_prefix(variable, "DARKTABLE.VERSION")
          || _has_prefix(variable, "DARKTABLE_VERSION"))
    result = g_strdup(darktable_package_version);

  else if(_has_prefix(variable, "DARKTABLE.NAME")
          || _has_prefix(variable, "DARKTABLE_NAME"))
    result = g_strdup(PACKAGE_NAME);
  else
  {
    // go past what looks like an invalid variable. we only expect to
    // see [a-zA-Z]* in a variable name.
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
// the descriptions in the comments are referring to the bash
// behaviour, dt doesn't do it 100% like that!
static char *_variable_get_value(dt_variables_params_t *params, char **variable)
{
  // invariant: the variable starts with "$(" which we can skip
  (*variable) += 2;

  // first get the value of the variable
  char *base_value = _get_base_value(params, variable); // this is never going to be NULL!
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
        char *replacement = _expand_source(params, variable, ')');
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
        char *replacement = _expand_source(params, variable, ')');
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

        If offset evaluates to a number less than zero, the value is
        used as an offset in characters from the end of the value of
        parameter. If length evaluates to a number less than zero, it
        is interpreted as an offset in characters from the end of the
        value of parameter rather than a number of characters, and the
        expansion is the characters between offset and that result.
      */
      {
        const glong base_value_utf8_length = g_utf8_strlen(base_value, -1);
        const glong offset = strtol(*variable, variable, 10);

        // find where to start
        char *start; // from where to copy ...
        if(offset >= 0)
          start = g_utf8_offset_to_pointer
            (base_value, MIN(offset, base_value_utf8_length));
        else
          start = g_utf8_offset_to_pointer
            (base_value + base_value_length, MAX(offset, -base_value_utf8_length));

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
            end = g_utf8_offset_to_pointer
              (base_value + base_value_length, MAX(length, -start_utf8_length));
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
        char *pattern = _expand_source(params, variable, ')');
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
        char *pattern = _expand_source(params, variable, ')');
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
          First match of Pattern, within var replaced with
          Replacement.  If Replacement is omitted, then the first
          match of Pattern is replaced by nothing, that is, deleted.

        $(var//Pattern/Replacement)
          Global replacement. All matches of Pattern, within var
          replaced with Replacement.  As above, if Replacement is
          omitted, then all occurrences of Pattern are replaced by
          nothing, that is, deleted.

        $(var/#Pattern/Replacement)
          If prefix of var matches Pattern, then substitute Replacement for Pattern.

        $(var/%Pattern/Replacement)
          If suffix of var matches Pattern, then substitute Replacement for Pattern.
      */
      {
        const char mode = **variable;

        if(mode == '/' || mode == '#' || mode == '%') (*variable)++;
        char *pattern = _expand_source(params, variable, '/');
        const size_t pattern_length = strlen(pattern);
        (*variable)++;
        char *replacement = _expand_source(params, variable, ')');
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
              char *_base_value =
                g_malloc(base_value_length - pattern_length + replacement_length + 1);
              char *end = g_stpcpy(_base_value, replacement);
              g_stpcpy(end, base_value + pattern_length);
              g_free(base_value);
              base_value = _base_value;
            }
            break;
          }
          case '%':
          {
            if(!strncmp(base_value + base_value_length - pattern_length,
                        pattern,
                        pattern_length))
            {
              char *_base_value =
                g_malloc(base_value_length - pattern_length + replacement_length + 1);
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
              char *_base_value =
                g_malloc(base_value_length - pattern_length + replacement_length + 1);
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
          changed = operation == '^'
            ? g_unichar_toupper(changed)
            : g_unichar_tolower(changed);

          int utf8_length = g_unichar_to_utf8(changed, NULL);
          char *next = g_utf8_next_char(base_value);
          _base_value =
            g_malloc0(base_value_length - (next - base_value) + utf8_length + 1);
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

static void _grow_buffer(char **result,
                         char **result_iter,
                         size_t *result_length,
                         const size_t extra_space)
{
  const size_t used_length = *result_iter - *result;
  if(used_length + extra_space > *result_length)
  {
    *result_length = used_length + extra_space;
    *result = g_realloc(*result, *result_length + 1);
    *result_iter = *result + used_length;
  }
}

static char *_expand_source(dt_variables_params_t *params,
                            char **source,
                            const char extra_stop)
{
  char *result = g_strdup("");
  if(!*source) return result;
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
        _grow_buffer(&result,
                     &result_iter,
                     &result_length,
                     source_length - (source_iter - *source));
      *result_iter = c;
      result_iter++;
      source_iter++;

    }

    // it seems we have a variable here
    if(*source_iter == '$')
    {
      char *old_source_iter = source_iter;
      char *replacement = _variable_get_value(params, &source_iter);
      if(replacement)
      {
        const size_t replacement_length = strlen(replacement);
        _grow_buffer(&result, &result_iter, &result_length, replacement_length);
        memcpy(result_iter, replacement, replacement_length);
        result_iter += replacement_length;
        g_free(replacement);
      }
      else
      {
        // the error case of missing closing ')' -- try to recover
        source_iter = old_source_iter;
        _grow_buffer(&result,
                     &result_iter,
                     &result_length,
                     source_length - (source_iter - *source));
        *result_iter++ = *source_iter++;
      }
    }
  }

  *result_iter = '\0';
  *source = source_iter;

  return result;
}

char *dt_variables_expand(dt_variables_params_t *params,
                          gchar *source,
                          const gboolean iterate)
{
  _init_expansion(params, iterate);

  char *result = _expand_source(params, &source, '\0');

  _cleanup_expansion(params);

  return result;
}

void dt_variables_params_init(dt_variables_params_t **params)
{
  *params = g_malloc0(sizeof(dt_variables_params_t));
  (*params)->data = g_malloc0(sizeof(dt_variables_data_t));
  (*params)->data->time = g_date_time_new_now_local();
  (*params)->data->exif_time = NULL;
  (*params)->sequence = -1;
  (*params)->img = NULL;
}

void dt_variables_params_destroy(dt_variables_params_t *params)
{
  if(params->data->time)
    g_date_time_unref(params->data->time);
  if(params->data->exif_time)
    g_date_time_unref(params->data->exif_time);
  g_free(params->data->exif_maker);
  g_free(params->data->exif_model);
  g_free(params->data);
  g_free(params);
}

void dt_variables_set_max_width_height(dt_variables_params_t *params,
                                       const int max_width,
                                       const int max_height)
{
  params->data->max_width = max_width;
  params->data->max_height = max_height;
}

void dt_variables_set_upscale(dt_variables_params_t *params,
                              const gboolean upscale)
{
  params->data->upscale = upscale;
}

void dt_variables_set_time(dt_variables_params_t *params,
                           const char *time)
{
  params->data->time = dt_datetime_exif_to_gdatetime(time, darktable.utc_tz);
}

void dt_variables_set_exif_basic_info(dt_variables_params_t *params,
                                      const dt_image_basic_exif_t *basic_exif)
{
  if(params->data->exif_time)
  {
    g_date_time_unref(params->data->exif_time);
    params->data->exif_time = NULL;
  }
  if(basic_exif->datetime[0])
    params->data->exif_time =
      dt_datetime_exif_to_gdatetime(basic_exif->datetime, darktable.utc_tz);

  g_free(params->data->exif_maker);
  params->data->exif_maker = NULL;
  if(basic_exif->maker[0])
    params->data->exif_maker = g_strdup(basic_exif->maker);

  g_free(params->data->exif_model);
  params->data->exif_model = NULL;
  if(basic_exif->model[0])
    params->data->exif_model = g_strdup(basic_exif->model);
}

void dt_variables_reset_sequence(dt_variables_params_t *params)
{
  params->data->sequence = 0;
}

void dt_variables_set_tags_flags(dt_variables_params_t *params,
                                 const uint32_t flags)
{
  params->data->tags_flags = flags;
}



// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
