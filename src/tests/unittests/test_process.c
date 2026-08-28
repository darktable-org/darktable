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
#include <string.h>

#include <cmocka.h>

#include "common/process.h"

#ifdef _WIN32
#include "win/main_wrapper.h"
#include <glib/gstdio.h>
#include <windows.h>

typedef struct _decoy_interpreter
{
  gchar *directory;
  gchar *path;
} _decoy_interpreter_t;

static _decoy_interpreter_t _create_decoy_interpreter(void)
{
  _decoy_interpreter_t decoy = { 0 };
  GError *error = NULL;
  decoy.directory = g_dir_make_tmp("darktable-process-test-XXXXXX", &error);
  assert_non_null(decoy.directory);
  assert_null(error);
  decoy.path = g_build_filename(decoy.directory, "cmd.exe", NULL);

  wchar_t module_path[32768];
  const DWORD length = GetModuleFileNameW(NULL, module_path, G_N_ELEMENTS(module_path));
  assert_true(length > 0 && length < G_N_ELEMENTS(module_path));
  gunichar2 *wide_decoy = g_utf8_to_utf16(decoy.path, -1, NULL, NULL, NULL);
  assert_non_null(wide_decoy);
  assert_true(CopyFileW(module_path, (const wchar_t *)wide_decoy, FALSE));
  g_free(wide_decoy);
  return decoy;
}

static void _remove_decoy_interpreter(_decoy_interpreter_t *decoy)
{
  if(decoy->path)
  {
    g_remove(decoy->path);
    g_free(decoy->path);
  }
  if(decoy->directory)
  {
    g_rmdir(decoy->directory);
    g_free(decoy->directory);
  }
}

static gchar *_save_comspec(void)
{
  return g_strdup(g_getenv("COMSPEC"));
}

static void _restore_comspec(gchar *saved)
{
  if(saved) g_setenv("COMSPEC", saved, TRUE);
  else g_unsetenv("COMSPEC");
  g_free(saved);
}
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

#ifdef _WIN32
static void test_exec_ignores_current_directory_decoy(void **state)
{
  _decoy_interpreter_t decoy = _create_decoy_interpreter();
  gchar *original_directory = g_get_current_dir();
  assert_int_equal(g_chdir(decoy.directory), 0);
  const int status = dt_exec_command_sync("exit 0");
  const int restore_status = g_chdir(original_directory);
  g_free(original_directory);
  _remove_decoy_interpreter(&decoy);

  assert_int_equal(restore_status, 0);
  assert_int_equal(status, 0);
}

static void test_exec_honors_comspec(void **state)
{
  _decoy_interpreter_t decoy = _create_decoy_interpreter();
  gchar *saved_comspec = _save_comspec();
  assert_true(g_setenv("COMSPEC", decoy.path, TRUE));
  const int status = dt_exec_command_sync("exit 0");
  _restore_comspec(saved_comspec);
  _remove_decoy_interpreter(&decoy);

  assert_int_equal(status, 91);
}
#endif

int main(int argc, char *argv[])
{
#ifdef _WIN32
  // the test executable doubles as a controlled COMSPEC target
  if(argc > 1 && strcmp(argv[1], "/d") == 0) return 91;
#endif

  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_exec_success),
    cmocka_unit_test(test_exec_shell_builtin),
    cmocka_unit_test(test_exec_nonzero_exit),
    cmocka_unit_test(test_exec_null),
#ifdef _WIN32
    cmocka_unit_test(test_exec_ignores_current_directory_decoy),
    cmocka_unit_test(test_exec_honors_comspec),
#endif
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
