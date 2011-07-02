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
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 600 // for setenv
#endif
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <math.h>
#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <pthread.h>

#include "common/darktable.h"
#ifdef HAVE_GPHOTO2
#   include "common/camera_control.h"
#   include "views/capture.h"
#endif
#include "common/collection.h"
#include "common/image.h"
#include "common/image_cache.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "dtgtk/label.h"
#include "dtgtk/button.h"
#include "gui/contrast.h"
#include "gui/gtk.h"

#include "gui/presets.h"
#include "gui/preferences.h"
#include "control/control.h"
#include "control/jobs.h"
#include "control/conf.h"
#include "control/signal.h"
#include "views/view.h"

/*                                                                                         
 * NEW UI API                                                             
 */

#define DT_UI_PANEL_MODULE_SPACING 10

typedef struct dt_ui_t {
  /* container widgets */
  GtkWidget *containers[DT_UI_CONTAINER_SIZE];

  /* border widgets */
  GtkWidget *borders[DT_UI_BORDER_SIZE];

  /* panel widgets */
  GtkWidget *panels[DT_UI_PANEL_SIZE];

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
/* intialize the center top panel */
static void _ui_init_panel_center_top(dt_ui_t *ui, GtkWidget *container);
/* initialize the center bottom panel */
static void _ui_init_panel_center_bottom(dt_ui_t *ui, GtkWidget *container);


/*
 * OLD UI API
 */
static void init_widgets();

static void init_main_table(GtkWidget *container);

static void init_filter_box(GtkWidget *container);
static void init_top_controls(GtkWidget *container);

static void init_center(GtkWidget *container);

static void init_lighttable_box(GtkWidget* container);


static void key_accel_changed(GtkAccelMap *object,
                              gchar *accel_path,
                              guint accel_key,
                              GdkModifierType accel_mods,
                              gpointer user_data)
{
  // Updating all the stored accelerator keys/mods for key_pressed shortcuts

  // Filmstrip
  gtk_accel_map_lookup_entry("<Darktable>/darkroom/filmstrip/scroll forward",
                             &darktable.control->accels.filmstrip_forward);
  gtk_accel_map_lookup_entry("<Darktable>/darkroom/filmstrip/scroll back",
                             &darktable.control->accels.filmstrip_back);

  // Lighttable
  gtk_accel_map_lookup_entry("<Darktable>/lighttable/scroll/up",
                             &darktable.control->accels.lighttable_up);
  gtk_accel_map_lookup_entry("<Darktable>/lighttable/scroll/down",
                             &darktable.control->accels.lighttable_down);
  gtk_accel_map_lookup_entry("<Darktable>/lighttable/scroll/left",
                             &darktable.control->accels.lighttable_left);
  gtk_accel_map_lookup_entry("<Darktable>/lighttable/scroll/right",
                             &darktable.control->accels.lighttable_right);
  gtk_accel_map_lookup_entry("<Darktable>/lighttable/scroll/center",
                             &darktable.control->accels.lighttable_center);
  gtk_accel_map_lookup_entry("<Darktable>/lighttable/preview",
                             &darktable.control->accels.lighttable_preview);

  // Global
  gtk_accel_map_lookup_entry("<Darktable>/interface/toggle side borders",
                             &darktable.control->accels.global_sideborders);

}

static void brightness_key_accel_callback(GtkAccelGroup *accel_group,
                                          GObject *acceleratable, guint keyval,
                                          GdkModifierType modifier,
                                          gpointer data)
{
  GtkWidget *widget;

  if(data)
    dt_gui_brightness_increase();
  else
    dt_gui_brightness_decrease();

  widget = darktable.gui->widgets.center;
  gtk_widget_queue_draw(widget);
}

static void contrast_key_accel_callback(GtkAccelGroup *accel_group,
                                        GObject *acceleratable, guint keyval,
                                        GdkModifierType modifier,
                                        gpointer data)
{
  GtkWidget *widget;

  if(data)
    dt_gui_contrast_increase();
  else
    dt_gui_contrast_decrease();

  widget = darktable.gui->widgets.center;
  gtk_widget_queue_draw(widget);
}

static void fullscreen_key_accel_callback(GtkAccelGroup *accel_group,
                                          GObject *acceleratable, guint keyval,
                                          GdkModifierType modifier,
                                          gpointer data)
{
  GtkWidget *widget;
  int fullscreen;

  if(data)
  {
    widget = darktable.gui->widgets.main_window;
    fullscreen = dt_conf_get_bool("ui_last/fullscreen");
    if(fullscreen) gtk_window_unfullscreen(GTK_WINDOW(widget));
    else           gtk_window_fullscreen  (GTK_WINDOW(widget));
    fullscreen ^= 1;
    dt_conf_set_bool("ui_last/fullscreen", fullscreen);
    dt_dev_invalidate(darktable.develop);
  }
  else
  {
    widget = darktable.gui->widgets.main_window;
    gtk_window_unfullscreen(GTK_WINDOW(widget));
    fullscreen = 0;
    dt_conf_set_bool("ui_last/fullscreen", fullscreen);
    dt_dev_invalidate(darktable.develop);
  }

  widget = darktable.gui->widgets.center;
  gtk_widget_queue_draw(widget);
}

static void view_switch_key_accel_callback(GtkAccelGroup *accel_group,
                                          GObject *acceleratable, guint keyval,
                                          GdkModifierType modifier,
                                          gpointer data)
{
  GtkWidget *widget;

  dt_ctl_switch_mode();

  widget = darktable.gui->widgets.center;
  gtk_widget_queue_draw(widget);
}

static gboolean
borders_button_pressed (GtkWidget *w, GdkEventButton *event, gpointer user_data)
{
  dt_ui_t *ui = (dt_ui_t*)user_data;

  int32_t bit = 0;
  int mode = dt_conf_get_int("ui_last/view");

  long which = (long)g_object_get_data(G_OBJECT(w),"border");
  int panel = 0;

  switch(which)
  {
    case 0:
      bit = dt_conf_get_int("ui_last/panel_left");
      panel = DT_UI_PANEL_LEFT;
      break;
    case 1:
      bit = dt_conf_get_int("ui_last/panel_right");
      panel = DT_UI_PANEL_RIGHT;
      break;
    case 2:
      bit = dt_conf_get_int("ui_last/panel_top");
      panel = DT_UI_PANEL_CENTER_TOP;
      break;
    default:
      bit = dt_conf_get_int("ui_last/panel_bottom");
      panel = DT_UI_PANEL_CENTER_BOTTOM;
      break;
  }

  if(dt_ui_panel_visible(ui, panel))
  {
    dt_ui_panel_show(ui, panel,FALSE);
    bit &= ~(1<<mode);
  }
  else
  {
    dt_ui_panel_show(ui, panel, TRUE);
    bit |=   1<<mode;
  }

  switch(which)
  {
    case 0:
      dt_conf_set_int("ui_last/panel_left", bit);
      break;
    case 1:
      dt_conf_set_int("ui_last/panel_right", bit);
      break;
    case 2:
      dt_conf_set_int("ui_last/panel_top", bit);
      break;
    default:
      dt_conf_set_int("ui_last/panel_bottom", bit);
      break;
  }
  gtk_widget_queue_draw(w);

  return TRUE;
}

static gboolean
_widget_focus_in_block_key_accelerators (GtkWidget *widget,GdkEventFocus *event,gpointer data)
{
  dt_control_key_accelerators_off (darktable.control);
  return FALSE;
}

static gboolean
_widget_focus_out_unblock_key_accelerators (GtkWidget *widget,GdkEventFocus *event,gpointer data)
{
  dt_control_key_accelerators_on (darktable.control);
  return FALSE;
}

void
dt_gui_key_accel_block_on_focus (GtkWidget *w)
{
  /* first off add focus change event mask */
  gtk_widget_add_events(w, GDK_FOCUS_CHANGE_MASK);

  /* conenct the signals */
  g_signal_connect (G_OBJECT (w), "focus-in-event", G_CALLBACK(_widget_focus_in_block_key_accelerators), (gpointer)w);
  g_signal_connect (G_OBJECT (w), "focus-out-event", G_CALLBACK(_widget_focus_out_unblock_key_accelerators), (gpointer)w);
}

static gint _strcmp(gconstpointer a, gconstpointer b)
{
  return strcmp((const char*)b, (const char*)a);
}

void dt_accel_group_connect_by_path(GtkAccelGroup *accel_group,
                                    const gchar *accel_path,
                                    GClosure *closure)
{
  if(!accel_group)
    return;

  GSList **list = NULL;

  // First register with GTK...
  if(closure)
    gtk_accel_group_connect_by_path(accel_group, accel_path, closure);

  // ... and then with our accelerator lists
  if(accel_group == darktable.control->accels_global)
    list = &darktable.control->accels_list_global;
  else if(accel_group == darktable.control->accels_lighttable)
    list = &darktable.control->accels_list_lighttable;
  else if(accel_group == darktable.control->accels_darkroom)
    list = &darktable.control->accels_list_darkroom;
  else if(accel_group == darktable.control->accels_capture)
    list = &darktable.control->accels_list_capture;
  else if(accel_group == darktable.control->accels_filmstrip)
    list = &darktable.control->accels_list_filmstrip;

  // Only add if the accel isn't already in the list
  if(!g_slist_find_custom(*list, (gconstpointer)accel_path, _strcmp))
    *list = g_slist_prepend(*list, strdup(accel_path));
}

void dt_accel_group_disconnect(GtkAccelGroup *accel_group,
                               GClosure *closure)
{
  if(!accel_group)
    return;

  gtk_accel_group_disconnect(accel_group, closure);
}

static gboolean
expose_borders (GtkWidget *widget, GdkEventExpose *event, gpointer user_data)
{
  // draw arrows on borders
  if(!dt_control_running()) return TRUE;
  long int which = (long int)user_data;
  float width = widget->allocation.width, height = widget->allocation.height;
  cairo_surface_t *cst = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);
  GtkWidget *cwidget = darktable.gui->widgets.center;
  GtkStyle *style = gtk_widget_get_style(cwidget);
  cairo_set_source_rgb (cr,
                        .5f*style->bg[GTK_STATE_NORMAL].red/65535.0,
                        .5f*style->bg[GTK_STATE_NORMAL].green/65535.0,
                        .5f*style->bg[GTK_STATE_NORMAL].blue/65535.0
                       );
  // cairo_set_source_rgb (cr, .13, .13, .13);
  cairo_paint(cr);

  // draw scrollbar indicators
  int v = darktable.view_manager->current_view;
  dt_view_t *view = NULL;
  if(v >= 0 && v < darktable.view_manager->num_views) view = darktable.view_manager->view + v;
  // cairo_set_source_rgb (cr, .16, .16, .16);
  cairo_set_source_rgb (cr,
                        style->bg[GTK_STATE_NORMAL].red/65535.0,
                        style->bg[GTK_STATE_NORMAL].green/65535.0,
                        style->bg[GTK_STATE_NORMAL].blue/65535.0
                       );
  const float border = 0.3;
  if(!view) cairo_paint(cr);
  else
  {
    switch(which)
    {
      case 0:
      case 1: // left, right: vertical
        cairo_rectangle(cr, 0.0, view->vscroll_pos/view->vscroll_size * height, width, view->vscroll_viewport_size/view->vscroll_size * height);
        break;
      default:        // bottom, top: horizontal
        cairo_rectangle(cr, view->hscroll_pos/view->hscroll_size * width, 0.0, view->hscroll_viewport_size/view->hscroll_size * width, height);
        break;
    }
    cairo_fill(cr);
    switch(which)
    {
      case 0:
        cairo_rectangle(cr, (1.0-border)*width, 0.0, border*width, height);
        break;
      case 1:
        cairo_rectangle(cr, 0.0, 0.0, border*width, height);
        break;
      case 2:
        cairo_rectangle(cr, (1.0-border)*height, (1.0-border)*height, width-2*(1.0-border)*height, border*height);
        break;
      default:
        cairo_rectangle(cr, (1.0-border)*height, 0.0, width-2*(1.0-border)*height, border*height);
        break;
    }
    cairo_fill(cr);
  }

  // draw gui arrows.
  cairo_set_source_rgb (cr, .6, .6, .6);

  switch(which)
  {
    case 0: // left
      if(dt_ui_panel_visible(darktable.gui->ui, DT_UI_PANEL_LEFT))
      {
        cairo_move_to (cr, width, height/2-width);
        cairo_rel_line_to (cr, 0.0, 2*width);
        cairo_rel_line_to (cr, -width, -width);
      }
      else
      {
        cairo_move_to (cr, 0.0, height/2-width);
        cairo_rel_line_to (cr, 0.0, 2*width);
        cairo_rel_line_to (cr, width, -width);
      }
      break;
    case 1: // right
      if(dt_ui_panel_visible(darktable.gui->ui, DT_UI_PANEL_RIGHT))
      {
        cairo_move_to (cr, 0.0, height/2-width);
        cairo_rel_line_to (cr, 0.0, 2*width);
        cairo_rel_line_to (cr, width, -width);
      }
      else
      {
        cairo_move_to (cr, width, height/2-width);
        cairo_rel_line_to (cr, 0.0, 2*width);
        cairo_rel_line_to (cr, -width, -width);
      }
      break;
    case 2: // top
      if(dt_ui_panel_visible(darktable.gui->ui, DT_UI_PANEL_CENTER_TOP))
      {
        cairo_move_to (cr, width/2-height, height);
        cairo_rel_line_to (cr, 2*height, 0.0);
        cairo_rel_line_to (cr, -height, -height);
      }
      else
      {
        cairo_move_to (cr, width/2-height, 0.0);
        cairo_rel_line_to (cr, 2*height, 0.0);
        cairo_rel_line_to (cr, -height, height);
      }
      break;
    default: // bottom
      if(dt_ui_panel_visible(darktable.gui->ui, DT_UI_PANEL_CENTER_BOTTOM))
      {
        cairo_move_to (cr, width/2-height, 0.0);
        cairo_rel_line_to (cr, 2*height, 0.0);
        cairo_rel_line_to (cr, -height, height);
      }
      else
      {
        cairo_move_to (cr, width/2-height, height);
        cairo_rel_line_to (cr, 2*height, 0.0);
        cairo_rel_line_to (cr, -height, -height);
      }
      break;
  }
  cairo_close_path (cr);
  cairo_fill(cr);

  cairo_destroy(cr);
  cairo_t *cr_pixmap = gdk_cairo_create(gtk_widget_get_window(widget));
  cairo_set_source_surface (cr_pixmap, cst, 0, 0);
  cairo_paint(cr_pixmap);
  cairo_destroy(cr_pixmap);
  cairo_surface_destroy(cst);
  return TRUE;
}

#if 0 // TODO: move this to module 
static dt_iop_module_t *get_colorout_module()
{
  GList *modules = darktable.develop->iop;
  while(modules)
  {
    dt_iop_module_t *module = (dt_iop_module_t *)modules->data;
    if(!strcmp(module->op, "colorout")) return module;
    modules = g_list_next(modules);
  }
  return NULL;
}

static void
update_colorpicker_panel()
{
  // synch bottom panel for develop mode
  dt_iop_module_t *module = get_colorout_module();
  if(module)
  {
    char colstring[512];
    char paddedstring[512];
    GtkWidget *w;

    w = darktable.gui->widgets.colorpicker_button;
    darktable.gui->reset = 1;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w),
                                 module->request_color_pick);
    darktable.gui->reset = 0;

    int input_color = dt_conf_get_int("ui_last/colorpicker_model");

    // always adjust picked color:
    int m = dt_conf_get_int("ui_last/colorpicker_mode");
    float fallback_col[] = {0,0,0};
    float *col = fallback_col;
    switch(m)
    {
      case 0: // mean
        if(input_color == 0)
          col = darktable.gui->picked_color_output_cs;
        else if(input_color == 1)
          col = module->picked_color;
        break;
      case 1: //min
        if(input_color == 0)
          col = darktable.gui->picked_color_output_cs_min;
        else if(input_color == 1)
          col = module->picked_color_min;
        break;
      default:
        if(input_color == 0)
          col = darktable.gui->picked_color_output_cs_max;
        else if(input_color == 1)
          col = module->picked_color_max;
        break;
    }
    w = darktable.gui->widgets.colorpicker_output_label;
    switch(input_color)
    {
    case 0: // rgb
      snprintf(colstring, 512, "(%d, %d, %d)", (int)(255 * col[0]),
               (int)(255 * col[1]), (int)(255 * col[2]));
      break;
    case 1: // Lab
      snprintf(colstring, 512, "(%.03f, %.03f, %.03f)", col[0], col[1], col[2]);
      break;
    default: // linear rgb
      snprintf(colstring, 512, "(%.03f, %.03f, %.03f)", col[0], col[1], col[2]);
      break;
    }
    snprintf(paddedstring, 512, "%-27s", colstring);
    gtk_label_set_label(GTK_LABEL(w), paddedstring);
  }
}

#endif

static gboolean
expose (GtkWidget *da, GdkEventExpose *event, gpointer user_data)
{
  dt_control_expose(NULL);
  gdk_draw_drawable(da->window,
                    da->style->fg_gc[GTK_WIDGET_STATE(da)], darktable.gui->pixmap,
                    // Only copy the area that was exposed.
                    event->area.x, event->area.y,
                    event->area.x, event->area.y,
                    event->area.width, event->area.height);

  //  update_colorpicker_panel();

  // test quit cond (thread safe, 2nd pass)
  if(!dt_control_running())
  {
    dt_cleanup();
    gtk_main_quit();
  }
 
  return TRUE;
}

#if 0 // TODO: move this to module

static void
colorpicker_mean_changed (GtkComboBox *widget, gpointer p)
{
  dt_conf_set_int("ui_last/colorpicker_mode", gtk_combo_box_get_active(widget));
  update_colorpicker_panel();
}

static void
colorpicker_model_changed(GtkComboBox *widget, gpointer p)
{
  dt_conf_set_int("ui_last/colorpicker_model", gtk_combo_box_get_active(widget));
  update_colorpicker_panel();
}

static void
colorpicker_toggled (GtkToggleButton *button, gpointer p)
{
  if(darktable.gui->reset) return;
  dt_iop_module_t *module = get_colorout_module();
  if(module)
  {
    dt_iop_request_focus(module);
    module->request_color_pick = gtk_toggle_button_get_active(button);
  }
  else
  {
    dt_iop_request_focus(NULL);
  }
  dt_control_gui_queue_draw();
}

#endif

static void
lighttable_zoom_changed (GtkSpinButton *widget, gpointer user_data)
{
  const int i = gtk_spin_button_get_value(widget);
  dt_conf_set_int("plugins/lighttable/images_in_row", i);
  dt_control_gui_queue_draw();
}

static void
lighttable_layout_changed (GtkComboBox *widget, gpointer user_data)
{
  const int i = gtk_combo_box_get_active(widget);
  dt_conf_set_int("plugins/lighttable/layout", i);
  dt_control_gui_queue_draw();
}

static void
update_query()
{
  /* sometimes changes, for similarity search e.g. */
  dt_collection_set_query_flags(darktable.collection, COLLECTION_QUERY_FULL);

  /* updates query */
  dt_collection_update_query (darktable.collection);

  /* updates visual */
  GtkWidget *win = darktable.gui->widgets.center;
  gtk_widget_queue_draw (win);

  /* update film strip, jump to currently opened image, if any: */
  if(darktable.develop->image)
    dt_view_film_strip_scroll_to(darktable.view_manager, darktable.develop->image->id);
}

static void
image_filter_changed (GtkComboBox *widget, gpointer user_data)
{
  // image_filter
  int i = gtk_combo_box_get_active(widget);
  if     (i == 0)  dt_conf_set_int("ui_last/combo_filter",     DT_LIB_FILTER_ALL);
  else if(i == 1)  dt_conf_set_int("ui_last/combo_filter",     DT_LIB_FILTER_STAR_NO);
  else if(i == 2)  dt_conf_set_int("ui_last/combo_filter",     DT_LIB_FILTER_STAR_1);
  else if(i == 3)  dt_conf_set_int("ui_last/combo_filter",     DT_LIB_FILTER_STAR_2);
  else if(i == 4)  dt_conf_set_int("ui_last/combo_filter",     DT_LIB_FILTER_STAR_3);
  else if(i == 5)  dt_conf_set_int("ui_last/combo_filter",     DT_LIB_FILTER_STAR_4);
  else if(i == 6)  dt_conf_set_int("ui_last/combo_filter",     DT_LIB_FILTER_STAR_5);
  else if(i == 7)  dt_conf_set_int("ui_last/combo_filter",     DT_LIB_FILTER_REJECT);


  /* update collection star filter flags */
  if (i == 0)
    dt_collection_set_filter_flags (darktable.collection, dt_collection_get_filter_flags (darktable.collection) & ~(COLLECTION_FILTER_ATLEAST_RATING|COLLECTION_FILTER_EQUAL_RATING));
  else if (i == 1 || i == 7)
    dt_collection_set_filter_flags (darktable.collection, (dt_collection_get_filter_flags (darktable.collection) | COLLECTION_FILTER_EQUAL_RATING) & ~COLLECTION_FILTER_ATLEAST_RATING);
  else
    dt_collection_set_filter_flags (darktable.collection, dt_collection_get_filter_flags (darktable.collection) | COLLECTION_FILTER_ATLEAST_RATING );

  /* set the star filter in collection */
  dt_collection_set_rating(darktable.collection, i-1);

  update_query();
}


static void
image_sort_changed (GtkComboBox *widget, gpointer user_data)
{
  // image_sort
  int i = gtk_combo_box_get_active(widget);
  if     (i == 0)  dt_conf_set_int("ui_last/combo_sort",     DT_LIB_SORT_FILENAME);
  else if(i == 1)  dt_conf_set_int("ui_last/combo_sort",     DT_LIB_SORT_DATETIME);
  else if(i == 2)  dt_conf_set_int("ui_last/combo_sort",     DT_LIB_SORT_RATING);
  else if(i == 3)  dt_conf_set_int("ui_last/combo_sort",     DT_LIB_SORT_ID);
  else if(i == 4)  dt_conf_set_int("ui_last/combo_sort",     DT_LIB_SORT_COLOR);

  update_query();
}

void
preferences_button_clicked (GtkWidget *widget, gpointer user_data)
{
  dt_gui_preferences_show();
}


static gboolean
scrolled (GtkWidget *widget, GdkEventScroll *event, gpointer user_data)
{
  dt_view_manager_scrolled(darktable.view_manager, event->x, event->y, event->direction == GDK_SCROLL_UP, event->state & 0xf);
  gtk_widget_queue_draw(widget);
  return TRUE;
}

static gboolean
borders_scrolled (GtkWidget *widget, GdkEventScroll *event, gpointer user_data)
{
  dt_view_manager_border_scrolled(darktable.view_manager, event->x, event->y, (long int)user_data, event->direction == GDK_SCROLL_UP);
  gtk_widget_queue_draw(widget);
  return TRUE;
}

void quit()
{
  // thread safe quit, 1st pass:
  GtkWindow *win = GTK_WINDOW(darktable.gui->widgets.main_window);
  gtk_window_iconify(win);

  GtkWidget *widget;
  widget = darktable.gui->widgets.left_border;
  g_signal_handlers_block_by_func (widget, expose_borders, (gpointer)0);
  widget = darktable.gui->widgets.right_border;
  g_signal_handlers_block_by_func (widget, expose_borders, (gpointer)1);
  widget = darktable.gui->widgets.top_border;
  g_signal_handlers_block_by_func (widget, expose_borders, (gpointer)2);
  widget = darktable.gui->widgets.bottom_border;
  g_signal_handlers_block_by_func (widget, expose_borders, (gpointer)3);

  dt_pthread_mutex_lock(&darktable.control->cond_mutex);
  dt_pthread_mutex_lock(&darktable.control->run_mutex);
  darktable.control->running = 0;
  dt_pthread_mutex_unlock(&darktable.control->run_mutex);
  dt_pthread_mutex_unlock(&darktable.control->cond_mutex);
  widget = darktable.gui->widgets.center;
  gtk_widget_queue_draw(widget);
}

static void _gui_switch_view_key_accel_callback(GtkAccelGroup *accel_group,
                                                GObject *acceleratable,
                                                guint keyval,
                                                GdkModifierType modifier,
                                                gpointer p)
{
  int view=(long int)p;
  dt_ctl_gui_mode_t mode=DT_MODE_NONE;
  /* do some setup before switch view*/
  switch (view)
  {
#ifdef HAVE_GPHOTO2
    case DT_GUI_VIEW_SWITCH_TO_TETHERING:
      // switching to capture view using "plugins/capture/current_filmroll" as session...
      // and last used camera
      if (dt_camctl_can_enter_tether_mode(darktable.camctl,NULL) )
      {
        dt_conf_set_int( "plugins/capture/mode", DT_CAPTURE_MODE_TETHERED);
        mode = DT_CAPTURE;
      }
      break;
#endif

    case DT_GUI_VIEW_SWITCH_TO_DARKROOM:
      mode = DT_DEVELOP;
      break;

    case DT_GUI_VIEW_SWITCH_TO_LIBRARY:
      mode = DT_LIBRARY;
      break;

  }

  /* try switch to mode */
  dt_ctl_switch_mode_to (mode);
}

static void quit_callback(GtkAccelGroup *accel_group,
                          GObject *acceleratable, guint keyval,
                          GdkModifierType modifier)
{
  quit();
}

static gboolean
configure (GtkWidget *da, GdkEventConfigure *event, gpointer user_data)
{
  static int oldw = 0;
  static int oldh = 0;
  //make our selves a properly sized pixmap if our window has been resized
  if (oldw != event->width || oldh != event->height)
  {
    //create our new pixmap with the correct size.
    GdkPixmap *tmppixmap = gdk_pixmap_new(da->window, event->width,  event->height, -1);
    //copy the contents of the old pixmap to the new pixmap.  This keeps ugly uninitialized
    //pixmaps from being painted upon resize
    int minw = oldw, minh = oldh;
    if(event->width  < minw) minw = event->width;
    if(event->height < minh) minh = event->height;
    gdk_draw_drawable(tmppixmap, da->style->fg_gc[GTK_WIDGET_STATE(da)], darktable.gui->pixmap, 0, 0, 0, 0, minw, minh);
    //we're done with our old pixmap, so we can get rid of it and replace it with our properly-sized one.
    g_object_unref(darktable.gui->pixmap);
    darktable.gui->pixmap = tmppixmap;
  }
  oldw = event->width;
  oldh = event->height;

  return dt_control_configure(da, event, user_data);
}

static gboolean
key_pressed_override (GtkWidget *w, GdkEventKey *event, gpointer user_data)
{
  // fprintf(stderr,"Key Press state: %d hwkey: %d\n",event->state, event->hardware_keycode);


  return dt_control_key_pressed_override(event->keyval,
                                         event->state & KEY_STATE_MASK);
}

static gboolean
key_pressed (GtkWidget *w, GdkEventKey *event, gpointer user_data)
{
  return dt_control_key_pressed(event->keyval, event->state & KEY_STATE_MASK);
}

static gboolean
key_released (GtkWidget *w, GdkEventKey *event, gpointer user_data)
{
  return dt_control_key_released(event->keyval, event->state & KEY_STATE_MASK);
}

static gboolean
button_pressed (GtkWidget *w, GdkEventButton *event, gpointer user_data)
{
  dt_control_button_pressed(event->x, event->y, event->button, event->type, event->state & 0xf);
  gtk_widget_grab_focus(w);
  gtk_widget_queue_draw(w);
  return TRUE;
}

static gboolean
button_released (GtkWidget *w, GdkEventButton *event, gpointer user_data)
{
  dt_control_button_released(event->x, event->y, event->button, event->state & 0xf);
  gtk_widget_queue_draw(w);
  return TRUE;
}

static gboolean
mouse_moved (GtkWidget *w, GdkEventMotion *event, gpointer user_data)
{
  dt_control_mouse_moved(event->x, event->y, event->state & 0xf);
  gint x, y;
  gdk_window_get_pointer(event->window, &x, &y, NULL);
  return TRUE;
}

static gboolean
center_leave(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  dt_control_mouse_leave();
  return TRUE;
}

static gboolean
center_enter(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  dt_control_mouse_enter();
  return TRUE;
}

int
dt_gui_gtk_init(dt_gui_gtk_t *gui, int argc, char *argv[])
{
  // unset gtk rc from kde:
  char path[1024], datadir[1024];
  dt_get_datadir(datadir, 1024);
  gchar *themefile = dt_conf_get_string("themefile");
  if(themefile && themefile[0] == '/') snprintf(path, 1023, "%s", themefile);
  else snprintf(path, 1023, "%s/%s", datadir, themefile ? themefile : "darktable.gtkrc");
  if(!g_file_test(path, G_FILE_TEST_EXISTS))
    snprintf(path, 1023, "%s/%s", DARKTABLE_DATADIR, themefile ? themefile : "darktable.gtkrc");
  (void)setenv("GTK2_RC_FILES", path, 1);

  GtkWidget *widget;
  gui->ui = dt_ui_initialize(argc,argv);
  gui->pixmap = NULL;
  gui->center_tooltip = 0;
  gui->presets_popup_menu = NULL;
  if (!g_thread_supported ()) g_thread_init(NULL);
  gdk_threads_init();
  gdk_threads_enter();
  gtk_init (&argc, &argv);

  if(g_file_test(path, G_FILE_TEST_EXISTS)) gtk_rc_parse (path);
  else
  {
    fprintf(stderr, "[gtk_init] could not find `%s' in . or %s!\n", themefile, datadir);
    g_free(themefile);
    return 1;
  }
  g_free(themefile);

  // Initializing the shortcut groups
  darktable.control->accels_global = gtk_accel_group_new();
  darktable.control->accels_darkroom = gtk_accel_group_new();
  darktable.control->accels_lighttable = gtk_accel_group_new();
  darktable.control->accels_capture = gtk_accel_group_new();
  darktable.control->accels_filmstrip = gtk_accel_group_new();

  darktable.control->accels_list_global = NULL;
  darktable.control->accels_list_lighttable = NULL;
  darktable.control->accels_list_darkroom = NULL;
  darktable.control->accels_list_capture = NULL;
  darktable.control->accels_list_filmstrip = NULL;

  // Connecting the callback to update keyboard accels for key_pressed
  g_signal_connect(G_OBJECT(gtk_accel_map_get()),
                   "changed",
                   G_CALLBACK(key_accel_changed),
                   NULL);

  // Initializing widgets
  init_widgets();

  // Adding the global shortcut group to the main window
  gtk_window_add_accel_group(GTK_WINDOW(darktable.gui->widgets.main_window),
                             darktable.control->accels_global);

  // set constant width from gconf key
  int panel_width = dt_conf_get_int("panel_width");
  if(panel_width < 20 || panel_width > 500)
  {
    // fix for unset/insane values.
    panel_width = 300;
    dt_conf_set_int("panel_width", panel_width);
  }

  //  dt_gui_background_jobs_init();

  /* Have the delete event (window close) end the program */
  dt_get_datadir(datadir, 1024);
  snprintf(path, 1024, "%s/icons", datadir);
  gtk_icon_theme_append_search_path (gtk_icon_theme_get_default (), path);

  widget = darktable.gui->widgets.center;

  g_signal_connect (G_OBJECT (widget), "key-press-event",
                    G_CALLBACK (key_pressed), NULL);
  g_signal_connect (G_OBJECT (widget), "configure-event",
                    G_CALLBACK (configure), NULL);
  g_signal_connect (G_OBJECT (widget), "expose-event",
                    G_CALLBACK (expose), NULL);
  g_signal_connect (G_OBJECT (widget), "motion-notify-event",
                    G_CALLBACK (mouse_moved), NULL);
  g_signal_connect (G_OBJECT (widget), "leave-notify-event",
                    G_CALLBACK (center_leave), NULL);
  g_signal_connect (G_OBJECT (widget), "enter-notify-event",
                    G_CALLBACK (center_enter), NULL);
  g_signal_connect (G_OBJECT (widget), "button-press-event",
                    G_CALLBACK (button_pressed), NULL);
  g_signal_connect (G_OBJECT (widget), "button-release-event",
                    G_CALLBACK (button_released), NULL);
  g_signal_connect (G_OBJECT (widget), "scroll-event",
                    G_CALLBACK (scrolled), NULL);
  // TODO: left, right, top, bottom:
  //leave-notify-event

  widget = darktable.gui->widgets.left_border;
  g_signal_connect (G_OBJECT (widget), "expose-event", G_CALLBACK (expose_borders), (gpointer)0);
  g_signal_connect (G_OBJECT (widget), "button-press-event", G_CALLBACK (borders_button_pressed), darktable.gui->ui);
  g_signal_connect (G_OBJECT (widget), "scroll-event", G_CALLBACK (borders_scrolled), (gpointer)0);
  g_object_set_data(G_OBJECT (widget), "border", (gpointer)0);
  widget = darktable.gui->widgets.right_border;
  g_signal_connect (G_OBJECT (widget), "expose-event", G_CALLBACK (expose_borders), (gpointer)1);
  g_signal_connect (G_OBJECT (widget), "button-press-event", G_CALLBACK (borders_button_pressed), darktable.gui->ui);
  g_signal_connect (G_OBJECT (widget), "scroll-event", G_CALLBACK (borders_scrolled), (gpointer)1);
  g_object_set_data(G_OBJECT (widget), "border", (gpointer)1);
  widget = darktable.gui->widgets.top_border;
  g_signal_connect (G_OBJECT (widget), "expose-event", G_CALLBACK (expose_borders), (gpointer)2);
  g_signal_connect (G_OBJECT (widget), "button-press-event", G_CALLBACK (borders_button_pressed), darktable.gui->ui);
  g_signal_connect (G_OBJECT (widget), "scroll-event", G_CALLBACK (borders_scrolled), (gpointer)2);
  g_object_set_data(G_OBJECT (widget), "border", (gpointer)2);
  widget = darktable.gui->widgets.bottom_border;
  g_signal_connect (G_OBJECT (widget), "expose-event", G_CALLBACK (expose_borders), (gpointer)3);
  g_signal_connect (G_OBJECT (widget), "button-press-event", G_CALLBACK (borders_button_pressed), darktable.gui->ui);
  g_signal_connect (G_OBJECT (widget), "scroll-event", G_CALLBACK (borders_scrolled), (gpointer)3);
  g_object_set_data(G_OBJECT (widget), "border", (gpointer)3);
  dt_gui_presets_init();

  // color picker
  for(int k = 0; k < 3; k++)
    darktable.gui->picked_color_output_cs[k] =
        darktable.gui->picked_color_output_cs_max[k] =
        darktable.gui->picked_color_output_cs_min[k] = 0;

  widget = darktable.gui->widgets.center;
  GTK_WIDGET_UNSET_FLAGS (widget, GTK_DOUBLE_BUFFERED);
  // GTK_WIDGET_SET_FLAGS (widget, GTK_DOUBLE_BUFFERED);
  GTK_WIDGET_SET_FLAGS   (widget, GTK_APP_PAINTABLE);

  // TODO: make this work as: libgnomeui testgnome.c
  /*  GtkContainer *box = GTK_CONTAINER(darktable.gui->widgets.plugins_vbox);
  GtkScrolledWindow *swin = GTK_SCROLLED_WINDOW(darktable.gui->
                                                widgets.right_scrolled_window);
  gtk_container_set_focus_vadjustment (box, gtk_scrolled_window_get_vadjustment (swin));
  */
  dt_ctl_get_display_profile(widget, &darktable.control->xprofile_data, &darktable.control->xprofile_size);

  // register keys for view switching
  gtk_accel_map_add_entry("<Darktable>/views/capture", GDK_t, 0);
  gtk_accel_map_add_entry("<Darktable>/views/lighttable", GDK_l, 0);
  gtk_accel_map_add_entry("<Darktable>/views/darkroom", GDK_d, 0);

  dt_accel_group_connect_by_path(
      darktable.control->accels_global, "<Darktable>/views/capture",
      g_cclosure_new(G_CALLBACK(_gui_switch_view_key_accel_callback),
                     (gpointer)DT_GUI_VIEW_SWITCH_TO_TETHERING, NULL));
  dt_accel_group_connect_by_path(
      darktable.control->accels_global, "<Darktable>/views/lighttable",
      g_cclosure_new(G_CALLBACK(_gui_switch_view_key_accel_callback),
                     (gpointer)DT_GUI_VIEW_SWITCH_TO_LIBRARY, NULL));
  dt_accel_group_connect_by_path(
      darktable.control->accels_global, "<Darktable>/views/darkroom",
      g_cclosure_new(G_CALLBACK(_gui_switch_view_key_accel_callback),
                     (gpointer)DT_GUI_VIEW_SWITCH_TO_DARKROOM, NULL));

  // register ctrl-q to quit:
  gtk_accel_map_add_entry("<Darktable>/quit", GDK_q, GDK_CONTROL_MASK);

  dt_accel_group_connect_by_path(
      darktable.control->accels_global, "<Darktable>/quit",
      g_cclosure_new(G_CALLBACK(quit_callback), NULL, NULL));

  // Contrast and brightness accelerators
  gtk_accel_map_add_entry("<Darktable>/interface/increase brightness",
                         GDK_F10, 0);
  gtk_accel_map_add_entry("<Darktable>/interface/decrease brightness",
                          GDK_F9, 0);
  gtk_accel_map_add_entry("<Darktable>/interface/increase contrast",
                          GDK_F8, 0);
  gtk_accel_map_add_entry("<Darktable>/interface/decrease contrast",
                          GDK_F7, 0);

  dt_accel_group_connect_by_path(
      darktable.control->accels_global,
      "<Darktable>/interface/increase brightness",
      g_cclosure_new(G_CALLBACK(brightness_key_accel_callback),
                     (gpointer)1, NULL));
  dt_accel_group_connect_by_path(
      darktable.control->accels_global,
      "<Darktable>/interface/decrease brightness",
      g_cclosure_new(G_CALLBACK(brightness_key_accel_callback),
                     (gpointer)0, NULL));
  dt_accel_group_connect_by_path(
      darktable.control->accels_global,
      "<Darktable>/interface/increase contrast",
      g_cclosure_new(G_CALLBACK(contrast_key_accel_callback),
                     (gpointer)1, NULL));
  dt_accel_group_connect_by_path(
      darktable.control->accels_global,
      "<Darktable>/interface/decrease contrast",
      g_cclosure_new(G_CALLBACK(contrast_key_accel_callback),
                     (gpointer)0, NULL));

  // Full-screen accelerators
  gtk_accel_map_add_entry("<Darktable>/interface/toggle fullscreen",
                          GDK_F11, 0);
  gtk_accel_map_add_entry("<Darktable>/interface/leave fullscreen",
                          GDK_Escape, 0);

  dt_accel_group_connect_by_path(
      darktable.control->accels_global,
      "<Darktable>/interface/toggle fullscreen",
      g_cclosure_new(G_CALLBACK(fullscreen_key_accel_callback),
                     (gpointer)1, NULL));
  dt_accel_group_connect_by_path(
      darktable.control->accels_global,
      "<Darktable>/interface/leave fullscreen",
      g_cclosure_new(G_CALLBACK(fullscreen_key_accel_callback),
                     (gpointer)0, NULL));

  // Side-border hide/show
  gtk_accel_map_add_entry("<Darktable>/interface/toggle side borders",
                          GDK_Tab, 0);

  dt_accel_group_connect_by_path(darktable.control->accels_global,
                                 "<Darktable>/interface/toggle side borders",
                                 NULL);

  // View-switch
  gtk_accel_map_add_entry("<Darktable>/switch view",
                          GDK_period, 0);

  dt_accel_group_connect_by_path(
      darktable.control->accels_global,
      "<Darktable>/switch view",
      g_cclosure_new(G_CALLBACK(view_switch_key_accel_callback), NULL, NULL));

  darktable.gui->reset = 0;
  for(int i=0; i<3; i++) darktable.gui->bgcolor[i] = 0.1333;

  /* apply contrast to theme */
  dt_gui_contrast_init ();

  return 0;
}

void dt_gui_gtk_cleanup(dt_gui_gtk_t *gui)
{
  g_free(darktable.control->xprofile_data);
  darktable.control->xprofile_size = 0;
}

void dt_gui_gtk_run(dt_gui_gtk_t *gui)
{
  GtkWidget *widget = darktable.gui->widgets.center;
  darktable.gui->pixmap = gdk_pixmap_new(widget->window, widget->allocation.width, widget->allocation.height, -1);
  /* start the event loop */
  gtk_main ();
  gdk_threads_leave();
}

void init_widgets()
{

  GtkWidget* container;
  GtkWidget* widget;

  // Creating the main window
  widget = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  darktable.gui->widgets.main_window = widget;
  gtk_window_set_default_size(GTK_WINDOW(widget), 900, 500);

  gtk_window_set_icon_name(GTK_WINDOW(widget), "darktable");
  gtk_window_set_title(GTK_WINDOW(widget), "Darktable");

  g_signal_connect (G_OBJECT (widget), "delete_event",
                    G_CALLBACK (quit), NULL);
  g_signal_connect (G_OBJECT (widget), "key-press-event",
                    G_CALLBACK (key_pressed_override), NULL);
  g_signal_connect (G_OBJECT (widget), "key-release-event",
                    G_CALLBACK (key_released), NULL);

  container = widget;

  // Adding the outermost vbox
  widget = gtk_vbox_new(FALSE, 0);
  gtk_container_add(GTK_CONTAINER(container), widget);
  gtk_widget_show(widget);

  container = widget;

  // Initializing the top border
  widget = gtk_drawing_area_new();
  darktable.gui->widgets.top_border = widget;
  gtk_box_pack_start(GTK_BOX(container), widget, FALSE, TRUE, 0);
  gtk_widget_set_size_request(widget, -1, 10);
  gtk_widget_set_app_paintable(widget, TRUE);
  gtk_widget_set_events(widget,
                        GDK_EXPOSURE_MASK
                        | GDK_BUTTON_PRESS_MASK
                        | GDK_BUTTON_RELEASE_MASK
                        | GDK_ENTER_NOTIFY_MASK
                        | GDK_LEAVE_NOTIFY_MASK
                        | GDK_STRUCTURE_MASK
                        | GDK_SCROLL_MASK);
  gtk_widget_show(widget);

  // Initializing the main table
  init_main_table(container);

  // Initializing the bottom border
  widget = gtk_drawing_area_new();
  darktable.gui->widgets.bottom_border = widget;
  gtk_box_pack_start(GTK_BOX(container), widget, FALSE, TRUE, 0);
  gtk_widget_set_size_request(widget, -1, 10);
  gtk_widget_set_app_paintable(widget, TRUE);
  gtk_widget_set_events(widget,
                        GDK_EXPOSURE_MASK
                        | GDK_BUTTON_PRESS_MASK
                        | GDK_BUTTON_RELEASE_MASK
                        | GDK_ENTER_NOTIFY_MASK
                        | GDK_LEAVE_NOTIFY_MASK
                        | GDK_STRUCTURE_MASK
                        | GDK_SCROLL_MASK);
  gtk_widget_show(widget);

  // Showing everything
  gtk_widget_show_all(darktable.gui->widgets.main_window);
}

void init_main_table(GtkWidget *container)
{
  GtkWidget *widget;

  // Creating the table
  widget = gtk_table_new(2, 5, FALSE);
  gtk_box_pack_start(GTK_BOX(container), widget, TRUE, TRUE, 0);
  gtk_widget_show(widget);

  container = widget;

  // Adding the left border
  widget = gtk_drawing_area_new();
  darktable.gui->widgets.left_border = widget;

  gtk_widget_set_size_request(widget, 10, -1);
  gtk_widget_set_app_paintable(widget, TRUE);
  gtk_widget_set_events(widget,
                        GDK_EXPOSURE_MASK
                        | GDK_BUTTON_PRESS_MASK
                        | GDK_BUTTON_RELEASE_MASK
                        | GDK_ENTER_NOTIFY_MASK
                        | GDK_LEAVE_NOTIFY_MASK
                        | GDK_STRUCTURE_MASK
                        | GDK_SCROLL_MASK);
  gtk_table_attach(GTK_TABLE(container), widget, 0, 1, 0, 2,
                   GTK_FILL, GTK_FILL, 0, 0);
  gtk_widget_show(widget);

  // Adding the right border
  widget = gtk_drawing_area_new();
  darktable.gui->widgets.right_border = widget;

  gtk_widget_set_size_request(widget, 10, -1);
  gtk_widget_set_app_paintable(widget, TRUE);
  gtk_widget_set_events(widget,
                        GDK_EXPOSURE_MASK
                        | GDK_BUTTON_PRESS_MASK
                        | GDK_BUTTON_RELEASE_MASK
                        | GDK_ENTER_NOTIFY_MASK
                        | GDK_LEAVE_NOTIFY_MASK
                        | GDK_STRUCTURE_MASK
                        | GDK_SCROLL_MASK);
  gtk_table_attach(GTK_TABLE(container), widget, 4, 5, 0, 2,
                   GTK_FILL, GTK_FILL, 0, 0);
  gtk_widget_show(widget);

  /* initialize the top container */
  _ui_init_panel_top(darktable.gui->ui, container);


  // Initializing the center
  widget = gtk_vbox_new(FALSE, 0);
  gtk_table_attach(GTK_TABLE(container), widget, 2, 3, 1, 2,
                   GTK_EXPAND | GTK_FILL, GTK_EXPAND | GTK_FILL, 0, 0);
  init_center(widget);
  gtk_widget_show(widget);

  /* initialize  left panel */
  _ui_init_panel_left(darktable.gui->ui, container);

  /* initialize right panel */
  _ui_init_panel_right(darktable.gui->ui, container);
}

void init_filter_box(GtkWidget *container)
{

  GtkWidget *widget;

  // Adding the list label
  widget = gtk_label_new(_("list"));
  gtk_box_pack_start(GTK_BOX(container), widget, FALSE, FALSE, 7);
  gtk_widget_show(widget);

  // Adding the list combobox
  widget = gtk_combo_box_new_text();
  darktable.gui->widgets.image_filter = widget;
  gtk_box_pack_start(GTK_BOX(container), widget, FALSE, FALSE, 0);
  gtk_combo_box_append_text(GTK_COMBO_BOX(widget), _("all"));
  gtk_combo_box_append_text(GTK_COMBO_BOX(widget), _("unstarred"));
  gtk_combo_box_append_text(GTK_COMBO_BOX(widget), _("1 star"));
  gtk_combo_box_append_text(GTK_COMBO_BOX(widget), _("2 star"));
  gtk_combo_box_append_text(GTK_COMBO_BOX(widget), _("3 star"));
  gtk_combo_box_append_text(GTK_COMBO_BOX(widget), _("4 star"));
  gtk_combo_box_append_text(GTK_COMBO_BOX(widget), _("5 star"));
  gtk_combo_box_append_text(GTK_COMBO_BOX(widget), _("rejected"));
  gtk_widget_show(widget);

  g_signal_connect (G_OBJECT (widget), "changed",
                    G_CALLBACK (image_filter_changed),
                    (gpointer)0);

  // Adding the sort label
  widget = gtk_label_new(_("images sorted by"));
  gtk_box_pack_start(GTK_BOX(container), widget, FALSE, FALSE, 7);
  gtk_widget_show(widget);

  // Adding the sort combobox
  widget = gtk_combo_box_new_text();
  darktable.gui->widgets.image_sort = widget;
  gtk_box_pack_start(GTK_BOX(container), widget, FALSE, FALSE, 0);
  gtk_combo_box_append_text(GTK_COMBO_BOX(widget), _("filename"));
  gtk_combo_box_append_text(GTK_COMBO_BOX(widget), _("time"));
  gtk_combo_box_append_text(GTK_COMBO_BOX(widget), _("rating"));
  gtk_combo_box_append_text(GTK_COMBO_BOX(widget), _("id"));
  gtk_combo_box_append_text(GTK_COMBO_BOX(widget), _("color label"));
  gtk_widget_show(widget);

  g_signal_connect (G_OBJECT (widget), "changed",
                    G_CALLBACK (image_sort_changed),
                    (gpointer)0);


  // create preferences button:
  widget = dtgtk_button_new(dtgtk_cairo_paint_preferences, CPF_STYLE_FLAT);
  gtk_box_pack_end(GTK_BOX(container), widget, FALSE, FALSE, 20);
  g_object_set(G_OBJECT(widget), "tooltip-text", _("show global preferences"),
               (char *)NULL);
  g_signal_connect (G_OBJECT (widget), "clicked",
                    G_CALLBACK (preferences_button_clicked),
                    NULL);
}

void init_top_controls(GtkWidget *container)
{
  GtkWidget *widget;

  // Adding the alignment
  widget = gtk_alignment_new(.5, 1, 0, 0);
  gtk_box_pack_start(GTK_BOX(container), widget, TRUE, TRUE, 0);
  gtk_widget_show(widget);

  // Adding the hbox
  container = widget;

  widget = gtk_hbox_new(FALSE, 0);
  gtk_container_add(GTK_CONTAINER(container), widget);
  gtk_widget_show(widget);

  container = widget;

  // Adding the filter controls
  widget = gtk_hbox_new(FALSE, 0);
  gtk_box_pack_start(GTK_BOX(container), widget, FALSE, FALSE, 0);
  gtk_widget_show(widget);
  init_filter_box(widget);

  // Adding the right toolbox
  // Currently empty, replaces "top_right_toolbox" in the gladefile
  widget = gtk_hbox_new(FALSE, 0);
  gtk_box_pack_start(GTK_BOX(container), widget, FALSE, FALSE, 0);
  gtk_widget_show(widget);
}

static void _gui_widget_redraw_callback(gpointer instance, GtkWidget *widget)
{
  g_return_if_fail(GTK_IS_WIDGET(widget) && gtk_widget_is_drawable(widget));
  gtk_widget_queue_draw(widget);
}

void init_center(GtkWidget *container)
{
  GtkWidget* widget;

  /* intiialize the center top panel */
  _ui_init_panel_center_top(darktable.gui->ui, container);

  // Adding the center drawing area
  widget = gtk_drawing_area_new();
  gtk_box_pack_start(GTK_BOX(container), widget, TRUE, TRUE, 0);
  darktable.gui->widgets.center = widget;

  /* connect widget into redraw signals */
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_CONTROL_REDRAW_ALL, G_CALLBACK(_gui_widget_redraw_callback), widget);
  

  // Configuring the drawing area
  gtk_widget_set_size_request(widget, -1, 500);
  gtk_widget_set_app_paintable(widget, TRUE);
  gtk_widget_set_events(widget,
                        GDK_POINTER_MOTION_MASK
                        | GDK_POINTER_MOTION_HINT_MASK
                        | GDK_BUTTON_PRESS_MASK
                        | GDK_BUTTON_RELEASE_MASK
                        | GDK_ENTER_NOTIFY_MASK
                        | GDK_LEAVE_NOTIFY_MASK);
  gtk_widget_set_can_focus(widget, TRUE);
  gtk_widget_set_visible(widget, TRUE);


  /* initialize the center bottom panel */
  _ui_init_panel_center_bottom(darktable.gui->ui, container);

}


#if 0
void init_colorpicker(GtkWidget *container)
{
  GtkWidget* widget;

  // Creating the picker button
  widget = dtgtk_togglebutton_new(dtgtk_cairo_paint_colorpicker,
                                  CPF_STYLE_FLAT);
  darktable.gui->widgets.colorpicker_button = widget;
  g_signal_connect(G_OBJECT(widget), "toggled",
                   G_CALLBACK(colorpicker_toggled), NULL);

  gtk_box_pack_start(GTK_BOX(container), widget, TRUE, TRUE, 0);
  gtk_widget_show(widget);

  // Creating the colorpicker stat selection box
  widget = gtk_combo_box_new_text();
  gtk_combo_box_append_text(GTK_COMBO_BOX(widget), _("mean"));
  gtk_combo_box_append_text(GTK_COMBO_BOX(widget), _("min"));
  gtk_combo_box_append_text(GTK_COMBO_BOX(widget), _("max"));
  darktable.gui->widgets.colorpicker_stat_combobox = widget;

  gtk_combo_box_set_active(GTK_COMBO_BOX(widget),
                           dt_conf_get_int("ui_last/colorpicker_mode"));
  g_signal_connect(G_OBJECT(widget), "changed",
                   G_CALLBACK(colorpicker_mean_changed), NULL);

  gtk_box_pack_start(GTK_BOX(container), widget, TRUE, TRUE, 0);
  gtk_widget_show(widget);

  // Creating the colorpicker model selection box
  widget = gtk_combo_box_new_text();
  gtk_combo_box_append_text(GTK_COMBO_BOX(widget), _("rgb"));
  gtk_combo_box_append_text(GTK_COMBO_BOX(widget), _("Lab"));
  darktable.gui->widgets.colorpicker_model_combobox = widget;

  gtk_combo_box_set_active(GTK_COMBO_BOX(widget),
                           dt_conf_get_int("ui_last/colorpicker_model"));
  g_signal_connect(G_OBJECT(widget), "changed",
                   G_CALLBACK(colorpicker_model_changed), NULL);

  gtk_box_pack_start(GTK_BOX(container), widget, TRUE, TRUE, 0);
  gtk_widget_show(widget);

  // Creating the colorpicker output label
  widget = gtk_label_new(_("(---)"));
  gtk_widget_show(widget);

  darktable.gui->widgets.colorpicker_output_label = widget;
  gtk_box_pack_start(GTK_BOX(container), widget, TRUE, TRUE, 0);
}
#endif

void init_lighttable_box(GtkWidget* container)
{
  GtkWidget* widget;

  // Creating the layout combobox
  widget = gtk_combo_box_new_text();
  gtk_combo_box_append_text(GTK_COMBO_BOX(widget), _("zoomable light table"));
  gtk_combo_box_append_text(GTK_COMBO_BOX(widget), _("file manager"));
  darktable.gui->widgets.lighttable_layout_combobox = widget;

  g_signal_connect (G_OBJECT (widget), "changed",
                    G_CALLBACK (lighttable_layout_changed),
                    (gpointer)0);
  gtk_box_pack_start(GTK_BOX(container), widget, TRUE, TRUE, 0);
  gtk_widget_show(widget);

  // Creating the zoom spinbutton
  widget = gtk_spin_button_new(GTK_ADJUSTMENT(gtk_adjustment_new(7,
                                                                 1,
                                                                 26,
                                                                 1,
                                                                 3,
                                                                 0)),
                               0, 0);
  darktable.gui->widgets.lighttable_zoom_spinbutton = widget;

  g_signal_connect (G_OBJECT (widget), "value-changed",
                    G_CALLBACK (lighttable_zoom_changed),
                    (gpointer)0);
  gtk_box_pack_start(GTK_BOX(container), widget, TRUE, TRUE, 0);
  gtk_widget_show(widget);

}

/*
 * NEW UI API
 */


dt_ui_t *dt_ui_initialize(int argc, char **argv)
{
  dt_ui_t *ui=g_malloc(sizeof(dt_ui_t));
  memset(ui,0,sizeof(dt_ui_t));
  return ui;
}

void dt_ui_destroy(struct dt_ui_t *ui)
{
  g_free(ui);
}

void dt_ui_container_add_widget(dt_ui_t *ui, const dt_ui_container_t c, GtkWidget *w)
{
  //  if(!GTK_IS_BOX(ui->containers[c])) return;
  g_return_if_fail(GTK_IS_BOX(ui->containers[c]));
  gtk_box_pack_start(GTK_BOX(ui->containers[c]),w,FALSE,FALSE,0);
  gtk_widget_show_all(w);
}

void dt_ui_container_focus_widget(dt_ui_t *ui, const dt_ui_container_t c, GtkWidget *w)
{
  //if(!GTK_IS_CONTAINER(ui->containers[c])) return;
  g_return_if_fail(GTK_IS_CONTAINER(ui->containers[c]));
  gtk_container_set_focus_child(GTK_CONTAINER(ui->containers[c]), w);
  gtk_widget_queue_draw(ui->containers[c]);
}

void dt_ui_container_clear(struct dt_ui_t *ui, const dt_ui_container_t c)
{
  g_return_if_fail(GTK_IS_CONTAINER(ui->containers[c]));
  gtk_container_foreach(GTK_CONTAINER(ui->containers[c]), (GtkCallback)gtk_widget_destroy, (gpointer)c);
}

void dt_ui_panel_show(dt_ui_t *ui,const dt_ui_panel_t p, gboolean show)
{
  //if(!GTK_IS_WIDGET(ui->panels[p])) return;
  g_return_if_fail(GTK_IS_WIDGET(ui->panels[p]));

  if(show)
    gtk_widget_show(ui->panels[p]);
  else
    gtk_widget_hide(ui->panels[p]);
}

gboolean dt_ui_panel_visible(dt_ui_t *ui,const dt_ui_panel_t p)
{
  //if(!GTK_IS_WIDGET(ui->panels[p])) return FALSE;
  g_return_val_if_fail(GTK_IS_WIDGET(ui->panels[p]),FALSE);
  return gtk_widget_get_visible(ui->panels[p]);
}

static GtkWidget * _ui_init_panel_container_top(GtkWidget *container)
{
  GtkWidget *w = gtk_vbox_new(FALSE, DT_UI_PANEL_MODULE_SPACING);
  gtk_box_pack_start(GTK_BOX(container),w,FALSE,FALSE,DT_UI_PANEL_MODULE_SPACING);
  return w;
}

static GtkWidget * _ui_init_panel_container_center(GtkWidget *container, gboolean left)
{

  GtkWidget *widget;
  GtkAdjustment *a[4];

  a[0] = GTK_ADJUSTMENT(gtk_adjustment_new(0,0,100,1,10,10));
  a[1] = GTK_ADJUSTMENT(gtk_adjustment_new(0,0,100,1,10,10));
  a[2] = GTK_ADJUSTMENT(gtk_adjustment_new(0,0,100,1,10,10));
  a[3] = GTK_ADJUSTMENT(gtk_adjustment_new(0,0,100,1,10,10));

  /* create the scrolled window */
  widget = gtk_scrolled_window_new(a[0],a[1]);
  gtk_widget_set_can_focus(widget, TRUE);
  gtk_scrolled_window_set_placement(GTK_SCROLLED_WINDOW(widget), left?GTK_CORNER_TOP_LEFT:GTK_CORNER_TOP_RIGHT);
  gtk_box_pack_start(GTK_BOX(container), widget, TRUE, TRUE, 0);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(widget), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_size_request (widget,dt_conf_get_int("panel_width")-5-13, -1);

  /* create the scrolled viewport */
  container = widget;
  widget = gtk_viewport_new(a[2],a[3]);
  gtk_viewport_set_shadow_type(GTK_VIEWPORT(widget), GTK_SHADOW_NONE);
  gtk_container_set_resize_mode(GTK_CONTAINER(widget), GTK_RESIZE_QUEUE);
  gtk_container_add(GTK_CONTAINER(container), widget);

  /* create the container */
  container = widget;
  widget = gtk_vbox_new(FALSE, DT_UI_PANEL_MODULE_SPACING);
  gtk_widget_set_name(widget, "plugins_vbox_left");
  gtk_widget_set_size_request (widget,0, -1);
  gtk_container_add(GTK_CONTAINER(container),widget);

  return widget;
}

static GtkWidget * _ui_init_panel_container_bottom(GtkWidget *container)
{
  GtkWidget *w = gtk_vbox_new(FALSE, DT_UI_PANEL_MODULE_SPACING);
  gtk_box_pack_start(GTK_BOX(container),w,FALSE,FALSE,DT_UI_PANEL_MODULE_SPACING);
  return w;
}
     
static void _ui_init_panel_left(dt_ui_t *ui, GtkWidget *container)
{
  GtkWidget *widget;
  
  /* create left panel main widget and add it to ui */
  widget = ui->panels[DT_UI_PANEL_LEFT] = gtk_alignment_new(.5, .5, 1, 1);
  gtk_widget_set_name(widget, "left");
  gtk_alignment_set_padding(GTK_ALIGNMENT(widget), 0, 0, 5, 0);
  gtk_table_attach(GTK_TABLE(container), widget, 1, 2, 1, 2,
	    GTK_SHRINK, GTK_EXPAND | GTK_SHRINK | GTK_FILL, 0, 0);

  /* set panel width */
  gtk_widget_set_size_request(widget,dt_conf_get_int("panel_width"), -1);
    
  // Adding the vbox which will containt TOP,CENTER,BOTTOM                                                                                                                                                                             
  container = widget;
  widget = gtk_vbox_new(FALSE, 0);
  gtk_container_add(GTK_CONTAINER(container), widget);
  
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
  widget = ui->panels[DT_UI_PANEL_RIGHT] = gtk_alignment_new(.5, .5, 1, 1);
  gtk_widget_set_name(widget, "right");
  gtk_alignment_set_padding(GTK_ALIGNMENT(widget), 0, 0, 0, 5);
  gtk_table_attach(GTK_TABLE(container), widget, 3, 4, 1, 2,
		   GTK_SHRINK, GTK_EXPAND | GTK_SHRINK | GTK_FILL, 0, 0);

  /* set panel width */
  gtk_widget_set_size_request(widget,dt_conf_get_int("panel_width"), -1);

  // Adding the vbox which will containt TOP,CENTER,BOTTOM                                                                                                                                                                             
  container = widget;
  widget = gtk_vbox_new(FALSE, 0);
  gtk_container_add(GTK_CONTAINER(container), widget);
  gtk_widget_set_size_request(widget, 0, -1);

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
  ui->panels[DT_UI_PANEL_TOP] = widget = gtk_hbox_new(FALSE, 0);
  gtk_table_attach(GTK_TABLE(container), widget, 1, 4, 0, 1,
                   GTK_EXPAND | GTK_FILL | GTK_SHRINK, GTK_SHRINK, 0, 0);

  /* add container for top left */
  ui->containers[DT_UI_CONTAINER_PANEL_TOP_LEFT] = gtk_hbox_new(FALSE,0);
  gtk_box_pack_start(GTK_BOX(widget), ui->containers[DT_UI_CONTAINER_PANEL_TOP_LEFT], FALSE, FALSE, 10);

  /* add container for top center */
  ui->containers[DT_UI_CONTAINER_PANEL_TOP_CENTER] = gtk_hbox_new(TRUE,0);
  gtk_box_pack_start(GTK_BOX(widget), ui->containers[DT_UI_CONTAINER_PANEL_TOP_CENTER], TRUE, TRUE, 0);
  /* add a filler to top center widget */
  gtk_box_pack_start(GTK_BOX(ui->containers[DT_UI_CONTAINER_PANEL_TOP_CENTER]), gtk_event_box_new(), TRUE,TRUE,0);
  
  /* add container for top right */
  ui->containers[DT_UI_CONTAINER_PANEL_TOP_RIGHT] = gtk_hbox_new(FALSE,0);
  gtk_box_pack_end(GTK_BOX(widget), ui->containers[DT_UI_CONTAINER_PANEL_TOP_RIGHT], FALSE, FALSE, 10);

}

static void _ui_init_panel_center_top(dt_ui_t *ui, GtkWidget *container)
{
  GtkWidget *widget;

  /* create the panel box */
  ui->panels[DT_UI_PANEL_CENTER_TOP] = widget = gtk_hbox_new(FALSE, 0);
  gtk_box_pack_start(GTK_BOX(container), widget, FALSE, TRUE, 0);

  /* add container for center top left */
  ui->containers[DT_UI_CONTAINER_PANEL_CENTER_TOP_LEFT] = gtk_hbox_new(FALSE,0);
  gtk_box_pack_start(GTK_BOX(widget), ui->containers[DT_UI_CONTAINER_PANEL_CENTER_TOP_LEFT], FALSE, FALSE, 10);

  /* add container for center top center */
  ui->containers[DT_UI_CONTAINER_PANEL_CENTER_TOP_CENTER] = gtk_hbox_new(FALSE,0);
  gtk_box_pack_start(GTK_BOX(widget), ui->containers[DT_UI_CONTAINER_PANEL_CENTER_TOP_CENTER], TRUE, TRUE, 0);

  /* add container for center top right */
  ui->containers[DT_UI_CONTAINER_PANEL_CENTER_TOP_RIGHT] = gtk_hbox_new(FALSE,0);
  gtk_box_pack_end(GTK_BOX(widget), ui->containers[DT_UI_CONTAINER_PANEL_CENTER_TOP_RIGHT], FALSE, FALSE, 10);


  /* TODO: Make modules out of these wigets */
  init_top_controls(widget);
}

static void _ui_init_panel_center_bottom(dt_ui_t *ui, GtkWidget *container)
{
  GtkWidget *widget;
  ui->panels[DT_UI_PANEL_CENTER_BOTTOM] = widget = gtk_hbox_new(FALSE, 0);
  gtk_box_pack_start(GTK_BOX(container), widget, FALSE, TRUE, 0);

  container = widget;
  GtkWidget* subcontainer;

  /* adding the center bottom left toolbox */
  ui->containers[DT_UI_CONTAINER_PANEL_CENTER_BOTTOM_LEFT] = widget = gtk_hbox_new(FALSE, 0);
  gtk_box_pack_start(GTK_BOX(container), widget, TRUE, TRUE, 0);
  darktable.gui->widgets.bottom_left_toolbox = widget;

  /* adding the center box */
  ui->containers[DT_UI_CONTAINER_PANEL_CENTER_BOTTOM_CENTER] = subcontainer = gtk_vbox_new(FALSE, 0);
  gtk_box_pack_start(GTK_BOX(container), subcontainer, FALSE, TRUE, 0);

  /* initializeing the lightable layout box 
     TODO: Make module out of this
   */
  widget = gtk_hbox_new(FALSE, 5);
  darktable.gui->widgets.bottom_lighttable_box = widget;
  gtk_box_pack_start(GTK_BOX(subcontainer), widget, TRUE, TRUE, 0);
  init_lighttable_box(widget);
  gtk_widget_show(widget);

  /* adding the right toolbox */
  ui->containers[DT_UI_CONTAINER_PANEL_CENTER_BOTTOM_RIGHT] = widget = gtk_hbox_new(FALSE, 0);
  gtk_box_pack_start(GTK_BOX(container), widget, TRUE, TRUE, 0);
  darktable.gui->widgets.bottom_right_toolbox = widget;

}
