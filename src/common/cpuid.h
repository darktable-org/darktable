/*
    This file is part of darktable,
    Copyright (C) 2014-2020 darktable developers.

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

#include <glib.h>

typedef enum dt_cpu_flags_t
{
  CPU_FLAG_MMX = 1 << 0,
  CPU_FLAG_SSE = 1 << 1,
  CPU_FLAG_CMOV = 1 << 2,
  CPU_FLAG_3DNOW = 1 << 3,
  CPU_FLAG_3DNOW_EXT = 1 << 4,
  CPU_FLAG_AMD_ISSE = 1 << 5,
  CPU_FLAG_SSE2 = 1 << 6,
  CPU_FLAG_SSE3 = 1 << 7,
  CPU_FLAG_SSSE3 = 1 << 8,
  CPU_FLAG_SSE4_1 = 1 << 9,
  CPU_FLAG_SSE4_2 = 1 << 10,
  CPU_FLAG_AVX = 1 << 11
} dt_cpu_flags_t;

dt_cpu_flags_t dt_detect_cpu_features();

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

