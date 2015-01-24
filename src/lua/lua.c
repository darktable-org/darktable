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
#include "common/darktable.h"
#include "control/control.h"
#include "lua/call.h"

void dt_lua_debug_stack_internal(lua_State *L, const char *function, int line)
{
  printf("lua stack at %s:%d (total %d)\n", function, line,lua_gettop(L));
  for(int i = 1; i <= lua_gettop(L); i++)
  {
    printf("\t%d:%s %s\n", i, lua_typename(L, lua_type(L, i)), luaL_tolstring(L, i, NULL));
    lua_pop(L, 1);
  }
}
void dt_lua_debug_table_internal(lua_State *L, int t, const char *function, int line)
{
  /* table is in the stack at index 't' */
  printf("lua table at index %d at %s:%d\n", t, function, line);
  if(lua_type(L, t) != LUA_TTABLE)
  {
    printf("\tnot a table: %s\n", lua_typename(L, lua_type(L, t)));
    return;
  }
  lua_pushnil(L); /* first key */
  while(lua_next(L, t - 1) != 0)
  {
    /* uses 'key' (at index -2) and 'value' (at index -1) */
    printf("%s - %s\n", luaL_checkstring(L, -2), lua_typename(L, lua_type(L, -1)));
    /* removes 'value'; keeps 'key' for next iteration */
    lua_pop(L, 1);
  }
}


int dt_lua_push_darktable_lib(lua_State *L)
{
  lua_getfield(L, LUA_REGISTRYINDEX, "dt_lua_dtlib");
  if(lua_isnil(L, -1))
  {
    lua_pop(L, 1);
    lua_newtable(L);
    lua_newtable(L);
    lua_setmetatable(L, -2);
    lua_pushvalue(L, -1);
    lua_setfield(L, LUA_REGISTRYINDEX, "dt_lua_dtlib");
  }
  return 1;
}


void dt_lua_goto_subtable(lua_State *L, const char *sub_name)
{
  luaL_checktype(L, -1, LUA_TTABLE);
  lua_getfield(L, -1, sub_name);
  if(lua_isnil(L, -1))
  {
    lua_pop(L, 1);
    lua_newtable(L);
    lua_setfield(L, -2, sub_name);
    lua_getfield(L, -1, sub_name);
  }
  lua_remove(L, -2);
}

void dt_lua_init_lock()
{
  pthread_mutexattr_t a;
  pthread_mutexattr_init(&a);
  pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
  dt_pthread_mutex_init(&darktable.lua_state.mutex, &a);
  pthread_mutexattr_destroy(&a);
}

void dt_lua_lock()
{
  if(!darktable.lua_state.ending && pthread_equal(darktable.control->gui_thread, pthread_self()) != 0)
  {
    dt_print(DT_DEBUG_LUA, "LUA WARNING locking from the gui thread should be avoided\n");
  }

  dt_pthread_mutex_lock(&darktable.lua_state.mutex);
}
void dt_lua_unlock()
{
  dt_pthread_mutex_unlock(&darktable.lua_state.mutex);
}

static gboolean async_redraw(gpointer data)
{
    dt_control_signal_raise(darktable.signals, DT_SIGNAL_FILMROLLS_CHANGED); // just for good measure
    dt_control_queue_redraw();
    return false;
}

void dt_lua_redraw_screen()
{
  if(darktable.gui != NULL)
  {
    g_idle_add(async_redraw,NULL);
  }
}

typedef struct gtk_wrap_communication {
  GCond end_cond;
  GMutex end_mutex;
  lua_State *L;
  int retval;
} gtk_wrap_communication;

gboolean dt_lua_gtk_wrap_callback(gpointer data)
{
  gtk_wrap_communication *communication = (gtk_wrap_communication*)data;
  g_mutex_lock(&communication->end_mutex);
  communication->retval = dt_lua_do_chunk(communication->L,lua_gettop(communication->L)-1,LUA_MULTRET);
  g_cond_signal(&communication->end_cond);
  g_mutex_unlock(&communication->end_mutex);
  return false;
} 

int dt_lua_gtk_wrap(lua_State*L)
{ 
  lua_pushvalue(L,lua_upvalueindex(1));
  lua_insert(L,1);
  if(pthread_equal(darktable.control->gui_thread, pthread_self())) {
    return dt_lua_do_chunk_raise(L,lua_gettop(L)-1,LUA_MULTRET);
  } else {
    gtk_wrap_communication communication;
    g_mutex_init(&communication.end_mutex);
    g_cond_init(&communication.end_cond);
    communication.L = L;
    g_mutex_lock(&communication.end_mutex);
    g_main_context_invoke(NULL,dt_lua_gtk_wrap_callback,&communication);
    g_cond_wait(&communication.end_cond,&communication.end_mutex);
    g_mutex_unlock(&communication.end_mutex);
    g_mutex_clear(&communication.end_mutex);
    if(communication.retval == LUA_OK) {
      return lua_gettop(L);
    } else {
      return lua_error(L);
    }
  }

}


// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
