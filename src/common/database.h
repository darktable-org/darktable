/*
    This file is part of darktable,
    Copyright (C) 2011-2020 darktable developers.

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

#pragma once

#include <glib.h>

struct dt_database_t;

/** allocates and initializes database */
struct dt_database_t *dt_database_init(const char *alternative, const gboolean load_data, const gboolean has_gui);
/** closes down database and frees memory */
void dt_database_destroy(const struct dt_database_t *);
/** get handle */
struct sqlite3 *dt_database_get(const struct dt_database_t *);
/** Returns database path */
const gchar *dt_database_get_path(const struct dt_database_t *db);
/** test if database was already locked by another instance */
gboolean dt_database_get_lock_acquired(const struct dt_database_t *db);
/** show an error popup. this has to be postponed until after we tried using dbus to reach another instance */
void dt_database_show_error(const struct dt_database_t *db);
/** perform pre-db-close optimizations (always call when quiting darktable) */
void dt_database_optimize(const struct dt_database_t *);
/** conditionally perfrom db maintenance */
gboolean dt_database_maybe_maintenance(const struct dt_database_t *db);
void dt_database_perform_maintenance(const struct dt_database_t *db);
/** cleanup busy statements on closing dt, just before performing maintenance */
void dt_database_cleanup_busy_statements(const struct dt_database_t *db);
/** simply create db snapshot of both library and data */
gboolean dt_database_snapshot(const struct dt_database_t *db);
/** check if creating database snapshot is recommended */
gboolean dt_database_maybe_snapshot(const struct dt_database_t *db);
/** get list of snapshot files to remove after successful snapshot */
char **dt_database_snaps_to_remove(const struct dt_database_t *db);
/** get possibly the freshest snapshot to restore */
gchar *dt_database_get_most_recent_snap(const char* db_filename);


// nested transactions support

void dt_database_start_transaction(const struct dt_database_t *db);
void dt_database_release_transaction(const struct dt_database_t *db);
void dt_database_rollback_transaction(const struct dt_database_t *db);

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

