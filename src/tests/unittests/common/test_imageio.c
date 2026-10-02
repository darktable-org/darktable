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
 * cmocka tests for image-file identity (sha1sum/filesize) policy:
 *   - when to (re)compute from the file
 *   - never accept identity from a sidecar into the image/DB
 *   - mirror identity into XMP only when the image already has it
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

/* --- recompute from file --- */

static void test_no_checksum_always_recomputes(void **state)
{
  assert_true(dt_imageio_identity_needs_recompute(FALSE, 0, TRUE, 12345));
  assert_true(dt_imageio_identity_needs_recompute(FALSE, 0, FALSE, 0));
}

static void test_matching_size_keeps_checksum(void **state)
{
  assert_false(dt_imageio_identity_needs_recompute(TRUE, 12345, TRUE, 12345));
}

static void test_size_mismatch_forces_recompute(void **state)
{
  assert_true(dt_imageio_identity_needs_recompute(TRUE, 12345, TRUE, 99999));
}

static void test_stat_failure_keeps_checksum(void **state)
{
  assert_false(dt_imageio_identity_needs_recompute(TRUE, 12345, FALSE, 0));
}

/* --- sidecar must never update the image --- */

static void test_sidecar_never_accepted_as_identity_source(void **state)
{
  // foreign/stale XMP, missing image identity, or both present: still reject
  assert_false(dt_imageio_identity_accept_from_sidecar());
}

/* --- XMP write mirrors DB only when identity exists --- */

static void test_mirror_to_sidecar_when_image_has_identity(void **state)
{
  assert_true(dt_imageio_identity_mirror_to_sidecar(TRUE));
}

static void test_omit_sidecar_identity_when_image_has_none(void **state)
{
  // no inventing tags; also clears stale ones on write
  assert_false(dt_imageio_identity_mirror_to_sidecar(FALSE));
}

int main(int argc, char* argv[])
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_no_checksum_always_recomputes),
    cmocka_unit_test(test_matching_size_keeps_checksum),
    cmocka_unit_test(test_size_mismatch_forces_recompute),
    cmocka_unit_test(test_stat_failure_keeps_checksum),
    cmocka_unit_test(test_sidecar_never_accepted_as_identity_source),
    cmocka_unit_test(test_mirror_to_sidecar_when_image_has_identity),
    cmocka_unit_test(test_omit_sidecar_identity_when_image_has_none),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
