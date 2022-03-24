/*
    This file is part of darktable,
    Copyright (C) 2010-2020 darktable developers.

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

#include "gui/gtk.h"


#ifdef USE_DARKTABLE_PROFILING
#define TIMER_START(name, description)                                                                       \
  dt_timer_t *name = dt_timer_start_with_name(__FILE__, __FUNCTION__, description)
#else
#define TIMER_START(name, description)                                                                       \
  {                                                                                                          \
  }
#endif

#ifdef USE_DARKTABLE_PROFILING
#define TIMER_STOP(name) dt_timer_stop_with_name(name)
#else
#define TIMER_STOP(name)                                                                                     \
  {                                                                                                          \
  }
#endif

#ifdef USE_DARKTABLE_PROFILING
typedef struct dt_timer_t
{
  const char *file;
  const char *function;
  const char *description;
  GTimer *timer;
} dt_timer_t;

dt_timer_t *dt_timer_start_with_name(const char *file, const char *function, const char *description);
void dt_timer_stop_with_name(dt_timer_t *);
#endif

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

