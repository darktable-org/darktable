
/*
    This file is part of darktable,
    Copyright (C) 2013-2021 darktable developers.

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

#pragma once

#include <gtk/gtk.h>
#include <lautoc.h>
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>

/**
  these defines can be used with luaA_struct_member to have checks on read added
  */
typedef char *char_20;
typedef char *char_32;
typedef char *char_52;
typedef char *char_64;
typedef char *char_128;
typedef char *char_256;
typedef char *char_512;
typedef char *char_1024;
typedef char *char_filename_length;
typedef char *char_path_length;
typedef const char *const_string; // string that has no push function
typedef double protected_double;  // like double, but NAN is mapped to nil
typedef double progress_double; // a double in [0.0,1.0] any value out of bound will be silently converted to
                                // the bound both at push and pull time

// Types added to the lua type system and usable externally
typedef GtkOrientation dt_lua_orientation_t;
typedef GtkAlign dt_lua_align_t;
typedef PangoEllipsizeMode dt_lua_ellipsize_mode_t;



/** (0,0)
  register a C type to the dt-lua subsystem

  the type can be converted to/from C using the usual luaA functions.
  the type becomes a full userdata (i.e malloc+memcpy then pushed on the lua stack, released when not
 referenced in lua)
  you can use luaL_checkudata to get and check the data from the stack

  the following metamethods are defined for the type
 * __luaA_TypeName : string with the associated C type
 * __luaA_Type : int, the associated luaA_Type
 * __pairs : will return (__next,obj,nil)
 * __next : will iteratethrough the __get table of obj
 * __index : will look into the __get table to find a callback, then raise an error
 * __newindex : will look into the __set table to find a callback, then raise an error
 * __get : empty table, contains getters, similar API to __index
 * __set : empty table, contains setters, similar API to __newindex

   */
#define dt_lua_init_type(L, type_name) dt_lua_init_type_type(L, luaA_type(L, type_name))
luaA_Type dt_lua_init_type_type(lua_State *L, luaA_Type type_id);



/*********************************/
/* MEMBER REGISTRATION FUNCTIONS */
/*********************************/
/// register a read-only member, the member function is popped from the stack
#define dt_lua_type_register_const(L, type_name, name)                                                       \
  dt_lua_type_register_const_type(L, luaA_type_find(L, #type_name), name)
void dt_lua_type_register_const_type(lua_State *L, luaA_Type type_id, const char *name);

/// register a read-write member, the member function is popped from the stack
#define dt_lua_type_register(L, type_name, name)                                                             \
  dt_lua_type_register_type(L, luaA_type_find(L, #type_name), name)
void dt_lua_type_register_type(lua_State *L, luaA_Type type_id, const char *name);

/// register a function for all fields of luaautoc struct, the member function is popped from the stack
/// detects red-only vs read-write automatically
#define dt_lua_type_register_struct(L, type_name)                                                            \
  dt_lua_type_register_struct_type(L, luaA_type_find(L, #type_name))
void dt_lua_type_register_struct_type(lua_State *L, luaA_Type type_id);

// register a function for number index
// first push the len function (can be nil)
// then push the member function
#define dt_lua_type_register_number(L, type_name)                                                            \
  dt_lua_type_register_number_type(L, luaA_type_find(L, #type_name))
void dt_lua_type_register_number_type(lua_State *L, luaA_Type type_id);
#define dt_lua_type_register_number_const(L, type_name)                                                      \
  dt_lua_type_register_number_const_type(L, luaA_type_find(L, #type_name))
void dt_lua_type_register_number_const_type(lua_State *L, luaA_Type type_id);

/// register a type as a parent type
/// the type will reuse all members and metafiels from the parent (unless it has its own)
/// inheritance will be marked in __luaA_ParentMetatable
/// THIS FUNCTION MUST BE CALLED AFTER PARENT WAS COMPLETELY DEFINED
#define dt_lua_type_register_parent(L, type_name, parent_type_name)                                          \
  dt_lua_type_register_parent_type(L, luaA_type_find(L, #type_name), luaA_type_find(L, #parent_type_name))
void dt_lua_type_register_parent_type(lua_State *L, luaA_Type type_id, luaA_Type parent_type_id);

/********************/
/* MEMBER FUNCTIONS */
/********************/
/// member function for common members. The common member must be the only upvalue of the function
int dt_lua_type_member_common(lua_State *L);
/// member function for luaautoc struct, will use luaautoc to push/pull content
int dt_lua_type_member_luaautoc(lua_State *L);

/***********/
/* HELPERS */
/***********/

/**
  * similar to dt_lua_init_type but creates a type for int or gpointer singletons
  * the type must match and will guarantee a singleton per value
  * i.e if you push the same int twice, you will push the same lua object
  * not recreate a different one each time
  * the singleton objects will still correctly be garbage collected
  */
#define dt_lua_init_int_type(L, type_name) dt_lua_init_int_type_type(L, luaA_type(L, type_name))
luaA_Type dt_lua_init_int_type_type(lua_State *L, luaA_Type type_id);
#define dt_lua_init_gpointer_type(L, type_name) dt_lua_init_gpointer_type_type(L, luaA_type(L, type_name))
luaA_Type dt_lua_init_gpointer_type_type(lua_State *L, luaA_Type type_id);

/**
  * make a pointer an alias of another pointer. Both pointers will push the same lua object
  * when pushed on the stack. The object contains the original pointer
  */
#define dt_lua_type_gpointer_alias(L,type_name,pointer,alias) \
  dt_lua_type_gpointer_alias_type(L,luaA_type(L,type_name),pointer,alias)
void dt_lua_type_gpointer_alias_type(lua_State*L,luaA_Type type_id,void* pointer,void* alias);


/**
  * drop a gpointer. Pushing the pointer again will create a new object.
  * We can't guarantee when the original object will be GC, but it will point to NULL
  * instead of its normal content. accessing it from the lua side will cause an error
  * luaA_to will also raise an error
  * NOTE : if the object had aliases, the aliases will return NULL too.
  */
void dt_lua_type_gpointer_drop(lua_State*L, void* pointer);

/**
 * similar to dt_lua_init_type but creates a singleton type
 * that is : a type who has only one instance (which is a void* pointer)
 * returns the associated luaA_Type so it can be decorated
 * push the single instance of the object on the stack
 */
luaA_Type dt_lua_init_singleton(lua_State *L, const char *unique_name, void *data);

/**
 * similar to dt_lua_init_singleton but the singleton has push and pop functions to save/restore
 * the lua object called on
 */
luaA_Type dt_lua_init_wrapped_singleton(lua_State *L, lua_CFunction pusher, lua_CFunction getter,
                                        const char *unique_name, void *data);



#define dt_lua_isa(L, index, type) dt_lua_isa_type(L, index, luaA_type(L, type))

gboolean dt_lua_isa_type(lua_State *L, int index, luaA_Type type_id);
gboolean dt_lua_typeisa_type(lua_State *L, luaA_Type obj_type, luaA_Type type_id);

#define dt_lua_type_setmetafield(L,type_name,name) dt_lua_type_setmetafield_type(L,luaA_type(L,type_name),name)
void dt_lua_type_setmetafield_type(lua_State*L,luaA_Type type,const char* method_name);

int dt_lua_init_early_types(lua_State *L);

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
