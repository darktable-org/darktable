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

#define dt_image_cache_get __wrap_dt_image_cache_get
#define dt_image_cache_read_release __wrap_dt_image_cache_read_release
#define dt_colorspaces_get_profile __wrap_dt_colorspaces_get_profile

#include "iop/colorin.c"

#undef dt_image_cache_get
#undef dt_image_cache_read_release
#undef dt_colorspaces_get_profile

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

static dt_image_t *cached_image;
static gboolean cache_locked;

dt_image_t *__wrap_dt_image_cache_get(const dt_imgid_t imgid, const char mode)
{
  assert_int_equal(imgid, 1);
  assert_int_equal(mode, 'r');
  assert_false(cache_locked);
  cache_locked = TRUE;
  return cached_image;
}

void __wrap_dt_image_cache_read_release(const dt_image_t *img)
{
  assert_ptr_equal(img, cached_image);
  assert_true(cache_locked);
  cache_locked = FALSE;
}

const dt_colorspaces_color_profile_t *
__wrap_dt_colorspaces_get_profile(dt_colorspaces_color_profile_type_t type,
                                  const char *filename,
                                  const dt_colorspaces_profile_direction_t direction)
{
  static const dt_colorspaces_color_profile_t profile = { 0 };
  assert_int_equal(type, DT_COLORSPACE_LAB);
  return &profile;
}

static void test_hsm_owned_copy_and_profile_change(void **state)
{
  const float table[] = { -30.0f, 1.0f, 1.0f, 30.0f, 0.5f, 2.0f };
  dt_image_t img = { 0 };
  img.profile_hsm_data = g_malloc(sizeof(table));
  memcpy(img.profile_hsm_data, table, sizeof(table));
  img.profile_hsm_data_size = sizeof(table);
  img.profile_hsm_hue_div = img.profile_hsm_val_div = 1;
  img.profile_hsm_sat_div = 2;
  img.profile_hsm_encoding = 1;
  cached_image = &img;

  dt_dev_pixelpipe_iop_t piece = { 0 };
  init_pipe(NULL, NULL, &piece);
  dt_iop_colorin_data_t *d = piece.data;
  assert_null(d->hsm);
  _commit_hsm(d, 1);
  assert_false(cache_locked);
  assert_non_null(d->hsm);
  assert_ptr_not_equal(d->hsm, img.profile_hsm_data);
  assert_memory_equal(d->hsm, table, sizeof(table));
  assert_int_equal(d->hue_div, 1);
  assert_int_equal(d->sat_div, 2);
  assert_int_equal(d->val_div, 1);
  assert_int_equal(d->hsm_encode, 1);

  g_free(img.profile_hsm_data);
  img.profile_hsm_data = NULL;
  assert_memory_equal(d->hsm, table, sizeof(table));

  dt_iop_colorin_params_t p = { .type = DT_COLORSPACE_LAB };
  commit_params(NULL, (dt_iop_params_t *)&p, NULL, &piece);
  assert_null(d->hsm);
  assert_int_equal(d->hue_div | d->sat_div | d->val_div | d->hsm_encode, 0);
  commit_params(NULL, (dt_iop_params_t *)&p, NULL, &piece);
  cleanup_pipe(NULL, NULL, &piece);
  assert_null(piece.data);

  img.profile_hsm_data = (float *)table;
  init_pipe(NULL, NULL, &piece);
  _commit_hsm(piece.data, 1);
  cleanup_pipe(NULL, NULL, &piece);
  assert_null(piece.data);
  cached_image = NULL;
}

static void test_hsm_invalid_dimensions_and_size(void **state)
{
  float table[] = { 0.0f, 1.0f, 1.0f, 0.0f, 1.0f, 1.0f };
  dt_image_t img = { .profile_hsm_data = table, .profile_hsm_data_size = sizeof(table) };
  cached_image = &img;
  const int dims[][3] = {
    { 0, 2, 1 }, { -1, 2, 1 }, { 1, 1, 1 }, { 1, 2, 0 },
    { G_MAXINT, 2, 1 }, { 1, G_MAXINT, 1 }, { 1, 2, G_MAXINT },
    { 1, 2, 2 }
  };
  dt_iop_colorin_data_t *d = calloc(1, sizeof(*d));
  for(size_t i = 0; i < G_N_ELEMENTS(dims); i++)
  {
    img.profile_hsm_hue_div = dims[i][0];
    img.profile_hsm_sat_div = dims[i][1];
    img.profile_hsm_val_div = dims[i][2];
    _commit_hsm(d, 1);
    assert_null(d->hsm);
    assert_false(cache_locked);
  }
  cached_image = NULL;
  _commit_hsm(d, 1);
  assert_null(d->hsm);
  assert_false(cache_locked);
  free(d);
}

static void test_hsm_invalid_factors(void **state)
{
  float table[] = { 0.0f, 1.0f, 1.0f, 0.0f, 1.0f, 1.0f };
  dt_image_t img = {
    .profile_hsm_data = table, .profile_hsm_data_size = sizeof(table),
    .profile_hsm_hue_div = 1, .profile_hsm_sat_div = 2, .profile_hsm_val_div = 1
  };
  cached_image = &img;
  dt_iop_colorin_data_t *d = calloc(1, sizeof(*d));
  const float invalid[] = { NAN, INFINITY, -INFINITY, -1.0f };
  for(size_t i = 0; i < G_N_ELEMENTS(table); i++)
  {
    const float saved = table[i];
    for(size_t j = 0; j < G_N_ELEMENTS(invalid); j++)
    {
      if(i % 3 == 0 && invalid[j] == -1.0f) continue;
      table[i] = invalid[j];
      _commit_hsm(d, 1);
      assert_null(d->hsm);
      assert_false(cache_locked);
    }
    table[i] = saved;
  }
  free(d);
  cached_image = NULL;
}

int main(int argc, char *argv[])
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_hsm_owned_copy_and_profile_change),
    cmocka_unit_test(test_hsm_invalid_dimensions_and_size),
    cmocka_unit_test(test_hsm_invalid_factors)
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
