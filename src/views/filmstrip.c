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
/** this is the view for the film strip module.  */
#include "views/view.h"
#include "libs/lib.h"
#include "control/jobs.h"
#include "control/settings.h"
#include "control/control.h"
#include "control/conf.h"
#include "common/image_cache.h"
#include "common/darktable.h"
#include "common/colorlabels.h"
#include "common/collection.h"
#include "common/history.h"
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


/**
 * this organises the whole library:
 * previously imported film rolls..
 */
typedef struct dt_film_strip_t
{
  // tmp mouse vars:
  int32_t last_selected_id;
  int32_t offset;
  dt_view_image_over_t image_over;
  int32_t stars_registered;
  int32_t history_copy_imgid;
}
dt_film_strip_t;

const char *name(dt_view_t *self)
{
  return _("film strip");
}

void init(dt_view_t *self)
{
  self->data = malloc(sizeof(dt_film_strip_t));
  dt_film_strip_t *strip = (dt_film_strip_t *)self->data;
  strip->last_selected_id = -1;
  strip->offset = 0;
  strip->history_copy_imgid=-1;
}

void cleanup(dt_view_t *self)
{
  free(self->data);
}

void expose (dt_view_t *self, cairo_t *cr, int32_t width, int32_t height, int32_t pointerx, int32_t pointery)
{
  dt_film_strip_t *strip = (dt_film_strip_t *)self->data;
  strip->image_over = DT_VIEW_DESERT;
  int32_t mouse_over_id;
  DT_CTL_GET_GLOBAL(mouse_over_id, lib_image_mouse_over_id);
  DT_CTL_SET_GLOBAL(lib_image_mouse_over_id, -1);
  cairo_set_source_rgb (cr, .9, .9, .9);
  cairo_paint(cr);

  int offset = strip->offset;

  const float wd = height;
  const float ht = height;

  const int seli = pointerx / (float)wd;

  const int img_pointerx = fmodf(pointerx, wd);
  const int img_pointery = pointery;

  const int max_cols = (int)(1+width/(float)wd);
  sqlite3_stmt *stmt = NULL;


    /* get the count of current collection */
  int count = dt_collection_get_count (darktable.collection);

  /* get the collection query */
  const gchar *query=dt_collection_get_query (darktable.collection);
  if(!query)
  return;
  
  if(offset < 0)                strip->offset = offset = 0;
  if(offset > count-max_cols+1) strip->offset = offset = count-max_cols+1;
  // dt_view_set_scrollbar(self, offset, count, max_cols, 0, 1, 1);

  sqlite3_prepare_v2(darktable.db, query, -1, &stmt, NULL);
  sqlite3_bind_int (stmt, 1, offset);
  sqlite3_bind_int (stmt, 2, max_cols);

  for(int col = 0; col < max_cols; col++)
  {
    if(sqlite3_step(stmt) == SQLITE_ROW)
    {
      int id = sqlite3_column_int(stmt, 0);
      dt_image_t *image = dt_image_cache_get(id, 'r');
      // set mouse over id
      if(seli == col)
      {
        mouse_over_id = id;
        DT_CTL_SET_GLOBAL(lib_image_mouse_over_id, mouse_over_id);
      }
      cairo_save(cr);
      dt_view_image_expose(image, &(strip->image_over), id, cr, wd, ht, max_cols, img_pointerx, img_pointery);
      cairo_restore(cr);
      dt_image_cache_release(image, 'r');
    }
    else goto failure;
    cairo_translate(cr, wd, 0.0f);
  }
failure:
  sqlite3_finalize(stmt);

#ifdef _DEBUG
  if(darktable.unmuted & DT_DEBUG_CACHE)
    dt_mipmap_cache_print(darktable.mipmap_cache);
#endif
}

static void
copy_history_key_accel_callback(void *data)
{
  dt_film_strip_t *strip = (dt_film_strip_t *)data;
  int32_t mouse_over_id;
  DT_CTL_GET_GLOBAL(mouse_over_id, lib_image_mouse_over_id);
  if(mouse_over_id <= 0) return;
  strip->history_copy_imgid = mouse_over_id;
}

static void
past_history_key_accel_callback(void *data)
{
  dt_film_strip_t *strip = (dt_film_strip_t *)data;
  if (strip->history_copy_imgid==-1) return;
  
  int32_t mouse_over_id;
  DT_CTL_GET_GLOBAL(mouse_over_id, lib_image_mouse_over_id);
  if(mouse_over_id <= 0) return;
  
  int mode = dt_conf_get_int("plugins/lighttable/copy_history/pastemode");
  
  dt_history_copy_and_paste_on_image(strip->history_copy_imgid, mouse_over_id, (mode == 0)?TRUE:FALSE);
  dt_control_queue_draw_all();
}

static void
discard_history_key_accel_callback(void *data)
{
  dt_film_strip_t *strip = (dt_film_strip_t *)data;
  if (strip->history_copy_imgid==-1) return;
  
  int32_t mouse_over_id;
  DT_CTL_GET_GLOBAL(mouse_over_id, lib_image_mouse_over_id);
  if(mouse_over_id <= 0) return;

  dt_history_delete_on_image(mouse_over_id);
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
      if(mouse_over_id <= 0) return;
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
      dt_control_queue_draw_all();
      break;
    }
    default:
      break;
  }
}

void mouse_enter(dt_view_t *self)
{
  dt_film_strip_t *strip = (dt_film_strip_t *)self->data;
  if(!strip->stars_registered)
  {
    dt_gui_key_accel_register(0, GDK_1, star_key_accel_callback, (void *)DT_VIEW_STAR_1);
    dt_gui_key_accel_register(0, GDK_2, star_key_accel_callback, (void *)DT_VIEW_STAR_2);
    dt_gui_key_accel_register(0, GDK_3, star_key_accel_callback, (void *)DT_VIEW_STAR_3);
    dt_gui_key_accel_register(0, GDK_4, star_key_accel_callback, (void *)DT_VIEW_STAR_4);
    strip->stars_registered = 1;
  }
  
 
}

void mouse_leave(dt_view_t *self)
{
  dt_film_strip_t *strip = (dt_film_strip_t *)self->data;
  dt_gui_key_accel_unregister(star_key_accel_callback);
  strip->stars_registered = 0;
  
}

void enter(dt_view_t *self)
{
  dt_film_strip_t *strip = (dt_film_strip_t *)self->data;
  dt_gui_key_accel_register(0, GDK_1, star_key_accel_callback, (void *)DT_VIEW_STAR_1);
  dt_gui_key_accel_register(0, GDK_2, star_key_accel_callback, (void *)DT_VIEW_STAR_2);
  dt_gui_key_accel_register(0, GDK_3, star_key_accel_callback, (void *)DT_VIEW_STAR_3);
  dt_gui_key_accel_register(0, GDK_4, star_key_accel_callback, (void *)DT_VIEW_STAR_4);
  strip->stars_registered = 1;
  
  dt_gui_key_accel_register(GDK_CONTROL_MASK, GDK_c, copy_history_key_accel_callback, (void *)strip);
  dt_gui_key_accel_register(GDK_CONTROL_MASK, GDK_v, past_history_key_accel_callback, (void *)strip);
  dt_gui_key_accel_register(GDK_CONTROL_MASK, GDK_d, discard_history_key_accel_callback, (void *)strip);
  
  dt_gui_key_accel_register(GDK_CONTROL_MASK, GDK_BackSpace, star_key_accel_callback, (void *)666);
  dt_colorlabels_register_key_accels();
  // scroll to opened image.
  int imgid = darktable.view_manager->film_strip_scroll_to;
  char query[1024];
  const gchar *qin = dt_collection_get_query (darktable.collection);
  if(qin)
  {
    snprintf(query, 1024, "select rowid from (%s) where id=?3", qin);
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(darktable.db, query, -1, &stmt, NULL);
    sqlite3_bind_int(stmt, 1,  0);
    sqlite3_bind_int(stmt, 2, -1);
    sqlite3_bind_int(stmt, 3, imgid);
    if(sqlite3_step(stmt) == SQLITE_ROW)
    {
      strip->offset = sqlite3_column_int(stmt, 0) - 1;
    }
    sqlite3_finalize(stmt);
  }

}

void leave(dt_view_t *self)
{
  
  dt_colorlabels_unregister_key_accels();
  dt_film_strip_t *strip = (dt_film_strip_t *)self->data;
  strip->stars_registered = 0;
  strip->history_copy_imgid=-1;
  dt_gui_key_accel_unregister(star_key_accel_callback);
  dt_gui_key_accel_unregister(copy_history_key_accel_callback);
  dt_gui_key_accel_unregister(past_history_key_accel_callback);
  dt_gui_key_accel_unregister(discard_history_key_accel_callback);
  
}

// TODO: go to currently selected image in sister view (lt/tethered/darkroom)
void reset(dt_view_t *self)
{
  dt_film_strip_t *strip = (dt_film_strip_t *)self->data;
  strip->offset = 0;
  DT_CTL_SET_GLOBAL(lib_image_mouse_over_id, -1);
}

void mouse_moved(dt_view_t *self, double x, double y, int which)
{
  // update stars/etc :(
  dt_control_queue_draw_all();
}

int button_pressed(dt_view_t *self, double x, double y, int which, int type, uint32_t state)
{
  int32_t mouse_over_id;
  DT_CTL_GET_GLOBAL(mouse_over_id, lib_image_mouse_over_id);
  dt_film_strip_t *strip = (dt_film_strip_t *)self->data;
  if(which == 1 && type == GDK_2BUTTON_PRESS)
  {
    // emit some selection event.
    if(mouse_over_id > 0 && darktable.view_manager->film_strip_activated) 
      darktable.view_manager->film_strip_activated(mouse_over_id, darktable.view_manager->film_strip_data);
  }
  // image button pressed?
  switch(strip->image_over)
  {
    case DT_VIEW_DESERT: break;
    case DT_VIEW_STAR_1: case DT_VIEW_STAR_2: case DT_VIEW_STAR_3: case DT_VIEW_STAR_4:
    { 
      dt_image_t *image = dt_image_cache_get(mouse_over_id, 'r');
      if(strip->image_over == DT_VIEW_STAR_1 && ((image->flags & 0x7) == 1)) image->flags &= ~0x7;
      else
      {
        image->flags &= ~0x7;
        image->flags |= strip->image_over;
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
  dt_film_strip_t *strip = (dt_film_strip_t *)self->data;
  switch (which)
  {
    case KEYCODE_Left: case KEYCODE_a:
    case KEYCODE_Up: case KEYCODE_comma:
      strip->offset --;
      break;
    case KEYCODE_Right: case KEYCODE_e:
    case KEYCODE_Down: case KEYCODE_o:
      strip->offset ++;
      break;
    default:
      return 0;
  }
  return 1;
}

void scrolled(dt_view_t *view, double x, double y, int up, int state)
{
  dt_film_strip_t *strip = (dt_film_strip_t *)view->data;
  if(up) strip->offset --;
  else   strip->offset ++;
  // expose will take care of bounds checking
  dt_control_queue_draw_all();
}
