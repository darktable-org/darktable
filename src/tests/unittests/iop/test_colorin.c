/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable. If not, see <http://www.gnu.org/licenses/>.
*/

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <cmocka.h>

#define dt_image_cache_get __wrap_dt_image_cache_get
#define dt_image_cache_read_release __wrap_dt_image_cache_read_release
#define dt_bauhaus_combobox_clear __wrap_dt_bauhaus_combobox_clear
#define dt_bauhaus_combobox_add __wrap_dt_bauhaus_combobox_add
#define dt_bauhaus_combobox_get __wrap_dt_bauhaus_combobox_get
#define dt_iop_request_focus __wrap_dt_iop_request_focus
#define dt_dev_add_history_item __wrap_dt_dev_add_history_item
#define dt_control_signal_raise __wrap_dt_control_signal_raise

#include "common/iop_profile.c"
#include "iop/colorin.c"

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

static dt_image_t image;
static gboolean cache_locked;
static int selected_profile;

dt_image_t *__wrap_dt_image_cache_get(const dt_imgid_t imgid, const char mode)
{
  assert_int_equal(imgid, image.id);
  assert_int_equal(mode, 'r');
  assert_false(cache_locked);
  cache_locked = TRUE;
  return &image;
}

void __wrap_dt_image_cache_read_release(const dt_image_t *img)
{
  assert_ptr_equal(img, &image);
  assert_true(cache_locked);
  cache_locked = FALSE;
}

void __wrap_dt_bauhaus_combobox_clear(GtkWidget *widget) {}
void __wrap_dt_bauhaus_combobox_add(GtkWidget *widget, const char *text) {}
int __wrap_dt_bauhaus_combobox_get(GtkWidget *widget) { return selected_profile; }
void __wrap_dt_iop_request_focus(dt_iop_module_t *module) {}
void __wrap_dt_control_signal_raise(const struct dt_control_signal_t *signals,
                                    const dt_signal_t signal, ...) {}

void __wrap_dt_dev_add_history_item(dt_develop_t *dev, dt_iop_module_t *module,
                                    const gboolean enable)
{
  assert_ptr_equal(dev, module->dev);
  assert_true(enable);
  dev->full.pipe->changed |= DT_DEV_PIPE_TOP_CHANGED;
  dev->preview_pipe->changed |= DT_DEV_PIPE_TOP_CHANGED;
  dev->preview2.pipe->changed |= DT_DEV_PIPE_TOP_CHANGED;
}

static void test_dng_look_profile_list(void **state)
{
  dt_colorspaces_t profiles = { 0 };
  darktable.color_profiles = &profiles;
  dt_develop_t dev = { 0 };
  dt_iop_colorin_gui_data_t gui = { 0 };
  dt_iop_module_t module = { .dev = &dev, .gui_data = &gui };
  float table[] = { 0.0f, 1.0f, 1.0f };
  for(int d65 = 0; d65 < 2; d65++)
    for(int forward = 0; forward < 2; forward++)
      for(int data = 0; data < 2; data++)
      {
        dt_mark_colormatrix_invalid(&dev.image_storage.d65_color_matrix[0]);
        dt_mark_colormatrix_invalid(&dev.image_storage.dng_forward_matrix[0]);
        if(d65) dev.image_storage.d65_color_matrix[0] = 1.0f;
        if(forward) dev.image_storage.dng_forward_matrix[0] = 1.0f;
        image.profile_hsm_data = data & 1 ? table : NULL;
        update_profile_list(&module);
        assert_false(cache_locked);
        int found = 0;
        int pos = 0;
        for(const GList *l = gui.image_profiles; l; l = l->next)
        {
          const dt_colorspaces_color_profile_t *profile = l->data;
          assert_int_equal(profile->in_pos, pos++);
          if(profile->type == DT_COLORSPACE_DNG_LOOK) found++;
        }
        assert_int_equal(found, d65 && !forward && data != 0);
        assert_int_equal(gui.n_image_profiles, pos);
      }
  g_list_free_full(gui.image_profiles, free);
  darktable.color_profiles = NULL;
}

static void test_embedded_matrix_commit(void **state)
{
  const float camera_matrix[9] =
    { 3.2404542f, -1.5371385f, -0.4985314f,
      -0.9692660f, 1.8760108f, 0.0415560f,
      0.0556434f, -0.2040259f, 1.0572252f };
  cmsCIExyY whitepoint;
  cmsWhitePointFromTemp(&whitepoint, 6504.0);
  const cmsCIExyYTRIPLE primaries = { { 0.64, 0.33, 1.0 },
                                    { 0.30, 0.60, 1.0 },
                                    { 0.15, 0.06, 1.0 } };
  cmsToneCurve *gamma = cmsBuildGamma(NULL, 1.0);
  cmsToneCurve *curves[] = { gamma, gamma, gamma };
  dt_colorspaces_color_profile_t lab = { .type = DT_COLORSPACE_LAB,
                                         .profile = cmsCreateLab4Profile(NULL) };
  dt_colorspaces_color_profile_t rec709 = { .type = DT_COLORSPACE_LIN_REC709,
    .profile = cmsCreateRGBProfile(&whitepoint, &primaries, curves) };
  cmsFreeToneCurve(gamma);
  dt_colorspaces_t profiles = { 0 };
  profiles.profiles = g_list_append(NULL, &lab);
  profiles.profiles = g_list_append(profiles.profiles, &rec709);
  darktable.color_profiles = &profiles;
  dt_develop_t dev = { 0 };
  dt_iop_module_t module = { .dev = &dev };
  dt_dev_pixelpipe_t pipe = { 0 };
  dt_dev_pixelpipe_iop_t piece = { .pipe = &pipe };
  dt_iop_colorin_params_t params = { .type_work = DT_COLORSPACE_LIN_REC709 };
  init_pipe(&module, &pipe, &piece);
  dt_iop_colorin_data_t *d = piece.data;

  for(int fallback = 0; fallback < 3; fallback++)
  {
    memcpy(image.d65_color_matrix, camera_matrix, sizeof(camera_matrix));
    memset(pipe.image.adobe_XYZ_to_CAM, 0, sizeof(pipe.image.adobe_XYZ_to_CAM));
    if(fallback) dt_mark_colormatrix_invalid(&image.d65_color_matrix[0]);
    if(fallback == 1)
      memcpy(pipe.image.adobe_XYZ_to_CAM, camera_matrix, sizeof(camera_matrix));
    params.type = DT_COLORSPACE_EMBEDDED_MATRIX;
    commit_params(&module, (dt_iop_params_t *)&params, &pipe, &piece);
    assert_true(dt_is_valid_colormatrix(d->cmatrix[0][0]));
    dt_colormatrix_t expected;
    memcpy(expected, d->cmatrix, sizeof(expected));
    params.type = DT_COLORSPACE_DNG_LOOK;
    commit_params(&module, (dt_iop_params_t *)&params, &pipe, &piece);
    assert_int_equal(d->type, DT_COLORSPACE_DNG_LOOK);
    assert_int_equal(d->clear_input, fallback < 2);
    assert_int_equal(pipe.input_profile_info->type, DT_COLORSPACE_DNG_LOOK);
    assert_memory_equal(d->cmatrix, expected, sizeof(expected));
    assert_memory_equal(pipe.input_profile_info->matrix_in, expected, sizeof(expected));
    assert_true(dt_is_valid_colormatrix(pipe.input_profile_info->matrix_out[0][0]));
    assert_false(cache_locked);
  }

  const float forward_matrix[9] =
    { 0.4124564f, 0.3575761f, 0.1804375f,
      0.2126729f, 0.7151522f, 0.0721750f,
      0.0193339f, 0.1191920f, 0.9503041f };
  memcpy(pipe.image.dng_forward_matrix, forward_matrix, sizeof(forward_matrix));
  params.type = DT_COLORSPACE_FORWARD_MATRIX;
  commit_params(&module, (dt_iop_params_t *)&params, &pipe, &piece);
  assert_int_equal(pipe.input_profile_info->type, DT_COLORSPACE_FORWARD_MATRIX);
  assert_memory_equal(pipe.input_profile_info->matrix_in, d->cmatrix, sizeof(d->cmatrix));
  assert_true(dt_is_valid_colormatrix(pipe.input_profile_info->matrix_out[0][0]));

  cleanup_pipe(&module, &pipe, &piece);
  for(GList *l = dev.allprofile_info; l; l = l->next)
  {
    dt_ioppr_cleanup_profile_info(l->data);
    dt_free_align(l->data);
  }
  g_list_free(dev.allprofile_info);
  cmsCloseProfile(lab.profile);
  cmsCloseProfile(rec709.profile);
  g_list_free(profiles.profiles);
  darktable.color_profiles = NULL;
}

static void test_profile_switch_resync(void **state)
{
  dt_gui_gtk_t gui = { 0 };
  darktable.gui = &gui;
  dt_dev_pixelpipe_t pipes[3] = { 0 };
  dt_develop_t dev = { .preview_pipe = &pipes[1] };
  dev.full.pipe = &pipes[0];
  dev.preview2.pipe = &pipes[2];
  darktable.develop = &dev;
  dt_iop_colorin_params_t params = { 0 };
  dt_colorspaces_color_profile_t profile = { .in_pos = 0 };
  dt_iop_colorin_gui_data_t data = { .n_image_profiles = 1 };
  data.image_profiles = g_list_append(NULL, &profile);
  dt_iop_module_t module = { .dev = &dev, .params = &params, .gui_data = &data };
  const dt_colorspaces_color_profile_type_t types[] =
    { DT_COLORSPACE_FORWARD_MATRIX, DT_COLORSPACE_DNG_LOOK,
      DT_COLORSPACE_EMBEDDED_MATRIX, DT_COLORSPACE_LIN_REC709 };
  for(int old = 0; old < (int)G_N_ELEMENTS(types); old++)
    for(int next = 0; next < (int)G_N_ELEMENTS(types); next++)
    {
      params.type = types[old];
      profile.type = types[next];
      for(int i = 0; i < 3; i++) pipes[i].changed = DT_DEV_PIPE_UNCHANGED;
      _profile_changed(NULL, &module);
      assert_int_equal(params.type, profile.type);
      const gboolean expected = (old < 2) != (next < 2);
      for(int i = 0; i < 3; i++)
      {
        assert_true(pipes[i].changed & DT_DEV_PIPE_TOP_CHANGED);
        assert_int_equal(!!(pipes[i].changed & DT_DEV_PIPE_SYNCH), expected);
      }
    }
  g_list_free(data.image_profiles);
  darktable.gui = NULL;
  darktable.develop = NULL;
}

int main(int argc, char *argv[])
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_dng_look_profile_list),
    cmocka_unit_test(test_embedded_matrix_commit),
    cmocka_unit_test(test_profile_switch_resync),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
