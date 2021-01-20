/*
    This file is part of darktable,
    Copyright (C) 2012-2020 darktable developers.

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
// #include "common/darktable.h"
// #include "control/control.h"
#include "common/debug.h"
#include "common/file_location.h"
#include "common/image_cache.h"
#include "common/collection.h"
#include "control/conf.h"
// #include "gui/accelerators.h"
// #include "gui/gtk.h"
#include "dtgtk/button.h"
// #include "dtgtk/paint.h"
// #include "libs/lib.h"
#include "control/jobs.h"
#include "libs/lib_api.h"
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif

#include <gdk/gdkkeysyms.h>

DT_MODULE(1)

typedef struct tz_tuple_t
{
  char *name, *display;
} tz_tuple_t;

typedef struct dt_lib_geotagging_t
{
  GtkWidget *datetime;
  GtkWidget *datetime0;
  GtkWidget *offset;
  int offset_int;
  char *dt0_text;
  time_t dt0_int;
  GList *timezones;
  GtkWidget *floating_window, *floating_window_ok, *floating_window_cancel, *floating_window_entry;
} dt_lib_geotagging_t;

static void free_tz_tuple(gpointer data)
{
  tz_tuple_t *tz_tuple = (tz_tuple_t *)data;
  g_free(tz_tuple->display);
#ifdef _WIN32
  g_free(tz_tuple->name); // on non-Windows both point to the same string
#endif
  free(tz_tuple);
}

const char *name(dt_lib_module_t *self)
{
  return _("geotagging");
}

const char **views(dt_lib_module_t *self)
{
  static const char *v[] = {"lighttable", NULL};
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_RIGHT_CENTER;
}


int position()
{
  return 450;
}

static time_t _get_datetime_from_string(const char *value)
{
  struct tm dt = {0};
  time_t datetime = -1;
  if(sscanf(value,"%d:%d:%d %d:%d:%d",
    &dt.tm_year, &dt.tm_mon, &dt.tm_mday, &dt.tm_hour, &dt.tm_min, &dt.tm_sec) == 6)
  {
    dt.tm_year -= 1900;
    dt.tm_mon--;
    dt.tm_isdst = -1;    // no daylight saving time
    datetime = mktime(&dt);
  }
  return datetime;
}

static void _get_datetime_offset_string(const time_t offset_int, char *offset_text)
{
  printf("_get_datetime_offset_string %ld\n", offset_int);
  const gboolean neg = offset_int < 0;
  time_t off = neg ? - offset_int : offset_int;
  time_t off2 = off / 60;
  const int sec = off - off2 * 60;
  off = off2;
  off2 = off / 60;
  const int min = off - off2 * 60;
  off = off2;
  off2 = off / 60;
  const int hou = off - off2 * 60;
  off = off2;
  off2 = off / 24;
  const int day = off - off2 * 24;
  printf("%s%d %d:%d:%d\n", neg ? "-" : "", day, hou, min, sec);
  snprintf(offset_text, 21, "        %s%02d %02d:%02d:%02d", neg ? "-" : " ", day, hou, min, sec);
}

static void _datetime_changed(GtkWidget *entry, dt_lib_module_t *self)
{
  dt_lib_geotagging_t *d = (dt_lib_geotagging_t *)self->data;
  const gchar *value = gtk_entry_get_text(GTK_ENTRY(d->datetime));
  printf("_datetime_changed %s\n", value);
  time_t new_dt = _get_datetime_from_string(value);
  if(new_dt != -1)
  {
    d->offset_int = new_dt - d->dt0_int;
    char offset_text[21];
    _get_datetime_offset_string(d->offset_int, offset_text);
    gtk_label_set_text(GTK_LABEL(d->offset), offset_text);
  }
  else
  {
    gtk_label_set_text(GTK_LABEL(d->offset), "XX XX:XX:XX");
  }
}

// modify the datetime_taken field in the db/cache
static void _apply_offset_callback(GtkWidget *widget, dt_lib_module_t *self)
{
  dt_lib_geotagging_t *d = (dt_lib_geotagging_t *)self->data;
  const char *offset_text = gtk_label_get_text(GTK_LABEL(d->offset));
  if(g_strcmp0(offset_text, "XX XX:XX:XX"))
    dt_control_time_offset(d->offset_int, -1);
}

static gboolean _lib_geotagging_filter_gpx(const GtkFileFilterInfo *filter_info, gpointer data)
{
  if(!g_ascii_strcasecmp(filter_info->mime_type, "application/gpx+xml")) return TRUE;

  const gchar *filename = filter_info->filename;
  const char *cc = filename + strlen(filename);
  for(; *cc != '.' && cc > filename; cc--)
    ;

  if(!g_ascii_strcasecmp(cc, ".gpx")) return TRUE;

  return FALSE;
}

static void _lib_geotagging_gpx_callback(GtkWidget *widget, dt_lib_module_t *self)
{
  dt_lib_geotagging_t *d = (dt_lib_geotagging_t *)self->data;
  /* bring a filechooser to select the gpx file to apply to selection */
  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  GtkWidget *filechooser = gtk_file_chooser_dialog_new(
      _("open GPX file"), GTK_WINDOW(win), GTK_FILE_CHOOSER_ACTION_OPEN, _("_cancel"), GTK_RESPONSE_CANCEL,
      _("_open"), GTK_RESPONSE_ACCEPT, (char *)NULL);
#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(filechooser);
#endif

  char *last_directory = dt_conf_get_string("ui_last/gpx_last_directory");
  if(last_directory != NULL)
  {
    gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(filechooser), last_directory);
    g_free(last_directory);
  }

  GtkFileFilter *filter;
  filter = GTK_FILE_FILTER(gtk_file_filter_new());
  gtk_file_filter_add_custom(filter, GTK_FILE_FILTER_FILENAME | GTK_FILE_FILTER_MIME_TYPE,
                             _lib_geotagging_filter_gpx, NULL, NULL);
  gtk_file_filter_set_name(filter, _("GPS data exchange format"));
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(filechooser), filter);

  filter = GTK_FILE_FILTER(gtk_file_filter_new());
  gtk_file_filter_add_pattern(filter, "*");
  gtk_file_filter_set_name(filter, _("all files"));
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(filechooser), filter);

  // add time zone selection
  GtkWidget *extra_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *label = gtk_label_new(_("camera time zone"));
  gtk_widget_set_tooltip_text(label, _("most cameras don't store the time zone in EXIF. "
                                       "give the correct time zone so the GPX data can be correctly matched"));

  GtkCellRenderer *renderer;
  GtkTreeIter tree_iter;
  GtkListStore *model = gtk_list_store_new(2, G_TYPE_STRING /*display*/, G_TYPE_STRING /*name*/);

  GtkWidget *tz_selection = gtk_combo_box_new_with_model(GTK_TREE_MODEL(model));
  renderer = gtk_cell_renderer_text_new();
  gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(tz_selection), renderer, FALSE);
  gtk_cell_layout_set_attributes(GTK_CELL_LAYOUT(tz_selection), renderer, "text", 0, NULL);

  int i = 0;
  gchar *old_tz = dt_conf_get_string("plugins/lighttable/geotagging/tz");
  for(GList *iter = d->timezones; iter; iter = g_list_next(iter))
  {
    tz_tuple_t *tz_tuple = (tz_tuple_t *)iter->data;
    gtk_list_store_append(model, &tree_iter);
    gtk_list_store_set(model, &tree_iter, 0, tz_tuple->display, 1, tz_tuple->name, -1);
    if(i == 0 || !strcmp(tz_tuple->name, old_tz)) gtk_combo_box_set_active(GTK_COMBO_BOX(tz_selection), i);
    i++;
  }
  g_free(old_tz);

  gtk_box_pack_start(GTK_BOX(extra_box), label, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(extra_box), tz_selection, FALSE, FALSE, 0);
  gtk_widget_show_all(extra_box);
  gtk_file_chooser_set_extra_widget(GTK_FILE_CHOOSER(filechooser), extra_box);

  if(gtk_dialog_run(GTK_DIALOG(filechooser)) == GTK_RESPONSE_ACCEPT)
  {
    gchar *folder = gtk_file_chooser_get_current_folder(GTK_FILE_CHOOSER(filechooser));
    dt_conf_set_string("ui_last/gpx_last_directory", folder);
    g_free(folder);

    gchar *tz;
    if(gtk_combo_box_get_active_iter(GTK_COMBO_BOX(tz_selection), &tree_iter) == TRUE)
    {
      GValue value = { 0, };
      gtk_tree_model_get_value(GTK_TREE_MODEL(model), &tree_iter, 1, &value);
      tz = g_strdup((gchar *)g_value_get_string(&value));
      g_value_unset(&value);
    }
    else
      tz = g_strdup("UTC");
    dt_conf_set_string("plugins/lighttable/geotagging/tz", tz);
    gchar *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(filechooser));
    dt_control_gpx_apply(filename, -1, tz);
    g_free(filename);
    g_free(tz);
  }

  g_object_unref(model);
  gtk_widget_destroy(extra_box);
  gtk_widget_destroy(filechooser);
  //   dt_control_queue_redraw_center();
}

static int _sort_timezones(gconstpointer a, gconstpointer b)
{
  const tz_tuple_t *tz_a = (tz_tuple_t *)a;
  const tz_tuple_t *tz_b = (tz_tuple_t *)b;

#ifdef _WIN32
  gboolean utc_neg_a = g_str_has_prefix(tz_a->display, "(UTC-");
  gboolean utc_neg_b = g_str_has_prefix(tz_b->display, "(UTC-");

  gboolean utc_pos_a = g_str_has_prefix(tz_a->display, "(UTC+");
  gboolean utc_pos_b = g_str_has_prefix(tz_b->display, "(UTC+");

  if(utc_neg_a && utc_neg_b)
  {
    char *iter_a = tz_a->display + strlen("(UTC-");
    char *iter_b = tz_b->display + strlen("(UTC-");

    while(((*iter_a >= '0' && *iter_a <= '9') || *iter_a == ':') &&
          ((*iter_b >= '0' && *iter_b <= '9') || *iter_b == ':'))
    {
      if(*iter_a != *iter_b) return *iter_b - *iter_a;
      iter_a++;
      iter_b++;
    }
  }
  else if(utc_neg_a && utc_pos_b) return -1;
  else if(utc_pos_a && utc_neg_b) return 1;
#endif

  return g_strcmp0(tz_a->display, tz_b->display);
}

// create a list of possible time zones
static GList *_lib_geotagging_get_timezones(void)
{
  GList *timezones = NULL;

#ifndef _WIN32
  // possible locations for zone.tab:
  // - /usr/share/zoneinfo
  // - /usr/lib/zoneinfo
  // - getenv("TZDIR")
  // - apparently on solaris there is no zones.tab. we need to collect the information ourselves like this:
  //   /bin/grep -h ^Zone /usr/share/lib/zoneinfo/src/* | /bin/awk '{print "??\t+9999+99999\t" $2}'
#define MAX_LINE_LENGTH 256
  FILE *fp;
  char line[MAX_LINE_LENGTH];

  // find the file using known possible locations
  gchar *zone_tab = g_strdup("/usr/share/zoneinfo/zone.tab");
  if(!g_file_test(zone_tab, G_FILE_TEST_IS_REGULAR))
  {
    g_free(zone_tab);
    zone_tab = g_strdup("/usr/lib/zoneinfo/zone.tab");
    if(!g_file_test(zone_tab, G_FILE_TEST_IS_REGULAR))
    {
      g_free(zone_tab);
      zone_tab = g_build_filename(g_getenv("TZDIR"), "zone.tab", NULL);
      if(!g_file_test(zone_tab, G_FILE_TEST_IS_REGULAR))
      {
        g_free(zone_tab);
        char datadir[PATH_MAX] = { 0 };
        dt_loc_get_datadir(datadir, sizeof(datadir));
        zone_tab = g_build_filename(datadir, "zone.tab", NULL);
        if(!g_file_test(zone_tab, G_FILE_TEST_IS_REGULAR))
        {
          g_free(zone_tab);
          // TODO: Solaris test
          return NULL;
        }
      }
    }
  }

  // parse zone.tab and put all time zone descriptions into timezones
  fp = g_fopen(zone_tab, "r");
  g_free(zone_tab);

  if(!fp) return NULL;

  while(fgets(line, MAX_LINE_LENGTH, fp))
  {
    if(line[0] == '#' || line[0] == '\0') continue;
    gchar **tokens = g_strsplit_set(line, " \t\n", 0);
    // sometimes files are not separated by single tabs but multiple spaces, resulting in empty strings in tokens
    // so we have to look for the 3rd non-empty entry
    int n_found = -1, i;
    for(i = 0; tokens[i] && n_found < 2; i++) if(*tokens[i]) n_found++;
    if(n_found != 2)
    {
      g_strfreev(tokens);
      continue;
    }
    gchar *name = g_strdup(tokens[i - 1]);
    g_strfreev(tokens);
    if(name[0] == '\0')
    {
      g_free(name);
      continue;
    }
    size_t last_char = strlen(name) - 1;
    if(name[last_char] == '\n') name[last_char] = '\0';
    tz_tuple_t *tz_tuple = (tz_tuple_t *)malloc(sizeof(tz_tuple_t));
    tz_tuple->display = name;
    tz_tuple->name = name;
    timezones = g_list_prepend(timezones, tz_tuple);
  }

  fclose(fp);

  // sort timezones
  timezones = g_list_sort(timezones, _sort_timezones);

  tz_tuple_t *utc = (tz_tuple_t *)malloc(sizeof(tz_tuple_t));
  utc->display = g_strdup("UTC");
  utc->name = utc->display;
  timezones = g_list_prepend(timezones, utc);

#undef MAX_LINE_LENGTH

#else // !_WIN32
  // on Windows we have to grab the time zones from the registry
  char *keypath = "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Time Zones\\";
  HKEY hKey;

  if(RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                   keypath,
                   0,
                   KEY_READ,
                   &hKey) == ERROR_SUCCESS)
  {
    DWORD n_subkeys, max_subkey_len;

    if(RegQueryInfoKey(hKey,
                       NULL,
                       NULL,
                       NULL,
                       &n_subkeys,
                       &max_subkey_len,
                       NULL,
                       NULL,
                       NULL,
                       NULL,
                       NULL,
                       NULL) == ERROR_SUCCESS)
    {
      wchar_t *subkeyname = (wchar_t *)malloc(sizeof(wchar_t) * (max_subkey_len + 1));

      for(DWORD i = 1; i < n_subkeys; i++)
      {
        DWORD subkeyname_length = max_subkey_len + 1;
        if(RegEnumKeyExW(hKey,
                         i,
                         subkeyname,
                         &subkeyname_length,
                         NULL,
                         NULL,
                         NULL,
                         NULL) == ERROR_SUCCESS)
        {
          DWORD buffer_size;
          char *subkeyname_utf8 = g_utf16_to_utf8(subkeyname, -1, NULL, NULL, NULL);
          char *subkeypath_utf8 = g_strconcat(keypath, "\\", subkeyname_utf8, NULL);
          wchar_t *subkeypath = g_utf8_to_utf16(subkeypath_utf8, -1, NULL, NULL, NULL);
          if(RegGetValueW(HKEY_LOCAL_MACHINE,
                          subkeypath,
                          L"Display",
                          RRF_RT_ANY,
                          NULL,
                          NULL,
                          &buffer_size) == ERROR_SUCCESS)
          {
            wchar_t *display_name = (wchar_t *)malloc(buffer_size);
            if(RegGetValueW(HKEY_LOCAL_MACHINE,
                            subkeypath,
                            L"Display",
                            RRF_RT_ANY,
                            NULL,
                            display_name,
                            &buffer_size) == ERROR_SUCCESS)
            {
              tz_tuple_t *tz = (tz_tuple_t *)malloc(sizeof(tz_tuple_t));

              tz->name = subkeyname_utf8;
              tz->display = g_utf16_to_utf8(display_name, -1, NULL, NULL, NULL);
              timezones = g_list_prepend(timezones, tz);

              subkeyname_utf8 = NULL; // to not free it later
            }
            free(display_name);
          }
          g_free(subkeyname_utf8);
          g_free(subkeypath_utf8);
          g_free(subkeypath);
        }
      }

      free(subkeyname);
    }
  }

  RegCloseKey(hKey);

  timezones = g_list_sort(timezones, _sort_timezones);
#endif // !_WIN32

  return timezones;
}

static void _set_datetime0(dt_lib_module_t *self)
{
  dt_lib_geotagging_t *d = (dt_lib_geotagging_t *)self->data;
  GList *selected = dt_collection_get_selected(darktable.collection, 1);
  const int imgid = !selected ? dt_view_get_image_to_act_on() : GPOINTER_TO_INT(selected->data);
  d->dt0_text = NULL;
  if(imgid > 0)
  {
    dt_image_t *img = dt_image_cache_get(darktable.image_cache, imgid, 'r');
    d->dt0_text = g_strdup(img->exif_datetime_taken);
    dt_image_cache_read_release(darktable.image_cache, img);
    d->dt0_int = _get_datetime_from_string(d->dt0_text);
  }
  else
  {
    d->dt0_text = g_strdup("");
    d->dt0_int = 0;
  }
}

static void _mouse_over_image_callback(gpointer instance, dt_lib_module_t *self)
{
  dt_lib_geotagging_t *d = (dt_lib_geotagging_t *)self->data;
  printf("_mouse_over_image_callback\n");
  _set_datetime0(self);
  gtk_label_set_text(GTK_LABEL(d->datetime0), d->dt0_text);
  gtk_entry_set_text(GTK_ENTRY(d->datetime), d->dt0_text);
}

void gui_init(dt_lib_module_t *self)
{
  dt_lib_geotagging_t *d = (dt_lib_geotagging_t *)malloc(sizeof(dt_lib_geotagging_t));
  self->data = (void *)d;
  d->timezones = _lib_geotagging_get_timezones();
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  dt_gui_add_help_link(self->widget, dt_get_help_url(self->plugin_name));

  GtkGrid *grid = GTK_GRID(gtk_grid_new());
  gtk_grid_set_column_spacing(grid, DT_PIXEL_APPLY_DPI(5));
  int line = 0;

  _set_datetime0(self);

  GtkWidget *label = gtk_label_new(_("new date time"));
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
  gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
  gtk_grid_attach(grid, label, 0, line, 1, 1);
  d->datetime = gtk_entry_new();
  dt_gui_key_accel_block_on_focus_connect(d->datetime);
  gtk_entry_set_alignment(GTK_ENTRY(d->datetime), 1.0);
  gtk_widget_set_halign(d->datetime, GTK_ALIGN_END);
  gtk_widget_set_hexpand(d->datetime, TRUE);
  gtk_grid_attach(grid, d->datetime, 1, line++, 1, 1);
  gtk_entry_set_text(GTK_ENTRY(d->datetime), d->dt0_text);

  label = gtk_label_new(_("original date time"));
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
  gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
  gtk_grid_attach(grid, label, 0, line, 1, 1);
  label = gtk_label_new(d->dt0_text);
  d->datetime0 = label;
  gtk_widget_set_halign(label, GTK_ALIGN_END);
  gtk_widget_set_hexpand(label, TRUE);
//  gtk_label_set_xalign(GTK_LABEL(label), 1.0f);
  gtk_grid_attach(grid, label, 1, line++, 1, 1);

  label = gtk_label_new(_("date time offset"));
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
  gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
  gtk_grid_attach(grid, label, 0, line, 1, 1);

  d->offset_int = 0;
  char offset_text[20];
  _get_datetime_offset_string(d->offset_int, offset_text);
  label = gtk_label_new(offset_text);
  gtk_widget_set_halign(label, GTK_ALIGN_END);
  gtk_widget_set_hexpand(label, TRUE);
//  gtk_label_set_xalign(GTK_LABEL(label), 1.0f);
  gtk_grid_attach(grid, label, 1, line++, 1, 1);
  d->offset = label;
  g_signal_connect(d->datetime, "changed", G_CALLBACK(_datetime_changed), self);

  // apply offset
  GtkWidget *button = dt_ui_button_new(_("apply offset"), _("apply offset to selected images"), NULL);
  gtk_grid_attach(grid, button, 0, line, 1, 1);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(_apply_offset_callback), self);

  // gpx
  button = dt_ui_button_new(_("apply GPX track file..."),
                            _("parses a GPX file and updates location of selected images"), NULL);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(_lib_geotagging_gpx_callback), self);
  gtk_grid_attach(grid, button, 1, line++, 1, 1);

  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(grid), TRUE, TRUE, 0);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_MOUSE_OVER_IMAGE_CHANGE,
                            G_CALLBACK(_mouse_over_image_callback), self);
}

void gui_cleanup(dt_lib_module_t *self)
{
  dt_lib_geotagging_t *d = (dt_lib_geotagging_t *)self->data;
  dt_gui_key_accel_block_on_focus_disconnect(d->datetime);
  g_list_free_full(d->timezones, free_tz_tuple);
  d->timezones = NULL;
  g_free(d->dt0_text);
  free(self->data);
  self->data = NULL;
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_mouse_over_image_callback), self);
}

void init_key_accels(dt_lib_module_t *self)
{
}

void connect_key_accels(dt_lib_module_t *self)
{
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
