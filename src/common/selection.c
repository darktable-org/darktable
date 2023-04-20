/*
    This file is part of darktable,
    Copyright (C) 2011-2021 darktable developers.

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
#include "views/view.h"

typedef struct dt_selection_t
{
  /* the collection clone used for selection */
  const dt_collection_t *collection;

  /* this stores the last single clicked image id indicating
     the start of a selection range */
  int32_t last_single_id;
} dt_selection_t;

const dt_collection_t *dt_selection_get_collection(struct dt_selection_t *selection)
{
  return selection->collection;
}

static void _selection_raise_signal()
{
  // discard cached images_to_act_on list
  dt_act_on_reset_cache(TRUE);
  dt_act_on_reset_cache(FALSE);

  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_SELECTION_CHANGED);
}

/* updates the internal collection of an selection */
static void _selection_update_collection(gpointer instance, dt_collection_change_t query_change,
                                         dt_collection_properties_t changed_property, gpointer imgs, int next,
                                         gpointer user_data);

static void _selection_select(dt_selection_t *selection, dt_imgid_t imgid)
{
  if(dt_is_valid_imgid(imgid))
  {
    const dt_image_t *image = dt_image_cache_get(darktable.image_cache, imgid, 'r');
    if(image)
    {
      const int img_group_id = image->group_id;
      dt_image_cache_read_release(darktable.image_cache, image);

      gchar *query = NULL;
      if(!darktable.gui || !darktable.gui->grouping || darktable.gui->expanded_group_id == img_group_id
         || !selection->collection)
      {
        query = g_strdup_printf("INSERT OR IGNORE INTO main.selected_images VALUES (%u)", imgid);
      }
      else
      {
        // clang-format off
        query = g_strdup_printf("INSERT OR IGNORE INTO main.selected_images"
                                "  SELECT id"
                                "  FROM main.images "
                                "  WHERE group_id = %d AND id IN (%s)",
                                img_group_id, dt_collection_get_query_no_group(selection->collection));
        // clang-format on
      }

      DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), query, NULL, NULL, NULL);
      g_free(query);
    }
  }

  _selection_raise_signal();

  /* update hint message */
  dt_collection_hint_message(darktable.collection);
}

void _selection_update_collection(gpointer instance, dt_collection_change_t query_change,
                                  dt_collection_properties_t changed_property, gpointer imgs, int next,
                                  gpointer user_data)
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
  _selection_update_collection(NULL, DT_COLLECTION_CHANGE_RELOAD, DT_COLLECTION_PROP_UNDEF, NULL, -1, (gpointer)s);

  /* initialize last_single_id based on current database */
  s->last_single_id = NO_IMGID;

  if(dt_collection_get_selected_count(darktable.collection) >= 1)
  {
    GList *selected_image = dt_collection_get_selected(darktable.collection, 1);
    if(selected_image)
    {
      s->last_single_id = GPOINTER_TO_INT(selected_image->data);
      g_list_free(selected_image);
    }
  }

  /* setup signal handler for darktable collection update
   to update the internal collection of the selection */
  //TODO: check whether this is actually necessary, since dt_collection_update_query calls dt_collection_update
  //  before raising the signal
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_COLLECTION_CHANGED,
                            G_CALLBACK(_selection_update_collection), (gpointer)s);

  return s;
}

void dt_selection_free(dt_selection_t *selection)
{
  g_free(selection);
}

void dt_selection_invert(dt_selection_t *selection)
{
  if(!selection->collection) return;

  gchar *fullq = g_strdup_printf("INSERT OR IGNORE INTO main.selected_images %s",
                                 dt_collection_get_query(selection->collection));

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

  _selection_raise_signal();

  /* update hint message */
  dt_collection_hint_message(darktable.collection);
}

void dt_selection_clear(const dt_selection_t *selection)
{
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "DELETE FROM main.selected_images", NULL, NULL, NULL);

  _selection_raise_signal();

  /* update hint message */
  dt_collection_hint_message(darktable.collection);
}

void dt_selection_select(dt_selection_t *selection, dt_imgid_t imgid)
{
  if(!dt_is_valid_imgid(imgid)) return;
  _selection_select(selection, imgid);
  selection->last_single_id = imgid;
}

void dt_selection_deselect(dt_selection_t *selection, dt_imgid_t imgid)
{
  selection->last_single_id = NO_IMGID;

  if(dt_is_valid_imgid(imgid))
  {
    const dt_image_t *image = dt_image_cache_get(darktable.image_cache, imgid, 'r');
    if(image)
    {
      const int img_group_id = image->group_id;
      dt_image_cache_read_release(darktable.image_cache, image);

      gchar *query = NULL;
      if(!darktable.gui || !darktable.gui->grouping || darktable.gui->expanded_group_id == img_group_id)
      {
        query = g_strdup_printf("DELETE FROM main.selected_images WHERE imgid = %u", imgid);
      }
      else
      {
        // clang-format off
        query = g_strdup_printf("DELETE FROM main.selected_images WHERE imgid IN "
                                "(SELECT id FROM main.images WHERE group_id = %d)",
                                img_group_id);
        // clang-format on
      }

      DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), query, NULL, NULL, NULL);
      g_free(query);
    }
  }

  _selection_raise_signal();

  /* update hint message */
  dt_collection_hint_message(darktable.collection);
}

void dt_selection_select_single(dt_selection_t *selection, dt_imgid_t imgid)
{
  selection->last_single_id = imgid;
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "DELETE FROM main.selected_images", NULL, NULL, NULL);
  dt_selection_select(selection, imgid);
}

void dt_selection_toggle(dt_selection_t *selection, dt_imgid_t imgid)
{
  sqlite3_stmt *stmt;
  gboolean exists = FALSE;

  if(!dt_is_valid_imgid(imgid)) return;

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
    selection->last_single_id = imgid;
  }

  _selection_raise_signal();

  /* update hint message */
  dt_collection_hint_message(darktable.collection);
}

void dt_selection_select_all(dt_selection_t *selection)
{
  if(!selection->collection) return;

  gchar *fullq = g_strdup_printf("INSERT OR IGNORE INTO main.selected_images %s",
                                 dt_collection_get_query_no_group(selection->collection));

  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "DELETE FROM main.selected_images", NULL, NULL, NULL);
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), fullq, NULL, NULL, NULL);

  selection->last_single_id = NO_IMGID;

  g_free(fullq);

  _selection_raise_signal();

  /* update hint message */
  dt_collection_hint_message(darktable.collection);
}

void dt_selection_select_range(dt_selection_t *selection, dt_imgid_t imgid)
{
  if(!selection->collection) return;

  // if no selection is made, add the selected image to the selection and return
  if(!dt_collection_get_selected_count(darktable.collection))
  {
    dt_selection_select(selection, imgid);
    return;
  }

  /* get start and end rows for range selection */
  sqlite3_stmt *stmt;
  int rc = 0;
  int sr = -1, er = -1;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), dt_collection_get_query_no_group(selection->collection),
                              -1, &stmt, NULL);

  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const dt_imgid_t id = sqlite3_column_int(stmt, 0);
    if(id == selection->last_single_id) sr = rc;

    if(id == imgid) er = rc;

    if(sr != -1 && er != -1) break;

    rc++;
  }
  sqlite3_finalize(stmt);

  // if imgid not in collection, nothing to do
  if(er < 0) return;

  // if last_single_id not in collection, we either use last selected image or first collected one
  dt_imgid_t srid = selection->last_single_id;
  if(sr < 0)
  {
    sr = 0;
    srid = NO_IMGID;
    // clang-format off
    DT_DEBUG_SQLITE3_PREPARE_V2(
        dt_database_get(darktable.db),
        "SELECT m.rowid, m.imgid FROM memory.collected_images AS m, main.selected_images AS s"
        " WHERE m.imgid=s.imgid"
        " ORDER BY m.rowid DESC"
        " LIMIT 1",
        -1, &stmt, NULL);
    // clang-format on
    if(sqlite3_step(stmt) == SQLITE_ROW)
    {
      sr = sqlite3_column_int(stmt, 0);
      srid = sqlite3_column_int(stmt, 1);
    }
    sqlite3_finalize(stmt);
  }

  /* select the images in range from start to end */
  const uint32_t old_flags = dt_collection_get_query_flags(selection->collection);

  /* use the limit to select range of images */
  dt_collection_set_query_flags(selection->collection, (old_flags | COLLECTION_QUERY_USE_LIMIT));

  dt_collection_update(selection->collection);

  gchar *fullq = g_strdup_printf("INSERT OR IGNORE INTO main.selected_images %s",
                                 dt_collection_get_query_no_group(selection->collection));

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), fullq, -1, &stmt, NULL);

  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, MIN(sr, er));
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, (MAX(sr, er) - MIN(sr, er)) + 1);

  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  /* reset filter */
  dt_collection_set_query_flags(selection->collection, old_flags);
  dt_collection_update(selection->collection);

  // The logic above doesn't handle groups, so explicitly select the beginning and end to make sure those are selected properly
  dt_selection_select(selection, srid);
  dt_selection_select(selection, imgid);

  g_free(fullq);
}

void dt_selection_select_filmroll(dt_selection_t *selection)
{
  // clear at start, too, just to be sure:
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "DELETE FROM memory.tmp_selection", NULL, NULL, NULL);
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db),
                        "INSERT INTO memory.tmp_selection SELECT imgid FROM main.selected_images", NULL, NULL,
                        NULL);
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "DELETE FROM main.selected_images", NULL, NULL, NULL);
  // clang-format off
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db),
                        "INSERT OR IGNORE INTO main.selected_images SELECT id FROM main.images WHERE film_id IN "
                        "(SELECT film_id FROM main.images AS a JOIN memory.tmp_selection AS "
                        "b ON a.id = b.imgid)",
                        NULL, NULL, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "DELETE FROM memory.tmp_selection", NULL, NULL, NULL);

  dt_collection_update(selection->collection);

  selection->last_single_id = NO_IMGID;

  _selection_raise_signal();

  /* update hint message */
  dt_collection_hint_message(darktable.collection);
}

void dt_selection_select_unaltered(dt_selection_t *selection)
{
  if(!selection->collection) return;

  /* clean current selection and select unaltered images */
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db),
                        "DELETE FROM main.selected_images", NULL, NULL, NULL);

  DT_DEBUG_SQLITE3_EXEC
    (dt_database_get(darktable.db),
     "INSERT OR IGNORE"
     " INTO main.selected_images"
     " SELECT h.imgid"
     "  FROM memory.collected_images as ci, main.history_hash as h"
     "  WHERE ci.imgid = h.imgid"
     "    AND (h.current_hash = h.auto_hash"
     "         OR h.current_hash IS NULL)",
     NULL, NULL, NULL);

  selection->last_single_id = NO_IMGID;
  _selection_raise_signal();

  /* update hint message */
  dt_collection_hint_message(darktable.collection);
}


void dt_selection_select_list(struct dt_selection_t *selection, GList *list)
{
  if(!list) return;
  while(list)
  {
    int count = 1;
    dt_imgid_t imgid = GPOINTER_TO_INT(list->data);
    selection->last_single_id = imgid;
    gchar *query = g_strdup_printf("INSERT OR IGNORE INTO main.selected_images VALUES (%d)", imgid);
    list = g_list_next(list);
    while(list && count < 400)
    {
      imgid = GPOINTER_TO_INT(list->data);
      count++;
      selection->last_single_id = imgid;
      query = dt_util_dstrcat(query, ",(%d)", imgid);
      list = g_list_next(list);
    }
    DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), query, NULL, NULL, NULL);

    g_free(query);
  }

  _selection_raise_signal();

  /* update hint message */
  dt_collection_hint_message(darktable.collection);
}

// return the query used to get the selection
// be carefull : if ordering is TRUE, the order depend of only_visible :
// DESC order if only_visible is TRUE ; ASC order otherwise...
gchar *dt_selection_get_list_query(struct dt_selection_t *selection, const gboolean only_visible,
                                   const gboolean ordering)
{
  gchar *query = NULL;
  if(only_visible)
  {
    // we don't want to get image hidden because of grouping
    // clang-format off
    query = g_strdup_printf("SELECT m.imgid"
                            " FROM memory.collected_images as m"
                            " WHERE m.imgid IN (SELECT s.imgid FROM main.selected_images as s)%s",
                            ordering ? " ORDER BY m.rowid DESC" : "");
    // clang-format on
  }
  else
  {
    // we need to get hidden grouped images too, and the
    // selection already contains them, but not in right order
    if(ordering)
    {
      // clang-format off
      query = g_strdup_printf("SELECT DISTINCT ng.id"
                              " FROM (%s) AS ng"
                              " WHERE ng.id IN (SELECT s.imgid FROM main.selected_images as s)",
                              dt_collection_get_query_no_group(dt_selection_get_collection(selection)));
      // clang-format on
    }
    else
    {
      query = g_strdup("SELECT imgid FROM main.selected_images");
    }
  }
  return query;
}

// return a list of all selected imgid
GList *dt_selection_get_list(struct dt_selection_t *selection, const gboolean only_visible, const gboolean ordering)
{
  GList *l = NULL;
  gchar *query = dt_selection_get_list_query(selection, only_visible, ordering);

  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  g_free(query);
  while(stmt != NULL && sqlite3_step(stmt) == SQLITE_ROW)
  {
    l = g_list_prepend(l, GINT_TO_POINTER(sqlite3_column_int(stmt, 0)));
  }
  if(!(only_visible && ordering)) l = g_list_reverse(l);
  if(stmt) sqlite3_finalize(stmt);

  return l;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
