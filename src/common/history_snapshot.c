/*
    This file is part of darktable,
    Copyright (C) 2019-2023 darktable developers.

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

#include "common/history_snapshot.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/history.h"
#include "control/signal.h"

dt_undo_lt_history_t *dt_history_snapshot_item_init(void)
{
  return (dt_undo_lt_history_t *)g_malloc0(sizeof(dt_undo_lt_history_t));
}

void dt_history_snapshot_undo_create(const dt_imgid_t imgid, int *snap_id, int *history_end)
{
  // create history & mask snapshots for imgid, return the snapshot id
  sqlite3_stmt *stmt;
  gboolean all_ok = TRUE;

  dt_lock_image(imgid);

  // get current history end
  *history_end = 0;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT history_end FROM main.images WHERE id=?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);

  if(sqlite3_step(stmt) == SQLITE_ROW)
    *history_end = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);

  // get max snapshot

  *snap_id = 0;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT MAX(id) FROM memory.undo_history WHERE imgid=?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);

  if(sqlite3_step(stmt) == SQLITE_ROW)
    *snap_id = sqlite3_column_int(stmt, 0) + 1;
  sqlite3_finalize(stmt);

  dt_database_start_transaction(darktable.db);

  if(*history_end == 0)
  {
    // insert a dummy undo_histroy to ensure proper snap_id later
    // clang-format off
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "INSERT INTO memory.undo_history"
                                "  VALUES (?1, ?2, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, 0)"
                                , -1, &stmt, NULL);
    // clang-format on
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, *snap_id);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, imgid);
    all_ok = all_ok && (sqlite3_step(stmt) == SQLITE_DONE);

    goto end_create;
  }

  // copy current state into undo_history

  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "INSERT INTO memory.undo_history"
                              "  SELECT ?1, imgid, num, module, operation, op_params,"
                              "         enabled, blendop_params, blendop_version,"
                              "         multi_priority, multi_name, multi_name_hand_edited "
                              "  FROM main.history"
                              "  WHERE imgid=?2", -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, *snap_id);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, imgid);
  all_ok = all_ok && (sqlite3_step(stmt) == SQLITE_DONE);
  sqlite3_finalize(stmt);

  // copy current state into undo_masks_history

  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "INSERT INTO memory.undo_masks_history"
                              "  SELECT ?1, imgid, num, formid, form, name, version,"
                              "         points, points_count, source"
                              "  FROM main.masks_history"
                              "  WHERE imgid=?2", -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, *snap_id);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, imgid);
  all_ok = all_ok && (sqlite3_step(stmt) == SQLITE_DONE);
  sqlite3_finalize(stmt);

  // copy the module order

  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "INSERT INTO memory.undo_module_order"
                              "  SELECT ?1, imgid, version, iop_list"
                              "  FROM main.module_order"
                              "  WHERE imgid=?2", -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, *snap_id);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, imgid);
  all_ok = all_ok && (sqlite3_step(stmt) == SQLITE_DONE);

 end_create:

  sqlite3_finalize(stmt);

  if(all_ok)
    dt_database_release_transaction(darktable.db);
  else
  {
    dt_database_rollback_transaction(darktable.db);
    dt_print(DT_DEBUG_ALWAYS,
             "[dt_history_snapshot_undo_create] fails to create a snapshot for %d\n", imgid);
  }

  dt_unlock_image(imgid);
}

static void _history_snapshot_undo_restore(const dt_imgid_t imgid, const int snap_id, const int history_end)
{
  // restore the given snapshot for imgid
  sqlite3_stmt *stmt;
  gboolean all_ok = TRUE;

  dt_lock_image(imgid);

  dt_database_start_transaction(darktable.db);

  dt_history_delete_on_image_ext(imgid, FALSE);
  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_TAG_CHANGED);

  // if no history end it means the image history was discarded, nothing more to restore
  if(history_end == 0)
  {
    goto end_restore;
  }

  // copy undo_history snapshot back as current history state

  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "INSERT INTO main.history"
                              "  SELECT imgid, num, module, operation, op_params, enabled, "
                              "         blendop_params, blendop_version, multi_priority,"
                              "         multi_name, multi_name_hand_edited "
                              "  FROM memory.undo_history"
                              "  WHERE imgid=?2 AND id=?1", -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, snap_id);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, imgid);
  all_ok &= (sqlite3_step(stmt) == SQLITE_DONE);
  sqlite3_finalize(stmt);

  // copy undo_masks_history snapshot back as current masks_history state

  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "INSERT INTO main.masks_history"
                              "  SELECT imgid, num, formid, form, name, version, "
                              "         points, points_count, source"
                              "  FROM memory.undo_masks_history"
                              "  WHERE imgid=?2 AND id=?1",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, snap_id);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, imgid);
  all_ok &= (sqlite3_step(stmt) == SQLITE_DONE);
  sqlite3_finalize(stmt);

  // restore module order

  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "INSERT INTO main.module_order"
                              "  SELECT imgid, version, iop_list"
                              "  FROM memory.undo_module_order"
                              "  WHERE imgid=?2 AND id=?1", -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, snap_id);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, imgid);
  all_ok &= (sqlite3_step(stmt) == SQLITE_DONE);
  sqlite3_finalize(stmt);

 end_restore:

  // set history end

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "UPDATE main.images SET history_end=?2 WHERE id=?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, history_end);
  all_ok &= (sqlite3_step(stmt) == SQLITE_DONE);
  sqlite3_finalize(stmt);

  if(all_ok)
    dt_database_release_transaction(darktable.db);
  else
  {
    dt_database_rollback_transaction(darktable.db);
    dt_print(DT_DEBUG_ALWAYS,
             "[_history_snapshot_undo_restore] fails to restore a snapshot for %d\n", imgid);
  }
  dt_unlock_image(imgid);

  dt_history_hash_write_from_history(imgid, DT_HISTORY_HASH_CURRENT);
}

static void _clear_undo_snapshot(const dt_imgid_t imgid, const int snap_id)
{
  sqlite3_stmt *stmt;

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "DELETE FROM memory.undo_history WHERE id=?1 AND imgid=?2", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, snap_id);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "DELETE FROM memory.undo_masks_history WHERE id=?1 AND imgid=?2", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, snap_id);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "DELETE FROM memory.undo_module_order WHERE id=?1 AND imgid=?2", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, snap_id);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

void dt_history_snapshot_undo_lt_history_data_free(gpointer data)
{
  dt_undo_lt_history_t *hist = (dt_undo_lt_history_t *)data;

  _clear_undo_snapshot(hist->imgid, hist->after);

  // this is the first element in for this image, it corresponds to the initial status, we can safely remove it now
  if(hist->before == 0)
    _clear_undo_snapshot(hist->imgid, hist->before);

  g_free(hist);
}

void dt_history_snapshot_undo_pop(gpointer user_data, dt_undo_type_t type, dt_undo_data_t data, dt_undo_action_t action, GList **imgs)
{
  if(type == DT_UNDO_LT_HISTORY)
  {
    dt_undo_lt_history_t *hist = (dt_undo_lt_history_t *)data;

    if(action == DT_ACTION_UNDO)
    {
      _history_snapshot_undo_restore(hist->imgid, hist->before, hist->before_history_end);
    }
    else
    {
      _history_snapshot_undo_restore(hist->imgid, hist->after, hist->after_history_end);
    }

    *imgs = g_list_append(*imgs, GINT_TO_POINTER(hist->imgid));
  }
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
