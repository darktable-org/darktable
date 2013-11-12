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
#ifndef DT_LUA_CALL_H
#define DT_LUA_CALL_H


/** runs a command in the DT lua environement, command in any valid lua string */
int dt_lua_dostring(lua_State *L,const char* command);

/** executes a CFunction through dt_lua_do_chunk, no parameter, no result */
int dt_lua_protect_call(lua_State *L,lua_CFunction func);

/** runs the content of a file */
int dt_lua_dofile(lua_State *L,const char* filename);

/** directly run a lua chunk */
int dt_lua_do_chunk(lua_State *L,int nargs,int nresults);


int dt_lua_init_call(lua_State *L);

#endif
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
