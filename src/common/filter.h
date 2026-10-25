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

G_BEGIN_DECLS

typedef struct dt_filter_daterange_t
{
  double min_r;
  double max_r;
} dt_filter_daterange_t;

void dt_filter_daterange_free(gpointer data);

typedef struct dt_filter_date_count_t
{
  int64_t dt;
  int count;
} dt_filter_date_count_t;

void dt_filter_date_count_free(gpointer data);

/** get the date range; free with dt_filter_daterange_free() */
dt_filter_daterange_t* dt_filter_get_daterange(const gchar *colname, const gchar *where_ext);

/** get the count of images per date */
GList *dt_filter_get_date_counts(const gchar* colname, const gchar *where_ext);

/** get the mask of used months */
int dt_filter_get_used_months_mask(const gchar *colname, const gchar *where_ext);

G_END_DECLS

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

