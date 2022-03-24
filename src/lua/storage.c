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
#include "lua/storage.h"
#include "common/imageio.h"
#include "control/conf.h"
#include "lua/image.h"
#include "lua/modules.h"
#include "lua/types.h"

static int supports_format(lua_State *L)
{
  luaL_argcheck(L, dt_lua_isa(L, 1, dt_imageio_module_storage_t), 1, "dt_imageio_module_storage_t expected");
  luaL_getmetafield(L, 1, "__associated_object");
  dt_imageio_module_storage_t *storage = lua_touserdata(L, -1);
  lua_pop(L, 1);

  luaL_argcheck(L, dt_lua_isa(L, 2, dt_imageio_module_format_t), 2, "dt_imageio_module_format_t expected");
  luaL_getmetafield(L, 2, "__associated_object");
  dt_imageio_module_format_t *format = lua_touserdata(L, -1);
  lua_pop(L, 1);

  lua_pushboolean(L, storage->supported(storage, format));
  return 1;
}

static int plugin_name_member(lua_State *L)
{
  luaL_getmetafield(L, 1, "__associated_object");
  dt_imageio_module_storage_t *storage = lua_touserdata(L, -1);
  lua_pushstring(L, storage->plugin_name);
  return 1;
}

static int name_member(lua_State *L)
{
  luaL_getmetafield(L, 1, "__associated_object");
  dt_imageio_module_storage_t *storage = lua_touserdata(L, -1);
  lua_pushstring(L, storage->name(storage));
  return 1;
}

static int width_member(lua_State *L)
{
  luaL_getmetafield(L, 1, "__associated_object");
  dt_imageio_module_storage_t *storage = lua_touserdata(L, -1);
  dt_imageio_module_data_t *data = lua_touserdata(L, 1);
  uint32_t width, height;
  width = 0;
  height = 0;
  storage->dimension(storage, data, &width, &height);
  lua_pushinteger(L, width);
  return 1;
}

static int height_member(lua_State *L)
{
  luaL_getmetafield(L, 1, "__associated_object");
  dt_imageio_module_storage_t *storage = lua_touserdata(L, -1);
  dt_imageio_module_data_t *data = lua_touserdata(L, 1);
  uint32_t width, height;
  width = 0;
  height = 0;
  storage->dimension(storage, data, &width, &height);
  lua_pushinteger(L, height);
  return 1;
}

static int recommended_width_member(lua_State *L)
{
  luaL_getmetafield(L, 1, "__associated_object");
  dt_imageio_module_storage_t *storage = lua_touserdata(L, -1);
  dt_imageio_module_data_t *data = lua_touserdata(L, 1);
  uint32_t width, height;
  width = dt_conf_get_int("plugins/lighttable/export/width");
  height = dt_conf_get_int("plugins/lighttable/export/height");
  storage->recommended_dimension(storage, data, &width, &height);
  lua_pushinteger(L, width);
  return 1;
}

static int recommended_height_member(lua_State *L)
{
  luaL_getmetafield(L, 1, "__associated_object");
  dt_imageio_module_storage_t *storage = lua_touserdata(L, -1);
  dt_imageio_module_data_t *data = lua_touserdata(L, 1);
  uint32_t width, height;
  width = dt_conf_get_int("plugins/lighttable/export/width");
  height = dt_conf_get_int("plugins/lighttable/export/height");
  storage->recommended_dimension(storage, data, &width, &height);
  lua_pushinteger(L, height);
  return 1;
}



static int get_storage_params(lua_State *L)
{
  dt_imageio_module_storage_t *storage_module = lua_touserdata(L, lua_upvalueindex(1));
  dt_imageio_module_data_t *fdata = storage_module->get_params(storage_module);
  if(!fdata)
  {
    // facebook does that if it hasn't been authenticated yet
    lua_pushnil(L);
    return 1;
  }
  luaA_push_type(L, storage_module->parameter_lua_type, fdata);
  storage_module->free_params(storage_module, fdata);
  return 1;
}

void dt_lua_register_storage_type(lua_State *L, dt_imageio_module_storage_t *module, luaA_Type type_id)
{
  dt_lua_type_register_parent_type(L, type_id, luaA_type_find(L, "dt_imageio_module_storage_t"));
  lua_pushlightuserdata(L, module);
  dt_lua_type_setmetafield_type(L,type_id,"__associated_object");
  // add to the table
  lua_pushlightuserdata(L, module);
  lua_pushcclosure(L, get_storage_params, 1);
  dt_lua_module_entry_new(L, -1, "storage", module->plugin_name);
  lua_pop(L, 1);
};

static int new_storage(lua_State *L)
{
  const char *entry_name = luaL_checkstring(L, 1);
  dt_lua_module_entry_push(L, "storage", entry_name);
  lua_call(L, 0, 1);
  return 1;
}

int dt_lua_init_early_storage(lua_State *L)
{

  dt_lua_init_type(L, dt_imageio_module_storage_t);
  lua_pushcfunction(L, plugin_name_member);
  dt_lua_type_register(L, dt_imageio_module_storage_t, "plugin_name");
  lua_pushcfunction(L, name_member);
  dt_lua_type_register(L, dt_imageio_module_storage_t, "name");
  lua_pushcfunction(L, width_member);
  dt_lua_type_register(L, dt_imageio_module_storage_t, "width");
  lua_pushcfunction(L, height_member);
  dt_lua_type_register(L, dt_imageio_module_storage_t, "height");
  lua_pushcfunction(L, recommended_width_member);
  dt_lua_type_register(L, dt_imageio_module_storage_t, "recommended_width");
  lua_pushcfunction(L, recommended_height_member);
  dt_lua_type_register(L, dt_imageio_module_storage_t, "recommended_height");

  lua_pushcfunction(L, supports_format);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const(L, dt_imageio_module_storage_t, "supports_format");

  dt_lua_module_new(L, "storage");

  dt_lua_push_darktable_lib(L);
  lua_pushstring(L, "new_storage");
  lua_pushcfunction(L, &new_storage);
  lua_settable(L, -3);
  lua_pop(L, 1);
  return 0;
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

