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
#include "lua/image.h"
#include "common/colorlabels.h"
#include "common/history.h"
#include "common/film.h"
#include "lua/stmt.h"

typedef struct {
	const char* evt_name;
	lua_CFunction on_register;
	lua_CFunction on_event;
} event_handler;

static dt_lua_type* types[] = {
	&dt_lua_stmt,
	&dt_colorlabels_lua_type,
	&dt_history_lua_type,
	&dt_lua_image,
	&dt_lua_images,
	NULL
};
static int do_chunk(lua_State *L,int loadresult,int nargs,int nresult) {
	int result;
	if(loadresult){
		dt_control_log("LUA ERROR %s",lua_tostring(L,-1));
		dt_print(DT_DEBUG_LUA,"LUA ERROR %s\n",lua_tostring(L,-1));
		lua_pop(L,1);
		return 0;
	}
	result = lua_gettop(L)-(nargs+1); // remember the stack size to findout the number of results in case of multiret
	if(lua_pcall(L, nargs, nresult,0)) {
		dt_control_log("LUA ERROR %s\n",lua_tostring(L,-1));
		dt_print(DT_DEBUG_LUA,"LUA ERROR %s\n",lua_tostring(L,-1));
		lua_pop(L,1);
	}
	result= lua_gettop(L) -result;

	dt_lua_type** cur_type = types;
	while(*cur_type) {
		if((*cur_type)->clean) {
			lua_pushcfunction(darktable.lua_state,(*cur_type)->clean);
			if(lua_pcall(darktable.lua_state, 0, 0, 0)) {
				dt_control_log("LUA ERROR while cleaning %s : %s\n",(*cur_type)->name,lua_tostring(darktable.lua_state,-1));
				dt_print(DT_DEBUG_LUA,"LUA ERROR while cleaning %s : %s\n",(*cur_type)->name,lua_tostring(darktable.lua_state,-1));
				lua_pop(darktable.lua_state,1);
			}
		}
		cur_type++;
	}
	lua_gc(darktable.lua_state,LUA_GCCOLLECT,0);
	return result;
}
// closed on GC of the dt lib, usually when the lua interpreter closes
static int dt_luacleanup(lua_State*L) {
	const int init_gui = (darktable.gui != NULL);
	dt_film_remove_empty();
	if(!init_gui)
		dt_cleanup();
	return 0;
}

static int lua_quit(lua_State *L) {
	dt_control_quit();
	return 0;
}

static int lua_print(lua_State *L) {
	const int init_gui = (darktable.gui != NULL);
	if(init_gui)
		dt_control_log("%s",luaL_checkstring(L,-1));
	else
		printf("%s\n",luaL_checkstring(L,-1));

	return 0;
}
#if 0
static void debug_table(lua_State * L,int t) {
   /* table is in the stack at index 't' */
     lua_pushnil(L);  /* first key */
     while (lua_next(L, t-1) != 0) {
       /* uses 'key' (at index -2) and 'value' (at index -1) */
       printf("%s - %s\n",
		       luaL_checkstring(L,-2),
              lua_typename(L, lua_type(L, -1)));
       /* removes 'value'; keeps 'key' for next iteration */
       lua_pop(L, 1);
     }
}
#endif
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

static int trigger_multiinstance_event(lua_State* L) {
	// -1 is the event name;
	const char * evt_name =  lua_tostring(L,-1);
	// -2 is nargs
	const int nargs = lua_tointeger(L,-2);
	// -3 is nresults
	const int nresults = lua_tointeger(L,-3);
	// -4..-n are our args
	const int top_marker=lua_gettop(L);
	lua_getfield(L,LUA_REGISTRYINDEX,"dt_lua_event_data");
	lua_getfield(L,-1,evt_name);
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
		for(int i = 1 ; i<=nargs ;i++) { // event dependant parameters
			lua_pushvalue(L, top_marker -3-nargs +i); 
		}
		result += do_chunk(L,0,nargs+2,nresults);
		lua_pushinteger(L,loop_index);
	}
	lua_remove(L,top_marker+1); //our data
	return result;
}


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

static int trigger_singleton_event(lua_State* L) {
	// -1 is the event name;
	const char * evt_name =  lua_tostring(L,-1);
	// -2 is nargs
	const int nargs = lua_tointeger(L,-2);
	// -3 is nresults
	const int nresults = lua_tointeger(L,-3);
	// -4..-n are our args
	const int top_marker=lua_gettop(L);
	lua_getfield(L,LUA_REGISTRYINDEX,"dt_lua_event_data");
	lua_getfield(L,-1,evt_name);
	// prepare the call
	lua_getfield(L,-1,"action"); // function to call
	lua_pushstring(L,evt_name);// param 1 is the event
	lua_getfield(L,-3,"data");// callback data
	lua_remove(L,-5);
	lua_remove(L,-4);
	for(int i = 0 ; i<nargs ;i++) { // event dependant parameters
		lua_pushvalue(L, top_marker -3-nargs +i); 
	}
	return do_chunk(L,0,nargs+2,nresults);
}



static event_handler event_list[] = {
	{"post-import-image",register_multiinstance_event,trigger_multiinstance_event},
	{"post-export-image",register_multiinstance_event,trigger_multiinstance_event},
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

int dt_lua_trigger_event(const char*event,int nargs,int nresult) {
	lua_getfield(darktable.lua_state,LUA_REGISTRYINDEX,"dt_lua_event_list");
	lua_getfield(darktable.lua_state,-1,event);
	luaL_checktype(darktable.lua_state,-1,LUA_TLIGHTUSERDATA);
	event_handler * handler =  lua_touserdata(darktable.lua_state,-1);
	lua_pop(darktable.lua_state,2);
	lua_pushinteger(darktable.lua_state,nresult);
	lua_pushinteger(darktable.lua_state,nargs);
	lua_pushstring(darktable.lua_state,event);
	const int result = handler->on_event(darktable.lua_state);
	lua_remove(darktable.lua_state,-(result +1));
	return result;
	
}


static int load_darktable_lib(lua_State *L) {
	const int init_gui = (darktable.gui != NULL);
	lua_newtable(L);
	// set the metatable
	lua_newtable(L);
	lua_pushcfunction(L,dt_luacleanup);
	lua_setfield(L,-2,"__gc");
	lua_setmetatable(L,-2);

	if(init_gui) {
		lua_pushstring(L,"quit");
		lua_pushcfunction(L,&lua_quit);
		lua_settable(L,-3);

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
	
	lua_pushstring(L,"import");
	lua_pushcfunction(L,&dt_film_import_lua);
	lua_settable(L,-3);
	
	lua_pushstring(L,"print");
	lua_pushcfunction(L,&lua_print);
	lua_settable(L,-3);
	
	dt_lua_type** cur_type = types;
	char tmp[1024];
	while(*cur_type) {
		snprintf(tmp,1024,"dt_lua_%s",(*cur_type)->name);
		lua_pushstring(L,(*cur_type)->name);
		lua_pushcfunction(L,(*cur_type)->load);
		luaL_newmetatable(L,tmp);
		if(lua_pcall(L, 1, 1, 0)) {
			dt_print(DT_DEBUG_LUA,"LUA ERROR while loading type %s : %s\n",(*cur_type)->name,lua_tostring(L,-1));
			lua_pop(L,1);
		}
		lua_settable(L,-3); // attach the object (if any) to the name
		cur_type++;
	}

	return 1;
}

static const luaL_Reg loadedlibs[] = {
  {"_G", luaopen_base},
  {LUA_LOADLIBNAME, luaopen_package},
  {"darktable", load_darktable_lib},
  {NULL, NULL}
};


/*
** these libs are preloaded and must be required before used
*/
static const luaL_Reg preloadedlibs[] = {
  {LUA_COLIBNAME, luaopen_coroutine},
  {LUA_TABLIBNAME, luaopen_table},
  {LUA_IOLIBNAME, luaopen_io},
  {LUA_OSLIBNAME, luaopen_os},
  {LUA_STRLIBNAME, luaopen_string},
  {LUA_BITLIBNAME, luaopen_bit32},
  {LUA_MATHLIBNAME, luaopen_math},
  //{LUA_DBLIBNAME, luaopen_debug},
  {NULL, NULL}
};


void dt_lua_init() {
	// init the global lua context
	darktable.lua_state= luaL_newstate();
	const luaL_Reg *lib;
	/* call open functions from 'loadedlibs' and set results to global table */
	for (lib = loadedlibs; lib->func; lib++) {
		luaL_requiref(darktable.lua_state, lib->name, lib->func, 1);
		lua_pop(darktable.lua_state, 1);  /* remove lib */
	}
	/* add open functions from 'preloadedlibs' into 'package.preload' table */
	luaL_getsubtable(darktable.lua_state, LUA_REGISTRYINDEX, "_PRELOAD");
	for (lib = preloadedlibs; lib->func; lib++) {
		lua_pushcfunction(darktable.lua_state, lib->func);
		lua_setfield(darktable.lua_state, -2, lib->name);
	}
	lua_pop(darktable.lua_state, 1);  /* remove _PRELOAD table */
	
	char configdir[PATH_MAX],lua_path[PATH_MAX];
	dt_loc_get_user_config_dir(configdir, PATH_MAX);
	g_snprintf(lua_path, PATH_MAX, "%s/lua_init", configdir);
	if (g_file_test(lua_path, G_FILE_TEST_IS_DIR)) {
		GError * error;
		GDir * lua_dir = g_dir_open(lua_path,0,&error);
		if(error) {
			dt_print(DT_DEBUG_LUA,"error opening %s : %s\n",lua_path,error->message);
		} else {
			gchar *tmp;
			const gchar * filename = g_dir_read_name(lua_dir);
			while(filename) {
				tmp = g_strconcat(lua_path,"/",filename,NULL);
				if (g_file_test(tmp, G_FILE_TEST_IS_REGULAR) && filename[0] != '.') {
					do_chunk(darktable.lua_state,luaL_loadfile(darktable.lua_state,tmp),0,0);
				}
				g_free(tmp);
				filename = g_dir_read_name(lua_dir);
			}
		}
		g_dir_close(lua_dir);
	}

}


void dt_lua_dostring(const char* command) {
      do_chunk(darktable.lua_state,luaL_loadstring(darktable.lua_state, command),0,0);
}

static int char_list_next(lua_State *L){
	int index;
	const char **list = lua_touserdata(L,lua_upvalueindex(1));
	if(lua_isnil(L,-1)) {
		index = 0;
	} else {
		index = luaL_checkoption(L,-1,NULL,list);
		index++;
	}
	lua_pop(L,1); // pop the key
	if(!list[index]) { // no need to test < 0 or > max, luaL_checkoption catches it for us
		return 0;
	} else {
		if (!luaL_getmetafield(L, -1, "__index"))  /* no metafield? */
			luaL_error(L,"object doesn't have an __index method"); // should never happen
		lua_pushvalue(L,-2);// the object called
		lua_pushstring(L,list[index]); // push the index string
		lua_call(L, 2, 1);
		lua_pushstring(L,list[index]); // push the index string
		lua_insert(L,-2); // move the index string below the value object
		return 2;
	}
}
static int char_list_pairs(lua_State *L){
	// one upvalue, the lightuserdata 
	const char **list = lua_touserdata(L,lua_upvalueindex(1));
	lua_pushlightuserdata(L,list);
	lua_pushcclosure(L,char_list_next,1);
	lua_pushvalue(L,-2);
	lua_pushnil(L); // index set to null for reset
	return 3;
}

void dt_lua_init_name_list_pair(lua_State* L, const char **list){
	lua_pushlightuserdata(L,list);
	lua_pushcclosure(L,char_list_pairs,1);
	lua_setfield(L,-2,"__pairs");
}

void dt_lua_init_singleton(lua_State* L){
	// add a new table to keep ref to allocated objects
	lua_newtable(L);
	// add a metatable to that table, just for the __mode field
	lua_newtable(L);
	lua_pushstring(L,"v");
	lua_setfield(L,-2,"__mode");
	lua_setmetatable(L,-2);
	// attach the ref table
	lua_setfield(L,-2,"allocated");
}



int dt_lua_singleton_find(lua_State* L,int id,const dt_lua_type*type) {
	char tmp[1024];
	snprintf(tmp,1024,"dt_lua_%s",type->name);
	// ckeck if colorlabel already is in the env
	// get the metatable and put it on top (side effect of newtable)
	luaL_newmetatable(L,tmp);
	lua_getfield(L,-1,"allocated");
	lua_pushinteger(L,id);
	lua_gettable(L,-2);
	// at this point our stack is :
	// -1 : the object or nil if it is not allocated
	// -2 : the allocation table
	// -3 : the metatable
	if(!lua_isnil(L,-1)) {
		lua_remove(L,-3);
		lua_remove(L,-2);
		return 1;
	} else {
		lua_pop(L,3);
		return 0;
	}
}

void dt_lua_singleton_register(lua_State* L,int id,const dt_lua_type*type ){
	char tmp[1024];
	snprintf(tmp,1024,"dt_lua_%s",type->name);
	// ckeck if colorlabel already is in the env
	// get the metatable and put it on top (side effect of newtable)
	luaL_newmetatable(L,tmp);
	lua_getfield(L,-1,"allocated");
	lua_pushinteger(L,id);
	lua_gettable(L,-2);
	// at this point our stack is :
	// -1 : the object or nil if it is not allocated
	// -2 : the allocation table
	// -3 : the metatable
	// -4 : the object to register
	if(!lua_isnil(L,-1)) {
		luaL_error(L,"double registration for type %s with id %d",tmp,id);
	}
	lua_pushinteger(L,id);
	lua_pushvalue(L,-5);
	luaL_setmetatable(L,tmp);
	lua_settable(L,-4);
	lua_pop(L,3);
}
void *dt_lua_check(lua_State* L,int index,const dt_lua_type*type) {
	char tmp[1024];
	snprintf(tmp,1024,"dt_lua_%s",type->name);
	return luaL_checkudata(L,index,tmp);
}
void dt_lua_singleton_foreach(lua_State*L,const dt_lua_type* type,lua_CFunction function){
	char tmp[1024];
	snprintf(tmp,1024,"dt_lua_%s",type->name);
	luaL_newmetatable(L,tmp);
	lua_getfield(L,-1,"allocated");
	lua_pushnil(L);
	while (lua_next(L, -2) != 0) {
		/* image is at top, imgid is just under */
		lua_pushcfunction(L,function);
		lua_pushvalue(L,-2);
		lua_call(L,1,0); 
		/* remove the image, for the call to lua_next */
		lua_pop(L, 1);
	}
}


// function used by the lua interpreter to load darktable
int luaopen_darktable(lua_State *L) {
	int tmp_argc = 0;
	char *tmp_argv[]={"lua",NULL};
	char **tmp_argv2 = &tmp_argv[0];
	gtk_init (&tmp_argc, &tmp_argv2);
	char *m_arg[] = {"darktable-cli", "--library", ":memory:", NULL};
	// init dt without gui:
	if(dt_init(3, m_arg, 0)) exit(1);
	load_darktable_lib(L);
	return 1;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
