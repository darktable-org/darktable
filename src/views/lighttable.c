/*
    This file is part of darktable,
    copyright (c) 2009--2010 johannes hanika.

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
/** this is the view for the lighttable module.  */
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

#define DT_LIBRARY_MAX_ZOOM 13


/**
 * this organises the whole library:
 * previously imported film rolls..
 */
typedef struct dt_library_t
{
  // tmp mouse vars:
  float select_offset_x, select_offset_y;
  int32_t last_selected_id;
  int button;
  uint32_t modifiers;
  uint32_t center, pan;
  int32_t track, offset, first_visible_zoomable, first_visible_filemanager;
  float zoom_x, zoom_y;
  dt_view_image_over_t image_over;
}
dt_library_t;

const char *name(dt_view_t *self)
{
  return _("lighttable");
}

void init(dt_view_t *self)
{
  self->data = malloc(sizeof(dt_library_t));
  dt_library_t *lib = (dt_library_t *)self->data;
  lib->select_offset_x = lib->select_offset_y = 0.5f;
  lib->last_selected_id = -1;
  lib->first_visible_zoomable = lib->first_visible_filemanager = 0;
  lib->button = 0;
  lib->modifiers = 0;
  lib->center = lib->pan = lib->track = 0;
  lib->zoom_x = 0.0f;
  lib->zoom_y = 0.0f;
}


void cleanup(dt_view_t *self)
{
  free(self->data);
}


static void
expose_filemanager (dt_view_t *self, cairo_t *cr, int32_t width, int32_t height, int32_t pointerx, int32_t pointery)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  const int iir = dt_conf_get_int("plugins/lighttable/images_in_row");
  lib->image_over = DT_VIEW_DESERT;
  int32_t mouse_over_id;
  DT_CTL_GET_GLOBAL(mouse_over_id, lib_image_mouse_over_id);
  DT_CTL_SET_GLOBAL(lib_image_mouse_over_id, -1);
  cairo_set_source_rgb (cr, .9, .9, .9);
  cairo_paint(cr);

  // zoom to one case:
  static int oldzoom = -1;
  static int firstsel = -1;

  if(lib->first_visible_zoomable >= 0)
  {
    lib->offset = lib->first_visible_zoomable;
  }
  lib->first_visible_zoomable = -1;

  if(iir == 1 && oldzoom != 1 && firstsel >= 0)
    lib->offset = firstsel;
  oldzoom = iir;
  firstsel = -1;

  if(lib->track >  2) lib->offset += iir;
  if(lib->track < -2) lib->offset -= iir;
  lib->track = 0;
  if(lib->center) lib->offset = 0;
  lib->center = 0;
  int offset = lib->offset;
  lib->first_visible_filemanager = offset;
  static int oldpan = 0;
  const int pan = lib->pan;

  const float wd = width/(float)iir;
  const float ht = width/(float)iir;

  const int seli = pointerx / (float)wd;
  const int selj = pointery / (float)ht;

  const int img_pointerx = iir == 1 ? pointerx : fmodf(pointerx, wd);
  const int img_pointery = iir == 1 ? pointery : fmodf(pointery, ht);

  const int max_rows = 1 + (int)((height)/ht + .5);
  const int max_cols = iir;
  sqlite3_stmt *stmt = NULL;
  int id, last_seli = 1<<30, last_selj = 1<<30;
  int clicked1 = (oldpan == 0 && pan == 1 && lib->button == 1);

  gchar *query = dt_conf_get_string ("plugins/lighttable/query");
  if(!query) return;
  if(query[0] == '\0')
  {
    g_free(query);
    return;
  }
  char newquery[1024];
  snprintf(newquery, 1024, "select count(id) %s", query + 17);
  sqlite3_prepare_v2(darktable.db, newquery, -1, &stmt, NULL);
  sqlite3_bind_int (stmt, 1, 0);
  sqlite3_bind_int (stmt, 2, -1);
  int count = 1;
  if(sqlite3_step(stmt) == SQLITE_ROW)
    count = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  if(offset < 0)         lib->offset = offset = 0;
  if(offset > count-iir) lib->offset = offset = count-iir;
  dt_view_set_scrollbar(self, 0, 1, 1, offset, count, max_cols*iir);

  sqlite3_prepare_v2(darktable.db, query, -1, &stmt, NULL);
  sqlite3_bind_int (stmt, 1, offset);
  sqlite3_bind_int (stmt, 2, max_rows*iir);
  g_free(query);
  for(int row = 0; row < max_rows; row++)
  {
    for(int col = 0; col < max_cols; col++)
    {
      if(sqlite3_step(stmt) == SQLITE_ROW)
      {
        id = sqlite3_column_int(stmt, 0);
        dt_image_t *image = dt_image_cache_get(id, 'r');
        if(image)
        {
          // set mouse over id
          if(seli == col && selj == row)
          {
            firstsel = lib->offset + selj*iir + seli;
            mouse_over_id = image->id;
            DT_CTL_SET_GLOBAL(lib_image_mouse_over_id, mouse_over_id);
          }
          // add clicked image to selected table
          if(clicked1)
          {
            if((lib->modifiers & GDK_SHIFT_MASK) == 0 && (lib->modifiers & GDK_CONTROL_MASK) == 0 && seli == col && selj == row)
            { // clear selected if no modifier
              sqlite3_stmt *stmt2;
              sqlite3_prepare_v2(darktable.db, "delete from selected_images where imgid != ?1", -1, &stmt2, NULL);
              sqlite3_bind_int(stmt2, 1, image->id);
              sqlite3_step(stmt2);
              sqlite3_finalize(stmt2);
            }
            if((lib->modifiers & GDK_SHIFT_MASK) && image->id == lib->last_selected_id) { last_seli = col; last_selj = row; }
            if((last_seli < (1<<30) && ((lib->modifiers & GDK_SHIFT_MASK) && (col >= last_seli && row >= last_selj &&
                    col <= seli && row <= selj) && (col != last_seli || row != last_selj))) ||
               (seli == col && selj == row))
            { // insert all in range if shift, or only the one the mouse is over for ctrl or plain click.
              dt_view_toggle_selection(image->id);
              lib->last_selected_id = image->id;
            }
          }
          cairo_save(cr);
          if(iir == 1) dt_image_prefetch(image, DT_IMAGE_MIPF);
          dt_view_image_expose(image, &(lib->image_over), image->id, cr, wd, iir == 1 ? height : ht, iir, img_pointerx, img_pointery);
          cairo_restore(cr);
          dt_image_cache_release(image, 'r');
          if (iir == 1) goto failure; // only one image in one-image mode ;)
        }
      }
      else goto failure;
      cairo_translate(cr, wd, 0.0f);
    }
    cairo_translate(cr, -max_cols*wd, ht);
  }
failure:
  sqlite3_finalize(stmt);

  oldpan = pan;
#ifdef _DEBUG
  if(darktable.unmuted & DT_DEBUG_CACHE)
    dt_mipmap_cache_print(darktable.mipmap_cache);
#endif
}

static void
expose_zoomable (dt_view_t *self, cairo_t *cr, int32_t width, int32_t height, int32_t pointerx, int32_t pointery)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  float zoom, zoom_x, zoom_y;
  int32_t mouse_over_id, pan, track, center;
  DT_CTL_GET_GLOBAL(mouse_over_id, lib_image_mouse_over_id);
  zoom   = dt_conf_get_int("plugins/lighttable/images_in_row");
  zoom_x = lib->zoom_x;
  zoom_y = lib->zoom_y;
  pan    = lib->pan;
  center = lib->center;
  track  = lib->track;

  lib->image_over = DT_VIEW_DESERT;

  if(zoom == 1) cairo_set_source_rgb (cr, .8, .8, .8);
  else cairo_set_source_rgb (cr, .9, .9, .9);
  cairo_paint(cr);

  const float wd = width/zoom;
  const float ht = width/zoom;

  static int oldpan = 0;
  static float oldzoom = -1;
  if(oldzoom < 0) oldzoom = zoom;

  // TODO: exaggerate mouse gestures to pan when zoom == 1
  if(pan)// && mouse_over_id >= 0)
  {
    zoom_x = lib->select_offset_x - /* (zoom == 1 ? 2. : 1.)*/pointerx;
    zoom_y = lib->select_offset_y - /* (zoom == 1 ? 2. : 1.)*/pointery;
  }

  gchar *query = dt_conf_get_string ("plugins/lighttable/query");
  if(!query) return;
  if(query[0] == '\0')
  {
    g_free(query);
    return;
  }

  if     (track == 0);
  else if(track >  1)  zoom_y += ht;
  else if(track >  0)  zoom_x += wd;
  else if(track > -2)  zoom_x -= wd;
  else                 zoom_y -= ht;
  if(zoom > DT_LIBRARY_MAX_ZOOM)
  { // double speed.
    if     (track == 0);
    else if(track >  1)  zoom_y += ht;
    else if(track >  0)  zoom_x += wd;
    else if(track > -2)  zoom_x -= wd;
    else                 zoom_y -= ht;
    if(zoom > 1.5*DT_LIBRARY_MAX_ZOOM)
    { // quad speed.
      if     (track == 0);
      else if(track >  1)  zoom_y += ht;
      else if(track >  0)  zoom_x += wd;
      else if(track > -2)  zoom_x -= wd;
      else                 zoom_y -= ht;
    }
  }

  if(oldzoom != zoom)
  {
    float oldx = (pointerx + zoom_x)*oldzoom/width;
    float oldy = (pointery + zoom_y)*oldzoom/width;
    if(zoom == 1)
    {
      zoom_x = (int)oldx*wd;
      zoom_y = (int)oldy*ht;
      lib->offset = 0x7fffffff;
    }
    else
    {
      zoom_x = oldx*wd - pointerx;
      zoom_y = oldy*ht - pointery;
    }
  }
  oldzoom = zoom;

  // TODO: replace this with center on top of selected/developed image
  if(center)
  {
    if(mouse_over_id >= 0)
    {
      zoom_x = wd*((int)(zoom_x)/(int)wd);
      zoom_y = ht*((int)(zoom_y)/(int)ht);
    }
    else zoom_x = zoom_y = 0.0;
    center = 0;
  }

  // mouse left the area, but we leave mouse over as it was, especially during panning
  // if(!pan && pointerx > 0 && pointerx < width && pointery > 0 && pointery < height) DT_CTL_SET_GLOBAL(lib_image_mouse_over_id, -1);
  if(!pan && zoom != 1) DT_CTL_SET_GLOBAL(lib_image_mouse_over_id, -1);

  // set scrollbar positions, clamp zoom positions
  sqlite3_stmt *stmt = NULL;
  int rc;
  char newquery[1024];
  snprintf(newquery, 1024, "select count(id) %s", query + 17);
  sqlite3_prepare_v2(darktable.db, newquery, -1, &stmt, NULL);
  sqlite3_bind_int (stmt, 1, 0);
  sqlite3_bind_int (stmt, 2, -1);
  int count = 1;
  if(sqlite3_step(stmt) == SQLITE_ROW)
    count = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  if(count == 0)
  {
    zoom_x = zoom_y = 0.0f;
  }
  else if(zoom < 1.01)
  {
    if(zoom_x < 0)                         zoom_x = 0;
    if(zoom_x > wd*DT_LIBRARY_MAX_ZOOM-wd) zoom_x = wd*DT_LIBRARY_MAX_ZOOM-wd;
    if(zoom_y < 0)                         zoom_y = 0;
    if(zoom_y > ht*count/MIN(DT_LIBRARY_MAX_ZOOM, zoom)-ht)
                                           zoom_y =  ht*count/MIN(DT_LIBRARY_MAX_ZOOM, zoom)-ht;
  }
  else
  {
    if(zoom_x < -wd*DT_LIBRARY_MAX_ZOOM/2)  zoom_x = -wd*DT_LIBRARY_MAX_ZOOM/2;
    if(zoom_x >  wd*DT_LIBRARY_MAX_ZOOM-wd) zoom_x =  wd*DT_LIBRARY_MAX_ZOOM-wd;
    if(zoom_y < -height+ht)                 zoom_y = -height+ht;
    if(zoom_y >  ht*count/MIN(DT_LIBRARY_MAX_ZOOM, zoom)-ht)
                                            zoom_y =  ht*count/MIN(DT_LIBRARY_MAX_ZOOM, zoom)-ht;
  }

  int offset_i = (int)(zoom_x/wd);
  int offset_j = (int)(zoom_y/ht);
  if(lib->first_visible_filemanager >= 0)
  {
    offset_i = lib->first_visible_filemanager % DT_LIBRARY_MAX_ZOOM;
    offset_j = lib->first_visible_filemanager / DT_LIBRARY_MAX_ZOOM;
  }
  lib->first_visible_filemanager = -1;
  lib->first_visible_zoomable = offset_i + DT_LIBRARY_MAX_ZOOM*offset_j;
  // arbitrary 1000 to avoid bug due to round towards zero using (int)
  int seli = zoom == 1 ? 0 : (int)(1000 + (pointerx + zoom_x)/wd) - MAX(offset_i, 0) - 1000;
  int selj = zoom == 1 ? 0 : (int)(1000 + (pointery + zoom_y)/ht) - offset_j         - 1000;
  float offset_x = zoom == 1 ? 0.0 : zoom_x/wd - (int)(zoom_x/wd);
  float offset_y = zoom == 1 ? 0.0 : zoom_y/ht - (int)(zoom_y/ht);
  const int max_rows = zoom == 1 ? 1 : 2 + (int)((height)/ht + .5);
  const int max_cols = zoom == 1 ? 1 : MIN(DT_LIBRARY_MAX_ZOOM - MAX(0, offset_i), 1 + (int)(zoom+.5));

  int offset = MAX(0, offset_i) + DT_LIBRARY_MAX_ZOOM*offset_j;
  int img_pointerx = zoom == 1 ? pointerx : fmodf(pointerx + zoom_x, wd);
  int img_pointery = zoom == 1 ? pointery : fmodf(pointery + zoom_y, ht);

  // assure 1:1 is not switching images on resize/tab events:
  if(!track && lib->offset != 0x7fffffff && zoom == 1)
  {
    offset = lib->offset;
    zoom_x = wd*(offset % DT_LIBRARY_MAX_ZOOM);
    zoom_y = ht*(offset / DT_LIBRARY_MAX_ZOOM);
  }
  else lib->offset = offset;

  int id, clicked1, last_seli = 1<<30, last_selj = 1<<30;
  clicked1 = (oldpan == 0 && pan == 1 && lib->button == 1);

  dt_view_set_scrollbar(self, MAX(0, offset_i), DT_LIBRARY_MAX_ZOOM, zoom, DT_LIBRARY_MAX_ZOOM*offset_j, count, DT_LIBRARY_MAX_ZOOM*max_cols);

  sqlite3_prepare_v2(darktable.db, query, -1, &stmt, NULL);
  g_free(query);
  cairo_translate(cr, -offset_x*wd, -offset_y*ht);
  cairo_translate(cr, -MIN(offset_i*wd, 0.0), 0.0);
  for(int row = 0; row < max_rows; row++)
  {
    if(offset < 0)
    {
      cairo_translate(cr, 0, ht);
      offset += DT_LIBRARY_MAX_ZOOM;
      continue;
    }
    rc = sqlite3_bind_int  (stmt, 1, offset);
    rc = sqlite3_bind_int  (stmt, 2, max_cols);
    for(int col = 0; col < max_cols; col++)
    {
      if(sqlite3_step(stmt) == SQLITE_ROW)
      {
        id = sqlite3_column_int(stmt, 0);
        dt_image_t *image = dt_image_cache_get(id, 'r');
        if(image)
        {
          // printf("flags %d > k %d\n", image->flags, col);

          // set mouse over id
          if((zoom == 1 && mouse_over_id < 0) || ((!pan || track) && seli == col && selj == row))
          {
            mouse_over_id = image->id;
            DT_CTL_SET_GLOBAL(lib_image_mouse_over_id, mouse_over_id);
          }
          // add clicked image to selected table
          if(clicked1)
          {
            if((lib->modifiers & GDK_SHIFT_MASK) == 0 && (lib->modifiers & GDK_CONTROL_MASK) == 0 && seli == col && selj == row)
            { // clear selected if no modifier
              sqlite3_stmt *stmt2;
              sqlite3_prepare_v2(darktable.db, "delete from selected_images where imgid != ?1", -1, &stmt2, NULL);
              sqlite3_bind_int(stmt2, 1, image->id);
              sqlite3_step(stmt2);
              sqlite3_finalize(stmt2);
            }
            // FIXME: whatever comes first assumtion is broken!
            // if((lib->modifiers & GDK_SHIFT_MASK) && (last_seli == (1<<30)) &&
            //    (image->id == lib->last_selected_id || image->id == mouse_over_id)) { last_seli = col; last_selj = row; }
            // if(last_seli < (1<<30) && ((lib->modifiers & GDK_SHIFT_MASK) && (col >= MIN(last_seli,seli) && row >= MIN(last_selj,selj) &&
            //         col <= MAX(last_seli,seli) && row <= MAX(last_selj,selj)) && (col != last_seli || row != last_selj)) ||
            if((lib->modifiers & GDK_SHIFT_MASK) && image->id == lib->last_selected_id) { last_seli = col; last_selj = row; }
            if((last_seli < (1<<30) && ((lib->modifiers & GDK_SHIFT_MASK) && (col >= last_seli && row >= last_selj &&
                    col <= seli && row <= selj) && (col != last_seli || row != last_selj))) ||
               (seli == col && selj == row))
            { // insert all in range if shift, or only the one the mouse is over for ctrl or plain click.
              dt_view_toggle_selection(image->id);
              lib->last_selected_id = image->id;
            }
          }
          cairo_save(cr);
          if(zoom == 1) dt_image_prefetch(image, DT_IMAGE_MIPF);
          dt_view_image_expose(image, &(lib->image_over), image->id, cr, wd, zoom == 1 ? height : ht, zoom, img_pointerx, img_pointery);
          cairo_restore(cr);
          dt_image_cache_release(image, 'r');
        }
      }
      else goto failure;
      cairo_translate(cr, wd, 0.0f);
    }
    cairo_translate(cr, -max_cols*wd, ht);
    offset += DT_LIBRARY_MAX_ZOOM;
    rc = sqlite3_reset(stmt);
    rc = sqlite3_clear_bindings(stmt);
  }
failure:
  sqlite3_finalize(stmt);

  oldpan = pan;
  lib->zoom_x = zoom_x;
  lib->zoom_y = zoom_y;
  lib->track  = 0;
  lib->center = center;
#ifdef _DEBUG
  if(darktable.unmuted & DT_DEBUG_CACHE)
    dt_mipmap_cache_print(darktable.mipmap_cache);
#endif
}

void expose(dt_view_t *self, cairo_t *cr, int32_t width, int32_t height, int32_t pointerx, int32_t pointery)
{
  const int i = dt_conf_get_int("plugins/lighttable/layout");
  switch(i)
  {
    case 1: // file manager
      expose_filemanager(self, cr, width, height, pointerx, pointery);
      break;
    default: // zoomable
      expose_zoomable(self, cr, width, height, pointerx, pointery);
      break;
  }
}

static void
go_up_key_accel_callback(void *data)
{
  dt_view_t *self = (dt_view_t *)data;
  dt_library_t *lib = (dt_library_t *)self->data;
  lib->offset = 0;
  dt_control_queue_draw_all();
}

static void
go_down_key_accel_callback(void *data)
{
  dt_view_t *self = (dt_view_t *)data;
  dt_library_t *lib = (dt_library_t *)self->data;
  lib->offset = 0x1fffffff;
  dt_control_queue_draw_all();
}

static void
star_key_accel_callback(void *data)
{
  long int num = (long int)data;
  switch (num)
  {
    case DT_VIEW_STAR_1: case DT_VIEW_STAR_2: case DT_VIEW_STAR_3: case DT_VIEW_STAR_4: case 666:
    { 
      int32_t mouse_over_id;
      DT_CTL_GET_GLOBAL(mouse_over_id, lib_image_mouse_over_id);
      if(mouse_over_id <= 0)
      {
        sqlite3_stmt *stmt;
        sqlite3_prepare_v2(darktable.db, "select imgid from selected_images", -1, &stmt, NULL);
        while(sqlite3_step(stmt) == SQLITE_ROW)
        {
          dt_image_t *image = dt_image_cache_get(sqlite3_column_int(stmt, 0), 'r');
          if(num == 666) image->flags &= ~0xf;
          else if(num == DT_VIEW_STAR_1 && ((image->flags & 0x7) == 1)) image->flags &= ~0x7;
          else
          {
            image->flags &= ~0x7;
            image->flags |= num;
          }
          dt_image_cache_flush(image);
          dt_image_cache_release(image, 'r');
        }
        sqlite3_finalize(stmt);
      }
      else
      {
        dt_image_t *image = dt_image_cache_get(mouse_over_id, 'r');
        if(num == 666) image->flags &= ~0xf;
        else if(num == DT_VIEW_STAR_1 && ((image->flags & 0x7) == 1)) image->flags &= ~0x7;
        else
        {
          image->flags &= ~0x7;
          image->flags |= num;
        }
        dt_image_cache_flush(image);
        dt_image_cache_release(image, 'r');
      }
      dt_control_queue_draw_all();
      break;
    }
    default:
      break;
  }
}

void enter(dt_view_t *self)
{
  // add expanders
  GtkBox *box = GTK_BOX(glade_xml_get_widget (darktable.gui->main_window, "plugins_vbox"));
  GList *modules = g_list_last(darktable.lib->plugins);
  
  // Adjust gui
  GtkWidget *widget = glade_xml_get_widget (darktable.gui->main_window, "devices_eventbox");
  gtk_widget_set_visible(widget, TRUE);
  
  while(modules)
  {
    dt_lib_module_t *module = (dt_lib_module_t *)(modules->data);
    if( module->views() & DT_LIGHTTABLE_VIEW )
    { // Module does support this view let's add it to plugin box
      module->gui_init(module);
      // add the widget created by gui_init to an expander and both to list.
      GtkWidget *expander = dt_lib_gui_get_expander(module);
      gtk_box_pack_start(box, expander, FALSE, FALSE, 0);
    }
    modules = g_list_previous(modules);
  }

  // end marker widget:
  GtkWidget *endmarker = gtk_drawing_area_new();
  gtk_widget_set_size_request(GTK_WIDGET(endmarker), 250, 50);
  gtk_box_pack_start(box, endmarker, FALSE, FALSE, 0);
  g_signal_connect (G_OBJECT (endmarker), "expose-event",
                    G_CALLBACK (dt_control_expose_endmarker), 0);

  gtk_widget_show_all(GTK_WIDGET(box));

  // close expanders
  modules = darktable.lib->plugins;
  while(modules)
  {
    dt_lib_module_t *module = (dt_lib_module_t *)(modules->data);
    if( module->views() & DT_LIGHTTABLE_VIEW )
    { // Module does support this view let's add it to plugin box
      char var[1024];
      snprintf(var, 1024, "plugins/lighttable/%s/expanded", module->plugin_name);
      gboolean expanded = dt_conf_get_bool(var);
      gtk_expander_set_expanded (module->expander, expanded);
      if(expanded) gtk_widget_show_all(module->widget);
      else         gtk_widget_hide_all(module->widget);
    }
    modules = g_list_next(modules);
  }
  dt_gui_key_accel_register(GDK_MOD1_MASK, GDK_1, star_key_accel_callback, (void *)DT_VIEW_STAR_1);
  dt_gui_key_accel_register(GDK_MOD1_MASK, GDK_2, star_key_accel_callback, (void *)DT_VIEW_STAR_2);
  dt_gui_key_accel_register(GDK_MOD1_MASK, GDK_3, star_key_accel_callback, (void *)DT_VIEW_STAR_3);
  dt_gui_key_accel_register(GDK_MOD1_MASK, GDK_4, star_key_accel_callback, (void *)DT_VIEW_STAR_4);
  dt_gui_key_accel_register(GDK_CONTROL_MASK, GDK_BackSpace, star_key_accel_callback, (void *)666);
  dt_gui_key_accel_register(GDK_CONTROL_MASK, GDK_g, go_up_key_accel_callback, (void *)self);
  dt_gui_key_accel_register(GDK_CONTROL_MASK, GDK_G, go_down_key_accel_callback, (void *)self);
}

void dt_lib_remove_child(GtkWidget *widget, gpointer data)
{
  gtk_container_remove(GTK_CONTAINER(data), widget);
}

void leave(dt_view_t *self)
{
  dt_gui_key_accel_unregister(star_key_accel_callback);
  dt_gui_key_accel_unregister(go_up_key_accel_callback);
  dt_gui_key_accel_unregister(go_down_key_accel_callback);
  GList *it = darktable.lib->plugins;
  while(it)
  {
    dt_lib_module_t *module = (dt_lib_module_t *)(it->data);
    if( module->views() & DT_LIGHTTABLE_VIEW )
      module->gui_cleanup(module);
    it = g_list_next(it);
  }
  GtkBox *box = GTK_BOX(glade_xml_get_widget (darktable.gui->main_window, "plugins_vbox"));
  gtk_container_foreach(GTK_CONTAINER(box), (GtkCallback)dt_lib_remove_child, (gpointer)box);
}

void reset(dt_view_t *self)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  lib->center = 1;
  lib->track = lib->pan = 0;
  lib->offset = 0x7fffffff;
  lib->first_visible_zoomable    = -1;
  lib->first_visible_filemanager = -1;
  DT_CTL_SET_GLOBAL(lib_image_mouse_over_id, -1);
}


void mouse_leave(dt_view_t *self)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  if(!lib->pan && dt_conf_get_int("plugins/lighttable/images_in_row") != 1)
  {
    DT_CTL_SET_GLOBAL(lib_image_mouse_over_id, -1);
    dt_control_queue_draw_all(); // remove focus
  }
}


void mouse_moved(dt_view_t *self, double x, double y, int which)
{
  // update stars/etc :(
  dt_control_queue_draw_all();
}


int button_released(dt_view_t *self, double x, double y, int which, uint32_t state)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  lib->pan = 0;
  if(which == 1) dt_control_change_cursor(GDK_LEFT_PTR);
  return 1;
}


int button_pressed(dt_view_t *self, double x, double y, int which, int type, uint32_t state)
{
  dt_library_t *lib = (dt_library_t *)self->data;
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
    case DT_VIEW_DESERT: break;
    case DT_VIEW_STAR_1: case DT_VIEW_STAR_2: case DT_VIEW_STAR_3: case DT_VIEW_STAR_4:
    { 
      int32_t mouse_over_id;
      DT_CTL_GET_GLOBAL(mouse_over_id, lib_image_mouse_over_id);
      dt_image_t *image = dt_image_cache_get(mouse_over_id, 'r');
      if(lib->image_over == DT_VIEW_STAR_1 && ((image->flags & 0x7) == 1)) image->flags &= ~0x7;
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
  }
  return 1;
}


int key_pressed(dt_view_t *self, uint16_t which)
{
  dt_library_t *lib = (dt_library_t *)self->data;
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
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(widget), zoom);
  return 1;
}

void border_scrolled(dt_view_t *view, double x, double y, int which, int up)
{
  dt_library_t *lib = (dt_library_t *)view->data;
  if(which == 0 || which == 1)
  {
    if(up) lib->track = -DT_LIBRARY_MAX_ZOOM;
    else   lib->track =  DT_LIBRARY_MAX_ZOOM;
  }
  else if(which == 2 || which == 3)
  {
    if(up) lib->track = -1;
    else   lib->track =  1;
  }
  dt_control_queue_draw_all();
}

void scrolled(dt_view_t *view, double x, double y, int up)
{
  dt_library_t *lib = (dt_library_t *)view->data;
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
  }
}

