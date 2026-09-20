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
 * cmocka unit tests for the extension-handling helpers in common/utility.c
 */
#include <limits.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <string.h>

#include <cmocka.h>

#include "common/utility.h"

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

static void test_ends_with_extension(void **state)
{
  assert_true(dt_util_str_ends_with_extension("name.jpg", "jpg"));
  assert_true(dt_util_str_ends_with_extension("name.JPG", "jpg"));
  assert_true(dt_util_str_ends_with_extension("name.JpG", "jpg"));
  assert_false(dt_util_str_ends_with_extension("name.png", "jpg"));
  assert_false(dt_util_str_ends_with_extension("namejpg", "jpg"));
  assert_false(dt_util_str_ends_with_extension(".jpg", "jpg")); // no basename before the dot
  assert_false(dt_util_str_ends_with_extension("jpg", "jpg")); // no dot at all
  assert_false(dt_util_str_ends_with_extension(NULL, "jpg"));
  assert_false(dt_util_str_ends_with_extension("name.jpg", NULL));
}

static void test_extension_offset(void **state)
{
  // no extension present yet: offset is the end of the string, i.e. where
  // the caller should append ".ext"
  assert_int_equal(dt_util_str_extension_offset("name", "jpg"), strlen("name"));

  // extension already present, any case: offset is the boundary right
  // before it. This is what the disk storage export's "generate unique
  // filename on conflict" handling relies on to insert a "_01" suffix in
  // the right place for patterns like "$(FILE_NAME).JPG" -- getting this
  // wrong produced "name.JPG_01.jpg" instead of "name_01.jpg".
  assert_int_equal(dt_util_str_extension_offset("name.jpg", "jpg"), strlen("name"));
  assert_int_equal(dt_util_str_extension_offset("name.JPG", "jpg"), strlen("name"));

  // mismatched extension: treated as "not present", offset is the full
  // (mismatched) string end
  assert_int_equal(dt_util_str_extension_offset("name.png", "jpg"), strlen("name.png"));
}

int main(int argc, char *argv[])
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_ends_with_extension),
    cmocka_unit_test(test_extension_offset),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
