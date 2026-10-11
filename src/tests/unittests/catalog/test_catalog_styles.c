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

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "catalog_fixture.h"
#include "common/database.h"
#include "common/debug.h"
#include "common/styles.h"

static int _count_styles(void)
{
  int count = -1;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT COUNT(*) FROM data.styles", -1, &stmt, NULL);
  if(sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  return count;
}

static void test_count_and_nth(void **state)
{
  const dt_imgid_t cr2 = dt_test_catalog_image("mire1.cr2");
  const uint32_t before = dt_styles_count();
  assert_int_equal(before, _count_styles());

  // created out of order: the n-th style is by name
  assert_true(dt_styles_create_from_image("test style b", "", cr2, NULL, FALSE));
  assert_true(dt_styles_create_from_image("test style a", "", cr2, NULL, FALSE));
  const uint32_t n = dt_styles_count();
  assert_int_equal(n, before + 2);

  gchar *prev = NULL;
  gboolean seen_a = FALSE, seen_b = FALSE;
  for(uint32_t i = 0; i < n; i++)
  {
    gchar *name = dt_styles_get_nth_name(i);
    assert_non_null(name);
    if(prev) assert_true(strcmp(prev, name) <= 0);
    if(!strcmp(name, "test style a")) seen_a = TRUE;
    if(!strcmp(name, "test style b"))
    {
      assert_true(seen_a);
      seen_b = TRUE;
    }
    g_free(prev);
    prev = name;
  }
  g_free(prev);
  assert_true(seen_a && seen_b);
  assert_null(dt_styles_get_nth_name(n));
}

int main(int argc, char *argv[])
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_count_and_nth),
  };
  return cmocka_run_group_tests(tests, dt_test_catalog_setup, dt_test_catalog_teardown);
}
