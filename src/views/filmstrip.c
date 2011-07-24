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

static void star_key_accel_callback(GtkAccelGroup *accel_group,
                                    GObject *acceleratable, guint keyval,
                                    GdkModifierType modifier, gpointer data);

static void discard_history_key_accel_callback(GtkAccelGroup *accel_group,
                                               GObject *acceleratable,
                                               guint keyval,
                                               GdkModifierType modifier,
                                               gpointer data);
static void copy_history_key_accel_callback(GtkAccelGroup *accel_group,
                                            GObject *acceleratable,
                                            guint keyval,
                                            GdkModifierType modifier,
                                            gpointer data);
static void paste_history_key_accel_callback(GtkAccelGroup *accel_group,
                                             GObject *acceleratable,
                                             guint keyval,
                                             GdkModifierType modifier,
                                             gpointer data);
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
  int32_t history_copy_imgid;

  // Accel closure list
  GSList *closures;
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
  strip->closures = NULL;

  // Registering keyboard accelerators
  gtk_accel_map_add_entry("<Darktable>/filmstrip/rating/desert", GDK_0, 0);
  gtk_accel_map_add_entry("<Darktable>/filmstrip/rating/1", GDK_1, 0);
  gtk_accel_map_add_entry("<Darktable>/filmstrip/rating/2", GDK_2, 0);
  gtk_accel_map_add_entry("<Darktable>/filmstrip/rating/3", GDK_3, 0);
  gtk_accel_map_add_entry("<Darktable>/filmstrip/rating/4", GDK_4, 0);
  gtk_accel_map_add_entry("<Darktable>/filmstrip/rating/5", GDK_5, 0);
  gtk_accel_map_add_entry("<Darktable>/filmstrip/rating/reject", GDK_r,
                          0);

  dt_accel_group_connect_by_path(darktable.control->accels_filmstrip,
                                 "<Darktable>/filmstrip/rating/desert",
                                 NULL);
  dt_accel_group_connect_by_path(darktable.control->accels_filmstrip,
                                 "<Darktable>/filmstrip/rating/1",
                                 NULL);
  dt_accel_group_connect_by_path(darktable.control->accels_filmstrip,
                                 "<Darktable>/filmstrip/rating/2",
                                 NULL);
  dt_accel_group_connect_by_path(darktable.control->accels_filmstrip,
                                 "<Darktable>/filmstrip/rating/3",
                                 NULL);
  dt_accel_group_connect_by_path(darktable.control->accels_filmstrip,
                                 "<Darktable>/filmstrip/rating/4",
                                 NULL);
  dt_accel_group_connect_by_path(darktable.control->accels_filmstrip,
                                 "<Darktable>/filmstrip/rating/5",
                                 NULL);
  dt_accel_group_connect_by_path(darktable.control->accels_filmstrip,
                                 "<Darktable>/filmstrip/rating/reject",
                                 NULL);

  gtk_accel_map_add_entry("<Darktable>/filmstrip/history/copy",
                          GDK_c, GDK_CONTROL_MASK);
  gtk_accel_map_add_entry("<Darktable>/filmstrip/history/paste",
                          GDK_v, GDK_CONTROL_MASK);
  gtk_accel_map_add_entry("<Darktable>/filmstrip/history/discard",
                          GDK_d, GDK_CONTROL_MASK);

  dt_accel_group_connect_by_path(
      darktable.control->accels_filmstrip,
      "<Darktable>/filmstrip/history/copy",
      NULL);
  dt_accel_group_connect_by_path(
      darktable.control->accels_filmstrip,
      "<Darktable>/filmstrip/history/paste",
      NULL);
  dt_accel_group_connect_by_path(
      darktable.control->accels_filmstrip,
      "<Darktable>/filmstrip/history/discard",
      NULL);

  gtk_accel_map_add_entry("<Darktable>/filmstrip/color/red", GDK_F1, 0);
  gtk_accel_map_add_entry("<Darktable>/filmstrip/color/yellow", GDK_F2, 0);
  gtk_accel_map_add_entry("<Darktable>/filmstrip/color/green", GDK_F3, 0);
  gtk_accel_map_add_entry("<Darktable>/filmstrip/color/blue", GDK_F4, 0);
  gtk_accel_map_add_entry("<Darktable>/filmstrip/color/purple", GDK_F5, 0);

  dt_accel_group_connect_by_path(
      darktable.control->accels_filmstrip,
      "<Darktable>/filmstrip/color/red",
      NULL);
  dt_accel_group_connect_by_path(
      darktable.control->accels_filmstrip,
      "<Darktable>/filmstrip/color/yellow",
      NULL);
  dt_accel_group_connect_by_path(
      darktable.control->accels_filmstrip,
      "<Darktable>/filmstrip/color/green",
      NULL);
  dt_accel_group_connect_by_path(
      darktable.control->accels_filmstrip,
      "<Darktable>/filmstrip/color/blue",
      NULL);
  dt_accel_group_connect_by_path(
      darktable.control->accels_filmstrip,
      "<Darktable>/filmstrip/color/purple",
      NULL);

  gtk_accel_map_add_entry("<Darktable>/filmstrip/scroll forward",
                          GDK_Right, 0);
  gtk_accel_map_add_entry("<Darktable>/filmstrip/scroll back",
                          GDK_Left, 0);

  dt_accel_group_connect_by_path(darktable.control->accels_filmstrip,
                                 "<Darktable>/filmstrip/scroll forward", NULL);
  dt_accel_group_connect_by_path(darktable.control->accels_filmstrip,
                                 "<Darktable>/filmstrip/scroll back", NULL);
}

void cleanup(dt_view_t *self)
{
  free(self->data);
}

static void
scroll_to_image(dt_view_t *self)
{
  dt_film_strip_t *strip = (dt_film_strip_t *)self->data;
  int imgid = darktable.view_manager->film_strip_scroll_to;
  if(imgid <= 0) return;
  char query[1024];
  const gchar *qin = dt_collection_get_query (darktable.collection);
  if(qin)
  {
    snprintf(query, 1024, "select rowid from (%s) where id=?3", qin);
    sqlite3_stmt *stmt;
    DT_DEBUG_SQLITE3_PREPARE_V2(darktable.db, query, -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1,  0);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, -1);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, imgid);
    if(sqlite3_step(stmt) == SQLITE_ROW)
    {
      strip->offset = sqlite3_column_int(stmt, 0) - 1;
    }
    sqlite3_finalize(stmt);
  }
}

void expose (dt_view_t *self, cairo_t *cr, int32_t width, int32_t height, int32_t pointerx, int32_t pointery)
{
  dt_film_strip_t *strip = (dt_film_strip_t *)self->data;

  if(darktable.gui->center_tooltip == 1)
    darktable.gui->center_tooltip++;

  strip->image_over = DT_VIEW_DESERT;
  int32_t mouse_over_id;
  DT_CTL_GET_GLOBAL(mouse_over_id, lib_image_mouse_over_id);
  DT_CTL_SET_GLOBAL(lib_image_mouse_over_id, -1);
  cairo_set_source_rgb (cr, .2, .2, .2);
  cairo_paint(cr);

  scroll_to_image(self);
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

  DT_DEBUG_SQLITE3_PREPARE_V2(darktable.db, query, -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, offset);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, max_cols);

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
      // FIXME find out where the y translation is done, how big the value is and use it directly instead of getting it from the matrix ...
      cairo_matrix_t m;
      cairo_get_matrix(cr, &m);
      dt_view_image_expose(image, &(strip->image_over), id, cr, wd, ht, max_cols, img_pointerx, img_pointery-m.y0+darktable.control->tabborder);
      cairo_restore(cr);
      dt_image_cache_release(image, 'r');
    }
    else goto failure;
    cairo_translate(cr, wd, 0.0f);
  }
failure:
  sqlite3_finalize(stmt);

  if(darktable.gui->center_tooltip == 2) // not set in this round
  {
    darktable.gui->center_tooltip = 0;
    GtkWidget *widget = darktable.gui->widgets.center;
    g_object_set(G_OBJECT(widget), "tooltip-text", "", (char *)NULL);
  }


#ifdef _DEBUG
  if(darktable.unmuted & DT_DEBUG_CACHE)
    dt_mipmap_cache_print(darktable.mipmap_cache);
#endif
}

static void
copy_history_key_accel_callback(GtkAccelGroup *accel_group,
                                GObject *acceleratable, guint keyval,
                                GdkModifierType modifier, gpointer data)
{
  dt_film_strip_t *strip = (dt_film_strip_t *)data;
  int32_t mouse_over_id;
  DT_CTL_GET_GLOBAL(mouse_over_id, lib_image_mouse_over_id);
  if(mouse_over_id <= 0) return;
  strip->history_copy_imgid = mouse_over_id;

  /* check if images is currently loaded in darkroom */
  if (dt_dev_is_current_image(darktable.develop, mouse_over_id))
    dt_dev_write_history(darktable.develop);
}

static void
paste_history_key_accel_callback(GtkAccelGroup *accel_group,
                                 GObject *acceleratable, guint keyval,
                                 GdkModifierType modifier, gpointer data)
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
discard_history_key_accel_callback(GtkAccelGroup *accel_group,
                                   GObject *acceleratable, guint keyval,
                                   GdkModifierType modifier, gpointer data)
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
star_key_accel_callback(GtkAccelGroup *accel_group,
                        GObject *acceleratable, guint keyval,
                        GdkModifierType modifier, gpointer data)
{
  long int num = (long int)data;
  switch (num)
  {
    case DT_VIEW_DESERT:
    case DT_VIEW_REJECT:
    case DT_VIEW_STAR_1:
    case DT_VIEW_STAR_2:
    case DT_VIEW_STAR_3:
    case DT_VIEW_STAR_4:
    case DT_VIEW_STAR_5:
    case 666:
    {
      int32_t mouse_over_id;
      DT_CTL_GET_GLOBAL(mouse_over_id, lib_image_mouse_over_id);
      if(mouse_over_id <= 0) return;
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
      dt_control_queue_draw_all();
      break;
    }
    default:
      break;
  }
}

void mouse_enter(dt_view_t *self)
{
}

void mouse_leave(dt_view_t *self)
{
}

static void connect_closures(dt_view_t *self)
{
  dt_film_strip_t *strip = (dt_film_strip_t*)self->data;
  GClosure *closure;

  // Registering keyboard accelerators

  closure = g_cclosure_new(
      G_CALLBACK(star_key_accel_callback),
      (gpointer)DT_VIEW_DESERT, NULL);
  strip->closures = g_slist_prepend(strip->closures, closure);
  dt_accel_group_connect_by_path(darktable.control->accels_filmstrip,
                                 "<Darktable>/filmstrip/rating/desert",
                                 closure);
  closure = g_cclosure_new(
      G_CALLBACK(star_key_accel_callback),
      (gpointer)DT_VIEW_STAR_1, NULL);
  strip->closures = g_slist_prepend(strip->closures, closure);
  dt_accel_group_connect_by_path(darktable.control->accels_filmstrip,
                                 "<Darktable>/filmstrip/rating/1",
                                 closure);
  closure = g_cclosure_new(
      G_CALLBACK(star_key_accel_callback),
      (gpointer)DT_VIEW_STAR_2, NULL);
  strip->closures = g_slist_prepend(strip->closures, closure);
  dt_accel_group_connect_by_path(darktable.control->accels_filmstrip,
                                 "<Darktable>/filmstrip/rating/2",
                                 closure);
  closure = g_cclosure_new(
      G_CALLBACK(star_key_accel_callback),
      (gpointer)DT_VIEW_STAR_3, NULL);
  strip->closures = g_slist_prepend(strip->closures, closure);
  dt_accel_group_connect_by_path(darktable.control->accels_filmstrip,
                                 "<Darktable>/filmstrip/rating/3",
                                 closure);
  closure = g_cclosure_new(
      G_CALLBACK(star_key_accel_callback),
      (gpointer)DT_VIEW_STAR_4, NULL);
  strip->closures = g_slist_prepend(strip->closures, closure);
  dt_accel_group_connect_by_path(darktable.control->accels_filmstrip,
                                 "<Darktable>/filmstrip/rating/4",
                                 closure);
  closure = g_cclosure_new(
      G_CALLBACK(star_key_accel_callback),
      (gpointer)DT_VIEW_STAR_5, NULL);
  strip->closures = g_slist_prepend(strip->closures, closure);
  dt_accel_group_connect_by_path(darktable.control->accels_filmstrip,
                                 "<Darktable>/filmstrip/rating/5",
                                 closure);
  closure = g_cclosure_new(
      G_CALLBACK(star_key_accel_callback),
      (gpointer)DT_VIEW_REJECT, NULL);
  strip->closures = g_slist_prepend(strip->closures, closure);
  dt_accel_group_connect_by_path(darktable.control->accels_filmstrip,
                                 "<Darktable>/filmstrip/rating/reject",
                                 closure);

  closure = g_cclosure_new(G_CALLBACK(copy_history_key_accel_callback),
                           (gpointer)strip, NULL);
  strip->closures = g_slist_prepend(strip->closures, closure);
  dt_accel_group_connect_by_path(
      darktable.control->accels_filmstrip,
      "<Darktable>/filmstrip/history/copy",
      closure);
  closure = g_cclosure_new(G_CALLBACK(paste_history_key_accel_callback),
                           (gpointer)strip, NULL);
  strip->closures = g_slist_prepend(strip->closures, closure);
  dt_accel_group_connect_by_path(
      darktable.control->accels_filmstrip,
      "<Darktable>/filmstrip/history/paste",
      closure);
  closure = g_cclosure_new(G_CALLBACK(discard_history_key_accel_callback),
                           (gpointer)strip, NULL);
  strip->closures = g_slist_prepend(strip->closures, closure);
  dt_accel_group_connect_by_path(
      darktable.control->accels_filmstrip,
      "<Darktable>/filmstrip/history/discard",
      closure);

  closure = g_cclosure_new(G_CALLBACK(dt_colorlabels_key_accel_callback),
                           (gpointer)0, NULL);
  strip->closures = g_slist_prepend(strip->closures, closure);
  dt_accel_group_connect_by_path(
      darktable.control->accels_filmstrip,
      "<Darktable>/filmstrip/color/red",
      closure);
  closure = g_cclosure_new(G_CALLBACK(dt_colorlabels_key_accel_callback),
                           (gpointer)1, NULL);
  strip->closures = g_slist_prepend(strip->closures, closure);
  dt_accel_group_connect_by_path(
      darktable.control->accels_filmstrip,
      "<Darktable>/filmstrip/color/yellow",
      closure);
  closure = g_cclosure_new(G_CALLBACK(dt_colorlabels_key_accel_callback),
                           (gpointer)2, NULL);
  strip->closures = g_slist_prepend(strip->closures, closure);
  dt_accel_group_connect_by_path(
      darktable.control->accels_filmstrip,
      "<Darktable>/filmstrip/color/green",
      closure);
  closure =  g_cclosure_new(G_CALLBACK(dt_colorlabels_key_accel_callback),
                            (gpointer)3, NULL);
  strip->closures = g_slist_prepend(strip->closures, closure);
  dt_accel_group_connect_by_path(
      darktable.control->accels_filmstrip,
      "<Darktable>/filmstrip/color/blue",
      closure);
  closure = g_cclosure_new(G_CALLBACK(dt_colorlabels_key_accel_callback),
                           (gpointer)4, NULL);
  strip->closures = g_slist_prepend(strip->closures, closure);
  dt_accel_group_connect_by_path(
      darktable.control->accels_filmstrip,
      "<Darktable>/filmstrip/color/purple",
      closure);

  dt_accel_group_connect_by_path(darktable.control->accels_filmstrip,
                                 "<Darktable>/filmstrip/scroll forward", NULL);
  dt_accel_group_connect_by_path(darktable.control->accels_filmstrip,
                                 "<Darktable>/filmstrip/scroll back", NULL);

}

void enter(dt_view_t *self)
{
  // Attaching accel group
  gtk_window_add_accel_group(GTK_WINDOW(darktable.gui->widgets.main_window),
                             darktable.control->accels_filmstrip);

  // Connecting the closures
  connect_closures(self);

  // scroll to opened image.
  scroll_to_image(self);
}

void leave(dt_view_t *self)
{
  dt_film_strip_t *strip = (dt_film_strip_t *)self->data;
  GSList *c = strip->closures;
  strip->history_copy_imgid=-1;

  while(c)
  {
    dt_accel_group_disconnect(darktable.control->accels_filmstrip, c->data);
    c = g_slist_next(c);
  }
  g_slist_free(strip->closures);
  strip->closures = NULL;

  gtk_window_remove_accel_group(GTK_WINDOW(darktable.gui->widgets.main_window),
                                darktable.control->accels_filmstrip);
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
    case DT_VIEW_DESERT:
      break;
    case DT_VIEW_REJECT:
    case DT_VIEW_STAR_1:
    case DT_VIEW_STAR_2:
    case DT_VIEW_STAR_3:
    case DT_VIEW_STAR_4:
    case DT_VIEW_STAR_5:
    {
      dt_image_t *image = dt_image_cache_get(mouse_over_id, 'r');
      image->dirty = 1;
      if(strip->image_over == DT_VIEW_STAR_1 && ((image->flags & 0x7) == 1)) image->flags &= ~0x7;
      else if(strip->image_over == DT_VIEW_REJECT && ((image->flags & 0x7) == 6)) image->flags &= ~0x7;
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


int key_pressed(dt_view_t *self, guint key, guint state)
{
  dt_film_strip_t *strip = (dt_film_strip_t *)self->data;
  dt_control_accels_t *accels = &darktable.control->accels;

  if(!darktable.control->key_accelerators_on)
    return 0;

  if(key == accels->filmstrip_back.accel_key
     && state == accels->filmstrip_back.accel_mods)
  {
    strip->offset--;
    darktable.view_manager->film_strip_scroll_to = -1;
    return 1;
  }

  if(key == accels->filmstrip_forward.accel_key
     && state == accels->filmstrip_forward.accel_mods)
  {
    strip->offset++;
    darktable.view_manager->film_strip_scroll_to = -1;
    return 1;
  }

  return 0;
}

void scrolled(dt_view_t *view, double x, double y, int up, int state)
{
  dt_film_strip_t *strip = (dt_film_strip_t *)view->data;
  if(up) strip->offset --;
  else   strip->offset ++;
  darktable.view_manager->film_strip_scroll_to = -1;
  // expose will take care of bounds checking
  dt_control_queue_draw_all();
}

// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
