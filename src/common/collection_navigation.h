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

#include <glib.h>

#define DT_COLLECTION_NAVIGATION_LIMIT 32

typedef struct dt_collection_navigation_entry_t
{
  char *rules;
  int position;
} dt_collection_navigation_entry_t;

typedef struct dt_collection_navigation_t
{
  dt_collection_navigation_entry_t entries[DT_COLLECTION_NAVIGATION_LIMIT];
  unsigned int length;
  unsigned int current;
} dt_collection_navigation_t;

void dt_collection_navigation_clear(dt_collection_navigation_t *history);
void dt_collection_navigation_record(dt_collection_navigation_t *history,
                                     const char *rules, const int position);
gboolean dt_collection_navigation_can_move(const dt_collection_navigation_t *history,
                                            const int direction);
const dt_collection_navigation_entry_t *dt_collection_navigation_move(
    dt_collection_navigation_t *history, const int direction, const int position);

void dt_collection_navigation_record_current(void);
void dt_collection_navigation_cleanup(void);
gboolean dt_collection_navigation_available(const int direction);
void dt_collection_navigation_go(const int direction);
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
