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
#include <string.h>

#include <cmocka.h>

#include "iop/colorzones.c"

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

static void _run_colorzones_count_case(const int version,
                                       const int count,
                                       const int expected,
                                       const int expected_anchors)
{
  dt_iop_colorzones_params_t params = { 0 };
  dt_iop_colorzones_data_t *data = calloc(1, sizeof(*data));
  dt_iop_module_t self = { 0 };
  dt_dev_pixelpipe_t pipe = { 0 };
  dt_dev_pixelpipe_iop_t piece = { 0 };
  assert_non_null(data);

  params.channel = DT_IOP_COLORZONES_h;
  params.splines_version = version;
  piece.data = data;
  for(int ch = 0; ch < DT_IOP_COLORZONES_MAX_CHANNELS; ch++)
  {
    params.curve_num_nodes[ch] = count;
    params.curve_type[ch] = MONOTONE_HERMITE;
    data->curve[ch] = dt_draw_curve_new(0.0f, 1.0f, MONOTONE_HERMITE);
    data->curve_nodes[ch] = -1;
    data->curve_type[ch] = -1;
    for(int k = 0; k < MAX_ANCHORS; k++)
    {
      params.curve[ch][k].x = (float)k / (MAX_ANCHORS - 1);
      params.curve[ch][k].y = params.curve[ch][k].x;
    }
  }

  commit_params(&self, (dt_iop_params_t *)&params, &pipe, &piece);
  for(int ch = 0; ch < DT_IOP_COLORZONES_MAX_CHANNELS; ch++)
  {
    assert_int_equal(data->curve_nodes[ch], expected);
    assert_int_equal(data->curve[ch]->c.m_numAnchors, expected_anchors);
    dt_draw_curve_destroy(data->curve[ch]);
  }
  free(data);
}

static void test_colorzones_v2_count_bounds(void **state)
{
  const int counts[] = { -1, 0, 1, MAX_ANCHORS, MAX_ANCHORS + 1 };
  const int expected[] = { 0, 0, 1, MAX_ANCHORS, MAX_ANCHORS };

  for(size_t i = 0; i < sizeof(counts) / sizeof(counts[0]); i++)
    _run_colorzones_count_case(DT_IOP_COLORZONES_SPLINES_V2,
                               counts[i], expected[i], expected[i]);
}

static void test_colorzones_v1_count_bounds(void **state)
{
  const int counts[] = { -1, 0, 1, 2, MAX_ANCHORS - 2, MAX_ANCHORS - 1,
                         MAX_ANCHORS };
  const int expected[] = { 2, 2, 2, 2, MAX_ANCHORS - 2, MAX_ANCHORS - 2,
                           MAX_ANCHORS - 2 };

  for(size_t i = 0; i < sizeof(counts) / sizeof(counts[0]); i++)
    _run_colorzones_count_case(DT_IOP_COLORZONES_SPLINES_V1,
                               counts[i], expected[i], expected[i] + 2);
}

static void test_colorzones_helper_bounds(void **state)
{
  const int counts[] = { INT_MIN, -1, 0, 1, 2, 10,
                         DT_IOP_COLORZONES_MAXNODES,
                         DT_IOP_COLORZONES_MAXNODES + 1, INT_MAX };
  const int expected_v1[] = { 2, 2, 2, 2, 2, 10,
                              DT_IOP_COLORZONES_MAXNODES - 2,
                              DT_IOP_COLORZONES_MAXNODES - 2,
                              DT_IOP_COLORZONES_MAXNODES - 2 };
  const int expected_v2[] = { 0, 0, 0, 1, 2, 10,
                              DT_IOP_COLORZONES_MAXNODES,
                              DT_IOP_COLORZONES_MAXNODES,
                              DT_IOP_COLORZONES_MAXNODES };
  dt_iop_colorzones_params_t params = { 0 };

  for(size_t i = 0; i < sizeof(counts) / sizeof(counts[0]); i++)
  {
    params.splines_version = DT_IOP_COLORZONES_SPLINES_V1;
    params.curve_num_nodes[0] = counts[i];
    assert_int_equal(_colorzones_nodes(&params, 0), expected_v1[i]);

    params.splines_version = DT_IOP_COLORZONES_SPLINES_V2;
    assert_int_equal(_colorzones_nodes(&params, 0), expected_v2[i]);
  }
}

static void _run_colorzones_edit_case(const int version,
                                      const int count,
                                      const int expected)
{
  struct
  {
    dt_iop_colorzones_params_t params;
    unsigned int sentinel[4];
  } guarded = { 0 };
  dt_iop_colorzones_gui_data_t gui = { 0 };
  dt_iop_colorzones_node_t before[DT_IOP_COLORZONES_MAXNODES];

  guarded.params.channel = DT_IOP_COLORZONES_L;
  guarded.params.splines_version = version;
  guarded.params.curve_num_nodes[DT_IOP_COLORZONES_L] = count;
  guarded.sentinel[0] = 0x13579bdfu;
  guarded.sentinel[1] = 0x2468ace0u;
  guarded.sentinel[2] = 0xdeadbeefu;
  guarded.sentinel[3] = 0x10203040u;
  for(int k = 0; k < DT_IOP_COLORZONES_MAXNODES; k++)
  {
    guarded.params.curve[DT_IOP_COLORZONES_L][k].x = (float)k / DT_IOP_COLORZONES_MAXNODES;
    guarded.params.curve[DT_IOP_COLORZONES_L][k].y = 0.25f + 0.01f * k;
  }
  memcpy(before, guarded.params.curve[DT_IOP_COLORZONES_L], sizeof(before));

  gui.zoom_factor = 1.f;
  gui.offset_x = gui.offset_y = 0.f;
  dt_iop_colorzones_get_params(&guarded.params, &gui, DT_IOP_COLORZONES_L,
                               0.5, 0.5, 0.5f);

  assert_int_equal(guarded.params.curve_num_nodes[DT_IOP_COLORZONES_L], expected);
  for(int k = expected; k < DT_IOP_COLORZONES_MAXNODES; k++)
    assert_memory_equal(&guarded.params.curve[DT_IOP_COLORZONES_L][k],
                        &before[k], sizeof(before[k]));
  assert_int_equal(guarded.sentinel[0], 0x13579bdfu);
  assert_int_equal(guarded.sentinel[1], 0x2468ace0u);
  assert_int_equal(guarded.sentinel[2], 0xdeadbeefu);
  assert_int_equal(guarded.sentinel[3], 0x10203040u);
}

static void test_colorzones_edit_bounds(void **state)
{
  _run_colorzones_edit_case(DT_IOP_COLORZONES_SPLINES_V1,
                            DT_IOP_COLORZONES_MAXNODES + 1,
                            DT_IOP_COLORZONES_MAXNODES - 2);
  _run_colorzones_edit_case(DT_IOP_COLORZONES_SPLINES_V2,
                            DT_IOP_COLORZONES_MAXNODES + 1,
                            DT_IOP_COLORZONES_MAXNODES);
  _run_colorzones_edit_case(DT_IOP_COLORZONES_SPLINES_V1, INT_MIN, 2);
}

int main(int argc, char *argv[])
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_colorzones_v2_count_bounds),
    cmocka_unit_test(test_colorzones_v1_count_bounds),
    cmocka_unit_test(test_colorzones_helper_bounds),
    cmocka_unit_test(test_colorzones_edit_bounds)
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
