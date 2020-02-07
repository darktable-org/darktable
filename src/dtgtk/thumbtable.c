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
#include "common/collection.h"
#include "common/colorlabels.h"
#include "common/debug.h"
#include "common/history.h"
#include "common/ratings.h"
#include "common/selection.h"
#include "control/control.h"
#include "gui/accelerators.h"
#include "gui/drag_and_drop.h"
#include "views/view.h"

#define ZOOM_MAX 13

static dt_thumbnail_t *_thumb_get_mouse_over(dt_thumbtable_t *table)
{
  const int imgid = dt_control_get_mouse_over_id();
  GList *l = table->list;
  while(l)
  {
    dt_thumbnail_t *th = (dt_thumbnail_t *)l->data;
    if(th->imgid == imgid) return th;
    l = g_list_next(l);
  }
  return NULL;
}

static int _thumb_get_imgid(int rowid)
{
  int id = -1;
  sqlite3_stmt *stmt;
  gchar *query = dt_util_dstrcat(NULL, "SELECT imgid FROM memory.collected_images WHERE rowid=%d", rowid);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    id = sqlite3_column_int(stmt, 0);
  }
  g_free(query);
  sqlite3_finalize(stmt);
  return id;
}
static int _thumb_get_rowid(int imgid)
{
  int id = -1;
  sqlite3_stmt *stmt;
  gchar *query = dt_util_dstrcat(NULL, "SELECT rowid FROM memory.collected_images WHERE imgid=%d", imgid);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    id = sqlite3_column_int(stmt, 0);
  }
  g_free(query);
  sqlite3_finalize(stmt);
  return id;
}

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
    y2 = MAX(y2, th->y);
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

  if(allocation.width <= 20 || allocation.height <= 20)
  {
    table->view_width = allocation.width;
    table->view_height = allocation.height;
    return FALSE;
  }

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
    if(th->y + table->thumb_size <= 0 || th->y > table->view_height
       || (table->mode == DT_THUMBTABLE_MODE_FILMSTRIP
           && (th->x + table->thumb_size <= 0 || th->x > table->view_width)))
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

static gboolean _move(dt_thumbtable_t *table, int x, int y)
{
  // we check bounds to allow or not the move
  int posx = x;
  int posy = y;
  if(table->mode == DT_THUMBTABLE_MODE_FILEMANAGER)
  {
    posx = 0; // to be sure, we don't want horizontal move
    if(posy == 0) return FALSE;

    // we stop when first rowid image is fully shown
    dt_thumbnail_t *first = (dt_thumbnail_t *)g_list_first(table->list)->data;
    if(first->rowid == 1 && posy > 0 && first->y >= 0)
    {
      // for some reasons, in filemanager, first image can not be at x=0
      // in that case, we count the number of "scroll-top" try and reallign after 2 try
      if(first->x != 0)
      {
        table->realign_top_try++;
        if(table->realign_top_try > 2)
        {
          table->realign_top_try = 0;
          dt_thumbtable_full_redraw(table, TRUE);
          return TRUE;
        }
      }
      return FALSE;
    }
    table->realign_top_try = 0;

    // we stop when last image is fully shown (that means empty space at the bottom)
    dt_thumbnail_t *last = (dt_thumbnail_t *)g_list_last(table->list)->data;
    if(last->y + table->thumb_size < table->view_height && posy < 0) return FALSE;
  }
  else if(table->mode == DT_THUMBTABLE_MODE_FILMSTRIP)
  {
    posy = 0; // to be sure, we don't want vertical move
    if(posx == 0) return FALSE;

    // we stop when first rowid image is fully shown
    dt_thumbnail_t *first = (dt_thumbnail_t *)g_list_first(table->list)->data;
    if(first->rowid == 1 && posx > 0 && first->x >= (table->view_width / 2) - table->thumb_size) return FALSE;

    // we stop when last image is fully shown (that means empty space at the bottom)
    dt_thumbnail_t *last = (dt_thumbnail_t *)g_list_last(table->list)->data;
    if(last->x < table->view_width / 2 && posx < 0) return FALSE;
  }
  else if(table->mode == DT_THUMBTABLE_MODE_ZOOM)
  {
    // we stop before thumb area completly disappear from screen
    const int space = table->thumb_size * 0.5;
    if(table->thumbs_area.y > table->view_height - space) posy = MIN(0, posy);        // we only allow to move up
    if(table->thumbs_area.y + table->thumbs_area.height < space) posy = MAX(0, posy); // we only allow to move down
    if(table->thumbs_area.x > table->view_width - space) posx = MIN(0, posx);         // we only allow to move left
    if(table->thumbs_area.x + table->thumbs_area.width < space) posx = MAX(0, posx); // we only allow to move right

    if(posy == 0 && posx == 0) return FALSE;
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
  {
    table->offset = MAX(1, table->offset - (posy / table->thumb_size) * table->thumbs_per_row);
    table->offset_imgid = _thumb_get_imgid(table->offset);
  }
  else if(table->mode == DT_THUMBTABLE_MODE_FILMSTRIP)
  {
    table->offset = MAX(1, table->offset - posx / table->thumb_size);
    table->offset_imgid = _thumb_get_imgid(table->offset);
  }
  else if(table->mode == DT_THUMBTABLE_MODE_ZOOM)
  {
    dt_thumbnail_t *first = (dt_thumbnail_t *)g_list_first(table->list)->data;
    table->offset = first->rowid;
    table->offset_imgid = first->imgid;
  }

  // and we store it
  dt_conf_set_int("plugins/lighttable/recentcollect/pos0", table->offset);

  return TRUE;
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
  if(!GTK_IS_CONTAINER(gtk_widget_get_parent(widget))) return TRUE;

  // we render the background (can be visible if before first image / after last image)
  GtkStyleContext *context = gtk_widget_get_style_context(widget);
  gtk_render_background(context, cr, 0, 0, gtk_widget_get_allocated_width(widget),
                        gtk_widget_get_allocated_height(widget));

  // but we don't really want to draw something, this is just to know when the widget is really ready
  dt_thumbtable_t *table = (dt_thumbtable_t *)user_data;
  dt_thumbtable_full_redraw(table, FALSE);
  return FALSE; // let's propagate this event
}

static gboolean _event_leave_notify(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  dt_thumbtable_t *table = (dt_thumbtable_t *)user_data;
  // if we leave thumbtable in favour of an inferior (a thumbnail) it's not a real leave !
  if(event->detail == GDK_NOTIFY_INFERIOR) return FALSE;

  table->mouse_inside = FALSE;
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
    table->drag_dx = table->drag_dy = 0;
    table->drag_thumb = _thumb_get_mouse_over(table);
  }
  return TRUE;
}
static gboolean _event_motion_notify(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
  dt_thumbtable_t *table = (dt_thumbtable_t *)user_data;
  table->mouse_inside = TRUE;
  if(table->mode != DT_THUMBTABLE_MODE_ZOOM) return FALSE;

  if(table->dragging)
  {
    int dx = ceil(event->x_root) - table->last_x;
    int dy = ceil(event->y_root) - table->last_y;
    table->last_x = ceil(event->x_root);
    table->last_y = ceil(event->y_root);
    _move(table, dx, dy);
    table->drag_dx += dx;
    table->drag_dy += dy;
    if(table->drag_thumb)
    {
      // we only considers that this is a real move if the total distance is not too low
      table->drag_thumb->moved = ((abs(table->drag_dx) + abs(table->drag_dy)) > DT_PIXEL_APPLY_DPI(8));
    }
  }
  return TRUE;
}
static gboolean _event_button_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_thumbtable_t *table = (dt_thumbtable_t *)user_data;
  if(table->mode != DT_THUMBTABLE_MODE_ZOOM) return FALSE;

  table->dragging = FALSE;

  // we ensure that all thumbnails moved property is reset
  GList *l = table->list;
  while(l)
  {
    dt_thumbnail_t *th = (dt_thumbnail_t *)l->data;
    th->moved = FALSE;
    l = g_list_next(l);
  }

  // we register the position
  dt_conf_set_int("lighttable/ui/pos_x", table->thumbs_area.x);
  dt_conf_set_int("lighttable/ui/pos_y", table->thumbs_area.y);
  return TRUE;
}

// this is called each time mouse_over id change
static void _dt_mouse_over_image_callback(gpointer instance, gpointer user_data)
{
  if(!user_data) return;
  dt_thumbtable_t *table = (dt_thumbtable_t *)user_data;

  const int imgid = dt_control_get_mouse_over_id();

  if(imgid > 0)
  {
    // let's be absolutely sure that the right widget has the focus
    // otherwise accels don't work...
    gtk_widget_grab_focus(dt_ui_center(darktable.gui->ui));
  }

  int groupid = -1;
  // we crawl over all images to find the right one
  GList *l = table->list;
  while(l)
  {
    dt_thumbnail_t *th = (dt_thumbnail_t *)l->data;
    // if needed, the change mouseover value of the thumb
    if(th->mouse_over != (th->imgid == imgid)) dt_thumbnail_set_mouseover(th, (th->imgid == imgid));
    // now the grouping stuff
    if(th->imgid == imgid && th->is_grouped) groupid = th->groupid;
    if(th->group_borders)
    {
      // to be sure we don't have any borders remaining
      dt_thumbnail_set_group_border(th, DT_THUMBNAIL_BORDER_NONE);
    }
    l = g_list_next(l);
  }

  // we recrawl over all image for groups borders
  // this is somewhat complex as we want to draw borders around the group and not around each image of the group
  if(groupid > 0)
  {
    l = table->list;
    int pos = 0;
    while(l)
    {
      dt_thumbnail_t *th = (dt_thumbnail_t *)l->data;
      dt_thumbnail_border_t old_borders = th->group_borders;
      if(th->groupid == groupid)
      {
        gboolean b = TRUE;
        if(table->mode != DT_THUMBTABLE_MODE_FILMSTRIP)
        {
          // left brorder
          if(pos != 0 && th->x != table->thumbs_area.x)
          {
            dt_thumbnail_t *th1 = (dt_thumbnail_t *)g_list_nth_data(table->list, pos - 1);
            if(th1->groupid == groupid) b = FALSE;
          }
          if(b)
          {
            dt_thumbnail_set_group_border(th, DT_THUMBNAIL_BORDER_LEFT);
          }
          // right brorder
          b = TRUE;
          if(table->mode != DT_THUMBTABLE_MODE_FILMSTRIP && pos < g_list_length(table->list) - 1
             && (th->x + th->width * 1.5) < table->thumbs_area.width)
          {
            dt_thumbnail_t *th1 = (dt_thumbnail_t *)g_list_nth_data(table->list, pos + 1);
            if(th1->groupid == groupid) b = FALSE;
          }
          if(b)
          {
            dt_thumbnail_set_group_border(th, DT_THUMBNAIL_BORDER_RIGHT);
          }
        }
        else
        {
          // in filmstrip, top and left borders are always here (no images above or below)
          dt_thumbnail_set_group_border(th, DT_THUMBNAIL_BORDER_TOP);
          dt_thumbnail_set_group_border(th, DT_THUMBNAIL_BORDER_BOTTOM);
        }

        // top brorder
        b = TRUE;
        if(pos - table->thumbs_per_row >= 0)
        {
          dt_thumbnail_t *th1 = (dt_thumbnail_t *)g_list_nth_data(table->list, pos - table->thumbs_per_row);
          if(th1->groupid == groupid) b = FALSE;
        }
        if(b)
        {
          if(table->mode == DT_THUMBTABLE_MODE_FILMSTRIP)
            dt_thumbnail_set_group_border(th, DT_THUMBNAIL_BORDER_LEFT);
          else
            dt_thumbnail_set_group_border(th, DT_THUMBNAIL_BORDER_TOP);
        }
        // bottom brorder
        b = TRUE;
        if(pos + table->thumbs_per_row < g_list_length(table->list))
        {
          dt_thumbnail_t *th1 = (dt_thumbnail_t *)g_list_nth_data(table->list, pos + table->thumbs_per_row);
          if(th1->groupid == groupid) b = FALSE;
        }
        if(b)
        {
          if(table->mode == DT_THUMBTABLE_MODE_FILMSTRIP)
            dt_thumbnail_set_group_border(th, DT_THUMBNAIL_BORDER_RIGHT);
          else
            dt_thumbnail_set_group_border(th, DT_THUMBNAIL_BORDER_BOTTOM);
        }
      }
      if(th->group_borders != old_borders) gtk_widget_queue_draw(th->w_back);
      l = g_list_next(l);
      pos++;
    }
  }
}

// this is called each time collected images change
static void _dt_collection_changed_callback(gpointer instance, dt_collection_change_t query_change, gpointer imgs,
                                            int next, gpointer user_data)
{
  if(!user_data) return;
  dt_thumbtable_t *table = (dt_thumbtable_t *)user_data;

  if(query_change == DT_COLLECTION_CHANGE_RELOAD)
  {
    /**
     * here's how it works
     *
     * No list of change => keep same imgid as offset
     * offset img is not in changed imgs list => keep same imgid as offset
     * offset img is in changed list => rowid of offset img as changed => next imgid is valid => use next imgid as
     *offset next imgid is invalid => keep same imgid as offset rowid of offset img is the same => keep same imgid
     *as offset
     *
     **/
    int newid = table->offset_imgid;

    // is the current offset imgid in the changed list
    gboolean in_list = FALSE;
    GList *l = imgs;
    while(l)
    {
      if(table->offset_imgid == GPOINTER_TO_INT(l->data))
      {
        in_list = TRUE;
        break;
      }
      l = g_list_next(l);
    }

    if(in_list)
    {
      if(next > 0 && _thumb_get_rowid(table->offset_imgid) != table->offset)
      {
        // if offset has changed, that means the offset img has moved. So we use the next untouched image as offset
        newid = next;
      }
    }

    // get the new rowid of the new offset image
    table->offset_imgid = newid;
    table->offset = _thumb_get_rowid(newid);
    dt_conf_set_int("plugins/lighttable/recentcollect/pos0", table->offset);

    dt_thumbtable_full_redraw(table, TRUE);
  }
  else
  {
    // otherwise we reset the offset to the beginning
    table->offset = 1;
    dt_conf_set_int("plugins/lighttable/recentcollect/pos0", 1);
    dt_thumbtable_full_redraw(table, TRUE);
  }
}


static void _event_dnd_get(GtkWidget *widget, GdkDragContext *context, GtkSelectionData *selection_data,
                           guint target_type, guint time, gpointer user_data)
{
  dt_thumbtable_t *table = (dt_thumbtable_t *)user_data;
  g_assert(selection_data != NULL);

  switch(target_type)
  {
    case DND_TARGET_IMGID:
    {
      // TODO multiple ids
      int id = GPOINTER_TO_INT(g_list_nth_data(table->drag_list, 0));
      gtk_selection_data_set(selection_data, gtk_selection_data_get_target(selection_data), _DWORD, (guchar *)&id,
                             sizeof(id));
      break;
    }
    default: // return the location of the file as a last resort
    case DND_TARGET_URI:
    {
      GList *l = table->drag_list;
      if(g_list_length(l) == 1)
      {
        gchar pathname[PATH_MAX] = { 0 };
        gboolean from_cache = TRUE;
        const int id = GPOINTER_TO_INT(g_list_nth_data(l, 0));
        dt_image_full_path(id, pathname, sizeof(pathname), &from_cache);
        gchar *uri = g_strdup_printf("file://%s", pathname); // TODO: should we add the host?
        gtk_selection_data_set(selection_data, gtk_selection_data_get_target(selection_data), _BYTE, (guchar *)uri,
                               strlen(uri));
        g_free(uri);
      }
      else
      {
        GList *images = NULL;
        while(l)
        {
          const int id = GPOINTER_TO_INT(l->data);
          gchar pathname[PATH_MAX] = { 0 };
          gboolean from_cache = TRUE;
          dt_image_full_path(id, pathname, sizeof(pathname), &from_cache);
          gchar *uri = g_strdup_printf("file://%s", pathname); // TODO: should we add the host?
          images = g_list_append(images, uri);
          l = g_list_next(l);
        }
        gchar *uri_list = dt_util_glist_to_str("\r\n", images);
        g_list_free_full(images, g_free);
        gtk_selection_data_set(selection_data, gtk_selection_data_get_target(selection_data), _BYTE,
                               (guchar *)uri_list, strlen(uri_list));
        g_free(uri_list);
      }
      break;
    }
  }
}

static void _event_dnd_begin(GtkWidget *widget, GdkDragContext *context, gpointer user_data)
{
  const int ts = DT_PIXEL_APPLY_DPI(64);

  dt_thumbtable_t *table = (dt_thumbtable_t *)user_data;

  table->drag_list = dt_view_get_images_to_act_on();

  // if we are dragging a single image -> use the thumbnail of that image
  // otherwise use the generic d&d icon
  // TODO: have something pretty in the 2nd case, too.
  if(g_list_length(table->drag_list) == 1)
  {
    const int id = GPOINTER_TO_INT(g_list_nth_data(table->drag_list, 0));
    dt_mipmap_buffer_t buf;
    dt_mipmap_size_t mip = dt_mipmap_cache_get_matching_size(darktable.mipmap_cache, ts, ts);
    dt_mipmap_cache_get(darktable.mipmap_cache, &buf, id, mip, DT_MIPMAP_BLOCKING, 'r');

    if(buf.buf)
    {
      for(size_t i = 3; i < (size_t)4 * buf.width * buf.height; i += 4) buf.buf[i] = UINT8_MAX;

      int w = ts, h = ts;
      if(buf.width < buf.height)
        w = (buf.width * ts) / buf.height; // portrait
      else
        h = (buf.height * ts) / buf.width; // landscape

      GdkPixbuf *source = gdk_pixbuf_new_from_data(buf.buf, GDK_COLORSPACE_RGB, TRUE, 8, buf.width, buf.height,
                                                   buf.width * 4, NULL, NULL);
      GdkPixbuf *scaled = gdk_pixbuf_scale_simple(source, w, h, GDK_INTERP_HYPER);
      gtk_drag_set_icon_pixbuf(context, scaled, 0, h);

      if(source) g_object_unref(source);
      if(scaled) g_object_unref(scaled);
    }

    dt_mipmap_cache_release(darktable.mipmap_cache, &buf);
  }

  // if we can reorder, let's update the thumbtable class acoordingly
  // this will show up vertical bar for the image destination point
  if(darktable.collection->params.sort == DT_COLLECTION_SORT_CUSTOM_ORDER && table->mode != DT_THUMBTABLE_MODE_ZOOM)
  {
    // we set the class correctly
    GtkStyleContext *tablecontext = gtk_widget_get_style_context(table->widget);
    gtk_style_context_add_class(tablecontext, "dt_thumbtable_reorder");
  }
}

static void _event_dnd_received(GtkWidget *widget, GdkDragContext *context, gint x, gint y,
                                GtkSelectionData *selection_data, guint target_type, guint time,
                                gpointer user_data)
{
  gboolean success = FALSE;

  if((target_type == DND_TARGET_URI) && (selection_data != NULL)
     && (gtk_selection_data_get_length(selection_data) >= 0))
  {
    gchar **uri_list = g_strsplit_set((gchar *)gtk_selection_data_get_data(selection_data), "\r\n", 0);
    if(uri_list)
    {
      gchar **image_to_load = uri_list;
      while(*image_to_load)
      {
        if(**image_to_load)
        {
          dt_load_from_string(*image_to_load, FALSE, NULL); // TODO: do we want to open the image in darkroom mode?
                                                            // If yes -> set to TRUE.
        }
        image_to_load++;
      }
    }
    g_strfreev(uri_list);
    success = TRUE;
  }
  else if((target_type == DND_TARGET_IMGID) && (selection_data != NULL)
          && (gtk_selection_data_get_length(selection_data) >= 0))
  {
    dt_thumbtable_t *table = (dt_thumbtable_t *)user_data;
    if(table->drag_list)
    {
      if(darktable.collection->params.sort == DT_COLLECTION_SORT_CUSTOM_ORDER
         && table->mode != DT_THUMBTABLE_MODE_ZOOM)
      {
        // source = dest = thumbtable => we are reordering
        // set order to "user defined" (this shouldn't trigger anything)
        const int32_t mouse_over_id = dt_control_get_mouse_over_id();
        dt_collection_move_before(mouse_over_id, table->drag_list);
        dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD,
                                   g_list_copy(table->drag_list));
        success = TRUE;
      }
    }
    else
    {
      // we don't catch anything here at the moment
    }
  }
  gtk_drag_finish(context, success, FALSE, time);
}

static void _event_dnd_end(GtkWidget *widget, GdkDragContext *context, gpointer user_data)
{
  dt_thumbtable_t *table = (dt_thumbtable_t *)user_data;
  if(table->drag_list)
  {
    g_list_free(table->drag_list);
    table->drag_list = NULL;
  }
  // in any case, with reset the reordering class if any
  GtkStyleContext *tablecontext = gtk_widget_get_style_context(table->widget);
  gtk_style_context_remove_class(tablecontext, "dt_thumbtable_reorder");
}

dt_thumbtable_t *dt_thumbtable_new()
{
  dt_thumbtable_t *table = (dt_thumbtable_t *)calloc(1, sizeof(dt_thumbtable_t));
  table->widget = gtk_layout_new(NULL, NULL);

  // set css name and class
  gtk_widget_set_name(table->widget, "thumbtable_filemanager");
  GtkStyleContext *context = gtk_widget_get_style_context(table->widget);
  gtk_style_context_add_class(context, "dt_thumbtable");
  if(dt_conf_get_bool("lighttable/ui/expose_statuses")) gtk_style_context_add_class(context, "dt_show_overlays");

  table->offset = MAX(1, dt_conf_get_int("plugins/lighttable/recentcollect/pos0"));

  // set widget signals
  gtk_widget_set_events(table->widget, GDK_EXPOSURE_MASK | GDK_POINTER_MOTION_MASK | GDK_POINTER_MOTION_HINT_MASK
                                           | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_STRUCTURE_MASK
                                           | GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK);
  gtk_widget_set_app_paintable(table->widget, TRUE);
  gtk_widget_set_can_focus(table->widget, TRUE);

  // drag and drop : used for reordering, interactions with maps, exporting uri to external apps, importing images
  // in filmroll...
  gtk_drag_source_set(table->widget, GDK_BUTTON1_MASK, target_list_all, n_targets_all, GDK_ACTION_COPY);
  gtk_drag_dest_set(table->widget, GTK_DEST_DEFAULT_ALL, target_list_all, n_targets_all, GDK_ACTION_COPY);
  g_signal_connect_after(table->widget, "drag-begin", G_CALLBACK(_event_dnd_begin), table);
  g_signal_connect_after(table->widget, "drag-end", G_CALLBACK(_event_dnd_end), table);
  g_signal_connect(table->widget, "drag-data-get", G_CALLBACK(_event_dnd_get), table);
  g_signal_connect(table->widget, "drag-data-received", G_CALLBACK(_event_dnd_received), table);

  g_signal_connect(G_OBJECT(table->widget), "scroll-event", G_CALLBACK(_event_scroll), table);
  g_signal_connect(G_OBJECT(table->widget), "draw", G_CALLBACK(_event_draw), table);
  g_signal_connect(G_OBJECT(table->widget), "leave-notify-event", G_CALLBACK(_event_leave_notify), table);
  g_signal_connect(G_OBJECT(table->widget), "button-press-event", G_CALLBACK(_event_button_press), table);
  g_signal_connect(G_OBJECT(table->widget), "motion-notify-event", G_CALLBACK(_event_motion_notify), table);
  g_signal_connect(G_OBJECT(table->widget), "button-release-event", G_CALLBACK(_event_button_release), table);

  // we register globals signals
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_COLLECTION_CHANGED,
                            G_CALLBACK(_dt_collection_changed_callback), table);
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_MOUSE_OVER_IMAGE_CHANGE,
                            G_CALLBACK(_dt_mouse_over_image_callback), table);

  gtk_widget_show(table->widget);

  g_object_ref(table->widget);

  // we init key accels
  dt_thumbtable_init_accels(table);

  return table;
}

static void _thumb_remove(gpointer user_data)
{
  dt_thumbnail_t *thumb = (dt_thumbnail_t *)user_data;
  gtk_container_remove(GTK_CONTAINER(gtk_widget_get_parent(thumb->w_main)), thumb->w_main);
  dt_thumbnail_destroy(thumb);
}

// reload all thumbs from scratch.
// force define if this should occurs in any case or just if thumbtable sizing properties have changed
void dt_thumbtable_full_redraw(dt_thumbtable_t *table, gboolean force)
{
  if(!table) return;
  if(_compute_sizes(table, force))
  {
    table->dragging = FALSE;
    sqlite3_stmt *stmt;
    printf("reload thumbs from db. force=%d w=%d h=%d zoom=%d rows=%d size=%d ...\n", force, table->view_width,
           table->view_height, table->thumbs_per_row, table->rows, table->thumb_size);

    int posx = 0;
    int posy = 0;
    // in zoomable, we want the first thumb at the same position as the old one

    if(table->mode == DT_THUMBTABLE_MODE_ZOOM)
    {
      if(g_list_length(table->list) > 0)
      {
        dt_thumbnail_t *thumb = (dt_thumbnail_t *)g_list_nth_data(table->list, 0);
        posx = thumb->x;
        posy = thumb->y;
      }
      else
      {
        // first start let's retrieve values from rc file
        posx = dt_conf_get_int("lighttable/ui/pos_x");
        posy = dt_conf_get_int("lighttable/ui/pos_y");
        table->thumbs_area.x = posx;
        table->thumbs_area.y = posy;
      }
    }

    // we drop all the widgets
    g_list_free_full(table->list, _thumb_remove);
    table->list = NULL;

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
      // if it's the offset, we record the imgid
      if(sqlite3_column_int(stmt, 0) == table->offset) table->offset_imgid = sqlite3_column_int(stmt, 1);
    }

    _pos_compute_area(table);

    printf("done\n");
    g_free(query);
    sqlite3_finalize(stmt);
  }
}

// change thumbtable parent widget. Typically from center screen to filmstrip lib
void dt_thumbtable_set_parent(dt_thumbtable_t *table, GtkWidget *new_parent, dt_thumbtable_mode_t mode)
{
  GtkWidget *parent = gtk_widget_get_parent(table->widget);
  if(!GTK_IS_CONTAINER(new_parent))
  {
    if(parent)
    {
      // we just want to remove thumbtable from its parent
      gtk_container_remove(GTK_CONTAINER(parent), table->widget);
    }
    return;
  }

  // if table already has parent, then we remove it
  if(parent && parent != new_parent)
  {
    gtk_container_remove(GTK_CONTAINER(parent), table->widget);
  }

  // mode change
  if(table->mode != mode)
  {
    // we change the widget name
    if(mode == DT_THUMBTABLE_MODE_FILEMANAGER)
      gtk_widget_set_name(table->widget, "thumbtable_filemanager");
    else if(mode == DT_THUMBTABLE_MODE_FILMSTRIP)
      gtk_widget_set_name(table->widget, "thumbtable_filmstrip");
    else if(mode == DT_THUMBTABLE_MODE_ZOOM)
      gtk_widget_set_name(table->widget, "thumbtable_zoom");

    // if needed, we block/unblock drag and drop
    if(mode == DT_THUMBTABLE_MODE_ZOOM)
    {
      gtk_drag_source_unset(table->widget);
    }
    else if(table->mode == DT_THUMBTABLE_MODE_ZOOM)
    {
      gtk_drag_source_set(table->widget, GDK_BUTTON1_MASK, target_list_all, n_targets_all, GDK_ACTION_COPY);
    }

    table->mode = mode;
  }

  // we reparent the table
  if(!parent || parent != new_parent)
  {
    if(GTK_IS_OVERLAY(new_parent))
      gtk_overlay_add_overlay(GTK_OVERLAY(new_parent), table->widget);
    else
      gtk_container_add(GTK_CONTAINER(new_parent), table->widget);
  }
}

// define if overlays should always be shown or just on mouse-over
void dt_thumbtable_set_overlays(dt_thumbtable_t *table, gboolean show)
{
  GtkStyleContext *context = gtk_widget_get_style_context(table->widget);
  if(show)
    gtk_style_context_add_class(context, "dt_show_overlays");
  else
    gtk_style_context_remove_class(context, "dt_show_overlays");
}

// set offset and redraw if needed
gboolean dt_thumbtable_set_offset(dt_thumbtable_t *table, int offset, gboolean redraw)
{
  if(offset < 1 || offset == table->offset) return FALSE;
  table->offset = offset;
  if(redraw) dt_thumbtable_full_redraw(table, TRUE);
  return TRUE;
}

// set offset at specific imgid and redraw if needed
gboolean dt_thumbtable_set_offset_image(dt_thumbtable_t *table, int imgid, gboolean redraw)
{
  return dt_thumbtable_set_offset(table, _thumb_get_rowid(imgid), redraw);
}

static gboolean _accel_rate(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                            GdkModifierType modifier, gpointer data)
{
  GList *imgs = dt_view_get_images_to_act_on();
  dt_ratings_apply_on_list(imgs, GPOINTER_TO_INT(data), TRUE);
  dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, imgs);
  return TRUE;
}
static gboolean _accel_color(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                             GdkModifierType modifier, gpointer data)
{
  GList *imgs = dt_view_get_images_to_act_on();
  dt_colorlabels_toggle_label_on_list(imgs, GPOINTER_TO_INT(data), TRUE);
  dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, imgs);
  return TRUE;
}
static gboolean _accel_copy(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                            GdkModifierType modifier, gpointer data)
{
  return dt_history_copy(dt_view_get_image_to_act_on2());
}
static gboolean _accel_copy_parts(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                  GdkModifierType modifier, gpointer data)
{
  return dt_history_copy_parts(dt_view_get_image_to_act_on2());
}
static gboolean _accel_paste(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                             GdkModifierType modifier, gpointer data)
{
  GList *imgs = dt_view_get_images_to_act_on();
  gboolean ret = dt_history_paste_on_list(imgs, TRUE);
  if(ret) dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, imgs);
  return ret;
}
static gboolean _accel_paste_parts(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                   GdkModifierType modifier, gpointer data)
{
  GList *imgs = dt_view_get_images_to_act_on();
  gboolean ret = dt_history_paste_parts_on_list(imgs, TRUE);
  if(ret) dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, imgs);
  return ret;
}
static gboolean _accel_hist_discard(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                    GdkModifierType modifier, gpointer data)
{
  GList *imgs = dt_view_get_images_to_act_on();
  gboolean ret = dt_history_delete_on_list(imgs, TRUE);
  if(ret) dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, imgs);
  return ret;
}
static gboolean _accel_duplicate(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                 GdkModifierType modifier, gpointer data)
{
  const int sourceid = dt_view_get_image_to_act_on2();
  const int newimgid = dt_image_duplicate(sourceid);
  if(newimgid <= 0) return FALSE;

  if(GPOINTER_TO_INT(data))
    dt_history_delete_on_image(newimgid);
  else
    dt_history_copy_and_paste_on_image(sourceid, newimgid, FALSE, NULL);

  dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, NULL);
  return TRUE;
}
static gboolean _accel_select_all(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                  GdkModifierType modifier, gpointer data)
{
  dt_selection_select_all(darktable.selection);
  return TRUE;
}
static gboolean _accel_select_none(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                   GdkModifierType modifier, gpointer data)
{
  dt_selection_clear(darktable.selection);
  return TRUE;
}
static gboolean _accel_select_invert(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                     GdkModifierType modifier, gpointer data)
{
  dt_selection_invert(darktable.selection);
  return TRUE;
}
static gboolean _accel_select_film(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                   GdkModifierType modifier, gpointer data)
{
  dt_selection_select_filmroll(darktable.selection);
  return TRUE;
}
static gboolean _accel_select_untouched(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                        GdkModifierType modifier, gpointer data)
{
  dt_selection_select_unaltered(darktable.selection);
  return TRUE;
}

// init all accels
void dt_thumbtable_init_accels(dt_thumbtable_t *table)
{
  dt_view_type_flags_t views
      = DT_VIEW_LIGHTTABLE | DT_VIEW_DARKROOM | DT_VIEW_MAP | DT_VIEW_TETHERING | DT_VIEW_PRINT;
  /* setup rating key accelerators */
  dt_accel_register_manual(NC_("accel", "view/thumbtable/rate 0"), views, GDK_KEY_0, 0);
  dt_accel_register_manual(NC_("accel", "view/thumbtable/rate 1"), views, GDK_KEY_1, 0);
  dt_accel_register_manual(NC_("accel", "view/thumbtable/rate 2"), views, GDK_KEY_2, 0);
  dt_accel_register_manual(NC_("accel", "view/thumbtable/rate 3"), views, GDK_KEY_3, 0);
  dt_accel_register_manual(NC_("accel", "view/thumbtable/rate 4"), views, GDK_KEY_4, 0);
  dt_accel_register_manual(NC_("accel", "view/thumbtable/rate 5"), views, GDK_KEY_5, 0);
  dt_accel_register_manual(NC_("accel", "view/thumbtable/rate reject"), views, GDK_KEY_r, 0);

  /* setup history key accelerators */
  dt_accel_register_manual(NC_("accel", "view/thumbtable/copy history"), views, GDK_KEY_c, GDK_CONTROL_MASK);
  dt_accel_register_manual(NC_("accel", "view/thumbtable/copy history parts"), views, GDK_KEY_c,
                           GDK_CONTROL_MASK | GDK_SHIFT_MASK);
  dt_accel_register_manual(NC_("accel", "view/thumbtable/paste history"), views, GDK_KEY_v, GDK_CONTROL_MASK);
  dt_accel_register_manual(NC_("accel", "view/thumbtable/paste history parts"), views, GDK_KEY_v,
                           GDK_CONTROL_MASK | GDK_SHIFT_MASK);
  dt_accel_register_manual(NC_("accel", "view/thumbtable/discard history"), views, 0, 0);

  dt_accel_register_manual(NC_("accel", "view/thumbtable/duplicate image"), views, GDK_KEY_d, GDK_CONTROL_MASK);
  dt_accel_register_manual(NC_("accel", "view/thumbtable/duplicate image virgin"), views, GDK_KEY_d,
                           GDK_CONTROL_MASK | GDK_SHIFT_MASK);

  /* setup color label accelerators */
  dt_accel_register_manual(NC_("accel", "view/thumbtable/color red"), views, GDK_KEY_F1, 0);
  dt_accel_register_manual(NC_("accel", "view/thumbtable/color yellow"), views, GDK_KEY_F2, 0);
  dt_accel_register_manual(NC_("accel", "view/thumbtable/color green"), views, GDK_KEY_F3, 0);
  dt_accel_register_manual(NC_("accel", "view/thumbtable/color blue"), views, GDK_KEY_F4, 0);
  dt_accel_register_manual(NC_("accel", "view/thumbtable/color purple"), views, GDK_KEY_F5, 0);
  dt_accel_register_manual(NC_("accel", "view/thumbtable/clear color labels"), views, 0, 0);

  /* setup selection accelerators */
  dt_accel_register_manual(NC_("accel", "view/thumbtable/select all"), views, GDK_KEY_a, GDK_CONTROL_MASK);
  dt_accel_register_manual(NC_("accel", "view/thumbtable/select none"), views, GDK_KEY_a,
                           GDK_CONTROL_MASK | GDK_SHIFT_MASK);
  dt_accel_register_manual(NC_("accel", "view/thumbtable/invert selection"), views, GDK_KEY_i, GDK_CONTROL_MASK);
  dt_accel_register_manual(NC_("accel", "view/thumbtable/select film roll"), views, 0, 0);
  dt_accel_register_manual(NC_("accel", "view/thumbtable/select untouched"), views, 0, 0);
}
// connect all accels if thumbtable is active in the view and they are not loaded
// disconnect them if not
void dt_thumbtable_update_accels_connection(dt_thumbtable_t *table, int view)
{
  // we verify that thumbtable may be active for this view
  if(!(view & DT_VIEW_LIGHTTABLE) && !(view & DT_VIEW_DARKROOM) && !(view & DT_VIEW_TETHERING)
     && !(view & DT_VIEW_MAP) && !(view & DT_VIEW_PRINT))
  {
    // disconnect all accels
    dt_accel_disconnect_list(table->accel_closures);
    return;
  }
  else if(g_slist_length(table->accel_closures) > 1)
  {
    // already loaded, nothing to do !
    return;
  }

  // Rating accels
  dt_accel_connect_manual(table->accel_closures, "view/thumbtable/rate 0",
                          g_cclosure_new(G_CALLBACK(_accel_rate), GINT_TO_POINTER(DT_VIEW_DESERT), NULL));
  dt_accel_connect_manual(table->accel_closures, "view/thumbtable/rate 1",
                          g_cclosure_new(G_CALLBACK(_accel_rate), GINT_TO_POINTER(DT_VIEW_STAR_1), NULL));
  dt_accel_connect_manual(table->accel_closures, "view/thumbtable/rate 2",
                          g_cclosure_new(G_CALLBACK(_accel_rate), GINT_TO_POINTER(DT_VIEW_STAR_2), NULL));
  dt_accel_connect_manual(table->accel_closures, "view/thumbtable/rate 3",
                          g_cclosure_new(G_CALLBACK(_accel_rate), GINT_TO_POINTER(DT_VIEW_STAR_3), NULL));
  dt_accel_connect_manual(table->accel_closures, "view/thumbtable/rate 4",
                          g_cclosure_new(G_CALLBACK(_accel_rate), GINT_TO_POINTER(DT_VIEW_STAR_4), NULL));
  dt_accel_connect_manual(table->accel_closures, "view/thumbtable/rate 5",
                          g_cclosure_new(G_CALLBACK(_accel_rate), GINT_TO_POINTER(DT_VIEW_STAR_5), NULL));
  dt_accel_connect_manual(table->accel_closures, "view/thumbtable/rate reject",
                          g_cclosure_new(G_CALLBACK(_accel_rate), GINT_TO_POINTER(DT_VIEW_REJECT), NULL));

  // History key accels
  dt_accel_connect_manual(table->accel_closures, "view/thumbtable/copy history",
                          g_cclosure_new(G_CALLBACK(_accel_copy), NULL, NULL));
  dt_accel_connect_manual(table->accel_closures, "view/thumbtable/copy history parts",
                          g_cclosure_new(G_CALLBACK(_accel_copy_parts), NULL, NULL));
  dt_accel_connect_manual(table->accel_closures, "view/thumbtable/paste history",
                          g_cclosure_new(G_CALLBACK(_accel_paste), NULL, NULL));
  dt_accel_connect_manual(table->accel_closures, "view/thumbtable/paste history parts",
                          g_cclosure_new(G_CALLBACK(_accel_paste_parts), NULL, NULL));
  dt_accel_connect_manual(table->accel_closures, "view/thumbtable/discard history",
                          g_cclosure_new(G_CALLBACK(_accel_hist_discard), NULL, NULL));

  dt_accel_connect_manual(table->accel_closures, "view/thumbtable/duplicate image",
                          g_cclosure_new(G_CALLBACK(_accel_duplicate), GINT_TO_POINTER(0), NULL));
  dt_accel_connect_manual(table->accel_closures, "view/thumbtable/duplicate image virgin",
                          g_cclosure_new(G_CALLBACK(_accel_duplicate), GINT_TO_POINTER(1), NULL));

  // Color label accels
  dt_accel_connect_manual(table->accel_closures, "view/thumbtable/color red",
                          g_cclosure_new(G_CALLBACK(_accel_color), GINT_TO_POINTER(0), NULL));
  dt_accel_connect_manual(table->accel_closures, "view/thumbtable/color yellow",
                          g_cclosure_new(G_CALLBACK(_accel_color), GINT_TO_POINTER(1), NULL));
  dt_accel_connect_manual(table->accel_closures, "view/thumbtable/color green",
                          g_cclosure_new(G_CALLBACK(_accel_color), GINT_TO_POINTER(2), NULL));
  dt_accel_connect_manual(table->accel_closures, "view/thumbtable/color blue",
                          g_cclosure_new(G_CALLBACK(_accel_color), GINT_TO_POINTER(3), NULL));
  dt_accel_connect_manual(table->accel_closures, "view/thumbtable/color purple",
                          g_cclosure_new(G_CALLBACK(_accel_color), GINT_TO_POINTER(4), NULL));
  dt_accel_connect_manual(table->accel_closures, "view/thumbtable/clear color labels",
                          g_cclosure_new(G_CALLBACK(_accel_color), GINT_TO_POINTER(5), NULL));

  // Selection accels
  dt_accel_connect_manual(table->accel_closures, "view/thumbtable/select all",
                          g_cclosure_new(G_CALLBACK(_accel_select_all), NULL, NULL));
  dt_accel_connect_manual(table->accel_closures, "view/thumbtable/select none",
                          g_cclosure_new(G_CALLBACK(_accel_select_none), NULL, NULL));
  dt_accel_connect_manual(table->accel_closures, "view/thumbtable/invert selection",
                          g_cclosure_new(G_CALLBACK(_accel_select_invert), NULL, NULL));
  dt_accel_connect_manual(table->accel_closures, "view/thumbtable/select film roll",
                          g_cclosure_new(G_CALLBACK(_accel_select_film), NULL, NULL));
  dt_accel_connect_manual(table->accel_closures, "view/thumbtable/select untouched",
                          g_cclosure_new(G_CALLBACK(_accel_select_untouched), NULL, NULL));
}

static gboolean _filemanager_ensure_rowid_visibility(dt_thumbtable_t *table, int rowid)
{
  if(rowid < 1) return FALSE;
  // get first and last fully visible thumbnails
  dt_thumbnail_t *first = (dt_thumbnail_t *)g_list_first(table->list)->data;
  const int pos = MIN(g_list_length(table->list) - 1, table->thumbs_per_row * (table->rows - 1) - 1);
  dt_thumbnail_t *last = (dt_thumbnail_t *)g_list_nth_data(table->list, pos);

  if(first->rowid > rowid)
  {
    if(_move(table, 0, table->thumb_size))
      return _filemanager_ensure_rowid_visibility(table, rowid);
    else
      return FALSE;
  }
  else if(last->rowid < rowid)
  {
    if(_move(table, 0, -table->thumb_size))
      return _filemanager_ensure_rowid_visibility(table, rowid);
    else
      return FALSE;
  }
  return TRUE;
}
static gboolean _zoomable_ensure_rowid_visibility(dt_thumbtable_t *table, int rowid)
{
  if(rowid < 1) return FALSE;

  int minrowid = 0;
  int maxrowid = 0;
  // is the needed rowid inside the list
  // in this case, is it fully visible ?
  GList *l = g_list_first(table->list);
  int i = 0;
  int y_move = 0;
  int x_move = 0;
  gboolean inside = FALSE;
  while(l)
  {
    dt_thumbnail_t *th = (dt_thumbnail_t *)l->data;
    if(i == 0) minrowid = th->rowid;
    maxrowid = th->rowid;
    if(th->rowid == rowid)
    {
      // the thumbnail is inside the list but maybe not fully visible
      inside = TRUE;
      // vertical movement
      if(th->y < 0)
        y_move = -th->y;
      else if(th->y + table->thumb_size >= table->view_height)
        y_move = table->view_height - th->y - table->thumb_size;
      // horizontal movement
      if(th->x < 0)
        x_move = -th->x;
      else if(th->x + table->thumb_size >= table->view_width)
        x_move = table->view_width - th->x - table->thumb_size;
      // if the thumb is fully visible, nothing to do !
      if(x_move == 0 && y_move == 0) return TRUE;
      break;
    }
    l = g_list_next(l);
    i++;
  }

  // case where the thumb is inside but not fully visible
  if(inside)
  {
    return _move(table, x_move, y_move);
  }
  // case where the thumb is not in the list
  else
  {
    if(rowid < minrowid)
    {
      if(_move(table, 0, table->thumb_size))
        return _zoomable_ensure_rowid_visibility(table, rowid);
      else
        return FALSE;
    }
    else if(rowid > maxrowid)
    {
      if(_move(table, 0, -table->thumb_size))
        return _zoomable_ensure_rowid_visibility(table, rowid);
      else
        return FALSE;
    }
  }
  return FALSE;
}
gboolean dt_thumbtable_ensure_imgid_visibility(dt_thumbtable_t *table, int imgid)
{
  if(imgid < 1) return FALSE;
  if(table->mode == DT_THUMBTABLE_MODE_FILEMANAGER)
    return _filemanager_ensure_rowid_visibility(table, _thumb_get_rowid(imgid));
  else if(table->mode == DT_THUMBTABLE_MODE_ZOOM)
    return _zoomable_ensure_rowid_visibility(table, _thumb_get_rowid(imgid));

  return FALSE;
}

static gboolean _filemanager_key_move(dt_thumbtable_t *table, dt_thumbtable_move_t move, gboolean select)
{
  // base point
  int baseid = dt_control_get_mouse_over_id();
  int baserowid = 1;
  int newrowid = 1;
  if(baseid <= 0)
  {
    baserowid = table->offset;
    baseid = table->offset_imgid;
  }
  else
  {
    baserowid = _thumb_get_rowid(baseid);
  }

  // last rowid of the current collection
  int maxrowid = 1;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT MAX(rowid) FROM memory.collected_images", -1,
                              &stmt, NULL);
  if(sqlite3_step(stmt) == SQLITE_ROW) maxrowid = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);

  // classic keys
  if(move == DT_THUMBTABLE_MOVE_LEFT)
    newrowid = baserowid - 1;
  else if(move == DT_THUMBTABLE_MOVE_RIGHT)
    newrowid = baserowid + 1;
  else if(move == DT_THUMBTABLE_MOVE_UP)
    newrowid = baserowid - table->thumbs_per_row;
  else if(move == DT_THUMBTABLE_MOVE_DOWN)
    newrowid = baserowid + table->thumbs_per_row;
  // page key
  else if(move == DT_THUMBTABLE_MOVE_PAGEUP)
    newrowid = baserowid - table->thumbs_per_row * (table->rows - 1);
  else if(move == DT_THUMBTABLE_MOVE_PAGEDOWN)
    newrowid = baserowid + table->thumbs_per_row * (table->rows - 1);
  // direct start/end
  else if(move == DT_THUMBTABLE_MOVE_START)
    newrowid = 1;
  else if(move == DT_THUMBTABLE_MOVE_END)
    newrowid = maxrowid;

  newrowid = CLAMP(newrowid, 1, maxrowid);
  if(newrowid == baserowid) return FALSE;

  // change image_over
  const int imgid = _thumb_get_imgid(newrowid);
  if(imgid < 1) return FALSE;

  dt_control_set_mouse_over_id(imgid);

  // ensure the image is visible by moving the view if needed
  _filemanager_ensure_rowid_visibility(table, newrowid);

  // if needed, we set the selection
  if(select) dt_selection_select_range(darktable.selection, imgid);
  return TRUE;
}
static gboolean _zoomable_key_move(dt_thumbtable_t *table, dt_thumbtable_move_t move, gboolean select)
{
  // move step : if "select" we move twice
  int step = table->thumb_size;
  if(select) step *= 2;

  // classic keys
  if(move == DT_THUMBTABLE_MOVE_LEFT)
    return _move(table, step, 0);
  else if(move == DT_THUMBTABLE_MOVE_RIGHT)
    return _move(table, -step, 0);
  else if(move == DT_THUMBTABLE_MOVE_UP)
    return _move(table, 0, step);
  else if(move == DT_THUMBTABLE_MOVE_DOWN)
    return _move(table, 0, -step);
  // page key
  else if(move == DT_THUMBTABLE_MOVE_PAGEUP)
    return _move(table, 0, table->thumb_size * (table->rows - 1));
  else if(move == DT_THUMBTABLE_MOVE_PAGEDOWN)
    return _move(table, 0, -table->thumb_size * (table->rows - 1));
  // direct start/end
  else if(move == DT_THUMBTABLE_MOVE_START)
    return _zoomable_ensure_rowid_visibility(table, 1);
  else if(move == DT_THUMBTABLE_MOVE_END)
  {
    int maxrowid = 1;
    sqlite3_stmt *stmt;
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT MAX(rowid) FROM memory.collected_images",
                                -1, &stmt, NULL);
    if(sqlite3_step(stmt) == SQLITE_ROW) maxrowid = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    _zoomable_ensure_rowid_visibility(table, maxrowid);
  }
  return FALSE;
}

gboolean dt_thumbtable_key_move(dt_thumbtable_t *table, dt_thumbtable_move_t move, gboolean select)
{
  if(table->mode == DT_THUMBTABLE_MODE_FILEMANAGER)
    return _filemanager_key_move(table, move, select);
  else if(table->mode == DT_THUMBTABLE_MODE_ZOOM)
    return _zoomable_key_move(table, move, select);

  return FALSE;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;