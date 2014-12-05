/*
    This file is part of darktable,
    copyright (c) 2013 Jeremy Rosen

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
#ifndef DT_LUA_MODULES_H
#define DT_LUA_MODULES_H
#include <lua/lua.h>
#include <common/imageio_module.h>


#define dt_lua_register_module_member(L, storage, struct_type, member, member_type)                          \
  luaA_struct_member_type(L, storage->parameter_lua_type, #member, luaA_type(L, member_type),                \
                          offsetof(struct_type, member))


// define a new module type
void dt_lua_module_new(lua_State *L, const char *module_type_name);
// get the singleton object that represent this module type
void dt_lua_module_push(lua_State *L, const char *module_type_name);


/// create a new entry into the module, the object to be the entry is taken from the index)
void dt_lua_module_entry_new(lua_State *L, int index, const char *module_type_name, const char *entry_name);
/// create a new entry into the module, a singleton is created for you that contains entry
void dt_lua_module_entry_new_singleton(lua_State *L, const char *module_type_name, const char *entry_name,
                                       void *entry);
/// get the singleton reprensenting an entry
void dt_lua_module_entry_push(lua_State *L, const char *module_type_name, const char *entry_name);
/// get the type of an entry
luaA_Type dt_lua_module_entry_get_type(lua_State *L, const char *module_type_name, const char *entry_name);

/// preset handling
#define dt_lua_register_module_presets(L, module, entry, type)                                               \
  dt_lua_register_module_presets_type(L, module, entry, luaA_type_id(type))
void dt_lua_register_module_presets_type(lua_State *L, const char *module_type_name, const char *entry_name,
                                         luaA_Type preset_type);
luaA_Type dt_lua_module_get_preset_type(lua_State *L, const char *module_type_name, const char *entry_name);
void dt_lua_register_current_preset(lua_State *L, const char *module_type_name, const char *entry_name,
                                    lua_CFunction pusher, lua_CFunction getter);


int dt_lua_init_early_modules(lua_State *L);
#endif
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
