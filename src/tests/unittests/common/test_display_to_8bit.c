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
 * cmocka unit tests for dt_display_to_8bit(), which avoids libm so that
 * loops over it vectorize, and dt_display_to_8bit_pixels(), the loop. both
 * must give what clamping to [0, 1] and roundf() give, ties included.
 */

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>

#include <cmocka.h>

#include "common/colorspaces.h"

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

static float _from_bits(const uint32_t bits)
{
  float v;
  memcpy(&v, &bits, sizeof(v));
  return v;
}

static uint32_t _to_bits(const float v)
{
  uint32_t bits;
  memcpy(&bits, &v, sizeof(bits));
  return bits;
}

static uint8_t _reference(const float v)
{
  // glibc's fmaxf() passes a signaling NaN on, which the clamp then sends to
  // 255. dt_display_to_8bit() shows every NaN as 0
  if(isnan(v)) return 0;
  return (uint8_t)fminf(roundf(255.0f * fmaxf(v, 0.0f)), 255.0f);
}

// the floats around each rounding threshold and each clamp, a sweep over
// every bit pattern, and the special values
static float *_inputs(size_t *n)
{
  const int around = 4096;
  const uint32_t sweep = 4099;
  const float special[] = { 0.0f, -0.0f, INFINITY, -INFINITY, NAN, -NAN,
                            _from_bits(0x7f800001u), _from_bits(0xffbfffffu),
                            FLT_MIN, -FLT_MIN, _from_bits(0x00000001u), FLT_MAX, -FLT_MAX,
                            1.0f, nextafterf(1.0f, 2.0f), 2.0f, -1.0f,
                            0.5f / 255.0f, nextafterf(0.5f / 255.0f, 0.0f),
                            254.5f / 255.0f };
  const size_t nspecial = sizeof(special) / sizeof(special[0]);
  const size_t count = (size_t)2 * 257 * (2 * around + 1)
    + ((size_t)1 << 32) / sweep + 1 + nspecial;
  float *v = malloc(sizeof(float) * count);
  assert_non_null(v);

  size_t k = 0;
  for(int i = 0; i <= 256; i++)
    for(int half = 0; half < 2; half++)
    {
      const uint32_t center = _to_bits((i + 0.5f * half) / 255.0f);
      for(int d = -around; d <= around; d++) v[k++] = _from_bits(center + d);
    }
  for(uint64_t bits = 0; bits < ((uint64_t)1 << 32); bits += sweep)
    v[k++] = _from_bits((uint32_t)bits);
  for(size_t i = 0; i < nspecial; i++) v[k++] = special[i];

  *n = k;
  return v;
}

static void test_scalar(void **state)
{
  size_t n;
  float *v = _inputs(&n);
  for(size_t k = 0; k < n; k++)
  {
    const uint8_t got = dt_display_to_8bit(v[k]);
    if(got != _reference(v[k]))
      fail_msg("%08x (%g): %d, expected %d", _to_bits(v[k]), v[k], got, _reference(v[k]));
  }
  free(v);
}

static void test_pixels(void **state)
{
  size_t n;
  float *v = _inputs(&n);
  // an odd count, so that the loop has a remainder after its vector part
  const size_t npixels = (n / 3) | 1;
  float *in = malloc(sizeof(float) * 4 * npixels);
  uint8_t *rgb = malloc(4 * npixels);
  uint8_t *bgr = malloc(4 * npixels);
  assert_non_null(in);
  assert_non_null(rgb);
  assert_non_null(bgr);
  for(size_t k = 0; k < npixels; k++)
  {
    for(int c = 0; c < 3; c++) in[4 * k + c] = v[(3 * k + c) % n];
    in[4 * k + 3] = NAN;
  }

  dt_display_to_8bit_pixels(in, rgb, npixels, FALSE);
  dt_display_to_8bit_pixels(in, bgr, npixels, TRUE);

  for(size_t k = 0; k < npixels; k++)
  {
    for(int c = 0; c < 3; c++)
    {
      const uint8_t expected = dt_display_to_8bit(in[4 * k + c]);
      if(rgb[4 * k + c] != expected || bgr[4 * k + 2 - c] != expected)
        fail_msg("pixel %zu channel %d, %08x: rgb %d, bgr %d, expected %d", k, c,
                 _to_bits(in[4 * k + c]), rgb[4 * k + c], bgr[4 * k + 2 - c], expected);
    }
    assert_int_equal(rgb[4 * k + 3], 0xff);
    assert_int_equal(bgr[4 * k + 3], 0xff);
  }
  free(in);
  free(rgb);
  free(bgr);
  free(v);
}

int main(int argc, char *argv[])
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_scalar),
    cmocka_unit_test(test_pixels),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
