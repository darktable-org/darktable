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

// super ugly deprecation avoidance. Ubuntu 14.04 LTS only ships GTK3 3.10
#if GTK_CHECK_VERSION(3, 12, 0) == 0
#define gtk_widget_set_margin_start(w, m) gtk_widget_set_margin_left(w, m)
#define gtk_widget_set_margin_end(w, m) gtk_widget_set_margin_right(w, m)
#endif

#define DT_GUI_IOP_MODULE_CONTROL_SPACING 2

/* helper macro that applies the DPI transformation to fixed pixel values. input should be defaulting to 96
 * DPI */
#define DT_PIXEL_APPLY_DPI(value) (value * darktable.gui->dpi_factor)

typedef enum dt_gui_view_switch_t
{
  DT_GUI_VIEW_SWITCH_TO_TETHERING = 1,
  DT_GUI_VIEW_SWITCH_TO_LIBRARY,
  DT_GUI_VIEW_SWITCH_TO_DARKROOM,
  DT_GUI_VIEW_SWITCH_TO_MAP,
  DT_GUI_VIEW_SWITCH_TO_SLIDESHOW
} dt_gui_view_switch_to_t;

typedef struct dt_gui_widgets_t
{

  // Borders
  GtkWidget *left_border;
  GtkWidget *right_border;
  GtkWidget *bottom_border;
  GtkWidget *top_border;

  /* left panel */
  GtkGrid *panel_left; // panel grid 3 rows, top,center,bottom and file on center
  GtkGrid *panel_right;

} dt_gui_widgets_t;

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

  double dpi, dpi_factor, ppd;

  // store which gtkrc we loaded:
  char gtkrc[PATH_MAX];
} dt_gui_gtk_t;

#if (CAIRO_VERSION >= CAIRO_VERSION_ENCODE(1, 14, 0))
static inline cairo_surface_t *dt_cairo_image_surface_create(cairo_format_t format, int width, int height) {
  cairo_surface_t *cst = cairo_image_surface_create(format, width * darktable.gui->ppd, height * darktable.gui->ppd);
  cairo_surface_set_device_scale(cst, darktable.gui->ppd, darktable.gui->ppd);
  return cst;
}

static inline cairo_surface_t *dt_cairo_image_surface_create_for_data(unsigned char *data, cairo_format_t format, int width, int height, int stride) {
  cairo_surface_t *cst = cairo_image_surface_create_for_data(data, format, width, height, stride);
  cairo_surface_set_device_scale(cst, darktable.gui->ppd, darktable.gui->ppd);
  return cst;
}

static inline int dt_cairo_image_surface_get_width(cairo_surface_t *surface) {
  return cairo_image_surface_get_width(surface) / darktable.gui->ppd;
}

static inline int dt_cairo_image_surface_get_height(cairo_surface_t *surface) {
  return cairo_image_surface_get_height(surface) / darktable.gui->ppd;
}
#else
#define dt_cairo_image_surface_create cairo_image_surface_create
#define dt_cairo_image_surface_create_for_data cairo_image_surface_create_for_data
#define dt_cairo_image_surface_get_width cairo_image_surface_get_width
#define dt_cairo_image_surface_get_height cairo_image_surface_get_height
#endif

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
} dt_ui_container_t;

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
void dt_ui_panel_show(struct dt_ui_t *ui, const dt_ui_panel_t, gboolean show, gboolean write);
/** show or hide outermost borders with expand arrows */
void dt_ui_border_show(struct dt_ui_t *ui, gboolean show);
/** \brief restore saved state of panel visibility for current view */
void dt_ui_restore_panels(struct dt_ui_t *ui);
/** \brief toggle view of panels eg. collaps/expands to previous view state */
void dt_ui_toggle_panels_visibility(struct dt_ui_t *ui);
/** \brief get visible state of panel */
gboolean dt_ui_panel_visible(struct dt_ui_t *ui, const dt_ui_panel_t);
/** \brief get the center drawable widget */
GtkWidget *dt_ui_center(struct dt_ui_t *ui);
/** \brief get the main window widget */
GtkWidget *dt_ui_main_window(struct dt_ui_t *ui);

GtkBox *dt_ui_get_container(struct dt_ui_t *ui, const dt_ui_container_t c);

/*  activate ellipsization of the combox entries */
void dt_ellipsize_combo(GtkComboBox *cbox);

static inline GtkWidget *dt_ui_section_label_new(const gchar *str)
{
  GtkWidget *label = gtk_label_new(str);
  gtk_widget_set_halign(label, GTK_ALIGN_FILL); // make it span the whole available width
  gtk_widget_set_hexpand(label, TRUE); // not really needed, but it makes sure that parent containers expand
  g_object_set(G_OBJECT(label), "xalign", 1.0, NULL); // make the text right aligned
  gtk_widget_set_margin_bottom(label, DT_PIXEL_APPLY_DPI(10)); // gtk+ css doesn't support margins :(
  gtk_widget_set_margin_start(label, DT_PIXEL_APPLY_DPI(30)); // gtk+ css doesn't support margins :(
  gtk_widget_set_name(label, "section_label"); // make sure that we can style these easily
  return label;
};

#endif
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
