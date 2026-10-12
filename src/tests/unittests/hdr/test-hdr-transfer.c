/* Copyright (C) 2026 darktable developers.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <math.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <cmocka.h>
#include "common/hdr-transfer.h"

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
      float rgb[3] = { signals[i], signals[i], signals[i] };
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
    float encoded[3] = { linear[0], linear[1], linear[2] };
    dt_hdr_encode(encoded, DT_HDR_HLG_REC2020 + i);
    float decoded[3] = { codes[i][0], codes[i][1], codes[i][2] };
    dt_hdr_decode(decoded, DT_HDR_HLG_REC2020 + i);
    for(int c = 0; c < 3; c++)
    {
      assert_float_equal(encoded[c], codes[i][c], 0.0001f);
      assert_float_equal(decoded[c], linear[c], 0.0005f);
    }
  }
}

static void test_hdr_bounds_and_identity(void **state)
{
  (void)state;
  assert_float_equal(dt_hdr_pq_decode(-1.0f), 0.0f, 0.0f);
  assert_float_equal(dt_hdr_pq_encode(100.0f), 1.0f, 0.00001f);
  for(int mode = DT_HDR_HLG_REC2020; mode <= DT_HDR_HLG_P3; mode++)
  {
    float black[3] = { 0.0f, 0.0f, 0.0f };
    dt_hdr_encode(black, mode);
    for(int c = 0; c < 3; c++) assert_float_equal(black[c], 0.0f, 0.0f);
  }
  float rgb[3] = { -0.5f, 1.0f, 8.0f };
  dt_hdr_decode(rgb, DT_HDR_NONE);
  dt_hdr_encode(rgb, DT_HDR_NONE);
  assert_float_equal(rgb[0], -0.5f, 0.0f);
  assert_float_equal(rgb[1], 1.0f, 0.0f);
  assert_float_equal(rgb[2], 8.0f, 0.0f);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_pq_luminance),
    cmocka_unit_test(test_hlg_luminance),
    cmocka_unit_test(test_hlg_colored_pixels),
    cmocka_unit_test(test_hdr_bounds_and_identity)
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
