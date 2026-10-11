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
#include "common/film.h"
#include "common/grouping.h"
#include "common/image.h"
#include "common/image_cache.h"

static int _count(const char *query)
{
  int count = -1;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  if(sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  return count;
}

static void test_films(void **state)
{
  const int n = dt_film_count();
  assert_int_equal(n, _count("SELECT COUNT(*) FROM main.film_rolls"));
  assert_true(n >= 1);
  const dt_filmid_t first = dt_film_get_nth(0);
  assert_int_equal(first, _count("SELECT MIN(id) FROM main.film_rolls"));
  assert_int_equal(dt_film_get_nth(n), NO_FILMID);

  gchar *folder = dt_film_get_folder(first);
  assert_non_null(folder);
  assert_string_equal(folder, DT_TEST_IMAGES);
  g_free(folder);
  assert_null(dt_film_get_folder(first + 1000));

  // the fixture imported 3 images into this film roll, ordered by id
  assert_int_equal(dt_film_count_images(first), 3);
  dt_imgid_t prev = NO_IMGID;
  for(int i = 0; i < 3; i++)
  {
    const dt_imgid_t imgid = dt_film_get_nth_image(first, i);
    assert_true(imgid > prev);
    prev = imgid;
  }
  assert_int_equal(dt_film_get_nth_image(first, 3), NO_IMGID);
}

static void test_images(void **state)
{
  const int n = dt_image_count();
  assert_int_equal(n, _count("SELECT COUNT(*) FROM main.images"));
  assert_int_equal(dt_image_get_nth(0), _count("SELECT MIN(id) FROM main.images"));
  assert_int_equal(dt_image_get_nth(n - 1), _count("SELECT MAX(id) FROM main.images"));
  assert_int_equal(dt_image_get_nth(n), NO_IMGID);
}

static void test_image_lists(void **state)
{
  const int n = dt_image_count();
  GList *all = dt_image_get_all();
  assert_int_equal(g_list_length(all), n);
  assert_non_null(g_list_find(all, GINT_TO_POINTER(dt_test_catalog_image("mire1.cr2"))));
  g_list_free(all);

  dt_imgid_t first = NO_IMGID;
  assert_int_equal(dt_image_count_in_query("SELECT id FROM main.images", &first), n);
  assert_true(dt_is_valid_imgid(first));
  first = NO_IMGID;
  assert_int_equal(dt_image_count_in_query("SELECT id FROM main.images WHERE id < 0", &first), 0);

  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db),
                        "UPDATE main.images SET thumb_maxmip = 6", NULL, NULL, NULL);
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db),
                        "UPDATE main.images SET thumb_maxmip = 2 WHERE id = (SELECT MIN(id) FROM main.images)",
                        NULL, NULL, NULL);
  dt_image_cap_thumb_maxmip(4);
  assert_int_equal(_count("SELECT COUNT(*) FROM main.images WHERE thumb_maxmip = 4"), n - 1);
  assert_int_equal(_count("SELECT COUNT(*) FROM main.images WHERE thumb_maxmip = 2"), 1);
}

static void test_group_members(void **state)
{
  const dt_imgid_t crw = dt_test_catalog_image("bayer4.crw");
  const dt_imgid_t dup = dt_image_duplicate(crw);
  assert_true(dt_is_valid_imgid(dup));
  dt_grouping_add_to_group(crw, dup);

  const dt_image_t *img = dt_image_cache_get(crw, 'r');
  const dt_imgid_t group_id = img->group_id;
  dt_image_cache_read_release(img);

  GList *members = dt_grouping_get_members(group_id);
  assert_int_equal(g_list_length(members), 2);
  assert_non_null(g_list_find(members, GINT_TO_POINTER(crw)));
  assert_non_null(g_list_find(members, GINT_TO_POINTER(dup)));
  g_list_free(members);

  GList *info = dt_grouping_get_members_info(group_id);
  assert_int_equal(g_list_length(info), 2);
  for(const GList *l = info; l; l = g_list_next(l))
  {
    const dt_grouping_member_t *m = l->data;
    assert_true(m->id == crw || m->id == dup);
    assert_string_equal(m->filename, "bayer4.crw");
    assert_int_equal(m->version > 0, m->id == dup);
  }
  g_list_free_full(info, dt_grouping_member_free);

  assert_int_equal(dt_grouping_count_others(group_id, crw), 1);
  assert_true(dt_grouping_has_others(crw));
  assert_true(dt_grouping_has_others(dup));
  assert_false(dt_grouping_has_others(dt_test_catalog_image("mire1.cr2")));

  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db),
                        "DELETE FROM main.selected_images", NULL, NULL, NULL);
  dt_grouping_select_members(group_id);
  assert_int_equal(_count("SELECT COUNT(*) FROM main.selected_images"), 2);
}

int main(int argc, char *argv[])
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_films),
    cmocka_unit_test(test_images),
    cmocka_unit_test(test_image_lists),
    cmocka_unit_test(test_group_members),
  };
  return cmocka_run_group_tests(tests, dt_test_catalog_setup, dt_test_catalog_teardown);
}
