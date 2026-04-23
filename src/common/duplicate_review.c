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

#include "common/duplicate_review.h"
#include "common/darktable.h"
#include "common/database.h"
#include "common/debug.h"
#include "common/datetime.h"
#include "common/image.h"
#include "common/ratings.h"

#include <glib.h>
#include <sqlite3.h>
#include <stdlib.h>

/* Max time gap between consecutive shots in a burst (GTimeSpan / DB datetime_taken units). */
#define DT_DUP_REVIEW_BURST_GAP (3LL * G_TIME_SPAN_SECOND)

typedef struct dt_dup_review_row_t
{
  dt_imgid_t id;
  dt_filmid_t film_id;
  gint64 datetime_taken;
  int flags;
} dt_dup_review_row_t;

static int _star_rating(const int flags)
{
  if(flags & DT_IMAGE_REJECTED) return -1;
  return flags & DT_VIEW_RATINGS_MASK;
}

static gint64 _read_datetime_from_stmt(sqlite3_stmt *stmt, const int col)
{
  const int t = sqlite3_column_type(stmt, col);
  if(t == SQLITE_INTEGER)
    return sqlite3_column_int64(stmt, col);
  if(t == SQLITE_TEXT)
  {
    const char *txt = (const char *)sqlite3_column_text(stmt, col);
    if(txt && *txt)
      return dt_datetime_exif_to_gtimespan(txt);
  }
  return 0;
}

static int _cmp_row(const void *a, const void *b)
{
  const dt_dup_review_row_t *ra = a;
  const dt_dup_review_row_t *rb = b;
  if(ra->film_id != rb->film_id)
    return (ra->film_id < rb->film_id) ? -1 : 1;
  if(ra->datetime_taken != rb->datetime_taken)
    return (ra->datetime_taken < rb->datetime_taken) ? -1 : 1;
  if(ra->id != rb->id)
    return (ra->id < rb->id) ? -1 : 1;
  return 0;
}

static dt_imgid_t _pick_keeper(dt_dup_review_row_t *rows, const int start, const int n)
{
  int max_rating = -1;
  for(int i = start; i < start + n; i++)
  {
    const int r = _star_rating(rows[i].flags);
    if(r > max_rating) max_rating = r;
  }
  if(max_rating < 0) return NO_IMGID;

  dt_imgid_t keeper = NO_IMGID;
  gint64 best_dt = G_MAXINT64;
  for(int i = start; i < start + n; i++)
  {
    const int r = _star_rating(rows[i].flags);
    if(r != max_rating) continue;
    const gint64 dt = rows[i].datetime_taken;
    if(!dt_is_valid_imgid(keeper) || dt < best_dt
       || (dt == best_dt && rows[i].id < keeper))
    {
      keeper = rows[i].id;
      best_dt = dt;
    }
  }
  return keeper;
}

void dt_duplicate_review_free_id_list(GList *list)
{
  g_list_free(list);
}

GList *dt_duplicate_review_get_duplicate_ids(void)
{
  GList *list = NULL;
  sqlite3_stmt *stmt = NULL;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get(darktable.db),
      "SELECT mi.id"
      "  FROM main.images AS mi"
      "  JOIN memory.collected_images AS ci ON ci.imgid = mi.id"
      " WHERE mi.version > (SELECT MIN(m2.version)"
      "                     FROM main.images AS m2"
      "                    WHERE m2.film_id = mi.film_id"
      "                      AND m2.filename = mi.filename)",
      -1, &stmt, NULL);
  // clang-format on
  if(stmt == NULL) return NULL;

  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const dt_imgid_t id = sqlite3_column_int(stmt, 0);
    list = g_list_prepend(list, GINT_TO_POINTER(id));
  }
  sqlite3_finalize(stmt);
  return g_list_reverse(list);
}

GList *dt_duplicate_review_get_burst_candidate_ids(void)
{
  sqlite3_stmt *stmt = NULL;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get(darktable.db),
      "SELECT mi.id, mi.film_id, mi.datetime_taken, mi.flags"
      "  FROM main.images AS mi"
      "  JOIN memory.collected_images AS ci ON ci.imgid = mi.id"
      " WHERE mi.version = (SELECT MIN(m2.version)"
      "                       FROM main.images AS m2"
      "                      WHERE m2.film_id = mi.film_id"
      "                        AND m2.filename = mi.filename)",
      -1, &stmt, NULL);
  // clang-format on
  if(stmt == NULL) return NULL;

  int nalloc = 256;
  int n = 0;
  dt_dup_review_row_t *rows = malloc(sizeof(dt_dup_review_row_t) * (size_t)nalloc);

  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    if(n >= nalloc)
    {
      nalloc *= 2;
      dt_dup_review_row_t *nr = realloc(rows, sizeof(dt_dup_review_row_t) * (size_t)nalloc);
      if(!nr)
      {
        free(rows);
        sqlite3_finalize(stmt);
        return NULL;
      }
      rows = nr;
    }
    rows[n].id = sqlite3_column_int(stmt, 0);
    rows[n].film_id = sqlite3_column_int(stmt, 1);
    rows[n].datetime_taken = _read_datetime_from_stmt(stmt, 2);
    rows[n].flags = sqlite3_column_int(stmt, 3);
    n++;
  }
  sqlite3_finalize(stmt);

  if(n < 2)
  {
    free(rows);
    return NULL;
  }

  qsort(rows, (size_t)n, sizeof(dt_dup_review_row_t), _cmp_row);

  GList *candidates = NULL;
  int i = 0;
  while(i < n)
  {
    int j = i + 1;
    while(j < n)
    {
      if(rows[j].film_id != rows[j - 1].film_id) break;
      const gint64 dt_prev = rows[j - 1].datetime_taken;
      const gint64 dt_curr = rows[j].datetime_taken;
      if(dt_prev <= 0 || dt_curr <= 0) break;
      if(dt_curr - dt_prev > DT_DUP_REVIEW_BURST_GAP) break;
      j++;
    }
    const int cluster_size = j - i;
    if(cluster_size >= 2)
    {
      const dt_imgid_t keeper = _pick_keeper(rows, i, cluster_size);
      if(dt_is_valid_imgid(keeper))
      {
        for(int k = i; k < j; k++)
        {
          if(rows[k].id == keeper) continue;
          const int r = _star_rating(rows[k].flags);
          if(r < 0) continue;
          candidates = g_list_prepend(candidates, GINT_TO_POINTER(rows[k].id));
        }
      }
    }
    i = j;
  }

  free(rows);
  return g_list_reverse(candidates);
}
