/*
    This file is part of darktable,
    Copyright (C) 2025 darktable developers.

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

#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* Inline wrappers around gdk_event_get_*() accessor functions.
 *
 * These provide direct-return access to GdkEvent fields without direct struct
 * member access. They accept const void* so any typed event pointer
 * (GdkEventButton*, GdkEventKey*, etc.) can be passed without a cast.
 *
 * This is preparation for the GTK4 migration where GdkEvent is no longer
 * a publicly accessible struct.
 */

static inline GdkEventType dt_gdk_event_get_type(const void *e)
{
  return gdk_event_get_event_type((const GdkEvent *)e);
}

static inline guint32 dt_gdk_event_get_time(const void *e)
{
  return gdk_event_get_time((const GdkEvent *)e);
}

static inline guint dt_gdk_event_get_button(const void *e)
{
  guint b = 0;
  gdk_event_get_button((const GdkEvent *)e, &b);
  return b;
}

static inline guint dt_gdk_event_get_click_count(const void *e)
{
  guint c = 0;
  gdk_event_get_click_count((const GdkEvent *)e, &c);
  return c;
}

static inline GdkModifierType dt_gdk_event_get_state(const void *e)
{
  GdkModifierType s = 0;
  gdk_event_get_state((const GdkEvent *)e, &s);
  return s;
}

static inline gdouble dt_gdk_event_get_x(const void *e)
{
  gdouble x = 0, y = 0;
  gdk_event_get_coords((const GdkEvent *)e, &x, &y);
  (void)y;
  return x;
}

static inline gdouble dt_gdk_event_get_y(const void *e)
{
  gdouble x = 0, y = 0;
  gdk_event_get_coords((const GdkEvent *)e, &x, &y);
  (void)x;
  return y;
}

static inline gdouble dt_gdk_event_get_root_x(const void *e)
{
  gdouble x = 0, y = 0;
  gdk_event_get_root_coords((const GdkEvent *)e, &x, &y);
  (void)y;
  return x;
}

static inline gdouble dt_gdk_event_get_root_y(const void *e)
{
  gdouble x = 0, y = 0;
  gdk_event_get_root_coords((const GdkEvent *)e, &x, &y);
  (void)x;
  return y;
}

static inline guint dt_gdk_event_get_keyval(const void *e)
{
  guint k = 0;
  gdk_event_get_keyval((const GdkEvent *)e, &k);
  return k;
}

static inline guint16 dt_gdk_event_get_keycode(const void *e)
{
  guint16 k = 0;
  gdk_event_get_keycode((const GdkEvent *)e, &k);
  return k;
}

static inline GdkScrollDirection dt_gdk_event_get_scroll_direction(const void *e)
{
  GdkScrollDirection d = GDK_SCROLL_UP;
  gdk_event_get_scroll_direction((const GdkEvent *)e, &d);
  return d;
}

static inline gdouble dt_gdk_event_get_scroll_delta_x(const void *e)
{
  gdouble dx = 0, dy = 0;
  gdk_event_get_scroll_deltas((const GdkEvent *)e, &dx, &dy);
  (void)dy;
  return dx;
}

static inline gdouble dt_gdk_event_get_scroll_delta_y(const void *e)
{
  gdouble dx = 0, dy = 0;
  gdk_event_get_scroll_deltas((const GdkEvent *)e, &dx, &dy);
  (void)dx;
  return dy;
}

static inline GdkWindow *dt_gdk_event_get_window(const void *e)
{
  return gdk_event_get_window((const GdkEvent *)e);
}

static inline GdkDevice *dt_gdk_event_get_device(const void *e)
{
  return gdk_event_get_device((const GdkEvent *)e);
}

static inline GdkDevice *dt_gdk_event_get_source_device(const void *e)
{
  return gdk_event_get_source_device((const GdkEvent *)e);
}

static inline GdkScreen *dt_gdk_event_get_screen(const void *e)
{
  return gdk_event_get_screen((const GdkEvent *)e);
}

static inline GdkSeat *dt_gdk_event_get_seat(const void *e)
{
  return gdk_event_get_seat((const GdkEvent *)e);
}

static inline gboolean dt_gdk_event_get_pointer_emulated(void *e)
{
  return gdk_event_get_pointer_emulated((GdkEvent *)e);
}

static inline gboolean dt_gdk_event_get_axis(const void *e,
                                              GdkAxisUse axis_use,
                                              gdouble *value)
{
  return gdk_event_get_axis((const GdkEvent *)e, axis_use, value);
}

G_END_DECLS
