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


#define dt_lua_register_module_member(L,storage,struct_type,member,member_type) \
  luaA_struct_member_typeid(L,storage->parameter_lua_type,#member,luaA_type_id(member_type),offsetof(struct_type,member))

int dt_lua_init_modules(lua_State *L);

void dt_lua_init_module_type(lua_State *L,const char* module_type_name);

/// entry handling
void dt_lua_register_module_entry(lua_State *L, int index, const char* module_type_name,const char* entry_name);
void dt_lua_register_module_entry_new(lua_State *L, const char* module_type_name,const char* entry_name,void *entry);
void dt_lua_module_push_entry(lua_State *L, const char* module_type_name,const char* entry_name);
luaA_Type dt_lua_module_get_entry_typeid(lua_State *L, const char* module_type_name,const char* entry_name);

/// preset handling
#define dt_lua_register_module_presets(L,module,entry,type) \
  dt_lua_register_module_presets_typeid(L,module,entry,luaA_type_id(type))
void dt_lua_register_module_presets_typeid(lua_State*L, const char* module_type_name,const char* entry_name,luaA_Type preset_typeid);
luaA_Type dt_lua_module_get_preset_typeid(lua_State *L, const char* module_type_name,const char* entry_name);
void dt_lua_register_current_preset(lua_State*L, const char* module_type_name, const char*entry_name, lua_CFunction pusher, lua_CFunction getter);


#endif
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
