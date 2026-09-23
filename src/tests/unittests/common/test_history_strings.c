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
 * cmocka unit tests for the history tooltip's check on char array fields:
 * stored params need not terminate them, so a field without a NUL within
 * its introspected size must be skipped rather than read as a string.
 */

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>

#include <cmocka.h>

#include "libs/history.c"

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

static dt_introspection_field_t _char_array_field(const size_t count)
{
  dt_introspection_field_t field = { 0 };
  field.Array.header.type = DT_INTROSPECTION_TYPE_ARRAY;
  field.Array.type = DT_INTROSPECTION_TYPE_CHAR;
  field.Array.count = count;
  return field;
}

static void test_history_terminated_string(void **state)
{
  const char old_text[8] = "old";
  const char new_text[8] = "new";
  dt_introspection_field_t field = _char_array_field(sizeof(old_text));
  gchar *change = _lib_history_change_text(&field, "borders.aspect_text", NULL,
                                           (gpointer)new_text, (gpointer)old_text);

  assert_non_null(change);
  g_free(change);
}

static void test_history_unterminated_char_array(void **state)
{
  const char old_text[8] = { 'o', 'l', 'd', ' ', ' ', ' ', ' ', ' ' };
  const char new_text[8] = { 'n', 'e', 'w', ' ', ' ', ' ', ' ', ' ' };
  dt_introspection_field_t field = _char_array_field(sizeof(old_text));
  gchar *change = _lib_history_change_text(&field, "borders.aspect_text", NULL,
                                           (gpointer)new_text, (gpointer)old_text);

  assert_null(change);
}

static void test_history_binary_char_array(void **state)
{
  const char old_clut[6] = { 1, 2, 3, 4, 5, 6 };
  const char new_clut[6] = { 7, 8, 9, 10, 11, 12 };
  dt_introspection_field_t field = _char_array_field(sizeof(old_clut));
  gchar *change = _lib_history_change_text(&field, "lut3d.c_clut", NULL,
                                           (gpointer)new_clut, (gpointer)old_clut);

  assert_null(change);
}

int main(int argc, char *argv[])
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_history_terminated_string),
    cmocka_unit_test(test_history_unterminated_char_array),
    cmocka_unit_test(test_history_binary_char_array)
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
