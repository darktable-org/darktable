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

#include "iop/rgbcurve.c"

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

static void test_rgbcurve_node_count_bounds(void **state)
{
  const int counts[] = { -1, 0, 1, MAX_ANCHORS, MAX_ANCHORS + 1 };
  const int expected[] = { 0, 0, 1, MAX_ANCHORS, MAX_ANCHORS };

  for(size_t i = 0; i < sizeof(counts) / sizeof(counts[0]); i++)
  {
    dt_iop_rgbcurve_params_t params = { 0 };
    dt_iop_rgbcurve_data_t *data = calloc(1, sizeof(*data));
    dt_dev_pixelpipe_t pipe = { 0 };
    dt_dev_pixelpipe_iop_t piece = { 0 };
    assert_non_null(data);

    params.curve_autoscale = DT_S_SCALE_MANUAL_RGB;
    piece.data = data;
    for(int ch = 0; ch < DT_IOP_RGBCURVE_MAX_CHANNELS; ch++)
    {
      params.curve_num_nodes[ch] = counts[i];
      params.curve_type[ch] = MONOTONE_HERMITE;
      data->curve[ch] = dt_draw_curve_new(0.0f, 1.0f, MONOTONE_HERMITE);
      for(int k = 0; k < MAX_ANCHORS; k++)
      {
        params.curve_nodes[ch][k].x = (float)k / (MAX_ANCHORS - 1);
        params.curve_nodes[ch][k].y = params.curve_nodes[ch][k].x;
      }
    }

    commit_params(NULL, (dt_iop_params_t *)&params, &pipe, &piece);
    _generate_curve_lut(&pipe, data);
    for(int ch = 0; ch < DT_IOP_RGBCURVE_MAX_CHANNELS; ch++)
    {
      assert_int_equal(data->curve[ch]->c.m_numAnchors, expected[i]);
      dt_draw_curve_destroy(data->curve[ch]);
    }
    free(data);
  }
}

static void test_rgbcurve_helper_bounds(void **state)
{
  const int counts[] = { INT_MIN, -1, 0, 1, 10,
                         DT_IOP_RGBCURVE_MAXNODES,
                         DT_IOP_RGBCURVE_MAXNODES + 1, INT_MAX };
  const int expected[] = { 0, 0, 0, 1, 10,
                           DT_IOP_RGBCURVE_MAXNODES,
                           DT_IOP_RGBCURVE_MAXNODES,
                           DT_IOP_RGBCURVE_MAXNODES };

  for(size_t i = 0; i < sizeof(counts) / sizeof(counts[0]); i++)
    assert_int_equal(_rgbcurve_nodes(counts[i]), expected[i]);
}

int main(int argc, char *argv[])
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_rgbcurve_node_count_bounds),
    cmocka_unit_test(test_rgbcurve_helper_bounds)
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
