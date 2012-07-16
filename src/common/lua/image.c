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

#include "common/lua/image.h"
#include "common/darktable.h"

/***********************************************************************
  images global
  **********************************************************************/
static int images_tostring(lua_State *L) {
	lua_pushfstring(L,"%s",__FUNCTION__);
	return 1;
}

static int images_next(lua_State *L) {
	// 2 args, table, index, returns the next index or nil, return the first index if index is nil 
	printf("%s\n",__FUNCTION__);
	lua_pushnil(L); //TBSL
	lua_pushnil(L); //TBSL
	return 2;
}
static int images_pairs(lua_State *L) {
	printf("%s\n",__FUNCTION__);
	lua_pushcfunction(L,images_next);
	lua_pushvalue(L,-2);
	lua_pushnil(L);
	return 3;
}
static const luaL_Reg images_meta[] = {
{"__tostring", images_tostring },
{"__pairs", images_pairs },
{0,0}
};
/***********************************************************************
  Registering everything
  **********************************************************************/
void dt_lua_image_init(lua_State * L) {
	lua_pushstring(L,"images");
	lua_newuserdata(L,1); // placeholder, we are interested in 
	luaL_newlib(L,images_meta);
	lua_setmetatable(L,-2);
	lua_settable(L,-3);
	lua_pop(darktable.lua_state,1);
}
