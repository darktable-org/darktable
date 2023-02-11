/*
   This file is part of darktable,
   Copyright (C) 2013-2023 darktable developers.

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

#include "control/conf.h"
#include "imageio/imageio_common.h"
#include "lua/image.h"
#include "lua/modules.h"
#include "lua/types.h"


void dt_lua_module_new(lua_State *L, const char *module_type_name)
{
  dt_lua_init_singleton(L, module_type_name, NULL);

  lua_getfield(L, LUA_REGISTRYINDEX, "dt_lua_modules");
  lua_pushvalue(L, -2);
  lua_setfield(L, -2, module_type_name);
  lua_pop(L, 1);

  lua_pop(L, 1);
}


void dt_lua_module_push(lua_State *L, const char *module_type_name)
{
  lua_getfield(L, LUA_REGISTRYINDEX, "dt_lua_modules");
  lua_getfield(L, -1, module_type_name);
  lua_remove(L, -2);
}


void dt_lua_module_entry_new_singleton(lua_State *L, const char *module_type_name, const char *entry_name,
                                       void *entry)
{
  char tmp_string[1024];
  snprintf(tmp_string, sizeof(tmp_string), "module_%s_%s", module_type_name, entry_name);
  dt_lua_init_singleton(L, tmp_string, entry);
  dt_lua_module_entry_new(L, -1, module_type_name, entry_name);
  lua_pop(L, 1);
}

void dt_lua_module_entry_new(lua_State *L, int index, const char *module_type_name, const char *entry_name)
{
  dt_lua_module_push(L, module_type_name);

  lua_getmetatable(L, -1);
  lua_getfield(L, -1, "__luaA_Type");
  luaA_Type table_type = luaL_checkinteger(L, -1);
  lua_pop(L, 3);
  lua_pushvalue(L, index);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, table_type, entry_name);
}

void dt_lua_module_entry_push(lua_State *L, const char *module_type_name, const char *entry_name)
{
  dt_lua_module_push(L, module_type_name);
  lua_getfield(L, -1, entry_name);
  lua_remove(L, -2);
}


luaA_Type dt_lua_module_entry_get_type(lua_State *L, const char *module_type_name, const char *entry_name)
{
  dt_lua_module_entry_push(L, module_type_name, entry_name);
  lua_getmetatable(L, -1);
  lua_getfield(L, -1, "__luaA_Type");
  luaA_Type entry_type = luaL_checkinteger(L, -1);
  lua_pop(L, 3);
  return entry_type;
}

void dt_lua_register_module_presets_type(lua_State *L, const char *module_type_name, const char *entry_name,
                                         luaA_Type preset_type)
{
  dt_lua_module_entry_push(L, module_type_name, entry_name);
  lua_getmetatable(L, -1);

  lua_pushinteger(L, preset_type);
  lua_setfield(L, -2, "__preset_type");
  lua_pop(L, 2);
}

luaA_Type dt_lua_module_get_preset_type(lua_State *L, const char *module_type_name, const char *entry_name)
{
  dt_lua_module_entry_push(L, module_type_name, entry_name);
  lua_getmetatable(L, -1);
  lua_getfield(L, -1, "__preset_type");
  luaA_Type entry_type = luaL_checkinteger(L, -1);
  lua_pop(L, 3);
  return entry_type;
}
void dt_lua_register_current_preset(lua_State *L, const char *module_type_name, const char *entry_name,
                                    lua_CFunction pusher, lua_CFunction getter)
{
  // stack useful values
  dt_lua_module_entry_push(L, module_type_name, entry_name);
  void *entry = *(void **)lua_touserdata(L, -1);
  luaA_Type entry_type = dt_lua_module_entry_get_type(L, module_type_name, entry_name);
  lua_pop(L, 1);

  char tmp_string[1024];
  snprintf(tmp_string, sizeof(tmp_string), "module_current_settings_%s_%s", module_type_name, entry_name);
  dt_lua_init_wrapped_singleton(L, pusher, getter, tmp_string, entry);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, entry_type, "settings");
}



int dt_lua_init_early_modules(lua_State *L)
{
  lua_newtable(L);
  lua_setfield(L, LUA_REGISTRYINDEX, "dt_lua_modules");
  return 0;
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
