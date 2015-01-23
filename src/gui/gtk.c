
/*
    This file is part of darktable,
    copyright (c) 2009--2014 johannes hanika, henrik andersson

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
#include "common/image.h"
#include "common/image_cache.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "dtgtk/button.h"
#include "gui/accelerators.h"
#include "dtgtk/sidepanel.h"
#include "gui/gtk.h"

#include "gui/presets.h"
#include "control/control.h"
#include "control/jobs.h"
#include "control/conf.h"
#include "control/signal.h"
#include "views/view.h"
#include "common/styles.h"

#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <math.h>
#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#ifdef MAC_INTEGRATION
#include <gtkosxapplication.h>
#endif
#ifdef GDK_WINDOWING_QUARTZ
#include <Carbon/Carbon.h>
#include <ApplicationServices/ApplicationServices.h>
#include <CoreServices/CoreServices.h>
#include "osx/osx.h"
#endif
#include <pthread.h>


/*
 * NEW UI API
 */

#define DT_UI_PANEL_MODULE_SPACING 3

const char *_ui_panel_config_names[]
    = { "header", "toolbar_top", "toolbar_bottom", "left", "right", "bottom" };

typedef struct dt_ui_t
{
  /* container widgets */
  GtkWidget *containers[DT_UI_CONTAINER_SIZE];

  /* border widgets */
  GtkWidget *borders[DT_UI_BORDER_SIZE];

  /* panel widgets */
  GtkWidget *panels[DT_UI_PANEL_SIZE];

  /* center widget */
  GtkWidget *center;
  /* main widget */
  GtkWidget *main_window;
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

/*
 * OLD UI API
 */
static void init_widgets(dt_gui_gtk_t *gui);

static void init_main_table(GtkWidget *container);

static void key_accel_changed(GtkAccelMap *object, gchar *accel_path, guint accel_key,
                              GdkModifierType accel_mods, gpointer user_data)
{
  char path[256];

  // Updating all the stored accelerator keys/mods for key_pressed shortcuts

  dt_accel_path_view(path, sizeof(path), "filmstrip", "scroll forward");
  gtk_accel_map_lookup_entry(path, &darktable.control->accels.filmstrip_forward);
  dt_accel_path_view(path, sizeof(path), "filmstrip", "scroll back");
  gtk_accel_map_lookup_entry(path, &darktable.control->accels.filmstrip_back);

  // slideshow
  dt_accel_path_view(path, sizeof(path), "slideshow", "start and stop");
  gtk_accel_map_lookup_entry(path, &darktable.control->accels.slideshow_start);
  // Lighttable
  dt_accel_path_view(path, sizeof(path), "lighttable", "scroll up");
  gtk_accel_map_lookup_entry(path, &darktable.control->accels.lighttable_up);
  dt_accel_path_view(path, sizeof(path), "lighttable", "scroll down");
  gtk_accel_map_lookup_entry(path, &darktable.control->accels.lighttable_down);
  dt_accel_path_view(path, sizeof(path), "lighttable", "scroll left");
  gtk_accel_map_lookup_entry(path, &darktable.control->accels.lighttable_left);
  dt_accel_path_view(path, sizeof(path), "lighttable", "scroll right");
  gtk_accel_map_lookup_entry(path, &darktable.control->accels.lighttable_right);
  dt_accel_path_view(path, sizeof(path), "lighttable", "scroll center");
  gtk_accel_map_lookup_entry(path, &darktable.control->accels.lighttable_center);
  dt_accel_path_view(path, sizeof(path), "lighttable", "preview");
  gtk_accel_map_lookup_entry(path, &darktable.control->accels.lighttable_preview);
  dt_accel_path_view(path, sizeof(path), "lighttable", "preview with focus detection");
  gtk_accel_map_lookup_entry(path, &darktable.control->accels.lighttable_preview_display_focus);
  dt_accel_path_view(path, sizeof(path), "lighttable", "sticky preview");
  gtk_accel_map_lookup_entry(path, &darktable.control->accels.lighttable_preview_sticky);
  dt_accel_path_view(path, sizeof(path), "lighttable", "sticky preview with focus detection");
  gtk_accel_map_lookup_entry(path, &darktable.control->accels.lighttable_preview_sticky_focus);
  dt_accel_path_view(path, sizeof(path), "lighttable", "exit sticky preview");
  gtk_accel_map_lookup_entry(path, &darktable.control->accels.lighttable_preview_sticky_exit);
  // darkroom
  dt_accel_path_view(path, sizeof(path), "darkroom", "full preview");
  gtk_accel_map_lookup_entry(path, &darktable.control->accels.darkroom_preview);


  // Global
  dt_accel_path_global(path, sizeof(path), "toggle side borders");
  gtk_accel_map_lookup_entry(path, &darktable.control->accels.global_sideborders);

  dt_accel_path_global(path, sizeof(path), "toggle header");
  gtk_accel_map_lookup_entry(path, &darktable.control->accels.global_header);
}

static gboolean fullscreen_key_accel_callback(GtkAccelGroup *accel_group, GObject *acceleratable,
                                              guint keyval, GdkModifierType modifier, gpointer data)
{
  GtkWidget *widget;
  int fullscreen;

  if(data)
  {
    widget = dt_ui_main_window(darktable.gui->ui);
    fullscreen = dt_conf_get_bool("ui_last/fullscreen");
    if(fullscreen)
      gtk_window_unfullscreen(GTK_WINDOW(widget));
    else
      gtk_window_fullscreen(GTK_WINDOW(widget));
    fullscreen ^= 1;
    dt_conf_set_bool("ui_last/fullscreen", fullscreen);
    dt_dev_invalidate(darktable.develop);
  }
  else
  {
    widget = dt_ui_main_window(darktable.gui->ui);
    gtk_window_unfullscreen(GTK_WINDOW(widget));
    fullscreen = 0;
    dt_conf_set_bool("ui_last/fullscreen", fullscreen);
    dt_dev_invalidate(darktable.develop);
  }

  /* redraw center view */
  gtk_widget_queue_draw(dt_ui_center(darktable.gui->ui));
#ifdef __APPLE__
  // workaround for GTK Quartz backend bug
  gtk_window_set_title(GTK_WINDOW(widget), "Darktable");
#endif
  return TRUE;
}

static gboolean view_switch_key_accel_callback(GtkAccelGroup *accel_group, GObject *acceleratable,
                                               guint keyval, GdkModifierType modifier, gpointer data)
{
  dt_ctl_switch_mode();
  gtk_widget_queue_draw(dt_ui_center(darktable.gui->ui));
  return TRUE;
}

static gboolean borders_button_pressed(GtkWidget *w, GdkEventButton *event, gpointer user_data)
{
  dt_ui_t *ui = (dt_ui_t *)user_data;
  const dt_view_t *cv = dt_view_manager_get_current_view(darktable.view_manager);
  char key[512];


  int which = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(w), "border"));
  switch(which)
  {
    case 0: // left border
    {
      g_snprintf(key, sizeof(key), "%s/ui/%s_visible", cv->module_name,
                 _ui_panel_config_names[DT_UI_PANEL_LEFT]);
      dt_ui_panel_show(ui, DT_UI_PANEL_LEFT, !dt_conf_get_bool(key), TRUE);
    }
    break;

    case 1: // right border
    {
      g_snprintf(key, sizeof(key), "%s/ui/%s_visible", cv->module_name,
                 _ui_panel_config_names[DT_UI_PANEL_RIGHT]);
      dt_ui_panel_show(ui, DT_UI_PANEL_RIGHT, !dt_conf_get_bool(key), TRUE);
    }
    break;

    case 2: // top border
    {
      g_snprintf(key, sizeof(key), "%s/ui/%s_visible", cv->module_name,
                 _ui_panel_config_names[DT_UI_PANEL_CENTER_TOP]);
      gboolean show = !dt_conf_get_bool(key);
      dt_ui_panel_show(ui, DT_UI_PANEL_CENTER_TOP, show, TRUE);

      /* special case show header */
      g_snprintf(key, sizeof(key), "%s/ui/show_header", cv->module_name);
      if(dt_conf_get_bool(key)) dt_ui_panel_show(ui, DT_UI_PANEL_TOP, show, TRUE);
    }
    break;

    case 4: // bottom border
    default:
    {
      g_snprintf(key, sizeof(key), "%s/ui/%s_visible", cv->module_name,
                 _ui_panel_config_names[DT_UI_PANEL_CENTER_BOTTOM]);
      gboolean show = !dt_conf_get_bool(key);
      dt_ui_panel_show(ui, DT_UI_PANEL_CENTER_BOTTOM, show, TRUE);
      dt_ui_panel_show(ui, DT_UI_PANEL_BOTTOM, show, TRUE);
    }
    break;
  }

  gtk_widget_queue_draw(w);

  return TRUE;
}

static gboolean _widget_focus_in_block_key_accelerators(GtkWidget *widget, GdkEventFocus *event, gpointer data)
{
  dt_control_key_accelerators_off(darktable.control);
  return FALSE;
}

static gboolean _widget_focus_out_unblock_key_accelerators(GtkWidget *widget, GdkEventFocus *event,
                                                           gpointer data)
{
  dt_control_key_accelerators_on(darktable.control);
  return FALSE;
}

void dt_gui_key_accel_block_on_focus_disconnect(GtkWidget *w)
{
  g_signal_handlers_disconnect_by_func(G_OBJECT(w), _widget_focus_in_block_key_accelerators, (gpointer)w);
  g_signal_handlers_disconnect_by_func(G_OBJECT(w), _widget_focus_out_unblock_key_accelerators, (gpointer)w);
}

void dt_gui_key_accel_block_on_focus_connect(GtkWidget *w)
{
  /* first off add focus change event mask */
  gtk_widget_add_events(w, GDK_FOCUS_CHANGE_MASK);

  /* connect the signals */
  g_signal_connect(G_OBJECT(w), "focus-in-event", G_CALLBACK(_widget_focus_in_block_key_accelerators),
                   (gpointer)w);
  g_signal_connect(G_OBJECT(w), "focus-out-event", G_CALLBACK(_widget_focus_out_unblock_key_accelerators),
                   (gpointer)w);
}

static gboolean draw_borders(GtkWidget *widget, cairo_t *crf, gpointer user_data)
{
  // draw arrows on borders
  if(!dt_control_running()) return TRUE;
  int which = GPOINTER_TO_INT(user_data);
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  float width = allocation.width, height = allocation.height;
  cairo_surface_t *cst = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);

  GdkRGBA color;
  GtkStyleContext *context = gtk_widget_get_style_context(widget);
  gboolean color_found = gtk_style_context_lookup_color (context, "selected_bg_color", &color);
  if(!color_found)
  {
    color.red = 1.0;
    color.green = 0.0;
    color.blue = 0.0;
    color.alpha = 1.0;
  }
  gdk_cairo_set_source_rgba(cr, &color);
  cairo_paint(cr);

  // draw scrollbar indicators
  int v = darktable.view_manager->current_view;
  dt_view_t *view = NULL;
  if(v >= 0 && v < darktable.view_manager->num_views) view = darktable.view_manager->view + v;
  color_found = gtk_style_context_lookup_color (context, "bg_color", &color);
  if(!color_found)
  {
    color.red = 1.0;
    color.green = 0.0;
    color.blue = 0.0;
    color.alpha = 1.0;
  }
  gdk_cairo_set_source_rgba(cr, &color);
  const float border = 0.3;
  if(!view)
    cairo_paint(cr);
  else
  {
    switch(which)
    {
      case 0:
      case 1: // left, right: vertical
        cairo_rectangle(cr, 0.0, view->vscroll_pos / view->vscroll_size * height, width,
                        view->vscroll_viewport_size / view->vscroll_size * height);
        break;
      default: // bottom, top: horizontal
        cairo_rectangle(cr, view->hscroll_pos / view->hscroll_size * width, 0.0,
                        view->hscroll_viewport_size / view->hscroll_size * width, height);
        break;
    }
    cairo_fill(cr);
    switch(which)
    {
      case 0:
        cairo_rectangle(cr, (1.0 - border) * width, 0.0, border * width, height);
        break;
      case 1:
        cairo_rectangle(cr, 0.0, 0.0, border * width, height);
        break;
      case 2:
        cairo_rectangle(cr, (1.0 - border) * height, (1.0 - border) * height,
                        width - 2 * (1.0 - border) * height, border * height);
        break;
      default:
        cairo_rectangle(cr, (1.0 - border) * height, 0.0, width - 2 * (1.0 - border) * height,
                        border * height);
        break;
    }
    cairo_fill(cr);
  }

  // draw gui arrows.
  color_found = gtk_style_context_lookup_color (context, "fg_color", &color);
  if(!color_found)
  {
    color.red = 1.0;
    color.green = 0.0;
    color.blue = 0.0;
    color.alpha = 1.0;
  }
  gdk_cairo_set_source_rgba(cr, &color);

  switch(which)
  {
    case 0: // left
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
    case 1: // right
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
    case 2: // top
      if(dt_ui_panel_visible(darktable.gui->ui, DT_UI_PANEL_CENTER_TOP))
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
      if(dt_ui_panel_visible(darktable.gui->ui, DT_UI_PANEL_CENTER_BOTTOM))
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

static gboolean draw(GtkWidget *da, cairo_t *cr, gpointer user_data)
{
  dt_control_expose(NULL);
  if(darktable.gui->surface)
  {
    cairo_set_source_surface(cr, darktable.gui->surface, 0, 0);
    cairo_paint(cr);
  }

  if(darktable.lib->proxy.colorpicker.module)
  {
    darktable.lib->proxy.colorpicker.update_panel(darktable.lib->proxy.colorpicker.module);
    darktable.lib->proxy.colorpicker.update_samples(darktable.lib->proxy.colorpicker.module);
  }

  // test quit cond (thread safe, 2nd pass)
  if(!dt_control_running())
  {
    dt_cleanup();
    gtk_main_quit();
  }
  return TRUE;
}

static gboolean scrolled(GtkWidget *widget, GdkEventScroll *event, gpointer user_data)
{
  dt_view_manager_scrolled(darktable.view_manager, event->x, event->y, event->direction == GDK_SCROLL_UP,
                           event->state & 0xf);
  gtk_widget_queue_draw(widget);
  return TRUE;
}

static gboolean borders_scrolled(GtkWidget *widget, GdkEventScroll *event, gpointer user_data)
{
  dt_view_manager_border_scrolled(darktable.view_manager, event->x, event->y, GPOINTER_TO_INT(user_data),
                                  event->direction == GDK_SCROLL_UP);
  gtk_widget_queue_draw(widget);
  return TRUE;
}

void dt_gui_gtk_quit()
{
  GtkWindow *win = GTK_WINDOW(dt_ui_main_window(darktable.gui->ui));
  gtk_window_iconify(win);

  GtkWidget *widget;
  widget = darktable.gui->widgets.left_border;
  g_signal_handlers_block_by_func(widget, draw_borders, GINT_TO_POINTER(0));
  widget = darktable.gui->widgets.right_border;
  g_signal_handlers_block_by_func(widget, draw_borders, GINT_TO_POINTER(1));
  widget = darktable.gui->widgets.top_border;
  g_signal_handlers_block_by_func(widget, draw_borders, GINT_TO_POINTER(2));
  widget = darktable.gui->widgets.bottom_border;
  g_signal_handlers_block_by_func(widget, draw_borders, GINT_TO_POINTER(3));
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

static gboolean _gui_switch_view_key_accel_callback(GtkAccelGroup *accel_group, GObject *acceleratable,
                                                    guint keyval, GdkModifierType modifier, gpointer p)
{
  int view = GPOINTER_TO_INT(p);
  dt_control_gui_mode_t mode = DT_MODE_NONE;
  /* do some setup before switch view*/
  switch(view)
  {
#ifdef HAVE_GPHOTO2
    case DT_GUI_VIEW_SWITCH_TO_TETHERING:
      mode = DT_CAPTURE;
      break;
#endif

    case DT_GUI_VIEW_SWITCH_TO_DARKROOM:
      mode = DT_DEVELOP;
      break;

    case DT_GUI_VIEW_SWITCH_TO_LIBRARY:
      mode = DT_LIBRARY;
      break;

#ifdef HAVE_MAP
    case DT_GUI_VIEW_SWITCH_TO_MAP:
      mode = DT_MAP;
      break;
#endif
    case DT_GUI_VIEW_SWITCH_TO_SLIDESHOW:
      mode = DT_SLIDESHOW;
      break;
  }

  /* try switch to mode */
  if(mode != DT_MODE_NONE) dt_ctl_switch_mode_to(mode);
  return TRUE;
}

static gboolean quit_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                              GdkModifierType modifier)
{
  dt_control_quit();
  return TRUE; // for the sake of completeness ...
}

#ifdef MAC_INTEGRATION
#ifdef GTK_TYPE_OSX_APPLICATION
static gboolean osx_quit_callback(GtkOSXApplication *OSXapp, gpointer user_data)
#else
static gboolean osx_quit_callback(GtkosxApplication *OSXapp, gpointer user_data)
#endif
{
  GList *windows, *window;
  windows = gtk_window_list_toplevels();
  for(window = g_list_first(windows); window != NULL; window = g_list_next(window))
    if(gtk_window_get_modal(GTK_WINDOW(window->data)) && gtk_widget_get_visible(GTK_WIDGET(window->data)))
      break;
  if(window == NULL) dt_control_quit();
  g_list_free(windows);
  return TRUE;
}

#ifdef GTK_TYPE_OSX_APPLICATION
static gboolean osx_openfile_callback(GtkOSXApplication *OSXapp, gchar *path, gpointer user_data)
#else
static gboolean osx_openfile_callback(GtkosxApplication *OSXapp, gchar *path, gpointer user_data)
#endif
{
  return dt_load_from_string(path, FALSE) == 0 ? FALSE : TRUE;
}
#endif

static gboolean configure(GtkWidget *da, GdkEventConfigure *event, gpointer user_data)
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
    dt_ctl_set_display_profile(); // maybe we are on another screen now with > 50% of the area
  }
  oldw = event->width;
  oldh = event->height;

  return dt_control_configure(da, event, user_data);
}

static gboolean window_configure(GtkWidget *da, GdkEvent *event, gpointer user_data)
{
  static int oldx = 0;
  static int oldy = 0;
  if(oldx != event->configure.x || oldy != event->configure.y)
  {
    dt_ctl_set_display_profile(); // maybe we are on another screen now with > 50% of the area
    oldx = event->configure.x;
    oldy = event->configure.y;
  }
  return FALSE;
}

static gboolean key_pressed_override(GtkWidget *w, GdkEventKey *event, gpointer user_data)
{
  return dt_control_key_pressed_override(event->keyval, event->state & KEY_STATE_MASK);
}

static gboolean key_pressed(GtkWidget *w, GdkEventKey *event, gpointer user_data)
{
  return dt_control_key_pressed(event->keyval, event->state & KEY_STATE_MASK);
}

static gboolean key_released(GtkWidget *w, GdkEventKey *event, gpointer user_data)
{
  return dt_control_key_released(event->keyval, event->state & KEY_STATE_MASK);
}

static gboolean button_pressed(GtkWidget *w, GdkEventButton *event, gpointer user_data)
{
  double pressure = 1.0;
  if(gdk_device_get_source(event->device) == GDK_SOURCE_PEN)
  {
    gdouble axes[gdk_device_get_n_axes(event->device)];
    gdk_device_get_state(event->device, gtk_widget_get_window(w), axes, NULL);
    gdk_device_get_axis(event->device, axes, GDK_AXIS_PRESSURE, &pressure);
  }
  dt_control_button_pressed(event->x, event->y, pressure, event->button, event->type, event->state & 0xf);
  gtk_widget_grab_focus(w);
  gtk_widget_queue_draw(w);
  return TRUE;
}

static gboolean button_released(GtkWidget *w, GdkEventButton *event, gpointer user_data)
{
  dt_control_button_released(event->x, event->y, event->button, event->state & 0xf);
  gtk_widget_queue_draw(w);
  return TRUE;
}

static gboolean mouse_moved(GtkWidget *w, GdkEventMotion *event, gpointer user_data)
{
  double pressure = 1.0;
  if(gdk_device_get_source(event->device) == GDK_SOURCE_PEN)
  {
    gdouble axes[gdk_device_get_n_axes(event->device)];
    gdk_device_get_state(event->device, gtk_widget_get_window(w), axes, NULL);
    gdk_device_get_axis(event->device, axes, GDK_AXIS_PRESSURE, &pressure);
  }
  dt_control_mouse_moved(event->x, event->y, pressure, event->state & 0xf);
  gint x, y;
  gdk_window_get_device_position(event->window,
                                 gdk_device_manager_get_client_pointer(
                                     gdk_display_get_device_manager(gdk_window_get_display(event->window))),
                                 &x, &y, NULL);
  return FALSE;
}

static gboolean center_leave(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  dt_control_mouse_leave();
  return TRUE;
}

static gboolean center_enter(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  dt_control_mouse_enter();
  return TRUE;
}

int dt_gui_gtk_init(dt_gui_gtk_t *gui, int argc, char *argv[])
{
  /* lets zero mem */
  memset(gui, 0, sizeof(dt_gui_gtk_t));

  // unset gtk rc from kde:
  char path[PATH_MAX] = { 0 }, datadir[PATH_MAX] = { 0 }, configdir[PATH_MAX] = { 0 };
  dt_loc_get_datadir(datadir, sizeof(datadir));
  dt_loc_get_user_config_dir(configdir, sizeof(configdir));

  g_snprintf(gui->gtkrc, sizeof(gui->gtkrc), "%s/darktable.css", configdir);

  if(!g_file_test(gui->gtkrc, G_FILE_TEST_EXISTS))
    g_snprintf(gui->gtkrc, sizeof(gui->gtkrc), "%s/darktable.css", datadir);

#if !GLIB_CHECK_VERSION(2, 32, 0)
  if(!g_thread_supported()) g_thread_init(NULL);
#endif

  gdk_threads_init();

  gdk_threads_enter();

  gtk_init(&argc, &argv);

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
  g_signal_connect(G_OBJECT(OSXApp), "NSApplicationBlockTermination", G_CALLBACK(osx_quit_callback), NULL);
  g_signal_connect(G_OBJECT(OSXApp), "NSApplicationOpenFile", G_CALLBACK(osx_openfile_callback), NULL);
#endif

  GtkWidget *widget;
  gui->ui = dt_ui_initialize(argc, argv);
  gui->surface = NULL;
  gui->center_tooltip = 0;
  gui->grouping = dt_conf_get_bool("ui_last/grouping");
  gui->expanded_group_id = -1;
  gui->show_overlays = dt_conf_get_bool("lighttable/ui/expose_statuses");
  gui->presets_popup_menu = NULL;
  gui->last_preset = NULL;

  // load the style / theme
  GtkSettings *settings = gtk_settings_get_default();
  g_object_set(G_OBJECT(settings), "gtk-application-prefer-dark-theme", TRUE, NULL);
  g_object_set(G_OBJECT(settings), "gtk-theme-name", "Adwaita", NULL);
  g_object_unref(settings);

  GError *error = NULL;
  GtkStyleProvider *themes_style_provider = GTK_STYLE_PROVIDER(gtk_css_provider_new());
  gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), themes_style_provider,
                                            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);

  if(!gtk_css_provider_load_from_path(GTK_CSS_PROVIDER(themes_style_provider), gui->gtkrc, &error))
  {
    printf("%s: error parsing %s: %s\n", G_STRFUNC, gui->gtkrc, error->message);
    g_clear_error(&error);
  }

  g_object_unref(themes_style_provider);

  // Initializing the shortcut groups
  darktable.control->accelerators = gtk_accel_group_new();

  darktable.control->accelerator_list = NULL;

  // Connecting the callback to update keyboard accels for key_pressed
  g_signal_connect(G_OBJECT(gtk_accel_map_get()), "changed", G_CALLBACK(key_accel_changed), NULL);

  // Initializing widgets
  init_widgets(gui);

  // Adding the global shortcut group to the main window
  gtk_window_add_accel_group(GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)),
                             darktable.control->accelerators);

  // set constant width from conf key
  int panel_width = dt_conf_get_int("panel_width");
  if(panel_width < 20 || panel_width > 1000)
  {
    // fix for unset/insane values.
    panel_width = 300 * gui->dpi_factor;
    dt_conf_set_int("panel_width", panel_width);
  }

  //  dt_gui_background_jobs_init();

  /* Have the delete event (window close) end the program */
  dt_loc_get_datadir(datadir, sizeof(datadir));
  snprintf(path, sizeof(path), "%s/icons", datadir);
  gtk_icon_theme_append_search_path(gtk_icon_theme_get_default(), path);

  widget = dt_ui_center(darktable.gui->ui);

  g_signal_connect(G_OBJECT(widget), "key-press-event", G_CALLBACK(key_pressed), NULL);
  g_signal_connect(G_OBJECT(widget), "configure-event", G_CALLBACK(configure), NULL);
  g_signal_connect(G_OBJECT(widget), "draw", G_CALLBACK(draw), NULL);
  g_signal_connect(G_OBJECT(widget), "motion-notify-event", G_CALLBACK(mouse_moved), NULL);
  g_signal_connect(G_OBJECT(widget), "leave-notify-event", G_CALLBACK(center_leave), NULL);
  g_signal_connect(G_OBJECT(widget), "enter-notify-event", G_CALLBACK(center_enter), NULL);
  g_signal_connect(G_OBJECT(widget), "button-press-event", G_CALLBACK(button_pressed), NULL);
  g_signal_connect(G_OBJECT(widget), "button-release-event", G_CALLBACK(button_released), NULL);
  g_signal_connect(G_OBJECT(widget), "scroll-event", G_CALLBACK(scrolled), NULL);
  // TODO: left, right, top, bottom:
  // leave-notify-event

  widget = darktable.gui->widgets.left_border;
  g_signal_connect(G_OBJECT(widget), "draw", G_CALLBACK(draw_borders), GINT_TO_POINTER(0));
  g_signal_connect(G_OBJECT(widget), "button-press-event", G_CALLBACK(borders_button_pressed),
                   darktable.gui->ui);
  g_signal_connect(G_OBJECT(widget), "scroll-event", G_CALLBACK(borders_scrolled), GINT_TO_POINTER(0));
  g_object_set_data(G_OBJECT(widget), "border", GINT_TO_POINTER(0));
  widget = darktable.gui->widgets.right_border;
  g_signal_connect(G_OBJECT(widget), "draw", G_CALLBACK(draw_borders), GINT_TO_POINTER(1));
  g_signal_connect(G_OBJECT(widget), "button-press-event", G_CALLBACK(borders_button_pressed),
                   darktable.gui->ui);
  g_signal_connect(G_OBJECT(widget), "scroll-event", G_CALLBACK(borders_scrolled), GINT_TO_POINTER(1));
  g_object_set_data(G_OBJECT(widget), "border", GINT_TO_POINTER(1));
  widget = darktable.gui->widgets.top_border;
  g_signal_connect(G_OBJECT(widget), "draw", G_CALLBACK(draw_borders), GINT_TO_POINTER(2));
  g_signal_connect(G_OBJECT(widget), "button-press-event", G_CALLBACK(borders_button_pressed),
                   darktable.gui->ui);
  g_signal_connect(G_OBJECT(widget), "scroll-event", G_CALLBACK(borders_scrolled), GINT_TO_POINTER(2));
  g_object_set_data(G_OBJECT(widget), "border", GINT_TO_POINTER(2));
  widget = darktable.gui->widgets.bottom_border;
  g_signal_connect(G_OBJECT(widget), "draw", G_CALLBACK(draw_borders), GINT_TO_POINTER(3));
  g_signal_connect(G_OBJECT(widget), "button-press-event", G_CALLBACK(borders_button_pressed),
                   darktable.gui->ui);
  g_signal_connect(G_OBJECT(widget), "scroll-event", G_CALLBACK(borders_scrolled), GINT_TO_POINTER(3));
  g_object_set_data(G_OBJECT(widget), "border", GINT_TO_POINTER(3));
  dt_gui_presets_init();

  widget = dt_ui_center(darktable.gui->ui);
  gtk_widget_set_app_paintable(widget, TRUE);

  // TODO: make this work as: libgnomeui testgnome.c
  /*  GtkContainer *box = GTK_CONTAINER(darktable.gui->widgets.plugins_vbox);
  GtkScrolledWindow *swin = GTK_SCROLLED_WINDOW(darktable.gui->
                                                widgets.right_scrolled_window);
  gtk_container_set_focus_vadjustment (box, gtk_scrolled_window_get_vadjustment (swin));
  */
  dt_ctl_set_display_profile();
  // update the profile when the window is moved. resize is already handled in configure()
  widget = dt_ui_main_window(darktable.gui->ui);
  g_signal_connect(G_OBJECT(widget), "configure-event", G_CALLBACK(window_configure), NULL);

  // register keys for view switching
  dt_accel_register_global(NC_("accel", "capture view"), GDK_KEY_t, 0);
  dt_accel_register_global(NC_("accel", "lighttable view"), GDK_KEY_l, 0);
  dt_accel_register_global(NC_("accel", "darkroom view"), GDK_KEY_d, 0);
  dt_accel_register_global(NC_("accel", "map view"), GDK_KEY_m, 0);
  dt_accel_register_global(NC_("accel", "slideshow view"), GDK_KEY_s, 0);

  dt_accel_connect_global("capture view",
                          g_cclosure_new(G_CALLBACK(_gui_switch_view_key_accel_callback),
                                         GINT_TO_POINTER(DT_GUI_VIEW_SWITCH_TO_TETHERING), NULL));
  dt_accel_connect_global("lighttable view",
                          g_cclosure_new(G_CALLBACK(_gui_switch_view_key_accel_callback),
                                         GINT_TO_POINTER(DT_GUI_VIEW_SWITCH_TO_LIBRARY), NULL));
  dt_accel_connect_global("darkroom view",
                          g_cclosure_new(G_CALLBACK(_gui_switch_view_key_accel_callback),
                                         GINT_TO_POINTER(DT_GUI_VIEW_SWITCH_TO_DARKROOM), NULL));
  dt_accel_connect_global("map view", g_cclosure_new(G_CALLBACK(_gui_switch_view_key_accel_callback),
                                                     GINT_TO_POINTER(DT_GUI_VIEW_SWITCH_TO_MAP), NULL));
  dt_accel_connect_global("slideshow view",
                          g_cclosure_new(G_CALLBACK(_gui_switch_view_key_accel_callback),
                                         GINT_TO_POINTER(DT_GUI_VIEW_SWITCH_TO_SLIDESHOW), NULL));

  // register_keys for applying styles
  init_styles_key_accels();
  connect_styles_key_accels();
  // register ctrl-q to quit:
  dt_accel_register_global(NC_("accel", "quit"), GDK_KEY_q, GDK_CONTROL_MASK);

  dt_accel_connect_global("quit", g_cclosure_new(G_CALLBACK(quit_callback), NULL, NULL));

  // Full-screen accelerators
  dt_accel_register_global(NC_("accel", "toggle fullscreen"), GDK_KEY_F11, 0);
  dt_accel_register_global(NC_("accel", "leave fullscreen"), GDK_KEY_Escape, 0);

  dt_accel_connect_global("toggle fullscreen", g_cclosure_new(G_CALLBACK(fullscreen_key_accel_callback),
                                                              GINT_TO_POINTER(1), NULL));
  dt_accel_connect_global("leave fullscreen", g_cclosure_new(G_CALLBACK(fullscreen_key_accel_callback),
                                                             GINT_TO_POINTER(0), NULL));

  // Side-border hide/show
  dt_accel_register_global(NC_("accel", "toggle side borders"), GDK_KEY_Tab, 0);

  // toggle view of header
  dt_accel_register_global(NC_("accel", "toggle header"), GDK_KEY_h, GDK_CONTROL_MASK);

  // View-switch
  dt_accel_register_global(NC_("accel", "switch view"), GDK_KEY_period, 0);

  dt_accel_connect_global("switch view",
                          g_cclosure_new(G_CALLBACK(view_switch_key_accel_callback), NULL, NULL));

  darktable.gui->reset = 0;
  for(int i = 0; i < 3; i++) darktable.gui->bgcolor[i] = 0.1333;

  // let's try to support pressure sensitive input devices like tablets for mask drawing
  static const gchar *SOURCE_NAMES[]
      = { "GDK_SOURCE_MOUSE",    "GDK_SOURCE_PEN",         "GDK_SOURCE_ERASER",  "GDK_SOURCE_CURSOR",
          "GDK_SOURCE_KEYBOARD", "GDK_SOURCE_TOUCHSCREEN", "GDK_SOURCE_TOUCHPAD" };
  static const gchar *MODE_NAMES[] = { "GDK_MODE_DISABLED", "GDK_MODE_SCREEN", "GDK_MODE_WINDOW" };
  static const gchar *AXIS_NAMES[]
      = { "GDK_AXIS_IGNORE", "GDK_AXIS_X",     "GDK_AXIS_Y",     "GDK_AXIS_PRESSURE",
          "GDK_AXIS_XTILT",  "GDK_AXIS_YTILT", "GDK_AXIS_WHEEL", "GDK_AXIS_LAST" };
  dt_print(DT_DEBUG_INPUT, "[input device] Input devices found:\n\n");
  GList *input_devices = gdk_device_manager_list_devices(
      gdk_display_get_device_manager(gdk_display_get_default()), GDK_DEVICE_TYPE_MASTER);
  while(input_devices)
  {
    GdkDevice *device = (GdkDevice *)input_devices->data;
    GdkInputSource source = gdk_device_get_source(device);
    gint n_axes = (source == GDK_SOURCE_KEYBOARD ? 0 : gdk_device_get_n_axes(device));

    dt_print(DT_DEBUG_INPUT, "%s (%s), source: %s, mode: %s, %d axes, %d keys\n", gdk_device_get_name(device),
             (source != GDK_SOURCE_KEYBOARD) && gdk_device_get_has_cursor(device) ? "with cursor" : "no cursor",
             SOURCE_NAMES[source], MODE_NAMES[gdk_device_get_mode(device)], n_axes,
             source != GDK_SOURCE_KEYBOARD ? gdk_device_get_n_keys(device) : 0);

    for(int i = 0; i < n_axes; i++)
    {
      dt_print(DT_DEBUG_INPUT, "  %s\n", AXIS_NAMES[gdk_device_get_axis_use(device, i)]);
    }
    dt_print(DT_DEBUG_INPUT, "\n");
    input_devices = g_list_next(input_devices);
  }

  return 0;
}

void dt_gui_gtk_cleanup(dt_gui_gtk_t *gui)
{
  g_free(darktable.control->xprofile_data);
#ifdef USE_COLORDGTK
  g_free(darktable.control->colord_profile_file);
  darktable.control->colord_profile_file = NULL;
#endif
  darktable.control->xprofile_size = 0;
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
  int tb = darktable.control->tabborder;
  dt_view_manager_configure(darktable.view_manager, allocation.width - 2 * tb, allocation.height - 2 * tb);
#ifdef MAC_INTEGRATION
#ifdef GTK_TYPE_OSX_APPLICATION
  gtk_osxapplication_ready(g_object_new(GTK_TYPE_OSX_APPLICATION, NULL));
#else
  gtkosx_application_ready(g_object_new(GTKOSX_TYPE_APPLICATION, NULL));
#endif
#endif
  /* start the event loop */
  gtk_main();
  gdk_threads_leave();
}

static void init_widgets(dt_gui_gtk_t *gui)
{

  GtkWidget *container;
  GtkWidget *widget;

  // Creating the main window
  widget = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gui->ui->main_window = widget;

  // check if in HiDPI mode
#if (CAIRO_VERSION >= CAIRO_VERSION_ENCODE(1, 14, 0))
  float screen_ppd_overwrite = dt_conf_get_float("screen_ppd_overwrite");
  if(screen_ppd_overwrite > 0.0)
  {
    gui->ppd = screen_ppd_overwrite;
    dt_print(DT_DEBUG_CONTROL, "[HiDPI] setting ppd to %f as specified in the configuration file\n", screen_ppd_overwrite);
  }
  else
  {
#ifdef GDK_WINDOWING_QUARTZ
    gui->ppd = dt_osx_get_ppd();
    if(gui->ppd < 0.0)
    {
      gui->ppd = 1.0;
      dt_print(DT_DEBUG_CONTROL, "[HiDPI] can't detect screen settings, switching off\n", gui->ppd);
    }
    else
      dt_print(DT_DEBUG_CONTROL, "[HiDPI] setting ppd to %f\n", gui->ppd);
#else
    gui->ppd = 1.0;
#endif
  }
#else
  gui->ppd = 1.0;
#endif
  // get the screen resolution
  float screen_dpi_overwrite = dt_conf_get_float("screen_dpi_overwrite");
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
    GdkScreen *screen = gtk_widget_get_screen(widget);
    if(screen == NULL) screen = gdk_screen_get_default();
    int monitor = gdk_screen_get_primary_monitor(screen);
    CGDirectDisplayID ids[monitor + 1];
    uint32_t total_ids;
    CGSize size_in_mm;
    GdkRectangle size_in_px;
    if(CGGetOnlineDisplayList(monitor + 1, &ids[0], &total_ids) == kCGErrorSuccess && total_ids == monitor + 1)
    {
      size_in_mm = CGDisplayScreenSize(ids[monitor]);
      gdk_screen_get_monitor_geometry(screen, monitor, &size_in_px);
      gdk_screen_set_resolution(
          screen, 25.4 * sqrt(size_in_px.width * size_in_px.width + size_in_px.height * size_in_px.height)
                  / sqrt(size_in_mm.width * size_in_mm.width + size_in_mm.height * size_in_mm.height));
    }
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

  gtk_window_set_default_size(GTK_WINDOW(widget), DT_PIXEL_APPLY_DPI(900), DT_PIXEL_APPLY_DPI(500));

  gtk_window_set_icon_name(GTK_WINDOW(widget), "darktable");
  gtk_window_set_title(GTK_WINDOW(widget), "darktable");

  g_signal_connect(G_OBJECT(widget), "delete_event", G_CALLBACK(dt_gui_quit_callback), NULL);
  g_signal_connect(G_OBJECT(widget), "key-press-event", G_CALLBACK(key_pressed_override), NULL);
  g_signal_connect(G_OBJECT(widget), "key-release-event", G_CALLBACK(key_released), NULL);

  container = widget;

  // Adding the outermost vbox
  widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_container_add(GTK_CONTAINER(container), widget);
  gtk_widget_show(widget);

  /* connect to signal redraw all */
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_CONTROL_REDRAW_ALL,
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
                                | GDK_SCROLL_MASK);
  gtk_widget_show(widget);

  // Initializing the main table
  init_main_table(container);

  // Initializing the bottom border
  widget = gtk_drawing_area_new();
  gui->widgets.bottom_border = widget;
  gtk_box_pack_start(GTK_BOX(container), widget, FALSE, TRUE, 0);
  gtk_widget_set_size_request(widget, -1, DT_PIXEL_APPLY_DPI(10));
  gtk_widget_set_app_paintable(widget, TRUE);
  gtk_widget_set_events(widget, GDK_EXPOSURE_MASK | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK
                                | GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK | GDK_STRUCTURE_MASK
                                | GDK_SCROLL_MASK);
  gtk_widget_show(widget);

  // Showing everything
  gtk_widget_show_all(dt_ui_main_window(gui->ui));

  /* hide panels depending on last ui state */
  for(int k = 0; k < DT_UI_PANEL_SIZE; k++)
  {
    /* prevent show all */
    gtk_widget_set_no_show_all(GTK_WIDGET(gui->ui->containers[k]), TRUE);

    /* check last visible state of panel */
    char key[512];
    g_snprintf(key, sizeof(key), "ui_last/%s/visible", _ui_panel_config_names[k]);

    /* if no key, lets default to TRUE*/
    if(!dt_conf_key_exists(key)) dt_conf_set_bool(key, TRUE);

    if(!dt_conf_get_bool(key)) gtk_widget_set_visible(gui->ui->panels[k], FALSE);
  }
}

void init_main_table(GtkWidget *container)
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
                                | GDK_SCROLL_MASK);
  gtk_grid_attach(GTK_GRID(container), widget, 0, 0, 1, 2);
  gtk_widget_show(widget);

  // Adding the right border
  widget = gtk_drawing_area_new();
  darktable.gui->widgets.right_border = widget;

  gtk_widget_set_size_request(widget, DT_PIXEL_APPLY_DPI(10), -1);
  gtk_widget_set_app_paintable(widget, TRUE);
  gtk_widget_set_events(widget, GDK_EXPOSURE_MASK | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK
                                | GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK | GDK_STRUCTURE_MASK
                                | GDK_SCROLL_MASK);
  gtk_grid_attach(GTK_GRID(container), widget, 4, 0, 1, 2);
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

  /* initiialize the center top panel */
  _ui_init_panel_center_top(darktable.gui->ui, widget);

  /* setup center drawing area */
  GtkWidget *cda = gtk_drawing_area_new();
  gtk_widget_set_size_request(cda, DT_PIXEL_APPLY_DPI(50), DT_PIXEL_APPLY_DPI(200));
  gtk_widget_set_app_paintable(cda, TRUE);
  gtk_widget_set_events(cda, GDK_POINTER_MOTION_MASK | GDK_POINTER_MOTION_HINT_MASK | GDK_BUTTON_PRESS_MASK
                             | GDK_BUTTON_RELEASE_MASK | GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK
                             | GDK_SCROLL_MASK);
  gtk_widget_set_can_focus(cda, TRUE);
  gtk_widget_set_visible(cda, TRUE);

  gtk_box_pack_start(GTK_BOX(widget), cda, TRUE, TRUE, 0);
  darktable.gui->ui->center = cda;

  /* center should redraw when signal redraw center is raised*/
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_CONTROL_REDRAW_CENTER,
                            G_CALLBACK(_ui_widget_redraw_callback), darktable.gui->ui->center);

  /* initialize the center bottom panel */
  _ui_init_panel_center_bottom(darktable.gui->ui, widget);

  /* initialize the bottom panel */
  _ui_init_panel_bottom(darktable.gui->ui, container);

  /* initialize  left panel */
  _ui_init_panel_left(darktable.gui->ui, container);

  /* initialize right panel */
  _ui_init_panel_right(darktable.gui->ui, container);
}

/*
 * NEW UI API
 */
dt_ui_t *dt_ui_initialize(int argc, char **argv)
{
  dt_ui_t *ui = g_malloc0(sizeof(dt_ui_t));
  return ui;
}

void dt_ui_destroy(struct dt_ui_t *ui)
{
  g_free(ui);
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
      gtk_box_pack_end(GTK_BOX(ui->containers[c]), w, FALSE, FALSE, 2);
      break;

    /* if box is center we want it to fill as much as it can */
    case DT_UI_CONTAINER_PANEL_TOP_CENTER:
    case DT_UI_CONTAINER_PANEL_CENTER_TOP_CENTER:
    case DT_UI_CONTAINER_PANEL_CENTER_BOTTOM_CENTER:
    case DT_UI_CONTAINER_PANEL_BOTTOM:
      gtk_box_pack_start(GTK_BOX(ui->containers[c]), w, TRUE, TRUE, 2);
      break;

    default:
    {
      gtk_box_pack_start(GTK_BOX(ui->containers[c]), w, FALSE, FALSE, 2);
    }
    break;
  }
  gtk_widget_show_all(w);
}

void dt_ui_container_focus_widget(dt_ui_t *ui, const dt_ui_container_t c, GtkWidget *w)
{
  g_return_if_fail(GTK_IS_CONTAINER(ui->containers[c]));

  if(GTK_WIDGET(ui->containers[c]) != gtk_widget_get_parent(gtk_widget_get_parent(w))) return;

  gtk_container_set_focus_child(GTK_CONTAINER(ui->containers[c]), w);
  gtk_widget_queue_draw(ui->containers[c]);
}

void dt_ui_container_clear(struct dt_ui_t *ui, const dt_ui_container_t c)
{
  g_return_if_fail(GTK_IS_CONTAINER(ui->containers[c]));
  gtk_container_foreach(GTK_CONTAINER(ui->containers[c]), (GtkCallback)gtk_widget_destroy, (gpointer)c);
}

void dt_ui_toggle_panels_visibility(struct dt_ui_t *ui)
{
  char key[512];
  const dt_view_t *cv = dt_view_manager_get_current_view(darktable.view_manager);
  g_snprintf(key, sizeof(key), "%s/ui/panel_collaps_state", cv->module_name);
  uint32_t state = dt_conf_get_int(key);

  if(state)
  {
    /* restore previous panel view states */
    for(int k = 0; k < DT_UI_PANEL_SIZE; k++) dt_ui_panel_show(ui, k, (state >> k) & 1, TRUE);

    /* reset state */
    state = 0;
  }
  else
  {
    /* store current panel view state */
    for(int k = 0; k < DT_UI_PANEL_SIZE; k++) state |= (uint32_t)(dt_ui_panel_visible(ui, k)) << k;

    /* hide all panels */
    for(int k = 0; k < DT_UI_PANEL_SIZE; k++) dt_ui_panel_show(ui, k, FALSE, TRUE);
  }

  /* store new state */
  dt_conf_set_int(key, state);
}

void dt_ui_restore_panels(dt_ui_t *ui)
{
  /* restore visible state of panels for current view */
  const dt_view_t *cv = dt_view_manager_get_current_view(darktable.view_manager);
  char key[512];

  /* restore from a previous collapse all panel state if enabled */
  g_snprintf(key, sizeof(key), "%s/ui/panel_collaps_state", cv->module_name);
  uint32_t state = dt_conf_get_int(key);
  if(state)
  {
    /* hide all panels */
    for(int k = 0; k < DT_UI_PANEL_SIZE; k++) dt_ui_panel_show(ui, k, FALSE, TRUE);
  }
  else
  {
    /* restore the visible state of panels */
    for(int k = 0; k < DT_UI_PANEL_SIZE; k++)
    {
      g_snprintf(key, sizeof(key), "%s/ui/%s_visible", cv->module_name, _ui_panel_config_names[k]);
      if(dt_conf_key_exists(key))
        gtk_widget_set_visible(ui->panels[k], dt_conf_get_bool(key));
      else
        gtk_widget_set_visible(ui->panels[k], 1);
    }
  }
}

void dt_ui_border_show(dt_ui_t *ui, gboolean show)
{
  if(show)
  {
    gtk_widget_show(darktable.gui->widgets.left_border);
    gtk_widget_show(darktable.gui->widgets.right_border);
    gtk_widget_show(darktable.gui->widgets.top_border);
    gtk_widget_show(darktable.gui->widgets.bottom_border);
  }
  else
  {
    gtk_widget_hide(darktable.gui->widgets.left_border);
    gtk_widget_hide(darktable.gui->widgets.right_border);
    gtk_widget_hide(darktable.gui->widgets.top_border);
    gtk_widget_hide(darktable.gui->widgets.bottom_border);
  }
}

void dt_ui_panel_show(dt_ui_t *ui, const dt_ui_panel_t p, gboolean show, gboolean write)
{
  // if(!GTK_IS_WIDGET(ui->panels[p])) return;
  g_return_if_fail(GTK_IS_WIDGET(ui->panels[p]));

  const dt_view_t *cv = dt_view_manager_get_current_view(darktable.view_manager);
  if(write)
  {
    char key[512];
    g_snprintf(key, sizeof(key), "%s/ui/%s_visible", cv->module_name, _ui_panel_config_names[p]);
    dt_conf_set_bool(key, show);
  }

  if(show)
    gtk_widget_show(ui->panels[p]);
  else
    gtk_widget_hide(ui->panels[p]);
}

gboolean dt_ui_panel_visible(dt_ui_t *ui, const dt_ui_panel_t p)
{
  // if(!GTK_IS_WIDGET(ui->panels[p])) return FALSE;
  g_return_val_if_fail(GTK_IS_WIDGET(ui->panels[p]), FALSE);
  return gtk_widget_get_visible(ui->panels[p]);
}

GtkWidget *dt_ui_center(dt_ui_t *ui)
{
  return ui->center;
}

GtkWidget *dt_ui_main_window(dt_ui_t *ui)
{
  return ui->main_window;
}

static GtkWidget *_ui_init_panel_container_top(GtkWidget *container)
{
  GtkWidget *w = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_UI_PANEL_MODULE_SPACING);
  gtk_box_pack_start(GTK_BOX(container), w, FALSE, FALSE, 4);
  return w;
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
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(widget), GTK_POLICY_AUTOMATIC, GTK_POLICY_ALWAYS);

  /* create the scrolled viewport */
  container = widget;
  widget = gtk_viewport_new(a[2], a[3]);
  gtk_viewport_set_shadow_type(GTK_VIEWPORT(widget), GTK_SHADOW_NONE);
  gtk_container_add(GTK_CONTAINER(container), widget);

  /* create the container */
  container = widget;
  widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_UI_PANEL_MODULE_SPACING);
  gtk_widget_set_name(widget, "plugins_vbox_left");
  gtk_container_add(GTK_CONTAINER(container), widget);

  return widget;
}

static GtkWidget *_ui_init_panel_container_bottom(GtkWidget *container)
{
  GtkWidget *w = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_UI_PANEL_MODULE_SPACING);
  gtk_box_pack_start(GTK_BOX(container), w, FALSE, FALSE, DT_UI_PANEL_MODULE_SPACING);
  return w;
}

static void _ui_init_panel_left(dt_ui_t *ui, GtkWidget *container)
{
  GtkWidget *widget;

  /* create left panel main widget and add it to ui */
  widget = ui->panels[DT_UI_PANEL_LEFT] = dtgtk_side_panel_new();
  gtk_widget_set_name(widget, "left");
//   gtk_widget_set_margin_left(widget, DT_PIXEL_APPLY_DPI(5)); // i prefer it with less blank space
  gtk_grid_attach(GTK_GRID(container), widget, 1, 1, 1, 1);

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
  widget = ui->panels[DT_UI_PANEL_RIGHT] = dtgtk_side_panel_new();
  gtk_widget_set_name(widget, "right");
//   gtk_widget_set_margin_right(widget, DT_PIXEL_APPLY_DPI(5)); // i prefer it with less blank space
  gtk_grid_attach(GTK_GRID(container), widget, 3, 1, 1, 1);

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
  gtk_widget_set_hexpand(GTK_WIDGET(widget), TRUE);
  gtk_grid_attach(GTK_GRID(container), widget, 1, 2, 3, 1);

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
  gtk_box_pack_start(GTK_BOX(container), widget, FALSE, TRUE, 0);

  /* add container for center top left */
  ui->containers[DT_UI_CONTAINER_PANEL_CENTER_TOP_LEFT] = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_start(GTK_BOX(widget), ui->containers[DT_UI_CONTAINER_PANEL_CENTER_TOP_LEFT], FALSE, FALSE,
                     DT_UI_PANEL_MODULE_SPACING);

  /* add container for center top center */
  ui->containers[DT_UI_CONTAINER_PANEL_CENTER_TOP_CENTER] = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_start(GTK_BOX(widget), ui->containers[DT_UI_CONTAINER_PANEL_CENTER_TOP_CENTER], TRUE, TRUE,
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

void dt_ellipsize_combo(GtkComboBox *cbox)
{
  GList *renderers = gtk_cell_layout_get_cells(GTK_CELL_LAYOUT(cbox));
  GList *it = renderers;
  while(it)
  {
    GtkCellRendererText *tr = GTK_CELL_RENDERER_TEXT(it->data);
    g_object_set(G_OBJECT(tr), "ellipsize", PANGO_ELLIPSIZE_MIDDLE, (char *)NULL);
    it = g_list_next(it);
  }
  g_list_free(renderers);
}

// we only try to enable/disable those devices that are pens and that have a pressure axis
void dt_gui_enable_extended_input_devices()
{
  GdkDevice *core_pointer
      = gdk_device_manager_get_client_pointer(gdk_display_get_device_manager(gdk_display_get_default()));
  GList *input_devices = gdk_device_manager_list_devices(
      gdk_display_get_device_manager(gdk_display_get_default()), GDK_DEVICE_TYPE_MASTER);
  while(input_devices)
  {
    GdkDevice *device = (GdkDevice *)input_devices->data;
    if(device != core_pointer && gdk_device_get_source(device) == GDK_SOURCE_PEN)
    {
      for(int i = 0; i < gdk_device_get_n_axes(device); i++)
      {
        if(gdk_device_get_axis_use(device, i) == GDK_AXIS_PRESSURE)
        {
          gdk_device_set_mode(device, GDK_MODE_SCREEN);
          break;
        }
      }
    }
    input_devices = g_list_next(input_devices);
  }
}

void dt_gui_disable_extended_input_devices()
{
  GdkDevice *core_pointer
      = gdk_device_manager_get_client_pointer(gdk_display_get_device_manager(gdk_display_get_default()));
  GList *input_devices = gdk_device_manager_list_devices(
      gdk_display_get_device_manager(gdk_display_get_default()), GDK_DEVICE_TYPE_MASTER);
  while(input_devices)
  {
    GdkDevice *device = (GdkDevice *)input_devices->data;
    if(device != core_pointer && gdk_device_get_source(device) == GDK_SOURCE_PEN)
    {
      for(int i = 0; i < gdk_device_get_n_axes(device); i++)
      {
        if(gdk_device_get_axis_use(device, i) == GDK_AXIS_PRESSURE)
        {
          gdk_device_set_mode(device, GDK_MODE_DISABLED);
          break;
        }
      }
    }
    input_devices = g_list_next(input_devices);
  }
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
