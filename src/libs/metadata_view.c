/*
    This file is part of darktable,
    copyright (c) 2011-2012 Henrik Andersson.

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
#include "control/conf.h"
#include "control/control.h"
#include "develop/develop.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include "libs/lib_api.h"

#include <gdk/gdkkeysyms.h>
#include <sys/param.h>
#ifdef USE_LUA
#include "lua/call.h"
#include "lua/image.h"
#endif

#define SHOW_FLAGS 1

DT_MODULE(1)

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
#if SHOW_FLAGS
  md_internal_flags,
#endif

  /* exif */
  md_exif_model,
  md_exif_maker,
  md_exif_lens,
  md_exif_aperture,
  md_exif_exposure,
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
  md_xmp_title,
  md_xmp_creator,
  md_xmp_rights,

  /* geotagging */
  md_geotagging_lat,
  md_geotagging_lon,
  md_geotagging_ele,

  /* entries, do not touch! */
  md_size
};

static gchar *_md_labels[md_size];

/* initialize the labels text */
static void _lib_metatdata_view_init_labels()
{
  /* internal */
  _md_labels[md_internal_filmroll] = _("filmroll");
  _md_labels[md_internal_imgid] = _("image id");
  _md_labels[md_internal_groupid] = _("group id");
  _md_labels[md_internal_filename] = _("filename");
  _md_labels[md_internal_version] = _("version");
  _md_labels[md_internal_fullpath] = _("full path");
  _md_labels[md_internal_local_copy] = _("local copy");
#if SHOW_FLAGS
  _md_labels[md_internal_flags] = _("flags");
#endif

  /* exif */
  _md_labels[md_exif_model] = _("model");
  _md_labels[md_exif_maker] = _("maker");
  _md_labels[md_exif_lens] = _("lens");
  _md_labels[md_exif_aperture] = _("aperture");
  _md_labels[md_exif_exposure] = _("exposure");
  _md_labels[md_exif_focal_length] = _("focal length");
  _md_labels[md_exif_focus_distance] = _("focus distance");
  _md_labels[md_exif_iso] = _("ISO");
  _md_labels[md_exif_datetime] = _("datetime");
  _md_labels[md_exif_width] = _("width");
  _md_labels[md_exif_height] = _("height");

  _md_labels[md_width] = _("export width");
  _md_labels[md_height] = _("export height");

  /* xmp */
  _md_labels[md_xmp_title] = _("title");
  _md_labels[md_xmp_creator] = _("creator");
  _md_labels[md_xmp_rights] = _("copyright");

  /* geotagging */
  _md_labels[md_geotagging_lat] = _("latitude");
  _md_labels[md_geotagging_lon] = _("longitude");
  _md_labels[md_geotagging_ele] = _("elevation");
}


typedef struct dt_lib_metadata_view_t
{
  GtkLabel *metadata[md_size];
} dt_lib_metadata_view_t;

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

/* helper which eliminates non-printable characters from a string

Strings which are already in valid UTF-8 are retained.
*/
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

/* helper function for updating a metadata value */
static void _metadata_update_value(GtkLabel *label, const char *value)
{
  gboolean validated = g_utf8_validate(value, -1, NULL);
  const gchar *str = validated ? value : NODATA_STRING;
  gtk_label_set_text(GTK_LABEL(label), str);
  gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_MIDDLE);
  gtk_widget_set_tooltip_text(GTK_WIDGET(label), str);
}

static void _metadata_update_value_end(GtkLabel *label, const char *value)
{
  gboolean validated = g_utf8_validate(value, -1, NULL);
  const gchar *str = validated ? value : NODATA_STRING;
  gtk_label_set_text(GTK_LABEL(label), str);
  gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
  gtk_widget_set_tooltip_text(GTK_WIDGET(label), str);
}


#ifdef USE_LUA
static int lua_update_metadata(lua_State*L);
#endif
/* update all values to reflect mouse over image id or no data at all */
static void _metadata_view_update_values(dt_lib_module_t *self)
{
  dt_lib_metadata_view_t *d = (dt_lib_metadata_view_t *)self->data;
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

    // get the size before locking the image!
    // TODO: put that into dt_image_t and make sure it stays in sync
    int width = 0, height = 0;
//     dt_image_get_final_size(mouse_over_id, &width, &height); // kind of slow on some machines

    const dt_image_t *img = dt_image_cache_get(darktable.image_cache, mouse_over_id, 'r');
    if(!img) goto fill_minuses;
    if(img->film_id == -1)
    {
      dt_image_cache_read_release(darktable.image_cache, img);
      goto fill_minuses;
    }

    /* update all metadata */

    dt_image_film_roll(img, value, sizeof(value));
    _metadata_update_value(d->metadata[md_internal_filmroll], value);

    char tooltip[512];
    snprintf(tooltip, sizeof(tooltip), _("double click to jump to film roll\n%s"), value);
    gtk_widget_set_tooltip_text(GTK_WIDGET(d->metadata[md_internal_filmroll]), tooltip);

    snprintf(value, sizeof(value), "%d", img->id);
    _metadata_update_value(d->metadata[md_internal_imgid], value);

    snprintf(value, sizeof(value), "%d", img->group_id);
    _metadata_update_value(d->metadata[md_internal_groupid], value);

    _metadata_update_value(d->metadata[md_internal_filename], img->filename);

    snprintf(value, sizeof(value), "%d", img->version);
    _metadata_update_value(d->metadata[md_internal_version], value);

    gboolean from_cache = FALSE;
    dt_image_full_path(img->id, pathname, sizeof(pathname), &from_cache);
    _metadata_update_value(d->metadata[md_internal_fullpath], pathname);

    snprintf(value, sizeof(value), "%s", (img->flags & DT_IMAGE_LOCAL_COPY) ? _("yes") : _("no"));
    _metadata_update_value(d->metadata[md_internal_local_copy], value);

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
                                    N_("has .wav")
      };
      char *tooltip_parts[14] = { 0 };
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
      };

      int loader = (unsigned int)img->loader < sizeof(loaders) / sizeof(*loaders) ? img->loader : 0;
      value[12] = loaders[loader].flag;
      char *loader_tooltip = g_strdup_printf(_("loader: %s"), _(loaders[loader].tooltip));
      tooltip_parts[next_tooltip_part++] = loader_tooltip;

      value[13] = '\0';

      flags_tooltip = g_strjoinv("\n", tooltip_parts);
      g_free(loader_tooltip);

      _metadata_update_value(d->metadata[md_internal_flags], value);
      gtk_widget_set_tooltip_text(GTK_WIDGET(d->metadata[md_internal_flags]), flags_tooltip);

      g_free(star_string);
      g_free(flags_tooltip);

      #undef EMPTY_FIELD
      #undef FALSE_FIELD
      #undef TRUE_FIELD
    }
#endif // SHOW_FLAGS

    /* EXIF */
    _metadata_update_value_end(d->metadata[md_exif_model], img->camera_alias);
    _metadata_update_value_end(d->metadata[md_exif_lens], img->exif_lens);
    _metadata_update_value_end(d->metadata[md_exif_maker], img->camera_maker);

    snprintf(value, sizeof(value), "F/%.1f", img->exif_aperture);
    _metadata_update_value(d->metadata[md_exif_aperture], value);

    if(img->exif_exposure <= 0.5)
      snprintf(value, sizeof(value), "1/%.0f", 1.0 / img->exif_exposure);
    else
      snprintf(value, sizeof(value), "%.1f''", img->exif_exposure);
    _metadata_update_value(d->metadata[md_exif_exposure], value);

    snprintf(value, sizeof(value), "%.0f mm", img->exif_focal_length);
    _metadata_update_value(d->metadata[md_exif_focal_length], value);

    if(isnan(img->exif_focus_distance) || fpclassify(img->exif_focus_distance) == FP_ZERO)
    {
      _metadata_update_value(d->metadata[md_exif_focus_distance], NODATA_STRING);
    }
    else
    {
      snprintf(value, sizeof(value), "%.2f m", img->exif_focus_distance);
      _metadata_update_value(d->metadata[md_exif_focus_distance], value);
    }

    snprintf(value, sizeof(value), "%.0f", img->exif_iso);
    _metadata_update_value(d->metadata[md_exif_iso], value);

    struct tm tt_exif = { 0 };
    if(sscanf(img->exif_datetime_taken, "%d:%d:%d %d:%d:%d", &tt_exif.tm_year, &tt_exif.tm_mon,
      &tt_exif.tm_mday, &tt_exif.tm_hour, &tt_exif.tm_min, &tt_exif.tm_sec) == 6)
    {
      char datetime[200];
      tt_exif.tm_year -= 1900;
      tt_exif.tm_mon--;
      tt_exif.tm_isdst = -1;
      mktime(&tt_exif);
      // just %c is too long and includes a time zone that we don't know from exif
      strftime(datetime, sizeof(datetime), "%a %x %X", &tt_exif);
      _metadata_update_value(d->metadata[md_exif_datetime], datetime);
    }
    else
      _metadata_update_value(d->metadata[md_exif_datetime], img->exif_datetime_taken);

    snprintf(value, sizeof(value), "%d", img->height);
    _metadata_update_value(d->metadata[md_exif_height], value);
    snprintf(value, sizeof(value), "%d", img->width);
    _metadata_update_value(d->metadata[md_exif_width], value);

    snprintf(value, sizeof(value), "%d", height);
    _metadata_update_value(d->metadata[md_height], value);
    snprintf(value, sizeof(value), "%d", width);
    _metadata_update_value(d->metadata[md_width], value);

    /* XMP */
    GList *res;
    if((res = dt_metadata_get(img->id, "Xmp.dc.title", NULL)) != NULL)
    {
      snprintf(value, sizeof(value), "%s", (char *)res->data);
      _filter_non_printable(value, sizeof(value));
      g_list_free_full(res, &g_free);
    }
    else
      g_strlcpy(value, NODATA_STRING, sizeof(value));
    _metadata_update_value(d->metadata[md_xmp_title], value);

    if((res = dt_metadata_get(img->id, "Xmp.dc.creator", NULL)) != NULL)
    {
      snprintf(value, sizeof(value), "%s", (char *)res->data);
      _filter_non_printable(value, sizeof(value));
      g_list_free_full(res, &g_free);
    }
    else
      g_strlcpy(value, NODATA_STRING, sizeof(value));
    _metadata_update_value(d->metadata[md_xmp_creator], value);

    if((res = dt_metadata_get(img->id, "Xmp.dc.rights", NULL)) != NULL)
    {
      snprintf(value, sizeof(value), "%s", (char *)res->data);
      _filter_non_printable(value, sizeof(value));
      g_list_free_full(res, &g_free);
    }
    else
      g_strlcpy(value, NODATA_STRING, sizeof(value));
    _metadata_update_value(d->metadata[md_xmp_rights], value);

    /* geotagging */
    /* latitude */
    if(isnan(img->latitude))
    {
      _metadata_update_value(d->metadata[md_geotagging_lat], NODATA_STRING);
    }
    else
    {
      if(dt_conf_get_bool("plugins/lighttable/metadata_view/pretty_location"))
      {
        gchar *latitude = dt_util_latitude_str(img->latitude);
        _metadata_update_value(d->metadata[md_geotagging_lat], latitude);
        g_free(latitude);
      }
      else
      {
        gchar NS = img->latitude < 0 ? 'S' : 'N';
        snprintf(value, sizeof(value), "%c %09.6f", NS, fabs(img->latitude));
        _metadata_update_value(d->metadata[md_geotagging_lat], value);
      }
    }
    /* longitude */
    if(isnan(img->longitude))
    {
      _metadata_update_value(d->metadata[md_geotagging_lon], NODATA_STRING);
    }
    else
    {
      if(dt_conf_get_bool("plugins/lighttable/metadata_view/pretty_location"))
      {
        gchar *longitude = dt_util_longitude_str(img->longitude);
        _metadata_update_value(d->metadata[md_geotagging_lon], longitude);
        g_free(longitude);
      }
      else
      {
        gchar EW = img->longitude < 0 ? 'W' : 'E';
        snprintf(value, sizeof(value), "%c %010.6f", EW, fabs(img->longitude));
        _metadata_update_value(d->metadata[md_geotagging_lon], value);
      }
    }
    /* elevation */
    if(isnan(img->elevation))
    {
      _metadata_update_value(d->metadata[md_geotagging_ele], NODATA_STRING);
    }
    else
    {
      if(dt_conf_get_bool("plugins/lighttable/metadata_view/pretty_location"))
      {
        gchar *elevation = dt_util_elevation_str(img->elevation);
        _metadata_update_value(d->metadata[md_geotagging_ele], elevation);
        g_free(elevation);
      }
      else
      {
        snprintf(value, sizeof(value), "%.2f %s", img->elevation, _("m"));
        _metadata_update_value(d->metadata[md_geotagging_ele], value);
      }
    }

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
  for(int k = 0; k < md_size; k++) _metadata_update_value(d->metadata[k], NODATA_STRING);
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

static gboolean _filmroll_clicked(GtkWidget *widget, GdkEventButton *event, gpointer null)
{
  if(event->type != GDK_2BUTTON_PRESS) return FALSE;
  _jump_to();
  return TRUE;
}

static gboolean _jump_to_accel(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                               GdkModifierType modifier, gpointer data)
{
  _jump_to();
  return TRUE;
}

/* calback for the mouse over image change signal */
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

void gui_init(dt_lib_module_t *self)
{
  /* initialize ui widgets */
  dt_lib_metadata_view_t *d = (dt_lib_metadata_view_t *)g_malloc0(sizeof(dt_lib_metadata_view_t));
  self->data = (void *)d;
  _lib_metatdata_view_init_labels();

  self->widget = gtk_grid_new();
  dt_gui_add_help_link(self->widget, dt_get_help_url(self->plugin_name));
  gtk_grid_set_column_spacing(GTK_GRID(self->widget), DT_PIXEL_APPLY_DPI(5));
//   GtkWidget *last = NULL;

  /* initialize the metadata name/value labels */
  for(int k = 0; k < md_size; k++)
  {
    GtkWidget *evb = gtk_event_box_new();
    gtk_widget_set_name(evb, "brightbg");
    GtkLabel *name = GTK_LABEL(gtk_label_new(_md_labels[k]));
    d->metadata[k] = GTK_LABEL(gtk_label_new("-"));
    gtk_label_set_selectable(d->metadata[k], TRUE);
    gtk_container_add(GTK_CONTAINER(evb), GTK_WIDGET(d->metadata[k]));
    if(k == md_internal_filmroll)
    {
      // film roll jump to:
      g_signal_connect(G_OBJECT(evb), "button-press-event", G_CALLBACK(_filmroll_clicked), NULL);
    }
    gtk_widget_set_halign(GTK_WIDGET(name), GTK_ALIGN_START);
    gtk_widget_set_halign(GTK_WIDGET(d->metadata[k]), GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(self->widget), GTK_WIDGET(name), 0, k, 1, 1);
    gtk_grid_attach_next_to(GTK_GRID(self->widget), GTK_WIDGET(evb), GTK_WIDGET(name), GTK_POS_RIGHT, 1, 1);
  }

  /* lets signup for mouse over image change signals */
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_MOUSE_OVER_IMAGE_CHANGE,
                            G_CALLBACK(_mouse_over_image_callback), self);

  /* lets signup for develop image changed signals */
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_DEVELOP_IMAGE_CHANGED,
                            G_CALLBACK(_mouse_over_image_callback), self);

  /* signup for develop initialize to update info of current
     image in darkroom when enter */
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_DEVELOP_INITIALIZE,
                            G_CALLBACK(_mouse_over_image_callback), self);
}

void gui_cleanup(dt_lib_module_t *self)
{
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(_mouse_over_image_callback), self);
  g_free(self->data);
  self->data = NULL;
}
#ifdef USE_LUA
static int lua_update_widgets(lua_State*L)
{
  dt_lib_module_t *self = lua_touserdata(L, 1);
  dt_lua_module_entry_push(L,"lib",self->plugin_name);
  lua_getuservalue(L,2);
  lua_getfield(L,3,"values");
  lua_getfield(L,3,"widgets");
  lua_pushnil(L);
  while(lua_next(L, 4) != 0)
  {
    lua_getfield(L,5,lua_tostring(L,-2));
    GtkLabel *widget = lua_touserdata(L,-1);
    _metadata_update_value_end(widget,luaL_checkstring(L,7));
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
  lua_pushcfunction(L,lua_update_widgets);
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
    GtkWidget *evb = gtk_event_box_new();
    gtk_widget_set_name(evb, "brightbg");
    GtkLabel *name = GTK_LABEL(gtk_label_new(key));
    GtkLabel *value = GTK_LABEL(gtk_label_new("-"));
    gtk_label_set_selectable(value, TRUE);
    gtk_container_add(GTK_CONTAINER(evb), GTK_WIDGET(value));
    gtk_widget_set_halign(GTK_WIDGET(name), GTK_ALIGN_START);
    gtk_widget_set_halign(GTK_WIDGET(value), GTK_ALIGN_START);
    gtk_grid_attach_next_to(GTK_GRID(self->widget), GTK_WIDGET(name), NULL, GTK_POS_BOTTOM, 1, 1);
    gtk_grid_attach_next_to(GTK_GRID(self->widget), GTK_WIDGET(evb), GTK_WIDGET(name), GTK_POS_RIGHT, 1, 1);
    gtk_widget_show_all(self->widget);
    {
      lua_getfield(L,-1,"widgets");
      lua_pushstring(L,key);
      lua_pushlightuserdata(L,value);
      lua_settable(L,5);
      lua_pop(L,1);
    }
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
  lua_setfield(L,-2,"widgets");
  lua_pop(L,2);
}
#endif
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
