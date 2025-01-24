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

static GtkWidget *_scroll_widget = NULL;
static GtkWidget *_last_expanded = NULL;
static GtkWidget *_drop_widget = NULL;
static GtkAllocation _start_pos = {0};

void dtgtk_expander_set_expanded(GtkDarktableExpander *expander, gboolean expanded)
{
  g_return_if_fail(DTGTK_IS_EXPANDER(expander));

  expanded = expanded != FALSE;

  if(expander->expanded != expanded)
  {
    expander->expanded = expanded;

    if(expanded)
    {
      _last_expanded = GTK_WIDGET(expander);
      GtkWidget *sw = gtk_widget_get_ancestor(_last_expanded, GTK_TYPE_SCROLLED_WINDOW);
      if(sw)
      {
        gtk_widget_get_allocation(_last_expanded, &_start_pos);
        _start_pos.x = gtk_adjustment_get_value(gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(sw)));
      }
    }

    GtkWidget *frame = expander->body;
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

static gboolean _expander_scroll(GtkWidget *widget, GdkFrameClock *frame_clock, gpointer user_data)
{
  GtkWidget *sw = gtk_widget_get_ancestor(widget, GTK_TYPE_SCROLLED_WINDOW);
  if(!sw) return G_SOURCE_REMOVE;

  GtkAllocation allocation, available;
  gtk_widget_get_allocation(widget, &allocation);
  gtk_widget_get_allocation(sw, &available);

  GtkAdjustment *adjustment = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(sw));
  gdouble value = gtk_adjustment_get_value(adjustment);

  GtkWidget *header = dtgtk_expander_get_header(DTGTK_EXPANDER(widget));
  const int drop_space = (widget == _drop_widget) && header ? gtk_widget_get_allocated_height(header) : 0;
  allocation.y -= drop_space;

  const gboolean is_iop = !g_strcmp0("iop-expander", gtk_widget_get_name(widget));

  // try not to get dragged upwards if a module above is collapsing
  if(is_iop
     && widget == _last_expanded
     && allocation.y < _start_pos.y)
  {
    const int offset = _start_pos.y - allocation.y - _start_pos.x + value;
    value -= offset;
  }
  // scroll up if more space is needed below
  // if "scroll_to_module" is enabled scroll up or down
  // but don't scroll if not the whole module can be shown
  float prop = 1.0f;
  const gboolean scroll_to_top = !_drop_widget &&
    dt_conf_get_bool(is_iop ? "darkroom/ui/scroll_to_module" : "lighttable/ui/scroll_to_module");

  const int spare = available.height - allocation.height - 2 * drop_space;
  const int from_top = allocation.y - value;
  const int move = MAX(scroll_to_top ? from_top : from_top - MAX(0, MIN(from_top, spare)),
                       - MAX(0, spare - from_top));
  if(move)
  {
    gint64 interval = 0;
    gdk_frame_clock_get_refresh_info(frame_clock, 0, &interval, NULL);
    const int remaining = GPOINTER_TO_INT(user_data) - gdk_frame_clock_get_frame_time(frame_clock);
    prop = (float)interval / MAX(interval, remaining);
    value += prop * move;
  }

  if(is_iop)
  {
    _start_pos = allocation;
    _start_pos.x = value;
  }
  gtk_adjustment_set_value(adjustment, value);

  if(prop != 1.0f) return G_SOURCE_CONTINUE;

  _scroll_widget = NULL;
  return G_SOURCE_REMOVE;
}

static void _expander_resize(GtkWidget *widget, GdkRectangle *allocation, gpointer user_data)
{

  if(widget == _scroll_widget ||
     _drop_widget ? widget != _drop_widget :
     ((!(gtk_widget_get_state_flags(user_data) & GTK_STATE_FLAG_SELECTED) ||
       gtk_widget_get_allocated_height(widget) == _start_pos.height) &&
      (!darktable.lib->gui_module || darktable.lib->gui_module->expander != widget)))
    return;

  _scroll_widget = widget;
  gtk_widget_add_tick_callback(widget, _expander_scroll,
                               GINT_TO_POINTER(gdk_frame_clock_get_frame_time(gtk_widget_get_frame_clock(widget))
                               + dt_conf_get_int("darkroom/ui/transition_duration") * 1000), NULL);
}

void dtgtk_expander_set_drag_hover(GtkDarktableExpander *expander, gboolean allow, gboolean below, guint time)
{
  GtkWidget *widget = expander ? GTK_WIDGET(expander) : _drop_widget;
  // don't remove drop zone when switching between last expander and empty space to avoid jitter
  static guint last_time;
  if(!widget || (!allow && !below && widget == _drop_widget && time == last_time)) return;

  dt_gui_remove_class(widget, "module_drop_after");
  dt_gui_remove_class(widget, "module_drop_before");

  if(allow || below)
  {
    _drop_widget = widget;
    _last_expanded = NULL;
    last_time = time;

    if(!allow)
      gtk_widget_queue_resize(widget);
    else if(below)
      dt_gui_add_class(widget, "module_drop_before");
    else
      dt_gui_add_class(widget, "module_drop_after");
  }
}

static void _expander_drag_leave(GtkDarktableExpander *widget,
                           GdkDragContext *dc,
                           guint time,
                           gpointer user_data)
{
  dtgtk_expander_set_drag_hover(widget, FALSE, FALSE, time);
}

// FIXME: default highlight for the dnd is barely visible
// it should be possible to configure it
static void _expander_drag_begin(GtkWidget *widget, GdkDragContext *context, gpointer user_data)
{
  GtkAllocation allocation = {0};
  gtk_widget_get_allocation(widget, &allocation);
  // method from https://blog.gtk.org/2017/04/23/drag-and-drop-in-lists/
  cairo_surface_t *surface = dt_cairo_image_surface_create(CAIRO_FORMAT_RGB24, allocation.width, allocation.height);
  cairo_t *cr = cairo_create(surface);

  // hack to render not transparent
  dt_gui_add_class(widget, "module_drag_icon");
  gtk_widget_size_allocate(widget, &allocation);
  gtk_widget_draw(widget, cr);
  dt_gui_remove_class(widget, "module_drag_icon");

  int pointerx, pointery;
  gdk_window_get_device_position(gtk_widget_get_window(widget),
      gdk_seat_get_pointer(gdk_display_get_default_seat(gtk_widget_get_display(widget))),
      &pointerx, &pointery, NULL);
  cairo_surface_set_device_offset(surface, -pointerx, -CLAMP(pointery, 0, allocation.height));
  gtk_drag_set_icon_surface(context, surface);

  cairo_destroy(cr);
  cairo_surface_destroy(surface);

  gtk_widget_set_opacity(widget, 0.5);
}

static void _expander_drag_end(GtkWidget *widget, GdkDragContext *context, gpointer user_data)
{
  dtgtk_expander_set_drag_hover(NULL, FALSE, FALSE, 0);
  _drop_widget = NULL;
  gtk_widget_set_opacity(widget, 1.0);
}

static void dtgtk_expander_init(GtkDarktableExpander *expander)
{
}

// public functions
GtkWidget *dtgtk_expander_new(GtkWidget *header, GtkWidget *body)
{
  GtkDarktableExpander *expander;

  g_return_val_if_fail(GTK_IS_WIDGET(header), NULL);

  expander
      = g_object_new(dtgtk_expander_get_type(), "orientation", GTK_ORIENTATION_VERTICAL, "spacing", 0, NULL);
  expander->expanded = TRUE;
  expander->header = header;
  expander->body = body;

  expander->header_evb = gtk_event_box_new();
  gtk_container_add(GTK_CONTAINER(expander->header_evb), expander->header);
  expander->body_evb = gtk_event_box_new();
  if(expander->body)
    gtk_container_add(GTK_CONTAINER(expander->body_evb), expander->body);
  GtkWidget *frame = gtk_frame_new(NULL);
  gtk_container_add(GTK_CONTAINER(frame), expander->body_evb);
  expander->frame = gtk_revealer_new();
  gtk_revealer_set_transition_duration(GTK_REVEALER(expander->frame), 0);
  gtk_revealer_set_reveal_child(GTK_REVEALER(expander->frame), TRUE);
  gtk_container_add(GTK_CONTAINER(expander->frame), frame);

  gtk_box_pack_start(GTK_BOX(expander), expander->header_evb, TRUE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(expander), expander->frame, TRUE, FALSE, 0);

  g_signal_connect(expander->header_evb, "drag-begin", G_CALLBACK(_expander_drag_begin), NULL);
  g_signal_connect(expander->header_evb, "drag-end", G_CALLBACK(_expander_drag_end), NULL);
  g_signal_connect(expander, "drag-leave", G_CALLBACK(_expander_drag_leave), NULL);
  g_signal_connect(expander, "size-allocate", G_CALLBACK(_expander_resize), frame);

  return GTK_WIDGET(expander);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
