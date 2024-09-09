/*
   This file is part of darktable,
   Copyright (C) 2024 darktable developers.

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
#include "lua/call.h"
#include "lua/events.h"
#include <glib.h>
#include <stdlib.h>
#include <string.h>

static int message(lua_State *L)
{
  const char *sender = luaL_checkstring(L, 1);
  const char *receiver = luaL_checkstring(L, 2);
  const char *message = luaL_checkstring(L, 3);

  dt_lua_async_call_alien(dt_lua_event_trigger_wrapper,
      0, NULL, NULL,
      LUA_ASYNC_TYPENAME, "const char*", "inter-script-communication",
      LUA_ASYNC_TYPENAME, "const char*", sender,
      LUA_ASYNC_TYPENAME, "const char*", receiver,
      LUA_ASYNC_TYPENAME, "const char*", message,
      LUA_ASYNC_DONE);

  return 0;
}


int dt_lua_init_util(lua_State *L)
{
  dt_lua_push_darktable_lib(L);
  dt_lua_goto_subtable(L, "util");

  lua_pushcfunction(L, message);
  lua_setfield(L, -2, "message");

  lua_pop(L, 1);


  lua_pushcfunction(L, dt_lua_event_multiinstance_register);
  lua_pushcfunction(L, dt_lua_event_multiinstance_destroy);
  lua_pushcfunction(L, dt_lua_event_multiinstance_trigger);
  dt_lua_event_add(L, "inter-script-communication");

  return 0;
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

