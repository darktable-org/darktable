/*
    This file is part of darktable,
    Copyright (C) 2015-2020 darktable developers.

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

#include <libsoup/soup.h>

typedef gboolean (*dt_http_server_callback)(GHashTable *query, gpointer user_data);

typedef struct dt_http_server_t
{
  SoupServer *server;
  char *url;
} dt_http_server_t;

/** create a new http server, listening on one of the ports and using id as its path.
 *  the final url can be taken from the returned struct.
 *  when a connection is made the callback is called.
 */
dt_http_server_t *dt_http_server_create(const int *ports, const int n_ports, const char *id,
                                        const dt_http_server_callback callback, gpointer user_data);

/** call this to kill a server manually. don't call this if the request was received.
 *  this also frees server.
 */
void dt_http_server_kill(dt_http_server_t *server);

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
