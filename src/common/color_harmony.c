/*
    This file is part of darktable,
    Copyright (C) 2023 darktable developers.

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

#include "common/color_harmony.h"
#include "common/debug.h"

void dt_color_harmony_init(dt_color_harmony_guide_t *layout)
{
  layout->type = DT_COLOR_HARMONY_NONE;
  layout->rotation = 0;
  layout->width = DT_COLOR_HARMONY_WIDTH_NORMAL;
}

void dt_color_harmony_set(const dt_imgid_t imgid,
                          const dt_color_harmony_guide_t layout)
{
  sqlite3_stmt *stmt = NULL;

  if(layout.type == DT_COLOR_HARMONY_NONE)
  {
    DT_DEBUG_SQLITE3_PREPARE_V2
      (dt_database_get(darktable.db),
       "DELETE FROM main.harmony_guide"
       " WHERE imgid = ?1",
       -1, &stmt, NULL);
  }
  else
  {
    DT_DEBUG_SQLITE3_PREPARE_V2
      (dt_database_get(darktable.db),
       "INSERT OR REPLACE INTO main.harmony_guide"
       " (imgid, type, rotation, width)"
       " VALUES (?1, ?2, ?3, ?4)",
       -1, &stmt, NULL);

    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, layout.type);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, layout.rotation);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 4, layout.width);
  }

  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);

  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  // If inserted the proper link with the image table is done
  // by the color_harmony_insert trigger.
}

dt_harmony_guide_id_t dt_color_harmony_get_id(const dt_imgid_t imgid)
{
  dt_harmony_guide_id_t id = -1;

  sqlite3_stmt *stmt = NULL;

  DT_DEBUG_SQLITE3_PREPARE_V2
    (dt_database_get(darktable.db),
     "SELECT imgid"
     " FROM main.harmony_guide"
     " WHERE imgid = ?1",
     -1, &stmt, NULL);

  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);

  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    id = sqlite3_column_int(stmt, 0);
  }

  return id;
}

gboolean dt_color_harmony_get(const dt_imgid_t imgid,
                              dt_color_harmony_guide_t *layout)
{
  sqlite3_stmt *stmt = NULL;

  DT_DEBUG_SQLITE3_PREPARE_V2
    (dt_database_get(darktable.db),
     "SELECT type, rotation, width"
     " FROM main.harmony_guide"
     " WHERE main.harmony_guide.imgid = ?1",
     -1, &stmt, NULL);

  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);

  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    layout->type = sqlite3_column_int(stmt, 0);
    layout->rotation = sqlite3_column_int(stmt, 1);
    layout->width = sqlite3_column_int(stmt, 2);
    return TRUE;
  }
  else
    return FALSE;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
