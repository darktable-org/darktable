/*
    This file is part of darktable,
    Copyright (C) 2011-2024 darktable developers.

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
#include "dtgtk/thumbtable.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/preferences_dialogs.h"
#include "libs/collect.h"
#include "libs/lib.h"
#include "libs/lib_api.h"
#include <gdk/gdkkeysyms.h>

#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif

DT_MODULE(1)

/** this module stores recently used image collection queries and displays
 * them as one-click buttons to the user. */

static int _conf_get_max_saved_items()
{
  return (MAX(dt_conf_get_int("plugins/lighttable/recentcollect/max_items"),
              dt_conf_get_int("plugins/lighttable/collect/history_max")));
}

static int _conf_get_max_shown_items()
{
  return (dt_conf_get_int("plugins/lighttable/recentcollect/max_items"));
}

typedef struct dt_lib_recentcollect_item_t
{
  GtkWidget *button;
  int confid;
} dt_lib_recentcollect_item_t;

typedef struct dt_lib_recentcollect_t
{
  GtkWidget *box;
  int inited;
  // 1st is always most recently used entry (buttons stay fixed)
  GList *items;
} dt_lib_recentcollect_t;

const char *name(dt_lib_module_t *self)
{
  return _("recently used collections");
}

const char *description(dt_lib_module_t *self)
{
  return _("select among the most recent search\n"
           "criteria set in the collections module");
}

dt_view_type_flags_t views(dt_lib_module_t *self)
{
  return DT_VIEW_MULTI;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_LEFT_CENTER;
}

int position(const dt_lib_module_t *self)
{
  return 380;
}

static void pretty_print(const char *buf, char *out, size_t outsize)
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
    const int n = sscanf(buf, "%d:%d:%399[^$]", &mode, &item, str);

    if(n == 3)
    {
      if(k > 0) switch(mode)
        {
          case DT_LIB_COLLECT_MODE_AND:
            c = g_strlcpy(out, _(" and "), outsize);
            out += c;
            outsize -= c;
            break;
          case DT_LIB_COLLECT_MODE_OR:
            c = g_strlcpy(out, _(" or "), outsize);
            out += c;
            outsize -= c;
            break;
          default: // case DT_LIB_COLLECT_MODE_AND_NOT:
            c = g_strlcpy(out, _(" but not "), outsize);
            out += c;
            outsize -= c;
            break;
        }
      int i = 0;
      while(str[i] != '\0' && str[i] != '$') i++;
      if(str[i] == '$') str[i] = '\0';

      c = snprintf(out, outsize, "%s %s", item < DT_COLLECTION_PROP_LAST ? dt_collection_name(item) : "???",
                   item == 0 ? dt_image_film_roll_name(str) : str);
      out += c;
      outsize -= c;
    }
    while(buf[0] != '$' && buf[0] != '\0') buf++;
    if(buf[0] == '$') buf++;
  }
}

static void _button_pressed(GtkButton *button, dt_lib_module_t *self)
{
  dt_lib_recentcollect_t *d = self->data;

  // deserialize this button's preset
  int linenumber = 0;
  int found = FALSE;
  GList *k = d->items;
  while(k != NULL && !found)
  {
    GList *next = k->next;
    dt_lib_recentcollect_item_t *current = k->data;
    if(button == GTK_BUTTON(current->button))
      found = TRUE;
    else
    {
      k = next;
      linenumber++;
    }
  }

  if(!found) return;

  char confname[200];
  snprintf(confname, sizeof(confname), "plugins/lighttable/collect/history_pos%1d", linenumber);
  const int pos = dt_conf_get_int(confname);
  snprintf(confname, sizeof(confname), "plugins/lighttable/collect/history%1d", linenumber);
  const char *line = dt_conf_get_string_const(confname);
  if(line)
  {
    // we store the wanted offset which will be set by thumbtable on collection_change signal
    dt_conf_set_int("plugins/lighttable/collect/history_next_pos", pos);

    dt_collection_deserialize(line, FALSE);
  }
}

static void _lib_recentcollection_updated(gpointer instance, dt_collection_change_t query_change,
                                          dt_collection_properties_t changed_property, gpointer imgs, int next,
                                          dt_lib_module_t *self)
{
  dt_lib_recentcollect_t *d = self->data;

  // update button descriptions:
  char confname[200] = { 0 };
  GList *current = d->items;
  for(int k = 0; current; k++)
  {
    char str[2048] = { 0 };
    dt_lib_recentcollect_item_t *item = current->data;
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/history%1d", k);
    const char *line2 = dt_conf_get_string_const(confname);
    if(line2 && line2[0] != '\0') pretty_print(line2, str, sizeof(str));
    gtk_widget_set_tooltip_text(item->button, str);
    gtk_button_set_label(GTK_BUTTON(item->button), str);
    GtkWidget *child = gtk_bin_get_child(GTK_BIN(item->button));
    item->confid = k;
    if(child)
    {
      gtk_widget_set_halign(child, GTK_ALIGN_START);
      gtk_label_set_xalign(GTK_LABEL(child), 0.0); // without this the labels are not flush on the left
      gtk_label_set_ellipsize(GTK_LABEL(child), PANGO_ELLIPSIZE_END);
    }
    gtk_widget_set_no_show_all(item->button, TRUE);
    gtk_widget_set_visible(item->button, FALSE);
    current = g_list_next(current);
  }

  current = d->items;
  for(int k = 0; k < CLAMPS(_conf_get_max_shown_items(), 0, _conf_get_max_saved_items()) && current; k++)
  {
    dt_lib_recentcollect_item_t *item = current->data;
    const gchar *line = gtk_button_get_label(GTK_BUTTON(item->button));
    if(line && line[0] != '\0')
    {
      gtk_widget_set_no_show_all(item->button, FALSE);
      gtk_widget_set_visible(item->button, TRUE);
    }
    current = g_list_next(current);
  }
}

void _menuitem_preferences(GtkMenuItem *menuitem, dt_lib_module_t *self)
{
  char confname[200];
  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  GtkWidget *dialog = gtk_dialog_new_with_buttons(_("recent collections settings"), GTK_WINDOW(win),
                                                  GTK_DIALOG_DESTROY_WITH_PARENT,
                                                  _("_cancel"), GTK_RESPONSE_NONE,
                                                  _("_save"), GTK_RESPONSE_ACCEPT, NULL);
  gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
  dt_prefs_init_dialog_recentcollect(dialog);
  g_signal_connect(dialog, "key-press-event", G_CALLBACK(dt_handle_dialog_enter), NULL);

#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(dialog);
#endif
  gtk_widget_show_all(dialog);

  const int old_nb_items = _conf_get_max_saved_items(); // preserve previous value

  if(gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT)
  {
    dt_lib_recentcollect_t *d = self->data;

    const int new_nb_items = _conf_get_max_saved_items();
    const int delta = new_nb_items - old_nb_items;
    if(delta < 0)
    {
      // destroy old items
      GList *current = g_list_nth(d->items, new_nb_items);
      while(current)
      {
        dt_lib_recentcollect_item_t *item = current->data;
        snprintf(confname, sizeof(confname), "plugins/lighttable/collect/history%1d", item->confid);
        dt_conf_set_string(confname, "");
        snprintf(confname, sizeof(confname), "plugins/lighttable/collect/history_pos%1d", item->confid);
        dt_conf_set_int(confname, 0);
        gtk_widget_destroy(item->button);
        free(item);
        GList *old = current;
        current = g_list_next(current);
        d->items = g_list_delete_link(d->items, old);
      }
    }
    if(delta > 0)
    {
      // create new items
      for(int k = old_nb_items; k < new_nb_items; k++)
      {
        GtkWidget *box = GTK_WIDGET(d->box);
        dt_lib_recentcollect_item_t *item = malloc(sizeof(dt_lib_recentcollect_item_t));
        if(item)
        {
          d->items = g_list_append(d->items, item);
          item->button = gtk_button_new();
          gtk_box_pack_start(GTK_BOX(box), item->button, FALSE, TRUE, 0);
          g_signal_connect(G_OBJECT(item->button), "clicked", G_CALLBACK(_button_pressed), (gpointer)self);
          gtk_widget_set_no_show_all(item->button, TRUE);
          gtk_widget_set_name(GTK_WIDGET(item->button), "recent-collection-button");
          gtk_widget_set_visible(item->button, FALSE);
        }
      }
    }

    _lib_recentcollection_updated(NULL, DT_COLLECTION_CHANGE_NEW_QUERY, DT_COLLECTION_PROP_UNDEF, NULL, -1, self);
  }

  gtk_widget_destroy(dialog);
}

void set_preferences(void *menu, dt_lib_module_t *self)
{
  GtkWidget *mi = gtk_menu_item_new_with_label(_("preferences..."));
  g_signal_connect(G_OBJECT(mi), "activate", G_CALLBACK(_menuitem_preferences), self);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
}

void gui_reset(dt_lib_module_t *self)
{
  char confname[200] = { 0 };

  for(int k = 0; k < _conf_get_max_saved_items(); k++)
  {
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/history%1d", k);
    dt_conf_set_string(confname, "");
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/history_pos%1d", k);
    dt_conf_set_int(confname, 0);
  }
  _lib_recentcollection_updated(NULL, DT_COLLECTION_CHANGE_NEW_QUERY, DT_COLLECTION_PROP_UNDEF, NULL, -1, self);
}

void gui_init(dt_lib_module_t *self)
{
  dt_lib_recentcollect_t *d = malloc(sizeof(dt_lib_recentcollect_t));
  d->items = NULL;
  self->data = (void *)d;

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_container_add(GTK_CONTAINER(self->widget),
                    dt_ui_resize_wrap(box, 50, "plugins/lighttable/recentcollect/windowheight"));
  d->box = box;
  d->inited = 0;

  // add buttons in the list, set them all to invisible
  for(int k = 0; k < _conf_get_max_shown_items(); k++)
  {
    dt_lib_recentcollect_item_t *item = malloc(sizeof(dt_lib_recentcollect_item_t));
    d->items = g_list_append(d->items, item);
    item->button = gtk_button_new();
    gtk_box_pack_start(GTK_BOX(box), item->button, FALSE, TRUE, 0);
    g_signal_connect(G_OBJECT(item->button), "clicked", G_CALLBACK(_button_pressed), (gpointer)self);
    gtk_widget_set_no_show_all(item->button, TRUE);
    dt_gui_add_class(GTK_WIDGET(item->button), "dt_transparent_background");
    gtk_widget_set_name(GTK_WIDGET(item->button), "recent-collection-button");
    gtk_widget_set_visible(item->button, FALSE);
  }
  _lib_recentcollection_updated(NULL, DT_COLLECTION_CHANGE_NEW_QUERY, DT_COLLECTION_PROP_UNDEF, NULL, -1, self);

  /* connect collection changed signal */
  DT_CONTROL_SIGNAL_CONNECT(DT_SIGNAL_COLLECTION_CHANGED, _lib_recentcollection_updated, self);

  darktable.view_manager->proxy.module_recentcollect.module = self;
}

void gui_cleanup(dt_lib_module_t *self)
{
  const int curr_pos = dt_ui_thumbtable(darktable.gui->ui)->offset;
  dt_conf_set_int("plugins/lighttable/collect/history_pos0", curr_pos);
  DT_CONTROL_SIGNAL_DISCONNECT(_lib_recentcollection_updated, self);
  free(self->data);
  self->data = NULL;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
