/*
    This file is part of darktable,
    Copyright (C) 2020-2022 darktable developers.

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
#include "common/debug.h"
#include "common/selection.h"
#include "control/control.h"
#include "gui/gtk.h"
#include "views/view.h"

#define FULL_PREVIEW_IN_MEMORY_LIMIT 9
#define ZOOM_MAX 100000.0f

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
  const dt_imgid_t imgid = GPOINTER_TO_INT(b);
  if(th->imgid < 0 || imgid < 0) return 1;
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
  gchar *query = g_strdup(  //TODO: since this is a fixed string, do we need to copy?
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

// get imgid from rowid
static int _thumb_get_imgid(int rowid)
{
  int id = -1;
  sqlite3_stmt *stmt;
  gchar *query = g_strdup_printf("SELECT imgid FROM memory.collected_images WHERE rowid=%d", rowid);
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
static int _thumb_get_rowid(dt_imgid_t imgid)
{
  int id = -1;
  sqlite3_stmt *stmt;
  gchar *query = g_strdup_printf("SELECT rowid FROM memory.collected_images WHERE imgid=%d", imgid);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    id = sqlite3_column_int(stmt, 0);
  }
  g_free(query);
  sqlite3_finalize(stmt);
  return id;
}

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

  // check the offset
  if(table->list)
  {
    dt_thumbnail_t *th = (dt_thumbnail_t *)table->list->data;
    if(th->imgid != table->offset_imgid || th->display_focus != table->focus) ret = TRUE;
  }
  else if(table->offset_imgid > 0)
    ret = TRUE;

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

// set mouse_over_id to thumb under mouse or to first thumb
static void _thumbs_refocus(dt_culling_t *table)
{
  int overid = -1;

  if(table->mouse_inside)
  {
    // the exact position of the mouse
    int x = -1;
    int y = -1;
    gdk_window_get_origin(gtk_widget_get_window(table->widget), &x, &y);
    x = table->pan_x - x;
    y = table->pan_y - y;

    // which thumb is under the mouse ?
    for(GList *l = table->list; l; l = g_list_next(l))
    {
      dt_thumbnail_t *th = (dt_thumbnail_t *)l->data;
      if(th->x <= x && th->x + th->width > x && th->y <= y && th->y + th->height > y)
      {
        overid = th->imgid;
        break;
      }
    }
  }

  // if overid not valid, we use the offset image
  if(overid <= 0)
  {
    overid = table->offset_imgid;
  }

  // and we set the overid
  dt_control_set_mouse_over_id(overid);
}

static void _thumbs_move(dt_culling_t *table, int move)
{
  if(move == 0) return;
  int new_offset = table->offset;
  // we sanintize the values to be sure to stay in the allowed collection
  if(move < 0)
  {
    if(table->navigate_inside_selection)
    {
      sqlite3_stmt *stmt;
      // clang-format off
      gchar *query = g_strdup_printf("SELECT m.rowid FROM memory.collected_images as m, main.selected_images as s "
                                     "WHERE m.imgid=s.imgid AND m.rowid<=%d "
                                     "ORDER BY m.rowid DESC LIMIT 1 OFFSET %d",
                                     table->offset, -1 * move);
      // clang-format on
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
      if(sqlite3_step(stmt) == SQLITE_ROW)
      {
        new_offset = sqlite3_column_int(stmt, 0);
      }
      else
      {
        // if we are here, that means we don't have enough space to move as wanted. So we move to first position
        g_free(query);
        sqlite3_finalize(stmt);
        // clang-format off
        query = g_strdup_printf("SELECT m.rowid FROM memory.collected_images as m, main.selected_images as s "
                                    "WHERE m.imgid=s.imgid "
                                    "ORDER BY m.rowid LIMIT 1");
        // clang-format on
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
        if(sqlite3_step(stmt) == SQLITE_ROW)
        {
          new_offset = sqlite3_column_int(stmt, 0);
        }
      }
      g_free(query);
      sqlite3_finalize(stmt);
      if(new_offset == table->offset)
      {
        dt_control_log(_("you have reached the start of your selection"));
        return;
      }
    }
    else
    {
      new_offset = MAX(1, table->offset + move);
      if(new_offset == table->offset)
      {
        dt_control_log(_("you have reached the start of your collection"));
        return;
      }
    }
  }
  else
  {
    if(table->navigate_inside_selection)
    {
      sqlite3_stmt *stmt;
      // clang-format off
      gchar *query = g_strdup_printf(
                            "SELECT COUNT(m.rowid) FROM memory.collected_images as m, main.selected_images as s "
                            "WHERE m.imgid=s.imgid AND m.rowid>%d",
                            table->offset);
      // clang-format on
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
      int nb_after = 0;
      if(sqlite3_step(stmt) == SQLITE_ROW)
      {
        nb_after = sqlite3_column_int(stmt, 0);
      }
      g_free(query);
      sqlite3_finalize(stmt);

      if(nb_after >= table->thumbs_count)
      {
        const int delta = MIN(nb_after + 1 - table->thumbs_count, move);
        // clang-format off
        query = g_strdup_printf("SELECT m.rowid FROM memory.collected_images as m, main.selected_images as s "
                                "WHERE m.imgid=s.imgid AND m.rowid>=%d "
                                "ORDER BY m.rowid LIMIT 1 OFFSET %d",
                                table->offset, delta);
        // clang-format on
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
        if(sqlite3_step(stmt) == SQLITE_ROW)
        {
          new_offset = sqlite3_column_int(stmt, 0);
        }
        g_free(query);
        sqlite3_finalize(stmt);
      }

      if(new_offset == table->offset)
      {
        dt_control_log(_("you have reached the end of your selection"));
        return;
      }
    }
    else
    {
      sqlite3_stmt *stmt;
      // clang-format off
      gchar *query = g_strdup_printf("SELECT COUNT(m.rowid) FROM memory.collected_images as m "
                                     "WHERE m.rowid>%d",
                                     table->offset);
      // clang-format on
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
      if(sqlite3_step(stmt) == SQLITE_ROW)
      {
        const int nb = sqlite3_column_int(stmt, 0);
        if(nb >= table->thumbs_count)
        {
          new_offset = table->offset + MIN(nb + 1 - table->thumbs_count, move);
        }
      }
      g_free(query);
      sqlite3_finalize(stmt);
      if(new_offset == table->offset)
      {
        dt_control_log(_("you have reached the end of your collection"));
        return;
      }
    }
  }

  if(new_offset != table->offset)
  {
    table->offset = new_offset;
    dt_culling_full_redraw(table, TRUE);
    _thumbs_refocus(table);
  }
}

static void _set_table_zoom_ratio(dt_culling_t *table, dt_thumbnail_t *th)
{
  table->zoom_ratio = dt_thumbnail_get_zoom_ratio(th);
}

static void _get_root_offset(GtkWidget *w_image_box, float x_root, float y_root, int *x_offset, int *y_offset)
{
  gdk_window_get_origin(gtk_widget_get_window(w_image_box), x_offset, y_offset);
  *x_offset = x_root - *x_offset;
  *y_offset = y_root - *y_offset;
}

static gboolean _zoom_and_shift(dt_thumbnail_t *th, const int x_offset, const int y_offset, const float zoom_delta)
{
  const float zd = CLAMP(th->zoom + zoom_delta, 1.0f, th->zoom_100);
  if(zd == th->zoom)
    return FALSE; // delta_zoom did not change this thumbnail's zoom factor

  const float z_ratio = zd / th->zoom;
  th->zoom = zd;

  int posx = x_offset;
  int posy = y_offset;

  const int iw = gtk_widget_get_allocated_width(th->w_image);
  const int ih = gtk_widget_get_allocated_height(th->w_image);

  // we center the zoom around cursor position
  if(posx >= 0 && posy >= 0)
  {
    // we take in account that the image may be smaller that the imagebox
    posx -= (gtk_widget_get_allocated_width(th->w_image_box) - iw) / 2;
    posy -= (gtk_widget_get_allocated_height(th->w_image_box) - ih) / 2;
  }

  // we change the value. Values will be sanitized in the drawing event
  th->zoomx = posx - (posx - th->zoomx) * z_ratio;
  th->zoomy = posy - (posy - th->zoomy) * z_ratio;

  dt_thumbnail_image_refresh(th);

  return TRUE;
}

static gboolean _zoom_to_x_root(dt_thumbnail_t *th, const float x_root, const float y_root, const float zoom_delta)
{
  int x_offset = 0;
  int y_offset = 0;

  _get_root_offset(th->w_image_box, x_root, y_root, &x_offset, &y_offset);

  return _zoom_and_shift(th, x_offset, y_offset, zoom_delta);
}

static gboolean _zoom_to_center(dt_thumbnail_t *th, const float zoom_delta)
{
  const float zd = CLAMP(th->zoom + zoom_delta, 1.0f, th->zoom_100);
  if(zd == th->zoom)
    return FALSE; // delta_zoom did not change this thumbnail's zoom factor

  const float z_ratio = zd / th->zoom;
  th->zoom = zd;
  // we center the zoom around center of the shown image
  int iw = 0;
  int ih = 0;
  gtk_widget_get_size_request(th->w_image_box, &iw, &ih);
  th->zoomx = fmaxf(iw - th->img_width * z_ratio, fminf(0.0f, iw / 2.0 - (iw / 2.0 - th->zoomx) * z_ratio));
  th->zoomy = fmaxf(ih - th->img_height * z_ratio, fminf(0.0f, ih / 2.0 - (ih / 2.0 - th->zoomy) * z_ratio));

  dt_thumbnail_image_refresh(th);

  return TRUE;
}

static gboolean _thumbs_zoom_add(dt_culling_t *table, const float zoom_delta, const float x_root,
                                 const float y_root, int state)
{
  const int max_in_memory_images = _get_max_in_memory_images();
  if(table->mode == DT_CULLING_MODE_CULLING && table->thumbs_count > max_in_memory_images)
  {
    dt_control_log(_("zooming is limited to %d images"), max_in_memory_images);
    return TRUE;
  }

  // we ensure the zoom 100 is computed for all images
  for(GList *l = table->list; l; l = g_list_next(l))
  {
    dt_thumbnail_t *th = (dt_thumbnail_t *)l->data;
    dt_thumbnail_get_zoom100(th);
  }

  if(!g_list_shorter_than(table->list, 2))  // at least two images?
  {
    // CULLING with multiple images
    // if shift+ctrl, we only change the current image
    if(dt_modifiers_include(state, GDK_SHIFT_MASK))
    {
      const int mouseid = dt_control_get_mouse_over_id();
      for(GList *l = table->list; l; l = g_list_next(l))
      {
        dt_thumbnail_t *th = (dt_thumbnail_t *)l->data;
        if(th->imgid == mouseid)
        {
          if(_zoom_to_x_root(th, x_root, y_root, zoom_delta))
            _set_table_zoom_ratio(table, th);
          break;
        }
      }
    }
    else
    {
      const int mouseid = dt_control_get_mouse_over_id();
      int x_offset = 0;
      int y_offset = 0;
      gboolean to_pointer = FALSE;

      // get the offset for the image under the cursor
      for(GList *l = table->list; l; l = g_list_next(l))
      {
        dt_thumbnail_t *th = (dt_thumbnail_t *)l->data;
        if(th->imgid == mouseid)
        {
          _get_root_offset(th->w_image_box, x_root, y_root, &x_offset, &y_offset);
          to_pointer = TRUE;
          break;
        }
      }

      // apply the offset to all images
      for(GList *l = table->list; l; l = g_list_next(l))
      {
        dt_thumbnail_t *th = (dt_thumbnail_t *)l->data;
        if(to_pointer == TRUE ? _zoom_and_shift(th, x_offset, y_offset, zoom_delta)
                              : _zoom_to_center(th, zoom_delta))
          _set_table_zoom_ratio(table, th);
      }
    }
  }
  else if(table->list)
  {
    // FULL PREVIEW or CULLING with 1 image
    dt_thumbnail_t *th = (dt_thumbnail_t *)table->list->data;
    if(_zoom_to_x_root(th, x_root, y_root, zoom_delta))
      _set_table_zoom_ratio(table, th);
  }

  return TRUE;
}

static void _zoom_thumb_fit(dt_thumbnail_t *th)
{
  th->zoom = 1.0;
  th->zoomx = 0;
  th->zoomy = 0;
  dt_thumbnail_image_refresh(th);
}

static gboolean _zoom_thumb_max(dt_thumbnail_t *th, float x_root, float y_root)
{
  dt_thumbnail_get_zoom100(th);
  return _zoom_to_x_root(th, x_root, y_root, ZOOM_MAX);
}

// toggle zoom max / zoom fit of image currently having mouse over id
static void _toggle_zoom_current(dt_culling_t *table, float x_root, float y_root)
{
  const int id = dt_control_get_mouse_over_id();
  for(GList *l = table->list; l; l = g_list_next(l))
  {
    dt_thumbnail_t *th = (dt_thumbnail_t *)l->data;
    if(th->imgid == id)
    {
      if(th->zoom_100 < 1.0 || th->zoom < th->zoom_100)
        _zoom_thumb_max(th, x_root, y_root);
      else
        _zoom_thumb_fit(th);
      break;
    }
  }
}

// toggle zoom max / zoom fit of all images in culling table
static void _toggle_zoom_all(dt_culling_t *table, float x_root, float y_root)
{
  gboolean zmax = TRUE;
  for(GList *l = table->list; l; l = g_list_next(l))
  {
    dt_thumbnail_t *th = (dt_thumbnail_t *)l->data;
    if(th->zoom_100 < 1.0 || th->zoom < th->zoom_100)
    {
      zmax = FALSE;
      break;
    }
  }

  if(zmax)
    dt_culling_zoom_fit(table);
  else
    _thumbs_zoom_add(table, ZOOM_MAX, x_root, y_root, 0);
}

static gboolean _event_scroll(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
  GdkEventScroll *e = (GdkEventScroll *)event;
  dt_culling_t *table = (dt_culling_t *)user_data;
  int delta;

  if(dt_gui_get_scroll_unit_delta(e, &delta))
  {
    if(dt_modifiers_include(e->state, GDK_CONTROL_MASK))
    {
      // zooming
      const float zoom_delta = delta < 0 ? 0.5f : -0.5f;
      _thumbs_zoom_add(table, zoom_delta, e->x_root, e->y_root, e->state);
    }
    else
    {
      const int move = delta < 0 ? -1 : 1;
      _thumbs_move(table, move);
    }
  }
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
  // if the leaving cause is the hide of the widget, no mouseover change
  if(!gtk_widget_is_visible(widget))
  {
    table->mouse_inside = FALSE;
    return FALSE;
  }

  // if we leave thumbtable in favour of an inferior (a thumbnail) it's not a real leave !
  // same if this is not a mouse move action (shortcut that activate a button for example)
  if(event->detail == GDK_NOTIFY_INFERIOR || event->mode == GDK_CROSSING_GTK_GRAB
     || event->mode == GDK_CROSSING_GRAB)
    return FALSE;

  table->mouse_inside = FALSE;
  dt_control_set_mouse_over_id(NO_IMGID);
  return TRUE;
}

static gboolean _event_enter_notify(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  // we only handle the case where we enter thumbtable from an inferior (a thumbnail)
  // this is when the mouse enter an "empty" area of thumbtable
  if(event->detail != GDK_NOTIFY_INFERIOR) return FALSE;

  dt_control_set_mouse_over_id(NO_IMGID);
  return TRUE;
}

static gboolean _event_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_culling_t *table = (dt_culling_t *)user_data;

  if(event->button == 2)
  {
    // if shift is pressed, we work only with image hovered
    if(dt_modifier_is(event->state, GDK_SHIFT_MASK))
      _toggle_zoom_current(table, event->x_root, event->y_root);
    else
      _toggle_zoom_all(table, event->x_root, event->y_root);
    return TRUE;
  }

  const int id = dt_control_get_mouse_over_id();

  if(id > 0 && event->button == 1 && event->type == GDK_2BUTTON_PRESS)
  {
    dt_view_manager_switch(darktable.view_manager, "darkroom");
    return TRUE;
  }

  table->pan_x = event->x_root;
  table->pan_y = event->y_root;
  table->panning = TRUE;
  return TRUE;
}

static gboolean _event_motion_notify(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
  dt_culling_t *table = (dt_culling_t *)user_data;
  table->mouse_inside = TRUE;
  if(!table->panning)
  {
    table->pan_x = event->x_root;
    table->pan_y = event->y_root;
    return FALSE;
  }

  // get the max zoom of all images
  const int max_in_memory_images = _get_max_in_memory_images();
  if(table->mode == DT_CULLING_MODE_CULLING && table->thumbs_count > max_in_memory_images) return FALSE;

  float fz = 1.0f;
  for(GList *l = table->list; l; l = g_list_next(l))
  {
    dt_thumbnail_t *th = (dt_thumbnail_t *)l->data;
    fz = fmaxf(fz, th->zoom);
  }

  if(table->panning && fz > 1.0f)
  {
    const double x = event->x_root;
    const double y = event->y_root;
    // we want the images to stay in the screen
    const float scale = darktable.gui->ppd_thb / darktable.gui->ppd;
    const float valx = (x - table->pan_x) * scale;
    const float valy = (y - table->pan_y) * scale;

    if(dt_modifier_is(event->state, GDK_SHIFT_MASK))
    {
      int mouseid = dt_control_get_mouse_over_id();
      for(GList *l = table->list; l; l = g_list_next(l))
      {
        dt_thumbnail_t *th = (dt_thumbnail_t *)l->data;
        if(th->imgid == mouseid)
        {
          th->zoomx += valx;
          th->zoomy += valy;
          break;
        }
      }
    }
    else
    {
      for(GList *l = table->list; l; l = g_list_next(l))
      {
        dt_thumbnail_t *th = (dt_thumbnail_t *)l->data;
        th->zoomx += valx;
        th->zoomy += valy;
      }
    }
    // sanitize specific positions of individual images
    for(GList *l = table->list; l; l = g_list_next(l))
    {
      dt_thumbnail_t *th = (dt_thumbnail_t *)l->data;
      int iw = 0;
      int ih = 0;
      gtk_widget_get_size_request(th->w_image, &iw, &ih);
      const int mindx = iw * darktable.gui->ppd_thb - th->img_width;
      const int mindy = ih * darktable.gui->ppd_thb - th->img_height;
      if(th->zoomx > 0) th->zoomx = 0;
      if(th->zoomx < mindx) th->zoomx = mindx;
      if(th->zoomy > 0) th->zoomy = 0;
      if(th->zoomy < mindy) th->zoomy = mindy;
    }

    table->pan_x = x;
    table->pan_y = y;
  }

  for(GList *l = table->list; l; l = g_list_next(l))
  {
    dt_thumbnail_t *th = (dt_thumbnail_t *)l->data;
    dt_thumbnail_image_refresh_position(th);
  }
  return TRUE;
}

static gboolean _event_button_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_culling_t *table = (dt_culling_t *)user_data;
  table->panning = FALSE;
  return TRUE;
}

// called each time the preference change, to update specific parts
static void _dt_pref_change_callback(gpointer instance, gpointer user_data)
{
  if(!user_data) return;
  dt_culling_t *table = (dt_culling_t *)user_data;

  dt_culling_full_redraw(table, TRUE);

  for(GList *l = table->list; l; l = g_list_next(l))
  {
    dt_thumbnail_t *th = (dt_thumbnail_t *)l->data;
    th->overlay_timeout_duration = dt_conf_get_int("plugins/lighttable/overlay_timeout");
    dt_thumbnail_reload_infos(th);
    const float zoom_ratio = th->zoom_100 > 1 ? th->zoom / th->zoom_100 : table->zoom_ratio;
    dt_thumbnail_resize(th, th->width, th->height, TRUE, zoom_ratio);
  }
  dt_get_sysresource_level();
  dt_opencl_update_settings();
  dt_configure_ppd_dpi(darktable.gui);
}

static void _dt_selection_changed_callback(gpointer instance, gpointer user_data)
{
  if(!user_data) return;
  dt_culling_t *table = (dt_culling_t *)user_data;
  if(!gtk_widget_get_visible(table->widget)) return;

  // if we are in selection synchronisation mode, we exit this mode
  if(table->selection_sync) table->selection_sync = FALSE;

  // if we are in dynamic mode, zoom = selection count
  if(table->mode == DT_CULLING_MODE_CULLING
     && dt_view_lighttable_get_layout(darktable.view_manager) == DT_LIGHTTABLE_LAYOUT_CULLING_DYNAMIC)
  {
    sqlite3_stmt *stmt;
    int sel_count = 0;
    // clang-format off
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "SELECT count(*) "
                                "FROM memory.collected_images AS col, main.selected_images as sel "
                                "WHERE col.imgid=sel.imgid",
                                -1, &stmt, NULL);
    // clang-format on
    if(sqlite3_step(stmt) == SQLITE_ROW) sel_count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    const int nz = (sel_count <= 1) ? dt_conf_get_int("plugins/lighttable/culling_num_images") : sel_count;
    dt_view_lighttable_set_zoom(darktable.view_manager, nz);
  }
  // if we navigate only in the selection we just redraw to ensure no unselected image is present
  if(table->navigate_inside_selection)
  {
    dt_culling_full_redraw(table, TRUE);
    _thumbs_refocus(table);
  }
}

static void _dt_profile_change_callback(gpointer instance, int type, gpointer user_data)
{
  if(!user_data) return;
  dt_culling_t *table = (dt_culling_t *)user_data;
  if(!gtk_widget_get_visible(table->widget)) return;

  for(GList *l = table->list; l; l = g_list_next(l))
  {
    dt_thumbnail_t *th = (dt_thumbnail_t *)l->data;
    dt_thumbnail_image_refresh(th);
  }
}

// this is called each time mouse_over id change
static void _dt_mouse_over_image_callback(gpointer instance, gpointer user_data)
{
  if(!user_data) return;
  dt_culling_t *table = (dt_culling_t *)user_data;
  if(!gtk_widget_get_visible(table->widget)) return;

  const dt_imgid_t imgid = dt_control_get_mouse_over_id();

  // we crawl over all images to find the right one
  for(GList *l = table->list; l; l = g_list_next(l))
  {
    dt_thumbnail_t *th = (dt_thumbnail_t *)l->data;
    // if needed, the change mouseover value of the thumb
    if(th->mouse_over != (th->imgid == imgid)) dt_thumbnail_set_mouseover(th, (th->imgid == imgid));
  }
}

static void _dt_filmstrip_change(gpointer instance, dt_imgid_t imgid, gpointer user_data)
{
  if(!user_data || imgid <= 0) return;
  dt_culling_t *table = (dt_culling_t *)user_data;
  if(!gtk_widget_get_visible(table->widget)) return;

  table->offset = _thumb_get_rowid(imgid);
  dt_culling_full_redraw(table, TRUE);
  _thumbs_refocus(table);
}

// get the class name associated with the overlays mode
static gchar *_thumbs_get_overlays_class(dt_thumbnail_overlay_t over)
{
  switch(over)
  {
    case DT_THUMBNAIL_OVERLAYS_NONE:
      return g_strdup("dt_overlays_none");
    case DT_THUMBNAIL_OVERLAYS_HOVER_EXTENDED:
      return g_strdup("dt_overlays_hover_extended");
    case DT_THUMBNAIL_OVERLAYS_ALWAYS_NORMAL:
      return g_strdup("dt_overlays_always");
    case DT_THUMBNAIL_OVERLAYS_ALWAYS_EXTENDED:
      return g_strdup("dt_overlays_always_extended");
    case DT_THUMBNAIL_OVERLAYS_MIXED:
      return g_strdup("dt_overlays_mixed");
    case DT_THUMBNAIL_OVERLAYS_HOVER_BLOCK:
      return g_strdup("dt_overlays_hover_block");
    default:
      return g_strdup("dt_overlays_hover");
  }
}

dt_culling_t *dt_culling_new(dt_culling_mode_t mode)
{
  dt_culling_t *table = (dt_culling_t *)calloc(1, sizeof(dt_culling_t));
  table->mode = mode;
  table->zoom_ratio = IMG_TO_FIT;
  table->widget = gtk_layout_new(NULL, NULL);
  dt_gui_add_class(table->widget, "dt_fullview");
  // TODO dt_gui_add_help_link(table->widget, dt_get_help_url("lighttable_filemanager"));

  // overlays
  gchar *otxt = g_strdup_printf("plugins/lighttable/overlays/culling/%d", table->mode);
  table->overlays = dt_conf_get_int(otxt);
  g_free(otxt);

  gchar *cl0 = _thumbs_get_overlays_class(table->overlays);
  dt_gui_add_class(table->widget, cl0);
  free(cl0);

  otxt = g_strdup_printf("plugins/lighttable/overlays/culling_block_timeout/%d", table->mode);
  table->overlays_block_timeout = 2;
  if(!dt_conf_key_exists(otxt))
    table->overlays_block_timeout = dt_conf_get_int("plugins/lighttable/overlay_timeout");
  else
    table->overlays_block_timeout = dt_conf_get_int(otxt);
  g_free(otxt);

  otxt = g_strdup_printf("plugins/lighttable/tooltips/culling/%d", table->mode);
  table->show_tooltips = dt_conf_get_bool(otxt);
  g_free(otxt);

  // set widget signals
  gtk_widget_set_events(table->widget, GDK_EXPOSURE_MASK | GDK_POINTER_MOTION_MASK
                                           | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_STRUCTURE_MASK
                                           | GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK);
  gtk_widget_set_app_paintable(table->widget, TRUE);
  gtk_widget_set_can_focus(table->widget, TRUE);

  g_signal_connect(G_OBJECT(table->widget), "scroll-event", G_CALLBACK(_event_scroll), table);
  g_signal_connect(G_OBJECT(table->widget), "draw", G_CALLBACK(_event_draw), table);
  g_signal_connect(G_OBJECT(table->widget), "leave-notify-event", G_CALLBACK(_event_leave_notify), table);
  g_signal_connect(G_OBJECT(table->widget), "enter-notify-event", G_CALLBACK(_event_enter_notify), table);
  g_signal_connect(G_OBJECT(table->widget), "button-press-event", G_CALLBACK(_event_button_press), table);
  g_signal_connect(G_OBJECT(table->widget), "motion-notify-event", G_CALLBACK(_event_motion_notify), table);
  g_signal_connect(G_OBJECT(table->widget), "button-release-event", G_CALLBACK(_event_button_release), table);

  // we register globals signals
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_MOUSE_OVER_IMAGE_CHANGE,
                            G_CALLBACK(_dt_mouse_over_image_callback), table);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_CONTROL_PROFILE_USER_CHANGED,
                            G_CALLBACK(_dt_profile_change_callback), table);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_PREFERENCES_CHANGE, G_CALLBACK(_dt_pref_change_callback),
                            table);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_VIEWMANAGER_THUMBTABLE_ACTIVATE,
                            G_CALLBACK(_dt_filmstrip_change), table);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_SELECTION_CHANGED,
                            G_CALLBACK(_dt_selection_changed_callback), table);

  g_object_ref(table->widget);

  return table;
}

// initialize offset, ... values
// to be used when reentering culling
void dt_culling_init(dt_culling_t *table, int fallback_offset)
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
   *  culling dynamic mode                                    => OFF
   *  first image(s) == selection && selection is continuous  => ON
   */

  // init values
  table->navigate_inside_selection = FALSE;
  table->selection_sync = FALSE;
  table->zoom_ratio = IMG_TO_FIT;
  table->view_width = 0; // in order to force a full redraw

  // reset remaining zooming values if any
  for(GList *l = table->list; l; l = g_list_next(l))
  {
    dt_thumbnail_t *thumb = (dt_thumbnail_t *)l->data;
    thumb->zoom = 1.0f;
    thumb->zoomx = 0.0;
    thumb->zoomy = 0.0;
    thumb->img_surf_dirty = TRUE;
  }

  const gboolean culling_dynamic
      = (table->mode == DT_CULLING_MODE_CULLING
         && dt_view_lighttable_get_layout(darktable.view_manager) == DT_LIGHTTABLE_LAYOUT_CULLING_DYNAMIC);

  // get first id
  sqlite3_stmt *stmt;
  gchar *query = NULL;
  int first_id = -1;

  // prioritize mouseover if available
  first_id = dt_control_get_mouse_over_id();

  // try active images
  if(first_id < 1 && darktable.view_manager->active_images)
     first_id = GPOINTER_TO_INT(darktable.view_manager->active_images->data);

  // overwrite with selection no active images
  if(first_id < 1)
  {
    // search the first selected image
    // clang-format off
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "SELECT col.imgid "
                                "FROM memory.collected_images AS col, main.selected_images as sel "
                                "WHERE col.imgid=sel.imgid "
                                "ORDER BY col.rowid "
                                "LIMIT 1",
                                -1, &stmt, NULL);
    // clang-format on
    if(sqlite3_step(stmt) == SQLITE_ROW) first_id = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
  }

  // if no new offset is available until now, we continue with the fallback one
  if(first_id == -1)
    first_id = _thumb_get_imgid(fallback_offset);

  // if this also fails we start at the beginning of the collection
  if(first_id < 1)
  {
    first_id = _thumb_get_imgid(1);
  }

  if(first_id < 1)
  {
    // Collection probably empty?
    return;
  }

  // selection count
  int sel_count = 0;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT count(*) "
                              "FROM memory.collected_images AS col, main.selected_images as sel "
                              "WHERE col.imgid=sel.imgid",
                              -1, &stmt, NULL);
  // clang-format on
  if(sqlite3_step(stmt) == SQLITE_ROW) sel_count = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);

  // special culling dynamic mode
  if(culling_dynamic)
  {
    if(sel_count == 0)
    {
      dt_control_log(_("no image selected!"));
      first_id = -1;
    }
    table->navigate_inside_selection = TRUE;
    table->offset = _thumb_get_rowid(first_id);
    table->offset_imgid = first_id;
    return;
  }

  // is first_id inside selection ?
  gboolean inside = FALSE;
  // clang-format off
  query = g_strdup_printf("SELECT col.imgid "
                          "FROM memory.collected_images AS col, main.selected_images AS sel "
                          "WHERE col.imgid=sel.imgid AND col.imgid=%d",
                          first_id);
  // clang-format on
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
    // we first determine if we synchronize the selection with culling images
    table->selection_sync = FALSE;
    if(sel_count == 1 && inside)
      table->selection_sync = TRUE;
    else if(sel_count == zoom && inside)
    {
      // we ensure that the selection is continuous
      // clang-format off
      DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                  "SELECT MIN(rowid), MAX(rowid) "
                                  "FROM memory.collected_images AS col, main.selected_images as sel "
                                  "WHERE col.imgid=sel.imgid ",
                                  -1, &stmt, NULL);
      // clang-format on
      if(sqlite3_step(stmt) == SQLITE_ROW)
      {
        if(sqlite3_column_int(stmt, 0) + sel_count - 1 == sqlite3_column_int(stmt, 1))
        {
          table->selection_sync = TRUE;
        }
      }
      sqlite3_finalize(stmt);
    }

    // we now determine if we limit culling images to the selection
    table->navigate_inside_selection = (!table->selection_sync && inside);
  }

  table->offset = _thumb_get_rowid(first_id);
  table->offset_imgid = first_id;
}

static void _thumbs_prefetch(dt_culling_t *table)
{
  if(!table->list) return;

  // get the mip level by using the max image size actually shown
  int maxw = 0;
  int maxh = 0;
  for(GList *l = table->list; l; l = g_list_next(l))
  {
    dt_thumbnail_t *th = (dt_thumbnail_t *)l->data;
    maxw = MAX(maxw, th->width);
    maxh = MAX(maxh, th->height);
  }
  dt_mipmap_size_t mip = dt_mipmap_cache_get_matching_size(darktable.mipmap_cache, maxw, maxh);

  // prefetch next image
  gchar *query;
  sqlite3_stmt *stmt;
  dt_thumbnail_t *last = (dt_thumbnail_t *)g_list_last(table->list)->data;
  if(table->navigate_inside_selection)
  {
    // clang-format off
    query = g_strdup_printf(
                          "SELECT m.imgid "
                          "FROM memory.collected_images AS m, main.selected_images AS s "
                          "WHERE m.imgid = s.imgid"
                          " AND m.rowid > (SELECT mm.rowid FROM memory.collected_images AS mm WHERE mm.imgid=%d) "
                          "ORDER BY m.rowid "
                          "LIMIT 1",
                          last->imgid);
    // clang-format on
  }
  else
  {
    // clang-format off
    query = g_strdup_printf(
                          "SELECT m.imgid "
                          "FROM memory.collected_images AS m "
                          "WHERE m.rowid > (SELECT mm.rowid FROM memory.collected_images AS mm WHERE mm.imgid=%d) "
                          "ORDER BY m.rowid "
                          "LIMIT 1",
                          last->imgid);
    // clang-format on
  }
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const int id = sqlite3_column_int(stmt, 0);
    if(id > 0) dt_mipmap_cache_get(darktable.mipmap_cache, NULL, id, mip, DT_MIPMAP_PREFETCH, 'r');
  }
  sqlite3_finalize(stmt);
  g_free(query);

  // prefetch previous image
  dt_thumbnail_t *prev = (dt_thumbnail_t *)(table->list)->data;
  if(table->navigate_inside_selection)
  {
    // clang-format off
    query = g_strdup_printf(
                          "SELECT m.imgid "
                          "FROM memory.collected_images AS m, main.selected_images AS s "
                          "WHERE m.imgid = s.imgid"
                          " AND m.rowid < (SELECT mm.rowid FROM memory.collected_images AS mm WHERE mm.imgid=%d) "
                          "ORDER BY m.rowid DESC "
                          "LIMIT 1",
                          prev->imgid);
    // clang-format on
  }
  else
  {
    // clang-format off
    query = g_strdup_printf(
                          "SELECT m.imgid "
                          "FROM memory.collected_images AS m "
                          "WHERE m.rowid < (SELECT mm.rowid FROM memory.collected_images AS mm WHERE mm.imgid=%d) "
                          "ORDER BY m.rowid DESC "
                          "LIMIT 1",
                          prev->imgid);
    // clang-format on
  }
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const int id = sqlite3_column_int(stmt, 0);
    if(id > 0) dt_mipmap_cache_get(darktable.mipmap_cache, NULL, id, mip, DT_MIPMAP_PREFETCH, 'r');
  }
  sqlite3_finalize(stmt);
}

static gboolean _thumbs_recreate_list_at(dt_culling_t *table, const int offset)
{
  gchar *query = NULL;
  sqlite3_stmt *stmt;

  if(table->navigate_inside_selection)
  {
    // clang-format off
    query = g_strdup_printf("SELECT m.rowid, m.imgid, b.aspect_ratio "
                            "FROM memory.collected_images AS m, main.selected_images AS s, images AS b "
                            "WHERE m.imgid = b.id AND m.imgid = s.imgid AND m.rowid >= %d "
                            "ORDER BY m.rowid "
                            "LIMIT %d",
                            offset, table->thumbs_count);
    // clang-format on
  }
  else
  {
    // clang-format off
    query = g_strdup_printf("SELECT m.rowid, m.imgid, b.aspect_ratio "
                            "FROM (SELECT rowid, imgid "
                            "FROM memory.collected_images "
                            "WHERE rowid < %d + %d "
                            "ORDER BY rowid DESC "
                            "LIMIT %d) AS m, "
                            "images AS b "
                            "WHERE m.imgid = b.id "
                            "ORDER BY m.rowid",
                            offset, table->thumbs_count, table->thumbs_count);
    // clang-format on
  }

  GList *newlist = NULL;
  int pos = 0;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW && g_list_shorter_than(newlist, table->thumbs_count+1))
  {
    const int nrow = sqlite3_column_int(stmt, 0);
    const int nid = sqlite3_column_int(stmt, 1);
    // first, we search if the thumb is already here
    GList *tl = g_list_find_custom(table->list, GINT_TO_POINTER(nid), _list_compare_by_imgid);
    if(tl)
    {
      dt_thumbnail_t *thumb = (dt_thumbnail_t *)tl->data;
      thumb->rowid = nrow; // this may have changed
      thumb->display_focus = table->focus;
      newlist = g_list_prepend(newlist, thumb);
      // and we remove the thumb from the old list
      table->list = g_list_remove(table->list, thumb);
    }
    else
    {
      // we create a completely new thumb
      // we set its size to the thumb it replace in the list if any otherwise we set it to something > 0 to trigger
      // draw events
      int nw = 40;
      int nh = 40;
      if(table->mode == DT_CULLING_MODE_PREVIEW)
      {
        nw = table->view_width;
        nh = table->view_height;
      }
      else if(table->list)
      {
        dt_thumbnail_t *th_model
            = (dt_thumbnail_t *)g_list_nth_data(table->list, MIN(pos, g_list_length(table->list) - 1));
        nw = th_model->width;
        nh = th_model->height;
      }
      else if(newlist)
      {
        dt_thumbnail_t *th_model = (dt_thumbnail_t *)newlist->data;  // get most recently added
        nw = th_model->width;
        nh = th_model->height;
      }
      dt_thumbnail_t *thumb;
      if(table->mode == DT_CULLING_MODE_PREVIEW)
        thumb = dt_thumbnail_new(nw, nh, table->zoom_ratio, nid, nrow, table->overlays,
                                 DT_THUMBNAIL_CONTAINER_PREVIEW, table->show_tooltips);
      else
        thumb = dt_thumbnail_new(nw, nh, table->zoom_ratio, nid, nrow, table->overlays,
                                 DT_THUMBNAIL_CONTAINER_CULLING, table->show_tooltips);

      thumb->display_focus = table->focus;
      thumb->sel_mode = DT_THUMBNAIL_SEL_MODE_DISABLED;
      float aspect_ratio = sqlite3_column_double(stmt, 2);
      if(!aspect_ratio || aspect_ratio < 0.0001f)
      {
        aspect_ratio = dt_image_set_aspect_ratio(nid, FALSE);
        // if an error occurs, let's use 1:1 value
        if(aspect_ratio < 0.0001f) aspect_ratio = 1.0f;
      }
      thumb->aspect_ratio = aspect_ratio;
      newlist = g_list_prepend(newlist, thumb);
    }
    // if it's the offset, we record the imgid
    if(nrow == table->offset) table->offset_imgid = nid;
    pos++;
  }
  newlist = g_list_reverse(newlist); // list was built in reverse order, so un-reverse it

  // in rare cases, we can have less images than wanted
  // although there's images before (this shouldn't happen in preview)
  if(table->navigate_inside_selection && g_list_shorter_than(newlist, table->thumbs_count)
     && g_list_shorter_than(newlist, _get_selection_count()))
  {
    const int nb = table->thumbs_count - g_list_length(newlist);
    // clang-format off
    query = g_strdup_printf("SELECT m.rowid, m.imgid, b.aspect_ratio "
                            "FROM memory.collected_images AS m, main.selected_images AS s, images AS b "
                            "WHERE m.imgid = b.id AND m.imgid = s.imgid AND m.rowid < %d "
                            "ORDER BY m.rowid DESC "
                            "LIMIT %d",
                            offset, nb);
    // clang-format on
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
    if(stmt != NULL)
    {
      pos = 0;
      while(sqlite3_step(stmt) == SQLITE_ROW && g_list_shorter_than(newlist, table->thumbs_count+1))
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
          // we create a completely new thumb
          // we set its size to the thumb it replace in the list if any otherwise we set it to something > 0 to
          // trigger draw events
          int nw = 40;
          int nh = 40;
          if(table->list)
          {
            dt_thumbnail_t *th_model = (dt_thumbnail_t *)(table->list)->data;
            nw = th_model->width;
            nh = th_model->height;
          }
          else if(newlist)
          {
            dt_thumbnail_t *th_model = (dt_thumbnail_t *)newlist->data;
            nw = th_model->width;
            nh = th_model->height;
          }
          dt_thumbnail_t *thumb;
          if(table->mode == DT_CULLING_MODE_PREVIEW)
            thumb = dt_thumbnail_new(nw, nh, table->zoom_ratio, nid, nrow, table->overlays,
                                     DT_THUMBNAIL_CONTAINER_PREVIEW, table->show_tooltips);
          else
            thumb = dt_thumbnail_new(nw, nh, table->zoom_ratio, nid, nrow, table->overlays,
                                     DT_THUMBNAIL_CONTAINER_CULLING, table->show_tooltips);

          thumb->display_focus = table->focus;
          thumb->sel_mode = DT_THUMBNAIL_SEL_MODE_DISABLED;
          float aspect_ratio = sqlite3_column_double(stmt, 2);
          if(!aspect_ratio || aspect_ratio < 0.0001f)
          {
            aspect_ratio = dt_image_set_aspect_ratio(nid, FALSE);
            // if an error occurs, let's use 1:1 value
            if(aspect_ratio < 0.0001f) aspect_ratio = 1.0f;
          }
          thumb->aspect_ratio = aspect_ratio;
          newlist = g_list_prepend(newlist, thumb);
        }
        // if it's the offset, we record the imgid
        if(nrow == table->offset) table->offset_imgid = nid;
        pos++;
      }
      sqlite3_finalize(stmt);
    }
    g_free(query);
  }

  // now we cleanup all remaining thumbs from old table->list and set it again
  g_list_free_full(table->list, _list_remove_thumb);
  table->list = newlist;

  // and we ensure that we have the right offset
  if(table->list)
  {
    dt_thumbnail_t *thumb = (dt_thumbnail_t *)table->list->data;
    table->offset_imgid = thumb->imgid;
    table->offset = _thumb_get_rowid(thumb->imgid);
  }
  return TRUE;
}

static gboolean _thumbs_compute_positions(dt_culling_t *table)
{
  if(!gtk_widget_get_visible(table->widget)) return FALSE;
  if(!table->list) return FALSE;

  // if we have only 1 image, it should take the entire screen
  if(g_list_is_singleton(table->list))
  {
    dt_thumbnail_t *thumb = (dt_thumbnail_t *)table->list->data;
    thumb->width = table->view_width;
    thumb->height = table->view_height;
    thumb->x = 0;
    thumb->y = 0;
    return TRUE;
  }

  int sum_w = 0, max_h = 0, max_w = 0;

  unsigned int total_width = 0, total_height = 0;
  int distance = 1;
  float avg_ratio = 0;

  // reinit size and positions and get max values
  int count = 0;
  for(GList *l = table->list; l; l = g_list_next(l))
  {
    dt_thumbnail_t *thumb = (dt_thumbnail_t *)l->data;
    const float aspect_ratio = thumb->aspect_ratio;
    thumb->width = (gint)(sqrt(aspect_ratio) * 100);
    thumb->height = (gint)(1 / sqrt(aspect_ratio) * 100);
    thumb->x = thumb->y = 0;

    sum_w += thumb->width;
    max_w = MAX(max_w, thumb->width);
    max_h = MAX(max_h, thumb->height);
    avg_ratio += thumb->width / (float)thumb->height;
    count++;
  }

  avg_ratio /= count;

  int per_row, tmp_per_row, per_col, tmp_per_col;
  per_row = tmp_per_row = ceil(sqrt(count));
  per_col = tmp_per_col = (count + per_row - 1) / per_row;

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

    tmp_per_col = (count + tmp_per_row - 1) / tmp_per_row;

    tmp_slot_ratio = (table->view_width / (float)tmp_per_row) / (table->view_height / (float)tmp_per_col);

  } while(per_row > 0 && per_row <= count && _absmul(tmp_slot_ratio, avg_ratio) < _absmul(slot_ratio, avg_ratio));

  GList *slots = NULL;

  // Vertical layout
  for(GList *l = table->list; l; l = g_list_next(l))
  {
    dt_thumbnail_t *thumb = (dt_thumbnail_t *)l->data;
    GList *slot_iter = slots;
    for(; slot_iter; slot_iter = slot_iter->next)
    {
      GList *slot = (GList *)slot_iter->data;
      // Calculate current total height of slot
      int slot_h = distance;
      for(GList *slot_cw_iter = slot; slot_cw_iter; slot_cw_iter = g_list_next(slot_cw_iter))
      {
        dt_thumbnail_t *slot_cw = (dt_thumbnail_t *)slot_cw_iter->data;
        slot_h = slot_h + slot_cw->height + distance;
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
    if(!slot_iter) slots = g_list_prepend(slots, g_list_prepend(NULL, thumb));
  }
  slots = g_list_reverse(slots);  // list was built in reverse order, so un-reverse it

  GList *rows = g_list_append(NULL, NULL);
  {
    int row_y = 0, x = 0, row_h = 0;
    int max_row_w = sum_w / per_col;
    for(GList *slot_iter = slots; slot_iter; slot_iter = g_list_next(slot_iter))
    {
      GList *slot = (GList *)slot_iter->data;

      // Max width of windows in the slot
      int slot_max_w = 0;
      for(GList *slot_cw_iter = slot; slot_cw_iter; slot_cw_iter = g_list_next(slot_cw_iter))
      {
        dt_thumbnail_t *cw = (dt_thumbnail_t *)slot_cw_iter->data;
        slot_max_w = MAX(slot_max_w, cw->width);
      }

      int y = row_y;
      for(GList *slot_cw_iter = slot; slot_cw_iter; slot_cw_iter = g_list_next(slot_cw_iter))
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
        rows = rows->next;              // keep rows pointing at last element to avoid quadratic runtime
      }
      g_list_free(slot);
    }
    g_list_free(slots);
    slots = NULL;
  }

  rows = g_list_first(rows); // rows points at the last element of the constructed list, so move it back to the start

  total_width -= distance;
  total_height -= distance;

  for(const GList *iter = rows; iter; iter = g_list_next(iter))
  {
    GList *row = (GList *)iter->data;
    int row_w = 0, xoff;
    int max_rh = 0;

    for(GList *slot_cw_iter = row; slot_cw_iter; slot_cw_iter = g_list_next(slot_cw_iter))
    {
      dt_thumbnail_t *cw = (dt_thumbnail_t *)slot_cw_iter->data;
      row_w = MAX(row_w, cw->x + cw->width);
      max_rh = MAX(max_rh, cw->height);
    }

    xoff = (total_width - row_w) / 2;

    for(GList *cw_iter = row; cw_iter; cw_iter = g_list_next(cw_iter))
    {
      dt_thumbnail_t *cw = (dt_thumbnail_t *)cw_iter->data;
      cw->x += xoff;
      cw->height = max_rh;
    }
    g_list_free(row);
  }

  g_list_free(rows);

  float factor = (float)(table->view_width - 1) / total_width;
  if(factor * total_height > table->view_height - 1)
    factor = (float)(table->view_height - 1) / total_height;

  const int xoff = (table->view_width - (float)total_width * factor) / 2;
  const int yoff = (table->view_height - (float)total_height * factor) / 2;

  for(GList *l = table->list; l; l = g_list_next(l))
  {
    dt_thumbnail_t *thumb = (dt_thumbnail_t *)l->data;
    thumb->width  = thumb->width * factor;
    thumb->height = thumb->height * factor;
    thumb->x      = thumb->x * factor + xoff;
    thumb->y      = thumb->y * factor + yoff;
  }

  // we save the current first id
  dt_conf_set_int("plugins/lighttable/culling_last_id", table->offset_imgid);

  return TRUE;
}

void dt_culling_update_active_images_list(dt_culling_t *table)
{
  // we erase the list of active images
  g_slist_free(darktable.view_manager->active_images);
  darktable.view_manager->active_images = NULL;

  // and we effectively move and resize thumbs
  for(GList *l = table->list; l; l = g_list_next(l))
  {
    dt_thumbnail_t *thumb = (dt_thumbnail_t *)l->data;
    // we update the active images list
    darktable.view_manager->active_images
        = g_slist_append(darktable.view_manager->active_images, GINT_TO_POINTER(thumb->imgid));
  }

  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_ACTIVE_IMAGES_CHANGE);
}

// recreate the list of thumb if needed and recomputes sizes and positions if needed
void dt_culling_full_redraw(dt_culling_t *table, gboolean force)
{
  if(!gtk_widget_get_visible(table->widget)) return;
  const double start = dt_get_wtime();
  // first, we see if we need to do something
  if(!_compute_sizes(table, force)) return;

  // we store first image zoom and pos for new ones
  float old_zx = 0.0;
  float old_zy = 0.0;
  int old_margin_x = 0;
  int old_margin_y = 0;
  if(table->list)
  {
    dt_thumbnail_t *thumb = (dt_thumbnail_t *)table->list->data;
    old_zx = thumb->zoomx;
    old_zy = thumb->zoomy;
    old_margin_x = gtk_widget_get_margin_start(thumb->w_image_box);
    old_margin_y = gtk_widget_get_margin_top(thumb->w_image_box);
  }
  // we recreate the list of images
  _thumbs_recreate_list_at(table, table->offset);

  // we compute the sizes and positions of thumbs
  _thumbs_compute_positions(table);

  // we erase the list of active images
  g_slist_free(darktable.view_manager->active_images);
  darktable.view_manager->active_images = NULL;

  // and we effectively move and resize thumbs
  for(GList *l = table->list; l; l = g_list_next(l))
  {
    dt_thumbnail_t *thumb = (dt_thumbnail_t *)l->data;
    // we set the overlays timeout
    thumb->overlay_timeout_duration = table->overlays_block_timeout;
    // we add or move the thumb at the right position
    if(!gtk_widget_get_parent(thumb->w_main))
    {
      gtk_widget_set_margin_start(thumb->w_image_box, old_margin_x);
      gtk_widget_set_margin_top(thumb->w_image_box, old_margin_y);
      // and we resize the thumb
      dt_thumbnail_resize(thumb, thumb->width, thumb->height, FALSE, table->zoom_ratio);
      gtk_layout_put(GTK_LAYOUT(table->widget), thumb->w_main, thumb->x, thumb->y);
      thumb->zoomx = old_zx;
      thumb->zoomy = old_zy;
    }
    else
    {
      gtk_layout_move(GTK_LAYOUT(table->widget), thumb->w_main, thumb->x, thumb->y);
      // and we resize the thumb
      const float zoom_ratio = thumb->zoom_100 > 1 ? thumb->zoom / thumb->zoom_100 : IMG_TO_FIT;
      dt_thumbnail_resize(thumb, thumb->width, thumb->height, FALSE, zoom_ratio);
    }

    // we update the active images list
    darktable.view_manager->active_images
        = g_slist_append(darktable.view_manager->active_images, GINT_TO_POINTER(thumb->imgid));
  }

  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_ACTIVE_IMAGES_CHANGE);

  // if the selection should follow active images
  if(table->selection_sync)
  {
    // deactivate selection_change event
    table->select_desactivate = TRUE;
    // deselect all
    DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "DELETE FROM main.selected_images", NULL, NULL, NULL);
    // select all active images
    GList *ls = NULL;
    for(GList *l = table->list; l; l = g_list_next(l))
    {
      dt_thumbnail_t *thumb = (dt_thumbnail_t *)l->data;
      ls = g_list_prepend(ls, GINT_TO_POINTER(thumb->imgid));
    }
    ls = g_list_reverse(ls);  // list was built in reverse order, so un-reverse it
    dt_selection_select_list(darktable.selection, ls);
    g_list_free(ls);
    // reactivate selection_change event
    table->select_desactivate = FALSE;
  }

  // we prefetch next/previous images
  _thumbs_prefetch(table);

  // ensure that no hidden image as the focus
  const int selid = dt_control_get_mouse_over_id();
  if(selid >= 0)
  {
    gboolean in_list = FALSE;
    for(GList *l = table->list; l; l = g_list_next(l))
    {
      dt_thumbnail_t *thumb = (dt_thumbnail_t *)l->data;
      if(thumb->imgid == selid)
      {
        in_list = TRUE;
        break;
      }
    }
    if(!in_list)
    {
      dt_control_set_mouse_over_id(NO_IMGID);
    }
  }

  dt_print(DT_DEBUG_LIGHTTABLE, "done in %0.04f sec\n", dt_get_wtime() - start);

  if(darktable.unmuted & DT_DEBUG_CACHE) dt_mipmap_cache_print(darktable.mipmap_cache);
}

gboolean dt_culling_key_move(dt_culling_t *table, dt_culling_move_t move)
{
  int val = 0;
  switch(move)
  {
    case DT_CULLING_MOVE_LEFT:
    case DT_CULLING_MOVE_UP:
      val = -1;
      break;
    case DT_CULLING_MOVE_RIGHT:
    case DT_CULLING_MOVE_DOWN:
      val = 1;
      break;
    case DT_CULLING_MOVE_PAGEUP:
      val = -1 * table->thumbs_count;
      break;
    case DT_CULLING_MOVE_PAGEDOWN:
      val = table->thumbs_count;
      break;
    case DT_CULLING_MOVE_START:
      val = -1 * INT_MAX;
      break;
    case DT_CULLING_MOVE_END:
      val = INT_MAX;
      break;
    default:
      val = 0;
      break;
  }
  _thumbs_move(table, val);
  return TRUE;
}

void dt_culling_change_offset_image(dt_culling_t *table, dt_imgid_t imgid)
{
  table->offset = _thumb_get_rowid(imgid);
  dt_culling_full_redraw(table, TRUE);
  _thumbs_refocus(table);
}

void dt_culling_zoom_max(dt_culling_t *table)
{
  float x = 0;
  float y = 0;
  if(table->mode == DT_CULLING_MODE_PREVIEW && table->list)
  {
    dt_thumbnail_t *th = (dt_thumbnail_t *)table->list->data;
    x = gtk_widget_get_allocated_width(th->w_image_box) / 2.0;
    y = gtk_widget_get_allocated_height(th->w_image_box) / 2.0;
  }
  _thumbs_zoom_add(table, ZOOM_MAX, x, y, 0);
}

void dt_culling_zoom_fit(dt_culling_t *table)
{
  table->zoom_ratio = IMG_TO_FIT;
  for(GList *l = table->list; l; l = g_list_next(l))
  {
    _zoom_thumb_fit((dt_thumbnail_t *)l->data);
  }
}

// change the type of overlays that should be shown
void dt_culling_set_overlays_mode(dt_culling_t *table, dt_thumbnail_overlay_t over)
{
  if(!table) return;
  gchar *txt = g_strdup_printf("plugins/lighttable/overlays/culling/%d", table->mode);
  dt_conf_set_int(txt, over);
  g_free(txt);
  gchar *cl0 = _thumbs_get_overlays_class(table->overlays);
  gchar *cl1 = _thumbs_get_overlays_class(over);

  dt_gui_remove_class(table->widget, cl0);
  dt_gui_add_class(table->widget, cl1);

  txt = g_strdup_printf("plugins/lighttable/overlays/culling_block_timeout/%d", table->mode);
  int timeout = 2;
  if(!dt_conf_key_exists(txt))
    timeout = dt_conf_get_int("plugins/lighttable/overlay_timeout");
  else
    timeout = dt_conf_get_int(txt);
  g_free(txt);

  txt = g_strdup_printf("plugins/lighttable/tooltips/culling/%d", table->mode);
  table->show_tooltips = dt_conf_get_bool(txt);
  g_free(txt);

  // we need to change the overlay content if we pass from normal to extended overlays
  // this is not done on the fly with css to avoid computing extended msg for nothing and to reserve space if needed
  for(GList *l = table->list; l; l = g_list_next(l))
  {
    dt_thumbnail_t *th = (dt_thumbnail_t *)l->data;
    dt_thumbnail_set_overlay(th, over, timeout);
    th->tooltip = table->show_tooltips;
    // and we resize the bottom area
    const float zoom_ratio = th->zoom_100 > 1 ? th->zoom / th->zoom_100 : table->zoom_ratio;
    dt_thumbnail_resize(th, th->width, th->height, TRUE, zoom_ratio);
  }

  table->overlays = over;
  g_free(cl0);
  g_free(cl1);
}

// force the overlays to be shown
void dt_culling_force_overlay(dt_culling_t *table, const gboolean force)
{
  if(!table) return;

  int timeout = -1;

  gchar *txt = g_strdup_printf("plugins/lighttable/overlays/culling/%d", table->mode);
  dt_thumbnail_overlay_t over = dt_conf_get_int(txt);
  g_free(txt);
  gchar *cl0 = _thumbs_get_overlays_class(DT_THUMBNAIL_OVERLAYS_HOVER_BLOCK);
  gchar *cl1 = _thumbs_get_overlays_class(over);

  if(!force)
  {
    dt_gui_remove_class(table->widget, cl0);
    dt_gui_add_class(table->widget, cl1);

    txt = g_strdup_printf("plugins/lighttable/overlays/culling_block_timeout/%d", table->mode);
    timeout = 2;
    if(!dt_conf_key_exists(txt))
      timeout = dt_conf_get_int("plugins/lighttable/overlay_timeout");
    else
      timeout = dt_conf_get_int(txt);
    g_free(txt);
  }
  else
  {
    dt_gui_remove_class(table->widget, cl1);
    dt_gui_add_class(table->widget, cl0);
    over = DT_THUMBNAIL_OVERLAYS_HOVER_BLOCK;
  }

  g_free(cl0);
  g_free(cl1);

  // we need to change the overlay content if we pass from normal to extended overlays
  // this is not done on the fly with css to avoid computing extended msg for nothing and to reserve space if needed
  for(GList *l = table->list; l; l = g_list_next(l))
  {
    dt_thumbnail_t *th = (dt_thumbnail_t *)l->data;
    dt_thumbnail_set_overlay(th, over, timeout);
    // and we resize the bottom area
    const float zoom_ratio = th->zoom_100 > 1 ? th->zoom / th->zoom_100 : table->zoom_ratio;
    dt_thumbnail_resize(th, th->width, th->height, TRUE, zoom_ratio);
  }

  table->overlays = over;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

