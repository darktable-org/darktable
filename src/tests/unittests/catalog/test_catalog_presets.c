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
#include <float.h>

#include "catalog_fixture.h"
#include "common/presets.h"

// a module that registers no preset of its own
#define OP "helios_test_op"

static void test_edit_dialog_round_trip(void **state)
{
  assert_false(dt_presets_exists("mine", OP, 3));

  // a library module's preset: no module params, no blend params
  dt_presets_write_t p = {
    .name = "mine", .description = "first", .model = "%", .maker = "%", .lens = "%",
    .iso_min = 0.0, .iso_max = FLT_MAX, .exposure_min = 0.0, .exposure_max = 100000000.0,
    .aperture_min = 0.0, .aperture_max = 100000000.0,
    .focal_length_min = 0.0, .focal_length_max = 1000.0,
    .format = 0, .operation = OP, .op_version = 3,
  };
  dt_presets_write(-1, &p);
  assert_true(dt_presets_exists("mine", OP, 3));
  assert_false(dt_presets_exists("mine", OP, 4));

  dt_presets_details_t d = { 0 };
  assert_true(dt_presets_get_details("mine", OP, 3, &d));
  assert_string_equal(d.description, "first");
  assert_string_equal(d.maker, "%");
  // the dialog spots an empty upper ISO bound by SQLite's text of FLT_MAX
  assert_true(g_str_has_suffix(d.iso_max, "e+38"));
  assert_int_equal(d.op_params_size, 0);
  const int rowid = d.rowid;
  dt_presets_details_clear(&d);
  assert_null(d.description);

  char *operation = NULL;
  int version = 0;
  assert_true(dt_presets_get_operation(rowid, &operation, &version));
  assert_string_equal(operation, OP);
  assert_int_equal(version, 3);
  g_free(operation);
  assert_false(dt_presets_get_operation(-5, &operation, &version));

  // an update keeps the row and renames it
  p.name = "renamed";
  p.description = "second";
  p.autoapply = 1;
  dt_presets_write(rowid, &p);
  assert_false(dt_presets_exists("mine", OP, 3));
  assert_true(dt_presets_get_details("renamed", OP, 3, &d));
  assert_int_equal(d.rowid, rowid);
  assert_string_equal(d.description, "second");
  assert_int_equal(d.autoapply, 1);
  dt_presets_details_clear(&d);

  dt_presets_delete_unprotected(rowid);
  assert_false(dt_presets_exists("renamed", OP, 3));
}

static void _add(const char *name, const int params, const int autoapply)
{
  const int blend = 7;
  dt_presets_write_t p = {
    .name = name, .description = "", .model = "%", .maker = "%", .lens = "%",
    .iso_min = 0.0, .iso_max = FLT_MAX, .exposure_min = 0.0, .exposure_max = 100000000.0,
    .aperture_min = 0.0, .aperture_max = 100000000.0,
    .focal_length_min = 0.0, .focal_length_max = 1000.0,
    .autoapply = autoapply, .format = 0,
    .iop = TRUE, .op_params = &params, .op_params_size = sizeof(params), .enabled = 1,
    .multi_name = "", .multi_name_hand_edited = 0,
    .operation = OP, .op_version = 5,
    .blendop_params = &blend, .blendop_params_size = sizeof(blend), .blendop_version = 2,
  };
  dt_presets_write(-1, &p);
}

static void test_apply_and_step(void **state)
{
  _add("alpha", 1, 0);
  _add("beta", 2, 0);
  _add("gamma", 3, 0);

  dt_presets_params_t p = { 0 };
  assert_true(dt_presets_get_params(OP, 5, "beta", &p));
  assert_int_equal(p.op_params_size, sizeof(int));
  assert_int_equal(*(int *)p.op_params, 2);
  assert_int_equal(p.enabled, 1);
  assert_int_equal(p.blendop_version, 2);
  assert_string_equal(p.multi_name, "");
  dt_presets_params_clear(&p);
  assert_false(dt_presets_get_params(OP, 4, "beta", &p));

  // the menu's next and previous, by name
  gchar *n = dt_presets_adjacent_name(OP, 5, "beta", 1);
  assert_string_equal(n, "gamma");
  g_free(n);
  n = dt_presets_adjacent_name(OP, 5, "beta", -1);
  assert_string_equal(n, "alpha");
  g_free(n);
  n = dt_presets_adjacent_name(OP, 5, "alpha", 2);
  assert_string_equal(n, "gamma");
  g_free(n);
  assert_null(dt_presets_adjacent_name(OP, 5, "gamma", 1));

  // updating the params moves the preset to the given version
  const int params = 9;
  dt_presets_update_params(OP, 6, "beta", &params, sizeof(params), 0, NULL, 0, 3);
  assert_false(dt_presets_get_params(OP, 5, "beta", &p));
  assert_true(dt_presets_get_params(OP, 6, "beta", &p));
  assert_int_equal(*(int *)p.op_params, 9);
  assert_int_equal(p.enabled, 0);
  assert_null(p.blendop_params);
  dt_presets_params_clear(&p);

  // the menus' lists: every version, by name (no built-in preset here)
  GList *names = dt_presets_list_names(OP, TRUE);
  assert_int_equal(g_list_length(names), 3);
  assert_string_equal(((dt_presets_name_t *)names->data)->name, "alpha");
  assert_false(((dt_presets_name_t *)names->data)->writeprotect);
  assert_string_equal(((dt_presets_name_t *)g_list_last(names)->data)->name, "gamma");
  g_list_free_full(names, dt_presets_name_free);
  assert_null(dt_presets_list_names("no_such_op", FALSE));
}

static void test_autoapply(void **state)
{
  dt_image_t image;
  memset(&image, 0, sizeof(image));

  // the workflow preset is picked even without autoapply
  dt_presets_params_t p = { 0 };
  assert_true(dt_presets_find_autoapply(OP, 5, &image, TRUE, "gamma", &p));
  assert_string_equal(p.name, "gamma");
  assert_int_equal(*(int *)p.op_params, 3);
  dt_presets_params_clear(&p);
  assert_false(dt_presets_find_autoapply(OP, 5, &image, TRUE, "\t\n", &p));

  // an auto-applied user preset matching everything comes first
  _add("auto", 4, 1);
  assert_true(dt_presets_find_autoapply(OP, 5, &image, TRUE, "gamma", &p));
  assert_string_equal(p.name, "auto");
  dt_presets_params_clear(&p);
}

static void test_utility_module(void **state)
{
  // _add() writes OP at version 5: "alpha", "beta" moved to 6, "gamma", "auto"
  GList *names = dt_presets_version_names(OP, 5);
  assert_int_equal(g_list_length(names), 3);
  assert_string_equal(names->data, "alpha");
  assert_string_equal(g_list_last(names)->data, "gamma");
  g_list_free_full(names, g_free);

  GList *all = dt_presets_list_params(OP, 5, TRUE);
  assert_int_equal(g_list_length(all), 3);
  for(GList *l = all; l; l = g_list_next(l))
  {
    const dt_presets_params_t *row = l->data;
    assert_non_null(row->name);
    assert_int_equal(row->op_params_size, sizeof(int));
    // copies end with a zero byte for the modules that store strings
    assert_int_equal(((char *)row->op_params)[row->op_params_size], 0);
  }
  g_list_free_full(all, dt_presets_params_free);

  dt_presets_params_t p = { 0 };
  assert_true(dt_presets_get_params(OP, 5, "auto", &p));
  assert_int_equal(p.autoapply, 1);
  dt_presets_params_clear(&p);

  dt_presets_rename(OP, 5, "alpha", "omega");
  assert_false(dt_presets_exists("alpha", OP, 5));
  assert_true(dt_presets_exists("omega", OP, 5));

  // "auto" applies to everything; the descending name order picks it
  dt_image_t image;
  memset(&image, 0, sizeof(image));
  gchar *name = dt_presets_find_lib_autoapply(OP, 5, &image);
  assert_string_equal(name, "auto");
  g_free(name);
  assert_null(dt_presets_find_lib_autoapply(OP, 6, &image));

  dt_presets_delete_version(OP, 5);
  assert_null(dt_presets_version_names(OP, 5));
  assert_true(dt_presets_exists("beta", OP, 6));
}

static void test_legacy_upgrade(void **state)
{
  // after test_utility_module only "beta" is left, at version 6
  _add("delta", 5, 0);
  GList *stored = dt_presets_list_stored(OP);
  assert_int_equal(g_list_length(stored), 2);
  gboolean seen_beta = FALSE;
  for(GList *l = stored; l; l = g_list_next(l))
  {
    const dt_presets_params_t *row = l->data;
    if(!strcmp(row->name, "beta"))
    {
      seen_beta = TRUE;
      assert_int_equal(row->op_version, 6);
      assert_null(row->blendop_params);
    }
    else
    {
      assert_string_equal(row->name, "delta");
      assert_int_equal(row->blendop_version, 2);
      assert_int_equal(row->blendop_params_size, sizeof(int));
    }
  }
  assert_true(seen_beta);
  g_list_free_full(stored, dt_presets_params_free);

  dt_presets_set_op_version(OP, "delta", 7);
  assert_true(dt_presets_exists("delta", OP, 7));
  const int params = 11, blend = 12;
  dt_presets_set_op_params(OP, "delta", 8, &params, sizeof(params));
  dt_presets_set_blendop_params(OP, "delta", 4, &blend, sizeof(blend));
  dt_presets_params_t p = { 0 };
  assert_true(dt_presets_get_params(OP, 8, "delta", &p));
  assert_int_equal(*(int *)p.op_params, 11);
  assert_int_equal(*(int *)p.blendop_params, 12);
  assert_int_equal(p.blendop_version, 4);
  dt_presets_params_clear(&p);
  // an auto-init preset has no params
  dt_presets_set_op_params(OP, "delta", 8, NULL, 0);
  assert_true(dt_presets_get_params(OP, 8, "delta", &p));
  assert_null(p.op_params);
  dt_presets_params_clear(&p);

  GList *names = dt_presets_names_by_rowid(OP);
  assert_int_equal(g_list_length(names), 2);
  assert_string_equal(names->data, "beta");
  g_list_free_full(names, g_free);

  // no built-in preset here: nothing goes
  dt_presets_delete_builtin(OP);
  names = dt_presets_names_by_rowid(OP);
  assert_int_equal(g_list_length(names), 2);
  g_list_free_full(names, g_free);
}

int main(int argc, char *argv[])
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_edit_dialog_round_trip),
    cmocka_unit_test(test_apply_and_step),
    cmocka_unit_test(test_autoapply),
    cmocka_unit_test(test_utility_module),
    cmocka_unit_test(test_legacy_upgrade),
  };
  return cmocka_run_group_tests(tests, dt_test_catalog_setup, dt_test_catalog_teardown);
}
