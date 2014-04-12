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

#include "common/darktable.h"

#include <gtk/gtk.h>
#include <stdint.h>

#define DT_GUI_IOP_MODULE_CONTROL_SPACING 2

typedef enum dt_gui_view_switch_t
{
  DT_GUI_VIEW_SWITCH_TO_TETHERING = 1,
  DT_GUI_VIEW_SWITCH_TO_LIBRARY,
  DT_GUI_VIEW_SWITCH_TO_DARKROOM,
  DT_GUI_VIEW_SWITCH_TO_MAP,
  DT_GUI_VIEW_SWITCH_TO_SLIDESHOW
}
dt_gui_view_switch_to_t;

typedef struct dt_gui_widgets_t
{

  // Borders
  GtkWidget *left_border;
  GtkWidget *right_border;
  GtkWidget *bottom_border;
  GtkWidget *top_border;

  /* left panel */
  GtkTable *panel_left;                 // panel table 3 rows, top,center,bottom and file on center
  GtkTable *panel_right;

}
dt_gui_widgets_t;

typedef struct dt_gui_gtk_t
{

  struct dt_ui_t *ui;

  dt_gui_widgets_t widgets;

  cairo_surface_t *surface;
  GtkMenu *presets_popup_menu;
  char *last_preset;

  int32_t reset;
  float bgcolor[3];

  int32_t center_tooltip; // 0 = no tooltip, 1 = new tooltip, 2 = old tooltip

  gboolean grouping;
  int32_t expanded_group_id;

  gboolean show_overlays;

  double dpi;

  // store which gtkrc we loaded:
  char gtkrc[DT_MAX_PATH_LEN];
}
dt_gui_gtk_t;

int dt_gui_gtk_init(dt_gui_gtk_t *gui, int argc, char *argv[]);
void dt_gui_gtk_run(dt_gui_gtk_t *gui);
void dt_gui_gtk_cleanup(dt_gui_gtk_t *gui);
void dt_gui_gtk_quit();
void dt_gui_store_last_preset(const char *name);

/** block any keyaccelerators when widget have focus, block is released when widget lose focus. */
void dt_gui_key_accel_block_on_focus_connect(GtkWidget *w);
/** clean up connected signal handlers before destroying your widget: */
void dt_gui_key_accel_block_on_focus_disconnect(GtkWidget *w);

/** handle pressure sensitive input devices */
void dt_gui_enable_extended_input_devices();
void dt_gui_disable_extended_input_devices();

/*
 * new ui api
 */


typedef enum dt_ui_container_t
{
  /* the top container of left panel, the top container
     disables the module expander and does not scroll with other modules
  */
  DT_UI_CONTAINER_PANEL_LEFT_TOP = 0,

  /* the center container of left panel, the center container
     contains the scrollable area that all plugins are placed within and last
     widget is the end marker.
     This container will always expand|fill empty vertical space
  */
  DT_UI_CONTAINER_PANEL_LEFT_CENTER = 1,

  /* the bottom container of left panel, this container works just like
     the top container but will be attached to bottom in the panel, such as
     plugins like background jobs module in lighttable and the plugin selection
     module in darkroom,
  */
  DT_UI_CONTAINER_PANEL_LEFT_BOTTOM = 2,

  DT_UI_CONTAINER_PANEL_RIGHT_TOP = 3,
  DT_UI_CONTAINER_PANEL_RIGHT_CENTER = 4,
  DT_UI_CONTAINER_PANEL_RIGHT_BOTTOM = 5,


  /* the top header bar, left slot where darktable name is placed */
  DT_UI_CONTAINER_PANEL_TOP_LEFT = 6,
  /* center which is expanded as wide it can */
  DT_UI_CONTAINER_PANEL_TOP_CENTER = 7,
  /* right side were the different views are accessed */
  DT_UI_CONTAINER_PANEL_TOP_RIGHT = 8,

  DT_UI_CONTAINER_PANEL_CENTER_TOP_LEFT = 9,
  DT_UI_CONTAINER_PANEL_CENTER_TOP_CENTER = 10,
  DT_UI_CONTAINER_PANEL_CENTER_TOP_RIGHT = 11,

  DT_UI_CONTAINER_PANEL_CENTER_BOTTOM_LEFT = 12,
  DT_UI_CONTAINER_PANEL_CENTER_BOTTOM_CENTER = 13,
  DT_UI_CONTAINER_PANEL_CENTER_BOTTOM_RIGHT = 14,

  /* this panel is placed at bottom of ui
     only used by the filmstrip if shown */
  DT_UI_CONTAINER_PANEL_BOTTOM = 15,

  /* Count of containers */
  DT_UI_CONTAINER_SIZE
}
dt_ui_container_t;

typedef enum dt_ui_panel_t
{
  /* the header panel */
  DT_UI_PANEL_TOP,
  /* center top toolbar panel */
  DT_UI_PANEL_CENTER_TOP,
  /* center bottom toolbar panel */
  DT_UI_PANEL_CENTER_BOTTOM,
  /* left panel */
  DT_UI_PANEL_LEFT,
  /* right panel */
  DT_UI_PANEL_RIGHT,
  /* bottom panel */
  DT_UI_PANEL_BOTTOM,

  DT_UI_PANEL_SIZE
} dt_ui_panel_t;

typedef enum dt_ui_border_t
{
  DT_UI_BORDER_TOP,
  DT_UI_BORDER_BOTTOM,
  DT_UI_BORDER_LEFT,
  DT_UI_BORDER_RIGHT,

  DT_UI_BORDER_SIZE
} dt_ui_border_t;

/** \brief initialize the ui context */
struct dt_ui_t *dt_ui_initialize(int argc, char **argv);
/** \brief destroys the context and frees resources */
void dt_ui_destroy(struct dt_ui_t *ui);
/** \brief add's a widget to a defined container */
void dt_ui_container_add_widget(struct dt_ui_t *ui, const dt_ui_container_t c, GtkWidget *w);
/** \brief gives a widget focus in the container */
void dt_ui_container_focus_widget(struct dt_ui_t *ui, const dt_ui_container_t c, GtkWidget *w);
/** \brief removes all child widgets from container */
void dt_ui_container_clear(struct dt_ui_t *ui, const dt_ui_container_t c);
/** \brief shows/hide a panel */
void dt_ui_panel_show(struct dt_ui_t *ui,const dt_ui_panel_t, gboolean show);
/** show or hide outermost borders with expand arrows */
void dt_ui_border_show(struct dt_ui_t *ui, gboolean show);
/** \brief restore saved state of panel visibility for current view */
void dt_ui_restore_panels(struct dt_ui_t *ui);
/** \brief toggle view of panels eg. collaps/expands to previous view state */
void dt_ui_toggle_panels_visibility(struct dt_ui_t *ui);
/** \brief get visible state of panel */
gboolean dt_ui_panel_visible(struct dt_ui_t *ui,const dt_ui_panel_t);
/** \brief get the center drawable widget */
GtkWidget *dt_ui_center(struct dt_ui_t *ui);
/** \brief get the main window widget */
GtkWidget *dt_ui_main_window(struct dt_ui_t *ui);

GtkBox *dt_ui_get_container(struct dt_ui_t *ui, const dt_ui_container_t c);

/*  activate ellipsization of the combox entries */
void dt_ellipsize_combo(GtkComboBox *cbox);

#endif
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
