/*
    This file is part of darktable,
    copyright (c) 2019 philippe weyland.

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

#include "common/darktable.h"
#include "common/debug.h"
#include "common/tags.h"
#include "common/metadata_export.h"
#include "control/conf.h"
#include "control/control.h"
#include "dtgtk/button.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include "libs/lib_api.h"
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif
#include <gdk/gdkkeysyms.h>
#include <math.h>

// only difference with dt_lib_presets_add is writeprotect = 0
void dt_lib_export_metadata_presets_add(const char *name, const char *plugin_name, const int32_t version, const void *params,
                        const int32_t params_size)
{
  sqlite3_stmt *stmt;
  printf("lib1056 delete preset %s %s\n", plugin_name, name);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "DELETE FROM data.presets WHERE name=?1 AND operation=?2 AND op_version=?3", -1,
                              &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, name, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, plugin_name, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, version);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get(darktable.db),
      "INSERT INTO data.presets (name, description, operation, op_version, op_params, "
      "blendop_params, blendop_version, enabled, model, maker, lens, "
      "iso_min, iso_max, exposure_min, exposure_max, aperture_min, aperture_max, "
      "focal_length_min, focal_length_max, writeprotect, "
      "autoapply, filter, def, format) VALUES (?1, '', ?2, ?3, ?4, NULL, 0, 1, '%', "
      "'%', '%', 0, 340282346638528859812000000000000000000, 0, 10000000, 0, 100000000, 0, 1000, 0, 0, 0, 0, 0)",
      -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, name, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, plugin_name, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, version);
  DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 4, params, params_size, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

GList *dt_lib_export_metadata_get_presets_list()
{
  GList *result = NULL;

  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
    "SELECT name "
    "FROM data.presets "
    "WHERE operation='export_metadata'",
     -1, &stmt, NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    char *presets_name = g_strdup((char *)sqlite3_column_text(stmt, 0));
    result = g_list_append(result, presets_name);
  }
  sqlite3_finalize(stmt);

  return result;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
