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

    Regression tests for dt_exec_command_sync(): it must keep system()'s
    observable contract -- success returns 0, a failing command returns a
    non-zero integer, and a NULL command returns -1 -- and it must keep driving
    the command through a shell, so cmd builtins / redirection that rely on the
    shell layer don't start failing (see issue #17193).
*/
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include <cmocka.h>

#include "common/process.h"

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

static void test_exec_success(void **state)
{
  assert_int_equal(dt_exec_command_sync("exit 0"), 0);
}

static void test_exec_shell_builtin(void **state)
{
  // 'echo' is a shell builtin, not an executable. If the helper stops routing
  // the command through the shell (e.g. it spawns the first token directly), it
  // returns -1 on Windows because CreateProcess can't find an "echo.exe".
  assert_int_equal(dt_exec_command_sync("echo darktable is fine"), 0);
}

static void test_exec_nonzero_exit(void **state)
{
  assert_int_not_equal(dt_exec_command_sync("exit 1"), 0);
}

static void test_exec_null(void **state)
{
  assert_int_equal(dt_exec_command_sync(NULL), -1);
}

int main(int argc, char *argv[])
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_exec_success),
    cmocka_unit_test(test_exec_shell_builtin),
    cmocka_unit_test(test_exec_nonzero_exit),
    cmocka_unit_test(test_exec_null),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
