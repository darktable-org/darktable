/*
    This file is part of darktable,
    Copyright (C) 2022 darktable developers.

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

#include <gtk/gtk.h>
G_BEGIN_DECLS
#define DTGTK_RANGE_SELECT(obj)                                                                                   \
  G_TYPE_CHECK_INSTANCE_CAST(obj, dtgtk_range_select_get_type(), GtkDarktableRangeSelect)
#define DTGTK_RANGE_SELECT_CLASS(klass)                                                                           \
  G_TYPE_CHECK_CLASS_CAST(klass, dtgtk_range_select_get_type(), GtkDarktableRangeSelectClass)
#define DTGTK_IS_RANGE_SELECT(obj) G_TYPE_CHECK_INSTANCE_TYPE(obj, dtgtk_range_select_get_type())
#define DTGTK_IS_RANGE_SELECT_CLASS(klass) G_TYPE_CHECK_CLASS_TYPE(obj, dtgtk_range_select_get_type())

typedef enum dt_range_bounds_t
{
  DT_RANGE_BOUND_FIXED = 0,
  DT_RANGE_BOUND_MIN = 1 << 0,
  DT_RANGE_BOUND_MAX = 1 << 1
} dt_range_bounds_t;

typedef struct _GtkDarktableRangeSelect
{
  GtkBin widget;
  dt_range_bounds_t bounds;
  double min;
  double max;
  double step;
  GtkWidget *entry_min;
  GtkWidget *entry_max;
  gchar formater[8];
  GtkWidget *band;
} GtkDarktableRangeSelect;

typedef struct _GtkDarktableRangeSelectClass
{
  GtkBoxClass parent_class;
} GtkDarktableRangeSelectClass;

GType dtgtk_range_select_get_type(void);

/** instantiate a new range selection widget */
GtkWidget *dtgtk_range_select_new();

void dtgtk_range_select_set_range(GtkDarktableRangeSelect *range, const dt_range_bounds_t bounds, const double min,
                                  const double max, gboolean signal);
dt_range_bounds_t dtgtk_range_select_get_range(GtkDarktableRangeSelect *range, double *min, double *max);

G_END_DECLS