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
#include "common/film.h"
#include "control/control.h"
#include "lua/stmt.h"
#include "lua/events.h"

int dt_lua_do_chunk(lua_State *L,int loadresult,int nargs,int nresults) {
	int result;
	if(loadresult){
		dt_control_log("LUA ERROR %s",lua_tostring(L,-1));
		dt_print(DT_DEBUG_LUA,"LUA ERROR %s\n",lua_tostring(L,-1));
		lua_pop(L,1);
		return 0;
	}
	result = lua_gettop(L)-(nargs+1); // remember the stack size to findout the number of results in case of multiret
	if(lua_pcall(L, nargs, nresults,0)) {
		dt_control_log("LUA ERROR %s",lua_tostring(L,-1));
		dt_print(DT_DEBUG_LUA,"LUA ERROR %s\n",lua_tostring(L,-1));
		lua_pop(L,1);
	}
	result= lua_gettop(L) -result;

	dt_lua_push_type_table(L);
	lua_pushnil(L);  /* first key */
	while (lua_next(L, -2) != 0) {
		dt_lua_type * type = lua_touserdata(L,-1);
		lua_pop(L, 1);
		if(type->clean) {
			lua_pushcfunction(darktable.lua_state,type->clean);
			if(lua_pcall(darktable.lua_state, 0, 0, 0)) {
				dt_control_log("LUA ERROR while cleaning %s : %s",type->name,lua_tostring(darktable.lua_state,-1));
				dt_print(DT_DEBUG_LUA,"LUA ERROR while cleaning %s : %s\n",type->name,lua_tostring(darktable.lua_state,-1));
				lua_pop(darktable.lua_state,1);
			}
		}
	}
	lua_pop(L,1);
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
	printf("%d\n",__LINE__);
	for(int i=1 ;i<=lua_gettop(L);i++) printf("\t%s\n",lua_typename(L,lua_type(L,i)));
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

static int load_darktable_lib(lua_State *L) {
	lua_newtable(L);
	lua_pushvalue(L,-1);
	lua_setfield(L,LUA_REGISTRYINDEX,"dt_lua_dtlib");
	// set the metatable
	lua_newtable(L);
	lua_pushcfunction(L,dt_luacleanup);
	lua_setfield(L,-2,"__gc");
	lua_setmetatable(L,-2);

	
	lua_pushstring(L,"import");
	lua_pushcfunction(L,&dt_film_import_lua);
	lua_settable(L,-3);
	
	lua_pushstring(L,"print");
	lua_pushcfunction(L,&lua_print);
	lua_settable(L,-3);
	
	dt_lua_init_types(L);

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

	// add stuff that is here only for gui
	lua_getfield(darktable.lua_state,LUA_REGISTRYINDEX,"dt_lua_dtlib");
	lua_pushstring(darktable.lua_state,"quit");
	lua_pushcfunction(darktable.lua_state,&lua_quit);
	lua_settable(darktable.lua_state,-3);

	dt_lua_init_events(darktable.lua_state);

	// find the darktable library we have loaded previously
}
void dt_lua_run_init() {
	char configdir[PATH_MAX];
	dt_loc_get_user_config_dir(configdir, PATH_MAX);
	g_strlcat(configdir,"/lua_init",PATH_MAX);
	if (g_file_test(configdir, G_FILE_TEST_IS_DIR)) {
		GError * error;
		GDir * lua_dir = g_dir_open(configdir,0,&error);
		if(!lua_dir) {
			dt_print(DT_DEBUG_LUA,"error opening %s : %s\n",configdir,error->message);
		} else {
			gchar *tmp;
			const gchar * filename = g_dir_read_name(lua_dir);
			while(filename) {
				tmp = g_strconcat(configdir,"/",filename,NULL);
				if (g_file_test(tmp, G_FILE_TEST_IS_REGULAR) && filename[0] != '.') {
					dt_lua_do_chunk(darktable.lua_state,luaL_loadfile(darktable.lua_state,tmp),0,0);
				}
				g_free(tmp);
				filename = g_dir_read_name(lua_dir);
			}
		}
		g_dir_close(lua_dir);
	}

}


void dt_lua_dostring(const char* command) {
      dt_lua_do_chunk(darktable.lua_state,luaL_loadstring(darktable.lua_state, command),0,0);
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
