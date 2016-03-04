#include "lautoc.h"

void luaA_open(lua_State* L) {

  lua_pushinteger(L, 0); lua_setfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "type_index");
  lua_newtable(L); lua_setfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "type_ids");
  lua_newtable(L); lua_setfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "type_names");
  lua_newtable(L); lua_setfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "type_sizes");
  
  lua_newtable(L); lua_setfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "stack_push");
  lua_newtable(L); lua_setfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "stack_to");
  
  lua_newtable(L); lua_setfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "structs");
  lua_newtable(L); lua_setfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "structs_offset");
  lua_newtable(L); lua_setfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "enums");
  lua_newtable(L); lua_setfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "enums_sizes");
  lua_newtable(L); lua_setfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "enums_values");
  lua_newtable(L); lua_setfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "functions");
  
  lua_newuserdata(L, LUAA_RETURN_STACK_SIZE); lua_setfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "call_ret_stk");
  lua_newuserdata(L, LUAA_ARGUMENT_STACK_SIZE); lua_setfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "call_arg_stk");
  lua_pushinteger(L, 0); lua_setfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "call_ret_ptr");
  lua_pushinteger(L, 0); lua_setfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "call_arg_ptr");
  
  // compiler does weird macro expansion with "bool" so no magic macro for you
  luaA_conversion_type(L, luaA_type_add(L,"bool",sizeof(bool)), luaA_push_bool, luaA_to_bool);
  luaA_conversion_type(L, luaA_type_add(L,"_Bool",sizeof(bool)), luaA_push_bool, luaA_to_bool);
  luaA_conversion(L, char, luaA_push_char, luaA_to_char);
  luaA_conversion(L, signed char, luaA_push_signed_char, luaA_to_signed_char);
  luaA_conversion(L, unsigned char, luaA_push_unsigned_char, luaA_to_unsigned_char);
  luaA_conversion(L, short, luaA_push_short, luaA_to_short);
  luaA_conversion(L, unsigned short, luaA_push_unsigned_short, luaA_to_unsigned_short);
  luaA_conversion(L, int, luaA_push_int, luaA_to_int);
  luaA_conversion(L, unsigned int, luaA_push_unsigned_int, luaA_to_unsigned_int);
  luaA_conversion(L, long, luaA_push_long, luaA_to_long);
  luaA_conversion(L, unsigned long, luaA_push_unsigned_long, luaA_to_unsigned_long);
  luaA_conversion(L, long long, luaA_push_long_long, luaA_to_long_long);
  luaA_conversion(L, unsigned long long, luaA_push_unsigned_long_long, luaA_to_unsigned_long_long);
  luaA_conversion(L, float, luaA_push_float, luaA_to_float);
  luaA_conversion(L, double, luaA_push_double, luaA_to_double);
  luaA_conversion(L, long double, luaA_push_long_double, luaA_to_long_double);
  
  luaA_conversion_push_type(L, luaA_type_add(L,"const bool",sizeof(bool)), luaA_push_bool);
  luaA_conversion_push_type(L, luaA_type_add(L,"const _Bool",sizeof(bool)), luaA_push_bool);
  luaA_conversion_push(L, const char, luaA_push_char);
  luaA_conversion_push(L, const signed char, luaA_push_signed_char);
  luaA_conversion_push(L, const unsigned char, luaA_push_unsigned_char);
  luaA_conversion_push(L, const short, luaA_push_short);
  luaA_conversion_push(L, const unsigned short, luaA_push_unsigned_short);
  luaA_conversion_push(L, const int, luaA_push_int);
  luaA_conversion_push(L, const unsigned int, luaA_push_unsigned_int);
  luaA_conversion_push(L, const long, luaA_push_long);
  luaA_conversion_push(L, const unsigned long, luaA_push_unsigned_long);
  luaA_conversion_push(L, const long long, luaA_push_long_long);
  luaA_conversion_push(L, const unsigned long long, luaA_push_unsigned_long_long);
  luaA_conversion_push(L, const float, luaA_push_float);
  luaA_conversion_push(L, const double, luaA_push_double);
  luaA_conversion_push(L, const long double, luaA_push_long_double);
  
  luaA_conversion(L, char*, luaA_push_char_ptr, luaA_to_char_ptr);
  luaA_conversion(L, const char*, luaA_push_const_char_ptr, luaA_to_const_char_ptr);
  luaA_conversion(L, void*, luaA_push_void_ptr, luaA_to_void_ptr);
  
  luaA_conversion_push(L, void, luaA_push_void);
  
}

void luaA_close(lua_State* L) {

  lua_pushnil(L); lua_setfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "type_index");
  lua_pushnil(L); lua_setfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "type_ids");
  lua_pushnil(L); lua_setfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "type_names");
  lua_pushnil(L); lua_setfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "type_sizes");
  
  lua_pushnil(L); lua_setfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "stack_push");
  lua_pushnil(L); lua_setfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "stack_to");
  
  lua_pushnil(L); lua_setfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "structs");
  lua_pushnil(L); lua_setfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "structs_offset");
  lua_pushnil(L); lua_setfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "enums");
  lua_pushnil(L); lua_setfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "enums_sizes");
  lua_pushnil(L); lua_setfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "enums_values");
  lua_pushnil(L); lua_setfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "functions");
  
  lua_pushnil(L); lua_setfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "call_ret_stk");
  lua_pushnil(L); lua_setfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "call_arg_stk");
  lua_pushnil(L); lua_setfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "call_ret_ptr");
  lua_pushnil(L); lua_setfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "call_arg_ptr");
  
}

/*
** Types
*/

luaA_Type luaA_type_add(lua_State* L, const char* type, size_t size) {
  
  lua_getfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "type_ids");
  lua_getfield(L, -1, type);
  
  if (lua_isnumber(L, -1)) {
  
    luaA_Type id = lua_tointeger(L, -1);
    lua_pop(L, 2);
    return id;
  
  } else {
  
    lua_pop(L, 2);
    
    lua_getfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "type_index");
    
    luaA_Type id = lua_tointeger(L, -1);
    lua_pop(L, 1);
    id++;

    lua_pushinteger(L, id);
    lua_setfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "type_index");
    
    lua_getfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "type_ids");
    lua_pushinteger(L, id);
    lua_setfield(L, -2, type);
    lua_pop(L, 1);
    
    lua_getfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "type_names");
    lua_pushinteger(L, id);
    lua_pushstring(L, type);
    lua_settable(L, -3);
    lua_pop(L, 1);
    
    lua_getfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "type_sizes");
    lua_pushinteger(L, id);
    lua_pushinteger(L, size);
    lua_settable(L, -3);
    lua_pop(L, 1);
    
    return id;
    
  }
  
}

luaA_Type luaA_type_find(lua_State* L, const char* type) {
  
  lua_getfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "type_ids");  
  lua_getfield(L, -1, type);
  
  luaA_Type id = lua_isnil(L, -1) ? LUAA_INVALID_TYPE : lua_tointeger(L, -1);
  lua_pop(L, 2);
  
  return id;
}

const char* luaA_typename(lua_State* L, luaA_Type id) {
  
  lua_getfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "type_names");  
  lua_pushinteger(L, id);
  lua_gettable(L, -2);
  
  const char* type = lua_isnil(L, -1) ? "LUAA_INVALID_TYPE" : lua_tostring(L, -1);
  lua_pop(L, 2);
  
  return type;
}

size_t luaA_typesize(lua_State* L, luaA_Type id) {

  lua_getfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "type_sizes");  
  lua_pushinteger(L, id);
  lua_gettable(L, -2);
  
  size_t size = lua_isnil(L, -1) ? -1 : lua_tointeger(L, -1);
  lua_pop(L, 2);
  
  return size;
}

/*
** Stack
*/

int luaA_push_type(lua_State* L, luaA_Type type_id, const void* c_in) {
  
  lua_getfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "stack_push");
  lua_pushinteger(L, type_id);
  lua_gettable(L, -2);
  
  if (!lua_isnil(L, -1)) {
    luaA_Pushfunc func = lua_touserdata(L, -1);
    lua_pop(L, 2);
    return func(L, type_id, c_in);
  }
  
  lua_pop(L, 2);
  
  if (luaA_struct_registered_type(L, type_id)) {
    return luaA_struct_push_type(L, type_id, c_in);
  }
  
  if (luaA_enum_registered_type(L, type_id)) {
    return luaA_enum_push_type(L, type_id, c_in);
  }
  
  lua_pushfstring(L, "luaA_push: conversion to Lua object from type '%s' not registered!", luaA_typename(L, type_id));
  lua_error(L);
  return 0;
}

void luaA_to_type(lua_State* L, luaA_Type type_id, void* c_out, int index) {
  
  lua_getfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "stack_to");
  lua_pushinteger(L, type_id);
  lua_gettable(L, -2);
  
  if (!lua_isnil(L, -1)) {
    luaA_Tofunc func = lua_touserdata(L, -1);
    lua_pop(L, 2);
    func(L, type_id, c_out, index);
    return;
  }
  
  lua_pop(L, 2);
  
  if (luaA_struct_registered_type(L, type_id)) {
    luaA_struct_to_type(L, type_id, c_out, index);
    return;
  }
  
  if (luaA_enum_registered_type(L, type_id)) {
    luaA_enum_to_type(L, type_id, c_out, index);
    return;
  }
  
  lua_pushfstring(L, "luaA_to: conversion from Lua object to type '%s' not registered!", luaA_typename(L, type_id));
  lua_error(L);  
}

void luaA_conversion_type(lua_State* L, luaA_Type type_id, luaA_Pushfunc push_func, luaA_Tofunc to_func) {
  luaA_conversion_push_type(L, type_id, push_func);
  luaA_conversion_to_type(L, type_id, to_func);
}

void luaA_conversion_push_type(lua_State* L, luaA_Type type_id, luaA_Pushfunc func) {
  lua_getfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "stack_push");
  lua_pushinteger(L, type_id);
  lua_pushlightuserdata(L, func);
  lua_settable(L, -3);
  lua_pop(L, 1);
}

void luaA_conversion_to_type(lua_State* L, luaA_Type type_id, luaA_Tofunc func) {
  lua_getfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "stack_to");
  lua_pushinteger(L, type_id);
  lua_pushlightuserdata(L, func);
  lua_settable(L, -3);
  lua_pop(L, 1);
}

int luaA_push_bool(lua_State* L, luaA_Type type_id, const void* c_in) {
  lua_pushboolean(L, *(bool*)c_in);
  return 1;
}

void luaA_to_bool(lua_State* L, luaA_Type type_id,  void* c_out, int index) {
  *(bool*)c_out = lua_toboolean(L, index);
}

int luaA_push_char(lua_State* L, luaA_Type type_id, const void* c_in) {
  lua_pushinteger(L, *(char*)c_in);
  return 1;
}

void luaA_to_char(lua_State* L, luaA_Type type_id,  void* c_out, int index) {
  *(char*)c_out = lua_tointeger(L, index);
}

int luaA_push_signed_char(lua_State* L, luaA_Type type_id, const void* c_in) {
  lua_pushinteger(L, *(signed char*)c_in);
  return 1;
}

void luaA_to_signed_char(lua_State* L, luaA_Type type_id, void* c_out, int index) {
  *(signed char*)c_out = lua_tointeger(L, index);
}

int luaA_push_unsigned_char(lua_State* L, luaA_Type type_id, const void* c_in) {
  lua_pushinteger(L, *(unsigned char*)c_in);
  return 1;
}

void luaA_to_unsigned_char(lua_State* L, luaA_Type type_id, void* c_out, int index) {
  *(unsigned char*)c_out = lua_tointeger(L, index);
}

int luaA_push_short(lua_State* L, luaA_Type type_id, const void* c_in) {
  lua_pushinteger(L, *(short*)c_in);
  return 1;
}

void luaA_to_short(lua_State* L, luaA_Type type_id, void* c_out, int index) {
  *(short*)c_out = lua_tointeger(L, index);
}

int luaA_push_unsigned_short(lua_State* L, luaA_Type type_id, const void* c_in) {
  lua_pushinteger(L, *(unsigned short*)c_in);
  return 1;
}

void luaA_to_unsigned_short(lua_State* L, luaA_Type type_id, void* c_out, int index) {
  *(unsigned short*)c_out = lua_tointeger(L, index);
}

int luaA_push_int(lua_State* L, luaA_Type type_id, const void* c_in) {
  lua_pushinteger(L, *(int*)c_in);
  return 1;
}

void luaA_to_int(lua_State* L, luaA_Type type_id, void* c_out, int index) {
  *(int*)c_out = lua_tointeger(L, index);
}

int luaA_push_unsigned_int(lua_State* L, luaA_Type type_id, const void* c_in) {
  lua_pushinteger(L, *(unsigned int*)c_in);
  return 1;
}

void luaA_to_unsigned_int(lua_State* L, luaA_Type type_id, void* c_out, int index) {
  *(unsigned int*)c_out = lua_tointeger(L, index);
}

int luaA_push_long(lua_State* L, luaA_Type type_id, const void* c_in) {
  lua_pushinteger(L, *(long*)c_in);
  return 1;
}

void luaA_to_long(lua_State* L, luaA_Type type_id, void* c_out, int index) {
  *(long*)c_out = lua_tointeger(L, index);
}

int luaA_push_unsigned_long(lua_State* L, luaA_Type type_id, const void* c_in) {
  lua_pushinteger(L, *(unsigned long*)c_in);
  return 1;
}

void luaA_to_unsigned_long(lua_State* L, luaA_Type type_id, void* c_out, int index) {
  *(unsigned long*)c_out = lua_tointeger(L, index);
}

int luaA_push_long_long(lua_State* L, luaA_Type type_id, const void* c_in) {
  lua_pushinteger(L, *(long long*)c_in);
  return 1;
}

void luaA_to_long_long(lua_State* L, luaA_Type type_id, void* c_out, int index) {
  *(long long*)c_out = lua_tointeger(L, index);
}

int luaA_push_unsigned_long_long(lua_State* L, luaA_Type type_id, const void* c_in) {
  lua_pushinteger(L, *(unsigned long long*)c_in);
  return 1;
}

void luaA_to_unsigned_long_long(lua_State* L, luaA_Type type_id, void* c_out, int index) {
  *(unsigned long long*)c_out = lua_tointeger(L, index);
}

int luaA_push_float(lua_State* L, luaA_Type type_id, const void* c_in) {
  lua_pushnumber(L, *(float*)c_in);
  return 1;
}

void luaA_to_float(lua_State* L, luaA_Type type_id, void* c_out, int index) {
  *(float*)c_out = lua_tonumber(L, index);
}

int luaA_push_double(lua_State* L, luaA_Type type_id, const void* c_in) {
  lua_pushnumber(L, *(double*)c_in);
  return 1;
}

void luaA_to_double(lua_State* L, luaA_Type type_id, void* c_out, int index) {
  *(double*)c_out = lua_tonumber(L, index);
}

int luaA_push_long_double(lua_State* L, luaA_Type type_id, const void* c_in) {
  lua_pushnumber(L, *(long double*)c_in);
  return 1;
}

void luaA_to_long_double(lua_State* L, luaA_Type type_id, void* c_out, int index) {
  *(long double*)c_out = lua_tonumber(L, index);
}

int luaA_push_char_ptr(lua_State* L, luaA_Type type_id, const void* c_in) {
  lua_pushstring(L, *(char**)c_in);
  return 1;
}

void luaA_to_char_ptr(lua_State* L, luaA_Type type_id, void* c_out, int index) {
  *(char**)c_out = (char*)lua_tostring(L, index);
}

int luaA_push_const_char_ptr(lua_State* L, luaA_Type type_id, const void* c_in) {
  lua_pushstring(L, *(const char**)c_in);
  return 1;
}

void luaA_to_const_char_ptr(lua_State* L, luaA_Type type_id, void* c_out, int index) {
  *(const char**)c_out = lua_tostring(L, index);
}

int luaA_push_void_ptr(lua_State* L, luaA_Type type_id, const void* c_in) {
  lua_pushlightuserdata(L, *(void**)c_in);
  return 1;
}

void luaA_to_void_ptr(lua_State* L, luaA_Type type_id, void* c_out, int index) {
  *(void**)c_out = (void*)lua_touserdata(L, index);
}

int luaA_push_void(lua_State* L, luaA_Type type_id, const void* c_in) {
  lua_pushnil(L);
  return 1;
}

bool luaA_conversion_registered_type(lua_State* L, luaA_Type type_id) {
  return (luaA_conversion_push_registered_type(L, type_id)
       && luaA_conversion_to_registered_type(L, type_id));
}

bool luaA_conversion_push_registered_type(lua_State* L, luaA_Type type_id) {

  lua_getfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "stack_push");  
  lua_pushinteger(L, type_id);
  lua_gettable(L, -2);
  
  bool reg = !lua_isnil(L, -1);
  lua_pop(L, 2);
  
  return reg;
}

bool luaA_conversion_to_registered_type(lua_State* L, luaA_Type type_id) {

  lua_getfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "stack_to");  
  lua_pushinteger(L, type_id);
  lua_gettable(L, -2);
  
  bool reg = !lua_isnil(L, -1);
  lua_pop(L, 2);

  return reg;
}

/*
** Structs
*/

int luaA_struct_push_member_offset_type(lua_State* L, luaA_Type type, size_t offset, const void* c_in) {
  
  lua_getfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "structs_offset");
  lua_pushinteger(L, type);
  lua_gettable(L, -2);
  
  if (!lua_isnil(L, -1)) {
    
    lua_pushinteger(L, offset);
    lua_gettable(L, -2);
    
    if (!lua_isnil(L, -1)) {
      lua_getfield(L, -1, "type");
      luaA_Type stype = lua_tointeger(L, -1);
      lua_pop(L, 4);
      return luaA_push_type(L, stype, c_in + offset);
    }
    
    lua_pop(L, 3);
    lua_pushfstring(L, "luaA_struct_push_member: Member offset '%d' not registered for struct '%s'!", offset, luaA_typename(L, type));
    lua_error(L);

  }
  
  lua_pop(L, 2);
  lua_pushfstring(L, "luaA_struct_push_member: Struct '%s' not registered!", luaA_typename(L, type));
  lua_error(L);
  return 0;
  
}

int luaA_struct_push_member_name_type(lua_State* L, luaA_Type type, const char* member, const void* c_in) {

  lua_getfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "structs");
  lua_pushinteger(L, type);
  lua_gettable(L, -2);
  
  if (!lua_isnil(L, -1)) {
    
    lua_getfield(L, -1, member);
    
    if (!lua_isnil(L, -1)) {
      lua_getfield(L, -1, "type");
      luaA_Type stype = lua_tointeger(L, -1);
      lua_pop(L, 1);
      lua_getfield(L, -1, "offset");
      size_t offset = lua_tointeger(L, -1);
      lua_pop(L, 4);
      return luaA_push_type(L, stype, c_in + offset);
    }
    
    lua_pop(L, 3);
    lua_pushfstring(L, "luaA_struct_push_member: Member name '%s' not registered for struct '%s'!", member, luaA_typename(L, type));
    lua_error(L);

  }
  
  lua_pop(L, 2);
  lua_pushfstring(L, "luaA_struct_push_member: Struct '%s' not registered!", luaA_typename(L, type));
  lua_error(L);
  return 0;
}

void luaA_struct_to_member_offset_type(lua_State* L, luaA_Type type, size_t offset, void* c_out, int index) {

  lua_getfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "structs_offset");
  lua_pushinteger(L, type);
  lua_gettable(L, -2);
  
  if (!lua_isnil(L, -1)) {
    
    lua_pushinteger(L, offset);
    lua_gettable(L, -2);
    
    if (!lua_isnil(L, -1)) {
      lua_getfield(L, -1, "type");
      luaA_Type stype = lua_tointeger(L, -1);
      lua_pop(L, 4);
      luaA_to_type(L, stype, c_out + offset, index);
      return;
    }
    
    lua_pop(L, 3);
    lua_pushfstring(L, "luaA_struct_to_member: Member offset '%d' not registered for struct '%s'!", offset, luaA_typename(L, type));
    lua_error(L);

  }
  
  lua_pop(L, 2);
  lua_pushfstring(L, "luaA_struct_to_member: Struct '%s' not registered!", luaA_typename(L, type));
  lua_error(L);
  
}

void luaA_struct_to_member_name_type(lua_State* L, luaA_Type type, const char* member, void* c_out, int index) {

  lua_getfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "structs");
  lua_pushinteger(L, type);
  lua_gettable(L, -2);
  
  if (!lua_isnil(L, -1)) {
    
    lua_pushstring(L, member);
    lua_gettable(L, -2);
    
    if (!lua_isnil(L, -1)) {
      lua_getfield(L, -1, "type");
      luaA_Type stype = lua_tointeger(L, -1);
      lua_pop(L, 1);
      lua_getfield(L, -1, "offset");
      size_t offset = lua_tointeger(L, -1);
      lua_pop(L, 4);
      luaA_to_type(L, stype, c_out + offset, index);
      return;
    }
    
    lua_pop(L, 3);
    lua_pushfstring(L, "luaA_struct_to_member: Member name '%s' not registered for struct '%s'!", member, luaA_typename(L, type));
    lua_error(L);

  }
  
  lua_pop(L, 2);
  lua_pushfstring(L, "luaA_struct_to_member: Struct '%s' not registered!", luaA_typename(L, type));
  lua_error(L);
  
}

bool luaA_struct_has_member_offset_type(lua_State* L, luaA_Type type, size_t offset) {

  lua_getfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "structs_offset");
  lua_pushinteger(L, type);
  lua_gettable(L, -2);
  
  if (!lua_isnil(L, -1)) {
    
    lua_pushinteger(L, offset);
    lua_gettable(L, -2);
    
    if (!lua_isnil(L, -1)) {
      lua_pop(L, 3);
      return true;
    }
    
    lua_pop(L, 3);
    return false;

  }
  
  lua_pop(L, 2);
  lua_pushfstring(L, "luaA_struct_has_member: Struct '%s' not registered!", luaA_typename(L, type));
  lua_error(L);
  return false;
  
}

bool luaA_struct_has_member_name_type(lua_State* L, luaA_Type type, const char* member) {

  lua_getfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "structs");
  lua_pushinteger(L, type);
  lua_gettable(L, -2);
  
  if (!lua_isnil(L, -1)) {
    
    lua_pushstring(L, member);
    lua_gettable(L, -2);
    
    if (!lua_isnil(L, -1)) {
      lua_pop(L, 3);
      return true;
    }
    
    lua_pop(L, 3);
    return false;

  }
  
  lua_pop(L, 2);
  lua_pushfstring(L, "luaA_struct_has_member: Struct '%s' not registered!", luaA_typename(L, type));
  lua_error(L);
  return false;
  
}

luaA_Type luaA_struct_typeof_member_offset_type(lua_State* L, luaA_Type type,  size_t offset) {

  lua_getfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "structs_offset");
  lua_pushinteger(L, type);
  lua_gettable(L, -2);
  
  if (!lua_isnil(L, -1)) {
    
    lua_pushinteger(L, offset);
    lua_gettable(L, -2);
    
    if (!lua_isnil(L, -1)) {
      lua_getfield(L, -1, "type");
      luaA_Type stype = lua_tointeger(L, -1);
      lua_pop(L, 4);
      return stype;
    }
    
    lua_pop(L, 3);
    lua_pushfstring(L, "luaA_struct_typeof_member: Member offset '%d' not registered for struct '%s'!", offset, luaA_typename(L, type));
    lua_error(L);

  }
  
  lua_pop(L, 2);
  lua_pushfstring(L, "luaA_struct_typeof_member: Struct '%s' not registered!", luaA_typename(L, type));
  lua_error(L);
  return 0;
  
}

luaA_Type luaA_struct_typeof_member_name_type(lua_State* L, luaA_Type type,  const char* member) {

  lua_getfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "structs");
  lua_pushinteger(L, type);
  lua_gettable(L, -2);
  
  if (!lua_isnil(L, -1)) {
    
    lua_pushstring(L, member);
    lua_gettable(L, -2);
    
    if (!lua_isnil(L, -1)) {
      lua_getfield(L, -1, "type");
      luaA_Type type = lua_tointeger(L, -1);
      lua_pop(L, 4);
      return type;
    }
    
    lua_pop(L, 3);
    lua_pushfstring(L, "luaA_struct_typeof_member: Member name '%s' not registered for struct '%s'!", member, luaA_typename(L, type));
    lua_error(L);

  }
  
  lua_pop(L, 2);
  lua_pushfstring(L, "luaA_struct_typeof_member: Struct '%s' not registered!", luaA_typename(L, type));
  lua_error(L);
  return 0;
  
}

void luaA_struct_type(lua_State* L, luaA_Type type) {
  lua_getfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "structs");
  lua_pushinteger(L, type);
  lua_newtable(L);
  lua_settable(L, -3);
  lua_pop(L, 1);
  
  lua_getfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "structs_offset");
  lua_pushinteger(L, type);
  lua_newtable(L);
  lua_settable(L, -3);
  lua_pop(L, 1);
}

void luaA_struct_member_type(lua_State* L, luaA_Type type, const char* member, luaA_Type mtype, size_t offset) {

  lua_getfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "structs");
  lua_pushinteger(L, type);
  lua_gettable(L, -2);
  
  if (!lua_isnil(L, -1)) {
    
    lua_newtable(L);
    
    lua_pushinteger(L, mtype);  lua_setfield(L, -2, "type");
    lua_pushinteger(L, offset); lua_setfield(L, -2, "offset");
    lua_pushstring(L, member);  lua_setfield(L, -2, "name");
    
    lua_setfield(L, -2, member);
    
    lua_getfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "structs_offset");
    lua_pushinteger(L, type);
    lua_gettable(L, -2);

    lua_pushinteger(L, offset);
    lua_getfield(L, -4, member);
    lua_settable(L, -3);
    lua_pop(L, 4);
    return;
  }
  
  lua_pop(L, 2);
  lua_pushfstring(L, "luaA_struct_member: Struct '%s' not registered!", luaA_typename(L, type));
  lua_error(L);
}

bool luaA_struct_registered_type(lua_State* L, luaA_Type type) {

  lua_getfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "structs");
  lua_pushinteger(L, type);
  lua_gettable(L, -2);
  
  bool reg = !lua_isnil(L, -1);
  lua_pop(L, 2);
  
  return reg;
}

int luaA_struct_push_type(lua_State* L, luaA_Type type, const void* c_in) {

  lua_getfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "structs");
  lua_pushinteger(L, type);
  lua_gettable(L, -2);

  if (!lua_isnil(L, -1)) {
    lua_remove(L, -2);    
    lua_newtable(L);

    lua_pushnil(L);
    while (lua_next(L, -3)) {
      
      if (lua_type(L, -2) == LUA_TSTRING) {
        lua_getfield(L, -1, "name");
        const char* name = lua_tostring(L, -1);
        lua_pop(L, 1);
        int num = luaA_struct_push_member_name_type(L, type, name, c_in);
        if (num > 1) {
          lua_pop(L, 5);
          lua_pushfstring(L, "luaA_struct_push: Conversion pushed %d values to stack,"
                             " don't know how to include in struct!", num);
          lua_error(L);
        }
        lua_remove(L, -2);
        lua_pushvalue(L, -2);
        lua_insert(L, -2);
        lua_settable(L, -4);
      } else {
        lua_pop(L, 1);
      }

    }
    
    lua_remove(L, -2);
    return 1;
  }
  
  lua_pop(L, 2);
  lua_pushfstring(L, "lua_struct_push: Struct '%s' not registered!", luaA_typename(L, type));
  lua_error(L);
  return 0;
}

void luaA_struct_to_type(lua_State* L, luaA_Type type, void* c_out, int index) {
  
  lua_pushnil(L);
  while (lua_next(L, index-1)) {
    
    if (lua_type(L, -2) == LUA_TSTRING) {
      luaA_struct_to_member_name_type(L, type, lua_tostring(L, -2), c_out, -1);
    }
    
    lua_pop(L, 1);
  }

}

const char* luaA_struct_next_member_name_type(lua_State* L, luaA_Type type, const char* member) {

  lua_getfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "structs");
  lua_pushinteger(L, type);
  lua_gettable(L, -2);
  
  if(!lua_isnil(L,-1)) {

    if(!member) {
      lua_pushnil(L);
    } else {
      lua_pushstring(L,member);
    }
    if(!lua_next(L,-2)) {
      lua_pop(L,2);
      return LUAA_INVALID_MEMBER_NAME;
    }
    const char* result = lua_tostring(L,-2);
    lua_pop(L,4);
    return result;
  }
  
  lua_pop(L, 2);
  lua_pushfstring(L, "luaA_struct_next_member: Struct '%s' not registered!", luaA_typename(L, type));
  lua_error(L);
  return NULL;
}

/*
** Enums
*/

int luaA_enum_push_type(lua_State *L, luaA_Type type, const void* value) {
  
  lua_getfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "enums_values");
  lua_pushinteger(L, type);
  lua_gettable(L, -2);
  
  if (!lua_isnil(L, -1)) {
    
    lua_getfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "enums_sizes");
    lua_pushinteger(L, type);
    lua_gettable(L, -2);
    size_t size = lua_tointeger(L, -1);
    lua_pop(L, 2);
    
    lua_Integer lvalue =0;
    memcpy(&lvalue, value, size);
    
    lua_pushinteger(L, lvalue);
    lua_gettable(L, -2);
    
    if (!lua_isnil(L, -1)) {
      lua_getfield(L, -1, "name");
      lua_remove(L, -2);
      lua_remove(L, -2);
      lua_remove(L, -2);
      return 1;
    }
    
    lua_pop(L, 3);
    lua_pushfstring(L, "luaA_enum_push: Enum '%s' value %d not registered!", luaA_typename(L, type), lvalue);
    lua_error(L);
    return 0;
  }
  
  lua_pop(L, 2);
  lua_pushfstring(L, "luaA_enum_push: Enum '%s' not registered!", luaA_typename(L, type));
  lua_error(L);
  return 0;
  
}

void luaA_enum_to_type(lua_State* L, luaA_Type type, void* c_out, int index) {

  const char* name = lua_tostring(L, index);
  
  lua_getfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "enums");
  lua_pushinteger(L, type);
  lua_gettable(L, -2);
  
  if (!lua_isnil(L, -1)) {
    
    lua_getfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "enums_sizes");
    lua_pushinteger(L, type);
    lua_gettable(L, -2);
    size_t size = lua_tointeger(L, -1);
    lua_pop(L, 2);
    
    lua_pushstring(L, name);
    lua_gettable(L, -2);
    
    if (!lua_isnil(L, -1)) {
      lua_getfield(L, -1, "value");
      lua_Integer value = lua_tointeger(L, -1);
      lua_pop(L, 4);
      memcpy(c_out, &value, size);
      return;
    }
    
    lua_pop(L, 3);
    lua_pushfstring(L, "luaA_enum_to: Enum '%s' field '%s' not registered!", luaA_typename(L, type), name);
    lua_error(L);
    return;
  }
  
  lua_pop(L, 3);
  lua_pushfstring(L, "luaA_enum_to: Enum '%s' not registered!", luaA_typename(L, type));
  lua_error(L);
  return;
}

bool luaA_enum_has_value_type(lua_State* L, luaA_Type type, const void* value) {

  lua_getfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "enums_values");
  lua_pushinteger(L, type);
  lua_gettable(L, -2);
  
  if (!lua_isnil(L, -1)) {
    
    lua_getfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "enums_sizes");
    lua_pushinteger(L, type);
    lua_gettable(L, -2);
    size_t size = lua_tointeger(L, -1);
    lua_pop(L, 2);
    
    lua_Integer lvalue = 0;
    memcpy(&lvalue, value, size);
    
    lua_pushinteger(L, lvalue);
    lua_gettable(L, -2);
    
    if (lua_isnil(L, -1)) {
      lua_pop(L, 3);
      return false;
    } else {
      lua_pop(L, 3);
      return true;
    }
    
  }
  
  lua_pop(L, 2);
  lua_pushfstring(L, "luaA_enum_has_value: Enum '%s' not registered!", luaA_typename(L, type));
  lua_error(L);
  return false;
  
  return false;
}

bool luaA_enum_has_name_type(lua_State* L, luaA_Type type, const char* name) {
  lua_getfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "enums");
  lua_pushinteger(L, type);
  lua_gettable(L, -2);
  
  if (!lua_isnil(L, -1)) {
    
    lua_getfield(L, -1, name);
    
    if (lua_isnil(L, -1)) {
      lua_pop(L, 3);
      return false;
    } else {
      lua_pop(L, 3);
      return true;
    }
    
  }
  
  lua_pop(L, 2);
  lua_pushfstring(L, "luaA_enum_has_name: Enum '%s' not registered!", luaA_typename(L, type));
  lua_error(L);
  return false;
}

void luaA_enum_type(lua_State *L, luaA_Type type, size_t size) {
  lua_getfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "enums");
  lua_pushinteger(L, type);
  lua_newtable(L);
  lua_settable(L, -3);
  lua_pop(L, 1);
  
  lua_getfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "enums_values");
  lua_pushinteger(L, type);
  lua_newtable(L);
  lua_settable(L, -3);
  lua_pop(L, 1);
  
  lua_getfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "enums_sizes");
  lua_pushinteger(L, type);
  lua_pushinteger(L, size);
  lua_settable(L, -3);
  lua_pop(L, 1);
}

void luaA_enum_value_type(lua_State *L, luaA_Type type, const void* value, const char* name) {
  
  lua_getfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "enums");
  lua_pushinteger(L, type);
  lua_gettable(L, -2);
  
  if (!lua_isnil(L, -1)) {
  
    lua_getfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "enums_sizes");
    lua_pushinteger(L, type);
    lua_gettable(L, -2);
    size_t size = lua_tointeger(L, -1);
    lua_pop(L, 2);
    
    lua_newtable(L);
    
    lua_Integer lvalue=0;
    memcpy(&lvalue, value, size);
    
    lua_pushinteger(L, lvalue);
    lua_setfield(L, -2, "value");
    
    lua_pushstring(L, name);
    lua_setfield(L, -2, "name");
    
    lua_setfield(L, -2, name);

    lua_getfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "enums_values");
    lua_pushinteger(L, type);
    lua_gettable(L, -2);
    lua_pushinteger(L, lvalue);
    lua_getfield(L, -4, name);
    lua_settable(L, -3);
    
    lua_pop(L, 4);
    return;
    
  }
    
  lua_pop(L, 2);
  lua_pushfstring(L, "luaA_enum_value: Enum '%s' not registered!", luaA_typename(L, type));
  lua_error(L);
}

bool luaA_enum_registered_type(lua_State *L, luaA_Type type){
  lua_getfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "enums");
  lua_pushinteger(L, type);
  lua_gettable(L, -2);
  bool reg = !lua_isnil(L, -1);
  lua_pop(L, 2);
  return reg;
}

const char* luaA_enum_next_value_name_type(lua_State* L, luaA_Type type, const char* member) {

  lua_getfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "enums");
  lua_pushinteger(L, type);
  lua_gettable(L, -2);
  
  if(!lua_isnil(L,-1)) {

    if(!member) {
      lua_pushnil(L);
    } else {
      lua_pushstring(L,member);
    }
    if(!lua_next(L,-2)) {
      lua_pop(L,2);
      return LUAA_INVALID_MEMBER_NAME;
    }
    const char* result = lua_tostring(L,-2);
    lua_pop(L,4);
    return result;
  }
  
  lua_pop(L, 2);
  lua_pushfstring(L, "luaA_enum_next_enum_name_type: Enum '%s' not registered!", luaA_typename(L, type));
  lua_error(L);
  return NULL;
}

/*
** Functions
*/

static int luaA_call_entry(lua_State* L) {
  
  /* Get return size */
  
  lua_getfield(L, -1, "ret_type");
  luaA_Type ret_type = lua_tointeger(L, -1);
  lua_pop(L, 1);
  
  size_t ret_size = luaA_typesize(L, ret_type);
  
  /* Get total arguments sizes */
  
  lua_getfield(L, -1, "arg_types");
  
  size_t arg_size = 0;
  size_t arg_num  = lua_rawlen(L, -1);
  for (int i = 0; i < arg_num; i++) {
    lua_pushinteger(L, i+1);
    lua_gettable(L, -2);
    luaA_Type arg_type = lua_tointeger(L, -1);
    lua_pop(L, 1);
    arg_size += luaA_typesize(L, arg_type);
  }
  
  lua_pop(L, 1);
  
  /* Test to see if using heap */
  
  lua_getfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "call_ret_stk");
  void* ret_stack = lua_touserdata(L, -1);
  lua_pop(L, 1);
  
  lua_getfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "call_arg_stk");
  void* arg_stack = lua_touserdata(L, -1);
  lua_pop(L, 1);
  
  lua_getfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "call_ret_ptr");
  lua_Integer ret_ptr = lua_tointeger(L, -1);
  lua_pop(L, 1);
  
  lua_getfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "call_arg_ptr");
  lua_Integer arg_ptr = lua_tointeger(L, -1);
  lua_pop(L, 1);

  void* ret_data = ret_stack + ret_ptr;
  void* arg_data = arg_stack + arg_ptr;
  
  /* If fixed allocation exhausted use heap instead */
  
  bool ret_heap = false;
  bool arg_heap = false;
  
  if (ret_ptr + ret_size > LUAA_RETURN_STACK_SIZE) {
    ret_heap = true;
    ret_data = malloc(ret_size);
    if (ret_data == NULL) {
      lua_pushfstring(L, "luaA_call: Out of memory!");
      lua_error(L);
      return 0;
    }
  }
  
  if (arg_ptr + arg_size > LUAA_ARGUMENT_STACK_SIZE) {
    arg_heap = true;
    arg_data = malloc(arg_size);
    if (arg_data == NULL) {
      if (ret_heap) {
        free(ret_data);
      }
      lua_pushfstring(L, "luaA_call: Out of memory!");
      lua_error(L);
      return 0;
    }
  }
  
  /* If not using heap update stack pointers */
  
  if (!ret_heap) {
    lua_pushinteger(L, ret_ptr + ret_size);
    lua_setfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "call_ret_ptr");
  }
  
  if (!arg_heap) {
    lua_pushinteger(L, arg_ptr + arg_size);
    lua_setfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "call_arg_ptr");
  }
  
  /* Pop args and place in memory */
  
  lua_getfield(L, -1, "arg_types");

  void* arg_pos = arg_data;
  for (int i = 0; i < arg_num; i++) {
    lua_pushinteger(L, i+1);
    lua_gettable(L, -2);
    luaA_Type arg_type = lua_tointeger(L, -1);
    lua_pop(L, 1);
    luaA_to_type(L, arg_type, arg_pos, -arg_num+i-2);
    arg_pos += luaA_typesize(L, arg_type);
  }
  
  lua_pop(L, 1);
  
  /* Pop arguments from stack */
  
  for (int i = 0; i < arg_num; i++) {
    lua_remove(L, -2);  
  }
  
  /* Get Function Pointer and Call */
  
  lua_getfield(L, -1, "auto_func");
  luaA_Func auto_func = lua_touserdata(L, -1);
  lua_pop(L, 2);
  
  auto_func(ret_data, arg_data);
  
  int count = luaA_push_type(L, ret_type, ret_data);
  
  /* Either free heap data or reduce stack pointers */
  
  if (!ret_heap) {
    lua_pushinteger(L, ret_ptr);
    lua_setfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "call_ret_ptr");
  } else {
    free(ret_data);
  }
  
  if (!arg_heap) {
    lua_pushinteger(L, arg_ptr);
    lua_setfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "argument_ptr");
  } else {
    free(arg_data); 
  }
  
  return count;
}

int luaA_call(lua_State* L, void* func_ptr) {
  lua_getfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "functions");
  lua_pushlightuserdata(L, func_ptr);
  lua_gettable(L, -2);
  lua_remove(L, -2);
  
  if (!lua_isnil(L, -1)) { return luaA_call_entry(L); }
  
  lua_pop(L, 1);
  lua_pushfstring(L, "luaA_call: Function with address '%p' is not registered!", func_ptr);
  lua_error(L);
  return 0;
}

int luaA_call_name(lua_State* L, const char* func_name) {
  lua_getfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "functions");
  lua_pushstring(L, func_name);
  lua_gettable(L, -2);
  lua_remove(L, -2);
  
  if (!lua_isnil(L, -1)) { return luaA_call_entry(L); }
  
  lua_pop(L, 1);
  lua_pushfstring(L, "luaA_call_name: Function '%s' is not registered!", func_name);
  lua_error(L);
  return 0;
}

void luaA_function_register_type(lua_State* L, void* src_func, luaA_Func auto_func, const char* name, luaA_Type ret_t, int num_args, ...) {
  
  lua_getfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "functions");
  lua_pushstring(L, name);

  lua_newtable(L);
  
  lua_pushlightuserdata(L, src_func);  lua_setfield(L, -2, "src_func");
  lua_pushlightuserdata(L, auto_func); lua_setfield(L, -2, "auto_func");
  
  lua_pushinteger(L, ret_t);
  lua_setfield(L, -2, "ret_type");
  
  lua_pushstring(L, "arg_types");
  lua_newtable(L);
  
  va_list va;
  va_start(va, num_args);
  for (int i = 0; i < num_args; i++) {
    lua_pushinteger(L, i+1);
    lua_pushinteger(L, va_arg(va, luaA_Type));
    lua_settable(L, -3);
  }
  va_end(va);
  
  lua_settable(L, -3);
  lua_settable(L, -3);
  lua_pop(L, 1);
  
  lua_getfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "functions");
  lua_pushlightuserdata(L, src_func);
  
  lua_getfield(L, LUA_REGISTRYINDEX, LUAA_REGISTRYPREFIX "functions");
  lua_getfield(L, -1, name);
  lua_remove(L, -2);
  
  lua_settable(L, -3);
  lua_pop(L, 1);
  
}
