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
#include "common/darktable.h"
#include "common/file_location.h"
#include "control/control.h"
#include "control/jobs/control_jobs.h"
#include "lua/call.h"
#include "lua/events.h"
#include "lua/image.h"
#include "lua/film.h"
#include "gui/accelerators.h"
#include "common/imageio_module.h"
typedef struct event_handler{
  const char* evt_name;
  lua_CFunction on_register;
  lua_CFunction on_event;
  gboolean in_use;
} event_handler;

static void run_event(const char*event,int nargs);
static int register_shortcut_event(lua_State* L);
static int trigger_keyed_event(lua_State * L);
static int register_multiinstance_event(lua_State* L);
static int trigger_multiinstance_event(lua_State * L);


static event_handler event_list[] = {
  //{"pre-export",register_chained_event,trigger_chained_event},
  {"shortcut",register_shortcut_event,trigger_keyed_event,FALSE}, 
  {"post-import-image",register_multiinstance_event,trigger_multiinstance_event,FALSE},
  {"post-import-film",register_multiinstance_event,trigger_multiinstance_event,FALSE},
  {"intermediate-export-image",register_multiinstance_event,trigger_multiinstance_event,FALSE},
  //{"test",register_singleton_event,trigger_singleton_event},  // avoid error because of unused function
  {NULL,NULL,NULL}
};
#if 0
static int register_singleton_event(lua_State* L) {
  // 1 is the event name (checked)
  // 2 is the action to perform (checked)
  lua_getfield(L,LUA_REGISTRYINDEX,"dt_lua_event_data");
  lua_getfield(L,-1,lua_tostring(L,1));
  if(!lua_isnil(L,-1)) {
    lua_pop(L,2);
    return luaL_error(L,"an action has already been registered for event %s",lua_tostring(L,1));
  }
  lua_pop(L,1);
  lua_pushvalue(L,2);
  lua_setfield(L,-2,lua_tostring(L,1));
  lua_setfield(L,-2,"action");
  lua_pushvalue(L,3);
  lua_setfield(L,-2,"data");
  lua_pop(L,1);
  return 0;
}

static int trigger_singleton_event(lua_State * L,const char* evt_name, int nargs,int nresults) {
  // -1..-n are our args
  const int top_marker=lua_gettop(L);
  lua_getfield(L,LUA_REGISTRYINDEX,"dt_lua_event_data");
  lua_getfield(L,-1,evt_name);
  if(lua_isnil(L,-1)) {
    lua_pop(L,2+nargs);
    if(nresults == LUA_MULTRET) return 0;
    for(int i = 0 ; i < nresults ; i++) lua_pushnil(L);
    return nresults;
  }
  // prepare the call
  lua_insert(L,top_marker-nargs+1); // function to call
  lua_pushstring(L,evt_name);// param 1 is the event
  lua_insert(L,top_marker-nargs+2);
  lua_pop(L,1);
  return dt_lua_do_chunk(L,nargs+2,nresults);
}
#endif
/*
 * KEYED EVENTS
 * these are events that are triggered with a key
 * i.e the can be registered multiple time with a key parameter and only the handler with the corresponding
 * key will be triggered. there can be only one handler per key 
 *
 * when registering, the third argument is the key
 * when triggering, the first argument is the key
 * 
 * data tables is "event => {key => callback}"
 */
static int register_keyed_event(lua_State* L) {
  // 1 is the event name (checked)
  // 2 is the action to perform (checked)
  // 3 is the key (unchecked at this point)
  if(lua_isnoneornil(L,3)) 
    return luaL_error(L,"no key provided when registering event");
  lua_getfield(L,LUA_REGISTRYINDEX,"dt_lua_event_data");
  lua_getfield(L,-1,lua_tostring(L,1));
  if(lua_isnil(L,-1)) {
    lua_pop(L,1);
    lua_newtable(L);
    lua_pushvalue(L,-1);
    lua_setfield(L,-3,lua_tostring(L,1));
  }
  lua_getfield(L,-1,luaL_checkstring(L,3));
  if(!lua_isnil(L,-1)) 
    return luaL_error(L,"key already registered for event : %s",luaL_checkstring(L,3));
  lua_pop(L,1);

  lua_pushvalue(L,2);
  lua_setfield(L,-2,luaL_checkstring(L,3));

  lua_pop(L,2);
  return 0;
}


static int trigger_keyed_event(lua_State * L) {
  int nargs = luaL_checknumber(L,-1);
  const char* evt_name = luaL_checkstring(L,-2);
  lua_pop(L,2);
  // -1..-n are our args
  const int top_marker=lua_gettop(L);
  lua_getfield(L,LUA_REGISTRYINDEX,"dt_lua_event_data");
  lua_getfield(L,-1,evt_name);
  if(lua_isnil(L,-1)) {
    lua_pop(L,2+nargs);
    return 0;
  }
  const char*key=luaL_checkstring(L,-nargs-2); // first arg is the key itself
  lua_getfield(L,-1,key);
  if(lua_isnil(L,-1)) {
    lua_pop(L,2+nargs);
    return  0;
  }
  // prepare the call
  lua_insert(L,top_marker-nargs+1);
  lua_pushstring(L,evt_name);// param 1 is the event
  lua_insert(L,top_marker-nargs+2);
  lua_pop(L,2);
  return dt_lua_do_chunk(L,nargs+1,0);
}

/*
 * shortcut events
 * keyed event with a tuned registration to handle shortcuts
 */
typedef struct {
  char* name;
} shortcut_callback_data;
static int32_t shortcut_callback_job(struct dt_job_t *job) {
  gboolean has_lock = dt_lua_lock();
  shortcut_callback_data *t = (shortcut_callback_data*)job->param;
  lua_pushstring(darktable.lua_state.state,t->name);
  free(t->name);
  run_event("shortcut",1);
  dt_lua_unlock(has_lock);
  return 0;
}
static gboolean shortcut_callback(GtkAccelGroup *accel_group,
    GObject *acceleratable,
    guint keyval,
    GdkModifierType modifier,
    gpointer p)
{
  dt_job_t job;
  dt_control_job_init(&job, "lua: on shortcut");
  job.execute = &shortcut_callback_job;
  shortcut_callback_data *t = (shortcut_callback_data*)job.param;
  t->name = strdup(p);
  dt_control_add_job(darktable.control, &job);
  return TRUE;
}


static void closure_destroy(gpointer data,GClosure *closure) {
  free(data);
}
static int register_shortcut_event(lua_State* L) {
  // 1 is the event name (checked)
  // 2 is the action to perform (checked)
  // 3 is the key itself
  int result = register_keyed_event(L); // will raise an error in case of duplicate key
  const char* tmp = luaL_checkstring(L,3);
  dt_accel_register_lua(tmp,0,0);
  dt_accel_connect_lua(tmp, g_cclosure_new(G_CALLBACK(shortcut_callback),strdup(luaL_checkstring(L,3)),closure_destroy));
  return result;
}

/*
 * MULTIINSTANCE EVENTS
 * these events can be registered multiple time with multiple callbacks
 * all callbacks will be called in the order they were registered
 * 
 * all callbacks will receive the same parameters
 * the result is all return values from all callbacks
 *
 * data table is "event => { # => callback }
 */


static int register_multiinstance_event(lua_State* L) {
  // 1 is the event name (checked)
  // 2 is the action to perform (checked)
  lua_getfield(L,LUA_REGISTRYINDEX,"dt_lua_event_data");
  lua_getfield(L,-1,lua_tostring(L,1));
  if(lua_isnil(L,-1)) {
    lua_pop(L,1);
    lua_newtable(L);
    lua_pushvalue(L,-1);
    lua_setfield(L,-3,lua_tostring(L,1));
  }

  lua_pushvalue(L,2);
  luaL_ref(L,-2);
  lua_pop(L,2);
  return 0;
}

static int trigger_multiinstance_event(lua_State * L) {
  int nargs = luaL_checknumber(L,-1);
  const char* evt_name = luaL_checkstring(L,-2);
  lua_pop(L,2);
  // -1..-n are our args
  const int top_marker=lua_gettop(L);
  lua_getfield(L,LUA_REGISTRYINDEX,"dt_lua_event_data");
  lua_getfield(L,-1,evt_name);
  if(lua_isnil(L,-1)) {
    lua_pop(L,2+nargs);
    return 0;
  }
  lua_remove(L,-2);


  lua_pushnil(L);  /* first key */
  int nresult = 0;
  while (lua_next(L, top_marker +1) != 0) {
    /* uses 'key' (at index -2) and 'value' (at index -1)  value is the function to call*/
    // prepare the call
    lua_pushstring(L,evt_name);// param 1 is the event
    for(int i = 0 ; i<nargs ;i++) { // event dependant parameters
      lua_pushvalue(L, top_marker -nargs +1 +i); 
    }
    nresult += dt_lua_do_chunk(L,nargs+1,0);
  }
  return nresult;
}



static int lua_register_event(lua_State *L) {
  // 1 is event name
  const char*evt_name = luaL_checkstring(L,1);
  // 2 is event handler
  luaL_checktype(L,2,LUA_TFUNCTION);
  lua_getfield(L,LUA_REGISTRYINDEX,"dt_lua_event_list");
  lua_getfield(L,-1,evt_name);
  if(lua_isnil(L,-1)) {
    lua_pop(L,2);
    return luaL_error(L,"unknown event type : %s\n",evt_name);
  }
  event_handler * handler =  lua_touserdata(L,-1);
  lua_pop(L,2); // restore the stack to only have the 3 parameters
  handler->on_register(L);
  handler->in_use=TRUE;
  return 0;

}



static void run_event(const char*event,int nargs) {
  lua_getfield(darktable.lua_state.state,LUA_REGISTRYINDEX,"dt_lua_event_list");
  if(lua_isnil(darktable.lua_state.state,-1)) {// events have been disabled
    lua_settop(darktable.lua_state.state,0);
    return; 
  }
  lua_getfield(darktable.lua_state.state,-1,event);
  event_handler * handler =  lua_touserdata(darktable.lua_state.state,-1);
  lua_pop(darktable.lua_state.state,2);
  if(!handler->in_use) { 
    lua_pop(darktable.lua_state.state,nargs);
    return; 
  }
  lua_pushcfunction(darktable.lua_state.state,handler->on_event);
  lua_insert(darktable.lua_state.state,-nargs -1);
  lua_pushstring(darktable.lua_state.state,event);
  lua_pushnumber(darktable.lua_state.state,nargs);
  dt_lua_do_chunk(darktable.lua_state.state,nargs+2,0);
  
}
#if 0
static void on_export_selection(gpointer instance,dt_control_image_enumerator_t * export_descriptor,
     gpointer user_data){
  warning to self : add locking
  lua_State* L = darktable.lua_state.state;
  dt_control_export_t *export_data= (dt_control_export_t*)export_descriptor->data;

  dt_imageio_module_storage_t  *mstorage  = dt_imageio_get_storage_by_index(export_data->storage_index);
  g_assert(mstorage);
  dt_imageio_module_data_t *fdata = mstorage->get_params(mstorage);
  luaA_push_typeid(L,mstorage->parameter_lua_type,fdata);
  mstorage->free_params(mstorage,fdata);

  dt_imageio_module_format_t  *mformat  = dt_imageio_get_format_by_index(export_data->format_index);
  g_assert(mformat);
  fdata = mformat->get_params(mformat);
  luaA_push_typeid(L,mformat->parameter_lua_type,fdata);
  mformat->free_params(mformat,fdata);

  GList * elt = export_descriptor->index;
  lua_newtable(L);
  while(elt)
  {
    luaA_push(L,dt_lua_image_t,&elt->data);
    luaL_ref(L,-2);
    elt = g_list_next(elt);
  }
  g_list_free(export_descriptor->index);
  export_descriptor->index =NULL;

  queue_event("pre-export",3,3);

  // get the new storage data and the new storage
  luaL_getmetafield(L,-3,"__associated_object");
  mstorage = lua_touserdata(L,-1);
  lua_pop(L,1);
  fdata = mstorage->get_params(mstorage);
  luaL_getmetafield(L,-3,"__luaA_Type");
  luaA_Type storage_type = lua_tointeger(L,-1);
  lua_pop(L,1);
  luaA_to_typeid(L,storage_type,fdata,-3);
  mstorage->set_params(mstorage,fdata,mstorage->params_size(mstorage));
  mstorage->free_params(mstorage,fdata);
  export_data->storage_index = dt_imageio_get_index_of_storage(mstorage);

  // get the new format data and the new format
  luaL_getmetafield(L,-2,"__associated_object");
  mformat = lua_touserdata(L,-1);
  lua_pop(L,1);
  fdata = mformat->get_params(mformat);
  luaL_getmetafield(L,-2,"__luaA_Type");
  luaA_Type format_type = lua_tointeger(L,-1);
  lua_pop(L,1);
  luaA_to_typeid(L,format_type,fdata,-2);
  mformat->set_params(mformat,fdata,mstorage->params_size(mstorage));
  mformat->free_params(mformat,fdata);
  export_data->format_index = dt_imageio_get_index_of_format(mformat);

  // load the new list of images to process
  if(lua_isnoneornil(L,-1)) {lua_pop(L,3); return; }// everything already has been removed
  luaA_to(L,dt_lua_image_t,&export_descriptor->index,-1);

  lua_pop(L,1);
}
#endif

/*
   called on a signal, from a secondary thread
   => we have the gdk lock, but the main UI thread can run if we release it
   */

static void on_export_image_tmpfile(gpointer instance,
    int imgid,
    char *filename,
    dt_imageio_module_format_t* format,
    dt_imageio_module_data_t* fdata,
    dt_imageio_module_storage_t* storage,
    dt_imageio_module_data_t* sdata,
     gpointer user_data){
  gboolean has_lock = dt_lua_lock();
  luaA_push(darktable.lua_state.state,dt_lua_image_t,&imgid);
  lua_pushstring(darktable.lua_state.state,filename);
  luaA_push_typeid(darktable.lua_state.state,format->parameter_lua_type,fdata);
  if(storage) {
    luaA_push_typeid(darktable.lua_state.state,storage->parameter_lua_type,sdata);
  } else {
    lua_pushnil(darktable.lua_state.state);
  }
  run_event("intermediate-export-image",4);
  dt_lua_unlock(has_lock);
}

static void on_image_imported(gpointer instance,uint8_t id, gpointer user_data){
  gboolean has_lock = dt_lua_lock();
  luaA_push(darktable.lua_state.state,dt_lua_image_t,&id);
  run_event("post-import-image",1);
  dt_lua_unlock(has_lock);
}

static void on_film_imported(gpointer instance,uint8_t id, gpointer user_data){
  gboolean has_lock = dt_lua_lock();
  luaA_push(darktable.lua_state.state,dt_lua_film_t,&id);
  run_event("post-import-film",1);
  dt_lua_unlock(has_lock);
}

int dt_lua_init_events(lua_State *L) {
  lua_newtable(L);
  lua_setfield(L,LUA_REGISTRYINDEX,"dt_lua_event_data");
  lua_newtable(L);
  event_handler * handler = event_list;
  while(handler->evt_name) {
    lua_pushlightuserdata(L,handler);
    lua_setfield(L,-2,handler->evt_name);
    handler++;
  }
  lua_setfield(L,LUA_REGISTRYINDEX,"dt_lua_event_list");
  dt_lua_push_darktable_lib(L);
  lua_pushstring(L,"register_event");
  lua_pushcfunction(L,&lua_register_event);
  lua_settable(L,-3);
  lua_pop(L,1);
  dt_control_signal_connect(darktable.signals,DT_SIGNAL_IMAGE_IMPORT,G_CALLBACK(on_image_imported),NULL);
  dt_control_signal_connect(darktable.signals,DT_SIGNAL_FILMROLLS_IMPORTED,G_CALLBACK(on_film_imported),NULL);
  //dt_control_signal_connect(darktable.signals,DT_SIGNAL_IMAGE_EXPORT_MULTIPLE,G_CALLBACK(on_export_selection),NULL);
  dt_control_signal_connect(darktable.signals,DT_SIGNAL_IMAGE_EXPORT_TMPFILE,G_CALLBACK(on_export_image_tmpfile),NULL);
  return 0;
}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
