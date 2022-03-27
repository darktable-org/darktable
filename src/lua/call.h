/*
   This file is part of darktable,
   Copyright (C) 2013-2020 darktable developers.

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

#include "lua/lua.h"

/*
   pop a function from the top of the stack, push a new version on the stack
   that will be called from the GTK main thread
   */
#define dt_lua_gtk_wrap(L) dt_lua_gtk_wrap_internal(L,__FUNCTION__,__LINE__)
void dt_lua_gtk_wrap_internal(lua_State*L,const char* function, int line);

/*
   similar to pcall, but in case of error, it will call dt_lua_check_print_error with a proper stack
   */
int dt_lua_treated_pcall(lua_State*L, int nargs, int nresults);
/*
   deal with lua_pcall calling convention
   * print the string on the top of L if result != LUA_OK
   * pop the error string
   * return result
   */
int dt_lua_check_print_error(lua_State* L, int result);


/*
   call a function asynchronously
   a callback+data can be provided, the callback will be called when the job is finished
   if no callback is provided, a message will be printed in case of error
   */

typedef void (*dt_lua_finish_callback)(lua_State* L,int result,void* data);

void dt_lua_async_call_internal(const char * function, int line,lua_State *L, int nargs,int nresults,dt_lua_finish_callback cb, void*data);
#define dt_lua_async_call(L,nargs,nresults,cb,data) dt_lua_async_call_internal(__FUNCTION__,__LINE__,L,nargs,nresults,cb,data)


/*
   Call a lua function asynchronously, parameters are passed through varags,
   and temporarily stored in a glist before pushing in the stack

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
void dt_lua_async_call_alien_internal(const char * call_function, int line,lua_CFunction function,int nresults,dt_lua_finish_callback cb, void*data, dt_lua_async_call_arg_type arg_type,...);
#define dt_lua_async_call_alien(fn,nresults,cb,data,arg,...) dt_lua_async_call_alien_internal(__FUNCTION__,__LINE__,fn,nresults,cb,data,arg,__VA_ARGS__)



void dt_lua_async_call_string_internal(const char* function, int line,const char* lua_string,int nresults,dt_lua_finish_callback cb, void*cb_data);
#define dt_lua_async_call_string(lua_string,nresults,cb,data) dt_lua_async_call_string_internal(__FUNCTION__,__LINE__,lua_string,nresults,cb,data)




int dt_lua_init_call(lua_State *L);

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

