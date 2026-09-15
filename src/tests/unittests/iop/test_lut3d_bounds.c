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
 * cmocka unit tests for the checks iop/lut3d.c applies to stored params
 * before they reach G'MIC or the file system: the keypoint count, the
 * cache file name derived from the LUT name, and the relative LUT path.
 */

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>

#include <cmocka.h>

#include "iop/lut3d.c"

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

#ifdef HAVE_GMIC
// the G'MIC side lives in lut3dgmic.cpp, which is part of the module, not of
// lib_darktable; these stand-ins record whether the compressed path was taken
static int decompress_calls = 0;

void lut3d_decompress_clut(const unsigned char *const input_keypoints,
                           const unsigned int nb_input_keypoints,
                           const unsigned int output_resolution,
                           float *const output_clut_data,
                           const char *const filename)
{
  decompress_calls++;
}

unsigned int lut3d_get_cached_clut(float *const output_clut_data,
                                   const unsigned int output_resolution,
                                   const char *const filename)
{
  return 0;
}

gboolean lut3d_read_gmz(int *const nb_keypoints,
                        unsigned char *const keypoints,
                        const char *const filename,
                        int *const nb_lut,
                        void *g,
                        const char *const lutname,
                        const gboolean newlutname)
{
  return FALSE;
}

static void test_keypoint_count_bounds(void **state)
{
  dt_iop_lut3d_params_t *p = calloc(1, sizeof(dt_iop_lut3d_params_t));
  g_strlcpy(p->filepath, "luts.gmz", sizeof(p->filepath));
  g_strlcpy(p->lutname, "film", sizeof(p->lutname));
  float *clut = NULL;

  // the count bounds the read of the fixed c_clut array
  p->nb_keypoints = DT_IOP_LUT3D_MAX_KEYPOINTS + 1;
  decompress_calls = 0;
  assert_int_equal(_calculate_clut(p, &clut), 0);
  assert_int_equal(decompress_calls, 0);

  p->nb_keypoints = -1;
  assert_int_equal(_calculate_clut(p, &clut), 0);
  assert_int_equal(decompress_calls, 0);

  p->nb_keypoints = DT_IOP_LUT3D_MAX_KEYPOINTS;
  assert_int_equal(_calculate_clut(p, &clut), DT_IOP_LUT3D_CLUT_LEVEL);
  assert_int_equal(decompress_calls, 1);

  dt_free_align(clut);
  free(p);
}

static void test_cache_filename_carries_no_command_syntax(void **state)
{
  char cache_filename[DT_IOP_LUT3D_MAX_PATHNAME];
  gchar *cache_dir = g_build_filename(g_get_user_cache_dir(), "gmic", NULL);

  // the file name is inserted into -o "%s" and -i "%s"
  _get_cache_filename("x\" -exec \"sh -c id\" -o \"../../y", cache_filename);

  assert_true(g_str_has_prefix(cache_filename, cache_dir));
  const char *const basename = cache_filename + strlen(cache_dir) + 1;
  assert_null(strpbrk(basename, "\"/\\"));
  assert_true(g_str_has_suffix(basename, ".cimgz"));

  g_free(cache_dir);
}

static void test_gmic_arg_rejects_command_syntax(void **state)
{
  // the .gmz path is inserted into a quoted G'MIC command
  assert_true(_gmic_arg_is_safe("/lut/root/film.gmz"));
  assert_true(_gmic_arg_is_safe("/lut/root/sub/film.gmz"));

  assert_false(_gmic_arg_is_safe("/lut/root/a\" -exec \"sh -c id.gmz"));
  assert_false(_gmic_arg_is_safe("/lut/root/a{1+1}.gmz"));
  assert_false(_gmic_arg_is_safe("/lut/root/a$x.gmz"));
  assert_false(_gmic_arg_is_safe("/lut/root/a\\b.gmz"));
}
#endif // HAVE_GMIC

static void test_filepath_parent_components(void **state)
{
  assert_true(_filepath_is_safe("film.cube"));
  assert_true(_filepath_is_safe("sub/film.png"));
  assert_true(_filepath_is_safe("Film..v2.cube"));
  assert_true(_filepath_is_safe("...cube"));

  assert_false(_filepath_is_safe(".."));
  assert_false(_filepath_is_safe("../film.cube"));
  assert_false(_filepath_is_safe("sub/../../film.cube"));
  assert_false(_filepath_is_safe("sub/.."));
  assert_false(_filepath_is_safe("..\\film.gmz"));
  assert_false(_filepath_is_safe("sub\\..\\..\\film.gmz"));
}

int main(int argc, char *argv[])
{
  const struct CMUnitTest tests[] = {
#ifdef HAVE_GMIC
    cmocka_unit_test(test_keypoint_count_bounds),
    cmocka_unit_test(test_cache_filename_carries_no_command_syntax),
    cmocka_unit_test(test_gmic_arg_rejects_command_syntax),
#endif
    cmocka_unit_test(test_filepath_parent_components),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
