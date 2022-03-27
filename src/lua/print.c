
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
#include "common/darktable.h"
#include "control/control.h"
#include "lua/lua.h"

static int lua_print(lua_State *L)
{
  const int init_gui = (darktable.gui != NULL);
  if(init_gui)
    dt_control_log("%s", luaL_checkstring(L, -1));
  else
    printf("%s\n", luaL_checkstring(L, -1));

  return 0;
}

static int lua_print_toast(lua_State *L)
{

  const int init_gui = (darktable.gui != NULL);
  if(init_gui)
    dt_toast_log("%s", luaL_checkstring(L, -1));
  else
    printf("%s\n", luaL_checkstring(L, -1));

  return 0;
}

static int lua_print_hinter(lua_State *L)
{

  const int init_gui = (darktable.gui != NULL);
  if(init_gui)
  {

    char msg[256];
    if(snprintf(msg, sizeof(msg), "%s", luaL_checkstring(L, -1)) > 0)
    {
      dt_control_hinter_message(darktable.control, msg);
    }
  }
  else
    printf("%s\n", luaL_checkstring(L, -1));

  return 0;
}


static int lua_print_log(lua_State *L)
{
  dt_print(DT_DEBUG_LUA, "LUA %s\n", luaL_checkstring(L, -1));
  return 0;
}

static int lua_print_error(lua_State *L)
{
  dt_print(DT_DEBUG_LUA, "LUA ERROR %s\n", luaL_checkstring(L, -1));
  return 0;
}

int dt_lua_init_print(lua_State *L)
{
  dt_lua_push_darktable_lib(L);

  lua_pushstring(L, "print");
  lua_pushcfunction(L, &lua_print);
  lua_settable(L, -3);

  lua_pushstring(L, "print_toast");
  lua_pushcfunction(L, &lua_print_toast);
  lua_settable(L, -3);

  lua_pushstring(L, "print_hinter");
  lua_pushcfunction(L, &lua_print_hinter);
  lua_settable(L, -3);

  lua_pushstring(L, "print_log");
  lua_pushcfunction(L, &lua_print_log);
  lua_settable(L, -3);

  lua_pushstring(L, "print_error");
  lua_pushcfunction(L, &lua_print_error);
  lua_settable(L, -3);

  lua_pop(L, 1); // remove the configuration table from the stack
  return 0;
}



// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

