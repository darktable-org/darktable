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
#include "common/image.h"
#include "common/image_cache.h"
#include "common/database.h"
#include "common/debug.h"
#include "common/metadata.h"

static gchar *_ids(const dt_imgid_t a, const dt_imgid_t b)
{
  return g_strdup_printf("%d,%d", a, b);
}

static const dt_metadata_count_t *_find(GList *counts, const uint32_t key)
{
  for(GList *iter = counts; iter; iter = g_list_next(iter))
  {
    const dt_metadata_count_t *c = iter->data;
    if(c->key == key) return c;
  }
  return NULL;
}

static void test_get_counts(void **state)
{
  const dt_imgid_t cr2 = dt_test_catalog_image("mire1.cr2");
  const dt_imgid_t arw = dt_test_catalog_image("hlrecovery.arw");
  dt_metadata_set(cr2, "Xmp.dc.title", "shared", FALSE);
  dt_metadata_set(arw, "Xmp.dc.title", "shared", FALSE);
  dt_metadata_set(cr2, "Xmp.dc.description", "only cr2", FALSE);

  gchar *images = _ids(cr2, arw);
  GList *counts = dt_metadata_get_counts(images);
  g_free(images);

  const dt_metadata_count_t *title = _find(counts, dt_metadata_get_keyid("Xmp.dc.title"));
  const dt_metadata_count_t *desc = _find(counts, dt_metadata_get_keyid("Xmp.dc.description"));
  assert_non_null(title);
  assert_string_equal(title->value, "shared");
  assert_int_equal(title->count, 2);
  assert_non_null(desc);
  assert_string_equal(desc->value, "only cr2");
  assert_int_equal(desc->count, 1);
  g_list_free_full(counts, dt_metadata_count_free);
}

static void test_definition_lifecycle(void **state)
{
  const dt_imgid_t crw = dt_test_catalog_image("bayer4.crw");
  dt_metadata_t *md = calloc(1, sizeof(dt_metadata_t));
  md->tagname = g_strdup("Xmp.helios.test");
  md->name = g_strdup("helios test");
  md->visible = TRUE;
  assert_true(dt_metadata_add_metadata(md));
  dt_metadata_set(crw, "Xmp.helios.test", "value", FALSE);

  GList *keys = g_list_prepend(NULL, GINT_TO_POINTER(md->key));
  assert_int_equal(dt_metadata_count_assigned(keys), 1);

  dt_metadata_update_metadata(&(dt_metadata_t){ .key = md->key, .name = "renamed",
                                                .visible = FALSE, .priv = TRUE,
                                                .display_order = 42 });
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT name, visible, private, display_order FROM data.meta_data WHERE key = ?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, md->key);
  assert_int_equal(sqlite3_step(stmt), SQLITE_ROW);
  assert_string_equal((const char *)sqlite3_column_text(stmt, 0), "renamed");
  assert_int_equal(sqlite3_column_int(stmt, 1), 0);
  assert_int_equal(sqlite3_column_int(stmt, 2), 1);
  assert_int_equal(sqlite3_column_int(stmt, 3), 42);
  sqlite3_finalize(stmt);

  dt_metadata_remove_keys(keys);
  assert_int_equal(dt_metadata_count_assigned(keys), 0);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT COUNT(*) FROM data.meta_data WHERE key = ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, md->key);
  assert_int_equal(sqlite3_step(stmt), SQLITE_ROW);
  assert_int_equal(sqlite3_column_int(stmt, 0), 0);
  sqlite3_finalize(stmt);
  g_list_free(keys);
}

static void test_get_versions(void **state)
{
  const dt_imgid_t cr2 = dt_test_catalog_image("mire1.cr2");
  const dt_imgid_t dup = dt_image_duplicate(cr2);
  assert_true(dt_is_valid_imgid(dup));
  dt_metadata_set(dup, "Xmp.darktable.version_name", "second", FALSE);

  const dt_image_t *img = dt_image_cache_get(cr2, 'r');
  const dt_filmid_t filmid = img->film_id;
  gchar *filename = g_strdup(img->filename);
  dt_image_cache_read_release(img);

  GList *versions = dt_metadata_get_versions(filmid, filename);
  g_free(filename);
  assert_int_equal(g_list_length(versions), 2);
  const dt_metadata_version_t *v0 = versions->data;
  const dt_metadata_version_t *v1 = versions->next->data;
  assert_int_equal(v0->version, 0);
  assert_int_equal(v0->imgid, cr2);
  assert_null(v0->name);
  assert_int_equal(v1->version, 1);
  assert_int_equal(v1->imgid, dup);
  assert_string_equal(v1->name, "second");
  g_list_free_full(versions, dt_metadata_version_free);
}

int main(int argc, char *argv[])
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_get_counts),
    cmocka_unit_test(test_definition_lifecycle),
    cmocka_unit_test(test_get_versions),
  };
  return cmocka_run_group_tests(tests, dt_test_catalog_setup, dt_test_catalog_teardown);
}
