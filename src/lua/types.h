
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

/**
  these defines can be used with luaA_struct_member to have checks on read added
  */
typedef char* char_20;
typedef char* char_32;
typedef char* char_52;
typedef char* char_filename_length;

#define dt_lua_init_typed_name_list_pair(L,type_name, list) dt_lua_init_name_list_pair_internal(L,#type_name,list)
#define dt_lua_init_name_list_pair(L, list) dt_lua_init_name_list_pair_internal(L,NULL,list)
void dt_lua_init_name_list_pair_internal(lua_State* L, const char*type_name, const char ** list);
/** helper to build types
  (0,0)
  sets a metadata with an "allocate" subtable for objects that are unique for a given numerical id
  -1 : the metatdata to use
  */
void dt_lua_init_numid(lua_State* L);
/** helper to build types
  (0,+(0|1))
  checks if data of type type_name already has an element with id, if yes push it at top.
  returns 1 if the the value was pushed
  */
int dt_lua_numid_find(lua_State* L,int id,const char * type_name);
/** helper to build types
  (0,0)
  takes the object at top of the stack
  - set the metatable
  - register it with the correct ID (lua_error if already exist)

  -1 the object to register
  */
void dt_lua_numid_register(lua_State* L,int id,const char * type_name);

/**
  (0,0)
  iterates through all object of singelton type "type" and call "function" on each
  the call has one parameter
  -1 : the object of the type
  */
void dt_lua_numid_foreach(lua_State*L,const char * type_name,lua_CFunction function);

/**
  (-1,+1)
  check that the top of the stack is a table, creates or find a subtable named "name", 
  adds it on top of the stack, and remove the previous table

  used to easily do a tree organisation of objects
*/
void dt_lua_goto_subtable(lua_State *L,const char* sub_name);

/**
  used internally to initialize types at DT startup
  */
void dt_lua_initialize_types(lua_State *L);
#endif
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
