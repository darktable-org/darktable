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

#include "common/filter.h"
#include "common/utility.h"
#include "common/debug.h"
#include <glib-2.0/glib.h>

void dt_filter_daterange_free(gpointer data)
{
  g_free(data);
}

void dt_filter_date_count_free(gpointer data)
{
  g_free(data);
}

dt_filter_daterange_t* dt_filter_get_daterange(const gchar *colname,
                                               const gchar *where_ext)
{
  dt_filter_daterange_t *range = g_malloc0(sizeof(dt_filter_daterange_t));

  // clang-format off
  gchar *query = g_strdup_printf("SELECT MIN(%s), MAX(%s)"
                                 " FROM main.images"
                                 " WHERE %s IS NOT NULL",
                                 colname, colname, colname);
  if(where_ext != NULL)
    dt_util_str_cat(&query, " AND %s", where_ext); 
  // clang-format on

  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  g_free(query);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    range->min_r = sqlite3_column_int64(stmt, 0);
    range->max_r = sqlite3_column_int64(stmt, 1);
  }
  sqlite3_finalize(stmt);

  return range;
}

GList *dt_filter_get_date_counts(const gchar* colname, const gchar *where_ext)
{
  GList *counts = NULL;

  // clang-format off
  gchar *query = g_strdup_printf("SELECT %s AS date, COUNT(*) AS count"
                                 " FROM main.images AS mi"
                                 " WHERE %s IS NOT NULL AND %s"
                                 " GROUP BY date",
                                 colname, colname, where_ext);
  // clang-format on
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const int64_t dt = sqlite3_column_int64(stmt, 0);
    const int count = sqlite3_column_int(stmt, 1);
    dt_filter_date_count_t *entry = g_malloc0(sizeof(dt_filter_date_count_t));
    entry->dt = dt;
    entry->count = count;
    counts = g_list_prepend(counts, entry);
  }
  sqlite3_finalize(stmt);

  return counts;
}

int dt_filter_get_used_months_mask(const gchar *colname, const gchar *where_ext)
{
  int used_months_mask = 0;

  // clang-format off
  gchar *query = g_strdup_printf(
      "SELECT CAST(strftime('%%m', %s/1000000"
      "                     + unixepoch('0001-01-01 00:00:00'), 'unixepoch') AS INTEGER) AS month"
      " FROM main.images"
      " WHERE %s IS NOT NULL AND %s"
      " GROUP BY month",
       colname, colname, where_ext);
  // clang-format on
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const int m = sqlite3_column_int(stmt, 0);
    used_months_mask |= (1 << (m-1));
  }
  sqlite3_finalize(stmt);

  return used_months_mask;
}


// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

