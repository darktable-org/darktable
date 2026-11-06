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
 * cmocka unit tests for dt_display_output_diff(): a change of a window's
 * output is announced by what changed. the image encoding or the frame
 * format alone (a new headroom, a new backend's frames) reruns the
 * window's producers only; the device encoding or the owner also reaches
 * what GTK draws; the serial is not compared.
 */

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>

#include <cmocka.h>

#include "common/colorspaces.h"

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

static int _profile_a, _profile_b;

// a cairo window's output: its system display profile as both encodings
static dt_display_output_t _cairo(void)
{
  dt_display_output_t o;
  memset(&o, 0, sizeof(o));
  o.image.type = DT_COLORSPACE_DISPLAY;
  o.image.profile = &_profile_a;
  o.image.intent = DT_INTENT_PERCEPTUAL;
  o.image.headroom = 1.0f;
  o.device = o.image;
  o.format = DT_FRAME_RGB24;
  o.serial = 7;
  return o;
}

// a native window's output: linear Rec.2020 with headroom, sRGB device
static dt_display_output_t _native(const float headroom)
{
  dt_display_output_t o;
  memset(&o, 0, sizeof(o));
  o.image.type = DT_COLORSPACE_LIN_REC2020;
  o.image.intent = DT_INTENT_PERCEPTUAL;
  o.image.headroom = headroom;
  o.device.type = DT_COLORSPACE_SRGB;
  o.device.intent = DT_INTENT_PERCEPTUAL;
  o.device.headroom = 1.0f;
  o.format = DT_FRAME_FLOAT;
  o.platform_owned = TRUE;
  o.serial = 3;
  return o;
}

static void test_nothing(void **state)
{
  const dt_display_output_t a = _cairo();
  dt_display_output_t b = _cairo();
  b.serial = 8;
  assert_int_equal(dt_display_output_diff(&a, &b), DT_DISPLAY_OUTPUT_UNCHANGED);
}

static void test_headroom_is_image_only(void **state)
{
  const dt_display_output_t a = _native(2.0f);
  const dt_display_output_t b = _native(4.0f);
  assert_int_equal(dt_display_output_diff(&a, &b), DT_DISPLAY_OUTPUT_IMAGE);
}

static void test_format_is_image_only(void **state)
{
  // frames for the same encodings in another format are new images, and
  // GTK draws the same
  const dt_display_output_t a = _cairo();
  dt_display_output_t b = _cairo();
  b.format = DT_FRAME_FLOAT;
  assert_int_equal(dt_display_output_diff(&a, &b), DT_DISPLAY_OUTPUT_IMAGE);
  assert_int_equal(dt_display_output_diff(&b, &a), DT_DISPLAY_OUTPUT_IMAGE);
}

static void test_system_profile_is_both(void **state)
{
  // on cairo a new system display profile changes both encodings
  const dt_display_output_t a = _cairo();
  dt_display_output_t b = _cairo();
  b.image.profile = b.device.profile = &_profile_b;
  assert_int_equal(dt_display_output_diff(&a, &b),
                   DT_DISPLAY_OUTPUT_IMAGE | DT_DISPLAY_OUTPUT_DEVICE);
}

static void test_intent_is_both(void **state)
{
  const dt_display_output_t a = _cairo();
  dt_display_output_t b = _cairo();
  b.image.intent = b.device.intent = DT_INTENT_RELATIVE_COLORIMETRIC;
  assert_int_equal(dt_display_output_diff(&a, &b),
                   DT_DISPLAY_OUTPUT_IMAGE | DT_DISPLAY_OUTPUT_DEVICE);
}

static void test_device_only(void **state)
{
  const dt_display_output_t a = _native(2.0f);
  dt_display_output_t b = _native(2.0f);
  b.device.type = DT_COLORSPACE_DISPLAY_P3;
  assert_int_equal(dt_display_output_diff(&a, &b), DT_DISPLAY_OUTPUT_DEVICE);
}

static void test_owner_is_device(void **state)
{
  // the same encodings, reported by the platform: what GTK draws hears of it
  const dt_display_output_t a = _cairo();
  dt_display_output_t b = _cairo();
  b.platform_owned = TRUE;
  assert_int_equal(dt_display_output_diff(&a, &b), DT_DISPLAY_OUTPUT_DEVICE);
}

static void test_taking_and_giving_back(void **state)
{
  const dt_display_output_t cairo = _cairo();
  const dt_display_output_t native = _native(1.0f);
  const dt_display_output_change_t all = DT_DISPLAY_OUTPUT_IMAGE | DT_DISPLAY_OUTPUT_DEVICE;
  assert_int_equal(dt_display_output_diff(&cairo, &native), all);
  assert_int_equal(dt_display_output_diff(&native, &cairo), all);
}

int main(int argc, char *argv[])
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_nothing),
    cmocka_unit_test(test_headroom_is_image_only),
    cmocka_unit_test(test_format_is_image_only),
    cmocka_unit_test(test_system_profile_is_both),
    cmocka_unit_test(test_intent_is_both),
    cmocka_unit_test(test_device_only),
    cmocka_unit_test(test_owner_is_device),
    cmocka_unit_test(test_taking_and_giving_back),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
