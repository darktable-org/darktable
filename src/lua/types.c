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
#include "lautoc.h"
#include "lua/types.h"

void to_char_num(lua_State* L, void* c_out, int index,int size)
{
  size_t tgt_size;
  const char * value = luaL_checklstring(L,index,&tgt_size);
  if(tgt_size > size) {
    luaL_error(L,"string '%s' too long (max is %d)",value,size);
  }
  luaA_to_char_ptr(L,c_out,index);
}

int push_char_array(lua_State* L,const void* c_in) {
  lua_pushstring(L, c_in);
  return 1;
}

void to_char20(lua_State* L, void* c_out, int index) { to_char_num(L,c_out,index,20);}
void to_char32(lua_State* L, void* c_out, int index) { to_char_num(L,c_out,index,32);}
void to_char52(lua_State* L, void* c_out, int index) { to_char_num(L,c_out,index,52);}
void to_charfilename_length(lua_State* L, void* c_out, int index) { to_char_num(L,c_out,index,DT_MAX_FILENAME_LEN);}

static int char_list_next(lua_State *L){
  int index;
  const char **list = lua_touserdata(L,lua_upvalueindex(1));
  const char *type_name = lua_tostring(L,lua_upvalueindex(2));
  // all type related entries go first
  const char * member=NULL; // the next key
  luaA_Type my_type =  type_name?luaA_type_find(type_name):LUAA_INVALID_TYPE;
  const bool has_old_member = (type_name && 
      !lua_isnil(L,-1) &&
      luaA_struct_has_member_name_typeid(L,my_type,luaL_checkstring(L,-1)));
  if(type_name ) {
    if(lua_isnil(L,-1)) {
      member = luaA_struct_next_member_name_typeid(L,my_type,NULL);
    } else if(has_old_member){
      member = luaA_struct_next_member_name_typeid(L,my_type,luaL_checkstring(L,-1));
    }
  }
  if(!member && list) {
    if(lua_isnil(L,-1) || has_old_member) {
      index = 0;
    } else {
      index = luaL_checkoption(L,-1,NULL,list);
      index++;
    }
    member=list[index];
  }
  lua_pop(L,1); // pop the key
  if(!member) { // no need to test < 0 or > max, luaL_checkoption catches it for us
    return 0;
  }
  if (!luaL_getmetafield(L, -1, "__index"))  /* no metafield? */
    luaL_error(L,"object doesn't have an __index method"); // should never happen
  lua_pushvalue(L,-2);// the object called
  lua_pushstring(L,member); // push the index string
  lua_call(L, 2, 1);
  lua_pushstring(L,member); // push the index string
  lua_insert(L,-2); // move the index string below the value object
  return 2;
}
static int char_list_pairs(lua_State *L){
	// one upvalue, the lightuserdata 
	const char *type_name = lua_tostring(L,lua_upvalueindex(2));
	const char **list = lua_touserdata(L,lua_upvalueindex(1));
	lua_pushlightuserdata(L,list);
	lua_pushstring(L,type_name);
	lua_pushcclosure(L,char_list_next,2);
	lua_pushvalue(L,-2);
	lua_pushnil(L); // index set to null for reset
	return 3;
}

void dt_lua_init_name_list_pair_internal(lua_State* L,const char*type_name, const char **list){
	lua_pushlightuserdata(L,list);
  lua_pushstring(L,type_name);
	lua_pushcclosure(L,char_list_pairs,2);
	lua_setfield(L,-2,"__pairs");
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


void dt_lua_initialize_types(lua_State *L)
{
  luaA_conversion(char_20,push_char_array,to_char20);
  luaA_conversion_push(const char_20,push_char_array);
  luaA_conversion(char_32,push_char_array,to_char32);
  luaA_conversion_push(const char_32,push_char_array);
  luaA_conversion(char_52,push_char_array,to_char52);
  luaA_conversion_push(const char_52,push_char_array);
  luaA_conversion(char_filename_length,push_char_array,to_charfilename_length);
  luaA_conversion_push(const char_filename_length,push_char_array);

}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
