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

#include "imageio/imageio.c"
#include "common/display_transport.h"

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

static void _test_preview_encoding(void **state)
{
  dt_colorspaces_t profiles = { 0 };
  darktable.color_profiles = &profiles;
  profiles.ui_profile = dt_display_ui_create_profile();
  dt_colorspaces_color_profile_t transport = {
    .type = DT_COLORSPACE_DISPLAY_TRANSPORT,
    .profile = dt_display_transport_create_profile()
  };
  profiles.profiles = g_list_append(NULL, &transport);
  dt_iop_order_iccprofile_info_t encoding = { .type = DT_COLORSPACE_DISPLAY_TRANSPORT };
  dt_dev_pixelpipe_t pipe = { .output_encoding = &encoding };
  const dt_aligned_pixel_t ui[3] = {
    { 0.07f, 0.5f, 0.9f, 1.0f }, { 0.9f, 0.07f, 0.5f, 1.0f }, { 0.5f, 0.9f, 0.07f, 1.0f }
  };
  dt_aligned_pixel_t input[3];
  cmsHTRANSFORM to_transport = cmsCreateTransform
    (profiles.ui_profile, TYPE_RGBA_FLT, transport.profile, TYPE_RGBA_FLT,
     DT_INTENT_RELATIVE_COLORIMETRIC, cmsFLAGS_NOCACHE);
  assert_non_null(to_transport);
  cmsDoTransform(to_transport, ui, input, 3);
  cmsDeleteTransform(to_transport);
  uint32_t output[3] = { 0 };
  _imageio_preview_t preview = { .head = { .width = 3, .height = 1 },
                               .bpp = 32, .buf = (uint8_t *)output };
  assert_int_equal(_preview_bpp(&preview.head), 32);
  assert_int_equal(_preview_levels(&preview.head), IMAGEIO_RGB | IMAGEIO_FLOAT);
  assert_int_equal(_preview_write_image(&preview.head, NULL, input,
                                        DT_COLORSPACE_DISPLAY_TRANSPORT, "", NULL,
                                        0, 0, 1, 1, &pipe, FALSE), 0);
  for(int k = 0; k < 3; k++)
    for(int c = 0; c < 3; c++)
    {
      const int actual = (output[k] >> (16 - 8 * c)) & 255;
      const int expected = lroundf(ui[k][c] * 255.0f);
      assert_true(abs(actual - expected) <= 1);
    }
  cmsHPROFILE ui_profile = profiles.ui_profile;
  profiles.ui_profile = NULL;
  assert_int_equal(_preview_write_image(&preview.head, NULL, input,
                                        DT_COLORSPACE_DISPLAY_TRANSPORT, "", NULL,
                                        0, 0, 1, 1, &pipe, FALSE), 1);
  profiles.ui_profile = ui_profile;
  pipe.output_encoding = NULL;
  assert_int_equal(_preview_write_image(&preview.head, NULL, input,
                                        DT_COLORSPACE_DISPLAY_TRANSPORT, "", NULL,
                                        0, 0, 1, 1, &pipe, FALSE), 1);
  const uint32_t legacy[] = { 0x123456, 0xfedcba, 0x334455 };
  preview.bpp = 8;
  assert_int_equal(_preview_levels(&preview.head), IMAGEIO_RGB | IMAGEIO_INT8);
  assert_int_equal(_preview_write_image(&preview.head, NULL, legacy,
                                        DT_COLORSPACE_DISPLAY, "", NULL,
                                        0, 0, 1, 1, &pipe, FALSE), 0);
  assert_memory_equal(output, legacy, sizeof(output));
  cmsCloseProfile(transport.profile);
  cmsCloseProfile(profiles.ui_profile);
  g_list_free(profiles.profiles);
  darktable.color_profiles = NULL;
}

int main(int argc, char *argv[])
{
  const struct CMUnitTest tests[] = { cmocka_unit_test(_test_preview_encoding) };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
