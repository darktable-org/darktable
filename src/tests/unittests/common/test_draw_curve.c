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
/*
 * cmocka unit tests for the anchor and interpolation-type bounds of the
 * shared curve helpers in gui/draw.h and common/curve_tools.c.
 *
 * Modules fill these curves from stored params, so counts, indices and
 * types can be anything a sidecar supplies.
 */

#include <limits.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>

#include <cmocka.h>

#include "common/curve_tools.h"
#include "common/splines.h"
#include "gui/draw.h"

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

static void test_anchor_writes_stay_in_array(void **state)
{
  dt_draw_curve_t *c = dt_draw_curve_new(0.0f, 1.0f, MONOTONE_HERMITE);
  const uint16_t *const samples = c->csample.m_Samples;

  // CurveSample follows the anchor array in the same allocation
  for(int k = 0; k < MAX_ANCHORS + 5; k++)
    dt_draw_curve_add_point(c, k / 30.0f, k / 30.0f);
  dt_draw_curve_set_point(c, -1, 0.5f, 0.5f);
  dt_draw_curve_set_point(c, MAX_ANCHORS, 0.5f, 0.5f);
  dt_draw_curve_set_point(c, MAX_ANCHORS + 1, 0.5f, 0.5f);

  assert_int_equal(c->c.m_numAnchors, MAX_ANCHORS);
  assert_int_equal(c->csample.m_samplingRes, 0x10000);
  assert_int_equal(c->csample.m_outputRes, 0x10000);
  assert_ptr_equal(c->csample.m_Samples, samples);

  dt_draw_curve_destroy(c);
}

static void test_samplers_refuse_oversized_count(void **state)
{
  dt_draw_curve_t *c = dt_draw_curve_new(0.0f, 1.0f, MONOTONE_HERMITE);
  for(int k = 0; k < MAX_ANCHORS; k++)
    dt_draw_curve_add_point(c, k / 19.0f, k / 19.0f);

  // rgbcurve assigns the stored count to m_numAnchors directly
  c->c.m_numAnchors = MAX_ANCHORS + 1;
  c->csample.m_samplingRes = 256;

  assert_int_equal(CurveDataSample(&c->c, &c->csample), CT_ERROR);
  assert_int_equal(CurveDataSampleV2(&c->c, &c->csample), CT_ERROR);
  assert_int_equal(CurveDataSampleV2Periodic(&c->c, &c->csample), CT_ERROR);
  const float val = dt_draw_curve_calc_value(c, 0.5f);
  assert_true(val >= c->c.m_min_y && val <= c->c.m_max_y);

  dt_draw_curve_destroy(c);
}

static void test_interpolation_type_refused(void **state)
{
  float x[] = { 0.0f, 0.5f, 1.0f };
  float y[] = { 0.0f, 0.5f, 1.0f };

  // the type indexes the function-pointer tables
  assert_null(interpolate_set(3, x, y, MONOTONE_HERMITE + 1));
  assert_null(interpolate_set(3, x, y, UINT_MAX));
  assert_true(interpolate_val(3, x, 0.5f, y, y, MONOTONE_HERMITE + 1) == 0.0f);

  float *const ypp = interpolate_set(3, x, y, MONOTONE_HERMITE);
  assert_non_null(ypp);
  free(ypp);
}

int main(int argc, char *argv[])
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_anchor_writes_stay_in_array),
    cmocka_unit_test(test_samplers_refuse_oversized_count),
    cmocka_unit_test(test_interpolation_type_refused),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
