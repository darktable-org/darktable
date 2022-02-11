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

/* Precision about all the value reference used here
  We have in fact 3 different reference
  - the "real" value (for example ratio 0.67)
    suffix = "_r"
  - the "band" value : this is the value corrected in order to be shown on the graph.
    this differ from value because for some property, we need non linear repartition of values
    example : portrait ratio should be inverted
    suffix = "_bd"
  - the "pixels" value : this is the value in pixels as shown on the graph
    this value changed each time the widget size change
    suffix = "_px"
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

  gboolean show_entries; // do we show the line with the entry boxes ?
  double min_r;          // minimal value shown
  double max_r;          // maximal value shown
  double step_r;

  double select_min_r;      // low bound of the selection
  double select_max_r;      // hight bound of the selection
  dt_range_bounds_t bounds; // type of selection bounds

  double current_x_px;
  gboolean mouse_inside;
  gboolean set_selection;

  cairo_surface_t *surface;
  int surf_width_px;

  GtkWidget *entry_min;
  GtkWidget *current;
  GtkWidget *entry_max;
  GtkWidget *band;

  // fonction used to translate "real" value into band positions
  // this allow to have special value repartitions on the band
  // if NULL, band values == real values
  DTGTKTranslateValueFunc value_to_band;
  DTGTKTranslateValueFunc value_from_band;
  double band_start_bd; // band value of the start of the widget
  double band_factor; // factor for getting band value from widget position

  // function used to print and decode values so they are human readable
  // print function has detailled mode for extended infos
  DTGTKPrintValueFunc print;
  DTGTKDecodeValueFunc decode;

  GList *blocks;
  GList *icons;
  GList *markers;

  int band_margin_side_px;
  int band_real_width_px;
} GtkDarktableRangeSelect;

typedef struct _GtkDarktableRangeSelectClass
{
  GtkBoxClass parent_class;
} GtkDarktableRangeSelectClass;

GType dtgtk_range_select_get_type(void);

/** instantiate a new range selection widget */
GtkWidget *dtgtk_range_select_new(const gchar *property, gboolean show_entries);

void dtgtk_range_select_set_selection(GtkDarktableRangeSelect *range, const dt_range_bounds_t bounds,
                                      const double min_r, const double max_r, gboolean signal);
dt_range_bounds_t dtgtk_range_select_get_selection(GtkDarktableRangeSelect *range, double *min_r, double *max_r);

void dtgtk_range_select_add_block(GtkDarktableRangeSelect *range, const double value_r, const int count);
void dtgtk_range_select_add_range_block(GtkDarktableRangeSelect *range, const double min_r, const double max_r,
                                        const dt_range_bounds_t bounds, gchar *txt, const int count);
void dtgtk_range_select_reset_blocks(GtkDarktableRangeSelect *range);

void dtgtk_range_select_set_band_func(GtkDarktableRangeSelect *range, DTGTKTranslateValueFunc value_from_band,
                                      DTGTKTranslateValueFunc value_to_band);
void dtgtk_range_select_set_print_func(GtkDarktableRangeSelect *range, DTGTKPrintValueFunc print,
                                       DTGTKDecodeValueFunc decode);

void dtgtk_range_select_add_icon(GtkDarktableRangeSelect *range, const int posx, const double value_r,
                                 DTGTKCairoPaintIconFunc paint, gint flags, void *data);
void dtgtk_range_select_reset_icons(GtkDarktableRangeSelect *range);

void dtgtk_range_select_add_marker(GtkDarktableRangeSelect *range, const double value_r, const gboolean magnetic);
void dtgtk_range_select_reset_markers(GtkDarktableRangeSelect *range);
G_END_DECLS