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
#include "control/jobs.h"

typedef struct {
  GList* imgids;
  GList* file_names;
  gboolean data_created;
} lua_storage_t;

typedef struct {
  char * name;
  GList * supported_formats;
} lua_storage_gui_t;

static const char* name_wrapper(const struct dt_imageio_module_storage_t *self)
{
  return ((lua_storage_gui_t*)self->gui_data)->name;
}
static  void empty_wrapper(struct dt_imageio_module_storage_t *self) {};
static int default_supported_wrapper    (struct dt_imageio_module_storage_t *self, struct dt_imageio_module_format_t *format)
{
  if(g_list_find(((lua_storage_gui_t*)self->gui_data)->supported_formats,format)) {
    return TRUE;
  } else {
    return FALSE;
  }
}
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
  dt_image_path_append_version(imgid, dirname, DT_MAX_PATH_LEN);
  const gchar * filename = g_path_get_basename( dirname );
  gchar * end = g_strrstr( filename,".")+1;
  g_strlcpy( end, format->extension(fdata), sizeof(dirname)-(end-dirname));

  gchar* complete_name = g_build_filename( tmpdir, filename, (char *)NULL );

  if(dt_imageio_export(imgid, complete_name, format, fdata, high_quality,FALSE,self,self_data) != 0)
  {
    fprintf(stderr, "[%s] could not export to file: `%s'!\n", self->name(self),complete_name);
    g_free(complete_name);
    return 1;
  }

  lua_storage_t *d = (lua_storage_t*) self_data;
  d->imgids = g_list_prepend(d->imgids,(void*)(intptr_t)imgid);
  d->file_names = g_list_prepend(d->file_names,complete_name);

  gboolean has_lock = dt_lua_lock();
  if(!d->data_created) {
    lua_pushlightuserdata(darktable.lua_state.state,d);
    lua_newtable(darktable.lua_state.state);
    lua_settable(darktable.lua_state.state,LUA_REGISTRYINDEX);
    d->data_created = true;
  }



  lua_State *L =darktable.lua_state.state;

  lua_getfield(L,LUA_REGISTRYINDEX,"dt_lua_storages");
  lua_getfield(L,-1,self->plugin_name);
  lua_getfield(L,-1,"store");

  if(lua_isnil(L,-1)) {
    lua_pop(L,3);
    dt_lua_unlock(has_lock);
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
  lua_pop(L,3);
  dt_lua_unlock(has_lock);
  return result;

}
static void initialize_store_wrapper (struct dt_imageio_module_storage_t *self, dt_imageio_module_data_t *data, dt_imageio_module_format_t *format, dt_imageio_module_data_t *fdata, GList **images, const gboolean high_quality)
{
  gboolean has_lock = dt_lua_lock();
  lua_State *L =darktable.lua_state.state;

  lua_getfield(L,LUA_REGISTRYINDEX,"dt_lua_storages");
  lua_getfield(L,-1,self->plugin_name);
  lua_getfield(L,-1,"initialize_store");

  if(lua_isnil(L,-1)) {
    lua_pop(L,3);
    dt_lua_unlock(has_lock);
    return;
  }

  luaA_push_typeid(L,self->parameter_lua_type,data);
  luaA_push_typeid(L,format->parameter_lua_type,fdata);

  GList* imgids =*images;
  lua_newtable(L);
  while(imgids) {
    luaA_push(L,dt_lua_image_t,&(imgids->data));
    luaL_ref(L,-2);
    imgids = g_list_next(imgids);
  }
  lua_pushboolean(L,high_quality);

  lua_storage_t *d = (lua_storage_t*) data;
  if(!d->data_created) {
    lua_pushlightuserdata(L,d);
    lua_newtable(L);
    lua_settable(L,LUA_REGISTRYINDEX);
    d->data_created = true;
  }
  lua_pushlightuserdata(L,data);
  lua_gettable(L,LUA_REGISTRYINDEX);

  dt_lua_do_chunk(L,5,1);
  if(!lua_isnoneornil(L,-1)) {
    luaL_checktype(L,-1,LUA_TTABLE);
    g_list_free(*images);
    GList *new_images=NULL;
    lua_pushnil(L);
    while(lua_next(L,-2)){
      dt_lua_image_t imgid;
      luaA_to(L,dt_lua_image_t,&imgid,-1);
      new_images = g_list_prepend(new_images,GINT_TO_POINTER(imgid));
      lua_pop(L,1);
    }
    new_images = g_list_reverse(new_images);
    *images = new_images;
  }
  lua_pop(L,3);
  dt_lua_unlock(has_lock);
}
static void finalize_store_wrapper (struct dt_imageio_module_storage_t *self, dt_imageio_module_data_t *data)
{
  gboolean has_lock = dt_lua_lock();
  lua_State *L =darktable.lua_state.state;

  lua_getfield(L,LUA_REGISTRYINDEX,"dt_lua_storages");
  lua_getfield(L,-1,self->plugin_name);
  lua_getfield(L,-1,"finalize_store");

  if(lua_isnil(L,-1)) {
    lua_pop(L,3);
    dt_lua_unlock(has_lock);
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

  if(!d->data_created) {
    lua_pushlightuserdata(darktable.lua_state.state,d);
    lua_newtable(darktable.lua_state.state);
    lua_settable(darktable.lua_state.state,LUA_REGISTRYINDEX);
    d->data_created = true;
  }
  lua_pushlightuserdata(L,data);
  lua_gettable(L,LUA_REGISTRYINDEX);

  dt_lua_do_chunk(L,3,0);
  lua_pop(L,2);
  dt_lua_unlock(has_lock);
}
static size_t params_size_wrapper   (struct dt_imageio_module_storage_t *self)
{
  return sizeof(lua_storage_t);
}
static void* get_params_wrapper   (struct dt_imageio_module_storage_t *self)
{
  lua_storage_t *d = malloc(sizeof(lua_storage_t));
  d->imgids = NULL;
  d->file_names = NULL;
  d->data_created = false;
  return d;
}

typedef struct {
  lua_storage_t* data;
} free_param_wrapper_data;

static int32_t free_param_wrapper_job(struct dt_job_t *job) 
{
  lua_storage_t *d = ((free_param_wrapper_data*) job->param)->data;
  g_list_free(d->imgids);
  g_list_free_full(d->file_names,free);
  if(d->data_created) {
    gboolean has_lock = dt_lua_lock();
    lua_pushlightuserdata(darktable.lua_state.state,d);
    lua_pushnil(darktable.lua_state.state);
    lua_settable(darktable.lua_state.state,LUA_REGISTRYINDEX);
    dt_lua_unlock(has_lock);
  }
  free(d);
  return 0;
}


static void  free_params_wrapper  (struct dt_imageio_module_storage_t *self, dt_imageio_module_data_t *data)
{
  dt_job_t job;
  dt_control_job_init(&job, "lua: destroy storage param");
  job.execute = &free_param_wrapper_job;
  free_param_wrapper_data *t = (free_param_wrapper_data*)job.param;
  t->data = (lua_storage_t*)data;
  dt_control_add_job(darktable.control, &job);
}

static int   set_params_wrapper   (struct dt_imageio_module_storage_t *self, const void *params, const int size)
{
  return 0;
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
  .initialize_store = initialize_store_wrapper,
  .params_size = params_size_wrapper,
  .get_params = get_params_wrapper,
  .free_params = free_params_wrapper,
  .set_params = set_params_wrapper,
  .parameter_lua_type = LUAA_INVALID_TYPE,

};



static int register_storage(lua_State *L)
{
  lua_settop(L,6);
  lua_getfield(L,LUA_REGISTRYINDEX,"dt_lua_storages");
  lua_newtable(L);

  dt_imageio_module_storage_t * storage = malloc(sizeof(dt_imageio_module_storage_t));
  memcpy(storage,&ref_storage,sizeof(dt_imageio_module_storage_t));
  storage->gui_data = malloc(sizeof(lua_storage_gui_t));
  lua_storage_gui_t * data = storage->gui_data;

  const char * plugin_name = luaL_checkstring(L,1);
  lua_pushvalue(L,1);
  lua_setfield(L,-2,"plugin_name");
  g_strlcpy(storage->plugin_name,plugin_name,sizeof(storage->plugin_name));

  const char * name  = luaL_checkstring(L,2);
  lua_pushvalue(L,2);
  lua_setfield(L,-2,"name");
  data->name = strdup(name);
  data->supported_formats = NULL;

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

  if(lua_isnil(L,6) )
  {
    storage->initialize_store = NULL;
  }
  else
  {
    luaL_checktype(L,6,LUA_TFUNCTION);
    lua_pushvalue(L,6);
    lua_setfield(L,-2,"initialize_store");
  }

  lua_setfield(L,-2,plugin_name);

  char tmp[1024];
  snprintf(tmp,1024,"dt_imageio_module_data_pseudo_%s",storage->plugin_name);
  luaA_Type type_id = luaA_type_add(tmp,storage->params_size(storage));
  storage->parameter_lua_type = dt_lua_init_type_typeid(darktable.lua_state.state,type_id);
  luaA_struct_typeid(darktable.lua_state.state,type_id);
  dt_lua_register_storage_typeid(darktable.lua_state.state,storage,type_id);




  GList *it = darktable.imageio->plugins_format;
  if(!lua_isnoneornil(L,5)) {
    while(it)
    {
      lua_pushvalue(L,5);
      dt_imageio_module_format_t *format = (dt_imageio_module_format_t *)it->data;
      dt_imageio_module_data_t *sdata = storage->get_params(storage);
      dt_imageio_module_data_t *fdata = format->get_params(format);
      luaA_push_typeid(L,storage->parameter_lua_type,sdata);
      luaA_push_typeid(L,format->parameter_lua_type,fdata);
      format->free_params(format,fdata);
      storage->free_params(storage,sdata);
      dt_lua_do_chunk(L,2,1);
      int result = lua_toboolean(L,-1);
      lua_pop(L,1);
      if(result) {
        data->supported_formats = g_list_prepend(data->supported_formats,format);
      }
      it = g_list_next(it);
    }
  } else {
    // all formats are supported
    while(it)
    {
      dt_imageio_module_format_t *format = (dt_imageio_module_format_t *)it->data;
      data->supported_formats = g_list_prepend(data->supported_formats,format);
      it = g_list_next(it);
    }
  }
  
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
