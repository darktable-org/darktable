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

#define ZOOM_MAX 13

static void _pos_compute_area(dt_thumbtable_t *table)
{
  GList *l = g_list_first(table->list);
  int x1 = INT_MAX;
  int y1 = INT_MAX;
  int x2 = INT_MIN;
  int y2 = INT_MIN;
  while(l)
  {
    dt_thumbnail_t *th = (dt_thumbnail_t *)l->data;
    x1 = MIN(x1, th->x);
    y1 = MIN(y1, th->y);
    x2 = MAX(x2, th->x);
    y2 = MAX(y2, th->x);
    l = g_list_next(l);
  }
  table->thumbs_area.x = x1;
  table->thumbs_area.y = y1;
  table->thumbs_area.width = x2 + table->thumb_size - x1;
  table->thumbs_area.height = y2 + table->thumb_size - y1;
}

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
  else if(table->mode == DT_THUMBTABLE_MODE_ZOOM)
  {
    *x += table->thumb_size;
    if(*x + table->thumb_size > table->thumbs_area.x + table->thumbs_per_row * table->thumb_size)
    {
      *x = table->thumbs_area.x;
      *y += table->thumb_size;
    }
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
  else if(table->mode == DT_THUMBTABLE_MODE_ZOOM)
  {
    *x -= table->thumb_size;
    if(*x < table->thumbs_area.x)
    {
      *x = table->thumbs_area.x + (table->thumbs_per_row - 1) * table->thumb_size;
      *y -= table->thumb_size;
    }
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
  else if(table->mode == DT_THUMBTABLE_MODE_ZOOM)
  {
    const int npr = dt_view_lighttable_get_zoom(darktable.view_manager);

    if(force || allocation.width != table->view_width || allocation.height != table->view_height)
    {
      table->thumbs_per_row = ZOOM_MAX;
      table->view_width = allocation.width;
      table->view_height = allocation.height;
      table->thumb_size = table->view_width / npr;
      table->rows = table->view_height / table->thumb_size + 1;
      ret = TRUE;
    }
  }
  return ret;
}

static int _thumbs_remove_unneeded(dt_thumbtable_t *table)
{
  int pos = 0;
  int changed = 0;
  GList *l = g_list_nth(table->list, pos);
  while(l)
  {
    dt_thumbnail_t *th = (dt_thumbnail_t *)l->data;
    if(th->y + table->thumb_size < 0 || th->y > table->view_height
       || (table->mode == DT_THUMBTABLE_MODE_FILMSTRIP
           && (th->x + table->thumb_size < 0 || th->x > table->view_width)))
    {
      table->list = g_list_remove_link(table->list, l);
      gtk_container_remove(GTK_CONTAINER(gtk_widget_get_parent(th->w_main)), th->w_main);
      dt_thumbnail_destroy(th);
      g_list_free(l);
      changed++;
    }
    else
      pos++;
    l = g_list_nth(table->list, pos);
  }
  return changed;
}

static int _thumbs_load_needed(dt_thumbtable_t *table)
{
  if(g_list_length(table->list) == 0) return 0;
  sqlite3_stmt *stmt;
  int changed = 0;

  // we load image at the beginning
  dt_thumbnail_t *first = (dt_thumbnail_t *)g_list_first(table->list)->data;
  if(first->rowid > 1
     && (((table->mode == DT_THUMBTABLE_MODE_FILEMANAGER || table->mode == DT_THUMBTABLE_MODE_ZOOM) && first->y > 0)
         || (table->mode == DT_THUMBTABLE_MODE_FILMSTRIP && first->x > 0)))
  {

    int space = first->y;
    if(table->mode == DT_THUMBTABLE_MODE_FILMSTRIP) space = first->x;
    const int nb_to_load = space / table->thumb_size + (space % table->thumb_size != 0);
    gchar *query = dt_util_dstrcat(
        NULL, "SELECT rowid, imgid FROM memory.collected_images WHERE rowid<%d ORDER BY rowid DESC LIMIT %d",
        first->rowid, nb_to_load * table->thumbs_per_row);
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
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
      gtk_layout_put(GTK_LAYOUT(table->widget), thumb->w_main, posx, posy);
      _pos_get_previous(table, &posx, &posy);
      changed++;
    }
    g_free(query);
    sqlite3_finalize(stmt);
  }

  // we load images at the end
  dt_thumbnail_t *last = (dt_thumbnail_t *)g_list_last(table->list)->data;
  // if there's space under the last image, we have rows to load
  // if the last line is not full, we have already reached the end of the collection
  if((table->mode == DT_THUMBTABLE_MODE_FILEMANAGER && last->y + table->thumb_size < table->view_height
      && last->x == table->thumb_size * (table->thumbs_per_row - 1))
     || (table->mode == DT_THUMBTABLE_MODE_FILMSTRIP && last->x + table->thumb_size < table->view_width)
     || (table->mode == DT_THUMBTABLE_MODE_ZOOM && last->y + table->thumb_size < table->view_height))
  {
    int space = table->view_height - (last->y + table->thumb_size);
    if(table->mode == DT_THUMBTABLE_MODE_FILMSTRIP) space = table->view_width - (last->x + table->thumb_size);
    const int nb_to_load = space / table->thumb_size + (space % table->thumb_size != 0);
    gchar *query = dt_util_dstrcat(
        NULL, "SELECT rowid, imgid FROM memory.collected_images WHERE rowid>%d ORDER BY rowid LIMIT %d",
        last->rowid, nb_to_load * table->thumbs_per_row);
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
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
      gtk_layout_put(GTK_LAYOUT(table->widget), thumb->w_main, posx, posy);
      _pos_get_next(table, &posx, &posy);
      changed++;
    }
    g_free(query);
    sqlite3_finalize(stmt);
  }

  return changed;
}

static void _move(dt_thumbtable_t *table, int x, int y)
{
  // we check bounds to allow or not the move
  int posx = x;
  int posy = y;
  if(table->mode == DT_THUMBTABLE_MODE_FILEMANAGER)
  {
    posx = 0; // to be sure, we don't want horizontal move
    if(posy == 0) return;

    // we stop when first rowid image is fully shown
    dt_thumbnail_t *first = (dt_thumbnail_t *)g_list_first(table->list)->data;
    if(first->rowid == 1 && posy > 0 && first->y >= 0) return;

    // we stop when last image is fully shown (that means empty space at the bottom)
    dt_thumbnail_t *last = (dt_thumbnail_t *)g_list_last(table->list)->data;
    if(last->y + table->thumb_size < table->view_height && posy < 0) return;
  }
  else if(table->mode == DT_THUMBTABLE_MODE_FILMSTRIP)
  {
    posy = 0; // to be sure, we don't want vertical move
    if(posx == 0) return;

    // we stop when first rowid image is fully shown
    dt_thumbnail_t *first = (dt_thumbnail_t *)g_list_first(table->list)->data;
    if(first->rowid == 1 && posx > 0 && first->x >= (table->view_width / 2) - table->thumb_size) return;

    // we stop when last image is fully shown (that means empty space at the bottom)
    dt_thumbnail_t *last = (dt_thumbnail_t *)g_list_last(table->list)->data;
    if(last->x < table->view_width / 2 && posx < 0) return;
  }

  // we move all current thumbs
  GList *l = table->list;
  while(l)
  {
    dt_thumbnail_t *th = (dt_thumbnail_t *)l->data;
    th->y += posy;
    th->x += posx;
    gtk_layout_move(GTK_LAYOUT(table->widget), th->w_main, th->x, th->y);
    l = g_list_next(l);
  }

  // we update the thumbs_area
  table->thumbs_area.x += posx;
  table->thumbs_area.y += posy;

  // we load all needed thumbs
  int changed = _thumbs_load_needed(table);

  // we remove the images not visible on screen
  changed += _thumbs_remove_unneeded(table);

  // if there has been changed, we recompute thumbs area
  if(changed > 0) _pos_compute_area(table);

  // we update the offset
  if(table->mode == DT_THUMBTABLE_MODE_FILEMANAGER)
    table->offset = MAX(1, table->offset - (posy / table->thumb_size) * table->thumbs_per_row);
  else if(table->mode == DT_THUMBTABLE_MODE_FILMSTRIP)
    table->offset = MAX(1, table->offset - posx / table->thumb_size);
  else if(table->mode == DT_THUMBTABLE_MODE_ZOOM)
  {
    dt_thumbnail_t *first = (dt_thumbnail_t *)g_list_first(table->list)->data;
    table->offset = first->rowid;
  }
}

static void _zoomable_zoom(dt_thumbtable_t *table, double delta, int x, int y)
{
  // we determine the zoom ratio
  const int old = dt_view_lighttable_get_zoom(darktable.view_manager);
  int new = old;
  if(delta < 0)
    new = MIN(ZOOM_MAX, new + 1);
  else
    new = MAX(1, new - 1);

  if(old == new) return;
  const int new_size = table->view_width / new;
  const double ratio = (double)new_size / (double)table->thumb_size;

  // we get row/collumn numbers of the image under cursor
  const int anchor_x = (x - table->thumbs_area.x) / table->thumb_size;
  const int anchor_y = (y - table->thumbs_area.y) / table->thumb_size;
  // we compute the new position of this image. This will be our reference to compute sizes of other thumbs
  const int anchor_posx = x - (x - anchor_x * table->thumb_size - table->thumbs_area.x) * ratio;
  const int anchor_posy = y - (y - anchor_y * table->thumb_size - table->thumbs_area.y) * ratio;

  // we move and resize each thumbs
  GList *l = g_list_first(table->list);
  while(l)
  {
    dt_thumbnail_t *th = (dt_thumbnail_t *)l->data;
    // we get row/collumn numbers
    const int posx = (th->x - table->thumbs_area.x) / table->thumb_size;
    const int posy = (th->y - table->thumbs_area.y) / table->thumb_size;
    // we compute new position taking anchor image as reference
    th->x = anchor_posx - (anchor_x - posx) * new_size;
    th->y = anchor_posy - (anchor_y - posy) * new_size;
    gtk_layout_move(GTK_LAYOUT(table->widget), th->w_main, th->x, th->y);
    dt_thumbnail_resize(th, new_size, new_size);
    l = g_list_next(l);
  }

  // we update table values
  table->thumb_size = new_size;
  _pos_compute_area(table);

  // and we load/unload thumbs if needed
  int changed = _thumbs_load_needed(table);
  changed += _thumbs_remove_unneeded(table);
  if(changed > 0) _pos_compute_area(table);

  dt_view_lighttable_set_zoom(darktable.view_manager, new);
  gtk_widget_queue_draw(table->widget);
}

static gboolean _event_scroll(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
  GdkEventScroll *e = (GdkEventScroll *)event;
  dt_thumbtable_t *table = (dt_thumbtable_t *)user_data;
  gdouble delta;

  if(dt_gui_get_scroll_delta(e, &delta))
  {
    if(table->mode == DT_THUMBTABLE_MODE_FILEMANAGER || table->mode == DT_THUMBTABLE_MODE_FILMSTRIP)
    {
      // for filemanger and filmstrip, scrolled = move
      if(delta < 0 && table->mode == DT_THUMBTABLE_MODE_FILEMANAGER)
        _move(table, 0, table->thumb_size);
      else if(delta < 0 && table->mode == DT_THUMBTABLE_MODE_FILMSTRIP)
        _move(table, table->thumb_size, 0);
      if(delta >= 0 && table->mode == DT_THUMBTABLE_MODE_FILEMANAGER)
        _move(table, 0, -table->thumb_size);
      else if(delta >= 0 && table->mode == DT_THUMBTABLE_MODE_FILMSTRIP)
        _move(table, -table->thumb_size, 0);
    }
    else if(table->mode == DT_THUMBTABLE_MODE_ZOOM)
    {
      // for zoomable, scroll = zoom
      _zoomable_zoom(table, delta, e->x, e->y);
    }
  }
  // we stop here to avoid scrolledwindow to move
  return TRUE;
}

static gboolean _event_draw(GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
  // we don't really want to draw something, this is just to know when the widget is really ready
  dt_thumbtable_t *table = (dt_thumbtable_t *)user_data;
  dt_thumbtable_full_redraw(table, FALSE);
  return FALSE; // let's propagate this event for childs
}

static gboolean _event_leave_notify(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  dt_control_set_mouse_over_id(-1);
  return TRUE;
}

static gboolean _event_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_thumbtable_t *table = (dt_thumbtable_t *)user_data;
  if(table->mode != DT_THUMBTABLE_MODE_ZOOM) return FALSE;

  if(event->button == 1 && event->type == GDK_BUTTON_PRESS)
  {
    table->dragging = TRUE;
    table->last_x = event->x_root;
    table->last_y = event->y_root;
  }
  return TRUE;
}
static gboolean _event_button_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_thumbtable_t *table = (dt_thumbtable_t *)user_data;
  if(table->mode != DT_THUMBTABLE_MODE_ZOOM) return FALSE;

  table->dragging = FALSE;
  return TRUE;
}
static gboolean _event_motion_notify(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
  dt_thumbtable_t *table = (dt_thumbtable_t *)user_data;
  if(table->mode != DT_THUMBTABLE_MODE_ZOOM) return FALSE;

  if(table->dragging)
  {
    int dx = event->x_root - table->last_x;
    int dy = event->y_root - table->last_y;
    table->last_x = event->x_root;
    table->last_y = event->y_root;
    _move(table, dx, dy);
  }
  return TRUE;
}

dt_thumbtable_t *dt_thumbtable_new()
{
  dt_thumbtable_t *table = (dt_thumbtable_t *)calloc(1, sizeof(dt_thumbtable_t));
  table->widget = gtk_layout_new(NULL, NULL);
  gtk_widget_set_name(table->widget, "thumbtable");
  table->offset = 1; // TODO retrieve it from rc file ?
  gtk_widget_set_events(table->widget, GDK_EXPOSURE_MASK | GDK_POINTER_MOTION_MASK | GDK_POINTER_MOTION_HINT_MASK
                                           | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_STRUCTURE_MASK
                                           | GDK_ENTER_NOTIFY_MASK);
  gtk_widget_set_app_paintable(table->widget, TRUE);
  g_signal_connect(G_OBJECT(table->widget), "scroll-event", G_CALLBACK(_event_scroll), table);
  g_signal_connect(G_OBJECT(table->widget), "draw", G_CALLBACK(_event_draw), table);
  g_signal_connect(G_OBJECT(table->widget), "leave-notify-event", G_CALLBACK(_event_leave_notify), table);
  g_signal_connect(G_OBJECT(table->widget), "button-press-event", G_CALLBACK(_event_button_press), table);
  g_signal_connect(G_OBJECT(table->widget), "motion-notify-event", G_CALLBACK(_event_motion_notify), table);
  g_signal_connect(G_OBJECT(table->widget), "button-release-event", G_CALLBACK(_event_button_release), table);

  gtk_widget_show(table->widget);

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
    printf("reload thumbs from db. force=%d w=%d h=%d zoom=%d rows=%d size=%d ...\n", force, table->view_width,
           table->view_height, table->thumbs_per_row, table->rows, table->thumb_size);

    // we drop all the widgets
    g_list_free_full(table->list, _thumb_remove);
    table->list = NULL;

    int posx = 0;
    int posy = 0;
    int offset = table->offset;
    int empty_start = 0;
    if(table->mode == DT_THUMBTABLE_MODE_FILMSTRIP)
    {
      offset = MAX(1, table->offset - table->rows / 2);
      empty_start = -MIN(0, table->offset - table->rows / 2 - 1);
      posx = (table->view_width - table->rows * table->thumb_size) / 2;
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
      gtk_layout_put(GTK_LAYOUT(table->widget), thumb->w_main, posx, posy);
      _pos_get_next(table, &posx, &posy);
    }

    _pos_compute_area(table);

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
  if(parent && parent != new_parent)
  {
    gtk_container_remove(GTK_CONTAINER(parent), table->widget);
  }

  // we change the settings
  table->mode = mode;

  // we reparent the table
  if(!parent || parent != new_parent)
  {
    if(GTK_IS_OVERLAY(new_parent))
      gtk_overlay_add_overlay(GTK_OVERLAY(new_parent), table->widget);
    else
      gtk_container_add(GTK_CONTAINER(new_parent), table->widget);
  }
}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;