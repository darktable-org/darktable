/*
** LuaAutoC - Automatically Wrap C Structs and Functions at runtime for the Lua/C API
** https://github.com/orangeduck/LuaAutoC
** Daniel Holden - contact@daniel-holden.com
** Licensed under BSD
*/


#ifndef lautoc_h
#define lautoc_h

#include <stddef.h>
#include <stdbool.h>

#include "lua.h"

/*
** open-close functions
*/
void luaA_open(void);
void luaA_close(void);


/*
** type recording
*/
void luaA_type_open(void);
void luaA_type_close(void);

typedef int luaA_Type;
#define LUAA_INVALID_TYPE -1

typedef int (*luaA_Pushfunc)(lua_State*, luaA_Type, const void*);
typedef void (*luaA_Tofunc)(lua_State*, luaA_Type, void*, int);

#define luaA_type_id(type) luaA_type_add(#type, sizeof(type))

luaA_Type luaA_type_add(const char* type, size_t size);
luaA_Type luaA_type_find(const char* type);

const char* luaA_type_name(luaA_Type id);
size_t luaA_type_size(luaA_Type id);
bool luaA_type_has_push_func(luaA_Type id);
bool luaA_type_has_to_func(luaA_Type id);


/*
** stack functions
*/
void luaA_stack_open(void);
void luaA_stack_close(void);

#define luaA_push(L, type, c_in) luaA_push_typeid(L, luaA_type_id(type), c_in)
#define luaA_to(L, type, c_out, index) luaA_to_typeid(L, luaA_type_id(type), c_out, index)

int luaA_push_typeid(lua_State* L, luaA_Type type_id,const void* c_in);
void luaA_to_typeid(lua_State* L, luaA_Type type_id, void* c_out, int index);

#define luaA_conversion(type, push_func, to_func) luaA_conversion_typeid(luaA_type_id(type), push_func, to_func);
#define luaA_conversion_push(type, func) luaA_conversion_push_typeid(luaA_type_id(type), func)
#define luaA_conversion_to(type, func) luaA_conversion_to_typeid(luaA_type_id(type), func)

void luaA_conversion_typeid(luaA_Type type_id, luaA_Pushfunc push_func, luaA_Tofunc to_func);
void luaA_conversion_push_typeid(luaA_Type type_id, luaA_Pushfunc func);
void luaA_conversion_to_typeid(luaA_Type type_id, luaA_Tofunc func);


/* native type stack functions */
int luaA_push_char(lua_State* L,luaA_Type, const void* c_in);
void luaA_to_char(lua_State* L, luaA_Type, void* c_out, int index);
int luaA_push_signed_char(lua_State* L,luaA_Type, const void* c_in);
void luaA_to_signed_char(lua_State* L, luaA_Type,  void* c_out, int index);
int luaA_push_unsigned_char(lua_State* L, luaA_Type, const void* c_in);
void luaA_to_unsigned_char(lua_State* L, luaA_Type,  void* c_out, int index);
int luaA_push_short(lua_State* L, luaA_Type, const void* c_in);
void luaA_to_short(lua_State* L, luaA_Type,  void* c_out, int index);
int luaA_push_unsigned_short(lua_State* L, luaA_Type, const void* c_in);
void luaA_to_unsigned_short(lua_State* L, luaA_Type,  void* c_out, int index);
int luaA_push_int(lua_State* L, luaA_Type, const void* c_in);
void luaA_to_int(lua_State* L, luaA_Type,  void* c_out, int index);
int luaA_push_unsigned_int(lua_State* L, luaA_Type, const void* c_in);
void luaA_to_unsigned_int(lua_State* L, luaA_Type,  void* c_out, int index);
int luaA_push_long(lua_State* L, luaA_Type, const void* c_in);
void luaA_to_long(lua_State* L, luaA_Type,  void* c_out, int index);
int luaA_push_unsigned_long(lua_State* L, luaA_Type, const void* c_in);
void luaA_to_unsigned_long(lua_State* L, luaA_Type,  void* c_out, int index);
int luaA_push_long_long(lua_State* L, luaA_Type, const void* c_in);
void luaA_to_long_long(lua_State* L, luaA_Type,  void* c_out, int index);
int luaA_push_unsigned_long_long(lua_State* L, luaA_Type, const void* c_in);
void luaA_to_unsigned_long_long(lua_State* L, luaA_Type,  void* c_out, int index);
int luaA_push_float(lua_State* L, luaA_Type, const void* c_in);
void luaA_to_float(lua_State* L, luaA_Type,  void* c_out, int index);
int luaA_push_double(lua_State* L, luaA_Type, const void* c_in);
void luaA_to_double(lua_State* L, luaA_Type,  void* c_out, int index);
int luaA_push_long_double(lua_State* L, luaA_Type, const void* c_in);
void luaA_to_long_double(lua_State* L, luaA_Type,  void* c_out, int index);
int luaA_push_char_ptr(lua_State* L, luaA_Type, const void* c_in);
void luaA_to_char_ptr(lua_State* L, luaA_Type,  void* c_out, int index);
int luaA_push_const_char_ptr(lua_State* L, luaA_Type, const void* c_in);
void luaA_to_const_char_ptr(lua_State* L, luaA_Type,  void* c_out, int index);
int luaA_push_void(lua_State* L, luaA_Type, const void* c_in);


/*
** struct functions
*/
void luaA_struct_open(void);
void luaA_struct_close(void);

/* push and inspect struct members */
#define luaA_struct_push_member(L, type, cstruct, member) luaA_struct_push_member_offset_typeid(L, luaA_type_id(type), cstruct, offsetof(type, member))
#define luaA_struct_push_member_name(L, type, cstruct, member) luaA_struct_push_member_name_typeid(L, luaA_type_id(type), cstruct, member)
int luaA_struct_push_member_offset_typeid(lua_State* L, luaA_Type type,const void* cstruct, size_t offset);
int luaA_struct_push_member_name_typeid(lua_State* L, luaA_Type type,const void* cstruct, const char* member);

#define luaA_struct_to_member(L, type, cstruct, member, index) luaA_struct_to_member_offset_typeid(L, luaA_type_id(type), cstruct, offsetof(type, member), index)
#define luaA_struct_to_member_name(L, type, cstruct, member, index) luaA_struct_to_member_name_typeid(L, luaA_type_id(type), cstruct, member, index)
void luaA_struct_to_member_offset_typeid(lua_State* L, luaA_Type type, void* cstruct, size_t offset, int index);
void luaA_struct_to_member_name_typeid(lua_State* L, luaA_Type type, void* cstruct, const char* member, int index);

#define luaA_struct_has_member(L, type, member) luaA_struct_has_member_offset_typeid(L, luaA_type_id(type), offsetof(type, member))
#define luaA_struct_has_member_name(L, type, member) luaA_struct_has_member_name_typeid(L, luaA_type_id(type), member)
bool luaA_struct_has_member_offset_typeid(lua_State* L, luaA_Type type,  size_t offset);
bool luaA_struct_has_member_name_typeid(lua_State* L, luaA_Type type,  const char* member);

#define LUAA_INVALID_MEMBER_NAME NULL
#define luaA_struct_next_member_name(L, type, member) luaA_struct_next_member_name_typeid(L, luaA_type_id(type), member)
const char* luaA_struct_next_member_name_typeid(lua_State* L, luaA_Type type,  const char* member);

#define luaA_struct_typeof_member(L, type, member) luaA_struct_typeof_member_offset_typeid(L, luaA_type_id(type), offsetof(type, member))
#define luaA_struct_typeof_member_name(L, type, member) luaA_struct_typeof_member_name_typeid(L, luaA_type_id(type), member)
luaA_Type luaA_struct_typeof_member_offset_typeid(lua_State* L, luaA_Type type,  size_t offset);
luaA_Type luaA_struct_typeof_member_name_typeid(lua_State* L, luaA_Type type,  const char* member);

/* register structs */
#define luaA_struct(L, type) luaA_struct_typeid(L, luaA_type_id(type))
#define luaA_struct_member(L, type, member, member_type) luaA_struct_member_typeid(L, luaA_type_id(type), #member, luaA_type_id(member_type), offsetof(type, member))

#define luaA_struct_registered(L, type) luaA_struct_registered_typeid(L, luaA_type_id(type))

void luaA_struct_typeid(lua_State* L, luaA_Type type);
void luaA_struct_member_typeid(lua_State* L, luaA_Type type, const char* member, luaA_Type member_type, size_t offset);

bool luaA_struct_registered_typeid(lua_State* L, luaA_Type type);

/* push and inspect whole structs */
#define luaA_struct_push(L, type, c_in) luaA_struct_push_typeid(L, luaA_type_id(type), c_in)
#define luaA_struct_to(L, type, c_out, index) luaA_struct_to_typeid(L, luaA_type_id(type), pyobj, c_out, index)

int luaA_struct_push_typeid(lua_State* L, luaA_Type type,const void* c_in);
void luaA_struct_to_typeid(lua_State* L, luaA_Type type, void* c_out, int index);

/*
 ** enum function
 */
void luaA_enum_open(void);
void luaA_enum_close(void);
/* push and inspect enum members */
#define luaA_enum_push(L,type,value) luaA_enum_push_typeid(L,luaA_type_id(type),value)
int luaA_enum_push_typeid(lua_State *L, luaA_Type type, const void* value);

#define luaA_enum_to(L,type,ptr,index) luaA_enum_to_typeid(L,luaA_type_id(type),ptr,index)
void luaA_enum_to_typeid(lua_State* L, luaA_Type type, void *c_in,int index);

#define luaA_enum_has_value(L,type,value) LuaA_enum_has_value_typeid(L,luaA_type_id(type),value)
#define luaA_enum_has_name(L,type,value) LuaA_enum_has_name_typeid(L,luaA_type_id(type),value)
bool luaA_enum_has_value_typeid(lua_State* L, luaA_Type type, const void* value);
bool luaA_enum_has_name_typeid(lua_State* L, luaA_Type type, const char* name);

#define luaA_enum(L,type) luaA_enum_typeid(L,luaA_type_id(type),sizeof(type))
#define luaA_enum_value(L,type,value,case_sensitive) do{\
	type _var; \
	_var = value; \
	luaA_enum_value_typeid_name(L,luaA_type_id(type),&_var,#value,case_sensitive); \
}while(0)
#define luaA_enum_value_name(L,type,value,name,case_sensitive) do{ \
	type _var; \
	_var = value; \
	luaA_enum_value_typeid_name(L,luaA_type_id(type),&_var,name,case_sensitive); \
}while(0)

void luaA_enum_typeid(lua_State *L,luaA_Type type,size_t size);
void luaA_enum_value_typeid_name(lua_State *L, luaA_Type type, const void* value, const char*value_name,bool case_sensitive);

bool luaA_enum_registered_typeid(lua_State *L, luaA_Type type);



/*
** function calling and registration
*/
void luaA_call_open(void);
void luaA_call_close(void);

int luaA_call(lua_State* L, void* func_ptr);
int luaA_call_name(lua_State* L, const char* func_name);

#include "lautocfunc.h"

#define luaA_function(L, func, ret_t, num_args, ...) __VA_ARGS_APPLY__(luaA_function_args##num_args##_macro, L, func, ret_t, ##__VA_ARGS__ )
#define luaA_function_void(L, func, num_args, ...) __VA_ARGS_APPLY__(luaA_function_args##num_args##_void_macro, L, func, void, ##__VA_ARGS__ )

typedef void (*luaA_Func)(void*,void*);

void luaA_function_typeid(lua_State* L, void* src_func, luaA_Func auto_func, const char* name, luaA_Type ret_tid, int num_args, ...);


/*
** internal hashtable utility
*/
typedef struct luaA_Bucket {
  void* item;
  char* string;
  struct luaA_Bucket* next;
  struct luaA_Bucket* prev;
} luaA_Bucket;

typedef struct {
  luaA_Bucket** buckets;
  int size;
} luaA_Hashtable;

luaA_Hashtable* luaA_hashtable_new(int table_size);
void luaA_hashtable_delete(luaA_Hashtable* ht);

bool luaA_hashtable_contains(luaA_Hashtable* ht, const char* string);
void* luaA_hashtable_get(luaA_Hashtable* ht, const char* string);
void luaA_hashtable_set(luaA_Hashtable* ht, const char* string, void* item);

char* luaA_hashtable_find(luaA_Hashtable* ht, void* item);

void luaA_hashtable_map(luaA_Hashtable* ht, void (*func)(void*));

#endif
