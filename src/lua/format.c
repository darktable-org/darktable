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

static int plugin_name_member(lua_State*L) {
  luaL_getmetafield(L,1,"__associated_object");
  dt_imageio_module_format_t * format = lua_touserdata(L,-1);
  lua_pushstring(L,format->plugin_name);
  return 1;
}

static int name_member(lua_State*L) {
  luaL_getmetafield(L,1,"__associated_object");
  dt_imageio_module_format_t * format = lua_touserdata(L,-1);
  lua_pushstring(L,format->name());
  return 1;
}

static int extension_member(lua_State*L) {
  luaL_getmetafield(L,1,"__associated_object");
  dt_imageio_module_format_t * format = lua_touserdata(L,-1);
  dt_imageio_module_data_t * data = lua_touserdata(L,1);
  lua_pushstring(L,format->extension(data));
  return 1;
}

static int mime_member(lua_State*L) {
  luaL_getmetafield(L,1,"__associated_object");
  dt_imageio_module_format_t * format = lua_touserdata(L,-1);
  dt_imageio_module_data_t * data = lua_touserdata(L,1);
  lua_pushstring(L,format->mime(data));
  return 1;
}

static int max_width_member(lua_State*L) {
  luaL_getmetafield(L,1,"__associated_object");
  dt_imageio_module_format_t * format = lua_touserdata(L,-1);
  uint32_t width,height;
  width=0;
  height=0;
  format->dimension(format,&width,&height);
  lua_pushinteger(L,width);
  return 1;
}

static int max_height_member(lua_State*L) {
  uint32_t width,height;
  luaL_getmetafield(L,1,"__associated_object");
  dt_imageio_module_format_t * format = lua_touserdata(L,-1);

  width=0;
  height=0;
  format->dimension(format,&width,&height);
  lua_pushinteger(L,height);
  return 1;
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
  luaL_argcheck(L,dt_lua_isa(L,1,dt_imageio_module_format_t),-1,"dt_imageio_module_format_t expected");

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
  dt_lua_type_register_parent_typeid(L,type_id,luaA_type_find("dt_imageio_module_format_t"));
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
  lua_pushcfunction(L,plugin_name_member);
  dt_lua_type_register_const(L,dt_imageio_module_format_t,"plugin_name");
  lua_pushcfunction(L,name_member);
  dt_lua_type_register_const(L,dt_imageio_module_format_t,"name");
  lua_pushcfunction(L,extension_member);
  dt_lua_type_register_const(L,dt_imageio_module_format_t,"extension");
  lua_pushcfunction(L,mime_member);
  dt_lua_type_register_const(L,dt_imageio_module_format_t,"mime");
  lua_pushcfunction(L,max_width_member);
  dt_lua_type_register_const(L,dt_imageio_module_format_t,"max_width");
  lua_pushcfunction(L,max_height_member);
  dt_lua_type_register_const(L,dt_imageio_module_format_t,"max_height");
  lua_pushcfunction(L,write_image);
  lua_pushcclosure(L,dt_lua_type_member_common,1);
  dt_lua_type_register_const(L,dt_imageio_module_format_t,"write_image");

  dt_lua_init_module_type(L,"format");
  return 0;
}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
