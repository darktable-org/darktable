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
 * two profiles does not grow the list, and a report with no usable profile,
 * one that is not RGB included, gives the window sRGB again. the registry
 * entry of a window is its device encoding: the system display profile on
 * a cairo window, the backend's device on a platform-owned one, and the
 * signals say when it changes; it is no histogram or soft proof choice, so a
 * stored one becomes sRGB. colorspaces.c is included for its static
 * helpers, on a registry built here as dt_colorspaces_init() builds the
 * entries they touch.
 */

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>

#include <cmocka.h>

// count the signals colorspaces.c raises: signal.h is included once, so
// colorspaces.c expands this definition
#include "control/signal.h"
static int _raised[DT_SIGNAL_COUNT];
#undef DT_CONTROL_SIGNAL_RAISE
#define DT_CONTROL_SIGNAL_RAISE(signal, ...) (_raised[signal]++)

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
  self->system_display[0] = self->system_display_srgb[0] = f->placeholder;
  self->system_display[1] = self->system_display_srgb[1] = f->placeholder2;
  self->profiles = g_list_append(self->profiles,
                                 _create_profile(DT_COLORSPACE_DISPLAY, f->placeholder,
                                                 "display", -1, -1, 0, -1, -1, -1));
  self->profiles = g_list_append(self->profiles,
                                 _create_profile(DT_COLORSPACE_DISPLAY2, f->placeholder2,
                                                 "display2", -1, -1, -1, -1, -1, 0));
  self->profiles = g_list_append(self->profiles,
                                 _create_profile(DT_COLORSPACE_SRGB,
                                                 dt_colorspaces_create_srgb_profile(),
                                                 "sRGB", -1, 0, 1, 2, 0, 1));
  self->profiles = g_list_append(self->profiles,
                                 _create_profile(DT_COLORSPACE_ADOBERGB,
                                                 dt_colorspaces_create_adobergb_profile(),
                                                 "Adobe RGB", 0, 1, 2, 3, 1, 2));
  // every window has an output from the start, as dt_colorspaces_init()
  // gives it
  for(int w = 0; w < 2; w++)
  {
    _describe_system_display(self, w, &self->outputs[w]);
    self->outputs[w].serial = 1;
  }
  darktable.color_profiles = self;
  memset(_raised, 0, sizeof(_raised));

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
// the buffer, and the change is announced. NULL is a report without a
// profile. TRUE if the window's system display profile changed
static gboolean _replace(const dt_colorspaces_color_profile_type_t window,
                         const uint8_t *data,
                         const size_t size)
{
  dt_colorspaces_t *self = darktable.color_profiles;
  const int w = window == DT_COLORSPACE_DISPLAY2;
  pthread_rwlock_wrlock(&self->xprofile_lock);
  const dt_display_output_t before = self->outputs[w];
  guchar *copy = NULL;
  if(data)
  {
    copy = g_malloc(size);
    memcpy(copy, data, size);
  }
  const gboolean changed = _system_display_set(self, w, _system_profile(self, copy, size));
  const dt_display_output_t after = self->outputs[w];
  pthread_rwlock_unlock(&self->xprofile_lock);
  _output_changed(w, &before, &after);
  return changed;
}

// what the Wayland backend sets: linear BT.2020 images, GTK's untagged
// surface as sRGB
static void _own(const dt_colorspaces_color_profile_type_t window)
{
  const dt_display_output_t output =
    { .image = { .type = DT_COLORSPACE_LIN_REC2020, .intent = DT_INTENT_PERCEPTUAL,
                 .headroom = 1.0f },
      .device = { .type = DT_COLORSPACE_SRGB, .intent = DT_INTENT_PERCEPTUAL,
                  .headroom = 1.0f },
      .format = DT_FRAME_FLOAT };
  dt_colorspaces_set_window_output(window, &output);
}

static cmsHPROFILE _srgb(void)
{
  return dt_colorspaces_get_profile(DT_COLORSPACE_SRGB, "",
                                    DT_PROFILE_DIRECTION_DISPLAY)->profile;
}

static void _assert_raised(const int device, const int entry)
{
  assert_int_equal(_raised[DT_SIGNAL_CONTROL_PROFILE_USER_CHANGED], device);
  assert_int_equal(_raised[DT_SIGNAL_CONTROL_PROFILE_CHANGED], entry);
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

static void test_non_rgb_profile_means_srgb(void **state)
{
  _fixture_t *f = *state;
  dt_colorspaces_t *self = darktable.color_profiles;

  // a profile lcms reads but no screen can have: Lab, and gray, which a
  // transform from GTK's RGB cannot target
  cmsHPROFILE lab = cmsCreateLab4Profile(NULL);
  cmsToneCurve *gamma = cmsBuildGamma(NULL, 2.2);
  cmsHPROFILE gray = cmsCreateGrayProfile(cmsD50_xyY(), gamma);
  cmsFreeToneCurve(gamma);
  size_t lab_size = 0, gray_size = 0;
  uint8_t *lab_bytes = _bytes(lab, &lab_size);
  uint8_t *gray_bytes = _bytes(gray, &gray_size);
  cmsCloseProfile(lab);
  cmsCloseProfile(gray);

  assert_true(_replace(DT_COLORSPACE_DISPLAY, f->a, f->a_size));
  assert_true(_replace(DT_COLORSPACE_DISPLAY, lab_bytes, lab_size));
  assert_ptr_equal(_content(DT_COLORSPACE_DISPLAY), f->placeholder);
  assert_false(_replace(DT_COLORSPACE_DISPLAY, gray_bytes, gray_size));
  assert_ptr_equal(_content(DT_COLORSPACE_DISPLAY), f->placeholder);
  // neither is kept
  assert_int_equal(g_list_length(self->system_display_profiles), 3);
  g_free(lab_bytes);
  g_free(gray_bytes);
}

static void test_cairo_window_entry_is_system_profile(void **state)
{
  _fixture_t *f = *state;
  dt_colorspaces_t *self = darktable.color_profiles;

  assert_ptr_equal(_content(DT_COLORSPACE_DISPLAY), f->placeholder);
  assert_true(_replace(DT_COLORSPACE_DISPLAY, f->a, f->a_size));
  cmsHPROFILE a = self->system_display[0];
  assert_ptr_not_equal(a, f->placeholder);
  assert_ptr_equal(_content(DT_COLORSPACE_DISPLAY), a);
  assert_ptr_equal(self->outputs[0].device.profile, a);
  assert_ptr_equal(self->outputs[0].image.profile, a);
  // a new device encoding, and with it a new entry, each said once
  _assert_raised(1, 1);
  assert_ptr_equal(_content(DT_COLORSPACE_DISPLAY2), f->placeholder2);

  // the intent is part of the device encoding, not of the entry
  dt_conf_set_string("ui_last/display_intent", "relative colorimetric");
  dt_colorspaces_update_display_intent();
  assert_int_equal(self->outputs[0].device.intent, DT_INTENT_RELATIVE_COLORIMETRIC);
  assert_ptr_equal(_content(DT_COLORSPACE_DISPLAY), a);
  // both windows' device encodings changed
  _assert_raised(3, 1);
}

static void test_platform_owned_entry_is_device(void **state)
{
  _fixture_t *f = *state;
  dt_colorspaces_t *self = darktable.color_profiles;

  assert_true(_replace(DT_COLORSPACE_DISPLAY, f->a, f->a_size));
  cmsHPROFILE a = self->system_display[0];
  _assert_raised(1, 1);

  // the platform takes the window: the entry is the device it declared,
  // and the system display profile is kept for when cairo shows it again
  _own(DT_COLORSPACE_DISPLAY);
  assert_true(self->outputs[0].platform_owned);
  assert_ptr_equal(self->outputs[0].device.profile, _srgb());
  assert_ptr_equal(_content(DT_COLORSPACE_DISPLAY), _srgb());
  assert_ptr_equal(self->system_display[0], a);
  _assert_raised(2, 2);
  // the same output again changes nothing
  _own(DT_COLORSPACE_DISPLAY);
  _assert_raised(2, 2);

  // a new report describes the window for cairo only: neither its output
  // nor the entry changes, so nothing is announced
  const uint32_t serial = self->outputs[0].serial;
  assert_true(_replace(DT_COLORSPACE_DISPLAY, f->b, f->b_size));
  cmsHPROFILE b = self->system_display[0];
  assert_ptr_not_equal(b, a);
  assert_ptr_equal(_content(DT_COLORSPACE_DISPLAY), _srgb());
  assert_int_equal(self->outputs[0].serial, serial);
  _assert_raised(2, 2);
  // the other window keeps its own
  assert_ptr_equal(_content(DT_COLORSPACE_DISPLAY2), f->placeholder2);

  // given back: the entry is the system display profile again, the one
  // reported last
  dt_colorspaces_set_window_output(DT_COLORSPACE_DISPLAY, NULL);
  assert_false(self->outputs[0].platform_owned);
  assert_ptr_equal(self->outputs[0].device.profile, b);
  assert_ptr_equal(_content(DT_COLORSPACE_DISPLAY), b);
  _assert_raised(3, 3);
  _assert_valid(_content(DT_COLORSPACE_DISPLAY));

  // and taken again, as a window whose backend is chosen anew: cleanup
  // must then not free the entry's profile, which the sRGB entry owns
  _own(DT_COLORSPACE_DISPLAY);
  assert_ptr_equal(_content(DT_COLORSPACE_DISPLAY), _srgb());
  _assert_raised(4, 4);
}

static void test_display_is_no_histogram_choice(void **state)
{
  dt_colorspaces_t *self = darktable.color_profiles;

  // stored by a version that listed the system display profiles
  self->histogram_type = DT_COLORSPACE_DISPLAY;
  self->softproof_type = DT_COLORSPACE_DISPLAY2;
  _drop_unoffered_choices(self);
  assert_int_equal(self->histogram_type, DT_COLORSPACE_SRGB);
  assert_int_equal(self->softproof_type, DT_COLORSPACE_SRGB);

  self->histogram_type = DT_COLORSPACE_DISPLAY2;
  _drop_unoffered_choices(self);
  assert_int_equal(self->histogram_type, DT_COLORSPACE_SRGB);

  // what the menus offer stays
  self->histogram_type = DT_COLORSPACE_ADOBERGB;
  self->softproof_type = DT_COLORSPACE_ADOBERGB;
  _drop_unoffered_choices(self);
  assert_int_equal(self->histogram_type, DT_COLORSPACE_ADOBERGB);
  assert_int_equal(self->softproof_type, DT_COLORSPACE_ADOBERGB);
}

int main(int argc, char *argv[])
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test_setup_teardown(test_switching_reuses_profiles, _setup, _teardown),
    cmocka_unit_test_setup_teardown(test_windows_share_profiles, _setup, _teardown),
    cmocka_unit_test_setup_teardown(test_no_profile_means_srgb, _setup, _teardown),
    cmocka_unit_test_setup_teardown(test_non_rgb_profile_means_srgb, _setup, _teardown),
    cmocka_unit_test_setup_teardown(test_cairo_window_entry_is_system_profile, _setup,
                                    _teardown),
    cmocka_unit_test_setup_teardown(test_platform_owned_entry_is_device, _setup, _teardown),
    cmocka_unit_test_setup_teardown(test_display_is_no_histogram_choice, _setup, _teardown),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
