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
#include "lua/stmt.h"
#include <lauxlib.h>
#include <stdio.h>
static int stmt_tostring(lua_State *L) {
	printf("%s\n",__FUNCTION__);
	lua_pushfstring(L,"%s",__FUNCTION__);
	return 1;
}
static int stmt_next(lua_State *L) {
	// 2 args, table, index, returns the next index,value or nil,nil return the first index,value if index is nil 
	printf("%s\n",__FUNCTION__);
	sqlite3_stmt *stmt = lua_touserdata(L,-2);
	int result = sqlite3_step(stmt);
	if(result != SQLITE_ROW){
		lua_pushnil(L);
		lua_pushnil(L);
		return 2;
	}
	//TBSL : column 1 is imgid
	printf("imgid %d\n",sqlite3_column_int(stmt,0));
	lua_pushinteger(L,sqlite3_column_int(stmt,0));
	lua_pushnil(L);
	return 2;
}

static int stmt_gc(lua_State *L) {
	printf("%s\n",__FUNCTION__);
	sqlite3_stmt *stmt = lua_touserdata(L,lua_upvalueindex(1));
	sqlite3_finalize(stmt);
	return 0;
}

static int stmt_pairs(lua_State *L) {
	printf("%s\n",__FUNCTION__);
	sqlite3_stmt *stmt = lua_touserdata(L,lua_upvalueindex(1));
	sqlite3_reset(stmt);
	lua_pushcfunction(L,stmt_next);
	lua_pushvalue(L,lua_upvalueindex(1));
	lua_pushnil(L);
	return 3;
}
static const luaL_Reg stmt_meta[] = {
	{"__tostring", stmt_tostring },
	{"__pairs", stmt_pairs },
	{"__gc", stmt_gc },
	{0,0}
};
void dt_lua_stmt_pseudo_array(lua_State * L,sqlite3_stmt *stmt) {
	lua_newuserdata(L,1); // placeholder, we are interested in 
	luaL_newlibtable(L,stmt_meta);
	lua_pushlightuserdata(L,stmt);
	luaL_setfuncs(L,stmt_meta,1);
	lua_setmetatable(L,-2);
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
