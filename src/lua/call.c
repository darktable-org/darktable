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
#include "lua/lua.h"
#include "lua/call.h"
#include "control/control.h"

static int dump_error(lua_State *L)
{
  const char * message = lua_tostring(L,-1);
  if(darktable.unmuted & DT_DEBUG_LUA)
    luaL_traceback(L,L,message,0);
  // else : the message is already on the top of the stack, don't touch
  return 1;
}

int dt_lua_do_chunk(lua_State *L,int nargs,int nresults)
{
  int result;
  lua_pushcfunction(L,dump_error);
  lua_insert(L,lua_gettop(L)-(nargs+1));
  result = lua_gettop(L)-(nargs+1); // remember the stack size to findout the number of results in case of multiret
  if(lua_pcall(L, nargs, nresults,result))
  {
    dt_print(DT_DEBUG_LUA,"LUA ERROR %s\n",lua_tostring(L,-1));
    lua_pop(L,2);
    if(nresults !=LUA_MULTRET)
    {
      for(int i= 0 ; i < nresults; i++)
      {
        lua_pushnil(L);
      }
    }
  } else {
    lua_remove(L,result); // remove the error handler
  }
  result= lua_gettop(L) -result;

  if(darktable.gui!=NULL)
  {
    dt_control_signal_raise(darktable.signals, DT_SIGNAL_FILMROLLS_CHANGED); // just for good measure
    dt_control_queue_redraw();
  }
  return result;
}

int dt_lua_protect_call(lua_State *L,lua_CFunction func)
{
  lua_pushcfunction(L,func);
  return dt_lua_do_chunk(L,0,0);
}
int dt_lua_dostring(lua_State *L,const char* command)
{
  if(luaL_loadstring(L, command))
  {
    dt_print(DT_DEBUG_LUA,"LUA ERROR %s\n",lua_tostring(L,-1));
    lua_pop(L,1);
    return -1;
  }
  return dt_lua_do_chunk(L,0,0);
}

int dt_lua_dofile(lua_State *L,const char* filename)
{
  if(luaL_loadfile(L, filename))
  {
    dt_print(DT_DEBUG_LUA,"LUA ERROR %s\n",lua_tostring(L,-1));
    lua_pop(L,1);
    return -1;
  }
  return dt_lua_do_chunk(L,0,0);
}

static gboolean poll_events(gpointer data)
{
  lua_getfield(darktable.lua_state,LUA_REGISTRYINDEX,"dt_lua_delayed_events");
  lua_pushlightuserdata(darktable.lua_state,data);
  lua_gettable(darktable.lua_state,-2);
  if(lua_isnoneornil(darktable.lua_state,-1)) {
    lua_pop(darktable.lua_state,2);
    luaL_error(darktable.lua_state,"Unknown thread was called for delay action");
    return FALSE;
  }
  lua_State * L = lua_tothread(darktable.lua_state,-1);
  dt_lua_do_chunk(L,lua_gettop(L) -1,0);
  lua_pushlightuserdata(darktable.lua_state,data);
  lua_pushnil(darktable.lua_state);
  lua_settable(darktable.lua_state,-4);
  lua_pop(darktable.lua_state,2);
  return FALSE;
}


void dt_lua_delay_chunk(lua_State *L,int nargs) {
  lua_getfield(darktable.lua_state,LUA_REGISTRYINDEX,"dt_lua_delayed_events");
  lua_State * new_thread = lua_newthread(L);
  lua_pushlightuserdata(L,new_thread);
  lua_insert(L,-2);
  lua_settable(L,-3);
  lua_pop(L,1);
  lua_xmove(L,new_thread,nargs+1);
  gdk_threads_add_idle(poll_events,new_thread);
}




int dt_lua_init_call(lua_State *L) {
  lua_newtable(L);
  lua_setfield(darktable.lua_state,LUA_REGISTRYINDEX,"dt_lua_delayed_events");
  return 0;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
