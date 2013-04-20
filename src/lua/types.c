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
#include <stdarg.h>

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
void to_char1024(lua_State* L, luaA_Type type_id, void* c_out, int index) { to_char_num(L,type_id,c_out,index,1024);}
void to_charfilename_length(lua_State* L, luaA_Type type_id, void* c_out, int index) { to_char_num(L,type_id,c_out,index,DT_MAX_FILENAME_LEN);}
void to_charpath_length(lua_State* L, luaA_Type type_id, void* c_out, int index) { to_char_num(L,type_id,c_out,index,DT_MAX_PATH_LEN);}

int dt_lua_autotype_next(lua_State *L){
  int top = lua_gettop(L);
  luaL_getmetafield(L,-2,"__get");
  lua_pushvalue(L,-2);
  int nresults = lua_next(L,-2);
  lua_remove(L,top);
  lua_remove(L,top);
  if(!nresults) return 0;
  lua_pushvalue(L,-3);
  lua_pushvalue(L,-3);
  lua_call(L,2,LUA_MULTRET);
  return 2;
}



int dt_lua_autotype_pairs(lua_State *L){
  luaL_getmetafield(L,-1,"__next");
  lua_pushvalue(L,-2);
  lua_pushnil(L); // index set to null for reset
  return 3;
}

int dt_lua_autotype_index(lua_State *L){
  luaL_getmetafield(L,-2,"__get");
  int pos_get = lua_gettop(L); // points at __get
  lua_pushvalue(L,-2);
  lua_gettable(L,-2);
  if(lua_isnil(L,-1)) {
    lua_pop(L,1);
    if(!luaL_getmetafield(L,-1,"__default_index")){
      luaL_getmetafield(L,-3,"__luaA_TypeName");
      return luaL_error(L,"field %s not found for type %s\n",lua_tostring(L,-3),lua_tostring(L,-1));
    }
  }
  lua_pushvalue(L,-4);
  lua_pushvalue(L,-4);
  lua_call(L,2,LUA_MULTRET);
  lua_remove(L,pos_get);
  return (lua_gettop(L)-pos_get+1);
}


int dt_lua_autotype_newindex(lua_State *L){
  luaL_getmetafield(L,-3,"__set");
  int pos_set = lua_gettop(L); // points at __get
  lua_pushvalue(L,-3);
  lua_gettable(L,-2);
  if(lua_isnil(L,-1)) {
    lua_pop(L,1);
    if(!luaL_getmetafield(L,-1,"__default_index")){

      return luaL_error(L,"field %s not found for type %s\n",lua_tostring(L,-3),luaL_getmetafield(L,-4,"__luaA_TypeName"));
    }
  }
  lua_pushvalue(L,-5);
  lua_pushvalue(L,-5);
  lua_pushvalue(L,-5);
  lua_call(L,3,LUA_MULTRET);
  lua_remove(L,pos_set);
  return (lua_gettop(L)-pos_set+1);
}


int dt_lua_autotype_full_pushfunc(lua_State *L, luaA_Type type_id, const void *cin) 
{
  size_t type_size= luaA_type_size(type_id);
  void* udata = lua_newuserdata(L,type_size);
  memcpy(udata,cin,type_size);
  luaL_setmetatable(L,luaA_type_name(type_id));
  return 1;
}

void dt_lua_autotype_tofunc(lua_State*L, luaA_Type type_id, void* cout, int index)
{
  void * udata = luaL_checkudata(L,index,luaA_type_name(type_id));
  memcpy(cout,udata,luaA_type_size(type_id));
}

void dt_lua_register_type_callback_internal(lua_State* L,const char* type_name,lua_CFunction index, lua_CFunction newindex,...){
  luaL_getmetatable(L,type_name); // gets the metatable since it's supposed to exist
  luaL_getsubtable(L,-1,"__get");
  luaL_getsubtable(L,-2,"__set");
  va_list key_list;
  va_start(key_list,newindex);
  const char* key = va_arg(key_list,const char*);
  if(key) {
    while(key) {
      lua_pushcfunction(L,index);
      lua_setfield(L,-3,key);

      if(newindex) {
        lua_pushcfunction(L,newindex);
        lua_setfield(L,-2,key);
      }
      key = va_arg(key_list,const char*);
    }
  } else {
    lua_newtable(L);
    lua_pushcfunction(L,index);
    lua_setfield(L,-2,"__default_index");
    lua_pushcfunction(L,newindex);
    lua_setfield(L,-2,"__default_newindex");
    lua_setmetatable(L,-3);
    lua_getmetatable(L,-2);
    lua_setmetatable(L,-2);
  }
  va_end(key_list);
  lua_pop(L,3);
}
void dt_lua_register_type_callback_list_internal(lua_State* L,const char* type_name,lua_CFunction index, lua_CFunction newindex,const char**list)
{
  luaL_getmetatable(L,type_name); // gets the metatable since it's supposed to exist
  luaL_getsubtable(L,-1,"__get");
  luaL_getsubtable(L,-2,"__set");
  const char** key = list;
  while(*key) {
    lua_pushcfunction(L,index);
    lua_setfield(L,-3,*key);

    if(newindex) {
      lua_pushcfunction(L,newindex);
      lua_setfield(L,-2,*key);
    }
    key ++;
  }
  lua_pop(L,3);
}
void dt_lua_register_type_callback_default_internal(lua_State* L,const char* type_name,lua_CFunction index, lua_CFunction newindex){
  luaL_getmetatable(L,type_name); // gets the metatable since it's supposed to exist
  luaL_getsubtable(L,-1,"__get");
  luaL_getsubtable(L,-2,"__set");
  lua_newtable(L);
  lua_pushcfunction(L,index);
  lua_setfield(L,-2,"__default_index");
  lua_pushcfunction(L,newindex);
  lua_setfield(L,-2,"__default_newindex");
  lua_setmetatable(L,-3);
  lua_getmetatable(L,-2);
  lua_setmetatable(L,-2);
  lua_pop(L,3);
}


static int lautoc_struct_index(lua_State *L) {
    return luaA_struct_push_member_name_typeid(L, lua_tonumber(L,lua_upvalueindex(1)), lua_touserdata(L,-2), lua_tostring(L,-1));
}
static int lautoc_struct_newindex(lua_State *L) {
    luaA_struct_to_member_name_typeid(L, lua_tonumber(L,lua_upvalueindex(1)), lua_touserdata(L,-3), lua_tostring(L,-2),-1);
    return 0;
}

void dt_lua_register_type_callback_type_internal(lua_State* L,const char* type_name,lua_CFunction index, lua_CFunction newindex,const char* struct_type_name)
{
  luaL_getmetatable(L,type_name); // gets the metatable since it's supposed to exist
  luaL_getsubtable(L,-1,"__get");
  luaL_getsubtable(L,-2,"__set");
  if(!index && !newindex) {
    index = lautoc_struct_index;
    newindex = lautoc_struct_newindex;
  }
  const char* member = luaA_struct_next_member_name_typeid(L,luaA_type_find(struct_type_name),LUAA_INVALID_MEMBER_NAME);
  while(member != LUAA_INVALID_MEMBER_NAME){
    lua_pushnumber(L,luaA_type_find(struct_type_name));
    lua_pushcclosure(L,index,1);
    lua_setfield(L,-3,member);

    if(newindex) {
      lua_pushnumber(L,luaA_type_find(struct_type_name));
      lua_pushcclosure(L,newindex,1);
      lua_setfield(L,-2,member);
    }
    member = luaA_struct_next_member_name_typeid(L,luaA_type_find(struct_type_name),member);
  }
  lua_pop(L,3);
}



luaA_Type dt_lua_init_type_internal(lua_State* L, const char*type_name,size_t size){
  luaA_Type my_type = luaA_type_add(type_name,size); 
  luaL_newmetatable(L,type_name);

  lua_pushstring(L,type_name);
  lua_setfield(L,-2,"__luaA_TypeName");

  lua_pushnumber(L,my_type);
  lua_setfield(L,-2,"__luaA_Type");


  luaA_conversion_typeid(my_type,dt_lua_autotype_full_pushfunc,dt_lua_autotype_tofunc);

  lua_pushcfunction(L,dt_lua_autotype_next);
  lua_setfield(L,-2,"__next");

  lua_pushcfunction(L,dt_lua_autotype_pairs);
  lua_setfield(L,-2,"__pairs");

  lua_pushcfunction(L,dt_lua_autotype_index);
  lua_setfield(L,-2,"__index");

  lua_pushcfunction(L,dt_lua_autotype_newindex);
  lua_setfield(L,-2,"__newindex");

  lua_newtable(L);
  lua_setfield(L,-2,"__get");

  lua_newtable(L);
  lua_setfield(L,-2,"__set");
  // remove the metatable
  lua_pop(L,1);
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
  luaA_conversion(char_1024,push_char_array,to_char1024);
  luaA_conversion_push(const char_1024,push_char_array);
  luaA_conversion(char_filename_length,push_char_array,to_charfilename_length);
  luaA_conversion_push(const char_filename_length,push_char_array);
  luaA_conversion(char_path_length,push_char_array,to_charfilename_length);
  luaA_conversion_push(const char_path_length,push_char_array);
  luaA_conversion(int32_t,luaA_push_int, luaA_to_int);
  luaA_conversion_push(const int32_t,luaA_push_int);
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
