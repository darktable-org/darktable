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

// specials functions for GList globals actions
static gint _list_compare_by_imgid(gconstpointer a, gconstpointer b)
{
  dt_thumbnail_t *th = (dt_thumbnail_t *)a;
  const int imgid = GPOINTER_TO_INT(b);
  if(th->imgid < 0 || b < 0) return 1;
  return (th->imgid != imgid);
}
static void _list_remove_thumb(gpointer user_data)
{
  dt_thumbnail_t *thumb = (dt_thumbnail_t *)user_data;
  gtk_container_remove(GTK_CONTAINER(gtk_widget_get_parent(thumb->w_main)), thumb->w_main);
  dt_thumbnail_destroy(thumb);
}

// get the class name associated with the overlays mode
static gchar *_thumbs_get_overlays_class(dt_thumbnail_overlay_t over)
{
  switch(over)
  {
    case DT_THUMBNAIL_OVERLAYS_NONE:
      return dt_util_dstrcat(NULL, "dt_overlays_none");
    case DT_THUMBNAIL_OVERLAYS_HOVER_EXTENDED:
      return dt_util_dstrcat(NULL, "dt_overlays_hover_extended");
    case DT_THUMBNAIL_OVERLAYS_ALWAYS_NORMAL:
      return dt_util_dstrcat(NULL, "dt_overlays_always");
    case DT_THUMBNAIL_OVERLAYS_ALWAYS_EXTENDED:
      return dt_util_dstrcat(NULL, "dt_overlays_always_extended");
    case DT_THUMBNAIL_OVERLAYS_MIXED:
      return dt_util_dstrcat(NULL, "dt_overlays_mixed");
    case DT_THUMBNAIL_OVERLAYS_HOVER_BLOCK:
      return dt_util_dstrcat(NULL, "dt_overlays_hover_block");
    default:
      return dt_util_dstrcat(NULL, "dt_overlays_hover");
  }
}

// get the size categorie, depending on the thumb size
static int _thumbs_get_prefs_size(dt_thumbtable_t *table)
{
  // we get the size delimitations to differentiate sizes categories
  // one we set as many categories as we want (this can be usefull if we want to finetune very precisely css)
  gchar *txt = dt_conf_get_string("plugins/lighttable/thumbnail_sizes");
  gchar **ts = g_strsplit(txt, "|", -1);
  int i = 0;
  while(ts[i])
  {
    const int s = g_ascii_strtoll(ts[i], NULL, 10);
    if(table->thumb_size < s) break;
    i++;
  }
  g_strfreev(ts);
  g_free(txt);
  return i;
}

// update thumbtable class and overlays mode, depending on size categorie
static void _thumbs_update_overlays_mode(dt_thumbtable_t *table)
{
  int ns = _thumbs_get_prefs_size(table);

  // we change the class that indicate the thumb size
  gchar *c0 = dt_util_dstrcat(NULL, "dt_thumbnails_%d", table->prefs_size);
  gchar *c1 = dt_util_dstrcat(NULL, "dt_thumbnails_%d", ns);
  GtkStyleContext *context = gtk_widget_get_style_context(table->widget);
  gtk_style_context_remove_class(context, c0);
  gtk_style_context_add_class(context, c1);
  g_free(c0);
  g_free(c1);
  table->prefs_size = ns;

  // we change the overlay mode
  gchar *txt = dt_util_dstrcat(NULL, "plugins/lighttable/overlays/%d/%d", table->mode, ns);
  dt_thumbnail_overlay_t over = dt_conf_get_int(txt);
  g_free(txt);
  txt = dt_util_dstrcat(NULL, "plugins/lighttable/tooltips/%d/%d", table->mode, ns);
  table->show_tooltips = dt_conf_get_bool(txt);
  g_free(txt);

  dt_thumbtable_set_overlays_mode(table, over);
}

// change the type of overlays that should be shown
void dt_thumbtable_set_overlays_mode(dt_thumbtable_t *table, dt_thumbnail_overlay_t over)
{
  if(!table) return;
  // we ensure the tooltips change in any cases
  gchar *txt = dt_util_dstrcat(NULL, "plugins/lighttable/tooltips/%d/%d", table->mode, table->prefs_size);
  dt_conf_set_bool(txt, table->show_tooltips);
  g_free(txt);
  GList *l = table->list;
  while(l)
  {
    dt_thumbnail_t *th = (dt_thumbnail_t *)l->data;
    th->tooltip = table->show_tooltips;
    dt_thumbnail_update_infos(th);
    l = g_list_next(l);
  }

  if(over == table->overlays) return;
  txt = dt_util_dstrcat(NULL, "plugins/lighttable/overlays/%d/%d", table->mode, table->prefs_size);
  dt_conf_set_int(txt, over);
  g_free(txt);
  gchar *cl0 = _thumbs_get_overlays_class(table->overlays);
  gchar *cl1 = _thumbs_get_overlays_class(over);

  GtkStyleContext *context = gtk_widget_get_style_context(table->widget);
  gtk_style_context_remove_class(context, cl0);
  gtk_style_context_add_class(context, cl1);

  txt = dt_util_dstrcat(NULL, "plugins/lighttable/overlays_block_timeout/%d/%d", table->mode, table->prefs_size);
  int timeout = 2;
  if(!dt_conf_key_exists(txt))
    timeout = dt_conf_get_int("plugins/lighttable/overlay_timeout");
  else
    timeout = dt_conf_get_int(txt);
  g_free(txt);

  // we need to change the overlay content if we pass from normal to extended overlays
  // this is not done on the fly with css to avoid computing extended msg for nothing and to reserve space if needed
  l = table->list;
  while(l)
  {
    dt_thumbnail_t *th = (dt_thumbnail_t *)l->data;
    dt_thumbnail_set_overlay(th, over, timeout);
    // and we resize the bottom area
    dt_thumbnail_resize(th, th->width, th->height, TRUE);
    l = g_list_next(l);
  }

  table->overlays = over;
  table->overlays_block_timeout = timeout;
  g_free(cl0);
  g_free(cl1);
}

// change the type of overlays that should be shown
void dt_thumbtable_set_overlays_block_timeout(dt_thumbtable_t *table, const int timeout)
{
  if(!table) return;
  gchar *txt
      = dt_util_dstrcat(NULL, "plugins/lighttable/overlays_block_timeout/%d/%d", table->mode, table->prefs_size);
  dt_conf_set_int(txt, timeout);
  g_free(txt);

  table->overlays_block_timeout = timeout;

  // we need to change the overlay timeout for each thumbnails
  GList *l = table->list;
  while(l)
  {
    dt_thumbnail_t *th = (dt_thumbnail_t *)l->data;
    th->overlay_timeout_duration = timeout;
    l = g_list_next(l);
  }
}

// get the thumb at specific position
static dt_thumbnail_t *_thumb_get_at_pos(dt_thumbtable_t *table, int x, int y)
{
  GList *l = table->list;
  while(l)
  {
    dt_thumbnail_t *th = (dt_thumbnail_t *)l->data;
    if(th->x <= x && th->x + th->width > x && th->y <= y && th->y + th->height > y) return th;
    l = g_list_next(l);
  }

  return NULL;
}

// get the thumb which is currently under mouse cursor
static dt_thumbnail_t *_thumb_get_under_mouse(dt_thumbtable_t *table)
{
  if(!table->mouse_inside) return NULL;

  int x = -1;
  int y = -1;
  gdk_window_get_origin(gtk_widget_get_window(table->widget), &x, &y);
  x = table->last_x - x;
  y = table->last_y - y;

  return _thumb_get_at_pos(table, x, y);
}

// get imgid from rowid
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
// get rowid from imgid
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

// get the coordinate of the rectangular area used by all the loaded thumbs
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

// get the position of the next image after the one at (x,y)
static void _pos_get_next(dt_thumbtable_t *table, int *x, int *y)
{
  if(table->mode == DT_THUMBTABLE_MODE_FILEMANAGER)
  {
    *x += table->thumb_size;
    if(*x + table->thumb_size > table->view_width)
    {
      *x = table->center_offset;
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
// get the position of the previous image after the one at (x,y)
static void _pos_get_previous(dt_thumbtable_t *table, int *x, int *y)
{
  if(table->mode == DT_THUMBTABLE_MODE_FILEMANAGER)
  {
    *x -= table->thumb_size;
    if(*x < 0)
    {
      *x = (table->thumbs_per_row - 1) * table->thumb_size + table->center_offset;
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

// compute thumb_size, thumbs_per_row and rows for the current widget size
// return TRUE if something as changed (or forced) FALSE otherwise
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

  int old_size = table->thumb_size;
  if(table->mode == DT_THUMBTABLE_MODE_FILEMANAGER)
  {
    const int npr = dt_view_lighttable_get_zoom(darktable.view_manager);

    if(force || allocation.width != table->view_width || allocation.height != table->view_height
       || npr != table->thumbs_per_row)
    {
      table->thumbs_per_row = npr;
      table->view_width = allocation.width;
      table->view_height = allocation.height;
      table->thumb_size = MIN(table->view_width / table->thumbs_per_row, table->view_height);
      table->rows = table->view_height / table->thumb_size + 1;
      table->center_offset = (table->view_width - table->thumbs_per_row * table->thumb_size) / 2;
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
      table->center_offset = 0;
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
      table->thumbs_per_row = DT_ZOOMABLE_NB_PER_ROW;
      table->view_width = allocation.width;
      table->view_height = allocation.height;
      table->thumb_size = table->view_width / npr;
      table->rows = (table->view_height - table->thumbs_area.y) / table->thumb_size + 1;
      table->center_offset = 0;
      ret = TRUE;
    }
  }

  // if the thumb size has changed, we need to set overlays, etc... correctly
  if(table->thumb_size != old_size)
  {
    _thumbs_update_overlays_mode(table);
  }
  return ret;
}

// update scrollbars positions and visibility
// return their visibility state
static gboolean _thumbtable_update_scrollbars(dt_thumbtable_t *table)
{
  if(table->mode != DT_THUMBTABLE_MODE_FILEMANAGER && table->mode != DT_THUMBTABLE_MODE_ZOOM) return FALSE;
  if(!table->scrollbars) return FALSE;

  table->code_scrolling = TRUE;

  // get the total number of images
  int nbid = 1;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT COUNT(*) FROM memory.collected_images", -1,
                              &stmt, NULL);
  if(sqlite3_step(stmt) == SQLITE_ROW) nbid = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);

  // the number of line before
  int lbefore = (table->offset - 1) / table->thumbs_per_row;
  if((table->offset - 1) % table->thumbs_per_row) lbefore++;

  // the number of line after (including the current one)
  int lafter = (nbid - table->offset) / table->thumbs_per_row;
  if((nbid - table->offset) % table->thumbs_per_row) lafter++;


  if(table->mode == DT_THUMBTABLE_MODE_FILEMANAGER)
  {
    // in filemanager, no horizontal bar, and vertical bar reference is 1 thumb.
    dt_view_set_scrollbar(darktable.view_manager->current_view, 0, 0, 0, 0, lbefore, 0, lbefore + lafter,
                          table->rows - 1);
    table->code_scrolling = FALSE;
    return (lbefore + lafter > table->rows - 1);
  }
  else if(table->mode == DT_THUMBTABLE_MODE_ZOOM)
  {
    const int total_height
        = (lbefore + lafter) * table->thumb_size + 2 * (table->view_height - table->thumb_size * 0.5);
    const int pos_h
        = lbefore * table->thumb_size + table->view_height - table->thumb_size * 0.5 - table->thumbs_area.y;

    const int total_width
        = DT_ZOOMABLE_NB_PER_ROW * table->thumb_size + 2 * (table->view_width - table->thumb_size * 0.5);
    const int pos_w = table->view_width - table->thumb_size * 0.5 - table->thumbs_area.x;

    dt_view_set_scrollbar(darktable.view_manager->current_view, pos_w, 0, total_width, table->view_width, pos_h, 0,
                          total_height, table->view_height);

    table->code_scrolling = FALSE;
    return TRUE;
  }
  table->code_scrolling = FALSE;
  return FALSE;
}

// remove all uneeded thumbnails from the list and the widget
// uneeded == completly hidden
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

// load all needed thumbnails in the list and the widget
// needed == that should appear in the current view (possibly not entirely)
static int _thumbs_load_needed(dt_thumbtable_t *table)
{
  if(g_list_length(table->list) == 0) return 0;
  sqlite3_stmt *stmt;
  int changed = 0;

  // we rememeber image margins for new thumbs (this limit flickering)
  dt_thumbnail_t *first = (dt_thumbnail_t *)g_list_first(table->list)->data;
  const int old_margin_start = gtk_widget_get_margin_start(first->w_image_box);
  const int old_margin_top = gtk_widget_get_margin_top(first->w_image_box);

  // we load image at the beginning
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
      if(posy < table->view_height) // we don't load invisible thumbs
      {
        dt_thumbnail_t *thumb
            = dt_thumbnail_new(table->thumb_size, table->thumb_size, sqlite3_column_int(stmt, 1),
                               sqlite3_column_int(stmt, 0), table->overlays, FALSE, table->show_tooltips);
        if(table->mode == DT_THUMBTABLE_MODE_FILMSTRIP)
        {
          thumb->single_click = TRUE;
          thumb->sel_mode = DT_THUMBNAIL_SEL_MODE_MOD_ONLY;
        }
        thumb->x = posx;
        thumb->y = posy;
        table->list = g_list_prepend(table->list, thumb);
        gtk_widget_set_margin_start(thumb->w_image_box, old_margin_start);
        gtk_widget_set_margin_top(thumb->w_image_box, old_margin_top);
        gtk_layout_put(GTK_LAYOUT(table->widget), thumb->w_main, posx, posy);
        changed++;
      }
      _pos_get_previous(table, &posx, &posy);
    }
    g_free(query);
    sqlite3_finalize(stmt);
  }

  // we load images at the end
  dt_thumbnail_t *last = (dt_thumbnail_t *)g_list_last(table->list)->data;
  // if there's space under the last image, we have rows to load
  // if the last line is not full, we have already reached the end of the collection
  if((table->mode == DT_THUMBTABLE_MODE_FILEMANAGER && last->y + table->thumb_size < table->view_height
      && last->x >= table->thumb_size * (table->thumbs_per_row - 1))
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
      if(posy + table->thumb_size >= 0) // we don't load invisible thumbs
      {
        dt_thumbnail_t *thumb
            = dt_thumbnail_new(table->thumb_size, table->thumb_size, sqlite3_column_int(stmt, 1),
                               sqlite3_column_int(stmt, 0), table->overlays, FALSE, table->show_tooltips);
        if(table->mode == DT_THUMBTABLE_MODE_FILMSTRIP)
        {
          thumb->single_click = TRUE;
          thumb->sel_mode = DT_THUMBNAIL_SEL_MODE_MOD_ONLY;
        }
        thumb->x = posx;
        thumb->y = posy;
        table->list = g_list_append(table->list, thumb);
        gtk_widget_set_margin_start(thumb->w_image_box, old_margin_start);
        gtk_widget_set_margin_top(thumb->w_image_box, old_margin_top);
        gtk_layout_put(GTK_LAYOUT(table->widget), thumb->w_main, posx, posy);
        changed++;
      }
      _pos_get_next(table, &posx, &posy);
    }
    g_free(query);
    sqlite3_finalize(stmt);
  }

  return changed;
}

// move all thumbs from the table.
// if clamp, we verify that the move is allowed (collection bounds, etc...)
static gboolean _move(dt_thumbtable_t *table, const int x, const int y, gboolean clamp)
{
  if(!table->list || g_list_length(table->list) == 0) return FALSE;
  int posx = x;
  int posy = y;
  if(clamp)
  {
    // we check bounds to allow or not the move
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

      dt_thumbnail_t *last = (dt_thumbnail_t *)g_list_last(table->list)->data;
      if(table->thumbs_per_row == 1 && posy < 0 && g_list_length(table->list) == 1)
      {
        // special case for zoom == 1 as we don't want any space under last image (the image would have disappear)
        int nbid = 1;
        sqlite3_stmt *stmt;
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT COUNT(*) FROM memory.collected_images",
                                    -1, &stmt, NULL);
        if(sqlite3_step(stmt) == SQLITE_ROW) nbid = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
        if(nbid <= last->rowid) return FALSE;
      }
      else
      {
        // we stop when last image is fully shown (that means empty space at the bottom)
        if(last->y + table->thumb_size < table->view_height && posy < 0) return FALSE;
      }
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
      const int space = table->thumb_size * 0.5; // we want at least 1/2 thumb to stay visible
      posy = MIN(table->view_height - space - table->thumbs_area.y, posy);
      posy = MAX(space - table->thumbs_area.y - table->thumbs_area.height, posy);
      posx = MIN(table->view_width - space - table->thumbs_area.x, posx);
      posx = MAX(space - table->thumbs_area.x - table->thumbs_area.width, posx);
    }
  }

  if(posy == 0 && posx == 0) return FALSE;

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
  if(table->mode == DT_THUMBTABLE_MODE_ZOOM)
  {
    dt_conf_set_int("lighttable/zoomable/last_offset", table->offset);
  }

  // update scrollbars
  _thumbtable_update_scrollbars(table);

  return TRUE;
}

static dt_thumbnail_t *_thumbtable_get_thumb(dt_thumbtable_t *table, int imgid)
{
  if(imgid <= 0) return NULL;
  GList *l = table->list;
  while(l)
  {
    dt_thumbnail_t *th = (dt_thumbnail_t *)l->data;
    if(th->imgid == imgid) return th;
    l = g_list_next(l);
  }
  return NULL;
}

// change zoom value for the zoomable tumbtable
static void _zoomable_zoom(dt_thumbtable_t *table, int oldzoom, int newzoom)
{
  // determine the center of the zoom
  int x = 0;
  int y = 0;
  if(table->mouse_inside)
  {
    // if the mouse is inside the table, let's use his position
    gdk_window_get_origin(gtk_widget_get_window(table->widget), &x, &y);
    x = table->last_x - x;
    y = table->last_y - y;
  }
  else
  {
    // otherwise we use the center of the view
    x = table->view_width / 2;
    y = table->view_height / 2;
  }


  const int new_size = table->view_width / newzoom;
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
    dt_thumbnail_resize(th, new_size, new_size, FALSE);
    l = g_list_next(l);
  }

  // we update table values
  table->thumb_size = new_size;
  _pos_compute_area(table);

  // we ensure there's still some visible thumbnails
  const int space = new_size * 0.5; // we want at least 1/2 thumb to stay visible
  int posy = MIN(table->view_height - space - table->thumbs_area.y, 0);
  posy = MAX(space - table->thumbs_area.y - table->thumbs_area.height, posy);
  int posx = MIN(table->view_width - space - table->thumbs_area.x, 0);
  posx = MAX(space - table->thumbs_area.x - table->thumbs_area.width, posx);
  if(posx != 0 || posy != 0) _move(table, posx, posy, FALSE);

  // and we load/unload thumbs if needed
  int changed = _thumbs_load_needed(table);
  changed += _thumbs_remove_unneeded(table);
  if(changed > 0) _pos_compute_area(table);

  // we update all the values
  dt_thumbnail_t *first = (dt_thumbnail_t *)g_list_first(table->list)->data;
  table->offset = first->rowid;
  table->offset_imgid = first->imgid;
  dt_conf_set_int("plugins/lighttable/recentcollect/pos0", table->offset);
  dt_conf_set_int("lighttable/zoomable/last_offset", table->offset);
  dt_conf_set_int("lighttable/zoomable/last_pos_x", table->thumbs_area.x);
  dt_conf_set_int("lighttable/zoomable/last_pos_y", table->thumbs_area.y);

  dt_view_lighttable_set_zoom(darktable.view_manager, newzoom);
  gtk_widget_queue_draw(table->widget);
}

// change zoom value for the classic thumbtable
static void _filemanager_zoom(dt_thumbtable_t *table, int oldzoom, int newzoom)
{
  // we find the image to zoom around
  int x = 0;
  int y = 0;
  dt_thumbnail_t *thumb = NULL;
  if(table->mouse_inside)
  {
    // if the mouse is inside the table, let's use his position
    gdk_window_get_origin(gtk_widget_get_window(table->widget), &x, &y);
    x = table->last_x - x;
    y = table->last_y - y;
    thumb = _thumb_get_at_pos(table, x, y);
  }

  if(!thumb)
  {
    // otherwise we use the classic retrieving method
    const int id = dt_view_get_image_to_act_on();
    thumb = _thumbtable_get_thumb(table, id);
    if(thumb)
    {
      // and we take the center of the thumb
      x = thumb->x + thumb->width / 2;
      y = thumb->y + thumb->height / 2;
    }
    else
    {
      // still no thumb, try to use the one at screen center
      x = table->view_width / 2;
      y = table->view_height / 2;
      thumb = _thumb_get_at_pos(table, x, y);
      if(!thumb)
      {
        // and last, take the first at screen
        thumb = (dt_thumbnail_t *)g_list_nth_data(table->list, 0);
        x = thumb->x + thumb->width / 2;
        y = thumb->y + thumb->height / 2;
      }
    }
  }

  // how many images will be displayed before the current position ?
  const int new_size = table->view_width / newzoom;
  const int new_pos = y / new_size * newzoom + x / new_size;

  dt_thumbtable_set_offset(table, thumb->rowid - new_pos, FALSE);

  dt_view_lighttable_set_zoom(darktable.view_manager, newzoom);
  gtk_widget_queue_draw(table->widget);
}

void dt_thumbtable_zoom_changed(dt_thumbtable_t *table, const int oldzoom, const int newzoom)
{
  if(oldzoom == newzoom) return;
  if(!table->list || g_list_length(table->list) == 0) return;

  if(table->mode == DT_THUMBTABLE_MODE_FILEMANAGER)
  {
    _filemanager_zoom(table, oldzoom, newzoom);
  }
  else if(table->mode == DT_THUMBTABLE_MODE_ZOOM)
  {
    _zoomable_zoom(table, oldzoom, newzoom);
  }
}

static gboolean _event_scroll(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
  GdkEventScroll *e = (GdkEventScroll *)event;
  dt_thumbtable_t *table = (dt_thumbtable_t *)user_data;
  int delta;

  if(dt_gui_get_scroll_unit_delta(e, &delta))
  {
    if(table->mode == DT_THUMBTABLE_MODE_FILEMANAGER && (e->state & GDK_CONTROL_MASK) == GDK_CONTROL_MASK)
    {
      const int old = dt_view_lighttable_get_zoom(darktable.view_manager);
      int new = old;
      if(delta > 0)
        new = MIN(DT_LIGHTTABLE_MAX_ZOOM, new + 1);
      else
        new = MAX(1, new - 1);

      if(old != new) _filemanager_zoom(table, old, new);
    }
    else if(table->mode == DT_THUMBTABLE_MODE_FILEMANAGER || table->mode == DT_THUMBTABLE_MODE_FILMSTRIP)
    {
      // for filemanger and filmstrip, scrolled = move
      if(delta < 0 && table->mode == DT_THUMBTABLE_MODE_FILEMANAGER)
        _move(table, 0, table->thumb_size, TRUE);
      else if(delta < 0 && table->mode == DT_THUMBTABLE_MODE_FILMSTRIP)
        _move(table, table->thumb_size, 0, TRUE);
      if(delta >= 0 && table->mode == DT_THUMBTABLE_MODE_FILEMANAGER)
        _move(table, 0, -table->thumb_size, TRUE);
      else if(delta >= 0 && table->mode == DT_THUMBTABLE_MODE_FILMSTRIP)
        _move(table, -table->thumb_size, 0, TRUE);

      // ensure the hovered image is the right one
      dt_thumbnail_t *th = _thumb_get_under_mouse(table);
      if(th) dt_control_set_mouse_over_id(th->imgid);
    }
    else if(table->mode == DT_THUMBTABLE_MODE_ZOOM)
    {
      // for zoomable, scroll = zoom
      const int old = dt_view_lighttable_get_zoom(darktable.view_manager);
      int new = old;
      if(delta > 0)
        new = MIN(DT_LIGHTTABLE_MAX_ZOOM, new + 1);
      else
        new = MAX(1, new - 1);

      if(old != new) _zoomable_zoom(table, old, new);
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
  // if the leaving cause is the hide of the widget, no mouseover change
  if(!gtk_widget_is_visible(widget))
  {
    table->mouse_inside = FALSE;
    return FALSE;
  }

  // if we leave thumbtable in favour of an inferior (a thumbnail) it's not a real leave !
  if(event->detail == GDK_NOTIFY_INFERIOR) return FALSE;

  table->mouse_inside = FALSE;
  dt_control_set_mouse_over_id(-1);
  return TRUE;
}

static gboolean _event_enter_notify(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  // we only handle the case where we enter thumbtable from an inferior (a thumbnail)
  // this is when the mouse enter an "empty" area of thumbtable
  if(event->detail != GDK_NOTIFY_INFERIOR) return FALSE;

  dt_control_set_mouse_over_id(-1);
  return TRUE;
}

static gboolean _event_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_thumbtable_t *table = (dt_thumbtable_t *)user_data;
  dt_view_manager_t *vm = darktable.view_manager;
  dt_view_t *view = vm->current_view;
  const int id = dt_control_get_mouse_over_id();

  if(id > 0 && event->button == 1
     && (table->mode == DT_THUMBTABLE_MODE_FILEMANAGER || table->mode == DT_THUMBTABLE_MODE_ZOOM)
     && event->type == GDK_2BUTTON_PRESS)
  {
    dt_view_manager_switch(darktable.view_manager, "darkroom");
  }
  else if(id > 0 && event->button == 1 && table->mode == DT_THUMBTABLE_MODE_FILMSTRIP
          && event->type == GDK_BUTTON_PRESS && strcmp(view->module_name, "map")
          && (event->state & (GDK_SHIFT_MASK | GDK_CONTROL_MASK | GDK_MOD1_MASK)) == 0)
  {
    DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_VIEWMANAGER_THUMBTABLE_ACTIVATE, id);
  }

  if(event->button == 1 && event->type == GDK_BUTTON_PRESS)
  {
    // make sure any edition field loses the focus
    gtk_widget_grab_focus(dt_ui_center(darktable.gui->ui));
  }

  if(table->mode != DT_THUMBTABLE_MODE_ZOOM && id < 1 && event->button == 1 && event->type == GDK_BUTTON_PRESS)
  {
    // we click in an empty area, let's deselect all images
    dt_selection_clear(darktable.selection);
    return TRUE;
  }

  if(table->mode != DT_THUMBTABLE_MODE_ZOOM) return FALSE;

  if(event->button == 1 && event->type == GDK_BUTTON_PRESS)
  {
    table->dragging = TRUE;
    table->drag_dx = table->drag_dy = 0;
    GList *tl = g_list_find_custom(table->list, GINT_TO_POINTER(id), _list_compare_by_imgid);
    if(tl)
      table->drag_thumb = (dt_thumbnail_t *)tl->data;
    else
      table->drag_thumb = NULL;
  }
  return TRUE;
}

static gboolean _event_motion_notify(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
  dt_thumbtable_t *table = (dt_thumbtable_t *)user_data;
  table->mouse_inside = TRUE;

  if(table->dragging && table->mode == DT_THUMBTABLE_MODE_ZOOM)
  {
    const int dx = ceil(event->x_root) - table->last_x;
    const int dy = ceil(event->y_root) - table->last_y;
    _move(table, dx, dy, TRUE);
    table->drag_dx += dx;
    table->drag_dy += dy;
    if(table->drag_thumb)
    {
      // we only considers that this is a real move if the total distance is not too low
      table->drag_thumb->moved = ((abs(table->drag_dx) + abs(table->drag_dy)) > DT_PIXEL_APPLY_DPI(8));
    }
  }

  table->last_x = ceil(event->x_root);
  table->last_y = ceil(event->y_root);
  return TRUE;
}

static gboolean _event_button_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_thumbtable_t *table = (dt_thumbtable_t *)user_data;

  if(table->dragging == FALSE)
  {
    // on map view consider click release instead of press
    dt_view_manager_t *vm = darktable.view_manager;
    dt_view_t *view = vm->current_view;
    const int id = dt_control_get_mouse_over_id();
    if(id > 0 && event->button == 1 && table->mode == DT_THUMBTABLE_MODE_FILMSTRIP
            && event->type == GDK_BUTTON_RELEASE && !strcmp(view->module_name, "map")
            && (event->state & (GDK_SHIFT_MASK | GDK_CONTROL_MASK | GDK_MOD1_MASK)) == 0)
    {
      DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_VIEWMANAGER_THUMBTABLE_ACTIVATE, id);
      return TRUE;
    }
  }

  if(table->mode != DT_THUMBTABLE_MODE_ZOOM) return FALSE;

  table->dragging = FALSE;

  if((abs(table->drag_dx) + abs(table->drag_dy)) <= DT_PIXEL_APPLY_DPI(8) && dt_control_get_mouse_over_id() < 1)
  {
    // if we are on empty area and have detect no real movement, we deselect
    dt_selection_clear(darktable.selection);
  }

  // we ensure that all thumbnails moved property is reset
  GList *l = table->list;
  while(l)
  {
    dt_thumbnail_t *th = (dt_thumbnail_t *)l->data;
    th->moved = FALSE;
    l = g_list_next(l);
  }

  // we register the position
  dt_conf_set_int("lighttable/zoomable/last_pos_x", table->thumbs_area.x);
  dt_conf_set_int("lighttable/zoomable/last_pos_y", table->thumbs_area.y);
  return TRUE;
}

// set scrollbars visibility
static void _thumbtable_restore_scrollbars(dt_thumbtable_t *table)
{
  table->scrollbars = FALSE;

  if(table->mode == DT_THUMBTABLE_MODE_FILMSTRIP)
  {
    table->scrollbars = dt_conf_get_bool("darkroom/ui/scrollbars");
  }

  if(table->mode == DT_THUMBTABLE_MODE_FILEMANAGER)
  {
    table->scrollbars = dt_conf_get_bool("lighttable/ui/scrollbars");
  }

  dt_ui_scrollbars_show(darktable.gui->ui, table->scrollbars);
}

// called each time the preference change, to update specific parts
static void _dt_pref_change_callback(gpointer instance, gpointer user_data)
{
  if(!user_data) return;
  dt_thumbtable_t *table = (dt_thumbtable_t *)user_data;

  dt_thumbtable_full_redraw(table, TRUE);

  GList *l = table->list;
  while(l)
  {
    dt_thumbnail_t *th = (dt_thumbnail_t *)l->data;
    dt_thumbnail_reload_infos(th);
    dt_thumbnail_resize(th, th->width, th->height, TRUE);
    l = g_list_next(l);
  }
}

static void _dt_profile_change_callback(gpointer instance, int type, gpointer user_data)
{
  if(!user_data) return;
  dt_thumbtable_t *table = (dt_thumbtable_t *)user_data;

  GList *l = table->list;
  while(l)
  {
    dt_thumbnail_t *th = (dt_thumbnail_t *)l->data;
    dt_thumbnail_image_refresh(th);
    l = g_list_next(l);
  }
}

// this is called each time the list of active images change
static void _dt_active_images_callback(gpointer instance, gpointer user_data)
{
  // we only ensure here that the active image is the offset one
  // everything else (css, etc...) is handled in dt_thumbnail_t
  if(!user_data) return;
  dt_thumbtable_t *table = (dt_thumbtable_t *)user_data;

  if(g_slist_length(darktable.view_manager->active_images) == 0) return;
  int activeid = GPOINTER_TO_INT(g_slist_nth_data(darktable.view_manager->active_images, 0));
  dt_thumbtable_set_offset_image(table, activeid, TRUE);
}

// this is called each time mouse_over id change
static void _dt_mouse_over_image_callback(gpointer instance, gpointer user_data)
{
  if(!user_data) return;
  dt_thumbtable_t *table = (dt_thumbtable_t *)user_data;

  const int imgid = dt_control_get_mouse_over_id();

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
          // left border
          if(pos != 0 && th->x != table->thumbs_area.x)
          {
            dt_thumbnail_t *th1 = (dt_thumbnail_t *)g_list_nth_data(table->list, pos - 1);
            if(th1->groupid == groupid) b = FALSE;
          }
          if(b)
          {
            dt_thumbnail_set_group_border(th, DT_THUMBNAIL_BORDER_LEFT);
          }
          // right border
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

        // top border
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
        // bottom border
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
                                            const int next, gpointer user_data)
{
  if(!user_data) return;
  dt_thumbtable_t *table = (dt_thumbtable_t *)user_data;
  if(query_change == DT_COLLECTION_CHANGE_RELOAD)
  {
    int old_hover = dt_control_get_mouse_over_id();
    /** Here's how it works
     *
     *          list of change|   | x | x | x | x |
     *  offset inside the list| ? |   | x | x | x |
     * offset rowid as changed| ? | ? |   | x | x |
     *     next imgid is valid| ? | ? | ? |   | x |
     *                        |   |   |   |   |   |
     *                        | S | S | S | S | N |
     * S = same imgid as offset ; N = next imgid as offset
     **/

    // in filmstrip mode, let's first ensure the offset is the right one. Otherwise we move to it
    int old_offset = -1;
    if(table->mode == DT_THUMBTABLE_MODE_FILMSTRIP && g_slist_length(darktable.view_manager->active_images) > 0)
    {
      const int tmpoff = GPOINTER_TO_INT(g_slist_nth_data(darktable.view_manager->active_images, 0));
      if(tmpoff != table->offset_imgid)
      {
        old_offset = table->offset_imgid;
        table->offset = _thumb_get_rowid(tmpoff);
        table->offset_imgid = tmpoff;
        dt_thumbtable_full_redraw(table, TRUE);
      }
    }
    int newid = table->offset_imgid;
    if(newid <= 0 && table->offset > 0) newid = _thumb_get_imgid(table->offset);

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
        // but we have to ensure next is in the selection if we navigate inside sel.
        newid = next;
        if(table->navigate_inside_selection)
        {
          sqlite3_stmt *stmt;
          gchar *query = dt_util_dstrcat(
              NULL,
              "SELECT m.imgid FROM memory.collected_images as m, main.selected_images as s "
              "WHERE m.imgid=s.imgid AND m.rowid>=(SELECT rowid FROM memory.collected_images WHERE imgid=%d) "
              "ORDER BY m.rowid LIMIT 1",
              next);
          DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
          if(sqlite3_step(stmt) == SQLITE_ROW)
          {
            newid = sqlite3_column_int(stmt, 0);
          }
          else
          {
            // no select image after, search before
            g_free(query);
            sqlite3_finalize(stmt);
            query = dt_util_dstrcat(
                NULL,
                "SELECT m.imgid FROM memory.collected_images as m, main.selected_images as s "
                "WHERE m.imgid=s.imgid AND m.rowid<(SELECT rowid FROM memory.collected_images WHERE imgid=%d) "
                "ORDER BY m.rowid DESC LIMIT 1",
                next);
            DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
            if(sqlite3_step(stmt) == SQLITE_ROW)
            {
              newid = sqlite3_column_int(stmt, 0);
            }
          }
          g_free(query);
          sqlite3_finalize(stmt);
        }
      }
    }

    // get the new rowid of the new offset image
    const int nrow = _thumb_get_rowid(newid);
    const gboolean offset_changed = (MAX(1, nrow) != table->offset);
    if(nrow >= 1)
      table->offset_imgid = newid;
    else
      table->offset_imgid = _thumb_get_imgid(1);
    table->offset = MAX(1, nrow);
    if(offset_changed) dt_conf_set_int("plugins/lighttable/recentcollect/pos0", table->offset);
    if(offset_changed && table->mode == DT_THUMBTABLE_MODE_ZOOM)
      dt_conf_set_int("lighttable/zoomable/last_offset", table->offset);

    dt_thumbtable_full_redraw(table, TRUE);

    if(offset_changed) dt_view_lighttable_change_offset(darktable.view_manager, FALSE, newid);
    else
    {
      // if we are in culling or preview mode, ensure to refresh active images
      dt_view_lighttable_culling_preview_refresh(darktable.view_manager);
    }

    // if needed, we restore back the position of the filmstrip
    if(old_offset > 0 && old_offset != table->offset)
    {
      const int tmpoff = _thumb_get_rowid(old_offset);
      if(tmpoff > 0)
      {
        table->offset = tmpoff;
        table->offset_imgid = old_offset;
        dt_thumbtable_full_redraw(table, TRUE);
      }
    }

    // if the previous hovered image isn't here anymore, try to hover "next" image
    if(old_hover > 0 && next > 0)
    {
      in_list = FALSE;
      gboolean in_list_next = FALSE;
      l = table->list;
      while(l)
      {
        dt_thumbnail_t *thumb = (dt_thumbnail_t *)l->data;
        if(thumb->imgid == old_hover) in_list = TRUE;
        if(thumb->imgid == next) in_list_next = TRUE;
        l = g_list_next(l);
      }
      if(!in_list && in_list_next) dt_control_set_mouse_over_id(next);
    }
    dt_control_queue_redraw_center();
  }
  else
  {
    // otherwise we reset the offset to the beginning
    table->offset = 1;
    table->offset_imgid = _thumb_get_imgid(table->offset);
    dt_conf_set_int("plugins/lighttable/recentcollect/pos0", 1);
    dt_conf_set_int("lighttable/zoomable/last_offset", 1);
    dt_conf_set_int("lighttable/zoomable/last_pos_x", 0);
    dt_conf_set_int("lighttable/zoomable/last_pos_y", 0);
    dt_thumbtable_full_redraw(table, TRUE);
    dt_view_lighttable_change_offset(darktable.view_manager, TRUE, table->offset_imgid);
  }
}

static void _event_dnd_get(GtkWidget *widget, GdkDragContext *context, GtkSelectionData *selection_data,
                           const guint target_type, const guint time, gpointer user_data)
{
  dt_thumbtable_t *table = (dt_thumbtable_t *)user_data;
  g_assert(selection_data != NULL);

  switch(target_type)
  {
    case DND_TARGET_IMGID:
    {
      const int imgs_nb = g_list_length(table->drag_list);
      if(imgs_nb)
      {
        uint32_t *imgs = malloc(imgs_nb * sizeof(uint32_t));
        GList *l = table->drag_list;
        for(int i = 0; i < imgs_nb; i++)
        {
          imgs[i] = GPOINTER_TO_INT(l->data);
          l = g_list_next(l);
        }
        gtk_selection_data_set(selection_data, gtk_selection_data_get_target(selection_data),
                               _DWORD, (guchar *)imgs, imgs_nb * sizeof(uint32_t));
        free(imgs);
      }
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
        gtk_selection_data_set(selection_data, gtk_selection_data_get_target(selection_data),
                               _BYTE, (guchar *)uri, strlen(uri));
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
  const int ts = DT_PIXEL_APPLY_DPI(128);

  dt_thumbtable_t *table = (dt_thumbtable_t *)user_data;

  table->drag_list = g_list_copy((GList *)dt_view_get_images_to_act_on(FALSE, TRUE));

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
  dt_gui_add_help_link(table->widget, dt_get_help_url("lighttable_filemanager"));

  // set css name and class
  gtk_widget_set_name(table->widget, "thumbtable_filemanager");
  GtkStyleContext *context = gtk_widget_get_style_context(table->widget);
  gtk_style_context_add_class(context, "dt_thumbtable");
  if(dt_conf_get_bool("lighttable/ui/expose_statuses")) gtk_style_context_add_class(context, "dt_show_overlays");

  // overlays mode
  table->overlays = DT_THUMBNAIL_OVERLAYS_NONE;
  gchar *cl = _thumbs_get_overlays_class(table->overlays);
  gtk_style_context_add_class(context, cl);
  g_free(cl);

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
  g_signal_connect(G_OBJECT(table->widget), "enter-notify-event", G_CALLBACK(_event_enter_notify), table);
  g_signal_connect(G_OBJECT(table->widget), "button-press-event", G_CALLBACK(_event_button_press), table);
  g_signal_connect(G_OBJECT(table->widget), "motion-notify-event", G_CALLBACK(_event_motion_notify), table);
  g_signal_connect(G_OBJECT(table->widget), "button-release-event", G_CALLBACK(_event_button_release), table);

  // we register globals signals
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_COLLECTION_CHANGED,
                            G_CALLBACK(_dt_collection_changed_callback), table);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_MOUSE_OVER_IMAGE_CHANGE,
                            G_CALLBACK(_dt_mouse_over_image_callback), table);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_ACTIVE_IMAGES_CHANGE,
                            G_CALLBACK(_dt_active_images_callback), table);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_CONTROL_PROFILE_USER_CHANGED,
                            G_CALLBACK(_dt_profile_change_callback), table);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_PREFERENCES_CHANGE,
                            G_CALLBACK(_dt_pref_change_callback), table);
  gtk_widget_show(table->widget);

  g_object_ref(table->widget);

  // we init key accels
  dt_thumbtable_init_accels(table);

  return table;
}

void dt_thumbtable_scrollbar_changed(dt_thumbtable_t *table, const int x, const int y)
{
  if(g_list_length(table->list) == 0 || table->code_scrolling || !table->scrollbars) return;

  if(table->mode == DT_THUMBTABLE_MODE_FILEMANAGER)
  {
    const int first_offset = (table->offset - 1) % table->thumbs_per_row;
    int new_offset = table->offset;
    if(first_offset == 0)
    {
      // first line is full, so it's counted
      new_offset = 1 + y * table->thumbs_per_row;
    }
    else if(y == 0)
    {
      new_offset = 1;
    }
    else
    {
      new_offset = first_offset + (y - 1) * table->thumbs_per_row;
    }

    if(new_offset != table->offset)
    {
      table->offset = new_offset;
      dt_thumbtable_full_redraw(table, TRUE);
    }
  }
  else if(table->mode == DT_THUMBTABLE_MODE_ZOOM)
  {
    // we get the actual root position
    int lbefore = (table->offset - 1) / table->thumbs_per_row;
    if((table->offset - 1) % table->thumbs_per_row) lbefore++;
    const int abs_posy
        = lbefore * table->thumb_size + table->view_height - table->thumb_size * 0.5 - table->thumbs_area.y;
    const int abs_posx = table->view_width - table->thumb_size * 0.5 - table->thumbs_area.x;

    // and we move
    _move(table, abs_posx - x, abs_posy - y, FALSE);
  }
}

// reload all thumbs from scratch.
// force define if this should occurs in any case or just if thumbtable sizing properties have changed
void dt_thumbtable_full_redraw(dt_thumbtable_t *table, gboolean force)
{
  if(!table) return;
  if(_compute_sizes(table, force))
  {
    // we update the scrollbars
    _thumbtable_update_scrollbars(table);

    const double start = dt_get_wtime();
    table->dragging = FALSE;
    sqlite3_stmt *stmt;
    dt_print(DT_DEBUG_LIGHTTABLE,
             "reload thumbs from db. force=%d w=%d h=%d zoom=%d rows=%d size=%d offset=%d centering=%d...\n",
             force, table->view_width, table->view_height, table->thumbs_per_row, table->rows, table->thumb_size,
             table->offset, table->center_offset);

    int posx = 0;
    int posy = 0;
    int offset = table->offset;
    int empty_start = 0;

    if(table->mode == DT_THUMBTABLE_MODE_ZOOM)
    {
      // retrieve old values to avoid layout modifications
      posx = dt_conf_get_int("lighttable/zoomable/last_pos_x");
      posy = dt_conf_get_int("lighttable/zoomable/last_pos_y");
      offset = dt_conf_get_int("lighttable/zoomable/last_offset");
      // ensure that the overall layout doesn't change
      // (i.e. we don't get empty spaces in the very first row)
      offset = (offset - 1) / table->thumbs_per_row * table->thumbs_per_row + 1;
      table->thumbs_area.x = posx;
      table->thumbs_area.y = posy;
    }
    else if(table->mode == DT_THUMBTABLE_MODE_FILEMANAGER)
    {
      // in filemanager, we need to take care of the center offset
      posx = table->center_offset;

      // ensure that the overall layout doesn't change
      // (i.e. we don't get empty spaces in the very first row)
      const int offset_row = (table->offset-1) / table->thumbs_per_row;
      offset = offset_row * table->thumbs_per_row + 1;
    }
    else if(table->mode == DT_THUMBTABLE_MODE_FILMSTRIP)
    {
      // in filmstrip, the offset is the centered image, so we need to find the first image to load
      offset = MAX(1, table->offset - table->rows / 2);
      empty_start = -MIN(0, table->offset - table->rows / 2 - 1);
      posx = (table->view_width - table->rows * table->thumb_size) / 2;
      posx += empty_start * table->thumb_size;
    }

    // we store image margin from frist thumb to apply to new ones and limit flickering
    int old_margin_start = 0;
    int old_margin_top = 0;
    if(g_list_length(table->list) > 0)
    {
      dt_thumbnail_t *first = (dt_thumbnail_t *)g_list_first(table->list)->data;
      old_margin_start = gtk_widget_get_margin_start(first->w_image_box);
      old_margin_top = gtk_widget_get_margin_top(first->w_image_box);
      // if margins > thumb size, then margins are irrelevant (thumb size as just changed), better set them to 0
      if(old_margin_start >= table->thumb_size || old_margin_top >= table->thumb_size)
      {
        old_margin_start = 0;
        old_margin_top = 0;
      }
    }

    // we add the thumbs
    GList *newlist = NULL;
    int nbnew = 0;
    gchar *query
        = dt_util_dstrcat(NULL, "SELECT rowid, imgid FROM memory.collected_images WHERE rowid>=%d LIMIT %d",
                          offset, table->rows * table->thumbs_per_row - empty_start);
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
    while(sqlite3_step(stmt) == SQLITE_ROW)
    {
      const int nrow = sqlite3_column_int(stmt, 0);
      const int nid = sqlite3_column_int(stmt, 1);

      // first, we search if the thumb is already here
      GList *tl = g_list_find_custom(table->list, GINT_TO_POINTER(nid), _list_compare_by_imgid);
      if(tl)
      {
        dt_thumbnail_t *thumb = (dt_thumbnail_t *)tl->data;
        GtkStyleContext *context = gtk_widget_get_style_context(thumb->w_main);
        gtk_style_context_remove_class(context, "dt_last_active");
        thumb->rowid = nrow; // this may have changed
        // we set new position/size if needed
        if(thumb->x != posx || thumb->y != posy)
        {
          thumb->x = posx;
          thumb->y = posy;
          gtk_layout_move(GTK_LAYOUT(table->widget), thumb->w_main, posx, posy);
        }
        dt_thumbnail_resize(thumb, table->thumb_size, table->thumb_size, FALSE);
        newlist = g_list_append(newlist, thumb);
        // and we remove the thumb from the old list
        table->list = g_list_remove(table->list, thumb);
      }
      else
      {
        // we create a completly new thumb
        dt_thumbnail_t *thumb = dt_thumbnail_new(table->thumb_size, table->thumb_size, nid, nrow, table->overlays,
                                                 FALSE, table->show_tooltips);
        if(table->mode == DT_THUMBTABLE_MODE_FILMSTRIP)
        {
          thumb->single_click = TRUE;
          thumb->sel_mode = DT_THUMBNAIL_SEL_MODE_MOD_ONLY;
        }
        thumb->x = posx;
        thumb->y = posy;
        newlist = g_list_append(newlist, thumb);
        gtk_widget_set_margin_start(thumb->w_image_box, old_margin_start);
        gtk_widget_set_margin_top(thumb->w_image_box, old_margin_top);
        gtk_layout_put(GTK_LAYOUT(table->widget), thumb->w_main, posx, posy);
        nbnew++;
      }
      _pos_get_next(table, &posx, &posy);
      // if it's the offset, we record the imgid
      if(nrow == table->offset) table->offset_imgid = nid;
    }

    // now we cleanup all remaining thumbs from old table->list and set it again
    g_list_free_full(table->list, _list_remove_thumb);
    table->list = newlist;

    _pos_compute_area(table);

    // we need to ensure there's no need to load other image on top/bottom
    if(table->mode == DT_THUMBTABLE_MODE_ZOOM) nbnew += _thumbs_load_needed(table);

    if(g_slist_length(darktable.view_manager->active_images) > 0
       && (table->mode == DT_THUMBTABLE_MODE_ZOOM || table->mode == DT_THUMBTABLE_MODE_FILEMANAGER)
       && gtk_widget_is_visible(table->widget))
    {
      // this mean we arrive from filmstrip with some active images
      // we need to ensure they are visible and to mark them with some css effect
      const int lastid = GPOINTER_TO_INT(g_slist_last(darktable.view_manager->active_images)->data);
      dt_thumbtable_ensure_imgid_visibility(table, lastid);

      GSList *l = darktable.view_manager->active_images;
      while(l)
      {
        dt_thumbnail_t *th = _thumbtable_get_thumb(table, GPOINTER_TO_INT(l->data));
        if(th)
        {
          GtkStyleContext *context = gtk_widget_get_style_context(th->w_main);
          gtk_style_context_add_class(context, "dt_last_active");
        }
        l = g_slist_next(l);
      }
      g_slist_free(darktable.view_manager->active_images);
      darktable.view_manager->active_images = NULL;
      DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_ACTIVE_IMAGES_CHANGE);
    }

    // if we force the redraw, we ensure selection is updated
    if(force) DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_SELECTION_CHANGED);

    // be sure the focus is in the right widget (needed for accels)
    gtk_widget_grab_focus(dt_ui_center(darktable.gui->ui));

    dt_print(DT_DEBUG_LIGHTTABLE, "done in %0.04f sec %d thumbs reloaded\n", dt_get_wtime() - start, nbnew);
    g_free(query);
    sqlite3_finalize(stmt);

    if(darktable.unmuted & DT_DEBUG_CACHE) dt_mipmap_cache_print(darktable.mipmap_cache);
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
    {
      gtk_widget_set_name(table->widget, "thumbtable_filemanager");
      dt_gui_add_help_link(table->widget, dt_get_help_url("lighttable_filemanager"));
    }
    else if(mode == DT_THUMBTABLE_MODE_FILMSTRIP)
    {
      gtk_widget_set_name(table->widget, "thumbtable_filmstrip");
      dt_gui_add_help_link(table->widget, dt_get_help_url("filmstrip"));
    }
    else if(mode == DT_THUMBTABLE_MODE_ZOOM)
    {
      gtk_widget_set_name(table->widget, "thumbtable_zoom");
      dt_gui_add_help_link(table->widget, dt_get_help_url("lighttable_zoomable"));
    }

    // if needed, we block/unblock drag and drop
    if(mode == DT_THUMBTABLE_MODE_ZOOM)
    {
      gtk_drag_source_unset(table->widget);
    }
    else if(table->mode == DT_THUMBTABLE_MODE_ZOOM)
    {
      gtk_drag_source_set(table->widget, GDK_BUTTON1_MASK, target_list_all, n_targets_all, GDK_ACTION_COPY);
    }

    // we set selection/activation properties of all thumbs
    dt_thumbnail_selection_mode_t sel_mode = DT_THUMBNAIL_SEL_MODE_NORMAL;
    gboolean single_click = FALSE;
    if(mode == DT_THUMBTABLE_MODE_FILMSTRIP)
    {
      sel_mode = DT_THUMBNAIL_SEL_MODE_MOD_ONLY;
      single_click = TRUE;
    }
    GList *l = table->list;
    while(l)
    {
      dt_thumbnail_t *th = (dt_thumbnail_t *)l->data;
      th->sel_mode = sel_mode;
      th->single_click = single_click;
      l = g_list_next(l);
    }

    table->mode = mode;

    // we force overlays update as the size may not change in certain cases
    _thumbs_update_overlays_mode(table);
  }

  // do we show scrollbars ?
  table->code_scrolling = TRUE;
  _thumbtable_restore_scrollbars(table);

  // we reparent the table
  if(!parent || parent != new_parent)
  {
    if(GTK_IS_OVERLAY(new_parent))
    {
      gtk_overlay_add_overlay(GTK_OVERLAY(new_parent), table->widget);
      // be sure that log msg is always placed on top
      if(new_parent == dt_ui_center_base(darktable.gui->ui))
      {
        gtk_overlay_reorder_overlay(GTK_OVERLAY(dt_ui_center_base(darktable.gui->ui)),
                                    gtk_widget_get_parent(dt_ui_log_msg(darktable.gui->ui)), -1);
        gtk_overlay_reorder_overlay(GTK_OVERLAY(dt_ui_center_base(darktable.gui->ui)),
                                    gtk_widget_get_parent(dt_ui_toast_msg(darktable.gui->ui)), -1);
      }
    }
    else
      gtk_container_add(GTK_CONTAINER(new_parent), table->widget);
  }
  table->code_scrolling = FALSE;
}

// get current offset
int dt_thumbtable_get_offset(dt_thumbtable_t *table)
{
  return table->offset;
}
// set offset and redraw if needed
gboolean dt_thumbtable_set_offset(dt_thumbtable_t *table, const int offset, const gboolean redraw)
{
  if(offset < 1 || offset == table->offset) return FALSE;
  table->offset = offset;
  dt_conf_set_int("plugins/lighttable/recentcollect/pos0", table->offset);
  if(redraw) dt_thumbtable_full_redraw(table, TRUE);
  return TRUE;
}

// set offset at specific imgid and redraw if needed
gboolean dt_thumbtable_set_offset_image(dt_thumbtable_t *table, const int imgid, const gboolean redraw)
{
  table->offset_imgid = imgid;
  return dt_thumbtable_set_offset(table, _thumb_get_rowid(imgid), redraw);
}

static gboolean _accel_rate(GtkAccelGroup *accel_group, GObject *acceleratable, const guint keyval,
                            GdkModifierType modifier, gpointer data)
{
  GList *imgs = g_list_copy((GList *)dt_view_get_images_to_act_on(FALSE, TRUE));
  dt_ratings_apply_on_list(imgs, GPOINTER_TO_INT(data), TRUE);
  dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, imgs);
  return TRUE;
}
static gboolean _accel_color(GtkAccelGroup *accel_group, GObject *acceleratable, const guint keyval,
                             GdkModifierType modifier, gpointer data)
{
  GList *imgs = g_list_copy((GList *)dt_view_get_images_to_act_on(FALSE, TRUE));
  dt_colorlabels_toggle_label_on_list(imgs, GPOINTER_TO_INT(data), TRUE);
  dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, imgs);
  return TRUE;
}
static gboolean _accel_copy(GtkAccelGroup *accel_group, GObject *acceleratable, const guint keyval,
                            GdkModifierType modifier, gpointer data)
{
  dt_history_copy(dt_view_get_image_to_act_on());
  return TRUE;
}
static gboolean _accel_copy_parts(GtkAccelGroup *accel_group, GObject *acceleratable, const guint keyval,
                                  GdkModifierType modifier, gpointer data)
{
  dt_history_copy_parts(dt_view_get_image_to_act_on());
  return TRUE;
}
static gboolean _accel_paste(GtkAccelGroup *accel_group, GObject *acceleratable, const guint keyval,
                             GdkModifierType modifier, gpointer data)
{
  GList *imgs = g_list_copy((GList *)dt_view_get_images_to_act_on(TRUE, TRUE));
  const gboolean ret = dt_history_paste_on_list(imgs, TRUE);
  if(ret) dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, imgs);
  return TRUE;
}
static gboolean _accel_paste_parts(GtkAccelGroup *accel_group, GObject *acceleratable, const guint keyval,
                                   GdkModifierType modifier, gpointer data)
{
  GList *imgs = g_list_copy((GList *)dt_view_get_images_to_act_on(TRUE, TRUE));
  const gboolean ret = dt_history_paste_parts_on_list(imgs, TRUE);
  if(ret) dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, imgs);
  return TRUE;
}
static gboolean _accel_hist_discard(GtkAccelGroup *accel_group, GObject *acceleratable, const guint keyval,
                                    GdkModifierType modifier, gpointer data)
{
  GList *imgs = g_list_copy((GList *)dt_view_get_images_to_act_on(TRUE, TRUE));
  const gboolean ret = dt_history_delete_on_list(imgs, TRUE);
  if(ret) dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, imgs);
  return TRUE;
}
static gboolean _accel_duplicate(GtkAccelGroup *accel_group, GObject *acceleratable, const guint keyval,
                                 GdkModifierType modifier, gpointer data)
{
  const int sourceid = dt_view_get_image_to_act_on();
  const int newimgid = dt_image_duplicate(sourceid);
  if(newimgid <= 0) return FALSE;

  if(GPOINTER_TO_INT(data))
    dt_history_delete_on_image(newimgid);
  else
    dt_history_copy_and_paste_on_image(sourceid, newimgid, FALSE, NULL, TRUE, TRUE);

  dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, NULL);
  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_TAG_CHANGED);
  return TRUE;
}
static gboolean _accel_select_all(GtkAccelGroup *accel_group, GObject *acceleratable, const guint keyval,
                                  GdkModifierType modifier, gpointer data)
{
  dt_selection_select_all(darktable.selection);
  return TRUE;
}
static gboolean _accel_select_none(GtkAccelGroup *accel_group, GObject *acceleratable, const guint keyval,
                                   GdkModifierType modifier, gpointer data)
{
  dt_selection_clear(darktable.selection);
  return TRUE;
}
static gboolean _accel_select_invert(GtkAccelGroup *accel_group, GObject *acceleratable, const guint keyval,
                                     GdkModifierType modifier, gpointer data)
{
  dt_selection_invert(darktable.selection);
  return TRUE;
}
static gboolean _accel_select_film(GtkAccelGroup *accel_group, GObject *acceleratable, const guint keyval,
                                   GdkModifierType modifier, gpointer data)
{
  dt_selection_select_filmroll(darktable.selection);
  return TRUE;
}
static gboolean _accel_select_untouched(GtkAccelGroup *accel_group, GObject *acceleratable, const guint keyval,
                                        GdkModifierType modifier, gpointer data)
{
  dt_selection_select_unaltered(darktable.selection);
  return TRUE;
}

// init all accels
void dt_thumbtable_init_accels(dt_thumbtable_t *table)
{
  const dt_view_type_flags_t views
      = DT_VIEW_LIGHTTABLE | DT_VIEW_DARKROOM | DT_VIEW_MAP | DT_VIEW_TETHERING | DT_VIEW_PRINT;
  const dt_view_type_flags_t views_nolt = DT_VIEW_DARKROOM | DT_VIEW_MAP | DT_VIEW_TETHERING | DT_VIEW_PRINT;
  /* setup rating key accelerators */
  dt_accel_register_manual(NC_("accel", "views/thumbtable/rate 0"), views, GDK_KEY_0, 0);
  dt_accel_register_manual(NC_("accel", "views/thumbtable/rate 1"), views, GDK_KEY_1, 0);
  dt_accel_register_manual(NC_("accel", "views/thumbtable/rate 2"), views, GDK_KEY_2, 0);
  dt_accel_register_manual(NC_("accel", "views/thumbtable/rate 3"), views, GDK_KEY_3, 0);
  dt_accel_register_manual(NC_("accel", "views/thumbtable/rate 4"), views, GDK_KEY_4, 0);
  dt_accel_register_manual(NC_("accel", "views/thumbtable/rate 5"), views, GDK_KEY_5, 0);
  dt_accel_register_manual(NC_("accel", "views/thumbtable/rate reject"), views, GDK_KEY_r, 0);

  /* setup history key accelerators */
  dt_accel_register_manual(NC_("accel", "views/thumbtable/copy history"), views_nolt, GDK_KEY_c, GDK_CONTROL_MASK);
  dt_accel_register_manual(NC_("accel", "views/thumbtable/copy history parts"), views_nolt, GDK_KEY_c,
                           GDK_CONTROL_MASK | GDK_SHIFT_MASK);
  dt_accel_register_manual(NC_("accel", "views/thumbtable/paste history"), views_nolt, GDK_KEY_v, GDK_CONTROL_MASK);
  dt_accel_register_manual(NC_("accel", "views/thumbtable/paste history parts"), views_nolt, GDK_KEY_v,
                           GDK_CONTROL_MASK | GDK_SHIFT_MASK);
  dt_accel_register_manual(NC_("accel", "views/thumbtable/discard history"), views_nolt, 0, 0);

  dt_accel_register_manual(NC_("accel", "views/thumbtable/duplicate image"), views, GDK_KEY_d, GDK_CONTROL_MASK);
  dt_accel_register_manual(NC_("accel", "views/thumbtable/duplicate image virgin"), views, GDK_KEY_d,
                           GDK_CONTROL_MASK | GDK_SHIFT_MASK);

  /* setup color label accelerators */
  dt_accel_register_manual(NC_("accel", "views/thumbtable/color red"), views, GDK_KEY_F1, 0);
  dt_accel_register_manual(NC_("accel", "views/thumbtable/color yellow"), views, GDK_KEY_F2, 0);
  dt_accel_register_manual(NC_("accel", "views/thumbtable/color green"), views, GDK_KEY_F3, 0);
  dt_accel_register_manual(NC_("accel", "views/thumbtable/color blue"), views, GDK_KEY_F4, 0);
  dt_accel_register_manual(NC_("accel", "views/thumbtable/color purple"), views, GDK_KEY_F5, 0);
  dt_accel_register_manual(NC_("accel", "views/thumbtable/clear color labels"), views, 0, 0);

  /* setup selection accelerators */
  dt_accel_register_manual(NC_("accel", "views/thumbtable/select all"), views, GDK_KEY_a, GDK_CONTROL_MASK);
  dt_accel_register_manual(NC_("accel", "views/thumbtable/select none"), views, GDK_KEY_a,
                           GDK_CONTROL_MASK | GDK_SHIFT_MASK);
  dt_accel_register_manual(NC_("accel", "views/thumbtable/invert selection"), views, GDK_KEY_i, GDK_CONTROL_MASK);
  dt_accel_register_manual(NC_("accel", "views/thumbtable/select film roll"), views, 0, 0);
  dt_accel_register_manual(NC_("accel", "views/thumbtable/select untouched"), views, 0, 0);
}
// connect all accels if thumbtable is active in the view and they are not loaded
// disconnect them if not
void dt_thumbtable_update_accels_connection(dt_thumbtable_t *table, const int view)
{
  //disconnect all accels and reconnect if thumbtable may be active for this view

  dt_accel_disconnect_list(&table->accel_closures);

  if((view & DT_VIEW_LIGHTTABLE) || (view & DT_VIEW_DARKROOM) || (view & DT_VIEW_TETHERING)
     || (view & DT_VIEW_MAP) || (view & DT_VIEW_PRINT))
  {
    // Rating accels
    dt_accel_connect_manual(&table->accel_closures, "views/thumbtable/rate 0",
                            g_cclosure_new(G_CALLBACK(_accel_rate), GINT_TO_POINTER(DT_VIEW_DESERT), NULL));
    dt_accel_connect_manual(&table->accel_closures, "views/thumbtable/rate 1",
                            g_cclosure_new(G_CALLBACK(_accel_rate), GINT_TO_POINTER(DT_VIEW_STAR_1), NULL));
    dt_accel_connect_manual(&table->accel_closures, "views/thumbtable/rate 2",
                            g_cclosure_new(G_CALLBACK(_accel_rate), GINT_TO_POINTER(DT_VIEW_STAR_2), NULL));
    dt_accel_connect_manual(&table->accel_closures, "views/thumbtable/rate 3",
                            g_cclosure_new(G_CALLBACK(_accel_rate), GINT_TO_POINTER(DT_VIEW_STAR_3), NULL));
    dt_accel_connect_manual(&table->accel_closures, "views/thumbtable/rate 4",
                            g_cclosure_new(G_CALLBACK(_accel_rate), GINT_TO_POINTER(DT_VIEW_STAR_4), NULL));
    dt_accel_connect_manual(&table->accel_closures, "views/thumbtable/rate 5",
                            g_cclosure_new(G_CALLBACK(_accel_rate), GINT_TO_POINTER(DT_VIEW_STAR_5), NULL));
    dt_accel_connect_manual(&table->accel_closures, "views/thumbtable/rate reject",
                            g_cclosure_new(G_CALLBACK(_accel_rate), GINT_TO_POINTER(DT_VIEW_REJECT), NULL));

    // History key accels
    if(!(view & DT_VIEW_LIGHTTABLE))
    {
      dt_accel_connect_manual(&table->accel_closures, "views/thumbtable/copy history",
                              g_cclosure_new(G_CALLBACK(_accel_copy), NULL, NULL));
      dt_accel_connect_manual(&table->accel_closures, "views/thumbtable/copy history parts",
                              g_cclosure_new(G_CALLBACK(_accel_copy_parts), NULL, NULL));
      dt_accel_connect_manual(&table->accel_closures, "views/thumbtable/paste history",
                              g_cclosure_new(G_CALLBACK(_accel_paste), NULL, NULL));
      dt_accel_connect_manual(&table->accel_closures, "views/thumbtable/paste history parts",
                              g_cclosure_new(G_CALLBACK(_accel_paste_parts), NULL, NULL));
      dt_accel_connect_manual(&table->accel_closures, "views/thumbtable/discard history",
                              g_cclosure_new(G_CALLBACK(_accel_hist_discard), NULL, NULL));
    }

    dt_accel_connect_manual(&table->accel_closures, "views/thumbtable/duplicate image",
                            g_cclosure_new(G_CALLBACK(_accel_duplicate), GINT_TO_POINTER(0), NULL));
    dt_accel_connect_manual(&table->accel_closures, "views/thumbtable/duplicate image virgin",
                            g_cclosure_new(G_CALLBACK(_accel_duplicate), GINT_TO_POINTER(1), NULL));

    // Color label accels
    dt_accel_connect_manual(&table->accel_closures, "views/thumbtable/color red",
                            g_cclosure_new(G_CALLBACK(_accel_color), GINT_TO_POINTER(0), NULL));
    dt_accel_connect_manual(&table->accel_closures, "views/thumbtable/color yellow",
                            g_cclosure_new(G_CALLBACK(_accel_color), GINT_TO_POINTER(1), NULL));
    dt_accel_connect_manual(&table->accel_closures, "views/thumbtable/color green",
                            g_cclosure_new(G_CALLBACK(_accel_color), GINT_TO_POINTER(2), NULL));
    dt_accel_connect_manual(&table->accel_closures, "views/thumbtable/color blue",
                            g_cclosure_new(G_CALLBACK(_accel_color), GINT_TO_POINTER(3), NULL));
    dt_accel_connect_manual(&table->accel_closures, "views/thumbtable/color purple",
                            g_cclosure_new(G_CALLBACK(_accel_color), GINT_TO_POINTER(4), NULL));
    dt_accel_connect_manual(&table->accel_closures, "views/thumbtable/clear color labels",
                            g_cclosure_new(G_CALLBACK(_accel_color), GINT_TO_POINTER(5), NULL));

    // Selection accels
    dt_accel_connect_manual(&table->accel_closures, "views/thumbtable/select all",
                            g_cclosure_new(G_CALLBACK(_accel_select_all), NULL, NULL));
    dt_accel_connect_manual(&table->accel_closures, "views/thumbtable/select none",
                            g_cclosure_new(G_CALLBACK(_accel_select_none), NULL, NULL));
    dt_accel_connect_manual(&table->accel_closures, "views/thumbtable/invert selection",
                            g_cclosure_new(G_CALLBACK(_accel_select_invert), NULL, NULL));
    dt_accel_connect_manual(&table->accel_closures, "views/thumbtable/select film roll",
                            g_cclosure_new(G_CALLBACK(_accel_select_film), NULL, NULL));
    dt_accel_connect_manual(&table->accel_closures, "views/thumbtable/select untouched",
                            g_cclosure_new(G_CALLBACK(_accel_select_untouched), NULL, NULL));
  }
}

static gboolean _filemanager_ensure_rowid_visibility(dt_thumbtable_t *table, int rowid)
{
  if(rowid < 1) rowid = 1;
  if(!table->list || g_list_length(table->list) == 0) return FALSE;
  // get first and last fully visible thumbnails
  dt_thumbnail_t *first = (dt_thumbnail_t *)g_list_first(table->list)->data;
  const int pos = MIN(g_list_length(table->list) - 1, table->thumbs_per_row * (table->rows - 1) - 1);
  dt_thumbnail_t *last = (dt_thumbnail_t *)g_list_nth_data(table->list, pos);

  if(first->rowid > rowid)
  {
    const int rows = MAX(1,(first->rowid-rowid)/table->thumbs_per_row);
    if(_move(table, 0, rows*table->thumb_size, TRUE))
      return _filemanager_ensure_rowid_visibility(table, rowid);
    else
      return FALSE;
  }
  else if(last->rowid < rowid)
  {
    const int rows = MAX(1,(rowid-last->rowid)/table->thumbs_per_row);
    if(_move(table, 0, -rows*table->thumb_size, TRUE))
      return _filemanager_ensure_rowid_visibility(table, rowid);
    else
      return FALSE;
  }
  return TRUE;
}
static gboolean _zoomable_ensure_rowid_visibility(dt_thumbtable_t *table, const int rowid)
{
  if(rowid < 1) return FALSE;
  if(!table->list || g_list_length(table->list) == 0) return FALSE;

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
    return _move(table, x_move, y_move, TRUE);
  }
  // case where the thumb is not in the list
  else
  {
    if(rowid < minrowid)
    {
      if(_move(table, 0, table->thumb_size, TRUE))
        return _zoomable_ensure_rowid_visibility(table, rowid);
      else
        return FALSE;
    }
    else if(rowid > maxrowid)
    {
      if(_move(table, 0, -table->thumb_size, TRUE))
        return _zoomable_ensure_rowid_visibility(table, rowid);
      else
        return FALSE;
    }
  }
  return FALSE;
}
gboolean dt_thumbtable_ensure_imgid_visibility(dt_thumbtable_t *table, const int imgid)
{
  if(imgid < 1) return FALSE;
  if(table->mode == DT_THUMBTABLE_MODE_FILEMANAGER)
    return _filemanager_ensure_rowid_visibility(table, _thumb_get_rowid(imgid));
  else if(table->mode == DT_THUMBTABLE_MODE_ZOOM)
    return _zoomable_ensure_rowid_visibility(table, _thumb_get_rowid(imgid));

  return FALSE;
}

static gboolean _filemanager_check_rowid_visibility(dt_thumbtable_t *table, const int rowid)
{
  if(rowid < 1) return FALSE;
  if(!table->list || g_list_length(table->list) == 0) return FALSE;
  // get first and last fully visible thumbnails
  dt_thumbnail_t *first = (dt_thumbnail_t *)g_list_first(table->list)->data;
  const int pos = MIN(g_list_length(table->list) - 1, table->thumbs_per_row * (table->rows - 1) - 1);
  dt_thumbnail_t *last = (dt_thumbnail_t *)g_list_nth_data(table->list, pos);

  if(first->rowid <= rowid && last->rowid >= rowid) return TRUE;
  return FALSE;
}
static gboolean _zoomable_check_rowid_visibility(dt_thumbtable_t *table, const int rowid)
{
  if(rowid < 1) return FALSE;
  if(!table->list || g_list_length(table->list) == 0) return FALSE;

  // is the needed rowid inside the list
  // in this case, is it fully visible ?
  GList *l = g_list_first(table->list);
  int i = 0;
  int y_move = 0;
  int x_move = 0;
  while(l)
  {
    dt_thumbnail_t *th = (dt_thumbnail_t *)l->data;
    if(th->rowid == rowid)
    {
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
  return FALSE;
}
gboolean dt_thumbtable_check_imgid_visibility(dt_thumbtable_t *table, const int imgid)
{
  if(imgid < 1) return FALSE;
  if(table->mode == DT_THUMBTABLE_MODE_FILEMANAGER)
    return _filemanager_check_rowid_visibility(table, _thumb_get_rowid(imgid));
  else if(table->mode == DT_THUMBTABLE_MODE_ZOOM)
    return _zoomable_check_rowid_visibility(table, _thumb_get_rowid(imgid));

  return FALSE;
}

static gboolean _filemanager_key_move(dt_thumbtable_t *table, dt_thumbtable_move_t move, const gboolean select)
{
  // base point
  int baseid = dt_control_get_mouse_over_id();
  // let's be sure that the current image is selected
  if(baseid > 0 && select) dt_selection_select(darktable.selection, baseid);

  int baserowid = 1;
  if(baseid <= 0)
  {
    baserowid = table->offset;
    baseid = table->offset_imgid;
  }
  else
  {
    baserowid = _thumb_get_rowid(baseid);
  }

  int newrowid = baserowid;
  // last rowid of the current collection
  int maxrowid = 1;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT MAX(rowid) FROM memory.collected_images", -1,
                              &stmt, NULL);
  if(sqlite3_step(stmt) == SQLITE_ROW) maxrowid = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);

  // classic keys
  if(move == DT_THUMBTABLE_MOVE_LEFT && baserowid > 1)
    newrowid = baserowid - 1;
  else if(move == DT_THUMBTABLE_MOVE_RIGHT && baserowid < maxrowid)
    newrowid = baserowid + 1;
  else if(move == DT_THUMBTABLE_MOVE_UP && baserowid - table->thumbs_per_row >= 1)
    newrowid = baserowid - table->thumbs_per_row;
  else if(move == DT_THUMBTABLE_MOVE_DOWN && baserowid + table->thumbs_per_row <= maxrowid)
    newrowid = baserowid + table->thumbs_per_row;
  // page key
  else if(move == DT_THUMBTABLE_MOVE_PAGEUP)
  {
    newrowid = baserowid - table->thumbs_per_row * (table->rows - 1);
    while(newrowid < 2 - table->thumbs_per_row) newrowid += table->thumbs_per_row;
  }
  else if(move == DT_THUMBTABLE_MOVE_PAGEDOWN)
  {
    newrowid = baserowid + table->thumbs_per_row * (table->rows - 1);
    while(newrowid > maxrowid) newrowid -= table->thumbs_per_row;
  }
  // direct start/end
  else if(move == DT_THUMBTABLE_MOVE_START)
    newrowid = 1;
  else if(move == DT_THUMBTABLE_MOVE_END)
    newrowid = maxrowid;

  if(newrowid == baserowid) return FALSE;

  // change image_over
  const int imgid = _thumb_get_imgid(newrowid);

  dt_control_set_mouse_over_id(imgid);

  // ensure the image is visible by moving the view if needed
  _filemanager_ensure_rowid_visibility(table, newrowid);

  // if needed, we set the selection
  if(select && imgid > 0) dt_selection_select_range(darktable.selection, imgid);
  return TRUE;
}
static gboolean _zoomable_key_move(dt_thumbtable_t *table, dt_thumbtable_move_t move, const gboolean select)
{
  // let's be sure that the current image is selected
  int baseid = dt_control_get_mouse_over_id();
  if(baseid > 0 && select) dt_selection_select(darktable.selection, baseid);

  // first, we move the view by 1 thumb_size
  // move step
  const int step = table->thumb_size;
  gboolean moved = FALSE;

  // classic keys
  if(move == DT_THUMBTABLE_MOVE_LEFT)
    moved = _move(table, step, 0, TRUE);
  else if(move == DT_THUMBTABLE_MOVE_RIGHT)
    moved = _move(table, -step, 0, TRUE);
  else if(move == DT_THUMBTABLE_MOVE_UP)
    moved = _move(table, 0, step, TRUE);
  else if(move == DT_THUMBTABLE_MOVE_DOWN)
    moved = _move(table, 0, -step, TRUE);
  // page key
  else if(move == DT_THUMBTABLE_MOVE_PAGEUP)
    moved = _move(table, 0, step * (table->rows - 1), TRUE);
  else if(move == DT_THUMBTABLE_MOVE_PAGEDOWN)
    moved = _move(table, 0, -step * (table->rows - 1), TRUE);
  // direct start/end
  else if(move == DT_THUMBTABLE_MOVE_START)
    moved = _zoomable_ensure_rowid_visibility(table, 1);
  else if(move == DT_THUMBTABLE_MOVE_END)
  {
    int maxrowid = 1;
    sqlite3_stmt *stmt;
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT MAX(rowid) FROM memory.collected_images",
                                -1, &stmt, NULL);
    if(sqlite3_step(stmt) == SQLITE_ROW) maxrowid = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    moved = _zoomable_ensure_rowid_visibility(table, maxrowid);
  }
  else if(move == DT_THUMBTABLE_MOVE_ALIGN)
  {
    const int newx
        = (table->thumbs_area.x / table->thumb_size) * table->thumb_size; // this is NOT a noop due to rounding...
    const int newy
        = (table->thumbs_area.y / table->thumb_size) * table->thumb_size; // this is NOT a noop due to rounding...
    moved = _move(table, newx - table->thumbs_area.x, newy - table->thumbs_area.y, TRUE);
  }

  // and we set mouseover if we can
  dt_thumbnail_t *thumb = _thumb_get_under_mouse(table);
  if(thumb) dt_control_set_mouse_over_id(thumb->imgid);
  // if needed, we set the selection
  if(thumb && select) dt_selection_select_range(darktable.selection, thumb->imgid);

  // and we record new positions values
  dt_thumbnail_t *first = (dt_thumbnail_t *)g_list_first(table->list)->data;
  table->offset = first->rowid;
  table->offset_imgid = first->imgid;
  dt_conf_set_int("plugins/lighttable/recentcollect/pos0", table->offset);
  dt_conf_set_int("lighttable/zoomable/last_offset", table->offset);
  dt_conf_set_int("lighttable/zoomable/last_pos_x", table->thumbs_area.x);
  dt_conf_set_int("lighttable/zoomable/last_pos_y", table->thumbs_area.y);

  return moved;
}

gboolean dt_thumbtable_key_move(dt_thumbtable_t *table, dt_thumbtable_move_t move, const gboolean select)
{
  if(table->mode == DT_THUMBTABLE_MODE_FILEMANAGER)
    return _filemanager_key_move(table, move, select);
  else if(table->mode == DT_THUMBTABLE_MODE_ZOOM)
    return _zoomable_key_move(table, move, select);

  return FALSE;
}

gboolean dt_thumbtable_reset_first_offset(dt_thumbtable_t *table)
{
  if(table->mode != DT_THUMBTABLE_MODE_FILEMANAGER && table->mode != DT_THUMBTABLE_MODE_ZOOM) return FALSE;

  dt_thumbnail_t *first = (dt_thumbnail_t *)g_list_first(table->list)->data;
  const int offset = table->thumbs_per_row - ((first->rowid - 1) % table->thumbs_per_row);
  if(offset == 0) return FALSE;

  // we scroll up the list by the number offset
  dt_thumbtable_set_offset(table, table->offset + offset, TRUE);
  return TRUE;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
