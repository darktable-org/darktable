/*
** LuaAutoC - Automagically use C Functions and Structs with the Lua API
** https://github.com/orangeduck/LuaAutoC
** Daniel Holden - contact@theorangeduck.com
** Licensed under BSD
*/

#ifndef lautoc_h
#define lautoc_h

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"

#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

/*
** Open / Close
*/

#define LUAA_REGISTRYPREFIX "lautoc_"

void luaA_open(lua_State* L);
void luaA_close(lua_State* L);

/*
** Types
*/

#define luaA_type(L, type) luaA_type_add(L, #type, sizeof(type))

enum {
  LUAA_INVALID_TYPE = -1
};

typedef lua_Integer luaA_Type;
typedef int (*luaA_Pushfunc)(lua_State*, luaA_Type, const void*);
typedef void (*luaA_Tofunc)(lua_State*, luaA_Type, void*, int);

luaA_Type luaA_type_add(lua_State* L, const char* type, size_t size);
luaA_Type luaA_type_find(lua_State* L, const char* type);

const char* luaA_typename(lua_State* L, luaA_Type id);
size_t luaA_typesize(lua_State* L, luaA_Type id);

/*
** Stack
*/

#define luaA_push(L, type, c_in) luaA_push_type(L, luaA_type(L, type), c_in)
#define luaA_to(L, type, c_out, index) luaA_to_type(L, luaA_type(L, type), c_out, index)

#define luaA_conversion(L, type, push_func, to_func) luaA_conversion_type(L, luaA_type(L, type), push_func, to_func);
#define luaA_conversion_push(L, type, func) luaA_conversion_push_type(L, luaA_type(L, type), func)
#define luaA_conversion_to(L, type, func) luaA_conversion_to_type(L, luaA_type(L, type), func)

#define luaA_conversion_registered(L, type) luaA_conversion_registered_type(L, luaA_type(L, type));
#define luaA_conversion_push_registered(L, type) luaA_conversion_push_registered_typ(L, luaA_type(L, type));
#define luaA_conversion_to_registered(L, type) luaA_conversion_to_registered_type(L, luaA_type(L, type));

int luaA_push_type(lua_State* L, luaA_Type type, const void* c_in);
void luaA_to_type(lua_State* L, luaA_Type type, void* c_out, int index);

void luaA_conversion_type(lua_State* L, luaA_Type type_id, luaA_Pushfunc push_func, luaA_Tofunc to_func);
void luaA_conversion_push_type(lua_State* L, luaA_Type type_id, luaA_Pushfunc func);
void luaA_conversion_to_type(lua_State* L, luaA_Type type_id, luaA_Tofunc func);

bool luaA_conversion_registered_type(lua_State* L, luaA_Type type);
bool luaA_conversion_push_registered_type(lua_State* L, luaA_Type type);
bool luaA_conversion_to_registered_type(lua_State* L, luaA_Type type);

int luaA_push_char(lua_State* L, luaA_Type, const void* c_in);
int luaA_push_signed_char(lua_State* L,luaA_Type, const void* c_in);
int luaA_push_unsigned_char(lua_State* L, luaA_Type, const void* c_in);
int luaA_push_short(lua_State* L, luaA_Type, const void* c_in);
int luaA_push_unsigned_short(lua_State* L, luaA_Type, const void* c_in);
int luaA_push_int(lua_State* L, luaA_Type, const void* c_in);
int luaA_push_unsigned_int(lua_State* L, luaA_Type, const void* c_in);
int luaA_push_long(lua_State* L, luaA_Type, const void* c_in);
int luaA_push_unsigned_long(lua_State* L, luaA_Type, const void* c_in);
int luaA_push_long_long(lua_State* L, luaA_Type, const void* c_in);
int luaA_push_unsigned_long_long(lua_State* L, luaA_Type, const void* c_in);
int luaA_push_float(lua_State* L, luaA_Type, const void* c_in);
int luaA_push_double(lua_State* L, luaA_Type, const void* c_in);
int luaA_push_long_double(lua_State* L, luaA_Type, const void* c_in);
int luaA_push_char_ptr(lua_State* L, luaA_Type, const void* c_in);
int luaA_push_const_char_ptr(lua_State* L, luaA_Type, const void* c_in);
int luaA_push_void_ptr(lua_State* L, luaA_Type, const void* c_in);
int luaA_push_void(lua_State* L, luaA_Type, const void* c_in);

void luaA_to_char(lua_State* L, luaA_Type, void* c_out, int index);
void luaA_to_signed_char(lua_State* L, luaA_Type,  void* c_out, int index);
void luaA_to_unsigned_char(lua_State* L, luaA_Type,  void* c_out, int index);
void luaA_to_short(lua_State* L, luaA_Type,  void* c_out, int index);
void luaA_to_unsigned_short(lua_State* L, luaA_Type,  void* c_out, int index);
void luaA_to_int(lua_State* L, luaA_Type,  void* c_out, int index);
void luaA_to_unsigned_int(lua_State* L, luaA_Type,  void* c_out, int index);
void luaA_to_long(lua_State* L, luaA_Type,  void* c_out, int index);
void luaA_to_unsigned_long(lua_State* L, luaA_Type,  void* c_out, int index);
void luaA_to_long_long(lua_State* L, luaA_Type,  void* c_out, int index);
void luaA_to_unsigned_long_long(lua_State* L, luaA_Type,  void* c_out, int index);
void luaA_to_float(lua_State* L, luaA_Type,  void* c_out, int index);
void luaA_to_double(lua_State* L, luaA_Type,  void* c_out, int index);
void luaA_to_long_double(lua_State* L, luaA_Type,  void* c_out, int index);
void luaA_to_char_ptr(lua_State* L, luaA_Type,  void* c_out, int index);
void luaA_to_const_char_ptr(lua_State* L, luaA_Type,  void* c_out, int index);
void luaA_to_void_ptr(lua_State* L, luaA_Type,  void* c_out, int index);

/*
** Structs
*/
#define LUAA_INVALID_MEMBER_NAME NULL

#define luaA_struct(L, type) luaA_struct_type(L, luaA_type(L, type))
#define luaA_struct_member(L, type, member, member_type) luaA_struct_member_type(L, luaA_type(L, type), #member, luaA_type(L, member_type), offsetof(type, member))

#define luaA_struct_push(L, type, c_in) luaA_struct_push_type(L, luaA_type(L, type), c_in)
#define luaA_struct_push_member(L, type, member, c_in) luaA_struct_push_member_offset_type(L, luaA_type(L, type), offsetof(type, member), c_in)
#define luaA_struct_push_member_name(L, type, member, c_in) luaA_struct_push_member_name_type(L, luaA_type(L, type), member, c_in)

#define luaA_struct_to(L, type, c_out, index) luaA_struct_to_type(L, luaA_type(L, type), c_out, index)
#define luaA_struct_to_member(L, type, member, c_out, index) luaA_struct_to_member_offset_type(L, luaA_type(L, type), offsetof(type, member), c_out, index)
#define luaA_struct_to_member_name(L, type, member, c_out, index) luaA_struct_to_member_name_type(L, luaA_type(L, type), member, c_out, index)

#define luaA_struct_has_member(L, type, member) luaA_struct_has_member_offset_type(L, luaA_type(L, type), offsetof(type, member))
#define luaA_struct_has_member_name(L, type, member) luaA_struct_has_member_name_type(L, luaA_type(L, type), member)

#define luaA_struct_typeof_member(L, type, member) luaA_struct_typeof_member_offset_type(L, luaA_type(L, type), offsetof(type, member))
#define luaA_struct_typeof_member_name(L, type, member) luaA_struct_typeof_member_name_type(L, luaA_type(L, type), member)

#define luaA_struct_registered(L, type) luaA_struct_registered_type(L, luaA_type(L, type))
#define luaA_struct_next_member_name(L, type, member) luaA_struct_next_member_name_type(L, luaA_type(L,type), member)

void luaA_struct_type(lua_State* L, luaA_Type type);
void luaA_struct_member_type(lua_State* L, luaA_Type type, const char* member, luaA_Type member_type, size_t offset);

int luaA_struct_push_type(lua_State* L, luaA_Type type, const void* c_in);
int luaA_struct_push_member_offset_type(lua_State* L, luaA_Type type, size_t offset, const void* c_in);
int luaA_struct_push_member_name_type(lua_State* L, luaA_Type type, const char* member, const void* c_in);

void luaA_struct_to_type(lua_State* L, luaA_Type type, void* c_out, int index);
void luaA_struct_to_member_offset_type(lua_State* L, luaA_Type type, size_t offset, void* c_out, int index);
void luaA_struct_to_member_name_type(lua_State* L, luaA_Type type, const char* member, void* c_out, int index);

bool luaA_struct_has_member_offset_type(lua_State* L, luaA_Type type, size_t offset);
bool luaA_struct_has_member_name_type(lua_State* L, luaA_Type type, const char* member);

luaA_Type luaA_struct_typeof_member_offset_type(lua_State* L, luaA_Type type, size_t offset);
luaA_Type luaA_struct_typeof_member_name_type(lua_State* L, luaA_Type type, const char* member);

bool luaA_struct_registered_type(lua_State* L, luaA_Type type);

const char* luaA_struct_next_member_name_type(lua_State* L, luaA_Type type, const char* member);

/*
** Enums
*/

#define luaA_enum(L, type) luaA_enum_type(L, luaA_type(L, type), sizeof(type))
#define luaA_enum_value(L, type, value) luaA_enum_value_type(L, luaA_type(L, type), (const type[]){value}, #value);
#define luaA_enum_value_name(L, type, value, name) luaA_enum_value_type(L, luaA_type(L, type), (const type[]){value}, name);

#define luaA_enum_push(L, type, c_in) luaA_enum_push_type(L, luaA_type(L, type), c_in)
#define luaA_enum_to(L, type, c_out, index) luaA_enum_to_type(L, luaA_type(L, type), c_out, index)

#define luaA_enum_has_value(L, type, value) luaA_enum_has_value_type(L, luaA_type(L, type), (const type[]){value})
#define luaA_enum_has_name(L, type, name) luaA_enum_has_name_type(L, luaA_type(L, type), name)

#define luaA_enum_registered(L, type) luaA_enum_registered_type(L, luaA_type(L, type))
#define luaA_enum_next_value_name(L, type, member) luaA_enum_next_value_name_type(L, luaA_type(L,type), member)

void luaA_enum_type(lua_State* L, luaA_Type type, size_t size);
void luaA_enum_value_type(lua_State *L, luaA_Type type, const void* value, const char* name);

int luaA_enum_push_type(lua_State *L, luaA_Type type, const void* c_in);
void luaA_enum_to_type(lua_State* L, luaA_Type type, void *c_out, int index);

bool luaA_enum_has_value_type(lua_State* L, luaA_Type type, const void* value);
bool luaA_enum_has_name_type(lua_State* L, luaA_Type type, const char* name);

bool luaA_enum_registered_type(lua_State *L, luaA_Type type);
const char* luaA_enum_next_value_name_type(lua_State* L, luaA_Type type, const char* member);

/*
** Functions
*/

#include "lautocall.h"

#define luaA_function(L, func, ret_t, ...) luaA_function_declare(func, ret_t, ##__VA_ARGS__); luaA_function_register(L, func, ret_t, ##__VA_ARGS__)
#define luaA_function_declare(func, ret_t, ...) LUAA_DECLARE(func, ret_t, LUAA_COUNT(__VA_ARGS__), LUAA_SUFFIX(ret_t), ##__VA_ARGS__)
#define luaA_function_register(L, func, ret_t, ...) LUAA_REGISTER(L, func, ret_t, LUAA_COUNT(__VA_ARGS__), ##__VA_ARGS__)

enum {
  LUAA_RETURN_STACK_SIZE   =  256,
  LUAA_ARGUMENT_STACK_SIZE = 2048
};

typedef void (*luaA_Func)(void*, void*);

int luaA_call(lua_State* L, void* func_ptr);
int luaA_call_name(lua_State* L, const char* func_name);

void luaA_function_register_type(lua_State* L, void* src_func, luaA_Func auto_func, const char* name, luaA_Type ret_tid, int num_args, ...);

#endif
