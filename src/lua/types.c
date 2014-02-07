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
  if(tgt_size > size)
  {
    luaL_error(L,"string '%s' too long (max is %d)",value,size);
  }
  luaA_to_char_ptr(L,type_id,c_out,index);
}

int push_char_array(lua_State* L, luaA_Type type_id,const void* c_in)
{
  lua_pushstring(L, c_in);
  return 1;
}

void to_char20(lua_State* L, luaA_Type type_id, void* c_out, int index)
{
  to_char_num(L,type_id,c_out,index,20);
}
void to_char32(lua_State* L, luaA_Type type_id, void* c_out, int index)
{
  to_char_num(L,type_id,c_out,index,32);
}
void to_char52(lua_State* L, luaA_Type type_id, void* c_out, int index)
{
  to_char_num(L,type_id,c_out,index,52);
}
void to_char64(lua_State* L, luaA_Type type_id, void* c_out, int index)
{
  to_char_num(L,type_id,c_out,index,64);
}
void to_char128(lua_State* L, luaA_Type type_id, void* c_out, int index)
{
  to_char_num(L,type_id,c_out,index,128);
}
void to_char512(lua_State* L, luaA_Type type_id, void* c_out, int index)
{
  to_char_num(L,type_id,c_out,index,512);
}
void to_char1024(lua_State* L, luaA_Type type_id, void* c_out, int index)
{
  to_char_num(L,type_id,c_out,index,1024);
}
void to_charfilename_length(lua_State* L, luaA_Type type_id, void* c_out, int index)
{
  to_char_num(L,type_id,c_out,index,DT_MAX_FILENAME_LEN);
}
void to_charpath_length(lua_State* L, luaA_Type type_id, void* c_out, int index)
{
  to_char_num(L,type_id,c_out,index,DT_MAX_PATH_LEN);
}

int dt_lua_autotype_inext(lua_State *L)
{
  lua_getfield(L,lua_upvalueindex(1),"__len");
  lua_pushvalue(L,-3);
  lua_call(L,1,1);
  int length = lua_tonumber(L,-1);
  lua_pop(L,1);
  int key = 0;
  if(length == 0) {
    lua_pop(L,1);
    lua_pushnil(L);
    return 1;
  }else if(lua_isnil(L,-1)) {
    key = 1;
    lua_pop(L,1);
    lua_pushnumber(L,key);
    lua_pushnumber(L,key);
    lua_gettable(L,-3);
    return 2;
  } else if(luaL_checknumber(L,-1) < length) {
    key = lua_tonumber(L,-1) +1;
    lua_pop(L,1);
    lua_pushnumber(L,key);
    lua_pushnumber(L,key);
    lua_gettable(L,-3);
    return 2;
  } else  {
    // we reached the end of ipairs
    lua_pop(L,1);
    lua_pushnil(L);
    return 1;
  }
}

static int autotype_next(lua_State *L)
{
  /* CONVENTION
     each block has the following stack on entry and exit
    1 : the object
    2 : the last entry ("next" convention)
    each block should return according to "next" convention on success
    each block should leave the key untouched if it doesn't know about it
    each block should replace the key with "nil" if the key was the last entry it can handle

    */
      //printf("aaaaa %s %d\n",__FUNCTION__,__LINE__);
  lua_getfield(L,lua_upvalueindex(1),"__len");
  if(lua_isnil(L,-1)) {
    lua_pop(L,1);
  } else {
    lua_pushvalue(L,-3);
    lua_call(L,1,1);
    int length = lua_tonumber(L,-1);
    lua_pop(L,1);
    int key = 0;
    if(lua_isnil(L,-1) && length > 0)
    {
      key = 1;
    }
    else if(lua_isnumber(L,-1) && lua_tonumber(L,-1) < length)
    {
      key = lua_tonumber(L,-1) +1;
    }
    else if(lua_isnumber(L,-1) && lua_tonumber(L,-1) == length)
    {
      // numbers are done, move-on to something else
      lua_pop(L,1);
      lua_pushnil(L);
    }
    if(key)
    {
      lua_pop(L,1);
      lua_pushnumber(L,key);
      lua_pushnumber(L,key);
      lua_gettable(L,-3);
      return 2;
    }
  }
  // stack at this point : {object,key}
  int key_in_get = false;
  lua_getfield(L,lua_upvalueindex(1),"__get");
  if(lua_isnil(L,-2)) {
      key_in_get = true;
  } else {
    lua_pushvalue(L,-2);
    lua_gettable(L,-2);
    if(lua_isnil(L,-1)) {
      key_in_get = false;
      lua_pop(L,2);
    } else {
      key_in_get = true;
      lua_pop(L,1);
    }
  }
  if(key_in_get) {
    lua_pushvalue(L,-2);
    if(lua_next(L,-2)) {
      // we have a next
      lua_pop(L,1);
      lua_remove(L,-2);
      lua_remove(L,-2);
      lua_pushvalue(L,-1);
      lua_gettable(L,-3);
      return 2;
    } else {
      // key was the last for __get
      lua_pop(L,2);
      lua_pushnil(L);
    }
  }
  // stack at this point : {object,key}
  lua_getfield(L,lua_upvalueindex(1),"__luaA_ParentMetatable");
  if(lua_isnil(L,-1)) {
    lua_pop(L,1);
  } else{
    // call parent's index, we can't do anything
    lua_getfield(L,-1,"__next");
    lua_insert(L,-4);
    lua_pop(L,1);
    int pos_get = lua_absindex(L,-3);
    lua_call(L,2,LUA_MULTRET);
    return lua_gettop(L)-pos_get +1;
  }

  // stack at this point : {object,key}
  if(lua_isnil(L,-1)) {
    return 1;
  } else {
    return luaL_error(L,"invalid key to 'next'");
  }
}



int dt_lua_autotype_ipairs(lua_State *L)
{
  lua_getfield(L,lua_upvalueindex(1),"__inext");
  lua_pushvalue(L,-2);
  lua_pushinteger(L,0); // index set to 0 for reset
  return 3;
}

static int autotype_pairs(lua_State *L)
{
  lua_getfield(L,lua_upvalueindex(1),"__next");
  lua_pushvalue(L,-2);
  lua_pushnil(L); // index set to null for reset
  return 3;
}

static int autotype_index(lua_State *L)
{
  lua_getfield(L,lua_upvalueindex(1),"__get");
  int pos_get = lua_gettop(L); // points at __get
  lua_pushvalue(L,-2);
  lua_gettable(L,-2);
  if(lua_isnil(L,-1) && lua_isnumber(L,-3)) {
    lua_getfield(L,lua_upvalueindex(1),"__number_index");
    if(lua_isnil(L,-1))
    {
      lua_pop(L,1);
    }else {
      lua_remove(L,-2);
    }
  }
  if(lua_isnil(L,-1)) {
    lua_getfield(L,lua_upvalueindex(1),"__luaA_ParentMetatable");
    if(lua_isnil(L,-1)) {
      lua_pop(L,1);
    } else{
      // call parent's index, we can't do anything
      lua_getfield(L,-1,"__index");
      lua_insert(L,-6);
      lua_pop(L,3);
      pos_get = lua_absindex(L,-3);
      lua_call(L,2,LUA_MULTRET);
      return lua_gettop(L)-pos_get +1;
    }
  }
  if(lua_isnil(L,-1))
  {
    lua_pop(L,1);
    luaL_getmetafield(L,-3,"__luaA_TypeName");
    return luaL_error(L,"field \"%s\" not found for type %s\n",lua_tostring(L,-3),lua_tostring(L,-1));
  }
  lua_pushvalue(L,-4);
  lua_pushvalue(L,-4);
  lua_call(L,2,LUA_MULTRET);
  lua_remove(L,pos_get);
  return (lua_gettop(L)-pos_get+1);
}


static int autotype_newindex(lua_State *L)
{
  lua_getfield(L,lua_upvalueindex(1),"__set");
  int pos_set = lua_gettop(L); // points at __get
  lua_pushvalue(L,-3);
  lua_gettable(L,-2);
  if(lua_isnil(L,-1) && lua_isnumber(L,-4)) {
    luaL_getmetafield(L,-5,"__number_newindex");
    if(lua_isnil(L,-1)) {
      lua_pop(L,1);
    } else  {
      lua_remove(L,-2);
    }
  }
  if(lua_isnil(L,-1)) {
    lua_getfield(L,lua_upvalueindex(1),"__luaA_ParentMetatable");
    if(lua_isnil(L,-1)) {
      lua_pop(L,1);
    } else{
      // call parent's index, we can't do anything
      lua_getfield(L,-1,"__newindex");
      lua_insert(L,-7);
      lua_pop(L,3);
      pos_set = lua_absindex(L,-4);
      lua_call(L,3,LUA_MULTRET);
      return lua_gettop(L)-pos_set +1;
    }
  }
  if(lua_isnil(L,-1))
  {
    lua_pop(L,1);
    luaL_getmetafield(L,-4,"__luaA_TypeName");
    return luaL_error(L,"field \"%s\" can't be written for type %s\n",lua_tostring(L,-4),lua_tostring(L,-1));
  }
  lua_pushvalue(L,-5);
  lua_pushvalue(L,-5);
  lua_pushvalue(L,-5);
  lua_call(L,3,LUA_MULTRET);
  lua_remove(L,pos_set);
  return (lua_gettop(L)-pos_set+1);
}


static int full_pushfunc(lua_State *L, luaA_Type type_id, const void *cin)
{
  size_t type_size= luaA_type_size(type_id);
  void* udata = lua_newuserdata(L,type_size);
  memcpy(udata,cin,type_size);
  luaL_setmetatable(L,luaA_type_name(type_id));
  return 1;
}

static void full_tofunc(lua_State*L, luaA_Type type_id, void* cout, int index)
{
  void * udata = luaL_checkudata(L,index,luaA_type_name(type_id));
  memcpy(cout,udata,luaA_type_size(type_id));
}

static int int_pushfunc(lua_State *L, luaA_Type type_id, const void *cin)
{
  luaL_getmetatable(L,luaA_type_name(type_id));
  luaL_getsubtable(L,-1,"__values");
  int singleton = *(int*)cin;
  lua_pushnumber(L,singleton);
  lua_gettable(L,-2);
  if(lua_isnoneornil(L,-1)) {
    lua_pop(L,1);
    int* udata = lua_newuserdata(L,sizeof(int));
    *udata = singleton;
    luaL_setmetatable(L,luaA_type_name(type_id));
    lua_pushinteger(L,singleton);
    lua_pushvalue(L,-2);
    lua_settable(L,-4);

  }
  lua_remove(L,-2);//__values
  lua_remove(L,-2);//metatable
  return 1;
}

static void int_tofunc(lua_State*L, luaA_Type type_id, void* cout, int index)
{
  void * udata = luaL_checkudata(L,index,luaA_type_name(type_id));
  memcpy(cout,udata,sizeof(int));
}

void dt_lua_register_type_callback_typeid(lua_State* L,luaA_Type type_id,lua_CFunction index, lua_CFunction newindex,...)
{
  luaL_getmetatable(L,luaA_type_name(type_id)); // gets the metatable since it's supposed to exist
  luaL_getsubtable(L,-1,"__get");
  luaL_getsubtable(L,-2,"__set");
  va_list key_list;
  va_start(key_list,newindex);
  const char* key = va_arg(key_list,const char*);
  while(key)
  {
    lua_pushcfunction(L,index);
    lua_setfield(L,-3,key);

    if(newindex)
    {
      lua_pushcfunction(L,newindex);
    }
    else
    {
      lua_pushnil(L);
    }
    lua_setfield(L,-2,key);
    key = va_arg(key_list,const char*);
  }
  va_end(key_list);
  lua_pop(L,3);
}
void dt_lua_register_type_callback_list_typeid(lua_State* L,luaA_Type type_id,lua_CFunction index, lua_CFunction newindex,const char**list)
{
  luaL_getmetatable(L,luaA_type_name(type_id)); // gets the metatable since it's supposed to exist
  luaL_getsubtable(L,-1,"__get");
  luaL_getsubtable(L,-2,"__set");
  const char** key = list;
  while(*key)
  {
    lua_pushcfunction(L,index);
    lua_setfield(L,-3,*key);

    if(newindex)
    {
      lua_pushcfunction(L,newindex);
    }
    else
    {
      lua_pushnil(L);
    }
    lua_setfield(L,-2,*key);
    key ++;
  }
  lua_pop(L,3);
}

void dt_lua_register_type_callback_number_typeid(lua_State* L,luaA_Type type_id,lua_CFunction index, lua_CFunction newindex,lua_CFunction length)
{
  luaL_getmetatable(L,luaA_type_name(type_id)); // gets the metatable since it's supposed to exist

  lua_pushvalue(L,-1);
  lua_pushcclosure(L,index,1);
  lua_setfield(L,-2,"__number_index");

  lua_pushvalue(L,-1);
  lua_pushcclosure(L,newindex,1);
  lua_setfield(L,-2,"__number_newindex");

  if(length) {
    lua_pushvalue(L,-1);
	  lua_pushcclosure(L,length,1);
	  lua_setfield(L,-2,"__len");

    lua_pushvalue(L,-1);
	  lua_pushcclosure(L,dt_lua_autotype_ipairs,1);
	  lua_setfield(L,-2,"__ipairs");

    lua_pushvalue(L,-1);
    lua_pushcclosure(L,dt_lua_autotype_inext,1);
	  lua_setfield(L,-2,"__inext");
  }

  lua_pop(L,1);

}


static int lautoc_struct_index(lua_State *L)
{
  return luaA_struct_push_member_name_typeid(L, lua_tonumber(L,lua_upvalueindex(1)), lua_touserdata(L,-2), lua_tostring(L,-1));
}
static int lautoc_struct_newindex(lua_State *L)
{
  luaA_struct_to_member_name_typeid(L, lua_tonumber(L,lua_upvalueindex(1)), lua_touserdata(L,-3), lua_tostring(L,-2),-1);
  return 0;
}

void dt_lua_register_type_callback_type_typeid(lua_State* L,luaA_Type type_id,lua_CFunction index, lua_CFunction newindex,luaA_Type struct_type_id)
{
  luaL_getmetatable(L,luaA_type_name(type_id)); // gets the metatable since it's supposed to exist
  luaL_getsubtable(L,-1,"__get");
  luaL_getsubtable(L,-2,"__set");
  if(!index && !newindex)
  {
    index = lautoc_struct_index;
    newindex = lautoc_struct_newindex;
  }
  const char* member = luaA_struct_next_member_name_typeid(L,struct_type_id,LUAA_INVALID_MEMBER_NAME);
  while(member != LUAA_INVALID_MEMBER_NAME)
  {
    lua_pushnumber(L,struct_type_id);
    lua_pushcclosure(L,index,1);
    lua_setfield(L,-3,member);

    if(newindex)
    {
      lua_pushnumber(L,struct_type_id);
      lua_pushcclosure(L,newindex,1);
      lua_setfield(L,-2,member);
    }
    member = luaA_struct_next_member_name_typeid(L,struct_type_id,member);
  }
  lua_pop(L,3);
}


static int type_const_index(lua_State* L)
{
  lua_pushvalue(L,lua_upvalueindex(1));
  return 1;
}
void dt_lua_register_type_callback_stack_typeid(lua_State* L,luaA_Type type_id,const char* name)
{
  luaL_getmetatable(L,luaA_type_name(type_id)); // gets the metatable since it's supposed to exist
  luaL_getsubtable(L,-1,"__get");
  luaL_getsubtable(L,-2,"__set");
  lua_pushvalue(L,-4);
  lua_pushcclosure(L,type_const_index,1);
  lua_setfield(L,-3,name);
  lua_pop(L,4);
}

void dt_lua_register_type_callback_inherit_typeid(lua_State* L,luaA_Type type_id,luaA_Type parent_type_id)
{
  luaL_getmetatable(L,luaA_type_name(type_id)); // gets the metatable since it's supposed to exist
  luaL_getmetatable(L,luaA_type_name(parent_type_id)); // gets the metatable since it's supposed to exist
  lua_setfield(L,-2,"__luaA_ParentMetatable");
}

static void init_metatable(lua_State* L, luaA_Type type_id)
{
  luaL_newmetatable(L,luaA_type_name(type_id));

  lua_pushstring(L,luaA_type_name(type_id));
  lua_setfield(L,-2,"__luaA_TypeName");

  lua_pushnumber(L,type_id);
  lua_setfield(L,-2,"__luaA_Type");

  lua_pushvalue(L,-1);
  lua_pushcclosure(L,autotype_next,1);
  lua_setfield(L,-2,"__next");

  lua_pushvalue(L,-1);
  lua_pushcclosure(L,autotype_pairs,1);
  lua_setfield(L,-2,"__pairs");

  lua_pushvalue(L,-1);
  lua_pushcclosure(L,autotype_index,1);
  lua_setfield(L,-2,"__index");

  lua_pushvalue(L,-1);
  lua_pushcclosure(L,autotype_newindex,1);
  lua_setfield(L,-2,"__newindex");

  lua_newtable(L);
  lua_setfield(L,-2,"__get");

  lua_newtable(L);
  lua_setfield(L,-2,"__set");
  // leave metatable on top of stack
}


luaA_Type dt_lua_init_type_typeid(lua_State* L, luaA_Type type_id)
{
  init_metatable(L,type_id);
  lua_pop(L,1);
  luaA_conversion_typeid(type_id,full_pushfunc,full_tofunc);
  return type_id;
}

luaA_Type dt_lua_init_singleton(lua_State* L, const char* unique_name)
{
  char tmp_name[1024];
  snprintf(tmp_name,1024,"dt_lua_singleton_%s",unique_name);

  luaA_Type type_id = luaA_type_add(tmp_name,sizeof(void*));
  init_metatable(L,type_id);

  void* udata = lua_newuserdata(L,sizeof(void*));
  memset(udata,0,sizeof(void*));

  lua_pushvalue(L,-1);
  luaL_setmetatable(L,tmp_name);
  lua_setfield(L,-3,"__singleton");
  lua_remove(L,-2);

  return type_id;
}

luaA_Type dt_lua_init_int_type_typeid(lua_State* L, luaA_Type type_id)
{
  init_metatable(L,type_id);
  lua_newtable(L);
  lua_setfield(L,-2,"__values");
  lua_pop(L,1);
  luaA_conversion_typeid(type_id,int_pushfunc,int_tofunc);
  return type_id;
}

void dt_lua_initialize_types(lua_State *L)
{
  luaA_conversion(char_20,push_char_array,to_char20);
  luaA_conversion_push(const char_20,push_char_array);
  luaA_conversion(char_32,push_char_array,to_char32);
  luaA_conversion_push(const char_32,push_char_array);
  luaA_conversion(char_52,push_char_array,to_char52);
  luaA_conversion_push(const char_52,push_char_array);
  luaA_conversion(char_64,push_char_array,to_char64);
  luaA_conversion_push(const char_64,push_char_array);
  luaA_conversion(char_128,push_char_array,to_char128);
  luaA_conversion_push(const char_128,push_char_array);
  luaA_conversion(char_512,push_char_array,to_char512);
  luaA_conversion_push(const char_512,push_char_array);
  luaA_conversion(char_1024,push_char_array,to_char512);
  luaA_conversion_push(const char_1024,push_char_array);
  luaA_conversion(char_filename_length,push_char_array,to_charfilename_length);
  luaA_conversion_push(const char_filename_length,push_char_array);
  luaA_conversion(char_path_length,push_char_array,to_charfilename_length);
  luaA_conversion_push(const char_path_length,push_char_array);
  luaA_conversion(int32_t,luaA_push_int, luaA_to_int);
  luaA_conversion_push(const int32_t,luaA_push_int);
  luaA_conversion_push(const_string,luaA_push_const_char_ptr);
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
