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

void dt_filter_range_free(gpointer data)
{
  g_free(data);
}

void dt_filter_range_count_free(gpointer data)
{
  g_free(data);
}

void dt_filter_int_count_free(gpointer data)
{
  g_free(data);
}

void dt_filter_name_count_free(gpointer data)
{
  dt_filter_name_count_t *d = data;

  g_free(d->name);
  g_free(d);
}

void dt_filter_ext_flag_count_free(gpointer data)
{
  dt_filter_ext_flag_count_t *d = data;

  g_free(d->name);
  g_free(d);
}

dt_filter_range_t* dt_filter_get_range(const gchar *colname,
                                       const gchar *where_ext)
{
  dt_filter_range_t *range = g_malloc0(sizeof(dt_filter_range_t));

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
    range->min_r = sqlite3_column_double(stmt, 0);
    range->max_r = sqlite3_column_double(stmt, 1);
  }
  sqlite3_finalize(stmt);

  return range;
}

static GList *_get_range_counts_for_query(const gchar *query)
{
  GList *counts = NULL;

  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const double value_r = sqlite3_column_double(stmt, 0);
    const int count = sqlite3_column_int(stmt, 1);
    dt_filter_range_count_t *entry = g_malloc0(sizeof(dt_filter_range_count_t));
    entry->value_r = value_r;
    entry->count = count;
    counts = g_list_prepend(counts, entry);
  }
  sqlite3_finalize(stmt);

  return counts;
}

static GList *_get_int_counts_for_query(const gchar *query)
{
  GList *counts = NULL;

  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const double val= sqlite3_column_double(stmt, 0);
    const int count = sqlite3_column_int(stmt, 1);
    dt_filter_int_count_t *entry = g_malloc0(sizeof(dt_filter_int_count_t));
    entry->val= val;
    entry->count = count;
    counts = g_list_prepend(counts, entry);
  }
  sqlite3_finalize(stmt);

  return counts;
}

static GList *_get_name_counts_for_query(const gchar *query)
{
  GList *counts = NULL;

  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const char *name = (const char *)sqlite3_column_text(stmt, 0);
    if(name == NULL) continue; // safeguard against degenerated db entries
    const int count = sqlite3_column_int(stmt, 1);
    dt_filter_name_count_t *entry = g_malloc0(sizeof(dt_filter_name_count_t));
    entry->name = g_strdup(name);
    entry->count = count;
    counts = g_list_prepend(counts, entry);
  }
  sqlite3_finalize(stmt);

  return counts;
}

GList *dt_filter_get_range_counts(const gchar* colname,
                                  const gchar *where_ext)
{
  // clang-format off
  gchar *query = g_strdup_printf("SELECT %s AS val, COUNT(*) AS count"
                                 " FROM main.images AS mi"
                                 " WHERE %s IS NOT NULL AND %s"
                                 " GROUP BY val",
                                 colname, colname, where_ext);
  // clang-format on
  
  GList *counts = _get_range_counts_for_query(query);
  g_free(query);

  return counts;
}

// how do we separate filename and extension directly in sqlite :
// starting example : 'nice.bird.cr2'
// replace(filename, '.', '') => nicebirdcr2 (remove all the point)
// rtrim(filename, replace(filename, '.', '')) => nice.bird. (remove ending chars presents in 'nice.bird.cr2' and
// 'nicebirdcr2') rtrim(rtrim(filename, replace(filename, '.', '')), '.') => nice.bird (remove ending '.')
// replace(filename, rtrim(filename, replace(filename, '.', '')), '.') => .cr2 (replace the filename part by a
// '.')

GList *dt_filter_get_filename_counts(const gchar *where_ext)
{
  // clang-format off
  gchar *query = g_strdup_printf("SELECT rtrim(rtrim(filename, replace(filename, '.', '')), '.') AS fn, COUNT(*) AS count"
                                 " FROM main.images AS mi"
                                 " WHERE %s"
                                 " GROUP BY fn"
                                 " ORDER BY filename",
                                 where_ext);
  // clang-format on

  GList *counts = _get_name_counts_for_query(query);
  g_free(query);

  return counts;
}

GList *dt_filter_get_extension_counts(const gchar *where_ext)
{
  GList *counts = NULL;

  // clang-format off
  gchar *query = g_strdup_printf("SELECT upper(replace(filename, rtrim(filename, replace(filename, '.', '')), '.')) AS ext, COUNT(*) AS count, flags"
                                 " FROM main.images AS mi"
                                 " WHERE %s"
                                 " GROUP BY ext"
                                 " ORDER BY ext",
                                 where_ext);
  // clang-format on

  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  g_free(query);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const char *name = (const char *)sqlite3_column_text(stmt, 0);
    if(name == NULL) continue; // safeguard against degenerated db entries
    const int count = sqlite3_column_int(stmt, 1);
    const int flags = sqlite3_column_int(stmt, 2);
    dt_filter_ext_flag_count_t *entry = g_malloc0(sizeof(dt_filter_name_count_t));
    entry->name = g_strdup(name);
    entry->count = count;
    entry->flags = flags;
    counts = g_list_prepend(counts, entry);
  }
  sqlite3_finalize(stmt);

  return counts;
}

GList *dt_filter_get_history_counts(const gchar *where_ext)
{
  // clang-format off
  gchar *query = g_strdup_printf("SELECT CASE"
                                 "       WHEN basic_hash == current_hash THEN 0"
                                 "       WHEN auto_hash == current_hash THEN 1"
                                 "       WHEN current_hash IS NOT NULL THEN 2"
                                 "       ELSE 0"
                                 "     END as altered, COUNT(*) AS count"
                                 " FROM main.images AS mi"
                                 " LEFT JOIN (SELECT DISTINCT imgid, basic_hash, auto_hash, current_hash"
                                 "            FROM main.history_hash) ON id = imgid"
                                 " WHERE %s"
                                 " GROUP BY altered"
                                 " ORDER BY altered ASC",
                                 where_ext);
  // clang-format on
  
  GList *counts = _get_int_counts_for_query(query);
  g_free(query);

  return counts;
}

GList *dt_filter_get_duplicate_counts(const gchar *where_ext)
{
  // clang-format off
  gchar *query = g_strdup_printf("SELECT CASE "
                                 "         WHEN dups.min_version = version THEN 0"
                                 "         ELSE 1"
                                 "       END AS orig"
                                 "     , COUNT(*) AS count"
                                 " FROM main.images AS mi"
                                 " JOIN ("
                                 "   SELECT film_id AS f_id"
                                 "        , filename"
                                 "        , MIN(version) AS min_version"
                                 "   FROM main.images"
                                 "   GROUP BY f_id"
                                 "          , filename"
                                 "   HAVING COUNT(*) > 1"
                                 " ) dups ON mi.film_id = dups.f_id AND mi.filename = dups.filename"
                                 " WHERE %s"
                                 " GROUP BY orig"
                                 " ORDER BY orig ASC",
                                 where_ext);
  // clang-format on
  
  GList *counts = _get_int_counts_for_query(query);
  g_free(query);

  return counts;
}

int dt_filter_get_used_months_mask(const gchar *colname,
                                   const gchar *where_ext)
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
  g_free(query);
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

