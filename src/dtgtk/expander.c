/*
    This file is part of darktable,
    Copyright (C) 2015-2021 darktable developers.

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

#include "dtgtk/expander.h"
#include "common/darktable.h"
#include "control/conf.h"
#include "gui/gtk.h"
#include "libs/lib.h"

#include <gtk/gtk.h>

G_DEFINE_TYPE(GtkDarktableExpander, dtgtk_expander, GTK_TYPE_BOX);

static void dtgtk_expander_class_init(GtkDarktableExpanderClass *class)
{
}

GtkWidget *dtgtk_expander_get_frame(GtkDarktableExpander *expander)
{
  g_return_val_if_fail(DTGTK_IS_EXPANDER(expander), NULL);

  return gtk_bin_get_child(GTK_BIN(expander->frame));
}

GtkWidget *dtgtk_expander_get_header(GtkDarktableExpander *expander)
{
  g_return_val_if_fail(DTGTK_IS_EXPANDER(expander), NULL);

  return expander->header;
}

GtkWidget *dtgtk_expander_get_header_event_box(GtkDarktableExpander *expander)
{
  g_return_val_if_fail(DTGTK_IS_EXPANDER(expander), NULL);

  return expander->header_evb;
}

GtkWidget *dtgtk_expander_get_body(GtkDarktableExpander *expander)
{
  g_return_val_if_fail(DTGTK_IS_EXPANDER(expander), NULL);

  return expander->body;
}

GtkWidget *dtgtk_expander_get_body_event_box(GtkDarktableExpander *expander)
{
  g_return_val_if_fail(DTGTK_IS_EXPANDER(expander), NULL);

  return expander->body_evb;
}

static GtkAllocation _start_pos = {0};

void dtgtk_expander_set_expanded(GtkDarktableExpander *expander, gboolean expanded)
{
  g_return_if_fail(DTGTK_IS_EXPANDER(expander));

  expanded = expanded != FALSE;

  if(expander->expanded != expanded)
  {
    expander->expanded = expanded;

    GtkWidget *frame = expander->body;

    if(expanded && gtk_widget_get_mapped(GTK_WIDGET(expander)))
    {
      GtkWidget *sw = gtk_widget_get_parent(gtk_widget_get_parent(gtk_widget_get_parent(GTK_WIDGET(expander))));
      if(GTK_IS_SCROLLED_WINDOW(sw))
      {
        gtk_widget_get_allocation(GTK_WIDGET(expander), &_start_pos);
        _start_pos.x = gtk_adjustment_get_value(gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(sw)));
      }
    }

    if(frame)
    {
      gtk_widget_set_visible(frame, TRUE); // for collapsible sections
      gtk_revealer_set_transition_duration(GTK_REVEALER(expander->frame), dt_conf_get_int("darkroom/ui/transition_duration"));
      gtk_revealer_set_reveal_child(GTK_REVEALER(expander->frame), expander->expanded);
    }
  }
}

gboolean dtgtk_expander_get_expanded(GtkDarktableExpander *expander)
{
  g_return_val_if_fail(DTGTK_IS_EXPANDER(expander), FALSE);

  return expander->expanded;
}

static GtkWidget *_scroll_widget = NULL;

static gboolean _expander_scroll(GtkWidget *widget, GdkFrameClock *frame_clock, gpointer user_data)
{
  GtkWidget *scrolled_window = gtk_widget_get_parent(gtk_widget_get_parent(gtk_widget_get_parent(widget)));
  g_return_val_if_fail(GTK_IS_SCROLLED_WINDOW(scrolled_window), G_SOURCE_REMOVE);

  GtkAllocation allocation, available;
  gtk_widget_get_allocation(widget, &allocation);
  gtk_widget_get_allocation(scrolled_window, &available);

  GtkAdjustment *adjustment = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scrolled_window));
  gdouble value = gtk_adjustment_get_value(adjustment);

  if(allocation.y < _start_pos.y)
  {
    const int offset = _start_pos.y - allocation.y - _start_pos.x + value;
    value -= offset;
    _start_pos.y = allocation.y;
  }

  float prop = 1.0f;
  const gboolean scroll_to_top = dt_conf_get_bool("darkroom/ui/scroll_to_module");
  const int spare = available.height - allocation.height;
  const int from_top = allocation.y - value;
  const int move = MAX(from_top - scroll_to_top ? 0 : MAX(0, MIN(from_top, spare)),
                       - MAX(0, spare - from_top));
  if(move)
  {
    gint64 interval = 0;
    gdk_frame_clock_get_refresh_info(frame_clock, 0, &interval, NULL);
    const int remaining = GPOINTER_TO_INT(user_data) - gdk_frame_clock_get_frame_time(frame_clock);
    prop = (float)interval / MAX(interval, remaining);
    value += prop * move;
  }

  _start_pos.x = value;
  gtk_adjustment_set_value(adjustment, value);

  if(prop != 1.0f) return G_SOURCE_CONTINUE;

  _scroll_widget = NULL;
  return G_SOURCE_REMOVE;
}

static void _expander_resize(GtkWidget *widget, GdkRectangle *allocation, gpointer user_data)
{
  if(widget == _scroll_widget ||
     (!(gtk_widget_get_state_flags(user_data) & GTK_STATE_FLAG_SELECTED) &&
     (!darktable.lib->gui_module || darktable.lib->gui_module->expander != widget)))
    return;

  _scroll_widget = widget;
  gtk_widget_add_tick_callback(widget, _expander_scroll,
                               GINT_TO_POINTER(gdk_frame_clock_get_frame_time(gtk_widget_get_frame_clock(widget))
                               + dt_conf_get_int("darkroom/ui/transition_duration") * 1000), NULL);
}

static void dtgtk_expander_init(GtkDarktableExpander *expander)
{
}

// public functions
GtkWidget *dtgtk_expander_new(GtkWidget *header, GtkWidget *body)
{
  GtkDarktableExpander *expander;

  g_return_val_if_fail(GTK_IS_WIDGET(header), NULL);
  g_return_val_if_fail(GTK_IS_WIDGET(body), NULL);

  expander
      = g_object_new(dtgtk_expander_get_type(), "orientation", GTK_ORIENTATION_VERTICAL, "spacing", 0, NULL);
  expander->expanded = -1;
  expander->header = header;
  expander->body = body;

  expander->header_evb = gtk_event_box_new();
  gtk_container_add(GTK_CONTAINER(expander->header_evb), expander->header);
  expander->body_evb = gtk_event_box_new();
  gtk_container_add(GTK_CONTAINER(expander->body_evb), expander->body);
  GtkWidget *frame = gtk_frame_new(NULL);
  gtk_container_add(GTK_CONTAINER(frame), expander->body_evb);
  expander->frame = gtk_revealer_new();
  gtk_container_add(GTK_CONTAINER(expander->frame), frame);

  gtk_box_pack_start(GTK_BOX(expander), expander->header_evb, TRUE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(expander), expander->frame, TRUE, FALSE, 0);

  g_signal_connect(G_OBJECT(expander), "size-allocate", G_CALLBACK(_expander_resize), frame);

  return GTK_WIDGET(expander);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

