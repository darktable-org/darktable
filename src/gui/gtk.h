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

#define DT_GUI_IOP_MODULE_CONTROL_SPACING 2

#define DT_GUI_VIEW_SWITCH_TO_TETHERING	1
#define DT_GUI_VIEW_SWITCH_TO_LIBRARY   2
#define DT_GUI_VIEW_SWITCH_TO_DARKROOM  3

typedef struct dt_gui_key_accel_t
{
  guint   state;
  guint   keyval;
  guint16 hardware_keycode;
  void (*callback)(void *);
  void *data;
}
dt_gui_key_accel_t;

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

  GtkWidget *bottom_left_toolbox;
  GtkWidget *bottom_right_toolbox;

  // Drawing areas
  GtkWidget *center;

  // Borders
  GtkWidget *left_border;
  GtkWidget *right_border;
  GtkWidget *bottom_border;
  GtkWidget *top_border;

  // Jobs list
  GtkWidget *jobs_content_box;

  // Image filters
  GtkWidget *image_filter;
  GtkWidget *image_sort;

  // Top-right label
  GtkWidget *view_label;

  /* left panel */
  GtkTable *panel_left;                 // panel table 3 rows, top,center,bottom and fille on center
  GtkTable *panel_right;               

}
dt_gui_widgets_t;

typedef struct dt_gui_gtk_t
{

  struct dt_ui_t *ui;

  dt_gui_widgets_t widgets;

  GdkPixmap *pixmap;
  GList *key_accels;
  GtkMenu *presets_popup_menu;
  
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

/** register an accel callback for the whole window. data is not freed on unregister. */
void dt_gui_key_accel_register(guint state, guint keyval, void (*callback)(void *), void *data);
void dt_gui_key_accel_unregister(void (*callback)(void *));


/*
 * new ui api 
 */


typedef enum dt_ui_container_t
{
  /* the top container of left panel, the top container
     disables the module expander and does not scroll with other modules 
  */
  DT_UI_CONTAINER_PANEL_LEFT_TOP,

  /* the center container of left panel, the center container
     contains the scrollable area that all plugins are placed within and last
     widget is the end marker. 
     This container will always expand|fill empty veritcal space
  */
  DT_UI_CONTAINER_PANEL_LEFT_CENTER,

  /* the bottom container of left panel, this container works just like
     the top container but will be attached to bottom in the panel, such as
     plugins like background jobs module in lighttable and the plugin selection 
     module in darkroom,
  */
  DT_UI_CONTAINER_PANEL_LEFT_BOTTOM,

  DT_UI_CONTAINER_PANEL_RIGHT_TOP,
  DT_UI_CONTAINER_PANEL_RIGHT_CENTER,
  DT_UI_CONTAINER_PANEL_RIGHT_BOTTOM,

  /* Count of containers */
  DT_UI_CONTAINER_SIZE
} dt_ui_container_t;

typedef enum dt_ui_panel_t
{
  DT_UI_PANEL_TOP,
  DT_UI_PANEL_BOTTOM,
  DT_UI_PANEL_LEFT,
  DT_UI_PANEL_RIGHT,

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
/** \biref shows/hide a panel */
void dt_ui_panel_show(struct dt_ui_t *ui,const dt_ui_panel_t, gboolean show);
/** \biref get visible state of panel */
gboolean dt_ui_panel_visible(struct dt_ui_t *ui,const dt_ui_panel_t);
#endif
