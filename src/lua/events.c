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
#include "lua/dt_lua.h"
typedef struct event_handler{
	const char* evt_name;
	lua_CFunction on_register;
	int (*on_event)(lua_State * L,const char* evt_name, int nargs,int nresults);
} event_handler;


static int register_singleton_event(lua_State* L) {
	// 1 is the event name (checked)
	// 2 is the action to perform (checked)
	// 3 is the extra param (exist at this point)
	lua_getfield(L,LUA_REGISTRYINDEX,"dt_lua_event_data");
	lua_getfield(L,-1,lua_tostring(L,1));
	if(!lua_isnil(L,-1)) {
		lua_pop(L,2);
		return luaL_error(L,"an action has already been registered for event %s",lua_tostring(L,1));
	}
	lua_pop(L,1);
	lua_newtable(L);
	lua_pushvalue(L,2);
	lua_setfield(L,-2,"action");
	lua_pushvalue(L,3);
	lua_setfield(L,-2,"data");
	lua_setfield(L,-2,lua_tostring(L,1));
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
	lua_getfield(L,-1,"action"); // function to call
	lua_insert(L,top_marker-nargs+1);
	lua_pushstring(L,evt_name);// param 1 is the event
	lua_insert(L,top_marker-nargs+2);
	lua_getfield(L,-1,"data");// callback data
	lua_insert(L,top_marker-nargs+3);
	lua_pop(L,2);
	return dt_lua_do_chunk(L,0,nargs+2,nresults);
}

static int register_multiinstance_event(lua_State* L) {
	// 1 is the event name (checked)
	// 2 is the action to perform (checked)
	// 3 is the extra param (exist at this point)
	lua_getfield(L,LUA_REGISTRYINDEX,"dt_lua_event_data");
	lua_getfield(L,-1,lua_tostring(L,1));
	if(lua_isnil(L,-1)) {
		lua_pop(L,1);
		lua_newtable(L);
		lua_pushvalue(L,-1);
		lua_setfield(L,-3,lua_tostring(L,1));
	}

	lua_newtable(L);
	lua_pushvalue(L,2);
	lua_setfield(L,-2,"action");
	lua_pushvalue(L,3);
	lua_setfield(L,-2,"data");

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
		const int loop_index=luaL_checkint(L,-2);
		lua_remove(L,-2);
		/* uses 'key' (at index -2) and 'value' (at index -1) */
		// prepare the call
		lua_getfield(L,-1,"action"); // function to call
		lua_pushstring(L,evt_name);// param 1 is the event
		lua_getfield(L,-3,"data");// callback data
		lua_remove(L,-4);
		for(int i = 0 ; i<nargs ;i++) { // event dependant parameters
			lua_pushvalue(L, top_marker -nargs +1 +i); 
		}
		result += dt_lua_do_chunk(L,0,nargs+2,nresults);
		lua_pushinteger(L,loop_index);
	}
	for(int i=0; i < nargs+1;i++)
		lua_remove(L,top_marker-nargs+1); //the table of events + our params
	return result;
}


static int register_chained_event(lua_State* L) {
	// 1 is the event name (checked)
	// 2 is the action to perform (checked)
	// 3 is the extra param (exist at this point)
	lua_getfield(L,LUA_REGISTRYINDEX,"dt_lua_event_data");
	lua_getfield(L,-1,lua_tostring(L,1));
	if(lua_isnil(L,-1)) {
		lua_pop(L,1);
		lua_newtable(L);
		lua_pushvalue(L,-1);
		lua_setfield(L,-3,lua_tostring(L,1));
	}

	lua_newtable(L);
	lua_pushvalue(L,2);
	lua_setfield(L,-2,"action");
	lua_pushvalue(L,3);
	lua_setfield(L,-2,"data");

	luaL_ref(L,-2);
	lua_pop(L,2);
	return 0;
}

static int trigger_chained_event(lua_State * L,const char* evt_name, int nargs,int nresults) {
	// -1..-n are our args
	lua_getfield(L,LUA_REGISTRYINDEX,"dt_lua_event_data");
	lua_getfield(L,-1,evt_name);
	if(lua_isnil(L,-1)) {
		lua_pop(L,1);
		lua_newtable(L);
		lua_pushvalue(L,-1);
		lua_setfield(L,-3,lua_tostring(L,1));
	}
	lua_remove(L,-2);
	lua_insert(L,-nargs -1); // push the table below the args


	lua_pushnil(L);  /* first key */
	int lastnargs = nargs;
	while (lua_next(L, -(lastnargs+2)) != 0) {
		const int loop_index=luaL_checkint(L,-2);
		lua_remove(L,-2);
		/* uses 'key' (at index -2) and 'value' (at index -1) */
		// prepare the call
		lua_getfield(L,-1,"action"); // function to call
		lua_insert(L,-(nargs+2));
		lua_pushstring(L,evt_name);// param 1 is the event
		lua_insert(L,-(nargs+2));
		lua_getfield(L,-1,"data");// callback data
		lua_insert(L,-(nargs+2));
		lua_pop(L,1);
		lastnargs = dt_lua_do_chunk(L,0,nargs+2,nargs);
		lua_pushinteger(L,loop_index);
	}
	lua_remove(L,-lastnargs -1); //our data
	if(nresults == LUA_MULTRET) return lastnargs;
	else if(lastnargs < nresults) for(int i = lastnargs; i< nresults; i++) lua_pushnil(L);
	else if(lastnargs > nresults) for(int i = nresults; i< lastnargs; i++) lua_pop(L,1);

	return nresults;
}


static event_handler event_list[] = {
	//{"post-import-image",register_multiinstance_event,trigger_multiinstance_event},
	{"pre-export",register_chained_event,trigger_chained_event},
	{"post-import-image",register_multiinstance_event,trigger_multiinstance_event}, // avoid error because of unused function
	{"test",register_singleton_event,trigger_singleton_event}, // avoid error because of unused function
	{NULL,NULL,NULL}
};

static int lua_register_event(lua_State *L) {
	// 1 is event name
	const char*evt_name = luaL_checkstring(L,1);
	// 2 is event handler
	luaL_checktype(L,2,LUA_TFUNCTION);
	// 3 is an optional arg of any type
	if(lua_gettop(L) == 2) {
		lua_pushnil(L);
	}
	lua_getfield(L,LUA_REGISTRYINDEX,"dt_lua_event_list");
	lua_getfield(L,-1,evt_name);
	if(lua_isnil(L,-1)) {
		lua_pop(L,3);
		return luaL_error(L,"incorrect event type : %s\n",evt_name);
	}
	luaL_checktype(L,-1,LUA_TLIGHTUSERDATA);
	event_handler * handler =  lua_touserdata(L,-1);
	lua_pop(L,2); // restore the stack to only have the 3 parameters
	handler->on_register(L);
	return 0;


}

int dt_lua_trigger_event(const char*event,int nargs,int nresults) {
	lua_getfield(darktable.lua_state,LUA_REGISTRYINDEX,"dt_lua_event_list");
	lua_getfield(darktable.lua_state,-1,event);
	luaL_checktype(darktable.lua_state,-1,LUA_TLIGHTUSERDATA);
	event_handler * handler =  lua_touserdata(darktable.lua_state,-1);
	lua_pop(darktable.lua_state,2);
	const int result = handler->on_event(darktable.lua_state,event,nargs,nresults);
	return result;
	
}


void dt_lua_init_events(lua_State *L) {
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
		lua_pushstring(L,"register_event");
		lua_pushcfunction(L,&lua_register_event);
		lua_settable(L,-3);
}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
