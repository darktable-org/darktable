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

static gboolean _compute_sizes(dt_thumbtable_t *table, gboolean force)
{
  gboolean ret = FALSE; // return value to show if something as changed
  const int npr = dt_view_lighttable_get_zoom(darktable.view_manager);
  GtkAllocation allocation;
  gtk_widget_get_allocation(table->widget, &allocation);

  if(allocation.width <= 20 || allocation.height <= 20) return FALSE;

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
  return ret;
}

static void _move_up(dt_thumbtable_t *table)
{
  const int new_rowid = MAX(0, table->offset - table->thumbs_per_row);

  // we insert the images at the beginning
  const int nb = table->offset - new_rowid;
  dt_thumbnail **thumbs = g_newa(dt_thumbnail *, nb);

  sqlite3_stmt *stmt;
  gchar *query = dt_util_dstrcat(
      NULL, "SELECT imgid FROM memory.collected_images WHERE rowid>=%d ORDER BY rowid LIMIT %d", new_rowid, nb);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  int pos = 0;
  while(sqlite3_step(stmt) == SQLITE_ROW && pos < nb)
  {
    int imgid_sel = sqlite3_column_int(stmt, 0);
    thumbs[pos] = g_object_new(TYPE_DT_THUMBNAIL, NULL);
    thumbs[pos]->imgid = imgid_sel;
    thumbs[pos]->width = table->thumb_size;
    thumbs[pos]->height = table->thumb_size;
    pos++;
  }
  g_free(query);
  sqlite3_finalize(stmt);

  g_list_store_splice(table->fstore, 0, 0, (gpointer *)thumbs, nb);

  for(int i = 0; i < nb; i++) g_object_unref(thumbs[i]);

  // we remove the images at the end
  g_list_store_splice(table->fstore, table->rows * table->thumbs_per_row, nb, NULL, 0);

  table->offset = new_rowid;
}

static void _move_down(dt_thumbtable_t *table)
{
  // we want the nb of images in collection
  int count = 0;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT count(*) FROM memory.collected_images", -1,
                              &stmt, NULL);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    count = sqlite3_column_int(stmt, 0);
  }
  sqlite3_finalize(stmt);

  // the number of images fully visible on screen
  const int nb_on_screen = (table->rows - 1) * table->thumbs_per_row;
  if(table->offset + nb_on_screen > count) return;

  // and we can compute the new offset
  const int new_rowid = table->offset + table->thumbs_per_row;

  // we insert the images at the end
  const int nb = new_rowid - table->offset;
  dt_thumbnail **thumbs = g_newa(dt_thumbnail *, nb);

  gchar *query
      = dt_util_dstrcat(NULL, "SELECT imgid FROM memory.collected_images WHERE rowid>=%d  ORDER BY rowid LIMIT %d",
                        new_rowid + nb_on_screen, nb);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  int pos = 0;
  while(sqlite3_step(stmt) == SQLITE_ROW && pos < nb)
  {
    int imgid_sel = sqlite3_column_int(stmt, 0);
    thumbs[pos] = g_object_new(TYPE_DT_THUMBNAIL, NULL);
    thumbs[pos]->imgid = imgid_sel;
    thumbs[pos]->width = table->thumb_size;
    thumbs[pos]->height = table->thumb_size;
    pos++;
  }
  g_free(query);
  sqlite3_finalize(stmt);

  // and blank images if needed
  for(int i = pos; i < nb; i++)
  {
    thumbs[i] = g_object_new(TYPE_DT_THUMBNAIL, NULL);
    thumbs[i]->imgid = -1;
    thumbs[i]->width = table->thumb_size;
    thumbs[i]->height = table->thumb_size;
  }

  g_list_store_splice(table->fstore, table->rows * table->thumbs_per_row, 0, (gpointer *)thumbs, nb);

  for(int i = 0; i < nb; i++) g_object_unref(thumbs[i]);

  // we remove the images at the beginning
  g_list_store_splice(table->fstore, 0, nb, NULL, 0);

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
  table->fstore = g_list_store_new(TYPE_DT_THUMBNAIL);
  table->flow = gtk_flow_box_new();
  gtk_widget_set_events(table->flow, GDK_EXPOSURE_MASK | GDK_POINTER_MOTION_MASK | GDK_POINTER_MOTION_HINT_MASK
                                         | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_STRUCTURE_MASK
                                         | GDK_ENTER_NOTIFY_MASK);
  gtk_widget_set_app_paintable(table->flow, TRUE);
  gtk_flow_box_bind_model(GTK_FLOW_BOX(table->flow), G_LIST_MODEL(table->fstore), dt_thumbnail_get_widget, NULL,
                          NULL);
  table->widget = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(table->widget), GTK_POLICY_EXTERNAL, GTK_POLICY_EXTERNAL);
  g_signal_connect(G_OBJECT(table->widget), "scroll-event", G_CALLBACK(_scroll_event_callback), table);
  g_signal_connect(G_OBJECT(table->flow), "draw", G_CALLBACK(_draw_event_callback), table);
  g_signal_connect(G_OBJECT(table->flow), "leave-notify-event", G_CALLBACK(_leave_notify_callback), table);

  gtk_container_add(GTK_CONTAINER(table->widget), table->flow);
  gtk_widget_show_all(table->widget);

  g_object_ref(table->widget);
  return table;
}

void dt_thumbtable_full_redraw(dt_thumbtable_t *table, gboolean force)
{
  if(!table) return;
  if(_compute_sizes(table, force))
  {
    sqlite3_stmt *stmt;
    printf("reload thumbs from db. force=%d w=%d h=%d zoom=%d ...\n", force, table->view_width, table->view_height,
           table->thumbs_per_row);

    g_list_store_remove_all(table->fstore);

    gchar *query = dt_util_dstrcat(NULL, "SELECT imgid FROM memory.collected_images WHERE rowid>=%d LIMIT %d",
                                   table->offset, table->rows * table->thumbs_per_row);
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
    while(sqlite3_step(stmt) == SQLITE_ROW)
    {
      int imgid_sel = sqlite3_column_int(stmt, 0);
      dt_thumbnail *thumb = g_object_new(TYPE_DT_THUMBNAIL, NULL);
      thumb->imgid = imgid_sel;
      thumb->width = table->thumb_size;
      thumb->height = table->thumb_size;
      g_list_store_append(table->fstore, thumb);
      g_object_unref(thumb);
    }
    printf("done\n");
    g_free(query);
    sqlite3_finalize(stmt);
  }
}


// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;