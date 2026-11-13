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
 * cmocka unit tests for how the presenter core chooses a window's backend
 * again (gui/presenter.c, dt_presenter_window_choose()): on a platform
 * whose verdict the test sets, with backends that record what is done to
 * them, and a window built here, so that no display is needed.
 * presenter.c is included with the backends' constructors, the output
 * registry, the preference and GTK's redraw wrapped
 */

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include <cmocka.h>

#define dt_presenter_layered_new __wrap_dt_presenter_layered_new
#define dt_presenter_cairo_new __wrap_dt_presenter_cairo_new
#define dt_colorspaces_set_window_output __wrap_dt_colorspaces_set_window_output
#define dt_conf_get_bool __wrap_dt_conf_get_bool
#define gtk_widget_queue_draw __wrap_gtk_widget_queue_draw

#include "gui/presenter.c"

#undef dt_presenter_layered_new
#undef dt_presenter_cairo_new
#undef dt_colorspaces_set_window_output
#undef dt_conf_get_bool
#undef gtk_widget_queue_draw

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

// what was done, in order, one letter each: n and c a layered or cairo
// backend made, f a layered one that could not set up, r and R a widget's
// layered or cairo data released, x and X a layered or cairo backend
// freed, t a target given to the layered backend, o an output published,
// g the output given back, d a redraw
static GString *_log;
static gboolean _force_cairo, _platform_native, _transport_fails;
static dt_display_output_t _published;
static float _target;

typedef struct _fake_backend_t
{
  dt_presenter_backend_t backend;
  gboolean native;
} _fake_backend_t;

typedef struct _fake_data_t
{
  gboolean native;
} _fake_data_t;

static cairo_t *_fake_begin(dt_presenter_backend_t *self,
                            GtkWidget *widget,
                            void **data,
                            cairo_t *widget_cr)
{
  return widget_cr;
}

static void _fake_release(dt_presenter_backend_t *self,
                          void **data)
{
  _fake_data_t *d = *data;
  if(!d) return;
  // a backend only ever releases its own data
  assert_int_equal(d->native, ((_fake_backend_t *)self)->native);
  g_string_append_c(_log, d->native ? 'r' : 'R');
  g_free(d);
  *data = NULL;
}

static void _fake_free(dt_presenter_backend_t *self)
{
  g_string_append_c(_log, ((_fake_backend_t *)self)->native ? 'x' : 'X');
  g_free(self);
}

// a transport describes its image for the target, and publishes the
// output when that description is ready, which the tests do themselves
static void _fake_target(dt_presenter_backend_t *self,
                         const float headroom)
{
  g_string_append_c(_log, 't');
  _target = headroom;
}

static dt_presenter_backend_t *_fake_new(const gboolean native)
{
  _fake_backend_t *b = g_malloc0(sizeof(_fake_backend_t));
  b->native = native;
  b->backend.begin = _fake_begin;
  b->backend.release = _fake_release;
  b->backend.free = _fake_free;
  if(native) b->backend.target = _fake_target;
  return &b->backend;
}

static void _fake_publish(dt_presenter_window_t *window,
                          const float headroom)
{
  const dt_display_output_t output = { .image = { .type = DT_COLORSPACE_LIN_REC2020,
                                                  .headroom = headroom },
                                       .device = { .type = DT_COLORSPACE_SRGB },
                                       .format = DT_FRAME_FLOAT };
  dt_presenter_window_set_output(window, &output);
}

// as a transport does when it sets up: it describes its image for the
// target, and the platform owns the output from then on
dt_presenter_backend_t *__wrap_dt_presenter_layered_new(dt_presenter_window_t *window)
{
  // never made while the window has another backend
  assert_null(window->backend);
  if(_transport_fails)
  {
    g_string_append_c(_log, 'f');
    return NULL;
  }
  g_string_append_c(_log, 'n');
  _fake_publish(window, dt_presenter_window_target(window));
  return _fake_new(TRUE);
}

dt_presenter_backend_t *__wrap_dt_presenter_cairo_new(dt_presenter_window_t *window)
{
  assert_null(window->backend);
  g_string_append_c(_log, 'c');
  return _fake_new(FALSE);
}

void __wrap_dt_colorspaces_set_window_output(const dt_colorspaces_color_profile_type_t window,
                                             const dt_display_output_t *output)
{
  g_string_append_c(_log, output ? 'o' : 'g');
  if(output) _published = *output;
}

gboolean __wrap_dt_conf_get_bool(const char *name)
{
  assert_string_equal(name, "ui_last/display_force_cairo");
  return _force_cairo;
}

void __wrap_gtk_widget_queue_draw(GtkWidget *widget)
{
  g_string_append_c(_log, 'd');
}

static gboolean _fake_native(dt_presenter_platform_t *self)
{
  return _platform_native;
}

static void _fake_platform_free(dt_presenter_platform_t *self)
{
}

static dt_presenter_platform_t _platform = { .native = _fake_native,
                                             .free = _fake_platform_free };

typedef struct _state_t
{
  dt_presenter_window_t *window;
  _widget_t *widgets[2];
} _state_t;

// each widget draws on the window's backend: its data and one image
static void _draw(_state_t *s)
{
  for(int k = 0; k < 2; k++)
  {
    _widget_t *w = s->widgets[k];
    _fake_data_t *d = g_malloc0(sizeof(_fake_data_t));
    d->native = s->window->native;
    w->data = d;
    const dt_presenter_image_t image = { .data = d };
    g_array_append_val(w->images, image);
  }
  g_string_truncate(_log, 0);
}

// a main window on a native platform, as _window_get() makes it
static int _setup(void **state)
{
  _log = g_string_new(NULL);
  _force_cairo = _transport_fails = FALSE;
  _platform_native = TRUE;
  memset(&_published, 0, sizeof(_published));
  _target = 0.0f;

  _state_t *s = g_new0(_state_t, 1);
  s->window = g_malloc0(sizeof(dt_presenter_window_t));
  s->window->kind = DT_COLORSPACE_DISPLAY;
  s->window->platform = &_platform;
  dt_presenter_target_init(&s->window->target);
  _set_backend(s->window);
  for(int k = 0; k < 2; k++)
  {
    _widget_t *w = g_malloc0(sizeof(_widget_t));
    w->images = g_array_new(FALSE, FALSE, sizeof(dt_presenter_image_t));
    w->frames = g_ptr_array_new();
    w->window = s->window;
    s->window->widgets = g_list_prepend(s->window->widgets, w);
    s->widgets[k] = w;
  }
  _draw(s);
  *state = s;
  return 0;
}

static int _teardown(void **state)
{
  _state_t *s = *state;
  for(int k = 0; k < 2; k++)
  {
    _widget_t *w = s->widgets[k];
    _widget_detach(w);
    g_array_free(w->images, TRUE);
    g_ptr_array_free(w->frames, TRUE);
    g_free(w);
  }
  _settle_stop(s->window);
  s->window->backend->free(s->window->backend);
  g_free(s->window);
  g_free(s);
  g_string_free(_log, TRUE);
  return 0;
}

static void _assert_released(const _state_t *s)
{
  for(int k = 0; k < 2; k++)
  {
    assert_null(s->widgets[k]->data);
    assert_int_equal(s->widgets[k]->images->len, 0);
    // the widgets stay the window's, and draw on the new backend
    assert_ptr_equal(s->widgets[k]->window, s->window);
  }
}

static void test_native_at_first(void **state)
{
  _state_t *s = *state;
  assert_true(s->window->native);
  assert_int_equal(_published.image.type, DT_COLORSPACE_LIN_REC2020);
  assert_true(_published.image.headroom == 1.0f);
}

static void test_unchanged_verdict_does_nothing(void **state)
{
  _state_t *s = *state;
  dt_presenter_backend_t *backend = s->window->backend;
  dt_presenter_window_choose(s->window);
  assert_string_equal(_log->str, "");
  assert_ptr_equal(s->window->backend, backend);
}

static void test_platform_flips_both_ways(void **state)
{
  _state_t *s = *state;

  // every widget's data goes before the backend that made it, the new
  // backend is made after, cairo gives the output back, then a redraw
  _platform_native = FALSE;
  dt_presenter_window_choose(s->window);
  assert_string_equal(_log->str, "rrxcgd");
  assert_false(s->window->native);
  _assert_released(s);

  _draw(s);
  _platform_native = TRUE;
  dt_presenter_window_choose(s->window);
  assert_string_equal(_log->str, "RRXnod");
  assert_true(s->window->native);
  _assert_released(s);
}

static void test_force_cairo(void **state)
{
  _state_t *s = *state;
  _force_cairo = TRUE;
  dt_presenter_window_choose(s->window);
  assert_string_equal(_log->str, "rrxcgd");
  assert_false(s->window->native);

  _draw(s);
  _force_cairo = FALSE;
  dt_presenter_window_choose(s->window);
  assert_string_equal(_log->str, "RRXnod");
  assert_true(s->window->native);
}

static void test_failed_transport_is_tried_again(void **state)
{
  _state_t *s = *state;
  _platform_native = FALSE;
  dt_presenter_window_choose(s->window);
  _draw(s);

  // the platform manages the window again, but its transport cannot set
  // up: cairo, which gives back what a native backend may have reported
  _transport_fails = TRUE;
  _platform_native = TRUE;
  dt_presenter_window_choose(s->window);
  assert_string_equal(_log->str, "RRXfcgd");
  assert_false(s->window->native);

  // the next choice tries again
  _draw(s);
  _transport_fails = FALSE;
  dt_presenter_window_choose(s->window);
  assert_string_equal(_log->str, "RRXnod");
  assert_true(s->window->native);
}

static void test_no_platform_is_cairo(void **state)
{
  _state_t *s = *state;
  s->window->platform = NULL;
  dt_presenter_window_choose(s->window);
  assert_string_equal(_log->str, "rrxcgd");
  assert_false(s->window->native);
  g_string_truncate(_log, 0);
  dt_presenter_window_choose(s->window);
  assert_string_equal(_log->str, "");
}

// a verdict the platform reaches while it sets up, before the window has
// a backend, is taken when _window_get() chooses
static void test_choice_while_setting_up_waits(void **state)
{
  _state_t *s = *state;
  dt_presenter_backend_t *backend = s->window->backend;
  s->window->backend = NULL;
  _platform_native = FALSE;
  dt_presenter_window_choose(s->window);
  assert_string_equal(_log->str, "");
  s->window->backend = backend;
}

// the target keeps settling from the platform's reports while the window
// is on cairo, and a native backend chosen again describes its image for it
// at setup
static void test_target_kept_across_cairo(void **state)
{
  _state_t *s = *state;
  _platform_native = FALSE;
  dt_presenter_window_choose(s->window);
  _draw(s);

  const dt_presenter_headroom_t report = { .value = 4.0f, .generation = 1 };
  dt_presenter_window_report_headroom(s->window, &report);
  // not published: the output is the system display profile's
  assert_string_equal(_log->str, "");
  assert_true(fabsf(dt_presenter_window_target(s->window) - 4.0f) < 1e-6f);

  _platform_native = TRUE;
  dt_presenter_window_choose(s->window);
  assert_string_equal(_log->str, "RRXnod");
  assert_true(fabsf(_published.image.headroom - 4.0f) < 1e-6f);
}

// a settled target goes to the native backend only: the output keeps the
// headroom the backend published until it publishes another, which the
// core takes as it is
static void test_backend_publishes_target(void **state)
{
  _state_t *s = *state;
  const dt_presenter_headroom_t report = { .value = 4.0f, .generation = 1 };
  dt_presenter_window_report_headroom(s->window, &report);
  assert_string_equal(_log->str, "t");
  assert_true(fabsf(_target - 4.0f) < 1e-6f);
  assert_true(_published.image.headroom == 1.0f);

  // the description made for the target is ready, its range a little more
  g_string_truncate(_log, 0);
  _fake_publish(s->window, 813.0f / 203.0f);
  assert_string_equal(_log->str, "o");
  assert_true(_published.image.headroom == 813.0f / 203.0f);

  // a cairo window takes no target, and publishes none
  _platform_native = FALSE;
  dt_presenter_window_choose(s->window);
  g_string_truncate(_log, 0);
  const dt_presenter_headroom_t next = { .value = 2.0f, .generation = 2 };
  dt_presenter_window_report_headroom(s->window, &next);
  assert_string_equal(_log->str, "");
  assert_true(fabsf(dt_presenter_window_target(s->window) - 2.0f) < 1e-6f);
}

int main(int argc, char *argv[])
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test_setup_teardown(test_native_at_first, _setup, _teardown),
    cmocka_unit_test_setup_teardown(test_unchanged_verdict_does_nothing, _setup, _teardown),
    cmocka_unit_test_setup_teardown(test_platform_flips_both_ways, _setup, _teardown),
    cmocka_unit_test_setup_teardown(test_force_cairo, _setup, _teardown),
    cmocka_unit_test_setup_teardown(test_failed_transport_is_tried_again, _setup, _teardown),
    cmocka_unit_test_setup_teardown(test_no_platform_is_cairo, _setup, _teardown),
    cmocka_unit_test_setup_teardown(test_choice_while_setting_up_waits, _setup, _teardown),
    cmocka_unit_test_setup_teardown(test_target_kept_across_cairo, _setup, _teardown),
    cmocka_unit_test_setup_teardown(test_backend_publishes_target, _setup, _teardown),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
