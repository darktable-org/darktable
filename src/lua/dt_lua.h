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
void dt_lua_init(lua_State*L,const int init_gui);
/** runs a command in the DT lua environement, command in any valid lua string */
void dt_lua_dostring(const char* command);
/** executes the chunk on the top of the stack with nargs and nresult
  calling convention is similar to lua_pcall
  loadresult is to ease loading, if it's not 0 we assume the top message is an error message, we print it and return 0
  */
int dt_lua_do_chunk(lua_State *L,int loadresult,int nargs,int nresults);

/** executes a CFunction through dt_lua_do_chunk, no parameter, no result */
void dt_lua_protect_call(lua_State *L,lua_CFunction func);
// can be called within init functions
int dt_lua_push_darktable_lib(lua_State* L);

#endif
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
