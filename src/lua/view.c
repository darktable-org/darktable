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
#include "lua/view.h"
#include "lua/modules.h"
#include "lua/types.h"

static int id_member(lua_State *L) 
{
  dt_view_t * module = *(dt_view_t**)lua_touserdata(L,1);
  lua_pushstring(L,module->module_name);
  return 1;
}

static int name_member(lua_State *L) 
{
  dt_view_t * module = *(dt_view_t**)lua_touserdata(L,1);
      lua_pushstring(L,module->name(module));
  return 1;
}

static int view_tostring(lua_State* L)
{
  dt_view_t * module = *(dt_view_t**)lua_touserdata(L,-1);
  lua_pushstring(L,module->module_name);
  return 1;
}

void dt_lua_register_view(lua_State* L,dt_view_t* module)
{
  dt_lua_register_module_entry_new(L,"view",module->module_name,module);
  int my_type = dt_lua_module_get_entry_type(L,"view",module->module_name);
  dt_lua_type_register_parent_type(L,my_type,luaA_type_find(L,"dt_view_t"));
  luaL_getmetatable(L,luaA_typename(L,my_type));
  lua_pushcfunction(L,view_tostring);
  lua_setfield(L,-2,"__tostring");
  lua_pop(L,1);
};
int dt_lua_init_view(lua_State *L)
{

  dt_lua_init_type(L,dt_view_t);
  lua_pushcfunction(L,id_member);
  dt_lua_type_register_const(L,dt_view_t,"id");
  lua_pushcfunction(L,name_member);
  dt_lua_type_register_const(L,dt_view_t,"name");

  dt_lua_init_module_type(L,"view");
  return 0;
}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
