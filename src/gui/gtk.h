/*
    This file is part of darktable,
    Copyright (C) 2009-2026 darktable developers.

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

#include "common/atomic.h"
#include "common/darktable.h"
#include "common/dtpthread.h"

#include <gtk/gtk.h>
#include <stdint.h>

G_BEGIN_DECLS

#define DT_GUI_THUMBSIZE_REDUCE 0.7f

/* helper macro that applies the DPI transformation to fixed pixel
 * values. input should be defaulting to 96 DPI */
#define DT_PIXEL_APPLY_DPI(value) ((value) * darktable.gui->dpi_factor)

#define DT_RESIZE_HANDLE_SIZE DT_PIXEL_APPLY_DPI(5)

typedef struct dt_gui_widgets_t
{

  // Borders
  GtkWidget *left_border;
  GtkWidget *right_border;
  GtkWidget *bottom_border;
  GtkWidget *top_border;

  /* resize of left/right panels */
  gboolean panel_handle_dragging;
  int panel_handle_x, panel_handle_y;
} dt_gui_widgets_t;

typedef struct dt_gui_scrollbars_t
{
    GtkWidget *vscrollbar;
    GtkWidget *hscrollbar;

    gboolean visible;
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
  DT_GUI_COLOR_COLOR_ASSESSMENT_BG,
  DT_GUI_COLOR_COLOR_ASSESSMENT_FG,
  DT_GUI_COLOR_LAST
} dt_gui_color_t;

typedef enum dt_gui_session_type_t
{
  DT_GUI_SESSION_UNKNOWN,
  DT_GUI_SESSION_X11,
  DT_GUI_SESSION_QUARTZ,
  DT_GUI_SESSION_WAYLAND,
} dt_gui_session_type_t;

typedef struct dt_gui_gtk_t
{
  struct dt_ui_t *ui;

  dt_gui_widgets_t widgets;

  dt_gui_scrollbars_t scrollbars;

  cairo_surface_t *surface;  // cached prior image when config var ui/loading_screen is FALSE
  gboolean drawing_snapshot;

  char *last_preset;

  dt_atomic_int reset;
  GdkRGBA colors[DT_GUI_COLOR_LAST];

  int32_t hide_tooltips;

  gboolean grouping;
  dt_imgid_t expanded_group_id;

  gboolean show_overlays;
  gboolean show_focus_peaking;
  gboolean touchpad_gestures_enabled;
  double overlay_red, overlay_blue, overlay_green, overlay_contrast;
  GtkWidget *focus_peaking_button;

  double dpi, dpi_factor, ppd, ppd_thb;
  gboolean have_pen_pressure;

  int icon_size; // size of top panel icons

  // store which gtkrc we loaded:
  char gtkrc[PATH_MAX];

  gint scroll_mask;
  guint sidebar_scroll_mask;

  GMainLoop *main_loop;

  cairo_filter_t filter_image;    // filtering used to scale images to screen
} dt_gui_gtk_t;

typedef struct _gui_collapsible_section_t
{
  GtkBox *parent;       // the parent widget
  gchar *confname;      // configuration name for the toggle status
  GtkWidget *toggle;    // toggle button
  GtkWidget *expander;  // the expanded
  GtkWidget *label;	// the label containing the section's title text
  GtkBox *container;    // the container for all widgets into the section
  struct dt_action_t *module; // the lib or iop module that contains this section
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
/* true while the pointer is grabbed by our own shortcut machinery; used to
 * ignore the synthetic crossing events GDK generates for those grabs */
gboolean dt_gui_pointer_is_grabbed(void);

void dt_open_url(const char *url);
int dt_gui_theme_init(dt_gui_gtk_t *gui);
int dt_gui_gtk_init(dt_gui_gtk_t *gui);
void dt_gui_gtk_run(dt_gui_gtk_t *gui);
void dt_gui_gtk_cleanup(dt_gui_gtk_t *gui);
void dt_gui_gtk_quit();
void dt_gui_store_last_preset(const char *name);
int dt_gui_gtk_load_config();
int dt_gui_gtk_write_config();
void dt_gui_gtk_set_source_rgb(cairo_t *cr, dt_gui_color_t color);
void dt_gui_gtk_set_source_rgba(cairo_t *cr, dt_gui_color_t color,
                                const float opacity_coef);
double dt_get_system_gui_ppd(GtkWidget *widget);
double dt_get_screen_resolution(GtkWidget *widget);

/* Check sidebar_scroll_default and modifier keys to determine if scroll event
 * should be processed by control or by panel. If default is panel scroll but
 * modifiers are pressed to indicate the control should be scrolled, then remove
 * the modifiers from the event before returning false */
gboolean dt_gui_ignore_scroll(GdkEventScroll *event);
/* Same decision for a GtkEventControllerScroll callback: the modifiers are
 * taken from the current event (GTK4: gtk_event_controller_get_current_event_state).
 * The GdkEvent flavor clears the sidebar_scroll_mask from the event to consume
 * the modifier; the controller world cannot mutate events, so the decision is
 * identical but there is no side effect. */
gboolean dt_gui_ignore_scroll_controller(GtkEventControllerScroll *controller);
/* Scale factor converting normalized smooth scroll deltas (as returned by
 * dt_gui_get_scroll_deltas() on macOS) back to pixels for panning. */
#define DT_UI_SCROLL_SMOOTH_DELTA_SCALE 50.0
/* Return requested scroll delta(s) from event. If delta_x or delta_y
 * is NULL, do not return that delta. Return TRUE if requested deltas
 * can be retrieved. Handles both GDK_SCROLL_UP/DOWN/LEFT/RIGHT and
 * GDK_SCROLL_SMOOTH style scroll events.  Takes the opaque GdkEvent
 * (GTK4-compatible); GTK3 callers may pass a GdkEventScroll*. */
gboolean dt_gui_get_scroll_deltas(const GdkEvent *event, gdouble *delta_x, gdouble *delta_y);
/* Same as above, except accumulate smooth scrolls deltas of < 1 and
 * only set deltas and return TRUE once scrolls accumulate to >= 1.
 * Effectively makes smooth scroll events act like old-style unit
 * scroll events. */
gboolean dt_gui_get_scroll_unit_deltas(const GdkEvent *event, int *delta_x, int *delta_y);

/* Note that on macOS Shift+vertical scroll can be reported as Shift+horizontal scroll.
 * So if Shift changes scrolling effect, both scrolls should be handled the same.
 * For this case (or if it's otherwise useful) use the following 2 functions. */

/* Return delta of larger magnitude from the event. Return TRUE if any deltas
 * can be retrieved. Handles both GDK_SCROLL_UP/DOWN/LEFT/RIGHT and
 * GDK_SCROLL_SMOOTH style scroll events. */
gboolean dt_gui_get_scroll_delta(const GdkEvent *event, gdouble *delta);
/* Same as above, except accumulate smooth scrolls deltas of < 1 and
 * only set delta and return TRUE once scrolls accumulate to >= 1.
 * Effectively makes smooth scroll events act like old-style unit
 * scroll events. */
gboolean dt_gui_get_scroll_unit_delta(const GdkEvent *event, int *delta);

/* Return TRUE if a scroll event should be treated as a touchpad pan gesture
 * (two-finger swipe) rather than a plain scroll: requires the touchpad-gestures
 * preference, a smooth non-stop scroll without ctrl, and a touchpad source
 * device (on macOS any smooth non-ctrl scroll qualifies, as the built-in
 * trackpad reports as a mouse; a device that just produced a pinch/swipe
 * gesture also qualifies).  Mouse wheels therefore never pan, even where GTK
 * delivers wheel scrolls as smooth events. */
gboolean dt_gui_scroll_should_pan(const GdkEvent *event);

/* Return the zoom delta (+/-0.5, positive = zoom in) for a scroll event.
 * Vertical scrolls zoom in on up.  Horizontal scrolls zoom in on LEFT when
 * the shift modifier is set (the OS rotated a vertical wheel step into a
 * left/right scroll, so LEFT keeps meaning "wheel up") and on RIGHT
 * otherwise (wheel tilt, two-finger swipe).  Discrete events are resolved
 * from the normalized GDK direction; smooth (fractional) scrolls use their
 * dominant delta.  dx/dy are the caller's deltas (raw or accumulated units). */
float dt_gui_scroll_zoom_delta(const GdkEvent *event, gdouble dx, gdouble dy);

/* Event timestamp as seen by a controller callback (same contract as
 * dt_gui_get_current_event()). */
static inline guint32 dt_gui_get_current_event_time(GtkEventController *controller)
{
#if GTK_CHECK_VERSION(4, 0, 0)
  GdkEvent *event = gtk_event_controller_get_current_event(controller);
  return event ? gdk_event_get_time(event) : GDK_CURRENT_TIME;
#else
  return gtk_get_current_event_time();
#endif
}

/* Current event as seen by a controller callback.  GTK4 reads the borrowed
 * event from the controller (do NOT free); GTK3 returns an owned copy of
 * gtk_get_current_event() which the caller must gdk_event_free(). */
static inline GdkEvent *dt_gui_get_current_event(GtkEventController *controller)
{
#if GTK_CHECK_VERSION(4, 0, 0)
  return gtk_event_controller_get_current_event(controller);
#else
  return gtk_get_current_event();
#endif
}

/* Event coordinates in the frame the old root-coords convention used:
 * GTK3 root (screen-absolute) coordinates, GTK4 the surface-relative
 * position (GTK4 has no root-coords API).  Delta-based tracking (panel
 * drag, culling/thumbtable pan) is unaffected; absolute positioning needs
 * a coordinate decision on the GTK4 port. */
static inline void dt_gui_get_event_coords(const GdkEvent *event,
                                           gdouble *x,
                                           gdouble *y)
{
#if GTK_CHECK_VERSION(4, 0, 0)
  gdk_event_get_position(event, x, y);
#else
  gdk_event_get_root_coords(event, x, y);
#endif
}

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

/** \brief swap the container in the left and right panels */
void dt_ui_container_swap_left_right(struct dt_ui_t *ui,
                                     gboolean swap);
/** \brief add's a widget to a defined container */
void dt_ui_container_add_widget(const struct dt_ui_t *ui,
                                const dt_ui_container_t c,
                                GtkWidget *w);
/** \brief gives a widget focus in the container */
void dt_ui_container_focus_widget(const struct dt_ui_t *ui,
                                  const dt_ui_container_t c,
                                  GtkWidget *w);
/** \brief calls a callback on all children widgets from container */
void dt_ui_container_foreach(const struct dt_ui_t *ui,
                             const dt_ui_container_t c,
                             GtkCallback callback);
/** \brief destroy all child widgets from container */
void dt_ui_container_destroy_children(const struct dt_ui_t *ui,
                                      const dt_ui_container_t c);
/** \brief shows/hide a panel */
void dt_ui_panel_show(const struct dt_ui_t *ui,
                      const dt_ui_panel_t,
                      const gboolean show,
                      const gboolean write);
/** \brief restore saved state of panel visibility for current view */
void dt_ui_restore_panels(const struct dt_ui_t *ui);
/** \brief update scrollbars for current view */
void dt_ui_update_scrollbars(struct dt_ui_t *ui);
/** show or hide scrollbars */
void dt_ui_scrollbars_show(struct dt_ui_t *ui, const gboolean show);
/** \brief toggle view of panels eg. collapse/expands to previous view state */
void dt_ui_toggle_panels_visibility(const struct dt_ui_t *ui);
/** \brief draw user's attention */
void dt_ui_notify_user();
/** \brief get visible state of panel */
gboolean dt_ui_panel_visible(const struct dt_ui_t *ui,
                             const dt_ui_panel_t);
/**  \brief get width of right, left, or bottom panel */
int dt_ui_panel_get_size(struct dt_ui_t *ui,
                         const dt_ui_panel_t p);
/**  \brief set width of right, left, or bottom panel */
void dt_ui_panel_set_size(const struct dt_ui_t *ui,
                          const dt_ui_panel_t p,
                          int s);
/** \brief is the panel ancestor of widget */
gboolean dt_ui_panel_ancestor(const struct dt_ui_t *ui,
                              const dt_ui_panel_t p,
                              GtkWidget *w);
/** \brief get the center drawable widget */
GtkWidget *dt_ui_center(const struct dt_ui_t *ui);
GtkWidget *dt_ui_center_base(const struct dt_ui_t *ui);
GtkWidget *dt_ui_snapshot(const struct dt_ui_t *ui);
/** \brief get the main window widget */
GtkWidget *dt_ui_main_window(const struct dt_ui_t *ui);
/** \brief get the thumb table */
struct dt_thumbtable_t *dt_ui_thumbtable(const struct dt_ui_t *ui);
/** \brief get the log message widget */
GtkWidget *dt_ui_log_msg(const struct dt_ui_t *ui);
/** \brief get the toast message widget */
GtkWidget *dt_ui_toast_msg(const struct dt_ui_t *ui);

GtkBox *dt_ui_get_container(const struct dt_ui_t *ui,
                            const dt_ui_container_t c);

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
  g_object_set(label, "halign", GTK_ALIGN_START, "xalign", 0.0f, "ellipsize", PANGO_ELLIPSIZE_END, (void *)0);
  return label;
};

static inline GtkWidget *dt_ui_entry_new(gint width_chars)
{
  GtkWidget *entry = gtk_entry_new();
  gtk_drag_dest_unset(entry);
  gtk_entry_set_width_chars(GTK_ENTRY(entry), width_chars);
  return entry;
};

extern const struct dt_action_def_t dt_action_def_tabs_all_rgb;
extern const struct dt_action_def_t dt_action_def_tabs_rgb;
extern const struct dt_action_def_t dt_action_def_tabs_none;

GtkNotebook *dt_ui_notebook_new(struct dt_action_def_t *def);

GtkWidget *dt_ui_notebook_page(GtkNotebook *notebook,
                               const char *text,
                               const char *tooltip);

// show a dialog box with 2 buttons in case some user interaction is
// required BEFORE dt's gui is initialised.  this expects gtk_init()
// to be called already which should be the case during most of dt's
// init phase.
gboolean dt_gui_show_standalone_yes_no_dialog(const char *title,
                                              const char *markup,
                                              const char *no_text,
                                              const char *yes_text);

// similar to the one above. this one asks the user for some
// string. the hint is shown in the empty entry box
char *dt_gui_show_standalone_string_dialog(const char *title,
                                           const char *markup,
                                           const char *placeholder,
                                           const char *no_text,
                                           const char *yes_text);

// returns TRUE if YES was answered, FALSE otherwise
gboolean dt_gui_show_yes_no_dialog(const char *title,
                                   const char *wname,
                                   const char *format, ...);

void dt_gui_add_help_link(GtkWidget *widget,
                          const char *link);
char *dt_gui_get_help_url(GtkWidget *widget);
void dt_gui_dialog_add_help(GtkDialog *dialog,
                            const char *topic);
void dt_gui_show_help(GtkWidget *widget);

// load a CSS theme
void dt_gui_load_theme(const char *theme); // read them and add user tweaks
void dt_gui_apply_theme();                 // apply the loaded theme to darktable's windows

// reload GUI scalings
void dt_configure_ppd_dpi(dt_gui_gtk_t *gui);

// translate key press events to remove any modifiers used to produce the keyval
// for example when the shift key is used to create the asterisk character
guint dt_gui_translated_key_state(const GdkEventKey *event);

// return modifier keys currently pressed, independent of any key event
GdkModifierType dt_key_modifier_state();

GtkWidget *dt_ui_resize_wrap(GtkWidget *w,
                             const gint min_size,
                             char *config_str);

// check whether the given container has any user-added children
gboolean dt_gui_container_has_children(GtkContainer *container);
// return a count of the user-added children in the given container
int dt_gui_container_num_children(GtkContainer *container);
// return the first child of the given container
GtkWidget *dt_gui_container_first_child(GtkContainer *container);
// return the requested child of the given container, or NULL if it has fewer children
GtkWidget *dt_gui_container_nth_child(GtkContainer *container,
                                      const int which);

// remove all of the children we've added to the container.  Any which
// no longer have any references will be destroyed.
void dt_gui_container_remove_children(GtkContainer *container);

// delete all of the children we've added to the container.  Use this
// function only if you are SURE there are no other references to any
// of the children (if in doubt, use dt_gui_container_remove_children
// instead; it's a bit slower but safer).
void dt_gui_container_destroy_children(GtkContainer *container);

void dt_gui_menu_popup(GtkMenu *menu,
                       GtkWidget *button,
                       GdkGravity widget_anchor,
                       GdkGravity menu_anchor);

void dt_gui_draw_rounded_rectangle(cairo_t *cr,
                                   const float width,
                                   const float height,
                                   const float x,
                                   const float y);

void dt_gui_widget_reallocate_now(GtkWidget *widget);

// event handler for "key-press-event" of GtkTreeView to decide if
// focus switches to GtkSearchEntry
gboolean dt_gui_search_start(GtkWidget *widget,
                             GdkEventKey *event,
                             GtkSearchEntry *entry);

// event handler for "stop-search" of GtkSearchEntry
void dt_gui_search_stop(GtkSearchEntry *entry,
                        GtkWidget *widget);

// create a collapsible section, insert in parent, return the container
void dt_gui_new_collapsible_section(dt_gui_collapsible_section_t *cs,
                                    const char *confname,
                                    const char *label,
                                    GtkBox *parent,
                                    struct dt_action_t *module);
// update the collapsible section's label text
void dt_gui_collapsible_section_set_label(dt_gui_collapsible_section_t *cs,
                                          const char *label);
// routine to be called from gui_update
void dt_gui_update_collapsible_section(const dt_gui_collapsible_section_t *cs);

// routine to hide the collapsible section
void dt_gui_hide_collapsible_section(const dt_gui_collapsible_section_t *cs);

// is delay between first and second click/press longer than double-click time?
gboolean dt_gui_long_click(const guint second,
                           const guint first);

#define ASSERT_FUNC_TYPE(func, expected_type) (void)((expected_type)(func))

/*
 * Give our controller reference to the widget, like GTK4's
 * gtk_widget_add_controller() (transfer full).  On GTK3 the widget only weakly
 * points at the controller, so we unref on "destroy" — never from a weak
 * notify on the widget: the controller's dispose removes its own weak pointer,
 * which reenters the weak reference list being torn down and corrupts the heap.
 *
 * GTK4 migration: keep the calls, drop the widget argument from the
 * gtk_gesture_*_new() / gtk_event_controller_*_new() calls above them.
 */
void dt_gui_add_controller(GtkWidget *widget,
                           gpointer controller);

/*
 * Root (screen-absolute) coordinates of the current event, or FALSE when
 * there is no current event.  Owns and releases the gtk_get_current_event()
 * copy internally (GTK3 returns transfer-full).
 *
 * GTK4 migration: gtk_get_current_event() disappears; use
 * gtk_gesture_get_last_event()/gtk_event_controller_get_current_event()
 * (borrowed) and drop this helper.
 */
gboolean dt_gui_get_current_root_coords(gdouble *x, gdouble *y);
/* Current modifier state as seen by a controller callback.  GTK4 reads it from
 * the controller; GTK3 (which has no gtk_event_controller_get_current_event*)
 * reads the event currently being dispatched, same as dt_gui_get_current_root_coords. */
GdkModifierType dt_gui_get_current_event_state(GtkEventController *controller);

GtkGestureSingle *(dt_gui_connect_click)(GtkWidget *widget,
                                         GCallback pressed,
                                         GCallback released,
                                         gpointer data);
#define dt_gui_connect_click(widget, pressed, released, data) ( \
  ASSERT_FUNC_TYPE(pressed, void(*)(GtkGestureSingle *, int, double, double, __typeof__(data))), \
  ASSERT_FUNC_TYPE(released, void(*)(GtkGestureSingle *, int, double, double, __typeof__(data))), \
  dt_gui_connect_click(GTK_WIDGET(widget), G_CALLBACK(pressed), G_CALLBACK(released), (data)))
/* dt_gui_connect_click() already listens to any button (button=0): callbacks
 * decide via gtk_gesture_single_get_current_button(), as the old
 * button-press-event handlers did.  This alias only documents that intent. */
#define dt_gui_connect_click_all(widget, pressed, released, data) \
  dt_gui_connect_click(widget, pressed, released, data)

GtkGestureSingle *(dt_gui_connect_click_secondary)(GtkWidget *widget,
                                                   GCallback pressed,
                                                   GCallback released,
                                                   gpointer data);
/* dt_gui_connect_click() restricted to the right button: the gesture's button
 * filter does the exclusivity, so the handler needs no button check.  This is
 * the right-click-popup idiom, previously written by hand at every call site. */
#define dt_gui_connect_click_secondary(widget, pressed, released, data) ( \
  ASSERT_FUNC_TYPE(pressed, void(*)(GtkGestureSingle *, int, double, double, __typeof__(data))), \
  ASSERT_FUNC_TYPE(released, void(*)(GtkGestureSingle *, int, double, double, __typeof__(data))), \
  dt_gui_connect_click_secondary(GTK_WIDGET(widget), G_CALLBACK(pressed), G_CALLBACK(released), (data)))

/* claim the event sequence for a gesture so that all its events (press,
 * release, motion) are consumed and never reach the widget's class handlers
 * or other controllers.  Connect to the gesture's "begin" signal; implies
 * GTK_PHASE_CAPTURE.  Same pattern as dt_iop_togglebutton_new and the
 * standalone picker buttons, survives GTK4 unchanged. */
void dt_gui_gesture_claim(GtkGesture *gesture,
                          GdkEventSequence *sequence,
                          gpointer user_data);

GtkGesture *(dt_gui_connect_drag)(GtkWidget *widget,
                                  GCallback drag_begin,
                                  GCallback drag_end,
				  GCallback drag_update,
                                  gpointer data);
#define dt_gui_connect_drag(widget, drag_begin, drag_end, drag_update, data) ( \
  ASSERT_FUNC_TYPE(drag_begin, void(*)(GtkGestureDrag *, double, double, __typeof__(data))), \
  ASSERT_FUNC_TYPE(drag_end, void(*)(GtkGestureDrag *, double, double, __typeof__(data))), \
  ASSERT_FUNC_TYPE(drag_update, void(*)(GtkGestureDrag *, double, double, __typeof__(data))), \
  dt_gui_connect_drag(GTK_WIDGET(widget), G_CALLBACK(drag_begin), G_CALLBACK(drag_end), G_CALLBACK(drag_update), (data)))

/* touchpad pinch via GtkGestureZoom: the old GTK3-only "event" signal
 * handler forwarded raw GDK_TOUCHPAD_PINCH events; the phase field becomes
 * the begin / scale-changed / end signals (and "end" fires on cancel as
 * well, so the consumer's END/CANCEL reset still runs).  GtkGestureZoom also
 * recognizes touchscreen pinches, which were never handled before -- ignored
 * here for parity, only touchpad pinches reach the handler.  dx/dy, scale,
 * state and the focal point are pulled from the gesture's last event (root
 * coords on GTK3, surface-relative on GTK4 -- see dt_gui_get_event_coords);
 * on END the event is already gone, so the handler gets NULL and the deltas
 * default to zero.  The touchpad_gestures_enabled pref and the per-gesture
 * active tracking live inside the helper; handlers only forward the parsed
 * event (e.g. to dt_view_manager_gesture_pinch). */
typedef struct dt_gui_pinch_event_t
{
  const GdkEvent *event;          /* borrowed, NULL on END (also on cancel) */
  GdkTouchpadGesturePhase phase;  /* BEGIN / UPDATE / END */
  gdouble x, y;                   /* focal point */
  gdouble dx, dy;                 /* pan deltas */
  gdouble scale;                  /* pinch scale */
  guint state;                    /* modifier state (low 4 bits) */
} dt_gui_pinch_event_t;

typedef void (*dt_gui_pinch_handler_t)(GtkGesture *gesture,
                                       const dt_gui_pinch_event_t *event,
                                       gpointer user_data);

GtkGesture *(dt_gui_connect_pinch)(GtkWidget *widget,
                                   dt_gui_pinch_handler_t handler,
                                   gpointer data);
#define dt_gui_connect_pinch(widget, handler, data) ( \
  ASSERT_FUNC_TYPE(handler, void(*)(GtkGesture *, const dt_gui_pinch_event_t *, __typeof__(data))), \
  dt_gui_connect_pinch(GTK_WIDGET(widget), (handler), (data)))

GtkEventController *(dt_gui_connect_motion)(GtkWidget *widget,
                                            GCallback motion,
                                            GCallback enter,
                                            GCallback leave,
                                            gpointer data);
#define dt_gui_connect_motion(widget, motion, enter, leave, data) ( \
  ASSERT_FUNC_TYPE(motion, void(*)(GtkEventControllerMotion *, double, double, __typeof__(data))), \
  ASSERT_FUNC_TYPE(enter, void(*)(GtkEventControllerMotion *, double, double, __typeof__(data))), \
  ASSERT_FUNC_TYPE(leave, void(*)(GtkEventControllerMotion *, __typeof__(data))), \
  dt_gui_connect_motion(GTK_WIDGET(widget), G_CALLBACK(motion), G_CALLBACK(enter), G_CALLBACK(leave), (data)))

GtkEventController *(dt_gui_connect_scroll)(GtkWidget *widget,
					    GtkEventControllerScrollFlags flags,
                                            GCallback scroll,
                                            gpointer data);
#define dt_gui_connect_scroll(widget, flags, scroll, data) ( \
  ASSERT_FUNC_TYPE(scroll, void(*)(GtkEventControllerScroll *, double, double, __typeof__(data))), \
  dt_gui_connect_scroll(GTK_WIDGET(widget), (flags), G_CALLBACK(scroll), (data)))

GtkEventController *(dt_gui_connect_key)(GtkWidget *widget,
                                          GCallback pressed,
                                          gpointer data);
#define dt_gui_connect_key(widget, pressed, data) ( \
  ASSERT_FUNC_TYPE(pressed, gboolean(*)(GtkEventControllerKey *, guint, guint, GdkModifierType, __typeof__(data))), \
  dt_gui_connect_key(GTK_WIDGET(widget), G_CALLBACK(pressed), (data)))

#define dt_gui_get_widget(controller) \
      gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(controller))

/* object-data key on the gesture carrying a shortcut-activated press'
 * button+state, encoded as (state << 8) | button (see
 * _action_process_toggle/_action_process_button).  Set right before the
 * synthetic "pressed" emit and cleared after it, so a NULL means the press
 * came from a real event. */
#define DT_ACTION_GESTURE_SYNTH_KEY "_dt_action_gesture_synth"

/* button of the current gesture press; for a shortcut-activated press the
 * effect-determined button, else the real press' button (GtkGestureSingle
 * resets current-button to 0 on release, so a 0 is never a real press and
 * stands for a primary click) */
static inline guint dt_gui_current_button(GtkGestureSingle *gesture)
{
  const gpointer synth = g_object_get_data(G_OBJECT(gesture), DT_ACTION_GESTURE_SYNTH_KEY);
  if(synth) return GPOINTER_TO_INT(synth) & 0xff;
  const guint button = gtk_gesture_single_get_current_button(gesture);
  return button ? button : GDK_BUTTON_PRIMARY;
}

/* modifiers of the current gesture press: for a shortcut-activated press the
 * effect-determined modifiers (the action effect wins over whatever key
 * produced the shortcut), else the current event's */
static inline GdkModifierType dt_gui_current_state(GtkGestureSingle *gesture)
{
  const gpointer synth = g_object_get_data(G_OBJECT(gesture), DT_ACTION_GESTURE_SYNTH_KEY);
  if(synth) return (GdkModifierType)(GPOINTER_TO_INT(synth) >> 8);
  GdkModifierType state;
  gtk_get_current_event_state(&state);
  return state;
}

#define dt_gui_claim(gesture) \
      gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED)
#define dt_gui_deny(gesture) \
      gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_DENIED)

// GTK4 gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(controller));
#define dt_modifier_eq(controller, mask)\
  dt_modifier_is(dt_key_modifier_state(), mask)

// control whether the mouse pointer displays as a "busy" cursor, e.g. watch or timer
// the calls may be nested, but must be matched
void dt_gui_cursor_set_busy();
void dt_gui_cursor_clear_busy();

// run all pending Gtk/GDK events
// should be called after making Gtk calls if we won't resume the main event loop for a while
// (i.e. the current function will do a lot of work before returning)
void dt_gui_process_events();

#ifdef __cplusplus
extern "C++"
{
template<typename... Widgets>
inline GtkWidget *dt_gui_box_add(gpointer box, Widgets*... w)
{
  // fold expression: expands to gtk_container_add(box, a), gtk_container_add(box, b), ...
  (gtk_container_add(GTK_CONTAINER(box), GTK_WIDGET(w)), ...);
  return GTK_WIDGET(box);
}
}
#else
GtkWidget *(dt_gui_box_add)(const char *file, const int line, const char *function, GtkBox *box, gpointer list[]);
#define dt_gui_box_add(box, ...) dt_gui_box_add(__FILE__, __LINE__, __FUNCTION__, GTK_BOX(box), (gpointer[]){ __VA_ARGS__ __VA_OPT__(,) (gpointer)-1 })
#endif
#define dt_gui_hbox(...) dt_gui_box_add(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0) __VA_OPT__(,) __VA_ARGS__)
#define dt_gui_vbox(...) dt_gui_box_add(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0) __VA_OPT__(,) __VA_ARGS__)
#define dt_gui_dialog_add(dialog, ...) dt_gui_box_add(gtk_dialog_get_content_area(GTK_DIALOG(dialog)), __VA_ARGS__)
#define dt_gui_expand(widget) dt_gui_expand(GTK_WIDGET(widget))
#define dt_gui_align_right(widget) dt_gui_align_right(GTK_WIDGET(widget))

static inline GtkWidget *(dt_gui_expand)(GtkWidget *widget)
{
  gtk_widget_set_hexpand(widget, TRUE);
  return widget;
}

static inline GtkWidget *(dt_gui_align_right)(GtkWidget *widget)
{
  gtk_widget_set_halign(widget, GTK_ALIGN_END);
  return dt_gui_expand(widget);
}

static inline GtkWidget *dt_gui_scroll_wrap(GtkWidget *widget)
{
  GtkWidget *scrolled_window = gtk_scrolled_window_new(NULL, NULL);
  gtk_widget_set_vexpand(scrolled_window, TRUE);
  gtk_container_add(GTK_CONTAINER(scrolled_window), widget);
  return scrolled_window;
}

/* Menu items: the shell emits "activate" for mouse clicks as well as for
 * keyboard (Enter/mnemonic/accel) activation, so handlers that apply on
 * press (item gestures, see the menu conversion commits) mark the item
 * here and skip the release-time activate.  This replaces the
 * gtk_get_current_event() GDK_KEY_PRESS check, which does not exist in
 * GTK4. */
#define DT_GUI_MENUITEM_MOUSE_KEY "dt-gui-menuitem-mouse"

static inline void dt_gui_menuitem_mark_pressed(GtkWidget *menuitem)
{
  g_object_set_data(G_OBJECT(menuitem), DT_GUI_MENUITEM_MOUSE_KEY, GINT_TO_POINTER(TRUE));
}

static inline gboolean dt_gui_menuitem_activated_by_keyboard(GtkWidget *menuitem)
{
  if(g_object_get_data(G_OBJECT(menuitem), DT_GUI_MENUITEM_MOUSE_KEY))
  {
    g_object_set_data(G_OBJECT(menuitem), DT_GUI_MENUITEM_MOUSE_KEY, NULL);
    return FALSE;
  }
  return TRUE;
}

// Setup auto-commit on focus loss for editable renderers
void dt_gui_commit_on_focus_loss(GtkCellRenderer *renderer,
                                 GtkCellEditable **active_editable);

// restore dialog size from config file
void dt_gui_dialog_restore_size(GtkDialog *dialog,
                                const char *conf);

PangoFontDescription *dt_gui_get_font(void);

// returns the session type at runtime
dt_gui_session_type_t dt_gui_get_session_type(void);

#if !defined(__cplusplus)
#undef G_CALLBACK
static inline GCallback G_CALLBACK(void *f) { return (GCallback)f; } // as a macro it gets expanded before reaching here
#define DISABLINGPREFIXG_CALLBACK
#define BOOLSIGNAL(s, signal) || !strcmp(s, #signal)
#undef _Static_assert
#undef  g_signal_connect
#define g_signal_connect(instance, signal, c_handler, user_data) do { \
  _Static_assert(((strlen(signal)>4 && !strcmp("event", &signal[strlen(signal)-5])) \
    BOOLSIGNAL(signal, drag-motion) \
    BOOLSIGNAL(signal, drag-failed) \
    BOOLSIGNAL(signal, drag-drop) \
    BOOLSIGNAL(signal, focus) \
    BOOLSIGNAL(signal, draw) \
    BOOLSIGNAL(signal, popup-menu) \
    BOOLSIGNAL(signal, query-tooltip) \
    BOOLSIGNAL(signal, match-selected) \
    BOOLSIGNAL(signal, get-child-position) \
    ) == _Generic((DISABLINGPREFIX##c_handler), gboolean(*)(): TRUE, default: FALSE), \
    "signal " signal " return type does not match specified handler " #c_handler); \
  g_signal_connect_data((instance), (signal), (GCallback)(c_handler), (user_data), NULL, (GConnectFlags) 0); } while(0)
#endif // __cplusplus

G_END_DECLS

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
