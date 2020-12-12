/*
    This file is part of darktable,
    Copyright (C) 2020 darktable developers.

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
 * Extra assertion macros in addition to the ones defined by cmocka.
 *
 * Please see ../README.md for more detailed documentation.
 */
#include <cmocka.h>

// assert_float_equal() is not available on Ubuntu 18.04 (state 2020-01):
#ifndef assert_float_equal
#define assert_float_equal(a, b, epsilon)\
{\
  assert_true(a < (b + epsilon));\
  assert_true(a > (b - epsilon));\
}
#endif
