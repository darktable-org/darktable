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

#pragma once

#include "common/darktable.h"

#include <gtk/gtk.h>
#include <stdint.h>

#define DT_GUI_IOP_MODULE_CONTROL_SPACING 2

/* helper macro that applies the DPI transformation to fixed pixel values. input should be defaulting to 96
 * DPI */
#define DT_PIXEL_APPLY_DPI(value) ((value) * darktable.gui->dpi_factor)

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

typedef struct dt_gui_scrollbars_t
{
    GtkWidget *vscrollbar;
    GtkWidget *hscrollbar;

    gboolean visible;
    gboolean dragging;
} dt_gui_scrollbars_t;

typedef enum dt_gui_color_t {
  DT_GUI_COLOR_BG = 0,
  DT_GUI_COLOR_DARKROOM_BG,
  DT_GUI_COLOR_DARKROOM_PREVIEW_BG,
  DT_GUI_COLOR_LIGHTTABLE_BG,
  DT_GUI_COLOR_LIGHTTABLE_PREVIEW_BG,
  DT_GUI_COLOR_BRUSH_CURSOR,
  DT_GUI_COLOR_BRUSH_TRACE,
  DT_GUI_COLOR_LAST
} dt_gui_color_t;

typedef struct dt_gui_gtk_t
{

  struct dt_ui_t *ui;

  dt_gui_widgets_t widgets;

  dt_gui_scrollbars_t scrollbars;

  cairo_surface_t *surface;
  GtkMenu *presets_popup_menu;
  char *last_preset;

  int32_t reset;
  GdkRGBA colors[DT_GUI_COLOR_LAST];

  int32_t center_tooltip; // 0 = no tooltip, 1 = new tooltip, 2 = old tooltip

  gboolean grouping;
  int32_t expanded_group_id;

  gboolean show_overlays;

  double dpi, dpi_factor, ppd;

  // store which gtkrc we loaded:
  char gtkrc[PATH_MAX];

  GtkWidget *scroll_to[2]; // one for left, one for right

  gint scroll_mask;
} dt_gui_gtk_t;

#if (CAIRO_VERSION >= CAIRO_VERSION_ENCODE(1, 13, 1))
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

static inline cairo_surface_t *dt_cairo_image_surface_create_from_png(const char *filename) {
  cairo_surface_t *cst = cairo_image_surface_create_from_png(filename);
  cairo_surface_set_device_scale(cst, darktable.gui->ppd, darktable.gui->ppd);
  return cst;
}

static inline int dt_cairo_image_surface_get_width(cairo_surface_t *surface) {
  return cairo_image_surface_get_width(surface) / darktable.gui->ppd;
}

static inline int dt_cairo_image_surface_get_height(cairo_surface_t *surface) {
  return cairo_image_surface_get_height(surface) / darktable.gui->ppd;
}

static inline cairo_surface_t *dt_gdk_cairo_surface_create_from_pixbuf(const GdkPixbuf *pixbuf, int scale, GdkWindow *for_window) {
  cairo_surface_t *cst = gdk_cairo_surface_create_from_pixbuf(pixbuf, scale, for_window);
  cairo_surface_set_device_scale(cst, darktable.gui->ppd, darktable.gui->ppd);
  return cst;
}

static inline GdkPixbuf *dt_gdk_pixbuf_new_from_file_at_size(const char *filename, int width, int height, GError **error) {
  return gdk_pixbuf_new_from_file_at_size(filename, width * darktable.gui->ppd, height * darktable.gui->ppd, error);
}
#else
#define dt_cairo_image_surface_create cairo_image_surface_create
#define dt_cairo_image_surface_create_for_data cairo_image_surface_create_for_data
#define dt_cairo_image_surface_create_from_png cairo_image_surface_create_from_png
#define dt_cairo_image_surface_get_width cairo_image_surface_get_width
#define dt_cairo_image_surface_get_height cairo_image_surface_get_height
#define dt_gdk_cairo_surface_create_from_pixbuf gdk_cairo_surface_create_from_pixbuf
#define dt_gdk_pixbuf_new_from_file_at_size gdk_pixbuf_new_from_file_at_size
#endif

int dt_gui_gtk_init(dt_gui_gtk_t *gui);
void dt_gui_gtk_run(dt_gui_gtk_t *gui);
void dt_gui_gtk_cleanup(dt_gui_gtk_t *gui);
void dt_gui_gtk_quit();
void dt_gui_store_last_preset(const char *name);
int dt_gui_gtk_load_config();
int dt_gui_gtk_write_config();
void dt_gui_gtk_set_source_rgb(cairo_t *cr, dt_gui_color_t);
void dt_gui_gtk_set_source_rgba(cairo_t *cr, dt_gui_color_t, float opacity_coef);

/* Return requested scroll delta(s) from event. If delta_x or delta_y
 * is NULL, do not return that delta. Return TRUE if requested deltas
 * can be retrieved. Handles both GDK_SCROLL_UP/DOWN/LEFT/RIGHT and
 * GDK_SCROLL_SMOOTH style scroll events. */
gboolean dt_gui_get_scroll_deltas(const GdkEventScroll *event, gdouble *delta_x, gdouble *delta_y);
/* Same as above, except accumulate smooth scrolls deltas of < 1 and
 * only set deltas and return TRUE once scrolls accumulate to >= 1.
 * Effectively makes smooth scroll events act like old-style unit
 * scroll events. */
gboolean dt_gui_get_scroll_unit_deltas(const GdkEventScroll *event, int *delta_x, int *delta_y);

/** block any keyaccelerators when widget have focus, block is released when widget lose focus. */
void dt_gui_key_accel_block_on_focus_connect(GtkWidget *w);
/** clean up connected signal handlers before destroying your widget: */
void dt_gui_key_accel_block_on_focus_disconnect(GtkWidget *w);

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

/** \brief add's a widget to a defined container */
void dt_ui_container_add_widget(struct dt_ui_t *ui, const dt_ui_container_t c, GtkWidget *w);
/** \brief gives a widget focus in the container */
void dt_ui_container_focus_widget(struct dt_ui_t *ui, const dt_ui_container_t c, GtkWidget *w);
/** \brief calls a callback on all children widgets from container */
void dt_ui_container_foreach(struct dt_ui_t *ui, const dt_ui_container_t c, GtkCallback callback);
/** \brief destroy all child widgets from container */
void dt_ui_container_destroy_children(struct dt_ui_t *ui, const dt_ui_container_t c);
/** \brief shows/hide a panel */
void dt_ui_panel_show(struct dt_ui_t *ui, const dt_ui_panel_t, gboolean show, gboolean write);
/** show or hide outermost borders with expand arrows */
void dt_ui_border_show(struct dt_ui_t *ui, gboolean show);
/** \brief restore saved state of panel visibility for current view */
void dt_ui_restore_panels(struct dt_ui_t *ui);
/** \brief update scrollbars for current view */
void dt_ui_update_scrollbars(struct dt_ui_t *ui);
/** show or hide scrollbars */
void dt_ui_scrollbars_show(struct dt_ui_t *ui, gboolean show);
/** \brief toggle view of panels eg. collaps/expands to previous view state */
void dt_ui_toggle_panels_visibility(struct dt_ui_t *ui);
/** \brief draw user's attention */
void dt_ui_notify_user();
/** \brief get visible state of panel */
gboolean dt_ui_panel_visible(struct dt_ui_t *ui, const dt_ui_panel_t);
/** \brief get the center drawable widget */
GtkWidget *dt_ui_center(struct dt_ui_t *ui);
/** \brief get the main window widget */
GtkWidget *dt_ui_main_window(struct dt_ui_t *ui);

GtkBox *dt_ui_get_container(struct dt_ui_t *ui, const dt_ui_container_t c);

/*  activate ellipsization of the combox entries */
void dt_ellipsize_combo(GtkComboBox *cbox);

static inline void dt_ui_section_label_set(GtkWidget *label)
{
  gtk_widget_set_halign(label, GTK_ALIGN_FILL); // make it span the whole available width
  g_object_set(G_OBJECT(label), "xalign", 1.0, (gchar *)0);    // make the text right aligned
  gtk_widget_set_margin_bottom(label, DT_PIXEL_APPLY_DPI(10)); // gtk+ css doesn't support margins :(
  gtk_widget_set_margin_start(label, DT_PIXEL_APPLY_DPI(30)); // gtk+ css doesn't support margins :(
  gtk_widget_set_name(label, "section_label"); // make sure that we can style these easily
}
static inline GtkWidget *dt_ui_section_label_new(const gchar *str)
{
  GtkWidget *label = gtk_label_new(str);
  dt_ui_section_label_set(label);
  return label;
};

// show a dialog box with 2 buttons in case some user interaction is required BEFORE dt's gui is initialised.
// this expects gtk_init() to be called already which should be the case during most of dt's init phase.
gboolean dt_gui_show_standalone_yes_no_dialog(const char *title, const char *markup, const char *no_text,
                                              const char *yes_text);

void *dt_gui_show_splashscreen();
void dt_gui_close_splashscreen(void *splashscreen);

void dt_gui_add_help_link(GtkWidget *widget, const char *link);

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
