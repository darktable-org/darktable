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

void dt_lua_init_numid(lua_State* L){
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



int dt_lua_numid_find(lua_State* L,int id,const char*type_name) {
	// get the metatable and put it on top (side effect of newtable)
	luaL_newmetatable(L,type_name);
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

void dt_lua_numid_register(lua_State* L,int id,const char *type_name ){
	// get the metatable and put it on top (side effect of newtable)
	luaL_newmetatable(L,type_name);
	lua_getfield(L,-1,"allocated");
	lua_pushinteger(L,id);
	lua_gettable(L,-2);
	// at this point our stack is :
	// -1 : the object or nil if it is not allocated
	// -2 : the allocation table
	// -3 : the metatable
	// -4 : the object to register
	if(!lua_isnil(L,-1)) {
		luaL_error(L,"double registration for type %s with id %d",type_name,id);
	}
	lua_pushinteger(L,id);
	lua_pushvalue(L,-5);
	luaL_setmetatable(L,type_name);
	lua_settable(L,-4);
	lua_pop(L,3);
}
void dt_lua_numid_foreach(lua_State*L,const char* type_name,lua_CFunction function){
	luaL_newmetatable(L,type_name);
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


void dt_lua_goto_subtable(lua_State *L,const char* sub_name) {
	luaL_checktype(L,-1,LUA_TTABLE);
	lua_getfield(L,-1,sub_name);
	if(lua_isnil(L,-1)) {
		lua_pop(L,1);
		lua_newtable(L);
		lua_setfield(L,-2,sub_name);
		lua_getfield(L,-1,sub_name);
	}
	lua_remove(L,-2);
}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
