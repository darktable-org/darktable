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
#include "common/darktable.h"
#ifdef HAVE_GPHOTO2
#include "common/camera_control.h"
#endif
#include "common/collection.h"
#include "common/colorspaces.h"
#include "common/file_location.h"
#include "common/image.h"
#include "common/image_cache.h"
#include "gui/guides.h"
#include "bauhaus/bauhaus.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "dtgtk/button.h"
#include "dtgtk/expander.h"
#include "dtgtk/sidepanel.h"
#include "dtgtk/thumbtable.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"

#include "common/styles.h"
#include "control/conf.h"
#include "control/control.h"
#include "control/jobs.h"
#include "control/signal.h"
#include "gui/presets.h"
#include "views/view.h"

#include <gdk/gdkkeysyms.h>
#ifdef GDK_WINDOWING_WAYLAND
#include <gdk/gdkwayland.h>
#endif
#include <gtk/gtk.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef MAC_INTEGRATION
#include <gtkosxapplication.h>
#endif
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif
#include <pthread.h>

/*
 * NEW UI API
 */

#define DT_UI_PANEL_MODULE_SPACING 0
#define DT_UI_PANEL_SIDE_DEFAULT_SIZE 350
#define DT_UI_PANEL_BOTTOM_DEFAULT_SIZE 120

typedef enum dt_gui_view_switch_t
{
  DT_GUI_VIEW_SWITCH_TO_TETHERING = 1,
  DT_GUI_VIEW_SWITCH_TO_LIGHTTABLE,
  DT_GUI_VIEW_SWITCH_TO_DARKROOM,
  DT_GUI_VIEW_SWITCH_TO_MAP,
  DT_GUI_VIEW_SWITCH_TO_SLIDESHOW,
  DT_GUI_VIEW_SWITCH_TO_PRINT
} dt_gui_view_switch_to_t;

const char *_ui_panel_config_names[]
    = { "header", "toolbar_top", "toolbar_bottom", "left", "right", "bottom" };

typedef struct dt_ui_t
{
  /* container widgets */
  GtkWidget *containers[DT_UI_CONTAINER_SIZE];

  /* panel widgets */
  GtkWidget *panels[DT_UI_PANEL_SIZE];

  /* center widget */
  GtkWidget *center;
  GtkWidget *center_base;

  /* main widget */
  GtkWidget *main_window;

  /* thumb table */
  dt_thumbtable_t *thumbtable;

  /* log msg and toast labels */
  GtkWidget *log_msg, *toast_msg;
} dt_ui_t;

/* initialize the whole left panel */
static void _ui_init_panel_left(struct dt_ui_t *ui, GtkWidget *container);
/* initialize the whole right panel */
static void _ui_init_panel_right(dt_ui_t *ui, GtkWidget *container);
/* initialize the top container of panel */
static GtkWidget *_ui_init_panel_container_top(GtkWidget *container);
/* initialize the center container of panel */
static GtkWidget *_ui_init_panel_container_center(GtkWidget *container, gboolean left);
/* initialize the bottom container of panel */
static GtkWidget *_ui_init_panel_container_bottom(GtkWidget *container);
/* initialize the top container of panel */
static void _ui_init_panel_top(dt_ui_t *ui, GtkWidget *container);
/* initialize the center top panel */
static void _ui_init_panel_center_top(dt_ui_t *ui, GtkWidget *container);
/* initialize the center bottom panel */
static void _ui_init_panel_center_bottom(dt_ui_t *ui, GtkWidget *container);
/* initialize the bottom panel */
static void _ui_init_panel_bottom(dt_ui_t *ui, GtkWidget *container);
/* generic callback for redraw widget signals */
static void _ui_widget_redraw_callback(gpointer instance, GtkWidget *widget);
/* callback for redraw log signals */
static void _ui_log_redraw_callback(gpointer instance, GtkWidget *widget);
static void _ui_toast_redraw_callback(gpointer instance, GtkWidget *widget);

// set class function to add CSS classes with just a simple line call
void dt_gui_add_class(GtkWidget *widget, const gchar *class_name)
{
  GtkStyleContext *context = gtk_widget_get_style_context(widget);
  gtk_style_context_add_class(context, class_name);
}

void dt_gui_remove_class(GtkWidget *widget, const gchar *class_name)
{
  GtkStyleContext *context = gtk_widget_get_style_context(widget);
  gtk_style_context_remove_class(context, class_name);
}

/*
 * OLD UI API
 */
static void _init_widgets(dt_gui_gtk_t *gui);

static void _init_main_table(GtkWidget *container);

static void _fullscreen_key_accel_callback(dt_action_t *action)
{
  GtkWidget *widget = darktable.develop &&
                      darktable.develop->second_window.second_wnd &&
                      gtk_window_is_active(GTK_WINDOW(darktable.develop->second_window.second_wnd))
                    ? darktable.develop->second_window.second_wnd
                    : dt_ui_main_window(darktable.gui->ui);

  if(gdk_window_get_state(gtk_widget_get_window(widget)) & GDK_WINDOW_STATE_FULLSCREEN)
    gtk_window_unfullscreen(GTK_WINDOW(widget));
  else
    gtk_window_fullscreen(GTK_WINDOW(widget));

  dt_dev_invalidate(darktable.develop);

  /* redraw center view */
  gtk_widget_queue_draw(widget);

#ifdef __APPLE__
  // workaround for GTK Quartz backend bug
  gtk_window_set_title(GTK_WINDOW(widget), widget == dt_ui_main_window(darktable.gui->ui)
                                         ? "darktable" : _("darktable - darkroom preview"));
#endif
}

static void _toggle_tooltip_visibility(dt_action_t *action)
{
  if(gdk_screen_is_composited(gdk_screen_get_default()))
  {
    gboolean tooltip_hidden = !dt_conf_get_bool("ui/hide_tooltips");
    dt_conf_set_bool("ui/hide_tooltips", tooltip_hidden);
    if(tooltip_hidden)
      dt_toast_log(_("tooltips off"));
    else
      dt_toast_log(_("tooltips on"));
  }
  else
  {
    dt_conf_set_bool("ui/hide_tooltips", FALSE);
    dt_control_log(_("tooltip visibility can only be toggled if compositing is enabled in your window manager"));
  }

  const char *theme = dt_conf_get_string_const("ui_last/theme");
  dt_gui_load_theme(theme);
  dt_bauhaus_load_theme();
}

static inline void _update_focus_peaking_button()
{
  // read focus peaking global state and update toggle button accordingly
  dt_pthread_mutex_lock(&darktable.gui->mutex);
  const gboolean state = darktable.gui->show_focus_peaking;
  dt_pthread_mutex_unlock(&darktable.gui->mutex);

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(darktable.gui->focus_peaking_button), state);
}

static void _focuspeaking_switch_button_callback(GtkWidget *button, gpointer user_data)
{
  // button method
  dt_pthread_mutex_lock(&darktable.gui->mutex);
  const gboolean state_memory = darktable.gui->show_focus_peaking;
  dt_pthread_mutex_unlock(&darktable.gui->mutex);

  const gboolean state_new = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(button));

  if(state_memory == state_new) return; // nothing to change, bypass

  dt_pthread_mutex_lock(&darktable.gui->mutex);
  darktable.gui->show_focus_peaking = state_new;
  dt_pthread_mutex_unlock(&darktable.gui->mutex);

  gtk_widget_queue_draw(button);

  // we inform that all thumbnails need to be redraw
  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_DEVELOP_MIPMAP_UPDATED, -1);
}

static gchar *_panels_get_view_path(char *suffix)
{
  if(!darktable.view_manager) return NULL;
  const dt_view_t *cv = dt_view_manager_get_current_view(darktable.view_manager);
  // in lighttable, we store panels states per layout
  char lay[32] = "";
  if(g_strcmp0(cv->module_name, "lighttable") == 0)
  {
    if(dt_view_lighttable_preview_state(darktable.view_manager))
      g_snprintf(lay, sizeof(lay), "preview/");
    else
      g_snprintf(lay, sizeof(lay), "%d/", dt_view_lighttable_get_layout(darktable.view_manager));
  }
  else if(g_strcmp0(cv->module_name, "darkroom") == 0)
  {
    g_snprintf(lay, sizeof(lay), "%d/", dt_view_darkroom_get_layout(darktable.view_manager));
  }

  return g_strdup_printf("%s/ui/%s%s", cv->module_name, lay, suffix);
}

static gchar *_panels_get_panel_path(dt_ui_panel_t panel, char *suffix)
{
  gchar *v = _panels_get_view_path("");
  if(!v) return NULL;
  return dt_util_dstrcat(v, "%s%s", _ui_panel_config_names[panel], suffix);
}

static gboolean _panel_is_visible(dt_ui_panel_t panel)
{
  gchar *key = _panels_get_view_path("panel_collaps_state");
  if(dt_conf_get_int(key))
  {
    g_free(key);
    return FALSE;
  }
  key = _panels_get_panel_path(panel, "_visible");
  const gboolean ret = dt_conf_get_bool(key);
  g_free(key);
  return ret;
}

static void _panels_controls_accel_callback(dt_action_t *action)
{
  gchar *key = _panels_get_view_path("panels_collapse_controls");
  gboolean visible = TRUE;
  if(dt_conf_key_exists(key)) visible = dt_conf_get_bool(key);

  // Inverse the current parameter and save it
  visible = !visible;
  dt_conf_set_bool(key, visible);
  g_free(key);

  // Show/hide the collapsing controls in the borders
  gtk_widget_set_visible(GTK_WIDGET(darktable.gui->widgets.right_border), visible);
  gtk_widget_set_visible(GTK_WIDGET(darktable.gui->widgets.left_border), visible);
  gtk_widget_set_visible(GTK_WIDGET(darktable.gui->widgets.top_border), visible);
  gtk_widget_set_visible(GTK_WIDGET(darktable.gui->widgets.bottom_border), visible);
}

static void _panel_toggle(dt_ui_border_t border, dt_ui_t *ui)
{
  switch(border)
  {
    case DT_UI_BORDER_LEFT: // left border
    {
      dt_ui_panel_show(ui, DT_UI_PANEL_LEFT, !_panel_is_visible(DT_UI_PANEL_LEFT), TRUE);
    }
    break;

    case DT_UI_BORDER_RIGHT: // right border
    {
      dt_ui_panel_show(ui, DT_UI_PANEL_RIGHT, !_panel_is_visible(DT_UI_PANEL_RIGHT), TRUE);
    }
    break;

    case DT_UI_BORDER_TOP: // top border
    {
      const gboolean show_ct = _panel_is_visible(DT_UI_PANEL_CENTER_TOP);
      const gboolean show_t = _panel_is_visible(DT_UI_PANEL_TOP);
      // all visible => toolbar hidden => all hidden => toolbar visible => all visible
      if(show_ct && show_t)
        dt_ui_panel_show(ui, DT_UI_PANEL_CENTER_TOP, FALSE, TRUE);
      else if(!show_ct && show_t)
        dt_ui_panel_show(ui, DT_UI_PANEL_TOP, FALSE, TRUE);
      else if(!show_ct && !show_t)
        dt_ui_panel_show(ui, DT_UI_PANEL_CENTER_TOP, TRUE, TRUE);
      else
        dt_ui_panel_show(ui, DT_UI_PANEL_TOP, TRUE, TRUE);
    }
    break;

    case DT_UI_BORDER_BOTTOM: // bottom border
    default:
    {
      const gboolean show_cb = _panel_is_visible(DT_UI_PANEL_CENTER_BOTTOM);
      const gboolean show_b = _panel_is_visible(DT_UI_PANEL_BOTTOM);
      // all visible => toolbar hidden => all hidden => toolbar visible => all visible
      if(show_cb && show_b)
        dt_ui_panel_show(ui, DT_UI_PANEL_CENTER_BOTTOM, FALSE, TRUE);
      else if(!show_cb && show_b)
        dt_ui_panel_show(ui, DT_UI_PANEL_BOTTOM, FALSE, TRUE);
      else if(!show_cb && !show_b)
        dt_ui_panel_show(ui, DT_UI_PANEL_CENTER_BOTTOM, TRUE, TRUE);
      else
        dt_ui_panel_show(ui, DT_UI_PANEL_BOTTOM, TRUE, TRUE);
    }
    break;
  }
}

static void _toggle_side_borders_accel_callback(dt_action_t *action)
{
  /* toggle panel viewstate */
  dt_ui_toggle_panels_visibility(darktable.gui->ui);

  /* trigger invalidation of centerview to reprocess pipe */
  dt_dev_invalidate(darktable.develop);
  gtk_widget_queue_draw(dt_ui_center(darktable.gui->ui));
}

static void _toggle_panel_accel_callback(dt_action_t *action)
{
  if(!strcmp(action->id, "left"))
    _panel_toggle(DT_UI_BORDER_LEFT, darktable.gui->ui);
  else if(!strcmp(action->id, "right"))
    _panel_toggle(DT_UI_BORDER_RIGHT, darktable.gui->ui);
  else if(!strcmp(action->id, "top"))
    _panel_toggle(DT_UI_BORDER_TOP, darktable.gui->ui);
  else
    _panel_toggle(DT_UI_BORDER_BOTTOM, darktable.gui->ui);
}

static void _toggle_header_accel_callback(dt_action_t *action)
{
  dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_TOP, !_panel_is_visible(DT_UI_PANEL_TOP), TRUE);
}
static void _toggle_filmstrip_accel_callback(dt_action_t *action)
{
  dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_BOTTOM, !_panel_is_visible(DT_UI_PANEL_BOTTOM), TRUE);
}
static void _toggle_top_tool_accel_callback(dt_action_t *action)
{
  dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_CENTER_TOP, !_panel_is_visible(DT_UI_PANEL_CENTER_TOP), TRUE);
}
static void _toggle_bottom_tool_accel_callback(dt_action_t *action)
{
  dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_CENTER_BOTTOM, !_panel_is_visible(DT_UI_PANEL_CENTER_BOTTOM),
                   TRUE);
}
static void _toggle_top_all_accel_callback(dt_action_t *action)
{
  const gboolean v = (_panel_is_visible(DT_UI_PANEL_CENTER_TOP) || _panel_is_visible(DT_UI_PANEL_TOP));
  dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_TOP, !v, TRUE);
  dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_CENTER_TOP, !v, TRUE);
}
static void _toggle_bottom_all_accel_callback(dt_action_t *action)
{
  const gboolean v = (_panel_is_visible(DT_UI_PANEL_CENTER_BOTTOM) || _panel_is_visible(DT_UI_PANEL_BOTTOM));
  dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_BOTTOM, !v, TRUE);
  dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_CENTER_BOTTOM, !v, TRUE);
}

static gboolean _borders_button_pressed(GtkWidget *w, GdkEventButton *event, gpointer user_data)
{
  _panel_toggle(GPOINTER_TO_INT(user_data), darktable.gui->ui);

  return TRUE;
}

gboolean dt_gui_ignore_scroll(GdkEventScroll *event)
{
  const gboolean ignore_without_mods = dt_conf_get_bool("darkroom/ui/sidebar_scroll_default");
  const GdkModifierType mods_pressed = (event->state & gtk_accelerator_get_default_mod_mask());

  if(mods_pressed == 0)
  {
    return ignore_without_mods;
  }
  else
  {
    if(mods_pressed == darktable.gui->sidebar_scroll_mask)
    {
      if(!ignore_without_mods) return TRUE;

      event->state &= ~darktable.gui->sidebar_scroll_mask;
    }

    return FALSE;
  }
}

gboolean dt_gui_get_scroll_deltas(const GdkEventScroll *event, gdouble *delta_x, gdouble *delta_y)
{
  // avoid double counting real and emulated events when receiving smooth scrolls
  if(gdk_event_get_pointer_emulated((GdkEvent*)event)) return FALSE;

  gboolean handled = FALSE;
  switch(event->direction)
  {
    // is one-unit cardinal, e.g. from a mouse scroll wheel
    case GDK_SCROLL_LEFT:
      if(delta_x)
      {
        *delta_x = -1.0;
        if(delta_y) *delta_y = 0.0;
        handled = TRUE;
      }
      break;
    case GDK_SCROLL_RIGHT:
      if(delta_x)
      {
        *delta_x = 1.0;
        if(delta_y) *delta_y = 0.0;
        handled = TRUE;
      }
      break;
    case GDK_SCROLL_UP:
      if(delta_y)
      {
        if(delta_x) *delta_x = 0.0;
        *delta_y = -1.0;
        handled = TRUE;
      }
      break;
    case GDK_SCROLL_DOWN:
      if(delta_y)
      {
        if(delta_x) *delta_x = 0.0;
        *delta_y = 1.0;
        handled = TRUE;
      }
      break;
    // is trackpad (or touch) scroll
    case GDK_SCROLL_SMOOTH:
      if((delta_x && event->delta_x != 0) || (delta_y && event->delta_y != 0))
      {
#ifdef GDK_WINDOWING_QUARTZ // on macOS deltas need to be scaled
        if(delta_x) *delta_x = event->delta_x / 50;
        if(delta_y) *delta_y = event->delta_y / 50;
#else
         if(delta_x) *delta_x = event->delta_x;
         if(delta_y) *delta_y = event->delta_y;
#endif
        handled = TRUE;
      }
    default:
      break;
    }
  return handled;
}

gboolean dt_gui_get_scroll_unit_deltas(const GdkEventScroll *event, int *delta_x, int *delta_y)
{
  // avoid double counting real and emulated events when receiving smooth scrolls
  if(gdk_event_get_pointer_emulated((GdkEvent*)event)) return FALSE;

  // accumulates scrolling regardless of source or the widget being scrolled
  static gdouble acc_x = 0.0, acc_y = 0.0;

  gboolean handled = FALSE;

  switch(event->direction)
  {
    // is one-unit cardinal, e.g. from a mouse scroll wheel
    case GDK_SCROLL_LEFT:
      if(delta_x)
      {
        *delta_x = -1;
        if(delta_y) *delta_y = 0;
        handled = TRUE;
      }
      break;
    case GDK_SCROLL_RIGHT:
      if(delta_x)
      {
        *delta_x = 1;
        if(delta_y) *delta_y = 0;
        handled = TRUE;
      }
      break;
    case GDK_SCROLL_UP:
      if(delta_y)
      {
        if(delta_x) *delta_x = 0;
        *delta_y = -1;
        handled = TRUE;
      }
      break;
    case GDK_SCROLL_DOWN:
      if(delta_y)
      {
        if(delta_x) *delta_x = 0;
        *delta_y = 1;
        handled = TRUE;
      }
      break;
    // is trackpad (or touch) scroll
    case GDK_SCROLL_SMOOTH:
      // stop events reset accumulated delta
      if(event->is_stop)
      {
        acc_x = acc_y = 0.0;
        break;
      }
      // accumulate trackpad/touch scrolls until they make a unit
      // scroll, and only then tell caller that there is a scroll to
      // handle
#ifdef GDK_WINDOWING_QUARTZ // on macOS deltas need to be scaled
      acc_x += event->delta_x / 50;
      acc_y += event->delta_y / 50;
#else
      acc_x += event->delta_x;
      acc_y += event->delta_y;
#endif
      const gdouble amt_x = trunc(acc_x);
      const gdouble amt_y = trunc(acc_y);
      if(amt_x != 0 || amt_y != 0)
      {
        acc_x -= amt_x;
        acc_y -= amt_y;
        if((delta_x && amt_x != 0) || (delta_y && amt_y != 0))
        {
          if(delta_x) *delta_x = (int)amt_x;
          if(delta_y) *delta_y = (int)amt_y;
          handled = TRUE;
        }
      }
      break;
    default:
      break;
  }
  return handled;
}

gboolean dt_gui_get_scroll_delta(const GdkEventScroll *event, gdouble *delta)
{
  gdouble delta_x, delta_y;
  if(dt_gui_get_scroll_deltas(event, &delta_x, &delta_y))
  {
    *delta = delta_x + delta_y;
    return TRUE;
  }
  return FALSE;
}

gboolean dt_gui_get_scroll_unit_delta(const GdkEventScroll *event, int *delta)
{
  int delta_x, delta_y;
  if(dt_gui_get_scroll_unit_deltas(event, &delta_x, &delta_y))
  {
    *delta = delta_x + delta_y;
    return TRUE;
  }
  return FALSE;
}

static gboolean _draw_borders(GtkWidget *widget, cairo_t *crf, gpointer user_data)
{
  // draw arrows on borders
  if(!dt_control_running()) return TRUE;
  const int which = GPOINTER_TO_INT(user_data);
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  const float width = allocation.width, height = allocation.height;
  cairo_surface_t *cst = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);

  GdkRGBA color;
  GtkStyleContext *context = gtk_widget_get_style_context(widget);
  gtk_render_background(context, cr, 0, 0, width, height);
  gtk_style_context_get_color(context, gtk_widget_get_state_flags(widget), &color);

  // draw gui arrows.
  gdk_cairo_set_source_rgba(cr, &color);

  switch(which)
  {
    case DT_UI_BORDER_LEFT:
      if(dt_ui_panel_visible(darktable.gui->ui, DT_UI_PANEL_LEFT))
      {
        cairo_move_to(cr, width, height / 2 - width);
        cairo_rel_line_to(cr, 0.0, 2 * width);
        cairo_rel_line_to(cr, -width, -width);
      }
      else
      {
        cairo_move_to(cr, 0.0, height / 2 - width);
        cairo_rel_line_to(cr, 0.0, 2 * width);
        cairo_rel_line_to(cr, width, -width);
      }
      break;
    case DT_UI_BORDER_RIGHT:
      if(dt_ui_panel_visible(darktable.gui->ui, DT_UI_PANEL_RIGHT))
      {
        cairo_move_to(cr, 0.0, height / 2 - width);
        cairo_rel_line_to(cr, 0.0, 2 * width);
        cairo_rel_line_to(cr, width, -width);
      }
      else
      {
        cairo_move_to(cr, width, height / 2 - width);
        cairo_rel_line_to(cr, 0.0, 2 * width);
        cairo_rel_line_to(cr, -width, -width);
      }
      break;
    case DT_UI_BORDER_TOP:
      if(dt_ui_panel_visible(darktable.gui->ui, DT_UI_PANEL_TOP))
      {
        cairo_move_to(cr, width / 2 - height, height);
        cairo_rel_line_to(cr, 2 * height, 0.0);
        cairo_rel_line_to(cr, -height, -height);
      }
      else
      {
        cairo_move_to(cr, width / 2 - height, 0.0);
        cairo_rel_line_to(cr, 2 * height, 0.0);
        cairo_rel_line_to(cr, -height, height);
      }
      break;
    default: // bottom
      if(dt_ui_panel_visible(darktable.gui->ui, DT_UI_PANEL_BOTTOM))
      {
        cairo_move_to(cr, width / 2 - height, 0.0);
        cairo_rel_line_to(cr, 2 * height, 0.0);
        cairo_rel_line_to(cr, -height, height);
      }
      else
      {
        cairo_move_to(cr, width / 2 - height, height);
        cairo_rel_line_to(cr, 2 * height, 0.0);
        cairo_rel_line_to(cr, -height, -height);
      }
      break;
  }
  cairo_close_path(cr);
  cairo_fill(cr);

  cairo_destroy(cr);
  cairo_set_source_surface(crf, cst, 0, 0);
  cairo_paint(crf);
  cairo_surface_destroy(cst);
  return TRUE;
}

static gboolean _draw(GtkWidget *da, cairo_t *cr, gpointer user_data)
{
  dt_control_expose(NULL);
  if(darktable.gui->surface)
  {
    cairo_set_source_surface(cr, darktable.gui->surface, 0, 0);
    cairo_paint(cr);
  }

  return TRUE;
}

static gboolean _scrolled(GtkWidget *widget, GdkEventScroll *event, gpointer user_data)
{
  int delta_y;
  if(dt_gui_get_scroll_unit_delta(event, &delta_y))
  {
    dt_view_manager_scrolled(darktable.view_manager, event->x, event->y,
                             delta_y < 0,
                             event->state & 0xf);
    gtk_widget_queue_draw(widget);
  }

  return TRUE;
}

static gboolean _borders_scrolled(GtkWidget *widget, GdkEventScroll *event, gpointer user_data)
{
  // pass the scroll event to the matching side panel
  gtk_widget_event(GTK_WIDGET(user_data), (GdkEvent*)event);

  return TRUE;
}

static gboolean _scrollbar_changed(GtkWidget *widget, gpointer user_data)
{
  GtkAdjustment *adjustment_x = gtk_range_get_adjustment(GTK_RANGE(darktable.gui->scrollbars.hscrollbar));
  GtkAdjustment *adjustment_y = gtk_range_get_adjustment(GTK_RANGE(darktable.gui->scrollbars.vscrollbar));

  const gdouble value_x = gtk_adjustment_get_value (adjustment_x);
  const gdouble value_y = gtk_adjustment_get_value (adjustment_y);

  dt_view_manager_scrollbar_changed(darktable.view_manager, value_x, value_y);

  return TRUE;
}

static gboolean _scrollbar_press_event(GtkWidget *widget, gpointer user_data)
{
  darktable.gui->scrollbars.dragging = TRUE;
  return FALSE;
}

static gboolean _scrollbar_release_event(GtkWidget *widget, gpointer user_data)
{
  darktable.gui->scrollbars.dragging = FALSE;
  return FALSE;
}

int dt_gui_gtk_load_config()
{
  dt_pthread_mutex_lock(&darktable.gui->mutex);

  GtkWidget *widget = dt_ui_main_window(darktable.gui->ui);
  const int width = dt_conf_get_int("ui_last/window_w");
  const int height = dt_conf_get_int("ui_last/window_h");
  const gint x = MAX(0, dt_conf_get_int("ui_last/window_x"));
  const gint y = MAX(0, dt_conf_get_int("ui_last/window_y"));

  gtk_window_move(GTK_WINDOW(widget), x, y);
  gtk_window_resize(GTK_WINDOW(widget), width, height);
  const gboolean fullscreen = dt_conf_get_bool("ui_last/fullscreen");

  if(fullscreen)
    gtk_window_fullscreen(GTK_WINDOW(widget));
  else
  {
    gtk_window_unfullscreen(GTK_WINDOW(widget));
    const gboolean maximized = dt_conf_get_bool("ui_last/maximized");
    if(maximized)
      gtk_window_maximize(GTK_WINDOW(widget));
    else
      gtk_window_unmaximize(GTK_WINDOW(widget));
  }

  if(dt_conf_key_exists("ui/show_focus_peaking"))
    darktable.gui->show_focus_peaking = dt_conf_get_bool("ui/show_focus_peaking");
  else
    darktable.gui->show_focus_peaking = FALSE;

  dt_pthread_mutex_unlock(&darktable.gui->mutex);

  return 0;
}

int dt_gui_gtk_write_config()
{
  dt_pthread_mutex_lock(&darktable.gui->mutex);

  GtkWidget *widget = dt_ui_main_window(darktable.gui->ui);
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  gint x, y;
  gtk_window_get_position(GTK_WINDOW(widget), &x, &y);
  dt_conf_set_int("ui_last/window_x", x);
  dt_conf_set_int("ui_last/window_y", y);
  dt_conf_set_int("ui_last/window_w", allocation.width);
  dt_conf_set_int("ui_last/window_h", allocation.height);
  dt_conf_set_bool("ui_last/maximized",
                   (gdk_window_get_state(gtk_widget_get_window(widget)) & GDK_WINDOW_STATE_MAXIMIZED));
  dt_conf_set_bool("ui_last/fullscreen",
                   (gdk_window_get_state(gtk_widget_get_window(widget)) & GDK_WINDOW_STATE_FULLSCREEN));
  dt_conf_set_bool("ui/show_focus_peaking", darktable.gui->show_focus_peaking);

  dt_pthread_mutex_unlock(&darktable.gui->mutex);

  return 0;
}

void dt_gui_gtk_set_source_rgb(cairo_t *cr, dt_gui_color_t color)
{
  const GdkRGBA bc = darktable.gui->colors[color];
  cairo_set_source_rgb(cr, bc.red, bc.green, bc.blue);
}

void dt_gui_gtk_set_source_rgba(cairo_t *cr, dt_gui_color_t color, float opacity_coef)
{
  GdkRGBA bc = darktable.gui->colors[color];
  cairo_set_source_rgba(cr, bc.red, bc.green, bc.blue, bc.alpha * opacity_coef);
}

void dt_gui_gtk_quit()
{
  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  dt_gui_add_class(win, "dt_gui_quit");
  gtk_window_set_title(GTK_WINDOW(win), _("closing darktable..."));

  // Write out windows dimension
  dt_gui_gtk_write_config();

  GtkWidget *widget;
  widget = darktable.gui->widgets.left_border;
  g_signal_handlers_block_by_func(widget, _draw_borders, GINT_TO_POINTER(DT_UI_BORDER_LEFT));
  widget = darktable.gui->widgets.right_border;
  g_signal_handlers_block_by_func(widget, _draw_borders, GINT_TO_POINTER(DT_UI_BORDER_RIGHT));
  widget = darktable.gui->widgets.top_border;
  g_signal_handlers_block_by_func(widget, _draw_borders, GINT_TO_POINTER(DT_UI_BORDER_TOP));
  widget = darktable.gui->widgets.bottom_border;
  g_signal_handlers_block_by_func(widget, _draw_borders, GINT_TO_POINTER(DT_UI_BORDER_BOTTOM));

  // hide main window
  gtk_widget_hide(dt_ui_main_window(darktable.gui->ui));
}

gboolean dt_gui_quit_callback(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
  dt_control_quit();
  return TRUE;
}

void dt_gui_store_last_preset(const char *name)
{
  g_free(darktable.gui->last_preset);
  darktable.gui->last_preset = g_strdup(name);
}

static void _gui_noop_action_callback(dt_action_t *action)
{
}

static void _gui_switch_view_key_accel_callback(dt_action_t *action)
{
  dt_ctl_switch_mode_to(action->id);
}

static void _quit_callback(dt_action_t *action)
{
  dt_control_quit();
}

#ifdef MAC_INTEGRATION
#ifdef GTK_TYPE_OSX_APPLICATION
static gboolean _osx_quit_callback(GtkOSXApplication *OSXapp, gpointer user_data)
#else
static gboolean _osx_quit_callback(GtkosxApplication *OSXapp, gpointer user_data)
#endif
{
  GList *windows, *window;
  windows = gtk_window_list_toplevels();
  for(window = windows; window != NULL; window = g_list_next(window))
    if(gtk_window_get_modal(GTK_WINDOW(window->data)) && gtk_widget_get_visible(GTK_WIDGET(window->data)))
      break;
  if(window == NULL) dt_control_quit();
  g_list_free(windows);
  return TRUE;
}

#ifdef GTK_TYPE_OSX_APPLICATION
static gboolean _osx_openfile_callback(GtkOSXApplication *OSXapp, gchar *path, gpointer user_data)
#else
static gboolean _osx_openfile_callback(GtkosxApplication *OSXapp, gchar *path, gpointer user_data)
#endif
{
  return dt_load_from_string(path, TRUE, NULL) > 0;
}
#endif

static gboolean _configure(GtkWidget *da, GdkEventConfigure *event, gpointer user_data)
{
  static int oldw = 0;
  static int oldh = 0;
  // make our selves a properly sized pixmap if our window has been resized
  if(oldw != event->width || oldh != event->height)
  {
    // create our new pixmap with the correct size.
    cairo_surface_t *tmpsurface
        = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, event->width, event->height);
    // copy the contents of the old pixmap to the new pixmap.  This keeps ugly uninitialized
    // pixmaps from being painted upon resize
    //     int minw = oldw, minh = oldh;
    //     if(event->width  < minw) minw = event->width;
    //     if(event->height < minh) minh = event->height;

    cairo_t *cr = cairo_create(tmpsurface);
    cairo_set_source_surface(cr, darktable.gui->surface, 0, 0);
    cairo_paint(cr);
    cairo_destroy(cr);

    // we're done with our old pixmap, so we can get rid of it and replace it with our properly-sized one.
    cairo_surface_destroy(darktable.gui->surface);
    darktable.gui->surface = tmpsurface;
    dt_colorspaces_set_display_profile(
        DT_COLORSPACE_DISPLAY); // maybe we are on another screen now with > 50% of the area
  }
  oldw = event->width;
  oldh = event->height;

#ifndef GDK_WINDOWING_QUARTZ
  dt_configure_ppd_dpi((dt_gui_gtk_t *) user_data);
#endif

  return dt_control_configure(da, event, user_data);
}

static gboolean _window_configure(GtkWidget *da, GdkEvent *event, gpointer user_data)
{
  static int oldx = 0;
  static int oldy = 0;
  if(oldx != event->configure.x || oldy != event->configure.y)
  {
    dt_colorspaces_set_display_profile(
        DT_COLORSPACE_DISPLAY); // maybe we are on another screen now with > 50% of the area
    oldx = event->configure.x;
    oldy = event->configure.y;
  }
  return FALSE;
}

guint dt_gui_translated_key_state(GdkEventKey *event)
{
  if(gdk_keyval_to_lower(event->keyval) == gdk_keyval_to_upper(event->keyval) )
  {
    //not an alphabetic character
    //find any modifiers consumed to produce keyval
    guint consumed;
    gdk_keymap_translate_keyboard_state(gdk_keymap_get_for_display(gdk_display_get_default()), event->hardware_keycode, event->state, event->group, NULL, NULL, NULL, &consumed);
    return event->state & ~consumed & gtk_accelerator_get_default_mod_mask();
  }
  else
    return event->state & gtk_accelerator_get_default_mod_mask();
}

static gboolean _button_pressed(GtkWidget *w, GdkEventButton *event, gpointer user_data)
{
  double pressure = 1.0;
  GdkDevice *device = gdk_event_get_source_device((GdkEvent *)event);

  if(device && gdk_device_get_source(device) == GDK_SOURCE_PEN)
  {
    gdk_event_get_axis ((GdkEvent *)event, GDK_AXIS_PRESSURE, &pressure);
  }
  dt_control_button_pressed(event->x, event->y, pressure, event->button, event->type, event->state & 0xf);
  gtk_widget_grab_focus(w);
  gtk_widget_queue_draw(w);
  return FALSE;
}

static gboolean _button_released(GtkWidget *w, GdkEventButton *event, gpointer user_data)
{
  dt_control_button_released(event->x, event->y, event->button, event->state & 0xf);
  gtk_widget_queue_draw(w);
  return TRUE;
}

static gboolean _mouse_moved(GtkWidget *w, GdkEventMotion *event, gpointer user_data)
{
  double pressure = 1.0;
  GdkDevice *device = gdk_event_get_source_device((GdkEvent *)event);

  if(device && gdk_device_get_source(device) == GDK_SOURCE_PEN)
  {
    gdk_event_get_axis ((GdkEvent *)event, GDK_AXIS_PRESSURE, &pressure);
  }
  dt_control_mouse_moved(event->x, event->y, pressure, event->state & 0xf);
  return FALSE;
}

static gboolean _center_leave(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  dt_control_mouse_leave();
  return TRUE;
}

static gboolean _center_enter(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  dt_control_mouse_enter();
  return TRUE;
}

static const char* _get_source_name(int pos)
{
  static const gchar *SOURCE_NAMES[]
    = { "GDK_SOURCE_MOUSE",    "GDK_SOURCE_PEN",         "GDK_SOURCE_ERASER",   "GDK_SOURCE_CURSOR",
        "GDK_SOURCE_KEYBOARD", "GDK_SOURCE_TOUCHSCREEN", "GDK_SOURCE_TOUCHPAD", "GDK_SOURCE_TRACKPOINT",
        "GDK_SOURCE_TABLET_PAD" };
  if(pos >= G_N_ELEMENTS(SOURCE_NAMES)) return "<UNKNOWN>";
  return SOURCE_NAMES[pos];
}

static const char* _get_mode_name(int pos)
{
  static const gchar *MODE_NAMES[] = { "GDK_MODE_DISABLED", "GDK_MODE_SCREEN", "GDK_MODE_WINDOW" };
  if(pos >= G_N_ELEMENTS(MODE_NAMES)) return "<UNKNOWN>";
  return MODE_NAMES[pos];
}

static const char* _get_axis_name(int pos)
{
  static const gchar *AXIS_NAMES[]
    = { "GDK_AXIS_IGNORE",   "GDK_AXIS_X",      "GDK_AXIS_Y",     "GDK_AXIS_PRESSURE",
        "GDK_AXIS_XTILT",    "GDK_AXIS_YTILT",  "GDK_AXIS_WHEEL", "GDK_AXIS_DISTANCE",
        "GDK_AXIS_ROTATION", "GDK_AXIS_SLIDER", "GDK_AXIS_LAST" };
  if(pos >= G_N_ELEMENTS(AXIS_NAMES)) return "<UNKNOWN>";
  return AXIS_NAMES[pos];
}

int dt_gui_gtk_init(dt_gui_gtk_t *gui)
{
  /* lets zero mem */
  memset(gui, 0, sizeof(dt_gui_gtk_t));

  dt_pthread_mutex_init(&gui->mutex, NULL);

  // force gtk3 to use normal scroll bars instead of the popup thing. they get in the way of controls
  // the alternative would be to gtk_scrolled_window_set_overlay_scrolling(..., FALSE); every single widget
  // that might have scroll bars
  g_setenv("GTK_OVERLAY_SCROLLING", "0", 0);

  // same for ubuntus overlay-scrollbar-gtk3
  g_setenv("LIBOVERLAY_SCROLLBAR", "0", 0);

  // unset gtk rc from kde:
  char path[PATH_MAX] = { 0 }, datadir[PATH_MAX] = { 0 }, sharedir[PATH_MAX] = { 0 }, configdir[PATH_MAX] = { 0 };
  dt_loc_get_datadir(datadir, sizeof(datadir));
  dt_loc_get_sharedir(sharedir, sizeof(sharedir));
  dt_loc_get_user_config_dir(configdir, sizeof(configdir));

  const char *css_theme = dt_conf_get_string_const("ui_last/theme");
  if(css_theme)
  {
    g_strlcpy(gui->gtkrc, css_theme, sizeof(gui->gtkrc));
  }
  else
    g_snprintf(gui->gtkrc, sizeof(gui->gtkrc), "darktable");

#ifdef MAC_INTEGRATION
#ifdef GTK_TYPE_OSX_APPLICATION
  GtkOSXApplication *OSXApp = g_object_new(GTK_TYPE_OSX_APPLICATION, NULL);
  gtk_osxapplication_set_menu_bar(
      OSXApp, GTK_MENU_SHELL(gtk_menu_bar_new())); // needed for default entries to show up
#else
  GtkosxApplication *OSXApp = g_object_new(GTKOSX_TYPE_APPLICATION, NULL);
  gtkosx_application_set_menu_bar(
      OSXApp, GTK_MENU_SHELL(gtk_menu_bar_new())); // needed for default entries to show up
#endif
  g_signal_connect(G_OBJECT(OSXApp), "NSApplicationBlockTermination", G_CALLBACK(_osx_quit_callback), NULL);
  g_signal_connect(G_OBJECT(OSXApp), "NSApplicationOpenFile", G_CALLBACK(_osx_openfile_callback), NULL);
#endif

  GtkWidget *widget;
  gui->ui = g_malloc0(sizeof(dt_ui_t));
  gui->surface = NULL;
  gui->center_tooltip = 0;
  gui->grouping = dt_conf_get_bool("ui_last/grouping");
  gui->expanded_group_id = -1;
  gui->show_overlays = dt_conf_get_bool("lighttable/ui/expose_statuses");
  gui->presets_popup_menu = NULL;
  gui->last_preset = NULL;

  // load the style / theme
  GtkSettings *settings = gtk_settings_get_default();
  g_object_set(G_OBJECT(settings), "gtk-application-prefer-dark-theme", TRUE, (gchar *)0);
  g_object_set(G_OBJECT(settings), "gtk-theme-name", "Adwaita", (gchar *)0);
  g_object_unref(settings);

  // smooth scrolling must be enabled to handle trackpad/touch events
  gui->scroll_mask = GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK;

  // key accelerator that enables scrolling of side panels
  gui->sidebar_scroll_mask = GDK_MOD1_MASK | GDK_CONTROL_MASK;

  // Init focus peaking
  gui->show_focus_peaking = dt_conf_get_bool("ui/show_focus_peaking");

  // Initializing widgets
  _init_widgets(gui);

  //init overlay colors
  dt_guides_set_overlay_colors();

  /* Have the delete event (window close) end the program */
  snprintf(path, sizeof(path), "%s/icons", datadir);
  gtk_icon_theme_append_search_path(gtk_icon_theme_get_default(), path);
  snprintf(path, sizeof(path), "%s/icons", sharedir);
  gtk_icon_theme_append_search_path(gtk_icon_theme_get_default(), path);

  widget = dt_ui_center(darktable.gui->ui);

  g_signal_connect(G_OBJECT(widget), "configure-event", G_CALLBACK(_configure), gui);
  g_signal_connect(G_OBJECT(widget), "draw", G_CALLBACK(_draw), NULL);
  g_signal_connect(G_OBJECT(widget), "motion-notify-event", G_CALLBACK(_mouse_moved), NULL);
  g_signal_connect(G_OBJECT(widget), "leave-notify-event", G_CALLBACK(_center_leave), NULL);
  g_signal_connect(G_OBJECT(widget), "enter-notify-event", G_CALLBACK(_center_enter), NULL);
  g_signal_connect(G_OBJECT(widget), "button-press-event", G_CALLBACK(_button_pressed), NULL);
  g_signal_connect(G_OBJECT(widget), "button-release-event", G_CALLBACK(_button_released), NULL);
  g_signal_connect(G_OBJECT(widget), "scroll-event", G_CALLBACK(_scrolled), NULL);

  // TODO: left, right, top, bottom:
  // leave-notify-event

  widget = darktable.gui->scrollbars.vscrollbar;
  g_signal_connect(G_OBJECT(widget), "value-changed", G_CALLBACK(_scrollbar_changed), NULL);
  g_signal_connect(G_OBJECT(widget), "button-press-event", G_CALLBACK(_scrollbar_press_event), NULL);
  g_signal_connect(G_OBJECT(widget), "button-release-event", G_CALLBACK(_scrollbar_release_event), NULL);

  widget = darktable.gui->scrollbars.hscrollbar;
  g_signal_connect(G_OBJECT(widget), "value-changed", G_CALLBACK(_scrollbar_changed), NULL);
  g_signal_connect(G_OBJECT(widget), "button-press-event", G_CALLBACK(_scrollbar_press_event), NULL);
  g_signal_connect(G_OBJECT(widget), "button-release-event", G_CALLBACK(_scrollbar_release_event), NULL);

  dt_action_t *pnl = dt_action_section(&darktable.control->actions_global, N_("panels"));
  dt_action_t *ac;
  widget = darktable.gui->widgets.left_border;
  g_signal_connect(G_OBJECT(widget), "draw", G_CALLBACK(_draw_borders), GINT_TO_POINTER(DT_UI_BORDER_LEFT));
  g_signal_connect(G_OBJECT(widget), "button-press-event", G_CALLBACK(_borders_button_pressed),
                   GINT_TO_POINTER(DT_UI_BORDER_LEFT));
  ac = dt_action_define(pnl, NULL, N_("left"), widget, NULL);
  dt_action_register(ac, NULL, _toggle_panel_accel_callback, GDK_KEY_L, GDK_CONTROL_MASK | GDK_SHIFT_MASK);

  widget = darktable.gui->widgets.right_border;
  g_signal_connect(G_OBJECT(widget), "draw", G_CALLBACK(_draw_borders), GINT_TO_POINTER(DT_UI_BORDER_RIGHT));
  g_signal_connect(G_OBJECT(widget), "button-press-event", G_CALLBACK(_borders_button_pressed),
                   GINT_TO_POINTER(DT_UI_BORDER_RIGHT));
  ac = dt_action_define(pnl, NULL, N_("right"), widget, NULL);
  dt_action_register(ac, NULL, _toggle_panel_accel_callback, GDK_KEY_R, GDK_CONTROL_MASK | GDK_SHIFT_MASK);

  widget = darktable.gui->widgets.top_border;
  g_signal_connect(G_OBJECT(widget), "draw", G_CALLBACK(_draw_borders), GINT_TO_POINTER(DT_UI_BORDER_TOP));
  g_signal_connect(G_OBJECT(widget), "button-press-event", G_CALLBACK(_borders_button_pressed),
                   GINT_TO_POINTER(DT_UI_BORDER_TOP));
  ac = dt_action_define(pnl, NULL, N_("top"), widget, NULL);
  dt_action_register(ac, NULL, _toggle_panel_accel_callback, GDK_KEY_T, GDK_CONTROL_MASK | GDK_SHIFT_MASK);

  widget = darktable.gui->widgets.bottom_border;
  g_signal_connect(G_OBJECT(widget), "draw", G_CALLBACK(_draw_borders), GINT_TO_POINTER(DT_UI_BORDER_BOTTOM));
  g_signal_connect(G_OBJECT(widget), "button-press-event", G_CALLBACK(_borders_button_pressed),
                   GINT_TO_POINTER(DT_UI_BORDER_BOTTOM));
  ac = dt_action_define(pnl, NULL, N_("bottom"), widget, NULL);
  dt_action_register(ac, NULL, _toggle_panel_accel_callback, GDK_KEY_B, GDK_CONTROL_MASK | GDK_SHIFT_MASK);

  dt_gui_presets_init();

  widget = dt_ui_center(darktable.gui->ui);
  gtk_widget_set_app_paintable(widget, TRUE);

  // TODO: make this work as: libgnomeui testgnome.c
  /*  GtkContainer *box = GTK_CONTAINER(darktable.gui->widgets.plugins_vbox);
  GtkScrolledWindow *swin = GTK_SCROLLED_WINDOW(darktable.gui->
                                                widgets.right_scrolled_window);
  gtk_container_set_focus_vadjustment (box, gtk_scrolled_window_get_vadjustment (swin));
  */
  dt_colorspaces_set_display_profile(DT_COLORSPACE_DISPLAY);
  // update the profile when the window is moved. resize is already handled in configure()
  widget = dt_ui_main_window(darktable.gui->ui);
  g_signal_connect(G_OBJECT(widget), "configure-event", G_CALLBACK(_window_configure), NULL);
  g_signal_connect(G_OBJECT(widget), "event", G_CALLBACK(dt_shortcut_dispatcher), NULL);
  g_signal_override_class_handler("query-tooltip", gtk_widget_get_type(), G_CALLBACK(dt_shortcut_tooltip_callback));

  //an action that does nothing - used for overriding/removing default shortcuts
  dt_action_register(&darktable.control->actions_global, N_("no-op"), _gui_noop_action_callback, 0, 0);

  ac = dt_action_section(&darktable.control->actions_global, N_("switch views"));
  dt_action_register(ac, N_("tethering"), _gui_switch_view_key_accel_callback, GDK_KEY_t, 0);
  dt_action_register(ac, N_("lighttable"), _gui_switch_view_key_accel_callback, GDK_KEY_l, 0);
  dt_action_register(ac, N_("darkroom"), _gui_switch_view_key_accel_callback, GDK_KEY_d, 0);
  dt_action_register(ac, N_("map"), _gui_switch_view_key_accel_callback, GDK_KEY_m, 0);
  dt_action_register(ac, N_("slideshow"), _gui_switch_view_key_accel_callback, GDK_KEY_s, 0);
  dt_action_register(ac, N_("print"), _gui_switch_view_key_accel_callback, GDK_KEY_p, 0);

  // register actions for applying styles via shortcuts
  dt_init_styles_actions();

  // register ctrl-q to quit:
  dt_action_register(&darktable.control->actions_global, N_("quit"), _quit_callback, GDK_KEY_q, GDK_CONTROL_MASK);

  // Full-screen accelerator (no ESC handler here to enable quit-slideshow using ESC)
  dt_action_register(&darktable.control->actions_global, N_("fullscreen"), _fullscreen_key_accel_callback, GDK_KEY_F11, 0);

  // Side-border hide/show
  dt_action_register(pnl, N_("all"), _toggle_side_borders_accel_callback, GDK_KEY_Tab, 0);
  dt_action_register(pnl, N_("collapsing controls"), _panels_controls_accel_callback, GDK_KEY_b, 0);
  // specific top/bottom toggles
  dt_action_register(pnl, N_("header"), _toggle_header_accel_callback, GDK_KEY_h, GDK_CONTROL_MASK);
  dt_action_register(pnl, N_("filmstrip and timeline"), _toggle_filmstrip_accel_callback, GDK_KEY_f, GDK_CONTROL_MASK);
  dt_action_register(pnl, N_("top toolbar"), _toggle_top_tool_accel_callback, 0, 0);
  dt_action_register(pnl, N_("bottom toolbar"), _toggle_bottom_tool_accel_callback, 0, 0);
  dt_action_register(pnl, N_("all top"), _toggle_top_all_accel_callback, 0, 0);
  dt_action_register(pnl, N_("all bottom"), _toggle_bottom_all_accel_callback, 0, 0);

  dt_action_register(&darktable.control->actions_global, N_("toggle tooltip visibility"), _toggle_tooltip_visibility, GDK_KEY_T, GDK_SHIFT_MASK);
  dt_action_register(&darktable.control->actions_global, N_("reinitialise input devices"), dt_shortcuts_reinitialise, GDK_KEY_I, GDK_CONTROL_MASK | GDK_SHIFT_MASK | GDK_MOD1_MASK);

  darktable.gui->reset = 0;

  // load theme
  dt_gui_load_theme(gui->gtkrc);

  // let's try to support pressure sensitive input devices like tablets for mask drawing
  dt_print(DT_DEBUG_INPUT, "[input device] Input devices found:\n\n");

  GList *input_devices
      = gdk_seat_get_slaves(gdk_display_get_default_seat(gdk_display_get_default()), GDK_SEAT_CAPABILITY_ALL);
  for(GList *l = input_devices; l != NULL; l = g_list_next(l))
  {
    GdkDevice *device = (GdkDevice *)l->data;
    const GdkInputSource source = gdk_device_get_source(device);
    const gint n_axes = (source == GDK_SOURCE_KEYBOARD ? 0 : gdk_device_get_n_axes(device));

    dt_print(DT_DEBUG_INPUT, "%s (%s), source: %s, mode: %s, %d axes, %d keys\n", gdk_device_get_name(device),
             (source != GDK_SOURCE_KEYBOARD) && gdk_device_get_has_cursor(device) ? "with cursor" : "no cursor",
             _get_source_name(source),
             _get_mode_name(gdk_device_get_mode(device)), n_axes,
             source != GDK_SOURCE_KEYBOARD ? gdk_device_get_n_keys(device) : 0);

    for(int i = 0; i < n_axes; i++)
    {
      dt_print(DT_DEBUG_INPUT, "  %s\n", _get_axis_name(gdk_device_get_axis_use(device, i)));
    }
    dt_print(DT_DEBUG_INPUT, "\n");
  }
  g_list_free(input_devices);

  // finally set the cursor to be the default.
  // for some reason this is needed on some systems to pick up the correctly themed cursor
  dt_control_change_cursor(GDK_LEFT_PTR);

  // create focus-peaking button
  darktable.gui->focus_peaking_button = dtgtk_togglebutton_new(dtgtk_cairo_paint_focus_peaking, 0, NULL);
  gtk_widget_set_tooltip_text(darktable.gui->focus_peaking_button, _("toggle focus-peaking mode"));
  g_signal_connect(G_OBJECT(darktable.gui->focus_peaking_button), "clicked", G_CALLBACK(_focuspeaking_switch_button_callback), NULL);
  _update_focus_peaking_button();

  // toggle focus peaking everywhere
  ac = dt_action_define(&darktable.control->actions_global, NULL, N_("toggle focus peaking"),
                        darktable.gui->focus_peaking_button, &dt_action_def_toggle);
  dt_shortcut_register(ac, 0, 0, GDK_KEY_f, GDK_CONTROL_MASK | GDK_SHIFT_MASK);

  return 0;
}

void dt_gui_gtk_run(dt_gui_gtk_t *gui)
{
  GtkWidget *widget = dt_ui_center(darktable.gui->ui);
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  darktable.gui->surface
      = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, allocation.width, allocation.height);
  // need to pre-configure views to avoid crash caused by draw coming before configure-event
  darktable.control->tabborder = 8;
  const int tb = darktable.control->tabborder;
  dt_view_manager_configure(darktable.view_manager, allocation.width - 2 * tb, allocation.height - 2 * tb);
#ifdef MAC_INTEGRATION
#ifdef GTK_TYPE_OSX_APPLICATION
  gtk_osxapplication_ready(g_object_new(GTK_TYPE_OSX_APPLICATION, NULL));
#else
  gtkosx_application_ready(g_object_new(GTKOSX_TYPE_APPLICATION, NULL));
#endif
#endif
#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_focus_window();
#endif
  /* start the event loop */
  gtk_main();

  if(darktable.gui->surface)
  {
    cairo_surface_destroy(darktable.gui->surface);
    darktable.gui->surface = NULL;
  }
  dt_cleanup();
}

// refactored function to read current ppd, because gtk for osx has been unreliable
// we use the specific function here. Anyway, if nothing meaningful is found we default back to 1.0
double dt_get_system_gui_ppd(GtkWidget *widget)
{
  double res = 0.0f;
#ifdef GDK_WINDOWING_QUARTZ
  res = dt_osx_get_ppd();
#else
  res = gtk_widget_get_scale_factor(widget);
#endif
  if((res < 1.0f) || (res > 4.0f))
  {
    dt_print(DT_DEBUG_CONTROL, "[dt_get_system_gui_ppd] can't detect system ppd\n");
    return 1.0f;
  }
  dt_print(DT_DEBUG_CONTROL, "[dt_get_system_gui_ppd] system ppd is %f\n", res);
  return res;
}

void dt_configure_ppd_dpi(dt_gui_gtk_t *gui)
{
  GtkWidget *widget = gui->ui->main_window;

  gui->ppd = gui->ppd_thb = dt_get_system_gui_ppd(widget);
  gui->filter_image = CAIRO_FILTER_GOOD;
  gui->dr_filter_image = CAIRO_FILTER_BEST;
  if(dt_conf_get_bool("ui/performance"))
  {
      gui->ppd_thb *= DT_GUI_THUMBSIZE_REDUCE;
      gui->filter_image = CAIRO_FILTER_FAST;
      gui->dr_filter_image = CAIRO_FILTER_GOOD;
  }
  // get the screen resolution
  const float screen_dpi_overwrite = dt_conf_get_float("screen_dpi_overwrite");
  if(screen_dpi_overwrite > 0.0)
  {
    gui->dpi = screen_dpi_overwrite;
    gdk_screen_set_resolution(gtk_widget_get_screen(widget), screen_dpi_overwrite);
    dt_print(DT_DEBUG_CONTROL, "[screen resolution] setting the screen resolution to %f dpi as specified in "
                               "the configuration file\n",
             screen_dpi_overwrite);
  }
  else
  {
#ifdef GDK_WINDOWING_QUARTZ
    dt_osx_autoset_dpi(widget);
#endif
    gui->dpi = gdk_screen_get_resolution(gtk_widget_get_screen(widget));
    if(gui->dpi < 0.0)
    {
      gui->dpi = 96.0;
      gdk_screen_set_resolution(gtk_widget_get_screen(widget), 96.0);
      dt_print(DT_DEBUG_CONTROL, "[screen resolution] setting the screen resolution to the default 96 dpi\n");
    }
    else
      dt_print(DT_DEBUG_CONTROL, "[screen resolution] setting the screen resolution to %f dpi\n", gui->dpi);
  }
  gui->dpi_factor
      = gui->dpi / 96; // according to man xrandr and the docs of gdk_screen_set_resolution 96 is the default
}

static gboolean _focus_in_out_event(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
  gtk_window_set_urgency_hint(GTK_WINDOW(user_data), FALSE);
  return FALSE;
}


static gboolean _ui_log_button_press_event(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
  gtk_widget_hide(GTK_WIDGET(user_data));
  return TRUE;
}

static gboolean _ui_toast_button_press_event(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
  gtk_widget_hide(GTK_WIDGET(user_data));
  return TRUE;
}

static void _init_widgets(dt_gui_gtk_t *gui)
{

  GtkWidget *container;
  GtkWidget *widget;

  // Creating the main window
  widget = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_widget_set_name(widget, "main_window");
  gui->ui->main_window = widget;

  dt_configure_ppd_dpi(gui);

  gtk_window_set_default_size(GTK_WINDOW(widget), DT_PIXEL_APPLY_DPI(900), DT_PIXEL_APPLY_DPI(500));

  gtk_window_set_icon_name(GTK_WINDOW(widget), "darktable");
  gtk_window_set_title(GTK_WINDOW(widget), "darktable");

  g_signal_connect(G_OBJECT(widget), "delete_event", G_CALLBACK(dt_gui_quit_callback), NULL);
  g_signal_connect(G_OBJECT(widget), "focus-in-event", G_CALLBACK(_focus_in_out_event), widget);
  g_signal_connect(G_OBJECT(widget), "focus-out-event", G_CALLBACK(_focus_in_out_event), widget);

  container = widget;

  // Adding the outermost vbox
  widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_container_add(GTK_CONTAINER(container), widget);
  gtk_widget_show(widget);

  /* connect to signal redraw all */
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_CONTROL_REDRAW_ALL,
                            G_CALLBACK(_ui_widget_redraw_callback), gui->ui->main_window);

  container = widget;

  // Initializing the top border
  widget = gtk_drawing_area_new();
  gui->widgets.top_border = widget;
  gtk_box_pack_start(GTK_BOX(container), widget, FALSE, TRUE, 0);
  gtk_widget_set_size_request(widget, -1, DT_PIXEL_APPLY_DPI(10));
  gtk_widget_set_app_paintable(widget, TRUE);
  gtk_widget_set_events(widget, GDK_EXPOSURE_MASK | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK
                                | GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK | GDK_STRUCTURE_MASK
                                | darktable.gui->scroll_mask);
  gtk_widget_set_name(GTK_WIDGET(widget), "outer-border");
  gtk_widget_show(widget);

  // Initializing the main table
  _init_main_table(container);

  // Initializing the bottom border
  widget = gtk_drawing_area_new();
  gui->widgets.bottom_border = widget;
  gtk_box_pack_start(GTK_BOX(container), widget, FALSE, TRUE, 0);
  gtk_widget_set_size_request(widget, -1, DT_PIXEL_APPLY_DPI(10));
  gtk_widget_set_app_paintable(widget, TRUE);
  gtk_widget_set_events(widget, GDK_EXPOSURE_MASK | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK
                                | GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK | GDK_STRUCTURE_MASK
                                | darktable.gui->scroll_mask);
  gtk_widget_set_name(GTK_WIDGET(widget), "outer-border");
  gtk_widget_show(widget);

  // Showing everything
  gtk_widget_show_all(dt_ui_main_window(gui->ui));

  gtk_widget_set_visible(dt_ui_log_msg(gui->ui), FALSE);
  gtk_widget_set_visible(dt_ui_toast_msg(gui->ui), FALSE);
  gtk_widget_set_visible(gui->scrollbars.hscrollbar, FALSE);
  gtk_widget_set_visible(gui->scrollbars.vscrollbar, FALSE);

}

static void _init_main_table(GtkWidget *container)
{
  GtkWidget *widget;

  // Creating the table
  widget = gtk_grid_new();
  gtk_box_pack_start(GTK_BOX(container), widget, TRUE, TRUE, 0);
  gtk_widget_show(widget);

  container = widget;

  // Adding the left border
  widget = gtk_drawing_area_new();
  darktable.gui->widgets.left_border = widget;
  gtk_widget_set_size_request(widget, DT_PIXEL_APPLY_DPI(10), -1);
  gtk_widget_set_app_paintable(widget, TRUE);
  gtk_widget_set_events(widget, GDK_EXPOSURE_MASK | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK
                                | GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK | GDK_STRUCTURE_MASK
                                | darktable.gui->scroll_mask);
  gtk_grid_attach(GTK_GRID(container), widget, 0, 0, 1, 2);
  gtk_widget_set_name(GTK_WIDGET(widget), "outer-border");
  gtk_widget_show(widget);

  // Adding the right border
  widget = gtk_drawing_area_new();
  darktable.gui->widgets.right_border = widget;
  gtk_widget_set_size_request(widget, DT_PIXEL_APPLY_DPI(10), -1);
  gtk_widget_set_app_paintable(widget, TRUE);
  gtk_widget_set_events(widget, GDK_EXPOSURE_MASK | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK
                                | GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK | GDK_STRUCTURE_MASK
                                | darktable.gui->scroll_mask);
  gtk_grid_attach(GTK_GRID(container), widget, 4, 0, 1, 2);
  gtk_widget_set_name(GTK_WIDGET(widget), "outer-border");
  gtk_widget_show(widget);

  /* initialize the top container */
  _ui_init_panel_top(darktable.gui->ui, container);

  /*
   * initialize the center top/center/bottom
   */
  widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_hexpand(GTK_WIDGET(widget), TRUE);
  gtk_widget_set_vexpand(GTK_WIDGET(widget), TRUE);
  gtk_grid_attach(GTK_GRID(container), widget, 2, 1, 1, 1);

  /* initialize the center top panel */
  _ui_init_panel_center_top(darktable.gui->ui, widget);

  GtkWidget *centergrid = gtk_grid_new();
  gtk_box_pack_start(GTK_BOX(widget), centergrid, TRUE, TRUE, 0);

  /* setup center drawing area */
  GtkWidget *ocda = gtk_overlay_new();
  GtkWidget *cda = gtk_drawing_area_new();
  gtk_widget_set_size_request(cda, DT_PIXEL_APPLY_DPI(50), DT_PIXEL_APPLY_DPI(200));
  gtk_widget_set_hexpand(ocda, TRUE);
  gtk_widget_set_vexpand(ocda, TRUE);
  gtk_widget_set_app_paintable(cda, TRUE);
  gtk_widget_set_events(cda, GDK_POINTER_MOTION_MASK | GDK_BUTTON_PRESS_MASK
                             | GDK_BUTTON_RELEASE_MASK | GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK
                             | darktable.gui->scroll_mask);
  gtk_widget_set_can_focus(cda, TRUE);
  gtk_widget_set_visible(cda, TRUE);
  gtk_overlay_add_overlay(GTK_OVERLAY(ocda), cda);

  gtk_grid_attach(GTK_GRID(centergrid), ocda, 0, 0, 1, 1);
  darktable.gui->ui->center = cda;
  darktable.gui->ui->center_base = ocda;

  /* initialize the thumb panel */
  darktable.gui->ui->thumbtable = dt_thumbtable_new();

  /* the log message */
  GtkWidget *eb = gtk_event_box_new();
  darktable.gui->ui->log_msg = gtk_label_new("");
  g_signal_connect(G_OBJECT(eb), "button-press-event", G_CALLBACK(_ui_log_button_press_event),
                   darktable.gui->ui->log_msg);
  gtk_label_set_ellipsize(GTK_LABEL(darktable.gui->ui->log_msg), PANGO_ELLIPSIZE_MIDDLE);
  dt_gui_add_class(darktable.gui->ui->log_msg, "dt_messages");
  gtk_container_add(GTK_CONTAINER(eb), darktable.gui->ui->log_msg);
  gtk_widget_set_valign(eb, GTK_ALIGN_END);
  gtk_widget_set_halign(eb, GTK_ALIGN_CENTER);
  gtk_overlay_add_overlay(GTK_OVERLAY(ocda), eb);

  /* the toast message */
  eb = gtk_event_box_new();
  darktable.gui->ui->toast_msg = gtk_label_new("");
  g_signal_connect(G_OBJECT(eb), "button-press-event", G_CALLBACK(_ui_toast_button_press_event),
                   darktable.gui->ui->toast_msg);
  gtk_widget_set_events(eb, GDK_BUTTON_PRESS_MASK | darktable.gui->scroll_mask);
  g_signal_connect(G_OBJECT(eb), "scroll-event", G_CALLBACK(_scrolled), NULL);
  gtk_label_set_ellipsize(GTK_LABEL(darktable.gui->ui->toast_msg), PANGO_ELLIPSIZE_MIDDLE);

  PangoAttrList *attrlist = pango_attr_list_new();
  PangoAttribute *attr = pango_attr_font_features_new("tnum");
  pango_attr_list_insert(attrlist, attr);
  gtk_label_set_attributes(GTK_LABEL(darktable.gui->ui->toast_msg), attrlist);
  pango_attr_list_unref(attrlist);

  dt_gui_add_class(darktable.gui->ui->toast_msg, "dt_messages");
  gtk_container_add(GTK_CONTAINER(eb), darktable.gui->ui->toast_msg);
  gtk_widget_set_valign(eb, GTK_ALIGN_START);
  gtk_widget_set_halign(eb, GTK_ALIGN_CENTER);
  gtk_overlay_add_overlay(GTK_OVERLAY(ocda), eb);

  /* center should redraw when signal redraw center is raised*/
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_CONTROL_REDRAW_CENTER,
                            G_CALLBACK(_ui_widget_redraw_callback), darktable.gui->ui->center);

  /* update log message label */
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_CONTROL_LOG_REDRAW, G_CALLBACK(_ui_log_redraw_callback),
                            darktable.gui->ui->log_msg);

  /* update toast message label */
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_CONTROL_TOAST_REDRAW, G_CALLBACK(_ui_toast_redraw_callback),
                            darktable.gui->ui->toast_msg);

  // Adding the scrollbars
  GtkWidget *vscrollBar = gtk_scrollbar_new(GTK_ORIENTATION_VERTICAL, NULL);
  GtkWidget *hscrollBar = gtk_scrollbar_new(GTK_ORIENTATION_HORIZONTAL, NULL);

  gtk_grid_attach_next_to(GTK_GRID(centergrid), vscrollBar, ocda, GTK_POS_RIGHT, 1, 1);
  gtk_grid_attach_next_to(GTK_GRID(centergrid), hscrollBar, ocda, GTK_POS_BOTTOM, 1, 1);

  darktable.gui->scrollbars.vscrollbar = vscrollBar;
  darktable.gui->scrollbars.hscrollbar = hscrollBar;

  /* initialize the center bottom panel */
  _ui_init_panel_center_bottom(darktable.gui->ui, widget);

  /* initialize the bottom panel */
  _ui_init_panel_bottom(darktable.gui->ui, container);

  /* initialize  left panel */
  _ui_init_panel_left(darktable.gui->ui, container);

  /* initialize right panel */
  _ui_init_panel_right(darktable.gui->ui, container);
}

GtkBox *dt_ui_get_container(struct dt_ui_t *ui, const dt_ui_container_t c)
{
  return GTK_BOX(ui->containers[c]);
}
void dt_ui_container_add_widget(dt_ui_t *ui, const dt_ui_container_t c, GtkWidget *w)
{
  //  if(!GTK_IS_BOX(ui->containers[c])) return;
  g_return_if_fail(GTK_IS_BOX(ui->containers[c]));
  switch(c)
  {
    /* if box is right lets pack at end for nicer alignment */
    case DT_UI_CONTAINER_PANEL_TOP_RIGHT:
    case DT_UI_CONTAINER_PANEL_CENTER_TOP_RIGHT:
    case DT_UI_CONTAINER_PANEL_CENTER_BOTTOM_RIGHT:
      gtk_box_pack_end(GTK_BOX(ui->containers[c]), w, FALSE, FALSE, 0);
      break;

    /* if box is center we want it to fill as much as it can */
    case DT_UI_CONTAINER_PANEL_TOP_CENTER:
    case DT_UI_CONTAINER_PANEL_CENTER_TOP_LEFT:
    case DT_UI_CONTAINER_PANEL_CENTER_TOP_CENTER:
    case DT_UI_CONTAINER_PANEL_CENTER_BOTTOM_CENTER:
    case DT_UI_CONTAINER_PANEL_BOTTOM:
      gtk_box_pack_start(GTK_BOX(ui->containers[c]), w, TRUE, TRUE, 0);
      break;

    default:
    {
      gtk_box_pack_start(GTK_BOX(ui->containers[c]), w, FALSE, FALSE, 0);
    }
    break;
  }
  gtk_widget_show_all(w);
}

void dt_ui_container_focus_widget(dt_ui_t *ui, const dt_ui_container_t c, GtkWidget *w)
{
  g_return_if_fail(GTK_IS_CONTAINER(ui->containers[c]));

  if(GTK_WIDGET(ui->containers[c]) != gtk_widget_get_parent(w)) return;

  gtk_container_set_focus_child(GTK_CONTAINER(ui->containers[c]), w);
  gtk_widget_queue_draw(ui->containers[c]);
}

void dt_ui_container_foreach(struct dt_ui_t *ui, const dt_ui_container_t c, GtkCallback callback)
{
  g_return_if_fail(GTK_IS_CONTAINER(ui->containers[c]));
  gtk_container_foreach(GTK_CONTAINER(ui->containers[c]), callback, (gpointer)ui->containers[c]);
}

void dt_ui_container_destroy_children(struct dt_ui_t *ui, const dt_ui_container_t c)
{
  dt_gui_container_destroy_children(GTK_CONTAINER(ui->containers[c]));
}

void dt_ui_toggle_panels_visibility(struct dt_ui_t *ui)
{
  gchar *key = _panels_get_view_path("panel_collaps_state");
  const uint32_t state = dt_conf_get_int(key);

  if(state)
  {
    dt_conf_set_int(key, 0);
  }
  else
  {
    dt_conf_set_int(key, 1);
  }

  dt_ui_restore_panels(ui);
  g_free(key);
}

void dt_ui_notify_user()
{
  if(darktable.gui && !gtk_window_is_active(GTK_WINDOW(dt_ui_main_window(darktable.gui->ui))))
  {
    gtk_window_set_urgency_hint(GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)), TRUE);
#ifdef MAC_INTEGRATION
#ifdef GTK_TYPE_OSX_APPLICATION
    gtk_osxapplication_attention_request(g_object_new(GTK_TYPE_OSX_APPLICATION, NULL), INFO_REQUEST);
#else
    gtkosx_application_attention_request(g_object_new(GTKOSX_TYPE_APPLICATION, NULL), INFO_REQUEST);
#endif
#endif
  }
}

static void _ui_init_panel_size(GtkWidget *widget)
{
  gchar *key = NULL;

  int s = 128;
  if(strcmp(gtk_widget_get_name(widget), "right") == 0)
  {
    key = _panels_get_panel_path(DT_UI_PANEL_RIGHT, "_size");
    s = DT_UI_PANEL_SIDE_DEFAULT_SIZE; // default panel size
    if(key && dt_conf_key_exists(key))
      s = CLAMP(dt_conf_get_int(key), dt_conf_get_int("min_panel_width"), dt_conf_get_int("max_panel_width"));
    if(key) gtk_widget_set_size_request(widget, s, -1);
  }
  else if(strcmp(gtk_widget_get_name(widget), "left") == 0)
  {
    key = _panels_get_panel_path(DT_UI_PANEL_LEFT, "_size");
    s = DT_UI_PANEL_SIDE_DEFAULT_SIZE; // default panel size
    if(key && dt_conf_key_exists(key))
      s = CLAMP(dt_conf_get_int(key), dt_conf_get_int("min_panel_width"), dt_conf_get_int("max_panel_width"));
    if(key) gtk_widget_set_size_request(widget, s, -1);
  }
  else if(strcmp(gtk_widget_get_name(widget), "bottom") == 0)
  {
    key = _panels_get_panel_path(DT_UI_PANEL_BOTTOM, "_size");
    s = DT_UI_PANEL_BOTTOM_DEFAULT_SIZE; // default panel size
    if(key && dt_conf_key_exists(key))
      s = CLAMP(dt_conf_get_int(key), dt_conf_get_int("min_panel_height"), dt_conf_get_int("max_panel_height"));
    if(key) gtk_widget_set_size_request(widget, -1, s);
  }

  g_free(key);
}

void dt_ui_restore_panels(dt_ui_t *ui)
{
  /* restore left & right panel size */
  _ui_init_panel_size(ui->panels[DT_UI_PANEL_LEFT]);
  _ui_init_panel_size(ui->panels[DT_UI_PANEL_RIGHT]);
  _ui_init_panel_size(ui->panels[DT_UI_PANEL_BOTTOM]);

  /* restore from a previous collapse all panel state if enabled */
  gchar *key = _panels_get_view_path("panel_collaps_state");
  const uint32_t state = dt_conf_get_int(key);
  g_free(key);
  if(state)
  {
    /* hide all panels (we let saved state as it is, to recover them when pressing TAB)*/
    for(int k = 0; k < DT_UI_PANEL_SIZE; k++) dt_ui_panel_show(ui, k, FALSE, FALSE);
  }
  else
  {
    /* restore the visible state of panels */
    for(int k = 0; k < DT_UI_PANEL_SIZE; k++)
    {
      key = _panels_get_panel_path(k, "_visible");
      if(dt_conf_key_exists(key))
        dt_ui_panel_show(ui, k, dt_conf_get_bool(key), FALSE);
      else
        dt_ui_panel_show(ui, k, TRUE, TRUE);

      g_free(key);
    }
  }

  // restore the visible state of the collapsing controls
  gint visible = TRUE;
  key = _panels_get_view_path("panels_collapse_controls");
  if(dt_conf_key_exists(key)) visible = dt_conf_get_bool(key);
  dt_conf_set_bool(key, visible);
  g_free(key);

  gtk_widget_set_visible(GTK_WIDGET(darktable.gui->widgets.right_border), visible);
  gtk_widget_set_visible(GTK_WIDGET(darktable.gui->widgets.left_border), visible);
  gtk_widget_set_visible(GTK_WIDGET(darktable.gui->widgets.top_border), visible);
  gtk_widget_set_visible(GTK_WIDGET(darktable.gui->widgets.bottom_border), visible);
}

void dt_ui_update_scrollbars(dt_ui_t *ui)
{
  if(!darktable.gui->scrollbars.visible) return;

  /* update scrollbars for current view */
  const dt_view_t *cv = dt_view_manager_get_current_view(darktable.view_manager);

  if(cv->vscroll_size > cv->vscroll_viewport_size){
    gtk_adjustment_configure(gtk_range_get_adjustment(GTK_RANGE(darktable.gui->scrollbars.vscrollbar)),
                             cv->vscroll_pos, cv->vscroll_lower, cv->vscroll_size, 0, cv->vscroll_viewport_size,
                             cv->vscroll_viewport_size);
  }

  if(cv->hscroll_size > cv->hscroll_viewport_size){
    gtk_adjustment_configure(gtk_range_get_adjustment(GTK_RANGE(darktable.gui->scrollbars.hscrollbar)),
                             cv->hscroll_pos, cv->hscroll_lower, cv->hscroll_size, 0, cv->hscroll_viewport_size,
                             cv->hscroll_viewport_size);
  }

  gtk_widget_set_visible(darktable.gui->scrollbars.vscrollbar, cv->vscroll_size > cv->vscroll_viewport_size);
  gtk_widget_set_visible(darktable.gui->scrollbars.hscrollbar, cv->hscroll_size > cv->hscroll_viewport_size);
}

void dt_ui_scrollbars_show(dt_ui_t *ui, gboolean show)
{
  darktable.gui->scrollbars.visible = show;

  if(show)
  {
    dt_ui_update_scrollbars(ui);
  }
  else
  {
    gtk_widget_hide(darktable.gui->scrollbars.vscrollbar);
    gtk_widget_hide(darktable.gui->scrollbars.hscrollbar);
  }
}

void dt_ui_panel_show(dt_ui_t *ui, const dt_ui_panel_t p, gboolean show, gboolean write)
{
  g_return_if_fail(GTK_IS_WIDGET(ui->panels[p]));

  // for left and right sides, panels are inside a gtkoverlay
  GtkWidget *over_panel = NULL;
  if(p == DT_UI_PANEL_LEFT || p == DT_UI_PANEL_RIGHT || p == DT_UI_PANEL_BOTTOM)
    over_panel = gtk_widget_get_parent(ui->panels[p]);

  if(show)
  {
    gtk_widget_show(ui->panels[p]);
    if(over_panel) gtk_widget_show(over_panel);
  }
  else
  {
    gtk_widget_hide(ui->panels[p]);
    if(over_panel) gtk_widget_hide(over_panel);
  }

  // force redraw of the border (to be sure the arrow in the right direction)
  if(p == DT_UI_PANEL_TOP || p == DT_UI_PANEL_CENTER_TOP)
    gtk_widget_queue_draw(darktable.gui->widgets.top_border);
  else if(p == DT_UI_PANEL_BOTTOM || p == DT_UI_PANEL_CENTER_BOTTOM)
    gtk_widget_queue_draw(darktable.gui->widgets.bottom_border);
  else if(p == DT_UI_PANEL_LEFT)
    gtk_widget_queue_draw(darktable.gui->widgets.left_border);
  else if(p == DT_UI_PANEL_RIGHT)
    gtk_widget_queue_draw(darktable.gui->widgets.right_border);

  if(write)
  {
    gchar *key;
    if(show)
    {
      // we reset the collaps_panel value if we show a panel
      key = _panels_get_view_path("panel_collaps_state");
      if(dt_conf_get_int(key) != 0)
      {
        dt_conf_set_int(key, 0);
        g_free(key);
        // we ensure that all panels state are recorded as hidden
        for(int k = 0; k < DT_UI_PANEL_SIZE; k++)
        {
          key = _panels_get_panel_path(k, "_visible");
          dt_conf_set_bool(key, FALSE);
          g_free(key);
        }
      }
      else
        g_free(key);
      key = _panels_get_panel_path(p, "_visible");
      dt_conf_set_bool(key, show);
      g_free(key);
    }
    else
    {
      // if it was the last visible panel, we set collaps_panel value instead
      // so collapsing panels after will have an effect
      gboolean collapse = TRUE;
      for(int k = 0; k < DT_UI_PANEL_SIZE; k++)
      {
        if(k != p && dt_ui_panel_visible(ui, k))
        {
          collapse = FALSE;
          break;
        }
      }

      if(collapse)
      {
        key = _panels_get_view_path("panel_collaps_state");
        dt_conf_set_int(key, 1);
        g_free(key);
      }
      else
      {
        key = _panels_get_panel_path(p, "_visible");
        dt_conf_set_bool(key, show);
        g_free(key);
      }
    }
  }
}

gboolean dt_ui_panel_visible(dt_ui_t *ui, const dt_ui_panel_t p)
{
  g_return_val_if_fail(GTK_IS_WIDGET(ui->panels[p]), FALSE);
  return gtk_widget_get_visible(ui->panels[p]);
}

int dt_ui_panel_get_size(dt_ui_t *ui, const dt_ui_panel_t p)
{
  gchar *key = NULL;

  if(p == DT_UI_PANEL_LEFT || p == DT_UI_PANEL_RIGHT || p == DT_UI_PANEL_BOTTOM)
  {
    int size = 0;

    key = _panels_get_panel_path(p, "_size");
    if(key && dt_conf_key_exists(key))
    {
      size = dt_conf_get_int(key);
      g_free(key);
    }
    else // size hasn't been adjusted, so return default sizes
    {
      if(p == DT_UI_PANEL_BOTTOM)
        size = DT_UI_PANEL_BOTTOM_DEFAULT_SIZE;
      else
        size = DT_UI_PANEL_SIDE_DEFAULT_SIZE;
    }
    return size;
  }
  return -1;
}

void dt_ui_panel_set_size(dt_ui_t *ui, const dt_ui_panel_t p, int s)
{
  gchar *key = NULL;

  if(p == DT_UI_PANEL_LEFT || p == DT_UI_PANEL_RIGHT || p == DT_UI_PANEL_BOTTOM)
  {
    const int width = CLAMP(s, dt_conf_get_int("min_panel_width"), dt_conf_get_int("max_panel_width"));
    gtk_widget_set_size_request(ui->panels[p], width, -1);
    key = _panels_get_panel_path(p, "_size");
    dt_conf_set_int(key, width);
    g_free(key);
  }
}

gboolean dt_ui_panel_ancestor(struct dt_ui_t *ui, const dt_ui_panel_t p, GtkWidget *w)
{
  g_return_val_if_fail(GTK_IS_WIDGET(ui->panels[p]), FALSE);
  return gtk_widget_is_ancestor(w, ui->panels[p]) || gtk_widget_is_ancestor(ui->panels[p], w);
}

GtkWidget *dt_ui_center(dt_ui_t *ui)
{
  return ui->center;
}
GtkWidget *dt_ui_center_base(dt_ui_t *ui)
{
  return ui->center_base;
}
dt_thumbtable_t *dt_ui_thumbtable(struct dt_ui_t *ui)
{
  return ui->thumbtable;
}
GtkWidget *dt_ui_log_msg(struct dt_ui_t *ui)
{
  return ui->log_msg;
}
GtkWidget *dt_ui_toast_msg(struct dt_ui_t *ui)
{
  return ui->toast_msg;
}

GtkWidget *dt_ui_main_window(dt_ui_t *ui)
{
  return ui->main_window;
}

static GtkWidget *_ui_init_panel_container_top(GtkWidget *container)
{
  GtkWidget *w = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_UI_PANEL_MODULE_SPACING);
  gtk_box_pack_start(GTK_BOX(container), w, FALSE, FALSE, 0);
  return w;
}

static gboolean _ui_init_panel_container_center_scroll_event(GtkWidget *widget, GdkEventScroll *event)
{
  // just make sure nothing happens unless ctrl-alt are pressed:
  return (((event->state & gtk_accelerator_get_default_mod_mask()) != darktable.gui->sidebar_scroll_mask) != dt_conf_get_bool("darkroom/ui/sidebar_scroll_default"));
}

static GtkWidget *_ui_init_panel_container_center(GtkWidget *container, gboolean left)
{
  GtkWidget *widget;
  GtkAdjustment *a[4];

  a[0] = GTK_ADJUSTMENT(gtk_adjustment_new(0, 0, 100, 1, 10, 10));
  a[1] = GTK_ADJUSTMENT(gtk_adjustment_new(0, 0, 100, 1, 10, 10));
  a[2] = GTK_ADJUSTMENT(gtk_adjustment_new(0, 0, 100, 1, 10, 10));
  a[3] = GTK_ADJUSTMENT(gtk_adjustment_new(0, 0, 100, 1, 10, 10));

  /* create the scrolled window */
  widget = gtk_scrolled_window_new(a[0], a[1]);
  gtk_widget_set_can_focus(widget, TRUE);
  gtk_scrolled_window_set_placement(GTK_SCROLLED_WINDOW(widget),
                                    left ? GTK_CORNER_TOP_LEFT : GTK_CORNER_TOP_RIGHT);
  gtk_box_pack_start(GTK_BOX(container), widget, TRUE, TRUE, 0);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(widget), GTK_POLICY_AUTOMATIC,
                                 dt_conf_get_bool("panel_scrollbars_always_visible")?GTK_POLICY_ALWAYS:GTK_POLICY_AUTOMATIC);

  // we want the left/right window border to scroll the module lists
  g_signal_connect(G_OBJECT(left ? darktable.gui->widgets.right_border : darktable.gui->widgets.left_border),
                   "scroll-event", G_CALLBACK(_borders_scrolled), widget);

  /* create the scrolled viewport */
  container = widget;
  widget = gtk_viewport_new(a[2], a[3]);
  gtk_viewport_set_shadow_type(GTK_VIEWPORT(widget), GTK_SHADOW_NONE);
  gtk_container_add(GTK_CONTAINER(container), widget);

  /* avoid scrolling with wheel, it's distracting (you'll end up over a control, and scroll it's value) */
  g_signal_connect(G_OBJECT(widget), "scroll-event", G_CALLBACK(_ui_init_panel_container_center_scroll_event),
                   NULL);

  /* create the container */
  container = widget;
  widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_name(widget, "plugins_vbox_left");
  gtk_container_add(GTK_CONTAINER(container), widget);

  return widget;
}

static GtkWidget *_ui_init_panel_container_bottom(GtkWidget *container)
{
  GtkWidget *w = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_pack_start(GTK_BOX(container), w, FALSE, FALSE, 0);
  return w;
}

static gboolean _panel_handle_button_callback(GtkWidget *w, GdkEventButton *e, gpointer user_data)
{
  if(e->button == 1)
  {
    if(e->type == GDK_BUTTON_PRESS)
    {
      /* store current  mousepointer position */
      gdk_window_get_device_position(e->window,
                                     gdk_seat_get_pointer(gdk_display_get_default_seat(gdk_window_get_display(
                                         gtk_widget_get_window(dt_ui_main_window(darktable.gui->ui))))),
                                     &darktable.gui->widgets.panel_handle_x,
                                     &darktable.gui->widgets.panel_handle_y, 0);

      darktable.gui->widgets.panel_handle_dragging = TRUE;
    }
    else if(e->type == GDK_BUTTON_RELEASE)
    {
      darktable.gui->widgets.panel_handle_dragging = FALSE;
    }
    else if(e->type == GDK_2BUTTON_PRESS)
    {
      darktable.gui->widgets.panel_handle_dragging = FALSE;
      // we hide the panel
      if(strcmp(gtk_widget_get_name(w), "panel-handle-right") == 0)
        dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_RIGHT, FALSE, TRUE);
      else if(strcmp(gtk_widget_get_name(w), "panel-handle-left") == 0)
        dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_LEFT, FALSE, TRUE);
      else if(strcmp(gtk_widget_get_name(w), "panel-handle-bottom") == 0)
        dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_BOTTOM, FALSE, TRUE);
    }
  }
  return TRUE;
}
static gboolean _panel_handle_cursor_callback(GtkWidget *w, GdkEventCrossing *e, gpointer user_data)
{
  if(strcmp(gtk_widget_get_name(w), "panel-handle-bottom") == 0)
    dt_control_change_cursor((e->type == GDK_ENTER_NOTIFY) ? GDK_SB_V_DOUBLE_ARROW : GDK_LEFT_PTR);
  else
    dt_control_change_cursor((e->type == GDK_ENTER_NOTIFY) ? GDK_SB_H_DOUBLE_ARROW : GDK_LEFT_PTR);
  return TRUE;
}
static gboolean _panel_handle_motion_callback(GtkWidget *w, GdkEventButton *e, gpointer user_data)
{
  GtkWidget *widget = (GtkWidget *)user_data;
  if(darktable.gui->widgets.panel_handle_dragging)
  {
    gint x, y, sx, sy;
    // FIXME: can work with the event x,y to skip the gdk_window_get_device_position() call?
    gdk_window_get_device_position(e->window,
                                   gdk_seat_get_pointer(gdk_display_get_default_seat(gdk_window_get_display(
                                       gtk_widget_get_window(dt_ui_main_window(darktable.gui->ui))))),
                                   &x, &y, 0);
    gtk_widget_get_size_request(widget, &sx, &sy);

    // conf entry to store the new size
    gchar *key = NULL;
    if(strcmp(gtk_widget_get_name(w), "panel-handle-right") == 0)
    {
      sx = CLAMP((sx + darktable.gui->widgets.panel_handle_x - x), dt_conf_get_int("min_panel_width"),
                 dt_conf_get_int("max_panel_width"));
      key = _panels_get_panel_path(DT_UI_PANEL_RIGHT, "_size");
      gtk_widget_set_size_request(widget, sx, -1);
    }
    else if(strcmp(gtk_widget_get_name(w), "panel-handle-left") == 0)
    {
      sx = CLAMP((sx - darktable.gui->widgets.panel_handle_x + x), dt_conf_get_int("min_panel_width"),
                 dt_conf_get_int("max_panel_width"));
      key = _panels_get_panel_path(DT_UI_PANEL_LEFT, "_size");
      gtk_widget_set_size_request(widget, sx, -1);
    }
    else if(strcmp(gtk_widget_get_name(w), "panel-handle-bottom") == 0)
    {
      sx = CLAMP((sy + darktable.gui->widgets.panel_handle_y - y), dt_conf_get_int("min_panel_height"),
                 dt_conf_get_int("max_panel_height"));
      key = _panels_get_panel_path(DT_UI_PANEL_BOTTOM, "_size");
      gtk_widget_set_size_request(widget, -1, sx);
    }

    // we store and apply the new value
    dt_conf_set_int(key, sx);
    g_free(key);

    return TRUE;
  }

  return FALSE;
}

static void _ui_init_panel_left(dt_ui_t *ui, GtkWidget *container)
{
  GtkWidget *widget;

  /* create left panel main widget and add it to ui */
  darktable.gui->widgets.panel_handle_dragging = FALSE;
  widget = ui->panels[DT_UI_PANEL_LEFT] = dtgtk_side_panel_new();
  gtk_widget_set_name(widget, "left");
  _ui_init_panel_size(widget);

  GtkWidget *over = gtk_overlay_new();
  gtk_container_add(GTK_CONTAINER(over), widget);
  // we add a transparent overlay over the modules margins to resize the panel
  GtkWidget *handle = gtk_drawing_area_new();
  gtk_widget_set_halign(handle, GTK_ALIGN_END);
  gtk_widget_set_valign(handle, GTK_ALIGN_FILL);
  gtk_widget_set_size_request(handle, DT_PIXEL_APPLY_DPI(5), -1);
  gtk_overlay_add_overlay(GTK_OVERLAY(over), handle);
  gtk_widget_set_events(handle, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_ENTER_NOTIFY_MASK
                                    | GDK_LEAVE_NOTIFY_MASK | GDK_POINTER_MOTION_MASK);
  gtk_widget_set_name(GTK_WIDGET(handle), "panel-handle-left");
  g_signal_connect(G_OBJECT(handle), "button-press-event", G_CALLBACK(_panel_handle_button_callback), handle);
  g_signal_connect(G_OBJECT(handle), "button-release-event", G_CALLBACK(_panel_handle_button_callback), handle);
  g_signal_connect(G_OBJECT(handle), "motion-notify-event", G_CALLBACK(_panel_handle_motion_callback), widget);
  g_signal_connect(G_OBJECT(handle), "leave-notify-event", G_CALLBACK(_panel_handle_cursor_callback), handle);
  g_signal_connect(G_OBJECT(handle), "enter-notify-event", G_CALLBACK(_panel_handle_cursor_callback), handle);
  gtk_widget_show(handle);

  gtk_grid_attach(GTK_GRID(container), over, 1, 1, 1, 1);

  /* add top,center,bottom*/
  container = widget;
  ui->containers[DT_UI_CONTAINER_PANEL_LEFT_TOP] = _ui_init_panel_container_top(container);
  ui->containers[DT_UI_CONTAINER_PANEL_LEFT_CENTER] = _ui_init_panel_container_center(container, FALSE);
  ui->containers[DT_UI_CONTAINER_PANEL_LEFT_BOTTOM] = _ui_init_panel_container_bottom(container);

  /* lets show all widgets */
  gtk_widget_show_all(ui->panels[DT_UI_PANEL_LEFT]);
}

static void _ui_init_panel_right(dt_ui_t *ui, GtkWidget *container)
{
  GtkWidget *widget;

  /* create left panel main widget and add it to ui */
  darktable.gui->widgets.panel_handle_dragging = FALSE;
  widget = ui->panels[DT_UI_PANEL_RIGHT] = dtgtk_side_panel_new();
  gtk_widget_set_name(widget, "right");
  _ui_init_panel_size(widget);

  GtkWidget *over = gtk_overlay_new();
  gtk_container_add(GTK_CONTAINER(over), widget);
  // we add a transparent overlay over the modules margins to resize the panel
  GtkWidget *handle = gtk_drawing_area_new();
  gtk_widget_set_halign(handle, GTK_ALIGN_START);
  gtk_widget_set_valign(handle, GTK_ALIGN_FILL);
  gtk_widget_set_size_request(handle, DT_PIXEL_APPLY_DPI(5), -1);
  gtk_overlay_add_overlay(GTK_OVERLAY(over), handle);
  gtk_widget_set_events(handle, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_ENTER_NOTIFY_MASK
                                    | GDK_LEAVE_NOTIFY_MASK | GDK_POINTER_MOTION_MASK);
  gtk_widget_set_name(GTK_WIDGET(handle), "panel-handle-right");
  g_signal_connect(G_OBJECT(handle), "button-press-event", G_CALLBACK(_panel_handle_button_callback), handle);
  g_signal_connect(G_OBJECT(handle), "button-release-event", G_CALLBACK(_panel_handle_button_callback), handle);
  g_signal_connect(G_OBJECT(handle), "motion-notify-event", G_CALLBACK(_panel_handle_motion_callback), widget);
  g_signal_connect(G_OBJECT(handle), "leave-notify-event", G_CALLBACK(_panel_handle_cursor_callback), handle);
  g_signal_connect(G_OBJECT(handle), "enter-notify-event", G_CALLBACK(_panel_handle_cursor_callback), handle);
  gtk_widget_show(handle);

  gtk_grid_attach(GTK_GRID(container), over, 3, 1, 1, 1);

  /* add top,center,bottom*/
  container = widget;
  ui->containers[DT_UI_CONTAINER_PANEL_RIGHT_TOP] = _ui_init_panel_container_top(container);
  ui->containers[DT_UI_CONTAINER_PANEL_RIGHT_CENTER] = _ui_init_panel_container_center(container, TRUE);
  ui->containers[DT_UI_CONTAINER_PANEL_RIGHT_BOTTOM] = _ui_init_panel_container_bottom(container);

  /* lets show all widgets */
  gtk_widget_show_all(ui->panels[DT_UI_PANEL_RIGHT]);
}

static void _ui_init_panel_top(dt_ui_t *ui, GtkWidget *container)
{
  GtkWidget *widget;

  /* create the panel box */
  ui->panels[DT_UI_PANEL_TOP] = widget = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand(GTK_WIDGET(widget), TRUE);
  gtk_grid_attach(GTK_GRID(container), widget, 1, 0, 3, 1);

  /* add container for top left */
  ui->containers[DT_UI_CONTAINER_PANEL_TOP_LEFT] = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_start(GTK_BOX(widget), ui->containers[DT_UI_CONTAINER_PANEL_TOP_LEFT], FALSE, FALSE,
                     DT_UI_PANEL_MODULE_SPACING);

  /* add container for top center */
  ui->containers[DT_UI_CONTAINER_PANEL_TOP_CENTER] = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_start(GTK_BOX(widget), ui->containers[DT_UI_CONTAINER_PANEL_TOP_CENTER], TRUE, TRUE,
                     DT_UI_PANEL_MODULE_SPACING);

  /* add container for top right */
  ui->containers[DT_UI_CONTAINER_PANEL_TOP_RIGHT] = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_end(GTK_BOX(widget), ui->containers[DT_UI_CONTAINER_PANEL_TOP_RIGHT], FALSE, FALSE,
                   DT_UI_PANEL_MODULE_SPACING);
}

static void _ui_init_panel_bottom(dt_ui_t *ui, GtkWidget *container)
{
  GtkWidget *widget;

  /* create the panel box */
  ui->panels[DT_UI_PANEL_BOTTOM] = widget = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  // gtk_widget_set_hexpand(GTK_WIDGET(widget), TRUE);
  // gtk_widget_set_vexpand(GTK_WIDGET(widget), TRUE);
  gtk_widget_set_name(widget, "bottom");
  _ui_init_panel_size(widget);

  GtkWidget *over = gtk_overlay_new();
  gtk_container_add(GTK_CONTAINER(over), widget);
  // we add a transparent overlay over the modules margins to resize the panel
  GtkWidget *handle = gtk_drawing_area_new();
  gtk_widget_set_halign(handle, GTK_ALIGN_FILL);
  gtk_widget_set_valign(handle, GTK_ALIGN_START);
  gtk_widget_set_size_request(handle, -1, DT_PIXEL_APPLY_DPI(5));
  gtk_overlay_add_overlay(GTK_OVERLAY(over), handle);
  gtk_widget_set_events(handle, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_ENTER_NOTIFY_MASK
                                    | GDK_LEAVE_NOTIFY_MASK | GDK_POINTER_MOTION_MASK);
  gtk_widget_set_name(GTK_WIDGET(handle), "panel-handle-bottom");
  g_signal_connect(G_OBJECT(handle), "button-press-event", G_CALLBACK(_panel_handle_button_callback), handle);
  g_signal_connect(G_OBJECT(handle), "button-release-event", G_CALLBACK(_panel_handle_button_callback), handle);
  g_signal_connect(G_OBJECT(handle), "motion-notify-event", G_CALLBACK(_panel_handle_motion_callback), widget);
  g_signal_connect(G_OBJECT(handle), "leave-notify-event", G_CALLBACK(_panel_handle_cursor_callback), handle);
  g_signal_connect(G_OBJECT(handle), "enter-notify-event", G_CALLBACK(_panel_handle_cursor_callback), handle);
  gtk_widget_show(handle);

  gtk_grid_attach(GTK_GRID(container), over, 1, 2, 3, 1);

  /* add the container */
  ui->containers[DT_UI_CONTAINER_PANEL_BOTTOM] = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_start(GTK_BOX(widget), ui->containers[DT_UI_CONTAINER_PANEL_BOTTOM], TRUE, TRUE,
                     DT_UI_PANEL_MODULE_SPACING);
}


static void _ui_init_panel_center_top(dt_ui_t *ui, GtkWidget *container)
{
  GtkWidget *widget;

  /* create the panel box */
  ui->panels[DT_UI_PANEL_CENTER_TOP] = widget = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name(widget, "header-toolbar");
  dt_gui_add_class(widget, "dt_big_btn_canvas");

  gtk_box_pack_start(GTK_BOX(container), widget, FALSE, TRUE, 0);

  /* add container for center top left */
  ui->containers[DT_UI_CONTAINER_PANEL_CENTER_TOP_LEFT] = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_start(GTK_BOX(widget), ui->containers[DT_UI_CONTAINER_PANEL_CENTER_TOP_LEFT], TRUE, TRUE,
                     DT_UI_PANEL_MODULE_SPACING);

  /* add container for center top center */
  ui->containers[DT_UI_CONTAINER_PANEL_CENTER_TOP_CENTER] = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_start(GTK_BOX(widget), ui->containers[DT_UI_CONTAINER_PANEL_CENTER_TOP_CENTER], TRUE, FALSE,
                     DT_UI_PANEL_MODULE_SPACING);

  /* add container for center top right */
  ui->containers[DT_UI_CONTAINER_PANEL_CENTER_TOP_RIGHT] = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_end(GTK_BOX(widget), ui->containers[DT_UI_CONTAINER_PANEL_CENTER_TOP_RIGHT], FALSE, FALSE,
                   DT_UI_PANEL_MODULE_SPACING);
}

static void _ui_init_panel_center_bottom(dt_ui_t *ui, GtkWidget *container)
{
  GtkWidget *widget;

  /* create the panel box */
  ui->panels[DT_UI_PANEL_CENTER_BOTTOM] = widget = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name(widget, "footer-toolbar");
  dt_gui_add_class(widget, "dt_big_btn_canvas");
  gtk_box_pack_start(GTK_BOX(container), widget, FALSE, TRUE, 0);

  /* adding the center bottom left toolbox */
  ui->containers[DT_UI_CONTAINER_PANEL_CENTER_BOTTOM_LEFT] = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_start(GTK_BOX(widget), ui->containers[DT_UI_CONTAINER_PANEL_CENTER_BOTTOM_LEFT], TRUE, TRUE,
                     DT_UI_PANEL_MODULE_SPACING);

  /* adding the center box */
  ui->containers[DT_UI_CONTAINER_PANEL_CENTER_BOTTOM_CENTER] = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_pack_start(GTK_BOX(widget), ui->containers[DT_UI_CONTAINER_PANEL_CENTER_BOTTOM_CENTER], FALSE, TRUE,
                     DT_UI_PANEL_MODULE_SPACING);

  /* adding the right toolbox */
  ui->containers[DT_UI_CONTAINER_PANEL_CENTER_BOTTOM_RIGHT] = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_start(GTK_BOX(widget), ui->containers[DT_UI_CONTAINER_PANEL_CENTER_BOTTOM_RIGHT], TRUE, TRUE,
                     DT_UI_PANEL_MODULE_SPACING);
}

/* this is called as a signal handler, the signal raising logic asserts the gdk lock. */
static void _ui_widget_redraw_callback(gpointer instance, GtkWidget *widget)
{
   gtk_widget_queue_draw(widget);
}

static void _ui_log_redraw_callback(gpointer instance, GtkWidget *widget)
{
  // draw log message, if any
  dt_pthread_mutex_lock(&darktable.control->log_mutex);
  if(darktable.control->log_ack != darktable.control->log_pos)
  {
    if(strcmp(darktable.control->log_message[darktable.control->log_ack], gtk_label_get_text(GTK_LABEL(widget))))
      gtk_label_set_markup(GTK_LABEL(widget), darktable.control->log_message[darktable.control->log_ack]);
    if(!gtk_widget_get_visible(widget))
    {
      const int h = gtk_widget_get_allocated_height(dt_ui_center_base(darktable.gui->ui));
      gtk_widget_set_margin_bottom(gtk_widget_get_parent(widget), 0.15 * h - DT_PIXEL_APPLY_DPI(10));
      gtk_widget_show(widget);
    }
  }
  else
  {
    if(gtk_widget_get_visible(widget)) gtk_widget_hide(widget);
  }
  dt_pthread_mutex_unlock(&darktable.control->log_mutex);
}

static void _ui_toast_redraw_callback(gpointer instance, GtkWidget *widget)
{
  // draw toast message, if any
  dt_pthread_mutex_lock(&darktable.control->toast_mutex);
  if(darktable.control->toast_ack != darktable.control->toast_pos)
  {
    if(strcmp(darktable.control->toast_message[darktable.control->toast_ack], gtk_label_get_text(GTK_LABEL(widget))))
      gtk_label_set_markup(GTK_LABEL(widget), darktable.control->toast_message[darktable.control->toast_ack]);
    if(!gtk_widget_get_visible(widget))
    {
      const int h = gtk_widget_get_allocated_height(dt_ui_center_base(darktable.gui->ui));
      gtk_widget_set_margin_bottom(gtk_widget_get_parent(widget), 0.15 * h - DT_PIXEL_APPLY_DPI(10));
      gtk_widget_show(widget);
    }
  }
  else
  {
    if(gtk_widget_get_visible(widget)) gtk_widget_hide(widget);
  }
  dt_pthread_mutex_unlock(&darktable.control->toast_mutex);
}

void dt_ellipsize_combo(GtkComboBox *cbox)
{
  GList *renderers = gtk_cell_layout_get_cells(GTK_CELL_LAYOUT(cbox));
  for(const GList *it = renderers; it; it = g_list_next(it))
  {
    GtkCellRendererText *tr = GTK_CELL_RENDERER_TEXT(it->data);
    g_object_set(G_OBJECT(tr), "ellipsize", PANGO_ELLIPSIZE_MIDDLE, (gchar *)0);
  }
  g_list_free(renderers);
}

typedef struct result_t
{
  enum {RESULT_NONE, RESULT_NO, RESULT_YES} result;
  char *entry_text;
  GtkWidget *window, *entry, *button_yes, *button_no;
} result_t;

static void _yes_no_button_handler(GtkButton *button, gpointer data)
{
  result_t *result = (result_t *)data;

  if((void *)button == (void *)result->button_yes)
    result->result = RESULT_YES;
  else if((void *)button == (void *)result->button_no)
    result->result = RESULT_NO;

  if(result->entry)
    result->entry_text = g_strdup(gtk_entry_get_text(GTK_ENTRY(result->entry)));
  gtk_widget_destroy(result->window);
  gtk_main_quit();
}

gboolean dt_gui_show_standalone_yes_no_dialog(const char *title, const char *markup, const char *no_text,
                                              const char *yes_text)
{
  GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(window);
#endif

  // themes not yet loaded, no CSS add some manual padding
  const int padding = darktable.themes ? 0 : 5;

  gtk_window_set_icon_name(GTK_WINDOW(window), "darktable");
  gtk_window_set_title(GTK_WINDOW(window), title);
  g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

  if(darktable.gui)
  {
    GtkWindow *win = GTK_WINDOW(dt_ui_main_window(darktable.gui->ui));
    gtk_window_set_transient_for(GTK_WINDOW(window), win);
    gtk_window_set_modal(GTK_WINDOW(window), TRUE);
    if(gtk_widget_get_visible(GTK_WIDGET(win)))
    {
      gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER_ON_PARENT);
    }
    else
    {
      gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_MOUSE);
    }
  }
  else
  {
    gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_MOUSE);
  }

  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, padding);
  gtk_container_add(GTK_CONTAINER(window), vbox);

  GtkWidget *mhbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, padding);
  gtk_box_pack_start(GTK_BOX(vbox), mhbox, TRUE, TRUE, padding);

  if(padding)
  {
    gtk_box_pack_start(GTK_BOX(mhbox),
                       gtk_box_new(GTK_ORIENTATION_VERTICAL, padding), TRUE, TRUE, padding);
  }

  GtkWidget *label = gtk_label_new(NULL);
  gtk_label_set_markup(GTK_LABEL(label), markup);
  gtk_box_pack_start(GTK_BOX(mhbox), label, TRUE, TRUE, padding);

  if(padding)
  {
    gtk_box_pack_start(GTK_BOX(mhbox),
                       gtk_box_new(GTK_ORIENTATION_VERTICAL, padding), TRUE, TRUE, padding);
  }

  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_start(GTK_BOX(vbox), hbox, TRUE, TRUE, 0);

  result_t result = {.result = RESULT_NONE, .window = window};

  GtkWidget *button;

  if(no_text)
  {
    button = gtk_button_new_with_label(no_text);
    result.button_no = button;
    g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(_yes_no_button_handler), &result);
    gtk_box_pack_start(GTK_BOX(hbox), button, TRUE, TRUE, 0);
  }

  if(yes_text)
  {
    button = gtk_button_new_with_label(yes_text);
    result.button_yes = button;
    g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(_yes_no_button_handler), &result);
    gtk_box_pack_start(GTK_BOX(hbox), button, TRUE, TRUE, 0);
  }

  gtk_widget_show_all(window);
  gtk_main();

  return result.result == RESULT_YES;
}

char *dt_gui_show_standalone_string_dialog(const char *title, const char *markup, const char *placeholder,
                                           const char *no_text, const char *yes_text)
{
  GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(window);
#endif

  gtk_window_set_icon_name(GTK_WINDOW(window), "darktable");
  gtk_window_set_title(GTK_WINDOW(window), title);
  g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

  if(darktable.gui)
  {
    GtkWindow *win = GTK_WINDOW(dt_ui_main_window(darktable.gui->ui));
    gtk_window_set_transient_for(GTK_WINDOW(window), win);
    if(gtk_widget_get_visible(GTK_WIDGET(win)))
    {
      gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER_ON_PARENT);
    }
    else
    {
      gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_MOUSE);
    }
  }
  else
  {
    gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_MOUSE);
  }

  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
  gtk_widget_set_margin_start(vbox, 10);
  gtk_widget_set_margin_end(vbox, 10);
  gtk_widget_set_margin_top(vbox, 7);
  gtk_widget_set_margin_bottom(vbox, 5);
  gtk_container_add(GTK_CONTAINER(window), vbox);

  GtkWidget *label = gtk_label_new(NULL);
  gtk_label_set_markup(GTK_LABEL(label), markup);
  gtk_box_pack_start(GTK_BOX(vbox), label, TRUE, TRUE, 0);

  GtkWidget *entry = gtk_entry_new();
  g_object_ref(entry);
  if(placeholder)
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), placeholder);
  gtk_box_pack_start(GTK_BOX(vbox), entry, TRUE, TRUE, 0);

  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
  gtk_widget_set_margin_top(hbox, 10);
  gtk_box_pack_start(GTK_BOX(vbox), hbox, TRUE, TRUE, 0);

  result_t result = {.result = RESULT_NONE, .window = window, .entry = entry};

  GtkWidget *button;

  if(no_text)
  {
    button = gtk_button_new_with_label(no_text);
    result.button_no = button;
    g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(_yes_no_button_handler), &result);
    gtk_box_pack_start(GTK_BOX(hbox), button, TRUE, TRUE, 0);
  }

  if(yes_text)
  {
    button = gtk_button_new_with_label(yes_text);
    result.button_yes = button;
    g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(_yes_no_button_handler), &result);
    gtk_box_pack_start(GTK_BOX(hbox), button, TRUE, TRUE, 0);
  }

  gtk_widget_show_all(window);
  gtk_main();

  if(result.result == RESULT_YES)
    return result.entry_text;

  g_free(result.entry_text);
  return NULL;
}

// TODO: should that go to another place than gtk.c?
void dt_gui_add_help_link(GtkWidget *widget, const char *link)
{
  g_object_set_data(G_OBJECT(widget), "dt-help-url", (void *)link);
  gtk_widget_add_events(widget, GDK_BUTTON_PRESS_MASK);
}

// load a CSS theme
void dt_gui_load_theme(const char *theme)
{
  char theme_css[PATH_MAX] = { 0 };
  g_snprintf(theme_css, sizeof(theme_css), "%s.css", theme);

  if(!dt_conf_key_exists("use_system_font"))
    dt_conf_set_bool("use_system_font", TRUE);

  //set font size
  if(dt_conf_get_bool("use_system_font"))
    gtk_settings_reset_property(gtk_settings_get_default(), "gtk-font-name");
  else
  {
    //font name can only use period as decimal separator
    //but printf format strings use comma for some locales, so replace comma with period
    gchar *font_size = g_strdup_printf(_("%.1f"), dt_conf_get_float("font_size"));
    gchar *font_size_updated = dt_util_str_replace(font_size, ",", ".");
    gchar *font_name = g_strdup_printf(_("Sans %s"), font_size_updated);
    g_object_set(gtk_settings_get_default(), "gtk-font-name", font_name, NULL);
    g_free(font_size_updated);
    g_free(font_size);
    g_free(font_name);
  }

  gchar *path, *usercsspath;
  char datadir[PATH_MAX] = { 0 }, configdir[PATH_MAX] = { 0 };
  dt_loc_get_datadir(datadir, sizeof(datadir));
  dt_loc_get_user_config_dir(configdir, sizeof(configdir));

  // user dir theme
  path = g_build_filename(configdir, "themes", theme_css, NULL);
  if(!g_file_test(path, G_FILE_TEST_EXISTS))
  {
    // dt dir theme
    g_free(path);
    path = g_build_filename(datadir, "themes", theme_css, NULL);
    if(!g_file_test(path, G_FILE_TEST_EXISTS))
    {
      // fallback to default theme
      g_free(path);
      path = g_build_filename(datadir, "themes", "darktable.css", NULL);
      dt_conf_set_string("ui_last/theme", "darktable");
    }
    else
      dt_conf_set_string("ui_last/theme", theme);
  }
  else
    dt_conf_set_string("ui_last/theme", theme);

  GError *error = NULL;

  GtkStyleProvider *themes_style_provider = GTK_STYLE_PROVIDER(gtk_css_provider_new());
  gtk_style_context_add_provider_for_screen
    (gdk_screen_get_default(), themes_style_provider, GTK_STYLE_PROVIDER_PRIORITY_USER + 1);

  usercsspath = g_build_filename(configdir, "user.css", NULL);

  gchar *path_uri = g_filename_to_uri(path, NULL, &error);
  if(path_uri == NULL)
    fprintf(stderr, "%s: could not convert path %s to URI. Error: %s\n", G_STRFUNC, path, error->message);

  gchar *usercsspath_uri = g_filename_to_uri(usercsspath, NULL, &error);
  if(usercsspath_uri == NULL)
    fprintf(stderr, "%s: could not convert path %s to URI. Error: %s\n", G_STRFUNC, usercsspath, error->message);

  gchar *themecss = NULL;
  if(dt_conf_get_bool("themes/usercss") && g_file_test(usercsspath, G_FILE_TEST_EXISTS))
  {
    themecss = g_strjoin(NULL, "@import url('", path_uri,
                                           "'); @import url('", usercsspath_uri, "');", NULL);
  }
  else
  {
    themecss = g_strjoin(NULL, "@import url('", path_uri, "');", NULL);
  }

  g_free(path_uri);
  g_free(usercsspath_uri);
  g_free(path);
  g_free(usercsspath);

  if(dt_conf_get_bool("ui/hide_tooltips"))
  {
    gchar *newcss = g_strjoin(NULL, themecss, " tooltip {opacity: 0; background: transparent;}", NULL);
    g_free(themecss);
    themecss = newcss;
  }

  if(!gtk_css_provider_load_from_data(GTK_CSS_PROVIDER(themes_style_provider), themecss, -1, &error))
  {
    fprintf(stderr, "%s: error parsing combined CSS %s: %s\n", G_STRFUNC, themecss, error->message);
    g_clear_error(&error);
  }

  g_free(themecss);

  g_object_unref(themes_style_provider);

  // setup the colors

  GdkRGBA *c = darktable.gui->colors;
  GtkWidget *main_window = dt_ui_main_window(darktable.gui->ui);
  GtkStyleContext *ctx = gtk_widget_get_style_context(main_window);

  c[DT_GUI_COLOR_BG] = (GdkRGBA){ 0.1333, 0.1333, 0.1333, 1.0 };

  struct color_init
  {
    const char *name;
    GdkRGBA default_col;
  } init[DT_GUI_COLOR_LAST] = {
    [DT_GUI_COLOR_DARKROOM_BG] = { "darkroom_bg_color", { .2, .2, .2, 1.0 } },
    [DT_GUI_COLOR_DARKROOM_PREVIEW_BG] = { "darkroom_preview_bg_color", { .1, .1, .1, 1.0 } },
    [DT_GUI_COLOR_LIGHTTABLE_BG] = { "lighttable_bg_color", { .2, .2, .2, 1.0 } },
    [DT_GUI_COLOR_LIGHTTABLE_PREVIEW_BG] = { "lighttable_preview_bg_color", { .1, .1, .1, 1.0 } },
    [DT_GUI_COLOR_LIGHTTABLE_FONT] = { "lighttable_bg_font_color", { .7, .7, .7, 1.0 } },
    [DT_GUI_COLOR_PRINT_BG] = { "print_bg_color", { .2, .2, .2, 1.0 } },
    [DT_GUI_COLOR_BRUSH_CURSOR] = { "brush_cursor", { 1., 1., 1., 0.9 } },
    [DT_GUI_COLOR_BRUSH_TRACE] = { "brush_trace", { 0., 0., 0., 0.8 } },
    [DT_GUI_COLOR_BUTTON_FG] = { "button_fg", { 0.7, 0.7, 0.7, 0.55 } },
    [DT_GUI_COLOR_THUMBNAIL_BG] = { "thumbnail_bg_color", { 0.4, 0.4, 0.4, 1.0 } },
    [DT_GUI_COLOR_THUMBNAIL_SELECTED_BG] = { "thumbnail_selected_bg_color", { 0.6, 0.6, 0.6, 1.0 } },
    [DT_GUI_COLOR_THUMBNAIL_HOVER_BG] = { "thumbnail_hover_bg_color", { 0.8, 0.8, 0.8, 1.0 } },
    [DT_GUI_COLOR_THUMBNAIL_OUTLINE] = { "thumbnail_outline_color", { 0.2, 0.2, 0.2, 1.0 } },
    [DT_GUI_COLOR_THUMBNAIL_SELECTED_OUTLINE] = { "thumbnail_selected_outline_color", { 0.4, 0.4, 0.4, 1.0 } },
    [DT_GUI_COLOR_THUMBNAIL_HOVER_OUTLINE] = { "thumbnail_hover_outline_color", { 0.6, 0.6, 0.6, 1.0 } },
    [DT_GUI_COLOR_THUMBNAIL_FONT] = { "thumbnail_font_color", { 0.425, 0.425, 0.425, 1.0 } },
    [DT_GUI_COLOR_THUMBNAIL_SELECTED_FONT] = { "thumbnail_selected_font_color", { 0.5, 0.5, 0.5, 1.0 } },
    [DT_GUI_COLOR_THUMBNAIL_HOVER_FONT] = { "thumbnail_hover_font_color", { 0.7, 0.7, 0.7, 1.0 } },
    [DT_GUI_COLOR_THUMBNAIL_BORDER] = { "thumbnail_border_color", { 0.1, 0.1, 0.1, 1.0 } },
    [DT_GUI_COLOR_THUMBNAIL_SELECTED_BORDER] = { "thumbnail_selected_border_color", { 0.9, 0.9, 0.9, 1.0 } },
    [DT_GUI_COLOR_FILMSTRIP_BG] = { "filmstrip_bg_color", { 0.2, 0.2, 0.2, 1.0 } },
    [DT_GUI_COLOR_TIMELINE_BG] = { "timeline_bg_color", { 0.4, 0.4, 0.4, 1.0 } },
    [DT_GUI_COLOR_TIMELINE_FG] = { "timeline_fg_color", { 0.8, 0.8, 0.8, 1.0 } },
    [DT_GUI_COLOR_TIMELINE_TEXT_BG] = { "timeline_text_bg_color", { 0., 0., 0., 0.8 } },
    [DT_GUI_COLOR_TIMELINE_TEXT_FG] = { "timeline_text_fg_color", { 1., 1., 1., 0.9 } },
    [DT_GUI_COLOR_CULLING_SELECTED_BORDER] = { "culling_selected_border_color", { 0.1, 0.1, 0.1, 1.0 } },
    [DT_GUI_COLOR_CULLING_FILMSTRIP_SELECTED_BORDER]
    = { "culling_filmstrip_selected_border_color", { 0.1, 0.1, 0.1, 1.0 } },
    [DT_GUI_COLOR_PREVIEW_HOVER_BORDER] = { "preview_hover_border_color", { 0.9, 0.9, 0.9, 1.0 } },
    [DT_GUI_COLOR_LOG_BG] = { "log_bg_color", { 0.1, 0.1, 0.1, 1.0 } },
    [DT_GUI_COLOR_LOG_FG] = { "log_fg_color", { 0.6, 0.6, 0.6, 1.0 } },
    [DT_GUI_COLOR_MAP_COUNT_SAME_LOC] = { "map_count_same_loc_color", { 1.0, 1.0, 1.0, 1.0 } },
    [DT_GUI_COLOR_MAP_COUNT_DIFF_LOC] = { "map_count_diff_loc_color", { 1.0, 0.85, 0.0, 1.0 } },
    [DT_GUI_COLOR_MAP_COUNT_BG] = { "map_count_bg_color", { 0.0, 0.0, 0.0, 1.0 } },
    [DT_GUI_COLOR_MAP_LOC_SHAPE_HIGH] = { "map_count_circle_color_h", { 1.0, 1.0, 0.8, 1.0 } },
    [DT_GUI_COLOR_MAP_LOC_SHAPE_LOW] = { "map_count_circle_color_l", { 0.0, 0.0, 0.0, 1.0 } },
    [DT_GUI_COLOR_MAP_LOC_SHAPE_DEF] = { "map_count_circle_color_d", { 1.0, 0.0, 0.0, 1.0 } },
    [DT_GUI_COLOR_RANGE_BG] = { "range_bg_color", { 1.0, 0.0, 0.0, 1.0 } },
    [DT_GUI_COLOR_RANGE_GRAPH] = { "range_graph_color", { 1.0, 0.0, 0.0, 1.0 } },
    [DT_GUI_COLOR_RANGE_SELECTION] = { "range_sel_color", { 1.0, 0.0, 0.0, 1.0 } },
    [DT_GUI_COLOR_RANGE_CURSOR] = { "range_cursor_color", { 1.0, 0.0, 0.0, 1.0 } },
    [DT_GUI_COLOR_RANGE_ICONS] = { "range_icon_color", { 1.0, 0.0, 0.0, 1.0 } },
  };

  // starting from 1 as DT_GUI_COLOR_BG is not part of this table
  for(int i = 1; i < DT_GUI_COLOR_LAST; i++)
  {
    if(!gtk_style_context_lookup_color(ctx, init[i].name, &c[i]))
    {
      c[i] = init[i].default_col;
    }
  }
}

GdkModifierType dt_key_modifier_state()
{
  guint state = 0;
  GdkWindow *window = gtk_widget_get_window(dt_ui_main_window(darktable.gui->ui));
  gdk_device_get_state(gdk_seat_get_pointer(gdk_display_get_default_seat(gdk_window_get_display(window))), window, NULL, &state);
  return state;

/* FIXME double check correct way of doing this (merge conflict with Input System NG 20210319)
  GdkKeymap *keymap = gdk_keymap_get_for_display(gdk_display_get_default());
  return gdk_keymap_get_modifier_state(keymap) & gdk_keymap_get_modifier_mask(keymap, GDK_MODIFIER_INTENT_DEFAULT_MOD_MASK);
*/
}

static void _notebook_size_callback(GtkNotebook *notebook, GdkRectangle *allocation, gpointer *data)
{
  const int n = gtk_notebook_get_n_pages(notebook);
  g_return_if_fail(n > 0);

  GtkRequestedSize *sizes = g_malloc_n(n, sizeof(GtkRequestedSize));

  for(int i = 0; i < n; i++)
  {
    sizes[i].data = gtk_notebook_get_tab_label(notebook, gtk_notebook_get_nth_page(notebook, i));
    sizes[i].minimum_size = 0;
    GtkRequisition natural_size;
    gtk_widget_get_preferred_size(sizes[i].data, NULL, &natural_size);
    sizes[i].natural_size = natural_size.width;
  }

  GtkAllocation first, last;
  gtk_widget_get_allocation(sizes[0].data, &first);
  gtk_widget_get_allocation(sizes[n - 1].data, &last);

  const gint total_space = last.x + last.width - first.x; // ignore tab padding; CSS sets padding for label

  if(total_space > 0)
  {
    gtk_distribute_natural_allocation(total_space, n, sizes);

    for(int i = 0; i < n; i++)
      gtk_widget_set_size_request(sizes[i].data, sizes[i].minimum_size, -1);

    gtk_widget_size_allocate(GTK_WIDGET(notebook), allocation);

    for(int i = 0; i < n; i++)
      gtk_widget_set_size_request(sizes[i].data, -1, -1);
  }

  g_free(sizes);
}

// GTK_STATE_FLAG_PRELIGHT does not seem to get set on the label on hover so
// state-flags-changed cannot update darktable.control->element for shortcut mapping
static gboolean _notebook_motion_notify_callback(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
  GtkAllocation notebook_alloc, label_alloc;
  gtk_widget_get_allocation(widget, &notebook_alloc);

  GtkNotebook *notebook = GTK_NOTEBOOK(widget);
  const int n = gtk_notebook_get_n_pages(notebook);
  for(int i = 0; i < n; i++)
  {
    gtk_widget_get_allocation(gtk_notebook_get_tab_label(notebook, gtk_notebook_get_nth_page(notebook, i)), &label_alloc);
    if(event->x + notebook_alloc.x < label_alloc.x + label_alloc.width)
    {
      darktable.control->element = i;
      break;
    }
  }

  return FALSE;
}

static float _action_process_tabs(gpointer target, dt_action_element_t element, dt_action_effect_t effect, float move_size)
{
  GtkNotebook *notebook = GTK_NOTEBOOK(target);
  if(!isnan(move_size))
  {
    switch(effect)
    {
    case DT_ACTION_EFFECT_ACTIVATE:
      gtk_notebook_set_current_page(notebook, element);
      break;
    case DT_ACTION_EFFECT_NEXT:
      gtk_notebook_next_page(notebook);
      break;
    case DT_ACTION_EFFECT_PREVIOUS:
      gtk_notebook_prev_page(notebook);
      break;
    default:
      fprintf(stderr, "[_action_process_tabs] unknown shortcut effect (%d) for tabs\n", effect);
      break;
    }
  }

  const int c = gtk_notebook_get_current_page(notebook);

  if(!isnan(move_size))
    dt_action_widget_toast(NULL, GTK_WIDGET(notebook),
                           gtk_notebook_get_tab_label_text(notebook, gtk_notebook_get_nth_page(notebook, c)));

  return -1 - c + (c == element ? DT_VALUE_PATTERN_ACTIVE : 0);
}

const gchar *dt_action_effect_tabs[]
  = { N_("activate"),
      N_("next"),
      N_("previous"),
      NULL };

static GtkNotebook *_current_notebook = NULL;
static dt_action_def_t *_current_action_def = NULL;

GtkNotebook *dt_ui_notebook_new(dt_action_def_t *def)
{
  _current_notebook = GTK_NOTEBOOK(gtk_notebook_new());
  if(!def->name)
  {
    _current_action_def = def;
    def->name = "tabs";
    def->process = _action_process_tabs;
  }

  return _current_notebook;
}

static gboolean _notebook_scroll_callback(GtkNotebook *notebook, GdkEventScroll *event, gpointer user_data)
{
  if(dt_gui_ignore_scroll(event)) return FALSE;

  int delta = 0;
  if(dt_gui_get_scroll_unit_delta(event, &delta) && delta)
    _action_process_tabs(notebook, DT_ACTION_EFFECT_DEFAULT_KEY,
                         delta < 0 ? DT_ACTION_EFFECT_NEXT : DT_ACTION_EFFECT_PREVIOUS, delta);

  return TRUE;
}


static void _reset_all_bauhaus(GtkWidget *w, gpointer module)
{
  if(GTK_IS_CONTAINER(w))
    gtk_container_foreach(GTK_CONTAINER(w), _reset_all_bauhaus, module);
  else if(DT_IS_BAUHAUS_WIDGET(w))
  {
    dt_bauhaus_widget_t *b = DT_BAUHAUS_WIDGET(w);
    *(dt_action_t **)module = b->module;

    dt_bauhaus_widget_reset(w);
  }
}

static gboolean _notebook_button_press_callback(GtkNotebook *notebook, GdkEventButton *event, gpointer user_data)
{
  if(event->type == GDK_2BUTTON_PRESS)
  {
    dt_action_t *module = NULL;

    GtkWidget *box = gtk_notebook_get_nth_page(notebook, gtk_notebook_get_current_page(notebook));
    ++darktable.gui->reset;
    _reset_all_bauhaus(box, &module);
    --darktable.gui->reset;
    if(module && module->type == DT_ACTION_TYPE_IOP_INSTANCE)
      dt_dev_add_history_item(darktable.develop, (dt_iop_module_t *)module, TRUE);
  }

  return FALSE;
}

GtkWidget *dt_ui_notebook_page(GtkNotebook *notebook, const char *text, const char *tooltip)
{
  if(notebook != _current_notebook)
  {
    _current_notebook = 0;
    _current_action_def = 0;
  }
  GtkWidget *label = gtk_label_new(_(text));
  GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  if(strlen(text) > 2)
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
  gtk_widget_set_tooltip_text(label, tooltip ? tooltip : _(text));
  gtk_widget_set_has_tooltip(GTK_WIDGET(notebook), FALSE);

  gint page_num = gtk_notebook_append_page(notebook, page, label);
  gtk_container_child_set(GTK_CONTAINER(notebook), page, "tab-expand", TRUE, "tab-fill", TRUE, NULL);
  if(page_num == 1 &&
     !g_signal_handler_find(G_OBJECT(notebook), G_SIGNAL_MATCH_FUNC, 0, 0, NULL, _notebook_size_callback, NULL))
  {
    g_signal_connect(G_OBJECT(notebook), "size-allocate", G_CALLBACK(_notebook_size_callback), NULL);
    g_signal_connect(G_OBJECT(notebook), "motion-notify-event", G_CALLBACK(_notebook_motion_notify_callback), NULL);
    g_signal_connect(G_OBJECT(notebook), "scroll-event", G_CALLBACK(_notebook_scroll_callback), NULL);
    g_signal_connect(G_OBJECT(notebook), "button-press-event", G_CALLBACK(_notebook_button_press_callback), NULL);
    gtk_widget_add_events(GTK_WIDGET(notebook), darktable.gui->scroll_mask);
  }
  if(_current_action_def)
  {
    dt_action_element_def_t *elements = calloc(page_num + 2, sizeof(dt_action_element_def_t));
    if(_current_action_def->elements)
      memcpy(elements, _current_action_def->elements, page_num * sizeof(dt_action_element_def_t));
    elements[page_num].name = text;
    elements[page_num].effects = dt_action_effect_tabs;
    if(_current_action_def->elements)
      free((void *)_current_action_def->elements);
    _current_action_def->elements = elements;
  }

  return page;
}

const dt_action_element_def_t _action_elements_tabs_all_rgb[]
  = { { N_("all"  ), dt_action_effect_tabs },
      { N_("red"  ), dt_action_effect_tabs },
      { N_("green"), dt_action_effect_tabs },
      { N_("blue" ), dt_action_effect_tabs },
      { NULL       , dt_action_effect_tabs } };

const dt_action_def_t dt_action_def_tabs_all_rgb
  = { N_("tabs"),
      _action_process_tabs,
      _action_elements_tabs_all_rgb };

const dt_action_def_t dt_action_def_tabs_rgb
  = { N_("tabs"),
      _action_process_tabs,
      _action_elements_tabs_all_rgb + 1 };

const dt_action_def_t dt_action_def_tabs_none
  = { N_("tabs"),
      _action_process_tabs,
      _action_elements_tabs_all_rgb + 4 };

static gint _get_container_row_heigth(GtkWidget *w)
{
  gint height = DT_PIXEL_APPLY_DPI(10);

  if(GTK_IS_TREE_VIEW(w))
  {
    gint row_height = 0;

    const gint num_columns = gtk_tree_view_get_n_columns(GTK_TREE_VIEW(w));
    for(int c = 0; c < num_columns; c++)
    {
      gint cell_height = 0;
      gtk_tree_view_column_cell_get_size(gtk_tree_view_get_column(GTK_TREE_VIEW(w), c),
                                        NULL, NULL, NULL, NULL, &cell_height);
      if(cell_height > row_height) row_height = cell_height;
    }
    GValue separation = { G_TYPE_INT };
    gtk_widget_style_get_property(w, "vertical-separator", &separation);

    if(row_height > 0) height = row_height + g_value_get_int(&separation);
  }
  else if(GTK_IS_TEXT_VIEW(w))
  {
    PangoLayout *layout = gtk_widget_create_pango_layout(w, "X");
    pango_layout_get_pixel_size(layout, NULL, &height);
    g_object_unref(layout);
  }
  else
  {
    GtkWidget *child = dt_gui_container_first_child(GTK_CONTAINER(w));
    if(child)
    {
      height = gtk_widget_get_allocated_height(child);
    }
  }

  return height;
}

static gboolean _scroll_wrap_resize(GtkWidget *w, void *cr, const char *config_str)
{
  GtkWidget *sw = gtk_widget_get_parent(w);
  if(GTK_IS_VIEWPORT(sw)) sw = gtk_widget_get_parent(sw);

  const gint increment = _get_container_row_heigth(w);

  gint height = dt_conf_get_int(config_str);

  const gint max_height = DT_PIXEL_APPLY_DPI(1000);

  height = (height < 1) ? 1 : (height > max_height) ? max_height : height;

  dt_conf_set_int(config_str, height);

  gint content_height;
  gtk_widget_get_preferred_height(w, NULL, &content_height);

  const gint min_height = - gtk_scrolled_window_get_min_content_height(GTK_SCROLLED_WINDOW(sw));

  if(content_height < min_height) content_height = min_height;

  if(height > content_height) height = content_height;

  height += increment - 1;
  height -= height % increment;

  GtkBorder padding;
  gtk_style_context_get_padding(gtk_widget_get_style_context(sw),
                                gtk_widget_get_state_flags(sw),
                                &padding);

  gint old_height = 0;
  gtk_widget_get_size_request(sw, NULL, &old_height);
  const gint new_height = height + padding.top + padding.bottom + (GTK_IS_TEXT_VIEW(w) ? 2 : 0);
  if(new_height != old_height)
  {
    gtk_widget_set_size_request(sw, -1, new_height);

    GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(sw));
    gint value = gtk_adjustment_get_value(adj);
    value -= value % increment;
    gtk_adjustment_set_value(adj, value);
  }

  return FALSE;
}

static gboolean _scroll_wrap_scroll(GtkScrolledWindow *sw, GdkEventScroll *event, const char *config_str)
{
  GtkWidget *w = gtk_bin_get_child(GTK_BIN(sw));
  if(GTK_IS_VIEWPORT(w)) w = gtk_bin_get_child(GTK_BIN(w));

  const gint increment = _get_container_row_heigth(w);

  int delta_y = 0;

  dt_gui_get_scroll_unit_deltas(event, NULL, &delta_y);

  if(dt_modifier_is(event->state, GDK_CONTROL_MASK))
  {
    const gint new_size = dt_conf_get_int(config_str) + increment*delta_y;

    dt_toast_log("%d", 1 + new_size / increment);

    dt_conf_set_int(config_str, new_size);

    _scroll_wrap_resize(w, NULL, config_str);
  }
  else
  {
    GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(sw);

    const gint before = gtk_adjustment_get_value(adj);
    gint value = before + increment*delta_y;
    value -= value % increment;
    gtk_adjustment_set_value(adj, value);
    const gint after = gtk_adjustment_get_value(adj);
    if(delta_y && after == before)
      gtk_propagate_event(gtk_widget_get_parent(GTK_WIDGET(sw)), (GdkEvent*)event);
  }

  return TRUE;
}

GtkWidget *dt_ui_scroll_wrap(GtkWidget *w, gint min_size, char *config_str)
{
  GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(sw), - DT_PIXEL_APPLY_DPI(min_size));
  g_signal_connect(G_OBJECT(sw), "scroll-event", G_CALLBACK(_scroll_wrap_scroll), config_str);
  g_signal_connect(G_OBJECT(w), "draw", G_CALLBACK(_scroll_wrap_resize), config_str);
  gtk_container_add(GTK_CONTAINER(sw), w);

  return sw;
}

gboolean dt_gui_container_has_children(GtkContainer *container)
{
  g_return_val_if_fail(GTK_IS_CONTAINER(container), FALSE);
  GList *children = gtk_container_get_children(container);
  gboolean has_children = children != NULL;
  g_list_free(children);
  return has_children;
}

int dt_gui_container_num_children(GtkContainer *container)
{
  g_return_val_if_fail(GTK_IS_CONTAINER(container), FALSE);
  GList *children = gtk_container_get_children(container);
  int num_children = g_list_length(children);
  g_list_free(children);
  return num_children;
}

GtkWidget *dt_gui_container_first_child(GtkContainer *container)
{
  g_return_val_if_fail(GTK_IS_CONTAINER(container), NULL);
  GList *children = gtk_container_get_children(container);
  GtkWidget *child = children ? (GtkWidget*)children->data : NULL;
  g_list_free(children);
  return child;
}

GtkWidget *dt_gui_container_nth_child(GtkContainer *container, int which)
{
  g_return_val_if_fail(GTK_IS_CONTAINER(container), NULL);
  GList *children = gtk_container_get_children(container);
  GtkWidget *child = (GtkWidget*)g_list_nth_data(children, which);
  g_list_free(children);
  return child;
}

static void _remove_child(GtkWidget *widget, gpointer data)
{
  gtk_container_remove((GtkContainer*)data, widget);
}

void dt_gui_container_remove_children(GtkContainer *container)
{
  g_return_if_fail(GTK_IS_CONTAINER(container));
  gtk_container_foreach(container, _remove_child, container);
}

static void _delete_child(GtkWidget *widget, gpointer data)
{
  (void)data;  // avoid unreferenced-parameter warning
  gtk_widget_destroy(widget);
}

void dt_gui_container_destroy_children(GtkContainer *container)
{
  g_return_if_fail(GTK_IS_CONTAINER(container));
  gtk_container_foreach(container, _delete_child, NULL);
}

void dt_gui_menu_popup(GtkMenu *menu, GtkWidget *button, GdkGravity widget_anchor, GdkGravity menu_anchor)
{
  gtk_widget_show_all(GTK_WIDGET(menu));

  GdkEvent *event = gtk_get_current_event();
  if(button && event)
  {
    gtk_menu_popup_at_widget(menu, button, widget_anchor, menu_anchor, event);
  }
  else
  {
    if(!event)
    {
      event = gdk_event_new(GDK_BUTTON_PRESS);
      event->button.device = gdk_seat_get_pointer(gdk_display_get_default_seat(gdk_display_get_default()));
      event->button.window = gtk_widget_get_window(GTK_WIDGET(darktable.gui->ui->main_window));
      g_object_ref(event->button.window);
    }

    gtk_menu_popup_at_pointer(menu, event);
  }
  gdk_event_free(event);
}

// draw rounded rectangle
void dt_gui_draw_rounded_rectangle(cairo_t *cr, float width, float height, float x, float y)
{
  const float radius = height / 5.0f;
  const float degrees = M_PI / 180.0;
  cairo_new_sub_path(cr);
  cairo_arc(cr, x + width - radius, y + radius, radius, -90 * degrees, 0 * degrees);
  cairo_arc(cr, x + width - radius, y + height - radius, radius, 0 * degrees, 90 * degrees);
  cairo_arc(cr, x + radius, y + height - radius, radius, 90 * degrees, 180 * degrees);
  cairo_arc(cr, x + radius, y + radius, radius, 180 * degrees, 270 * degrees);
  cairo_close_path(cr);
  cairo_fill(cr);
}

gboolean dt_gui_search_start(GtkWidget *widget, GdkEventKey *event, GtkSearchEntry *entry)
{
  if(gtk_search_entry_handle_event(entry, (GdkEvent *)event))
  {
    gtk_entry_grab_focus_without_selecting(GTK_ENTRY(entry));
    return TRUE;
  }

  return FALSE;
}

void dt_gui_search_stop(GtkSearchEntry *entry, GtkWidget *widget)
{
  gtk_widget_grab_focus(widget);

  gtk_entry_set_text(GTK_ENTRY(entry), "");

  if(GTK_IS_TREE_VIEW(widget))
  {
    GtkTreePath *path = NULL;
    gtk_tree_view_get_cursor(GTK_TREE_VIEW(widget), &path, NULL);
    gtk_tree_selection_select_path(gtk_tree_view_get_selection(GTK_TREE_VIEW(widget)), path);
    gtk_tree_path_free(path);
  }
}

static void _collapse_button_changed(GtkDarktableToggleButton *widget, gpointer user_data)
{
  dt_gui_collapsible_section_t *cs = (dt_gui_collapsible_section_t *)user_data;

  const gboolean active = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(cs->toggle));
  dtgtk_expander_set_expanded(DTGTK_EXPANDER(cs->expander), active);
  dtgtk_togglebutton_set_paint(DTGTK_TOGGLEBUTTON(cs->toggle), dtgtk_cairo_paint_solid_arrow,
                               (active ? CPF_DIRECTION_DOWN : CPF_DIRECTION_LEFT), NULL);
  dt_conf_set_bool(cs->confname, active);
}

static void _collapse_expander_click(GtkWidget *widget, GdkEventButton *e, gpointer user_data)
{
  if(e->type == GDK_2BUTTON_PRESS || e->type == GDK_3BUTTON_PRESS) return;

  dt_gui_collapsible_section_t *cs = (dt_gui_collapsible_section_t *)user_data;

  const gboolean active = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(cs->toggle));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(cs->toggle), !active);
}

void dt_gui_update_collapsible_section(dt_gui_collapsible_section_t *cs)
{
  const gboolean active = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(cs->toggle));
  dtgtk_togglebutton_set_paint(DTGTK_TOGGLEBUTTON(cs->toggle), dtgtk_cairo_paint_solid_arrow,
                               (active ? CPF_DIRECTION_DOWN : CPF_DIRECTION_LEFT), NULL);
  dtgtk_expander_set_expanded(DTGTK_EXPANDER(cs->expander), active);

  gtk_widget_set_visible(GTK_WIDGET(cs->container), active);
}

void dt_gui_hide_collapsible_section(dt_gui_collapsible_section_t *cs)
{
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(cs->toggle), FALSE);
  gtk_widget_hide(GTK_WIDGET(cs->container));
}

void dt_gui_new_collapsible_section(dt_gui_collapsible_section_t *cs,
                                    const char *confname, const char *label, GtkBox *parent)
{
  const gboolean expanded = dt_conf_get_bool(confname);

  cs->confname = g_strdup(confname);
  cs->parent = parent;

  // collapsible section header
  GtkWidget *destdisp_head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_BAUHAUS_SPACE);
  GtkWidget *header_evb = gtk_event_box_new();
  GtkWidget *destdisp = dt_ui_section_label_new(label);
  dt_gui_add_class(destdisp_head, "dt_section_expander");
  gtk_container_add(GTK_CONTAINER(header_evb), destdisp);

  cs->toggle = dtgtk_togglebutton_new(dtgtk_cairo_paint_solid_arrow,
                                      (expanded ? CPF_DIRECTION_DOWN : CPF_DIRECTION_LEFT), NULL);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(cs->toggle), expanded);
  dt_gui_add_class(cs->toggle, "dt_ignore_fg_state");
  dt_gui_add_class(cs->toggle, "dt_transparent_background");

  cs->container = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE));
  gtk_widget_set_name(GTK_WIDGET(cs->container), "collapsible");
  gtk_box_pack_start(GTK_BOX(destdisp_head), header_evb, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(destdisp_head), cs->toggle, FALSE, FALSE, 0);

  cs->expander = dtgtk_expander_new(destdisp_head, GTK_WIDGET(cs->container));
  gtk_box_pack_end(cs->parent, cs->expander, FALSE, FALSE, 0);
  dtgtk_expander_set_expanded(DTGTK_EXPANDER(cs->expander), expanded);
  gtk_widget_set_name(cs->expander, "collapse-block");

  g_signal_connect(G_OBJECT(cs->toggle), "toggled",
                   G_CALLBACK(_collapse_button_changed), cs);

  g_signal_connect(G_OBJECT(header_evb), "button-release-event",
                   G_CALLBACK(_collapse_expander_click), cs);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
