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
 * cmocka unit tests for dt_strlcpy_fixed_to_fixed(), which copies a
 * fixed-size char array from stored params that need not be NUL-terminated.
 */

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <string.h>

#include <cmocka.h>

#include "common/utility.h"

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

typedef struct stored_t
{
  char name[4];
  char following[8];
} stored_t;

static void test_unterminated_source(void **state)
{
  // a params blob darktable did not write: no NUL inside the field
  const stored_t stored = { { 'a', 'b', 'c', 'd' }, "tail" };
  char dest[16];

  dt_strlcpy_fixed_to_fixed(dest, sizeof(dest), stored.name, sizeof(stored.name));

  assert_string_equal(dest, "abcd");
}

static void test_destination_limit_and_clearing(void **state)
{
  const char src[8] = "abcdef";
  char dest[8];

  memset(dest, 'Q', sizeof(dest));
  dt_strlcpy_fixed_to_fixed(dest, 4, src, sizeof(src));
  assert_string_equal(dest, "abc");
  assert_int_equal(dest[4], 'Q');

  // the unused tail is cleared, since params are serialized whole
  memset(dest, 'Q', sizeof(dest));
  dt_strlcpy_fixed_to_fixed(dest, sizeof(dest), "ab", 3);
  assert_string_equal(dest, "ab");
  for(size_t k = 2; k < sizeof(dest); k++)
    assert_int_equal(dest[k], 0);

  dest[0] = 'Q';
  dt_strlcpy_fixed_to_fixed(dest, 0, src, sizeof(src));
  assert_int_equal(dest[0], 'Q');
}

int main(int argc, char *argv[])
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_unterminated_source),
    cmocka_unit_test(test_destination_limit_and_clearing),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
