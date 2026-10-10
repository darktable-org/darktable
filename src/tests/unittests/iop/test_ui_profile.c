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
#include "common/iop_profile.c"
#include "common/display_transport.h"
#include "iop/primaries.c"
#undef dt_wayland_color_available

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

static gboolean _managed;
gboolean _test_wayland_available(void)
{
  return _managed;
}

static void _test_ui_destination(void **state)
{
  dt_colorspaces_t profiles = { 0 };
  dt_develop_t dev = { 0 };
  darktable.color_profiles = &profiles;
  profiles.ui_profile = dt_display_ui_create_profile();
  assert_non_null(profiles.ui_profile);
  profiles.ui_profile_info = dt_ioppr_create_ui_profile_info(profiles.ui_profile);
  assert_non_null(profiles.ui_profile_info);
  dt_colorspaces_color_profile_t entries[] = {
    { .type = DT_COLORSPACE_SRGB, .profile = cmsCreate_sRGBProfile() },
    { .type = DT_COLORSPACE_DISPLAY_TRANSPORT, .profile = dt_display_transport_create_profile() }
  };
  for(int k = 0; k < G_N_ELEMENTS(entries); k++)
    profiles.profiles = g_list_append(profiles.profiles, &entries[k]);
  dt_iop_order_iccprofile_info_t *srgb = dt_ioppr_add_profile_info_to_list
    (&dev, DT_COLORSPACE_SRGB, "", DT_INTENT_RELATIVE_COLORIMETRIC);
  dt_dev_pixelpipe_t pipe = { 0 };
  dt_ioppr_set_pipe_output_profile_info(&dev, &pipe, DT_COLORSPACE_DISPLAY_TRANSPORT,
                                       "", DT_INTENT_RELATIVE_COLORIMETRIC);
  _managed = FALSE;
  assert_ptr_equal(dt_ioppr_get_ui_profile_info(&pipe), pipe.output_profile_info);
  _managed = TRUE;
  const dt_iop_order_iccprofile_info_t *ui = dt_ioppr_get_ui_profile_info(&pipe);
  assert_ptr_equal(ui, profiles.ui_profile_info);
  assert_ptr_not_equal(ui, pipe.output_profile_info);
  assert_int_equal(pipe.output_encoding->type, DT_COLORSPACE_DISPLAY_TRANSPORT);
  for(int primary = 0; primary < 3; primary++)
  {
    dt_aligned_pixel_t linear, encoded, reference;
    _rotated_primary_to_display_RGB(srgb, ui, srgb, primary, 0.0f, 0.4f, linear);
    _apply_trc_if_nonlinear(ui, linear, encoded);
    _rotated_primary_to_display_RGB(srgb, srgb, srgb, primary, 0.0f, 0.4f, reference);
    for(int c = 0; c < 3; c++)
      assert_float_equal(encoded[c], powf(reference[c], 1.0f / 2.2f), 0.0002f);
  }
  for(GList *l = dev.allprofile_info; l; l = l->next)
  {
    dt_ioppr_cleanup_profile_info(l->data);
    dt_free_align(l->data);
  }
  g_list_free(dev.allprofile_info);
  g_list_free(profiles.profiles);
  for(int k = 0; k < G_N_ELEMENTS(entries); k++) cmsCloseProfile(entries[k].profile);
  dt_ioppr_cleanup_profile_info(profiles.ui_profile_info);
  dt_free_align(profiles.ui_profile_info);
  cmsCloseProfile(profiles.ui_profile);
  darktable.color_profiles = NULL;
}

int main(int argc, char *argv[])
{
  const struct CMUnitTest tests[] = { cmocka_unit_test(_test_ui_destination) };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
