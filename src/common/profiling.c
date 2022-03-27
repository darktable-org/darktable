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

#include "common/profiling.h"

dt_timer_t *dt_timer_start_with_name(const char *file, const char *function, const char *description)
{
  dt_timer_t *t = g_malloc(sizeof(dt_timer_t));
  t->file = file;
  t->function = function;
  t->timer = g_timer_new();
  t->description = description;
  return t;
}

void dt_timer_stop_with_name(dt_timer_t *t)
{
  g_assert(t != NULL);
  g_timer_stop(t->timer);
  gulong ms = 0;
  fprintf(stderr, "Timer %s in function %s took %.3f seconds to execute.\n", t->description, t->function,
          g_timer_elapsed(t->timer, &ms));
  g_timer_destroy(t->timer);
  g_free(t);
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

