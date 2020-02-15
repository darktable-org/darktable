/*
    This file is part of darktable,
    copyright (c) 2020 Martin Burri.

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
#include <limits.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>

#include <cmocka.h>

#include "iop/filmicrgb.c"

// epsilon for floating point comparison (TODO: take more sophisticated value):
#define E 1e-6f


/*
 * MOCKED FUNCTIONS
 */

void __wrap_dt_iop_color_picker_reset(dt_iop_module_t *module, gboolean update)
{
  check_expected_ptr(module);
  check_expected(update);
}


/*
 * TEST FUNCTIONS
 */

static void test_sample(void **state)
{
  expect_value(__wrap_dt_iop_color_picker_reset, module, NULL);
  expect_value(__wrap_dt_iop_color_picker_reset, update, TRUE);
  gui_focus(NULL, 0);
}


/*
 * MAIN FUNCTION
 */
int main()
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_sample)
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}
