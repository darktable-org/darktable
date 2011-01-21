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
#include "common/collection.h"
#include "common/colorlabels.h"
#include "common/debug.h"
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
  int full_preview;
  int32_t full_preview_id;
  int32_t stars_registered;
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
  lib->full_preview=0; 
  lib->full_preview_id=-1; 
}


void cleanup(dt_view_t *self)
{
  free(self->data);
}


static void
expose_filemanager (dt_view_t *self, cairo_t *cr, int32_t width, int32_t height, int32_t pointerx, int32_t pointery)
{
  dt_library_t *lib = (dt_library_t *)self->data;

  if(darktable.gui->center_tooltip == 1)
    darktable.gui->center_tooltip = 2;

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
  
  /* get the count of current collection */
  int count = dt_collection_get_count (darktable.collection);

  /* get the collection query */
  const gchar *query=dt_collection_get_query (darktable.collection);
  if(!query)
	return;
  
  if(offset < 0) lib->offset = offset = 0;
  while(offset >= count) lib->offset = (offset -= iir);
  dt_view_set_scrollbar(self, 0, 1, 1, offset, count, max_rows*iir);

  int32_t imgids_num = 0;
  int32_t imgids[count];

  DT_DEBUG_SQLITE3_PREPARE_V2(darktable.db, query, -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, offset);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, max_rows*iir);
  sqlite3_stmt *stmt_del_sel;
  DT_DEBUG_SQLITE3_PREPARE_V2(darktable.db, "delete from selected_images where imgid != ?1", -1, &stmt_del_sel, NULL);
  for(int row = 0; row < max_rows; row++)
  {
    for(int col = 0; col < max_cols; col++)
    {
      if(sqlite3_step(stmt) == SQLITE_ROW)
      {
        id = sqlite3_column_int(stmt, 0);
        dt_image_t *image = dt_image_cache_get(id, 'r');
        if (iir == 1 && row)
        {
          dt_image_cache_release(image, 'r');
          continue;
        }
        // set mouse over id
        if(seli == col && selj == row)
        {
          firstsel = lib->offset + selj*iir + seli;
          mouse_over_id = id;
          DT_CTL_SET_GLOBAL(lib_image_mouse_over_id, mouse_over_id);
        }
        // add clicked image to selected table
        if(clicked1)
        {
          if((lib->modifiers & GDK_SHIFT_MASK) == 0 && (lib->modifiers & GDK_CONTROL_MASK) == 0 && seli == col && selj == row)
          { // clear selected if no modifier
            DT_DEBUG_SQLITE3_BIND_INT(stmt_del_sel, 1, id);
            sqlite3_step(stmt_del_sel);
            sqlite3_reset(stmt_del_sel);
            sqlite3_clear_bindings(stmt_del_sel);
          }
          if((lib->modifiers & GDK_SHIFT_MASK) && id == lib->last_selected_id) { last_seli = col; last_selj = row; }
          if((last_seli < (1<<30) && ((lib->modifiers & GDK_SHIFT_MASK) && (col >= last_seli && row >= last_selj &&
                    col <= seli && row <= selj) && (col != last_seli || row != last_selj))) ||
              (seli == col && selj == row))
          { // insert all in range if shift, or only the one the mouse is over for ctrl or plain click.
            dt_view_toggle_selection(id);
            lib->last_selected_id = id;
          }
        }
        cairo_save(cr);
        // if(iir == 1) dt_image_prefetch(image, DT_IMAGE_MIPF);
        dt_view_image_expose(image, &(lib->image_over), id, cr, wd, iir == 1 ? height : ht, iir, img_pointerx, img_pointery);
        cairo_restore(cr);
        dt_image_cache_release(image, 'r');
      }
      else goto failure;
      cairo_translate(cr, wd, 0.0f);
    }
    cairo_translate(cr, -max_cols*wd, ht);
  }
failure:
  sqlite3_finalize(stmt_del_sel);
#if 1
  sqlite3_reset(stmt);
  // not actually needed...
  //sqlite3_clear_bindings(stmt);
  const int prefetchrows = .5*max_rows+1;
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, offset + max_rows*iir);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, prefetchrows*iir);

  // prefetch jobs in inverse order: supersede previous jobs: most important last
  while(sqlite3_step(stmt) == SQLITE_ROW)
  { // prefetch
    imgids[imgids_num++] = sqlite3_column_int(stmt, 0);
  }
  while(imgids_num > 0)
  {
    imgids_num --;
    dt_image_t *image = dt_image_cache_get(imgids[imgids_num], 'r');
    if(image)
    {
      int32_t iwd, iht;
      float imgwd = iir == 1 ? 0.97 : 0.8;
      dt_image_buffer_t mip = dt_image_get_matching_mip_size(image, imgwd*wd, imgwd*(iir==1?height:ht), &iwd, &iht);
      dt_image_prefetch(image, mip);
      dt_image_cache_release(image, 'r');
    }
  }
#endif
  sqlite3_finalize(stmt);

  oldpan = pan;
  if(darktable.unmuted & DT_DEBUG_CACHE)
    dt_mipmap_cache_print(darktable.mipmap_cache);

  if(darktable.gui->center_tooltip == 2) // not set in this round
  {
    darktable.gui->center_tooltip = 0;
    GtkWidget *widget = glade_xml_get_widget (darktable.gui->main_window, "center");
    gtk_object_set(GTK_OBJECT(widget), "tooltip-text", "", (char *)NULL);
  }
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

  const gchar *query = dt_collection_get_query (darktable.collection);
  if(!query || query[0] == '\0') return;
  
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
  char newquery[1024];
  snprintf(newquery, 1024, "select count(id) %s", query + 17);
  DT_DEBUG_SQLITE3_PREPARE_V2(darktable.db, newquery, -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, 0);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, -1);
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

  DT_DEBUG_SQLITE3_PREPARE_V2(darktable.db, query, -1, &stmt, NULL);
  cairo_translate(cr, -offset_x*wd, -offset_y*ht);
  cairo_translate(cr, -MIN(offset_i*wd, 0.0), 0.0);
  sqlite3_stmt *stmt_del_sel;
  DT_DEBUG_SQLITE3_PREPARE_V2(darktable.db, "delete from selected_images where imgid != ?1", -1, &stmt_del_sel, NULL);
  for(int row = 0; row < max_rows; row++)
  {
    if(offset < 0)
    {
      cairo_translate(cr, 0, ht);
      offset += DT_LIBRARY_MAX_ZOOM;
      continue;
    }
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, offset);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, max_cols);
    for(int col = 0; col < max_cols; col++)
    {
      if(sqlite3_step(stmt) == SQLITE_ROW)
      {
        id = sqlite3_column_int(stmt, 0);
        dt_image_t *image = dt_image_cache_get(id, 'r');

        // set mouse over id
        if((zoom == 1 && mouse_over_id < 0) || ((!pan || track) && seli == col && selj == row))
        {
          mouse_over_id = id;
          DT_CTL_SET_GLOBAL(lib_image_mouse_over_id, mouse_over_id);
        }
        // add clicked image to selected table
        if(clicked1)
        {
          if((lib->modifiers & GDK_SHIFT_MASK) == 0 && (lib->modifiers & GDK_CONTROL_MASK) == 0 && seli == col && selj == row)
          { // clear selected if no modifier
            DT_DEBUG_SQLITE3_BIND_INT(stmt_del_sel, 1, id);
            sqlite3_step(stmt_del_sel);
            sqlite3_reset(stmt_del_sel);
            sqlite3_clear_bindings(stmt_del_sel);
          }
          // FIXME: whatever comes first assumtion is broken!
          // if((lib->modifiers & GDK_SHIFT_MASK) && (last_seli == (1<<30)) &&
          //    (image->id == lib->last_selected_id || image->id == mouse_over_id)) { last_seli = col; last_selj = row; }
          // if(last_seli < (1<<30) && ((lib->modifiers & GDK_SHIFT_MASK) && (col >= MIN(last_seli,seli) && row >= MIN(last_selj,selj) &&
          //         col <= MAX(last_seli,seli) && row <= MAX(last_selj,selj)) && (col != last_seli || row != last_selj)) ||
          if((lib->modifiers & GDK_SHIFT_MASK) && id == lib->last_selected_id) { last_seli = col; last_selj = row; }
          if((last_seli < (1<<30) && ((lib->modifiers & GDK_SHIFT_MASK) && (col >= last_seli && row >= last_selj &&
                  col <= seli && row <= selj) && (col != last_seli || row != last_selj))) ||
             (seli == col && selj == row))
          { // insert all in range if shift, or only the one the mouse is over for ctrl or plain click.
            dt_view_toggle_selection(id);
            lib->last_selected_id = id;
          }
        }
        cairo_save(cr);
        // if(zoom == 1) dt_image_prefetch(image, DT_IMAGE_MIPF);
        dt_view_image_expose(image, &(lib->image_over), id, cr, wd, zoom == 1 ? height : ht, zoom, img_pointerx, img_pointery);
        cairo_restore(cr);
        dt_image_cache_release(image, 'r');
      }
      else goto failure;
      cairo_translate(cr, wd, 0.0f);
    }
    cairo_translate(cr, -max_cols*wd, ht);
    offset += DT_LIBRARY_MAX_ZOOM;
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
  }
failure:
  sqlite3_finalize(stmt);
  sqlite3_finalize(stmt_del_sel);

  oldpan = pan;
  lib->zoom_x = zoom_x;
  lib->zoom_y = zoom_y;
  lib->track  = 0;
  lib->center = center;
  if(darktable.unmuted & DT_DEBUG_CACHE)
    dt_mipmap_cache_print(darktable.mipmap_cache);
}

void expose(dt_view_t *self, cairo_t *cr, int32_t width, int32_t height, int32_t pointerx, int32_t pointery)
{
  const int i = dt_conf_get_int("plugins/lighttable/layout");
  
  // Let's show full preview if in that state... 
  dt_library_t *lib = (dt_library_t *)self->data;
  int32_t mouse_over_id;
  DT_CTL_GET_GLOBAL(mouse_over_id, lib_image_mouse_over_id);
  if( lib->full_preview && lib->full_preview_id!=-1 ) {
    lib->image_over = DT_VIEW_DESERT;
    cairo_set_source_rgb (cr, .1, .1, .1);
    cairo_paint(cr);
    dt_image_t *image = dt_image_cache_get(lib->full_preview_id, 'r');
    if( image )
    {
      // dt_image_prefetch(image, DT_IMAGE_MIPF);
      const float wd = width/1.0;
      dt_view_image_expose(image, &(lib->image_over),mouse_over_id, cr, wd, height, 1, pointerx, pointery);
      dt_image_cache_release(image, 'r');
    }
  } 
  else // we do pass on expose to manager or zoomable
  {
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
zoom_key_accel_callback(void *data)
{
  GtkWidget *widget = glade_xml_get_widget (darktable.gui->main_window, "lighttable_zoom_spinbutton");
  int zoom = dt_conf_get_int("plugins/lighttable/images_in_row");
  switch((long int)data)
  {
    case 1:
      zoom = 1;
      break;
    case 2:
      if(zoom <= 1) zoom = 1;
      else zoom --;
      // if(layout == 0) lib->center = 1;
      break;
    case 3:
      if(zoom >= 2*DT_LIBRARY_MAX_ZOOM) zoom = 2*DT_LIBRARY_MAX_ZOOM;
      else zoom ++;
      // if(layout == 0) lib->center = 1;
      break;
    case 4:
      zoom = DT_LIBRARY_MAX_ZOOM;
      break;
  }
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(widget), zoom);
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
        DT_DEBUG_SQLITE3_PREPARE_V2(darktable.db, "select imgid from selected_images", -1, &stmt, NULL);
        while(sqlite3_step(stmt) == SQLITE_ROW)
        {
          dt_image_t *image = dt_image_cache_get(sqlite3_column_int(stmt, 0), 'r');
          image->dirty = 1;
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
        image->dirty = 1;
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
  GtkBox *box_left = GTK_BOX(glade_xml_get_widget (darktable.gui->main_window, "plugins_vbox_left"));
  GList *modules = g_list_last(darktable.lib->plugins);
  
  // Adjust gui
  GtkWidget *widget = glade_xml_get_widget (darktable.gui->main_window, "import_eventbox");
  gtk_widget_set_visible(widget, TRUE);
	
  gtk_widget_set_visible(glade_xml_get_widget (darktable.gui->main_window, "modulegroups_eventbox"), FALSE);
  
  while(modules)
  {
    dt_lib_module_t *module = (dt_lib_module_t *)(modules->data);
    if( module->views() & DT_LIGHTTABLE_VIEW )
    { // Module does support this view let's add it to plugin box
      module->gui_init(module);
      // add the widget created by gui_init to an expander and both to list.
      GtkWidget *expander = dt_lib_gui_get_expander(module);
      if(!strcmp(module->plugin_name, "collect")) gtk_box_pack_start(box_left, expander, FALSE, FALSE, 0);
      else gtk_box_pack_start(box, expander, FALSE, FALSE, 0);
    }
    modules = g_list_previous(modules);
  }

  // end marker widget:
  GtkWidget *endmarker = gtk_drawing_area_new();
  gtk_box_pack_start(box, endmarker, FALSE, FALSE, 0);
  g_signal_connect (G_OBJECT (endmarker), "expose-event",
                    G_CALLBACK (dt_control_expose_endmarker), 0);
  gtk_widget_set_size_request(endmarker, -1, 50);

  gtk_widget_show_all(GTK_WIDGET(box));
  gtk_widget_show_all(GTK_WIDGET(box_left));

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
  dt_gui_key_accel_register(0, GDK_1, star_key_accel_callback, (void *)DT_VIEW_STAR_1);
  dt_gui_key_accel_register(0, GDK_2, star_key_accel_callback, (void *)DT_VIEW_STAR_2);
  dt_gui_key_accel_register(0, GDK_3, star_key_accel_callback, (void *)DT_VIEW_STAR_3);
  dt_gui_key_accel_register(0, GDK_4, star_key_accel_callback, (void *)DT_VIEW_STAR_4);
  dt_library_t *lib = (dt_library_t *)self->data;
  lib->stars_registered = 1;
  dt_gui_key_accel_register(GDK_MOD1_MASK, GDK_1, zoom_key_accel_callback, (void *)1);
  dt_gui_key_accel_register(GDK_MOD1_MASK, GDK_2, zoom_key_accel_callback, (void *)2);
  dt_gui_key_accel_register(GDK_MOD1_MASK, GDK_3, zoom_key_accel_callback, (void *)3);
  dt_gui_key_accel_register(GDK_MOD1_MASK, GDK_4, zoom_key_accel_callback, (void *)4);
  dt_gui_key_accel_register(GDK_CONTROL_MASK, GDK_BackSpace, star_key_accel_callback, (void *)666);
  dt_gui_key_accel_register(GDK_CONTROL_MASK, GDK_g, go_up_key_accel_callback, (void *)self);
  dt_gui_key_accel_register(GDK_CONTROL_MASK|GDK_SHIFT_MASK, GDK_G, go_down_key_accel_callback, (void *)self);
  dt_colorlabels_register_key_accels();
}

void dt_lib_remove_child(GtkWidget *widget, gpointer data)
{
  gtk_container_remove(GTK_CONTAINER(data), widget);
}

void leave(dt_view_t *self)
{
  dt_gui_key_accel_unregister(star_key_accel_callback);
  dt_colorlabels_unregister_key_accels();
  dt_library_t *lib = (dt_library_t *)self->data;
  lib->stars_registered = 0;
  dt_gui_key_accel_unregister(zoom_key_accel_callback);
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
  box = GTK_BOX(glade_xml_get_widget (darktable.gui->main_window, "plugins_vbox_left"));
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


void mouse_enter(dt_view_t *self)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  if(!lib->stars_registered)
  {
    dt_gui_key_accel_register(0, GDK_1, star_key_accel_callback, (void *)DT_VIEW_STAR_1);
    dt_gui_key_accel_register(0, GDK_2, star_key_accel_callback, (void *)DT_VIEW_STAR_2);
    dt_gui_key_accel_register(0, GDK_3, star_key_accel_callback, (void *)DT_VIEW_STAR_3);
    dt_gui_key_accel_register(0, GDK_4, star_key_accel_callback, (void *)DT_VIEW_STAR_4);
    lib->stars_registered = 1;
  }
}

void mouse_leave(dt_view_t *self)
{
  dt_gui_key_accel_unregister(star_key_accel_callback);
  dt_library_t *lib = (dt_library_t *)self->data;
  lib->stars_registered = 0;
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
      if(!image) return 0;
      image->dirty = 1;
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

int key_released(dt_view_t *self, uint16_t which)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  switch (which)
  {
    case KEYCODE_z: 
    {
      lib->full_preview_id = -1;
      GtkWidget *widget = glade_xml_get_widget (darktable.gui->main_window, "left");
      if(lib->full_preview & 1) gtk_widget_show(widget);
      widget = glade_xml_get_widget (darktable.gui->main_window, "right");
      if(lib->full_preview & 2)gtk_widget_show(widget);
      widget = glade_xml_get_widget (darktable.gui->main_window, "bottom");
      if(lib->full_preview & 4)gtk_widget_show(widget);
      widget = glade_xml_get_widget (darktable.gui->main_window, "top");
      if(lib->full_preview & 8)gtk_widget_show(widget);
      lib->full_preview = 0;
    }
    break;
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
    case KEYCODE_z:
    {
      int32_t mouse_over_id;
      DT_CTL_GET_GLOBAL(mouse_over_id, lib_image_mouse_over_id);
      if(!lib->full_preview && mouse_over_id != -1 )
      {
        // encode panel visibility into full_preview
        lib->full_preview = 0;
        lib->full_preview_id = mouse_over_id;
        // let's hide some gui components
        GtkWidget *widget = glade_xml_get_widget (darktable.gui->main_window, "left");
        lib->full_preview |= (gtk_widget_get_visible(widget)&1) << 0;
        gtk_widget_hide(widget);
        widget = glade_xml_get_widget (darktable.gui->main_window, "right");
        lib->full_preview |= (gtk_widget_get_visible(widget)&1) << 1;
        gtk_widget_hide(widget);
        widget = glade_xml_get_widget (darktable.gui->main_window, "bottom");
        lib->full_preview |= (gtk_widget_get_visible(widget)&1) << 2;
        gtk_widget_hide(widget);
        widget = glade_xml_get_widget (darktable.gui->main_window, "top");
        lib->full_preview |= (gtk_widget_get_visible(widget)&1) << 3;
        gtk_widget_hide(widget);
        //dt_dev_invalidate(darktable.develop);
      } 
      return 0;
      
    }  break;
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

void scrolled(dt_view_t *view, double x, double y, int up, int state)
{
  dt_library_t *lib = (dt_library_t *)view->data;
  GtkWidget *widget = glade_xml_get_widget (darktable.gui->main_window, "lighttable_zoom_spinbutton");
  const int layout = dt_conf_get_int("plugins/lighttable/layout");
  if(layout == 1 && state == 0)
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

