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
#include <stdio.h>
#include <common/darktable.h>
#include "common/imageio_module.h"
#include "common/file_location.h"
#include "common/image.h"
#include "common/imageio.h"
#include "lua/call.h"
#include "lua/glist.h"

typedef struct {
  GList* imgids;
  GList* file_names;
} lua_storage_t;

static const char* name_wrapper(const struct dt_imageio_module_storage_t *self)
{
  dt_lua_lock();
  lua_State *L =darktable.lua_state;
  lua_getfield(L,LUA_REGISTRYINDEX,"dt_lua_storages");
  lua_getfield(L,-1,self->plugin_name);
  lua_getfield(L,-1,"name");
  const char* result = lua_tostring(L,-1);
  lua_pop(L,3);
  dt_lua_unlock();
  return result;
}
static  void empty_wrapper(struct dt_imageio_module_storage_t *self) {};
static int default_supported_wrapper    (struct dt_imageio_module_storage_t *self, struct dt_imageio_module_format_t *format)
{
  dt_lua_lock();
  lua_State *L =darktable.lua_state;
  lua_getfield(L,LUA_REGISTRYINDEX,"dt_lua_storages");
  lua_getfield(L,-1,self->plugin_name);
  lua_getfield(L,-1,"supported");

  if(lua_isnil(L,-1)) {
    lua_pop(L,3);
    dt_lua_unlock();
    return true;
  }
  dt_imageio_module_data_t *sdata = self->get_params(self);
  dt_imageio_module_data_t *fdata = format->get_params(format);
  luaA_push_typeid(L,self->parameter_lua_type,sdata);
  luaA_push_typeid(L,format->parameter_lua_type,fdata);
  format->free_params(format,fdata);
  self->free_params(self,sdata);
  dt_lua_do_chunk(L,2,1);
  int result = lua_toboolean(L,-1);
  lua_pop(L,2);
  dt_lua_unlock();
  return result;
};
static int default_dimension_wrapper    (struct dt_imageio_module_storage_t *self, uint32_t *width, uint32_t *height)
{
  return 0;
};

static int store_wrapper(struct dt_imageio_module_storage_t *self,struct dt_imageio_module_data_t *self_data, const int imgid, dt_imageio_module_format_t *format, dt_imageio_module_data_t *fdata, const int num, const int total, const gboolean high_quality)
{
  /* construct a temporary file name */
  char tmpdir[DT_MAX_PATH_LEN]= {0};
  gboolean from_cache = FALSE;
  dt_loc_get_tmp_dir (tmpdir,DT_MAX_PATH_LEN);

  char dirname[DT_MAX_PATH_LEN];
  dt_image_full_path(imgid, dirname, DT_MAX_PATH_LEN, &from_cache);
  const gchar * filename = g_path_get_basename( dirname );
  gchar * end = g_strrstr( filename,".")+1;
  g_strlcpy( end, format->extension(fdata), sizeof(dirname)-(end-dirname));

  gchar* complete_name = g_build_filename( tmpdir, filename, (char *)NULL );

  if(dt_imageio_export(imgid, complete_name, format, fdata, high_quality) != 0)
  {
    fprintf(stderr, "[%s] could not export to file: `%s'!\n", self->name(self),complete_name);
    g_free(complete_name);
    return 1;
  }

  lua_storage_t *d = (lua_storage_t*) self_data;
  d->imgids = g_list_prepend(d->imgids,(void*)(intptr_t)imgid);
  d->file_names = g_list_prepend(d->file_names,complete_name);



  dt_lua_lock();
  lua_State *L =darktable.lua_state;

  lua_getfield(L,LUA_REGISTRYINDEX,"dt_lua_storages");
  lua_getfield(L,-1,self->plugin_name);
  lua_getfield(L,-1,"store");

  if(lua_isnil(L,-1)) {
    lua_pop(L,3);
    dt_lua_unlock();
    return 1;
  }

  luaA_push_typeid(L,self->parameter_lua_type,self_data);
  luaA_push(L,dt_lua_image_t,&imgid);
  luaA_push_typeid(L,format->parameter_lua_type,fdata);
  lua_pushstring(L,complete_name);
  lua_pushnumber(L,num);
  lua_pushnumber(L,total);
  lua_pushboolean(L,high_quality);
  lua_pushlightuserdata(L,self_data);
  lua_gettable(L,LUA_REGISTRYINDEX);
  dt_lua_do_chunk(L,8,1);
  int result = lua_toboolean(L,-1);
  lua_pop(L,2);
  dt_lua_unlock();
  return result;

}
extern void finalize_store_wrapper (struct dt_imageio_module_storage_t *self, dt_imageio_module_data_t *data)
{
  dt_lua_lock();
  lua_State *L =darktable.lua_state;

  lua_getfield(L,LUA_REGISTRYINDEX,"dt_lua_storages");
  lua_getfield(L,-1,self->plugin_name);
  lua_getfield(L,-1,"finalize_store");

  if(lua_isnil(L,-1)) {
    lua_pop(L,3);
    dt_lua_unlock();
    return;
  }

  luaA_push_typeid(L,self->parameter_lua_type,data);

  lua_storage_t *d = (lua_storage_t*) data;
  GList* imgids =d->imgids;
  GList* file_names = d->file_names;
  lua_newtable(L);
  while(imgids) {
    luaA_push(L,dt_lua_image_t,&(imgids->data));
    lua_pushstring(L,file_names->data);
    lua_settable(L,-3);
    imgids = g_list_next(imgids);
    file_names = g_list_next(file_names);
  }

  lua_pushlightuserdata(L,data);
  lua_gettable(L,LUA_REGISTRYINDEX);

  dt_lua_do_chunk(L,3,0);
  lua_pop(L,2);
  dt_lua_unlock();
}
static size_t params_size_wrapper   (struct dt_imageio_module_storage_t *self)
{
  return sizeof(lua_storage_t);
}
static void* get_params_wrapper   (struct dt_imageio_module_storage_t *self)
{
  dt_lua_lock();
  lua_storage_t *d = malloc(sizeof(lua_storage_t));
  d->imgids = NULL;
  d->file_names = NULL;
  lua_pushlightuserdata(darktable.lua_state,d);
  lua_newtable(darktable.lua_state);
  lua_settable(darktable.lua_state,LUA_REGISTRYINDEX);
  dt_lua_unlock();
  return d;
}
static void  free_params_wrapper  (struct dt_imageio_module_storage_t *self, dt_imageio_module_data_t *data)
{
  dt_lua_lock();
  lua_pushlightuserdata(darktable.lua_state,data);
  lua_pushnil(darktable.lua_state);
  lua_settable(darktable.lua_state,LUA_REGISTRYINDEX);
  lua_storage_t *d = (lua_storage_t*) data;
  g_list_free(d->imgids);
  g_list_free_full(d->file_names,free);
  free(data);
  dt_lua_unlock();
}
static int   set_params_wrapper   (struct dt_imageio_module_storage_t *self, const void *params, const int size)
{
  return 1;
}

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
  .params_size = params_size_wrapper,
  .get_params = get_params_wrapper,
  .free_params = free_params_wrapper,
  .set_params = set_params_wrapper,
  .parameter_lua_type = LUAA_INVALID_TYPE,

};


/*static int extra_data_index(lua_State *L)
{
  void * udata;
  luaL_getmetafield(L,-2,"__luaA_Type");
  luaA_Type type = lua_tointeger(L,-1);
  lua_pop(L,1);
  luaA_to_typeid(L,type,&udata,-2);
  lua_pushlightuserdata(L,udata);
  lua_gettable(L,LUA_REGISTRYINDEX);
  lua_pushvalue(L,-2);
  lua_gettable(L,-2);
  return 1;
}

static int extra_data_newindex(lua_State *L)
{
  void * udata;
  luaL_getmetafield(L,-3,"__luaA_Type");
  luaA_Type type = lua_tointeger(L,-1);
  lua_pop(L,1);
  luaA_to_typeid(L,type,&udata,-3);
  lua_pushlightuserdata(L,udata);
  lua_gettable(L,LUA_REGISTRYINDEX);
  lua_pushvalue(L,-3);
  lua_pushvalue(L,-3);
  lua_settable(L,-3);
  return 0;
}

static int extra_data_length(lua_State *L)
{
  void * udata;
  luaL_getmetafield(L,-1,"__luaA_Type");
  luaA_Type type = lua_tointeger(L,-1);
  lua_pop(L,1);
  luaA_to_typeid(L,type,&udata,-1);
  lua_pushlightuserdata(L,udata);
  lua_gettable(L,LUA_REGISTRYINDEX);
  lua_len(L,-1);
  return 1;
}

static int extra_data_next(lua_State *L)
{
  void * udata;
  luaL_getmetafield(L,-2,"__luaA_Type");
  luaA_Type type = lua_tointeger(L,-1);
  lua_pop(L,1);
  luaA_to_typeid(L,type,&udata,-2);
  lua_pushlightuserdata(L,udata);
  lua_gettable(L,LUA_REGISTRYINDEX);
  lua_pushvalue(L,-2);
  if(lua_next(L,-2)) {
    while(lua_isnumber(L,-2) ){
      // skip all numbers, they were handled separately
      lua_pop(L,1);
      lua_next(L,-2);
    }
    lua_remove(L,-4);
    lua_remove(L,-4);
    return 2;
  } else {
    lua_pop(L,2);
    lua_pushnil(L);
    return 1;
  }
}*/

static int register_storage(lua_State *L)
{
  lua_settop(L,5);
  lua_getfield(L,LUA_REGISTRYINDEX,"dt_lua_storages");
  lua_newtable(L);

  dt_imageio_module_storage_t * storage = malloc(sizeof(dt_imageio_module_storage_t));
  memcpy(storage,&ref_storage,sizeof(dt_imageio_module_storage_t));

  const char * plugin_name = luaL_checkstring(L,1);
  lua_pushvalue(L,1);
  lua_setfield(L,-2,"plugin_name");
  g_strlcpy(storage->plugin_name,plugin_name,sizeof(storage->plugin_name));

  luaL_checkstring(L,2);
  lua_pushvalue(L,2);
  lua_setfield(L,-2,"name");

  if(!lua_isnoneornil(L,3)) {
    luaL_checktype(L,3,LUA_TFUNCTION);
    lua_pushvalue(L,3);
    lua_setfield(L,-2,"store");
  }

  if(lua_isnil(L,4) )
  {
    storage->finalize_store = NULL;
  }
  else
  {
    luaL_checktype(L,4,LUA_TFUNCTION);
    lua_pushvalue(L,4);
    lua_setfield(L,-2,"finalize_store");
  }
  if(!lua_isnoneornil(L,5)) {
    luaL_checktype(L,5,LUA_TFUNCTION);
    lua_pushvalue(L,5);
    lua_setfield(L,-2,"supported");
  }
  lua_setfield(L,-2,plugin_name);


  char tmp[1024];
  snprintf(tmp,1024,"dt_imageio_module_data_pseudo_%s",storage->plugin_name);
  luaA_Type type_id = luaA_type_add(tmp,storage->params_size(storage));
  storage->parameter_lua_type = dt_lua_init_type_typeid(darktable.lua_state,type_id);
  luaA_struct_typeid(darktable.lua_state,type_id);
  //dt_lua_register_type_callback_default_typeid(L,tmp,extra_data_index,extra_data_newindex,extra_data_next);
  //dt_lua_register_type_callback_number_typeid(L,tmp,extra_data_index,extra_data_newindex,extra_data_length);
  dt_lua_register_storage_typeid(darktable.lua_state,storage,type_id);

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
