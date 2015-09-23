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
 * doxxx_silent : same but errors go to dt_console_log and nil is put as a result on the stack returns the number of results
 * doxxx_raise : same but a lua error is raised returns the number of results
 * doxxx_later : call will be dispatched to a secondary job, returns immediately, can't return values, will treat errors in secondary thread as "silent"
 * doxxx_async : similar to doxxx_later, but has a different API allowing it to be called without the lua lock
   function : the lua function to call
   extra :  extra parameter to pusher
   ... : extra parameters for the final call, each param becomes three param in ...
         * the type of the type descriptor as a dt_lua_async_call_arg_type
         * the type descriptor itself (int or string, depending on the previous param
         * a gpointer with the value (GINT_TO_POINTER is fine)
       list must finish with LUA_ASYNC_DONE
 */
int dt_lua_do_chunk(lua_State *L, int nargs, int nresults);
int dt_lua_do_chunk_silent(lua_State *L, int nargs, int nresults);
int dt_lua_do_chunk_raise(lua_State *L, int nargs, int nresults);
void dt_lua_do_chunk_later_internal(const char * function, int line,lua_State *L, int nargs);
#define dt_lua_do_chunk_later(L,nargs) dt_lua_do_chunk_later_internal(__FUNCTION__,__LINE__,L,nargs)

int dt_lua_dostring(lua_State *L, const char *command, int nargs, int nresults);
int dt_lua_dostring_silent(lua_State *L, const char *command, int nargs, int nresults);

int dt_lua_dofile_silent(lua_State *L, const char *filename, int nargs, int nresults);


/*
   Call a lua function asynchronously, parameters are passed through varags,
   and temporarly stored in a glist before pushing in the stack

   This function CAN be called with the gtk lock held.
   * function : the function to call
   * ... triplets of dt_lua_asyc_call_arg_type, type_description, value,
     * LUA_ASYNC_TYPEID : the type description is a luaA_Typeid
     * LUA_ASYNC_TYPENAME : the type description is a const char * which is a type name for the lua typing system
     * ..._WITH_FREE : the value is a pointer that must be freed after being pushed on the stack
       add a GClosure parameter : the freeing function. will be invoked with the pointer to free plus any extra data given at closure creation
 */

typedef enum {
  LUA_ASYNC_TYPEID,
  LUA_ASYNC_TYPEID_WITH_FREE,
  LUA_ASYNC_TYPENAME,
  LUA_ASYNC_TYPENAME_WITH_FREE,
  LUA_ASYNC_DONE
} dt_lua_async_call_arg_type;
void dt_lua_do_chunk_async_internal(const char * call_function, int line, lua_CFunction function,dt_lua_async_call_arg_type arg_type,...);
#define dt_lua_do_chunk_async(L,arg,...) dt_lua_do_chunk_async_internal(__FUNCTION__,__LINE__,L,arg,__VA_ARGS__)

/*
   call a lua function that is its upvalue, with an unchanged stack
   the function is called within the gtk thread so it
   IS NOT ALLOWED TO CALL USER CODE AND SHOULD BE FAST


   */
int dt_lua_gtk_wrap(lua_State*L);



int dt_lua_init_call(lua_State *L);

#endif
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
