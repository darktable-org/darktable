/*
    This file is part of darktable,
    Copyright (C) 2019-2021 darktable developers.

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

#include "common/collection.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/datetime.h"
#include "control/control.h"
#include "develop/develop.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include "libs/lib_api.h"
#include "views/view.h"

DT_MODULE(1)

typedef enum dt_lib_timeline_zooms_t {
  DT_LIB_TIMELINE_ZOOM_YEAR = 0,
  DT_LIB_TIMELINE_ZOOM_4MONTH,
  DT_LIB_TIMELINE_ZOOM_MONTH,
  DT_LIB_TIMELINE_ZOOM_10DAY,
  DT_LIB_TIMELINE_ZOOM_DAY,
  DT_LIB_TIMELINE_ZOOM_6HOUR,
  DT_LIB_TIMELINE_ZOOM_HOUR,
  DT_LIB_TIMELINE_ZOOM_10MINUTE,
  DT_LIB_TIMELINE_ZOOM_MINUTE
} dt_lib_timeline_zooms_t;

typedef enum dt_lib_timeline_mode_t {
  DT_LIB_TIMELINE_MODE_AND = 0,
  DT_LIB_TIMELINE_MODE_RESET
} dt_lib_timeline_mode_t;

typedef struct dt_lib_timeline_block_t
{
  gchar *name;
  int *values;
  int *collect_values;
  int values_count;
  dt_datetime_t init;
  int width;

} dt_lib_timeline_block_t;



typedef struct dt_lib_timeline_t
{
  dt_datetime_t time_mini;
  dt_datetime_t time_maxi;
  dt_datetime_t time_pos;

  GtkWidget *timeline;
  cairo_surface_t *surface;
  int surface_width;
  int surface_height;
  int32_t panel_width;
  int32_t panel_height;

  GList *blocks;
  dt_lib_timeline_zooms_t zoom;
  dt_lib_timeline_zooms_t precision;

  int start_x;
  int stop_x;
  int current_x;
  dt_datetime_t start_t;
  dt_datetime_t stop_t;
  gboolean has_selection;
  gboolean selecting;
  gboolean move_edge;

  gboolean autoscroll;
  gboolean in;

  gboolean size_handle_is_dragging;
  gint size_handle_x, size_handle_y;
  int32_t size_handle_height;

} dt_lib_timeline_t;



const char *name(dt_lib_module_t *self)
{
  return _("timeline");
}

dt_view_type_flags_t views(dt_lib_module_t *self)
{
  return DT_VIEW_LIGHTTABLE;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_BOTTOM;
}

int expandable(dt_lib_module_t *self)
{
  return 0;
}

int position(const dt_lib_module_t *self)
{
  return 1002;
}

// get the number of days in a given month
static int _time_days_in_month(int year, int month)
{
  switch(month)
  {
    case 2:
      if((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0))
        return 29;
      else
        return 28;
    case 1:
    case 3:
    case 5:
    case 7:
    case 8:
    case 10:
    case 12:
      return 31;
    default:
      return 30;
  }
}

// free blocks
static void _block_free(gpointer data)
{
  dt_lib_timeline_block_t *bloc = (dt_lib_timeline_block_t *)data;
  if(bloc)
  {
    g_free(bloc->name);
    free(bloc->values);
    free(bloc->collect_values);
    free(bloc);
  }
}

// get the width of each bar in the graph, depending of the zoom level
static int _block_get_bar_width(dt_lib_timeline_zooms_t zoom)
{
  if(zoom == DT_LIB_TIMELINE_ZOOM_YEAR)
    return 10;
  else if(zoom == DT_LIB_TIMELINE_ZOOM_4MONTH)
    return 1;
  else if(zoom == DT_LIB_TIMELINE_ZOOM_MONTH)
    return 4;
  else if(zoom == DT_LIB_TIMELINE_ZOOM_10DAY)
    return 1;
  else if(zoom == DT_LIB_TIMELINE_ZOOM_DAY)
    return 5;
  else if(zoom == DT_LIB_TIMELINE_ZOOM_6HOUR)
    return 1;
  else if(zoom == DT_LIB_TIMELINE_ZOOM_HOUR)
    return 2;
  return 1; /* dummy value */
}
// get the number of bar in a block
static int _block_get_bar_count(dt_datetime_t t, dt_lib_timeline_zooms_t zoom)
{
  if(zoom == DT_LIB_TIMELINE_ZOOM_YEAR)
    return 12;
  else if(zoom == DT_LIB_TIMELINE_ZOOM_4MONTH)
  {
    int ti = (t.month - 1) / 4 * 4 + 1;
    return _time_days_in_month(t.year, ti) + _time_days_in_month(t.year, ti + 1)
           + _time_days_in_month(t.year, ti + 2) + _time_days_in_month(t.year, ti + 3);
  }
  else if(zoom == DT_LIB_TIMELINE_ZOOM_MONTH)
    return _time_days_in_month(t.year, t.month);
  else if(zoom == DT_LIB_TIMELINE_ZOOM_10DAY)
    return 120;
  else if(zoom == DT_LIB_TIMELINE_ZOOM_DAY)
    return 24;
  else if(zoom == DT_LIB_TIMELINE_ZOOM_6HOUR)
    return 120;
  else if(zoom == DT_LIB_TIMELINE_ZOOM_HOUR)
    return 60;
  return 1; /* dummy value */
}

static int _block_get_bar_height(int nb, int max_height)
{
  // we want height to be between 0 and max_height
  // small value should have visible height
  return max_height * (1.0 - 2.0 / sqrtf(nb + 4.0));
}

// init time
static dt_datetime_t _time_init()
{
  dt_datetime_t tt = { 0 };
  // we don't want 0 values for month and day
  tt.month = tt.day = 1;
  return tt;
}

// compare times
static int _time_compare_at_zoom(dt_datetime_t t1, dt_datetime_t t2, dt_lib_timeline_zooms_t zoom)
{
  if(t1.year != t2.year) return (t1.year - t2.year);
  if(zoom >= DT_LIB_TIMELINE_ZOOM_YEAR)
  {
    if(t1.month != t2.month) return (t1.month - t2.month);
    if(zoom >= DT_LIB_TIMELINE_ZOOM_4MONTH)
    {
      if(t1.day != t2.day) return (t1.day - t2.day);
      if(zoom >= DT_LIB_TIMELINE_ZOOM_10DAY)
      {
        if(t1.hour / 2 != t2.hour / 2) return (t1.hour / 2 - t2.hour / 2);
        if(zoom >= DT_LIB_TIMELINE_ZOOM_DAY)
        {
          if(t1.hour != t2.hour) return (t1.hour - t2.hour);
          if(zoom >= DT_LIB_TIMELINE_ZOOM_6HOUR)
          {
            if(t1.minute / 3 != t2.minute / 3) return (t1.minute / 3 - t2.minute / 3);
            if(zoom >= DT_LIB_TIMELINE_ZOOM_HOUR)
            {
              if(t1.minute != t2.minute) return (t1.minute - t2.minute);
            }
          }
        }
      }
    }
  }

  return 0;
}
static int _time_compare(dt_datetime_t t1, dt_datetime_t t2)
{
  if(t1.year != t2.year) return (t1.year - t2.year);
  if(t1.month != t2.month) return (t1.month - t2.month);
  if(t1.day != t2.day) return (t1.day - t2.day);
  if(t1.hour != t2.hour) return (t1.hour - t2.hour);
  if(t1.minute != t2.minute) return (t1.minute - t2.minute);

  return 0;
}

// add/subtract value to a time at certain level
static void _time_add(dt_datetime_t *t, int val, dt_lib_timeline_zooms_t level)
{
  if(level == DT_LIB_TIMELINE_ZOOM_YEAR)
  {
    t->year += val;
  }
  else if(level == DT_LIB_TIMELINE_ZOOM_4MONTH)
  {
    t->month += val * 4;
    while(t->month > 12)
    {
      t->year++;
      t->month -= 12;
    }
    while(t->month < 1)
    {
      t->year--;
      t->month += 12;
    }
  }
  else if(level == DT_LIB_TIMELINE_ZOOM_MONTH)
  {
    t->month += val;
    while(t->month > 12)
    {
      t->year++;
      t->month -= 12;
    }
    while(t->month < 1)
    {
      t->year--;
      t->month += 12;
    }
  }
  else if(level == DT_LIB_TIMELINE_ZOOM_10DAY)
  {
    t->day += val * 10;
    while(t->day > _time_days_in_month(t->year, t->month))
    {
      t->day -= _time_days_in_month(t->year, t->month);
      _time_add(t, 1, DT_LIB_TIMELINE_ZOOM_MONTH);
    }
    while(t->day < 1)
    {
      _time_add(t, -1, DT_LIB_TIMELINE_ZOOM_MONTH);
      t->day += _time_days_in_month(t->year, t->month);
    }
  }
  else if(level == DT_LIB_TIMELINE_ZOOM_DAY)
  {
    t->day += val;
    while(t->day > _time_days_in_month(t->year, t->month))
    {
      t->day -= _time_days_in_month(t->year, t->month);
      _time_add(t, 1, DT_LIB_TIMELINE_ZOOM_MONTH);
    }
    while(t->day < 1)
    {
      _time_add(t, -1, DT_LIB_TIMELINE_ZOOM_MONTH);
      t->day += _time_days_in_month(t->year, t->month);
    }
  }
  else if(level == DT_LIB_TIMELINE_ZOOM_6HOUR)
  {
    t->hour += val * 6;
    while(t->hour > 23)
    {
      t->hour -= 24;
      _time_add(t, 1, DT_LIB_TIMELINE_ZOOM_DAY);
    }
    while(t->hour < 0)
    {
      t->hour += 24;
      _time_add(t, -1, DT_LIB_TIMELINE_ZOOM_DAY);
    }
  }
  else if(level == DT_LIB_TIMELINE_ZOOM_HOUR)
  {
    t->hour += val;
    while(t->hour > 23)
    {
      t->hour -= 24;
      _time_add(t, 1, DT_LIB_TIMELINE_ZOOM_DAY);
    }
    while(t->hour < 0)
    {
      t->hour += 24;
      _time_add(t, -1, DT_LIB_TIMELINE_ZOOM_DAY);
    }
  }
  else if(level == DT_LIB_TIMELINE_ZOOM_MINUTE)
  {
    t->minute += val;
    while(t->minute > 59)
    {
      t->minute -= 60;
      _time_add(t, 1, DT_LIB_TIMELINE_ZOOM_HOUR);
    }
    while(t->minute < 0)
    {
      t->minute += 60;
      _time_add(t, -1, DT_LIB_TIMELINE_ZOOM_HOUR);
    }
  }

  // fix for date with year set to 0 (bug ?)
  if(t->year < 0) t->year = 0;
}

// retrieve the date from the position at given zoom level
static dt_datetime_t _time_get_from_pos(int pos, dt_lib_timeline_t *strip)
{
  dt_datetime_t tt = _time_init();

  int x = 0;
  for(const GList *bl = strip->blocks; bl; bl = g_list_next(bl))
  {
    dt_lib_timeline_block_t *blo = bl->data;
    if(pos < x + blo->width)
    {
      tt.year = blo->init.year;
      if(strip->zoom >= DT_LIB_TIMELINE_ZOOM_4MONTH) tt.month = blo->init.month;
      if(strip->zoom >= DT_LIB_TIMELINE_ZOOM_10DAY) tt.day = blo->init.day;
      if(strip->zoom >= DT_LIB_TIMELINE_ZOOM_6HOUR) tt.hour = blo->init.hour;

      if(strip->zoom == DT_LIB_TIMELINE_ZOOM_YEAR)
      {
        tt.month = (pos - x) / _block_get_bar_width(strip->zoom) + 1;
        if(tt.month < 1) tt.month = 1;
      }
      else if(strip->zoom == DT_LIB_TIMELINE_ZOOM_4MONTH)
      {
        int nb = (pos - x) / _block_get_bar_width(strip->zoom) + 1;
        _time_add(&tt, nb, DT_LIB_TIMELINE_ZOOM_DAY);
        if(tt.day < 1) tt.day = 1;
      }
      else if(strip->zoom == DT_LIB_TIMELINE_ZOOM_MONTH)
      {
        tt.day = (pos - x) / _block_get_bar_width(strip->zoom) + 1;
        if(tt.day < 1) tt.day = 1;
      }
      else if(strip->zoom == DT_LIB_TIMELINE_ZOOM_10DAY)
      {
        int nb = (pos - x) / _block_get_bar_width(strip->zoom) + 1;
        _time_add(&tt, nb * 2, DT_LIB_TIMELINE_ZOOM_HOUR);
        if(tt.hour < 0) tt.hour = 0;
      }
      else if(strip->zoom == DT_LIB_TIMELINE_ZOOM_DAY)
      {
        tt.hour = (pos - x) / _block_get_bar_width(strip->zoom) + 1;
        if(tt.hour < 0) tt.hour = 0;
      }
      else if(strip->zoom == DT_LIB_TIMELINE_ZOOM_6HOUR)
      {
        int nb = (pos - x) / _block_get_bar_width(strip->zoom) + 1;
        _time_add(&tt, nb * 3, DT_LIB_TIMELINE_ZOOM_MINUTE);
        if(tt.minute < 0) tt.minute = 0;
      }
      else if(strip->zoom == DT_LIB_TIMELINE_ZOOM_HOUR)
      {
        int nb = (pos - x) / _block_get_bar_width(strip->zoom) + 1;
        _time_add(&tt, nb, DT_LIB_TIMELINE_ZOOM_MINUTE);
        if(tt.minute < 0) tt.minute = 0;
      }

      return tt;
    }
    x += blo->width + 2;
  }

  return tt;
}

static dt_datetime_t _time_compute_offset_for_zoom(int pos, dt_lib_timeline_t *strip,
                                                            dt_lib_timeline_zooms_t new_zoom)
{
  if(new_zoom == strip->zoom) return strip->time_pos;

  dt_datetime_t tt = _time_get_from_pos(pos, strip);

  // we search the number of the bloc under pos
  int bloc_nb = 0;
  int x = 0;
  GList *bl;
  for(bl = strip->blocks; bl; bl = g_list_next(bl))
  {
    dt_lib_timeline_block_t *blo = bl->data;
    if(pos < x + blo->width) break;
    x += blo->width + 2;
    bloc_nb++;
  }
  if(!bl)
  {
    // we are outside the timeline
  }

  // we translate to the new date_pos at new_zoom level
  _time_add(&tt, -bloc_nb, new_zoom);

  // we need to verify that we are not out of the bounds
  if(_time_compare(tt, strip->time_mini) < 0) tt = strip->time_mini;
  return tt;
}

static gchar *_time_format_for_ui(dt_datetime_t t, dt_lib_timeline_zooms_t zoom)
{
  if(zoom == DT_LIB_TIMELINE_ZOOM_YEAR)
  {
    return g_strdup_printf("%04d", t.year);
  }
  else if(zoom == DT_LIB_TIMELINE_ZOOM_4MONTH)
  {
    int x = (t.month - 1) / 4 * 4 + 1; // This is NOT a no-op (rounding)
    return g_strdup_printf("(%02d-%02d)/%04d", x, x + 3, t.year);
  }
  else if(zoom == DT_LIB_TIMELINE_ZOOM_MONTH)
  {
    return g_strdup_printf("%02d/%04d", t.month, t.year);
  }
  else if(zoom == DT_LIB_TIMELINE_ZOOM_10DAY)
  {
    int x = (t.day - 1) / 10 * 10 + 1; // This is NOT a no-op (rounding)
    int x2 = x + 9;
    if(x2 == 30) x2 = _time_days_in_month(t.year, t.month);
    return g_strdup_printf("(%02d-%02d)/%02d/%02d", x, x2, t.month, t.year % 100);
  }
  else if(zoom == DT_LIB_TIMELINE_ZOOM_DAY)
  {
    return g_strdup_printf("%02d/%02d/%02d", t.day, t.month, t.year % 100);
  }
  else if(zoom == DT_LIB_TIMELINE_ZOOM_6HOUR)
  {
    return g_strdup_printf("%02d/%02d/%02d (h%02d-%02d)", t.day, t.month, t.year % 100, t.hour / 6 * 6,
                           t.hour / 6 * 6 + 5);
  }
  else if(zoom == DT_LIB_TIMELINE_ZOOM_HOUR)
  {
    return g_strdup_printf("%02d/%02d/%02d h%02d", t.day, t.month, t.year % 100, t.hour);
  }
  else if(zoom == DT_LIB_TIMELINE_ZOOM_10MINUTE)
  {
    return g_strdup_printf("%02d/%02d/%02d %02dh(%02d-%02d)", t.day, t.month, t.year % 100, t.hour,
                           t.minute / 10 * 10, t.minute / 10 * 10 + 9);
  }
  else if(zoom == DT_LIB_TIMELINE_ZOOM_MINUTE)
  {
    return g_strdup_printf("%02d/%02d/%02d %02d:%02d", t.day, t.month, t.year % 100, t.hour, t.minute);
  }

  return NULL;
}
static GTimeSpan _time_format_for_db(dt_datetime_t t, dt_lib_timeline_zooms_t zoom)
{
  dt_datetime_t lt = t;
  switch(zoom)
  {
    case DT_LIB_TIMELINE_ZOOM_YEAR:
      lt.month = 1;
    case DT_LIB_TIMELINE_ZOOM_4MONTH:
    case DT_LIB_TIMELINE_ZOOM_MONTH:
      lt.day = 1;
    case DT_LIB_TIMELINE_ZOOM_10DAY:
    case DT_LIB_TIMELINE_ZOOM_DAY:
      lt.hour = 0;
    case DT_LIB_TIMELINE_ZOOM_6HOUR:
    case DT_LIB_TIMELINE_ZOOM_HOUR:
      lt.minute = 0;
    case DT_LIB_TIMELINE_ZOOM_10MINUTE:
    case DT_LIB_TIMELINE_ZOOM_MINUTE:
      lt.second = 0;
      return dt_datetime_numbers_to_gtimespan(&lt);
    default:
      return 0;
  }
}

static gchar *_time_format_for_collect(dt_datetime_t t, dt_lib_timeline_zooms_t zoom)
{
  if(zoom == DT_LIB_TIMELINE_ZOOM_YEAR)
    return g_strdup_printf("%04d", t.year);
  else if(zoom == DT_LIB_TIMELINE_ZOOM_4MONTH || zoom == DT_LIB_TIMELINE_ZOOM_MONTH)
    return g_strdup_printf("%04d:%02d", t.year, t.month);
  else if(zoom == DT_LIB_TIMELINE_ZOOM_10DAY || zoom == DT_LIB_TIMELINE_ZOOM_DAY)
    return g_strdup_printf("%04d:%02d:%02d", t.year, t.month, t.day);
  else if(zoom == DT_LIB_TIMELINE_ZOOM_6HOUR || zoom == DT_LIB_TIMELINE_ZOOM_HOUR)
    return g_strdup_printf("%04d:%02d:%02d %02d", t.year, t.month, t.day, t.hour);
  else if(zoom == DT_LIB_TIMELINE_ZOOM_10MINUTE || zoom == DT_LIB_TIMELINE_ZOOM_MINUTE)
    return g_strdup_printf("%04d:%02d:%02d %02d:%02d", t.year, t.month, t.day, t.hour, t.minute);

  return NULL;
}

// get all the datetimes from the database
static gboolean _time_read_bounds_from_db(dt_lib_module_t *self)
{
  dt_lib_timeline_t *strip = (dt_lib_timeline_t *)self->data;

  sqlite3_stmt *stmt;
  const char *query = "SELECT MIN(datetime_taken) AS dt FROM main.images WHERE datetime_taken > 1";
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);

  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    strip->has_selection =
      dt_datetime_gtimespan_to_numbers(&strip->time_mini, sqlite3_column_int64(stmt, 0));
  }
  else
    strip->has_selection = FALSE;
  sqlite3_finalize(stmt);

  const char *query2 = "SELECT MAX(datetime_taken) AS dt FROM main.images";
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query2, -1, &stmt, NULL);

  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    dt_datetime_gtimespan_to_numbers(&strip->time_maxi, sqlite3_column_int64(stmt, 0));
  }
  sqlite3_finalize(stmt);

  return TRUE;
}

// get all the datetimes from the actual collection
static gboolean _time_read_bounds_from_collection(dt_lib_module_t *self)
{
  dt_lib_timeline_t *strip = (dt_lib_timeline_t *)self->data;

  sqlite3_stmt *stmt;
  // clang-format off
  const char *query = "SELECT MIN(db.datetime_taken) AS dt "
                      "FROM main.images AS db, memory.collected_images AS col "
                      "WHERE db.id=col.imgid AND db.datetime_taken > 1";
  // clang-format on
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);

  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    strip->has_selection =
      dt_datetime_gtimespan_to_numbers(&strip->start_t, sqlite3_column_int64(stmt, 0));
  }
  else
    strip->has_selection = FALSE;
  sqlite3_finalize(stmt);

  // clang-format off
  const char *query2 = "SELECT MAX(db.datetime_taken) AS dt "
                       "FROM main.images AS db, memory.collected_images AS col "
                       "WHERE db.id=col.imgid";
  // clang-format on
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query2, -1, &stmt, NULL);

  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    dt_datetime_gtimespan_to_numbers(&strip->stop_t, sqlite3_column_int64(stmt, 0));
  }
  sqlite3_finalize(stmt);

  return TRUE;
}

// get the time of the first block of the strip in order to show the selection
static dt_datetime_t _selection_scroll_to(dt_datetime_t t, dt_lib_timeline_t *strip)
{
  dt_datetime_t tt = t;
  int nb = strip->panel_width / 122;

  for(int i = 0; i < nb; i++)
  {
    // we ensure that we are not before the strip bound
    if(_time_compare(tt, strip->time_mini) <= 0) return strip->time_mini;

    // and we don't want to display blocks after the bounds too
    dt_datetime_t ttt = tt;
    _time_add(&ttt, nb - 1, strip->zoom);
    if(_time_compare(ttt, strip->time_maxi) <= 0) return tt;

    // we test the previous date
    _time_add(&tt, -1, strip->zoom);
  }
  // if we are here that means we fail to scroll... why ?
  return t;
}

// computes blocks at the current zoom level
static int _block_get_at_zoom(dt_lib_module_t *self, int width)
{
  dt_lib_timeline_t *strip = (dt_lib_timeline_t *)self->data;

  // we erase previous blocks if any
  if(strip->blocks)
  {
    g_list_free_full(strip->blocks, _block_free);
    strip->blocks = NULL;
  }

  int w = 0;

  // if selection start/stop if lower than the beginning of the strip
  if(_time_compare_at_zoom(strip->start_t, strip->time_pos, strip->zoom) < 0) strip->start_x = -2;
  if(_time_compare_at_zoom(strip->stop_t, strip->time_pos, strip->zoom) < 0) strip->stop_x = -1;

  sqlite3_stmt *stmt;
  // clang-format off
  gchar *query = g_strdup_printf("SELECT db.datetime_taken AS dt,"
                                 " col.imgid FROM main.images AS db "
                                 "LEFT JOIN memory.collected_images AS col ON db.id=col.imgid "
                                 "WHERE dt > %ld "
                                 "ORDER BY dt ASC",
                                 (long int)_time_format_for_db(strip->time_pos, strip->zoom));
  // clang-format on
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);

  dt_datetime_t tx;
  int id = 0;
  int stat = sqlite3_step(stmt);
  if(stat == SQLITE_ROW)
  {
    dt_datetime_gtimespan_to_numbers(&tx, sqlite3_column_int64(stmt, 0));
    id = sqlite3_column_int(stmt, 1);
  }
  else
    return 0;

  dt_datetime_t tt = strip->time_pos;
  // we round correctly this date
  if(strip->zoom <= DT_LIB_TIMELINE_ZOOM_HOUR)
  {
    tt.minute = 0;
    if(strip->zoom <= DT_LIB_TIMELINE_ZOOM_6HOUR)
    {
      tt.hour = tt.hour / 6 * 6;
      if(strip->zoom <= DT_LIB_TIMELINE_ZOOM_DAY)
      {
        tt.hour = 0;
        if(strip->zoom <= DT_LIB_TIMELINE_ZOOM_10DAY)
        {
          tt.day = (tt.day - 1) / 10 * 10 + 1;
          if(strip->zoom <= DT_LIB_TIMELINE_ZOOM_MONTH)
          {
            tt.day = 1;
            if(strip->zoom <= DT_LIB_TIMELINE_ZOOM_4MONTH)
            {
              tt.month = (tt.month - 1) / 4 * 4 + 1;
              if(strip->zoom <= DT_LIB_TIMELINE_ZOOM_YEAR)
              {
                tt.month = 1;
              }
            }
          }
        }
      }
    }
  }

  while(TRUE)
  {
    dt_lib_timeline_block_t *bloc = (dt_lib_timeline_block_t *)calloc(1, sizeof(dt_lib_timeline_block_t));
    bloc->name = _time_format_for_ui(tt, strip->zoom);
    bloc->init = tt;
    bloc->values_count = _block_get_bar_count(tt, strip->zoom);
    bloc->values = (int *)calloc(bloc->values_count, sizeof(int));
    bloc->collect_values = (int *)calloc(bloc->values_count, sizeof(int));
    bloc->width = bloc->values_count * _block_get_bar_width(strip->zoom);

    if(strip->zoom == DT_LIB_TIMELINE_ZOOM_YEAR)
      tt.month = 1;
    else if(strip->zoom == DT_LIB_TIMELINE_ZOOM_4MONTH || strip->zoom == DT_LIB_TIMELINE_ZOOM_MONTH)
      tt.day = 1;
    else if(strip->zoom == DT_LIB_TIMELINE_ZOOM_10DAY || strip->zoom == DT_LIB_TIMELINE_ZOOM_DAY)
      tt.hour = 0;
    else if(strip->zoom == DT_LIB_TIMELINE_ZOOM_6HOUR || strip->zoom == DT_LIB_TIMELINE_ZOOM_HOUR)
      tt.minute = 0;
    // we count the number of photos per month
    for(int i = 0; i < bloc->values_count; i++)
    {

      // if it's the selection start/stop time, we set the x value accordindgly
      if(_time_compare_at_zoom(strip->start_t, tt, strip->zoom) == 0)
        strip->start_x = w + i * _block_get_bar_width(strip->zoom);
      if(_time_compare_at_zoom(strip->stop_t, tt, strip->zoom) == 0)
        strip->stop_x = w + (i + 1) * _block_get_bar_width(strip->zoom);
      // and we count how many photos we have for this time
      while(stat == SQLITE_ROW && _time_compare_at_zoom(tt, tx, strip->zoom) == 0)
      {
        bloc->values[i]++;
        if(id > 0) bloc->collect_values[i]++;
        stat = sqlite3_step(stmt);
        dt_datetime_gtimespan_to_numbers(&tx, sqlite3_column_int64(stmt, 0));
        id = sqlite3_column_int(stmt, 1);
      }

      // and we jump to next date
      // if(i+1 >= bloc->values_count) break;
      if(strip->zoom == DT_LIB_TIMELINE_ZOOM_YEAR)
        _time_add(&tt, 1, DT_LIB_TIMELINE_ZOOM_MONTH);
      else if(strip->zoom == DT_LIB_TIMELINE_ZOOM_4MONTH || strip->zoom == DT_LIB_TIMELINE_ZOOM_MONTH)
        _time_add(&tt, 1, DT_LIB_TIMELINE_ZOOM_DAY);
      else if(strip->zoom == DT_LIB_TIMELINE_ZOOM_10DAY)
        _time_add(&tt, 2, DT_LIB_TIMELINE_ZOOM_HOUR);
      else if(strip->zoom == DT_LIB_TIMELINE_ZOOM_DAY)
        _time_add(&tt, 1, DT_LIB_TIMELINE_ZOOM_HOUR);
      else if(strip->zoom == DT_LIB_TIMELINE_ZOOM_6HOUR)
        _time_add(&tt, 3, DT_LIB_TIMELINE_ZOOM_MINUTE);
      else if(strip->zoom == DT_LIB_TIMELINE_ZOOM_HOUR)
        _time_add(&tt, 1, DT_LIB_TIMELINE_ZOOM_MINUTE);
    }
    strip->blocks = g_list_append(strip->blocks, bloc);

    w += bloc->width + 2;
    if(w > width || stat != SQLITE_ROW)
    {
      // if selection start/stop times are greater than the last time
      if(_time_compare_at_zoom(strip->start_t, tt, strip->zoom) >= 0) strip->start_x = strip->panel_width + 1;
      if(_time_compare_at_zoom(strip->stop_t, tt, strip->zoom) >= 0) strip->stop_x = strip->panel_width + 2;
      break;
    }
  }
  sqlite3_finalize(stmt);
  g_free(query);

  // and we return the width of the strip
  return w;
}

static gboolean _time_is_visible(dt_datetime_t t, dt_lib_timeline_t *strip)
{
  // first case, the date is before the strip
  if(_time_compare_at_zoom(t, strip->time_pos, strip->zoom) < 0) return FALSE;

  // now the end of the visible strip
  // if the date is in the last block, we consider it's outside, because the last block can be partially hidden
  GList *bl = g_list_last(strip->blocks);
  if(bl)
  {
    dt_lib_timeline_block_t *blo = bl->data;
    if(_time_compare_at_zoom(t, blo->init, strip->zoom) > 0) return FALSE;
  }

  return TRUE;
}

static void _lib_timeline_collection_changed(gpointer instance, dt_collection_change_t query_change,
                                             dt_collection_properties_t changed_property, gpointer imgs, int next,
                                             gpointer user_data)
{
  dt_lib_gui_queue_update(user_data);
}

void gui_update(dt_lib_module_t *self)
{
  dt_lib_timeline_t *strip = (dt_lib_timeline_t *)self->data;

  // we read the collect bounds
  _time_read_bounds_from_collection(self);

  // if the start in not visible, we recompute the start of the strip
  if(!_time_is_visible(strip->start_t, strip))
  {
    strip->time_pos = _selection_scroll_to(strip->start_t, strip);
  }

  // in any case we redraw the strip (to reflect any selected image change)
  cairo_surface_destroy(strip->surface);
  strip->surface = NULL;
}


static gboolean _timespec_has_date_only(const char *const spec)
{
  // spec could be "YYYY:MM", "YYYY:MM:DD", "YYYY:MM:DD HH", etc.
  return strlen(spec) <= 10; // is string YYYY:MM:DD or shorter?
}

// add the selected portions to the collect
static void _selection_collect(dt_lib_timeline_t *strip, dt_lib_timeline_mode_t mode)
{
  // if the last rule is date-time type or is empty, we modify it
  // else we add a new rule date-time rule

  int new_rule = 0;
  const int nb_rules = dt_conf_get_int("plugins/lighttable/collect/num_rules");
  if(nb_rules > 0 && mode != DT_LIB_TIMELINE_MODE_RESET)
  {
    char confname[200] = { 0 };
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/item%1d", nb_rules - 1);
    dt_collection_properties_t prop = dt_conf_get_int(confname);
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/mode%1d", nb_rules - 1);
    int rmode = dt_conf_get_int(confname);
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/string%1d", nb_rules - 1);
    gchar *string = dt_conf_get_string(confname);
    string = g_strstrip(string);
    if(((prop == DT_COLLECTION_PROP_TIME || prop == DT_COLLECTION_PROP_DAY) && rmode == 0)
       || !string || strlen(string) == 0 || g_strcmp0(string, "%") == 0)
      new_rule = nb_rules - 1;
    else
      new_rule = nb_rules;
    g_free(string);
  }

  // we construct the rule
  gchar *coll = NULL;
  gboolean date_only = FALSE;
  if(strip->start_x == strip->stop_x)
  {
    coll = _time_format_for_collect(strip->start_t, (strip->zoom + 1) / 2 * 2 + 2);
    date_only = _timespec_has_date_only(coll);
  }
  else
  {
    dt_datetime_t start = strip->start_t;
    dt_datetime_t stop = strip->stop_t;
    if(strip->start_x > strip->stop_x)
    {
      // we exchange the values
      start = strip->stop_t;
      stop = strip->start_t;
    }
    gchar *d1 = _time_format_for_collect(start, (strip->zoom + 1) / 2 * 2 + 2);
    gchar *d2 = _time_format_for_collect(stop, (strip->zoom + 1) / 2 * 2 + 2);
    if(d1 && d2)
    {
      coll = g_strdup_printf("[%s;%s]", d1, d2);
      date_only = _timespec_has_date_only(d1) && _timespec_has_date_only(d2);
    }
    g_free(d1);
    g_free(d2);
  }

  if(coll)
  {
    dt_conf_set_int("plugins/lighttable/collect/num_rules", new_rule + 1);
    char confname[200] = { 0 };
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/item%1d", new_rule);
    dt_conf_set_int(confname, date_only ? DT_COLLECTION_PROP_DAY : DT_COLLECTION_PROP_TIME);
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/mode%1d", new_rule);
    dt_conf_set_int(confname, 0);
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/string%1d", new_rule);
    dt_conf_set_string(confname, coll);
    g_free(coll);

    dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_NEW_QUERY, DT_COLLECTION_PROP_UNDEF,
                               NULL);
  }
}

static gboolean _lib_timeline_draw_callback(GtkWidget *widget, cairo_t *wcr, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_timeline_t *strip = (dt_lib_timeline_t *)self->data;

  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  const int32_t width = allocation.width;
  const int32_t height = allocation.height;

  // windows could have been expanded for example, we need to create a new surface of the good size and redraw
  if(width != strip->panel_width || height != strip->panel_height)
  {
    // if it's the first show, we need to recompute the scroll too
    if(strip->panel_width == 0 || strip->panel_height == 0)
    {
      strip->panel_width = width;
      strip->panel_height = height;
      strip->time_pos = _selection_scroll_to(strip->start_t, strip);
    }
    if(strip->surface)
    {
      cairo_surface_destroy(strip->surface);
      strip->surface = NULL;
    }
  }

  // create the persistent surface if it does not exists.
  if(!strip->surface)
  {
    strip->surface_width = _block_get_at_zoom(self, width);
    strip->panel_width = width;
    strip->panel_height = height;
    strip->surface_height = allocation.height;

    // we set the width of a unit (bar) in the drawing (depending of the zoom level)
    int wu = _block_get_bar_width(strip->zoom);

    strip->surface = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, allocation.width, allocation.height);

    // get cairo drawing handle
    cairo_t *cr = cairo_create(strip->surface);

    /* fill background */
    dt_gui_gtk_set_source_rgb(cr, DT_GUI_COLOR_FILMSTRIP_BG);
    cairo_paint(cr);

    // draw content depending of zoom level
    int posx = 0;
    for(const GList *bl = strip->blocks; bl; bl = g_list_next(bl))
    {
      dt_lib_timeline_block_t *blo = bl->data;

      // width of this block
      int wb = blo->values_count * wu;

      cairo_text_extents_t te;
      dt_gui_gtk_set_source_rgb(cr, DT_GUI_COLOR_TIMELINE_TEXT_FG);
      cairo_set_font_size(cr, 10 * (1 + (darktable.gui->dpi_factor - 1) / 2));
      cairo_text_extents(cr, blo->name, &te);
      int bh = allocation.height - te.height - 4;
      cairo_move_to(cr, posx + (wb - te.width) / 2 - te.x_bearing, allocation.height - 2);
      cairo_show_text(cr, blo->name);

      dt_gui_gtk_set_source_rgb(cr, DT_GUI_COLOR_TIMELINE_BG);
      cairo_rectangle(cr, posx, 0, wb, bh);
      cairo_fill(cr);

      for(int i = 0; i < blo->values_count; i++)
      {
        dt_gui_gtk_set_source_rgba(cr, DT_GUI_COLOR_TIMELINE_FG, 0.5);
        int h = _block_get_bar_height(blo->values[i], bh);
        cairo_rectangle(cr, posx + (i * wu), bh - h, wu, h);
        cairo_fill(cr);
        dt_gui_gtk_set_source_rgba(cr, DT_GUI_COLOR_TIMELINE_FG, 1.0);
        h = _block_get_bar_height(blo->collect_values[i], bh);
        cairo_rectangle(cr, posx + (i * wu), bh - h, wu, h);
        cairo_fill(cr);
      }

      posx += wb + 2;
      if(posx >= allocation.width) break;
    }

    // copy back the new content into the cairo handle of the draw callback
    cairo_destroy(cr);
  }
  cairo_set_source_surface(wcr, strip->surface, 0, 0);
  cairo_paint(wcr);

  // we draw the selection
  if(strip->has_selection)
  {
    int stop = 0;
    int start = 0;
    if(strip->selecting)
      stop = strip->current_x;
    else
      stop = strip->stop_x;
    if(stop > strip->start_x)
      start = strip->start_x;
    else
    {
      start = stop;
      stop = strip->start_x;
    }
    // we verify that the selection is not in a hidden zone
    if(!(start < 0 && stop < 0) && !(start > strip->panel_width && stop > strip->panel_width))
    {
      // we draw the selection
      if(start >= 0)
      {
        // dt_gui_gtk_set_source_rgb(wcr, DT_GUI_COLOR_THUMBNAIL_HOVER_BG);
        dt_gui_gtk_set_source_rgba(wcr, DT_GUI_COLOR_TIMELINE_FG, 0.8);
        cairo_move_to(wcr, start, 0);
        cairo_line_to(wcr, start, allocation.height);
        cairo_stroke(wcr);
        dt_gui_gtk_set_source_rgba(wcr, DT_GUI_COLOR_FILMSTRIP_BG, 0.3);
        cairo_move_to(wcr, start, 0);
        cairo_line_to(wcr, start, allocation.height);
        cairo_stroke(wcr);
      }
      dt_gui_gtk_set_source_rgba(wcr, DT_GUI_COLOR_TIMELINE_FG, 0.5);
      cairo_rectangle(wcr, start, 0, stop - start, allocation.height);
      cairo_fill(wcr);
      if(stop <= strip->panel_width)
      {
        dt_gui_gtk_set_source_rgba(wcr, DT_GUI_COLOR_TIMELINE_FG, 0.8);
        cairo_move_to(wcr, stop, 0);
        cairo_line_to(wcr, stop, allocation.height);
        cairo_stroke(wcr);
        dt_gui_gtk_set_source_rgba(wcr, DT_GUI_COLOR_FILMSTRIP_BG, 0.3);
        cairo_move_to(wcr, stop, 0);
        cairo_line_to(wcr, stop, allocation.height);
        cairo_stroke(wcr);
      }
    }
  }

  // we draw the line under cursor and the date-time
  if(strip->in && strip->current_x > 0)
  {
    dt_datetime_t tt;
    if(strip->selecting)
      tt = strip->stop_t;
    else
      tt = _time_get_from_pos(strip->current_x, strip);

    // we don't display NULL date (if it's outside bounds)
    if(_time_compare(tt, _time_init()) != 0)
    {
      dt_gui_gtk_set_source_rgb(wcr, DT_GUI_COLOR_TIMELINE_TEXT_BG);
      cairo_move_to(wcr, strip->current_x, 0);
      cairo_line_to(wcr, strip->current_x, allocation.height);
      cairo_stroke(wcr);
      gchar *dte = _time_format_for_ui(tt, strip->precision);
      cairo_text_extents_t te2;
      cairo_set_font_size(wcr, 10 * darktable.gui->dpi_factor);
      cairo_text_extents(wcr, dte, &te2);
      cairo_rectangle(wcr, strip->current_x, 8, te2.width + 4, te2.height + 4);
      dt_gui_gtk_set_source_rgb(wcr, DT_GUI_COLOR_TIMELINE_TEXT_BG);
      cairo_fill(wcr);
      cairo_move_to(wcr, strip->current_x + 2, 10 + te2.height);
      dt_gui_gtk_set_source_rgb(wcr, DT_GUI_COLOR_TIMELINE_TEXT_FG);
      cairo_show_text(wcr, dte);
      g_free(dte);
    }
  }

  return TRUE;
}

static gboolean _lib_timeline_button_press_callback(GtkWidget *w, GdkEventButton *e, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_timeline_t *strip = (dt_lib_timeline_t *)self->data;

  if(e->button == 1)
  {
    if(e->type == GDK_BUTTON_PRESS)
    {
      if(e->x - strip->start_x < 2 && e->x - strip->start_x > -2)
      {
        strip->start_x = strip->stop_x;
        strip->start_t = strip->stop_t;
        strip->stop_x = e->x;
        strip->stop_t = _time_get_from_pos(e->x, strip);
        strip->move_edge = TRUE;
      }
      else if(e->x - strip->stop_x < 2 && e->x - strip->stop_x > -2)
      {
        strip->stop_x = e->x;
        strip->stop_t = _time_get_from_pos(e->x, strip);
        strip->move_edge = TRUE;
      }
      else
      {
        strip->start_x = strip->stop_x = e->x;
        dt_datetime_t tt = _time_get_from_pos(e->x, strip);
        if(_time_compare(tt, _time_init()) == 0)
          strip->start_t = strip->stop_t = strip->time_maxi; //we are past the end so selection extends until the end
        else
          strip->start_t = strip->stop_t = tt;
        strip->move_edge = FALSE;
      }
      strip->selecting = TRUE;
      strip->has_selection = TRUE;
      gtk_widget_queue_draw(strip->timeline);
    }
  }
  else if(e->button == 3)
  {
    // we remove the last rule if it's a datetime one
    const int nb_rules = dt_conf_get_int("plugins/lighttable/collect/num_rules");
    if(nb_rules > 0)
    {
      char confname[200] = { 0 };
      snprintf(confname, sizeof(confname), "plugins/lighttable/collect/item%1d", nb_rules - 1);
      if(dt_conf_get_int(confname) == DT_COLLECTION_PROP_TIME)
      {
        dt_conf_set_int("plugins/lighttable/collect/num_rules", nb_rules - 1);
        dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, DT_COLLECTION_PROP_UNDEF,
                                   NULL);

        strip->selecting = FALSE;
      }
    }
  }

  return FALSE;
}

static gboolean _lib_timeline_button_release_callback(GtkWidget *w, GdkEventButton *e, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_timeline_t *strip = (dt_lib_timeline_t *)self->data;

  if(strip->selecting)
  {
    strip->stop_x = e->x;
    dt_datetime_t tt = _time_get_from_pos(e->x, strip);
    if(_time_compare(tt, _time_init()) == 0)
      strip->stop_t = strip->time_maxi; //we are past the end so selection extends until the end
    else
    {
      strip->stop_t = tt;
      // we want to be at the "end" of this date
      if(strip->zoom <= DT_LIB_TIMELINE_ZOOM_DAY)
      {
	strip->stop_t.minute = 59;
	if(strip->zoom <= DT_LIB_TIMELINE_ZOOM_MONTH)
	{
	  strip->stop_t.hour = 23;
	  if(strip->zoom <= DT_LIB_TIMELINE_ZOOM_YEAR)
	  {
	    strip->stop_t.day = _time_days_in_month(strip->stop_t.year, strip->stop_t.month);
	  }
	}
      }
    }
    strip->selecting = FALSE;

    if(!strip->move_edge && dt_modifier_is(e->state, GDK_SHIFT_MASK))
      _selection_collect(strip, DT_LIB_TIMELINE_MODE_RESET);
    else
      _selection_collect(strip, DT_LIB_TIMELINE_MODE_AND);
    gtk_widget_queue_draw(strip->timeline);
  }

  return TRUE;
}

static void _selection_start(dt_action_t *action)
{
  dt_lib_timeline_t *strip = dt_action_lib(action)->data;

  strip->start_x = strip->current_x;
  dt_datetime_t tt = _time_get_from_pos(strip->current_x, strip);
  if(_time_compare(tt, _time_init()) == 0)
    strip->start_t = strip->time_maxi; //we are past the end so selection extends until the end
  else
    strip->start_t = _time_get_from_pos(strip->current_x, strip);
  strip->stop_x = strip->start_x;
  strip->stop_t = strip->start_t;
  strip->selecting = TRUE;
  strip->has_selection = TRUE;

  gtk_widget_queue_draw(strip->timeline);
}
static void _selection_stop(dt_action_t *action)
{
  dt_lib_timeline_t *strip = dt_action_lib(action)->data;
  dt_datetime_t tt = _time_get_from_pos(strip->current_x, strip);

  strip->stop_x = strip->current_x;
  if(_time_compare(tt, _time_init()) == 0)
    strip->stop_t = strip->time_maxi; //we are past the end so selection extends until the end
  else
  {
    strip->stop_t = tt;
    // we want to be at the "end" of this date
    if(strip->zoom < DT_LIB_TIMELINE_ZOOM_HOUR)
    {
      strip->stop_t.minute = 59;
      if(strip->zoom < DT_LIB_TIMELINE_ZOOM_DAY)
      {
	strip->stop_t.hour = 23;
	if(strip->zoom < DT_LIB_TIMELINE_ZOOM_MONTH)
	{
	  strip->stop_t.day = _time_days_in_month(strip->stop_t.year, strip->stop_t.month);
	}
      }
    }
  }

  strip->selecting = FALSE;
  _selection_collect(strip, DT_LIB_TIMELINE_MODE_AND);
  gtk_widget_queue_draw(strip->timeline);
}

static gboolean _block_autoscroll(gpointer user_data)
{
  // this function is called repetidly until the pointer is not more in the autoscoll zone
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_timeline_t *strip = (dt_lib_timeline_t *)self->data;

  if(!strip->in)
  {
    strip->autoscroll = FALSE;
    return FALSE;
  }

  int move = 0;
  if(strip->current_x < 10)
    move = -1;
  else if(strip->current_x > strip->panel_width - 10)
    move = 1;

  if(move == 0)
  {
    strip->autoscroll = FALSE;
    return FALSE;
  }

  dt_datetime_t old_pos = strip->time_pos;
  _time_add(&(strip->time_pos), move, strip->zoom);
  // we ensure that the fimlstrip stay in the bounds
  dt_datetime_t tt = _selection_scroll_to(strip->time_pos, strip);
  if(_time_compare(tt, strip->time_pos) != 0)
  {
    strip->time_pos = old_pos; //no scroll, so we restore the previous position
    strip->autoscroll = FALSE;
    return FALSE;
  }

  cairo_surface_destroy(strip->surface);
  strip->surface = NULL;
  gtk_widget_queue_draw(strip->timeline);
  return TRUE;
}

static gboolean _lib_timeline_motion_notify_callback(GtkWidget *w, GdkEventMotion *e, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_timeline_t *strip = (dt_lib_timeline_t *)self->data;

  strip->in = TRUE;

  // auto-scroll if cursor is at one end of the panel
  if((e->x < 10 || e->x > strip->panel_width - 10) && !strip->autoscroll)
  {
    // first scroll immediately and then every 400ms until cursor quit the "auto-zone"
    if(_block_autoscroll(user_data))
    {
      strip->autoscroll = TRUE;
      g_timeout_add(400, _block_autoscroll, user_data);
    }
  }

  strip->current_x = e->x;

  if(strip->selecting)
  {
    strip->stop_x = e->x;
    strip->stop_t = _time_get_from_pos(e->x, strip);
    dt_control_change_cursor(GDK_LEFT_PTR);
  }
  else
  {
    // we change the cursor if we are close enough of a selection limit
    if(e->x - strip->start_x < 2 && e->x - strip->start_x > -2)
    {
      dt_control_change_cursor(GDK_LEFT_SIDE);
    }
    else if(e->x - strip->stop_x < 2 && e->x - strip->stop_x > -2)
    {
      dt_control_change_cursor(GDK_RIGHT_SIDE);
    }
    else
    {
      dt_control_change_cursor(GDK_LEFT_PTR);
    }
  }
  gtk_widget_queue_draw(strip->timeline);
  return TRUE;
}

static gboolean _lib_timeline_scroll_callback(GtkWidget *w, GdkEventScroll *e, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_timeline_t *strip = (dt_lib_timeline_t *)self->data;

  // zoom change (with Ctrl key)
  if(dt_modifier_is(e->state, GDK_CONTROL_MASK))
  {
    int z = strip->zoom;
    int delta_y = 0;
    if(dt_gui_get_scroll_unit_deltas(e, NULL, &delta_y))
    {
      if(delta_y < 0)
      {
        if(z != DT_LIB_TIMELINE_ZOOM_HOUR) z++;
      }
      else if(delta_y > 0)
      {
        if(z != DT_LIB_TIMELINE_ZOOM_YEAR) z--;
      }
    }

    // if the zoom as changed, we need to recompute blocks and redraw
    if(z != strip->zoom)
    {
      dt_conf_set_int("plugins/lighttable/timeline/last_zoom", z);
      strip->time_pos = _time_compute_offset_for_zoom(strip->current_x, strip, z);
      strip->zoom = z;
      if(z % 2 == 0)
        strip->precision = z + 2;
      else
        strip->precision = z + 1;
      cairo_surface_destroy(strip->surface);
      strip->surface = NULL;
      gtk_widget_queue_draw(strip->timeline);
    }
    return TRUE;
  }
  else
  {
    int delta;
    if(dt_gui_get_scroll_unit_delta(e, &delta))
    {
      int move = delta;
      if(dt_modifier_is(e->state, GDK_SHIFT_MASK)) move *= 2;

      _time_add(&(strip->time_pos), move, strip->zoom);
      // we ensure that the fimlstrip stay in the bounds
      strip->time_pos = _selection_scroll_to(strip->time_pos, strip);

      cairo_surface_destroy(strip->surface);
      strip->surface = NULL;
      gtk_widget_queue_draw(strip->timeline);
    }
  }
  return FALSE;
}

static gboolean _lib_timeline_mouse_leave_callback(GtkWidget *w, GdkEventCrossing *e, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_timeline_t *strip = (dt_lib_timeline_t *)self->data;

  strip->in = FALSE;

  gtk_widget_queue_draw(strip->timeline);
  return TRUE;
}

void gui_init(dt_lib_module_t *self)
{
  /* initialize ui widgets */
  dt_lib_timeline_t *d = (dt_lib_timeline_t *)calloc(1, sizeof(dt_lib_timeline_t));
  self->data = (void *)d;

  d->zoom = CLAMP(dt_conf_get_int("plugins/lighttable/timeline/last_zoom"), 0, 8);
  if(d->zoom % 2 == 0)
    d->precision = d->zoom + 2;
  else
    d->precision = d->zoom + 1;

  d->time_mini = _time_init();
  d->time_maxi = _time_init();
  d->start_t = _time_init();
  d->stop_t = _time_init();

  _time_read_bounds_from_db(self);
  d->time_pos = d->time_mini;
  /* creating drawing area */
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  /* creating timeline box*/
  d->timeline = gtk_event_box_new();

  gtk_widget_add_events(d->timeline, GDK_POINTER_MOTION_MASK | GDK_BUTTON_PRESS_MASK
                                         | GDK_BUTTON_RELEASE_MASK | darktable.gui->scroll_mask
                                         | GDK_LEAVE_NOTIFY_MASK);

  g_signal_connect(G_OBJECT(d->timeline), "draw", G_CALLBACK(_lib_timeline_draw_callback), self);
  g_signal_connect(G_OBJECT(d->timeline), "button-press-event", G_CALLBACK(_lib_timeline_button_press_callback),
                   self);
  g_signal_connect(G_OBJECT(d->timeline), "button-release-event",
                   G_CALLBACK(_lib_timeline_button_release_callback), self);
  g_signal_connect(G_OBJECT(d->timeline), "scroll-event", G_CALLBACK(_lib_timeline_scroll_callback), self);
  g_signal_connect(G_OBJECT(d->timeline), "motion-notify-event", G_CALLBACK(_lib_timeline_motion_notify_callback),
                   self);
  g_signal_connect(G_OBJECT(d->timeline), "leave-notify-event", G_CALLBACK(_lib_timeline_mouse_leave_callback),
                   self);

  gtk_box_pack_start(GTK_BOX(self->widget), d->timeline, TRUE, TRUE, 0);

  gtk_widget_show_all(self->widget);
  /* initialize view manager proxy */
  darktable.view_manager->proxy.timeline.module = self;

  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_COLLECTION_CHANGED,
                            G_CALLBACK(_lib_timeline_collection_changed), (gpointer)self);

  dt_action_register(DT_ACTION(self), N_("start selection"), _selection_start, GDK_KEY_bracketleft, 0);
  dt_action_register(DT_ACTION(self), N_("stop selection"), _selection_stop, GDK_KEY_bracketright, 0);
}

void gui_cleanup(dt_lib_module_t *self)
{
  /* cleanup */
  dt_lib_timeline_t *strip = (dt_lib_timeline_t *)self->data;
  if(strip->blocks) g_list_free_full(strip->blocks, _block_free);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_lib_timeline_collection_changed), self);
  /* unset viewmanager proxy */
  darktable.view_manager->proxy.timeline.module = NULL;
  free(self->data);
  self->data = NULL;
}


// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
