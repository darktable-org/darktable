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

#include "common/display_transport.h"
#include "iop/colorout.c"

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

static void _test_gamut_warning(void **state)
{
  dt_colorspaces_t profiles = { 0 };
  darktable.color_profiles = &profiles;
  darktable.num_openmp_threads = 1;
#ifdef _OPENMP
  omp_set_num_threads(1);
#endif

  cmsHPROFILE srgb = cmsCreate_sRGBProfile();
  cmsHPROFILE lab = cmsCreateLab4Profile(NULL);
  cmsHPROFILE transport = dt_display_transport_create_profile();
  assert_non_null(srgb);
  assert_non_null(lab);
  assert_non_null(transport);
  cmsSetProfileVersion(srgb, 2.4);
  cmsHPROFILE proof = dt_colorspaces_make_temporary_profile(srgb);
  assert_non_null(proof);
  profiles.transform_srgb_to_transport_float = cmsCreateTransform
    (srgb, TYPE_RGBA_FLT, transport, TYPE_RGBA_FLT,
     INTENT_RELATIVE_COLORIMETRIC, cmsFLAGS_NOCACHE | cmsFLAGS_COPY_ALPHA);
  assert_non_null(profiles.transform_srgb_to_transport_float);

  cmsUInt16Number global_alarm[cmsMAXCHANNELS], after[cmsMAXCHANNELS];
  cmsGetAlarmCodes(global_alarm);
  const cmsUInt32Number flags = cmsFLAGS_SOFTPROOFING | cmsFLAGS_NOCACHE
                               | cmsFLAGS_BLACKPOINTCOMPENSATION;
  dt_dev_pixelpipe_iop_t pieces[2] = { 0 };
  cmsHPROFILE outputs[2] = { srgb, transport };
  for(int k = 0; k < 2; k++)
  {
    init_pipe(NULL, NULL, &pieces[k]);
    dt_iop_colorout_data_t *d = pieces[k].data;
    d->mode = DT_PROFILE_GAMUTCHECK;
    d->type = k ? DT_COLORSPACE_DISPLAY_TRANSPORT : DT_COLORSPACE_SRGB;
    d->xform = _create_transform(d, lab, outputs[k], TYPE_RGBA_FLT, proof,
                                 DT_INTENT_RELATIVE_COLORIMETRIC, flags | cmsFLAGS_GAMUTCHECK);
    assert_non_null(d->xform);
    assert_non_null(d->context);
  }
  assert_ptr_not_equal(((dt_iop_colorout_data_t *)pieces[0].data)->context,
                       ((dt_iop_colorout_data_t *)pieces[1].data)->context);
  cmsGetAlarmCodes(after);
  assert_memory_equal(global_alarm, after, sizeof(global_alarm));

  dt_aligned_pixel_t input[2] = { { 50.0f, 100.0f, 100.0f, 0.0f }, { 0.0f } };
  const float gray = 0x7f00 / 65535.0f;
  const dt_aligned_pixel_t gray_rgb = { gray, gray, gray, 0.0f };
  cmsHTRANSFORM to_lab = cmsCreateTransform(srgb, TYPE_RGBA_FLT, lab, TYPE_LabA_FLT,
                                            INTENT_RELATIVE_COLORIMETRIC, cmsFLAGS_NOCACHE);
  assert_non_null(to_lab);
  cmsDoTransform(to_lab, gray_rgb, input[1], 1);
  cmsDeleteTransform(to_lab);

  for(int k = 0; k < 2; k++)
  {
    const dt_iop_colorout_data_t *d = pieces[k].data;
    dt_aligned_pixel_t result[2] = { { 0.0f } }, expected_gray, cyan;
    _gamut_warning_color(d, cyan);
    _transform_lcms(d, result[0], input[0], 2);
    cmsHTRANSFORM without_warning = cmsCreateProofingTransform
      (lab, TYPE_LabA_FLT, outputs[k], TYPE_RGBA_FLT, proof,
       INTENT_RELATIVE_COLORIMETRIC, INTENT_RELATIVE_COLORIMETRIC, flags);
    assert_non_null(without_warning);
    cmsDoTransform(without_warning, input[1], expected_gray, 1);
    for(int c = 0; c < 3; c++)
    {
      assert_float_equal(result[0][c], cyan[c], 2e-5f);
      assert_float_equal(result[1][c], expected_gray[c], 1e-6f);
    }
    cmsDeleteTransform(without_warning);
    cleanup_pipe(NULL, NULL, &pieces[k]);
  }
  cmsDeleteTransform(profiles.transform_srgb_to_transport_float);
  cmsCloseProfile(proof);
  cmsCloseProfile(transport);
  cmsCloseProfile(lab);
  cmsCloseProfile(srgb);
  darktable.color_profiles = NULL;
}

int main(int argc, char *argv[])
{
  const struct CMUnitTest tests[] = { cmocka_unit_test(_test_gamut_warning) };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
