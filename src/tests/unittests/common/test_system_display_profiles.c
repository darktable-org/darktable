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
 * cmocka unit tests for the system display profiles of common/colorspaces.c:
 * a replaced profile stays valid until dt_colorspaces_cleanup(), the same
 * bytes always give the same profile, so switching back and forth between
 * two profiles does not grow the list, and a report with no usable profile
 * gives the window sRGB again. colorspaces.c is included
 * for its static helpers, on a registry built here as dt_colorspaces_init()
 * builds the entries they touch.
 */

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>

#include <cmocka.h>

#include "common/colorspaces.c"
#include "control/conf.h"

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

typedef struct _fixture_t
{
  cmsHPROFILE placeholder, placeholder2;
  uint8_t *a, *b;
  size_t a_size, b_size;
} _fixture_t;

static uint8_t *_bytes(cmsHPROFILE profile,
                       size_t *size)
{
  cmsUInt32Number n = 0;
  cmsSaveProfileToMem(profile, NULL, &n);
  uint8_t *data = g_malloc(n);
  cmsSaveProfileToMem(profile, data, &n);
  *size = n;
  return data;
}

static int _setup(void **state)
{
  // dt_colorspaces_cleanup() saves the soft proof settings to the configuration
  darktable.conf = calloc(1, sizeof(dt_conf_t));
  dt_pthread_mutex_init(&darktable.conf->mutex);
  darktable.conf->table = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  darktable.conf->override_entries = g_hash_table_new(g_str_hash, g_str_equal);

  _fixture_t *f = calloc(1, sizeof(_fixture_t));
  dt_colorspaces_t *self = calloc(1, sizeof(dt_colorspaces_t));
  pthread_rwlock_init(&self->xprofile_lock, NULL);

  f->placeholder = dt_colorspaces_create_srgb_profile();
  f->placeholder2 = dt_colorspaces_create_srgb_profile();
  _system_profile_add(self, NULL, 0, f->placeholder);
  _system_profile_add(self, NULL, 0, f->placeholder2);
  self->system_display_srgb[0] = f->placeholder;
  self->system_display_srgb[1] = f->placeholder2;
  self->profiles = g_list_append(self->profiles,
                                 _create_profile(DT_COLORSPACE_DISPLAY, f->placeholder,
                                                 "display", -1, -1, 0, 0, -1, -1));
  self->profiles = g_list_append(self->profiles,
                                 _create_profile(DT_COLORSPACE_DISPLAY2, f->placeholder2,
                                                 "display2", -1, -1, -1, 1, -1, 0));
  self->profiles = g_list_append(self->profiles,
                                 _create_profile(DT_COLORSPACE_SRGB,
                                                 dt_colorspaces_create_srgb_profile(),
                                                 "sRGB", -1, 0, 1, 2, 0, 1));
  self->profiles = g_list_append(self->profiles,
                                 _create_profile(DT_COLORSPACE_ADOBERGB,
                                                 dt_colorspaces_create_adobergb_profile(),
                                                 "Adobe RGB", 0, 1, 2, 3, 1, 2));
  darktable.color_profiles = self;

  // two system profiles a screen may report
  cmsHPROFILE p3 = _colorspaces_create_display_p3_rgb_profile();
  cmsHPROFILE adobe = dt_colorspaces_create_adobergb_profile();
  f->a = _bytes(p3, &f->a_size);
  f->b = _bytes(adobe, &f->b_size);
  cmsCloseProfile(p3);
  cmsCloseProfile(adobe);

  *state = f;
  return 0;
}

static int _teardown(void **state)
{
  _fixture_t *f = *state;
  // frees every system profile once, also one both windows share
  dt_colorspaces_cleanup(darktable.color_profiles);
  darktable.color_profiles = NULL;
  g_free(f->a);
  g_free(f->b);
  free(f);

  g_hash_table_destroy(darktable.conf->table);
  g_hash_table_destroy(darktable.conf->override_entries);
  dt_pthread_mutex_destroy(&darktable.conf->mutex);
  free(darktable.conf);
  darktable.conf = NULL;
  return 0;
}

// what the platform and colord paths do with a report: the callee takes
// the buffer. NULL is a report without a profile
static gboolean _replace(const dt_colorspaces_color_profile_type_t window,
                         const uint8_t *data,
                         const size_t size)
{
  dt_colorspaces_t *self = darktable.color_profiles;
  pthread_rwlock_wrlock(&self->xprofile_lock);
  guchar *copy = NULL;
  if(data)
  {
    copy = g_malloc(size);
    memcpy(copy, data, size);
  }
  const gboolean changed =
    _system_display_set(self, window == DT_COLORSPACE_DISPLAY2,
                        _system_profile(self, copy, size));
  pthread_rwlock_unlock(&self->xprofile_lock);
  return changed;
}

static cmsHPROFILE _content(const dt_colorspaces_color_profile_type_t window)
{
  return dt_colorspaces_get_profile(window, "", DT_PROFILE_DIRECTION_ANY)->profile;
}

// the profile stays usable: lcms can still build a transform from it
static void _assert_valid(cmsHPROFILE profile)
{
  assert_int_equal(cmsGetColorSpace(profile), cmsSigRgbData);
  cmsHPROFILE srgb = dt_colorspaces_get_profile(DT_COLORSPACE_SRGB, "",
                                                DT_PROFILE_DIRECTION_ANY)->profile;
  cmsHTRANSFORM xform = cmsCreateTransform(srgb, TYPE_RGBA_FLT, profile, TYPE_RGBA_FLT,
                                           INTENT_PERCEPTUAL, 0);
  assert_non_null(xform);
  const float in[4] = { 0.5f, 0.25f, 0.125f, 1.0f };
  float out[4] = { 0.0f };
  cmsDoTransform(xform, in, out, 1);
  assert_true(out[0] > 0.0f && out[0] < 1.0f);
  cmsDeleteTransform(xform);
}

static void test_switching_reuses_profiles(void **state)
{
  _fixture_t *f = *state;
  dt_colorspaces_t *self = darktable.color_profiles;

  assert_true(_replace(DT_COLORSPACE_DISPLAY, f->a, f->a_size));
  cmsHPROFILE a = _content(DT_COLORSPACE_DISPLAY);
  assert_ptr_not_equal(a, f->placeholder);
  assert_int_equal(g_list_length(self->system_display_profiles), 3);
  assert_non_null(self->transform_srgb_to_display);
  // the same bytes again change nothing
  assert_false(_replace(DT_COLORSPACE_DISPLAY, f->a, f->a_size));
  assert_ptr_equal(_content(DT_COLORSPACE_DISPLAY), a);

  assert_true(_replace(DT_COLORSPACE_DISPLAY, f->b, f->b_size));
  cmsHPROFILE b = _content(DT_COLORSPACE_DISPLAY);
  assert_ptr_not_equal(b, a);
  assert_int_equal(g_list_length(self->system_display_profiles), 4);

  for(int k = 0; k < 4; k++)
  {
    assert_true(_replace(DT_COLORSPACE_DISPLAY, f->a, f->a_size));
    assert_ptr_equal(_content(DT_COLORSPACE_DISPLAY), a);
    assert_true(_replace(DT_COLORSPACE_DISPLAY, f->b, f->b_size));
    assert_ptr_equal(_content(DT_COLORSPACE_DISPLAY), b);
  }
  assert_int_equal(g_list_length(self->system_display_profiles), 4);

  // what frames made before still name
  _assert_valid(f->placeholder);
  _assert_valid(a);
  _assert_valid(b);
}

static void test_windows_share_profiles(void **state)
{
  _fixture_t *f = *state;
  dt_colorspaces_t *self = darktable.color_profiles;

  assert_true(_replace(DT_COLORSPACE_DISPLAY, f->a, f->a_size));
  assert_true(_replace(DT_COLORSPACE_DISPLAY2, f->a, f->a_size));
  assert_ptr_equal(_content(DT_COLORSPACE_DISPLAY2), _content(DT_COLORSPACE_DISPLAY));
  assert_int_equal(g_list_length(self->system_display_profiles), 3);

  // the second window moves on, the main window keeps the shared profile
  assert_true(_replace(DT_COLORSPACE_DISPLAY2, f->b, f->b_size));
  assert_ptr_not_equal(_content(DT_COLORSPACE_DISPLAY2), _content(DT_COLORSPACE_DISPLAY));
  assert_int_equal(g_list_length(self->system_display_profiles), 4);
  _assert_valid(_content(DT_COLORSPACE_DISPLAY));
  _assert_valid(f->placeholder2);
}

static void test_no_profile_means_srgb(void **state)
{
  _fixture_t *f = *state;
  dt_colorspaces_t *self = darktable.color_profiles;

  // a stale profile goes: the window moved to a screen without one, or
  // the system dropped it
  assert_true(_replace(DT_COLORSPACE_DISPLAY, f->a, f->a_size));
  assert_true(_replace(DT_COLORSPACE_DISPLAY2, f->b, f->b_size));
  assert_true(_replace(DT_COLORSPACE_DISPLAY, NULL, 0));
  assert_ptr_equal(_content(DT_COLORSPACE_DISPLAY), f->placeholder);
  assert_false(_replace(DT_COLORSPACE_DISPLAY, NULL, 0));
  // the other window keeps its own
  assert_ptr_not_equal(_content(DT_COLORSPACE_DISPLAY2), f->placeholder2);
  assert_true(_replace(DT_COLORSPACE_DISPLAY2, NULL, 0));
  assert_ptr_equal(_content(DT_COLORSPACE_DISPLAY2), f->placeholder2);

  // bytes lcms cannot read are no usable profile either
  assert_true(_replace(DT_COLORSPACE_DISPLAY, f->a, f->a_size));
  const uint8_t junk[128] = { 1, 2, 3 };
  assert_true(_replace(DT_COLORSPACE_DISPLAY, junk, sizeof(junk)));
  assert_ptr_equal(_content(DT_COLORSPACE_DISPLAY), f->placeholder);
  assert_int_equal(g_list_length(self->system_display_profiles), 4);
  _assert_valid(_content(DT_COLORSPACE_DISPLAY));
}

int main(int argc, char *argv[])
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test_setup_teardown(test_switching_reuses_profiles, _setup, _teardown),
    cmocka_unit_test_setup_teardown(test_windows_share_profiles, _setup, _teardown),
    cmocka_unit_test_setup_teardown(test_no_profile_means_srgb, _setup, _teardown),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
