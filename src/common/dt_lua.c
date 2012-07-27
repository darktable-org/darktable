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
#include "lua/colorlabel.h"
#include "lua/stmt.h"

static int lua_quit(lua_State *state) {
	dt_control_quit();
	return 0;
}

static dt_lua_type* types[] = {
	&dt_lua_stmt,
	&dt_lua_colorlabel,
	&dt_lua_image,
	&dt_lua_images,
	NULL
};

static int load_darktable_lib(lua_State *L) {
	lua_newtable(L);
	lua_pushstring(L,"quit");
	lua_pushcfunction(L,&lua_quit);
	lua_settable(L,-3);
	
	dt_lua_type** cur_type = types;
	while(*cur_type) {
		lua_pushstring(L,(*cur_type)->name);
		(*cur_type)->load(L);
		lua_settable(L,-3);
		cur_type++;
	}

	return 1;
}
static void do_chunck(int loadresult) {
	if(loadresult){
		dt_control_log("LUA ERROR %s\n",lua_tostring(darktable.lua_state,-1));
		printf("LUA ERROR %s\n",lua_tostring(darktable.lua_state,-1));
		lua_pop(darktable.lua_state,1);
		return;
	}
	// change the env variable here to a copy of _G
	if(lua_pcall(darktable.lua_state, 0, 0, 0)) {
		dt_control_log("LUA ERROR %s\n",lua_tostring(darktable.lua_state,-1));
		printf("LUA ERROR %s\n",lua_tostring(darktable.lua_state,-1));
		lua_pop(darktable.lua_state,1);
	}

	dt_lua_type** cur_type = types;
	while(*cur_type) {
		if((*cur_type)->clean)
			(*cur_type)->clean(darktable.lua_state);
		cur_type++;
	}
	lua_gc(darktable.lua_state,LUA_GCCOLLECT,0);
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
			printf("error opening %s : %s\n",lua_path,error->message);
		} else {
			gchar *tmp;
			const gchar * filename = g_dir_read_name(lua_dir);
			while(filename) {
				tmp = g_strconcat(lua_path,"/",filename,NULL);
				if (g_file_test(tmp, G_FILE_TEST_IS_REGULAR) && filename[0] != '.') {
					do_chunck(luaL_loadfile(darktable.lua_state,tmp));
				}
				g_free(tmp);
				filename = g_dir_read_name(lua_dir);
			}
		}
		g_dir_close(lua_dir);
	}

}


void dt_lua_dostring(const char* command) {
      do_chunck(luaL_loadstring(darktable.lua_state, command));
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
