/*
    This file is part of darktable,
    copyright (c) 2010 henrik andersson.

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

/** this is the view for the capture module.  
  The capture module purpose is to allow a workflow for capturing images
  which is module extendable but main purpos is to support tethered capture
  using gphoto library.
  
  Captures are directly added to a capture film roll in the library
  
*/

#include "views/view.h"
#include "libs/lib.h"
#include "control/jobs.h"
#include "control/settings.h"
#include "control/control.h"
#include "control/conf.h"
#include "common/image_cache.h"
#include "common/darktable.h"
#include "gui/gtk.h"
#include "gui/draw.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <math.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <assert.h>
#include <gdk/gdkkeysyms.h>

DT_MODULE(1)

typedef enum dt_capture_mode_t
{
  DT_CAPTURE_MODE_TETHERED          // Only one capture mode to start with...
}
dt_capture_mode_t;

/** data for the capture view */
typedef struct dt_capture_t
{
  dt_capture_mode_t mode;
}
dt_capture_t;

const char *name(dt_view_t *self)
{
  return _("capture");
}

void init(dt_view_t *self)
{
  self->data = malloc(sizeof(dt_capture_t));
  dt_capture_t *lib = (dt_capture_t *)self->data;
  // initialize capture data struct
  const int i = dt_conf_get_int("plugins/capture/mode");
  lib->mode = i;
}

void cleanup(dt_view_t *self)
{
  free(self->data);
}

void _expose_tethered_mode(dt_view_t *self, cairo_t *cr, int32_t width, int32_t height, int32_t pointerx, int32_t pointery)
{
  
}

void expose(dt_view_t *self, cairo_t *cr, int32_t width, int32_t height, int32_t pointerx, int32_t pointery)
{
  // Clean background
  cairo_set_source_rgb(cr, darktable.gui->bgcolor[0], darktable.gui->bgcolor[1], darktable.gui->bgcolor[2]);
  cairo_paint(cr);
  
  const int i = dt_conf_get_int("plugins/capture/layout");
  switch(i)
  {
    case DT_CAPTURE_MODE_TETHERED: // tethered mode
    default: 
      _expose_tethered_mode(self, cr, width, height, pointerx, pointery);
      break;
  }
}

void enter(dt_view_t *self)
{
  // add expanders
  GtkBox *box = GTK_BOX(glade_xml_get_widget (darktable.gui->main_window, "plugins_vbox"));
  /*GList *modules = g_list_last(darktable.lib->plugins);
  while(modules)
  {
    dt_lib_module_t *module = (dt_lib_module_t *)(modules->data);
    module->gui_init(module);
    // add the widget created by gui_init to an expander and both to list.
    GtkWidget *expander = dt_lib_gui_get_expander(module);
    gtk_box_pack_start(box, expander, FALSE, FALSE, 0);
    modules = g_list_previous(modules);
  }*/

  // end marker widget:
  GtkWidget *endmarker = gtk_drawing_area_new();
  gtk_widget_set_size_request(GTK_WIDGET(endmarker), 250, 50);
  gtk_box_pack_start(box, endmarker, FALSE, FALSE, 0);
  g_signal_connect (G_OBJECT (endmarker), "expose-event",
                    G_CALLBACK (dt_control_expose_endmarker), 0);

  gtk_widget_show_all(GTK_WIDGET(box));

  // close expanders
  /*modules = darktable.lib->plugins;
  while(modules)
  {
    dt_lib_module_t *module = (dt_lib_module_t *)(modules->data);
    char var[1024];
    snprintf(var, 1024, "plugins/lighttable/%s/expanded", module->plugin_name);
    gboolean expanded = dt_conf_get_bool(var);
    gtk_expander_set_expanded (module->expander, expanded);
    if(expanded) gtk_widget_show_all(module->widget);
    else         gtk_widget_hide_all(module->widget);
    modules = g_list_next(modules);
  }
  dt_gui_key_accel_register(GDK_MOD1_MASK, GDK_1, star_key_accel_callback, (void *)DT_LIB_STAR_1);
  dt_gui_key_accel_register(GDK_MOD1_MASK, GDK_2, star_key_accel_callback, (void *)DT_LIB_STAR_2);
  dt_gui_key_accel_register(GDK_MOD1_MASK, GDK_3, star_key_accel_callback, (void *)DT_LIB_STAR_3);
  dt_gui_key_accel_register(GDK_MOD1_MASK, GDK_4, star_key_accel_callback, (void *)DT_LIB_STAR_4);
  dt_gui_key_accel_register(GDK_CONTROL_MASK, GDK_BackSpace, star_key_accel_callback, (void *)666);*/
  
}


void leave(dt_view_t *self)
{
 // dt_gui_key_accel_unregister(star_key_accel_callback);
  /*GList *it = darktable.lib->plugins;
  while(it)
  {
    dt_lib_module_t *module = (dt_lib_module_t *)(it->data);
    module->gui_cleanup(module);
    it = g_list_next(it);
  }
  GtkBox *box = GTK_BOX(glade_xml_get_widget (darktable.gui->main_window, "plugins_vbox"));
  gtk_container_foreach(GTK_CONTAINER(box), (GtkCallback)dt_lib_remove_child, (gpointer)box);*/
  
}

void reset(dt_view_t *self)
{
  dt_capture_t *lib = (dt_capture_t *)self->data;
  lib->mode=DT_CAPTURE_MODE_TETHERED;
  //DT_CTL_SET_GLOBAL(lib_image_mouse_over_id, -1);
}


void mouse_leave(dt_view_t *self)
{
  /*dt_library_t *lib = (dt_library_t *)self->data;
  if(!lib->pan && dt_conf_get_int("plugins/lighttable/images_in_row") != 1)
  {
    DT_CTL_SET_GLOBAL(lib_image_mouse_over_id, -1);
    dt_control_queue_draw_all(); // remove focus
  }*/
}


void mouse_moved(dt_view_t *self, double x, double y, int which)
{
  // update stars/etc :(
  //dt_control_queue_draw_all();
}


int button_released(dt_view_t *self, double x, double y, int which, uint32_t state)
{
  /*dt_library_t *lib = (dt_library_t *)self->data;
  lib->pan = 0;
  if(which == 1) dt_control_change_cursor(GDK_ARROW);*/
  return 1;
}


int button_pressed(dt_view_t *self, double x, double y, int which, int type, uint32_t state)
{
  /*dt_library_t *lib = (dt_library_t *)self->data;
  lib->modifiers = state;
  lib->button = which;
  lib->select_offset_x = lib->zoom_x;
  lib->select_offset_y = lib->zoom_y;
  lib->select_offset_x += x;
  lib->select_offset_y += y;
  lib->pan = 1;
  if(which == 1) dt_control_change_cursor(GDK_HAND1);
  if(which == 1 && type == GDK_2BUTTON_PRESS) return 0;
  // image button pressed?
  switch(lib->image_over)
  {
    case DT_LIB_DESERT: break;
    case DT_LIB_STAR_1: case DT_LIB_STAR_2: case DT_LIB_STAR_3: case DT_LIB_STAR_4:
    { 
      int32_t mouse_over_id;
      DT_CTL_GET_GLOBAL(mouse_over_id, lib_image_mouse_over_id);
      dt_image_t *image = dt_image_cache_get(mouse_over_id, 'r');
      if(lib->image_over == DT_LIB_STAR_1 && ((image->flags & 0x7) == 1)) image->flags &= ~0x7;
      else
      {
        image->flags &= ~0x7;
        image->flags |= lib->image_over;
      }
      dt_image_cache_flush(image);
      dt_image_cache_release(image, 'r');
      break;
    }
    default:
      return 0;
  }*/
  return 1;
}


int key_pressed(dt_view_t *self, uint16_t which)
{
  /*dt_library_t *lib = (dt_library_t *)self->data;
  GtkWidget *widget = glade_xml_get_widget (darktable.gui->main_window, "lighttable_zoom_spinbutton");
  int zoom = dt_conf_get_int("plugins/lighttable/images_in_row");
  const int layout = dt_conf_get_int("plugins/lighttable/layout");
  switch (which)
  {
    case KEYCODE_Left: case KEYCODE_a:
      if(layout == 1 && zoom == 1) lib->track = -DT_LIBRARY_MAX_ZOOM;
      else lib->track = -1;
      break;
    case KEYCODE_Right: case KEYCODE_e:
      if(layout == 1 && zoom == 1) lib->track = DT_LIBRARY_MAX_ZOOM;
      else lib->track = 1;
      break;
    case KEYCODE_Up: case KEYCODE_comma:
      lib->track = -DT_LIBRARY_MAX_ZOOM;
      break;
    case KEYCODE_Down: case KEYCODE_o:
      lib->track = DT_LIBRARY_MAX_ZOOM;
      break;
    case KEYCODE_1:
      zoom = 1;
      break;
    case KEYCODE_2:
      if(zoom <= 1) zoom = 1;
      else zoom --;
      if(layout == 0) lib->center = 1;
      break;
    case KEYCODE_3:
      if(zoom >= 2*DT_LIBRARY_MAX_ZOOM) zoom = 2*DT_LIBRARY_MAX_ZOOM;
      else zoom ++;
      if(layout == 0) lib->center = 1;
      break;
    case KEYCODE_4:
      zoom = DT_LIBRARY_MAX_ZOOM;
      break;
    case KEYCODE_apostrophe:
      lib->center = 1;
      break;
    default:
      return 0;
  }
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(widget), zoom);*/
  return 1;
}

void border_scrolled(dt_view_t *view, double x, double y, int which, int up)
{
  /*dt_library_t *lib = (dt_library_t *)view->data;
  if(which == 0 || which == 1)
  {
    if(up) lib->track = -DT_LIBRARY_MAX_ZOOM;
    else   lib->track =  DT_LIBRARY_MAX_ZOOM;
  }
  else if(which == 2 || which == 3)
  {
    if(up) lib->track = -1;
    else   lib->track =  1;
  }*/
  dt_control_queue_draw_all();
}

void scrolled(dt_view_t *view, double x, double y, int up)
{
  /*dt_library_t *lib = (dt_library_t *)view->data;
  GtkWidget *widget = glade_xml_get_widget (darktable.gui->main_window, "lighttable_zoom_spinbutton");
  const int layout = dt_conf_get_int("plugins/lighttable/layout");
  if(layout == 1)
  {
    if(up) lib->track = -DT_LIBRARY_MAX_ZOOM;
    else   lib->track =  DT_LIBRARY_MAX_ZOOM;
  }
  else
  { // zoom
    int zoom = dt_conf_get_int("plugins/lighttable/images_in_row");
    if(up)
    {
      zoom--;
      if(zoom < 1) zoom = 1;
    }
    else
    {
      zoom++;
      if(zoom > 2*DT_LIBRARY_MAX_ZOOM) zoom = 2*DT_LIBRARY_MAX_ZOOM;
    }
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(widget), zoom);
  }*/
}

