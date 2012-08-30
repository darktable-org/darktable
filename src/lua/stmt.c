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

#define LUA_STMT "dt_lua_stmt"
typedef struct {
	sqlite3_stmt *stmt;
} lua_stmt;

static int stmt_gc(lua_State *L) {
	//printf("%s\n",__FUNCTION__);
	sqlite3_stmt *stmt = dt_lua_stmt_check(L,-1);
	sqlite3_finalize(stmt);
	return 0;
}

static const luaL_Reg stmt_meta[] = {
	{"__gc", stmt_gc },
	{0,0}
};

void dt_lua_stmt_push(lua_State * L,sqlite3_stmt *stmt) {
	//printf("%s\n",__FUNCTION__);
	lua_stmt * my_stmt = (lua_stmt*)lua_newuserdata(L,sizeof(lua_stmt));
	luaL_setmetatable(L,LUA_STMT);
	my_stmt->stmt=stmt;
}

sqlite3_stmt* dt_lua_stmt_check(lua_State * L,int index){
	lua_stmt* my_stmt= luaL_checkudata(L,index,LUA_STMT);
	return my_stmt->stmt;
}

static int init_stmt(lua_State * L) {
	luaL_setfuncs(L,stmt_meta,0);
	return 0;
}

dt_lua_type dt_lua_stmt = {
	"stmt",
	init_stmt,
	NULL	
};

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
