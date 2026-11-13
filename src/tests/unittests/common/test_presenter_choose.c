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
 * and platform (gui/presenter.c, _choose()): on platforms whose verdict
 * the test sets, with backends that record what is done to them, and a
 * window built here, so that no display is needed. presenter.c is
 * included with the constructors, the output registry, the preference,
 * the idle queue, the log and GTK's redraw wrapped. the queued choice runs
 * only when a test runs it, as the main loop would later
 */

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include <cmocka.h>

#define dt_presenter_platform_new __wrap_dt_presenter_platform_new
#define dt_presenter_layered_new __wrap_dt_presenter_layered_new
#define dt_presenter_cairo_new __wrap_dt_presenter_cairo_new
#define dt_colorspaces_set_window_output __wrap_dt_colorspaces_set_window_output
#define dt_conf_get_bool __wrap_dt_conf_get_bool
#define dt_control_signal_disconnect __wrap_dt_control_signal_disconnect
#define dt_print_ext __wrap_dt_print_ext
#define g_idle_add_full __wrap_g_idle_add_full
#define g_source_remove __wrap_g_source_remove
#define gtk_widget_queue_draw __wrap_gtk_widget_queue_draw

#include "gui/presenter.c"

#undef dt_presenter_platform_new
#undef dt_presenter_layered_new
#undef dt_presenter_cairo_new
#undef dt_colorspaces_set_window_output
#undef dt_conf_get_bool
#undef dt_control_signal_disconnect
#undef dt_print_ext
#undef g_idle_add_full
#undef g_source_remove
#undef gtk_widget_queue_draw

// the real one, for the settle timer's sources
gboolean g_source_remove(guint tag);

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

// what was done, in order, one letter each: P a platform made, q one that
// could not set up, a none on this platform, p a platform freed, n and c a
// layered or cairo backend made, f a layered one that could not set up, r
// and R a widget's layered or cairo data released, x and X a layered or
// cairo backend freed, t a target given to the layered backend, o an
// output published, g the output given back, d a redraw
static GString *_log;
// the core's lines about the window's backend
static GPtrArray *_lines;
static gboolean _force_cairo, _platform_native, _platform_fails, _platform_absent,
  _transport_fails;
static dt_display_output_t _published;
static float _target;
// the window's output is the platform's: published and not given back
static gboolean _owned;
// constructor calls, and the platforms not freed yet
static int _platforms_made, _layered_made, _cairo_made, _platforms_live;
// constructors running now: none may run inside another
static int _constructing;

// what a constructor does while it sets up, as Wayland's _info_done() may
typedef enum _hook_t
{
  HOOK_NONE,
  HOOK_CHOOSE,
  HOOK_REPORT,
} _hook_t;
static _hook_t _platform_hook, _layered_hook;
static const dt_presenter_headroom_t _setup_report = { .value = 2.0f, .generation = 1 };

// the one idle the core may queue; fake ids above any GLib gives here
static GSourceFunc _idle_func;
static gpointer _idle_data;
static guint _idle_id, _idle_next = 1000000;
static int _idles_queued, _idles_removed;
// the last other source removed: the settle timer's
static guint _removed;

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
  const gboolean native = ((_fake_backend_t *)self)->native;
  // a transport uses its platform until it is freed
  if(native) assert_true(_platforms_live > 0);
  g_string_append_c(_log, native ? 'x' : 'X');
  g_free(self);
}

// a transport describes its image for the target, and publishes the
// output when that description is ready, which the tests do themselves
static void _fake_target(dt_presenter_backend_t *self,
                         const float headroom)
{
  assert_true(((_fake_backend_t *)self)->native);
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

static void _hook(dt_presenter_window_t *window,
                  const _hook_t hook)
{
  if(hook == HOOK_CHOOSE) dt_presenter_window_choose(window);
  if(hook == HOOK_CHOOSE || hook == HOOK_REPORT)
    dt_presenter_window_report_headroom(window, &_setup_report);
}

static gboolean _fake_native(dt_presenter_platform_t *self)
{
  return _platform_native;
}

static void _fake_platform_free(dt_presenter_platform_t *self)
{
  g_string_append_c(_log, 'p');
  _platforms_live--;
  g_free(self);
}

dt_presenter_platform_t *__wrap_dt_presenter_platform_new(dt_presenter_window_t *window,
                                                          gchar **reason)
{
  // made only while the window may be native, and never twice
  assert_false(window->forced);
  assert_false(window->failed);
  assert_null(window->platform);
  assert_int_equal(_constructing++, 0);
  _platforms_made++;
  dt_presenter_platform_t *platform = NULL;
  *reason = NULL;
  if(_platform_fails)
  {
    g_string_append_c(_log, 'q');
    *reason = g_strdup("wp_color_manager_v1 not advertised");
  }
  else if(_platform_absent)
    g_string_append_c(_log, 'a');
  else
  {
    g_string_append_c(_log, 'P');
    platform = g_malloc0(sizeof(dt_presenter_platform_t));
    platform->native = _fake_native;
    platform->free = _fake_platform_free;
    _platforms_live++;
    _hook(window, _platform_hook);
  }
  _constructing--;
  return platform;
}

// as a transport does when it sets up: it describes its image for the
// target, and the platform owns the output from then on
dt_presenter_backend_t *__wrap_dt_presenter_layered_new(dt_presenter_window_t *window,
                                                        gchar **reason)
{
  // never made while the window has another backend, or no platform
  assert_null(window->backend);
  assert_non_null(window->platform);
  assert_int_equal(_constructing++, 0);
  _layered_made++;
  *reason = NULL;
  _hook(window, _layered_hook);
  dt_presenter_backend_t *backend = NULL;
  if(_transport_fails)
  {
    // as a transport that published for its first description before a
    // later step failed: the core must give the output back
    _fake_publish(window, dt_presenter_window_target(window));
    g_string_append_c(_log, 'f');
    *reason = g_strdup("image description for headroom 1.0000 failed (1): refused");
  }
  else
  {
    g_string_append_c(_log, 'n');
    _fake_publish(window, dt_presenter_window_target(window));
    backend = _fake_new(TRUE);
  }
  _constructing--;
  return backend;
}

dt_presenter_backend_t *__wrap_dt_presenter_cairo_new(dt_presenter_window_t *window)
{
  assert_null(window->backend);
  _cairo_made++;
  g_string_append_c(_log, 'c');
  return _fake_new(FALSE);
}

void __wrap_dt_colorspaces_set_window_output(const dt_colorspaces_color_profile_type_t window,
                                             const dt_display_output_t *output)
{
  g_string_append_c(_log, output ? 'o' : 'g');
  if(output) _published = *output;
  _owned = output != NULL;
}

gboolean __wrap_dt_conf_get_bool(const char *name)
{
  assert_string_equal(name, "ui_last/display_force_cairo");
  return _force_cairo;
}

void __wrap_dt_control_signal_disconnect(const struct dt_control_signal_t *ctlsig,
                                         GCallback cb,
                                         gpointer user_data)
{
}

void __wrap_dt_print_ext(const char *msg,
                         ...)
{
  va_list ap;
  va_start(ap, msg);
  gchar *line = g_strdup_vprintf(msg, ap);
  va_end(ap);
  if(g_str_has_prefix(line, "[presenter] main window: "))
    g_ptr_array_add(_lines, g_strdup(line + strlen("[presenter] main window: ")));
  g_free(line);
}

guint __wrap_g_idle_add_full(gint priority,
                             GSourceFunc function,
                             gpointer data,
                             GDestroyNotify notify)
{
  // above GDK's redraw, and at most one queued
  assert_int_equal(priority, G_PRIORITY_HIGH_IDLE);
  assert_true(priority < GDK_PRIORITY_REDRAW);
  assert_int_equal(_idle_id, 0);
  _idle_func = function;
  _idle_data = data;
  _idle_id = _idle_next++;
  _idles_queued++;
  return _idle_id;
}

gboolean __wrap_g_source_remove(guint id)
{
  if(!id || id != _idle_id)
  {
    _removed = id;
    return g_source_remove(id);
  }
  _idle_id = 0;
  _idles_removed++;
  return TRUE;
}

void __wrap_gtk_widget_queue_draw(GtkWidget *widget)
{
  g_string_append_c(_log, 'd');
}

// the main loop gets to the queued choice
static void _run_idle(void)
{
  assert_int_not_equal(_idle_id, 0);
  _idle_id = 0;
  assert_int_equal(_idle_func(_idle_data), G_SOURCE_REMOVE);
}

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

// a main window as _window_get() makes it, with two presenting widgets
// that drew in it
static void _make(_state_t *s)
{
  s->window = g_malloc0(sizeof(dt_presenter_window_t));
  s->window->kind = DT_COLORSPACE_DISPLAY;
  dt_presenter_target_init(&s->window->target);
  _choose(s->window);
  for(int k = 0; k < 2; k++)
  {
    _widget_t *w = s->widgets[k];
    w->window = s->window;
    s->window->widgets = g_list_prepend(s->window->widgets, w);
  }
}

// the tests make the window once they set what its first choice finds
static int _setup(void **state)
{
  _log = g_string_new(NULL);
  _lines = g_ptr_array_new_with_free_func(g_free);
  darktable.unmuted = DT_DEBUG_CONTROL;
  _force_cairo = _platform_fails = _platform_absent = _transport_fails = FALSE;
  _platform_native = TRUE;
  _platform_hook = _layered_hook = HOOK_NONE;
  memset(&_published, 0, sizeof(_published));
  _target = 0.0f;
  _platforms_made = _layered_made = _cairo_made = _platforms_live = _constructing = 0;
  _idle_id = _removed = 0;
  _idles_queued = _idles_removed = 0;
  _owned = FALSE;

  _state_t *s = g_new0(_state_t, 1);
  for(int k = 0; k < 2; k++)
  {
    _widget_t *w = g_malloc0(sizeof(_widget_t));
    w->images = g_array_new(FALSE, FALSE, sizeof(dt_presenter_image_t));
    w->frames = g_ptr_array_new();
    s->widgets[k] = w;
  }
  *state = s;
  return 0;
}

static int _teardown(void **state)
{
  _state_t *s = *state;
  if(s->window) _window_free(s->window);
  // nothing outlives the window
  assert_int_equal(_idle_id, 0);
  assert_int_equal(_platforms_live, 0);
  for(int k = 0; k < 2; k++)
  {
    _widget_t *w = s->widgets[k];
    assert_null(w->window);
    g_array_free(w->images, TRUE);
    g_ptr_array_free(w->frames, TRUE);
    g_free(w);
  }
  g_free(s);
  g_string_free(_log, TRUE);
  g_ptr_array_free(_lines, TRUE);
  darktable.unmuted = 0;
  return 0;
}

// what a platform does when its verdict changes, and the main loop later
static void _choose_now(_state_t *s)
{
  dt_presenter_window_choose(s->window);
  _run_idle();
}

// the preferences dialog closes, with the force key as the test set it
static void _preferences_close(_state_t *s)
{
  _preferences_changed(NULL, s->window);
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

static void _assert_line(const guint index,
                         const char *line)
{
  assert_true(_lines->len > index);
  assert_string_equal(g_ptr_array_index(_lines, index), line);
}

#define FORCED "cairo: forced by ui_last/display_force_cairo"
#define NOT_NOW "cairo: the compositor does not color-manage the window now"
#define PLATFORM_FAILED "cairo: native setup failed: wp_color_manager_v1 not advertised"
#define TRANSPORT_FAILED "cairo: native setup failed: image description for headroom" \
                         " 1.0000 failed (1): refused"

// the plain cases, each at the window's first choice, which is made at
// once

static void test_native(void **state)
{
  _state_t *s = *state;
  _make(s);
  assert_string_equal(_log->str, "Pno");
  assert_true(s->window->native);
  assert_false(s->window->forced);
  assert_false(s->window->failed);
  assert_non_null(s->window->platform);
  assert_int_equal(_published.image.type, DT_COLORSPACE_LIN_REC2020);
  assert_true(_published.image.headroom == 1.0f);
  assert_int_equal(_idles_queued, 0);
  assert_int_equal(_lines->len, 1);
  _assert_line(0, "native");
}

// not implemented here: cairo, which is no failure. taking a force back
// asks again, which changes nothing
static void test_no_platform(void **state)
{
  _state_t *s = *state;
  _platform_absent = TRUE;
  _make(s);
  assert_string_equal(_log->str, "acg");
  assert_false(s->window->native);
  assert_false(s->window->failed);
  _assert_line(0, "cairo: no native presentation on this platform");
  _draw(s);
  _force_cairo = TRUE;
  _preferences_close(s);
  _run_idle();
  _force_cairo = FALSE;
  _preferences_close(s);
  _run_idle();
  assert_string_equal(_log->str, "a");
  assert_int_equal(_lines->len, 1);
}

// no platform is made at all, so the window makes no color-management
// requests
static void test_forced(void **state)
{
  _state_t *s = *state;
  _force_cairo = TRUE;
  _make(s);
  assert_string_equal(_log->str, "cg");
  assert_int_equal(_platforms_made, 0);
  assert_true(s->window->forced);
  assert_null(s->window->platform);
  _assert_line(0, FORCED);
}

// a window being made has no backend, which is not a cairo one: cairo is
// made, and the platform stays to watch the verdict
static void test_not_now(void **state)
{
  _state_t *s = *state;
  _platform_native = FALSE;
  _make(s);
  assert_string_equal(_log->str, "Pcg");
  assert_non_null(s->window->backend);
  assert_false(s->window->native);
  assert_non_null(s->window->platform);
  _assert_line(0, NOT_NOW);
}

// the platform's verdict changing, and the headroom target meanwhile
static void test_verdicts(void **state)
{
  _state_t *s = *state;
  _make(s);
  _draw(s);

  // an unchanged verdict changes nothing
  _choose_now(s);
  assert_string_equal(_log->str, "");

  // queued, not done inside the platform's call. every widget's data goes
  // before the backend that made it, the new backend is made after, cairo
  // gives the output back, then a redraw. the platform stays, watching
  _platform_native = FALSE;
  dt_presenter_window_choose(s->window);
  assert_string_equal(_log->str, "");
  assert_true(s->window->native);
  _run_idle();
  assert_string_equal(_log->str, "rrxcgd");
  assert_false(s->window->native);
  assert_non_null(s->window->platform);
  _assert_released(s);
  _assert_line(1, NOT_NOW);

  // reports reach no cairo backend, and the target keeps settling while
  // the platform watches: a native backend chosen again describes its
  // image for it at setup
  _draw(s);
  const dt_presenter_headroom_t report = { .value = 4.0f, .generation = 1 };
  dt_presenter_window_report_headroom(s->window, &report);
  assert_string_equal(_log->str, "");
  assert_true(fabsf(dt_presenter_window_target(s->window) - 4.0f) < 1e-6f);
  _platform_native = TRUE;
  _choose_now(s);
  assert_string_equal(_log->str, "RRXnod");
  assert_true(s->window->native);
  assert_true(fabsf(_published.image.headroom - 4.0f) < 1e-6f);
  _assert_released(s);
  _assert_line(2, "native");

  // a settled target goes to the native backend only: the output keeps
  // the headroom the backend published until it publishes another, which
  // the core takes as it is
  g_string_truncate(_log, 0);
  const dt_presenter_headroom_t next = { .value = 2.0f, .generation = 2 };
  dt_presenter_window_report_headroom(s->window, &next);
  assert_string_equal(_log->str, "t");
  assert_true(fabsf(_target - 2.0f) < 1e-6f);
  assert_true(fabsf(_published.image.headroom - 4.0f) < 1e-6f);
  _fake_publish(s->window, 407.0f / 203.0f);
  assert_true(_published.image.headroom == 407.0f / 203.0f);

  // verdicts flipping back before the idle runs queue one choice, which
  // then changes nothing
  _draw(s);
  const int queued = _idles_queued;
  for(int k = 0; k < 4; k++)
  {
    _platform_native = k % 2;
    dt_presenter_window_choose(s->window);
  }
  assert_int_equal(_idles_queued, queued + 1);
  _run_idle();
  assert_string_equal(_log->str, "");
  assert_int_equal(_layered_made, 2);
  assert_int_equal(_cairo_made, 1);
}

// failed setups: logged even when the backend stays cairo, not tried
// again by verdicts or by preference closes that leave the force key
// alone, and tried once more whenever the user changes the key. the
// backend always goes before its platform
static void test_setup_failures(void **state)
{
  _state_t *s = *state;
  _make(s);
  _platform_native = FALSE;
  _choose_now(s);
  _draw(s);

  // a failed promotion of a cairo window that watched
  _transport_fails = TRUE;
  _platform_native = TRUE;
  _choose_now(s);
  assert_string_equal(_log->str, "RRXofcgpd");
  assert_false(_owned);
  assert_false(s->window->native);
  assert_true(s->window->failed);
  assert_null(s->window->platform);
  assert_int_equal(_lines->len, 3);
  _assert_line(2, TRANSPORT_FAILED);

  // requests the old platform queued before it went, verdicts both ways,
  // preference closes, and a transport able to set up again: no retry
  _draw(s);
  _transport_fails = FALSE;
  for(int k = 0; k < 4; k++)
  {
    _platform_native = k % 2;
    _choose_now(s);
    _preferences_close(s);
    assert_int_equal(_idle_id, 0);
  }
  assert_string_equal(_log->str, "");
  assert_true(s->window->failed);
  assert_int_equal(_platforms_made, 1);
  assert_int_equal(_layered_made, 2);
  assert_int_equal(_cairo_made, 2);
  assert_int_equal(_lines->len, 3);

  // forced: the failure is cleared, nothing is tried, the backend stays
  _transport_fails = TRUE;
  _force_cairo = TRUE;
  _preferences_close(s);
  assert_false(s->window->failed);
  _run_idle();
  assert_string_equal(_log->str, "");
  assert_int_equal(_platforms_made, 1);
  assert_int_equal(_lines->len, 3);

  // taken back: tried exactly once, failed again, logged again
  _force_cairo = FALSE;
  _preferences_close(s);
  _run_idle();
  assert_string_equal(_log->str, "PRRXofcgpd");
  assert_false(_owned);
  assert_int_equal(_platforms_made, 2);
  assert_int_equal(_layered_made, 3);
  assert_true(s->window->failed);
  assert_int_equal(_lines->len, 4);
  _assert_line(3, TRANSPORT_FAILED);

  // a platform that cannot set up says why, and is tried again only on
  // the next change of the key
  _draw(s);
  _transport_fails = FALSE;
  _platform_fails = TRUE;
  _force_cairo = TRUE;
  _preferences_close(s);
  _run_idle();
  _force_cairo = FALSE;
  _preferences_close(s);
  _run_idle();
  assert_string_equal(_log->str, "q");
  assert_true(s->window->failed);
  assert_null(s->window->platform);
  assert_int_equal(_lines->len, 5);
  _assert_line(4, PLATFORM_FAILED);
  _preferences_close(s);
  dt_presenter_window_choose(s->window);
  _run_idle();
  assert_int_equal(_platforms_made, 3);

  g_string_truncate(_log, 0);
  _platform_fails = FALSE;
  _force_cairo = TRUE;
  _preferences_close(s);
  _run_idle();
  _force_cairo = FALSE;
  _preferences_close(s);
  _run_idle();
  assert_string_equal(_log->str, "PRRXnod");
  assert_true(s->window->native);
  assert_false(s->window->failed);
  assert_int_equal(_platforms_made, 4);
}

// the platform exists only while the window may be native, so a window
// has several in turn. the key is remembered from the first choice on:
// else taking back a force the window started with would equal the zeroed
// record and choose nothing. every platform starts a target of its own,
// its first report taken at once even with its predecessor's generation
static void test_platform_lifetime(void **state)
{
  _state_t *s = *state;
  _force_cairo = TRUE;
  _make(s);
  _draw(s);
  _force_cairo = FALSE;
  _preferences_close(s);
  _run_idle();
  assert_string_equal(_log->str, "PRRXnod");
  assert_int_equal(_platforms_made, 1);

  // whole steps of the target (dt_presenter_target_steps())
  const float values[] = { 4.0f, 2.0f, 8.0f };
  for(int k = 0; k < 3; k++)
  {
    _draw(s);
    const dt_presenter_headroom_t report = { .value = values[k], .generation = 1 };
    dt_presenter_window_report_headroom(s->window, &report);
    assert_string_equal(_log->str, "t");
    assert_true(fabsf(_target - values[k]) < 1e-6f);
    // a lower level waits to settle, on a timer
    const dt_presenter_headroom_t lower = { .value = values[k] / 2.0f, .generation = 1 };
    dt_presenter_window_report_headroom(s->window, &lower);
    assert_true(fabsf(_target - values[k]) < 1e-6f);
    const guint settle = s->window->settle;
    assert_int_not_equal(settle, 0);

    // the backend goes before the platform, and the target and its timer
    // with it
    _force_cairo = TRUE;
    _preferences_close(s);
    _run_idle();
    assert_string_equal(_log->str, "trrxcgpd");
    assert_true(dt_presenter_window_target(s->window) == 1.0f);
    assert_int_equal(s->window->settle, 0);
    assert_int_equal(_removed, settle);

    _draw(s);
    _force_cairo = FALSE;
    _preferences_close(s);
    _run_idle();
    assert_string_equal(_log->str, "PRRXnod");
    assert_true(_published.image.headroom == 1.0f);
  }
  assert_int_equal(_platforms_made, 4);

  // a cairo window watching a "not now" verdict that gets forced keeps
  // its backend, with no line, and loses the platform and its target
  _platform_native = FALSE;
  _choose_now(s);
  const dt_presenter_headroom_t report = { .value = 4.0f, .generation = 2 };
  dt_presenter_window_report_headroom(s->window, &report);
  const dt_presenter_headroom_t lower = { .value = 2.0f, .generation = 2 };
  dt_presenter_window_report_headroom(s->window, &lower);
  const guint settle = s->window->settle;
  assert_int_not_equal(settle, 0);
  _draw(s);
  dt_presenter_backend_t *backend = s->window->backend;
  const guint lines = _lines->len;
  _force_cairo = TRUE;
  _preferences_close(s);
  _run_idle();
  assert_string_equal(_log->str, "p");
  assert_ptr_equal(s->window->backend, backend);
  assert_non_null(s->widgets[0]->data);
  assert_null(s->window->platform);
  assert_true(dt_presenter_window_target(s->window) == 1.0f);
  assert_false(s->window->target.started);
  assert_int_equal(s->window->settle, 0);
  assert_int_equal(_removed, settle);
  assert_int_equal(_lines->len, lines);
}

// a platform choosing from inside its own setup, as Wayland's
// _info_done() does, and a choice during the transport's setup only
// queue: the constructors run once, never nested (they assert it), and
// the idle finds nothing to change. a report during setup, before the
// window has a backend, reaches none and is the target the transport
// describes its image for
static void test_requests_during_setup(void **state)
{
  _state_t *s = *state;
  _platform_hook = HOOK_CHOOSE;
  _layered_hook = HOOK_CHOOSE;
  _make(s);
  assert_string_equal(_log->str, "Pno");
  assert_true(_published.image.headroom == 2.0f);
  assert_int_equal(_idles_queued, 1);
  _draw(s);
  _run_idle();
  assert_string_equal(_log->str, "");
  assert_int_equal(_platforms_made, 1);
  assert_int_equal(_layered_made, 1);
  assert_int_equal(_idle_id, 0);

  // a platform made by a queued choice asks while that idle runs: another
  // choice is queued, which finds nothing to change either
  _force_cairo = TRUE;
  _preferences_close(s);
  _run_idle();
  _draw(s);
  _force_cairo = FALSE;
  _preferences_close(s);
  _run_idle();
  assert_string_equal(_log->str, "PRRXnod");
  assert_int_not_equal(_idle_id, 0);
  _draw(s);
  _run_idle();
  assert_string_equal(_log->str, "");
  assert_int_equal(_platforms_made, 2);
  assert_int_equal(_layered_made, 2);

  // a report during the setup of a platform whose verdict is "not now"
  _force_cairo = TRUE;
  _preferences_close(s);
  _run_idle();
  _draw(s);
  _platform_hook = HOOK_REPORT;
  _platform_native = FALSE;
  _force_cairo = FALSE;
  _preferences_close(s);
  _run_idle();
  assert_string_equal(_log->str, "P");
  assert_true(dt_presenter_window_target(s->window) == 2.0f);

  // a new window whose first choice left a choice queued, and a force
  // set before that idle runs: compared with the key the first choice
  // read, it joins the queued choice, which takes the new key
  _window_free(s->window);
  _platform_hook = HOOK_CHOOSE;
  _platform_native = TRUE;
  _make(s);
  assert_true(s->window->native);
  assert_int_not_equal(_idle_id, 0);
  const int queued = _idles_queued;
  _draw(s);
  _force_cairo = TRUE;
  _preferences_close(s);
  assert_int_equal(_idles_queued, queued);
  _run_idle();
  assert_string_equal(_log->str, "rrxcgpd");
  assert_true(s->window->forced);
  assert_null(s->window->platform);
}

// a window destroyed with a choice queued takes the idle with it, and
// frees its backend after every widget's data and before its platform
static void test_free_with_choice_queued(void **state)
{
  _state_t *s = *state;
  _make(s);
  _draw(s);
  _platform_native = FALSE;
  dt_presenter_window_choose(s->window);
  assert_int_not_equal(_idle_id, 0);
  _window_free(s->window);
  s->window = NULL;
  assert_int_equal(_idles_removed, 1);
  assert_int_equal(_idle_id, 0);
  assert_string_equal(_log->str, "rrxpg");
}

int main(int argc, char *argv[])
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test_setup_teardown(test_native, _setup, _teardown),
    cmocka_unit_test_setup_teardown(test_no_platform, _setup, _teardown),
    cmocka_unit_test_setup_teardown(test_forced, _setup, _teardown),
    cmocka_unit_test_setup_teardown(test_not_now, _setup, _teardown),
    cmocka_unit_test_setup_teardown(test_verdicts, _setup, _teardown),
    cmocka_unit_test_setup_teardown(test_setup_failures, _setup, _teardown),
    cmocka_unit_test_setup_teardown(test_platform_lifetime, _setup, _teardown),
    cmocka_unit_test_setup_teardown(test_requests_during_setup, _setup, _teardown),
    cmocka_unit_test_setup_teardown(test_free_with_choice_queued, _setup, _teardown),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
