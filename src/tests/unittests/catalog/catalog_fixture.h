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

#pragma once

#include "common/darktable.h"

// darktable started without GUI on an in-memory library, as darktable-cli
// does, with the integration test images imported. One per test process:
// call from the group setup and teardown.
int dt_test_catalog_setup(void **state);
int dt_test_catalog_teardown(void **state);

// id of an imported test image, by file name (e.g. "mire1.cr2")
dt_imgid_t dt_test_catalog_image(const char *filename);
