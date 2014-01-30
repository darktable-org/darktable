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
#include "lua/storage.h"
#include "lua/modules.h"
#include "lua/types.h"
#include "lua/image.h"
#include "control/conf.h"
#include "common/imageio.h"


typedef enum
{
  GET_STORAGE_PLUGIN_NAME,
  GET_STORAGE_NAME,
  GET_WIDTH,
  GET_HEIGHT,
  GET_RECOMMENDED_WIDTH,
  GET_RECOMMENDED_HEIGHT,
  GET_SUPPORTED_FORMAT,
  LAST_STORAGE_FIELD
} storage_fields;
static const char *storage_fields_name[] =
{
  "plugin_name",
  "name",
  "width",
  "height",
  "recommended_width",
  "recommended_height",
  "supports_format",
  NULL
};

static int support_format(lua_State *L)
{
  luaL_argcheck(L,dt_lua_isa(L,1,dt_imageio_module_storage_t),1,"dt_imageio_module_storage_t expected");
  luaL_getmetafield(L,1,"__associated_object");
  dt_imageio_module_storage_t * storage = lua_touserdata(L,-1);
  lua_pop(L,1);

  luaL_argcheck(L,dt_lua_isa(L,2,dt_imageio_module_format_t),2,"dt_imageio_module_format_t expected");
  luaL_getmetafield(L,2,"__associated_object");
  dt_imageio_module_format_t * format = lua_touserdata(L,-1);
  lua_pop(L,1);

  lua_pushboolean(L,storage->supported(storage,format));
  return 1;
}


static int storage_index(lua_State*L)
{
  uint32_t width,height;
  int index = luaL_checkoption(L,-1,NULL,storage_fields_name);
  luaL_getmetafield(L,-2,"__associated_object");
  dt_imageio_module_storage_t * storage = lua_touserdata(L,-1);
  switch(index)
  {
    case GET_STORAGE_PLUGIN_NAME:
      lua_pushstring(L,storage->plugin_name);
      return 1;
    case GET_STORAGE_NAME:
      lua_pushstring(L,storage->name(storage));
      return 1;
    case GET_WIDTH:
      width=0;
      height=0;
      storage->dimension(storage,&width,&height);
      lua_pushinteger(L,width);
      return 1;
    case GET_HEIGHT:
      width=0;
      height=0;
      storage->dimension(storage,&width,&height);
      lua_pushinteger(L,height);
      return 1;
    case GET_RECOMMENDED_WIDTH:
      width = dt_conf_get_int("plugins/lighttable/export/width");
      height = dt_conf_get_int("plugins/lighttable/export/height");
      storage->recommended_dimension(storage,&width,&height);
      lua_pushinteger(L,width);
      return 1;
    case GET_RECOMMENDED_HEIGHT:
      width = dt_conf_get_int("plugins/lighttable/export/width");
      height = dt_conf_get_int("plugins/lighttable/export/height");
      storage->recommended_dimension(storage,&width,&height);
      lua_pushinteger(L,height);
      return 1;
    case GET_SUPPORTED_FORMAT:
      lua_pushcfunction(L,support_format);
      return 1;
    default:
      return luaL_error(L,"should never happen %d",index);
  }
}

static int get_storage_params(lua_State *L)
{
  dt_imageio_module_storage_t *storage_module = lua_touserdata(L,lua_upvalueindex(1));
  dt_imageio_module_data_t *fdata = storage_module->get_params(storage_module);
  if(!fdata)
  {
    // facebook does that if it hasn't been authenticated yet
    lua_pushnil(L);
    return 1;
  }
  luaA_push_typeid(L,storage_module->parameter_lua_type,fdata);
  storage_module->free_params(storage_module,fdata);
  return 1;
}

void dt_lua_register_storage_typeid(lua_State* L, dt_imageio_module_storage_t* module, luaA_Type type_id)
{
  dt_lua_register_type_callback_inherit_typeid(L,type_id,luaA_type_find("dt_imageio_module_storage_t"));
  luaL_getmetatable(L,luaA_type_name(type_id));
  lua_pushlightuserdata(L,module);
  lua_setfield(L,-2,"__associated_object");
  lua_pop(L,1); // pop the metatable
  // add to the table
  lua_pushlightuserdata(L,module);
  lua_pushcclosure(L,get_storage_params,1);
  dt_lua_register_module_entry(L,-1,"storage",module->plugin_name);
  lua_pop(L,1);
};

int dt_lua_init_storage(lua_State *L)
{

  dt_lua_init_type(L,dt_imageio_module_storage_t);
  dt_lua_register_type_callback_list(L,dt_imageio_module_storage_t,storage_index,NULL,storage_fields_name);

  dt_lua_init_module_type(L,"storage");
  return 0;
}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
