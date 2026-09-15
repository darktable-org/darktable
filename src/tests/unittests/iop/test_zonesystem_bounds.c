/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable. If not, see <http://www.gnu.org/licenses/>.
*/

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <limits.h>

#include <cmocka.h>

#include "iop/zonesystem.c"

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

static void test_zonesystem_size_bounds(void **state)
{
  const int sizes[] = { INT_MIN, -1, 0, 4, 12, MAX_ZONE_SYSTEM_SIZE,
                        MAX_ZONE_SYSTEM_SIZE + 1, INT_MAX };
  const int expected[] = { 4, 4, 4, 4, 12, MAX_ZONE_SYSTEM_SIZE,
                           MAX_ZONE_SYSTEM_SIZE, MAX_ZONE_SYSTEM_SIZE };

  for(size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
  {
    assert_int_equal(_zonesystem_size(sizes[i]), expected[i]);

    dt_iop_zonesystem_params_t params = { 0 };
    dt_iop_zonesystem_data_t data = { 0 };
    dt_dev_pixelpipe_iop_t piece = { 0 };
    params.size = sizes[i];
    piece.data = &data;

    commit_params(NULL, (dt_iop_params_t *)&params, NULL, &piece);
    assert_int_equal(data.params.size, expected[i]);
  }
}

static void test_zonesystem_zonemap_bounds(void **state)
{
  const int sizes[] = { INT_MIN, INT_MAX };

  for(size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
  {
    dt_iop_zonesystem_params_t params = { 0 };
    float zonemap_storage[MAX_ZONE_SYSTEM_SIZE + 2];
    dt_iop_zonesystem_params_t bounded = params;

    params.size = sizes[i];
    bounded.size = _zonesystem_size(params.size);
    for(size_t k = 0; k < sizeof(zonemap_storage) / sizeof(zonemap_storage[0]); k++)
      zonemap_storage[k] = 1234.5f;

    _iop_zonesystem_calculate_zonemap(&bounded, zonemap_storage + 1);

    assert_int_equal(params.size, sizes[i]);
    assert_float_equal(zonemap_storage[0], 1234.5f, 0.0f);
    assert_float_equal(zonemap_storage[MAX_ZONE_SYSTEM_SIZE + 1], 1234.5f, 0.0f);
  }
}

int main(int argc, char *argv[])
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_zonesystem_size_bounds),
    cmocka_unit_test(test_zonesystem_zonemap_bounds)
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
