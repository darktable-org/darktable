/*
    This file is part of darktable,
    Copyright (C) 2009-2022 darktable developers.

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
#include "common/dtpthread.h"

#include <gtk/gtk.h>
#include <stdint.h>

#define DT_GUI_IOP_MODULE_CONTROL_SPACING 0

#define DT_GUI_THUMBSIZE_REDUCE 0.7f

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

  /* resize of left/right panels */
  gboolean panel_handle_dragging;
  int panel_handle_x, panel_handle_y;
} dt_gui_widgets_t;

typedef struct dt_gui_scrollbars_t
{
    GtkWidget *vscrollbar;
    GtkWidget *hscrollbar;

    gboolean visible;
    gboolean dragging;
} dt_gui_scrollbars_t;

typedef enum dt_gui_color_t
{
  DT_GUI_COLOR_BG = 0,
  DT_GUI_COLOR_DARKROOM_BG,
  DT_GUI_COLOR_DARKROOM_PREVIEW_BG,
  DT_GUI_COLOR_LIGHTTABLE_BG,
  DT_GUI_COLOR_LIGHTTABLE_PREVIEW_BG,
  DT_GUI_COLOR_LIGHTTABLE_FONT,
  DT_GUI_COLOR_PRINT_BG,
  DT_GUI_COLOR_BRUSH_CURSOR,
  DT_GUI_COLOR_BRUSH_TRACE,
  DT_GUI_COLOR_BUTTON_FG,
  DT_GUI_COLOR_THUMBNAIL_BG,
  DT_GUI_COLOR_THUMBNAIL_SELECTED_BG,
  DT_GUI_COLOR_THUMBNAIL_HOVER_BG,
  DT_GUI_COLOR_THUMBNAIL_OUTLINE,
  DT_GUI_COLOR_THUMBNAIL_SELECTED_OUTLINE,
  DT_GUI_COLOR_THUMBNAIL_HOVER_OUTLINE,
  DT_GUI_COLOR_THUMBNAIL_FONT,
  DT_GUI_COLOR_THUMBNAIL_SELECTED_FONT,
  DT_GUI_COLOR_THUMBNAIL_HOVER_FONT,
  DT_GUI_COLOR_THUMBNAIL_BORDER,
  DT_GUI_COLOR_THUMBNAIL_SELECTED_BORDER,
  DT_GUI_COLOR_FILMSTRIP_BG,
  DT_GUI_COLOR_TIMELINE_BG,
  DT_GUI_COLOR_TIMELINE_FG,
  DT_GUI_COLOR_TIMELINE_TEXT_BG,
  DT_GUI_COLOR_TIMELINE_TEXT_FG,
  DT_GUI_COLOR_CULLING_SELECTED_BORDER,
  DT_GUI_COLOR_CULLING_FILMSTRIP_SELECTED_BORDER,
  DT_GUI_COLOR_PREVIEW_HOVER_BORDER,
  DT_GUI_COLOR_LOG_BG,
  DT_GUI_COLOR_LOG_FG,
  DT_GUI_COLOR_MAP_COUNT_SAME_LOC,
  DT_GUI_COLOR_MAP_COUNT_DIFF_LOC,
  DT_GUI_COLOR_MAP_COUNT_BG,
  DT_GUI_COLOR_MAP_LOC_SHAPE_HIGH,
  DT_GUI_COLOR_MAP_LOC_SHAPE_LOW,
  DT_GUI_COLOR_MAP_LOC_SHAPE_DEF,
  DT_GUI_COLOR_RANGE_BG,
  DT_GUI_COLOR_RANGE_GRAPH,
  DT_GUI_COLOR_RANGE_SELECTION,
  DT_GUI_COLOR_RANGE_CURSOR,
  DT_GUI_COLOR_RANGE_ICONS,
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
  gboolean show_focus_peaking;
  double overlay_red, overlay_blue, overlay_green, overlay_contrast;
  GtkWidget *focus_peaking_button;

  double dpi, dpi_factor, ppd, ppd_thb;

  int icon_size; // size of top panel icons

  // store which gtkrc we loaded:
  char gtkrc[PATH_MAX];

  gint scroll_mask;
  guint sidebar_scroll_mask;

  cairo_filter_t filter_image;    // filtering used for all modules expect darkroom
  cairo_filter_t dr_filter_image; // filtering used in the darkroom

  dt_pthread_mutex_t mutex;
} dt_gui_gtk_t;

typedef struct _gui_collapsible_section_t
{
  GtkBox *parent;       // the parent widget
  gchar *confname;      // configuration name for the toggle status
  GtkWidget *toggle;    // toggle button
  GtkWidget *expander;  // the expanded
  GtkBox *container;    // the container for all widgets into the section
} dt_gui_collapsible_section_t;

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

// call class function to add or remove CSS classes (need to be set on top of this file as first function is used in this file)
void dt_gui_add_class(GtkWidget *widget, const gchar *class_name);
void dt_gui_remove_class(GtkWidget *widget, const gchar *class_name);

int dt_gui_gtk_init(dt_gui_gtk_t *gui);
void dt_gui_gtk_run(dt_gui_gtk_t *gui);
void dt_gui_gtk_cleanup(dt_gui_gtk_t *gui);
void dt_gui_gtk_quit();
void dt_gui_store_last_preset(const char *name);
int dt_gui_gtk_load_config();
int dt_gui_gtk_write_config();
void dt_gui_gtk_set_source_rgb(cairo_t *cr, dt_gui_color_t);
void dt_gui_gtk_set_source_rgba(cairo_t *cr, dt_gui_color_t, float opacity_coef);
double dt_get_system_gui_ppd(GtkWidget *widget);

/* Check sidebar_scroll_default and modifier keys to determine if scroll event
 * should be processed by control or by panel. If default is panel scroll but
 * modifiers are pressed to indicate the control should be scrolled, then remove
 * the modifiers from the event before returning false */
gboolean dt_gui_ignore_scroll(GdkEventScroll *event);
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

/* Note that on macOS Shift+vertical scroll can be reported as Shift+horizontal scroll.
 * So if Shift changes scrolling effect, both scrolls should be handled the same.
 * For this case (or if it's otherwise useful) use the following 2 functions. */

/* Return sum of scroll deltas from event. Return TRUE if any deltas
 * can be retrieved. Handles both GDK_SCROLL_UP/DOWN/LEFT/RIGHT and
 * GDK_SCROLL_SMOOTH style scroll events. */
gboolean dt_gui_get_scroll_delta(const GdkEventScroll *event, gdouble *delta);
/* Same as above, except accumulate smooth scrolls deltas of < 1 and
 * only set delta and return TRUE once scrolls accumulate to >= 1.
 * Effectively makes smooth scroll events act like old-style unit
 * scroll events. */
gboolean dt_gui_get_scroll_unit_delta(const GdkEventScroll *event, int *delta);

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
/** \brief restore saved state of panel visibility for current view */
void dt_ui_restore_panels(struct dt_ui_t *ui);
/** \brief update scrollbars for current view */
void dt_ui_update_scrollbars(struct dt_ui_t *ui);
/** show or hide scrollbars */
void dt_ui_scrollbars_show(struct dt_ui_t *ui, gboolean show);
/** \brief toggle view of panels eg. collapse/expands to previous view state */
void dt_ui_toggle_panels_visibility(struct dt_ui_t *ui);
/** \brief draw user's attention */
void dt_ui_notify_user();
/** \brief get visible state of panel */
gboolean dt_ui_panel_visible(struct dt_ui_t *ui, const dt_ui_panel_t);
/**  \brief get width of right, left, or bottom panel */
int dt_ui_panel_get_size(struct dt_ui_t *ui, const dt_ui_panel_t p);
/**  \brief set width of right, left, or bottom panel */
void dt_ui_panel_set_size(struct dt_ui_t *ui, const dt_ui_panel_t p, int s);
/** \brief is the panel ancestor of widget */
gboolean dt_ui_panel_ancestor(struct dt_ui_t *ui, const dt_ui_panel_t p, GtkWidget *w);
/** \brief get the center drawable widget */
GtkWidget *dt_ui_center(struct dt_ui_t *ui);
GtkWidget *dt_ui_center_base(struct dt_ui_t *ui);
/** \brief get the main window widget */
GtkWidget *dt_ui_main_window(struct dt_ui_t *ui);
/** \brief get the thumb table */
struct dt_thumbtable_t *dt_ui_thumbtable(struct dt_ui_t *ui);
/** \brief get the log message widget */
GtkWidget *dt_ui_log_msg(struct dt_ui_t *ui);
/** \brief get the toast message widget */
GtkWidget *dt_ui_toast_msg(struct dt_ui_t *ui);

GtkBox *dt_ui_get_container(struct dt_ui_t *ui, const dt_ui_container_t c);

/*  activate ellipsization of the combox entries */
void dt_ellipsize_combo(GtkComboBox *cbox);

static inline void dt_ui_section_label_set(GtkWidget *label)
{
  gtk_widget_set_halign(label, GTK_ALIGN_FILL); // make it span the whole available width
  gtk_label_set_xalign (GTK_LABEL(label), 0.5f);
  gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END); // ellipsize labels
  dt_gui_add_class(label, "dt_section_label"); // make sure that we can style these easily
}

static inline GtkWidget *dt_ui_section_label_new(const gchar *str)
{
  GtkWidget *label = gtk_label_new(str);
  dt_ui_section_label_set(label);
  return label;
};

static inline GtkWidget *dt_ui_label_new(const gchar *str)
{
  GtkWidget *label = gtk_label_new(str);
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  gtk_label_set_xalign (GTK_LABEL(label), 0.0f);
  gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
  return label;
};

extern const struct dt_action_def_t dt_action_def_tabs_all_rgb;
extern const struct dt_action_def_t dt_action_def_tabs_rgb;
extern const struct dt_action_def_t dt_action_def_tabs_none;

GtkNotebook *dt_ui_notebook_new(struct dt_action_def_t *def);

GtkWidget *dt_ui_notebook_page(GtkNotebook *notebook, const char *text, const char *tooltip);

// show a dialog box with 2 buttons in case some user interaction is required BEFORE dt's gui is initialised.
// this expects gtk_init() to be called already which should be the case during most of dt's init phase.
gboolean dt_gui_show_standalone_yes_no_dialog(const char *title, const char *markup, const char *no_text,
                                              const char *yes_text);

// similar to the one above. this one asks the user for some string. the hint is shown in the empty entry box
char *dt_gui_show_standalone_string_dialog(const char *title, const char *markup, const char *placeholder,
                                           const char *no_text, const char *yes_text);

// returns TRUE if YES was answered, FALSE otherwise
gboolean dt_gui_show_yes_no_dialog(const char *title, const char *format, ...);

void dt_gui_add_help_link(GtkWidget *widget, const char *link);

// load a CSS theme
void dt_gui_load_theme(const char *theme);

// reload GUI scalings
void dt_configure_ppd_dpi(dt_gui_gtk_t *gui);

// translate key press events to remove any modifiers used to produce the keyval
// for example when the shift key is used to create the asterisk character
guint dt_gui_translated_key_state(GdkEventKey *event);

// return modifier keys currently pressed, independent of any key event
GdkModifierType dt_key_modifier_state();

GtkWidget *dt_ui_scroll_wrap(GtkWidget *w, gint min_size, char *config_str);

// check whether the given container has any user-added children
gboolean dt_gui_container_has_children(GtkContainer *container);
// return a count of the user-added children in the given container
int dt_gui_container_num_children(GtkContainer *container);
// return the first child of the given container
GtkWidget *dt_gui_container_first_child(GtkContainer *container);
// return the requested child of the given container, or NULL if it has fewer children
GtkWidget *dt_gui_container_nth_child(GtkContainer *container, int which);

// remove all of the children we've added to the container.  Any which no longer have any references will
// be destroyed.
void dt_gui_container_remove_children(GtkContainer *container);

// delete all of the children we've added to the container.  Use this function only if you are SURE
// there are no other references to any of the children (if in doubt, use dt_gui_container_remove_children
// instead; it's a bit slower but safer).
void dt_gui_container_destroy_children(GtkContainer *container);

void dt_gui_menu_popup(GtkMenu *menu, GtkWidget *button, GdkGravity widget_anchor, GdkGravity menu_anchor);

void dt_gui_draw_rounded_rectangle(cairo_t *cr, float width, float height, float x, float y);

// event handler for "key-press-event" of GtkTreeView to decide if focus switches to GtkSearchEntry
gboolean dt_gui_search_start(GtkWidget *widget, GdkEventKey *event, GtkSearchEntry *entry);

// event handler for "stop-search" of GtkSearchEntry
void dt_gui_search_stop(GtkSearchEntry *entry, GtkWidget *widget);

// create a collapsible section, insert in parent, return the container
void dt_gui_new_collapsible_section(dt_gui_collapsible_section_t *cs,
                                    const char *confname, const char *label,
                                    GtkBox *parent);
// routine to be called from gui_update
void dt_gui_update_collapsible_section(dt_gui_collapsible_section_t *cs);

// routine to hide the collapsible section
void dt_gui_hide_collapsible_section(dt_gui_collapsible_section_t *cs);

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
