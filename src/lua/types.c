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
#include "lua/types.h"
#include <string.h>

void to_char_num(lua_State* L, luaA_Type type_id,void* c_out, int index,int size)
{
  size_t tgt_size;
  const char * value = luaL_checklstring(L,index,&tgt_size);
  if(tgt_size > size) {
    luaL_error(L,"string '%s' too long (max is %d)",value,size);
  }
  luaA_to_char_ptr(L,type_id,c_out,index);
}

int push_char_array(lua_State* L, luaA_Type type_id,const void* c_in) {
  lua_pushstring(L, c_in);
  return 1;
}

void to_char20(lua_State* L, luaA_Type type_id, void* c_out, int index) { to_char_num(L,type_id,c_out,index,20);}
void to_char32(lua_State* L, luaA_Type type_id, void* c_out, int index) { to_char_num(L,type_id,c_out,index,32);}
void to_char52(lua_State* L, luaA_Type type_id, void* c_out, int index) { to_char_num(L,type_id,c_out,index,52);}
void to_charfilename_length(lua_State* L, luaA_Type type_id, void* c_out, int index) { to_char_num(L,type_id,c_out,index,DT_MAX_FILENAME_LEN);}
void to_charpath_length(lua_State* L, luaA_Type type_id, void* c_out, int index) { to_char_num(L,type_id,c_out,index,DT_MAX_PATH_LEN);}

static int type_next(lua_State *L){
  int index;
  const char **list = lua_touserdata(L,lua_upvalueindex(1));
  // all type related entries go first
  luaL_getmetafield(L,-2,"__luaA_Type");
  const luaA_Type my_type =lua_tointeger(L,-1);
  lua_pop(L,1);
  const char * member=NULL; // the next key
  bool has_old_member = false;
  if(my_type != LUAA_INVALID_TYPE && luaA_struct_registered_typeid(L,my_type)) {
    if(lua_isnil(L,-1)) {
      member = luaA_struct_next_member_name_typeid(L,my_type,NULL);
    } else if(luaA_struct_has_member_name_typeid(L,my_type,luaL_checkstring(L,-1))){
      has_old_member = true;
      member = luaA_struct_next_member_name_typeid(L,my_type,luaL_checkstring(L,-1));
      // member is NULL if we reached the end of the luaA members
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
int dt_lua_autotype_pairs(lua_State *L){
	// one upvalue, the lightuserdata 
	const char **list = lua_touserdata(L,lua_upvalueindex(1));
	lua_pushlightuserdata(L,list);
	lua_pushcclosure(L,type_next,1);
	lua_pushvalue(L,-2);
	lua_pushnil(L); // index set to null for reset
	return 3;
}

static int type_index(lua_State *L){
	const char **list = lua_touserdata(L,lua_upvalueindex(1));
	const lua_CFunction index_function = lua_tocfunction(L,lua_upvalueindex(2));

  const char* membername = lua_tostring(L, -1);
  const void* object = lua_touserdata(L, -2);
  luaL_getmetafield(L,-2,"__luaA_Type");
  const luaA_Type my_type =lua_tointeger(L,-1);
  lua_pop(L,1);

  if(luaA_struct_registered_typeid(L,my_type) && luaA_struct_has_member_name_typeid(L,my_type,membername)) {
      const int result = luaA_struct_push_member_name_typeid(L, my_type, object, membername);
      return result;
  }
  if( index_function) {
    int switch_index = luaL_checkoption(L,-1,NULL,list);
    lua_pop(L,1);
    lua_pushinteger(L,switch_index);
    return index_function(L);
  }
  return luaL_error(L,"field %s not found for type %s\n",membername,luaA_type_name(my_type));
}



static int type_newindex(lua_State *L){
	const char **list = lua_touserdata(L,lua_upvalueindex(1));
	const lua_CFunction newindex_function = lua_tocfunction(L,lua_upvalueindex(2));

  const char* membername = lua_tostring(L, -2);
  void* object = lua_touserdata(L, -3);
  luaL_getmetafield(L,-2,"__luaA_Type");
  const luaA_Type my_type =lua_tointeger(L,-1);
  lua_pop(L,1);

  if(luaA_struct_registered_typeid(L,my_type) && luaA_struct_has_member_name_typeid(L,my_type,membername)) {
    if(luaA_type_has_to_func(luaA_struct_typeof_member_name_typeid(L,my_type,membername))) {
      luaA_struct_to_member_name_typeid(L, my_type, object, membername,-1);
      return 0;
    } else {
      return luaL_error(L,"%s is read only",membername);
    }
  }
  if( newindex_function) {
    int switch_index = luaL_checkoption(L,-2,NULL,list);
    lua_remove(L,-2);
    lua_pushinteger(L,switch_index);
    lua_insert(L,-2);
    return newindex_function(L);
  }
  return luaL_error(L,"field %s not found for type %s\n",membername,luaA_type_name(my_type));
}


static int autotype_full_pushfunc(lua_State *L, luaA_Type type_id, const void *cin) 
{
  size_t type_size= luaA_type_size(type_id);
  void* udata = lua_newuserdata(L,type_size);
  memcpy(udata,cin,type_size);
	luaL_setmetatable(L,luaA_type_name(type_id));
  return 1;
}

static void autotype_tofunc(lua_State*L, luaA_Type type_id, void* cout, int index)
{
  void * udata = luaL_checkudata(L,index,luaA_type_name(type_id));
  memcpy(cout,udata,luaA_type_size(type_id));
}


luaA_Type dt_lua_init_type_internal(lua_State* L, char*type_name,const char ** list,lua_CFunction index,lua_CFunction newindex,size_t size){
  luaA_type_add(type_name,size); \
  luaL_newmetatable(L,type_name);

  luaA_Type my_type =  luaA_type_find(type_name);
  if(my_type == LUAA_INVALID_TYPE) {
    // can only happen if someone calls the function without the wrapper, and does it incorrectly
    printf("program error : type %s is not registered to luaAutoC\n",type_name);
  }
  lua_pushnumber(L,my_type);
	lua_setfield(L,-2,"__luaA_Type");


  luaA_conversion_typeid(my_type,autotype_full_pushfunc,autotype_tofunc);

	lua_pushlightuserdata(L,list);
	lua_pushcclosure(L,dt_lua_autotype_pairs,1);
	lua_setfield(L,-2,"__pairs");

	lua_pushlightuserdata(L,list);
	lua_pushcfunction(L,index);
	lua_pushcclosure(L,type_index,2);
	lua_setfield(L,-2,"__index");

	lua_pushlightuserdata(L,list);
	lua_pushcfunction(L,newindex);
	lua_pushcclosure(L,type_newindex,2);
	lua_setfield(L,-2,"__newindex");

  return my_type;
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
  luaA_conversion(char_path_length,push_char_array,to_charfilename_length);
  luaA_conversion_push(const char_path_length,push_char_array);

}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
