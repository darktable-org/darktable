/*
    This file is part of darktable,
    Copyright (C) 2011-2020 darktable developers.

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

#include "common/collection.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/image_cache.h"
#include "common/metadata.h"
#include "common/tags.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/develop.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include "libs/lib_api.h"

#include <gdk/gdkkeysyms.h>
#include <sys/param.h>
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif
#ifdef USE_LUA
#include "lua/call.h"
#include "lua/image.h"
#endif

#define SHOW_FLAGS 1

DT_MODULE(1)

typedef enum dt_metadata_pref_cols_t
{
  DT_METADATA_PREF_COL_INDEX = 0, // index
  DT_METADATA_PREF_COL_NAME_L,    // displayed name
  DT_METADATA_PREF_COL_VISIBLE,   // visibility
  DT_METADATA_PREF_NUM_COLS
} dt_metadata_pref_cols_t;

typedef enum dt_metadata_cols_t
{
  DT_METADATA_COL_INDEX = 0,      // index
  DT_METADATA_COL_NAME,           // metadata english name
  DT_METADATA_COL_NAME_L,         // displayed name
  DT_METADATA_COL_TOOLTIP,        // tooltip
  DT_METADATA_COL_VALUE,          // metadata value
  DT_METADATA_COL_VISIBLE,        // visibility
  DT_METADATA_COL_ORDER,          // display order
  DT_METADATA_NUM_COLS
} dt_metadata_cols_t;

typedef struct dt_lib_metadata_view_t
{
  GtkTreeModel *model;
  GtkTreeModel *sort_model;
  GtkTreeModel *filter_model;
  GtkWidget *view;
} dt_lib_metadata_view_t;

enum
{
  /* internal */
  md_internal_filmroll = 0,
  md_internal_imgid,
  md_internal_groupid,
  md_internal_filename,
  md_internal_version,
  md_internal_fullpath,
  md_internal_local_copy,
  md_internal_import_timestamp,
  md_internal_change_timestamp,
  md_internal_export_timestamp,
  md_internal_print_timestamp,
#if SHOW_FLAGS
  md_internal_flags,
#endif

  /* exif */
  md_exif_model,
  md_exif_maker,
  md_exif_lens,
  md_exif_aperture,
  md_exif_exposure,
  md_exif_exposure_bias,
  md_exif_focal_length,
  md_exif_focus_distance,
  md_exif_iso,
  md_exif_datetime,
  md_exif_width,
  md_exif_height,

  /* size of final image */
  md_width,
  md_height,

  /* xmp */
  md_xmp_metadata,

  /* geotagging */
  md_geotagging_lat = md_xmp_metadata + DT_METADATA_NUMBER,
  md_geotagging_lon,
  md_geotagging_ele,

  /* tags */
  md_tag_names,
  md_categories,

  /* entries, do not touch! */
  md_size
};

static const char *_labels[] = {
  /* internal */
  N_("filmroll"),
  N_("image id"),
  N_("group id"),
  N_("filename"),
  N_("version"),
  N_("full path"),
  N_("local copy"),
  N_("import timestamp"),
  N_("change timestamp"),
  N_("export timestamp"),
  N_("print timestamp"),
#if SHOW_FLAGS
  N_("flags"),
#endif

  /* exif */
  N_("model"),
  N_("maker"),
  N_("lens"),
  N_("aperture"),
  N_("exposure"),
  N_("exposure bias"),
  N_("focal length"),
  N_("focus distance"),
  N_("ISO"),
  N_("datetime"),
  N_("width"),
  N_("height"),
  N_("export width"),
  N_("export height"),

  /* xmp */
  //FIXME: reserve DT_METADATA_NUMBER places
  "","","","","","","",

  /* geotagging */
  N_("latitude"),
  N_("longitude"),
  N_("elevation"),

  /* tags */
  N_("tags"),
  N_("categories"),
};

static gboolean _dndactive = FALSE;

const char *name(dt_lib_module_t *self)
{
  return _("image information");
}

const char **views(dt_lib_module_t *self)
{
  static const char *v[] = {"*", NULL};
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_LEFT_CENTER;
}

int position()
{
  return 299;
}

static gboolean _is_metadata_ui(const int i)
{
  // internal metadata ar not to be shown on the ui
  if(i >= md_xmp_metadata && i < md_xmp_metadata + DT_METADATA_NUMBER)
  {
    const uint32_t keyid = dt_metadata_get_keyid_by_display_order(i);
    return !(dt_metadata_get_type(keyid) == DT_METADATA_TYPE_INTERNAL);
  }
  else return TRUE;
}

static const char *_get_label(const int i)
{
  if(i >= md_xmp_metadata && i < md_xmp_metadata + DT_METADATA_NUMBER)
  {
    const uint32_t keyid = dt_metadata_get_keyid_by_display_order(i - md_xmp_metadata);
    return(dt_metadata_get_name(keyid));
  }
  else return _labels[i];
}

/* initialize the labels text */
static void _lib_metadata_view_init_labels(GtkTreeModel *model)
{
  GtkListStore *store = GTK_LIST_STORE(model);
  for(int i = 0; i < md_size; i++)
  {
    GtkTreeIter iter;
    gtk_list_store_append(store, &iter);
    const char *name = _get_label(i);
    gtk_list_store_set(store, &iter,
                       DT_METADATA_COL_INDEX, i,
                       DT_METADATA_COL_NAME, name,
                       DT_METADATA_COL_NAME_L, _(name),
                       DT_METADATA_COL_VALUE, "-",
                       DT_METADATA_COL_VISIBLE, _is_metadata_ui(i),
                       DT_METADATA_COL_ORDER, i,
                       -1);
  }
}

// helper which eliminates non-printable characters from a string
// strings which are already in valid UTF-8 are retained.
static void _filter_non_printable(char *string, size_t length)
{
  /* explicitly tell the validator to ignore the trailing nulls, otherwise this fails */
  if(g_utf8_validate(string, -1, 0)) return;

  unsigned char *str = (unsigned char *)string;
  int n = 0;

  while(*str != '\000' && n < length)
  {
    if((*str < 0x20) || (*str >= 0x7f)) *str = '.';

    str++;
    n++;
  }
}

#define NODATA_STRING "-"

static void _get_index_iter(const int i, GtkTreeModel *model, GtkTreeIter *iter)
{
  char path_str[4];
  snprintf(path_str, sizeof(path_str), "%d", i);
  GtkTreePath *path = gtk_tree_path_new_from_string(path_str);
  gtk_tree_model_get_iter(GTK_TREE_MODEL(model), iter, path);
  gtk_tree_path_free(path);
}

/* helper function for updating a metadata value */
static void _metadata_update_value(const int i, const char *value, GtkTreeModel *model)
{
  gboolean validated = g_utf8_validate(value, -1, NULL);
  const gchar *str = validated ? value : NODATA_STRING;
  GtkTreeIter iter;
  _get_index_iter(i, model, &iter);
  gtk_list_store_set(GTK_LIST_STORE(model), &iter, DT_METADATA_COL_VALUE, str, -1);
}

static void _metadata_update_tooltip(const int i, const char *tooltip, GtkTreeModel *model)
{
  GtkTreeIter iter;
  _get_index_iter(i, model, &iter);
  gtk_list_store_set(GTK_LIST_STORE(model), &iter, DT_METADATA_COL_TOOLTIP, tooltip, -1);
}

static void _metadata_update_timestamp(const int i, const time_t *value, GtkTreeModel *model)
{
  char datetime[200];
  // just %c is too long and includes a time zone that we don't know from exif
  const size_t datetime_len = strftime(datetime, sizeof(datetime), "%a %x %X", localtime(value));
  if(datetime_len > 0)
  {
    const gboolean valid_utf = g_utf8_validate(datetime, datetime_len, NULL);
    if(valid_utf)
    {
      _metadata_update_value(i, datetime, model);
    }
    else
    {
      GError *error = NULL;
      gchar *local_datetime = g_locale_to_utf8(datetime,datetime_len,NULL,NULL, &error);
      if(local_datetime)
      {
        _metadata_update_value(i, local_datetime, model);
        g_free(local_datetime);
      }
      else
      {
        _metadata_update_value(i, NODATA_STRING, model);
        fprintf(stderr, "[metadata timestamp] could not convert '%s' to UTF-8: %s\n", datetime, error->message);
        g_error_free(error);
      }
    }
  }
  else
    _metadata_update_value(i, NODATA_STRING, model);
}

#ifdef USE_LUA
static int lua_update_metadata(lua_State*L);
#endif
/* update all values to reflect mouse over image id or no data at all */
static void _metadata_view_update_values(dt_lib_module_t *self)
{
  dt_lib_metadata_view_t *d = (dt_lib_metadata_view_t *)self->data;
  GtkTreeModel *model = GTK_TREE_MODEL(d->model);
  int32_t mouse_over_id = dt_control_get_mouse_over_id();

  if(mouse_over_id == -1)
  {
    const dt_view_t *cv = dt_view_manager_get_current_view(darktable.view_manager);
    if(cv->view((dt_view_t *)cv) == DT_VIEW_DARKROOM)
    {
      mouse_over_id = darktable.develop->image_storage.id;
    }
    else
    {
      sqlite3_stmt *stmt;
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT imgid FROM main.selected_images LIMIT 1",
                                  -1, &stmt, NULL);
      if(sqlite3_step(stmt) == SQLITE_ROW) mouse_over_id = sqlite3_column_int(stmt, 0);
      sqlite3_finalize(stmt);
    }
  }

  if(mouse_over_id >= 0)
  {
    char value[512];
    char pathname[PATH_MAX] = { 0 };

    const dt_image_t *img = dt_image_cache_get(darktable.image_cache, mouse_over_id, 'r');
    if(!img) goto fill_minuses;
    if(img->film_id == -1)
    {
      dt_image_cache_read_release(darktable.image_cache, img);
      goto fill_minuses;
    }

    /* update all metadata */

    dt_image_film_roll(img, value, sizeof(value));
    _metadata_update_value(md_internal_filmroll, value, model);
    char tooltip[512];
    snprintf(tooltip, sizeof(tooltip), _("double click to jump to film roll\n%s"), value);
    _metadata_update_tooltip(md_internal_filmroll, tooltip, model);

    snprintf(value, sizeof(value), "%d", img->id);
    _metadata_update_value(md_internal_imgid, value, model);

    snprintf(value, sizeof(value), "%d", img->group_id);
    _metadata_update_value(md_internal_groupid, value, model);

    _metadata_update_value(md_internal_filename, img->filename, model);

    snprintf(value, sizeof(value), "%d", img->version);
    _metadata_update_value(md_internal_version, value, model);

    gboolean from_cache = FALSE;
    dt_image_full_path(img->id, pathname, sizeof(pathname), &from_cache);
    _metadata_update_value(md_internal_fullpath, pathname, model);

    g_strlcpy(value, (img->flags & DT_IMAGE_LOCAL_COPY) ? _("yes") : _("no"), sizeof(value));
    _metadata_update_value(md_internal_local_copy, value, model);

    if (img->import_timestamp >=0)
      _metadata_update_timestamp(md_internal_import_timestamp, &img->import_timestamp, model);
    else
      _metadata_update_value(md_internal_import_timestamp, NODATA_STRING, model);

    if (img->change_timestamp >=0)
      _metadata_update_timestamp(md_internal_change_timestamp, &img->change_timestamp, model);
    else
      _metadata_update_value(md_internal_change_timestamp, NODATA_STRING, model);

    if (img->export_timestamp >=0)
      _metadata_update_timestamp(md_internal_export_timestamp, &img->export_timestamp, model);
    else
      _metadata_update_value(md_internal_export_timestamp, NODATA_STRING, model);

    if (img->print_timestamp >=0)
      _metadata_update_timestamp(md_internal_print_timestamp, &img->print_timestamp, model);
    else
      _metadata_update_value(md_internal_print_timestamp, NODATA_STRING, model);

    // TODO: decide if this should be removed for a release. maybe #ifdef'ing to only add it to git compiles?

    // the bits of the flags
#if SHOW_FLAGS
    {
      #define EMPTY_FIELD '.'
      #define FALSE_FIELD '.'
      #define TRUE_FIELD '!'

      char *flags_tooltip = NULL;
      char *flag_descriptions[] = { N_("unused"),
                                    N_("unused/deprecated"),
                                    N_("ldr"),
                                    N_("raw"),
                                    N_("hdr"),
                                    N_("marked for deletion"),
                                    N_("auto-applying presets applied"),
                                    N_("legacy flag. set for all new images"),
                                    N_("local copy"),
                                    N_("has .txt"),
                                    N_("has .wav"),
                                    N_("monochrome")
      };
      char *tooltip_parts[15] = { 0 };
      int next_tooltip_part = 0;

      memset(value, EMPTY_FIELD, sizeof(value));

      int stars = img->flags & 0x7;
      char *star_string = NULL;
      if(stars == 6)
      {
        value[0] = 'x';
        tooltip_parts[next_tooltip_part++] = _("image rejected");
      }
      else
      {
        value[0] = '0' + stars;
        tooltip_parts[next_tooltip_part++] = star_string = g_strdup_printf(ngettext("image has %d star", "image has %d stars", stars), stars);
      }


      if(img->flags & 8)
      {
        value[1] = TRUE_FIELD;
        tooltip_parts[next_tooltip_part++] = _(flag_descriptions[0]);
      }
      else
        value[1] = FALSE_FIELD;

      if(img->flags & DT_IMAGE_THUMBNAIL_DEPRECATED)
      {
        value[2] = TRUE_FIELD;
        tooltip_parts[next_tooltip_part++] = _(flag_descriptions[1]);
      }
      else
        value[2] = FALSE_FIELD;

      if(img->flags & DT_IMAGE_LDR)
      {
        value[3] = 'l';
        tooltip_parts[next_tooltip_part++] = _(flag_descriptions[2]);
      }

      if(img->flags & DT_IMAGE_RAW)
      {
        value[4] = 'r';
        tooltip_parts[next_tooltip_part++] = _(flag_descriptions[3]);
      }

      if(img->flags & DT_IMAGE_HDR)
      {
        value[5] = 'h';
        tooltip_parts[next_tooltip_part++] = _(flag_descriptions[4]);
      }

      if(img->flags & DT_IMAGE_REMOVE)
      {
        value[6] = 'd';
        tooltip_parts[next_tooltip_part++] = _(flag_descriptions[5]);
      }

      if(img->flags & DT_IMAGE_AUTO_PRESETS_APPLIED)
      {
        value[7] = 'a';
        tooltip_parts[next_tooltip_part++] = _(flag_descriptions[6]);
      }

      if(img->flags & DT_IMAGE_NO_LEGACY_PRESETS)
      {
        value[8] = 'p';
        tooltip_parts[next_tooltip_part++] = _(flag_descriptions[7]);
      }

      if(img->flags & DT_IMAGE_LOCAL_COPY)
      {
        value[9] = 'c';
        tooltip_parts[next_tooltip_part++] = _(flag_descriptions[8]);
      }

      if(img->flags & DT_IMAGE_HAS_TXT)
      {
        value[10] = 't';
        tooltip_parts[next_tooltip_part++] = _(flag_descriptions[9]);
      }

      if(img->flags & DT_IMAGE_HAS_WAV)
      {
        value[11] = 'w';
        tooltip_parts[next_tooltip_part++] = _(flag_descriptions[10]);
      }

      if(dt_image_monochrome_flags(img))
      {
        value[12] = 'm';
        tooltip_parts[next_tooltip_part++] = _(flag_descriptions[11]);
      }

      static const struct
      {
        char *tooltip;
        char flag;
      } loaders[] =
      {
        { N_("unknown"), EMPTY_FIELD},
        { N_("tiff"), 't'},
        { N_("png"), 'p'},
        { N_("j2k"), 'J'},
        { N_("jpeg"), 'j'},
        { N_("exr"), 'e'},
        { N_("rgbe"), 'R'},
        { N_("pfm"), 'P'},
        { N_("GraphicsMagick"), 'g'},
        { N_("rawspeed"), 'r'},
        { N_("netpnm"), 'n'},
        { N_("avif"), 'a'},
      };

      const int loader = (unsigned int)img->loader < sizeof(loaders) / sizeof(*loaders) ? img->loader : 0;
      value[13] = loaders[loader].flag;
      char *loader_tooltip = g_strdup_printf(_("loader: %s"), _(loaders[loader].tooltip));
      tooltip_parts[next_tooltip_part++] = loader_tooltip;

      value[14] = '\0';

      flags_tooltip = g_strjoinv("\n", tooltip_parts);
      g_free(loader_tooltip);

      _metadata_update_value(md_internal_flags, value, model);
      _metadata_update_tooltip(md_internal_flags, flags_tooltip, model);

      g_free(star_string);
      g_free(flags_tooltip);

      #undef EMPTY_FIELD
      #undef FALSE_FIELD
      #undef TRUE_FIELD
    }
#endif // SHOW_FLAGS

    /* EXIF */
    _metadata_update_value(md_exif_model, img->camera_alias, model);
    _metadata_update_value(md_exif_lens, img->exif_lens, model);
    _metadata_update_value(md_exif_maker, img->camera_maker, model);

    snprintf(value, sizeof(value), "f/%.1f", img->exif_aperture);
    _metadata_update_value(md_exif_aperture, value, model);

    char *exposure_str = dt_util_format_exposure(img->exif_exposure);
    _metadata_update_value(md_exif_exposure, exposure_str, model);
    g_free(exposure_str);

    if(isnan(img->exif_exposure_bias))
    {
      _metadata_update_value(md_exif_exposure_bias, NODATA_STRING, model);
    }
    else
    {
      snprintf(value, sizeof(value), _("%+.2f EV"), img->exif_exposure_bias);
      _metadata_update_value(md_exif_exposure_bias, value, model);
    }

    snprintf(value, sizeof(value), "%.0f mm", img->exif_focal_length);
    _metadata_update_value(md_exif_focal_length, value, model);

    if(isnan(img->exif_focus_distance) || fpclassify(img->exif_focus_distance) == FP_ZERO)
    {
      _metadata_update_value(md_exif_focus_distance, NODATA_STRING, model);
    }
    else
    {
      snprintf(value, sizeof(value), "%.2f m", img->exif_focus_distance);
      _metadata_update_value(md_exif_focus_distance, value, model);
    }

    snprintf(value, sizeof(value), "%.0f", img->exif_iso);
    _metadata_update_value(md_exif_iso, value, model);

    struct tm tt_exif = { 0 };
    if(sscanf(img->exif_datetime_taken, "%d:%d:%d %d:%d:%d", &tt_exif.tm_year, &tt_exif.tm_mon,
      &tt_exif.tm_mday, &tt_exif.tm_hour, &tt_exif.tm_min, &tt_exif.tm_sec) == 6)
    {
      tt_exif.tm_year -= 1900;
      tt_exif.tm_mon--;
      tt_exif.tm_isdst = -1;
      const time_t exif_timestamp = mktime(&tt_exif);
      _metadata_update_timestamp(md_exif_datetime, &exif_timestamp, model);
    }
    else
      _metadata_update_value(md_exif_datetime, img->exif_datetime_taken, model);

    if(((img->p_width != img->width) || (img->p_height != img->height))  &&
       (img->p_width || img->p_height))
    {
      snprintf(value, sizeof(value), "%d (%d)", img->p_height, img->height);
      _metadata_update_value(md_exif_height, value, model);
      snprintf(value, sizeof(value), "%d (%d) ",img->p_width, img->width);
      _metadata_update_value(md_exif_width, value, model);
    }
    else {
    snprintf(value, sizeof(value), "%d", img->height);
    _metadata_update_value(md_exif_height, value, model);
    snprintf(value, sizeof(value), "%d", img->width);
    _metadata_update_value(md_exif_width, value, model);
    }

    if(img->verified_size)
    {
      snprintf(value, sizeof(value), "%d", img->final_height);
    _metadata_update_value(md_height, value, model);
      snprintf(value, sizeof(value), "%d", img->final_width);
    _metadata_update_value(md_width, value, model);
    }
    else
    {
      _metadata_update_value(md_height, "-", model);
      _metadata_update_value(md_width, "-", model);
    }
    /* XMP */
    for(unsigned int i = 0; i < DT_METADATA_NUMBER; i++)
    {
      const uint32_t keyid = dt_metadata_get_keyid_by_display_order(i);
      const gchar *key = dt_metadata_get_key(keyid);
      const gboolean hidden = dt_metadata_get_type(keyid) == DT_METADATA_TYPE_INTERNAL;
      if(hidden)
      {
        g_strlcpy(value, NODATA_STRING, sizeof(value));
      }
      else
      {
        GList *res = dt_metadata_get(img->id, key, NULL);
        if(res)
        {
          g_strlcpy(value, (char *)res->data, sizeof(value));
          _filter_non_printable(value, sizeof(value));
          g_list_free_full(res, &g_free);
        }
        else
          g_strlcpy(value, NODATA_STRING, sizeof(value));
      }
      _metadata_update_value(md_xmp_metadata+i, value, model);
    }

    /* geotagging */
    /* latitude */
    if(isnan(img->geoloc.latitude))
    {
      _metadata_update_value(md_geotagging_lat, NODATA_STRING, model);
    }
    else
    {
      if(dt_conf_get_bool("plugins/lighttable/metadata_view/pretty_location"))
      {
        gchar *latitude = dt_util_latitude_str(img->geoloc.latitude);
        _metadata_update_value(md_geotagging_lat, latitude, model);
        g_free(latitude);
      }
      else
      {
        const gchar NS = img->geoloc.latitude < 0 ? 'S' : 'N';
        snprintf(value, sizeof(value), "%c %09.6f", NS, fabs(img->geoloc.latitude));
        _metadata_update_value(md_geotagging_lat, value, model);
      }
    }
    /* longitude */
    if(isnan(img->geoloc.longitude))
    {
      _metadata_update_value(md_geotagging_lon, NODATA_STRING, model);
    }
    else
    {
      if(dt_conf_get_bool("plugins/lighttable/metadata_view/pretty_location"))
      {
        gchar *longitude = dt_util_longitude_str(img->geoloc.longitude);
        _metadata_update_value(md_geotagging_lon, longitude, model);
        g_free(longitude);
      }
      else
      {
        const gchar EW = img->geoloc.longitude < 0 ? 'W' : 'E';
        snprintf(value, sizeof(value), "%c %010.6f", EW, fabs(img->geoloc.longitude));
        _metadata_update_value(md_geotagging_lon, value, model);
      }
    }
    /* elevation */
    if(isnan(img->geoloc.elevation))
    {
      _metadata_update_value(md_geotagging_ele, NODATA_STRING, model);
    }
    else
    {
      if(dt_conf_get_bool("plugins/lighttable/metadata_view/pretty_location"))
      {
        gchar *elevation = dt_util_elevation_str(img->geoloc.elevation);
        _metadata_update_value(md_geotagging_ele, elevation, model);
        g_free(elevation);
      }
      else
      {
        snprintf(value, sizeof(value), "%.2f %s", img->geoloc.elevation, _("m"));
        _metadata_update_value(md_geotagging_ele, value, model);
      }
    }

    /* tags */
    GList *tags = NULL;
    char *tagstring = NULL;
    char *categoriesstring = NULL;
    if(dt_tag_get_attached(mouse_over_id, &tags, TRUE))
    {
      gint length = 0;
      for(GList *taglist = tags; taglist; taglist = g_list_next(taglist))
      {
        const char *tagname = ((dt_tag_t *)taglist->data)->leave;
        if (!(((dt_tag_t *)taglist->data)->flags & DT_TF_CATEGORY))
        {
          // tags - just keywords
          length = length + strlen(tagname) + 2;
          if(length < 45)
            tagstring = dt_util_dstrcat(tagstring, "%s, ", tagname);
          else
          {
            tagstring = dt_util_dstrcat(tagstring, "\n%s, ", tagname);
            length = strlen(tagname) + 2;
          }
        }
        else
        {
          // categories - needs parent category to make sense
          char *category = g_strdup(((dt_tag_t *)taglist->data)->tag);
          char *catend = g_strrstr(category, "|");
          if (catend)
          {
            catend[0] = '\0';
            char *catstart = g_strrstr(category, "|");
            catstart = catstart ? catstart + 1 : category;
            categoriesstring = dt_util_dstrcat(categoriesstring, categoriesstring ? "\n%s: %s " : "%s: %s ",
                  catstart, ((dt_tag_t *)taglist->data)->leave);
          }
          else
            categoriesstring = dt_util_dstrcat(categoriesstring, categoriesstring ? "\n%s" : "%s",
                  ((dt_tag_t *)taglist->data)->leave);
          g_free(category);
        }
      }
      if(tagstring) tagstring[strlen(tagstring)-2] = '\0';
    }
    _metadata_update_value(md_tag_names, tagstring ? tagstring : NODATA_STRING, model);
    _metadata_update_value(md_categories, categoriesstring ? categoriesstring : NODATA_STRING, model);

    g_free(tagstring);
    g_free(categoriesstring);
    dt_tag_free_result(&tags);

    /* release img */
    dt_image_cache_read_release(darktable.image_cache, img);

#ifdef USE_LUA
    dt_lua_async_call_alien(lua_update_metadata,
        0,NULL,NULL,
        LUA_ASYNC_TYPENAME,"void*",self,
        LUA_ASYNC_TYPENAME,"int32_t",mouse_over_id,LUA_ASYNC_DONE);
#endif
  }

  return;

/* reset */
fill_minuses:
  for(int k = 0; k < md_size; k++) _metadata_update_value(k, NODATA_STRING, model);
#ifdef USE_LUA
  dt_lua_async_call_alien(lua_update_metadata,
      0,NULL,NULL,
        LUA_ASYNC_TYPENAME,"void*",self,
        LUA_ASYNC_TYPENAME,"int32_t",-1,LUA_ASYNC_DONE);
#endif
}

static void _jump_to()
{
  int32_t imgid = dt_control_get_mouse_over_id();
  if(imgid == -1)
  {
    sqlite3_stmt *stmt;

    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT imgid FROM main.selected_images", -1, &stmt,
                                NULL);

    if(sqlite3_step(stmt) == SQLITE_ROW) imgid = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
  }
  if(imgid != -1)
  {
    char path[512];
    const dt_image_t *img = dt_image_cache_get(darktable.image_cache, imgid, 'r');
    dt_image_film_roll_directory(img, path, sizeof(path));
    dt_image_cache_read_release(darktable.image_cache, img);
    char collect[1024];
    snprintf(collect, sizeof(collect), "1:0:0:%s$", path);
    dt_collection_deserialize(collect);
  }
}

static gboolean _row_tooltip_setup(GtkWidget *view, gint x, gint y, gboolean kb_mode,
      GtkTooltip* tooltip, dt_lib_module_t *self)
{
  gboolean res = FALSE;
  GtkTreePath *path = NULL;
  GtkTreeViewColumn *column = NULL;
  // Get view path mouse position
  if(gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(view), x, y, &path, &column, NULL, NULL))
  {
    gint x_offset = gtk_tree_view_column_get_x_offset(column);
    GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(view));
    GtkTreeIter iter;
    if (gtk_tree_model_get_iter(model, &iter, path))
    {
      char *value = NULL;
      if(x_offset > 0)
      {
        char *text = NULL;
        gtk_tree_model_get(model, &iter, DT_METADATA_COL_VALUE, &value,
                                         DT_METADATA_COL_TOOLTIP, &text, -1);
        if(text)
        {
          g_free(value);
          value = text;
        }
        if(value && value[0] != '-')
        {
          gtk_tooltip_set_text(tooltip, value);
          res = TRUE;
        }
      }
      else
      {
        gtk_tree_model_get(model, &iter, DT_METADATA_COL_NAME_L, &value, -1);
        gtk_tooltip_set_text(tooltip, value);
        res = TRUE;
      }
      g_free(value);
    }
  }
  gtk_tree_path_free(path);

  return res;
}

static gboolean _filmroll_clicked(GtkWidget *view, GdkEventButton *event, gpointer null)
{
  if(event->type != GDK_2BUTTON_PRESS) return FALSE;
  GtkTreePath *path = NULL;
  // Get view path for row that was clicked
  if(gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(view), (gint)event->x, (gint)event->y,
                                   &path, NULL, NULL, NULL))
  {
    GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(view));
    GtkTreeIter iter;
    if(gtk_tree_model_get_iter(model, &iter, path))
    {
      char *name = NULL;
      gtk_tree_model_get(model, &iter,
                         DT_METADATA_COL_NAME, &name, -1);
      if(name && !g_strcmp0(name, _get_label(md_internal_filmroll)))
      {
        g_free(name);
        _jump_to();
        return TRUE;
      }
      g_free(name);
    }
  }
  return FALSE;
}

static gboolean _jump_to_accel(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                               GdkModifierType modifier, gpointer data)
{
  _jump_to();
  return TRUE;
}

/* callback for the mouse over image change signal */
static void _mouse_over_image_callback(gpointer instance, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  if(dt_control_running()) _metadata_view_update_values(self);
}

void init_key_accels(dt_lib_module_t *self)
{
  dt_accel_register_lib(self, NC_("accel", "jump to film roll"), GDK_KEY_j, GDK_CONTROL_MASK);
}

void connect_key_accels(dt_lib_module_t *self)
{
  GClosure *closure = g_cclosure_new(G_CALLBACK(_jump_to_accel), (gpointer)self, NULL);
  dt_accel_connect_lib(self, "jump to film roll", closure);
}

static char *_get_current_configuration(dt_lib_module_t *self)
{
  dt_lib_metadata_view_t *d = (dt_lib_metadata_view_t *)self->data;
  char *pref = NULL;
  GtkTreeIter iter;
  gboolean valid = gtk_tree_model_get_iter_first(d->sort_model, &iter);
  while(valid)
  {
    int index = 0;
    char *name = NULL;
    gboolean visible = TRUE;
    gtk_tree_model_get(d->sort_model, &iter,
                       DT_METADATA_COL_INDEX, &index,
                       DT_METADATA_COL_NAME, &name,
                       DT_METADATA_COL_VISIBLE, &visible,
                       -1);
    if(_is_metadata_ui(index))
      pref = dt_util_dstrcat(pref, "%s%s,", visible ? "" : "|", name);
    g_free(name);
    valid = gtk_tree_model_iter_next(d->sort_model, &iter);
  }
  if(pref)
  {
    pref[strlen(pref) - 1] = '\0';
  }
  return pref;
}

static void _apply_preferences(const char *pref, dt_lib_module_t *self)
{
  if(!pref || !pref[0]) return;
  dt_lib_metadata_view_t *d = (dt_lib_metadata_view_t *)self->data;
  g_object_ref(d->filter_model);
  gtk_tree_view_set_model(GTK_TREE_VIEW(d->view), NULL);
  GList *prefs = dt_util_str_to_glist(",", pref);
  int k = 0;
  for(GList *meta = prefs; meta; meta = g_list_next(meta))
  {
    const char *name = (char *)meta->data;
    gboolean visible = TRUE;
    if(name)
    {
      if(name[0] == '|')
      {
        name++;
        visible = FALSE;
      }
      GtkTreeIter iter;
      gboolean valid = gtk_tree_model_get_iter_first(d->model, &iter);
      while(valid)
      {
        char *text = NULL;
        gtk_tree_model_get(d->model, &iter, DT_METADATA_COL_NAME, &text, -1);
        if(name && !g_strcmp0(name, text))
        {
          gtk_list_store_set(GTK_LIST_STORE(d->model), &iter,
                                            DT_METADATA_COL_ORDER, k,
                                            DT_METADATA_COL_VISIBLE, visible,
                                            -1);
          g_free(text);
          break;
        }
        g_free(text);
        valid = gtk_tree_model_iter_next(d->model, &iter);
      }
    }
    else continue;
    k++;
  }
  g_list_free_full(prefs, g_free);
  gtk_tree_view_set_model(GTK_TREE_VIEW(d->view), d->filter_model);
  g_object_unref(d->filter_model);
}

static void _save_preferences(dt_lib_module_t *self)
{
  char *pref = _get_current_configuration(self);
  dt_conf_set_string("plugins/lighttable/metadata_view/visible", pref);
  g_free(pref);
}

static void _select_toggled_callback(GtkCellRendererToggle *cell_renderer, gchar *path_str, gpointer user_data)
{
  GtkListStore *store = (GtkListStore *)user_data;
  GtkTreeIter iter;
  GtkTreePath *path = gtk_tree_path_new_from_string(path_str);
  gboolean selected;

  gtk_tree_model_get_iter(GTK_TREE_MODEL(store), &iter, path);
  gtk_tree_model_get(GTK_TREE_MODEL(store), &iter, DT_METADATA_PREF_COL_VISIBLE, &selected, -1);
  gtk_list_store_set(store, &iter, DT_METADATA_PREF_COL_VISIBLE, !selected, -1);

  gtk_tree_path_free(path);
}

static void _drag_data_inserted(GtkTreeModel *tree_model, GtkTreePath *path, GtkTreeIter *iter, gpointer user_data)
{
  _dndactive = TRUE;
}

void _menuitem_preferences(GtkMenuItem *menuitem, dt_lib_module_t *self)
{
  dt_lib_metadata_view_t *d = (dt_lib_metadata_view_t *)self->data;

  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  GtkWidget *dialog = gtk_dialog_new_with_buttons(_("metadata settings"), GTK_WINDOW(win), GTK_DIALOG_DESTROY_WITH_PARENT,
                                       _("cancel"), GTK_RESPONSE_NONE, _("save"), GTK_RESPONSE_YES, NULL);
  GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));

  GtkWidget *w = gtk_scrolled_window_new(NULL, NULL);
  gtk_widget_set_size_request(w, -1, DT_PIXEL_APPLY_DPI(300));
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(w), GTK_POLICY_NEVER, GTK_POLICY_ALWAYS);
  gtk_scrolled_window_set_overlay_scrolling(GTK_SCROLLED_WINDOW(w), FALSE);
  gtk_box_pack_start(GTK_BOX(area), w, TRUE, TRUE, 0);

  GtkListStore *store = gtk_list_store_new(DT_METADATA_PREF_NUM_COLS,
                                           G_TYPE_INT, G_TYPE_STRING, G_TYPE_BOOLEAN);
  GtkTreeModel *model = GTK_TREE_MODEL(store);

  {
    GtkTreeIter sort_iter;
    gboolean valid = gtk_tree_model_get_iter_first(d->sort_model, &sort_iter);
    while(valid)
    {
      GtkTreeIter iter;
      int index;
      gboolean visible;
      char *name = NULL;
      gtk_tree_model_get(d->sort_model, &sort_iter,
                         DT_METADATA_COL_INDEX, &index,
                         DT_METADATA_COL_NAME_L, &name,
                         DT_METADATA_COL_VISIBLE, &visible,
                         -1);
      if(!_is_metadata_ui(index))
        continue;
      gtk_list_store_append(store, &iter);
      gtk_list_store_set(store, &iter,
                         DT_METADATA_PREF_COL_INDEX, index,
                         DT_METADATA_PREF_COL_NAME_L, name,
                         DT_METADATA_PREF_COL_VISIBLE, visible,
                         -1);
      g_free(name);
      valid = gtk_tree_model_iter_next(d->sort_model, &sort_iter);
    }
  }

  GtkWidget *view = gtk_tree_view_new_with_model(model);
  g_object_unref(model);
  GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
  GtkTreeViewColumn *column = gtk_tree_view_column_new_with_attributes(_("metadata"), renderer,
                                                    "text", DT_METADATA_PREF_COL_NAME_L, NULL);
  gtk_tree_view_column_set_expand(column, TRUE);
  gtk_tree_view_append_column(GTK_TREE_VIEW(view), column);
  GtkWidget *header = gtk_tree_view_column_get_button(column);
  gtk_widget_set_tooltip_text(header,
                _("drag and drop one row at a time until you get the desired order"
                "\nuntick to hide metadata which are not of interest for you"
                "\nif different settings are needed, use presets"));
  renderer = gtk_cell_renderer_toggle_new();
  g_signal_connect(renderer, "toggled", G_CALLBACK(_select_toggled_callback), store);
  column = gtk_tree_view_column_new_with_attributes(_("visible"), renderer,
                                                    "active", DT_METADATA_PREF_COL_VISIBLE, NULL);
  gtk_tree_view_append_column(GTK_TREE_VIEW(view), column);

  // drag & drop
  gtk_tree_view_set_reorderable(GTK_TREE_VIEW(view), TRUE);
  g_signal_connect(G_OBJECT(model), "row-inserted", G_CALLBACK(_drag_data_inserted), NULL);

  gtk_container_add(GTK_CONTAINER(w), view);

#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(dialog);
#endif
  gtk_widget_show_all(dialog);

  int i = 0;
  if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_YES)
  {
    g_object_ref(d->filter_model);
    gtk_tree_view_set_model(GTK_TREE_VIEW(d->view), NULL);
    GtkTreeIter iter;
    gboolean valid = gtk_tree_model_get_iter_first(model, &iter);
    while(valid)
    {
      gboolean visible;
      uint32_t index;
      gtk_tree_model_get(model, &iter,
                         DT_METADATA_PREF_COL_INDEX, &index,
                         DT_METADATA_PREF_COL_VISIBLE, &visible,
                         -1);
      GtkTreeIter mv_iter;
      _get_index_iter(index , d->model, &mv_iter);

      gtk_list_store_set(GTK_LIST_STORE(d->model), &mv_iter,
                         DT_METADATA_COL_ORDER, i,
                         DT_METADATA_COL_VISIBLE, visible,
                         -1);
      valid = gtk_tree_model_iter_next(model, &iter);
      i++;
    }
    gtk_tree_view_set_model(GTK_TREE_VIEW(d->view), d->filter_model);
    g_object_unref(d->filter_model);
    _save_preferences(self);
  }
  gtk_widget_destroy(dialog);
}

void set_preferences(void *menu, dt_lib_module_t *self)
{
  GtkWidget *mi = gtk_menu_item_new_with_label(_("preferences..."));
  g_signal_connect(G_OBJECT(mi), "activate", G_CALLBACK(_menuitem_preferences), self);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
}

void init_presets(dt_lib_module_t *self)
{
}

void *get_params(dt_lib_module_t *self, int *size)
{
  *size = 0;
  char *params = _get_current_configuration(self);
  if(params)
    *size =strlen(params) + 1;
  return params;
}

int set_params(dt_lib_module_t *self, const void *params, int size)
{
  if(!params) return 1;

  _apply_preferences(params, self);
  _save_preferences(self);
  return 0;
}

static void _set_ellipsize(GtkTreeViewColumn *col, GtkCellRenderer *renderer,
                           GtkTreeModel *model, GtkTreeIter *iter, gpointer data)
{
  int i;
  gtk_tree_model_get(model, iter, DT_METADATA_COL_INDEX, &i, -1);
  const int ellipsize = i == md_exif_model || i == md_exif_lens || i == md_exif_maker
                        ? PANGO_ELLIPSIZE_END
                        : PANGO_ELLIPSIZE_MIDDLE;
  g_object_set(renderer, "ellipsize", ellipsize, NULL);
}

static gboolean _view_redraw(GtkWidget *view, cairo_t *cr, dt_lib_module_t *self)
{
  GtkTreeViewColumn *col0 = gtk_tree_view_get_column(GTK_TREE_VIEW(view), 0);
  GtkTreeViewColumn *col1 = gtk_tree_view_get_column(GTK_TREE_VIEW(view), 1);
  const int width0 = gtk_tree_view_column_get_width(col0);
  const int width1 = gtk_tree_view_column_get_width(col1);
  // keep 1/3-2/3 ratio
  const int w0 = (width0 + width1) / 3;
  // strange. The logic would be to apply it on col0, but only works the other way
  gtk_tree_view_column_set_fixed_width(col1, w0);
  return FALSE;
}

void gui_init(dt_lib_module_t *self)
{
  /* initialize ui */
  dt_lib_metadata_view_t *d = (dt_lib_metadata_view_t *)g_malloc0(sizeof(dt_lib_metadata_view_t));
  self->data = (void *)d;

  GtkListStore *store = gtk_list_store_new(DT_METADATA_NUM_COLS,
                                           G_TYPE_INT,      // index
                                           G_TYPE_STRING,  // name
                                           G_TYPE_STRING,  // displayed name
                                           G_TYPE_STRING,  // tooltip
                                           G_TYPE_STRING,  // value
                                           G_TYPE_BOOLEAN, // visibility
                                           G_TYPE_INT      // order
                                           );
  d->model = GTK_TREE_MODEL(store);

  _lib_metadata_view_init_labels(d->model);

  GtkTreeModel *sort_model = gtk_tree_model_sort_new_with_model(d->model);
  d->sort_model = sort_model;

  gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(sort_model),
                                      DT_METADATA_COL_ORDER,
                                      GTK_SORT_ASCENDING);
  GtkTreeModel *filter_model = gtk_tree_model_filter_new(sort_model, NULL);
  d->filter_model = filter_model;
  gtk_tree_model_filter_set_visible_column(GTK_TREE_MODEL_FILTER(filter_model),
                                           DT_METADATA_COL_VISIBLE);

  GtkWidget *view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(filter_model));
  d->view = view;
  gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(view), FALSE);
  gtk_widget_set_name(view, "image-infos");
  g_signal_connect(G_OBJECT(view), "draw", G_CALLBACK(_view_redraw), self);
  g_object_unref(filter_model);

  // metadata column
  GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
  GtkTreeViewColumn *column = gtk_tree_view_column_new_with_attributes(_("name"), renderer,
                                        "text", DT_METADATA_COL_NAME_L, NULL);
  g_object_set(renderer, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
  gtk_tree_view_column_set_expand(column, TRUE);
  gtk_tree_view_append_column(GTK_TREE_VIEW(view), column);

  // value column
  renderer = gtk_cell_renderer_text_new();
  g_object_set(renderer, "xalign", 0.0, NULL);
  // FIXME
  //  gtk_widget_set_name(GTK_WIDGET(d->metadata[k]), "brightbg");
  column = gtk_tree_view_column_new_with_attributes(_("value"), renderer,
                                        "text", DT_METADATA_COL_VALUE, NULL);
  gtk_tree_view_column_set_expand(column, TRUE);
  gtk_tree_view_column_set_cell_data_func(column, renderer, _set_ellipsize, self, NULL);
  gtk_tree_view_append_column(GTK_TREE_VIEW(view), column);

  // film roll jump to:
  g_signal_connect(G_OBJECT(view), "button-press-event", G_CALLBACK(_filmroll_clicked), NULL);

  g_object_set(G_OBJECT(view), "has-tooltip", TRUE, NULL);
  g_signal_connect(G_OBJECT(view), "query-tooltip", G_CALLBACK(_row_tooltip_setup), self);

  self->widget = dt_ui_scroll_wrap(view, 100, "plugins/lighttable/metadata_view/windowheight");

  dt_gui_add_help_link(self->widget, dt_get_help_url(self->plugin_name));

  char *pref = dt_conf_get_string("plugins/lighttable/metadata_view/visible");
  _apply_preferences(pref, self);
  g_free(pref);

  /* lets signup for mouse over image change signals */
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_MOUSE_OVER_IMAGE_CHANGE,
                            G_CALLBACK(_mouse_over_image_callback), self);

  /* lets signup for develop image changed signals */
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_DEVELOP_IMAGE_CHANGED,
                            G_CALLBACK(_mouse_over_image_callback), self);

  /* signup for develop initialize to update info of current
     image in darkroom when enter */
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_DEVELOP_INITIALIZE,
                            G_CALLBACK(_mouse_over_image_callback), self);

  /* signup for tags changes */
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_TAG_CHANGED,
                            G_CALLBACK(_mouse_over_image_callback), self);

  /* signup for metadata changes */
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_METADATA_UPDATE,
                            G_CALLBACK(_mouse_over_image_callback), self);
}

void gui_cleanup(dt_lib_module_t *self)
{
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_mouse_over_image_callback), self);
  g_free(self->data);
  self->data = NULL;
}

void gui_reset(dt_lib_module_t *self)
{
  dt_lib_metadata_view_t *d = (dt_lib_metadata_view_t *)self->data;
  GtkTreeIter iter;
  gboolean valid = gtk_tree_model_get_iter_first(d->model, &iter);
  int i = 0;
  while(valid)
  {
    gtk_list_store_set(GTK_LIST_STORE(d->model), &iter,
                       DT_METADATA_COL_ORDER, i,
                       DT_METADATA_COL_VISIBLE, TRUE,
                       -1);
    valid = gtk_tree_model_iter_next(d->model, &iter);
    i++;
  }
  _save_preferences(self);
}

#ifdef USE_LUA
static int lua_update_values(lua_State*L)
{
  dt_lib_module_t *self = lua_touserdata(L, 1);
  dt_lib_metadata_view_t *d = (dt_lib_metadata_view_t *)self->data;
  dt_lua_module_entry_push(L,"lib",self->plugin_name);
  lua_getuservalue(L,2);
  lua_getfield(L,3,"values");
  lua_getfield(L,3,"indexes");
  lua_pushnil(L);
  while(lua_next(L, 4) != 0)
  {
    lua_getfield(L,5,lua_tostring(L,-2));
    int index = lua_tointeger(L,-1);
    _metadata_update_value(index,luaL_checkstring(L,7),d->model);
    lua_pop(L,2);
  }
  return 0;
}
static int lua_update_metadata(lua_State*L)
{
  dt_lib_module_t *self = lua_touserdata(L, 1);
  int32_t imgid = lua_tointeger(L,2);
  dt_lua_module_entry_push(L,"lib",self->plugin_name);
  lua_getuservalue(L,-1);
  lua_getfield(L,4,"callbacks");
  lua_getfield(L,4,"values");
  lua_pushnil(L);
  while(lua_next(L, 5) != 0)
  {
    lua_pushvalue(L,-1);
    luaA_push(L,dt_lua_image_t,&imgid);
    lua_call(L,1,1);
    lua_pushvalue(L,7);
    lua_pushvalue(L,9);
    lua_settable(L,6);
    lua_pop(L, 2);
  }
  lua_pushcfunction(L,lua_update_values);
  dt_lua_gtk_wrap(L);
  lua_pushlightuserdata(L,self);
  lua_call(L,1,0);
  return 0;
}

static int lua_register_info(lua_State *L)
{
  dt_lib_module_t *self = lua_touserdata(L, lua_upvalueindex(1));
  dt_lua_module_entry_push(L,"lib",self->plugin_name);
  lua_getuservalue(L,-1);
  const char* key = luaL_checkstring(L,1);
  luaL_checktype(L,2,LUA_TFUNCTION);
  {
    lua_getfield(L,-1,"callbacks");
    lua_pushstring(L,key);
    lua_pushvalue(L,2);
    lua_settable(L,5);
    lua_pop(L,1);
  }
  {
    lua_getfield(L,-1,"values");
    lua_pushstring(L,key);
    lua_pushstring(L,"-");
    lua_settable(L,5);
    lua_pop(L,1);
  }
  {
    GtkTreeIter iter;
    dt_lib_metadata_view_t *d = (dt_lib_metadata_view_t *)self->data;
    gtk_list_store_append(GTK_LIST_STORE(d->model), &iter);
    // get the row index
    GtkTreePath *path = gtk_tree_model_get_path(d->model, &iter);
    gint *i = gtk_tree_path_get_indices(path);
    const int index = i[0];
    gtk_tree_path_free(path);
    gtk_list_store_set(GTK_LIST_STORE(d->model), &iter,
                       DT_METADATA_COL_INDEX, index,
                       DT_METADATA_COL_NAME, key,
                       DT_METADATA_COL_NAME_L, key,
                       DT_METADATA_COL_VALUE, "-",
                       DT_METADATA_COL_VISIBLE, TRUE,
                       DT_METADATA_COL_ORDER, index,
                       -1);
    {
      lua_getfield(L,-1,"indexes");
      lua_pushstring(L,key);
      lua_pushinteger(L,index);
      lua_settable(L,5);
      lua_pop(L,1);
    }
    // apply again preferences because it's already done
    char *pref = dt_conf_get_string("plugins/lighttable/metadata_view/visible");
    _apply_preferences(pref, self);
    g_free(pref);
  }
  return 0;
}

void init(struct dt_lib_module_t *self)
{
  lua_State *L = darktable.lua_state.state;
  int my_type = dt_lua_module_entry_get_type(L, "lib", self->plugin_name);
  lua_pushlightuserdata(L, self);
  lua_pushcclosure(L, lua_register_info,1);
  dt_lua_gtk_wrap(L);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, my_type, "register_info");

  dt_lua_module_entry_push(L,"lib",self->plugin_name);
  lua_getuservalue(L,-1);
  lua_newtable(L);
  lua_setfield(L,-2,"callbacks");
  lua_newtable(L);
  lua_setfield(L,-2,"values");
  lua_newtable(L);
  lua_setfield(L,-2,"indexes");
  lua_pop(L,2);
}
#endif
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
