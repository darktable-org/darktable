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

#include "paint.h"
#include <gtk/gtk.h>
G_BEGIN_DECLS
#define DTGTK_RANGE_SELECT(obj)                                                                                   \
  G_TYPE_CHECK_INSTANCE_CAST(obj, dtgtk_range_select_get_type(), GtkDarktableRangeSelect)
#define DTGTK_RANGE_SELECT_CLASS(klass)                                                                           \
  G_TYPE_CHECK_CLASS_CAST(klass, dtgtk_range_select_get_type(), GtkDarktableRangeSelectClass)
#define DTGTK_IS_RANGE_SELECT(obj) G_TYPE_CHECK_INSTANCE_TYPE(obj, dtgtk_range_select_get_type())
#define DTGTK_IS_RANGE_SELECT_CLASS(klass) G_TYPE_CHECK_CLASS_TYPE(obj, dtgtk_range_select_get_type())

typedef double (*DTGTKTranslateValueFunc)(const double value);
typedef gchar *(*DTGTKPrintValueFunc)(const double value, const gboolean detailled);
typedef double (*DTGTKDecodeValueFunc)(const gchar *text);

typedef enum dt_range_bounds_t
{
  DT_RANGE_BOUND_RANGE = 0,
  DT_RANGE_BOUND_MIN = 1 << 0,
  DT_RANGE_BOUND_MAX = 1 << 1,
  DT_RANGE_BOUND_FIXED = 1 << 2
} dt_range_bounds_t;

typedef struct _GtkDarktableRangeSelect
{
  GtkBin widget;

  double min; // minimal value shown
  double max; // maximal value shown
  double step;

  double select_min;        // low bound of the selection
  double select_max;        // hight bound of the selection
  dt_range_bounds_t bounds; // type of selection bounds

  double current_x;
  gboolean mouse_inside;
  gboolean set_selection;

  cairo_surface_t *surface;
  int surf_width;

  GtkWidget *entry_min;
  GtkWidget *current;
  GtkWidget *entry_max;
  GtkWidget *band;

  // fonction used to translate "real" value into band positions
  // this allow to have special value repartitions on the band
  // if NULL, band values == real values
  DTGTKTranslateValueFunc band_value;
  DTGTKTranslateValueFunc value_band;
  double band_start;  // band value of the start of the widget
  double band_factor; // factor for getting band value from widget position

  // function used to print and decode values so they are human readable
  // print function has detailled mode for extended infos
  DTGTKPrintValueFunc print;
  DTGTKDecodeValueFunc decode;

  GList *blocks;
  GList *icons;
  GList *markers;
} GtkDarktableRangeSelect;

typedef struct _GtkDarktableRangeSelectClass
{
  GtkBoxClass parent_class;
} GtkDarktableRangeSelectClass;

GType dtgtk_range_select_get_type(void);

/** instantiate a new range selection widget */
GtkWidget *dtgtk_range_select_new(const gchar *property);

void dtgtk_range_select_set_selection(GtkDarktableRangeSelect *range, const dt_range_bounds_t bounds,
                                      const double min, const double max, gboolean signal);
dt_range_bounds_t dtgtk_range_select_get_selection(GtkDarktableRangeSelect *range, double *min, double *max);

void dtgtk_range_select_add_block(GtkDarktableRangeSelect *range, const double value, const int count);
void dtgtk_range_select_reset_blocks(GtkDarktableRangeSelect *range);

void dtgtk_range_select_set_band_func(GtkDarktableRangeSelect *range, DTGTKTranslateValueFunc band_value,
                                      DTGTKTranslateValueFunc value_band);
void dtgtk_range_select_set_print_func(GtkDarktableRangeSelect *range, DTGTKPrintValueFunc print,
                                       DTGTKDecodeValueFunc decode);

void dtgtk_range_select_add_icon(GtkDarktableRangeSelect *range, const int posx, const double value,
                                 DTGTKCairoPaintIconFunc paint, gint flags, void *data);
void dtgtk_range_select_reset_icons(GtkDarktableRangeSelect *range);

void dtgtk_range_select_add_marker(GtkDarktableRangeSelect *range, const double value, const gboolean magnetic);
void dtgtk_range_select_reset_markers(GtkDarktableRangeSelect *range);
G_END_DECLS