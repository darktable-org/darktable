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

static int _logged_errors;
static cmsContext _logged_context;

static void _test_error_handler(const cmsContext context,
                                const cmsUInt32Number code,
                                const char *text)
{
  _logged_errors++;
  _logged_context = context;
}

#define dt_colorspaces_error_handler _test_error_handler
#include "iop/colorout.c"
#undef dt_colorspaces_error_handler

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
    const unsigned char invalid_profile[] = { 0 };
    const int before = _logged_errors;
    _logged_context = NULL;
    assert_null(cmsOpenProfileFromMemTHR(d->context, invalid_profile, sizeof(invalid_profile)));
    assert_true(_logged_errors > before);
    assert_ptr_equal(_logged_context, d->context);
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

static cmsHPROFILE _create_lut_profile(void)
{
  cmsHPROFILE profile = cmsCreateProfilePlaceholder(NULL);
  assert_non_null(profile);
  cmsSetProfileVersion(profile, 2.4);
  cmsSetDeviceClass(profile, cmsSigDisplayClass);
  cmsSetColorSpace(profile, cmsSigRgbData);
  cmsSetPCS(profile, cmsSigLabData);
  assert_true(cmsWriteTag(profile, cmsSigMediaWhitePointTag, cmsD50_XYZ()));

  // identity LUTs exercise a valid LCMS output profile without matrix metadata
  cmsUInt16Number table[24];
  for(int k = 0; k < 8; k++)
    for(int c = 0; c < 3; c++)
      table[3 * k + c] = (k & (1 << (2 - c))) ? 65535 : 0;
  cmsPipeline *lut = cmsPipelineAlloc(NULL, 3, 3);
  assert_non_null(lut);
  assert_true(cmsPipelineInsertStage(lut, cmsAT_END, cmsStageAllocCLut16bit(NULL, 2, 3, 3, table)));
  assert_true(cmsWriteTag(profile, cmsSigAToB0Tag, lut));
  assert_true(cmsWriteTag(profile, cmsSigBToA0Tag, lut));
  cmsPipelineFree(lut);
  return profile;
}

static void _test_output_encoding(void **state)
{
  dt_colorspaces_t profiles = { 0 };
  dt_conf_t conf = { 0 };
  dt_develop_t dev = { 0 };
  darktable.color_profiles = &profiles;
  darktable.conf = &conf;
  darktable.num_openmp_threads = 1;
  pthread_rwlock_init(&profiles.xprofile_lock, NULL);
  dt_pthread_mutex_init(&conf.mutex);
  conf.table = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  conf.override_entries = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  dt_conf_set_bool("plugins/lighttable/export/force_lcms2", FALSE);

  dt_colorspaces_color_profile_t entries[] = {
    { .type = DT_COLORSPACE_SRGB, .profile = cmsCreate_sRGBProfile() },
    { .type = DT_COLORSPACE_LAB, .profile = cmsCreateLab4Profile(NULL) },
    { .type = DT_COLORSPACE_DISPLAY, .profile = _create_lut_profile() },
    { .type = DT_COLORSPACE_DISPLAY2, .profile = cmsCreateProfilePlaceholder(NULL) },
    { .type = DT_COLORSPACE_DISPLAY_TRANSPORT, .profile = dt_display_transport_create_profile() }
  };
  for(int k = 0; k < G_N_ELEMENTS(entries); k++)
    profiles.profiles = g_list_append(profiles.profiles, &entries[k]);

  dt_iop_module_t module = { .dev = &dev };
  dt_iop_colorout_params_t params = { .type = DT_COLORSPACE_SRGB,
                                    .intent = DT_INTENT_RELATIVE_COLORIMETRIC };
  dt_dev_pixelpipe_t pipe = { .type = DT_DEV_PIXELPIPE_FULL };
  dt_dev_pixelpipe_iop_t piece = { .colors = 4, .pipe = &pipe };
  init_pipe(&module, &pipe, &piece);
  dt_iop_colorout_data_t *d = piece.data;
  const dt_iop_roi_t roi = { .width = 1, .height = 1, .scale = 1.0f };
  const dt_aligned_pixel_t input = { 50.0f, 10.0f, -15.0f, 1.0f };
  dt_hash_t lut_hash = DT_INVALID_HASH;
  const dt_colorspaces_color_profile_type_t selected[] = {
    DT_COLORSPACE_DISPLAY, DT_COLORSPACE_DISPLAY2, DT_COLORSPACE_FILE
  };
  for(int k = 0; k < G_N_ELEMENTS(selected); k++)
  {
    profiles.display_type = selected[k];
    g_strlcpy(profiles.display_filename, k == 2 ? "missing-output.icc" : "",
              sizeof(profiles.display_filename));
    profiles.display_intent = DT_INTENT_RELATIVE_COLORIMETRIC;
    commit_params(&module, (dt_iop_params_t *)&params, &pipe, &piece);
    const dt_iop_order_iccprofile_info_t *encoding = dt_ioppr_get_pipe_output_encoding(&pipe);
    assert_non_null(encoding);
    assert_int_equal(encoding->type, k == 0 ? DT_COLORSPACE_DISPLAY : DT_COLORSPACE_SRGB);
    assert_string_equal(encoding->filename, "");
    assert_int_equal(d->type, encoding->type);
    dt_ioppr_set_pipe_work_profile_info(&dev, &pipe, DT_COLORSPACE_SRGB, "", params.intent);
    assert_ptr_equal(dt_ioppr_get_pipe_output_encoding(&pipe), encoding);
    assert_int_equal(dt_ioppr_get_pipe_output_profile_info(&pipe)->type, DT_COLORSPACE_SRGB);
    if(k == 0)
    {
      assert_non_null(d->xform);
      assert_false(dt_is_valid_colormatrix(encoding->matrix_in[0][0]));
      lut_hash = dt_dev_pixelpipe_cache_hash(NULL, &pipe, 0);
    }
    else
      assert_int_not_equal(dt_dev_pixelpipe_cache_hash(NULL, &pipe, 0), lut_hash);

    dt_aligned_pixel_t actual, expected;
    process(&module, &piece, input, actual, &roi, &roi);
    cmsHTRANSFORM reference = cmsCreateTransform
      (entries[1].profile, TYPE_LabA_FLT, entries[k == 0 ? 2 : 0].profile, TYPE_RGBA_FLT,
       INTENT_RELATIVE_COLORIMETRIC, 0);
    assert_non_null(reference);
    cmsDoTransform(reference, input, expected, 1);
    for(int c = 0; c < 3; c++)
      assert_float_equal(actual[c], expected[c], 2e-4f);
    cmsDeleteTransform(reference);
    if(k == 0)
    {
      dt_iop_colorspace_type_t converted;
      dt_aligned_pixel_t picked;
      dt_ioppr_transform_image_colorspace(&module, actual, picked, 1, 1,
                                           IOP_CS_RGB, IOP_CS_LAB, &converted, encoding);
      assert_int_equal(converted, IOP_CS_LAB);
      reference = cmsCreateTransform(entries[2].profile, TYPE_RGBA_FLT,
                                     entries[1].profile, TYPE_LabA_FLT,
                                     INTENT_RELATIVE_COLORIMETRIC, 0);
      assert_non_null(reference);
      cmsDoTransform(reference, actual, expected, 1);
      for(int c = 0; c < 3; c++)
        assert_float_equal(picked[c], expected[c], 1e-4f);
      cmsDeleteTransform(reference);
    }
  }

  // overlay sources use their host's committed output, even in a FULL temporary pipe
  pipe.type = DT_DEV_PIXELPIPE_FULL | DT_DEV_PIXELPIPE_IMAGE | DT_DEV_PIXELPIPE_IMAGE_FINAL;
  dt_dev_image_output_t output = { .intent = DT_INTENT_ABSOLUTE_COLORIMETRIC,
                                  .proof_type = DT_COLORSPACE_NONE };
  pipe.image_output = &output;
  profiles.mode = DT_PROFILE_GAMUTCHECK;
  const int destinations[] = { 4, 0, 2, -1, 3 };
  for(int k = 0; k < G_N_ELEMENTS(destinations); k++)
  {
    const int index = destinations[k];
    output.type = index < 0 ? DT_COLORSPACE_FILE : entries[index].type;
    g_strlcpy(output.filename, index < 0 ? "missing-output.icc" : "", sizeof(output.filename));
    commit_params(&module, (dt_iop_params_t *)&params, &pipe, &piece);
    assert_int_equal(d->mode, DT_PROFILE_NORMAL);
    assert_int_equal(pipe.output_intent, output.intent);
    assert_int_equal(pipe.output_proof_mode, DT_PROFILE_NORMAL);
    const int actual_index = index < 0 || index == 3 ? 0 : index;
    assert_int_equal(pipe.output_encoding->type, entries[actual_index].type);
    dt_aligned_pixel_t actual, expected;
    process(&module, &piece, input, actual, &roi, &roi);
    cmsHTRANSFORM reference = cmsCreateTransform
      (entries[1].profile, TYPE_LabA_FLT, entries[actual_index].profile, TYPE_RGBA_FLT,
       output.intent, 0);
    assert_non_null(reference);
    cmsDoTransform(reference, input, expected, 1);
    for(int c = 0; c < 3; c++)
      assert_float_equal(actual[c], expected[c], 2e-4f);
    cmsDeleteTransform(reference);
  }

  output.type = DT_COLORSPACE_SRGB;
  output.filename[0] = '\0';
  output.mode = DT_PROFILE_GAMUTCHECK;
  output.proof_type = DT_COLORSPACE_FILE;
  g_strlcpy(output.proof_filename, "missing-proof.icc", sizeof(output.proof_filename));
  profiles.mode = DT_PROFILE_NORMAL;
  commit_params(&module, (dt_iop_params_t *)&params, &pipe, &piece);
  assert_int_equal(d->mode, DT_PROFILE_GAMUTCHECK);
  assert_int_equal(pipe.output_proof_mode, DT_PROFILE_GAMUTCHECK);
  assert_int_equal(pipe.output_proof_type, DT_COLORSPACE_SRGB);
  assert_string_equal(pipe.output_proof_filename, "");
  const dt_aligned_pixel_t outside = { 50.0f, 100.0f, 100.0f, 1.0f };
  dt_aligned_pixel_t actual, cyan;
  process(&module, &piece, outside, actual, &roi, &roi);
  _gamut_warning_color(d, cyan);
  for(int c = 0; c < 3; c++)
    assert_float_equal(actual[c], cyan[c], 2e-5f);

  cleanup_pipe(&module, &pipe, &piece);
  for(GList *iter = dev.allprofile_info; iter; iter = g_list_next(iter))
  {
    dt_ioppr_cleanup_profile_info(iter->data);
    dt_free_align(iter->data);
  }
  g_list_free(dev.allprofile_info);
  g_list_free(profiles.profiles);
  for(int k = 0; k < G_N_ELEMENTS(entries); k++)
    cmsCloseProfile(entries[k].profile);
  pthread_rwlock_destroy(&profiles.xprofile_lock);
  g_hash_table_destroy(conf.table);
  g_hash_table_destroy(conf.override_entries);
  dt_pthread_mutex_destroy(&conf.mutex);
  darktable.conf = NULL;
  darktable.color_profiles = NULL;
}

int main(int argc, char *argv[])
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(_test_gamut_warning),
    cmocka_unit_test(_test_output_encoding)
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
