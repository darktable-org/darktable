/*
    This file is part of darktable,
    copyright (c) 2009--2011 johannes hanika.

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
#include "control/conf.h"
#include "control/signal.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "libs/collect.h"
#include "libs/lib.h"
#include "libs/lib_api.h"

#include <gdk/gdkkeysyms.h>

DT_MODULE(1)

/** this module stores recently used image collection queries and displays
  * them as one-click buttons to the user. */

#define NUM_LINES 10

typedef struct dt_lib_recentcollect_item_t
{
  GtkWidget *button;
} dt_lib_recentcollect_item_t;

typedef struct dt_lib_recentcollect_t
{
  // 1st is always most recently used entry (buttons stay fixed)
  dt_lib_recentcollect_item_t item[NUM_LINES];
  int inited;
} dt_lib_recentcollect_t;

const char *name(dt_lib_module_t *self)
{
  return _("recently used collections");
}

const char **views(dt_lib_module_t *self)
{
  static const char *v[] = {"lighttable", "map", NULL};
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_LEFT_CENTER;
}

int position()
{
  return 350;
}

static gboolean _goto_previous(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                               GdkModifierType modifier, gpointer data)
{
  gchar *line = dt_conf_get_string("plugins/lighttable/recentcollect/line1");
  if(line)
  {
    dt_collection_deserialize(line);
    g_free(line);
  }
  return TRUE;
}

void init_key_accels(dt_lib_module_t *self)
{
  dt_accel_register_lib(self, NC_("accel", "jump back to previous collection"), GDK_KEY_k, GDK_CONTROL_MASK);
}

void connect_key_accels(dt_lib_module_t *self)
{
  GClosure *closure = g_cclosure_new(G_CALLBACK(_goto_previous), (gpointer)self, NULL);
  dt_accel_connect_lib(self, "jump back to previous collection", closure);
}


static void pretty_print(char *buf, char *out, size_t outsize)
{
  memset(out, 0, outsize);

  if(!buf || buf[0] == '\0') return;

  int num_rules = 0;
  char str[400] = { 0 };
  int mode, item;
  int c;
  sscanf(buf, "%d", &num_rules);
  while(buf[0] != '\0' && buf[0] != ':') buf++;
  if(buf[0] == ':') buf++;

  for(int k = 0; k < num_rules; k++)
  {
    int n = sscanf(buf, "%d:%d:%399[^$]", &mode, &item, str);

    if(n == 3)
    {
      if(k > 0) switch(mode)
        {
          case DT_LIB_COLLECT_MODE_AND:
            c = snprintf(out, outsize, "%s", _(" and "));
            out += c;
            outsize -= c;
            break;
          case DT_LIB_COLLECT_MODE_OR:
            c = snprintf(out, outsize, "%s", _(" or "));
            out += c;
            outsize -= c;
            break;
          default: // case DT_LIB_COLLECT_MODE_AND_NOT:
            c = snprintf(out, outsize, "%s", _(" but not "));
            out += c;
            outsize -= c;
            break;
        }
      int i = 0;
      while(str[i] != '\0' && str[i] != '$') i++;
      if(str[i] == '$') str[i] = '\0';

      c = snprintf(out, outsize, "%s %s", item < dt_lib_collect_string_cnt ? _(dt_lib_collect_string[item]) : "???",
                   item == 0 ? dt_image_film_roll_name(str) : str);
      out += c;
      outsize -= c;
    }
    while(buf[0] != '$' && buf[0] != '\0') buf++;
    if(buf[0] == '$') buf++;
  }
}

static void _button_pressed(GtkButton *button, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_recentcollect_t *d = (dt_lib_recentcollect_t *)self->data;

  // deserialize this button's preset
  int n = -1;
  for(int k = 0; k < NUM_LINES; k++)
  {
    if(button == GTK_BUTTON(d->item[k].button))
    {
      n = k;
      break;
    }
  }
  if(n < 0) return;
  char confname[200];
  snprintf(confname, sizeof(confname), "plugins/lighttable/recentcollect/line%1d", n);
  gchar *line = dt_conf_get_string(confname);
  if(line)
  {
    dt_collection_deserialize(line);
    g_free(line);
    // position will be updated when the list of recent collections is.
    // that way it'll also catch cases when this is triggered by a signal,
    // not only our button press here.
  }
}

static void _lib_recentcollection_updated(gpointer instance, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_recentcollect_t *d = (dt_lib_recentcollect_t *)self->data;
  // serialize, check for recently used
  char confname[200];

  char buf[4096];
  if(dt_collection_serialize(buf, sizeof(buf))) return;

  // is the current position, i.e. the one to be stored with the old collection (pos0, pos1-to-be)
  uint32_t curr_pos = dt_view_lighttable_get_position(darktable.view_manager);
  uint32_t new_pos = -1;

  if(!d->inited)
  {
    new_pos = dt_conf_get_int("plugins/lighttable/recentcollect/pos0");
    d->inited = 1;
    dt_view_lighttable_set_position(darktable.view_manager, new_pos);
  }
  else if(curr_pos != -1)
  {
    dt_conf_set_int("plugins/lighttable/recentcollect/pos0", curr_pos);
  }

  int n = -1;
  for(int k = 0; k < CLAMPS(dt_conf_get_int("plugins/lighttable/recentcollect/num_items"), 0, NUM_LINES); k++)
  {
    // is it already in the current list?
    snprintf(confname, sizeof(confname), "plugins/lighttable/recentcollect/line%1d", k);
    gchar *line = dt_conf_get_string(confname);
    if(!line) continue;
    if(!strcmp(line, buf))
    {
      snprintf(confname, sizeof(confname), "plugins/lighttable/recentcollect/pos%1d", k);
      new_pos = dt_conf_get_int(confname);
      n = k;
      g_free(line);
      break;
    }
    g_free(line);
  }
  if(n < 0)
  {
    const int num_items = CLAMPS(dt_conf_get_int("plugins/lighttable/recentcollect/num_items"), 0, NUM_LINES);
    if(num_items < NUM_LINES)
    {
      // new, unused entry
      n = num_items;
      dt_conf_set_int("plugins/lighttable/recentcollect/num_items", num_items + 1);
    }
    else
    {
      // kill least recently used entry:
      n = num_items - 1;
    }
  }
  if(n >= 0 && n < NUM_LINES)
  {
    // sort n to the top
    for(int k = n; k > 0; k--)
    {
      snprintf(confname, sizeof(confname), "plugins/lighttable/recentcollect/line%1d", k - 1);
      gchar *line1 = dt_conf_get_string(confname);
      snprintf(confname, sizeof(confname), "plugins/lighttable/recentcollect/pos%1d", k - 1);
      uint32_t pos1 = dt_conf_get_int(confname);
      if(line1 && line1[0] != '\0')
      {
        snprintf(confname, sizeof(confname), "plugins/lighttable/recentcollect/line%1d", k);
        dt_conf_set_string(confname, line1);
        snprintf(confname, sizeof(confname), "plugins/lighttable/recentcollect/pos%1d", k);
        dt_conf_set_int(confname, pos1);
      }
      g_free(line1);
    }
    dt_conf_set_string("plugins/lighttable/recentcollect/line0", buf);
    dt_conf_set_int("plugins/lighttable/recentcollect/pos0",
                    (new_pos != -1 ? new_pos : (curr_pos != -1 ? curr_pos : 0)));
  }
  // update button descriptions:
  for(int k = 0; k < NUM_LINES; k++)
  {
    char str[2048] = { 0 };
    snprintf(confname, sizeof(confname), "plugins/lighttable/recentcollect/line%1d", k);
    gchar *line2 = dt_conf_get_string(confname);
    if(line2 && line2[0] != '\0') pretty_print(line2, str, sizeof(str));
    g_free(line2);
    gtk_widget_set_tooltip_text(d->item[k].button, str);
    gtk_button_set_label(GTK_BUTTON(d->item[k].button), str);
    GtkWidget *child = gtk_bin_get_child(GTK_BIN(d->item[k].button));
    if(child)
    {
      gtk_widget_set_halign(child, GTK_ALIGN_START);
#if GTK_CHECK_VERSION(3, 16, 0)
      gtk_label_set_xalign(GTK_LABEL(child), 0.0); // without this the labels are not flush on the left
#endif
      gtk_label_set_ellipsize(GTK_LABEL(child), PANGO_ELLIPSIZE_END);
    }
    gtk_widget_set_no_show_all(d->item[k].button, TRUE);
    gtk_widget_set_visible(d->item[k].button, FALSE);
  }
  for(int k = 0; k < CLAMPS(dt_conf_get_int("plugins/lighttable/recentcollect/num_items"), 0, NUM_LINES); k++)
  {
    gtk_widget_set_no_show_all(d->item[k].button, FALSE);
    gtk_widget_set_visible(d->item[k].button, TRUE);
  }
  if((new_pos != -1) && (new_pos != curr_pos))
    dt_view_lighttable_set_position(darktable.view_manager, new_pos);
}

void gui_reset(dt_lib_module_t *self)
{
  dt_conf_set_int("plugins/lighttable/recentcollect/num_items", 0);
  char confname[200];
  for(int k = 0; k < NUM_LINES; k++)
  {
    snprintf(confname, sizeof(confname), "plugins/lighttable/recentcollect/line%1d", k);
    dt_conf_set_string(confname, "");
    snprintf(confname, sizeof(confname), "plugins/lighttable/recentcollect/pos%1d", k);
    dt_conf_set_int(confname, 0);
  }
  _lib_recentcollection_updated(NULL, self);
}

void gui_init(dt_lib_module_t *self)
{
  dt_lib_recentcollect_t *d = (dt_lib_recentcollect_t *)calloc(1, sizeof(dt_lib_recentcollect_t));
  self->data = (void *)d;
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  dt_gui_add_help_link(self->widget, dt_get_help_url(self->plugin_name));
  d->inited = 0;

  gtk_widget_set_name(self->widget, "recent-collection-ui");

  // add buttons in the list, set them all to invisible
  for(int k = 0; k < NUM_LINES; k++)
  {
    d->item[k].button = gtk_button_new();
    gtk_box_pack_start(GTK_BOX(self->widget), d->item[k].button, FALSE, TRUE, 0);
    g_signal_connect(G_OBJECT(d->item[k].button), "clicked", G_CALLBACK(_button_pressed), (gpointer)self);
    gtk_widget_set_no_show_all(d->item[k].button, TRUE);
    gtk_widget_set_visible(d->item[k].button, FALSE);
  }
  _lib_recentcollection_updated(NULL, self);

  /* connect collection changed signal */
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_COLLECTION_CHANGED,
                            G_CALLBACK(_lib_recentcollection_updated), (gpointer)self);
}

void gui_cleanup(dt_lib_module_t *self)
{
  uint32_t curr_pos = dt_view_lighttable_get_position(darktable.view_manager);
  dt_conf_set_int("plugins/lighttable/recentcollect/pos0", curr_pos);
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(_lib_recentcollection_updated), self);
  free(self->data);
  self->data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
