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
 * cmocka unit tests for the module imageio/imageio_libraw.c
 *
 * These tests cover the LibRaw maker/model normalization table. The table is
 * used to map the EXIF maker/model strings of cameras handled by LibRaw (such
 * as Canon CR3) to the clean maker/model/alias strings darktable uses for
 * camera identification.
 *
 * Please see README.md for more detailed documentation.
 */
#include <limits.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <cmocka.h>

#include "imageio/imageio_libraw.h"

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

/*
 * TEST FUNCTIONS
 */

// A known camera resolves to its clean maker/model/alias.
static void test_lookup_known_resolves(void **state)
{
  char mk[64], md[64], al[64];
  const gboolean found =
    dt_libraw_lookup_makermodel("Canon", "Canon EOS R6",
                                mk, sizeof(mk), md, sizeof(md), al, sizeof(al));
  assert_true(found);
  assert_string_equal(mk, "Canon");
  assert_string_equal(md, "EOS R6");
  assert_string_equal(al, "EOS R6");
}

// The Canon EOS R6 Mark III mapping added for CR3 support resolves correctly.
static void test_lookup_r6_mark_iii(void **state)
{
  char mk[64], md[64], al[64];
  const gboolean found =
    dt_libraw_lookup_makermodel("Canon", "Canon EOS R6 Mark III",
                                mk, sizeof(mk), md, sizeof(md), al, sizeof(al));
  assert_true(found);
  assert_string_equal(mk, "Canon");
  assert_string_equal(md, "EOS R6 Mark III");
  assert_string_equal(al, "EOS R6 Mark III");
}

// Adding the Mark III entry must not regress the neighbouring R6 variants:
// matching is exact, so each generation resolves to its own distinct model.
// Note Canon reports the Mark II body with the abbreviated EXIF model string
// "Canon EOS R6m2", while the Mark III body uses the full "Mark III" form.
static void test_lookup_r6_variants_distinct(void **state)
{
  struct
  {
    const char *exif_model;
    const char *clean_model;
  } cases[] = {
    { "Canon EOS R6", "EOS R6" },
    { "Canon EOS R6m2", "EOS R6 Mark II" },
    { "Canon EOS R6 Mark III", "EOS R6 Mark III" },
  };

  for(size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
  {
    char mk[64], md[64], al[64];
    const gboolean found =
      dt_libraw_lookup_makermodel("Canon", cases[i].exif_model,
                                  mk, sizeof(mk), md, sizeof(md),
                                  al, sizeof(al));
    assert_true(found);
    assert_string_equal(md, cases[i].clean_model);
  }
}

// An unknown camera is reported as not found and outputs are left untouched.
static void test_lookup_unknown_not_found(void **state)
{
  char mk[64] = "untouched", md[64] = "untouched", al[64] = "untouched";
  const gboolean found =
    dt_libraw_lookup_makermodel("NoSuchMaker", "NoSuchModel",
                                mk, sizeof(mk), md, sizeof(md), al, sizeof(al));
  assert_false(found);
  assert_string_equal(mk, "untouched");
  assert_string_equal(md, "untouched");
  assert_string_equal(al, "untouched");
}

// A correct model with the wrong maker must not match.
static void test_lookup_wrong_maker(void **state)
{
  char mk[64], md[64], al[64];
  const gboolean found =
    dt_libraw_lookup_makermodel("Nikon", "Canon EOS R6 Mark III",
                                mk, sizeof(mk), md, sizeof(md), al, sizeof(al));
  assert_false(found);
}

int main(int argc, char *argv[])
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_lookup_known_resolves),
    cmocka_unit_test(test_lookup_r6_mark_iii),
    cmocka_unit_test(test_lookup_r6_variants_distinct),
    cmocka_unit_test(test_lookup_unknown_not_found),
    cmocka_unit_test(test_lookup_wrong_maker),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
