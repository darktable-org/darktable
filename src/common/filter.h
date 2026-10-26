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

typedef struct dt_filter_range_t
{
  double min_r;
  double max_r;
} dt_filter_range_t;

void dt_filter_range_free(gpointer data);

typedef struct dt_filter_range_count_t
{
  double value_r;
  int count;
} dt_filter_range_count_t;

void dt_filter_range_count_free(gpointer data);

typedef struct dt_filter_name_count_t
{
  gchar *name;
  int count;
} dt_filter_name_count_t;

void dt_filter_name_count_free(gpointer data);

typedef struct dt_filter_ext_flag_count_t
{
  gchar *name;
  int count;
  int flags;
} dt_filter_ext_flag_count_t;

void dt_filter_ext_flag_count_free(gpointer data);


/** get the range; free with dt_filter_range_free() */
dt_filter_range_t* dt_filter_get_range(const gchar *colname, const gchar *where_ext);

/** get the count of images per group */
GList *dt_filter_get_range_counts(const gchar* colname, const gchar *where_ext);

/** get the count of images per filename */
GList *dt_filter_get_filename_counts(const gchar *where_ext);

/** get the count of images per extension */
GList *dt_filter_get_extension_counts(const gchar *where_ext);

/** get the history type counts */
GList *dt_filter_get_history_counts(const gchar *where_ext);

/** get the duplicate counts */
GList *dt_filter_get_duplicate_counts(const gchar *where_ext);

/** get the mask of used months */
int dt_filter_get_used_months_mask(const gchar *colname, const gchar *where_ext);


G_END_DECLS

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

