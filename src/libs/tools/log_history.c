/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

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

#include "control/signal.h"
#include "control/control.h"
#include "dtgtk/button.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include "libs/lib_api.h"

DT_MODULE(1)

typedef struct dt_lib_log_history_t
{
  GtkWidget *button;
  GtkWidget *popover;
  GtkWidget *list_box;
  GtkWidget *scrolled;
} dt_lib_log_history_t;

static void _populate_list_box(dt_lib_module_t *self);
static void _log_redraw_callback(gpointer instance, dt_lib_module_t *self);
static gboolean _button_press_release(GtkWidget *button,
                                      GdkEventButton *event,
                                      dt_lib_module_t *self);
static gboolean _suppress_popup(GtkWidget *widget,
                                gpointer user_data);
static gboolean _label_button_press(GtkWidget *widget,
                                    GdkEventButton *event,
                                    gpointer user_data);

const char *name(dt_lib_module_t *self)
{
  return N_("log history");
}

dt_view_type_flags_t views(dt_lib_module_t *self)
{
  return DT_VIEW_ALL;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_CENTER_BOTTOM_RIGHT;
}

gboolean expandable(dt_lib_module_t *self)
{
  return FALSE;
}

int position(const dt_lib_module_t *self)
{
  return 1000;
}

static void _populate_list_box(dt_lib_module_t *self)
{
  dt_lib_log_history_t *d = self->data;
  if(!d || !d->list_box) return;

  // Remove existing rows
  GList *children = gtk_container_get_children(GTK_CONTAINER(d->list_box));
  for(GList *elem = children; elem; elem = elem->next)
    gtk_widget_destroy(GTK_WIDGET(elem->data));
  g_list_free(children);

  GList *entries = dt_control_log_history_get_entries();

  if(entries)
  {
    entries = g_list_reverse(entries); // show newest entries at the top

     for(GList *elem = g_list_first(entries);
         elem;
         elem = g_list_next(elem))
     {
       gchar *line = (gchar *)elem->data;
       GtkWidget *label = gtk_label_new(line);
       gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
       gtk_label_set_selectable(GTK_LABEL(label), TRUE);
       dt_gui_add_class(label, "dt_monospace");
       g_signal_connect(G_OBJECT(label), "button-press-event",
                       G_CALLBACK(_label_button_press), NULL);
       g_signal_connect(G_OBJECT(label), "popup-menu",
                       G_CALLBACK(_suppress_popup), NULL);
       gtk_list_box_insert(GTK_LIST_BOX(d->list_box), label, -1);
     }
    g_list_free_full(entries, g_free);
  }
  gtk_widget_show_all(d->popover);
}

static void _log_redraw_callback(gpointer instance, dt_lib_module_t *self)
{
  dt_lib_log_history_t *d = self->data;
  if(d->popover && gtk_widget_is_visible(d->popover))
    _populate_list_box(self);
}

static gboolean _suppress_popup(GtkWidget *widget,
                                gpointer user_data)
{
  return TRUE;
}

static gboolean _label_button_press(GtkWidget *widget,
                                    GdkEventButton *event,
                                    gpointer user_data)
{
  if(event->button == GDK_BUTTON_SECONDARY)
    return TRUE;
  return FALSE;
}

static gboolean _button_press_release(GtkWidget *button,
                                      GdkEventButton *event,
                                      dt_lib_module_t *self)
{
  static guint start_time = 0;

  int delay = 0;
  g_object_get(gtk_settings_get_default(), "gtk-long-press-time", &delay, NULL);

  if((event->type == GDK_BUTTON_PRESS
      && (event->button == GDK_BUTTON_PRIMARY
          || event->button == GDK_BUTTON_SECONDARY))
     || (event->type == GDK_BUTTON_RELEASE
         && event->time - start_time > delay))
  {
    dt_lib_log_history_t *d = self->data;
    if(gtk_widget_is_visible(d->popover))
      gtk_popover_popdown(GTK_POPOVER(d->popover));
    else
    {
      _populate_list_box(self);
      gtk_popover_popup(GTK_POPOVER(d->popover));
    }
    return TRUE;
  }
  else
  {
    start_time = event->time;
    return FALSE;
  }
}

void gui_init(dt_lib_module_t *self)
{
  dt_lib_log_history_t *d = g_malloc0(sizeof(dt_lib_log_history_t));
  self->data = d;

  d->button = dtgtk_button_new(dtgtk_cairo_paint_messages, CPF_NONE, NULL);
  gtk_widget_set_tooltip_text(d->button, _("view log history"));
  dt_gui_add_help_link(d->button, "message_log");

  d->popover = gtk_popover_new(d->button);
  gtk_widget_set_name(d->popover, "log-history-popover");
  gtk_popover_set_position(GTK_POPOVER(d->popover), GTK_POS_TOP);

  d->list_box = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(d->list_box), GTK_SELECTION_NONE);
  gtk_widget_set_name(d->list_box, "log-history-list");

  d->scrolled = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(d->scrolled),
                                 GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(d->scrolled),
                                             DT_PIXEL_APPLY_DPI(200));

  gtk_widget_set_size_request(d->scrolled,
                              DT_PIXEL_APPLY_DPI(700),
                              DT_PIXEL_APPLY_DPI(200));

  gtk_container_add(GTK_CONTAINER(d->scrolled), d->list_box);
  gtk_container_add(GTK_CONTAINER(d->popover), d->scrolled);

  g_signal_connect(G_OBJECT(d->button), "button-press-event",
                   G_CALLBACK(_button_press_release), self);
  g_signal_connect(G_OBJECT(d->button), "button-release-event",
                   G_CALLBACK(_button_press_release), self);

  self->widget = dt_gui_hbox(d->button);

  DT_CONTROL_SIGNAL_CONNECT(DT_SIGNAL_CONTROL_LOG_REDRAW,
                            G_CALLBACK(_log_redraw_callback), self);
}

void gui_cleanup(dt_lib_module_t *self)
{
  dt_lib_log_history_t *d = self->data;

  DT_CONTROL_SIGNAL_DISCONNECT(G_CALLBACK(_log_redraw_callback), self);

  if(d->popover)
    gtk_widget_destroy(d->popover);

  g_free(d);
  self->data = NULL;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
