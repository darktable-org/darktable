/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable. If not, see <http://www.gnu.org/licenses/>.
*/

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <cmocka.h>
#include <glib.h>
#include "color-management-v1-client-protocol.h"

static int _test_dispatch(struct wl_display *display);
static int _test_prepare(struct wl_display *display);
static int _test_flush(struct wl_display *display);
static int _test_read(struct wl_display *display);
static int _test_fd(struct wl_display *display);
static void _test_cancel(struct wl_display *display);
static int _test_poll(GPollFD *fds, guint nfds, gint timeout);
static struct wp_image_description_info_v1 *_test_information(struct wp_image_description_v1 *description);
static int _test_listener(struct wp_image_description_info_v1 *info,
                          const struct wp_image_description_info_v1_listener *listener, void *data);
static void _test_destroy(struct wp_image_description_info_v1 *info);

#define wl_display_dispatch_pending _test_dispatch
#define wl_display_prepare_read _test_prepare
#define wl_display_flush _test_flush
#define wl_display_read_events _test_read
#define wl_display_get_fd _test_fd
#define wl_display_cancel_read _test_cancel
#define g_poll _test_poll
#define wp_image_description_v1_get_information _test_information
#define wp_image_description_info_v1_add_listener _test_listener
#define wp_image_description_info_v1_destroy _test_destroy
#include "cmstest/wayland.c"
#undef wl_display_dispatch_pending
#undef wl_display_prepare_read
#undef wl_display_flush
#undef wl_display_read_events
#undef wl_display_get_fd
#undef wl_display_cancel_read
#undef g_poll
#undef wp_image_description_v1_get_information
#undef wp_image_description_info_v1_add_listener
#undef wp_image_description_info_v1_destroy

enum { DELAYED, FAILED, REMOVED, TIMEOUT, READ_ERROR, BACKPRESSURE };
static int _scenario, _phase, _canceled, _destroyed, _requested, _polls;
static dt_cmstest_wayland_t _data;
static dt_cmstest_output_t _output;
static char _description_token, _info_token;

static struct wp_image_description_info_v1 *_test_information(struct wp_image_description_v1 *description)
{
  assert_ptr_equal(description, _output.description);
  _requested++;
  return (struct wp_image_description_info_v1 *)&_info_token;
}

static int _test_listener(struct wp_image_description_info_v1 *info,
                          const struct wp_image_description_info_v1_listener *listener, void *data)
{
  assert_ptr_equal(info, _output.info);
  assert_ptr_equal(listener, &_info_listener);
  assert_ptr_equal(data, &_output);
  return 0;
}

static void _test_destroy(struct wp_image_description_info_v1 *info)
{
  assert_ptr_equal(info, _output.info);
  _destroyed++;
}

static int _test_dispatch(struct wl_display *display)
{
  if(_phase == 1)
  {
    if(_scenario == FAILED) _failed(&_output, _output.description, 1, "test failure");
    else if(_scenario == REMOVED) _global_remove(&_data, NULL, _output.id);
    else _ready(&_output, _output.description, 1);
  }
  else if(_phase == 2) _info_done(&_output, _output.info);
  return 0;
}

static int _test_prepare(struct wl_display *display) { return 0; }
static int _test_fd(struct wl_display *display) { return 7; }
static void _test_cancel(struct wl_display *display) { _canceled++; }
static int _test_read(struct wl_display *display)
{
  if(_scenario == READ_ERROR) return -1;
  _phase++;
  return 0;
}

static int _test_flush(struct wl_display *display)
{
  if(_scenario == BACKPRESSURE && !_polls)
  {
    errno = EAGAIN;
    return -1;
  }
  return 0;
}

static int _test_poll(GPollFD *fds, guint nfds, gint timeout)
{
  assert_int_equal(nfds, 1);
  assert_int_equal(fds->fd, 7);
  assert_true(timeout > 0);
  assert_true(timeout <= 1000);
  if(_scenario == TIMEOUT) return 0;
  if(_scenario == BACKPRESSURE && !_polls++)
  {
    assert_true(fds->events & G_IO_OUT);
    fds->revents = G_IO_OUT;
  }
  else fds->revents = G_IO_IN;
  return 1;
}

static void _test_descriptions(void **state)
{
  for(_scenario = DELAYED; _scenario <= BACKPRESSURE; _scenario++)
  {
    _phase = _canceled = _destroyed = _requested = _polls = 0;
    _output = (dt_cmstest_output_t){ .id = 1,
      .description = (struct wp_image_description_v1 *)&_description_token,
      .details = g_string_new(NULL) };
    _data = (dt_cmstest_wayland_t){ .outputs = g_list_append(NULL, &_output) };
    const int result = _wait_for_descriptions(NULL, &_data,
                                             g_get_monotonic_time() + G_TIME_SPAN_SECOND);
    if(_scenario == TIMEOUT)
    {
      assert_int_equal(result, 0);
      assert_false(_output.complete);
      assert_int_equal(_canceled, 1);
    }
    else if(_scenario == READ_ERROR) assert_int_equal(result, -1);
    else
    {
      assert_int_equal(result, 1);
      if(_scenario == REMOVED) assert_true(_output.removed);
      else assert_true(_output.complete);
      if(_scenario == FAILED) assert_true(_output.failed);
      if(_scenario == DELAYED || _scenario == BACKPRESSURE)
      {
        assert_int_equal(_phase, 2);
        assert_int_equal(_requested, 1);
        assert_int_equal(_destroyed, 1);
        assert_null(_output.info);
      }
    }
    g_string_free(_output.details, TRUE);
    g_list_free(_data.outputs);
  }
}

int main(void)
{
  const struct CMUnitTest tests[] = { cmocka_unit_test(_test_descriptions) };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
