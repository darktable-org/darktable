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
/*
 * cmocka unit tests for the single-delta scroll helpers in gui/gtk.c.
 *
 * On macOS a physical (non-precise) mouse wheel turned while Shift is held
 * reaches GDK as a discrete horizontal scroll: wheel up -> GDK_SCROLL_LEFT,
 * wheel down -> GDK_SCROLL_RIGHT (gdk/quartz/gdkevents-quartz.c). Shift+wheel
 * must still act like the wheel, so there it has to match UP/DOWN. Elsewhere,
 * and without Shift, a horizontal scroll keeps "right == up".
 *
 * The events are synthesized with gdk_event_new(); no display is needed.
 */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include <gtk/gtk.h>

#include "gui/gtk.h"

static GdkEvent *_scroll(const GdkScrollDirection direction,
                         const gdouble delta_x,
                         const gdouble delta_y,
                         const GdkModifierType state)
{
  GdkEvent *event = gdk_event_new(GDK_SCROLL);
  event->scroll.direction = direction;
  event->scroll.delta_x = delta_x;
  event->scroll.delta_y = delta_y;
  event->scroll.state = state;
  return event;
}

static int _unit_delta(const GdkScrollDirection direction, const GdkModifierType state)
{
  GdkEvent *event = _scroll(direction, 0.0, 0.0, state);
  int delta = 0;
  assert_true(dt_gui_get_scroll_unit_delta(event, &delta));
  gdk_event_free(event);
  return delta;
}

static gdouble _delta(const GdkScrollDirection direction, const GdkModifierType state)
{
  GdkEvent *event = _scroll(direction, 0.0, 0.0, state);
  gdouble delta = 0.0;
  assert_true(dt_gui_get_scroll_delta(event, &delta));
  gdk_event_free(event);
  return delta;
}

// vertical wheel: up < 0, down > 0, with or without Shift, on every platform
static void test_vertical_wheel(void **state)
{
  (void)state;
  assert_int_equal(_unit_delta(GDK_SCROLL_UP, 0), -1);
  assert_int_equal(_unit_delta(GDK_SCROLL_DOWN, 0), 1);
  assert_int_equal(_unit_delta(GDK_SCROLL_UP, GDK_SHIFT_MASK), -1);
  assert_int_equal(_unit_delta(GDK_SCROLL_DOWN, GDK_SHIFT_MASK), 1);
  assert_true(_delta(GDK_SCROLL_UP, GDK_SHIFT_MASK) == -1.0);
  assert_true(_delta(GDK_SCROLL_DOWN, GDK_SHIFT_MASK) == 1.0);
}

// horizontal wheel/tilt without Shift keeps "right == up" everywhere
static void test_horizontal_without_shift(void **state)
{
  (void)state;
  assert_int_equal(_unit_delta(GDK_SCROLL_RIGHT, 0), -1);
  assert_int_equal(_unit_delta(GDK_SCROLL_LEFT, 0), 1);
  assert_true(_delta(GDK_SCROLL_RIGHT, 0) == -1.0);
  assert_true(_delta(GDK_SCROLL_LEFT, 0) == 1.0);
}

// Shift + discrete horizontal: on macOS it is Shift + wheel, so LEFT is wheel
// up and must give the same delta as UP. Other GDK backends deliver
// Shift + wheel as UP/DOWN, so their horizontal handling stays unchanged.
static void test_shift_horizontal_wheel(void **state)
{
  (void)state;
#ifdef GDK_WINDOWING_QUARTZ
  assert_int_equal(_unit_delta(GDK_SCROLL_LEFT, GDK_SHIFT_MASK),
                   _unit_delta(GDK_SCROLL_UP, GDK_SHIFT_MASK));
  assert_int_equal(_unit_delta(GDK_SCROLL_RIGHT, GDK_SHIFT_MASK),
                   _unit_delta(GDK_SCROLL_DOWN, GDK_SHIFT_MASK));
  assert_true(_delta(GDK_SCROLL_LEFT, GDK_SHIFT_MASK) == -1.0);
  assert_true(_delta(GDK_SCROLL_RIGHT, GDK_SHIFT_MASK) == 1.0);
  // other modifiers held together with Shift do not change that
  assert_int_equal(_unit_delta(GDK_SCROLL_LEFT, GDK_SHIFT_MASK | GDK_CONTROL_MASK), -1);
#else
  assert_int_equal(_unit_delta(GDK_SCROLL_RIGHT, GDK_SHIFT_MASK), -1);
  assert_int_equal(_unit_delta(GDK_SCROLL_LEFT, GDK_SHIFT_MASK), 1);
#endif
}

// smooth (trackpad / precise) horizontal scroll with Shift is not a wheel
// converted by the OS: "right == up" still applies on every platform
static void test_shift_smooth_horizontal(void **state)
{
  (void)state;
  GdkEvent *event = _scroll(GDK_SCROLL_SMOOTH, 3.0, 0.0, GDK_SHIFT_MASK);
  gdouble delta = 0.0;
  assert_true(dt_gui_get_scroll_delta(event, &delta));
  assert_true(delta < 0.0);
  gdk_event_free(event);

  event = _scroll(GDK_SCROLL_SMOOTH, -3.0, 0.0, GDK_SHIFT_MASK);
  assert_true(dt_gui_get_scroll_delta(event, &delta));
  assert_true(delta > 0.0);
  gdk_event_free(event);
}

int main(int argc, char *argv[])
{
  (void)argc;
  (void)argv;
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_vertical_wheel),
    cmocka_unit_test(test_horizontal_without_shift),
    cmocka_unit_test(test_shift_horizontal_wheel),
    cmocka_unit_test(test_shift_smooth_horizontal),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
