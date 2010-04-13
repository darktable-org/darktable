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
}

void cleanup(dt_view_t *self)
{
  free(self->data);
}


// TODO: remove/ change into film_view expose
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
  // const int selj = pointery / (float)ht;

  const int img_pointerx = fmodf(pointerx, wd);
  const int img_pointery = pointery;

  // const int max_rows = 1;
  const int max_cols = (int)(1+width/(float)wd);
  sqlite3_stmt *stmt = NULL;
  // int id, last_seli = 1<<30, last_selj = 1<<30;
  // int clicked1 = (oldpan == 0 && pan == 1 && lib->button == 1);

  gchar *query = dt_conf_get_string ("plugins/lighttable/query");
  if(!query) return;
  if(query[0] == '\0')
  {
    g_free(query);
    return;
  }
  char newquery[1024];
  snprintf(newquery, 1024, "select count(id) %s", query + 8);
  sqlite3_prepare_v2(darktable.db, newquery, -1, &stmt, NULL);
  sqlite3_bind_int (stmt, 1, 0);
  sqlite3_bind_int (stmt, 2, -1);
  int count = 1, id;
  if(sqlite3_step(stmt) == SQLITE_ROW)
    count = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  if(offset < 0)              strip->offset = offset = 0;
  if(offset > count-max_cols) strip->offset = offset = count-max_cols;
  // dt_view_set_scrollbar(self, offset, count, max_cols, 0, 1, 1);

  sqlite3_prepare_v2(darktable.db, query, -1, &stmt, NULL);
  sqlite3_bind_int (stmt, 1, offset);
  sqlite3_bind_int (stmt, 2, max_cols);
  g_free(query);
  for(int col = 0; col < max_cols; col++)
  {
    if(sqlite3_step(stmt) == SQLITE_ROW)
    {
      id = sqlite3_column_int(stmt, 0);
      dt_image_t *image = dt_image_cache_get(id, 'r');
      if(image)
      {
        // set mouse over id
        if(seli == col)
        {
          // firstsel = lib->offset + selj*iir + seli;
          mouse_over_id = image->id;
          DT_CTL_SET_GLOBAL(lib_image_mouse_over_id, mouse_over_id);
        }
#if 0
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
            dt_library_toggle_selection(image->id);
            lib->last_selected_id = image->id;
          }
        }
#endif
        cairo_save(cr);
        dt_image_prefetch(image, DT_IMAGE_MIPF);
        dt_view_image_expose(image, &(strip->image_over), image->id, cr, wd, ht, max_cols, img_pointerx, img_pointery);
        cairo_restore(cr);
        dt_image_cache_release(image, 'r');
      }
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
star_key_accel_callback(void *data)
{
  long int num = (long int)data;
  switch (num)
  {
    case DT_VIEW_STAR_1: case DT_VIEW_STAR_2: case DT_VIEW_STAR_3: case DT_VIEW_STAR_4: case 666:
    { 
      int32_t mouse_over_id;
      DT_CTL_GET_GLOBAL(mouse_over_id, lib_image_mouse_over_id);
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

void enter(dt_view_t *self)
{
  dt_gui_key_accel_register(GDK_MOD1_MASK, GDK_1, star_key_accel_callback, (void *)DT_VIEW_STAR_1);
  dt_gui_key_accel_register(GDK_MOD1_MASK, GDK_2, star_key_accel_callback, (void *)DT_VIEW_STAR_2);
  dt_gui_key_accel_register(GDK_MOD1_MASK, GDK_3, star_key_accel_callback, (void *)DT_VIEW_STAR_3);
  dt_gui_key_accel_register(GDK_MOD1_MASK, GDK_4, star_key_accel_callback, (void *)DT_VIEW_STAR_4);
  dt_gui_key_accel_register(GDK_CONTROL_MASK, GDK_BackSpace, star_key_accel_callback, (void *)666);
}

void leave(dt_view_t *self)
{
  dt_gui_key_accel_unregister(star_key_accel_callback);
}

// TODO: go to currently selected image in sister view (lt/tethered/darkroom)
void reset(dt_view_t *self)
{
  dt_film_strip_t *strip = (dt_film_strip_t *)self->data;
  strip->offset = 0;
  DT_CTL_SET_GLOBAL(lib_image_mouse_over_id, -1);
}


// TODO: what's with this?
#if 0
void mouse_leave(dt_view_t *self)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  if(!lib->pan && dt_conf_get_int("plugins/lighttable/images_in_row") != 1)
  {
    DT_CTL_SET_GLOBAL(lib_image_mouse_over_id, -1);
    dt_control_queue_draw_all(); // remove focus
  }
}
#endif


void mouse_moved(dt_view_t *self, double x, double y, int which)
{
  // update stars/etc :(
  dt_control_queue_draw_all();
}


#if 0
int button_released(dt_view_t *self, double x, double y, int which, uint32_t state)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  if(which == 1) dt_control_change_cursor(GDK_ARROW);
  return 1;
}
#endif


int button_pressed(dt_view_t *self, double x, double y, int which, int type, uint32_t state)
{
  dt_film_strip_t *strip = (dt_film_strip_t *)self->data;
  // strip->modifiers = state;
  // strip->button = which;
  if(which == 1 && type == GDK_2BUTTON_PRESS)
  {
    // TODO: emit some selection event.
  }
  // image button pressed?
  switch(strip->image_over)
  {
    case DT_VIEW_DESERT: break;
    case DT_VIEW_STAR_1: case DT_VIEW_STAR_2: case DT_VIEW_STAR_3: case DT_VIEW_STAR_4:
    { 
      int32_t mouse_over_id;
      DT_CTL_GET_GLOBAL(mouse_over_id, lib_image_mouse_over_id);
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

void scrolled(dt_view_t *view, double x, double y, int up)
{
  dt_film_strip_t *strip = (dt_film_strip_t *)view->data;
  if(up) strip->offset --;
  else   strip->offset ++;
  // expose will take care of bounds checking
  dt_control_queue_draw_all();
}

