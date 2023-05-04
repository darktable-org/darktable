/*
    This file is part of darktable,
    Copyright (C) 2020-2023 darktable developers.

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
 * cmocka unit tests for the module iop/filmicrgb.c
 *
 * Please see README.md for more detailed documentation.
 */
#include <limits.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <math.h>

#include <cmocka.h>

#include "../util/assert.h"
#include "../util/tracing.h"
#include "../util/testimg.h"

#include "iop/filmicrgb.c"

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

/*
 * DEFINITIONS
 */

// epsilon for floating point comparison (1e-6 is approximately 20 EV below pure
// white):
#define E 1e-6f

/*
 * MOCKED FUNCTIONS
 */

void __wrap_dt_iop_color_picker_reset(dt_iop_module_t *module, gboolean update)
{
  check_expected_ptr(module);
  check_expected(update);
}


/*
 * TEST FUNCTIONS
 */

static void test_name(void **state)
{
  assert_string_equal(name(), "filmic rgb");
}

static void test_default_group(void **state)
{
  assert_int_equal(default_group(), IOP_GROUP_TONE | IOP_GROUP_TECHNICAL);
}

static void test_clamp_simd(void **state)
{
  for(float x = -0.5f; x <= 1.5f; x += 0.1f)
  {
    if(x < 0.0f)
    {
      assert_float_equal(clamp_simd(x), 0.0f, E);
    }
    else if(x > 1.0f)
    {
      assert_float_equal(clamp_simd(x), 1.0f, E);
    }
    else
    {
      assert_float_equal(clamp_simd(x), x, E);
    }
  }
}

static void test_pixel_rgb_norm_power(void **state)
{
  Testimg *ti;

  TR_STEP("verify that norm is correct and in ]0.0; 1.0] for rgb values "
    "in ]0.0; 1.0]");
  ti = testimg_gen_rgb_space(TESTIMG_STD_WIDTH);
  for_testimg_pixels_p_xy(ti)
  {
    p[3] = 2.0f;  // to make sure pixel[3] has no influence
    float norm = pixel_rgb_norm_power(p);
    TR_DEBUG("pixel={%e, %e, %e) => norm=%e", p[0], p[1], p[2], norm);
    float numerator = p[0] * p[0] * p[0] + p[1] * p[1] * p[1] + p[2] * p[2] * p[2];
    float denominator = p[0] * p[0] + p[1] * p[1] + p[2] * p[2];
    float exp_norm = numerator / denominator;
    assert_float_equal(norm, exp_norm, E);
    assert_true(norm > 0.0f);
    assert_true(norm <= 1.0f + 1e-6f);
  }
  testimg_free(ti);

  TR_STEP("verify that norm is equal to pixel (r=g=b) value on greyscale "
    "values");
  ti = testimg_gen_grey_space(TESTIMG_STD_WIDTH);
  for_testimg_pixels_p_xy(ti)
  {
    p[3] = 2.0f;  // to make sure pixel[3] has no influence
    float norm = pixel_rgb_norm_power(p);
    TR_DEBUG("pixel={%e, %e, %e) => norm=%e", p[0], p[1], p[2], norm);
    assert_float_equal(norm, p[0], E);
  }
  testimg_free(ti);

  TR_STEP("verify that norm is in ]0; +inf[ for bad greyscale pixels in "
    "]0;  +inf[");
  TR_BUG("norm is undefined for extreme values, thus values outside "
    "[1e-6; 1e6] are excluded from assertion.");
  ti = testimg_gen_grey_max_dr();
  for_testimg_pixels_p_xy(ti)
  {
    float norm = pixel_rgb_norm_power(p);
    TR_DEBUG("pixel={%e, %e, %e) => norm=%e", p[0], p[1], p[2], norm);
    if(p[0] > 1e-6 && p[0] < 1e6)
    {
      assert_true(norm > 0.0f);
      assert_true(norm <= FLT_MAX);
    }
  }
  testimg_free(ti);

  TR_STEP("verify that norm is in ]0; +inf[ for bad negative greyscale pixels "
    "in ]-inf; 0]");
  TR_BUG("norm is undefined for extreme values, thus values outside "
    "[1e-6; 1e6] are excluded from assertion.");
  TR_BUG("norm is 0 if input is 0.");
  ti = testimg_gen_grey_max_dr_neg();
  for_testimg_pixels_p_xy(ti)
  {
    float norm = pixel_rgb_norm_power(p);
    TR_DEBUG("pixel={%e, %e, %e) => norm=%e", p[0], p[1], p[2], norm);
    if(fabsf(p[0]) > 1e-6 && fabsf(p[0]) < 1e6)
    {
      assert_true(norm > 0.0f);
      assert_true(norm <= FLT_MAX);
    }
    if(p[0] > -FLT_MIN && p[0] < FLT_MIN) // translates to: if(p[0] == 0)
    {
      assert_float_equal(norm, 0.0f, FLT_MIN);
    }
  }
  testimg_free(ti);
}

static void test_get_pixel_norm(void **state)
{
  Testimg *ti;
  // dt_iop_order_iccprofile_info_t work_profile; // see TODOs below

  TR_STEP("verify that max-rgb norm is correct and in ]0.0; 1.0] for rgb "
    "values in ]0.0; 1.0]");
  ti = testimg_gen_rgb_space(TESTIMG_STD_WIDTH);
  for_testimg_pixels_p_xy(ti)
  {
    p[3] = 2.0f;  // to make sure pixel[3] has no influence
    float norm = get_pixel_norm(p, DT_FILMIC_METHOD_MAX_RGB, NULL);
    TR_DEBUG("pixel={%e, %e, %e, %e} => norm=%e", p[0], p[1], p[2], p[3], norm);
    assert_float_equal(norm, fmax(p[0], fmax(p[1], p[2])), E);
    assert_true(norm > 0.0f);
    assert_true(norm <= 1.0f + E);
  }
  testimg_free(ti);

  TR_STEP("verify that max-rgb norm is equal to pixel (r=g=b) value on "
    "greyscale values");
  ti = testimg_gen_grey_space(TESTIMG_STD_WIDTH);
  for_testimg_pixels_p_xy(ti)
  {
    p[3] = 2.0f;  // to make sure pixel[3] has no influence
    float norm = get_pixel_norm(p, DT_FILMIC_METHOD_MAX_RGB, NULL);
    TR_DEBUG("pixel={%e, %e, %e) => norm=%e", p[0], p[1], p[2], norm);
    assert_float_equal(norm, p[0], E);
  }
  testimg_free(ti);

  TR_STEP("verify that max-rgb norm is in ]0; +inf[ for bad greyscale pixels "
    "in ]0;  +inf[");
  ti = testimg_gen_grey_max_dr();
  for_testimg_pixels_p_xy(ti)
  {
    float norm = get_pixel_norm(p, DT_FILMIC_METHOD_MAX_RGB, NULL);
    TR_DEBUG("pixel={%e, %e, %e, %e} => norm=%e", p[0], p[1], p[2], p[3], norm);
    assert_true(norm > 0.0f);
    assert_true(norm <= FLT_MAX);
  }
  testimg_free(ti);

  TR_STEP("verify that max-rgb norm is in ]0; +inf[ for bad negative greyscale "
    "pixels in ]-inf; 0]");
  TR_BUG("max-rgb norm is unbounded and negative for pixels with all-negative "
    "colors.");
  ti = testimg_gen_grey_max_dr_neg();
  for_testimg_pixels_p_xy(ti)
  {
    float norm = get_pixel_norm(p, DT_FILMIC_METHOD_MAX_RGB, NULL);
    TR_DEBUG("pixel={%e, %e, %e, %e} => norm=%e", p[0], p[1], p[2], p[3], norm);
    // bug: assert_true(norm > 0.0f);
    assert_true(norm <= FLT_MAX);
  }
  testimg_free(ti);

  TR_STEP("verify luminance-y norm (verify subsequent function calls)");
  // TODO: find out how to mock inline functions!

  TR_STEP("verify power norm (verify subsequent function calls)");
  // note: the norm itself is verified in test_pixel_rgb_norm_power(), so here
  // we only verify that the function pixel_rgb_norm_power() is called.
  // TODO: find out how to mock inline functions!
}

static void test_log_tonemapping_v2(void **state)
{
  Testimg *ti;
  float grey = 0.1845f;
  float dyn_range = TESTIMG_STD_DYN_RANGE_EV;
  float black = log2f(1.0f/grey) - dyn_range;
  const float MIN = 0.0f;
  const float MAX = 1.0f;

  TR_STEP("verify that output is equal to log-mapped input for equal dynamic "
    "range and grey/black points");
  ti = testimg_gen_grey_space(TESTIMG_STD_WIDTH);
  for_testimg_pixels_p_xy(ti)
  {
    float ret = log_tonemapping_v2_1ch(p[0], grey, black, dyn_range);
    TR_DEBUG("%e => %e", p[0], ret);
    float exp = testimg_val_to_log(p[0]);
    if(exp < MIN)
    {
      assert_float_equal(ret, MIN, E);  // bound to -16EV
    }
    else
    {
      assert_float_equal(ret, exp, E);
    }
  }
  testimg_free(ti);

  TR_STEP("verify that output is 1 EV brighter (and clipped to [0; 1]) when "
    "grey is set to half");
  ti = testimg_gen_grey_space(TESTIMG_STD_WIDTH);
  for_testimg_pixels_p_xy(ti)
  {
    float ret = log_tonemapping_v2(p[0], (grey / 2.0f), black, dyn_range);
    TR_DEBUG("%e => %e", p[0], ret);
    float exp = testimg_val_to_log(p[0] * 2.0f);  // *2.0 means +1EV
    if(exp < MIN)
    {
      assert_float_equal(ret, MIN, E);  // bound to 2^-16
    }
    else if(exp > MAX)
    {
      assert_float_equal(ret, MAX, E);  // bound to 1.0
    }
    else
    {
      assert_float_equal(ret, exp, E);
    }
  }
  testimg_free(ti);

  TR_STEP("verify that output is bound to [0; 1] for all non-negative values");
  ti = testimg_gen_grey_max_dr();
  for_testimg_pixels_p_xy(ti)
  {
    float ret = log_tonemapping_v2(p[0], grey, black, dyn_range);
    TR_DEBUG("{%e, %e, %e, %e} => %e", p[0], p[1], p[2], p[3], ret);
    assert_true(ret >= MIN);
    assert_true(ret <= MAX);
  }
  testimg_free(ti);

  TR_STEP("verify that output is bound to [0; 1] for all negative values "
    "(incl. 0.0)");
  ti = testimg_gen_grey_max_dr_neg();
  for_testimg_pixels_p_xy(ti)
  {
    float ret = log_tonemapping_v2(p[0], grey, black, dyn_range);
    TR_DEBUG("{%e, %e, %e, %e} => %e", p[0], p[1], p[2], p[3], ret);
    assert_true(ret >= MIN);
    assert_true(ret <= MAX);
  }
  testimg_free(ti);
}

static void test_filmic_spline(void **state)
{
  // TODO: write tests for the method test_filmic_spline
  //
  // The problem with this method is that it needs the spline parameters that
  // are hard to figure out. We could call dt_iop_filmic_rgb_compute_spline() to
  // get the parameters but then it is still hard to estimate what the asserts
  // should look like.
  //
  // Done a code review of the method test_filmic_spline() and I think it is ok.

  TR_NOTE("method verified by code review only since it is hard to test it and "
    "the benefit is questionable");
}

// helper method to map gui saturation to internally used one:
static float saturation_gui_to_internal(float saturation_percent)
{
  // TODO: there is a flaw in conversion of saturation from gui value to
  // internal value. Discussed this with @aurelienpierre and decision was to
  // leave it for the moment (Feb 2020). This code here needs to be adapted when
  // the bug gets fixed.

  TR_BUG("saturation conversion from gui to internal is wrong");
  return (2.0f * saturation_percent / 100.0f + 1.0f);  // copied from filmicrgb.c
  //fix: return 100.0f / fmaxf(100.0f - saturation_percent, 1e-6);
}

static void test_filmic_desaturate_v1(void **state)
{
  Testimg *ti;

  // input values
  float lattitude_min = 0.2;
  float lattitude_max = 0.2;  // symmetrical
  float saturation_percent = 5.0f;

  // computed values
  // copied 2 lines from filmicrgb.c:
  float sigma_toe = powf(lattitude_min / 3.0f, 2.0f);
  float sigma_shoulder = powf(lattitude_max / 3.0f, 2.0f);

  float saturation = saturation_gui_to_internal(saturation_percent);

  TR_STEP("verify values are correct for different latitudes");
  TR_BUG("values inside latitude are not always 1.0 (but very close), "
    "especially at the borders");
  for(float latitude_min = 0.1f; latitude_min < 0.5f + E; latitude_min += 0.1f)
  {
    for(float latitude_max = 0.1f; latitude_max < 0.5f + E; latitude_max += 0.1f)
    {
      TR_DEBUG("saturation=%e", saturation);
      TR_DEBUG("latitude_min=%e", latitude_min);
      TR_DEBUG("latitude_max=%e", latitude_max);

      // copied 2 lines from filmicrgb.c:
      sigma_toe = powf(lattitude_min / 3.0f, 2.0f);
      sigma_shoulder = powf(lattitude_max / 3.0f, 2.0f);

      TR_DEBUG("sigma_toe=%e", sigma_toe);
      TR_DEBUG("sigma_shoulder=%e", sigma_shoulder);

      // filmic_desaturate works in log space:
      // create image with values from 0.0 to 1.0 in 0.05 steps:
      ti = testimg_to_log(testimg_gen_grey_space(21));
      for_testimg_pixels_p_yx(ti)
      {
        float ret =
          filmic_desaturate_v1(p[0], sigma_toe, sigma_shoulder, saturation);
        TR_DEBUG("%e => %e", p[0], ret);

        if(lattitude_min == lattitude_max)
        {
          // values symmetric (due to sigma_shoulder = sigma_toe):
          float *p1 = get_pixel(ti, ti->width - x - 1, y);
          float exp = filmic_desaturate_v1(p1[0], sigma_toe, sigma_shoulder,
            saturation);
          assert_float_equal(ret, exp, E);
        }

        // values correct on extreme borders:
        if(x == 0 || x == ti->width - 1)
        {
          assert_float_equal(ret, 1.0f - 1.0f / saturation, E);
        }

        //bug: values close to 1.0 during latitude, not exactly 1.0:
        if(x > (lattitude_min * ti->width) &&
            x < ((1.0f - lattitude_max) * ti->width - 1))
        {
          assert_float_equal(ret, 1.0f, 1e-2);
        }
      }
      testimg_free(ti);
    }
  }

  TR_STEP("verify return value is always 1.0 when saturation is set to maximum");
  TR_BUG("values inside latitude are not always 1.0 (but very close), "
    "especially at the borders");
  // create image with values from 0.0 to 1.0 in 0.05 steps:
  ti = testimg_to_log(testimg_gen_grey_space(21));
  saturation = saturation_gui_to_internal(1e6); // TODO: take 100%
  for_testimg_pixels_p_xy(ti)
  {
    float ret = filmic_desaturate_v1(p[0], sigma_toe, sigma_shoulder, saturation);
    TR_DEBUG("%e => %e", p[0], ret);
    //bug: values close to 1.0 during latitude, not exactly 1.0:
    assert_float_equal(ret, 1.0f, 1e-2);
  }
  // set saturation back:
  saturation = saturation_gui_to_internal(saturation_percent);
  testimg_free(ti);

  TR_STEP("verify output is in ]0; 1] for bad values in ]0; +inf[");
  ti = testimg_gen_grey_max_dr();
  for_testimg_pixels_p_xy(ti)
  {
    float ret = filmic_desaturate_v1(p[0], sigma_toe, sigma_shoulder, saturation);
    TR_DEBUG("{%e} => %e", p[0], ret);
    assert_true(ret > 0.0f);
    assert_true(ret <= 1.0f);
  }
  testimg_free(ti);

  TR_STEP("verify output is in ]0; 1] for bad negative values in ]-inf; 0]");
  ti = testimg_gen_grey_max_dr_neg();
  for_testimg_pixels_p_xy(ti)
  {
    float ret = filmic_desaturate_v1(p[0], sigma_toe, sigma_shoulder, saturation);
    TR_DEBUG("{%e} => %e", p[0], ret);
    assert_true(ret > 0.0f);
    assert_true(ret <= 1.0f);
  }
  testimg_free(ti);
}

static void test_linear_saturation(void **state)
{
  Testimg *ti;

  float luminance = 1.0f;
  float saturation = 0.05f;
  float ratios[] = { 0.2126, 0.7152, 0.0722 };

  TR_STEP("verify that output is equal to value for greyscale values");
  ti = testimg_gen_grey_space(TESTIMG_STD_WIDTH);
  for_testimg_pixels_p_xy(ti)
  {
    luminance = p[0];  // luminance := value, for greyscale values
    float s0 = linear_saturation(p[0], luminance, saturation);
    float s1 = linear_saturation(p[1], luminance, saturation);
    float s2 = linear_saturation(p[2], luminance, saturation);
    TR_DEBUG("pixel={%e, %e, %e) => linear_saturation={%e, %e, %e}",
      p[0], p[1], p[2], s0, s1, s2);
    assert_float_equal(s0, p[0], E);
    assert_float_equal(s1, p[1], E);
    assert_float_equal(s2, p[2], E);
  }
  testimg_free(ti);

  TR_STEP("verify that output is equal to value for rgb values when saturation "
    "is 1.0");
  saturation = 1.0f;
  ti = testimg_gen_rgb_space(TESTIMG_STD_WIDTH);
  for_testimg_pixels_p_xy(ti)
  {
    luminance = p[0] * ratios[0] + p[1] * ratios[1] + p[2] * ratios[2];
    float s0 = linear_saturation(p[0], luminance, saturation);
    float s1 = linear_saturation(p[1], luminance, saturation);
    float s2 = linear_saturation(p[2], luminance, saturation);
    TR_DEBUG("pixel={%e, %e, %e) => linear_saturation={%e, %e, %e}",
      p[0], p[1], p[2], s0, s1, s2);
    assert_float_equal(s0, p[0], E);
    assert_float_equal(s1, p[1], E);
    assert_float_equal(s2, p[2], E);
  }
  testimg_free(ti);

  TR_STEP("verify that output is pure grey, equal to luminance, for rgb values "
    "when saturation is 0.0");
  saturation = 0.0f;
  ti = testimg_gen_rgb_space(TESTIMG_STD_WIDTH);
  for_testimg_pixels_p_xy(ti)
  {
    luminance = p[0] * ratios[0] + p[1] * ratios[1] + p[2] * ratios[2];
    float s0 = linear_saturation(p[0], luminance, saturation);
    float s1 = linear_saturation(p[1], luminance, saturation);
    float s2 = linear_saturation(p[2], luminance, saturation);
    TR_DEBUG("pixel={%e, %e, %e) => linear_saturation={%e, %e, %e}",
      p[0], p[1], p[2], s0, s1, s2);
    assert_float_equal(s0, s1, E);
    assert_float_equal(s0, s2, E);
    assert_float_equal(s0, luminance, E);
  }
  testimg_free(ti);
}

/*
 * MAIN FUNCTION
 */
int main(int argc, char* argv[])
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_name),
    cmocka_unit_test(test_default_group),
    cmocka_unit_test(test_clamp_simd),
    cmocka_unit_test(test_pixel_rgb_norm_power),
    cmocka_unit_test(test_get_pixel_norm),
    cmocka_unit_test(test_log_tonemapping_v2),
    cmocka_unit_test(test_filmic_spline),
    cmocka_unit_test(test_filmic_desaturate_v1),
    cmocka_unit_test(test_linear_saturation)
  };

  TR_DEBUG("epsilon = %e", E);

  return cmocka_run_group_tests(tests, NULL, NULL);
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

