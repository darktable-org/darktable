/*
    This file is part of darktable,
    copyright (c) 2012 tobias ellinghaus.

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
#include "common/image_cache.h"
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
  GtkWidget *offset_entry;
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

/* try to parse the offset string. returns true if it worked, false otherwise.
 *if seconds != NULL the offset will be put there in seconds or 0 if it failed. always look at the return
 *value before using the seconds value! */
static gboolean _lib_geotagging_parse_offset(const char *str, long int *seconds)
{
  const gchar *str_bak = str;
  long int result = 0;
  int numbers[3] = { 0, 0, 0 };
  int fields = 0;
  char sign = '+';
  if(seconds) *seconds = 0;

  if(!str) return FALSE;

  size_t len = strlen(str);

  // optional sign
  if(*str == '+' || *str == '-')
  {
    sign = *str;
    str++;
    len--;
  }

  // hh, mm or ss
  if(len < 2) return FALSE;
  if(!g_ascii_isdigit(str[0]) || !g_ascii_isdigit(str[1]))
    return FALSE;
  else
  {
    numbers[fields++] = 10 * (str[0] - '0') + (str[1] - '0');
    str += 2;
    len -= 2;
  }

  // : or end
  if(*str == '\0') goto parse_success;
  if(*str == ':')
  {
    str++;
    len--;
  }

  // mm or ss
  if(len < 2) return FALSE;
  if(!g_ascii_isdigit(str[0]) || !g_ascii_isdigit(str[1]))
    return FALSE;
  else
  {
    numbers[fields++] = 10 * (str[0] - '0') + (str[1] - '0');
    str += 2;
    len -= 2;
  }

  // : or end
  if(*str == '\0') goto parse_success;
  if(*str == ':')
  {
    str++;
    len--;
  }

  // ss
  if(len < 2) return FALSE;
  if(!g_ascii_isdigit(str[0]) || !g_ascii_isdigit(str[1]))
    return FALSE;
  else
  {
    numbers[fields++] = 10 * (str[0] - '0') + (str[1] - '0');
    str += 2;
//     len -= 2;
  }

  // end
  if(*str == '\0') goto parse_success;

  return FALSE;

parse_success:
  if(seconds)
  {
    // assemble the numbers in numbers[] into a single seconds value
    switch(fields)
    {
      case 1: // 0: seconds
        result = numbers[0];
        break;
      case 2: // 0: minutes, 1: seconds
        result = 60 * numbers[0] + numbers[1];
        break;
      case 3: // 0: hours, 1: minutes, 2: seconds
        result = 60 * 60 * numbers[0] + 60 * numbers[1] + numbers[2];
        break;
      default: // shouldn't happen
        fprintf(stderr, "[geotagging] error: something went terribly wrong while parsing the offset, %d "
                        "fields found in %s\n",
                fields, str_bak);
    }

    if(sign == '-') result *= -1;
    *seconds = result;
  }

  return TRUE;
}

static gboolean _lib_geotagging_offset_key_press(GtkWidget *entry, GdkEventKey *event, dt_lib_module_t *self)
{
  dt_lib_geotagging_t *d = (dt_lib_geotagging_t *)self->data;
  switch(event->keyval)
  {
    case GDK_KEY_Escape:
    case GDK_KEY_Tab:
    {
      // reset
      gchar *str = dt_conf_get_string("plugins/lighttable/geotagging/offset");
      if(_lib_geotagging_parse_offset(str, NULL))
      {
        gtk_entry_set_text(GTK_ENTRY(d->offset_entry), str);
      }
      else
      {
        gtk_entry_set_text(GTK_ENTRY(d->offset_entry), "+00:00:00");
        dt_conf_set_string("plugins/lighttable/geotagging/offset", "+00:00:00");
      }
      g_free(str);
      gtk_window_set_focus(GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)), NULL);
      return FALSE;
    }

    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
    {
      const gchar *value = gtk_entry_get_text(GTK_ENTRY(d->offset_entry));
      if(_lib_geotagging_parse_offset(value, NULL))
      {
        dt_conf_set_string("plugins/lighttable/geotagging/offset", value);
      }
      else
      {
        gtk_entry_set_text(GTK_ENTRY(d->offset_entry), "+00:00:00");
        dt_conf_set_string("plugins/lighttable/geotagging/offset", "+00:00:00");
      }
      gtk_window_set_focus(GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)), NULL);
      return FALSE;
    }

    // allow +, -, :, 0 .. 9, left/right/home/end movement using arrow keys and del/backspace
    case GDK_KEY_plus:
    case GDK_KEY_KP_Add:
    case GDK_KEY_minus:
    case GDK_KEY_KP_Subtract:
    case GDK_KEY_colon:
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

    case GDK_KEY_Left:
    case GDK_KEY_Right:
    case GDK_KEY_Home:
    case GDK_KEY_KP_Home:
    case GDK_KEY_End:
    case GDK_KEY_KP_End:
    case GDK_KEY_Delete:
    case GDK_KEY_KP_Delete:
    case GDK_KEY_BackSpace:
      return FALSE;

    default: // block everything else
      return TRUE;
  }
}

static gboolean _lib_geotagging_offset_focus_out(GtkWidget *widget, GdkEvent *event, dt_lib_module_t *self)
{
  dt_lib_geotagging_t *d = (dt_lib_geotagging_t *)self->data;
  const gchar *value = gtk_entry_get_text(GTK_ENTRY(d->offset_entry));
  if(_lib_geotagging_parse_offset(value, NULL))
    dt_conf_set_string("plugins/lighttable/geotagging/offset", value);
  else
  {
    gchar *str = dt_conf_get_string("plugins/lighttable/geotagging/offset");
    gtk_entry_set_text(GTK_ENTRY(d->offset_entry), str);
    g_free(str);
  }
  return FALSE;
}

static void _lib_geotagging_calculate_offset_callback(GtkWidget *widget, dt_lib_module_t *self)
{
  dt_lib_geotagging_t *d = (dt_lib_geotagging_t *)self->data;
  const gchar *gps_time = gtk_entry_get_text(GTK_ENTRY(d->floating_window_entry));
  if(gps_time)
  {
    gchar **tokens = g_strsplit(gps_time, ":", 0);
    if(tokens[0] != NULL && tokens[1] != NULL && tokens[2] != NULL)
    {
      if(g_ascii_isdigit(tokens[0][0]) && g_ascii_isdigit(tokens[0][1]) && tokens[0][2] == '\0'
         && g_ascii_isdigit(tokens[1][0]) && g_ascii_isdigit(tokens[1][1]) && tokens[1][2] == '\0'
         && g_ascii_isdigit(tokens[2][0]) && g_ascii_isdigit(tokens[2][1]) && tokens[2][2] == '\0')
      {
        int h, m, s;
        h = (tokens[0][0] - '0') * 10 + tokens[0][1] - '0';
        m = (tokens[1][0] - '0') * 10 + tokens[1][1] - '0';
        s = (tokens[2][0] - '0') * 10 + tokens[2][1] - '0';
        if(h < 24 && m < 60 && s < 60)
        {
          // finally a valid time
          // get imgid
          int32_t imgid = -1;
          sqlite3_stmt *stmt;
          DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                      "SELECT imgid FROM main.selected_images ORDER BY imgid ASC LIMIT 1", -1,
                                      &stmt, NULL);
          if(sqlite3_step(stmt) == SQLITE_ROW)
            imgid = sqlite3_column_int(stmt, 0);
          else // no selection is used, use mouse over id
            imgid = dt_control_get_mouse_over_id();
          sqlite3_finalize(stmt);

          if(imgid > 0)
          {
            const dt_image_t *cimg = dt_image_cache_get(darktable.image_cache, imgid, 'r');
            // get the exif_datetime_taken and parse it
            gint year;
            gint month;
            gint day;
            gint hour;
            gint minute;
            gint second;

            if(sscanf(cimg->exif_datetime_taken, "%d:%d:%d %d:%d:%d", (int *)&year, (int *)&month,
                      (int *)&day, (int *)&hour, (int *)&minute, (int *)&second) == 6)
            {
              // calculate the offset
              long int exif_seconds = hour * 60 * 60 + minute * 60 + second;
              long int gps_seconds = h * 60 * 60 + m * 60 + s;
              long int offset = gps_seconds - exif_seconds;
              // transform the offset back into a string
              gchar sign = (offset < 0) ? '-' : '+';
              offset = labs(offset);
              gint offset_h = offset / (60 * 60);
              offset -= offset_h * 60 * 60;
              gint offset_m = offset / 60;
              offset -= offset_m * 60;
              gchar *offset_str = g_strdup_printf("%c%02d:%02d:%02ld", sign, offset_h, offset_m, offset);
              // write the offset into d->offset_entry
              gtk_entry_set_text(GTK_ENTRY(d->offset_entry), offset_str);
              g_free(offset_str);
            }

            dt_image_cache_read_release(darktable.image_cache, cimg);
          }
        }
      }
    }
    g_strfreev(tokens);
  }
  gtk_widget_destroy(d->floating_window);
}

static gboolean _lib_geotagging_floating_key_press(GtkWidget *entry, GdkEventKey *event, dt_lib_module_t *self)
{
  dt_lib_geotagging_t *d = (dt_lib_geotagging_t *)self->data;
  switch(event->keyval)
  {
    case GDK_KEY_Escape:
      gtk_widget_destroy(d->floating_window);
      return TRUE;

    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
      _lib_geotagging_calculate_offset_callback(NULL, self);
      return TRUE;

    default:
      return FALSE;
  }
}

static void _lib_geotagging_show_offset_window(GtkWidget *widget, dt_lib_module_t *self)
{
  dt_lib_geotagging_t *d = self->data;
  gint x, y;
  gint px, py, center_w, center_h, window_w, window_h;
  GtkWidget *window = dt_ui_main_window(darktable.gui->ui);
  GtkWidget *center = dt_ui_center(darktable.gui->ui);
  gdk_window_get_origin(gtk_widget_get_window(center), &px, &py);

  center_w = gdk_window_get_width(gtk_widget_get_window(center));
  center_h = gdk_window_get_height(gtk_widget_get_window(center));

  d->floating_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(d->floating_window);
#endif
  gtk_widget_set_can_focus(d->floating_window, TRUE);
  gtk_window_set_decorated(GTK_WINDOW(d->floating_window), FALSE);
  gtk_window_set_type_hint(GTK_WINDOW(d->floating_window), GDK_WINDOW_TYPE_HINT_POPUP_MENU);
  gtk_window_set_transient_for(GTK_WINDOW(d->floating_window), GTK_WINDOW(window));
  gtk_widget_set_opacity(d->floating_window, 0.8);
  gtk_window_set_modal(GTK_WINDOW(d->floating_window), TRUE);

  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
  gtk_widget_set_margin_top(vbox, DT_PIXEL_APPLY_DPI(2));
  gtk_widget_set_margin_bottom(vbox, DT_PIXEL_APPLY_DPI(5));
  gtk_widget_set_margin_start(vbox, DT_PIXEL_APPLY_DPI(5));
  gtk_widget_set_margin_end(vbox, DT_PIXEL_APPLY_DPI(5));
  gtk_container_add(GTK_CONTAINER(d->floating_window), vbox);

  d->floating_window_entry = gtk_entry_new();
  gtk_widget_add_events(d->floating_window_entry, GDK_FOCUS_CHANGE_MASK);
  g_signal_connect_swapped(d->floating_window, "focus-out-event", G_CALLBACK(gtk_widget_destroy),
                           d->floating_window);
  gtk_widget_set_tooltip_text(d->floating_window_entry,
                              _("enter the time shown on the selected picture\nformat: hh:mm:ss"));

  gtk_editable_select_region(GTK_EDITABLE(d->floating_window_entry), 0, -1);
  gtk_box_pack_start(GTK_BOX(vbox), d->floating_window_entry, TRUE, TRUE, 0);
  g_signal_connect(d->floating_window_entry, "key-press-event",
                   G_CALLBACK(_lib_geotagging_floating_key_press), self);

  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
  GtkWidget *cancel_button = gtk_button_new_with_label(_("cancel"));
  GtkWidget *ok_button = gtk_button_new_with_label(_("ok"));

  gtk_box_pack_start(GTK_BOX(hbox), cancel_button, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(hbox), ok_button, TRUE, TRUE, 0);
  g_signal_connect_swapped(G_OBJECT(cancel_button), "clicked", G_CALLBACK(gtk_widget_destroy),
                           d->floating_window);
  g_signal_connect(G_OBJECT(ok_button), "clicked", G_CALLBACK(_lib_geotagging_calculate_offset_callback),
                   self);

  gtk_box_pack_start(GTK_BOX(vbox), hbox, TRUE, TRUE, 0);

  gtk_widget_show_all(d->floating_window);
  gtk_widget_grab_focus(d->floating_window_entry);

  window_w = gdk_window_get_width(gtk_widget_get_window(d->floating_window));
  window_h = gdk_window_get_height(gtk_widget_get_window(d->floating_window));

  x = px + 0.5 * (center_w - window_w);
  y = py + center_h - 20 - window_h;
  gtk_window_move(GTK_WINDOW(d->floating_window), x, y);

  gtk_window_present(GTK_WINDOW(d->floating_window));
}

// modify the datetime_taken field in the db/cache
static void _lib_geotagging_apply_offset_callback(GtkWidget *widget, gpointer user_data)
{
  dt_lib_geotagging_t *l = (dt_lib_geotagging_t *)((dt_lib_module_t *)user_data)->data;
  long int offset = 0;
  _lib_geotagging_parse_offset(gtk_entry_get_text(GTK_ENTRY(l->offset_entry)), &offset);
  dt_control_time_offset(offset, -1);
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
  GtkWidget *extra_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
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
      wchar_t *subkeyname = (wchar_t *)malloc((max_subkey_len + 1) * sizeof(wchar_t));

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


void gui_init(dt_lib_module_t *self)
{
  dt_lib_geotagging_t *d = (dt_lib_geotagging_t *)malloc(sizeof(dt_lib_geotagging_t));
  self->data = (void *)d;
  d->timezones = _lib_geotagging_get_timezones();
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_PIXEL_APPLY_DPI(5));
  dt_gui_add_help_link(self->widget, dt_get_help_url(self->plugin_name));
  GtkBox *hbox;
  GtkWidget *button, *label;

  /* the offset line */
  hbox = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_PIXEL_APPLY_DPI(10)));
  label = GTK_WIDGET(gtk_label_new(_("time offset")));
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  gtk_box_pack_start(hbox, label, FALSE, TRUE, 0);

  d->offset_entry = gtk_entry_new();
  dt_gui_key_accel_block_on_focus_connect(d->offset_entry);
  gtk_entry_set_max_length(GTK_ENTRY(d->offset_entry), 9);
  gtk_entry_set_width_chars(GTK_ENTRY(d->offset_entry), 0);
  gtk_box_pack_start(hbox, d->offset_entry, TRUE, TRUE, 0);
  g_signal_connect(d->offset_entry, "key-press-event", G_CALLBACK(_lib_geotagging_offset_key_press), self);
  g_signal_connect(d->offset_entry, "focus-out-event", G_CALLBACK(_lib_geotagging_offset_focus_out), self);
  gtk_widget_set_tooltip_text(d->offset_entry, _("time offset\nformat: [+-]?[[hh:]mm:]ss"));
  gchar *str = dt_conf_get_string("plugins/lighttable/geotagging/offset");
  if(_lib_geotagging_parse_offset(str, NULL))
    gtk_entry_set_text(GTK_ENTRY(d->offset_entry), str);
  else
    gtk_entry_set_text(GTK_ENTRY(d->offset_entry), "+00:00:00");
  g_free(str);

  GtkBox *button_box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_PIXEL_APPLY_DPI(5)));
  button = dtgtk_button_new(dtgtk_cairo_paint_zoom, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
  gtk_widget_set_tooltip_text(button, _("calculate the time offset from an image"));
  gtk_box_pack_start(button_box, button, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(_lib_geotagging_show_offset_window), self);

  button = dtgtk_button_new(dtgtk_cairo_paint_check_mark, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
  gtk_widget_set_tooltip_text(button, _("apply time offset to selected images"));
  gtk_box_pack_start(button_box, button, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(_lib_geotagging_apply_offset_callback), self);

  gtk_box_pack_start(hbox, GTK_WIDGET(button_box), FALSE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(hbox), TRUE, TRUE, 0);

  /* gpx */
  button = gtk_button_new_with_label(_("apply GPX track file"));
  gtk_widget_set_tooltip_text(button, _("parses a GPX file and updates location of selected images"));
  gtk_box_pack_start(GTK_BOX(self->widget), button, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(_lib_geotagging_gpx_callback), self);
}

void gui_cleanup(dt_lib_module_t *self)
{
  dt_lib_geotagging_t *d = (dt_lib_geotagging_t *)self->data;
  dt_gui_key_accel_block_on_focus_disconnect(d->offset_entry);
  g_list_free_full(d->timezones, free_tz_tuple);
  d->timezones = NULL;
  free(self->data);
  self->data = NULL;
}

void init_key_accels(dt_lib_module_t *self)
{
  //   dt_accel_register_lib(self, NC_("accel", "remove from collection"),
  //                         GDK_Delete, 0);
  //   dt_accel_register_lib(self, NC_("accel", "delete from disk"), 0, 0);
  //   dt_accel_register_lib(self,
  //                         NC_("accel", "rotate selected images 90 degrees CW"),
  //                         0, 0);
  //   dt_accel_register_lib(self,
  //                         NC_("accel", "rotate selected images 90 degrees CCW"),
  //                         0, 0);
  //   dt_accel_register_lib(self, NC_("accel", "create hdr"), 0, 0);
  //   dt_accel_register_lib(self, NC_("accel", "duplicate"), 0, 0);
  //   dt_accel_register_lib(self, NC_("accel", "reset rotation"), 0, 0);
}

void connect_key_accels(dt_lib_module_t *self)
{
  //   dt_lib_image_t *d = (dt_lib_image_t*)self->data;
  //
  //   dt_accel_connect_button_lib(self, "remove from collection", d->remove_button);
  //   dt_accel_connect_button_lib(self, "delete from disk", d->delete_button);
  //   dt_accel_connect_button_lib(self, "rotate selected images 90 degrees CW",
  //                               d->rotate_cw_button);
  //   dt_accel_connect_button_lib(self, "rotate selected images 90 degrees CCW",
  //                               d->rotate_ccw_button);
  //   dt_accel_connect_button_lib(self, "create hdr", d->create_hdr_button);
  //   dt_accel_connect_button_lib(self, "duplicate", d->duplicate_button);
  //   dt_accel_connect_button_lib(self, "reset rotation", d->reset_button);
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
