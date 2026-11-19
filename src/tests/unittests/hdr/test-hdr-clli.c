/* Copyright (C) 2026 darktable developers.
 * SPDX-License-Identifier: GPL-3.0-or-later */

/* HDR10 content light levels from dt_imageio_pq_light_level(), the one
 * MaxCLL/MaxFALL measurement the AVIF, HEIF and JPEG XL writers share.
 * The PQ curve itself is covered by test-hdr-transfer.c. */

#include <math.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdlib.h>
#include <cmocka.h>

#include "common/darktable.h"
#include "imageio/imageio_common.h"

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

/* Independent ST 2084 codes for 100 and 1000 nits. */
#define PQ_100_NITS 0.50807842f
#define PQ_1000_NITS 0.75182710f

/* the float PQ decode is good to about 0.03 nits at 1000 nits */
#define NITS_TOLERANCE 0.05f

static void _measure(const float *const rgba,
                     const size_t npixels,
                     const float cll,
                     const float fall)
{
  float max_cll = -1.0f, max_fall = -1.0f;
  dt_imageio_pq_light_level(rgba, npixels, &max_cll, &max_fall);
  assert_float_equal(max_cll, cll, NITS_TOLERANCE);
  assert_float_equal(max_fall, fall, NITS_TOLERANCE);
}

/* peaks of 1000, 100 and 0 nits; alpha is ignored */
static void test_three_pixels(void **state)
{
  (void)state;
  const float rgba[] = {
    PQ_1000_NITS, 0.0f, 0.0f, 1.0f,
    0.0f, PQ_100_NITS, 0.0f, 1.0f,
    0.0f, 0.0f, 0.0f, 1.0f,
  };
  _measure(rgba, 3, 1000.0f, 1100.0f / 3.0f);
}

/* a NaN pixel counts as black, not as the PQ maximum */
static void test_nan_pixel(void **state)
{
  (void)state;
  const float rgba[] = {
    PQ_1000_NITS, 0.0f, 0.0f, 1.0f,
    NAN, NAN, NAN, 1.0f,
  };
  _measure(rgba, 2, 1000.0f, 500.0f);
}

/* a NaN channel next to a finite one drops out of the per-pixel max */
static void test_nan_channel(void **state)
{
  (void)state;
  const float rgba[] = {
    NAN, PQ_100_NITS, 0.0f, 1.0f,
    0.0f, 0.0f, 0.0f, 1.0f,
  };
  _measure(rgba, 2, 100.0f, 50.0f);
}

/* codes above 1.0, +Inf included, clamp to 10000 nits, the ceiling of the
   16-bit HDR10 fields */
static void test_above_one(void **state)
{
  (void)state;
  const float rgba[] = {
    PQ_1000_NITS, 0.0f, 0.0f, 1.0f,
    1.5f, 0.0f, 0.0f, 1.0f,
    INFINITY, 0.0f, 0.0f, 1.0f,
  };
  _measure(rgba, 3, 10000.0f, 21000.0f / 3.0f);
}

/* negative samples count as black */
static void test_negative_pixel(void **state)
{
  (void)state;
  const float rgba[] = {
    -1.0f, -1.0f, -1.0f, 1.0f,
    PQ_100_NITS, 0.0f, 0.0f, 1.0f,
  };
  _measure(rgba, 2, 100.0f, 50.0f);
}

/* an empty buffer is never read and gives 0 / 0 */
static void test_empty(void **state)
{
  (void)state;
  _measure(NULL, 0, 0.0f, 0.0f);
}

/* a buffer large enough to split across threads keeps the exact reduction:
   one 1000-nit pixel among black ones */
static void test_threaded_reduction(void **state)
{
  (void)state;
  const size_t npixels = 4096;
  float *const rgba = calloc(4 * npixels, sizeof(float));
  assert_non_null(rgba);
  rgba[4 * (npixels - 1) + 1] = PQ_1000_NITS;
  _measure(rgba, npixels, 1000.0f, 1000.0f / (float)npixels);
  free(rgba);
}

int main(int argc, char *argv[])
{
  /* the OpenMP loops cap their team at darktable's thread count, which
     dt_init() would set */
  darktable.num_openmp_threads = dt_get_num_procs();

  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_three_pixels),
    cmocka_unit_test(test_nan_pixel),
    cmocka_unit_test(test_nan_channel),
    cmocka_unit_test(test_above_one),
    cmocka_unit_test(test_negative_pixel),
    cmocka_unit_test(test_empty),
    cmocka_unit_test(test_threaded_reduction),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
