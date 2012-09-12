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
#include "control/conf.h"
// #include "gui/accelerators.h"
// #include "gui/gtk.h"
#include "dtgtk/button.h"
// #include "dtgtk/paint.h"
// #include "libs/lib.h"
#include "control/jobs.h"

#include <gdk/gdkkeysyms.h>

DT_MODULE(1)

typedef struct dt_lib_geolocation_t
{
  GtkWidget *offset_entry;
  GList *timezones;
}
dt_lib_geolocation_t;

const char*
name ()
{
  return _("geolocation");
}

uint32_t views()
{
  return DT_VIEW_LIGHTTABLE;
}

uint32_t container()
{
  return DT_UI_CONTAINER_PANEL_RIGHT_CENTER;
}


int
position ()
{
  return 550;
}

/* try to parse the offset string. returns true if it worked, false otherwise.
 *if seconds != NULL the offset will be put there in seconds or 0 if it failed. always look at the return value before using the seconds value! */
static gboolean
_lib_geolocation_parse_offset(const char* str, long int *seconds)
{
  const gchar *str_bak = str;
  long int result = 0;
  int numbers[3] = {0, 0, 0};
  int fields = 0;
  char sign = '+';
  if(seconds) *seconds = 0;

  if(!str) return FALSE;

  size_t len = strlen(str);

  // optional sign
  if(*str == '+' || *str == '-') { sign = *str; str++; len--; }

  // hh, mm or ss
  if(len < 2) return FALSE;
  if(!g_ascii_isdigit(str[0]) || !g_ascii_isdigit(str[1])) return FALSE;
  else
  {
    numbers[fields++] = 10*(str[0]-'0')+(str[1]-'0');
    str += 2; len -= 2;
  }

  // : or end
  if(*str == '\0') goto parse_success;
  if(*str == ':') { str++; len--; }

  // mm or ss
  if(len < 2) return FALSE;
  if(!g_ascii_isdigit(str[0]) || !g_ascii_isdigit(str[1])) return FALSE;
  else
  {
    numbers[fields++] = 10*(str[0]-'0')+(str[1]-'0');
    str += 2; len -= 2;
  }

  // : or end
  if(*str == '\0') goto parse_success;
  if(*str == ':') { str++; len--; }

  // ss
  if(len < 2) return FALSE;
  if(!g_ascii_isdigit(str[0]) || !g_ascii_isdigit(str[1])) return FALSE;
  else
  {
    numbers[fields++] = 10*(str[0]-'0')+(str[1]-'0');
    str += 2; len -= 2;
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
        result = 60*numbers[0] + numbers[1];
        break;
      case 3: // 0: hours, 1: minutes, 2: seconds
        result = 60*60*numbers[0] + 60*numbers[1] + numbers[2];
        break;
      default: // shouldn't happen
        fprintf(stderr, "[geolocation] error: something went terribly wrong while parsing the offset, %d fields found in %s\n", fields, str_bak);
    }

    if(sign == '-') result *= -1;
    *seconds = result;
  }

  return TRUE;
}

static gboolean
_lib_geolocation_offset_key_press(GtkWidget *entry, GdkEventKey *event, dt_lib_module_t *self)
{
  dt_lib_geolocation_t *d = (dt_lib_geolocation_t*)self->data;
  switch(event->keyval) {
    case GDK_Escape:
    case GDK_Tab:{
      // reset
      gchar *str = dt_conf_get_string("plugins/lighttable/geolocation/offset");
      if(_lib_geolocation_parse_offset(str, NULL))
      {
        gtk_entry_set_text(GTK_ENTRY(d->offset_entry), str);
      }
      else
      {
        gtk_entry_set_text(GTK_ENTRY(d->offset_entry), "+00:00:00");
        dt_conf_set_string("plugins/lighttable/geolocation/offset", "+00:00:00");
      }
      g_free(str);
      gtk_window_set_focus(GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)), NULL);
      return FALSE;
    }

    case GDK_Return:
    case GDK_KP_Enter:{
      const gchar* value = gtk_entry_get_text(GTK_ENTRY(d->offset_entry));
      if(_lib_geolocation_parse_offset(value, NULL))
      {
        dt_conf_set_string("plugins/lighttable/geolocation/offset", value);
      }
      else
      {
        gtk_entry_set_text(GTK_ENTRY(d->offset_entry), "+00:00:00");
        dt_conf_set_string("plugins/lighttable/geolocation/offset", "+00:00:00");
      }
      gtk_window_set_focus(GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)), NULL);
      return FALSE;
    }

    // allow +, -, :, 0 .. 9, left/right/home/end movement using arrow keys and del/backspace
    case GDK_plus:
    case GDK_minus:
    case GDK_colon:
    case GDK_0:
    case GDK_KP_0:
    case GDK_1:
    case GDK_KP_1:
    case GDK_2:
    case GDK_KP_2:
    case GDK_3:
    case GDK_KP_3:
    case GDK_4:
    case GDK_KP_4:
    case GDK_5:
    case GDK_KP_5:
    case GDK_6:
    case GDK_KP_6:
    case GDK_7:
    case GDK_KP_7:
    case GDK_8:
    case GDK_KP_8:
    case GDK_9:
    case GDK_KP_9:

    case GDK_Left:
    case GDK_Right:
    case GDK_Home:
    case GDK_KP_Home:
    case GDK_End:
    case GDK_KP_End:
    case GDK_Delete:
    case GDK_BackSpace:
      return FALSE;

    default: // block everything else
      return TRUE;
  }
}

static gboolean
_lib_geolocation_offset_focus_out(GtkWidget *widget, GdkEvent *event, dt_lib_module_t *self)
{
  dt_lib_geolocation_t *d = (dt_lib_geolocation_t*)self->data;
  const gchar* value = gtk_entry_get_text(GTK_ENTRY(d->offset_entry));
  if(_lib_geolocation_parse_offset(value, NULL))
    dt_conf_set_string("plugins/lighttable/geolocation/offset", value);
  else
    gtk_entry_set_text(GTK_ENTRY(d->offset_entry), dt_conf_get_string("plugins/lighttable/geolocation/offset"));
  return FALSE;
}

// TODO:
// - show a popup or something like the quick tag line
// - selection is done the same as with copying of history stacks
// - entry boxes for offset, will be set when the button from 1. is clicked. should be stored in config
static void
_lib_geolocation_calculate_offset_callback(GtkWidget *widget, gpointer user_data)
{
}

// modify the datetime_taken field in the db/cache
static void
_lib_geolocation_apply_offset_callback(GtkWidget *widget, gpointer user_data)
{
  dt_lib_geolocation_t* l = (dt_lib_geolocation_t*)((dt_lib_module_t*)user_data)->data;
  long int offset = 0;
  _lib_geolocation_parse_offset(gtk_entry_get_text(GTK_ENTRY(l->offset_entry)), &offset);
  dt_control_time_offset(offset, -1);
}

static void
_lib_geolocation_gpx_callback(GtkWidget *widget, dt_lib_module_t *self)
{
  dt_lib_geolocation_t *d = (dt_lib_geolocation_t*)self->data;
  /* bring a filechooser to select the gpx file to apply to selection */
  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  GtkWidget *filechooser = gtk_file_chooser_dialog_new(_("open gpx file"),
                                                       GTK_WINDOW (win),
                                                       GTK_FILE_CHOOSER_ACTION_OPEN,
                                                       GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
                                                       GTK_STOCK_OPEN, GTK_RESPONSE_ACCEPT,
                                                       (char *)NULL);
  GtkFileFilter *filter;
  filter = GTK_FILE_FILTER(gtk_file_filter_new());
  gtk_file_filter_add_pattern(filter, "*.gpx");
  gtk_file_filter_set_name(filter, _("GPS Data Exchange Format"));
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(filechooser), filter);

  filter = GTK_FILE_FILTER(gtk_file_filter_new());
  gtk_file_filter_add_pattern(filter, "*");
  gtk_file_filter_set_name(filter, _("all files"));
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(filechooser), filter);

  // add time zone selection
  GtkWidget *extra_box = gtk_hbox_new(FALSE, 5);
  GtkWidget *label = gtk_label_new("camera time zone");
  g_object_set(G_OBJECT(label), "tooltip-text", _("most cameras don't store the time zone in exif. give the correct time zone so the gpx data can be correctly matched"), (char *)NULL);
  GtkWidget *tz_selection = gtk_combo_box_text_new();
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(tz_selection), "UTC");
  gtk_combo_box_set_active(GTK_COMBO_BOX(tz_selection), 0);

  GList *iter = d->timezones;
  int i = 0;
  gchar *old_tz= dt_conf_get_string("plugins/lighttable/geolocation/tz");
  if(iter)
  {
    do
    {
      i++;
      gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(tz_selection), (gchar*)iter->data);
      if(!strcmp((gchar*)iter->data, old_tz))
        gtk_combo_box_set_active(GTK_COMBO_BOX(tz_selection), i);
    } while( (iter = g_list_next(iter)) != NULL);
  }

  gtk_box_pack_start(GTK_BOX(extra_box), label, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(extra_box), tz_selection, FALSE, FALSE, 0);
  gtk_widget_show_all(extra_box);
  gtk_file_chooser_set_extra_widget(GTK_FILE_CHOOSER(filechooser), extra_box);

  if(gtk_dialog_run(GTK_DIALOG (filechooser)) == GTK_RESPONSE_ACCEPT)
  {
    gchar *tz = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(tz_selection));
    dt_conf_set_string("plugins/lighttable/geolocation/tz", tz);
    gchar *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER (filechooser));
    dt_control_gpx_apply(filename, -1, tz);
    g_free(filename);
    g_free(tz);
  }

  gtk_widget_destroy(extra_box);
  gtk_widget_destroy(filechooser);
//   dt_control_queue_redraw_center();
}

// create a list of possible time zones
// possible locations for zone.tab:
// - /usr/share/zoneinfo
// - /usr/lib/zoneinfo
// - getenv("TZDIR")
// - apparently on solaris there is no zones.tab. we need to collect the information ourselves like this:
//   /bin/grep -h ^Zone /usr/share/lib/zoneinfo/src/* | /bin/awk '{print "??\t+9999+99999\t" $2}'
static GList *
_lib_geolocation_get_timezones(void)
{
  GList *tz = NULL;
  FILE *fp;
  char *line = NULL;
  size_t len = 0;
  ssize_t read;

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
        // TODO: Solaris test
        return NULL;
      }
    }
  }

  // parse zone.tab and put all time zone descriptions into tz
  fp = fopen(zone_tab, "r");
  g_free(zone_tab);

  if(!fp)
    return NULL;

  while((read = getline(&line, &len, fp)) != -1)
  {
    if(line[0] == '#' || line[0] == '\0')
      continue;
    gchar **tokens = g_strsplit(line, "\t", 0);
    gchar *name = g_strdup(tokens[2]);
    g_strfreev(tokens);
    if(name[0] == '\0')
    {
      g_free(name);
      continue;
    }
    size_t last_char = strlen(name)-1;
    if(name[last_char] == '\n')
      name[last_char] = '\0';
    tz = g_list_append(tz, name);
  }

  g_free(line);
  fclose(fp);

  // sort tz
  tz = g_list_sort(tz, (GCompareFunc)g_strcmp0);

  return g_list_first(tz);
}

void
gui_init (dt_lib_module_t *self)
{
  dt_lib_geolocation_t *d = (dt_lib_geolocation_t *)malloc(sizeof(dt_lib_geolocation_t));
  self->data = (void *)d;
  d->timezones = _lib_geolocation_get_timezones();
  self->widget = gtk_vbox_new(TRUE, 5);
  GtkBox *hbox;
  GtkWidget *button, *label;

  /* the offset line */
  hbox = GTK_BOX(gtk_hbox_new(TRUE, 5));
  label = GTK_WIDGET(gtk_label_new(_("time offset")));
  gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
  gtk_box_pack_start(hbox, label, TRUE, TRUE, 0);

  d->offset_entry = gtk_entry_new();
  dt_gui_key_accel_block_on_focus(d->offset_entry);
  gtk_entry_set_max_length(GTK_ENTRY(d->offset_entry), 9);
  gtk_box_pack_start(hbox, d->offset_entry, TRUE, TRUE, 0);
  g_signal_connect(d->offset_entry, "key-press-event", G_CALLBACK(_lib_geolocation_offset_key_press), self);
  g_signal_connect(d->offset_entry, "focus-out-event", G_CALLBACK(_lib_geolocation_offset_focus_out), self);
  g_object_set(G_OBJECT(d->offset_entry), "tooltip-text", _("time offset\nformat: [+-]?[[hh:]mm:]ss"), (char *)NULL);
  gchar *str = dt_conf_get_string("plugins/lighttable/geolocation/offset");
  if(_lib_geolocation_parse_offset(str, NULL))
    gtk_entry_set_text(GTK_ENTRY(d->offset_entry), str);
  else
    gtk_entry_set_text(GTK_ENTRY(d->offset_entry), "+00:00:00");
  g_free(str);

  GtkBox *button_box = GTK_BOX(gtk_hbox_new(TRUE, 5));
  button = dtgtk_button_new(dtgtk_cairo_paint_zoom, 0);
  g_object_set(G_OBJECT(button), "tooltip-text", _("calculate the time offset from an image"), (char *)NULL);
  gtk_box_pack_start(button_box, button, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(_lib_geolocation_calculate_offset_callback), self);

  button = dtgtk_button_new(dtgtk_cairo_paint_check_mark, 0);
  g_object_set(G_OBJECT(button), "tooltip-text", _("apply time offset to selected images"), (char *)NULL);
  gtk_box_pack_start(button_box, button, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(_lib_geolocation_apply_offset_callback), self);

  gtk_box_pack_start(hbox, GTK_WIDGET(button_box), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(hbox), TRUE, TRUE, 0);

  /* gpx */
  button = gtk_button_new_with_label(_("apply gpx track file"));
  g_object_set(G_OBJECT(button), "tooltip-text", _("parses a gpx file and updates location of selected images"), (char *)NULL);
  gtk_box_pack_start(GTK_BOX(self->widget), button, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(_lib_geolocation_gpx_callback), self);

}

void
gui_cleanup (dt_lib_module_t *self)
{
  dt_lib_geolocation_t *d = (dt_lib_geolocation_t*)self->data;
  g_list_free_full(d->timezones, &g_free);
  d->timezones = NULL;
  g_free(self->data);
  self->data = NULL;
}

void init_key_accels(dt_lib_module_t *self)
{
//   dt_accel_register_lib(self, NC_("accel", "remove from collection"),
//                         GDK_Delete, 0);
//   dt_accel_register_lib(self, NC_("accel", "delete from disk"), 0, 0);
//   dt_accel_register_lib(self,
//                         NC_("accel", "rotate selected images 90 degrees cw"),
//                         0, 0);
//   dt_accel_register_lib(self,
//                         NC_("accel", "rotate selected images 90 degrees ccw"),
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
//   dt_accel_connect_button_lib(self, "rotate selected images 90 degrees cw",
//                               d->rotate_cw_button);
//   dt_accel_connect_button_lib(self, "rotate selected images 90 degrees ccw",
//                               d->rotate_ccw_button);
//   dt_accel_connect_button_lib(self, "create hdr", d->create_hdr_button);
//   dt_accel_connect_button_lib(self, "duplicate", d->duplicate_button);
//   dt_accel_connect_button_lib(self, "reset rotation", d->reset_button);
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
