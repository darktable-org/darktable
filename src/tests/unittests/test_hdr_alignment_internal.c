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

/* White-box unit tests for the internal (static) helpers of the HDR alignment
 * code.  Following the test_filmicrgb.c pattern, the translation unit is
 * #included directly so the static helpers are reachable; lib_darktable supplies
 * everything hdr_alignment.c links against (the OpenCV seam, glib, ...).
 *
 * Complements test_hdr_alignment.c, which exercises the public API end-to-end. */

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <cmocka.h>

#include "common/hdr_alignment.c"

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

static int _cmp_f(const void *a, const void *b)
{
  const float x = *(const float *)a, y = *(const float *)b;
  return (x > y) - (x < y);
}

// The histogram-based percentile must track the exact (sorted) order statistics
// to well under one part in a hundred of the data span.
static void test_percentile_bounds_accuracy(void **state)
{
  (void)state;
  const size_t n = 300000;
  float *v = malloc(n * sizeof(float));
  float *c = malloc(n * sizeof(float));
  assert_non_null(v);
  assert_non_null(c);
  unsigned s = 4242u;
  for(size_t i = 0; i < n; i++)
  {
    s = s * 1103515245u + 12345u;
    v[i] = ((s >> 9) & 0x7fff) / 32767.0f * 8.0f - 1.0f;  // ~[-1, 7]
  }
  memcpy(c, v, n * sizeof(float));
  qsort(c, n, sizeof(float), _cmp_f);
  const float ex_lo = c[(size_t)(0.01 * (n - 1) + 0.5)];
  const float ex_hi = c[(size_t)(0.99 * (n - 1) + 0.5)];

  float p_lo, p_hi;
  _percentile_bounds(v, n, DT_HDR_PERCENTILE_LOW, DT_HDR_PERCENTILE_HIGH, &p_lo, &p_hi);

  const float span = ex_hi - ex_lo;
  assert_true(fabsf(p_lo - ex_lo) < 0.01f * span);
  assert_true(fabsf(p_hi - ex_hi) < 0.01f * span);
  free(v);
  free(c);
}

// A flat image has no spread: the bounds must collapse to the constant value so
// the downstream stretch is a well-defined no-op (no division blow-up).
static void test_percentile_bounds_flat(void **state)
{
  (void)state;
  float v[1024];
  for(int i = 0; i < 1024; i++) v[i] = 0.5f;
  float p_lo = -1.0f, p_hi = -1.0f;
  _percentile_bounds(v, 1024, DT_HDR_PERCENTILE_LOW, DT_HDR_PERCENTILE_HIGH, &p_lo, &p_hi);
  assert_true(p_lo == 0.5f);
  assert_true(p_hi == 0.5f);
}

// _proxy_to_u8 must be monotone (preserves ordering) and use the full code range
// on a ramp, so SIFT sees the intended contrast.
static void test_proxy_to_u8_monotone(void **state)
{
  (void)state;
  const int pw = 256, ph = 1;
  float ramp[256];
  for(int i = 0; i < pw; i++) ramp[i] = (float)i;
  uint8_t *u8 = _proxy_to_u8(ramp, pw, ph, 2.2);
  assert_non_null(u8);
  for(int i = 1; i < pw; i++) assert_true(u8[i] >= u8[i - 1]);
  assert_int_equal(u8[0], 0);
  assert_int_equal(u8[pw - 1], 255);
  dt_free_align(u8);
}

#ifdef HAVE_OPENCV
// The Bayer same-color fast path must be bit-for-bit identical to the general
// X-Trans-capable sampler it replaces, across the whole sampling domain
// (including out-of-bounds edges) and every CFA colour.  A divergence here would
// silently corrupt every aligned Bayer frame.
static void test_bayer_sampler_bit_exact(void **state)
{
  (void)state;
  const int W = 71, H = 53;                 // odd dims exercise the edges
  const uint32_t filters = 0x94949494u;     // RGGB
  float *m = malloc(sizeof(float) * W * H);
  assert_non_null(m);
  for(int y = 0; y < H; y++)
    for(int x = 0; x < W; x++)
      m[y * W + x] = (float)((x * 7 + y * 13) % 101) / 101.0f
                   + 0.013f * (float)((x * x + 3 * y) % 17);

  for(double sy = -2.0; sy <= H + 2.0; sy += 0.31)
    for(double sx = -2.0; sx <= W + 2.0; sx += 0.27)
      for(int c = 0; c < 3; c++)            // R=0, G=1, B=2
      {
        const float fast = _sample_bayer_same_color(m, W, H, filters, c, sx, sy);
        const float general = _sample_cfa_same_color(m, W, H, filters, NULL, c, sx, sy);
        assert_true(fast == general);
      }
  free(m);
}
#endif // HAVE_OPENCV

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_percentile_bounds_accuracy),
    cmocka_unit_test(test_percentile_bounds_flat),
    cmocka_unit_test(test_proxy_to_u8_monotone),
#ifdef HAVE_OPENCV
    cmocka_unit_test(test_bayer_sampler_bit_exact),
#endif
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
