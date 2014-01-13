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
  lua_getmetatable(L,1);
  lua_getfield(L,-1,"__module_type");
  if(strcmp(lua_tostring(L,-1),"format")) {
    return luaL_argerror(L,1,"not a format description");
  }
  lua_pop(L,1);
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
  lua_pushstring(L,"format");
  lua_setfield(L,-2,"__module_type");
  lua_pop(L,1); // pop the metatable
  // add to the table
  dt_lua_push_darktable_lib(L);
  dt_lua_goto_subtable(L,"modules");
  dt_lua_goto_subtable(L,"format");
  lua_getmetatable(L,-1);
  lua_getfield(L,-1,"__luaA_Type");
  luaA_Type format_table_type = luaL_checkint(L,-1);
  lua_pushlightuserdata(L,module);
  lua_pushcclosure(L,get_format_params,1);
  dt_lua_register_type_callback_stack_typeid(L,format_table_type,module->plugin_name);
  lua_pop(L,3);
};


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
  luaL_getmetafield(L,1,"__module_type");
  const char* arg1_type = lua_tostring(L,-1);
  luaL_argcheck(L,!strcmp(arg1_type,"storage"),1,NULL);
  luaL_getmetafield(L,1,"__associated_object");
  dt_imageio_module_storage_t * storage = lua_touserdata(L,-1);
  lua_pop(L,2);

  luaL_getmetafield(L,2,"__module_type");
  const char* arg2_type = lua_tostring(L,-1);
  luaL_argcheck(L,!strcmp(arg2_type,"format"),2,NULL);
  luaL_getmetafield(L,2,"__associated_object");
  dt_imageio_module_format_t * format = lua_touserdata(L,-1);
  lua_pop(L,2);

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
  lua_pushstring(L,"storage");
  lua_setfield(L,-2,"__module_type");
  lua_pop(L,1); // pop the metatable
  // add to the table
  dt_lua_push_darktable_lib(L);
  dt_lua_goto_subtable(L,"modules");
  dt_lua_goto_subtable(L,"storage");
  lua_getmetatable(L,-1);
  lua_getfield(L,-1,"__luaA_Type");
  luaA_Type storage_table_type = luaL_checkint(L,-1);
  lua_pushlightuserdata(L,module);
  lua_pushcclosure(L,get_storage_params,1);
  dt_lua_register_type_callback_stack_typeid(L,storage_table_type,module->plugin_name);
  lua_pop(L,3);

};

int dt_lua_init_modules(lua_State *L)
{

  dt_lua_init_type(L,dt_imageio_module_format_t);
  dt_lua_register_type_callback_list(L,dt_imageio_module_format_t,format_index,NULL,format_fields_name);
  lua_pushcfunction(L,write_image);
  dt_lua_register_type_callback_stack(L,dt_imageio_module_format_t,"write_image");

  dt_lua_init_type(L,dt_imageio_module_storage_t);
  dt_lua_register_type_callback_list(L,dt_imageio_module_storage_t,storage_index,NULL,storage_fields_name);

  dt_lua_push_darktable_lib(L);
  dt_lua_goto_subtable(L,"modules");

  dt_lua_init_singleton(L,"format_table");
  lua_setfield(L,-2,"format");

  dt_lua_init_singleton(L,"module_table");
  lua_setfield(L,-2,"storage");

  lua_pop(L,1);

  return 0;
}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
