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
#include "control/conf.h"

#include <gdk/gdk.h>
#include <gtk/gtk.h>

G_DEFINE_TYPE(GtkDarktableExpander, dtgtk_expander, GTK_TYPE_BOX);

static void dtgtk_expander_class_init(GtkDarktableExpanderClass *class)
{
}

GtkWidget *dtgtk_expander_get_frame(GtkDarktableExpander *expander)
{
  g_return_val_if_fail(DTGTK_IS_EXPANDER(expander), NULL);

  return expander->frame;
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

void dtgtk_expander_set_expanded(GtkDarktableExpander *expander, gboolean expanded)
{
  g_return_if_fail(DTGTK_IS_EXPANDER(expander));

  expanded = expanded != FALSE;

  if(expander->expanded != expanded)
  {
    expander->expanded = expanded;

    GtkWidget *revealer = expander->revealer;

    if(revealer)
    {
      uint32_t transition_duration = dt_conf_get_int("ui/transition_duration");
      gtk_revealer_set_transition_duration(GTK_REVEALER(expander->revealer), transition_duration);
      gtk_revealer_set_reveal_child(GTK_REVEALER(revealer), expander->expanded);
    }
  }
}

gboolean dtgtk_expander_get_expanded(GtkDarktableExpander *expander)
{
  g_return_val_if_fail(DTGTK_IS_EXPANDER(expander), FALSE);

  return expander->expanded;
}

void dtgtk_expander_set_transition_duration(GtkDarktableExpander *expander, guint duration)
{
  g_return_if_fail(DTGTK_IS_EXPANDER(expander));

  gtk_revealer_set_transition_duration(GTK_REVEALER(expander->revealer), duration);
}

guint dtgtk_expander_get_transition_duration(GtkDarktableExpander *expander)
{
  g_return_val_if_fail(DTGTK_IS_EXPANDER(expander), 0);

  return gtk_revealer_get_transition_duration(GTK_REVEALER(expander->revealer));
}

static void dtgtk_expander_init(GtkDarktableExpander *expander)
{
}

/* From clutter-easing.c, based on Robert Penner's
 * infamous easing equations, MIT license.
 */
gdouble ease_out_cubic(gdouble t)
{
  gdouble p = t - 1;
  return p * p * p + 1;
}

typedef struct _smoothScrollData
{
  GtkAdjustment *adjustment;
  gdouble start;
  gdouble end;
  gint64 start_time;
  gint64 end_time;
} smoothScrollData;

static gboolean _scrolled_window_tick_callback(GtkWidget *scrolled_window, GdkFrameClock *clock, gpointer user_data)
{
  smoothScrollData *data = (smoothScrollData *)user_data;
  if(!GTK_IS_ADJUSTMENT(data->adjustment)) return G_SOURCE_REMOVE;

  gint64 now = gdk_frame_clock_get_frame_time(clock);
  gdouble current_pos = gtk_adjustment_get_value(data->adjustment);

  if(now < data->end_time && current_pos != data->end)
  {
    gdouble t = (gdouble)(now - data->start_time) / (gdouble)(data->end_time - data->start_time);
    t = ease_out_cubic(t);

    if(data->start > data->end)
      gtk_adjustment_set_value(data->adjustment, data->start - t * (data->start - data->end));
    else
      gtk_adjustment_set_value(data->adjustment, data->start + t * (data->end - data->start));

    return G_SOURCE_CONTINUE;
  }
  else
  {
    gtk_adjustment_set_value(data->adjustment, data->end);

    return G_SOURCE_REMOVE;
  }
}

void _scrolled_window_smooth_scroll(GtkWidget *scrolled_window, int target, guint duration)
{
  GtkAdjustment *adjustment = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scrolled_window));
  if(!GTK_IS_ADJUSTMENT(adjustment)) return;

  GdkFrameClock *clock = gtk_widget_get_frame_clock(scrolled_window);
  if(!clock) return;

  gdouble start = gtk_adjustment_get_value(adjustment);
  gdouble end = target;
  gint64 start_time = gdk_frame_clock_get_frame_time(clock);
  gint64 end_time = start_time + 1000 * duration;

  smoothScrollData *data = malloc(sizeof(smoothScrollData));
  data->adjustment = adjustment;
  data->start = start;
  data->end = end;
  data->start_time = start_time;
  data->end_time = end_time;

  gtk_widget_add_tick_callback(scrolled_window, (GtkTickCallback)_scrolled_window_tick_callback, data, NULL);
}

// this should work as long as everything happens in the gui thread
static void dtgtk_expander_scroll(GtkRevealer *widget)
{
  if(!dt_conf_get_bool("lighttable/ui/scroll_to_module") && !dt_conf_get_bool("darkroom/ui/scroll_to_module"))
    return;

  GtkAllocation allocation;

  GtkWidget *expander = gtk_widget_get_parent(GTK_WIDGET(widget));
  GtkWidget *box = gtk_widget_get_parent(expander);
  if(!GTK_IS_WIDGET(box)) return; // it may not have been added to a box yet

  GtkWidget *viewport = gtk_widget_get_parent(box);
  GtkWidget *scrolled_window = gtk_widget_get_parent(viewport);
  if(!GTK_IS_SCROLLED_WINDOW(scrolled_window)) return; // may not be part of a panel yet

  gtk_widget_get_allocation(expander, &allocation);
  _scrolled_window_smooth_scroll(scrolled_window, allocation.y, dt_conf_get_int("ui/transition_duration"));
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
  expander->frame = gtk_frame_new(NULL);
  gtk_container_add(GTK_CONTAINER(expander->frame), expander->body_evb);
  expander->revealer = gtk_revealer_new();
  gtk_container_add(GTK_CONTAINER(expander->revealer), expander->frame);

  g_signal_connect(G_OBJECT(expander->revealer), "notify::child-revealed", G_CALLBACK(dtgtk_expander_scroll), NULL);

  gtk_box_pack_start(GTK_BOX(expander), expander->header_evb, TRUE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(expander), expander->revealer, TRUE, FALSE, 0);

  return GTK_WIDGET(expander);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

