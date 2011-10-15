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
#   include "gui/devices.h"
#   include "views/capture.h"
#endif
#include "common/collection.h"
#include "common/image.h"
#include "common/image_cache.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "dtgtk/label.h"
#include "dtgtk/button.h"
#include "gui/accelerators.h"
#include "gui/contrast.h"
#include "gui/gtk.h"
#include "gui/metadata.h"
#include "gui/iop_history.h"
#include "gui/iop_modulegroups.h"

#include "gui/presets.h"
#include "gui/preferences.h"
#include "control/control.h"
#include "control/jobs.h"
#include "control/conf.h"
#include "views/view.h"
#include "tool_colorlabels.h"

static void init_widgets();

static void init_main_table(GtkWidget *container);

static void init_view_label(GtkWidget *container);
static void init_filter_box(GtkWidget *container);
static void init_top_controls(GtkWidget *container);
static void init_dt_label(GtkWidget *container);
static void init_top(GtkWidget *container);

static void init_navigation(GtkWidget *container);
static void init_left(GtkWidget *container);
static void init_history_box(GtkWidget *container);
static void init_info_box(GtkWidget *container);
static void init_snapshots(GtkWidget *container);
static void init_import(GtkWidget *container);
static void init_left_scroll_window(GtkWidget *container);
static void init_jobs_list(GtkWidget *container);

static void init_right(GtkWidget *container);
static void init_histogram(GtkWidget *container);
static void init_module_groups(GtkWidget *container);
static void init_plugins(GtkWidget *container);
static void init_module_list(GtkWidget *container);

static void init_center(GtkWidget *container);
static void init_center_bottom(GtkWidget *container);
static void init_colorpicker(GtkWidget *container);
static void init_lighttable_box(GtkWidget* container);


static void key_accel_changed(GtkAccelMap *object,
                              gchar *accel_path,
                              guint accel_key,
                              GdkModifierType accel_mods,
                              gpointer user_data)
{
  char path[256];

  // Updating all the stored accelerator keys/mods for key_pressed shortcuts

  dt_accel_path_view(path, 256, "filmstrip", "scroll forward");
  gtk_accel_map_lookup_entry(path,
                             &darktable.control->accels.filmstrip_forward);
  dt_accel_path_view(path, 256, "filmstrip", "scroll back");
  gtk_accel_map_lookup_entry(path,
                             &darktable.control->accels.filmstrip_back);

  // Lighttable
  dt_accel_path_view(path, 256, "lighttable", "scroll up");
  gtk_accel_map_lookup_entry(path,
                             &darktable.control->accels.lighttable_up);
  dt_accel_path_view(path, 256, "lighttable", "scroll down");
  gtk_accel_map_lookup_entry(path,
                             &darktable.control->accels.lighttable_down);
  dt_accel_path_view(path, 256, "lighttable", "scroll left");
  gtk_accel_map_lookup_entry(path,
                             &darktable.control->accels.lighttable_left);
  dt_accel_path_view(path, 256, "lighttable", "scroll right");
  gtk_accel_map_lookup_entry(path,
                             &darktable.control->accels.lighttable_right);
  dt_accel_path_view(path, 256, "lighttable", "scroll center");
  gtk_accel_map_lookup_entry(path,
                             &darktable.control->accels.lighttable_center);
  dt_accel_path_view(path, 256, "lighttable", "preview");
  gtk_accel_map_lookup_entry(path,
                             &darktable.control->accels.lighttable_preview);

  // Global
  dt_accel_path_global(path, 256, "toggle side borders");
  gtk_accel_map_lookup_entry(path,
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
  widget = darktable.gui->widgets.navigation;
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
  widget = darktable.gui->widgets.navigation;
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
  widget = darktable.gui->widgets.navigation;
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
  widget = darktable.gui->widgets.navigation;
  gtk_widget_queue_draw(widget);
}

static gboolean
borders_button_pressed (GtkWidget *w, GdkEventButton *event, gpointer user_data)
{
  GtkWidget *widget;
  long int which = (long int)user_data;
  int32_t bit = 0;
  int mode = dt_conf_get_int("ui_last/view");
  switch(which)
  {
    case 0:
      bit = dt_conf_get_int("ui_last/panel_left");
      widget = darktable.gui->widgets.left;
      break;
    case 1:
      bit = dt_conf_get_int("ui_last/panel_right");
      widget = darktable.gui->widgets.right;
      break;
    case 2:
      bit = dt_conf_get_int("ui_last/panel_top");
      widget = darktable.gui->widgets.top;
      break;
    default:
      bit = dt_conf_get_int("ui_last/panel_bottom");
      widget = darktable.gui->widgets.bottom;
      break;
  }

  if(GTK_WIDGET_VISIBLE(widget))
  {
    gtk_widget_hide(widget);
    bit &= ~(1<<mode);
  }
  else
  {
    gtk_widget_show(widget);
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

//static gint _strcmp(gconstpointer a, gconstpointer b)
//{
//  return strcmp((const char*)b, (const char*)a);
//}

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

  GtkWidget *panel;
  switch(which)
  {
    case 0: // left
      panel = darktable.gui->widgets.left;
      if(GTK_WIDGET_VISIBLE(panel))
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
      panel = darktable.gui->widgets.right;
      if(GTK_WIDGET_VISIBLE(panel))
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
      panel = darktable.gui->widgets.top;
      if(GTK_WIDGET_VISIBLE(panel))
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
      panel = darktable.gui->widgets.bottom;
      if(GTK_WIDGET_VISIBLE(panel))
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

  // update other widgets
  GtkWidget *widget = darktable.gui->widgets.navigation;
  gtk_widget_queue_draw(widget);

  GList *wdl = darktable.gui->redraw_widgets;
  while(wdl)
  {
    GtkWidget *w = (GtkWidget *)wdl->data;
    gtk_widget_queue_draw(w);
    wdl = g_list_next(wdl);
  }

  update_colorpicker_panel();

  // test quit cond (thread safe, 2nd pass)
  if(!dt_control_running())
  {
    dt_cleanup();
    gtk_main_quit();
  }
  else
  {
    widget = darktable.gui->widgets.metadata_expander;
    if(gtk_expander_get_expanded(GTK_EXPANDER(widget))) dt_gui_metadata_update();
  }

  return TRUE;
}

gboolean
view_label_clicked (GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  if(event->button == 1)
  {
    dt_ctl_switch_mode();
    return TRUE;
  }
  return FALSE;
}

gboolean
darktable_label_clicked (GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  GtkWidget *dialog = gtk_about_dialog_new();
  gtk_about_dialog_set_name(GTK_ABOUT_DIALOG(dialog), PACKAGE_NAME);
  gtk_about_dialog_set_version(GTK_ABOUT_DIALOG(dialog), PACKAGE_VERSION);
  gtk_about_dialog_set_copyright(GTK_ABOUT_DIALOG(dialog), "copyright (c) johannes hanika, henrik andersson, tobias ellinghaus et al. 2009-2011");
  gtk_about_dialog_set_comments(GTK_ABOUT_DIALOG(dialog), _("organize and develop images from digital cameras"));
  gtk_about_dialog_set_website(GTK_ABOUT_DIALOG(dialog), "http://www.darktable.org/");
  gtk_about_dialog_set_logo_icon_name(GTK_ABOUT_DIALOG(dialog), "darktable");
  const char *authors[] =
  {
    _("* developers *"),
    "Henrik Andersson",
    "Johannes Hanika",
    "Tobias Ellinghaus",
    "",
    _("* ubuntu packaging, color management, video tutorials *"),
    "Pascal de Bruijn",
    "",
    _("* networking, battle testing, translation expert *"),
    "Alexandre Prokoudine",
    "",
    _("* contributors *"),
    "Alexandre Prokoudine",
    "Alexander Rabtchevich",
    "Andrea Purracchio",
    "Andrey Kaminsky",
    "Anton Blanchard",
    "Antony Dovgal",
    "Bernhard Schneider",
    "Boucman",
    "Brian Teague",
    "Bruce Guenter",
    "Christian Fuchs",
    "Christian Himpel",
    "Christian Tellefsen",
    "Daniele Giorgis",
    "David Bremner",
    "Ger Siemerink",
    "Gianluigi Calcaterra",
    "Gregor Quade",
    "Jan Rinze",
    "Jochen Schroeder",
    "Jose Carlos Garcia Sogo",
    "Karl Mikaelsson",
    "Klaus Staedtler",
    "Mikko Ruohola",
    "Moritz Lipp",
    "Nao Nakashima",
    "Olivier Tribout",
    "Omari Stephens",
    "Pascal de Bruijn",
    "Pascal Obry",
    "Robert Bieber",
    "Robert Park",
    "Rostyslav Pidgornyi",
    "Richard Hughes",
    "Sergey Astanin",
    "Simon Spannagel",
    "Stephen van den Berg",
    "Stuart Henderson",
    "Thierry Leconte",
    "Ulrich Pegelow",
    "Wyatt Olson",
    "Xavier Besse",
    "Zeus Panchenko",
    NULL
  };
  gtk_about_dialog_set_authors(GTK_ABOUT_DIALOG(dialog), authors);

  gtk_about_dialog_set_translator_credits(GTK_ABOUT_DIALOG(dialog), _("translator-credits"));
  GtkWidget *win = darktable.gui->widgets.main_window;
  gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(win));
  gtk_dialog_run(GTK_DIALOG (dialog));
  gtk_widget_destroy(dialog);
  return TRUE;
}

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


static void
snapshot_add_button_clicked (GtkWidget *widget, gpointer user_data)
{

  if (!darktable.develop->image) return;
  char wdname[64], oldfilename[30];
  GtkWidget *sbody = darktable.gui->widgets.snapshots_body;
  GtkWidget *sbox = g_list_nth_data (gtk_container_get_children (GTK_CONTAINER (sbody)), 0);

  GtkWidget *wid = g_list_nth_data (gtk_container_get_children (GTK_CONTAINER (sbox)), 0);
  gchar *label1 = g_strdup (gtk_button_get_label (GTK_BUTTON (wid)));
  snprintf (oldfilename, 30, "%s", darktable.gui->snapshot[3].filename);
  for (int k=1; k<4; k++)
  {
    wid = g_list_nth_data (gtk_container_get_children (GTK_CONTAINER (sbox)), k);
    if (k<MIN (4,darktable.gui->num_snapshots+1)) gtk_widget_set_visible (wid, TRUE);
    gchar *label2 = g_strdup(gtk_button_get_label (GTK_BUTTON (wid)));
    gtk_button_set_label (GTK_BUTTON (wid), label1);
    g_free (label1);
    label1 = label2;
    darktable.gui->snapshot[k] = darktable.gui->snapshot[k-1];
  }

  // rotate filenames, so we don't waste hd space
  snprintf(darktable.gui->snapshot[0].filename, 30, "%s", oldfilename);
  g_free(label1);

  wid = g_list_nth_data (gtk_container_get_children (GTK_CONTAINER (sbox)), 0);
  char *fname = darktable.develop->image->filename + strlen(darktable.develop->image->filename);
  while(fname > darktable.develop->image->filename && *fname != '/') fname--;
  snprintf(wdname, 64, "%s", fname);
  fname = wdname + strlen(wdname);
  while(fname > wdname && *fname != '.') fname --;
  if(*fname != '.') fname = wdname + strlen(wdname);
  if(wdname + 64 - fname > 4) sprintf(fname, "(%d)", darktable.develop->history_end);
  // snprintf(wdname, 64, _("snapshot %d"), darktable.gui->num_snapshots+1);

  gtk_button_set_label (GTK_BUTTON (wid), wdname);
  gtk_widget_set_visible (wid, TRUE);

  /* get zoom pos from develop */
  dt_gui_snapshot_t *s = darktable.gui->snapshot + 0;
  DT_CTL_GET_GLOBAL (s->zoom_y, dev_zoom_y);
  DT_CTL_GET_GLOBAL (s->zoom_x, dev_zoom_x);
  DT_CTL_GET_GLOBAL (s->zoom, dev_zoom);
  DT_CTL_GET_GLOBAL (s->closeup, dev_closeup);
  DT_CTL_GET_GLOBAL (s->zoom_scale, dev_zoom_scale);

  /* set take snap bit for darkroom */
  darktable.gui->request_snapshot = 1;
  darktable.gui->num_snapshots ++;
  dt_control_gui_queue_draw ();

}

static void
snapshot_toggled (GtkToggleButton *widget, long int which)
{
  if(!gtk_toggle_button_get_active(widget) && darktable.gui->selected_snapshot == which)
  {
    if(darktable.gui->snapshot_image)
    {
      cairo_surface_destroy(darktable.gui->snapshot_image);
      darktable.gui->snapshot_image = NULL;
      dt_control_gui_queue_draw();
    }
  }
  else if(gtk_toggle_button_get_active(widget))
  {
    GtkWidget *sbody = darktable.gui->widgets.snapshots_body;
    GtkWidget *sbox = g_list_nth_data (gtk_container_get_children (GTK_CONTAINER (sbody)), 0);

    for(int k=0; k<4; k++)
    {
      GtkWidget *w = g_list_nth_data (gtk_container_get_children (GTK_CONTAINER(sbox)), k);
      if(GTK_TOGGLE_BUTTON(w) != widget)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w), FALSE);
    }
    if(darktable.gui->snapshot_image)
    {
      cairo_surface_destroy(darktable.gui->snapshot_image);
      darktable.gui->snapshot_image = NULL;
    }
    darktable.gui->selected_snapshot = which;
    dt_gui_snapshot_t *s = darktable.gui->snapshot + which;
    DT_CTL_SET_GLOBAL(dev_zoom_y,     s->zoom_y);
    DT_CTL_SET_GLOBAL(dev_zoom_x,     s->zoom_x);
    DT_CTL_SET_GLOBAL(dev_zoom,       s->zoom);
    DT_CTL_SET_GLOBAL(dev_closeup,    s->closeup);
    DT_CTL_SET_GLOBAL(dev_zoom_scale, s->zoom_scale);
    dt_dev_invalidate(darktable.develop);
    darktable.gui->snapshot_image = cairo_image_surface_create_from_png(s->filename);
    dt_control_gui_queue_draw();
  }
}

void
preferences_button_clicked (GtkWidget *widget, gpointer user_data)
{
  dt_gui_preferences_show();
}

void
import_button_clicked (GtkWidget *widget, gpointer user_data)
{
  GtkWidget *win = darktable.gui->widgets.main_window;
  GtkWidget *filechooser = gtk_file_chooser_dialog_new (_("import film"),
                           GTK_WINDOW (win),
                           GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
                           GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
                           GTK_STOCK_OPEN, GTK_RESPONSE_ACCEPT,
                           (char *)NULL);

  gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(filechooser), TRUE);

  char *last_directory = dt_conf_get_string("ui_last/import_last_directory");
  if(last_directory != NULL)
    gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER (filechooser), last_directory);

  // add extra lines to 'extra'. don't forget to destroy the widgets later.
  GtkWidget *extra;
  extra = gtk_vbox_new(FALSE, 0);

  // recursive opening.
  GtkWidget *recursive;
  recursive = gtk_check_button_new_with_label (_("import directories recursively"));
  g_object_set(recursive, "tooltip-text", _("recursively import subdirectories. each directory goes into a new film roll."), NULL);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON (recursive), dt_conf_get_bool("ui_last/import_recursive"));
  gtk_widget_show (recursive);
  gtk_box_pack_start(GTK_BOX (extra), recursive, FALSE, FALSE, 0);

  // ignoring of jpegs. hack while we don't handle raw+jpeg in the same directories.
  GtkWidget *ignore_jpeg;
  ignore_jpeg = gtk_check_button_new_with_label (_("ignore jpeg files"));
  g_object_set(ignore_jpeg, "tooltip-text", _("do not load files with an extension of .jpg or .jpeg. this can be useful when there are raw+jpeg in a directory."), NULL);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON (ignore_jpeg), dt_conf_get_bool("ui_last/import_ignore_jpegs"));
  gtk_widget_show (ignore_jpeg);
  gtk_box_pack_start(GTK_BOX (extra), ignore_jpeg, FALSE, FALSE, 0);

  gtk_file_chooser_set_extra_widget (GTK_FILE_CHOOSER (filechooser), extra);

  if (gtk_dialog_run (GTK_DIALOG (filechooser)) == GTK_RESPONSE_ACCEPT)
  {
    dt_conf_set_bool("ui_last/import_recursive", gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON (recursive)));
    dt_conf_set_bool("ui_last/import_ignore_jpegs", gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON (ignore_jpeg)));
    dt_conf_set_string("ui_last/import_last_directory", gtk_file_chooser_get_current_folder(GTK_FILE_CHOOSER (filechooser)));

    char *filename;
    GSList *list = gtk_file_chooser_get_filenames (GTK_FILE_CHOOSER (filechooser));
    GSList *it = list;
    int id = 0;
    while(it)
    {
      filename = (char *)it->data;
      id = dt_film_import(filename);
      g_free (filename);
      it = g_slist_next(it);
    }
    if(id)
    {
      dt_film_open(id);
      dt_ctl_switch_mode_to(DT_LIBRARY);
    }
    g_slist_free (list);
  }
  gtk_widget_destroy(recursive);
  gtk_widget_destroy(ignore_jpeg);
  gtk_widget_destroy(extra);
  gtk_widget_destroy (filechooser);
  win = darktable.gui->widgets.center;
  gtk_widget_queue_draw(win);
}

void
import_image_button_clicked (GtkWidget *widget, gpointer user_data)
{
  GtkWidget *win = darktable.gui->widgets.main_window;
  GtkWidget *filechooser = gtk_file_chooser_dialog_new (_("import image"),
                           GTK_WINDOW (win),
                           GTK_FILE_CHOOSER_ACTION_OPEN,
                           GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
                           GTK_STOCK_OPEN, GTK_RESPONSE_ACCEPT,
                           (char *)NULL);

  gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(filechooser), TRUE);

  char *last_directory = dt_conf_get_string("ui_last/import_last_directory");
  if(last_directory != NULL)
    gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER (filechooser), last_directory);

  char *cp, **extensions, ext[1024];
  GtkFileFilter *filter;
  filter = GTK_FILE_FILTER(gtk_file_filter_new());
  extensions = g_strsplit(dt_supported_extensions, ",", 100);
  for(char **i=extensions; *i!=NULL; i++)
  {
    snprintf(ext, 1024, "*.%s", *i);
    gtk_file_filter_add_pattern(filter, ext);
    gtk_file_filter_add_pattern(filter, cp=g_ascii_strup(ext, -1));
    g_free(cp);
  }
  g_strfreev(extensions);
  gtk_file_filter_set_name(filter, _("supported images"));
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(filechooser), filter);

  filter = GTK_FILE_FILTER(gtk_file_filter_new());
  gtk_file_filter_add_pattern(filter, "*");
  gtk_file_filter_set_name(filter, _("all files"));
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(filechooser), filter);

  if (gtk_dialog_run (GTK_DIALOG (filechooser)) == GTK_RESPONSE_ACCEPT)
  {
    dt_conf_set_string("ui_last/import_last_directory", gtk_file_chooser_get_current_folder(GTK_FILE_CHOOSER (filechooser)));

    char *filename = NULL;
    dt_film_t film;
    GSList *list = gtk_file_chooser_get_filenames (GTK_FILE_CHOOSER (filechooser));
    GSList *it = list;
    int id = 0;
    int filmid = 0;
    while(it)
    {
      filename = (char *)it->data;
      gchar *directory = g_path_get_dirname((const gchar *)filename);
      filmid = dt_film_new(&film, directory);
      id = dt_image_import(filmid, filename, TRUE);
      if(!id) dt_control_log(_("error loading file `%s'"), filename);
      g_free (filename);
      g_free (directory);
      it = g_slist_next(it);
    }

    if(id)
    {
      dt_film_open(filmid);
      // make sure buffers are loaded (load full for testing)
      dt_image_t *img = dt_image_cache_get(id, 'r');
      dt_image_buffer_t buf = dt_image_get_blocking(img, DT_IMAGE_FULL, 'r');
      if(!buf)
      {
        dt_image_cache_release(img, 'r');
        dt_control_log(_("file `%s' has unknown format!"), img->filename);
      }
      else
      {
        dt_image_release(img, DT_IMAGE_FULL, 'r');
        dt_image_cache_release(img, 'r');
        DT_CTL_SET_GLOBAL(lib_image_mouse_over_id, id);
        dt_ctl_switch_mode_to(DT_DEVELOP);
      }
    }
  }
  gtk_widget_destroy (filechooser);
  win = darktable.gui->widgets.center;
  gtk_widget_queue_draw(win);
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

#include "background_jobs.h"
int
dt_gui_gtk_init(dt_gui_gtk_t *gui, int argc, char *argv[])
{
  // unset gtk rc from kde:
  char path[1024], datadir[1024];
  dt_util_get_datadir(datadir, 1024);
  gchar *themefile = dt_conf_get_string("themefile");
  if(themefile && themefile[0] == '/') snprintf(path, 1023, "%s", themefile);
  else snprintf(path, 1023, "%s/%s", datadir, themefile ? themefile : "darktable.gtkrc");
  if(!g_file_test(path, G_FILE_TEST_EXISTS))
    snprintf(path, 1023, "%s/%s", DARKTABLE_DATADIR, themefile ? themefile : "darktable.gtkrc");
  (void)setenv("GTK2_RC_FILES", path, 1);

  GtkWidget *widget;

  gui->num_snapshots = 0;
  gui->request_snapshot = 0;
  gui->selected_snapshot = 0;
  gui->snapshot_image = NULL;
  gui->pixmap = NULL;
  gui->center_tooltip = 0;
  memset(gui->snapshot, 0, sizeof(gui->snapshot));
  for(int k=0; k<4; k++) snprintf(gui->snapshot[k].filename, 30, "/tmp/dt_snapshot_%d.png", k);
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
  darktable.control->accelerators = gtk_accel_group_new();

  darktable.control->accelerator_list = NULL;

  // Connecting the callback to update keyboard accels for key_pressed
  g_signal_connect(G_OBJECT(gtk_accel_map_get()),
                   "changed",
                   G_CALLBACK(key_accel_changed),
                   NULL);

  // Initializing widgets
  init_widgets();

  // Adding the global shortcut group to the main window
  gtk_window_add_accel_group(GTK_WINDOW(darktable.gui->widgets.main_window),
                             darktable.control->accelerators);

  // set constant width from gconf key
  int panel_width = dt_conf_get_int("panel_width");
  if(panel_width < 20 || panel_width > 500)
  {
    // fix for unset/insane values.
    panel_width = 300;
    dt_conf_set_int("panel_width", panel_width);
  }
  widget = darktable.gui->widgets.right;
  gtk_widget_set_size_request (widget, panel_width, -1);
  widget = darktable.gui->widgets.left;
  gtk_widget_set_size_request (widget, panel_width, -1);
  widget = darktable.gui->widgets.right;
  gtk_widget_set_size_request (widget, panel_width-5, -1);
  widget = darktable.gui->widgets.left_vbox;
  gtk_widget_set_size_request (widget, panel_width-5, -1);
  // leave some space for scrollbars to appear:
  widget = darktable.gui->widgets.right_scrolled_window;
  gtk_widget_set_size_request (widget, panel_width-5-13, -1);
  widget = darktable.gui->widgets.left_scrolled;
  gtk_widget_set_size_request (widget, panel_width-5-13, -1);
  // and make the scrollbars disappear when not needed:
  widget = darktable.gui->widgets.left_scrolled_window;
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(widget), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  widget = darktable.gui->widgets.right_scrolled_window;
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(widget), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

  // Update the devices module with available devices
#ifdef HAVE_GPHOTO2
  dt_gui_devices_init();
#endif
  dt_gui_background_jobs_init();

  /* Have the delete event (window close) end the program */
  dt_util_get_datadir(datadir, 1024);
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
  g_signal_connect (G_OBJECT (widget), "button-press-event", G_CALLBACK (borders_button_pressed), (gpointer)0);
  g_signal_connect (G_OBJECT (widget), "scroll-event", G_CALLBACK (borders_scrolled), (gpointer)0);
  widget = darktable.gui->widgets.right_border;
  g_signal_connect (G_OBJECT (widget), "expose-event", G_CALLBACK (expose_borders), (gpointer)1);
  g_signal_connect (G_OBJECT (widget), "button-press-event", G_CALLBACK (borders_button_pressed), (gpointer)1);
  g_signal_connect (G_OBJECT (widget), "scroll-event", G_CALLBACK (borders_scrolled), (gpointer)1);
  widget = darktable.gui->widgets.top_border;
  g_signal_connect (G_OBJECT (widget), "expose-event", G_CALLBACK (expose_borders), (gpointer)2);
  g_signal_connect (G_OBJECT (widget), "button-press-event", G_CALLBACK (borders_button_pressed), (gpointer)2);
  g_signal_connect (G_OBJECT (widget), "scroll-event", G_CALLBACK (borders_scrolled), (gpointer)2);
  widget = darktable.gui->widgets.bottom_border;
  g_signal_connect (G_OBJECT (widget), "expose-event", G_CALLBACK (expose_borders), (gpointer)3);
  g_signal_connect (G_OBJECT (widget), "button-press-event", G_CALLBACK (borders_button_pressed), (gpointer)3);
  g_signal_connect (G_OBJECT (widget), "scroll-event", G_CALLBACK (borders_scrolled), (gpointer)3);



  widget = darktable.gui->widgets.navigation;
  dt_gui_navigation_init(&gui->navigation, widget);
  gtk_widget_set_size_request(widget, -1, panel_width*.5);

  widget = darktable.gui->widgets.histogram;
  gtk_widget_set_size_request(widget, -1, panel_width*.5);
  dt_gui_histogram_init(&gui->histogram, widget);

  dt_gui_presets_init();

  // image op history
  dt_gui_iop_history_init();

  /* initializes the module groups buttonbar control */
  dt_gui_iop_modulegroups_init ();

  // snapshot management
  GtkWidget *sbody = darktable.gui->widgets.snapshots_body;
  GtkWidget *svbox = gtk_vbox_new (FALSE,0);
  GtkWidget *sbutton = gtk_button_new_with_label (_("take snapshot"));
  g_object_set (sbutton, "tooltip-text", _("take snapshot to compare with another image or the same image at another stage of development"), (char *)NULL);
  gtk_box_pack_start (GTK_BOX (sbody),svbox,FALSE,FALSE,0);
  gtk_box_pack_start (GTK_BOX (sbody),sbutton,FALSE,FALSE,0);
  g_signal_connect(G_OBJECT(sbutton), "clicked", G_CALLBACK(snapshot_add_button_clicked), NULL);
  gtk_widget_show_all(sbody);

  for (long k=1; k<5; k++)
  {
    char wdname[20];
    snprintf (wdname, 20, "snapshot_%ld_togglebutton", k);
    GtkWidget *button = dtgtk_togglebutton_new_with_label (wdname,NULL,CPF_STYLE_FLAT);
    gtk_box_pack_start (GTK_BOX (svbox),button,FALSE,FALSE,0);
    g_signal_connect (G_OBJECT (button), "clicked",
                      G_CALLBACK (snapshot_toggled),
                      (gpointer)(k-1));
  }

  // color picker
  for(int k = 0; k < 3; k++)
    darktable.gui->picked_color_output_cs[k] =
        darktable.gui->picked_color_output_cs_max[k] =
        darktable.gui->picked_color_output_cs_min[k] = 0;


  // nice endmarker drawing.
  widget = darktable.gui->widgets.endmarker_left;
  g_signal_connect (G_OBJECT (widget), "expose-event",
                    G_CALLBACK (dt_control_expose_endmarker), (gpointer)1);

  widget = darktable.gui->widgets.center;
  GTK_WIDGET_UNSET_FLAGS (widget, GTK_DOUBLE_BUFFERED);
  // GTK_WIDGET_SET_FLAGS (widget, GTK_DOUBLE_BUFFERED);
  GTK_WIDGET_SET_FLAGS   (widget, GTK_APP_PAINTABLE);

  // TODO: make this work as: libgnomeui testgnome.c
  GtkContainer *box = GTK_CONTAINER(darktable.gui->widgets.plugins_vbox);
  GtkScrolledWindow *swin = GTK_SCROLLED_WINDOW(darktable.gui->
                                                widgets.right_scrolled_window);
  gtk_container_set_focus_vadjustment (box, gtk_scrolled_window_get_vadjustment (swin));

  dt_ctl_get_display_profile(widget, &darktable.control->xprofile_data, &darktable.control->xprofile_size);

  darktable.gui->redraw_widgets = NULL;

  // register keys for view switching
  dt_accel_register_global(NC_("accel", "capture view"), GDK_t, 0);
  dt_accel_register_global(NC_("accel", "lighttable view"), GDK_l, 0);
  dt_accel_register_global(NC_("accel", "darkroom view"), GDK_d, 0);

  dt_accel_connect_global(
      "capture view",
      g_cclosure_new(G_CALLBACK(_gui_switch_view_key_accel_callback),
                     (gpointer)DT_GUI_VIEW_SWITCH_TO_TETHERING, NULL));
  dt_accel_connect_global(
      "lighttable view",
      g_cclosure_new(G_CALLBACK(_gui_switch_view_key_accel_callback),
                     (gpointer)DT_GUI_VIEW_SWITCH_TO_LIBRARY, NULL));
  dt_accel_connect_global(
      "darkroom view",
      g_cclosure_new(G_CALLBACK(_gui_switch_view_key_accel_callback),
                     (gpointer)DT_GUI_VIEW_SWITCH_TO_DARKROOM, NULL));

  // register ctrl-q to quit:
  dt_accel_register_global(NC_("accel", "quit"), GDK_q, GDK_CONTROL_MASK);

  dt_accel_connect_global(
      "quit",
      g_cclosure_new(G_CALLBACK(quit_callback), NULL, NULL));

  // Contrast and brightness accelerators
  dt_accel_register_global(NC_("accel", "increase brightness"),
                           GDK_F10, 0);
  dt_accel_register_global(NC_("accel", "decrease brightness"),
                           GDK_F9, 0);
  dt_accel_register_global(NC_("accel", "increase contrast"),
                           GDK_F8, 0);
  dt_accel_register_global(NC_("accel", "decrease contrast"),
                           GDK_F7, 0);

  dt_accel_connect_global(
      "increase brightness",
      g_cclosure_new(G_CALLBACK(brightness_key_accel_callback),
                     (gpointer)1, NULL));
  dt_accel_connect_global(
      "decrease brightness",
      g_cclosure_new(G_CALLBACK(brightness_key_accel_callback),
                     (gpointer)0, NULL));
  dt_accel_connect_global(
      "increase contrast",
      g_cclosure_new(G_CALLBACK(contrast_key_accel_callback),
                     (gpointer)1, NULL));
  dt_accel_connect_global(
      "decrease contrast",
      g_cclosure_new(G_CALLBACK(contrast_key_accel_callback),
                     (gpointer)0, NULL));

  // Full-screen accelerators
  dt_accel_register_global(NC_("accel", "toggle fullscreen"), GDK_F11, 0);
  dt_accel_register_global(NC_("accel", "leave fullscreen"), GDK_Escape, 0);

  dt_accel_connect_global(
      "toggle fullscreen",
      g_cclosure_new(G_CALLBACK(fullscreen_key_accel_callback),
                     (gpointer)1, NULL));
  dt_accel_connect_global(
      "leave fullscreen",
      g_cclosure_new(G_CALLBACK(fullscreen_key_accel_callback),
                     (gpointer)0, NULL));

  // Side-border hide/show
  dt_accel_register_global(NC_("accel", "toggle side borders"), GDK_Tab, 0);

  // View-switch
  dt_accel_register_global(NC_("accel", "switch view"), GDK_period, 0);

  dt_accel_connect_global(
      "switch view",
      g_cclosure_new(G_CALLBACK(view_switch_key_accel_callback), NULL, NULL));

  darktable.gui->reset = 0;
  for(int i=0; i<3; i++) darktable.gui->bgcolor[i] = 0.1333;

  /* apply contrast to theme */
  dt_gui_contrast_init ();

  // TODO: Scrap this temporary fix to properly hide colorpicker on startup
  gtk_widget_hide(darktable.gui->widgets.bottom_darkroom_box);

  return 0;
}

void dt_gui_gtk_cleanup(dt_gui_gtk_t *gui)
{
  g_free(darktable.control->xprofile_data);
  darktable.control->xprofile_size = 0;
  dt_gui_navigation_cleanup(&gui->navigation);
  dt_gui_histogram_cleanup(&gui->histogram);
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

  // Initializing the top
  init_top(container);

  // Initializing the center
  widget = gtk_vbox_new(FALSE, 0);
  gtk_table_attach(GTK_TABLE(container), widget, 2, 3, 1, 2,
                   GTK_EXPAND | GTK_FILL, GTK_EXPAND | GTK_FILL, 0, 0);
  init_center(widget);
  gtk_widget_show(widget);

  // Initializing the right side
  init_right(container);

  // Initializing the left side
  init_left(container);
}

void init_view_label(GtkWidget *container)
{
  GtkWidget *widget;

  // Adding the eventbox
  widget = gtk_event_box_new();
  gtk_box_pack_start(GTK_BOX(container), widget, FALSE, TRUE, 0);
  gtk_widget_show(widget);

  g_signal_connect (G_OBJECT (widget), "button-press-event",
                    G_CALLBACK (view_label_clicked),
                    (gpointer)0);

  // Adding the label
  container = widget;

  widget = gtk_label_new(_("<span color=\"#7f7f7f\"><big><b>"
                           "light table mode</b></big></span>"));
  darktable.gui->widgets.view_label = widget;
  gtk_widget_set_name(widget, "view_label");
  gtk_container_add(GTK_CONTAINER(container), widget);
  gtk_label_set_use_markup(GTK_LABEL(widget), TRUE);
  gtk_misc_set_padding(GTK_MISC(widget), 20, 10);
  gtk_widget_show(widget);

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

  // Adding color labels
  widget = gtk_hbox_new(FALSE, 0);
  gtk_box_pack_start(GTK_BOX(container), widget, FALSE, FALSE, 0);
  gtk_widget_show(widget);
  dt_create_color_label_buttons(GTK_BOX(widget));

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

void init_dt_label(GtkWidget *container)
{
  GtkWidget *widget;

  // Adding the eventbox
  widget = gtk_event_box_new();
  gtk_box_pack_start(GTK_BOX(container), widget, FALSE, TRUE, 0);
  gtk_widget_show(widget);

  g_signal_connect(G_OBJECT (widget), "button-press-event",
                   G_CALLBACK (darktable_label_clicked), (gpointer)0);

  // Adding the label
  container = widget;
  widget = gtk_label_new("");
  gtk_widget_set_name(widget, "darktable_label");
  gtk_container_add(GTK_CONTAINER(container), widget);
  gtk_widget_set_tooltip_text(widget, _("about darktable"));
  gtk_widget_set_has_tooltip(widget, TRUE);
  gtk_label_set_use_markup(GTK_LABEL(widget), TRUE);
  gtk_label_set_width_chars(GTK_LABEL(widget), -1);
  gtk_misc_set_padding(GTK_MISC(widget), 20, 10);
  gtk_label_set_label(GTK_LABEL(widget), "<span color=\"#7f7f7f\"><big><b>"
                      PACKAGE_NAME" "PACKAGE_VERSION"</b></big></span>");

  gtk_widget_show(widget);
}

void init_top(GtkWidget *container)
{
  GtkWidget *widget;

  // Adding the outer hbox
  widget = gtk_hbox_new(FALSE, 0);
  darktable.gui->widgets.top = widget;
  gtk_table_attach(GTK_TABLE(container), widget, 1, 4, 0, 1,
                   GTK_EXPAND | GTK_FILL | GTK_SHRINK, GTK_SHRINK, 0, 0);
  gtk_widget_show(widget);

  // Adding the dt label
  init_dt_label(widget);

  // Adding the top controls
  init_top_controls(widget);

  // Adding the view label
  init_view_label(widget);
}

void init_navigation(GtkWidget *container)
{
  GtkWidget *widget;

  // Adding the eventbox
  widget = gtk_event_box_new();
  gtk_box_pack_start(GTK_BOX(container), widget, FALSE, FALSE, 0);

  // Adding the expander
  container = widget;

  widget = gtk_expander_new(_("navigation"));
  darktable.gui->widgets.navigation_expander = widget;
  gtk_container_add(GTK_CONTAINER(container), widget);
  gtk_widget_set_can_focus(widget, TRUE);
  gtk_widget_set_no_show_all(widget, TRUE);

  // Adding the drawing surface
  container = widget;

  widget = gtk_drawing_area_new();
  darktable.gui->widgets.navigation = widget;
  gtk_container_add(GTK_CONTAINER(container), widget);
  gtk_widget_set_events(widget,
                        GDK_EXPOSURE_MASK
                        | GDK_POINTER_MOTION_MASK
                        | GDK_POINTER_MOTION_HINT_MASK
                        | GDK_BUTTON_PRESS_MASK
                        | GDK_BUTTON_RELEASE_MASK
                        | GDK_STRUCTURE_MASK);
  gtk_widget_show(widget);
}

void init_left(GtkWidget *container)
{

  GtkWidget *widget;

  // Adding the alignment
  widget = gtk_alignment_new(.5, .5, 1, 1);
  darktable.gui->widgets.left = widget;
  gtk_widget_set_name(widget, "left");
  gtk_alignment_set_padding(GTK_ALIGNMENT(widget), 0, 0, 5, 0);
  gtk_table_attach(GTK_TABLE(container), widget, 1, 2, 1, 2,
                   GTK_SHRINK, GTK_EXPAND | GTK_SHRINK | GTK_FILL, 0, 0);
  gtk_widget_show(widget);

  // Adding the vbox
  container = widget;

  widget = gtk_vbox_new(FALSE, 10);
  darktable.gui->widgets.left_vbox = widget;
  gtk_container_add(GTK_CONTAINER(container), widget);
  gtk_widget_set_size_request(widget, 0, -1);

  // Initializing the sub-sections
  container = widget;

  init_navigation(container);
  init_left_scroll_window(container);
  init_jobs_list(container);

}

void init_history_box(GtkWidget *container)
{
  GtkWidget *widget;

  // Adding the event box
  widget = gtk_event_box_new();
  darktable.gui->widgets.history_eventbox = widget;
  gtk_widget_set_name(widget, "history_eventbox");
  gtk_box_pack_start(GTK_BOX(container), widget, FALSE, FALSE, 0);
  gtk_widget_set_no_show_all(widget, TRUE);

  // Adding the expander
  container = widget;

  widget = gtk_expander_new(_("history"));
  darktable.gui->widgets.history_expander = widget;
  gtk_container_add(GTK_CONTAINER(container), widget);
  gtk_widget_set_can_focus(widget, TRUE);
  gtk_expander_set_spacing(GTK_EXPANDER(widget), 10);
  gtk_widget_show(widget);

  // Adding the alignment
  container = widget;

  widget = gtk_alignment_new(.5, .5, 1, 1);
  gtk_container_add(GTK_CONTAINER(container), widget);
  gtk_alignment_set_padding(GTK_ALIGNMENT(widget), 0, 10, 5, 10);
  gtk_widget_show(widget);

  // Adding the history body
  container = widget;

  widget = gtk_vbox_new(FALSE, 0);
  darktable.gui->widgets.history_expander_body = widget;
  gtk_container_add(GTK_CONTAINER(container), widget);
  gtk_widget_set_events(widget, GDK_EXPOSURE_MASK | GDK_STRUCTURE_MASK);
  gtk_widget_show(widget);

}

void init_info_box(GtkWidget *container)
{
  int i;
  GtkWidget *widget;

  GtkWidget** widgets[] =
  {
    &darktable.gui->widgets.metadata_label_filename,
    &darktable.gui->widgets.metadata_label_model,
    &darktable.gui->widgets.metadata_label_maker,
    &darktable.gui->widgets.metadata_label_aperture,
    &darktable.gui->widgets.metadata_label_exposure,
    &darktable.gui->widgets.metadata_label_focal_length,
    &darktable.gui->widgets.metadata_label_focus_distance,
    &darktable.gui->widgets.metadata_label_iso,
    &darktable.gui->widgets.metadata_label_datetime,
    &darktable.gui->widgets.metadata_label_lens,
    &darktable.gui->widgets.metadata_label_width,
    &darktable.gui->widgets.metadata_label_height,
    &darktable.gui->widgets.metadata_label_filmroll,
    &darktable.gui->widgets.metadata_label_title,
    &darktable.gui->widgets.metadata_label_creator,
    &darktable.gui->widgets.metadata_label_rights
  };

  gchar* labels[] =
  {
    _("filename"),
    _("model"),
    _("maker"),
    _("aperture"),
    _("exposure"),
    _("f-length"),
    _("distance"),
    _("iso"),
    _("date/time"),
    _("lens"),
    _("width"),
    _("height"),
    _("film roll"),
    _("title"),
    _("creator"),
    _("rights")
  };

  (void)widgets;
  (void)labels;

  // Adding the event box
  widget = gtk_event_box_new();
  gtk_widget_set_name(widget, "metadata_eventbox");
  gtk_box_pack_start(GTK_BOX(container), widget, FALSE, FALSE, 0);
  gtk_widget_show(widget);

  // Adding the expander
  container = widget;

  widget = gtk_expander_new(_("image information"));
  darktable.gui->widgets.metadata_expander = widget;
  gtk_container_add(GTK_CONTAINER(container), widget);
  gtk_expander_set_spacing(GTK_EXPANDER(widget), 10);
  gtk_widget_set_can_focus(widget, TRUE);
  gtk_widget_show(widget);

  // Adding the table
  container = widget;

  widget = gtk_table_new(16, 2, FALSE);
  gtk_container_add(GTK_CONTAINER(container), widget);
  gtk_widget_set_events(widget, GDK_EXPOSURE_MASK | GDK_STRUCTURE_MASK);
  gtk_container_set_border_width(GTK_CONTAINER(widget), 5);
  gtk_widget_show(widget);

  // Attaching the information labels
  container = widget;

  for(i = 0; i < 16; i++)
  {
    // Attaching the field title
    widget = gtk_label_new(labels[i]);
    gtk_table_attach(GTK_TABLE(container), widget, 0, 1, i, i + 1,
                     GTK_FILL, GTK_EXPAND | GTK_FILL, 0, 0);
    gtk_misc_set_alignment(GTK_MISC(widget), 0, 0.5);
    gtk_misc_set_padding(GTK_MISC(widget), 5, 0);
    gtk_widget_show(widget);

    // Attaching the label to hold the information
    widget = gtk_label_new(_("-"));
    *(widgets[i]) = widget;
    gtk_table_attach(GTK_TABLE(container), widget, 1, 2, i, i + 1,
                     GTK_EXPAND | GTK_FILL, GTK_EXPAND | GTK_FILL, 0, 0);
    gtk_misc_set_alignment(GTK_MISC(widget), 0, 0.5);
    gtk_widget_show(widget);
  }


}

void init_snapshots(GtkWidget *container)
{
  GtkWidget *widget;

  // Adding the event box
  widget = gtk_event_box_new();
  darktable.gui->widgets.snapshots_eventbox = widget;
  gtk_widget_set_name(widget, "snapshots_eventbox");
  gtk_widget_set_no_show_all(widget, TRUE);
  gtk_box_pack_start(GTK_BOX(container), widget, FALSE, FALSE, 0);
  gtk_widget_show(widget);

  // Adding the expander
  container = widget;

  widget = gtk_expander_new(_("snapshots"));
  darktable.gui->widgets.snapshots_expander = widget;
  gtk_container_add(GTK_CONTAINER(container), widget);
  gtk_expander_set_spacing(GTK_EXPANDER(widget), 10);
  gtk_widget_set_can_focus(widget, TRUE);
  gtk_widget_show(widget);

  // Adding the alignment
  container = widget;

  widget = gtk_alignment_new(.5, .5, 1, 1);
  gtk_container_add(GTK_CONTAINER(container), widget);
  gtk_alignment_set_padding(GTK_ALIGNMENT(widget), 0, 10, 5, 10);
  gtk_widget_show(widget);

  // Adding the snapshots body
  container = widget;

  widget = gtk_vbox_new(FALSE, 0);
  darktable.gui->widgets.snapshots_body = widget;
  gtk_container_add(GTK_CONTAINER(container), widget);
  gtk_widget_show(widget);

  gtk_widget_hide(darktable.gui->widgets.snapshots_eventbox);
}

void init_import(GtkWidget *container)
{
  GtkWidget *widget;

  // Adding the event box
  widget = gtk_event_box_new();
  darktable.gui->widgets.import_eventbox = widget;
  gtk_widget_set_name(widget, "import_eventbox");
  gtk_box_pack_start(GTK_BOX(container), widget, FALSE, FALSE, 0);
  gtk_widget_show(widget);

  // Adding the expander
  container = widget;

  widget = gtk_expander_new(C_("global import", "import"));
  darktable.gui->widgets.import_expander = widget;
  gtk_widget_set_can_focus(widget, TRUE);
  gtk_expander_set_expanded(GTK_EXPANDER(widget), TRUE);
  gtk_expander_set_spacing(GTK_EXPANDER(widget), 10);
  gtk_container_add(GTK_CONTAINER(container), widget);
  gtk_widget_show(widget);

  // Adding the alignment
  container = widget;

  widget = gtk_alignment_new(.5, .5, 1, 1);
  gtk_alignment_set_padding(GTK_ALIGNMENT(widget), 0, 10, 5, 10);
  gtk_container_add(GTK_CONTAINER(container), widget);
  gtk_widget_show(widget);

  // Adding the inner vbox
  container = widget;

  widget = gtk_vbox_new(FALSE, 5);
  gtk_container_add(GTK_CONTAINER(container), widget);
  gtk_widget_show(widget);

  container = widget;

  // Adding the single import button
  widget = gtk_button_new_with_label(_("image"));
  gtk_button_set_alignment(GTK_BUTTON(widget), 0.05, 5);
  gtk_widget_set_tooltip_text(widget, _("select one or more images to import"));
  gtk_widget_set_can_focus(widget, TRUE);
  gtk_widget_set_receives_default(widget, TRUE);
  gtk_box_pack_start(GTK_BOX(container), widget, TRUE, TRUE, 0);
  gtk_widget_show(widget);

  g_signal_connect (G_OBJECT (widget), "clicked",
                    G_CALLBACK (import_image_button_clicked),
                    NULL);

  // Adding the import button
  widget = gtk_button_new_with_label(_("folder"));
  gtk_button_set_alignment(GTK_BUTTON(widget), 0.05, 5);
  gtk_widget_set_tooltip_text(widget,
                              _("select a folder to import as film roll"));
  gtk_widget_set_can_focus(widget, TRUE);
  gtk_widget_set_receives_default(widget, TRUE);
  gtk_box_pack_start(GTK_BOX(container), widget, FALSE, FALSE, 0);
  gtk_widget_show(widget);

  g_signal_connect (G_OBJECT (widget), "clicked",
                    G_CALLBACK (import_button_clicked),
                    NULL);

  // Adding the devices expander
  widget = gtk_vbox_new(FALSE, 5);
  darktable.gui->widgets.devices_expander_body = widget;
  gtk_box_pack_start(GTK_BOX(container), widget, TRUE, TRUE, 0);
  gtk_widget_set_events(widget, GDK_EXPOSURE_MASK);
  gtk_widget_show(widget);
}

void init_left_scroll_window(GtkWidget *container)
{
  GtkWidget *widget;

  // Adding the outer scrolled window
  widget = gtk_scrolled_window_new(GTK_ADJUSTMENT(gtk_adjustment_new(0,
                                                                     0,
                                                                     100,
                                                                     1,
                                                                     10,
                                                                     10)),
                                   GTK_ADJUSTMENT(gtk_adjustment_new(0,
                                                                     0,
                                                                     100,
                                                                     1,
                                                                     10,
                                                                     10)));
  gtk_widget_set_can_focus(widget, TRUE);
  gtk_scrolled_window_set_placement(GTK_SCROLLED_WINDOW(widget),
                                    GTK_CORNER_TOP_RIGHT);
  darktable.gui->widgets.left_scrolled_window = widget;
  gtk_box_pack_start(GTK_BOX(container), widget, TRUE, TRUE, 0);
  gtk_widget_show(widget);

  // Adding the viewport
  container = widget;

  widget = gtk_viewport_new(GTK_ADJUSTMENT(gtk_adjustment_new(0,
                                                              0,
                                                              100,
                                                              1,
                                                              10,
                                                              10)),
                            GTK_ADJUSTMENT(gtk_adjustment_new(0,
                                                              0,
                                                              100,
                                                              1,
                                                              10,
                                                              10)));
  gtk_viewport_set_shadow_type(GTK_VIEWPORT(widget), GTK_SHADOW_NONE);
  gtk_container_set_resize_mode(GTK_CONTAINER(widget), GTK_RESIZE_QUEUE);
  gtk_container_add(GTK_CONTAINER(container), widget);
  gtk_widget_show(widget);

  // Adding the main vbox
  container = widget;

  widget = gtk_vbox_new(FALSE, 10);
  darktable.gui->widgets.left_scrolled = widget;
  gtk_widget_set_size_request(widget, 0, -1);
  gtk_container_add(GTK_CONTAINER(container), widget);
  gtk_widget_show(widget);

  container = widget;

  // Initializing the import controls
  init_import(container);

  // Initializing the left-side plugins box
  widget = gtk_vbox_new(FALSE, 10);
  darktable.gui->widgets.plugins_vbox_left = widget;
  gtk_widget_set_name(widget, "plugins_vbox_left");
  gtk_box_pack_start(GTK_BOX(container), widget, FALSE, TRUE, 0);
  gtk_widget_show(widget);

  // Initializing the snapshots box
  init_snapshots(container);

  // Initializing the image information box
  init_info_box(container);

  // Initializing the history box
  init_history_box(container);

  // Adding the left end marker
  widget = gtk_drawing_area_new();
  darktable.gui->widgets.endmarker_left = widget;
  gtk_box_pack_start(GTK_BOX(container), widget, FALSE, TRUE, 0);
  gtk_widget_set_size_request(widget, -1, 50);
  gtk_widget_show(widget);
}

void init_jobs_list(GtkWidget *container)
{
  GtkWidget *widget;

  // Adding the outer event box
  widget = gtk_event_box_new();
  gtk_widget_set_name(widget, "background_job_eventbox");
  gtk_box_pack_start(GTK_BOX(container), widget, FALSE, FALSE, 0);

  // Adding the content vbox
  container = widget;
  widget = gtk_vbox_new(FALSE, 0);
  darktable.gui->widgets.jobs_content_box = widget;
  gtk_widget_set_no_show_all(widget, TRUE);
  gtk_container_set_border_width(GTK_CONTAINER(widget), 5);
  gtk_container_add(GTK_CONTAINER(container), widget);
}

void init_right(GtkWidget *container)
{
  GtkWidget* widget;

  // Attaching the outer GtkAlignment
  widget = gtk_alignment_new(.5, .5, 1, 1);
  gtk_widget_set_name(widget, "right");
  gtk_alignment_set_padding(GTK_ALIGNMENT(widget), 0, 0, 0, 5);
  darktable.gui->widgets.right = widget;
  gtk_table_attach(GTK_TABLE(container), widget, 3, 4, 1, 2,
                   GTK_SHRINK,
                   GTK_SHRINK | GTK_EXPAND | GTK_FILL,
                   0, 0);
  gtk_widget_show(widget);

  // Adding the inner vbox
  container = widget;

  widget = gtk_vbox_new(FALSE, 10);
  darktable.gui->widgets.right_vbox = widget;
  gtk_widget_set_size_request(widget, 0, -1);
  gtk_container_add(GTK_CONTAINER(container), widget);
  gtk_widget_show(widget);

  // Initializing each section of the right side
  container = widget;

  init_histogram(container);
  init_module_groups(container);
  init_plugins(container);
  init_module_list(container);

}

void init_histogram(GtkWidget *container)
{
  GtkWidget* widget;

  // Creating the outer event box
  widget = gtk_event_box_new();
  gtk_box_pack_start(GTK_BOX(container), widget, FALSE, FALSE, 0);
  gtk_widget_show(widget);

  // Creating the expander
  container = widget;

  widget = gtk_expander_new(_("histogram"));
  darktable.gui->widgets.histogram_expander = widget;
  gtk_container_add(GTK_CONTAINER(container), widget);
  gtk_widget_set_no_show_all(widget, TRUE);
  gtk_widget_set_can_focus(widget, TRUE);

  // Creating the histogram surface
  container = widget;

  widget = gtk_drawing_area_new();
  darktable.gui->widgets.histogram = widget;
  gtk_container_add(GTK_CONTAINER(container), widget);
  gtk_widget_set_events(widget,
                        GDK_EXPOSURE_MASK
                        | GDK_POINTER_MOTION_MASK
                        | GDK_POINTER_MOTION_HINT_MASK
                        | GDK_BUTTON_PRESS_MASK
                        | GDK_BUTTON_RELEASE_MASK
                        | GDK_LEAVE_NOTIFY_MASK
                        | GDK_STRUCTURE_MASK);
  gtk_widget_show(widget);
}

void init_module_groups(GtkWidget *container)
{
  GtkWidget* widget;

  widget = gtk_event_box_new();
  gtk_box_pack_start(GTK_BOX(container), widget, FALSE, FALSE, 0);
  darktable.gui->widgets.modulegroups_eventbox = widget;

  gtk_widget_show(widget);
}

void init_plugins(GtkWidget *container)
{
  GtkWidget* widget;

  // Creating the outer scrolled window
  widget = gtk_scrolled_window_new(GTK_ADJUSTMENT(gtk_adjustment_new(0,
                                                                     0,
                                                                     100,
                                                                     1,
                                                                     10,
                                                                     10)),
                                   GTK_ADJUSTMENT(gtk_adjustment_new(0,
                                                                     0,
                                                                     100,
                                                                     1,
                                                                     10,
                                                                     10)));
  darktable.gui->widgets.right_scrolled_window = widget;
  gtk_widget_set_can_focus(widget, TRUE);
  gtk_box_pack_start(GTK_BOX(container), widget, TRUE, TRUE, 0);
  gtk_widget_show(widget);

  // Creating the viewport
  container = widget;

  widget = gtk_viewport_new(GTK_ADJUSTMENT(gtk_adjustment_new(0,
                                                              0,
                                                              100,
                                                              1,
                                                              10,
                                                              10)),
                            GTK_ADJUSTMENT(gtk_adjustment_new(0,
                                                              0,
                                                              100,
                                                              1,
                                                              10,
                                                              10)));
  gtk_viewport_set_shadow_type(GTK_VIEWPORT(widget), GTK_SHADOW_NONE);
  gtk_container_set_resize_mode(GTK_CONTAINER(widget), GTK_RESIZE_QUEUE);
  gtk_container_add(GTK_CONTAINER(container), widget);
  gtk_widget_show(widget);

  // Creating the innermost vbox
  container = widget;

  widget = gtk_vbox_new(FALSE, 10);
  darktable.gui->widgets.plugins_vbox = widget;
  gtk_widget_set_name(widget, "plugins_vbox");
  gtk_widget_set_size_request(widget, 0, -1);
  gtk_container_add(GTK_CONTAINER(container), widget);
  gtk_widget_show(widget);
}

void init_module_list(GtkWidget *container)
{
  GtkWidget* widget;

  // Adding the event box
  widget = gtk_event_box_new();
  gtk_widget_set_name(widget, "module_list_eventbox");
  gtk_widget_set_no_show_all(widget, TRUE);
  gtk_box_pack_start(GTK_BOX(container), widget, FALSE, FALSE, 0);
  darktable.gui->widgets.module_list_eventbox = widget;

  // Adding the expander
  container = widget;
  widget = gtk_expander_new(_("more plugins"));
  gtk_container_add(GTK_CONTAINER(container), widget);
  gtk_widget_show(widget);

  // Adding the grid
  container = widget;
  widget = gtk_table_new(2, 6, TRUE);
  gtk_container_add(GTK_CONTAINER(container), widget);
  darktable.gui->widgets.module_list = widget;
  gtk_widget_show(widget);
}

void init_center(GtkWidget *container)
{
  GtkWidget* widget;

  // Adding the center drawing area
  widget = gtk_drawing_area_new();
  gtk_box_pack_start(GTK_BOX(container), widget, TRUE, TRUE, 0);
  darktable.gui->widgets.center = widget;

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

  // Initializing the bottom center
  widget = gtk_hbox_new(FALSE, 0);
  gtk_box_pack_start(GTK_BOX(container), widget, FALSE, TRUE, 0);
  darktable.gui->widgets.bottom = widget;

  init_center_bottom(widget);

}

void init_center_bottom(GtkWidget *container)
{
  GtkWidget* widget;
  GtkWidget* subcontainer;

  // Adding the left toolbox
  widget = gtk_hbox_new(FALSE, 0);
  gtk_box_pack_start(GTK_BOX(container), widget, TRUE, TRUE, 0);
  gtk_widget_show(widget);
  darktable.gui->widgets.bottom_left_toolbox = widget;

  // Adding the center box
  subcontainer = gtk_vbox_new(FALSE, 0);
  gtk_box_pack_start(GTK_BOX(container), subcontainer, FALSE, TRUE, 0);

  // Initializing the color picker panel
  widget = gtk_hbox_new(FALSE, 5);
  darktable.gui->widgets.bottom_darkroom_box = widget;
  gtk_box_pack_start(GTK_BOX(subcontainer), widget, TRUE, TRUE, 0);
  init_colorpicker(widget);

  // Initializing the lightable layout box
  widget = gtk_hbox_new(FALSE, 5);
  darktable.gui->widgets.bottom_lighttable_box = widget;
  gtk_box_pack_start(GTK_BOX(subcontainer), widget, TRUE, TRUE, 0);
  init_lighttable_box(widget);
  gtk_widget_show(widget);

  // Adding the right toolbox
  widget = gtk_hbox_new(FALSE, 0);
  gtk_box_pack_start(GTK_BOX(container), widget, TRUE, TRUE, 0);
  gtk_widget_show(widget);
  darktable.gui->widgets.bottom_right_toolbox = widget;

}

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
