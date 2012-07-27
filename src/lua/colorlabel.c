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

#include "lua/colorlabel.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/colorlabels.h"

#define LUA_COLORLABEL "dt_lua_colorlabel"
typedef struct {
	int imgid;
} colorlabel_type;

int dt_lua_colorlabel_check(lua_State * L,int index){
	return ((colorlabel_type*)luaL_checkudata(L,index,LUA_COLORLABEL))->imgid;
}

void dt_lua_colorlabel_push(lua_State * L,int imgid) {
	// ckeck if colorlabel already is in the env
	// get the metatable and put it on top (side effect of newtable)
	luaL_newmetatable(L,LUA_COLORLABEL);
	lua_pushinteger(L,imgid);
	lua_gettable(L,-2);
	if(!lua_isnil(L,-1)) {
		//printf("%s %d (reuse)\n",__FUNCTION__,imgid);
		dt_lua_colorlabel_check(L,-1);
		lua_remove(L,-2); // remove the table, but leave the colorlabel on top of the stac
		return;
	} else {
		//printf("%s %d (create)\n",__FUNCTION__,imgid);
		lua_pop(L,1); // remove nil at top
		lua_pushinteger(L,imgid);
		colorlabel_type * my_colorlabel = (colorlabel_type*)lua_newuserdata(L,sizeof(colorlabel_type));
		luaL_setmetatable(L,LUA_COLORLABEL);
		my_colorlabel->imgid =imgid;
		// add the value to the metatable, so it can be reused
		lua_settable(L,-3);
		// put the value back on top
		lua_pushinteger(L,imgid);
		lua_gettable(L,-2);
		lua_remove(L,-2); // remove the table, but leave the colorlabel on top of the stac
	}
}


static int colorlabel_index(lua_State *L){
	int imgid=dt_lua_colorlabel_check(L,-2);
	const int value =luaL_checkoption(L,-1,NULL,dt_colorlabels_name);
	if(value < 0 || value >= DT_COLORLABELS_LAST) {
			return luaL_error(L,"should never happen %s",lua_tostring(L,-1));
	} else {
		lua_pushboolean(L,dt_colorlabels_check_label(imgid,value));
		return 1;
	}
}

static int colorlabel_newindex(lua_State *L){
	int imgid=dt_lua_colorlabel_check(L,-3);
	const int value =luaL_checkoption(L,-2,NULL,dt_colorlabels_name);
	if(value < 0 || value >= DT_COLORLABELS_LAST) {
			return luaL_error(L,"should never happen %s",lua_tostring(L,-1));
	} else {
			if(lua_toboolean(L,-1)) { // no testing of type so we can benefit from all types of values
				dt_colorlabels_set_label(imgid,value);
			} else {
				dt_colorlabels_remove_label(imgid,value);
			}
			return 0;

	}
}

static int colorlabel_next(lua_State *L){
	//printf("%s\n",__FUNCTION__);
	int index;
	if(lua_isnil(L,-1)) {
		index = 0;
	} else {
		index = luaL_checkoption(L,-1,NULL,dt_colorlabels_name);
	}
	index++;
	if(!dt_colorlabels_name[index]) {
		lua_pushnil(L);
		lua_pushnil(L);
	} else {
		lua_pop(L,1);// remove the key, table is at top
		lua_pushstring(L,dt_colorlabels_name[index]); // push the index string
		colorlabel_index(L);
	}
	return 2;
}
static int colorlabel_pairs(lua_State *L){
	lua_pushcfunction(L,colorlabel_next);
	lua_pushvalue(L,-2);
	lua_pushnil(L); // index set to null for reset
	return 3;
}
static const luaL_Reg dt_lua_colorlabel_meta[] = {
	{"__index", colorlabel_index },
	{"__newindex", colorlabel_newindex },
	{"__pairs", colorlabel_pairs },
	{0,0}
};
static int colorlabel_init(lua_State * L) {
	luaL_newmetatable(L,LUA_COLORLABEL);
	luaL_setfuncs(L,dt_lua_colorlabel_meta,0);
	// add a metatable to the metatable, just for the __mode field
	lua_newtable(L);
	lua_pushstring(L,"v");
	lua_setfield(L,-2,"__mode");
	lua_setmetatable(L,-2);
	//pop the metatable itself to be clean
	lua_pop(L,1);
	//loader convention, we declare a type but we don't create any function
	lua_pushnil(L);
	return 1;
}


dt_lua_type dt_lua_colorlabel ={
	"colorlabels",
	colorlabel_init,
	NULL
};

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
