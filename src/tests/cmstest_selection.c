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

#include "config.h"
#ifdef CMSTEST_TEST_WAYLAND
#define HAVE_WAYLAND_COLOR_MANAGEMENT
#else
#undef HAVE_WAYLAND_COLOR_MANAGEMENT
#endif
#define main _cmstest_main
#define XOpenDisplay _test_open_display
#define dt_cmstest_wayland _test_wayland
#include "cmstest/main.c"
#undef dt_cmstest_wayland
#undef XOpenDisplay
#undef main

const char darktable_package_version[] = "test";
static int _wayland_calls, _x11_calls;

Display *_test_open_display(const char *name)
{
  _x11_calls++;
  return NULL;
}

#ifdef CMSTEST_TEST_WAYLAND
static int _wayland_result;
int _test_wayland(void)
{
  _wayland_calls++;
  return _wayland_result;
}
#endif

static void _run(const char *option, const int expected_result,
                 const int expected_wayland, const int expected_x11)
{
  char *argv[] = { "darktable-cmstest", (char *)option, NULL };
  _wayland_calls = _x11_calls = 0;
  assert_int_equal(_cmstest_main(option ? 2 : 1, argv), expected_result);
  assert_int_equal(_wayland_calls, expected_wayland);
  assert_int_equal(_x11_calls, expected_x11);
}

static void _test_selection(void **state)
{
  g_unsetenv("WAYLAND_SOCKET");
  g_unsetenv("XDG_SESSION_TYPE");
  g_setenv("WAYLAND_DISPLAY", "test", TRUE);
#ifdef CMSTEST_TEST_WAYLAND
  _wayland_result = EXIT_SUCCESS;
  _run(NULL, EXIT_SUCCESS, 1, 0);
  _run("--wayland", EXIT_SUCCESS, 1, 0);
  _wayland_result = EXIT_FAILURE;
  _run(NULL, EXIT_FAILURE, 1, 0);
  _run("--wayland", EXIT_FAILURE, 1, 0);
  _wayland_result = DT_CMSTEST_WAYLAND_UNAVAILABLE;
  _run(NULL, EXIT_FAILURE, 1, 1);
  _run("--wayland", EXIT_FAILURE, 1, 0);
#else
  _run(NULL, EXIT_FAILURE, 0, 1);
  _run("--wayland", EXIT_FAILURE, 0, 0);
#endif
  _run("--x11", EXIT_FAILURE, 0, 1);
  g_unsetenv("WAYLAND_DISPLAY");
  _run(NULL, EXIT_FAILURE, 0, 1);
  _run("--help", EXIT_SUCCESS, 0, 0);
  _run("--invalid", EXIT_FAILURE, 0, 0);
}

int main(void)
{
  const struct CMUnitTest tests[] = { cmocka_unit_test(_test_selection) };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
