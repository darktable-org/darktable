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


/** runs a command in the DT lua environement, command in any valid lua string 
 * follows the lua_pcall calling convention for errors
 */
int dt_lua_dostring(lua_State *L,const char* command,int nargs,int nresults);

/** directly run a lua chunk
 * follows the lua_pcall calling convention for errors
 */
int dt_lua_do_chunk(lua_State *L,int nargs,int nresults);

/** Wrap a dt_lua_d_chunko call so that it prints an error on the console and ignores it.
 *  Used to call lua code from the main DT engine when we don't care about the result
 */
int dt_lua_do_chunk_silent(lua_State *L,int nargs, int nresults);

/** Wrap a dt_lua_do_chunk call so that it raises an lua error in case of internal error */
int dt_lua_do_chunk_raise(lua_State *L,int nargs, int nresults);

/** runs the content of a file
 */
int dt_lua_dofile_silent(lua_State *L,const char* filename,int nargs,int nresults);

int dt_lua_init_call(lua_State *L);

#endif
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
