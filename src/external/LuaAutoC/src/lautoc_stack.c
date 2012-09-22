#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lautoc.h"

static luaA_Hashtable* push_table;
static luaA_Hashtable* to_table;

void luaA_stack_open(void) {
  
  push_table = luaA_hashtable_new(256);
  to_table = luaA_hashtable_new(256);
  
  luaA_conversion(char, luaA_push_char, luaA_to_char);
  luaA_conversion(signed char, luaA_push_signed_char, luaA_to_signed_char);
  luaA_conversion(unsigned char, luaA_push_unsigned_char, luaA_to_unsigned_char);
  luaA_conversion(short, luaA_push_short, luaA_to_short);
  luaA_conversion(unsigned short, luaA_push_unsigned_short, luaA_to_unsigned_short);
  luaA_conversion(int, luaA_push_int, luaA_to_int);
  luaA_conversion(unsigned int, luaA_push_unsigned_int, luaA_to_unsigned_int);
  luaA_conversion(long, luaA_push_long, luaA_to_long);
  luaA_conversion(unsigned long, luaA_push_unsigned_long, luaA_to_unsigned_long);
  luaA_conversion(long long, luaA_push_long_long, luaA_to_long_long);
  luaA_conversion(unsigned long long, luaA_push_unsigned_long_long, luaA_to_unsigned_long_long);
  luaA_conversion(float, luaA_push_float, luaA_to_float);
  luaA_conversion(double, luaA_push_double, luaA_to_double);
  luaA_conversion(long double, luaA_push_long_double, luaA_to_long_double);
  luaA_conversion(int32_t, luaA_push_int, luaA_to_int);
  
  luaA_conversion_push(const char, luaA_push_char);
  luaA_conversion_push(const signed char, luaA_push_signed_char);
  luaA_conversion_push(const unsigned char, luaA_push_unsigned_char);
  luaA_conversion_push(const short, luaA_push_short);
  luaA_conversion_push(const unsigned short, luaA_push_unsigned_short);
  luaA_conversion_push(const int, luaA_push_int);
  luaA_conversion_push(const unsigned int, luaA_push_unsigned_int);
  luaA_conversion_push(const long, luaA_push_long);
  luaA_conversion_push(const unsigned long, luaA_push_unsigned_long);
  luaA_conversion_push(const long long, luaA_push_long_long);
  luaA_conversion_push(const unsigned long long, luaA_push_unsigned_long_long);
  luaA_conversion_push(const float, luaA_push_float);
  luaA_conversion_push(const double, luaA_push_double);
  luaA_conversion_push(const long double, luaA_push_long_double);
  luaA_conversion(const int32_t, luaA_push_int, luaA_to_int);
  
  luaA_conversion(char*, luaA_push_char_ptr, luaA_to_char_ptr);
  luaA_conversion(const char*, luaA_push_const_char_ptr, luaA_to_const_char_ptr);
  
  luaA_conversion_push(void, luaA_push_void);
  
}

void luaA_stack_close(void) {
  
  luaA_hashtable_delete(push_table);
  luaA_hashtable_delete(to_table);
  
}

int luaA_push_typeid(lua_State* L, luaA_Type type_id,const void* c_in) {
  
  luaA_Pushfunc push_func = luaA_hashtable_get(push_table, luaA_type_name(type_id));
  if (push_func != NULL) {
    return push_func(L, c_in);
  }
  
  if (luaA_struct_registered_typeid(L, type_id)) {
    return luaA_struct_push_typeid(L, type_id, c_in);
  }
  
  lua_pushfstring(L, "luaA_push: conversion to lua object from type '%s' not registered!", luaA_type_name(type_id));
  lua_error(L);
  return 0;
}

void luaA_to_typeid(lua_State* L, luaA_Type type_id, void* c_out, int index) {
  
  luaA_Tofunc to_func = luaA_hashtable_get(to_table, luaA_type_name(type_id));
  if (to_func != NULL) {
    return to_func(L, c_out, index);
  }
  
  if (luaA_struct_registered_typeid(L, type_id)) {
    return luaA_struct_to_typeid(L, type_id, c_out, index);
  }
  
  lua_pushfstring(L, "luaA_to: conversion from lua object to type '%s' not registered!", luaA_type_name(type_id));
  lua_error(L);  
}

void luaA_conversion_typeid(luaA_Type type_id, luaA_Pushfunc push_func, luaA_Tofunc to_func) {
  
  luaA_hashtable_set(push_table, luaA_type_name(type_id), push_func);
  luaA_hashtable_set(to_table, luaA_type_name(type_id), to_func);
  
}

void luaA_conversion_push_typeid(luaA_Type type_id, luaA_Pushfunc func) {
  luaA_hashtable_set(push_table, luaA_type_name(type_id), func); 
}

void luaA_conversion_to_typeid(luaA_Type type_id, luaA_Tofunc func) {
  luaA_hashtable_set(to_table, luaA_type_name(type_id), func);
}

int luaA_push_char(lua_State* L,const void* c_in) {
  lua_pushinteger(L, *(char*)c_in);
  return 1;
}

void luaA_to_char(lua_State* L, void* c_out, int index) {
  *(char*)c_out = lua_tointeger(L, index);
}

int luaA_push_signed_char(lua_State* L,const void* c_in) {
  lua_pushinteger(L, *(signed char*)c_in);
  return 1;
}

void luaA_to_signed_char(lua_State* L, void* c_out, int index) {
  *(signed char*)c_out = lua_tointeger(L, index);
}

int luaA_push_unsigned_char(lua_State* L,const void* c_in) {
  lua_pushinteger(L, *(unsigned char*)c_in);
  return 1;
}

void luaA_to_unsigned_char(lua_State* L, void* c_out, int index) {
  *(unsigned char*)c_out = lua_tointeger(L, index);
}

int luaA_push_short(lua_State* L,const void* c_in) {
  lua_pushinteger(L, *(short*)c_in);
  return 1;
}

void luaA_to_short(lua_State* L, void* c_out, int index) {
  *(short*)c_out = lua_tointeger(L, index);
}

int luaA_push_unsigned_short(lua_State* L,const void* c_in) {
  lua_pushinteger(L, *(unsigned short*)c_in);
  return 1;
}

void luaA_to_unsigned_short(lua_State* L, void* c_out, int index) {
  *(unsigned short*)c_out = lua_tointeger(L, index);
}

int luaA_push_int(lua_State* L,const void* c_in) {
  lua_pushinteger(L, *(int*)c_in);
  return 1;
}

void luaA_to_int(lua_State* L, void* c_out, int index) {
  *(int*)c_out = lua_tointeger(L, index);
}

int luaA_push_unsigned_int(lua_State* L,const void* c_in) {
  lua_pushinteger(L, *(unsigned int*)c_in);
  return 1;
}

void luaA_to_unsigned_int(lua_State* L, void* c_out, int index) {
  *(unsigned int*)c_out = lua_tointeger(L, index);
}

int luaA_push_long(lua_State* L,const void* c_in) {
  lua_pushinteger(L, *(long*)c_in);
  return 1;
}

void luaA_to_long(lua_State* L, void* c_out, int index) {
  *(long*)c_out = lua_tointeger(L, index);
}

int luaA_push_unsigned_long(lua_State* L,const void* c_in) {
  lua_pushinteger(L, *(unsigned long*)c_in);
  return 1;
}

void luaA_to_unsigned_long(lua_State* L, void* c_out, int index) {
  *(unsigned long*)c_out = lua_tointeger(L, index);
}

int luaA_push_long_long(lua_State* L,const void* c_in) {
  lua_pushinteger(L, *(long long*)c_in);
  return 1;
}

void luaA_to_long_long(lua_State* L, void* c_out, int index) {
  *(long long*)c_out = lua_tointeger(L, index);
}

int luaA_push_unsigned_long_long(lua_State* L,const void* c_in) {
  lua_pushinteger(L, *(unsigned long long*)c_in);
  return 1;
}

void luaA_to_unsigned_long_long(lua_State* L, void* c_out, int index) {
  *(unsigned long long*)c_out = lua_tointeger(L, index);
}

int luaA_push_float(lua_State* L,const void* c_in) {
  lua_pushnumber(L, *(float*)c_in);
  return 1;
}

void luaA_to_float(lua_State* L, void* c_out, int index) {
  *(float*)c_out = lua_tonumber(L, index);
}

int luaA_push_double(lua_State* L,const void* c_in) {
  lua_pushnumber(L, *(double*)c_in);
  return 1;
}

void luaA_to_double(lua_State* L, void* c_out, int index) {
  *(double*)c_out = lua_tonumber(L, index);
}

int luaA_push_long_double(lua_State* L,const void* c_in) {
  lua_pushnumber(L, *(long double*)c_in);
  return 1;
}

void luaA_to_long_double(lua_State* L, void* c_out, int index) {
  *(long double*)c_out = lua_tonumber(L, index);
}

int luaA_push_char_ptr(lua_State* L,const void* c_in) {
  lua_pushstring(L, *(char**)c_in);
  return 1;
}

void luaA_to_char_ptr(lua_State* L, void* c_out, int index) {
  *(char**)c_out = (char*)lua_tostring(L, index);
}

int luaA_push_const_char_ptr(lua_State* L,const void* c_in) {
  lua_pushstring(L, *(const char**)c_in);
  return 1;
}

void luaA_to_const_char_ptr(lua_State* L, void* c_out, int index) {
  *(const char**)c_out = lua_tostring(L, index);
}

int luaA_push_void(lua_State* L,const void* c_in) {
  lua_pushnil(L);
  return 1;
}

bool luaA_type_has_push_func(luaA_Type id) {
  if (id == -1) return false;
  if(luaA_hashtable_get(push_table, luaA_type_name(id))) return true;
  return false;
}

bool luaA_type_has_to_func(luaA_Type id) {
  if (id == -1) return false;
  if(luaA_hashtable_get(to_table, luaA_type_name(id))) return true;
  return false;
}
