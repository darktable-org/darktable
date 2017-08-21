/*
    This file is part of darktable,
    copyright (c) 2011 henrik andersson.

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
struct dt_database_t *dt_database_init(const char *alternative, const gboolean load_data);
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

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
