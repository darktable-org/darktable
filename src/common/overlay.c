/*
    This file is part of darktable,
    Copyright (C) 2023-2024 darktable developers.

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

#include <sqlite3.h>

#include "common/debug.h"
#include "common/introspection.h"
#include "common/tags.h"
#include "develop/imageop.h"
#include "overlay.h"

void dt_overlay_record(const dt_imgid_t imgid, const dt_imgid_t overlay_id)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "INSERT INTO overlay (imgid, overlay_id) "
                              "VALUES (?1, ?2)",
                              -1, &stmt, NULL);

  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, overlay_id);

  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  // add a tag with the main image
  guint tagid = 0;
  char tagname[512];
  snprintf(tagname, sizeof(tagname), "darktable|overlay|%d", imgid);
  dt_tag_new(tagname, &tagid);
  dt_tag_attach(tagid, overlay_id, FALSE, FALSE);
}

void dt_overlays_remove(const dt_imgid_t imgid)
{
  GList *overlay = dt_overlay_get_imgs(imgid);

  GList *l = overlay;
  while(l)
  {
    const dt_imgid_t _imgid = GPOINTER_TO_INT(l->data);
    dt_overlay_remove(imgid, _imgid);
    l = g_list_next(l);
  }

  g_list_free(overlay);
}

void dt_overlay_remove(const dt_imgid_t imgid, const dt_imgid_t overlay_id)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "DELETE FROM overlay"
                              " WHERE imgid = ?1 AND overlay_id = ?2",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, overlay_id);

  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  // remove tag with the main image
  char tagname[512];
  snprintf(tagname, sizeof(tagname), "darktable|overlay|%d", imgid);
  dt_tag_detach_by_string(tagname, overlay_id, FALSE, FALSE);
}

GList *dt_overlay_get_imgs(const dt_imgid_t imgid)
{
  sqlite3_stmt *stmt;

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT overlay_id"
                              " FROM overlay"
                              " WHERE imgid = ?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);

  GList *res = NULL;

  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const dt_imgid_t overlay_id = sqlite3_column_int(stmt, 0);
    res = g_list_prepend(res, GINT_TO_POINTER(overlay_id));
  }
  sqlite3_finalize(stmt);
  return res;
}

GList *dt_overlay_get_used_in_imgs(const dt_imgid_t overlay_id,
                                   const gboolean except_self)
{
  sqlite3_stmt *stmt;

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT imgid"
                              " FROM overlay"
                              " WHERE overlay_id = ?1"
                              "   AND imgid != ?2",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, overlay_id);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, except_self ? overlay_id : -1);

  GList *res = NULL;

  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const dt_imgid_t imgid = sqlite3_column_int(stmt, 0);
    res = g_list_prepend(res, GINT_TO_POINTER(imgid));
  }
  sqlite3_finalize(stmt);

  return res;
}

gboolean dt_overlay_used_by(const dt_imgid_t imgid, const dt_imgid_t overlay_id)
{
  sqlite3_stmt *stmt;

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "WITH RECURSIVE cte_overlay (imgid, overlay_id) AS ("
                              " SELECT imgid, overlay_id"
                              " FROM overlay o"
                              " WHERE o.imgid = ?1" // ID of the image we want to use as an overlay; we want to query its overlay tree
                              " UNION"
                              " SELECT o.imgid, o.overlay_id"
                              " FROM overlay o"
                              " JOIN cte_overlay c ON c.overlay_id = o.imgid" // the overlays of the image
                              ")"
                              " SELECT 1 FROM cte_overlay"
                              " WHERE overlay_id = ?2", // ID of the image for which we want to set the other as overlay; it must not appear in the overlay tree
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, overlay_id);

  gboolean result = FALSE;

  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    result = TRUE;
  }
  sqlite3_finalize(stmt);

  return result;
}

void dt_overlay_add_from_history(const dt_imgid_t imgid)
{
  // get all history for module overlay to add added overlay references
  // after a copy/paste or restoring an history (undo / redo)

  const dt_iop_module_so_t *overlay = dt_iop_get_module_so("overlay");
  if(overlay == NULL) return;

  // remove all overlays reference, the found ones will be added
  // just below by looking at the whole history.
  dt_overlays_remove(imgid);

  sqlite3_stmt *stmt;

  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
    "SELECT op_params"
    " FROM main.history"
    " WHERE imgid = ?1"
    "   AND operation = 'overlay'",
    -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);

  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const void *op_params = (void *)sqlite3_column_blob(stmt, 0);

    // get imgid (overlay id) using introspection
    const dt_imgid_t *overlay_id = overlay->get_p(op_params, "imgid");

    if(dt_is_valid_imgid(overlay_id))
    {
      dt_overlay_record(imgid, *overlay_id);

      dt_print(DT_DEBUG_PARAMS,
               "[dt_overlay_add_from_history] "
               "add overlay %d to imgid %d",
               *overlay_id, imgid);
    }
  }
  sqlite3_finalize(stmt);
}

void dt_overlay_remove_from_history(const dt_imgid_t imgid,
                                    const int num)
{
  // get all history above history_end for module overlay to clean-up
  // the overlay reference if any.

  const dt_iop_module_so_t *overlay = dt_iop_get_module_so("overlay");
  if(overlay == NULL) return;

  sqlite3_stmt *stmt;

  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
    "SELECT op_params"
    " FROM main.history"
    " WHERE imgid = ?1"
    "   AND operation = 'overlay'"
    "   AND num >= ?2", -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, num);

  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const void *op_params = (void *)sqlite3_column_blob(stmt, 0);

    // get imgid (overlay id) using introspection
    const dt_imgid_t *overlay_id = overlay->get_p(op_params, "imgid");

    if(dt_is_valid_imgid(overlay_id))
    {
      dt_overlay_remove(imgid, *overlay_id);

      dt_print(DT_DEBUG_PARAMS,
               "[dt_overlay_remove_from_history] "
               "remove overlay %d from imgid %d",
               *overlay_id, imgid);
    }
  }
  sqlite3_finalize(stmt);
}
