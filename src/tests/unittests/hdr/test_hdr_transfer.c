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

#include <float.h>
#include <math.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <cmocka.h>
#include "common/hdr-transfer.h"
#include "imageio/imageio_common.h"

static void test_pq_luminance(void **state)
{
  (void)state;
  /* Independent ST.2084 reference codes, not a round trip. */
  const float signals[] = { 0.0f, 0.50807842f, 0.58068888f, 0.75182710f, 1.0f };
  const float nits[] = { 0.0f, 100.0f, 203.0f, 1000.0f, 10000.0f };
  for(size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); i++)
  {
    assert_float_equal(dt_hdr_pq_decode(signals[i]), nits[i] / 203.0f, 0.003f);
    assert_float_equal(dt_hdr_pq_encode(nits[i] / 203.0f), signals[i], 0.00002f);
  }
}

/* ST.2084 EOTF in double, relative to reference white; its pole is at 1.992. */
static double pq_decode_reference(const double e)
{
  const double m1 = 2610.0 / 16384.0, m2 = 2523.0 / 32.0;
  const double c1 = 3424.0 / 4096.0, c2 = 2413.0 / 128.0, c3 = 2392.0 / 128.0;
  const double p = pow(e, 1.0 / m2);
  return 10000.0 / 203.0 * pow(fmax(p - c1, 0.0) / (c2 - c3 * p), 1.0 / m1);
}

static void test_pq_decode_precision(void **state)
{
  (void)state;
  /* p - C1 cancels near black and C2 - C3 p near peak. Over every float
   * signal in [0, 1] the decode stays within 3.4e-6 of the double EOTF.
   * Black is signal 7.31e-7; the dark branch ends at 1e-4. */
  const float signals[] = { 7.4e-7f, 1e-6f, 3e-6f, 9.99e-5f, 1e-4f,
                            0.58068888f, 0.99f, 0.998768f, 0.9999f, 1.0f,
                            1.2f, 1.5f };
  for(size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); i++)
  {
    const double ref = pq_decode_reference(signals[i]);
    assert_float_equal(dt_hdr_pq_decode(signals[i]), ref, 4e-6 * ref);
  }
}

static void test_hlg_luminance(void **state)
{
  (void)state;
  /* BT.2100 EOTF at nominal 1000-nit display with zero black. */
  const float signals[] = { 0.0f, 0.5f, 0.75f, 1.0f };
  const float nits[] = { 0.0f, 50.69702849f, 203.15214535f, 1000.0f };
  for(size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); i++)
  {
    for(int mode = DT_HDR_HLG_REC2020; mode <= DT_HDR_HLG_P3; mode++)
    {
      dt_aligned_pixel_t rgb = { signals[i], signals[i], signals[i] };
      dt_hdr_decode(rgb, mode);
      for(int c = 0; c < 3; c++)
        assert_float_equal(rgb[c], nits[i] / 203.0f, 0.0001f);
    }
  }
}

static void test_hlg_colored_pixels(void **state)
{
  (void)state;
  /* Display-linear RGB and independently calculated inverse-OOTF codes. */
  const float linear[3] = { 0.5f, 0.2f, 2.0f };
  const float codes[2][3] = {
    { 0.64144569f, 0.43154450f, 0.91205861f },
    { 0.63915679f, 0.42921356f, 0.91004408f }
  };
  for(int i = 0; i < 2; i++)
  {
    dt_aligned_pixel_t encoded = { linear[0], linear[1], linear[2] };
    dt_hdr_encode(encoded, DT_HDR_HLG_REC2020 + i);
    dt_aligned_pixel_t decoded = { codes[i][0], codes[i][1], codes[i][2] };
    dt_hdr_decode(decoded, DT_HDR_HLG_REC2020 + i);
    for(int c = 0; c < 3; c++)
    {
      assert_float_equal(encoded[c], codes[i][c], 0.0001f);
      assert_float_equal(decoded[c], linear[c], 0.0005f);
    }
  }
}

static void test_hdr_black_and_identity(void **state)
{
  (void)state;
  for(int mode = DT_HDR_HLG_REC2020; mode <= DT_HDR_HLG_P3; mode++)
  {
    dt_aligned_pixel_t black = { 0.0f, 0.0f, 0.0f };
    dt_hdr_encode(black, mode);
    for(int c = 0; c < 3; c++) assert_float_equal(black[c], 0.0f, 0.0f);
  }
  dt_aligned_pixel_t rgb = { -0.5f, 1.0f, 8.0f };
  dt_hdr_decode(rgb, DT_HDR_NONE);
  dt_hdr_encode(rgb, DT_HDR_NONE);
  assert_float_equal(rgb[0], -0.5f, 0.0f);
  assert_float_equal(rgb[1], 1.0f, 0.0f);
  assert_float_equal(rgb[2], 8.0f, 0.0f);
}

#define SWEEP_STEPS 65536
/* linear range in reference white units for the extended round trip */
#define LINEAR_MIN -8.0f
#define LINEAR_MAX 64.0f
/* Measured worst cases: signal round trip PQ 7.3e-7 (the PQ code of zero
   light), HLG 6e-8; linear round trip relative to max(|l|, 1) PQ 1.9e-6,
   HLG 7.9e-7, HLG pixels with the OOTF 1.1e-6. */
#define PQ_ROUND_TRIP_TOLERANCE 1e-6f
#define HLG_ROUND_TRIP_TOLERANCE 2e-7f
#define LINEAR_ROUND_TRIP_TOLERANCE 4e-6f

static float _sweep(const int i, const float lo, const float hi)
{
  return lo + (hi - lo) * (float)i / SWEEP_STEPS;
}

static void test_hdr_odd_symmetry(void **state)
{
  (void)state;
  for(int i = 1; i <= SWEEP_STEPS; i++)
  {
    const float s = _sweep(i, 0.0f, 2.5f);
    const float l = _sweep(i, 0.0f, LINEAR_MAX);
    assert_float_equal(dt_hdr_pq_decode(-s), -dt_hdr_pq_decode(s), 0.0f);
    assert_float_equal(dt_hdr_hlg_decode(-s), -dt_hdr_hlg_decode(s), 0.0f);
    assert_float_equal(dt_hdr_pq_encode(-l), -dt_hdr_pq_encode(l), 0.0f);
    assert_float_equal(dt_hdr_hlg_encode(-l), -dt_hdr_hlg_encode(l), 0.0f);
  }
  /* the HLG OOTF gain uses |Y|, so whole pixels are odd too */
  for(int mode = DT_HDR_HLG_REC2020; mode <= DT_HDR_HLG_P3; mode++)
  {
    dt_aligned_pixel_t pos = { 0.9f, -0.1f, 0.3f };
    dt_aligned_pixel_t neg = { -0.9f, 0.1f, -0.3f };
    dt_hdr_decode(pos, mode);
    dt_hdr_decode(neg, mode);
    for(int c = 0; c < 3; c++) assert_float_equal(neg[c], -pos[c], 0.0f);
    dt_hdr_encode(pos, mode);
    dt_hdr_encode(neg, mode);
    for(int c = 0; c < 3; c++) assert_float_equal(neg[c], -pos[c], 0.0f);
  }
}

static void test_hdr_monotonic(void **state)
{
  (void)state;
  /* encode outputs are float codes that repeat between nearby inputs, so
     non-decreasing is the property, not strictly increasing. Signals run
     past the PQ pole at 1.992 to check the clamp keeps the order. */
  for(int i = 1; i <= SWEEP_STEPS; i++)
  {
    const float s0 = _sweep(i - 1, 0.0f, 2.5f);
    const float s1 = _sweep(i, 0.0f, 2.5f);
    assert_true(dt_hdr_pq_decode(s1) >= dt_hdr_pq_decode(s0));
    assert_true(dt_hdr_hlg_decode(s1) >= dt_hdr_hlg_decode(s0));
    const float l0 = _sweep(i - 1, 0.0f, LINEAR_MAX);
    const float l1 = _sweep(i, 0.0f, LINEAR_MAX);
    assert_true(dt_hdr_pq_encode(l1) >= dt_hdr_pq_encode(l0));
    assert_true(dt_hdr_hlg_encode(l1) >= dt_hdr_hlg_encode(l0));
  }
}

static void test_hdr_pq_pole(void **state)
{
  (void)state;
  /* the encode approaches the pole as linear grows */
  const float pole = powf(DT_HDR_PQ_C2 / DT_HDR_PQ_C3, DT_HDR_PQ_M2);
  assert_true(dt_hdr_pq_encode(FLT_MAX) <= pole);
  assert_true(dt_hdr_pq_encode(1e30f) > DT_HDR_PQ_MAX_SIGNAL);
  /* the decode is finite up to and past the pole and flat beyond the
     clamp. the inputs are volatile so that every decode runs at runtime:
     gcc folds the decode of a constant without the fma it contracts
     (1 - C1) - C3 x into at runtime, and next to the pole that one
     rounding moves the result by 3e-5 */
  volatile const float max_signal = DT_HDR_PQ_MAX_SIGNAL;
  const float top = dt_hdr_pq_decode(max_signal);
  assert_true(isfinite(top));
  assert_true(top > dt_hdr_pq_decode(1.9f));
  volatile const float beyond[] = { 1.99f, pole, 2.0f, 4.0f, FLT_MAX };
  for(size_t i = 0; i < sizeof(beyond) / sizeof(beyond[0]); i++)
  {
    assert_float_equal(dt_hdr_pq_decode(beyond[i]), top, 0.0f);
    assert_float_equal(dt_hdr_pq_decode(-beyond[i]), -top, 0.0f);
  }
}

static void test_hdr_round_trip(void **state)
{
  (void)state;
  for(int i = 0; i <= SWEEP_STEPS; i++)
  {
    const float s = _sweep(i, 0.0f, 1.0f);
    assert_float_equal(dt_hdr_pq_encode(dt_hdr_pq_decode(s)), s, PQ_ROUND_TRIP_TOLERANCE);
    assert_float_equal(dt_hdr_hlg_encode(dt_hdr_hlg_decode(s)), s, HLG_ROUND_TRIP_TOLERANCE);
  }
}

static void test_hdr_linear_round_trip(void **state)
{
  (void)state;
  /* negative and above-white linear values survive encode then decode */
  for(int i = 0; i <= SWEEP_STEPS; i++)
  {
    const float l = _sweep(i, LINEAR_MIN, LINEAR_MAX);
    const float tol = LINEAR_ROUND_TRIP_TOLERANCE * fmaxf(fabsf(l), 1.0f);
    assert_float_equal(dt_hdr_pq_decode(dt_hdr_pq_encode(l)), l, tol);
    assert_float_equal(dt_hdr_hlg_decode(dt_hdr_hlg_encode(l)), l, tol);
    for(int mode = DT_HDR_PQ; mode <= DT_HDR_HLG_P3; mode++)
    {
      /* a coloured pixel with one negative channel and Y > 0 */
      const float pixel[3] = { l, 0.5f * fabsf(l) + 0.1f, -0.05f * l };
      dt_aligned_pixel_t rgb = { pixel[0], pixel[1], pixel[2] };
      dt_hdr_encode(rgb, mode);
      dt_hdr_decode(rgb, mode);
      const float ptol = LINEAR_ROUND_TRIP_TOLERANCE * fmaxf(fabsf(l), 1.0f);
      for(int c = 0; c < 3; c++) assert_float_equal(rgb[c], pixel[c], ptol);
    }
  }
}

static void test_pq_light_level_clips(void **state)
{
  (void)state;
  /* codes past 1.0, Inf, NaN and negatives, as an unclipped PQ export
     holds them: the light levels stay within what PQ stores */
  const float rgba[] = {
    2.0f, 0.5f, 0.5f, 1.0f,
    INFINITY, 0.0f, 0.0f, 1.0f,
    NAN, NAN, NAN, 1.0f,
    -1.0f, -1.0f, -1.0f, 1.0f
  };
  float max_cll = 0.0f, max_fall = 0.0f;
  dt_imageio_pq_light_level(rgba, 4, &max_cll, &max_fall);
  assert_float_equal(max_cll, DT_HDR_PQ_PEAK_NITS, 0.5f);
  assert_float_equal(max_fall, DT_HDR_PQ_PEAK_NITS / 2.0f, 0.5f);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_pq_luminance),
    cmocka_unit_test(test_pq_decode_precision),
    cmocka_unit_test(test_hlg_luminance),
    cmocka_unit_test(test_hlg_colored_pixels),
    cmocka_unit_test(test_hdr_black_and_identity),
    cmocka_unit_test(test_hdr_odd_symmetry),
    cmocka_unit_test(test_hdr_monotonic),
    cmocka_unit_test(test_hdr_pq_pole),
    cmocka_unit_test(test_hdr_round_trip),
    cmocka_unit_test(test_hdr_linear_round_trip),
    cmocka_unit_test(test_pq_light_level_clips)
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
