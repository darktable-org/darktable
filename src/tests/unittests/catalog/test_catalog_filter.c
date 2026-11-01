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

#include <glib-2.0/glib.h>
#include <sqlite3.h>
#include <stdarg.h>
#include <stddef.h>
#include <cmocka.h>

#include "catalog_fixture.h"
#include "common/debug.h"
#include "common/filter.h"


static void test_get_range(void **state)
{
  dt_filter_range_t *range = dt_filter_get_range("id", NULL);
  assert_non_null(range);
  assert_int_not_equal(range->min_r, range->max_r);
  dt_filter_range_free(range);
}

static void test_get_range_counts(void **state)
{
  GList *counts = dt_filter_get_range_counts("datetime_taken", "mi.id > 0");
  assert_non_null(counts);
  g_list_free_full(counts, dt_filter_range_count_free);
}

static void test_get_filename_counts(void **state)
{
  GList *counts = dt_filter_get_filename_counts("mi.id > 0");
  assert_non_null(counts);

  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT COUNT(*) FROM main.images",
                              -1, &stmt, NULL);
  assert_int_equal(sqlite3_step(stmt), SQLITE_ROW);
  assert_int_equal(sqlite3_column_int(stmt, 0), g_list_length(counts));
  g_list_free_full(counts, dt_filter_name_count_free);
}

static void test_get_extension_counts(void **state)
{
  GList *counts = dt_filter_get_extension_counts("mi.filename = 'mire1.cr2'");
  assert_int_equal(g_list_length(counts), 1);
  dt_filter_name_count_t *count = counts->data;
  assert_string_equal(count->name, ".CR2");
  assert_int_equal(count->count, 1);
  g_list_free_full(counts, dt_filter_name_count_free);
}

static void test_get_history_counts(void **state)
{
  GList *counts = dt_filter_get_history_counts("mi.filename = 'mire1.cr2'");
  assert_non_null(counts);
  g_list_free_full(counts, dt_filter_int_count_free);
}

static void test_get_local_copy_counts(void **state)
{
  GList *counts = dt_filter_get_local_copy_counts("mi.filename = 'mire1.cr2'");
  assert_non_null(counts);
  g_list_free_full(counts, dt_filter_int_count_free);
}

static void test_get_module_order_counts(void **state)
{
  GList *counts = dt_filter_get_local_copy_counts("mi.filename = 'mire1.cr2'");
  assert_int_equal(g_list_length(counts), 1);
  dt_filter_int_count_t *count = counts->data;
  assert_int_equal(count->val, 1);
  assert_int_equal(count->count, 1);
  g_list_free_full(counts, dt_filter_int_count_free);
}

static void test_get_rating_range_counts(void **state)
{
  GList *counts = dt_filter_get_rating_range_counts("mi.id > 0");
  assert_non_null(counts);
  g_list_free_full(counts, dt_filter_int_count_free);
}

static void test_get_camera_counts(void **state)
{
  GList *counts = dt_filter_get_camera_counts("mi.id > 0");
  assert_non_null(counts);
  g_list_free_full(counts, dt_filter_name_count_free);
}

static void test_get_lens_counts(void **state)
{
  GList *counts = dt_filter_get_lens_counts("mi.id > 0");
  assert_non_null(counts);
  g_list_free_full(counts, dt_filter_name_count_free);
}

static void test_get_image_dimension_counts(void **state)
{
  GList *counts = dt_filter_get_image_dimension_counts("mi.filename = 'mire1.cr2'");
  assert_int_equal(g_list_length(counts), 1);
  dt_filter_name_count_t *count = counts->data;
  assert_string_equal(count->name, "3888x2592");
 g_list_free_full(counts, dt_filter_name_count_free);
}

static void test_get_white_balance_counts(void **state)
{
  GList *counts = dt_filter_get_white_balance_counts("mi.filename = 'mire1.cr2'");
  assert_int_equal(g_list_length(counts), 1);
  dt_filter_name_count_t *count = counts->data;
  assert_string_equal(count->name, "Custom");
  assert_int_equal(count->count, 1);
  g_list_free_full(counts, dt_filter_name_count_free);
}

static void test_get_flash_counts(void **state)
{
  GList *counts = dt_filter_get_flash_counts("mi.filename = 'mire1.cr2'");
  assert_int_equal(g_list_length(counts), 1);
  dt_filter_name_count_t *count = counts->data;
  assert_string_equal(count->name, "No, compulsory");
  assert_int_equal(count->count, 1);
  g_list_free_full(counts, dt_filter_name_count_free);
}

static void test_get_exposure_program_counts(void **state)
{
  GList *counts = dt_filter_get_exposure_program_counts("mi.filename = 'mire1.cr2'");
  assert_int_equal(g_list_length(counts), 1);
  dt_filter_name_count_t *count = counts->data;
  assert_string_equal(count->name, "Manual");
  assert_int_equal(count->count, 1);
  g_list_free_full(counts, dt_filter_name_count_free);
}

static void test_get_metering_mode_counts(void **state)
{
  GList *counts = dt_filter_get_metering_mode_counts("mi.filename = 'mire1.cr2'");
  assert_int_equal(g_list_length(counts), 1);
  dt_filter_name_count_t *count = counts->data;
  assert_string_equal(count->name, "Multi-segment");
  assert_int_equal(count->count, 1);
  g_list_free_full(counts, dt_filter_name_count_free);
} 

static void test_get_group_id_counts(void **state)
{
  GList *counts = dt_filter_get_group_id_counts("mi.id > 0");
  assert_null(counts); // no groups in test database
  g_list_free_full(counts, dt_filter_name_count_free);
} 

static void test_get_get_used_months_mask(void **state)
{
  const int month_mask = dt_filter_get_used_months_mask("datetime_taken", "mi.filename = 'mire1.cr2'");
  assert_int_equal(month_mask, 256);
} 

int main(int argc, char *argv[])
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_get_range),
    cmocka_unit_test(test_get_range_counts),
    cmocka_unit_test(test_get_filename_counts),
    cmocka_unit_test(test_get_extension_counts),
    cmocka_unit_test(test_get_history_counts),
    cmocka_unit_test(test_get_local_copy_counts),
    cmocka_unit_test(test_get_module_order_counts),
    cmocka_unit_test(test_get_rating_range_counts),
    cmocka_unit_test(test_get_camera_counts),
    cmocka_unit_test(test_get_lens_counts),
    cmocka_unit_test(test_get_image_dimension_counts),
    cmocka_unit_test(test_get_white_balance_counts),
    cmocka_unit_test(test_get_flash_counts),
    cmocka_unit_test(test_get_exposure_program_counts),
    cmocka_unit_test(test_get_metering_mode_counts),
    cmocka_unit_test(test_get_group_id_counts),
    cmocka_unit_test(test_get_get_used_months_mask),
  };
  return cmocka_run_group_tests(tests, dt_test_catalog_setup, dt_test_catalog_teardown);
}

