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

#include "common/iop_profile.h"
#include "common/opencl.h"
#include "develop/develop.h"
#include <glib/gstdio.h>

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

static int _device;
static dt_develop_t _dev;
static dt_iop_module_t _module = { .op = "xyz test" };

static void _check(const dt_iop_colorspace_type_t from, const dt_iop_colorspace_type_t to,
                    const dt_iop_order_iccprofile_info_t *profile)
{
  DT_ALIGNED_PIXEL float input[6][4] = {
    { 0.f, 0.f, 0.f, 0.125f }, { 0.18f, 0.3f, 0.7f, 0.25f },
    { 1.f, 1.f, 1.f, 0.5f }, { -0.01f, 0.02f, 0.03f, 0.625f },
    { 2.f, 1.f, 0.5f, 0.75f }, { 0.9f, 0.05f, 0.3f, 1.f }
  };
  if(from == IOP_CS_RGB && profile && dt_colorspaces_hdr_transfer(profile->type))
  {
    input[3][0] = 0.01f;
    input[4][0] = 0.95f;
  }
  DT_ALIGNED_PIXEL float cpu[6][4], gpu[6][4], unchanged[6][4];
  dt_iop_colorspace_type_t converted = from;
  dt_ioppr_transform_image_colorspace(&_module, input[0], cpu[0], 3, 2,
                                      from, to, &converted, profile);
  assert_int_equal(converted, to);
  cl_mem src = dt_opencl_copy_host_to_image(_device, (void *)input, 3, 2, 4 * sizeof(float));
  cl_mem dst = dt_opencl_alloc_device(_device, 3, 2, 4 * sizeof(float));
  assert_non_null(src);
  assert_non_null(dst);
  converted = from;
  assert_true(dt_ioppr_transform_image_colorspace_cl(&_module, _device, src, dst,
                                                     3, 2, from, to, &converted, profile));
  assert_int_equal(converted, to);
  assert_int_equal(dt_opencl_copy_image_to_host(_device, gpu, dst, 3, 2, 4 * sizeof(float)), CL_SUCCESS);
  assert_int_equal(dt_opencl_copy_image_to_host(_device, unchanged, src, 3, 2, 4 * sizeof(float)), CL_SUCCESS);
  assert_memory_equal(unchanged, input, sizeof(input));
  for(int k = 0; k < 6; k++)
  {
    for(int c = 0; c < 3; c++)
      assert_float_equal(gpu[k][c], cpu[k][c], 5e-4f * MAX(1.f, fabsf(cpu[k][c])));
    assert_memory_equal(gpu[k] + 3, input[k] + 3, sizeof(float));
  }

  // the pipeline uses replacement images for in-place OpenCL conversion
  assert_true(dt_ioppr_transform_image_colorspace_replace_cl(&_module, _device, &src,
                                                             3, 2, from, to, &converted, profile));
  assert_int_equal(dt_opencl_copy_image_to_host(_device, gpu, src, 3, 2, 4 * sizeof(float)), CL_SUCCESS);
  for(int k = 0; k < 6; k++)
  {
    for(int c = 0; c < 3; c++)
      assert_float_equal(gpu[k][c], cpu[k][c], 5e-4f * MAX(1.f, fabsf(cpu[k][c])));
    assert_memory_equal(gpu[k] + 3, input[k] + 3, sizeof(float));
  }
  dt_opencl_release_mem_object(src);
  dt_opencl_release_mem_object(dst);
}

static void _lab_xyz(void **state)
{
  _check(IOP_CS_XYZ, IOP_CS_LAB, NULL);
  _check(IOP_CS_LAB, IOP_CS_XYZ, NULL);
  _check(IOP_CS_XYZ, IOP_CS_XYZ, NULL);
}

static void _rgb_xyz(void **state)
{
  const dt_colorspaces_color_profile_type_t types[] = {
    DT_COLORSPACE_LIN_REC2020, DT_COLORSPACE_SRGB,
    DT_COLORSPACE_PQ_REC2020, DT_COLORSPACE_HLG_REC2020
  };
  for(size_t i = 0; i < G_N_ELEMENTS(types); i++)
  {
    const dt_iop_order_iccprofile_info_t *p = dt_ioppr_add_profile_info_to_list(
      &_dev, types[i], "", DT_INTENT_RELATIVE_COLORIMETRIC);
    assert_non_null(p);
    _check(IOP_CS_RGB, IOP_CS_XYZ, p);
    _check(IOP_CS_XYZ, IOP_CS_RGB, p);
  }
}

static void _lcms_fallback(void **state)
{
  const dt_iop_order_iccprofile_info_t *p = dt_ioppr_add_profile_info_to_list(
    &_dev, DT_COLORSPACE_SRGB, "", DT_INTENT_RELATIVE_COLORIMETRIC);
  assert_non_null(p);
  dt_iop_order_iccprofile_info_t fallback = *p;
  dt_mark_colormatrix_invalid(&fallback.matrix_in[0][0]);
  dt_mark_colormatrix_invalid(&fallback.matrix_out[0][0]);
  _check(IOP_CS_RGB, IOP_CS_XYZ, &fallback);
  _check(IOP_CS_XYZ, IOP_CS_RGB, &fallback);
}

static void _remove_directory(const char *path)
{
  GDir *dir = g_dir_open(path, 0, NULL);
  if(!dir) return;
  const char *name;
  while((name = g_dir_read_name(dir)))
  {
    char *child = g_build_filename(path, name, NULL);
    if(g_file_test(child, G_FILE_TEST_IS_DIR)) _remove_directory(child);
    else g_unlink(child);
    g_free(child);
  }
  g_dir_close(dir);
  g_rmdir(path);
}

int main(void)
{
  char *tmpdir = g_dir_make_tmp("darktable-xyz-opencl-XXXXXX", NULL);
  if(!tmpdir) return 1;
  char *argv[] = { "test_iop_xyz_opencl", "--library", ":memory:",
    "--configdir", tmpdir, "--cachedir", tmpdir,
    "--datadir", XYZ_TEST_DATADIR, "--moduledir", XYZ_TEST_MODULEDIR,
    "--conf", "opencl=TRUE", "--conf", "clplatform_other=TRUE", "-t", "2", NULL };
  int result = dt_init(G_N_ELEMENTS(argv) - 1, argv, FALSE, TRUE, NULL);
  if(result == 0)
  {
    _device = dt_opencl_lock_device(DT_DEV_PIXELPIPE_FULL);
    if(_device == DT_DEVICE_CPU)
    {
      fprintf(stderr, "no usable OpenCL device, skipping XYZ backend comparisons\n");
      result = 77;
    }
    else
    {
      const struct CMUnitTest tests[] = {
        cmocka_unit_test(_lab_xyz),
        cmocka_unit_test(_rgb_xyz),
        cmocka_unit_test(_lcms_fallback),
      };
      result = cmocka_run_group_tests(tests, NULL, NULL);
      for(GList *l = _dev.allprofile_info; l; l = l->next)
      {
        dt_ioppr_cleanup_profile_info(l->data);
        dt_free_align(l->data);
      }
      g_list_free(_dev.allprofile_info);
      dt_opencl_unlock_device(_device);
    }
    dt_cleanup();
  }
  _remove_directory(tmpdir);
  g_free(tmpdir);
  return result;
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
