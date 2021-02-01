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

typedef struct dt_lib_datetime_t
{
  GtkWidget *widget[6];
  GtkWidget *sign;
} dt_lib_datetime_t;

typedef struct dt_lib_geotagging_t
{
  dt_lib_datetime_t dt;
  dt_lib_datetime_t dt0;
  dt_lib_datetime_t of;
  time_t datetime;
  time_t datetime0;
  time_t offset;
  uint32_t imgid;
  GtkWidget *apply_offset;
  GtkWidget *lock_offset;
  GtkWidget *apply_datetime;
  GtkWidget *timezone;
  GList *timezones;
} dt_lib_geotagging_t;

static void _datetime_changed(GtkWidget *entry, dt_lib_module_t *self);

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

// modify the datetime_taken field in the db/cache
static void _apply_offset_callback(GtkWidget *widget, dt_lib_module_t *self)
{
  dt_lib_geotagging_t *d = (dt_lib_geotagging_t *)self->data;
  if(d->offset)
    dt_control_datetime(d->offset, NULL, -1);
}

// modify the datetime_taken field in the db/cache
static void _apply_datetime_callback(GtkWidget *widget, dt_lib_module_t *self)
{
  dt_lib_geotagging_t *d = (dt_lib_geotagging_t *)self->data;
  if(d->datetime > 0)
  {
    char text[DT_DATETIME_LENGTH];
    struct tm dt;
    strftime(text, sizeof(text), "%Y:%m:%d %H:%M:%S", localtime_r(&d->datetime, &dt));
    dt_control_datetime(0, text, -1);
  }
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

  if(gtk_dialog_run(GTK_DIALOG(filechooser)) == GTK_RESPONSE_ACCEPT)
  {
    gchar *folder = gtk_file_chooser_get_current_folder(GTK_FILE_CHOOSER(filechooser));
    dt_conf_set_string("ui_last/gpx_last_directory", folder);
    g_free(folder);

    gchar *tz = dt_conf_get_string("plugins/lighttable/geotagging/tz");
    gchar *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(filechooser));
    dt_control_gpx_apply(filename, -1, tz);
    g_free(filename);
    g_free(tz);
  }

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

static void _display_offset(const time_t offset_int, const gboolean valid, dt_lib_module_t *self)
{
  dt_lib_geotagging_t *d = (dt_lib_geotagging_t *)self->data;
  time_t off2 = 0;
  if(valid)
  {
    const gboolean neg = offset_int < 0;
    gtk_label_set_text(GTK_LABEL(d->of.sign), neg ? "- " : "");
    char text[4];
    time_t off = neg ? -offset_int : offset_int;
    off2 = off / 60;
    snprintf(text, sizeof(text), "%02d", (int)(off - off2 * 60));
    gtk_entry_set_text(GTK_ENTRY(d->of.widget[5]), text);
    off = off2;
    off2 = off / 60;
    snprintf(text, sizeof(text), "%02d", (int)(off - off2 * 60));
    gtk_entry_set_text(GTK_ENTRY(d->of.widget[4]), text);
    off = off2;
    off2 = off / 24;
    snprintf(text, sizeof(text), "%02d", (int)(off - off2 * 24));
    gtk_entry_set_text(GTK_ENTRY(d->of.widget[3]), text);
    off = off2;
    off2 = off / 100;
    snprintf(text, sizeof(text), "%02d", (int)(off - off2 * 100));
    gtk_entry_set_text(GTK_ENTRY(d->of.widget[2]), text);
  }
  if(!valid || off2)
  {
    gtk_label_set_text(GTK_LABEL(d->of.sign), "");
    for(int i = 2; i < 6; i++)
      gtk_entry_set_text(GTK_ENTRY(d->of.widget[i]), "-");
  }
  const gboolean locked = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->lock_offset));
  gtk_widget_set_sensitive(d->apply_offset, d->imgid && valid && !off2 && offset_int);
  gtk_widget_set_sensitive(d->lock_offset, locked || (d->imgid && valid && !off2 && offset_int));
  gtk_widget_set_sensitive(d->apply_datetime, d->imgid && !locked);
}

static void _display_datetime(dt_lib_datetime_t *dtw, const time_t datetime)
{
  if(datetime > 0)
  {
    struct tm dt;
    localtime_r(&datetime, &dt);
    char text[6];
    snprintf(text, sizeof(text), "%04d", dt.tm_year + 1900);
    gtk_entry_set_text(GTK_ENTRY(dtw->widget[0]), text);
    snprintf(text, sizeof(text), "%02d", dt.tm_mon + 1);
    gtk_entry_set_text(GTK_ENTRY(dtw->widget[1]), text);
    snprintf(text, sizeof(text), "%02d", dt.tm_mday);
    gtk_entry_set_text(GTK_ENTRY(dtw->widget[2]), text);
    snprintf(text, sizeof(text), "%02d", dt.tm_hour);
    gtk_entry_set_text(GTK_ENTRY(dtw->widget[3]), text);
    snprintf(text, sizeof(text), "%02d", dt.tm_min);
    gtk_entry_set_text(GTK_ENTRY(dtw->widget[4]), text);
    snprintf(text, sizeof(text), "%02d", dt.tm_sec);
    gtk_entry_set_text(GTK_ENTRY(dtw->widget[5]), text);
  }
  else
  {
    for(int i = 0; i < 6; i++)
      gtk_entry_set_text(GTK_ENTRY(dtw->widget[i]), "-");
  }
}

static time_t _read_datetime_entry(dt_lib_module_t *self)
{
  dt_lib_geotagging_t *d = (dt_lib_geotagging_t *)self->data;
  struct tm dt = {0}; struct tm dt2 = {0};
  dt.tm_year = dt2.tm_year = atol(gtk_entry_get_text(GTK_ENTRY(d->dt.widget[0])));
  dt.tm_mon = dt2.tm_mon = atol(gtk_entry_get_text(GTK_ENTRY(d->dt.widget[1])));
  dt.tm_mday = dt2.tm_mday = atol(gtk_entry_get_text(GTK_ENTRY(d->dt.widget[2])));
  dt.tm_hour = dt2.tm_hour = atol(gtk_entry_get_text(GTK_ENTRY(d->dt.widget[3])));
  dt.tm_min = dt2.tm_min = atol(gtk_entry_get_text(GTK_ENTRY(d->dt.widget[4])));
  dt.tm_sec = dt2.tm_sec = atol(gtk_entry_get_text(GTK_ENTRY(d->dt.widget[5])));
  dt.tm_year -= 1900; dt2.tm_year -= 1900;
  dt.tm_mon --; dt2.tm_mon --;
  dt.tm_isdst = -1;

  time_t datetime = mktime(&dt);
  // mktime recalculate the different elements - for example 31/02
  // if difference display the new date
  if(datetime != -1 &&
     (dt.tm_year != dt2.tm_year || dt.tm_mon != dt2.tm_mon ||  dt.tm_mday != dt2.tm_mday ||
      dt.tm_hour != dt2.tm_hour || dt.tm_min != dt2.tm_min || dt.tm_sec != dt2.tm_sec))
  {
    for(int i = 0; i < 6; i++)
      g_signal_handlers_block_by_func(d->dt.widget[i], _datetime_changed, self);
    _display_datetime(&d->dt, datetime);
    for(int i = 0; i < 6; i++)
      g_signal_handlers_unblock_by_func(d->dt.widget[i], _datetime_changed, self);
  }
  return datetime;
}

static void _datetime_changed(GtkWidget *entry, dt_lib_module_t *self)
{
  dt_lib_geotagging_t *d = (dt_lib_geotagging_t *)self->data;
  const gboolean locked = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->lock_offset));
  if(!locked)
  {
    d->datetime = _read_datetime_entry(self);
    if(d->datetime > 0)
      d->offset = d->datetime - d->datetime0;
    _display_offset(d->offset, d->datetime > 0, self);
  }
}

static time_t _get_datetime_from_text(const char *text)
{
  struct tm dt = {0};
  if(sscanf(text, "%d:%d:%d %d:%d:%d",
       &dt.tm_year, &dt.tm_mon, &dt.tm_mday, &dt.tm_hour, &dt.tm_min, &dt.tm_sec) == 6)
  {
    dt.tm_year -= 1900;
    dt.tm_mon--;
    dt.tm_isdst = -1;    // no daylight saving time
    return mktime(&dt);
  }
  else return 0;
}

static time_t _get_image_datetime(dt_lib_module_t *self)
{
  dt_lib_geotagging_t *d = (dt_lib_geotagging_t *)self->data;
  GList *selected = dt_collection_get_selected(darktable.collection, 1);
  const int selid = selected ? GPOINTER_TO_INT(selected->data) : 0;
  const int imgid = dt_view_get_image_to_act_on();
  time_t datetime = 0;
  if((selid != 0) || ((selid == 0) && (imgid != -1)))
  {
    // consider act on only if no selected
    char datetime_s[DT_DATETIME_LENGTH];
    dt_image_get_datetime(selid ? selid : imgid, datetime_s);
    if(datetime_s[0] != '\0')
      datetime = _get_datetime_from_text(datetime_s);
    else
      datetime = time(NULL);
  }
  d->imgid = selid;
  return datetime;
}

static void _refresh_image_datetime(dt_lib_module_t *self)
{
  dt_lib_geotagging_t *d = (dt_lib_geotagging_t *)self->data;
  const gboolean locked = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->lock_offset));
  time_t datetime = _get_image_datetime(self);
  d->datetime0 = datetime;
  _display_datetime(&d->dt0, datetime);
  if(datetime && locked)
    datetime += d->offset;
  d->datetime = datetime;
  _display_datetime(&d->dt, datetime);
}

static void _mouse_over_image_callback(gpointer instance, dt_lib_module_t *self)
{
  dt_lib_geotagging_t *d = (dt_lib_geotagging_t *)self->data;
  if(!d->imgid)
    _refresh_image_datetime(self);
}

static void _selection_changed_callback(gpointer instance, dt_lib_module_t *self)
{
  _refresh_image_datetime(self);
}

static void _datetime_changed_callback(gpointer instance, gpointer imgs, dt_lib_module_t *self)
{
  dt_lib_geotagging_t *d = (dt_lib_geotagging_t *)self->data;
  for(GList *i = imgs; i; i = g_list_next(i))
  {
    if(GPOINTER_TO_INT(i->data) == d->imgid)
    {
      time_t datetime = _get_image_datetime(self);
      if(datetime)
      {
        d->datetime = d->datetime0 = datetime;
        _display_datetime(&d->dt0, datetime);
        _display_datetime(&d->dt, datetime);
      }
      break;
    }
  }
}

static gboolean _datetime_scroll_over(GtkWidget *w, GdkEventScroll *event, dt_lib_module_t *self)
{
  int min[6] = {1900, 0, 0, -1, -1, -1};
  int max[6] = {3000, 13, 32, 24, 60, 60};
  dt_lib_geotagging_t *d = (dt_lib_geotagging_t *)self->data;
  int value = atol(gtk_entry_get_text(GTK_ENTRY(w)));
  int i = 0;
  for(i = 0; i < 6; i++)
    if(w == d->dt.widget[i]) break;

  int increment = event->direction == GDK_SCROLL_DOWN ? -1 : 1;
  if(event->state & GDK_SHIFT_MASK)
    increment *= 10;
  value += increment;
  value = MAX(MIN(value, max[i]), min[i]);

  char text[6];
  snprintf(text, sizeof(text), i == 0 ? "%04d" : "%02d", value);
  gtk_entry_set_text(GTK_ENTRY(w), text);

  return TRUE;
}

static GtkWidget *_gui_init_datetime(dt_lib_datetime_t *dt, const int type, dt_lib_module_t *self)
{
  GtkBox *box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0));
  for(int i = 0; i < 6; i++)
  {
    if(i == 0 && type == 2)
    {
      dt->sign = gtk_label_new("");
      gtk_box_pack_start(box, dt->sign, FALSE, FALSE, 0);
    }
    if(((i < 2) && (type == 0 || type == 1)) || (i >= 2))
    {
      dt->widget[i] = gtk_entry_new();
      gtk_entry_set_width_chars(GTK_ENTRY(dt->widget[i]), i == 0 ? 4 : 2);
      gtk_entry_set_alignment(GTK_ENTRY(dt->widget[i]), 0.5);
      gtk_box_pack_start(box, dt->widget[i], FALSE, FALSE, 0);
      if(i < 5)
      {
        GtkWidget *label = gtk_label_new(i == 2 ? " " : ":");
        gtk_box_pack_start(box, label, FALSE, FALSE, 0);
      }
      if(type == 0)
      {
        dt_gui_key_accel_block_on_focus_connect(dt->widget[i]);
        gtk_widget_add_events(dt->widget[i], GDK_SCROLL_MASK);
      }
      else
      {
        gtk_widget_set_sensitive(dt->widget[i], FALSE);
      }
    }
  }
  return GTK_WIDGET(box);
}

static gboolean _datetime_key_pressed(GtkWidget *entry, GdkEventKey *event, dt_lib_module_t *self)
{
  switch(event->keyval)
  {
    case GDK_KEY_Escape:
    {
      // reset
      _refresh_image_datetime(self);
      gtk_window_set_focus(GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)), NULL);
      return FALSE;
    }
    // allow  0 .. 9, left/right/home/end movement using arrow keys and del/backspace
    case GDK_KEY_0:
    case GDK_KEY_KP_0:
    case GDK_KEY_1:
    case GDK_KEY_KP_1:
    case GDK_KEY_2:
    case GDK_KEY_KP_2:
    case GDK_KEY_3:
    case GDK_KEY_KP_3:
    case GDK_KEY_4:
    case GDK_KEY_KP_4:
    case GDK_KEY_5:
    case GDK_KEY_KP_5:
    case GDK_KEY_6:
    case GDK_KEY_KP_6:
    case GDK_KEY_7:
    case GDK_KEY_KP_7:
    case GDK_KEY_8:
    case GDK_KEY_KP_8:
    case GDK_KEY_9:
    case GDK_KEY_KP_9:
    case GDK_KEY_Tab:
    case GDK_KEY_Delete:
    case GDK_KEY_KP_Delete:
    case GDK_KEY_BackSpace:
    case GDK_KEY_Left:
    case GDK_KEY_Right:
    case GDK_KEY_Home:
    case GDK_KEY_KP_Home:
    case GDK_KEY_End:
    case GDK_KEY_KP_End:
      return FALSE;

    default: // block everything else
      return TRUE;
  }
}

static void _timezone_save(dt_lib_module_t *self)
{
  dt_lib_geotagging_t *d = (dt_lib_geotagging_t *)self->data;
  const gchar *tz = gtk_entry_get_text(GTK_ENTRY(d->timezone));

  gchar *name = NULL;
  for(GList *iter = d->timezones; iter; iter = g_list_next(iter))
  {
    tz_tuple_t *tz_tuple = (tz_tuple_t *)iter->data;
    if(!strcmp(tz_tuple->display, tz))
      name = tz_tuple->name;
  }
  dt_conf_set_string("plugins/lighttable/geotagging/tz", name ? name : "UTC");
  gtk_entry_set_text(GTK_ENTRY(d->timezone), name ? name : "UTC");
}

static gboolean _timezone_key_pressed(GtkWidget *entry, GdkEventKey *event, dt_lib_module_t *self)
{
  switch(event->keyval)
  {
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
      _timezone_save(self);
      gtk_window_set_focus(GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)), NULL);
      return TRUE;
    case GDK_KEY_Escape:
      gtk_window_set_focus(GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)), NULL);
      return TRUE;
    case GDK_KEY_Tab:
      return TRUE;
    default:
      break;
  }
  return FALSE;
}

static gboolean _completion_match_func(GtkEntryCompletion *completion, const gchar *key, GtkTreeIter *iter,
                                       gpointer user_data)
{
  gboolean res = FALSE;

  GtkEditable *e = (GtkEditable *)gtk_entry_completion_get_entry(completion);
  if(!GTK_IS_EDITABLE(e))
    return FALSE;

  GtkTreeModel *model = gtk_entry_completion_get_model(completion);
  const int column = gtk_entry_completion_get_text_column(completion);

  if(gtk_tree_model_get_column_type(model, column) != G_TYPE_STRING)
    return FALSE;

  char *tag = NULL;
  gtk_tree_model_get(model, iter, column, &tag, -1);
  if(tag)
  {
    char *normalized = g_utf8_normalize(tag, -1, G_NORMALIZE_ALL);
    if(normalized)
    {
      char *casefold = g_utf8_casefold(normalized, -1);
      if(casefold)
      {
        res = g_strstr_len(casefold, -1, key) != NULL;
      }
      g_free(casefold);
    }
    g_free(normalized);
    g_free(tag);
  }

  return res;
}

static void _toggle_lock_button_callback(GtkToggleButton *button, dt_lib_module_t *self)
{
  dt_lib_geotagging_t *d = (dt_lib_geotagging_t *)self->data;
  const gboolean locked = gtk_toggle_button_get_active(button);
  for(int i = 0; i < 6; i++)
  {
    gtk_widget_set_sensitive(d->dt.widget[i], !locked);
  }
  gtk_widget_set_sensitive(d->apply_datetime, d->imgid && !locked);
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

  GtkWidget *label = dt_ui_label_new(_("date time"));
  gtk_grid_attach(grid, label, 0, line, 1, 1);
  gtk_widget_set_tooltip_text(label, _("enter the new date time (yyyy:mm:dd hh:mm:ss)"
                                       "\nkey in the new numbers or scroll over the cell"));

  GtkWidget *box = _gui_init_datetime(&d->dt, 0, self);
  gtk_widget_set_halign(box, GTK_ALIGN_END);
  gtk_widget_set_hexpand(box, TRUE);
  gtk_grid_attach(grid, box, 1, line++, 2, 1);

  label = dt_ui_label_new(_("original date time"));
  gtk_grid_attach(grid, label, 0, line, 1, 1);

  box = _gui_init_datetime(&d->dt0, 1, self);
  gtk_widget_set_halign(box, GTK_ALIGN_END);
  gtk_widget_set_hexpand(box, TRUE);
  gtk_grid_attach(grid, box, 1, line++, 2, 1);

  label = dt_ui_label_new(_("date time offset"));
  gtk_grid_attach(grid, label, 0, line, 1, 1);
  gtk_widget_set_tooltip_text(label, _("offset or difference ([-]dd hh:mm:ss)"));

  d->lock_offset = dtgtk_togglebutton_new(dtgtk_cairo_paint_lock, CPF_STYLE_FLAT, NULL);
  gtk_widget_set_tooltip_text(d->lock_offset, _("lock date time offset value to apply it onto another selection"));
  gtk_widget_set_halign(d->lock_offset, GTK_ALIGN_END);
  gtk_grid_attach(grid, d->lock_offset, 1, line, 1, 1);
  g_signal_connect(G_OBJECT(d->lock_offset), "clicked", G_CALLBACK(_toggle_lock_button_callback), (gpointer)self);

  box = _gui_init_datetime(&d->of, 2, self);
  gtk_widget_set_halign(box, GTK_ALIGN_END);
  gtk_widget_set_hexpand(box, TRUE);
  gtk_grid_attach(grid, box, 2, line++, 1, 1);

  // apply
  d->apply_offset = dt_ui_button_new(_("apply offset"), _("apply offset to selected images"), NULL);
  gtk_widget_set_hexpand(d->apply_offset, TRUE);
  gtk_grid_attach(grid, d->apply_offset , 0, line, 1, 1);
  g_signal_connect(G_OBJECT(d->apply_offset), "clicked", G_CALLBACK(_apply_offset_callback), self);

  d->apply_datetime = dt_ui_button_new(_("apply date time"), _("apply the same date time to selected images"), NULL);
  gtk_widget_set_hexpand(d->apply_datetime, TRUE);
  gtk_grid_attach(grid, d->apply_datetime , 1, line++, 2, 1);
  g_signal_connect(G_OBJECT(d->apply_datetime), "clicked", G_CALLBACK(_apply_datetime_callback), self);

  // time zone entry
  label = dt_ui_label_new(_("camera time zone"));
  gtk_widget_set_tooltip_text(label, _("most cameras don't store the time zone in EXIF. "
                                       "give the correct time zone so the GPX data can be correctly matched"));
  gtk_grid_attach(grid, label , 0, line, 1, 1);

  d->timezone = gtk_entry_new();
  gtk_entry_set_text(GTK_ENTRY(d->timezone), dt_conf_get_string("plugins/lighttable/geotagging/tz"));
  gtk_grid_attach(grid, d->timezone, 1, line++, 2, 1);

  GtkCellRenderer *renderer;
  GtkTreeIter tree_iter;
  GtkListStore *model = gtk_list_store_new(2, G_TYPE_STRING /*display*/, G_TYPE_STRING /*name*/);
  GtkWidget *tz_selection = gtk_combo_box_new_with_model(GTK_TREE_MODEL(model));
  renderer = gtk_cell_renderer_text_new();
  gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(tz_selection), renderer, FALSE);
  gtk_cell_layout_set_attributes(GTK_CELL_LAYOUT(tz_selection), renderer, "text", 0, NULL);

  gchar *old_tz = dt_conf_get_string("plugins/lighttable/geotagging/tz");
  for(GList *iter = d->timezones; iter; iter = g_list_next(iter))
  {
    tz_tuple_t *tz_tuple = (tz_tuple_t *)iter->data;
    gtk_list_store_append(model, &tree_iter);
    gtk_list_store_set(model, &tree_iter, 0, tz_tuple->display, 1, tz_tuple->name, -1);
    if(!strcmp(tz_tuple->name, old_tz))
      gtk_entry_set_text(GTK_ENTRY(d->timezone), tz_tuple->display);
  }
  g_free(old_tz);

  // add entry completion
  GtkEntryCompletion *completion = gtk_entry_completion_new();
  gtk_entry_completion_set_model(completion, GTK_TREE_MODEL(model));
  gtk_entry_completion_set_text_column(completion, 0);
  gtk_entry_completion_set_inline_completion(completion, TRUE);
  gtk_entry_completion_set_popup_set_width(completion, FALSE);
  gtk_entry_completion_set_match_func(completion, _completion_match_func, NULL, NULL);
  gtk_entry_set_completion(GTK_ENTRY(d->timezone), completion);
  dt_gui_key_accel_block_on_focus_connect(GTK_WIDGET(d->timezone));
  g_signal_connect(G_OBJECT(d->timezone), "key-press-event", G_CALLBACK(_timezone_key_pressed), self);

  // gpx
  GtkWidget *button = dt_ui_button_new(_("apply GPX track file..."),
                            _("parses a GPX file and updates location of selected images"), NULL);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(_lib_geotagging_gpx_callback), self);
  gtk_grid_attach(grid, button, 0, line++, 3, 1);

  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(grid), TRUE, TRUE, 0);

  d->imgid = 0;
  d->datetime0 = _get_image_datetime(self);
  _display_datetime(&d->dt0, d->datetime0);
  _display_datetime(&d->dt, d->datetime0);
  d->offset = 0;
  _display_offset(d->offset, TRUE, self);

  for(int i = 0; i < 6; i++)
  {
    g_signal_connect(d->dt.widget[i], "changed", G_CALLBACK(_datetime_changed), self);
    g_signal_connect(d->dt.widget[i], "key-press-event", G_CALLBACK(_datetime_key_pressed), self);
    g_signal_connect(d->dt.widget[i], "scroll-event", G_CALLBACK(_datetime_scroll_over), self);
  }
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_SELECTION_CHANGED,
                            G_CALLBACK(_selection_changed_callback), self);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_MOUSE_OVER_IMAGE_CHANGE,
                            G_CALLBACK(_mouse_over_image_callback), self);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_IMAGE_INFO_CHANGED,
                            G_CALLBACK(_datetime_changed_callback), self);
}

void gui_cleanup(dt_lib_module_t *self)
{
  dt_lib_geotagging_t *d = (dt_lib_geotagging_t *)self->data;
  for(int i = 0; i < 6; i++)
  {
    dt_gui_key_accel_block_on_focus_disconnect(d->dt.widget[i]);
  }
  dt_gui_key_accel_block_on_focus_disconnect(d->timezone);
  g_list_free_full(d->timezones, free_tz_tuple);
  d->timezones = NULL;
  free(self->data);
  self->data = NULL;
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_selection_changed_callback), self);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_mouse_over_image_callback), self);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_datetime_changed_callback), self);
}

void gui_reset(dt_lib_module_t *self)
{
  _refresh_image_datetime(self);
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
