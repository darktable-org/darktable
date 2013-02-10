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
#include "lua/dt_lua.h"
#include "lua/events.h"
#include "lua/image.h"
#include "gui/accelerators.h"
#include "common/imageio_module.h"
typedef struct event_handler{
  const char* evt_name;
  lua_CFunction on_register;
  int (*on_event)(lua_State * L,const char* evt_name, int nargs,int nresults);
} event_handler;

static int dt_lua_trigger_event(const char*event,int nargs,int nresults);

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
  return dt_lua_do_chunk(L,0,nargs+2,nresults);
}

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
    return luaL_error(L,"key already registerd for event : %s",luaL_checkstring(L,3));
  lua_pop(L,1);

  lua_pushvalue(L,2);
  lua_setfield(L,-2,luaL_checkstring(L,3));

  lua_pop(L,2);
  return 0;
}


static int trigger_keyed_event(lua_State * L,const char* evt_name, int nargs,int nresults) {
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
  const char*key=luaL_checkstring(L,-nargs-2); // first arg is the key itself
  lua_getfield(L,-1,key);
  if(lua_isnil(L,-1)) {
    lua_pop(L,2+nargs);
    if(nresults == LUA_MULTRET) return 0;
    for(int i = 0 ; i < nresults ; i++) lua_pushnil(L);
    return nresults;
  }
  // prepare the call
  lua_insert(L,top_marker-nargs+1);
  lua_pushstring(L,evt_name);// param 1 is the event
  lua_insert(L,top_marker-nargs+2);
  lua_pop(L,2);
  return dt_lua_do_chunk(L,0,nargs+1,nresults);
}


static gboolean shortcut_callback(GtkAccelGroup *accel_group,
    GObject *acceleratable,
    guint keyval,
    GdkModifierType modifier,
    gpointer p)
{
  lua_pushstring(darktable.lua_state,(char*)p);
  dt_lua_trigger_event("shortcut",1,0);
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
  char tmp[1024];
  snprintf(tmp,1024,"lua/%s\n",luaL_checkstring(L,3));
  dt_accel_register_global(tmp,0,0);
  dt_accel_connect_global(tmp, g_cclosure_new(G_CALLBACK(shortcut_callback),strdup(luaL_checkstring(L,3)),closure_destroy));
  return result;
}




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

static int trigger_multiinstance_event(lua_State * L,const char* evt_name, int nargs,int nresults) {
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
  lua_remove(L,-2);


  lua_pushnil(L);  /* first key */
  int result = 0;
  while (lua_next(L, top_marker +1) != 0) {
    /* uses 'key' (at index -2) and 'value' (at index -1)  value is the function to call*/
    // prepare the call
    lua_pushstring(L,evt_name);// param 1 is the event
    for(int i = 0 ; i<nargs ;i++) { // event dependant parameters
      lua_pushvalue(L, top_marker -nargs +1 +i); 
    }
    int tmp_result = dt_lua_do_chunk(L,0,nargs+1,nresults);
    if(tmp_result > 0) {
      lua_pushvalue(L,-(tmp_result+1));//push index on top
      lua_remove(L,-(tmp_result+2));//remove old index still in stack
    }
    result += tmp_result;
    //result += dt_lua_do_chunk(L,0,nargs+1,nresults);
  }
  return result;
}


static int register_chained_event(lua_State* L) {
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

static int trigger_chained_event(lua_State * L,const char* evt_name, int nargs,int nresults) {
  // nresults is unused
  // -1..-n a
  lua_getfield(L,LUA_REGISTRYINDEX,"dt_lua_event_data");
  lua_getfield(L,-1,evt_name);
  if(lua_isnil(L,-1)) {
    lua_pop(L,2+nargs);
    for(int i = 0 ; i < nargs ; i++) lua_pushnil(L);
    return nargs;
  }
  lua_remove(L,-2);

  for(int i = 0 ; i< nargs; i++) {
    lua_pushvalue(L,-(nargs+1)); // copy all args over the table
  }

  lua_pushnil(L);  /* first key */
  while (lua_next(L, -(nargs+2)) != 0) {
    const int loop_index=luaL_checkint(L,-2);
    lua_remove(L,-2);
    /* uses 'key' (at index -2) and 'value' (at index -1) */
    // prepare the call
    lua_insert(L,-(nargs+1)); // move fn call below args
    lua_pushstring(L,evt_name);// param 1 is the event
    lua_insert(L,-(nargs+1)); // move evt name below params
    dt_lua_do_chunk(L,0,nargs+1,nargs);
    lua_pushinteger(L,loop_index);
  }
  lua_remove(L,-(nargs+1));
  return nargs;
}


static event_handler event_list[] = {
  {"pre-export",register_chained_event,trigger_chained_event},
  {"post-import-image",register_multiinstance_event,trigger_multiinstance_event},
  {"shortcut",register_shortcut_event,trigger_keyed_event}, 
  {"tmp-export-image",register_multiinstance_event,trigger_multiinstance_event},
  {"test",register_singleton_event,trigger_singleton_event},  // avoid error because of unused function
  {NULL,NULL,NULL}
};

static int lua_register_event(lua_State *L) {
  // 1 is event name
  const char*evt_name = luaL_checkstring(L,1);
  // 2 is event handler
  luaL_checktype(L,2,LUA_TFUNCTION);
  lua_getfield(L,LUA_REGISTRYINDEX,"dt_lua_event_list");
  lua_getfield(L,-1,evt_name);
  if(lua_isnil(L,-1)) {
    lua_pop(L,2);
    return luaL_error(L,"incorrect event type : %s\n",evt_name);
  }
  luaL_checktype(L,-1,LUA_TLIGHTUSERDATA);
  event_handler * handler =  lua_touserdata(L,-1);
  lua_pop(L,2); // restore the stack to only have the 3 parameters
  handler->on_register(L);
  return 0;

}

static int dt_lua_trigger_event_internal(const char*event,int nargs,int nresults) {
  lua_getfield(darktable.lua_state,LUA_REGISTRYINDEX,"dt_lua_event_list");
  if(lua_isnil(darktable.lua_state,-1)) {// events have been disabled
    lua_pop(darktable.lua_state,1+nargs);
    if(nresults== LUA_MULTRET) return 0;
    for(int i=0; i<nresults;i++) 
      lua_pushnil(darktable.lua_state);
    return nresults;
  }
  lua_getfield(darktable.lua_state,-1,event);
  luaL_checktype(darktable.lua_state,-1,LUA_TLIGHTUSERDATA);
  event_handler * handler =  lua_touserdata(darktable.lua_state,-1);
  lua_pop(darktable.lua_state,2);
  const int result = handler->on_event(darktable.lua_state,event,nargs,nresults);
  return result;

}
static int dt_lua_trigger_event(const char*event,int nargs,int nresults) {
  int res;
#pragma omp critical(running_lua)
  {
    res = dt_lua_trigger_event_internal(event,nargs,nresults);
  }
  return res;
}

static void on_export_selection(gpointer instance,dt_control_image_enumerator_t * export_descriptor,
     gpointer user_data){
  //dt_control_export_t *export_data= (dt_control_export_t*)export_descriptor->data;
  dt_lua_image_glist_push(darktable.lua_state,export_descriptor->index);
  g_list_free(export_descriptor->index);
  export_descriptor->index =NULL;
  dt_lua_trigger_event("pre-export",1,1);
  if(lua_isnoneornil(darktable.lua_state,-1)) {return; }// everything already has been removed
  export_descriptor->index = dt_lua_image_glist_get(darktable.lua_state,-1);
}

static void on_export_image_tmpfile(gpointer instance,
    int imgid,
    char *filename,
     gpointer user_data){
  dt_lua_image_push(darktable.lua_state,imgid);
  lua_pushstring(darktable.lua_state,filename);
  dt_lua_trigger_event("tmp-export-image",2,0);
}

static void on_image_imported(gpointer instance,uint8_t id, gpointer user_data){
  dt_lua_image_push(darktable.lua_state,id);
  dt_lua_trigger_event("post-import-image",1,0);
}

void dt_lua_init_events(lua_State *L) {
  printf("%s %d\n",__FUNCTION__,__LINE__);
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
  dt_control_signal_connect(darktable.signals,DT_SIGNAL_IMAGE_EXPORT_MULTIPLE,G_CALLBACK(on_export_selection),NULL);
  dt_control_signal_connect(darktable.signals,DT_SIGNAL_IMAGE_EXPORT_TMPFILE,G_CALLBACK(on_export_image_tmpfile),NULL);
}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
