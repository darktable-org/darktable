
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
#ifndef DT_LUA_TYPES_H
#define DT_LUA_TYPES_H
#include <lualib.h>
#include <lua.h>
#include <lauxlib.h>
#include <lautoc.h>

/**
  these defines can be used with luaA_struct_member to have checks on read added
  */
typedef char* char_20;
typedef char* char_32;
typedef char* char_52;
typedef char* char_64;
typedef char* char_128;
typedef char* char_512;
typedef char* char_1024;
typedef char* char_filename_length;
typedef char* char_path_length;
typedef const char * const_string; // string that has no push function



/**
  (0,0)
  register a C type to the dt-lua subsystem

  the type can be converted to/from C using the usual luaA functions.
  the type becomes a full userdata (i.e malloc+memcpy then pushed on the lua stack, released when not referenced in lua)
  you can use luaL_checkudata to get and check the data from the stack

  the following metamethods are defined for the type
 * __luaA_TypeName : string with the associated C type
 * __luaA_Type : int, the associated luaA_Type
 * __pairs : will retun (__next,obj,nil)
 * __next : will iteratethrough the __get table of obj
 * __index : will look into the __get table to find a callback, then  raise an error
 * __newindex : will look into the __set table to find a callback, then raise an error
 * __get : empty table, contains getters, similar API to __index
 * __set : empty table, contains setters, similar API to __newindex

   */

#define dt_lua_init_type(L,type_name) \
  dt_lua_init_type_typeid(L,luaA_type_id(type_name))
luaA_Type dt_lua_init_type_typeid(lua_State* L,luaA_Type type_id);

/** helper functions to register index hanlers
   each one follow the same logic, you give an index, optionally a newindex and a list of entries it can handle
   */
/// register for names (const char*) passed as varargs
#define dt_lua_register_type_callback(L,type_name,index,newindex,...) \
  dt_lua_register_type_callback_typeid(L,luaA_type_find(#type_name),index,newindex,__VA_ARGS__)
void dt_lua_register_type_callback_typeid(lua_State* L,luaA_Type type_id,lua_CFunction index, lua_CFunction newindex,...);
/// register for an array of char* ended by a NULL entry
#define dt_lua_register_type_callback_list(L,type_name,index,newindex,name_list) \
  dt_lua_register_type_callback_list_typeid(L,luaA_type_find(#type_name),index,newindex,name_list)
void dt_lua_register_type_callback_list_typeid(lua_State* L,luaA_Type type_id,lua_CFunction index, lua_CFunction newindex,const char**list);
/// register using luaautoc callbacks from a type's members. If both index and newindex are null, provide a default one
#define dt_lua_register_type_callback_type(L,type_name,index,newindex,struct_type_name) \
  dt_lua_register_type_callback_type_typeid(L,luaA_type_find(#type_name),index,newindex,luaA_type_find(#struct_type_name))
void dt_lua_register_type_callback_type_typeid(lua_State* L,luaA_Type type_id,lua_CFunction index, lua_CFunction newindex,luaA_Type struct_type_id);
/// register a special handler for number indexes
#define dt_lua_register_type_callback_number(L,type_name,index,newindex,length) \
  dt_lua_register_type_callback_number_typeid(L,luaA_type_find(#type_name),index,newindex,length)
void dt_lua_register_type_callback_number_typeid(lua_State* L,luaA_Type type_id,lua_CFunction index, lua_CFunction newindex,lua_CFunction length);
/// pop the top of the stack, register it as a const returned for the entry
#define dt_lua_register_type_callback_stack(L,type_name,name) \
  dt_lua_register_type_callback_stack_typeid(L,luaA_type_find(#type_name),name)
void dt_lua_register_type_callback_stack_typeid(lua_State* L,luaA_Type type_id,const char* name);
// use an other type as a fallback (inheritence
#define dt_lua_register_type_callback_inherit(L,type_name,parent_type_name) \
  dt_lua_register_type_callback_inherit_typeid(L,luaA_type_find(#type_name),luaA_type_find(#parent_type_name)
void dt_lua_register_type_callback_inherit_typeid(lua_State* L,luaA_Type type_id,luaA_Type parent_type_id);

/**
  * similar to dt_lua_init_type but creates a type for int id
  * the type must be an int and will guarentee a singleton per memory pointed
  * i.e if you push the same int twice, you will push the same lua object 
  * not recreate a different one each time
  */
#define dt_lua_init_int_type(L,type_name) \
  dt_lua_init_int_type_typeid(L,luaA_type_id(type_name))
luaA_Type dt_lua_init_int_type_typeid(lua_State* L,luaA_Type type_id);

/**
 * similar to dt_lua_init_type but creates a singleton type
 * that is : a type who has only one instance (which is a null pointer)
 * returns the associated luaA_Type so it can be decorated
 * push the single instance of the object on the stack
 */
luaA_Type dt_lua_init_singleton(lua_State* L,const char * unique_name);


void dt_lua_initialize_types(lua_State *L);



#endif
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
