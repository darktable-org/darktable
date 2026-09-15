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

#include "iop/basecurve.c"

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

static void test_basecurve_node_count_bounds(void **state)
{
  const int max_nodes = sizeof(((dt_iop_basecurve_params_t *)0)->basecurve[0])
                        / sizeof(dt_iop_basecurve_node_t);
  const int counts[] = { -1, 0, 1, max_nodes, max_nodes + 1 };
  const int expected[] = { 0, 0, 1, max_nodes, max_nodes };

  for(size_t i = 0; i < sizeof(counts) / sizeof(counts[0]); i++)
  {
    dt_iop_basecurve_params_t params = { 0 };
    dt_iop_basecurve_data_t data = { 0 };
    dt_dev_pixelpipe_t pipe = { 0 };
    dt_dev_pixelpipe_iop_t piece = { 0 };
    params.basecurve_nodes[0] = counts[i];
    params.basecurve_type[0] = MONOTONE_HERMITE;
    for(int k = 0; k < max_nodes; k++)
    {
      params.basecurve[0][k].x = (float)k / (max_nodes - 1);
      params.basecurve[0][k].y = params.basecurve[0][k].x;
    }
    piece.data = &data;

    commit_params(NULL, (dt_iop_params_t *)&params, &pipe, &piece);
    assert_int_equal(data.basecurve_nodes, expected[i]);
    assert_int_equal(data.curve->c.m_numAnchors, expected[i]);
    dt_draw_curve_destroy(data.curve);
  }
}

static void test_basecurve_helper_bounds(void **state)
{
  const int counts[] = { INT_MIN, -1, 0, 1, 10, MAXNODES, MAXNODES + 1, INT_MAX };
  const int expected[] = { 0, 0, 0, 1, 10, MAXNODES, MAXNODES, MAXNODES };

  for(size_t i = 0; i < sizeof(counts) / sizeof(counts[0]); i++)
    assert_int_equal(_basecurve_nodes(counts[i]), expected[i]);
}

int main(int argc, char *argv[])
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_basecurve_node_count_bounds),
    cmocka_unit_test(test_basecurve_helper_bounds)
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
