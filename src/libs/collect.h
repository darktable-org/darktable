/*
    This file is part of darktable,
    Copyright (C) 2011-2026 darktable developers.

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

#include <glib/gi18n.h>

typedef enum dt_lib_collect_mode_t
{
  DT_LIB_COLLECT_MODE_AND = 0,
  DT_LIB_COLLECT_MODE_OR,
  DT_LIB_COLLECT_MODE_AND_NOT
} dt_lib_collect_mode_t;

static const char *dt_month_names[] __attribute__((unused)) =
{
  N_("January"),
  N_("February"),
  N_("March"),
  N_("April"),
  N_("May"),
  N_("June"),
  N_("July"),
  N_("August"),
  N_("September"),
  N_("October"),
  N_("November"),
  N_("December"),
};

static const char *dt_month_short_names[] __attribute__((unused)) =
{
  N_("Jan"),
  N_("Feb"),
  N_("Mar"),
  N_("Apr"),
  NC_("short_month_name", "May"),
  N_("Jun"),
  N_("Jul"),
  N_("Aug"),
  N_("Sep"),
  N_("Oct"),
  N_("Nov"),
  N_("Dec"),
};

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
