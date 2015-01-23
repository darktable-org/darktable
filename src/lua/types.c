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
#include "lua/call.h"
#include <string.h>
#include <stdarg.h>
#include <math.h>

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
  if(!isnormal(value))
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
/* METATBLE CALLBACSK FOR AUTOTYPES */
/************************************/
static int autotype_inext(lua_State *L)
{
  luaL_getmetafield(L, 1, "__len");
  lua_pushvalue(L, -3);
  lua_call(L, 1, 1);
  int length = lua_tonumber(L, -1);
  lua_pop(L, 1);
  int key = 0;
  if(length == 0)
  {
    lua_pop(L, 1);
    lua_pushnil(L);
    return 1;
  }
  else if(lua_isnil(L, -1))
  {
    key = 1;
    lua_pop(L, 1);
    lua_pushnumber(L, key);
    lua_pushnumber(L, key);
    lua_gettable(L, -3);
    return 2;
  }
  else if(luaL_checknumber(L, -1) < length)
  {
    key = lua_tonumber(L, -1) + 1;
    lua_pop(L, 1);
    lua_pushnumber(L, key);
    lua_pushnumber(L, key);
    lua_gettable(L, -3);
    return 2;
  }
  else
  {
    // we reached the end of ipairs
    lua_pop(L, 1);
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
      lua_pushnumber(L, key);
      lua_pushnumber(L, key);
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
        int result = dt_lua_dostring(L, "args ={...}; return args[1][args[2]]", 2, 1);
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



static int autotype_ipairs(lua_State *L)
{
  luaL_getmetafield(L, 1, "__inext");
  lua_pushvalue(L, -2);
  lua_pushinteger(L, 0); // index set to 0 for reset
  return 3;
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

/*************************/
/* PUSH AND TO FUNCTIONS */
/*************************/

static int full_pushfunc(lua_State *L, luaA_Type type_id, const void *cin)
{
  size_t type_size = luaA_typesize(L, type_id);
  void *udata = lua_newuserdata(L, type_size);
  lua_newtable(L);
  lua_setuservalue(L, -2);
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
    lua_pushvalue(L, -2);                  // the new alocated object
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
  lua_pushnumber(L, singleton);
  lua_gettable(L, -2);
  if(lua_isnoneornil(L, -1))
  {
    lua_pop(L, 1);
    int *udata = lua_newuserdata(L, sizeof(int));
    *udata = singleton;
    luaL_setmetatable(L, luaA_typename(L, type_id));
    lua_pushinteger(L, singleton);
    // warning : no uservalue
    lua_pushvalue(L, -2);
    lua_settable(L, -4);
    if(luaL_getmetafield(L, -1, "__init"))
    {
      lua_pushvalue(L, -2);                  // the new alocated object
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
  luaL_getmetatable(L, luaA_typename(L, type_id));
  luaL_getsubtable(L, -1, "__values");
  gpointer singleton = *(gpointer *)cin;
  lua_pushlightuserdata(L, singleton);
  lua_gettable(L, -2);
  if(lua_isnoneornil(L, -1))
  {
    lua_pop(L, 1);
    gpointer *udata = lua_newuserdata(L, sizeof(gpointer));
    lua_newtable(L);
    lua_setuservalue(L, -2);
    *udata = singleton;
    luaL_setmetatable(L, luaA_typename(L, type_id));
    lua_pushlightuserdata(L, singleton);
    lua_pushvalue(L, -2);
    lua_settable(L, -4);
    if(luaL_getmetafield(L, -1, "__init"))
    {
      lua_pushvalue(L, -2);                  // the new alocated object
      lua_pushlightuserdata(L, (void *)cin); // forced to cast..
      lua_call(L, 2, 0);
    }
  }
  lua_remove(L, -2); //__values
  lua_remove(L, -2); // metatable
  return 1;
}

static void gpointer_tofunc(lua_State *L, luaA_Type type_id, void *cout, int index)
{
  if(!dt_lua_isa_type(L,index,type_id)) {
    char error_msg[256];
    snprintf(error_msg,sizeof(error_msg),"%s expected",luaA_typename(L,type_id));
    luaL_argerror(L,index,error_msg);
  } 
  void* udata = lua_touserdata(L,index);
  memcpy(cout, udata, sizeof(gpointer));
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

    lua_pushcfunction(L, autotype_ipairs);
    lua_setfield(L, -2, "__ipairs");

    lua_pushcfunction(L, autotype_inext);
    lua_setfield(L, -2, "__inext");
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

    lua_pushcfunction(L, autotype_ipairs);
    lua_setfield(L, -2, "__ipairs");

    lua_pushcfunction(L, autotype_inext);
    lua_setfield(L, -2, "__inext");
  }

  lua_pop(L, 3);
}

int dt_lua_type_member_luaautoc(lua_State *L)
{
  const char *member_name = luaL_checkstring(L, 2);
  luaL_getmetafield(L, 1, "__luaA_Type");
  luaA_Type my_type = luaL_checkint(L, -1);
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

  lua_pushnumber(L, type_id);
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

  void **udata = lua_newuserdata(L, sizeof(void *));
  if(!data)
  {
    memset(udata, 0, sizeof(void *));
  }
  else
  {
    *udata = data;
  }

  lua_pushvalue(L, -1);
  luaL_setmetatable(L, tmp_name);
  lua_setfield(L, -3, "__singleton");
  if(luaL_getmetafield(L, -1, "__init"))
  {
    lua_pushvalue(L, -2);                   // the new alocated object
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
static int wrapped_ipairs(lua_State *L)
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
  lua_pushcfunction(L, wrapped_ipairs);
  lua_setfield(L, -2, "__ipairs");
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


luaA_Type dt_lua_init_gpointer_type_type(lua_State *L, luaA_Type type_id)
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
  luaA_conversion_type(L, type_id, gpointer_pushfunc, gpointer_tofunc);
  return type_id;
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


  return 0;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
