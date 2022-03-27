/*
    This file is part of darktable,
    Copyright (C) 2013-2020 darktable developers.

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
#include <gio/gio.h>

typedef struct dt_dbus_t
{
  int connected;

  GDBusNodeInfo *introspection_data;
  guint owner_id;
  guint registration_id;

  // used for client actions on the bus
  GDBusConnection *dbus_connection;
} dt_dbus_t;

/** allocates and initializes dbus */
dt_dbus_t *dt_dbus_init();

/** closes down database and frees memory */
void dt_dbus_destroy(const dt_dbus_t *);

/** have we managed to get the dbus name? when not, then there is already another instance of darktable
 * running */
gboolean dt_dbus_connected(const dt_dbus_t *);

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

