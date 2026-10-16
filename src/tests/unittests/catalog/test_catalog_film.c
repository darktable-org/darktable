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
#include <string.h>
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

static void test_history_end(void **state)
{
  const dt_imgid_t imgid = dt_test_catalog_image("hlrecovery.arw");
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db),
                        "DELETE FROM main.history", NULL, NULL, NULL);

  // no history: synced to 0
  dt_image_sync_history_end(imgid);
  int end = -1;
  assert_true(dt_image_get_history_end(imgid, &end));
  assert_int_equal(end, 0);

  assert_true(dt_image_shift_history_end(imgid, 3));
  assert_true(dt_image_get_history_end(imgid, &end));
  assert_int_equal(end, 3);

  // missing image: FALSE, value untouched
  end = 42;
  assert_false(dt_image_get_history_end(imgid + 1000, &end));
  assert_int_equal(end, 42);

  dt_image_sync_history_end(imgid);
  assert_true(dt_image_get_history_end(imgid, &end));
  assert_int_equal(end, 0);
}

static void test_image_range(void **state)
{
  const dt_imgid_t lo = _count("SELECT MIN(id) FROM main.images");
  const dt_imgid_t hi = _count("SELECT MAX(id) FROM main.images");
  const int n = dt_image_count();
  assert_int_equal(dt_image_count_in_range(lo, hi), n);
  assert_int_equal(dt_image_count_in_range(lo, lo), 1);
  assert_int_equal(dt_image_count_in_range(hi, lo), 0);

  GList *imgs = dt_image_get_in_range(lo, hi);
  assert_int_equal(g_list_length(imgs), n);
  const dt_image_id_filename_t *first = imgs->data;
  assert_int_equal(first->id, lo);
  assert_non_null(first->filename);
  g_list_free_full(imgs, dt_image_id_filename_free);
  assert_null(dt_image_get_in_range(hi, lo));
}

static gboolean _collect_row(const dt_image_sidecar_row_t *row, void *data)
{
  GPtrArray *names = data;
  g_ptr_array_add(names, g_strdup(row->filename));
  assert_non_null(row->folder);
  return names->len < 2; // stop after two rows
}

static gboolean _count_row(const dt_image_sidecar_row_t *row, void *data)
{
  (*(int *)data)++;
  return TRUE;
}

static void test_sidecar_rows(void **state)
{
  int n = 0;
  dt_image_foreach_sidecar_row(NO_FILMID, _count_row, &n);
  assert_int_equal(n, dt_image_count());

  const dt_filmid_t film = dt_film_get_nth(0);
  n = 0;
  dt_image_foreach_sidecar_row(film, _count_row, &n);
  assert_int_equal(n, dt_film_count_images(film));

  // ordered by film roll then filename, and stopped by the callback
  GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
  dt_image_foreach_sidecar_row(film, _collect_row, names);
  assert_int_equal(names->len, 2);
  assert_true(strcmp(g_ptr_array_index(names, 0), g_ptr_array_index(names, 1)) <= 0);
  g_ptr_array_free(names, TRUE);
}

static void test_image_flags_and_stamps(void **state)
{
  const dt_imgid_t cr2 = dt_test_catalog_image("mire1.cr2");
  const dt_imgid_t arw = dt_test_catalog_image("hlrecovery.arw");
  const dt_imgid_t ids[] = { cr2, arw };
  gchar *q0 = g_strdup_printf("SELECT flags FROM main.images WHERE id = %d", cr2);
  gchar *q1 = g_strdup_printf("SELECT flags FROM main.images WHERE id = %d", arw);
  const int saved[] = { _count(q0), _count(q1) };
  g_free(q0);
  g_free(q1);
  const int flags[] = { 3, 5 };
  dt_image_set_flags_list(ids, flags, 2);
  gchar *q = g_strdup_printf("SELECT flags FROM main.images WHERE id = %d", cr2);
  assert_int_equal(_count(q), 3);
  g_free(q);
  q = g_strdup_printf("SELECT flags FROM main.images WHERE id = %d", arw);
  assert_int_equal(_count(q), 5);
  g_free(q);

  dt_image_set_flags_list(ids, saved, 2);

  dt_image_set_write_timestamp(cr2, 1234567);
  q = g_strdup_printf("SELECT write_timestamp FROM main.images WHERE id = %d", cr2);
  assert_int_equal(_count(q), 1234567);
  g_free(q);
}

static void test_thumb_state(void **state)
{
  const int n = dt_image_count();
  dt_image_reset_thumb_state();
  assert_int_equal(_count("SELECT COUNT(*) FROM main.images WHERE thumb_maxmip = 0 AND thumb_timestamp = -1"), n);
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db),
                        "UPDATE main.images SET import_timestamp = 100, change_timestamp = 200",
                        NULL, NULL, NULL);

  // all outdated, by descending id, with the newer of the two timestamps
  const dt_imgid_t last = _count("SELECT MAX(id) FROM main.images");
  GList *out = dt_image_get_outdated_thumbs(2);
  assert_int_equal(g_list_length(out), n);
  const dt_image_thumb_stamp_t *first = out->data;
  assert_int_equal(first->id, last);
  assert_int_equal(first->stamp, 200);
  g_list_free_full(out, g_free);

  // made up to mip 2 after the change: no longer outdated at 2, still at 3
  dt_image_set_thumb_state(last, 2, 300);
  out = dt_image_get_outdated_thumbs(2);
  assert_int_equal(g_list_length(out), n - 1);
  g_list_free_full(out, g_free);
  out = dt_image_get_outdated_thumbs(3);
  assert_int_equal(g_list_length(out), n);
  g_list_free_full(out, g_free);
}

static void test_film_by_access(void **state)
{
  GList *rolls = dt_film_get_by_access();
  assert_int_equal(g_list_length(rolls), dt_film_count());
  g_list_free(rolls);
}

static void test_jobs_queries(void **state)
{
  const dt_filmid_t film = dt_film_get_nth(0);
  GList *imgs = dt_film_get_images(film);
  assert_int_equal(g_list_length(imgs), dt_film_count_images(film));
  g_list_free(imgs);

  const dt_imgid_t cr2 = dt_test_catalog_image("mire1.cr2");
  gchar *q = g_strdup_printf("SELECT write_timestamp FROM main.images WHERE id = %d", cr2);
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db),
                        "UPDATE main.images SET write_timestamp = 0", NULL, NULL, NULL);
  dt_image_set_write_timestamp_now(cr2);
  assert_true(_count(q) > 0);
  g_free(q);

  // the remove and delete jobs pass ids as a text list built by
  // _get_image_list(); one id is what this covers
  gchar *one = g_strdup_printf("%6d", cr2);
  GList *paths = dt_image_get_full_paths_in_list(one);
  assert_int_equal(g_list_length(paths), 1);
  assert_non_null(strstr(paths->data, "mire1.cr2"));
  g_list_free_full(paths, g_free);

  assert_null(dt_image_get_with_flags_in_list(one, 1 << 30));
  dt_image_add_flags_in_list(one, 1 << 30);
  GList *flagged = dt_image_get_with_flags_in_list(one, 1 << 30);
  assert_int_equal(g_list_length(flagged), 1);
  assert_int_equal(GPOINTER_TO_INT(flagged->data), cr2);
  g_list_free(flagged);
  g_free(one);

  assert_int_equal(dt_image_count_same_file(cr2), 1);
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
    cmocka_unit_test(test_history_end),
    cmocka_unit_test(test_image_range),
    cmocka_unit_test(test_sidecar_rows),
    cmocka_unit_test(test_image_flags_and_stamps),
    cmocka_unit_test(test_thumb_state),
    cmocka_unit_test(test_film_by_access),
    cmocka_unit_test(test_jobs_queries),
    cmocka_unit_test(test_group_members),
  };
  return cmocka_run_group_tests(tests, dt_test_catalog_setup, dt_test_catalog_teardown);
}
