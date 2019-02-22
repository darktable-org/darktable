/*
    This file is part of darktable,
    copyright (c) 2018-2019 Aldric Renaudin.

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

typedef struct dt_lib_timeline_time_t
{
  int year;
  int month;
  int day;
  int hour;
  int minute;

} dt_lib_timeline_time_t;

typedef struct dt_lib_timeline_block_t
{
  gchar *name;
  int *values;
  int values_count;
  dt_lib_timeline_time_t init;
  int width;

} dt_lib_timeline_block_t;



typedef struct dt_lib_timeline_t
{
  dt_lib_timeline_time_t time_mini;
  dt_lib_timeline_time_t time_maxi;
  dt_lib_timeline_time_t time_pos;

  GtkWidget *timeline;
  cairo_surface_t *surface;
  int surface_width;
  int32_t panel_width;

  GList *blocks;
  dt_lib_timeline_zooms_t zoom;
  dt_lib_timeline_zooms_t precision;

  int start_x;
  int stop_x;
  int current_x;
  dt_lib_timeline_time_t start_t;
  dt_lib_timeline_time_t stop_t;
  gboolean has_selection;
  gboolean selecting;

  int last_motion;
  gboolean in;

  gboolean size_handle_is_dragging;
  gint size_handle_x, size_handle_y;
  int32_t size_handle_height;

} dt_lib_timeline_t;



const char *name(dt_lib_module_t *self)
{
  return _("timeline");
}

const char **views(dt_lib_module_t *self)
{
  static const char *v[] = { "lighttable", NULL };
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_BOTTOM;
}

int expandable(dt_lib_module_t *self)
{
  return 0;
}

int position()
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
static int _block_get_bar_count(dt_lib_timeline_time_t t, dt_lib_timeline_zooms_t zoom)
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
static dt_lib_timeline_time_t _time_init()
{
  dt_lib_timeline_time_t tt = { 0 };
  // we don't want 0 values for month and day
  tt.month = tt.day = 1;
  return tt;
}

// compare times
static int _time_compare_at_zoom(dt_lib_timeline_time_t t1, dt_lib_timeline_time_t t2, dt_lib_timeline_zooms_t zoom)
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
static int _time_compare(dt_lib_timeline_time_t t1, dt_lib_timeline_time_t t2)
{
  if(t1.year != t2.year) return (t1.year - t2.year);
  if(t1.month != t2.month) return (t1.month - t2.month);
  if(t1.day != t2.day) return (t1.day - t2.day);
  if(t1.hour != t2.hour) return (t1.hour - t2.hour);
  if(t1.minute != t2.minute) return (t1.minute - t2.minute);

  return 0;
}

// add/substract value to a time at certain level
static void _time_add(dt_lib_timeline_time_t *t, int val, dt_lib_timeline_zooms_t level)
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
      t->hour += 60;
      _time_add(t, -1, DT_LIB_TIMELINE_ZOOM_HOUR);
    }
  }

  // fix for date with year set to 0 (bug ?)
  if(t->year < 0) t->year = 0;
}

// retrieve the date from the position at given zoom level
static dt_lib_timeline_time_t _time_get_from_pos(int pos, dt_lib_timeline_t *strip)
{
  dt_lib_timeline_time_t tt = _time_init();

  int x = 0;
  GList *bl = strip->blocks;
  while(bl)
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
        tt.minute = (pos - x) / _block_get_bar_width(strip->zoom) + 1;
        if(tt.minute < 0) tt.minute = 0;
      }

      return tt;
    }
    x += blo->width + 2;
    bl = bl->next;
  }

  return tt;
}

static dt_lib_timeline_time_t _time_compute_offset_for_zoom(int pos, dt_lib_timeline_t *strip,
                                                            dt_lib_timeline_zooms_t new_zoom)
{
  if(new_zoom == strip->zoom) return strip->time_pos;

  dt_lib_timeline_time_t tt = _time_get_from_pos(pos, strip);

  // we search the number of the bloc under pos
  int bloc_nb = 0;
  int x = 0;
  GList *bl = strip->blocks;
  while(bl)
  {
    dt_lib_timeline_block_t *blo = bl->data;
    if(pos < x + blo->width) break;
    x += blo->width + 2;
    bl = bl->next;
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

// get all the datetimes from the database
static gboolean _time_read_bounds_from_db(dt_lib_module_t *self)
{
  dt_lib_timeline_t *strip = (dt_lib_timeline_t *)self->data;

  sqlite3_stmt *stmt;
  const char *query = "SELECT datetime_taken FROM main.images WHERE LENGTH(datetime_taken) = 19 ORDER BY "
                      "datetime_taken ASC LIMIT 1";
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);

  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const char *tx = (const char *)sqlite3_column_text(stmt, 0);
    strip->time_mini.year = MAX(strtol(tx, NULL, 10), 0);
    strip->time_mini.month = CLAMP(strtol(tx + 5, NULL, 10), 1, 12);
    strip->time_mini.day
        = CLAMP(strtol(tx + 8, NULL, 10), 1, _time_days_in_month(strip->time_mini.year, strip->time_mini.month));
    strip->time_mini.hour = CLAMP(strtol(tx + 11, NULL, 10), 0, 23);
    strip->time_mini.minute = CLAMP(strtol(tx + 14, NULL, 10), 0, 59);
  }
  sqlite3_finalize(stmt);

  const char *query2 = "SELECT datetime_taken FROM main.images WHERE LENGTH(datetime_taken) = 19 ORDER BY "
                       "datetime_taken DESC LIMIT 1";
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query2, -1, &stmt, NULL);

  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const char *tx = (const char *)sqlite3_column_text(stmt, 0);
    strip->time_maxi.year = MAX(strtol(tx, NULL, 10), 0);
    strip->time_maxi.month = CLAMP(strtol(tx + 5, NULL, 10), 1, 12);
    strip->time_maxi.day
        = CLAMP(strtol(tx + 8, NULL, 10), 1, _time_days_in_month(strip->time_mini.year, strip->time_mini.month));
    strip->time_maxi.hour = CLAMP(strtol(tx + 11, NULL, 10), 0, 23);
    strip->time_maxi.minute = CLAMP(strtol(tx + 14, NULL, 10), 0, 59);
  }
  sqlite3_finalize(stmt);

  return TRUE;
}

static gchar *_time_format_for_ui(dt_lib_timeline_time_t t, dt_lib_timeline_zooms_t zoom)
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
    return g_strdup_printf("%02d/%02d/%02d (%02dh-%02dh)", t.day, t.month, t.year % 100, t.hour / 6 * 6,
                           t.hour / 6 * 6 + 5);
  }
  else if(zoom == DT_LIB_TIMELINE_ZOOM_HOUR)
  {
    return g_strdup_printf("%02d/%02d/%02d %02dh", t.day, t.month, t.year % 100, t.hour);
  }
  else if(zoom == DT_LIB_TIMELINE_ZOOM_10MINUTE)
  {
    return g_strdup_printf("%02d/%02d/%02d %02dh(%02d-%02d)", t.day, t.month, t.year % 100, t.hour,
                           t.minute / 10 * 10, t.minute / 10 * 10 + 9);
  }
  else if(zoom == DT_LIB_TIMELINE_ZOOM_MINUTE)
  {
    return g_strdup_printf("%02d/%02d/%02d %02dh%02d", t.day, t.month, t.year % 100, t.hour, t.minute);
  }

  return NULL;
}
static gchar *_time_format_for_db(dt_lib_timeline_time_t t, dt_lib_timeline_zooms_t zoom, gboolean full)
{
  if(zoom == DT_LIB_TIMELINE_ZOOM_YEAR)
  {
    if(full)
      return g_strdup_printf("%04d:01:01 00:00:00", t.year);
    else
      return g_strdup_printf("%04d", t.year);
  }
  else if(zoom == DT_LIB_TIMELINE_ZOOM_4MONTH || zoom == DT_LIB_TIMELINE_ZOOM_MONTH)
  {
    if(full)
      return g_strdup_printf("%04d:%02d:01 00:00:00", t.year, t.month);
    else
      return g_strdup_printf("%04d:%02d", t.year, t.month);
  }
  else if(zoom == DT_LIB_TIMELINE_ZOOM_10DAY || zoom == DT_LIB_TIMELINE_ZOOM_DAY)
  {
    if(full)
      return g_strdup_printf("%04d:%02d:%02d 00:00:00", t.year, t.month, t.day);
    else
      return g_strdup_printf("%04d:%02d:%02d", t.year, t.month, t.day);
  }
  else if(zoom == DT_LIB_TIMELINE_ZOOM_6HOUR || zoom == DT_LIB_TIMELINE_ZOOM_HOUR)
  {
    if(full)
      return g_strdup_printf("%04d:%02d:%02d %02d:00:00", t.year, t.month, t.day, t.hour);
    else
      return g_strdup_printf("%04d:%02d:%02d %02d", t.year, t.month, t.day, t.hour);
  }
  else if(zoom == DT_LIB_TIMELINE_ZOOM_10MINUTE || zoom == DT_LIB_TIMELINE_ZOOM_MINUTE)
  {
    if(full)
      return g_strdup_printf("%04d:%02d:%02d %02d:%02d:00", t.year, t.month, t.day, t.hour, t.minute);
    else
      return g_strdup_printf("%04d:%02d:%02d %02d:%02d", t.year, t.month, t.day, t.hour, t.minute);
  }

  return NULL;
}
static dt_lib_timeline_time_t _time_get_from_db(gchar *tx, gboolean last)
{
  dt_lib_timeline_time_t tt = _time_init();
  if(strlen(tx) > 3) tt.year = CLAMP(strtol(tx, NULL, 10), 0, 4000);
  if(strlen(tx) > 6) tt.month = CLAMP(strtol(tx + 5, NULL, 10), 1, 12);
  if(strlen(tx) > 9) tt.day = CLAMP(strtol(tx + 8, NULL, 10), 1, _time_days_in_month(tt.year, tt.month));
  if(strlen(tx) > 12) tt.hour = CLAMP(strtol(tx + 11, NULL, 10), 0, 23);
  if(strlen(tx) > 15) tt.minute = CLAMP(strtol(tx + 14, NULL, 10), 0, 59);

  // if we need to complete a non full date to get the last one ("2012" > "2012:12:31 23:59")
  if(last)
  {
    if(strlen(tx) < 16)
    {
      tt.minute = 59;
      if(strlen(tx) < 13)
      {
        tt.hour = 23;
        if(strlen(tx) < 7)
        {
          tt.month = 12;
        }
        if(strlen(tx) < 10)
        {
          tt.day = _time_days_in_month(tt.year, tt.month);
        }
      }
    }
  }
  return tt;
}

// get the time of the first block of the strip in order to show the selection
static dt_lib_timeline_time_t _selection_scroll_to(dt_lib_timeline_time_t t, dt_lib_timeline_t *strip)
{
  dt_lib_timeline_time_t tt = t;
  int nb = strip->panel_width / 122;

  for(int i = 0; i < nb; i++)
  {
    // we ensure that we are not before the strip bound
    if(_time_compare(tt, strip->time_mini) <= 0) return strip->time_mini;

    // and we dont want to display blocks after the bounds too
    dt_lib_timeline_time_t ttt = tt;
    _time_add(&ttt, nb - 1, strip->zoom);
    if(_time_compare(ttt, strip->time_maxi) <= 0) return tt;

    // we test the previous date
    _time_add(&tt, -1, strip->zoom);
  }
  // if we are here that me we fail to scroll... why ?
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

  // if selection start/stop if lower than the begiining of the strip
  if(_time_compare_at_zoom(strip->start_t, strip->time_pos, strip->zoom) < 0) strip->start_x = -2;
  if(_time_compare_at_zoom(strip->stop_t, strip->time_pos, strip->zoom) < 0) strip->stop_x = -1;

  sqlite3_stmt *stmt;
  gchar *query = g_strdup_printf("SELECT datetime_taken FROM main.images WHERE LENGTH(datetime_taken) = 19 AND "
                                 "datetime_taken > '%s' ORDER BY datetime_taken ASC",
                                 _time_format_for_db(strip->time_pos, strip->zoom, TRUE));
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);

  char *tx = "";
  int stat = sqlite3_step(stmt);
  if(stat == SQLITE_ROW) tx = (char *)sqlite3_column_text(stmt, 0);
  dt_lib_timeline_time_t tt = strip->time_pos;
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
      while(stat == SQLITE_ROW && _time_compare_at_zoom(tt, _time_get_from_db(tx, FALSE), strip->zoom) == 0)
      {
        bloc->values[i]++;
        stat = sqlite3_step(stmt);
        tx = (char *)sqlite3_column_text(stmt, 0);
      }

      // and we jump to next date
      // if (i+1 >= bloc->values_count) break;
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

static void _lib_timeline_collection_changed(gpointer instance, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_timeline_t *strip = (dt_lib_timeline_t *)self->data;

  // we get the serialzed rules
  char buf[4096];
  if(dt_collection_serialize(buf, sizeof(buf))) return;

  // we only handle unique datetime queries
  if(!g_str_has_prefix(buf, "1:0:5:")) return;

  GRegex *regex;
  GMatchInfo *match_info;
  int match_count;
  dt_lib_timeline_time_t start, stop;
  start = stop = _time_init();
  gboolean read_ok = FALSE;
  // we test the range expression first
  // 2 elements : date-time1 and  date-time2
  regex = g_regex_new("^\\s*\\[\\s*(\\d{4}[:\\d\\s]*)\\s*;\\s*(\\d{4}[:\\d\\s]*)\\s*\\]\\s*\\$$", 0, 0, NULL);
  g_regex_match_full(regex, buf + 6, -1, 0, 0, &match_info, NULL);
  match_count = g_match_info_get_match_count(match_info);

  if(match_count == 3)
  {
    gchar *txt = g_match_info_fetch(match_info, 1);
    gchar *txt2 = g_match_info_fetch(match_info, 2);

    start = _time_get_from_db(txt, FALSE);
    stop = _time_get_from_db(txt2, TRUE);

    g_free(txt);
    g_free(txt2);
    read_ok = TRUE;
  }
  else
  {
    // let's see if it's a simple date
    g_match_info_free(match_info);
    g_regex_unref(regex);
    regex = g_regex_new("^\\s*(\\d{4}[:\\d\\s]*)\\s*\\$$", 0, 0, NULL);
    g_regex_match_full(regex, buf + 6, -1, 0, 0, &match_info, NULL);
    match_count = g_match_info_get_match_count(match_info);
    if(match_count == 2)
    {
      gchar *txt = g_match_info_fetch(match_info, 1);

      start = _time_get_from_db(txt, FALSE);
      stop = _time_get_from_db(txt, TRUE);

      g_free(txt);
      read_ok = TRUE;
    }
  }

  g_match_info_free(match_info);
  g_regex_unref(regex);

  // did we manage to read the rule ?
  if(!read_ok) return;

  // is the selection up to date ?
  if(_time_compare_at_zoom(start, strip->start_t, strip->zoom) == 0
     && _time_compare_at_zoom(stop, strip->stop_t, strip->zoom) == 0)
    return;

  strip->start_t = start;
  strip->stop_t = stop;
  strip->has_selection = TRUE;

  // now we want to scroll to the start pos
  strip->time_pos = _selection_scroll_to(strip->start_t, strip);
  cairo_surface_destroy(strip->surface);
  strip->surface = NULL;
  gtk_widget_queue_draw(strip->timeline);
}


// add the selected portions to the collect
static void _selection_collect(dt_lib_timeline_t *strip)
{
  /* NOTE :
   * we reuse the same routines as in recent-collect module
   * so we have to construct a "serialized" rule in the form
   * 1:0:5:xxxxxx$ where
   * 1=one unique rule
   * 0=add mode
   * 5=datetime type of rule
  */

  gchar *coll = NULL;
  if(!strip->has_selection)
  {
    coll = g_strdup("1:0:5:%$");
  }
  else if(strip->start_x == strip->stop_x)
  {
    gchar *d1 = _time_format_for_db(strip->start_t, (strip->zoom + 1) / 2 * 2 + 2, FALSE);
    if(d1) coll = g_strdup_printf("1:0:5:%s$", d1);
    g_free(d1);
  }
  else
  {
    dt_lib_timeline_time_t start = strip->start_t;
    dt_lib_timeline_time_t stop = strip->stop_t;
    if(strip->start_x > strip->stop_x)
    {
      // we exchange the values
      start = strip->stop_t;
      stop = strip->start_t;
    }
    gchar *d1 = _time_format_for_db(start, (strip->zoom + 1) / 2 * 2 + 2, FALSE);
    gchar *d2 = _time_format_for_db(stop, (strip->zoom + 1) / 2 * 2 + 2, FALSE);
    if(d1 && d2) coll = g_strdup_printf("1:0:5:[%s;%s]$", d1, d2);
    g_free(d1);
    g_free(d2);
  }

  // we write the new collect rule
  if(coll)
  {
    dt_collection_deserialize(coll);
    g_free(coll);
  }
}

static gboolean _lib_timeline_draw_callback(GtkWidget *widget, cairo_t *wcr, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_timeline_t *strip = (dt_lib_timeline_t *)self->data;

  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  const int32_t width = allocation.width;

  // windows could have been expanded for example, we need to create a new surface of the good size and redraw
  if(width != strip->panel_width)
  {
    // if it's the first show, we need to recompute the scroll too
    if(strip->panel_width == 0 && strip->has_selection)
    {
      strip->panel_width = width;
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
    // we set the width of a unit (bar) in the drawing (depending of the zoom level)
    int wu = _block_get_bar_width(strip->zoom);

    strip->surface = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, allocation.width, allocation.height);

    // get cairo drawing handle
    cairo_t *cr = cairo_create(strip->surface);

    /* fill background */
    dt_gui_gtk_set_source_rgb(cr, DT_GUI_COLOR_FILMSTRIP_BG);
    cairo_paint(cr);

    // draw content depending of zoom level
    GList *bl = strip->blocks;
    int posx = 0;
    while(bl)
    {
      dt_lib_timeline_block_t *blo = bl->data;

      // width of this block
      int wb = blo->values_count * wu;

      cairo_text_extents_t te;
      dt_gui_gtk_set_source_rgb(cr, DT_GUI_COLOR_BRUSH_CURSOR);
      cairo_set_font_size(cr, 10);
      cairo_text_extents(cr, blo->name, &te);
      int bh = allocation.height - te.height - 4;
      cairo_move_to(cr, posx + (wb - te.width) / 2 - te.x_bearing, allocation.height - 2);
      cairo_show_text(cr, blo->name);

      dt_gui_gtk_set_source_rgb(cr, DT_GUI_COLOR_THUMBNAIL_BG);
      cairo_rectangle(cr, posx, 0, wb, bh);
      cairo_fill(cr);

      for(int i = 0; i < blo->values_count; i++)
      {
        dt_gui_gtk_set_source_rgb(cr, DT_GUI_COLOR_THUMBNAIL_HOVER_BG);
        int h = _block_get_bar_height(blo->values[i], bh);
        cairo_rectangle(cr, posx + (i * wu), bh - h, wu, h);
        cairo_fill(cr);
      }

      bl = bl->next;
      posx += wb + 2;
      if(posx >= allocation.width) break;
    }

    // copy back the new content into the cairo handle of the draw callback
    cairo_destroy(cr);
  }
  cairo_set_source_surface(wcr, strip->surface, 0, 0);
  cairo_paint(wcr);

  // we eventually draw the selection
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
        dt_gui_gtk_set_source_rgb(wcr, DT_GUI_COLOR_THUMBNAIL_HOVER_BG);
        cairo_move_to(wcr, start, 0);
        cairo_line_to(wcr, start, allocation.height);
        cairo_stroke(wcr);
      }
      dt_gui_gtk_set_source_rgba(wcr, DT_GUI_COLOR_THUMBNAIL_HOVER_BG, 0.5);
      cairo_rectangle(wcr, start, 0, stop - start, allocation.height);
      cairo_fill(wcr);
      if(stop <= strip->panel_width)
      {
        dt_gui_gtk_set_source_rgb(wcr, DT_GUI_COLOR_THUMBNAIL_HOVER_BG);
        cairo_move_to(wcr, stop, 0);
        cairo_line_to(wcr, stop, allocation.height);
        cairo_stroke(wcr);
      }
    }
  }

  // we draw the line under cursor and the date-time
  if(strip->in && strip->current_x > 0)
  {
    dt_gui_gtk_set_source_rgb(wcr, DT_GUI_COLOR_BRUSH_TRACE);
    cairo_move_to(wcr, strip->current_x, 0);
    cairo_line_to(wcr, strip->current_x, allocation.height);
    cairo_stroke(wcr);
    dt_lib_timeline_time_t tt;
    if(strip->selecting)
      tt = strip->stop_t;
    else
      tt = _time_get_from_pos(strip->current_x, strip);
    gchar *dte = _time_format_for_ui(tt, strip->precision);
    cairo_text_extents_t te2;
    cairo_set_font_size(wcr, 10);
    cairo_text_extents(wcr, dte, &te2);
    cairo_rectangle(wcr, strip->current_x, 8, te2.width + 4, te2.height + 4);
    dt_gui_gtk_set_source_rgb(wcr, DT_GUI_COLOR_BRUSH_TRACE);
    cairo_fill(wcr);
    cairo_move_to(wcr, strip->current_x + 2, 10 + te2.height);
    dt_gui_gtk_set_source_rgb(wcr, DT_GUI_COLOR_BRUSH_CURSOR);
    cairo_show_text(wcr, dte);
    g_free(dte);
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
      }
      else if(e->x - strip->stop_x < 2 && e->x - strip->stop_x > -2)
      {
        strip->stop_x = e->x;
        strip->stop_t = _time_get_from_pos(e->x, strip);
      }
      else
      {
        strip->start_x = strip->stop_x = e->x;
        strip->start_t = strip->stop_t = _time_get_from_pos(e->x, strip);
      }
      strip->selecting = TRUE;
      strip->has_selection = TRUE;
      gtk_widget_queue_draw(strip->timeline);
    }
  }
  else if(e->button == 3)
  {
    // we remove the selection
    strip->selecting = FALSE;
    strip->has_selection = FALSE;
    _selection_collect(strip);
    gtk_widget_queue_draw(strip->timeline);
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
    strip->stop_t = _time_get_from_pos(e->x, strip);
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
    strip->selecting = FALSE;
    _selection_collect(strip);
    gtk_widget_queue_draw(strip->timeline);
  }

  return TRUE;
}

static gboolean _selection_start(GtkAccelGroup *accel_group, GObject *aceeleratable, guint keyval,
                                 GdkModifierType modifier, gpointer data)
{
  dt_lib_timeline_t *strip = (dt_lib_timeline_t *)data;

  strip->start_x = strip->current_x;
  strip->start_t = _time_get_from_pos(strip->current_x, strip);
  if(!strip->has_selection)
  {
    strip->stop_x = strip->start_x;
    strip->stop_t = strip->start_t;
    strip->selecting = TRUE;
  }
  strip->has_selection = TRUE;
  gtk_widget_queue_draw(strip->timeline);
  return TRUE;
}
static gboolean _selection_stop(GtkAccelGroup *accel_group, GObject *aceeleratable, guint keyval,
                                GdkModifierType modifier, gpointer data)
{
  dt_lib_timeline_t *strip = (dt_lib_timeline_t *)data;

  // we need to have defined a start point...
  if(!strip->has_selection) return FALSE;

  strip->stop_x = strip->current_x;
  strip->stop_t = _time_get_from_pos(strip->current_x, strip);
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

  strip->selecting = FALSE;
  _selection_collect(strip);
  gtk_widget_queue_draw(strip->timeline);
  return TRUE;
}

static gboolean _lib_timeline_motion_notify_callback(GtkWidget *w, GdkEventMotion *e, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_timeline_t *strip = (dt_lib_timeline_t *)self->data;

  strip->in = TRUE;

  // auto-scroll if cursor is at one end of the panel
  if(e->x < 10 && e->time - strip->last_motion > 500)
  {
    strip->last_motion = e->time;
    if(_time_compare(strip->time_pos, strip->time_mini) > 0)
    {
      _time_add(&(strip->time_pos), -1, strip->zoom);
      cairo_surface_destroy(strip->surface);
      strip->surface = NULL;
      gtk_widget_queue_draw(strip->timeline);
    }
  }
  else if(e->x > strip->panel_width - 10 && e->time - strip->last_motion > 500)
  {
    strip->last_motion = e->time;
    if(strip->surface_width >= strip->panel_width)
    {
      _time_add(&(strip->time_pos), 1, strip->zoom);
      cairo_surface_destroy(strip->surface);
      strip->surface = NULL;
      gtk_widget_queue_draw(strip->timeline);
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
    // we change the cursor if we are close enought of a selection limit
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
  if(e->state & GDK_CONTROL_MASK)
  {
    int z = strip->zoom;
    if(e->direction == GDK_SCROLL_UP)
    {
      if(z != DT_LIB_TIMELINE_ZOOM_HOUR) z++;
    }
    else if(e->direction == GDK_SCROLL_DOWN)
    {
      if(z != DT_LIB_TIMELINE_ZOOM_YEAR) z--;
    }

    // if the zoom as changed, we need to recompute blocks and redraw
    if(z != strip->zoom)
    {
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
    int delta_x, delta_y;
    if(dt_gui_get_scroll_unit_deltas(e, &delta_x, &delta_y))
    {
      int move = -delta_x - delta_y;
      if(e->state & GDK_SHIFT_MASK) move *= 2;

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

static gboolean _lib_timeline_size_handle_cursor_callback(GtkWidget *w, GdkEventCrossing *e, gpointer user_data)
{
  dt_control_change_cursor((e->type == GDK_ENTER_NOTIFY) ? GDK_SB_V_DOUBLE_ARROW : GDK_LEFT_PTR);
  return TRUE;
}

static gboolean _lib_timeline_size_handle_button_callback(GtkWidget *w, GdkEventButton *e, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_timeline_t *d = (dt_lib_timeline_t *)self->data;

  if(e->button == 1)
  {
    if(e->type == GDK_BUTTON_PRESS)
    {
/* store current  mousepointer position */
#if GTK_CHECK_VERSION(3, 20, 0)
      gdk_window_get_device_position(e->window,
                                     gdk_seat_get_pointer(gdk_display_get_default_seat(gdk_window_get_display(
                                         gtk_widget_get_window(dt_ui_main_window(darktable.gui->ui))))),
                                     &d->size_handle_x, &d->size_handle_y, 0);
#else
      gdk_window_get_device_position(
          gtk_widget_get_window(dt_ui_main_window(darktable.gui->ui)),
          gdk_device_manager_get_client_pointer(gdk_display_get_device_manager(
              gdk_window_get_display(gtk_widget_get_window(dt_ui_main_window(darktable.gui->ui))))),
          &d->size_handle_x, &d->size_handle_y, NULL);
#endif

      gtk_widget_get_size_request(d->timeline, NULL, &d->size_handle_height);
      d->size_handle_is_dragging = TRUE;
      cairo_surface_destroy(d->surface);
      d->surface = NULL;
    }
    else if(e->type == GDK_BUTTON_RELEASE)
      d->size_handle_is_dragging = FALSE;
  }
  return TRUE;
}

static gboolean _lib_timeline_size_handle_motion_notify_callback(GtkWidget *w, GdkEventButton *e,
                                                                 gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_timeline_t *d = (dt_lib_timeline_t *)self->data;
  if(d->size_handle_is_dragging)
  {
    gint x, y, sx, sy;
#if GTK_CHECK_VERSION(3, 20, 0)
    gdk_window_get_device_position(e->window,
                                   gdk_seat_get_pointer(gdk_display_get_default_seat(gdk_window_get_display(
                                       gtk_widget_get_window(dt_ui_main_window(darktable.gui->ui))))),
                                   &x, &y, 0);
#else
    gdk_window_get_device_position(
        gtk_widget_get_window(dt_ui_main_window(darktable.gui->ui)),
        gdk_device_manager_get_client_pointer(gdk_display_get_device_manager(
            gdk_window_get_display(gtk_widget_get_window(dt_ui_main_window(darktable.gui->ui))))),
        &x, &y, NULL);
#endif

    gtk_widget_get_size_request(d->timeline, &sx, &sy);
    sy = CLAMP(d->size_handle_height + (d->size_handle_y - y), DT_PIXEL_APPLY_DPI(64), DT_PIXEL_APPLY_DPI(400));

    dt_conf_set_int("plugins/lighttable/timeline/height", sy);

    cairo_surface_destroy(d->surface);
    d->surface = NULL;
    gtk_widget_set_size_request(d->timeline, -1, sy);

    return TRUE;
  }

  return FALSE;
}

void init_key_accels(dt_lib_module_t *self)
{
  dt_accel_register_lib(self, NC_("accel", "start selection"), GDK_KEY_bracketleft, 0);
  dt_accel_register_lib(self, NC_("accel", "stop selection"), GDK_KEY_bracketright, 0);
}

void connect_key_accels(dt_lib_module_t *self)
{
  GClosure *closure = g_cclosure_new(G_CALLBACK(_selection_start), (gpointer)self->data, NULL);
  dt_accel_connect_lib(self, "start selection", closure);
  closure = g_cclosure_new(G_CALLBACK(_selection_stop), (gpointer)self->data, NULL);
  dt_accel_connect_lib(self, "stop selection", closure);
}

void gui_init(dt_lib_module_t *self)
{
  /* initialize ui widgets */
  dt_lib_timeline_t *d = (dt_lib_timeline_t *)calloc(1, sizeof(dt_lib_timeline_t));
  self->data = (void *)d;

  _time_read_bounds_from_db(self);
  d->time_pos = d->time_mini;
  /* creating drawing area */
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  /* creating timeline box*/
  d->timeline = gtk_event_box_new();

  /* set size of timeline */
  int32_t height = dt_conf_get_int("plugins/lighttable/timeline/height");
  gtk_widget_set_size_request(d->timeline, -1, CLAMP(height, DT_PIXEL_APPLY_DPI(64), DT_PIXEL_APPLY_DPI(400)));

  gtk_widget_add_events(d->timeline, GDK_POINTER_MOTION_MASK | GDK_POINTER_MOTION_HINT_MASK | GDK_BUTTON_PRESS_MASK
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
  /* create the resize handle */
  GtkWidget *size_handle = gtk_event_box_new();
  gtk_widget_set_size_request(size_handle, -1, DT_PIXEL_APPLY_DPI(10));
  gtk_widget_add_events(size_handle, GDK_POINTER_MOTION_MASK | GDK_POINTER_MOTION_HINT_MASK | GDK_BUTTON_PRESS_MASK
                                         | GDK_BUTTON_RELEASE_MASK | GDK_ENTER_NOTIFY_MASK
                                         | GDK_LEAVE_NOTIFY_MASK);
  g_signal_connect(G_OBJECT(size_handle), "button-press-event",
                   G_CALLBACK(_lib_timeline_size_handle_button_callback), self);
  g_signal_connect(G_OBJECT(size_handle), "button-release-event",
                   G_CALLBACK(_lib_timeline_size_handle_button_callback), self);
  g_signal_connect(G_OBJECT(size_handle), "motion-notify-event",
                   G_CALLBACK(_lib_timeline_size_handle_motion_notify_callback), self);
  g_signal_connect(G_OBJECT(size_handle), "leave-notify-event",
                   G_CALLBACK(_lib_timeline_size_handle_cursor_callback), self);
  g_signal_connect(G_OBJECT(size_handle), "enter-notify-event",
                   G_CALLBACK(_lib_timeline_size_handle_cursor_callback), self);

  gtk_box_pack_start(GTK_BOX(self->widget), size_handle, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), d->timeline, FALSE, FALSE, 0);

  // we update the selection with actual collect rules
  _lib_timeline_collection_changed(NULL, self);

  /* initialize view manager proxy */
  darktable.view_manager->proxy.timeline.module = self;

  dt_control_signal_connect(darktable.signals, DT_SIGNAL_COLLECTION_CHANGED,
                            G_CALLBACK(_lib_timeline_collection_changed), (gpointer)self);
}

void gui_cleanup(dt_lib_module_t *self)
{
  /* cleanup */
  dt_lib_timeline_t *strip = (dt_lib_timeline_t *)self->data;
  if(strip->blocks) g_list_free_full(strip->blocks, _block_free);
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(_lib_timeline_collection_changed), self);
  /* unset viewmanager proxy */
  darktable.view_manager->proxy.timeline.module = NULL;
  free(self->data);
  self->data = NULL;
}


// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
