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
#if GTK_CHECK_VERSION(4, 0, 0)
  return gdk_event_get_modifier_state((const GdkEvent *)e);
#else
  GdkModifierType s = 0;
  gdk_event_get_state((const GdkEvent *)e, &s);
  return s;
#endif
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
#if GTK_CHECK_VERSION(4, 0, 0)
  return gdk_scroll_event_get_direction((GdkEvent *)e);
#else
  GdkScrollDirection d = GDK_SCROLL_UP;
  if(!gdk_event_get_scroll_direction((const GdkEvent *)e, &d))
  {
    // gdk_event_get_scroll_direction() fails for smooth scrolling events.
    // The consuming code (dt_gui_get_scroll_delta, dt_gui_get_scroll_unit_deltas,
    // _event_scroll pan routing, _scroll_proxy_real, etc.) already handles
    // GDK_SCROLL_SMOOTH by reading the actual deltas, so return SMOOTH here
    // to preserve the original struct-access behavior.
    d = GDK_SCROLL_SMOOTH;
  }
  return d;
#endif
}

static inline gdouble dt_gdk_event_get_scroll_delta_x(const void *e)
{
#if GTK_CHECK_VERSION(4, 0, 0)
  gdouble dx = 0, dy = 0;
  gdk_scroll_event_get_deltas((GdkEvent *)e, &dx, &dy);
  (void)dy;
  return dx;
#else
  gdouble dx = 0, dy = 0;
  gdk_event_get_scroll_deltas((const GdkEvent *)e, &dx, &dy);
  (void)dy;
  return dx;
#endif
}

static inline gdouble dt_gdk_event_get_scroll_delta_y(const void *e)
{
#if GTK_CHECK_VERSION(4, 0, 0)
  gdouble dx = 0, dy = 0;
  gdk_scroll_event_get_deltas((GdkEvent *)e, &dx, &dy);
  (void)dx;
  return dy;
#else
  gdouble dx = 0, dy = 0;
  gdk_event_get_scroll_deltas((const GdkEvent *)e, &dx, &dy);
  (void)dx;
  return dy;
#endif
}

static inline gboolean dt_gdk_event_is_scroll_stop(const void *e)
{
#if GTK_CHECK_VERSION(4, 0, 0)
  return gdk_scroll_event_is_stop((GdkEvent *)e);
#else
  return gdk_event_is_scroll_stop_event((const GdkEvent *)e);
#endif
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
#if GTK_CHECK_VERSION(4, 0, 0)
  // GTK4 dropped the separate "source device": the event device is the
  // source (the GdkDeviceTool distinguishes pen/eraser on top of it).
  return gdk_event_get_device((const GdkEvent *)e);
#else
  return gdk_event_get_source_device((const GdkEvent *)e);
#endif
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

/* --- touchpad gesture events ---
 * GTK3 exposes the GdkEventTouchpadPinch/GdkEventTouchpadSwipe structs, GTK4
 * the gdk_touchpad_event_* accessors; these wrappers keep both callable. */

static inline GdkTouchpadGesturePhase dt_gdk_touchpad_pinch_get_phase(const void *e)
{
#if GTK_CHECK_VERSION(4, 0, 0)
  return gdk_touchpad_event_get_gesture_phase((GdkEvent *)e);
#else
  return ((const GdkEvent *)e)->touchpad_pinch.phase;
#endif
}

static inline guint dt_gdk_touchpad_pinch_get_n_fingers(const void *e)
{
#if GTK_CHECK_VERSION(4, 0, 0)
  return gdk_touchpad_event_get_n_fingers((GdkEvent *)e);
#else
  return ((const GdkEvent *)e)->touchpad_pinch.n_fingers;
#endif
}

static inline void dt_gdk_touchpad_pinch_get_deltas(const void *e,
                                                     gdouble *dx,
                                                     gdouble *dy)
{
#if GTK_CHECK_VERSION(4, 0, 0)
  gdk_touchpad_event_get_deltas((GdkEvent *)e, dx, dy);
#else
  if(dx) *dx = ((const GdkEvent *)e)->touchpad_pinch.dx;
  if(dy) *dy = ((const GdkEvent *)e)->touchpad_pinch.dy;
#endif
}

static inline gdouble dt_gdk_touchpad_pinch_get_scale(const void *e)
{
#if GTK_CHECK_VERSION(4, 0, 0)
  return gdk_touchpad_event_get_pinch_scale((GdkEvent *)e);
#else
  return ((const GdkEvent *)e)->touchpad_pinch.scale;
#endif
}

static inline GdkTouchpadGesturePhase dt_gdk_touchpad_swipe_get_phase(const void *e)
{
#if GTK_CHECK_VERSION(4, 0, 0)
  return gdk_touchpad_event_get_gesture_phase((GdkEvent *)e);
#else
  return ((const GdkEvent *)e)->touchpad_swipe.phase;
#endif
}

G_END_DECLS
