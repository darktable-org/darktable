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
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <cmocka.h>

#define dt_wayland_color_available _test_wayland_available
#define dt_conf_key_exists _test_conf_key_exists
#define dt_conf_get_float _test_conf_get_float
#include "libs/neural_restore.c"
#undef dt_conf_get_float
#undef dt_conf_key_exists
#undef dt_wayland_color_available

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

static gboolean _managed;
static float _strength;
gboolean _test_wayland_available(void)
{
  return _managed;
}

gboolean _test_conf_key_exists(const char *key)
{
  return TRUE;
}

float _test_conf_get_float(const char *key)
{
  return _strength;
}

static void _assert_pixel(const uint32_t pixel, const float rgb[3], const float detail)
{
  cmsHPROFILE linear = cmsCreate_sRGBProfile();
  cmsHPROFILE destination = _managed ? dt_display_ui_create_profile() : cmsCreate_sRGBProfile();
  assert_non_null(linear);
  assert_non_null(destination);
  cmsToneCurve *curve = cmsBuildGamma(NULL, 1.0);
  assert_non_null(curve);
  assert_true(cmsWriteTag(linear, cmsSigRedTRCTag, curve));
  assert_true(cmsWriteTag(linear, cmsSigGreenTRCTag, curve));
  assert_true(cmsWriteTag(linear, cmsSigBlueTRCTag, curve));
  cmsFreeToneCurve(curve);
  cmsHTRANSFORM transform = cmsCreateTransform
    (linear, TYPE_RGB_FLT, destination, TYPE_RGB_8,
     INTENT_RELATIVE_COLORIMETRIC, cmsFLAGS_NOCACHE);
  assert_non_null(transform);
  const float input[] = { rgb[0] + detail, rgb[1] + detail, rgb[2] + detail };
  uint8_t expected[3];
  cmsDoTransform(transform, input, expected, 1);
  for(int c = 0; c < 3; c++)
    assert_true(abs((int)((pixel >> (16 - 8 * c)) & 255) - expected[c]) <= 1);
  cmsDeleteTransform(transform);
  cmsCloseProfile(destination);
  cmsCloseProfile(linear);
}

static void _test_preview_packers(void **state)
{
  float rgb[] = { 0.003f, 0.18f, 0.7f };
  float rgba[] = { 0.003f, 0.18f, 0.7f, 1.0f };
  float detail = 0.025f;
  for(int managed = 0; managed < 2; managed++)
  {
    _managed = managed;
    uint32_t before = 0, after = 0;
    _float_rgb_to_cairo(rgb, (unsigned char *)&before, 1, 1, 4);
    _assert_pixel(before, rgb, 0.0f);
    dt_lib_neural_restore_t data = { .preview_after = rgb, .preview_detail = &detail,
                                    .preview_w = 1, .preview_h = 1, .cairo_stride = 4,
                                    .cairo_after = (unsigned char *)&after,
                                    .export_pixels = rgba, .export_w = 1, .export_h = 1 };
    _strength = 100.0f;
    _rebuild_cairo_after(&data);
    assert_int_equal(after, before);
    _strength = 0.0f;
    _rebuild_cairo_after(&data);
    _assert_pixel(after, rgb, detail);
    _build_export_cairo(&data);
    assert_int_equal(*(uint32_t *)data.export_cairo, before);
    g_free(data.export_cairo);
  }
}

int main(int argc, char *argv[])
{
  const struct CMUnitTest tests[] = { cmocka_unit_test(_test_preview_packers) };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
