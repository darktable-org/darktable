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

#include "common/datetime.h"
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
typedef gboolean (*DTGTKDecodeValueFunc)(const gchar *text, double *value);
typedef struct _GtkDarktableRangeSelect GtkDarktableRangeSelect;
typedef gchar *(*DTGTKCurrentTextFunc)(GtkDarktableRangeSelect *range, const double current);

typedef enum dt_range_bounds_t
{
  DT_RANGE_BOUND_RANGE = 0,
  DT_RANGE_BOUND_MIN = 1 << 0,
  DT_RANGE_BOUND_MAX = 1 << 1,
  DT_RANGE_BOUND_FIXED = 1 << 2,
  DT_RANGE_BOUND_MAX_NOW = 1 << 3,
  DT_RANGE_BOUND_MIN_RELATIVE = 1 << 4,
  DT_RANGE_BOUND_MAX_RELATIVE = 1 << 5
} dt_range_bounds_t;

typedef enum dt_range_type_t
{
  DT_RANGE_TYPE_NUMERIC = 0,
  DT_RANGE_TYPE_DATETIME
} dt_range_type_t;

struct _GtkDarktableRangeSelect
{
  GtkEventBox widget;

  dt_range_type_t type;

  gboolean show_entries; // do we show the line with the entry boxes ?
  double min_r;          // minimal value shown
  double max_r;          // maximal value shown
  double step_bd;        // minimal step value in band reference

  double select_min_r;      // low bound of the selection
  double select_max_r;      // high bound of the selection
  dt_datetime_t select_relative_date_r; // relative date
  dt_range_bounds_t bounds; // type of selection bounds

  double current_x_px;    // current position of the pointer
  int mouse_inside;       // is the mouse inside the graph widget and does it hover min/max positions
  gboolean set_selection; // are we setting the selection

  cairo_surface_t *surface; // cached graph drawing

  GtkWidget *entry_min;
  GtkWidget *entry_max;
  GtkWidget *band;

  // function used to translate "real" value into band positions
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
  DTGTKCurrentTextFunc current_text;
  GList *blocks;
  GList *icons;
  GList *markers;

  GtkAllocation alloc_main;    // area of the total widget
  GtkAllocation alloc_margin;  // area of the widget without margins (for border and background)
  GtkAllocation alloc_padding; // area of the widget without margins and padding (for drawing)

  int max_width_px; // maximal width of the widget in pixels

  // window used to show the value under the cursor
  GtkWidget *cur_window;
  GtkWidget *cur_label;
  int y_root; // y position in the main window
  GtkPositionType cur_pos;

  struct _range_date_popup *date_popup;
};

typedef struct _GtkDarktableRangeSelectClass
{
  GtkEventBoxClass parent_class;
} GtkDarktableRangeSelectClass;

GType dtgtk_range_select_get_type(void);

// instantiate a new range selection widget
GtkWidget *dtgtk_range_select_new(const gchar *property, gboolean show_entries, const dt_range_type_t type);

// set selection range
void dtgtk_range_select_set_selection(GtkDarktableRangeSelect *range, const dt_range_bounds_t bounds,
                                      const double min_r, const double max_r, gboolean signal,
                                      gboolean round_values);
// directly decode raw_text and apply it to selection
void dtgtk_range_select_set_selection_from_raw_text(GtkDarktableRangeSelect *range, const gchar *txt,
                                                    gboolean signal);
// get selection range
dt_range_bounds_t dtgtk_range_select_get_selection(GtkDarktableRangeSelect *range, double *min_r, double *max_r);

// get the text used for collection queries
// result needs to be freed after use
gchar *dtgtk_range_select_get_raw_text(GtkDarktableRangeSelect *range);

// add a block for drawing bar on the graph
// the block will also be shown in context-menu
void dtgtk_range_select_add_block(GtkDarktableRangeSelect *range, const double value_r, const int count);
// add a predetermined range which will be shown in context-menu
void dtgtk_range_select_add_range_block(GtkDarktableRangeSelect *range, const double min_r, const double max_r,
                                        const dt_range_bounds_t bounds, gchar *txt, const int count);
// reset all the blocks
void dtgtk_range_select_reset_blocks(GtkDarktableRangeSelect *range);

// set the function to switch from real value to band value
// this is useful to have non-linear value repartitions
void dtgtk_range_select_set_band_func(GtkDarktableRangeSelect *range, DTGTKTranslateValueFunc value_from_band,
                                      DTGTKTranslateValueFunc value_to_band);
// set functions to switch between real values and text representation
void dtgtk_range_select_set_print_func(GtkDarktableRangeSelect *range, DTGTKPrintValueFunc print,
                                       DTGTKDecodeValueFunc decode);

// add an icon to draw on top of graph bar
// posx is percentage of the graph width
void dtgtk_range_select_add_icon(GtkDarktableRangeSelect *range, const int posx, const double value_r,
                                 DTGTKCairoPaintIconFunc paint, gint flags, void *data);
// remove all icons
void dtgtk_range_select_reset_icons(GtkDarktableRangeSelect *range);

// add a marker to emphase special values
// marker can be magnetic so the pointer "snap" to the value
void dtgtk_range_select_add_marker(GtkDarktableRangeSelect *range, const double value_r, const gboolean magnetic);
// remove all the markers
void dtgtk_range_select_reset_markers(GtkDarktableRangeSelect *range);

// force the graph redraw
void dtgtk_range_select_redraw(GtkDarktableRangeSelect *range);

// get a human readable text for bounds
gchar *dtgtk_range_select_get_bounds_pretty(GtkDarktableRangeSelect *range);
G_END_DECLS

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on