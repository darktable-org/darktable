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

/* naming conventions
 * doxxx : runs the xxx with pcall conventions (pops args from stack,put result on stack, returns an error
 * code)
     * file : takes a file name and runs it
     * chunk : function is on the stack below the args
     * string : runs a string as a command
 * doxxx_silent : same but errors go to dt_console_log and nil is put as a result on the stack
 * doxxx_raise : same but a lua error is raised
 * dxxx_later : call will be dispatched to a secondary job, returns immediately, can't return values, will treat errors in secondary thread as "silent"
 */
int dt_lua_do_chunk(lua_State *L, int nargs, int nresults);
int dt_lua_do_chunk_silent(lua_State *L, int nargs, int nresults);
int dt_lua_do_chunk_raise(lua_State *L, int nargs, int nresults);
void dt_lua_do_chunk_later(lua_State *L, int nargs);

int dt_lua_dostring(lua_State *L, const char *command, int nargs, int nresults);
int dt_lua_dostring_silent(lua_State *L, const char *command, int nargs, int nresults);

int dt_lua_dofile_silent(lua_State *L, const char *filename, int nargs, int nresults);



int dt_lua_init_call(lua_State *L);

#endif
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
