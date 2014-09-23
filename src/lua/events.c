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
  {"view-changed",register_multiinstance_event,trigger_multiinstance_event,FALSE},
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
  return dt_lua_do_chunk_silent(L,2,0);
}

/*
 * shortcut events
 * keyed event with a tuned registration to handle shortcuts
 */
typedef struct {
  char* name;
} shortcut_callback_data;
static int32_t shortcut_callback_job(dt_job_t *job) {
  gboolean has_lock = dt_lua_lock();
  shortcut_callback_data *t = dt_control_job_get_params(job);
  lua_pushstring(darktable.lua_state.state,t->name);
  free(t->name);
  free(t);
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
  dt_job_t *job = dt_control_job_create(&shortcut_callback_job, "lua: on shortcut");
  if(job)
  {
    shortcut_callback_data *t = (shortcut_callback_data*)calloc(1, sizeof(shortcut_callback_data));
    if(!t)
    {
      dt_control_job_dispose(job);
    }
    else
    {
      dt_control_job_set_params(job, t);
      t->name = strdup(p);
      dt_control_add_job(darktable.control, DT_JOB_QUEUE_USER_FG, job);
    }
  }
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
    nresult += dt_lua_do_chunk_silent(L,nargs+1,0);
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
  dt_lua_redraw_screen();
  
}

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
  luaA_push_type(darktable.lua_state.state,format->parameter_lua_type,fdata);
  if(storage) {
    luaA_push_type(darktable.lua_state.state,storage->parameter_lua_type,sdata);
  } else {
    lua_pushnil(darktable.lua_state.state);
  }
  run_event("intermediate-export-image",4);
  dt_lua_unlock(has_lock);
}


typedef struct {
  dt_view_t * old_view;
  dt_view_t * new_view;
} view_changed_callback_data_t;


static int32_t view_changed_callback_job(dt_job_t *job) {
  gboolean has_lock = dt_lua_lock();
  view_changed_callback_data_t *t = dt_control_job_get_params(job);
  dt_lua_module_push_entry(darktable.lua_state.state,"view",t->old_view->module_name);
  dt_lua_module_push_entry(darktable.lua_state.state,"view",t->new_view->module_name);
  free(t);
  run_event("view-changed",2);
  dt_lua_unlock(has_lock);
  return 0;
}

static void on_view_changed(gpointer instance,
    dt_view_t* old_view,
    dt_view_t* new_view,
     gpointer user_data){
  dt_job_t *job = dt_control_job_create(&view_changed_callback_job, "lua: on view changed");
  if(job)
  {
    view_changed_callback_data_t *t = (view_changed_callback_data_t*)calloc(1, sizeof(view_changed_callback_data_t));
    if(!t)
    {
      dt_control_job_dispose(job);
    }
    else
    {
      dt_control_job_set_params(job, t);
      t->old_view = old_view;
      t->new_view = new_view;
      dt_control_add_job(darktable.control, DT_JOB_QUEUE_USER_FG, job);
    }
  }
}

typedef struct {
  uint32_t imgid;
} on_image_imported_callback_data_t;


static int32_t on_image_imported_callback_job(dt_job_t *job) {
  gboolean has_lock = dt_lua_lock();
  on_image_imported_callback_data_t *t = dt_control_job_get_params(job);
  luaA_push(darktable.lua_state.state,dt_lua_image_t,&t->imgid);
  run_event("post-import-image",1);
  free(t); // i am not sure if the free() may happen before the run_event as a pointer to the imgid inside of it is pushed to the lua stack
  dt_lua_unlock(has_lock);
  return 0;
}

static void on_image_imported(gpointer instance,uint32_t id, gpointer user_data){
  dt_job_t *job = dt_control_job_create(&on_image_imported_callback_job, "lua: on image imported");
  if(job)
  {
    on_image_imported_callback_data_t *t = (on_image_imported_callback_data_t*)calloc(1, sizeof(on_image_imported_callback_data_t));
    if(!t)
    {
      dt_control_job_dispose(job);
    }
    else
    {
      dt_control_job_set_params(job, t);
      t->imgid = id;
      dt_control_add_job(darktable.control, DT_JOB_QUEUE_USER_FG, job);
    }
  }
}

static void on_film_imported(gpointer instance,uint32_t id, gpointer user_data){
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
  dt_control_signal_connect(darktable.signals,DT_SIGNAL_VIEWMANAGER_VIEW_CHANGED,G_CALLBACK(on_view_changed),NULL);
  return 0;
}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
