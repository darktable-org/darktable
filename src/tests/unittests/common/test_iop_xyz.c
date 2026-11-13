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

#include "common/color_picker.h"
#include "common/histogram.h"
#include "common/iop_profile.h"
#include "develop/blend.h"
#include "develop/develop.h"
#include "control/conf.h"

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

static dt_colorspaces_t _registry;
static dt_develop_t _dev;
static dt_iop_module_t _module = { .op = "xyz test" };

static void _register_profile(const dt_colorspaces_color_profile_type_t type,
                               cmsHPROFILE profile)
{
  assert_non_null(profile);
  dt_colorspaces_color_profile_t *p = calloc(1, sizeof(*p));
  p->type = type;
  p->profile = profile;
  _registry.profiles = g_list_append(_registry.profiles, p);
}

static int _setup(void **state)
{
  darktable.color_profiles = &_registry;
  const cmsCIExyY white = { 0.3127, 0.3290, 1.0 };
  const cmsCIExyYTRIPLE primaries = {
    { 0.708, 0.292, 1.0 }, { 0.170, 0.797, 1.0 }, { 0.131, 0.046, 1.0 }
  };
  cmsToneCurve *linear = cmsBuildGamma(NULL, 1.0);
  cmsToneCurve *curves[3] = { linear, linear, linear };
  _register_profile(DT_COLORSPACE_LIN_REC2020, cmsCreateRGBProfile(&white, &primaries, curves));
  _register_profile(DT_COLORSPACE_PQ_REC2020, cmsCreateRGBProfile(&white, &primaries, curves));
  _register_profile(DT_COLORSPACE_HLG_REC2020, cmsCreateRGBProfile(&white, &primaries, curves));
  cmsFreeToneCurve(linear);
  _register_profile(DT_COLORSPACE_SRGB, cmsCreate_sRGBProfile());
  _register_profile(DT_COLORSPACE_LAB, cmsCreateLab4Profile(NULL));
  _register_profile(DT_COLORSPACE_XYZ, cmsCreateXYZProfile());
  return 0;
}

static int _cleanup(void **state)
{
  for(GList *l = _dev.allprofile_info; l; l = l->next)
  {
    dt_ioppr_cleanup_profile_info(l->data);
    dt_free_align(l->data);
  }
  g_list_free(_dev.allprofile_info);
  for(GList *l = _registry.profiles; l; l = l->next)
  {
    dt_colorspaces_color_profile_t *p = l->data;
    cmsCloseProfile(p->profile);
    free(p);
  }
  g_list_free(_registry.profiles);
  darktable.color_profiles = NULL;
  return 0;
}

static dt_iop_order_iccprofile_info_t *_profile(const dt_colorspaces_color_profile_type_t type)
{
  dt_iop_order_iccprofile_info_t *p = dt_ioppr_add_profile_info_to_list(
    &_dev, type, "", DT_INTENT_RELATIVE_COLORIMETRIC);
  assert_non_null(p);
  return p;
}

static void _convert(const float *in, float *out, const int count,
                      const dt_iop_colorspace_type_t from, const dt_iop_colorspace_type_t to,
                      const dt_iop_order_iccprofile_info_t *profile)
{
  dt_iop_colorspace_type_t converted = from;
  dt_ioppr_transform_image_colorspace(&_module, in, out, count, 1, from, to, &converted, profile);
  assert_int_equal(converted, to);
}

static void _assert_pixels(const float *actual, const float *expected,
                            const int count, const float tolerance)
{
  for(int k = 0; k < count; k++)
  {
    for(int c = 0; c < 3; c++)
      assert_float_equal(actual[4*k+c], expected[4*k+c],
                           tolerance * MAX(1.f, fabsf(expected[4*k+c])));
    assert_memory_equal(actual + 4*k+3, expected + 4*k+3, sizeof(float));
  }
}

static void _lab_xyz_without_profile(void **state)
{
  const DT_ALIGNED_PIXEL float xyz[4][4] = {
    { 0.f, 0.f, 0.f, 0.25f },
    { 0.9642f, 1.f, 0.8249f, 0.5f },
    { -0.01f, 0.002f, 0.1f, 0.75f },
    { 3.8568f, 4.f, 3.2996f, 1.f }
  };
  DT_ALIGNED_PIXEL float lab[4][4], restored[4][4];
  _convert(xyz[0], lab[0], 4, IOP_CS_XYZ, IOP_CS_LAB, NULL);
  assert_float_equal(lab[0][0], 0.f, 1e-5f);
  assert_float_equal(lab[1][0], 100.f, 1e-4f);
  assert_float_equal(lab[1][1], 0.f, 1e-4f);
  assert_float_equal(lab[1][2], 0.f, 1e-4f);
  _convert(lab[0], restored[0], 4, IOP_CS_LAB, IOP_CS_XYZ, NULL);
  _assert_pixels(restored[0], xyz[0], 4, 2e-5f);
  _convert(lab[0], lab[0], 4, IOP_CS_LAB, IOP_CS_XYZ, NULL);
  _assert_pixels(lab[0], xyz[0], 4, 2e-5f);
  _convert(lab[0], lab[0], 4, IOP_CS_XYZ, IOP_CS_LAB, NULL);
  _convert(lab[0], lab[0], 4, IOP_CS_LAB, IOP_CS_XYZ, NULL);
  _assert_pixels(lab[0], xyz[0], 4, 2e-5f);
}

static void _linear_rgb_xyz_unbounded(void **state)
{
  const DT_ALIGNED_PIXEL float rgb[4][4] = {
    { 1.f, 1.f, 1.f, 0.125f }, { 0.f, 0.f, 0.f, 0.25f },
    { -0.1f, 0.4f, 1.2f, 0.5f }, { 4.f, 2.f, 8.f, 0.75f }
  };
  DT_ALIGNED_PIXEL float xyz[4][4], restored[4][4];
  const dt_iop_order_iccprofile_info_t *profile = _profile(DT_COLORSPACE_LIN_REC2020);
  _convert(rgb[0], xyz[0], 4, IOP_CS_RGB, IOP_CS_XYZ, profile);
  // the ICC chromatic adaptation produces the D50 reference white, not D65
  assert_float_equal(xyz[0][0], 0.9642f, 2e-4f);
  assert_float_equal(xyz[0][1], 1.f, 2e-4f);
  assert_float_equal(xyz[0][2], 0.8249f, 2e-4f);
  _convert(xyz[0], restored[0], 4, IOP_CS_XYZ, IOP_CS_RGB, profile);
  _assert_pixels(restored[0], rgb[0], 4, 2e-5f);
  _convert(xyz[0], xyz[0], 4, IOP_CS_XYZ, IOP_CS_RGB, profile);
  _assert_pixels(xyz[0], rgb[0], 4, 2e-5f);
}

static void _matrix_and_lcms_agree(void **state)
{
  const DT_ALIGNED_PIXEL float rgb[4][4] = {
    { 0.f, 0.f, 0.f, 0.25f }, { 1.f, 1.f, 1.f, 0.5f },
    { 0.04f, 0.3f, 0.9f, 0.75f }, { 0.7f, 0.2f, 0.02f, 1.f }
  };
  DT_ALIGNED_PIXEL float matrix[4][4], lcms[4][4], restored[4][4];
  const dt_iop_order_iccprofile_info_t *profile = _profile(DT_COLORSPACE_SRGB);
  dt_iop_order_iccprofile_info_t fallback = *profile;
  dt_mark_colormatrix_invalid(&fallback.matrix_in[0][0]);
  dt_mark_colormatrix_invalid(&fallback.matrix_out[0][0]);
  _convert(rgb[0], matrix[0], 4, IOP_CS_RGB, IOP_CS_XYZ, profile);
  _convert(rgb[0], lcms[0], 4, IOP_CS_RGB, IOP_CS_XYZ, &fallback);
  _assert_pixels(lcms[0], matrix[0], 4, 3e-4f);
  _convert(lcms[0], restored[0], 4, IOP_CS_XYZ, IOP_CS_RGB, &fallback);
  _assert_pixels(restored[0], rgb[0], 4, 3e-4f);
  _convert(matrix[0], matrix[0], 4, IOP_CS_XYZ, IOP_CS_RGB, profile);
  _assert_pixels(matrix[0], rgb[0], 4, 3e-4f);
}

static void _hdr_rgb_xyz(void **state)
{
  const DT_ALIGNED_PIXEL float linear[2][4] = {
    { 0.2f, 0.4f, 1.5f, 0.25f }, { 2.f, 0.01f, 0.3f, 0.75f }
  };
  DT_ALIGNED_PIXEL float expected[2][4], encoded[2][4], xyz[2][4], restored[2][4];
  _convert(linear[0], expected[0], 2, IOP_CS_RGB, IOP_CS_XYZ, _profile(DT_COLORSPACE_LIN_REC2020));
  const dt_colorspaces_color_profile_type_t types[] = { DT_COLORSPACE_PQ_REC2020, DT_COLORSPACE_HLG_REC2020 };
  for(size_t i = 0; i < G_N_ELEMENTS(types); i++)
  {
    const dt_iop_order_iccprofile_info_t *p = _profile(types[i]);
    memcpy(encoded, linear, sizeof(linear));
    for(int k = 0; k < 2; k++) dt_hdr_encode(encoded[k], dt_colorspaces_hdr_transfer(types[i]));
    _convert(encoded[0], xyz[0], 2, IOP_CS_RGB, IOP_CS_XYZ, p);
    _assert_pixels(xyz[0], expected[0], 2, 3e-4f);
    _convert(xyz[0], restored[0], 2, IOP_CS_XYZ, IOP_CS_RGB, p);
    _assert_pixels(restored[0], encoded[0], 2, 3e-4f);
  }
}

static void _conversion_chain(void **state)
{
  const DT_ALIGNED_PIXEL float rgb[2][4] = { { 0.3f, 0.6f, 0.9f, 0.5f }, { 2.f, 1.f, 0.5f, 1.f } };
  DT_ALIGNED_PIXEL float via_xyz[2][4], direct[2][4];
  const dt_iop_order_iccprofile_info_t *profile = _profile(DT_COLORSPACE_LIN_REC2020);
  _convert(rgb[0], direct[0], 2, IOP_CS_RGB, IOP_CS_LAB, profile);
  _convert(rgb[0], via_xyz[0], 2, IOP_CS_RGB, IOP_CS_XYZ, profile);
  _convert(via_xyz[0], via_xyz[0], 2, IOP_CS_XYZ, IOP_CS_LAB, NULL);
  _assert_pixels(via_xyz[0], direct[0], 2, 2e-5f);
  _convert(via_xyz[0], via_xyz[0], 2, IOP_CS_LAB, IOP_CS_XYZ, NULL);
  _convert(via_xyz[0], via_xyz[0], 2, IOP_CS_XYZ, IOP_CS_RGB, profile);
  _assert_pixels(via_xyz[0], rgb[0], 2, 2e-5f);
}

static void _xyz_identity(void **state)
{
  const DT_ALIGNED_PIXEL float xyz[2][4] = { { -1.f, 2.f, 3.f, 0.25f }, { 0.f, 0.1f, 0.2f, 1.f } };
  DT_ALIGNED_PIXEL float out[2][4];
  _convert(xyz[0], out[0], 2, IOP_CS_XYZ, IOP_CS_XYZ, NULL);
  assert_memory_equal(xyz, out, sizeof(xyz));
}

static void _xyz_histogram(void **state)
{
  const DT_ALIGNED_PIXEL float xyz[2][4] = { { 0.25f, 0.5f, 0.75f, 0.f }, { 1.f, 2.f, -1.f, 1.f } };
  dt_histogram_roi_t roi = { .width = 2, .height = 1 };
  dt_dev_histogram_collection_params_t params = { .roi = &roi, .bins_count = 5 };
  dt_dev_histogram_stats_t stats = { 0 };
  uint32_t *hist = NULL;
  DT_ALIGNED_PIXEL uint32_t max[4];
  dt_histogram_helper(&params, &stats, IOP_CS_XYZ, IOP_CS_XYZ, xyz, &hist, max, TRUE,
                       _profile(DT_COLORSPACE_SRGB));
  assert_int_equal(stats.ch, 3);
  assert_int_equal(stats.pixels, 2);
  assert_int_equal(hist[4*1+0], 1);
  assert_int_equal(hist[4*2+1], 1);
  assert_int_equal(hist[4*3+2], 1);
  assert_int_equal(hist[4*4+0], 1);
  assert_int_equal(hist[4*4+1], 1);
  assert_int_equal(hist[4*0+2], 1);
  dt_free_align(hist);
}

static void _xyz_color_picker(void **state)
{
  const DT_ALIGNED_PIXEL float rgb[4][4] = {
    { 0.4f, 0.2f, 0.7f, 1.f }, { 0.f, 0.f, 0.f, 1.f },
    { 1.f, 1.f, 1.f, 1.f }, { 0.1f, 0.9f, 0.2f, 1.f }
  };
  DT_ALIGNED_PIXEL float xyz[4][4];
  const dt_iop_order_iccprofile_info_t *profile = _profile(DT_COLORSPACE_SRGB);
  _convert(rgb[0], xyz[0], 4, IOP_CS_RGB, IOP_CS_XYZ, profile);
  const dt_iop_buffer_dsc_t dsc = { .channels = 4, .cst = IOP_CS_RGB };
  const dt_iop_roi_t roi = { .width = 4, .height = 1, .scale = 1.f };
  const int box[4] = { 1, 0, 3, 1 };
  lib_colorpicker_stats pick;
  dt_color_picker_helper(&_module, &dsc, rgb[0], &roi, box, FALSE,
                          pick, IOP_CS_RGB, IOP_CS_XYZ, profile);
  for(int c = 0; c < 3; c++)
    assert_float_equal(pick[DT_PICK_MEAN][c], xyz[2][c] / 2.f, 2e-5f);
  dt_color_picker_helper(&_module, &dsc, xyz[0], &roi, box, FALSE,
                          pick, IOP_CS_XYZ, IOP_CS_RGB, profile);
  for(int c = 0; c < 3; c++) assert_float_equal(pick[DT_PICK_MEAN][c], 0.5f, 3e-4f);
  dt_color_picker_helper(&_module, &dsc, xyz[0], &roi, box, FALSE,
                          pick, IOP_CS_XYZ, IOP_CS_LCH, NULL);
  assert_float_equal(pick[DT_PICK_MEAN][0], 50.f, 2e-3f);
}

static int _blend_flags(void)
{
  return IOP_FLAGS_SUPPORTS_BLENDING;
}

static dt_iop_colorspace_type_t _blend_cst(dt_iop_module_t *self,
                                           dt_dev_pixelpipe_t *pipe,
                                           dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_XYZ;
}

static void _xyz_blending(void **state)
{
  dt_conf_t conf = { 0 };
  dt_pthread_mutex_init(&conf.mutex);
  conf.table = g_hash_table_new(g_str_hash, g_str_equal);
  conf.override_entries = g_hash_table_new(g_str_hash, g_str_equal);
  darktable.conf = &conf;
  dt_iop_module_t module = { .flags = _blend_flags, .blend_colorspace = _blend_cst };
  g_hash_table_insert(conf.table, "plugins/darkroom/workflow", "scene-referred (sigmoid)");
  assert_int_equal(dt_develop_blend_default_module_blend_colorspace(&module),
                     DEVELOP_BLEND_CS_RGB_SCENE);
  g_hash_table_insert(conf.table, "plugins/darkroom/workflow", "display-referred (legacy)");
  assert_int_equal(dt_develop_blend_default_module_blend_colorspace(&module),
                     DEVELOP_BLEND_CS_RGB_DISPLAY);
  dt_develop_blend_params_t params;
  dt_develop_blend_init_blend_parameters(&params, DEVELOP_BLEND_CS_RGB_SCENE);
  const dt_dev_pixelpipe_iop_t piece = { .blendop_data = &params };
  assert_int_equal(dt_develop_blend_colorspace(&piece, IOP_CS_XYZ), IOP_CS_RGB);
  darktable.conf = NULL;
  g_hash_table_destroy(conf.table);
  g_hash_table_destroy(conf.override_entries);
  dt_pthread_mutex_destroy(&conf.mutex);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(_lab_xyz_without_profile),
    cmocka_unit_test(_linear_rgb_xyz_unbounded),
    cmocka_unit_test(_matrix_and_lcms_agree),
    cmocka_unit_test(_hdr_rgb_xyz),
    cmocka_unit_test(_conversion_chain),
    cmocka_unit_test(_xyz_identity),
    cmocka_unit_test(_xyz_histogram),
    cmocka_unit_test(_xyz_color_picker),
    cmocka_unit_test(_xyz_blending),
  };
  return cmocka_run_group_tests(tests, _setup, _cleanup);
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
