/*
    This file is part of darktable,
    copyright (c) 2019--2020 Aldric Renaudin.

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
/** a class to manage a table of thumbnail for lighttable and filmstrip.  */
#include "dtgtk/thumbtable.h"
#include "common/debug.h"
#include "control/control.h"
#include "dtgtk/thumbnail.h"
#include "views/view.h"

static void _pos_get_next(dt_thumbtable_t *table, int *x, int *y)
{
  if(table->mode == DT_THUMBTABLE_MODE_FILEMANAGER)
  {
    *x += table->thumb_size;
    if(*x + table->thumb_size > table->view_width)
    {
      *x = 0;
      *y += table->thumb_size;
    }
  }
  else if(table->mode == DT_THUMBTABLE_MODE_FILMSTRIP)
  {
    *x += table->thumb_size;
  }
}
static void _pos_get_previous(dt_thumbtable_t *table, int *x, int *y)
{
  if(table->mode == DT_THUMBTABLE_MODE_FILEMANAGER)
  {
    *x -= table->thumb_size;
    if(*x < 0)
    {
      *x = (table->thumbs_per_row - 1) * table->thumb_size;
      *y -= table->thumb_size;
    }
  }
  else if(table->mode == DT_THUMBTABLE_MODE_FILMSTRIP)
  {
    *x -= table->thumb_size;
  }
}

static gboolean _compute_sizes(dt_thumbtable_t *table, gboolean force)
{
  gboolean ret = FALSE; // return value to show if something as changed
  GtkAllocation allocation;
  gtk_widget_get_allocation(table->widget, &allocation);
  if(allocation.width <= 20 || allocation.height <= 20) return FALSE;

  if(table->mode == DT_THUMBTABLE_MODE_FILEMANAGER)
  {
    const int npr = dt_view_lighttable_get_zoom(darktable.view_manager);

    if(force || allocation.width != table->view_width || allocation.height != table->view_height
       || npr != table->thumbs_per_row)
    {
      table->thumbs_per_row = npr;
      table->view_width = allocation.width;
      table->view_height = allocation.height;
      table->thumb_size = table->view_width / table->thumbs_per_row;
      table->rows = table->view_height / table->thumb_size + 1;
      ret = TRUE;
    }
  }
  else if(table->mode == DT_THUMBTABLE_MODE_FILMSTRIP)
  {
    if(force || allocation.width != table->view_width || allocation.height != table->view_height)
    {
      table->thumbs_per_row = 1;
      table->view_width = allocation.width;
      table->view_height = allocation.height;
      table->thumb_size = table->view_height;
      table->rows = table->view_width / table->thumb_size;
      if(table->rows % 2)
        table->rows += 2;
      else
        table->rows += 1;
      ret = TRUE;
    }
  }
  return ret;
}

static void _thumb_remove_hidden(dt_thumbtable_t *table)
{
  int pos = 0;
  GList *l = g_list_nth(table->list, pos);
  while(l)
  {
    dt_thumbnail_t *th = (dt_thumbnail_t *)l->data;
    if(th->x + table->thumb_size < 0 || th->y + table->thumb_size < 0 || th->x > table->view_width
       || th->y > table->view_height)
    {
      table->list = g_list_remove_link(table->list, l);
      gtk_container_remove(GTK_CONTAINER(gtk_widget_get_parent(th->w_main)), th->w_main);
      dt_thumbnail_destroy(th);
      g_list_free(l);
    }
    else
      pos++;
    l = g_list_nth(table->list, pos);
  }
}
static void _move_up(dt_thumbtable_t *table)
{
  const int new_rowid = table->offset - table->thumbs_per_row;

  // we move all current thumbs
  GList *l = table->list;
  while(l)
  {
    dt_thumbnail_t *th = (dt_thumbnail_t *)l->data;
    if(table->mode == DT_THUMBTABLE_MODE_FILEMANAGER)
      th->y += table->thumb_size;
    else if(table->mode == DT_THUMBTABLE_MODE_FILMSTRIP)
      th->x += table->thumb_size;
    gtk_fixed_move(GTK_FIXED(table->area), th->w_main, th->x, th->y);
    l = g_list_next(l);
  }

  // the first loaded thumb
  dt_thumbnail_t *first = (dt_thumbnail_t *)g_list_first(table->list)->data;

  sqlite3_stmt *stmt;
  gchar *query = dt_util_dstrcat(
      NULL, "SELECT rowid, imgid FROM memory.collected_images WHERE rowid<%d ORDER BY rowid DESC LIMIT %d",
      first->rowid, table->thumbs_per_row);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  int nb = 0;
  int posx = first->x;
  int posy = first->y;
  _pos_get_previous(table, &posx, &posy);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    dt_thumbnail_t *thumb = dt_thumbnail_new(table->thumb_size, table->thumb_size, sqlite3_column_int(stmt, 1),
                                             sqlite3_column_int(stmt, 0));
    thumb->x = posx;
    thumb->y = posy;
    table->list = g_list_prepend(table->list, thumb);
    gtk_fixed_put(GTK_FIXED(table->area), thumb->w_main, posx, posy);
    _pos_get_previous(table, &posx, &posy);
    nb++;
  }
  g_free(query);
  sqlite3_finalize(stmt);

  // we remove the images not visible on screen
  _thumb_remove_hidden(table);

  table->offset = new_rowid;
}

static void _move_down(dt_thumbtable_t *table)
{
  const int new_rowid = table->offset + table->thumbs_per_row;

  // we move all current thumbs
  GList *l = table->list;
  while(l)
  {
    dt_thumbnail_t *th = (dt_thumbnail_t *)l->data;
    if(table->mode == DT_THUMBTABLE_MODE_FILEMANAGER)
      th->y -= table->thumb_size;
    else if(table->mode == DT_THUMBTABLE_MODE_FILMSTRIP)
      th->x -= table->thumb_size;
    gtk_fixed_move(GTK_FIXED(table->area), th->w_main, th->x, th->y);
    l = g_list_next(l);
  }

  // the last loaded thumb
  dt_thumbnail_t *last = (dt_thumbnail_t *)g_list_last(table->list)->data;

  sqlite3_stmt *stmt;
  gchar *query = dt_util_dstrcat(
      NULL, "SELECT rowid, imgid FROM memory.collected_images WHERE rowid>%d ORDER BY rowid LIMIT %d", last->rowid,
      table->thumbs_per_row);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  int nb = 0;
  int posx = last->x;
  int posy = last->y;
  _pos_get_next(table, &posx, &posy);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    dt_thumbnail_t *thumb = dt_thumbnail_new(table->thumb_size, table->thumb_size, sqlite3_column_int(stmt, 1),
                                             sqlite3_column_int(stmt, 0));
    thumb->x = posx;
    thumb->y = posy;
    table->list = g_list_append(table->list, thumb);
    gtk_fixed_put(GTK_FIXED(table->area), thumb->w_main, posx, posy);
    _pos_get_next(table, &posx, &posy);
    nb++;
  }
  g_free(query);
  sqlite3_finalize(stmt);

  // we remove the images not visible on screen
  _thumb_remove_hidden(table);

  table->offset = new_rowid;
}

static gboolean _scroll_event_callback(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
  GdkEventScroll *e = (GdkEventScroll *)event;
  gdouble delta_y;

  if(dt_gui_get_scroll_delta(e, &delta_y))
  {
    dt_thumbtable_t *table = (dt_thumbtable_t *)user_data;
    if(delta_y < 0)
      _move_up(table);
    else
      _move_down(table);
  }
  // we stop here to avoid scrolledwindow to move
  return TRUE;
}

static gboolean _draw_event_callback(GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
  // we don't really want to draw something, this is just to know when the flowbox is really ready
  dt_thumbtable_t *table = (dt_thumbtable_t *)user_data;
  dt_thumbtable_full_redraw(table, FALSE);
  return FALSE; // let's propagate this event for childs
}

static gboolean _leave_notify_callback(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  dt_control_set_mouse_over_id(-1);
  return TRUE;
}

dt_thumbtable_t *dt_thumbtable_new()
{
  dt_thumbtable_t *table = (dt_thumbtable_t *)calloc(1, sizeof(dt_thumbtable_t));
  table->area = gtk_fixed_new();
  table->offset = 1; // TODO retrieve it from rc file ?
  gtk_widget_set_events(table->area, GDK_EXPOSURE_MASK | GDK_POINTER_MOTION_MASK | GDK_POINTER_MOTION_HINT_MASK
                                         | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_STRUCTURE_MASK
                                         | GDK_ENTER_NOTIFY_MASK);
  gtk_widget_set_app_paintable(table->area, TRUE);
  table->widget = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(table->widget), GTK_POLICY_EXTERNAL, GTK_POLICY_EXTERNAL);
  g_signal_connect(G_OBJECT(table->widget), "scroll-event", G_CALLBACK(_scroll_event_callback), table);
  g_signal_connect(G_OBJECT(table->area), "draw", G_CALLBACK(_draw_event_callback), table);
  g_signal_connect(G_OBJECT(table->area), "leave-notify-event", G_CALLBACK(_leave_notify_callback), table);

  gtk_container_add(GTK_CONTAINER(table->widget), table->area);
  gtk_widget_show_all(table->widget);

  g_object_ref(table->widget);
  return table;
}

static void _thumb_remove(gpointer user_data)
{
  dt_thumbnail_t *thumb = (dt_thumbnail_t *)user_data;
  gtk_container_remove(GTK_CONTAINER(gtk_widget_get_parent(thumb->w_main)), thumb->w_main);
  dt_thumbnail_destroy(thumb);
}

void dt_thumbtable_full_redraw(dt_thumbtable_t *table, gboolean force)
{
  if(!table) return;
  if(_compute_sizes(table, force))
  {
    sqlite3_stmt *stmt;
    printf("reload thumbs from db. force=%d w=%d h=%d zoom=%d ...\n", force, table->view_width, table->view_height,
           table->thumbs_per_row);

    // we drop all the widgets
    g_list_free_full(table->list, _thumb_remove);

    int posx = 0;
    int posy = 0;
    int offset = table->offset;
    int empty_start = 0;
    if(table->mode == DT_THUMBTABLE_MODE_FILMSTRIP)
    {
      offset = MAX(1, table->offset - table->rows / 2);
      empty_start = -MIN(0, table->offset - table->rows / 2 - 1);
      posx = (table->rows * table->thumb_size - table->view_width) / 2;
      posx += empty_start * table->thumb_size;
    }

    // we add the thumbs
    gchar *query
        = dt_util_dstrcat(NULL, "SELECT rowid, imgid FROM memory.collected_images WHERE rowid>=%d LIMIT %d",
                          offset, table->rows * table->thumbs_per_row - empty_start);
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
    while(sqlite3_step(stmt) == SQLITE_ROW)
    {
      dt_thumbnail_t *thumb = dt_thumbnail_new(table->thumb_size, table->thumb_size, sqlite3_column_int(stmt, 1),
                                               sqlite3_column_int(stmt, 0));
      thumb->x = posx;
      thumb->y = posy;
      table->list = g_list_append(table->list, thumb);
      gtk_fixed_put(GTK_FIXED(table->area), thumb->w_main, posx, posy);
      _pos_get_next(table, &posx, &posy);
    }

    printf("done\n");
    g_free(query);
    sqlite3_finalize(stmt);
  }
}

void dt_thumbtable_set_parent(dt_thumbtable_t *table, GtkWidget *new_parent, dt_thumbtable_mode_t mode)
{
  if(!GTK_IS_CONTAINER(new_parent)) return;

  // if table already has parent, then we remove it
  GtkWidget *parent = gtk_widget_get_parent(table->widget);
  if(parent)
  {
    if(parent == new_parent) return;
    gtk_container_remove(GTK_CONTAINER(parent), table->widget);
  }

  // we change the settings
  table->mode = mode;

  // we reparent the table
  if(GTK_IS_OVERLAY(new_parent))
  {
    gtk_overlay_add_overlay(GTK_OVERLAY(new_parent), table->widget);
  }
  else
  {
    gtk_container_add(GTK_CONTAINER(new_parent), table->widget);
  }
}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;