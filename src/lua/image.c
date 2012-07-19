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

#include "lua/image.h"
#include "lua/stmt.h"
#include "common/darktable.h"
#include "common/debug.h"

/***********************************************************************
  handling of dt_image_t
  **********************************************************************/
#define LUA_IMAGE "dt_lua_image"
typedef struct {
	int imgid;
} lua_image;

static int dt_lua_image_tostring(lua_State *L) {
	//printf("%s %d\n",__FUNCTION__,dt_lua_checkimage(L,-1));
	lua_pushinteger(L,dt_lua_checkimage(L,-1));
	return 1;
}

void dt_lua_push_image(lua_State * L,int imgid) {
	lua_image * my_image = (lua_image*)lua_newuserdata(L,sizeof(lua_image));
	luaL_setmetatable(L,LUA_IMAGE);
	my_image->imgid=imgid;
}

int dt_lua_checkimage(lua_State * L,int index){
	lua_image* my_image= luaL_checkudata(L,index,LUA_IMAGE);
	return my_image->imgid;
}

static const luaL_Reg dt_lua_image_meta[] = {
	{"__tostring", dt_lua_image_tostring },
	{0,0}
};
void dt_lua_init_image(lua_State * L) {
	luaL_newmetatable(L,LUA_IMAGE);
	luaL_setfuncs(L,dt_lua_image_meta,0);
	lua_pop(L,1);
}
/***********************************************************************
  Creating the images global variable
  **********************************************************************/
static int images_next(lua_State *L) {
	//printf("%s\n",__FUNCTION__);
	//TBSL : check index and find the correct position in stmt if index was changed manually
	// 2 args, table, index, returns the next index,value or nil,nil return the first index,value if index is nil 
	
	sqlite3_stmt *stmt = dt_lua_checkstmt(L,-2);
	if(lua_isnil(L,-1)) {
		sqlite3_reset(stmt);
	} 
	int result = sqlite3_step(stmt);
	if(result != SQLITE_ROW){
		lua_pushnil(L);
		lua_pushnil(L);
		return 2;
	}
	int imgid = sqlite3_column_int(stmt,0);
	lua_pushinteger(L,imgid);
	dt_lua_push_image(L,imgid);
	return 2;
}


static int images_pairs(lua_State *L) {
	lua_pushcfunction(L,images_next);
	sqlite3_stmt *stmt;
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select id from images", -1, &stmt, NULL);
	dt_lua_push_stmt(L,stmt);
	lua_pushnil(L); // index set to null for reset
	return 3;
}
static const luaL_Reg stmt_meta[] = {
	{"__pairs", images_pairs },
	{0,0}
};
void dt_lua_images_init(lua_State * L) {
	lua_newuserdata(L,1); // placeholder
	luaL_newlib(L,stmt_meta);
	lua_setmetatable(L,-2);
	lua_setfield(L,-2,"images");
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
