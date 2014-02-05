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

typedef enum
{
  GET_ID,
  GET_NAME,
  LAST_VIEW_FIELD
} view_fields;
static const char *view_fields_name[] =
{
  "id",
  "name",
  NULL
};

static int view_index(lua_State*L)
{
  int index = luaL_checkoption(L,-1,NULL,view_fields_name);
  dt_view_t * module = *(dt_view_t**)lua_touserdata(L,-2);
  switch(index)
  {
    case GET_ID:
      lua_pushstring(L,module->module_name);
      return 1;
    case GET_NAME:
      lua_pushstring(L,module->name(module));
      return 1;
    default:
      return luaL_error(L,"should never happen %d",index);
  }
}
/*
static int lib_newindex(lua_State*L)
{
  int index = luaL_checkoption(L,2,NULL,lib_fields_name);
  dt_lib_module_t * module = *(dt_lib_module_t**)lua_touserdata(L,1);
  switch(index)
  {
    default:
      return luaL_error(L,"unknown index for lib : ",lua_tostring(L,-2));
  }
}*/

static int view_tostring(lua_State* L)
{
  dt_view_t * module = *(dt_view_t**)lua_touserdata(L,-1);
  lua_pushstring(L,module->module_name);
  return 1;
}

void dt_lua_register_view(lua_State* L,dt_view_t* module)
{
  dt_lua_register_module_entry_new(L,"view",module->module_name,module);
  int my_type = dt_lua_module_get_entry_typeid(L,"view",module->module_name);
  dt_lua_register_type_callback_inherit_typeid(L,my_type,luaA_type_find("dt_view_t"));
  luaL_getmetatable(L,luaA_type_name(my_type));
  lua_pushcfunction(L,view_tostring);
  lua_setfield(L,-2,"__tostring");
  lua_pop(L,1);
};
int dt_lua_init_view(lua_State *L)
{

  dt_lua_init_type(L,dt_view_t);
  dt_lua_register_type_callback_list(L,dt_view_t,view_index,NULL,view_fields_name);
  // add a writer to the read/write fields
  //dt_lua_register_type_callback(L,dt_view_t,lib_index,lib_newindex,
    //  "expanded","visible", NULL) ;

  dt_lua_init_module_type(L,"view");
  return 0;
}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
