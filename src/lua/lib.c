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
#include "lua/lib.h"
#include "lua/modules.h"
#include "lua/types.h"
#include "gui/gtk.h"

typedef enum
{
  GET_VERSION,
  GET_ID,
  GET_NAME,
  GET_EXPANDABLE,
  GET_EXPANDED,
  GET_POSITION,
  GET_VISIBLE,
  GET_CONTAINER,
  GET_VIEWS,
  LAST_LIB_FIELD
} lib_fields;
static const char *lib_fields_name[] =
{
  "version",
  "id",
  "name",
  "expandable",
  "expanded",
  "position",
  "visible",
  "container",
  "views",
  NULL
};

static int lib_index(lua_State*L)
{
  int index = luaL_checkoption(L,-1,NULL,lib_fields_name);
  dt_lib_module_t * module = *(dt_lib_module_t**)lua_touserdata(L,-2);
  switch(index)
  {
    case GET_VERSION:
      lua_pushinteger(L,module->version());
      return 1;
    case GET_ID:
      lua_pushstring(L,module->plugin_name);
      return 1;
    case GET_NAME:
      lua_pushstring(L,module->name());
      return 1;
    case GET_EXPANDABLE:
      lua_pushboolean(L,module->expandable());
      return 1;
    case GET_EXPANDED:
      if(!module->expandable()) {
        lua_pushboolean(L,true);
      } else {
        lua_pushboolean(L,dt_lib_gui_get_expanded(module));
      }
      return 1;
    case GET_POSITION:
      lua_pushinteger(L,module->position());
      return 1;
    case GET_VISIBLE:
      lua_pushboolean(L,dt_lib_is_visible(module));
      return 1;
    case GET_CONTAINER:
      {
        dt_ui_container_t container;
        container = module->container();
        luaA_push(L,dt_ui_container_t,&container);
        return 1;
      }
    case GET_VIEWS:
      {
        int i;
        lua_newtable(L);
        for(i=0; i<  darktable.view_manager->num_views ; i++) {
          if(darktable.view_manager->view[i].view(&darktable.view_manager->view[i]) & module->views()){
            dt_lua_module_push_entry(L,"view",(darktable.view_manager->view[i].module_name));
            luaL_ref(L,-2);
          }
        }
        return 1;
      }

    default:
      return luaL_error(L,"should never happen %d",index);
  }
}

static int lib_newindex(lua_State*L)
{
  int index = luaL_checkoption(L,2,NULL,lib_fields_name);
  dt_lib_module_t * module = *(dt_lib_module_t**)lua_touserdata(L,1);
  switch(index)
  {
    case GET_EXPANDED:
      dt_lua_unlock(true);
      dt_lib_gui_set_expanded(module,lua_toboolean(L,3));
      dt_lua_lock();
      return 0;
    case GET_VISIBLE:
      dt_lua_unlock(true);
      dt_lib_set_visible(module,lua_toboolean(L,3));
      dt_lua_lock();
      return 0;
    default:
      return luaL_error(L,"unknown index for lib : ",lua_tostring(L,-2));
  }
}

static int lib_tostring(lua_State* L)
{
  dt_lib_module_t * module = *(dt_lib_module_t**)lua_touserdata(L,-1);
  lua_pushstring(L,module->plugin_name);
  return 1;
}

void dt_lua_register_lib(lua_State* L,dt_lib_module_t* module)
{
  dt_lua_register_module_entry_new(L,"lib",module->plugin_name,module);
  int my_type = dt_lua_module_get_entry_typeid(L,"lib",module->plugin_name);
  dt_lua_register_type_callback_inherit_typeid(L,my_type,luaA_type_find("dt_lib_module_t"));
  luaL_getmetatable(L,luaA_type_name(my_type));
  lua_pushcfunction(L,lib_tostring);
  lua_setfield(L,-2,"__tostring");
  lua_pop(L,1);
};

int dt_lua_init_lib(lua_State *L)
{

  dt_lua_init_type(L,dt_lib_module_t);
  dt_lua_register_type_callback_list(L,dt_lib_module_t,lib_index,NULL,lib_fields_name);
  // add a writer to the read/write fields
  dt_lua_register_type_callback(L,dt_lib_module_t,lib_index,lib_newindex, "expanded","visible", NULL) ;

  dt_lua_init_module_type(L,"lib");
  return 0;
}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
