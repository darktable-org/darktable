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

#pragma once

/* Enable extra optimizations on GCC by including this header at the very
 * beginning of your *.c file (before any other includes). This applies
 * these optimizations for all of the source file.
 *
 * we use finite-math-only because divisions by zero are manually avoided
 * in the code, the rest is loop reorganization and vectorization optimization
 **/

#if defined(__GNUC__)
#pragma GCC optimize ("unroll-loops", "split-loops", \
                      "loop-nest-optimize", "tree-loop-im", \
                      "tree-loop-ivcanon", "ira-loop-pressure", \
                      "variable-expansion-in-unroller", \
                      "ivopts", "finite-math-only")
#endif
