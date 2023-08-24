/*
   This file is part of darktable,
   Copyright (C) 2013-2023 darktable developers.

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
#include "lua/types.h"
#include "common/darktable.h"
#include "common/file_location.h"
#include "control/control.h"
#include "lua/call.h"
#include <math.h>
#include <stdarg.h>
#include <string.h>

/*************/
/*   TYPES   */
/*************/

static void to_char_array(lua_State *L, luaA_Type type_id, void *c_out, int index, int size)
{
  size_t tgt_size;
  const char *value = luaL_checklstring(L, index, &tgt_size);
  if(tgt_size > size)
  {
    luaL_error(L, "string '%s' too long (max is %d)", value, size);
  }
  strncpy(c_out, value, size);
}

static int push_char_array(lua_State *L, luaA_Type type_id, const void *c_in)
{
  lua_pushstring(L, c_in);
  return 1;
}

static void to_char20(lua_State *L, luaA_Type type_id, void *c_out, int index)
{
  to_char_array(L, type_id, c_out, index, 20);
}
static void to_char32(lua_State *L, luaA_Type type_id, void *c_out, int index)
{
  to_char_array(L, type_id, c_out, index, 32);
}
static void to_char52(lua_State *L, luaA_Type type_id, void *c_out, int index)
{
  to_char_array(L, type_id, c_out, index, 52);
}
static void to_char64(lua_State *L, luaA_Type type_id, void *c_out, int index)
{
  to_char_array(L, type_id, c_out, index, 64);
}
static void to_char128(lua_State *L, luaA_Type type_id, void *c_out, int index)
{
  to_char_array(L, type_id, c_out, index, 128);
}
static void to_char256(lua_State *L, luaA_Type type_id, void *c_out, int index)
{
  to_char_array(L, type_id, c_out, index, 256);
}
static void to_char512(lua_State *L, luaA_Type type_id, void *c_out, int index)
{
  to_char_array(L, type_id, c_out, index, 512);
}
static void to_char1024(lua_State *L, luaA_Type type_id, void *c_out, int index)
{
  to_char_array(L, type_id, c_out, index, 1024);
}
static void to_charfilename_length(lua_State *L, luaA_Type type_id, void *c_out, int index)
{
  to_char_array(L, type_id, c_out, index, DT_MAX_FILENAME_LEN);
}
static void to_charpath_length(lua_State *L, luaA_Type type_id, void *c_out, int index)
{
  to_char_array(L, type_id, c_out, index, PATH_MAX);
}
static int push_protected_double(lua_State *L, luaA_Type type_id, const void *c_in)
{
  double value = *(double *)c_in;
  if(!dt_isnormal(value))
  {
    lua_pushnil(L);
  }
  else
  {
    lua_pushnumber(L, value);
  }
  return 1;
}

static int push_progress_double(lua_State *L, luaA_Type type_id, const void *c_in)
{
  double value = *(double *)c_in;
  if(value < 0.0) value = 0.0;
  if(value > 1.0) value = 1.0;
  lua_pushnumber(L, value);
  return 1;
}

static void to_progress_double(lua_State *L, luaA_Type type_id, void *c_out, int index)
{
  luaA_to_double(L, type_id, c_out, index);
  if(*(double *)c_out < 0.0) *(double *)c_out = 0.0;
  if(*(double *)c_out > 1.0) *(double *)c_out = 1.0;
}

/************************************/
/* METATBLE CALLBACKS FOR AUTOTYPES */
/************************************/
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
  // printf("aaaaa %s %d\n",__FUNCTION__,__LINE__);
  if(luaL_getmetafield(L, 1, "__len"))
  {
    lua_pushvalue(L, -3);
    lua_call(L, 1, 1);
    int length = lua_tonumber(L, -1);
    lua_pop(L, 1);
    int key = 0;
    if(lua_isnil(L, -1) && length > 0)
    {
      key = 1;
    }
    else if(lua_isnumber(L, -1) && lua_tonumber(L, -1) < length)
    {
      key = lua_tonumber(L, -1) + 1;
    }
    else if(lua_isnumber(L, -1) && lua_tonumber(L, -1) == length)
    {
      // numbers are done, move-on to something else
      lua_pop(L, 1);
      lua_pushnil(L);
    }
    if(key)
    {
      lua_pop(L, 1);
      lua_pushinteger(L, key);
      lua_pushinteger(L, key);
      lua_gettable(L, -3);
      return 2;
    }
  }
  // stack at this point : {object,key}
  int key_in_get = false;
  luaL_getmetafield(L, 1, "__get");
  if(lua_isnil(L, -2))
  {
    key_in_get = true;
  }
  else
  {
    lua_pushvalue(L, -2);
    lua_gettable(L, -2);
    if(lua_isnil(L, -1))
    {
      key_in_get = false;
      lua_pop(L, 2);
    }
    else
    {
      key_in_get = true;
      lua_pop(L, 1);
    }
  }
  if(key_in_get)
  {
    lua_pushvalue(L, -2);
    int nil_found = false;
    while(!nil_found)
    {
      if(lua_next(L, -2))
      {
        // we have a next
        lua_pop(L, 1);
        lua_pushvalue(L, -4);
        lua_pushvalue(L, -2);
        // hacky way to avoid a subfunction just to do a pcall around getting a value in a table
        luaL_loadstring(L,"args ={...}; return args[1][args[2]]");
        lua_insert(L,-3);
        int result = dt_lua_treated_pcall(L,2,1);
        if(result == LUA_OK)
        {
          return 2;
        }
        else
        {
          lua_pop(L, 1);
          // and loop to find the next possible value
        }
      }
      else
      {
        // key was the last for __get
        lua_pop(L, 2);
        lua_pushnil(L);
        nil_found = true;
      }
    }
  }

  // stack at this point : {object,key}
  if(lua_isnil(L, -1))
  {
    return 1;
  }
  else
  {
    return luaL_error(L, "invalid key to 'next' : %s", lua_tostring(L, 2));
  }
}



static int autotype_pairs(lua_State *L)
{
  luaL_getmetafield(L, 1, "__next");
  lua_pushvalue(L, -2);
  lua_pushnil(L); // index set to null for reset
  return 3;
}

static int autotype_index(lua_State *L)
{
  luaL_getmetafield(L, 1, "__get");
  int pos_get = lua_gettop(L); // points at __get
  lua_pushvalue(L, -2);
  lua_gettable(L, -2);
  if(lua_isnil(L, -1) && lua_isnumber(L, -3))
  {
    if(luaL_getmetafield(L, 1, "__number_index"))
    {
      lua_remove(L, -2);
    }
  }
  if(lua_isnil(L, -1))
  {
    lua_pop(L, 1);
    luaL_getmetafield(L, -3, "__luaA_TypeName");
    return luaL_error(L, "field \"%s\" not found for type %s\n", lua_tostring(L, -3), lua_tostring(L, -1));
  }
  lua_pushvalue(L, -4);
  lua_pushvalue(L, -4);
  lua_call(L, 2, LUA_MULTRET);
  lua_remove(L, pos_get);
  return (lua_gettop(L) - pos_get + 1);
}


static int autotype_newindex(lua_State *L)
{
  luaL_getmetafield(L, 1, "__set");
  int pos_set = lua_gettop(L); // points at __get
  lua_pushvalue(L, -3);
  lua_gettable(L, -2);
  if(lua_isnil(L, -1) && lua_isnumber(L, -4))
  {
    if(luaL_getmetafield(L, -5, "__number_newindex"))
    {
      lua_remove(L, -2);
    }
  }
  if(lua_isnil(L, -1))
  {
    lua_pop(L, 1);
    luaL_getmetafield(L, -4, "__luaA_TypeName");
    return luaL_error(L, "field \"%s\" can't be written for type %s\n", lua_tostring(L, -4),
                      lua_tostring(L, -1));
  }
  lua_pushvalue(L, -5);
  lua_pushvalue(L, -5);
  lua_pushvalue(L, -5);
  lua_call(L, 3, LUA_MULTRET);
  lua_remove(L, pos_set);
  return (lua_gettop(L) - pos_set + 1);
}


static int autotype_tostring(lua_State *L)
{
  if(luaL_getmetafield(L,1,"__real_tostring")) {
    lua_insert(L,1);
    lua_call(L,1,1);
    return 1;
  } else {
    char tmp[256];
    luaL_getmetafield(L,1,"__luaA_TypeName");
    snprintf(tmp,sizeof(tmp),"%s (%p)",lua_tostring(L,-1),lua_topointer(L,1));
    lua_pushstring(L,tmp);
    return 1;
  }
}

/*************************/
/* PUSH AND TO FUNCTIONS */
/*************************/

static int full_pushfunc(lua_State *L, luaA_Type type_id, const void *cin)
{
  size_t type_size = luaA_typesize(L, type_id);
  void *udata = lua_newuserdatauv(L, type_size, 1);
  lua_newtable(L);
  lua_setiuservalue(L, -2, 1);
  if(cin)
  {
    memcpy(udata, cin, type_size);
  }
  else
  {
    memset(udata, 0, type_size);
  }
  luaL_setmetatable(L, luaA_typename(L, type_id));

  if(luaL_getmetafield(L, -1, "__init"))
  {
    lua_pushvalue(L, -2);                  // the new allocated object
    lua_pushlightuserdata(L, (void *)cin); // forced to cast..
    lua_call(L, 2, 0);
  }
  return 1;
}

static void full_tofunc(lua_State *L, luaA_Type type_id, void *cout, int index)
{
  if(!dt_lua_isa_type(L,index,type_id)) {
    char error_msg[256];
    snprintf(error_msg,sizeof(error_msg),"%s expected",luaA_typename(L,type_id));
    luaL_argerror(L,index,error_msg);
  }
  void* udata = lua_touserdata(L,index);
  memcpy(cout, udata, luaA_typesize(L, type_id));
}

static int int_pushfunc(lua_State *L, luaA_Type type_id, const void *cin)
{
  luaL_getmetatable(L, luaA_typename(L, type_id));
  luaL_getsubtable(L, -1, "__values");
  int singleton = *(int *)cin;
  lua_pushinteger(L, singleton);
  lua_gettable(L, -2);
  if(lua_isnoneornil(L, -1))
  {
    lua_pop(L, 1);
    int *udata = lua_newuserdatauv(L, sizeof(int), 1);
    *udata = singleton;
    luaL_setmetatable(L, luaA_typename(L, type_id));
    lua_pushinteger(L, singleton);
    // warning : no uservalue
    lua_pushvalue(L, -2);
    lua_settable(L, -4);
    if(luaL_getmetafield(L, -1, "__init"))
    {
      lua_pushvalue(L, -2);                  // the new allocated object
      lua_pushlightuserdata(L, (void *)cin); // forced to cast..
      lua_call(L, 2, 0);
    }
  }
  lua_remove(L, -2); //__values
  lua_remove(L, -2); // metatable
  return 1;
}

static void int_tofunc(lua_State *L, luaA_Type type_id, void *cout, int index)
{
  if(!dt_lua_isa_type(L,index,type_id)) {
    char error_msg[256];
    snprintf(error_msg,sizeof(error_msg),"%s expected",luaA_typename(L,type_id));
    luaL_argerror(L,index,error_msg);
  }
  void* udata = lua_touserdata(L,index);
  memcpy(cout, udata, sizeof(int));
}

static int gpointer_pushfunc(lua_State *L, luaA_Type type_id, const void *cin)
{
  gpointer singleton = *(gpointer *)cin;
  if(!singleton) {
    lua_pushnil(L);
    return 1;
  }
  luaL_getsubtable(L, LUA_REGISTRYINDEX, "dt_lua_gpointer_values");
  lua_pushlightuserdata(L, singleton);
  lua_gettable(L, -2);
  if(lua_isnoneornil(L, -1))
  {
    lua_pop(L, 1);
    gpointer *udata = lua_newuserdatauv(L, sizeof(gpointer), 1);
    lua_newtable(L);
    lua_setiuservalue(L, -2, 1);
    *udata = singleton;
    luaL_setmetatable(L, luaA_typename(L, type_id));
    lua_pushlightuserdata(L, singleton);
    lua_pushvalue(L, -2);
    lua_settable(L, -4);
    if(luaL_getmetafield(L, -1, "__init"))
    {
      lua_pushvalue(L, -2);                  // the new allocated object
      lua_pushlightuserdata(L, (void *)cin); // forced to cast..
      lua_call(L, 2, 0);
    }
  }
  lua_remove(L, -2); //dt_lua_gpointer_values
  return 1;
}

static void gpointer_tofunc(lua_State *L, luaA_Type type_id, void *cout, int index)
{
  if(!dt_lua_isa_type(L,index,type_id)) {
    char error_msg[256];
    snprintf(error_msg,sizeof(error_msg),"%s expected",luaA_typename(L,type_id));
    luaL_argerror(L,index,error_msg);
  }
  gpointer* udata = lua_touserdata(L,index);
  memcpy(cout, udata, sizeof(gpointer));
  if(!*udata) {
    luaL_error(L,"Attempting to access of type %s after its destruction\n",luaA_typename(L,type_id));
  }
}

static int unknown_pushfunc(lua_State *L, luaA_Type type_id, const void *cin)
{
  gpointer singleton = *(gpointer *)cin;
  if(!singleton) {
    lua_pushnil(L);
    return 1;
  }
  luaL_getsubtable(L, LUA_REGISTRYINDEX, "dt_lua_gpointer_values");
  lua_pushlightuserdata(L, singleton);
  lua_gettable(L, -2);
  if(lua_isnoneornil(L, -1))
  {
    return luaL_error(L,"Attempting to push a pointer of unknown type on the stack\n");
  }
  lua_remove(L, -2); //dt_lua_gpointer_values
  return 1;
}


/*****************/
/* TYPE CREATION */
/*****************/
void dt_lua_type_register_type(lua_State *L, luaA_Type type_id, const char *name)
{
  luaL_getmetatable(L, luaA_typename(L, type_id)); // gets the metatable since it's supposed to exist
  luaL_getsubtable(L, -1, "__get");
  lua_pushvalue(L, -3);
  lua_setfield(L, -2, name);
  lua_pop(L, 1);

  luaL_getsubtable(L, -1, "__set");
  lua_pushvalue(L, -3);
  lua_setfield(L, -2, name);
  lua_pop(L, 3);
}

void dt_lua_type_register_const_type(lua_State *L, luaA_Type type_id, const char *name)
{
  luaL_getmetatable(L, luaA_typename(L, type_id)); // gets the metatable since it's supposed to exist

  luaL_getsubtable(L, -1, "__get");
  lua_pushvalue(L, -3);
  lua_setfield(L, -2, name);
  lua_pop(L, 3);
}

void dt_lua_type_register_number_const_type(lua_State *L, luaA_Type type_id)
{
  luaL_getmetatable(L, luaA_typename(L, type_id)); // gets the metatable since it's supposed to exist

  lua_pushvalue(L, -2);
  lua_setfield(L, -2, "__number_index");

  if(!lua_isnil(L, -3))
  {
    lua_pushvalue(L, -3);
    lua_setfield(L, -2, "__len");
  }

  lua_pop(L, 3);
}
void dt_lua_type_register_number_type(lua_State *L, luaA_Type type_id)
{
  luaL_getmetatable(L, luaA_typename(L, type_id)); // gets the metatable since it's supposed to exist

  lua_pushvalue(L, -2);
  lua_setfield(L, -2, "__number_index");

  lua_pushvalue(L, -2);
  lua_setfield(L, -2, "__number_newindex");

  if(!lua_isnil(L, -3))
  {
    lua_pushvalue(L, -3);
    lua_setfield(L, -2, "__len");
  }

  lua_pop(L, 3);
}

int dt_lua_type_member_luaautoc(lua_State *L)
{
  const char *member_name = luaL_checkstring(L, 2);
  luaL_getmetafield(L, 1, "__luaA_Type");
  luaA_Type my_type = luaL_checkinteger(L, -1);
  lua_pop(L, 1);
  void *object = lua_touserdata(L, 1);
  if(lua_gettop(L) != 3)
  {
    luaA_struct_push_member_name_type(L, my_type, member_name, object);
    return 1;
  }
  else
  {
    luaA_struct_to_member_name_type(L, my_type, member_name, object, 3);
    return 0;
  }
}

void dt_lua_type_register_struct_type(lua_State *L, luaA_Type type_id)
{
  const char *member_name = luaA_struct_next_member_name_type(L, type_id, LUAA_INVALID_MEMBER_NAME);
  while(member_name != LUAA_INVALID_MEMBER_NAME)
  {
    lua_pushvalue(L, -1);
    luaA_Type member_type = luaA_struct_typeof_member_name_type(L, type_id, member_name);
    if(luaA_conversion_to_registered_type(L, member_type) || luaA_struct_registered_type(L, member_type)
       || luaA_enum_registered_type(L, member_type))
    {
      dt_lua_type_register_type(L, type_id, member_name);
    }
    else
    {
      dt_lua_type_register_const_type(L, type_id, member_name);
    }
    member_name = luaA_struct_next_member_name_type(L, type_id, member_name);
  }
  lua_pop(L, 1);
}


int dt_lua_type_member_common(lua_State *L)
{
  if(lua_gettop(L) != 2)
  {
    luaL_getmetafield(L, 1, "__luaA_TypeName");
    return luaL_error(L, "field \"%s\" can't be written for type %s\n", lua_tostring(L, 2),
                      lua_tostring(L, -1));
  }
  lua_pushvalue(L, lua_upvalueindex(1));
  return 1;
}

void dt_lua_type_register_parent_type(lua_State *L, luaA_Type type_id, luaA_Type parent_type_id)
{
  luaL_getmetatable(L, luaA_typename(L, type_id));        // gets the metatable since it's supposed to exist
  luaL_getmetatable(L, luaA_typename(L, parent_type_id)); // gets the metatable since it's supposed to exist

  lua_pushvalue(L, -1);
  lua_setfield(L, -3, "__luaA_ParentMetatable");

  lua_getfield(L, -2, "__get");
  lua_getfield(L, -2, "__get");
  lua_pushnil(L); /* first key */
  while(lua_next(L, -2) != 0)
  {
    lua_getfield(L,-4,lua_tostring(L,-2));
    if(lua_isnil(L,-1)) {
      lua_pop(L,1);
      lua_setfield(L, -4, lua_tostring(L,-2));
    } else {
      lua_pop(L,2);
    }
  }
  lua_pop(L, 2);

  lua_getfield(L, -2, "__set");
  lua_getfield(L, -2, "__set");
  lua_pushnil(L); /* first key */
  while(lua_next(L, -2) != 0)
  {
    lua_getfield(L,-4,lua_tostring(L,-2));
    if(lua_isnil(L,-1)) {
      lua_pop(L,1);
      lua_setfield(L, -4, lua_tostring(L,-2));
    } else {
      lua_pop(L,2);
    }
  }
  lua_pop(L, 2);

  lua_pushnil(L); /* first key */
  while(lua_next(L, -2) != 0)
  {
    lua_getfield(L,-4,lua_tostring(L,-2));
    if(lua_isnil(L,-1)) {
      lua_pop(L,1);
      lua_setfield(L, -4, lua_tostring(L,-2));
    } else {
      lua_pop(L,2);
    }
  }


  lua_pop(L, 2);
}

static void init_metatable(lua_State *L, luaA_Type type_id)
{
  luaL_newmetatable(L, luaA_typename(L, type_id));

  lua_pushstring(L, luaA_typename(L, type_id));
  lua_setfield(L, -2, "__luaA_TypeName");

  lua_pushinteger(L, type_id);
  lua_setfield(L, -2, "__luaA_Type");

  lua_pushvalue(L, -1);
  lua_pushcclosure(L, autotype_next, 1);
  lua_setfield(L, -2, "__next");

  lua_pushvalue(L, -1);
  lua_pushcclosure(L, autotype_pairs, 1);
  lua_setfield(L, -2, "__pairs");

  lua_pushvalue(L, -1);
  lua_pushcclosure(L, autotype_index, 1);
  lua_setfield(L, -2, "__index");

  lua_pushvalue(L, -1);
  lua_pushcclosure(L, autotype_newindex, 1);
  lua_setfield(L, -2, "__newindex");

  lua_newtable(L);
  lua_setfield(L, -2, "__get");

  lua_newtable(L);
  lua_setfield(L, -2, "__set");

  lua_pushvalue(L, -1);
  lua_pushcclosure(L, autotype_tostring, 1);
  lua_setfield(L, -2, "__tostring");

  // leave metatable on top of stack
}


luaA_Type dt_lua_init_type_type(lua_State *L, luaA_Type type_id)
{
  init_metatable(L, type_id);
  lua_pop(L, 1);
  luaA_conversion_type(L, type_id, full_pushfunc, full_tofunc);
  return type_id;
}

luaA_Type dt_lua_init_singleton(lua_State *L, const char *unique_name, void *data)
{
  char tmp_name[1024];
  snprintf(tmp_name, sizeof(tmp_name), "dt_lua_singleton_%s", unique_name);

  luaA_Type type_id = luaA_type_add(L, tmp_name, sizeof(void *));
  init_metatable(L, type_id);

  void **udata = lua_newuserdatauv(L, sizeof(void *), 1);
  lua_newtable(L);
  lua_setiuservalue(L, -2, 1);
  if(!data)
  {
    memset(udata, 0, sizeof(void *));
  }
  else
  {
    *udata = data;
    luaL_getsubtable(L, LUA_REGISTRYINDEX, "dt_lua_gpointer_values");
    lua_pushlightuserdata(L, data);
    lua_pushvalue(L,-3);
    lua_settable(L,-3);
    lua_pop(L,1);
  }

  lua_pushvalue(L, -1);
  luaL_setmetatable(L, tmp_name);
  lua_setfield(L, -3, "__singleton");
  if(luaL_getmetafield(L, -1, "__init"))
  {
    lua_pushvalue(L, -2);                   // the new allocated object
    lua_pushlightuserdata(L, (void *)data); // forced to cast..
    lua_call(L, 2, 0);
  }
  lua_remove(L, -2);

  return type_id;
}


static int wrapped_index(lua_State *L)
{
  luaL_getmetafield(L, 1, "__pusher");
  lua_pushvalue(L, 1);
  lua_call(L, 1, 1);
  lua_pushvalue(L, 2);
  lua_gettable(L, -2);
  lua_remove(L, 1);
  lua_remove(L, 1);
  return 1;
}

static int wrapped_pairs(lua_State *L)
{
  luaL_getmetafield(L, 1, "__pusher");
  lua_pushvalue(L, 1);
  lua_call(L, 1, 1);
  luaL_getmetafield(L, -1, "__pairs");
  lua_pushvalue(L, -2);
  lua_call(L, 1, 3);
  return 3;
}
static int wrapped_newindex(lua_State *L)
{
  return luaL_error(L, "TBSL");
}
static int wrapped_tostring(lua_State *L)
{
  return luaL_error(L, "TBSL");
}


luaA_Type dt_lua_init_wrapped_singleton(lua_State *L, lua_CFunction pusher, lua_CFunction getter,
                                        const char *unique_name, void *data)
{
  luaA_Type result = dt_lua_init_singleton(L, unique_name, data);
  lua_getmetatable(L, -1);
  lua_pushcfunction(L, wrapped_index);
  lua_setfield(L, -2, "__index");
  lua_pushcfunction(L, wrapped_newindex);
  lua_setfield(L, -2, "__newindex");
  lua_pushcfunction(L, wrapped_pairs);
  lua_setfield(L, -2, "__pairs");
  lua_pushcfunction(L, wrapped_tostring);
  lua_setfield(L, -2, "__tostring");
  lua_pushcfunction(L, pusher);
  lua_setfield(L, -2, "__pusher");
  lua_pushcfunction(L, getter);
  lua_setfield(L, -2, "__getter");
  lua_pop(L, 1);
  return result;
}

luaA_Type dt_lua_init_int_type_type(lua_State *L, luaA_Type type_id)
{
  init_metatable(L, type_id);
  lua_newtable(L);
  // metatable of __values
  lua_newtable(L);
  lua_pushstring(L, "kv");
  lua_setfield(L, -2, "__mode");
  lua_setmetatable(L, -2);

  lua_setfield(L, -2, "__values");
  lua_pop(L, 1);
  luaA_conversion_type(L, type_id, int_pushfunc, int_tofunc);
  return type_id;
}

static int gpointer_wrapper(lua_State*L)
{
  gpointer *udata = (gpointer*)lua_touserdata(L,1);
  if(!*udata) {
    luaL_getmetafield(L,1,"__luaA_TypeName");
    luaL_error(L,"Attempting to access an invalid object of type %s",lua_tostring(L,-1));
  }
  lua_CFunction callback = lua_tocfunction(L,lua_upvalueindex(1));
  return callback(L);
}


luaA_Type dt_lua_init_gpointer_type_type(lua_State *L, luaA_Type type_id)
{
  init_metatable(L, type_id);

  lua_getfield(L,-1,"__next");
  lua_pushcclosure(L, gpointer_wrapper,1);
  lua_setfield(L, -2, "__next");

  lua_getfield(L,-1,"__index");
  lua_pushcclosure(L, gpointer_wrapper,1);
  lua_setfield(L, -2, "__index");

  lua_getfield(L,-1,"__newindex");
  lua_pushcclosure(L, gpointer_wrapper,1);
  lua_setfield(L, -2, "__newindex");

  lua_getfield(L,-1,"__pairs");
  lua_pushcclosure(L, gpointer_wrapper,1);
  lua_setfield(L, -2, "__pairs");

  lua_getfield(L,-1,"__tostring");
  lua_pushcclosure(L, gpointer_wrapper,1);
  lua_setfield(L, -2, "__tostring");

  lua_pop(L, 1);

  luaA_conversion_type(L, type_id, gpointer_pushfunc, gpointer_tofunc);
  return type_id;
}

void dt_lua_type_gpointer_alias_type(lua_State*L,luaA_Type type_id,void* pointer,void* alias)
{
  luaL_getsubtable(L, LUA_REGISTRYINDEX, "dt_lua_gpointer_values");
  lua_pushlightuserdata(L, pointer);
  lua_gettable(L, -2);
  if(lua_isnoneornil(L, -1))
  {
    luaL_error(L,"Adding an alias to an unknown object for type %s",luaA_typename(L,type_id));
  }
  lua_pushlightuserdata(L,alias);
  lua_insert(L,-2);
  lua_settable(L,-3);
  lua_pop(L,1);


}

void dt_lua_type_gpointer_drop(lua_State*L, void* pointer)
{
  luaL_getsubtable(L, LUA_REGISTRYINDEX, "dt_lua_gpointer_values");

  lua_pushlightuserdata(L, pointer);
  lua_gettable(L,-2);
  gpointer *udata = (gpointer*)lua_touserdata(L,-1);
  if(lua_isnil(L,-1)) {
    lua_pop(L,2);
    return; // this table is weak, the object has been gc
  }
  *udata = NULL;
  lua_pop(L,1);

  lua_pushlightuserdata(L, pointer);
  lua_pushnil(L);
  lua_settable(L,-3);

  lua_pop(L,1);

}

gboolean dt_lua_isa_type(lua_State *L, int index, luaA_Type type_id)
{
  if(!luaL_getmetafield(L, index, "__luaA_Type")) return false;
  int obj_type = luaL_checkinteger(L, -1);
  lua_pop(L, 1);
  return dt_lua_typeisa_type(L, obj_type, type_id);
}

gboolean dt_lua_typeisa_type(lua_State *L, luaA_Type obj_type, luaA_Type type_id)
{
  if(obj_type == type_id) return true;
  luaL_getmetatable(L, luaA_typename(L, obj_type));
  lua_getfield(L, -1, "__luaA_ParentMetatable");
  if(lua_isnil(L, -1))
  {
    lua_pop(L, 2);
    return false;
  }
  lua_getfield(L, -1, "__luaA_Type");
  int parent_type = luaL_checkinteger(L, -1);
  lua_pop(L, 3);
  return dt_lua_typeisa_type(L, parent_type, type_id);
}

void dt_lua_type_setmetafield_type(lua_State*L,luaA_Type type_id,const char* method_name)
{
  // These metafields should never be overridden by user code
  if(
      !strcmp(method_name,"__index") ||
      !strcmp(method_name,"__newindex") ||
      !strcmp(method_name,"__number_index") ||
      !strcmp(method_name,"__number_newindex") ||
      !strcmp(method_name,"__pairs") ||
      !strcmp(method_name,"__next") ||
      !strcmp(method_name,"__get") ||
      !strcmp(method_name,"__set") ||
      !strcmp(method_name,"__len") ||
      !strcmp(method_name,"__luaA_Type") ||
      !strcmp(method_name,"__luaA_TypeName") ||
      !strcmp(method_name,"__luaA_ParentMetatable") ||
      !strcmp(method_name,"__init") ||
      !strcmp(method_name,"__values") ||
      !strcmp(method_name,"__singleton") ||
      !strcmp(method_name,"__pusher") ||
      !strcmp(method_name,"__getter") ||
      !strcmp(method_name,"__mode") ||
      0) {
        luaL_error(L,"non-core lua code is not allowed to change meta-field %s\n",method_name);
  } else if(!strcmp(method_name,"__tostring")) {
    luaL_getmetatable(L, luaA_typename(L, type_id));
    lua_pushvalue(L,-2);
    lua_setfield(L, -2, "__real_tostring");
    lua_pop(L, 2); // pop the metatable and the value
    return;
  // whitelist for specific types
  } else if(
      // if you add a type here, make sure it handles inheritance of metamethods itself
      // typically, set the metamethod not for the parent type but just after inheritance
      ( !strcmp(method_name,"__associated_object")&& dt_lua_typeisa_type(L,type_id,luaA_type_find(L,"dt_imageio_module_format_t"))) ||
      ( !strcmp(method_name,"__associated_object")&& dt_lua_typeisa_type(L,type_id,luaA_type_find(L,"dt_imageio_module_storage_t"))) ||
      ( !strcmp(method_name,"__gc")&& dt_lua_typeisa_type(L,type_id,luaA_type_find(L,"dt_style_t"))) ||
      ( !strcmp(method_name,"__gc")&& dt_lua_typeisa_type(L,type_id,luaA_type_find(L,"dt_style_item_t"))) ||
      ( !strcmp(method_name,"__gc")&& dt_lua_typeisa_type(L,type_id,luaA_type_find(L,"lua_widget"))) ||
      ( !strcmp(method_name,"__call")&& dt_lua_typeisa_type(L,type_id,luaA_type_find(L,"lua_widget"))) ||
      ( !strcmp(method_name,"__gtk_signals")&& dt_lua_typeisa_type(L,type_id,luaA_type_find(L,"lua_widget"))) ||
      0) {
    // Nothing to be done
  } else {
    luaL_error(L,"metafield not handled :%s for type %s\n",method_name,luaA_typename(L,type_id));
  }
  luaL_getmetatable(L, luaA_typename(L, type_id));
  lua_pushvalue(L,-2);
  lua_setfield(L, -2, method_name);
  lua_pop(L, 2); // pop the metatable and the value
}

int dt_lua_init_early_types(lua_State *L)
{
  luaA_conversion(L, char_20, push_char_array, to_char20);
  luaA_conversion_push(L, const char_20, push_char_array);
  luaA_conversion(L, char_32, push_char_array, to_char32);
  luaA_conversion_push(L, const char_32, push_char_array);
  luaA_conversion(L, char_52, push_char_array, to_char52);
  luaA_conversion_push(L, const char_52, push_char_array);
  luaA_conversion(L, char_64, push_char_array, to_char64);
  luaA_conversion_push(L, const char_64, push_char_array);
  luaA_conversion(L, char_128, push_char_array, to_char128);
  luaA_conversion_push(L, const char_128, push_char_array);
  luaA_conversion(L, char_256, push_char_array, to_char256);
  luaA_conversion_push(L, const char_256, push_char_array);
  luaA_conversion(L, char_512, push_char_array, to_char512);
  luaA_conversion_push(L, const char_512, push_char_array);
  luaA_conversion(L, char_1024, push_char_array, to_char1024);
  luaA_conversion_push(L, const char_1024, push_char_array);
  luaA_conversion(L, char_filename_length, push_char_array, to_charfilename_length);
  luaA_conversion_push(L, const char_filename_length, push_char_array);
  luaA_conversion(L, char_path_length, push_char_array, to_charpath_length);
  luaA_conversion_push(L, const char_path_length, push_char_array);
  luaA_conversion(L, int32_t, luaA_push_int, luaA_to_int);
  luaA_conversion_push(L, const int32_t, luaA_push_int);
  luaA_conversion_push(L, const_string, luaA_push_const_char_ptr);
  luaA_conversion(L, protected_double, push_protected_double, luaA_to_double);
  luaA_conversion(L, progress_double, push_progress_double, to_progress_double);

  luaA_conversion_push_type(L, luaA_type_add(L,"unknown",sizeof(void*)), unknown_pushfunc);
  // table of gpointer values
  lua_newtable(L);
  lua_newtable(L);
  lua_pushstring(L, "kv");
  lua_setfield(L, -2, "__mode");
  lua_setmetatable(L, -2);

  lua_setfield(L, LUA_REGISTRYINDEX, "dt_lua_gpointer_values");

  luaA_enum(L,dt_lua_orientation_t);
  luaA_enum_value_name(L,dt_lua_orientation_t,GTK_ORIENTATION_HORIZONTAL,"horizontal");
  luaA_enum_value_name(L,dt_lua_orientation_t,GTK_ORIENTATION_VERTICAL,"vertical");

  luaA_enum(L, dt_lua_align_t);
  luaA_enum_value_name(L, dt_lua_align_t, GTK_ALIGN_FILL, "fill");
  luaA_enum_value_name(L, dt_lua_align_t, GTK_ALIGN_START, "start");
  luaA_enum_value_name(L, dt_lua_align_t, GTK_ALIGN_END, "end");
  luaA_enum_value_name(L, dt_lua_align_t, GTK_ALIGN_CENTER, "center");
  luaA_enum_value_name(L, dt_lua_align_t, GTK_ALIGN_BASELINE, "baseline");

  luaA_enum(L, dt_lua_ellipsize_mode_t);
  luaA_enum_value_name(L, dt_lua_ellipsize_mode_t, PANGO_ELLIPSIZE_NONE, "none");
  luaA_enum_value_name(L, dt_lua_ellipsize_mode_t, PANGO_ELLIPSIZE_START, "start");
  luaA_enum_value_name(L, dt_lua_ellipsize_mode_t, PANGO_ELLIPSIZE_MIDDLE, "middle");
  luaA_enum_value_name(L, dt_lua_ellipsize_mode_t, PANGO_ELLIPSIZE_END, "end");

  return 0;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

