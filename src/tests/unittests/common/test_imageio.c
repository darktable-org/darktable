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
 * cmocka unit tests for dt_imageio_identity_needs_recompute()
 * (imageio/imageio.c), which decides whether dt_imageio_open() should
 * (re)compute an image's sha1sum + filesize identity
 * (plugins/darkroom/compute_checksum).
 *
 * Please see README.md for more detailed documentation.
 */
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include <cmocka.h>

#include "imageio/imageio_common.h"

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

// no checksum on record yet: always (re)compute, regardless of stat()
static void test_no_checksum_always_recomputes(void **state)
{
  assert_true(dt_imageio_identity_needs_recompute(FALSE, 0, TRUE, 12345));
  assert_true(dt_imageio_identity_needs_recompute(FALSE, 0, FALSE, 0));
}

// checksum recorded, size on disk matches: trust the existing value
static void test_matching_size_keeps_checksum(void **state)
{
  assert_false(dt_imageio_identity_needs_recompute(TRUE, 12345, TRUE, 12345));
}

// checksum recorded, size on disk differs: stale/foreign value, force a
// recompute -- covers both a sidecar carrying another image's identity
// (e.g. shared/copied PlayRaw edits) and the underlying file having
// been replaced since it was last hashed
static void test_size_mismatch_forces_recompute(void **state)
{
  assert_true(dt_imageio_identity_needs_recompute(TRUE, 12345, TRUE, 99999));
}

// checksum recorded but stat() itself failed: can't tell either way,
// so don't force an unnecessary rehash -- trust the existing value
static void test_stat_failure_keeps_checksum(void **state)
{
  assert_false(dt_imageio_identity_needs_recompute(TRUE, 12345, FALSE, 0));
}

int main(int argc, char* argv[])
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_no_checksum_always_recomputes),
    cmocka_unit_test(test_matching_size_keeps_checksum),
    cmocka_unit_test(test_size_mismatch_forces_recompute),
    cmocka_unit_test(test_stat_failure_keeps_checksum),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
