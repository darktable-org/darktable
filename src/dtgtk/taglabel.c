/*
    This file is part of darktable,
    Copyright (C) 2024 darktable developers.

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

#include "taglabel.h"
#include "gui/gtk.h"
#include <string.h>

G_DEFINE_TYPE(GtkDarktableTagLabel, dtgtk_tag_label, GTK_TYPE_FLOW_BOX_CHILD);

static void dtgtk_tag_label_class_init(GtkDarktableTagLabelClass *klass)
{
}

static void dtgtk_tag_label_init(GtkDarktableTagLabel *tag_label)
{
}

static gboolean _tag_label_enter_leave_notify_callback(GtkWidget *widget,
                                                       GdkEventCrossing *event,
                                                       gpointer user_data)
{
  g_return_val_if_fail(widget != NULL, FALSE);

  GtkWidget *tag_label = gtk_widget_get_parent(widget);
  g_return_val_if_fail(tag_label != NULL, FALSE);

  if(event->type == GDK_ENTER_NOTIFY)
    dt_gui_add_class(tag_label, "hover");
  else
    dt_gui_remove_class(tag_label, "hover");
  return FALSE;
}

static gboolean _tag_label_button_press_notify_callback(GtkWidget *widget,
                                                        GdkEventButton *event,
                                                        gpointer user_data)
{
  g_return_val_if_fail(widget != NULL, FALSE);

  if(event->type == GDK_BUTTON_PRESS && event->button == 3)
  {
    GtkFlowBoxChild *tag_label = GTK_FLOW_BOX_CHILD(gtk_widget_get_parent(widget));
    g_return_val_if_fail(tag_label != NULL, FALSE);

    GtkFlowBox *flow_box = GTK_FLOW_BOX(gtk_widget_get_parent(GTK_WIDGET(tag_label)));
    g_return_val_if_fail(flow_box != NULL, FALSE);

    gtk_flow_box_select_child(flow_box, tag_label);
  }
  return FALSE;
}

// Public functions
GtkWidget *dtgtk_tag_label_new(const dt_tag_t *tag)
{
  GtkDarktableTagLabel *tag_label;
  tag_label = g_object_new(dtgtk_tag_label_get_type(), NULL);
  
  tag_label->tagid = tag->id;

  GtkWidget *event_box = gtk_event_box_new();
  gtk_container_add(GTK_CONTAINER(tag_label), event_box);
  gtk_event_box_set_visible_window(GTK_EVENT_BOX(event_box), FALSE);
  gtk_widget_set_events(event_box, 
                        GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK 
                        | GDK_BUTTON_PRESS_MASK);
  g_signal_connect(G_OBJECT(event_box), "enter-notify-event",
                   G_CALLBACK(_tag_label_enter_leave_notify_callback), NULL);
  g_signal_connect(G_OBJECT(event_box), "leave-notify-event",
                   G_CALLBACK(_tag_label_enter_leave_notify_callback), NULL);
  g_signal_connect(G_OBJECT(event_box), "button-press-event",
                   G_CALLBACK(_tag_label_button_press_notify_callback), NULL);
        
  GtkWidget *label = gtk_label_new(tag->leave);
  gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_START);
  gtk_label_set_max_width_chars(GTK_LABEL(label), 10);
  gtk_container_add(GTK_CONTAINER(event_box), label);
  
  gtk_widget_show_all(GTK_WIDGET(tag_label));

  gtk_widget_set_name(GTK_WIDGET(tag_label), "tag-label");

  GtkStyleContext *context = gtk_widget_get_style_context(GTK_WIDGET(tag_label));
  if(!dt_tag_is_user_tag(tag))
    gtk_style_context_add_class(context, "darktable");

  if(tag->flags & DT_TF_CATEGORY)
    gtk_style_context_add_class(context, "category");

  if(tag->flags & DT_TF_PRIVATE)
    gtk_style_context_add_class(context, "private");

  if(tag->select == DT_TS_SOME_IMAGES)
    gtk_style_context_add_class(context, "some");

  gtk_widget_set_tooltip_text(GTK_WIDGET(tag_label), tag->tag);
  return (GtkWidget *)tag_label;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

