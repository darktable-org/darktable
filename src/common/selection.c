/*
    This file is part of darktable,
    copyright (c) 2011 Henrik Andersson.

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

#include "common/collection.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/image_cache.h"
#include "control/signal.h"
#include "gui/gtk.h"

typedef struct dt_selection_t
{
  /* the collection clone used for selection */
  const dt_collection_t *collection;

  /* this stores the last single clicked image id indicating
     the start of a selection range */
  uint32_t last_single_id;
} dt_selection_t;

/* updates the internal collection of an selection */
static void _selection_update_collection(gpointer instance, gpointer user_data);

void _selection_update_collection(gpointer instance, gpointer user_data)
{
  dt_selection_t *selection = (dt_selection_t *)user_data;

  /* free previous collection copy if any */
  if(selection->collection) dt_collection_free(selection->collection);

  /* create a new fresh copy of darktable collection */
  selection->collection = dt_collection_new(darktable.collection);

  /* remove limit part of local collection */
  dt_collection_set_query_flags(selection->collection, (dt_collection_get_query_flags(selection->collection)
                                                        & (~(COLLECTION_QUERY_USE_LIMIT))));
  dt_collection_update(selection->collection);
}

const dt_selection_t *dt_selection_new()
{
  dt_selection_t *s = g_malloc0(sizeof(dt_selection_t));

  /* initialize the collection copy */
  _selection_update_collection(NULL, (gpointer)s);


  /* setup signal handler for darktable collection update
   to update the internal collection of the selection */
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_COLLECTION_CHANGED,
                            G_CALLBACK(_selection_update_collection), (gpointer)s);


  return s;
}

void dt_selection_free(dt_selection_t *selection)
{
  g_free(selection);
}

void dt_selection_invert(dt_selection_t *selection)
{
  gchar *fullq = NULL;

  if(!selection->collection) return;

  fullq = dt_util_dstrcat(fullq, "%s", "INSERT OR IGNORE INTO main.selected_images ");
  fullq = dt_util_dstrcat(fullq, "%s", dt_collection_get_query(selection->collection));

  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db),
                        "INSERT INTO memory.tmp_selection SELECT imgid FROM main.selected_images", NULL, NULL,
                        NULL);
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "DELETE FROM main.selected_images", NULL, NULL, NULL);
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), fullq, NULL, NULL, NULL);
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db),
                        "DELETE FROM main.selected_images WHERE imgid IN (SELECT imgid FROM memory.tmp_selection)",
                        NULL, NULL, NULL);
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "DELETE FROM memory.tmp_selection", NULL, NULL, NULL);

  g_free(fullq);

  /* update hint message */
  dt_collection_hint_message(darktable.collection);
}

void dt_selection_clear(const dt_selection_t *selection)
{
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "DELETE FROM main.selected_images", NULL, NULL, NULL);

  /* update hint message */
  dt_collection_hint_message(darktable.collection);
}

void dt_selection_select(dt_selection_t *selection, uint32_t imgid)
{
  gchar *query = NULL;

  if(imgid != -1)
  {
    const dt_image_t *image = dt_image_cache_get(darktable.image_cache, imgid, 'r');
    if(image)
    {
      int img_group_id = image->group_id;
      dt_image_cache_read_release(darktable.image_cache, image);

      if(!darktable.gui || !darktable.gui->grouping || darktable.gui->expanded_group_id == img_group_id || !selection->collection)
      {
        query = dt_util_dstrcat(query, "INSERT OR IGNORE INTO main.selected_images VALUES (%d)", imgid);
      }
      else
      {
        query = dt_util_dstrcat(query, "INSERT OR IGNORE INTO main.selected_images SELECT id FROM main.images "
                                       "WHERE group_id = %d AND id IN (%s)",
                                img_group_id, dt_collection_get_query_no_group(selection->collection));
      }

      DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), query, NULL, NULL, NULL);
      g_free(query);
    }
  }

  /* update hint message */
  dt_collection_hint_message(darktable.collection);
}

void dt_selection_deselect(dt_selection_t *selection, uint32_t imgid)
{
  gchar *query = NULL;
  selection->last_single_id = -1;

  if(imgid != -1)
  {
    const dt_image_t *image = dt_image_cache_get(darktable.image_cache, imgid, 'r');
    if(image)
    {
      int img_group_id = image->group_id;
      dt_image_cache_read_release(darktable.image_cache, image);

      if(!darktable.gui || !darktable.gui->grouping || darktable.gui->expanded_group_id == img_group_id)
      {
        query = dt_util_dstrcat(query, "DELETE FROM main.selected_images WHERE imgid = %d", imgid);
      }
      else
      {
        query = dt_util_dstrcat(query, "DELETE FROM main.selected_images WHERE imgid IN "
                                       "(SELECT id FROM main.images WHERE group_id = %d)",
                                img_group_id);
      }

      DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), query, NULL, NULL, NULL);
      g_free(query);
    }
  }

  /* update hint message */
  dt_collection_hint_message(darktable.collection);
}

void dt_selection_select_single(dt_selection_t *selection, uint32_t imgid)
{
  selection->last_single_id = imgid;
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "DELETE FROM main.selected_images", NULL, NULL, NULL);
  dt_selection_select(selection, imgid);
}

void dt_selection_toggle(dt_selection_t *selection, uint32_t imgid)
{
  sqlite3_stmt *stmt;
  gboolean exists = FALSE;

  if(imgid == -1) return;

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT imgid FROM main.selected_images WHERE imgid=?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);

  if(sqlite3_step(stmt) == SQLITE_ROW) exists = TRUE;

  sqlite3_finalize(stmt);

  if(exists)
  {
    dt_selection_deselect(selection, imgid);
  }
  else
  {
    dt_selection_select(selection, imgid);
  }

  /* update hint message */
  dt_collection_hint_message(darktable.collection);
}

void dt_selection_select_all(dt_selection_t *selection)
{
  gchar *fullq = NULL;

  if(!selection->collection) return;

  fullq = dt_util_dstrcat(fullq, "%s", "INSERT OR IGNORE INTO main.selected_images ");
  fullq = dt_util_dstrcat(fullq, "%s", dt_collection_get_query_no_group(selection->collection));

  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "DELETE FROM main.selected_images", NULL, NULL, NULL);
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), fullq, NULL, NULL, NULL);

  selection->last_single_id = -1;

  g_free(fullq);

  /* update hint message */
  dt_collection_hint_message(darktable.collection);
}

void dt_selection_select_range(dt_selection_t *selection, uint32_t imgid)
{
  gchar *fullq = NULL;
  sqlite3_stmt *stmt;
  if(!selection->collection || selection->last_single_id == -1) return;

  /* get start and end rows for range selection */
  int rc = 0;
  uint32_t sr = -1, er = -1;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), dt_collection_get_query_no_group(selection->collection),
                              -1, &stmt, NULL);

  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    int id = sqlite3_column_int(stmt, 0);
    if(id == selection->last_single_id) sr = rc;

    if(id == imgid) er = rc;

    if(sr != -1 && er != -1) break;

    rc++;
  }

  sqlite3_finalize(stmt);

  /* select the images in range from start to end */
  uint32_t old_flags = dt_collection_get_query_flags(selection->collection);

  /* use the limit to select range of images */
  dt_collection_set_query_flags(selection->collection, (old_flags | COLLECTION_QUERY_USE_LIMIT));

  dt_collection_update(selection->collection);

  fullq = dt_util_dstrcat(fullq, "%s", "INSERT OR IGNORE INTO main.selected_images ");
  fullq = dt_util_dstrcat(fullq, "%s", dt_collection_get_query_no_group(selection->collection));

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), fullq, -1, &stmt, NULL);

  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, MIN(sr, er));
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, (MAX(sr, er) - MIN(sr, er)) + 1);

  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  /* reset filter */
  dt_collection_set_query_flags(selection->collection, old_flags);
  dt_collection_update(selection->collection);

  // The logic above doesn't handle groups, so explicitly select the beginning and end to make sure those are selected properly
  dt_selection_select(selection, selection->last_single_id);
  dt_selection_select(selection, imgid);

  selection->last_single_id = -1;
}

void dt_selection_select_filmroll(dt_selection_t *selection)
{
  // clear at start, too, just to be sure:
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "DELETE FROM memory.tmp_selection", NULL, NULL, NULL);
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db),
                        "INSERT INTO memory.tmp_selection SELECT imgid FROM main.selected_images", NULL, NULL,
                        NULL);
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "DELETE FROM main.selected_images", NULL, NULL, NULL);
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db),
                        "INSERT OR IGNORE INTO main.selected_images SELECT id FROM main.images WHERE film_id IN "
                        "(SELECT film_id FROM main.images AS a JOIN memory.tmp_selection AS "
                        "b ON a.id = b.imgid)",
                        NULL, NULL, NULL);
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "DELETE FROM memory.tmp_selection", NULL, NULL, NULL);

  dt_collection_update(selection->collection);

  selection->last_single_id = -1;
}

void dt_selection_select_unaltered(dt_selection_t *selection)
{
  char *fullq = NULL;

  if(!selection->collection) return;

  /* set unaltered collection filter and update query */
  uint32_t old_filter_flags = dt_collection_get_filter_flags(selection->collection);
  dt_collection_set_filter_flags(selection->collection, (dt_collection_get_filter_flags(selection->collection)
                                                         | COLLECTION_FILTER_UNALTERED));
  dt_collection_update(selection->collection);


  fullq = dt_util_dstrcat(fullq, "%s", "INSERT OR IGNORE INTO main.selected_images ");
  fullq = dt_util_dstrcat(fullq, "%s", dt_collection_get_query(selection->collection));


  /* clean current selection and select unaltered images */
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "DELETE FROM main.selected_images", NULL, NULL, NULL);
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), fullq, NULL, NULL, NULL);

  /* restore collection filter and update query */
  dt_collection_set_filter_flags(selection->collection, old_filter_flags);
  dt_collection_update(selection->collection);

  g_free(fullq);

  selection->last_single_id = -1;
}


void dt_selection_select_list(struct dt_selection_t *selection, GList *list)
{
  if(!list) return;
  while(list)
  {
    int count = 1;
    gchar *query = NULL;

    int imgid = GPOINTER_TO_INT(list->data);
    selection->last_single_id = imgid;
    query = dt_util_dstrcat(query, "INSERT OR IGNORE INTO main.selected_images VALUES (%d)", imgid);
    list = g_list_next(list);
    while(list && count < 400)
    {
      imgid = GPOINTER_TO_INT(list->data);
      count++;
      selection->last_single_id = imgid;
      query = dt_util_dstrcat(query, ",(%d)", imgid);
      list = g_list_next(list);
    }
    char *result = NULL;

    sqlite3_exec(dt_database_get(darktable.db), query, NULL, NULL, &result);

    g_free(query);
  }

  /* update hint message */
  dt_collection_hint_message(darktable.collection);
}


// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
