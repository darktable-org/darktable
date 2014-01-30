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
typedef enum
{
  GET_FORMAT_PLUGIN_NAME,
  GET_FORMAT_NAME,
  GET_EXTENSION,
  GET_MIME,
  GET_MAX_WIDTH,
  GET_MAX_HEIGHT,
  LAST_FORMAT_FIELD
} format_fields;
static const char *format_fields_name[] =
{
  "plugin_name",
  "name",
  "extension",
  "mime",
  "max_width",
  "max_height",
  NULL
};

static int format_index(lua_State*L)
{
  uint32_t width,height;
  int index = luaL_checkoption(L,-1,NULL,format_fields_name);
  luaL_getmetafield(L,-2,"__associated_object");
  dt_imageio_module_format_t * format = lua_touserdata(L,-1);
  dt_imageio_module_data_t * data = lua_touserdata(L,-3);
  switch(index)
  {
    case GET_FORMAT_PLUGIN_NAME:
      lua_pushstring(L,format->plugin_name);
      return 1;
    case GET_FORMAT_NAME:
      lua_pushstring(L,format->name());
      return 1;
    case GET_EXTENSION:
      lua_pushstring(L,format->extension(data));
      return 1;
    case GET_MIME:
      lua_pushstring(L,format->mime(data));
      return 1;
    case GET_MAX_WIDTH:
      width=0;
      height=0;
      format->dimension(format,&width,&height);
      lua_pushinteger(L,width);
      return 1;
    case GET_MAX_HEIGHT:
      width=0;
      height=0;
      format->dimension(format,&width,&height);
      lua_pushinteger(L,height);
      return 1;
    default:
      return luaL_error(L,"should never happen %d",index);
  }
}

static int get_format_params(lua_State *L)
{
  dt_imageio_module_format_t *format_module = lua_touserdata(L,lua_upvalueindex(1));
  dt_imageio_module_data_t *fdata = format_module->get_params(format_module);
  luaA_push_typeid(L,format_module->parameter_lua_type,fdata);
  format_module->free_params(format_module,fdata);
  return 1;
}

static int write_image(lua_State *L)
{
  /* check that param 1 is a module_format_t */
  luaL_argcheck(L,dt_lua_isa(L,-1,dt_imageio_module_format_t),-1,"dt_imageio_module_format_t expected");

  lua_getmetatable(L,1);
  lua_getfield(L,-1,"__luaA_Type");
  luaA_Type format_type = luaL_checkint(L,-1);
  lua_pop(L,1);
  lua_getfield(L,-1,"__associated_object");
  dt_imageio_module_format_t * format = lua_touserdata(L,-1);
  lua_pop(L,2);
  dt_imageio_module_data_t* fdata = format->get_params(format);
  luaA_to_typeid(L,format_type,fdata,1);

  /* check that param 2 is an image */
  dt_lua_image_t imgid;
  luaA_to(L,dt_lua_image_t,&imgid,2);

  /* check that param 3 is a string (filename) */
  const char * filename = luaL_checkstring(L,3);


  dt_lua_unlock(false);
  gboolean high_quality = dt_conf_get_bool("plugins/lighttable/export/high_quality_processing");
  gboolean result = dt_imageio_export(imgid,filename,format,fdata,high_quality,FALSE,NULL,NULL);
  dt_lua_lock();
  lua_pushboolean(L,result);
  format->free_params(format,fdata);
  return 1;
}

void dt_lua_register_format_typeid(lua_State* L, dt_imageio_module_format_t* module, luaA_Type type_id)
{
  dt_lua_register_type_callback_inherit_typeid(L,type_id,luaA_type_find("dt_imageio_module_format_t"));
  luaL_getmetatable(L,luaA_type_name(type_id));
  lua_pushlightuserdata(L,module);
  lua_setfield(L,-2,"__associated_object");
  lua_pop(L,1); // pop the metatable
  // add to the table
  lua_pushlightuserdata(L,module);
  lua_pushcclosure(L,get_format_params,1);
  dt_lua_register_module_entry(L,-1,"format",module->plugin_name);
  lua_pop(L,1);
};


int dt_lua_init_format(lua_State *L)
{

  dt_lua_init_type(L,dt_imageio_module_format_t);
  dt_lua_register_type_callback_list(L,dt_imageio_module_format_t,format_index,NULL,format_fields_name);
  lua_pushcfunction(L,write_image);
  dt_lua_register_type_callback_stack(L,dt_imageio_module_format_t,"write_image");

  dt_lua_init_module_type(L,"format");
  return 0;
}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
