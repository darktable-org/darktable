/*
    This file is part of darktable,
    copyright (c) 2020 Aldric Renaudin.

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
/** a class to manage a collection of zoomable thumbnails for culling or full preview.  */
#include "dtgtk/culling.h"
#include "common/collection.h"
#include "common/colorlabels.h"
#include "common/debug.h"
#include "common/history.h"
#include "common/ratings.h"
#include "common/selection.h"
#include "control/control.h"
#include "gui/accelerators.h"
#include "gui/drag_and_drop.h"
#include "gui/gtk.h"
#include "views/view.h"

#define FULL_PREVIEW_IN_MEMORY_LIMIT 9

static inline float _absmul(float a, float b)
{
  return a > b ? a / b : b / a;
}
static inline int _get_max_in_memory_images()
{
  const int max_in_memory_images = dt_conf_get_int("plugins/lighttable/preview/max_in_memory_images");
  return MIN(max_in_memory_images, FULL_PREVIEW_IN_MEMORY_LIMIT);
}
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

static int _get_selection_count()
{
  int nb = 0;
  gchar *query = dt_util_dstrcat(
      NULL,
      "SELECT count(*) FROM main.selected_images AS s, memory.collected_images as m WHERE s.imgid = m.imgid");
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  if(stmt != NULL)
  {
    if(sqlite3_step(stmt) == SQLITE_ROW)
    {
      nb = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
  }
  g_free(query);

  return nb;
}
// get the thumb at specific position
/*static dt_thumbnail_t *_thumb_get_at_pos(dt_culling_t *table, int x, int y)
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
static dt_thumbnail_t *_thumb_get_under_mouse(dt_culling_t *table)
{
  if(!table->mouse_inside) return NULL;

  int x = -1;
  int y = -1;
  gdk_window_get_origin(gtk_widget_get_window(table->widget), &x, &y);
  x = table->last_x - x;
  y = table->last_y - y;

  return _thumb_get_at_pos(table, x, y);
}*/

// get imgid from rowid
/*static int _thumb_get_imgid(int rowid)
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
}*/

// compute thumb_size, thumbs_per_row and rows for the current widget size
// return TRUE if something as changed (or forced) FALSE otherwise
static gboolean _compute_sizes(dt_culling_t *table, gboolean force)
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

  if(table->mode == DT_CULLING_MODE_CULLING)
  {
    const int npr = dt_view_lighttable_get_zoom(darktable.view_manager);

    if(force || allocation.width != table->view_width || allocation.height != table->view_height
       || npr != table->thumbs_count)
    {
      table->thumbs_count = npr;
      table->view_width = allocation.width;
      table->view_height = allocation.height;
      ret = TRUE;
    }
  }
  else if(table->mode == DT_CULLING_MODE_PREVIEW)
  {
    if(force || allocation.width != table->view_width || allocation.height != table->view_height)
    {
      table->thumbs_count = 1;
      table->view_width = allocation.width;
      table->view_height = allocation.height;
      ret = TRUE;
    }
  }
  return ret;
}

// remove all uneeded thumbnails from the list and the widget
// uneeded == completly hidden
/*static int _thumbs_remove_unneeded(dt_culling_t *table)
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
static int _thumbs_load_needed(dt_culling_t *table)
{
  if(g_list_length(table->list) == 0) return 0;
  sqlite3_stmt *stmt;
  int changed = 0;

  // we load image at the beginning
  dt_thumbnail_t *first = (dt_thumbnail_t *)g_list_first(table->list)->data;
  if(first->rowid > 1
     && (((table->mode == DT_THUMBTABLE_MODE_FILEMANAGER || table->mode == DT_THUMBTABLE_MODE_ZOOM) && first->y >
0)
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
        dt_thumbnail_t *thumb = dt_thumbnail_new(table->thumb_size, table->thumb_size, sqlite3_column_int(stmt, 1),
                                                 sqlite3_column_int(stmt, 0));
        if(table->mode == DT_THUMBTABLE_MODE_FILMSTRIP)
        {
          thumb->single_click = TRUE;
          thumb->sel_mode = DT_THUMBNAIL_SEL_MODE_MOD_ONLY;
        }
        thumb->x = posx;
        thumb->y = posy;
        table->list = g_list_prepend(table->list, thumb);
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
        dt_thumbnail_t *thumb = dt_thumbnail_new(table->thumb_size, table->thumb_size, sqlite3_column_int(stmt, 1),
                                                 sqlite3_column_int(stmt, 0));
        if(table->mode == DT_THUMBTABLE_MODE_FILMSTRIP)
        {
          thumb->single_click = TRUE;
          thumb->sel_mode = DT_THUMBNAIL_SEL_MODE_MOD_ONLY;
        }
        thumb->x = posx;
        thumb->y = posy;
        table->list = g_list_append(table->list, thumb);
        gtk_layout_put(GTK_LAYOUT(table->widget), thumb->w_main, posx, posy);
        changed++;
      }
      _pos_get_next(table, &posx, &posy);
    }
    g_free(query);
    sqlite3_finalize(stmt);
  }

  return changed;
}*/

// move all thumbs from the table.
// if clamp, we verify that the move is allowed (collection bounds, etc...)
/*static gboolean _move(dt_culling_t *table, const int x, const int y, gboolean clamp)
{
  if(!table->list || g_list_length(table->list) == 0) return FALSE;
  int posx = x;
  int posy = y;
  if(clamp)
  {
    // we check bounds to allow or not the move
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

  // we load all needed thumbs
  int changed = _thumbs_load_needed(table);

  // we remove the images not visible on screen
  changed += _thumbs_remove_unneeded(table);

  return TRUE;
}*/

/*static dt_thumbnail_t *_thumbtable_get_thumb(dt_culling_t *table, int imgid)
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
}*/

static gboolean _event_scroll(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
  /*GdkEventScroll *e = (GdkEventScroll *)event;
  dt_culling_t *table = (dt_culling_t *)user_data;
  gdouble delta;

  if(dt_gui_get_scroll_delta(e, &delta))
  {
  }*/
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
  dt_culling_t *table = (dt_culling_t *)user_data;
  dt_culling_full_redraw(table, FALSE);
  return FALSE; // let's propagate this event
}

static gboolean _event_leave_notify(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  dt_culling_t *table = (dt_culling_t *)user_data;
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

  return TRUE;
}

static gboolean _event_motion_notify(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{

  return TRUE;
}

static gboolean _event_button_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{

  return TRUE;
}

// called each time the preference change, to update specific parts
static void _dt_pref_change_callback(gpointer instance, gpointer user_data)
{
  if(!user_data) return;
  // dt_culling_t *table = (dt_culling_t *)user_data;

  // dt_thumbtable_full_redraw(table, TRUE);
}

static void _dt_profile_change_callback(gpointer instance, int type, gpointer user_data)
{
  if(!user_data) return;
  dt_culling_t *table = (dt_culling_t *)user_data;

  GList *l = table->list;
  while(l)
  {
    dt_thumbnail_t *th = (dt_thumbnail_t *)l->data;
    dt_thumbnail_image_refresh(th);
    l = g_list_next(l);
  }
}

// this is called each time mouse_over id change
static void _dt_mouse_over_image_callback(gpointer instance, gpointer user_data)
{
  if(!user_data) return;
  // dt_culling_t *table = (dt_culling_t *)user_data;

  const int imgid = dt_control_get_mouse_over_id();

  if(imgid > 0)
  {
    // let's be absolutely sure that the right widget has the focus
    // otherwise accels don't work...
    gtk_widget_grab_focus(dt_ui_center(darktable.gui->ui));
  }
}

// this is called each time collected images change
static void _dt_collection_changed_callback(gpointer instance, dt_collection_change_t query_change, gpointer imgs,
                                            const int next, gpointer user_data)
{
  /*if(!user_data) return;
  dt_culling_t *table = (dt_culling_t *)user_data;
  if(query_change == DT_COLLECTION_CHANGE_RELOAD)
  {*/
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
  /*int newid = table->offset_imgid;
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
  table->offset_imgid = newid;
  table->offset = _thumb_get_rowid(newid);
  dt_conf_set_int("plugins/lighttable/recentcollect/pos0", table->offset);

  dt_thumbtable_full_redraw(table, TRUE);

  dt_view_lighttable_change_offset(darktable.view_manager, FALSE, newid);

  // and for images that have changed but are still in the view, we update datas
  l = imgs;
  while(l)
  {
    dt_thumbnail_t *th = _thumbtable_get_thumb(table, GPOINTER_TO_INT(l->data));
    if(th)
    {
      dt_thumbnail_update_infos(th);
    }
    l = g_list_next(l);
  }
}
else
{
  // otherwise we reset the offset to the beginning
  table->offset = 1;
  table->offset_imgid = _thumb_get_imgid(table->offset);
  dt_conf_set_int("plugins/lighttable/recentcollect/pos0", 1);
  // and we reset position of first thumb for zooming
  if(g_list_length(table->list) > 0)
  {
    dt_thumbnail_t *thumb = (dt_thumbnail_t *)g_list_nth_data(table->list, 0);
    thumb->x = 0;
    thumb->y = 0;
  }
  dt_thumbtable_full_redraw(table, TRUE);
  dt_view_lighttable_change_offset(darktable.view_manager, TRUE, table->offset_imgid);
}*/
}

static void _event_dnd_get(GtkWidget *widget, GdkDragContext *context, GtkSelectionData *selection_data,
                           const guint target_type, const guint time, gpointer user_data)
{
  /*dt_culling_t *table = (dt_culling_t *)user_data;
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
  }*/
}

static void _event_dnd_begin(GtkWidget *widget, GdkDragContext *context, gpointer user_data)
{
  /*const int ts = DT_PIXEL_APPLY_DPI(64);

  dt_culling_t *table = (dt_culling_t *)user_data;

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
  if(darktable.collection->params.sort == DT_COLLECTION_SORT_CUSTOM_ORDER && table->mode !=
  DT_THUMBTABLE_MODE_ZOOM)
  {
    // we set the class correctly
    GtkStyleContext *tablecontext = gtk_widget_get_style_context(table->widget);
    gtk_style_context_add_class(tablecontext, "dt_thumbtable_reorder");
  }*/
}

static void _event_dnd_received(GtkWidget *widget, GdkDragContext *context, gint x, gint y,
                                GtkSelectionData *selection_data, guint target_type, guint time,
                                gpointer user_data)
{
  /*gboolean success = FALSE;

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
    dt_culling_t *table = (dt_culling_t *)user_data;
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
  gtk_drag_finish(context, success, FALSE, time);*/
}

static void _event_dnd_end(GtkWidget *widget, GdkDragContext *context, gpointer user_data)
{
  /*dt_culling_t *table = (dt_culling_t *)user_data;
  if(table->drag_list)
  {
    g_list_free(table->drag_list);
    table->drag_list = NULL;
  }
  // in any case, with reset the reordering class if any
  GtkStyleContext *tablecontext = gtk_widget_get_style_context(table->widget);
  gtk_style_context_remove_class(tablecontext, "dt_thumbtable_reorder");*/
}

dt_culling_t *dt_culling_new(dt_culling_mode_t mode)
{
  dt_culling_t *table = (dt_culling_t *)calloc(1, sizeof(dt_culling_t));
  table->mode = mode;
  table->widget = gtk_layout_new(NULL, NULL);
  // TODO dt_gui_add_help_link(table->widget, dt_get_help_url("lighttable_filemanager"));

  // set css name and class
  gtk_widget_set_name(table->widget, "culling");
  GtkStyleContext *context = gtk_widget_get_style_context(table->widget);
  gtk_style_context_add_class(context, "dt_culling");
  if(dt_conf_get_bool("lighttable/ui/expose_statuses")) gtk_style_context_add_class(context, "dt_show_overlays");

  // set widget signals
  gtk_widget_set_events(table->widget, GDK_EXPOSURE_MASK | GDK_POINTER_MOTION_MASK | GDK_POINTER_MOTION_HINT_MASK
                                           | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_STRUCTURE_MASK
                                           | GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK);
  gtk_widget_set_app_paintable(table->widget, TRUE);
  gtk_widget_set_can_focus(table->widget, TRUE);

  // drag and drop : used for interactions with maps, exporting uri to external apps, importing images
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
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_COLLECTION_CHANGED,
                            G_CALLBACK(_dt_collection_changed_callback), table);
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_MOUSE_OVER_IMAGE_CHANGE,
                            G_CALLBACK(_dt_mouse_over_image_callback), table);
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_CONTROL_PROFILE_USER_CHANGED,
                            G_CALLBACK(_dt_profile_change_callback), table);
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_PREFERENCES_CHANGE, G_CALLBACK(_dt_pref_change_callback),
                            table);
  gtk_widget_show(table->widget);

  g_object_ref(table->widget);

  return table;
}

// initialize offset, ... values
// to be used when reentering culling
void dt_culling_init(dt_culling_t *table)
{
  /** HOW it works :
   *
   * For the first image :
   *  image_over OR first selected OR first OR -1
   *
   * For the navigation in selection :
   *  culling dynamic mode                       => OFF
   *  first image in selection AND selection > 1 => ON
   *  otherwise                                  => OFF
   *
   * For the selection following :
   *  culling dynamic mode         => OFF
   *  first image(s) == selection  => ON
   */

  // init values
  table->navigate_inside_selection = FALSE;
  table->selection_sync = FALSE;

  // get first id
  sqlite3_stmt *stmt;
  int first_id = -1;

  /* TODO : get offset from thumbtable ?
  if(!lib->already_started)
  {
    // first start, we retrieve the registered offset
    const int offset = dt_conf_get_int("plugins/lighttable/recentcollect/pos0");
    gchar *query = dt_util_dstrcat(NULL, "SELECT imgid FROM memory.collected_images WHERE rowid=%d", offset);
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
    if(sqlite3_step(stmt) == SQLITE_ROW)
    {
      first_id = sqlite3_column_int(stmt, 0);
    }
    g_free(query);
    sqlite3_finalize(stmt);
    lib->already_started = TRUE;
  }
  else*/
  first_id = dt_control_get_mouse_over_id();

  if(first_id < 1)
  {
    // search the first selected image
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "SELECT col.imgid "
                                "FROM memory.collected_images AS col, main.selected_images as sel "
                                "WHERE col.imgid=sel.imgid "
                                "ORDER BY col.rowid "
                                "LIMIT 1",
                                -1, &stmt, NULL);
    if(sqlite3_step(stmt) == SQLITE_ROW) first_id = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
  }
  if(first_id < 1)
  {
    // search the first image shown in view (this is the offset of thumbtable)
    // first_id = dt_ui_thumbtable(darktable.gui->ui)->offset_imgid;
  }
  if(first_id < 1)
  {
    // Arrrghh
    return;
  }

  // special culling dynamic mode
  if(table->mode == DT_CULLING_MODE_CULLING
     && dt_view_lighttable_get_culling_zoom_mode(darktable.view_manager) == DT_LIGHTTABLE_ZOOM_DYNAMIC)
  {
    table->selection_sync = TRUE;
    table->offset = first_id;
    return;
  }

  // selection count
  int sel_count = 0;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT count(*) "
                              "FROM memory.collected_images AS col, main.selected_images as sel "
                              "WHERE col.imgid=sel.imgid",
                              -1, &stmt, NULL);
  if(sqlite3_step(stmt) == SQLITE_ROW) sel_count = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  // is first_id inside selection ?
  gboolean inside = FALSE;
  gchar *query = dt_util_dstrcat(NULL,
                                 "SELECT col.imgid "
                                 "FROM memory.collected_images AS col, main.selected_images AS sel "
                                 "WHERE col.imgid=sel.imgid AND col.imgid=%d",
                                 first_id);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  if(sqlite3_step(stmt) == SQLITE_ROW) inside = TRUE;
  sqlite3_finalize(stmt);
  g_free(query);

  if(table->mode == DT_CULLING_MODE_PREVIEW)
  {
    table->navigate_inside_selection = (sel_count > 1 && inside);
    table->selection_sync = (sel_count == 1 && inside);
  }
  else if(table->mode == DT_CULLING_MODE_CULLING)
  {
    const int zoom = dt_view_lighttable_get_zoom(darktable.view_manager);
    table->navigate_inside_selection = (sel_count > zoom && inside);
    if(sel_count <= zoom && inside)
    {
      table->selection_sync = TRUE;
      // ensure that first_id is the first selected
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                  "SELECT col.imgid "
                                  "FROM memory.collected_images AS col, main.selected_images as sel "
                                  "WHERE col.imgid=sel.imgid "
                                  "ORDER BY col.rowid "
                                  "LIMIT 1",
                                  -1, &stmt, NULL);
      if(sqlite3_step(stmt) == SQLITE_ROW) first_id = sqlite3_column_int(stmt, 0);
      sqlite3_finalize(stmt);
    }
  }

  table->offset = first_id;
}

static gboolean _thumbs_recreate_list_at(dt_culling_t *table, const int display_first_image)
{
  gchar *query = NULL;
  sqlite3_stmt *stmt;
  gchar *rowid_txt = NULL;
  if(display_first_image >= 0)
  {
    rowid_txt = dt_util_dstrcat(NULL, "(SELECT rowid FROM memory.collected_images WHERE imgid = %d)",
                                display_first_image);
  }
  else
    rowid_txt = dt_util_dstrcat(NULL, "%d", 0);

  if(table->navigate_inside_selection)
  {
    query = dt_util_dstrcat(NULL,
                            "SELECT m.rowid, m.imgid, b.aspect_ratio "
                            "FROM memory.collected_images AS m, main.selected_images AS s, images AS b "
                            "WHERE m.imgid = b.id AND m.imgid = s.imgid AND m.rowid >= %s "
                            "ORDER BY m.rowid "
                            "LIMIT %d",
                            rowid_txt, table->thumbs_count);
  }
  else
  {
    query = dt_util_dstrcat(NULL,
                            "SELECT m.rowid, m.imgid, b.aspect_ratio "
                            "FROM (SELECT rowid, imgid "
                            "FROM memory.collected_images "
                            "WHERE rowid < %s + %d "
                            "ORDER BY rowid DESC "
                            "LIMIT %d) AS m, "
                            "images AS b "
                            "WHERE m.imgid = b.id "
                            "ORDER BY m.rowid",
                            rowid_txt, table->thumbs_count, table->thumbs_count);
  }

  GList *newlist = NULL;
  int nbnew = 0;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW && g_list_length(newlist) <= table->thumbs_count)
  {
    const int nrow = sqlite3_column_int(stmt, 0);
    const int nid = sqlite3_column_int(stmt, 1);
    // first, we search if the thumb is already here
    GList *tl = g_list_find_custom(table->list, GINT_TO_POINTER(nid), _list_compare_by_imgid);
    if(tl)
    {
      dt_thumbnail_t *thumb = (dt_thumbnail_t *)tl->data;
      thumb->rowid = nrow; // this may have changed
      newlist = g_list_append(newlist, thumb);
      // and we remove the thumb from the old list
      table->list = g_list_remove(table->list, thumb);
    }
    else
    {
      // we create a completly new thumb
      dt_thumbnail_t *thumb = dt_thumbnail_new(10, 10, nid, nrow);
      double aspect_ratio = sqlite3_column_double(stmt, 2);
      if(!aspect_ratio || aspect_ratio < 0.0001)
      {
        aspect_ratio = dt_image_set_aspect_ratio(nid, FALSE);
        // if an error occurs, let's use 1:1 value
        if(aspect_ratio < 0.0001) aspect_ratio = 1.0;
      }
      thumb->aspect_ratio = aspect_ratio;
      newlist = g_list_append(newlist, thumb);
      nbnew++;
    }
    // if it's the offset, we record the imgid
    if(nrow == table->offset) table->offset_imgid = nid;
  }

  // in rare cases, we can have less images than wanted
  // although there's images before
  if(table->navigate_inside_selection && g_list_length(newlist) < table->thumbs_count
     && g_list_length(newlist) < _get_selection_count())
  {
    const int nb = table->thumbs_count - g_list_length(newlist);
    query = dt_util_dstrcat(NULL,
                            "SELECT m.rowid, m.imgid, b.aspect_ratio "
                            "FROM memory.collected_images AS m, main.selected_images AS s, images AS b "
                            "WHERE m.imgid = b.id AND m.imgid = s.imgid AND m.rowid < %s "
                            "ORDER BY m.rowid DESC "
                            "LIMIT %d",
                            rowid_txt, nb);
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
    if(stmt != NULL)
    {
      while(sqlite3_step(stmt) == SQLITE_ROW && g_list_length(newlist) <= table->thumbs_count)
      {
        const int nrow = sqlite3_column_int(stmt, 0);
        const int nid = sqlite3_column_int(stmt, 1);
        // first, we search if the thumb is already here
        GList *tl = g_list_find_custom(table->list, GINT_TO_POINTER(nid), _list_compare_by_imgid);
        if(tl)
        {
          dt_thumbnail_t *thumb = (dt_thumbnail_t *)tl->data;
          thumb->rowid = nrow; // this may have changed
          newlist = g_list_prepend(newlist, thumb);
          // and we remove the thumb from the old list
          table->list = g_list_remove(table->list, thumb);
        }
        else
        {
          // we create a completly new thumb
          dt_thumbnail_t *thumb = dt_thumbnail_new(10, 10, nid, nrow);
          double aspect_ratio = sqlite3_column_double(stmt, 2);
          if(!aspect_ratio || aspect_ratio < 0.0001)
          {
            aspect_ratio = dt_image_set_aspect_ratio(nid, FALSE);
            // if an error occurs, let's use 1:1 value
            if(aspect_ratio < 0.0001) aspect_ratio = 1.0;
          }
          thumb->aspect_ratio = aspect_ratio;
          newlist = g_list_prepend(newlist, thumb);
          nbnew++;
        }
        // if it's the offset, we record the imgid
        if(nrow == table->offset) table->offset_imgid = nid;
      }
      sqlite3_finalize(stmt);
    }
    g_free(query);
  }

  g_free(rowid_txt);

  // now we cleanup all remaining thumbs from old table->list and set it again
  g_list_free_full(table->list, _list_remove_thumb);
  table->list = newlist;

  return TRUE;
}

static gboolean _thumbs_compute_positions(dt_culling_t *table)
{
  if(!table->list || g_list_length(table->list) == 0) return FALSE;

  int sum_w = 0, max_h = 0, max_w = 0;

  GList *slots = NULL;

  unsigned int total_width = 0, total_height = 0;
  int distance = 1;
  float avg_ratio = 0;

  // reinit size and positions and get max values
  GList *l = table->list;
  while(l)
  {
    dt_thumbnail_t *thumb = (dt_thumbnail_t *)l->data;
    const double aspect_ratio = thumb->aspect_ratio;
    thumb->width = (gint)(sqrt(aspect_ratio) * 100);
    thumb->height = (gint)(1 / sqrt(aspect_ratio) * 100);
    thumb->x = thumb->y = 0;

    sum_w += thumb->width;
    max_w = MAX(max_w, thumb->width);
    max_h = MAX(max_h, thumb->height);
    avg_ratio += thumb->width / (float)thumb->height;
    l = g_list_next(l);
  }

  avg_ratio /= g_list_length(table->list);

  int per_row, tmp_per_row, per_col, tmp_per_col;
  per_row = tmp_per_row = ceil(sqrt(g_list_length(table->list)));
  per_col = tmp_per_col = (g_list_length(table->list) + per_row - 1) / per_row; // ceil(sel_img_count/per_row)

  float tmp_slot_ratio, slot_ratio;
  tmp_slot_ratio = slot_ratio = (table->view_width / (float)per_row) / (table->view_height / (float)per_col);

  do
  {
    per_row = tmp_per_row;
    per_col = tmp_per_col;
    slot_ratio = tmp_slot_ratio;

    if(avg_ratio > slot_ratio)
    {
      tmp_per_row = per_row - 1;
    }
    else
    {
      tmp_per_row = per_row + 1;
    }

    if(tmp_per_row == 0) break;

    tmp_per_col
        = (g_list_length(table->list) + tmp_per_row - 1) / tmp_per_row; // ceil(sel_img_count / tmp_per_row);

    tmp_slot_ratio = (table->view_width / (float)tmp_per_row) / (table->view_height / (float)tmp_per_col);

  } while(per_row > 0 && per_row <= g_list_length(table->list)
          && _absmul(tmp_slot_ratio, avg_ratio) < _absmul(slot_ratio, avg_ratio));


  // Vertical layout
  l = table->list;
  while(l)
  {
    dt_thumbnail_t *thumb = (dt_thumbnail_t *)l->data;
    GList *slot_iter = g_list_first(slots);
    for(; slot_iter; slot_iter = slot_iter->next)
    {
      GList *slot = (GList *)slot_iter->data;
      // Calculate current total height of slot
      int slot_h = distance;
      GList *slot_cw_iter = slot;
      while(slot_cw_iter != NULL)
      {
        dt_thumbnail_t *slot_cw = (dt_thumbnail_t *)slot_cw_iter->data;
        slot_h = slot_h + slot_cw->height + distance;
        slot_cw_iter = slot_cw_iter->next;
      }
      // Add window to slot if the slot height after adding the window
      // doesn't exceed max window height
      if(slot_h + distance + thumb->height < max_h)
      {
        slot_iter->data = g_list_append(slot, thumb);
        break;
      }
    }
    // Otherwise, create a new slot with only this window
    if(!slot_iter) slots = g_list_append(slots, g_list_append(NULL, thumb));
    l = g_list_next(l);
  }

  GList *rows = g_list_append(NULL, NULL);
  {
    int row_y = 0, x = 0, row_h = 0;
    int max_row_w = sum_w / per_col; // sqrt((float) sum_w * max_h);// * pow((float) width/height, 0.02);
    for(GList *slot_iter = slots; slot_iter != NULL; slot_iter = slot_iter->next)
    {
      GList *slot = (GList *)slot_iter->data;

      // Max width of windows in the slot
      int slot_max_w = 0;
      for(GList *slot_cw_iter = slot; slot_cw_iter != NULL; slot_cw_iter = slot_cw_iter->next)
      {
        dt_thumbnail_t *cw = (dt_thumbnail_t *)slot_cw_iter->data;
        slot_max_w = MAX(slot_max_w, cw->width);
      }

      int y = row_y;
      for(GList *slot_cw_iter = slot; slot_cw_iter != NULL; slot_cw_iter = slot_cw_iter->next)
      {
        dt_thumbnail_t *cw = (dt_thumbnail_t *)slot_cw_iter->data;
        cw->x = x + (slot_max_w - cw->width) / 2;
        cw->y = y;
        y += cw->height + distance;
        rows->data = g_list_append(rows->data, cw);
      }

      row_h = MAX(row_h, y - row_y);
      total_height = MAX(total_height, y);
      x += slot_max_w + distance;
      total_width = MAX(total_width, x);

      if(x > max_row_w)
      {
        x = 0;
        row_y += row_h;
        row_h = 0;
        rows = g_list_append(rows, 0);
        rows = rows->next;
      }
      g_list_free(slot);
    }
    g_list_free(slots);
    slots = NULL;
  }

  total_width -= distance;
  total_height -= distance;

  for(GList *iter = g_list_first(rows); iter != NULL; iter = iter->next)
  {
    GList *row = (GList *)iter->data;
    int row_w = 0, xoff;
    int max_rh = 0;

    for(GList *slot_cw_iter = row; slot_cw_iter != NULL; slot_cw_iter = slot_cw_iter->next)
    {
      dt_thumbnail_t *cw = (dt_thumbnail_t *)slot_cw_iter->data;
      row_w = MAX(row_w, cw->x + cw->width);
      max_rh = MAX(max_rh, cw->height);
    }

    xoff = (total_width - row_w) / 2;

    for(GList *cw_iter = row; cw_iter != NULL; cw_iter = cw_iter->next)
    {
      dt_thumbnail_t *cw = (dt_thumbnail_t *)cw_iter->data;
      cw->x += xoff;
      cw->height = max_rh;
    }
    g_list_free(row);
  }

  g_list_free(rows);

  float factor;
  factor = (float)(table->view_width - 1) / total_width;
  if(factor * total_height > table->view_height - 1) factor = (float)(table->view_height - 1) / total_height;

  int xoff = (table->view_width - (float)total_width * factor) / 2;
  int yoff = (table->view_height - (float)total_height * factor) / 2;

  l = table->list;
  while(l)
  {
    dt_thumbnail_t *thumb = (dt_thumbnail_t *)l->data;
    thumb->width = thumb->width * factor;
    thumb->height = thumb->height * factor;
    thumb->x = thumb->x * factor + xoff;
    thumb->y = thumb->y * factor + yoff;
    l = g_list_next(l);
  }

  // we want to be sure the filmstrip stay in synch
  if(g_list_length(table->list) > 0)
  {
    // if the selection should follow active images
    if(table->navigate_inside_selection)
    {
      // deactivate selection_change event
      table->select_desactivate = TRUE;
      // deselect all
      DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "DELETE FROM main.selected_images", NULL, NULL, NULL);
      // select all active images
      GList *ls = NULL;
      l = table->list;
      while(l)
      {
        dt_thumbnail_t *thumb = (dt_thumbnail_t *)l->data;
        ls = g_list_append(ls, GINT_TO_POINTER(thumb->imgid));
        l = g_list_next(l);
      }
      dt_selection_select_list(darktable.selection, ls);
      g_list_free(ls);
      // reactivate selection_change event
      table->select_desactivate = FALSE;
    }
    // move filmstrip
    // TODO dt_thumbtable_set_offset(dt_ui_thumbtable(darktable.gui->ui), table->offset, TRUE);
  }

  // we save the current first id
  dt_conf_set_int("plugins/lighttable/culling_last_id", table->offset_imgid);

  return TRUE;
}

// recreate the list of thumb if needed and recomputes sizes and positions if needed
void dt_culling_full_redraw(dt_culling_t *table, gboolean force)
{
  const double start = dt_get_wtime();
  // first, we see if we need to do something
  if(!_compute_sizes(table, force)) return;

  // we recreate the list of images
  _thumbs_recreate_list_at(table, table->offset);

  // we compute the sizes and positions of thumbs
  _thumbs_compute_positions(table);

  // and we effectively move and resize thumbs
  GList *l = table->list;
  while(l)
  {
    dt_thumbnail_t *thumb = (dt_thumbnail_t *)l->data;
    // we add or move the thumb at the right position
    if(!gtk_widget_get_parent(thumb->w_main))
    {
      gtk_layout_put(GTK_LAYOUT(table->widget), thumb->w_main, thumb->x, thumb->y);
    }
    else
    {
      gtk_layout_move(GTK_LAYOUT(table->widget), thumb->w_main, thumb->x, thumb->y);
    }
    // and we resize the thumb
    dt_thumbnail_resize(thumb, thumb->width, thumb->height);
    l = g_list_next(l);
  }

  // be sure the focus is in the right widget (needed for accels)
  gtk_widget_grab_focus(dt_ui_center(darktable.gui->ui));

  dt_print(DT_DEBUG_LIGHTTABLE, "done in %0.04f sec\n", dt_get_wtime() - start);

  if(darktable.unmuted & DT_DEBUG_CACHE) dt_mipmap_cache_print(darktable.mipmap_cache);
}

// define if overlays should always be shown or just on mouse-over
/*void dt_thumbtable_set_overlays(dt_culling_t *table, gboolean show)
{
  GtkStyleContext *context = gtk_widget_get_style_context(table->widget);
  if(show)
    gtk_style_context_add_class(context, "dt_show_overlays");
  else
    gtk_style_context_remove_class(context, "dt_show_overlays");
}

// get current offset
int dt_thumbtable_get_offset(dt_culling_t *table)
{
  return table->offset;
}
// set offset and redraw if needed
gboolean dt_thumbtable_set_offset(dt_culling_t *table, const int offset, const gboolean redraw)
{
  if(offset < 1 || offset == table->offset) return FALSE;
  table->offset = offset;
  dt_conf_set_int("plugins/lighttable/recentcollect/pos0", table->offset);
  if(redraw) dt_culling_full_redraw(table, TRUE);
  return TRUE;
}*/

// set offset at specific imgid and redraw if needed
/*gboolean dt_thumbtable_set_offset_image(dt_culling_t *table, const int imgid, const gboolean redraw)
{
  return dt_thumbtable_set_offset(table, _thumb_get_rowid(imgid), redraw);
}*/

/*gboolean dt_thumbtable_key_move(dt_culling_t *table, dt_thumbtable_move_t move, const gboolean select)
{
 if(table->mode == DT_THUMBTABLE_MODE_FILEMANAGER)
    return _filemanager_key_move(table, move, select);
  else if(table->mode == DT_THUMBTABLE_MODE_ZOOM)
    return _zoomable_key_move(table, move, select);

  return FALSE;
}*/

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
