/*
    This file is part of darktable,
    copyright (c) 2009--2011 johannes hanika.

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
#ifndef DT_GUI_GTK_H
#define DT_GUI_GTK_H

#include <gtk/gtk.h>
#include "gui/navigation.h"
#include "gui/histogram.h"

#define DT_GUI_IOP_MODULE_CONTROL_SPACING 2

#define DT_GUI_VIEW_SWITCH_TO_TETHERING	1
#define DT_GUI_VIEW_SWITCH_TO_LIBRARY   2
#define DT_GUI_VIEW_SWITCH_TO_DARKROOM  3

typedef struct dt_gui_snapshot_t
{
  float zoom_x, zoom_y, zoom_scale;
  int32_t zoom, closeup;
  char filename[30];
}
dt_gui_snapshot_t;

// flat view of all our widgets. could probably be modularized
// to be a bit nicer (put metadata/histogram/.. in their gui/* files):
typedef struct dt_gui_widgets_t
{
  GtkWidget *main_window;

  // Colorpicker widgets
  GtkWidget *bottom_darkroom_box;
  GtkWidget *colorpicker_button;
  GtkWidget *colorpicker_stat_combobox;
  GtkWidget *colorpicker_model_combobox;
  GtkWidget *colorpicker_output_label;

  // Layout widgets
  GtkWidget *bottom_lighttable_box;
  GtkWidget *lighttable_layout_combobox;
  GtkWidget *lighttable_zoom_spinbutton;

  // Bottom containers
  GtkWidget *bottom;
  GtkWidget *bottom_left_toolbox;
  GtkWidget *bottom_right_toolbox;

  // Drawing areas
  GtkWidget *center;

  // Borders
  GtkWidget *left_border;
  GtkWidget *right_border;
  GtkWidget *bottom_border;
  GtkWidget *top_border;

  // Module list widgets
  GtkWidget *module_list_eventbox;
  GtkWidget *module_list;

  // Right scrolled window widgets
  GtkWidget *right_scrolled_window;
  GtkWidget *plugins_vbox;

  // Module groups box
  GtkWidget *modulegroups_eventbox;

  // Histogram widgets
  GtkWidget *histogram_expander;
  GtkWidget *histogram;

  // Right side widgets
  GtkWidget *right;
  GtkWidget *right_vbox;

  // Jobs list
  GtkWidget *jobs_content_box;

  // Left side widgets
  GtkWidget *left_scrolled_window;
  GtkWidget *left_scrolled;
  GtkWidget *left;
  GtkWidget *left_vbox;

  // Import widgets
  GtkWidget *import_eventbox;
  GtkWidget *import_expander;
  GtkWidget *devices_expander_body;

  // Left side plugins
  GtkWidget *plugins_vbox_left;

  // Snapshots window
  GtkWidget *snapshots_eventbox;
  GtkWidget *snapshots_expander;
  GtkWidget *snapshots_body;

  // Metadata
  GtkWidget *metadata_expander;

  GtkWidget
      *metadata_label_filename,
      *metadata_label_model,
      *metadata_label_maker,
      *metadata_label_aperture,
      *metadata_label_exposure,
      *metadata_label_focal_length,
      *metadata_label_focus_distance,
      *metadata_label_iso,
      *metadata_label_datetime,
      *metadata_label_lens,
      *metadata_label_width,
      *metadata_label_height,
      *metadata_label_filmroll,
      *metadata_label_title,
      *metadata_label_creator,
      *metadata_label_rights;

  // History box
  GtkWidget *history_eventbox;
  GtkWidget *history_expander;
  GtkWidget *history_expander_body;

  // Left end marker
  GtkWidget *endmarker_left;

  // Navigation panel
  GtkWidget *navigation_expander;
  GtkWidget *navigation;

  // Top panel
  GtkWidget *top;

  // Image filters
  GtkWidget *image_filter;
  GtkWidget *image_sort;

  // Top-right label
  GtkWidget *view_label;
}
dt_gui_widgets_t;

typedef struct dt_gui_gtk_t
{

  // GUI widgets
  dt_gui_widgets_t widgets;

  GdkPixmap *pixmap;
  GList *redraw_widgets;
  GtkMenu *presets_popup_menu;
  dt_gui_navigation_t navigation;
  dt_gui_histogram_t histogram;

  int32_t num_snapshots, request_snapshot, selected_snapshot;
  dt_gui_snapshot_t snapshot[4];
  cairo_surface_t *snapshot_image;

  int32_t reset;
  float bgcolor[3];

  int32_t center_tooltip; // 0 = no tooltip, 1 = new tooltip, 2 = old tooltip

  float picked_color_output_cs[3];
  float picked_color_output_cs_min[3];
  float picked_color_output_cs_max[3];
}
dt_gui_gtk_t;

int dt_gui_gtk_init(dt_gui_gtk_t *gui, int argc, char *argv[]);
void dt_gui_gtk_run(dt_gui_gtk_t *gui);
void dt_gui_gtk_cleanup(dt_gui_gtk_t *gui);

/** block any keyaccelerators when widget have focus, block is released when widget lose focus. */
void dt_gui_key_accel_block_on_focus (GtkWidget *w);

#endif
