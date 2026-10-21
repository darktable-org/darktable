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
#include "common/tags.h"

static int _count(const char *query)
{
  int count = -1;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  if(sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  return count;
}

static void test_nth_image(void **state)
{
  const dt_imgid_t cr2 = dt_test_catalog_image("mire1.cr2");
  const dt_imgid_t arw = dt_test_catalog_image("hlrecovery.arw");
  guint tagid = 0;
  assert_true(dt_tag_new("helios|nth", &tagid));
  dt_tag_attach(tagid, arw, FALSE, FALSE);
  dt_tag_attach(tagid, cr2, FALSE, FALSE);

  // ordered by image id
  assert_int_equal(dt_tag_get_nth_image(tagid, 0), MIN(cr2, arw));
  assert_int_equal(dt_tag_get_nth_image(tagid, 1), MAX(cr2, arw));
  assert_int_equal(dt_tag_get_nth_image(tagid, 2), NO_IMGID);
  assert_int_equal(dt_tag_images_count(tagid), 2);
}

static void test_tag_ids(void **state)
{
  const dt_imgid_t crw = dt_test_catalog_image("bayer4.crw");
  guint tagid = 0;
  assert_true(dt_tag_new("helios|ids", &tagid));
  dt_tag_attach(tagid, crw, FALSE, FALSE);

  GList *ids = dt_tag_get_tag_ids(crw);
  // darktable's own tags (format) are included
  gchar *q = g_strdup_printf("SELECT COUNT(*) FROM main.tagged_images WHERE imgid = %d", crw);
  assert_int_equal(g_list_length(ids), _count(q));
  g_free(q);
  assert_non_null(g_list_find(ids, GINT_TO_POINTER(tagid)));
  g_list_free(ids);
}

static void test_dictionary(void **state)
{
  const int n = dt_tag_count_tags();
  assert_int_equal(n, _count("SELECT COUNT(*) FROM data.tags"));
  assert_int_equal(dt_tag_get_nth_tag(0), _count("SELECT MIN(id) FROM data.tags"));
  assert_int_equal(dt_tag_get_nth_tag(n - 1), _count("SELECT MAX(id) FROM data.tags"));
  assert_int_equal(dt_tag_get_nth_tag(n), -1);
}

static void test_flags_counts(void **state)
{
  const dt_imgid_t cr2 = dt_test_catalog_image("mire1.cr2");
  const dt_imgid_t arw = dt_test_catalog_image("hlrecovery.arw");
  guint shared = 0, single = 0;
  assert_true(dt_tag_new("helios|flags shared", &shared));
  assert_true(dt_tag_new("helios|flags single", &single));
  dt_tag_attach(shared, cr2, FALSE, FALSE);
  dt_tag_attach(shared, arw, FALSE, FALSE);
  dt_tag_attach(single, arw, FALSE, FALSE);

  gchar *images = g_strdup_printf("%d,%d", cr2, arw);
  GList *counts = dt_tag_get_flags_counts(images);
  g_free(images);
  // one row per user tag on these images; darktable| tags are left out
  int twos = 0, ones = 0;
  for(GList *iter = counts; iter; iter = g_list_next(iter))
  {
    const dt_tag_flags_count_t *fc = iter->data;
    if(fc->count == 2) twos++;
    else if(fc->count == 1) ones++;
  }
  // helios|nth (from test_nth_image) is on both images too
  assert_int_equal(twos, 2);
  assert_int_equal(ones, 1);
  assert_int_equal(g_list_length(counts), 3);
  g_list_free_full(counts, g_free);
}

int main(int argc, char *argv[])
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_nth_image),
    cmocka_unit_test(test_tag_ids),
    cmocka_unit_test(test_dictionary),
    cmocka_unit_test(test_flags_counts),
  };
  return cmocka_run_group_tests(tests, dt_test_catalog_setup, dt_test_catalog_teardown);
}
