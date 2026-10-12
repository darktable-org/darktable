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

int main(int argc, char *argv[])
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_edit_dialog_round_trip),
  };
  return cmocka_run_group_tests(tests, dt_test_catalog_setup, dt_test_catalog_teardown);
}
