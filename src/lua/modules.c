/*
   This file is part of darktable,
   copyright (c) 2012 Jeremy Rosen

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
#include "lua/modules.h"
#include "lua/types.h"
#include "lua/image.h"
#include "control/conf.h"
#include "common/imageio.h"


void dt_lua_init_module_type(lua_State *L,const char* module_type_name)
{
  dt_lua_push_darktable_lib(L);
  dt_lua_goto_subtable(L,"modules");

  dt_lua_init_singleton(L,module_type_name,NULL);
  lua_setfield(L,-2,module_type_name);
  lua_pop(L,1);

}

void dt_lua_register_module_entry_new(lua_State *L, const char* module_type_name,const char* entry_name,void *entry)
{
  char tmp_string[1024];
  snprintf(tmp_string, sizeof(tmp_string),"module_%s_%s",module_type_name,entry_name);
  dt_lua_init_singleton(L,tmp_string,entry);
  dt_lua_push_darktable_lib(L);
  dt_lua_goto_subtable(L,"modules");
  dt_lua_goto_subtable(L,module_type_name);
  lua_getmetatable(L,-1);
  lua_getfield(L,-1,"__luaA_Type");
  luaA_Type table_type = luaL_checkint(L,-1);
  lua_pop(L,3);
  dt_lua_register_type_callback_stack_typeid(L,table_type,entry_name);
}

void dt_lua_register_module_entry(lua_State *L, int index, const char* module_type_name,const char* entry_name)
{
  dt_lua_push_darktable_lib(L);
  dt_lua_goto_subtable(L,"modules");
  dt_lua_goto_subtable(L,module_type_name);
  lua_getmetatable(L,-1);
  lua_getfield(L,-1,"__luaA_Type");
  luaA_Type table_type = luaL_checkint(L,-1);
  lua_pop(L,3);
  lua_pushvalue(L,index);
  dt_lua_register_type_callback_stack_typeid(L,table_type,entry_name);
}

void dt_lua_module_push_entry(lua_State *L, const char* module_type_name,const char* entry_name)
{
  dt_lua_push_darktable_lib(L);
  dt_lua_goto_subtable(L,"modules");
  dt_lua_goto_subtable(L,module_type_name);
  lua_getfield(L,-1,entry_name);
  lua_remove(L,-2);
}


luaA_Type dt_lua_module_get_entry_typeid(lua_State *L, const char* module_type_name,const char* entry_name)
{
  dt_lua_module_push_entry(L,module_type_name,entry_name);
  lua_getmetatable(L,-1);
  lua_getfield(L,-1,"__luaA_Type");
  luaA_Type entry_type = luaL_checkint(L,-1);
  lua_pop(L,3);
  return entry_type;

}

void dt_lua_register_module_presets_typeid(lua_State*L, const char* module_type_name,const char* entry_name,luaA_Type preset_typeid)
{
  dt_lua_module_push_entry(L,module_type_name,entry_name);
  lua_getmetatable(L,-1);

  lua_pushinteger(L,preset_typeid);
  lua_setfield(L,-2,"__preset_type");
  lua_pop(L,2);

}

luaA_Type dt_lua_module_get_preset_typeid(lua_State *L, const char* module_type_name,const char* entry_name)
{
  dt_lua_module_push_entry(L,module_type_name,entry_name);
  lua_getmetatable(L,-1);
  lua_getfield(L,-1,"__preset_type");
  luaA_Type entry_type = luaL_checkint(L,-1);
  lua_pop(L,3);
  return entry_type;

}
void dt_lua_register_current_preset(lua_State*L, const char* module_type_name, const char*entry_name, lua_CFunction pusher, lua_CFunction getter) {
  // stack usefull values
  dt_lua_module_push_entry(L,module_type_name,entry_name);
  void * entry =  *(void**)lua_touserdata(L,-1);
  luaA_Type entry_type = dt_lua_module_get_entry_typeid(L,module_type_name,entry_name);
  lua_pop(L,1);

  char tmp_string[1024];
  snprintf(tmp_string, sizeof(tmp_string),"module_current_settings_%s_%s",module_type_name,entry_name);
  dt_lua_init_wrapped_singleton(L,pusher,getter,tmp_string,entry);
  dt_lua_register_type_callback_stack_typeid(L,entry_type,"settings");
}



int dt_lua_init_modules(lua_State *L)
{
  return 0;
}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
