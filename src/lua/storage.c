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
#include "lua/image.h"
#include "lua/dt_lua.h"
#include <stdio.h>
#include <common/darktable.h>
#include "common/imageio_module.h"
#include <glib.h>

static const char* name_wrapper(const struct dt_imageio_module_storage_t *self)
{
  lua_State *L =darktable.lua_state;
  lua_getfield(L,LUA_REGISTRYINDEX,"dt_lua_storages");
  lua_getfield(L,-1,self->plugin_name);
  lua_getfield(L,-1,"name");
  const char* result = lua_tostring(L,-1);
  lua_pop(L,3);
  return result;
}
static  void empty_wrapper(struct dt_imageio_module_storage_t *self){}; 
static int default_supported_wrapper    (struct dt_imageio_module_storage_t *self, struct dt_imageio_module_format_t *format){return true;};
static int default_dimension_wrapper    (struct dt_imageio_module_storage_t *self, uint32_t *width, uint32_t *height){return 0;};

static int store_wrapper(struct dt_imageio_module_storage_t *self,struct dt_imageio_module_data_t *self_data, const int imgid, dt_imageio_module_format_t *format, dt_imageio_module_data_t *fdata, const int num, const int total, const gboolean high_quality)
{
  lua_State *L =darktable.lua_state;

  lua_getfield(L,LUA_REGISTRYINDEX,"dt_lua_storages");
  lua_getfield(L,-1,self->plugin_name);
  lua_getfield(L,-1,"store");

  luaA_push_typeid(L,self->parameter_lua_type,self_data);
  dt_lua_image_push(L,imgid);
  luaA_push_typeid(L,format->parameter_lua_type,fdata);
  lua_pushnumber(L,num);
  lua_pushnumber(L,total);
  lua_pushboolean(L,high_quality);
printf("%s %d\n",__FUNCTION__,__LINE__);
for(int i=1 ;i<=lua_gettop(L);i++) {printf("\t%d:%s %s\n",i,lua_typename(L,lua_type(L,i)),luaL_tolstring(L,i,NULL));lua_pop(L,1);}
  dt_lua_do_chunk(L,0,6,1);
  int result = lua_toboolean(L,-1);
  lua_pop(L,2);
  return result;

}
extern int finalize_store_wrapper (struct dt_imageio_module_storage_t *self, dt_imageio_module_data_t *data)
{
  lua_State *L =darktable.lua_state;

  lua_getfield(L,LUA_REGISTRYINDEX,"dt_lua_storages");
  lua_getfield(L,-1,self->plugin_name);
  lua_getfield(L,-1,"finalize_store");
  luaA_push_typeid(L,self->parameter_lua_type,data);
printf("%s %d\n",__FUNCTION__,__LINE__);
for(int i=1 ;i<=lua_gettop(L);i++) {printf("\t%d:%s %s\n",i,lua_typename(L,lua_type(L,i)),luaL_tolstring(L,i,NULL));lua_pop(L,1);}
  dt_lua_do_chunk(L,0,1,1);
  int result = lua_toboolean(L,-1);
  lua_pop(L,2);
  return result;
}
static void* get_params_wrapper   (struct dt_imageio_module_storage_t *self, int *size)
{
  *size = sizeof(int);
  void *d = malloc(*size);
  return d;
}
static void  free_params_wrapper  (struct dt_imageio_module_storage_t *self, dt_imageio_module_data_t *data){free(data);}
static int   set_params_wrapper   (struct dt_imageio_module_storage_t *self, const void *params, const int size){return 1; }

static dt_imageio_module_storage_t ref_storage =
{
  .plugin_name = {0},
  .module = NULL,
  .widget = NULL,
  .gui_data = NULL,
  .name = name_wrapper,
  .gui_init = empty_wrapper,
  .gui_cleanup = empty_wrapper,
  .gui_reset = empty_wrapper,
  .init = NULL,
  .supported = default_supported_wrapper,
  .dimension = default_dimension_wrapper,
  .recommended_dimension = default_dimension_wrapper,
  .store = store_wrapper,
  .finalize_store = finalize_store_wrapper,
  .get_params = get_params_wrapper,
  .free_params = free_params_wrapper,
  .set_params = set_params_wrapper,
  .parameter_lua_type = LUAA_INVALID_TYPE,

};

static int register_storage(lua_State *L) {
  lua_settop(L,4);
  lua_getfield(L,LUA_REGISTRYINDEX,"dt_lua_storages");
  lua_newtable(L);

  dt_imageio_module_storage_t * storage = malloc(sizeof(dt_imageio_module_storage_t));
  memcpy(storage,&ref_storage,sizeof(dt_imageio_module_storage_t));

  const char * plugin_name = luaL_checkstring(L,1);
  lua_pushvalue(L,1);
  lua_setfield(L,-2,"plugin_name");
  strncpy(storage->plugin_name,plugin_name,127);

  luaL_checkstring(L,2);
  lua_pushvalue(L,2);
  lua_setfield(L,-2,"name");

  luaL_checktype(L,3,LUA_TFUNCTION);
  lua_pushvalue(L,3);
  lua_setfield(L,-2,"store");
  
  if(lua_isnil(L,4) ){
    storage->finalize_store = NULL;
  } else {
    luaL_checktype(L,4,LUA_TFUNCTION);
    lua_pushvalue(L,4);
    lua_setfield(L,-2,"finalize_store");
  }
  lua_setfield(L,-2,plugin_name);


  char tmp[1024];
  snprintf(tmp,1024,"dt_imageio_module_data_pseudo_%s",storage->plugin_name);
  storage->parameter_lua_type = dt_lua_init_storage_internal(darktable.lua_state,storage,tmp,sizeof(int));

  dt_imageio_insert_storage(storage);

  return 0;
}

int dt_lua_init_storages(lua_State *L)
{
  dt_lua_push_darktable_lib(L);
  lua_pushstring(L,"register_storage");
  lua_pushcfunction(L,&register_storage);
  lua_settable(L,-3);
  lua_pop(L,1);

  lua_newtable(L);
  lua_setfield(L,LUA_REGISTRYINDEX,"dt_lua_storages");
  return 0;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
