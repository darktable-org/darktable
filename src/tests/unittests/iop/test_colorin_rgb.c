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
#include <glib/gstdio.h>

#define dt_ioppr_set_pipe_input_profile_info test_set_input_profile_info
#include "iop/colorin.c"
#undef dt_ioppr_set_pipe_input_profile_info

// committing input metadata normally accesses the image cache; these fixtures
// exercise color processing without creating catalog images
dt_iop_order_iccprofile_info_t *test_set_input_profile_info
  (dt_develop_t *dev, dt_dev_pixelpipe_t *pipe,
   const dt_colorspaces_color_profile_type_t type, const char *filename,
   const int intent, const dt_colormatrix_t matrix)
{
  return dt_ioppr_add_profile_info_to_list(dev, type, filename, intent);
}

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

static dt_colorspaces_t registry;
static dt_develop_t dev;
static dt_iop_module_t module = { .op = "colorin", .dev = &dev };
static int device = DT_DEVICE_CPU;

static const dt_aligned_pixel_t pixels[] = {
  { 0.0f, 0.0f, 0.0f, 0.0f }, { 0.18f, 0.18f, 0.18f, 0.25f },
  { 0.7f, 0.3f, 0.9f, 0.5f }, { 2.5f, 1.2f, 0.1f, 1.0f },
  { -0.1f, 0.5f, 0.2f, 0.75f }, { 0.1f, 0.0f, 0.9f, 0.125f }
};
static const dt_iop_roi_t roi = { .width = 3, .height = 2, .scale = 1.0f };

static void register_profile(const dt_colorspaces_color_profile_type_t type, cmsHPROFILE profile)
{
  assert_non_null(profile);
  dt_colorspaces_color_profile_t *p = calloc(1, sizeof(*p));
  p->type = type;
  p->profile = profile;
  p->work_pos = 0;
  registry.profiles = g_list_append(registry.profiles, p);
}

static cmsHPROFILE rgb_profile(const gboolean rec2020, const double gamma)
{
  const cmsCIExyY white = { 0.3127, 0.3290, 1.0 };
  const cmsCIExyYTRIPLE wide = {
    { 0.708, 0.292, 1.0 }, { 0.170, 0.797, 1.0 }, { 0.131, 0.046, 1.0 }
  };
  const cmsCIExyYTRIPLE narrow = {
    { 0.640, 0.330, 1.0 }, { 0.300, 0.600, 1.0 }, { 0.150, 0.060, 1.0 }
  };
  cmsToneCurve *curve = cmsBuildGamma(NULL, gamma);
  cmsToneCurve *curves[3] = { curve, curve, curve };
  cmsHPROFILE profile = cmsCreateRGBProfile(&white, rec2020 ? &wide : &narrow, curves);
  cmsFreeToneCurve(curve);
  return profile;
}

static int sample_clut(const float in[], float out[], void *transform)
{
  float xyz[3];
  cmsDoTransform(transform, in, xyz, 1);
  for(int c = 0; c < 3; c++) out[c] = xyz[c] * 0.5f;
  return 1;
}

static cmsHPROFILE clut_profile(void)
{
  cmsHPROFILE rgb = cmsCreate_sRGBProfile();
  cmsHPROFILE xyz = cmsCreateXYZProfile();
  cmsHTRANSFORM transform = cmsCreateTransform(rgb, TYPE_RGB_FLT, xyz, TYPE_XYZ_FLT,
                                               INTENT_RELATIVE_COLORIMETRIC, cmsFLAGS_NOOPTIMIZE);
  assert_non_null(transform);
  cmsPipeline *pipeline = cmsPipelineAlloc(NULL, 3, 3);
  cmsStage *clut = cmsStageAllocCLutFloat(NULL, 17, 3, 3, NULL);
  assert_true(cmsStageSampleCLutFloat(clut, sample_clut, transform, 0));
  assert_true(cmsPipelineInsertStage(pipeline, cmsAT_END, clut));
  cmsHPROFILE profile = cmsCreateProfilePlaceholder(NULL);
  cmsSetProfileVersion(profile, 4.3);
  cmsSetDeviceClass(profile, cmsSigInputClass);
  cmsSetColorSpace(profile, cmsSigRgbData);
  cmsSetPCS(profile, cmsSigXYZData);
  assert_true(cmsWriteTag(profile, cmsSigMediaWhitePointTag, cmsD50_XYZ()));
  assert_true(cmsWriteTag(profile, cmsSigAToB0Tag, pipeline));
  assert_true(cmsWriteTag(profile, cmsSigAToB1Tag, pipeline));
  assert_true(cmsWriteTag(profile, cmsSigAToB2Tag, pipeline));
  cmsPipelineFree(pipeline);
  cmsDeleteTransform(transform);
  cmsCloseProfile(rgb);
  cmsCloseProfile(xyz);
  return profile;
}

static int group_setup(void **state)
{
  darktable.num_openmp_threads = 2;
  darktable.color_profiles = &registry;
  register_profile(DT_COLORSPACE_LIN_REC2020, rgb_profile(TRUE, 1.0));
  register_profile(DT_COLORSPACE_LIN_REC709, rgb_profile(FALSE, 1.0));
  register_profile(DT_COLORSPACE_SRGB, cmsCreate_sRGBProfile());
  register_profile(DT_COLORSPACE_ADOBERGB, rgb_profile(TRUE, 2.2));
  register_profile(DT_COLORSPACE_PQ_REC2020, rgb_profile(TRUE, 1.0));
  register_profile(DT_COLORSPACE_HLG_REC2020, rgb_profile(TRUE, 1.0));
  register_profile(DT_COLORSPACE_LAB, cmsCreateLab4Profile(NULL));
  register_profile(DT_COLORSPACE_XYZ, cmsCreateXYZProfile());
  register_profile(DT_COLORSPACE_FILE, clut_profile());
  dt_colorspaces_color_profile_t *clut = g_list_last(registry.profiles)->data;
  g_strlcpy(clut->filename, "/colorin-tests/colorin-test.icc", sizeof(clut->filename));
  return 0;
}

static void free_profiles(void)
{
  for(GList *l = dev.allprofile_info; l; l = l->next)
  {
    dt_ioppr_cleanup_profile_info(l->data);
    dt_free_align(l->data);
  }
  g_clear_pointer(&dev.allprofile_info, g_list_free);
}

static int group_cleanup(void **state)
{
  free_profiles();
  for(GList *l = registry.profiles; l; l = l->next)
  {
    dt_colorspaces_color_profile_t *p = l->data;
    cmsCloseProfile(p->profile);
    free(p);
  }
  g_clear_pointer(&registry.profiles, g_list_free);
  darktable.color_profiles = NULL;
  return 0;
}

static dt_iop_colorin_params_t params(void)
{
  return (dt_iop_colorin_params_t) {
    .type = DT_COLORSPACE_LIN_REC709, .type_work = DT_COLORSPACE_LIN_REC2020,
    .intent = DT_INTENT_RELATIVE_COLORIMETRIC, .output_rgb = TRUE
  };
}

static void init_piece(dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece,
                        dt_iop_colorin_params_t *p)
{
  memset(pipe, 0, sizeof(*pipe));
  pipe->type = DT_DEV_PIXELPIPE_EXPORT;
  pipe->devid = device;
  pipe->image.flags = DT_IMAGE_RAW;
  dt_dev_wb_set_neutral(&pipe->wb);
  for_four_channels(c)
    pipe->dsc.processed_maximum[c] = 1.0f;
  *piece = (dt_dev_pixelpipe_iop_t) { .pipe = pipe, .module = &module, .colors = 4 };
  init_pipe(&module, pipe, piece);
  commit_params(&module, (dt_iop_params_t *)p, pipe, piece);
}

static void assert_pixels(const float *actual, const float *expected, const float tolerance)
{
  for(size_t k = 0; k < G_N_ELEMENTS(pixels); k++)
    for_each_channel(c)
      if(!isfinite(actual[4*k+c])
         || fabsf(actual[4*k+c] - expected[4*k+c]) > tolerance * MAX(1.0f, fabsf(expected[4*k+c])))
        fail_msg("pixel %zu channel %zu: %.8f != %.8f", k, c, actual[4*k+c], expected[4*k+c]);
}

static void test_migration(void **state)
{
  // versions 1–3 used names; versions 4–5 used 100-byte file names
  struct { char name[100]; int intent, normalize, blue; } named = { .name = "sRGB" };
  struct { int type; char file[100]; int intent, normalize, blue, work; char workfile[100]; }
    short_names = { .type = DT_COLORSPACE_SRGB, .work = DT_COLORSPACE_LIN_REC2020 };
  dt_iop_colorin_params_t current = params();
  for(int version = 1; version <= 7; version++)
  {
    const void *old = version <= 3 ? (void *)&named : version <= 5 ? (void *)&short_names : (void *)&current;
    void *converted = NULL;
    int32_t size = 0;
    int next = 0;
    assert_int_equal(legacy_params(NULL, old, version, &converted, &size, &next), 0);
    if(next == 7)
    {
      void *v7 = converted;
      assert_int_equal(legacy_params(NULL, v7, 7, &converted, &size, &next), 0);
      free(v7);
    }
    assert_int_equal(next, 8);
    assert_int_equal(size, sizeof(current));
    assert_false(((dt_iop_colorin_params_t *)converted)->output_rgb);
    if(version >= 6)
      assert_memory_equal(converted, &current, offsetof(dt_iop_colorin_params_t, output_rgb));
    free(converted);
  }
}

static void compare_legacy(dt_iop_colorin_params_t *p)
{
  dt_dev_pixelpipe_t pipe;
  dt_dev_pixelpipe_iop_t piece;
  init_piece(&pipe, &piece, p);
  dt_aligned_pixel_t direct[6], lab[6], expected[6];
  process(&module, &piece, pixels, direct, &roi, &roi);
  assert_int_equal(output_colorspace(&module, &pipe, &piece), IOP_CS_RGB);
  p->output_rgb = FALSE;
  commit_params(&module, (dt_iop_params_t *)p, &pipe, &piece);
  assert_int_equal(output_colorspace(&module, &pipe, &piece), IOP_CS_LAB);
  process(&module, &piece, pixels, lab, &roi, &roi);
  const dt_iop_colorin_data_t *d = piece.data;
  const dt_iop_order_iccprofile_info_t *work = pipe.work_profile_info;
  for(int k = 0; k < 6; k++)
  {
    dt_aligned_pixel_t xyz, linear;
    dt_Lab_to_XYZ(lab[k], xyz);
    dt_apply_transposed_color_matrix(xyz, work->matrix_out_transposed, linear);
    linear[3] = pixels[k][3];
    dt_ioppr_encode_rgb(linear, expected[k], work);
  }
  assert_pixels((float *)direct, (float *)expected, 8e-4f);
  for(int k = 0; k < 6; k++) assert_float_equal(direct[k][3], pixels[k][3], 0.0f);
  assert_non_null(d);
  cleanup_pipe(&module, &pipe, &piece);
  p->output_rgb = TRUE;
}

static void test_matrix_and_clipping(void **state)
{
  dt_iop_colorin_params_t p = params();
  const dt_colorspaces_color_profile_type_t input[] = {
    DT_COLORSPACE_LIN_REC709, DT_COLORSPACE_SRGB, DT_COLORSPACE_PQ_REC2020, DT_COLORSPACE_HLG_REC2020
  };
  const dt_colorspaces_color_profile_type_t work[] = {
    DT_COLORSPACE_LIN_REC2020, DT_COLORSPACE_SRGB, DT_COLORSPACE_PQ_REC2020, DT_COLORSPACE_HLG_REC2020
  };
  for(size_t i = 0; i < G_N_ELEMENTS(input); i++)
    for(size_t j = 0; j < G_N_ELEMENTS(work); j++)
      for(int clipping = DT_NORMALIZE_OFF; clipping <= DT_NORMALIZE_LINEAR_REC2020_RGB; clipping++)
      {
        p.type = input[i];
        p.type_work = work[j];
        p.normalize = clipping;
        compare_legacy(&p);
      }
}

static void test_lcms_fallback(void **state)
{
  dt_iop_colorin_params_t p = params();
  p.type = DT_COLORSPACE_XYZ;
  compare_legacy(&p);
  p.normalize = DT_NORMALIZE_SRGB;
  compare_legacy(&p);
  p.type = DT_COLORSPACE_FILE;
  g_strlcpy(p.filename, "colorin-test.icc", sizeof(p.filename));
  const dt_colorspaces_color_profile_t *profile =
    dt_colorspaces_get_profile(p.type, p.filename, DT_PROFILE_DIRECTION_IN);
  assert_non_null(profile);
  assert_true(cmsIsCLUT(profile->profile, INTENT_RELATIVE_COLORIMETRIC, LCMS_USED_AS_INPUT));
  for(int clip = 0; clip <= DT_NORMALIZE_LINEAR_REC2020_RGB; clip++)
  {
    p.normalize = clip;
    dt_dev_pixelpipe_t pipe;
    dt_dev_pixelpipe_iop_t piece;
    init_piece(&pipe, &piece, &p);
    const dt_iop_colorin_data_t *d = piece.data;
    assert_ptr_equal(d->input, profile->profile);
    assert_false(piece.process_cl_ready);
    assert_false(dt_is_valid_colormatrix(d->cmatrix[0][0]));
    assert_non_null(d->xform_cam_out);
    cleanup_pipe(&module, &pipe, &piece);
    compare_legacy(&p);
  }
}

static void test_lab_input(void **state)
{
  dt_iop_colorin_params_t p = params();
  p.type = DT_COLORSPACE_LAB;
  dt_dev_pixelpipe_t pipe;
  dt_dev_pixelpipe_iop_t piece;
  init_piece(&pipe, &piece, &p);
  assert_true(piece.enabled);
  dt_aligned_pixel_t lab[6], actual[6];
  for(int k = 0; k < 6; k++)
  {
    dt_XYZ_to_Lab(pixels[k], lab[k]);
    lab[k][3] = pixels[k][3];
  }
  process(&module, &piece, lab, actual, &roi, &roi);
  for(int k = 0; k < 6; k++) assert_true(isfinite(actual[k][0]));
  p.output_rgb = FALSE;
  commit_params(&module, (dt_iop_params_t *)&p, &pipe, &piece);
  assert_false(piece.enabled);
  process(&module, &piece, lab, actual, &roi, &roi);
  assert_memory_equal(lab, actual, sizeof(lab));
  cleanup_pipe(&module, &pipe, &piece);
}

static void test_white_balance_and_blue_mapping(void **state)
{
  dt_iop_colorin_params_t p = params();
  p.type_work = p.type;
  dt_dev_pixelpipe_t pipe;
  dt_dev_pixelpipe_iop_t piece;
  init_piece(&pipe, &piece, &p);
  pipe.dsc.temperature.enabled = TRUE;
  pipe.wb.late_correction = TRUE;
  pipe.wb.coeffs[0] = 2.0f;
  pipe.wb.coeffs[1] = 0.5f;
  pipe.wb.D65coeffs[0] = 2.4f;
  pipe.wb.D65coeffs[1] = 0.4f;
  dt_aligned_pixel_t actual[6], expected[6];
  process(&module, &piece, pixels, actual, &roi, &roi);
  for(int k = 0; k < 6; k++)
  {
    copy_pixel(expected[k], pixels[k]);
    expected[k][0] *= 1.2f;
    expected[k][1] *= 0.8f;
  }
  assert_pixels((float *)actual, (float *)expected, 2e-5f);
  dt_dev_pixelpipe_t other_pipe;
  dt_dev_pixelpipe_iop_t other_piece;
  init_piece(&other_pipe, &other_piece, &p);
  process(&module, &other_piece, pixels, actual, &roi, &roi);
  assert_pixels((float *)actual, (float *)pixels, 2e-5f);
  process(&module, &piece, pixels, actual, &roi, &roi);
  assert_pixels((float *)actual, (float *)expected, 2e-5f);
  cleanup_pipe(&module, &other_pipe, &other_piece);
  p.blue_mapping = TRUE;
  commit_params(&module, (dt_iop_params_t *)&p, &pipe, &piece);
  process(&module, &piece, pixels, actual, &roi, &roi);
  for(int k = 0; k < 6; k++) _apply_blue_mapping(expected[k], expected[k]);
  assert_pixels((float *)actual, (float *)expected, 2e-5f);
  pipe.wb.late_correction = FALSE;
  cleanup_pipe(&module, &pipe, &piece);
}

static void test_dng_look(void **state)
{
  dt_iop_colorin_params_t p = params();
  dt_dev_pixelpipe_t pipe;
  dt_dev_pixelpipe_iop_t piece;
  init_piece(&pipe, &piece, &p);
  dt_iop_colorin_data_t *d = piece.data;
  float hsm[] = { 0.0f, 1.0f, 1.0f, 30.0f, 0.8f, 0.9f };
  d->hsm = g_malloc(sizeof(hsm));
  memcpy(d->hsm, hsm, sizeof(hsm));
  d->hue_div = d->val_div = 1;
  d->sat_div = 2;
  for(int encoding = 0; encoding <= 1; encoding++)
  {
    dt_aligned_pixel_t rgb[6], lab[6], expected[6];
    d->hsm_encode = encoding;
    process(&module, &piece, pixels, rgb, &roi, &roi);
    d->output_rgb = FALSE;
    process(&module, &piece, pixels, lab, &roi, &roi);
    for(int k = 0; k < 6; k++)
    {
      dt_aligned_pixel_t xyz, linear;
      dt_Lab_to_XYZ(lab[k], xyz);
      dt_apply_transposed_color_matrix(xyz, d->work_profile->matrix_out_transposed, linear);
      linear[3] = pixels[k][3];
      dt_ioppr_encode_rgb(linear, expected[k], d->work_profile);
    }
    assert_pixels((float *)rgb, (float *)expected, 8e-4f);
    d->output_rgb = TRUE;
  }
  cleanup_pipe(&module, &pipe, &piece);
}

#ifdef HAVE_OPENCL
static void test_opencl(void **state)
{
  dt_iop_colorin_params_t p = params();
  const dt_colorspaces_color_profile_type_t input[] = {
    DT_COLORSPACE_LIN_REC709, DT_COLORSPACE_SRGB, DT_COLORSPACE_PQ_REC2020,
    DT_COLORSPACE_HLG_REC2020, DT_COLORSPACE_LAB
  };
  for(size_t i = 0; i < G_N_ELEMENTS(input); i++)
    for(int clipping = DT_NORMALIZE_OFF; clipping <= DT_NORMALIZE_LINEAR_REC2020_RGB; clipping++)
      for(int look = 0; look <= 3; look++)
      {
        p.type = input[i];
        const dt_colorspaces_color_profile_type_t work[] = {
          DT_COLORSPACE_LIN_REC2020, DT_COLORSPACE_SRGB, DT_COLORSPACE_PQ_REC2020, DT_COLORSPACE_HLG_REC2020
        };
        p.type_work = work[look];
        p.normalize = clipping;
        p.blue_mapping = look >= 2;
        dt_dev_pixelpipe_t pipe;
        dt_dev_pixelpipe_iop_t piece;
        init_piece(&pipe, &piece, &p);
        dt_iop_colorin_data_t *d = piece.data;
        if(look && p.type != DT_COLORSPACE_LAB)
        {
          float hsm[] = { 0.0f, 1.0f, 1.0f, 30.0f, 0.8f, 0.9f };
          d->hsm = g_malloc(sizeof(hsm));
          memcpy(d->hsm, hsm, sizeof(hsm));
          d->hue_div = d->val_div = 1;
          d->sat_div = 2;
          d->hsm_encode = look % 2;
        }
        print_message("input %d, working %d, clipping %d, look %d\n", p.type, p.type_work, clipping, look);
        pipe.dsc.temperature.enabled = look >= 2;
        pipe.wb.late_correction = look >= 2;
        if(pipe.wb.late_correction)
        {
          pipe.wb.D65coeffs[0] = 1.2f;
          pipe.wb.D65coeffs[1] = 0.8f;
        }
        dt_aligned_pixel_t cpu[6], gpu[6];
        process(&module, &piece, pixels, cpu, &roi, &roi);
        for_four_channels(c) pipe.dsc.processed_maximum[c] = 1.0f;
        cl_mem src = dt_opencl_copy_host_to_image(device, (void *)pixels, 3, 2, 4 * sizeof(float));
        cl_mem dst = dt_opencl_alloc_device(device, 3, 2, 4 * sizeof(float));
        assert_non_null(src);
        assert_non_null(dst);
        assert_int_equal(process_cl(&module, &piece, src, dst, &roi, &roi), CL_SUCCESS);
        assert_int_equal(dt_opencl_copy_image_to_host(device, gpu, dst, 3, 2, 4 * sizeof(float)), CL_SUCCESS);
        assert_pixels((float *)gpu, (float *)cpu, 8e-4f);
        for(int k = 0; k < 6; k++) assert_float_equal(gpu[k][3], pixels[k][3], 0.0f);
        dt_opencl_release_mem_object(src);
        dt_opencl_release_mem_object(dst);
        cleanup_pipe(&module, &pipe, &piece);
        pipe.wb.late_correction = FALSE;
      }
}
#endif

#ifdef HAVE_OPENCL
static void remove_directory(const char *path)
{
  GDir *dir = g_dir_open(path, 0, NULL);
  if(!dir) return;
  const char *name;
  while((name = g_dir_read_name(dir)))
  {
    char *child = g_build_filename(path, name, NULL);
    if(g_file_test(child, G_FILE_TEST_IS_DIR)) remove_directory(child);
    else g_unlink(child);
    g_free(child);
  }
  g_dir_close(dir);
  g_rmdir(path);
}

#endif

int main(int argc, char *argv[])
{
#ifdef HAVE_OPENCL
  if(argc > 1 && !strcmp(argv[1], "--opencl"))
  {
    char *tmpdir = g_dir_make_tmp("darktable-colorin-XXXXXX", NULL);
    if(!tmpdir) return 1;
    char *args[] = { "test_colorin_rgb", "--library", ":memory:",
      "--configdir", tmpdir, "--cachedir", tmpdir,
      "--datadir", COLORIN_TEST_DATADIR, "--moduledir", COLORIN_TEST_MODULEDIR,
      "--conf", "opencl=TRUE", "--conf", "clplatform_other=TRUE", "-t", "2", NULL };
    int result = dt_init(G_N_ELEMENTS(args) - 1, args, FALSE, TRUE, NULL);
    if(result == 0)
    {
      device = dt_opencl_lock_device(DT_DEV_PIXELPIPE_FULL);
      if(device == DT_DEVICE_CPU) result = 77;
      else
      {
        dt_iop_module_so_t so = { 0 };
        init_global(&so);
        module.global_data = so.data;
        const struct CMUnitTest tests[] = { cmocka_unit_test(test_opencl) };
        result = cmocka_run_group_tests(tests, NULL, NULL);
        cleanup_global(&so);
        free_profiles();
        dt_opencl_unlock_device(device);
      }
      dt_cleanup();
    }
    remove_directory(tmpdir);
    g_free(tmpdir);
    return result;
  }
#endif
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_migration),
    cmocka_unit_test(test_matrix_and_clipping),
    cmocka_unit_test(test_lcms_fallback),
    cmocka_unit_test(test_lab_input),
    cmocka_unit_test(test_white_balance_and_blue_mapping),
    cmocka_unit_test(test_dng_look)
  };
  return cmocka_run_group_tests(tests, group_setup, group_cleanup);
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
