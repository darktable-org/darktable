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
#include "lua/preferences.h"
#include "common/pwstorage/pwstorage.h"
#include "control/conf.h"
#include "gui/gtk.h"
#include "lua/call.h"
#include "lua/widget/widget.h"
#include <glib.h>
#include <stdlib.h>
#include <string.h>

static int get_password(lua_State *L)
{
  const char *application = luaL_checkstring(L, 1);
  const char *username = luaL_checkstring(L, 2);
  GHashTable *table = dt_pwstorage_get(application);
  gchar *password = g_strdup(g_hash_table_lookup(table, username));
  g_hash_table_destroy(table);
  lua_pushstring(L, password);
  return 1;
}

static int save_password(lua_State *L)
{
  const char *application = luaL_checkstring(L, 1);
  const char *username = luaL_checkstring(L, 2);
  const char *password = luaL_checkstring(L, 3);
  gboolean result = TRUE;

  GHashTable *table = g_hash_table_new(g_str_hash, g_str_equal);

  g_hash_table_insert(table, (gchar *)username, (gchar *)password);

  if(!dt_pwstorage_set(application, table))
  {
    dt_print(DT_DEBUG_PWSTORAGE, "[%s] cannot store username/token\n", application);
    result = FALSE;
  }

  g_hash_table_destroy(table);
  lua_pushboolean(L, result);
  return 1;
}

int dt_lua_init_password(lua_State *L)
{
  dt_lua_push_darktable_lib(L);
  dt_lua_goto_subtable(L, "password");

  lua_pushcfunction(L, get_password);
  lua_setfield(L, -2, "get");

  lua_pushcfunction(L, save_password);
  lua_setfield(L, -2, "save");

  lua_pop(L, 1);
  return 0;
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

