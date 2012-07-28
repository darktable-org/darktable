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
#ifndef DT_LUA_H
#define DT_LUA_H
#include <lualib.h>
#include <lua.h>
#include <lauxlib.h>

/** initialize lua stuff at DT start time */
void dt_lua_init();
/** runs a command in the DT lua environement, command in any valid lua string */
void dt_lua_dostring(const char* command);


/**
  define an OO type to create, will be used as a submodule of the darktable module
  */
typedef struct {
	const char *name; /// the recommanded name for the module (modname)
	lua_CFunction load; /// follows the loader convention, i.e returns a table, nil, or object that can be maped to name
	lua_CFunction clean; /// to be called when a DT lua run is finished to release locks etc... WARNING : this is not a GC, only for locks
} dt_lua_type;

/** helper to build types
  sets a metadata with an __pair methamethod that will go throught the list of names
  -1 : the metatdata to use
  */
void dt_lua_init_name_list_pair(lua_State* L, const char ** list);
/** helper to build types
  sets a metadata with an "allocate" subtable for imgid based singleton
  -1 : the metatdata to use
  */
void dt_lua_init_singleton(lua_State* L);
/** helper to build types
  checks if data of type type_name already has an element with id, if yes push it at top.
  returns 1 if the the value was pushed
  */
int dt_lua_singleton_find(lua_State* L,int id,const dt_lua_type*type);
/** helper to build types
  takes the object at top of the stack
  - set the metatable
  - register it with the correct ID (lua_error if already exist)

  -1 the object to register
  */
void dt_lua_singleton_register(lua_State* L,int id,const dt_lua_type*type);

/** helper to build types
  returns the object of type "type" at the index indicated
  */
void *dt_lua_check(lua_State* L,int index,const dt_lua_type*type);
/** helper to build types
  a function that can be used as is for the push field of singletons based on imgid
  */

#endif
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
