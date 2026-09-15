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
#include <string.h>

#include <cmocka.h>

#define dt_colorspaces_get_profile test_colorin_get_profile
#include "iop/colorin.c"
#undef dt_colorspaces_get_profile

const dt_colorspaces_color_profile_t *test_colorin_get_profile(
  dt_colorspaces_color_profile_type_t type,
  const char *filename,
  const dt_colorspaces_profile_direction_t direction)
{
  static const dt_colorspaces_color_profile_t profile = { 0 };
  return &profile;
}

static void test_colorin_unterminated_work_profile(void **state)
{
  dt_iop_colorin_params_t params = { 0 };
  dt_iop_colorin_data_t data = { 0 };
  dt_dev_pixelpipe_iop_t piece = { 0 };
  params.type = DT_COLORSPACE_LAB;
  memset(params.filename_work, 'x', sizeof(params.filename_work));
  piece.data = &data;

  commit_params(NULL, (dt_iop_params_t *)&params, NULL, &piece);

  assert_int_equal(data.filename_work[0], '\0');
}

int main(int argc, char *argv[])
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_colorin_unterminated_work_profile)
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
