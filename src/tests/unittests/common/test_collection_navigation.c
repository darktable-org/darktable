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
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <cmocka.h>

#include "common/collection_navigation.h"

static dt_collection_navigation_t _history;

static int _cleanup(void **state)
{
  dt_collection_navigation_clear(&_history);
  return 0;
}

static void _empty(void **state)
{
  assert_false(dt_collection_navigation_can_move(&_history, -1));
  assert_false(dt_collection_navigation_can_move(&_history, 1));
  assert_null(dt_collection_navigation_move(&_history, -1, 15));
}

static void _round_trip(void **state)
{
  dt_collection_navigation_record(&_history, "A", 1);
  dt_collection_navigation_record(&_history, "B", 15);
  dt_collection_navigation_record(&_history, "C", 10);
  const dt_collection_navigation_entry_t *entry =
    dt_collection_navigation_move(&_history, -1, 5);
  assert_string_equal(entry->rules, "B");
  assert_int_equal(entry->position, 10);
  entry = dt_collection_navigation_move(&_history, -1, 10);
  assert_string_equal(entry->rules, "A");
  assert_int_equal(entry->position, 15);
  assert_null(dt_collection_navigation_move(&_history, -1, 15));
  entry = dt_collection_navigation_move(&_history, 1, 15);
  assert_string_equal(entry->rules, "B");
  assert_int_equal(entry->position, 10);
  entry = dt_collection_navigation_move(&_history, 1, 10);
  assert_string_equal(entry->rules, "C");
  assert_int_equal(entry->position, 5);
  assert_false(dt_collection_navigation_can_move(&_history, 1));
}

static void _branch(void **state)
{
  dt_collection_navigation_record(&_history, "A", 1);
  dt_collection_navigation_record(&_history, "B", 15);
  dt_collection_navigation_record(&_history, "C", 10);
  dt_collection_navigation_move(&_history, -1, 5);
  dt_collection_navigation_record(&_history, "D", 10);
  assert_false(dt_collection_navigation_can_move(&_history, 1));
  assert_null(dt_collection_navigation_move(&_history, 1, 1));
  assert_string_equal(_history.entries[_history.current].rules, "D");
  assert_string_equal(dt_collection_navigation_move(&_history, -1, 7)->rules, "B");
  assert_int_equal(dt_collection_navigation_move(&_history, 1, 10)->position, 7);
}

static void _reload(void **state)
{
  dt_collection_navigation_record(&_history, "A", 1);
  dt_collection_navigation_record(&_history, "B", 15);
  dt_collection_navigation_move(&_history, -1, 10);
  dt_collection_navigation_record(&_history, "A", 20);
  assert_int_equal(_history.length, 2);
  assert_true(dt_collection_navigation_can_move(&_history, 1));
  dt_collection_navigation_move(&_history, 1, 20);
  assert_int_equal(dt_collection_navigation_move(&_history, -1, 10)->position, 20);
}

static void _chronological(void **state)
{
  dt_collection_navigation_record(&_history, "A", 1);
  dt_collection_navigation_record(&_history, "B", 15);
  dt_collection_navigation_record(&_history, "A", 10);
  assert_int_equal(_history.length, 3);
  assert_string_equal(dt_collection_navigation_move(&_history, -1, 7)->rules, "B");
  assert_int_equal(dt_collection_navigation_move(&_history, -1, 10)->position, 15);
}

static void _bounded(void **state)
{
  for(int i = 0; i < 100; i++)
  {
    char *rules = g_strdup_printf("collection %d", i);
    dt_collection_navigation_record(&_history, rules, i);
    g_free(rules);
  }
  assert_int_equal(_history.length, DT_COLLECTION_NAVIGATION_LIMIT);
  for(int i = 98; i >= 68; i--)
  {
    char *rules = g_strdup_printf("collection %d", i);
    assert_string_equal(dt_collection_navigation_move(&_history, -1, i + 1)->rules, rules);
    g_free(rules);
  }
  assert_null(dt_collection_navigation_move(&_history, -1, 1));
}

static void _invalid(void **state)
{
  dt_collection_navigation_record(&_history, NULL, 1);
  dt_collection_navigation_record(&_history, "", 1);
  assert_int_equal(_history.length, 0);
  dt_collection_navigation_record(&_history, "A", 0);
  dt_collection_navigation_record(&_history, "B", -5);
  assert_null(dt_collection_navigation_move(&_history, 2, 1));
  assert_int_equal(dt_collection_navigation_move(&_history, -1, 0)->position, 1);
}

static void _clear(void **state)
{
  dt_collection_navigation_record(&_history, "A", 1);
  dt_collection_navigation_record(&_history, "B", 15);
  dt_collection_navigation_clear(&_history);
  assert_int_equal(_history.length, 0);
  dt_collection_navigation_record(&_history, "C", 1);
  assert_false(dt_collection_navigation_can_move(&_history, -1));
  assert_string_equal(_history.entries[0].rules, "C");
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test_teardown(_empty, _cleanup),
    cmocka_unit_test_teardown(_round_trip, _cleanup),
    cmocka_unit_test_teardown(_branch, _cleanup),
    cmocka_unit_test_teardown(_reload, _cleanup),
    cmocka_unit_test_teardown(_chronological, _cleanup),
    cmocka_unit_test_teardown(_bounded, _cleanup),
    cmocka_unit_test_teardown(_invalid, _cleanup),
    cmocka_unit_test_teardown(_clear, _cleanup),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
