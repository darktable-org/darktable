/*
    This file is part of darktable,
    Copyright (C) 2022 darktable developers.

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
    scandir: Scan a directory, collecting all (selected) items into a an array.

    The original implementation of scandir has been made by Richard Salz.
    The original author put this code in the public domain.

    It has been modified to simplify slightly and increae readability.
*/

#pragma once

int alphasort(const struct dirent**, const struct dirent**);

int scandir(const char *directory_name,
            struct dirent ***array_pointer,
            int (*select_function) (const struct dirent *),
            int (*compare_function) (const struct dirent**, const struct dirent**));

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
