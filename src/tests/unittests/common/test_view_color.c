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

#define dt_wayland_color_available _test_native_available
#define dt_wayland_color_paint _test_native_paint
#define dt_mipmap_cache_get_with_caller _test_mipmap_get
#define dt_mipmap_cache_release_with_caller _test_mipmap_release
#define dt_mipmap_cache_get_matching_size _test_mipmap_size
#define dt_conf_get_bool _test_conf_bool
#include "views/view.c"
#include "common/display_transport.h"
#undef dt_wayland_color_available
#undef dt_wayland_color_paint
#undef dt_mipmap_cache_get_with_caller
#undef dt_mipmap_cache_release_with_caller
#undef dt_mipmap_cache_get_matching_size
#undef dt_conf_get_bool

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

static gboolean _native;
static int _paint_attempts;
static dt_mipmap_buffer_t _mip;

gboolean _test_native_available(void) { return _native; }
gboolean _test_native_paint(cairo_t *cr) { _paint_attempts++; return FALSE; }
gboolean _test_conf_bool(const char *name) { return FALSE; }
dt_mipmap_size_t _test_mipmap_size(const int32_t width, const int32_t height) { return DT_MIPMAP_0; }
void _test_mipmap_get(dt_mipmap_buffer_t *buf, const dt_imgid_t imgid,
                      const dt_mipmap_size_t mip, const dt_mipmap_get_flags_t flags,
                      const char mode, const char *file, const int line)
{
  *buf = _mip;
}
void _test_mipmap_release(dt_mipmap_buffer_t *buf, const char *file, const int line) { }

static cmsHPROFILE _adobe_profile(void)
{
  const cmsCIExyY white = { 0.3127, 0.3290, 1.0 };
  const cmsCIExyYTRIPLE primaries = {
    { 0.64, 0.33, 1.0 }, { 0.21, 0.71, 1.0 }, { 0.15, 0.06, 1.0 }
  };
  cmsToneCurve *curve = cmsBuildGamma(NULL, 2.19921875);
  cmsToneCurve *curves[] = { curve, curve, curve };
  cmsHPROFILE profile = cmsCreateRGBProfile(&white, &primaries, curves);
  cmsFreeToneCurve(curve);
  return profile;
}

static void _test_thumbnail_encoding(void **state)
{
  dt_gui_gtk_t gui = { .ppd = 1.0f, .ppd_thb = 1.0f };
  dt_mipmap_cache_t cache = { 0 };
  dt_colorspaces_t profiles = { 0 };
  darktable.gui = &gui;
  darktable.mipmap_cache = &cache;
  darktable.color_profiles = &profiles;
  pthread_rwlock_init(&profiles.xprofile_lock, NULL);
  cmsHPROFILE sources[] = { cmsCreate_sRGBProfile(), _adobe_profile() };
  cmsHPROFILE display = dt_display_ui_create_profile();
  cmsHTRANSFORM transforms[2];
  for(int k = 0; k < 2; k++)
  {
    transforms[k] = cmsCreateTransform(sources[k], TYPE_RGBA_8, display, TYPE_BGRA_8,
                                      INTENT_RELATIVE_COLORIMETRIC, 0);
    assert_non_null(transforms[k]);
  }
  profiles.transform_srgb_to_display = transforms[0];
  profiles.transform_adobe_rgb_to_display = transforms[1];
  uint8_t input[] = { 15, 100, 210, 255, 220, 40, 70, 255 };
  _mip = (dt_mipmap_buffer_t){ .buf = input, .width = 2, .height = 1,
                             .size = DT_MIPMAP_0, .imgid = 1 };
  for(int native = 0; native < 2; native++)
    for(int k = 0; k < 2; k++)
    {
      _native = native;
      _mip.color_space = k ? DT_COLORSPACE_ADOBERGB : DT_COLORSPACE_SRGB;
      uint8_t expected[8] = { 0 };
      cmsDoTransform(transforms[k], input, expected, 2);
      cairo_surface_t *surface = NULL;
      assert_int_equal(dt_view_image_get_surface(1, 2, 1, &surface, FALSE), DT_VIEW_SURFACE_OK);
      assert_non_null(surface);
      cairo_surface_flush(surface);
      const uint8_t *actual = cairo_image_surface_get_data(surface);
      for(int i = 0; i < 2; i++)
        assert_memory_equal(actual + 4 * i, expected + 4 * i, 3);
      cairo_surface_destroy(surface);
    }
  _native = FALSE;
  _mip.color_space = DT_COLORSPACE_DISPLAY;
  cairo_surface_t *surface = NULL;
  assert_int_equal(dt_view_image_get_surface(1, 2, 1, &surface, FALSE), DT_VIEW_SURFACE_OK);
  cairo_surface_flush(surface);
  const uint8_t *actual = cairo_image_surface_get_data(surface);
  for(int i = 0; i < 2; i++)
    for(int c = 0; c < 3; c++)
      assert_int_equal(actual[4 * i + c], input[4 * i + 2 - c]);
  cairo_surface_destroy(surface);
  for(int k = 0; k < 2; k++)
  {
    cmsDeleteTransform(transforms[k]);
    cmsCloseProfile(sources[k]);
  }
  cmsCloseProfile(display);
  pthread_rwlock_destroy(&profiles.xprofile_lock);
  darktable.gui = NULL;
  darktable.mipmap_cache = NULL;
  darktable.color_profiles = NULL;
}

static void _test_native_fallback(void **state)
{
  dt_colorspaces_t profiles = { 0 };
  darktable.color_profiles = &profiles;
  cmsHPROFILE transport = dt_display_transport_create_profile();
  cmsHPROFILE ui = dt_display_ui_create_profile();
  const cmsUInt32Number format = G_BYTE_ORDER == G_LITTLE_ENDIAN ? TYPE_BGRA_8 : TYPE_ARGB_8;
  profiles.transform_transport_to_ui8 = cmsCreateTransform
    (transport, TYPE_BGRA_8, ui, format, INTENT_RELATIVE_COLORIMETRIC, 0);
  assert_non_null(profiles.transform_transport_to_ui8);
  uint32_t input[] = { 0x305080, 0xc06030 };
  uint32_t expected[2] = { 0 };
  cmsDoTransform(profiles.transform_transport_to_ui8, input, expected, 2);
  cairo_surface_t *source = dt_view_create_surface((uint8_t *)input, 2, 1);
  for(int native = 0; native < 2; native++)
  {
    _native = native;
    _paint_attempts = 0;
    cairo_surface_t *target = cairo_image_surface_create(CAIRO_FORMAT_RGB24, 2, 1);
    cairo_t *cr = cairo_create(target);
    cairo_set_source_surface(cr, source, 0, 0);
    cairo_pattern_t *pattern = cairo_get_source(cr);
    dt_view_paint_display_surface(cr);
    assert_ptr_equal(cairo_get_source(cr), pattern);
    assert_int_equal(_paint_attempts, native);
    cairo_surface_flush(target);
    const uint32_t *actual = (const uint32_t *)cairo_image_surface_get_data(target);
    for(int i = 0; i < 2; i++)
      assert_int_equal(actual[i] & 0xffffff, (native ? expected[i] : input[i]) & 0xffffff);
    cairo_destroy(cr);
    cairo_surface_destroy(target);
  }
  _native = FALSE;
  cairo_surface_destroy(source);
  cmsDeleteTransform(profiles.transform_transport_to_ui8);
  cmsCloseProfile(transport);
  cmsCloseProfile(ui);
  darktable.color_profiles = NULL;
}

int main(int argc, char *argv[])
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(_test_thumbnail_encoding),
    cmocka_unit_test(_test_native_fallback)
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
