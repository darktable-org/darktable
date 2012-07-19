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
#include "control/control.h"
#include "lua/image.h"
#include "lua/stmt.h"
#include <lualib.h>

static int lua_quit(lua_State *state) {
	dt_control_quit();
	return 0;
}

void dt_lua_init() {
	// init the global lua context
	darktable.lua_state= luaL_newstate();
	luaopen_base(darktable.lua_state);
	luaopen_table(darktable.lua_state);
	luaopen_string(darktable.lua_state);
	luaopen_math(darktable.lua_state);
	lua_rawgeti(darktable.lua_state, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS);
	lua_pushstring(darktable.lua_state,"quit");
	lua_pushcfunction(darktable.lua_state,&lua_quit);
	lua_settable(darktable.lua_state,-3);
	dt_lua_init_stmt(darktable.lua_state);
	dt_lua_init_image(darktable.lua_state);
	dt_lua_images_init(darktable.lua_state);
	lua_pop(darktable.lua_state,1);
}



// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
