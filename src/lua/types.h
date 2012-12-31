
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
typedef char* char_path_length;



/**
  (0,+1)
   creates a struct-like type that is lua friendly
   * iteratable 
   * indexable
   * new indexable
   if the type has been registered with luaA and some members described there, they will be handle via luaA
   unknown fields are refused if not in the list, or passed to index/newindex for handling

   list can be NULL in which case the type will only use luaA (or be a struct with no member if there is no luaA type

   this function leaves the metatable on top of the stack for further editing

   */
   
#define dt_lua_init_type(L,type_name, list,index,newindex) do {\
  luaA_type_add(#type_name,sizeof(type_name)); \
  dt_lua_init_type_internal(L,#type_name,list,index,newindex); \
}while(0)

void dt_lua_init_type_internal(lua_State* L, const char*type_name,const char ** list,lua_CFunction index,lua_CFunction newindex);

/**
  (-1,+3)
  returns a next function, the top value and a nil
  see lua documentation on how pairs() work

  this function uses a __luaA_Type entry in the metadata containing the luaA_Type for the type iterated

  upvalue 1 : an array of const char* for special members (can be null, can be nil)

  this function is the one used by dt_lua_init_type internally
  */
int dt_lua_autotype_pairs(lua_State *L);
/**
  used internally to initialize types at DT startup
  */
void dt_lua_initialize_types(lua_State *L);
#endif
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
