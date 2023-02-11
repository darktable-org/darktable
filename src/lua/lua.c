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
#include "lua/lua.h"
#include "common/darktable.h"
#include "control/control.h"
#include "lua/call.h"

void dt_lua_debug_stack_internal(lua_State *L, const char *function, int line)
{
  printf("lua stack at %s:%d", function, line);
  if(!L)
  {
    printf(" Stack is NULL\n");
    return;
  }
  else
  {
    printf("(size %d),\n",lua_gettop(L)); //useful to detect underflows
  }
  for(int i = 1; i <= lua_gettop(L); i++)
  {
#if 1
    printf("\t%d:%s %s\n", i, lua_typename(L, lua_type(L, i)), luaL_tolstring(L, i, NULL));
    lua_pop(L, 1); // remove the result of luaL_tolstring() from the stack
#else
    // no tolstring when stack is really screwed up
    printf("\t%d:%s %p\n", i, lua_typename(L, lua_type(L, i)),lua_topointer(L,i));
#endif
  }
}

void dt_lua_debug_table_internal(lua_State *L, int t, const char *function, int line)
{
  t = lua_absindex(L,t);
  /* table is in the stack at index 't' */
  lua_len(L,t);
  printf("lua table at index %d at %s:%d (length %f)\n", t, function, line,lua_tonumber(L,-1));
  lua_pop(L,1);
  if(lua_type(L, t) != LUA_TTABLE)
  {
    printf("\tnot a table: %s\n", lua_typename(L, lua_type(L, t)));
    return;
  }
  lua_pushnil(L); /* first key */
  while(lua_next(L, t ) != 0)
  {
    /* uses 'key' (at index -2) and 'value' (at index -1) */
    if(lua_type(L,-2) != LUA_TNUMBER) {
      printf("%s - %s\n", lua_tostring(L, -2), lua_typename(L, lua_type(L, -1)));
    } else {
      printf("%f - %s\n", luaL_checknumber(L, -2), lua_typename(L, lua_type(L, -1)));
    }

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

/* LUA LOCKING
   Lua can only be run from a single thread at a time (the base lua engine
   is not protected against concurrent access) so we need a mutex to cover us

   However there are cases in lua/call.c where we need to lock the lua access
   from a thread and unlock it from another thread. This is done to guarantee
   that the lua code from the first thread is followed from the lua code in the
   second thread with no other lua thread having a chance to run in the middle

   pthread_mutex (and glib mutexes) have undefined behaviour if unlocked from
   a different thread. So we replace the simple mutex with a boolean protected
   by a pthread_cond, protected by a pthread_mutex
   */

void dt_lua_init_lock()
{
  pthread_mutexattr_t a;
  pthread_mutexattr_init(&a);
  //pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
  dt_pthread_mutex_init(&darktable.lua_state.mutex, &a);
  pthread_mutexattr_destroy(&a);
  pthread_cond_init(&darktable.lua_state.cond,NULL);
  // we want our lock initialized locked so that code between dt_lua_init_early() and dt_lua_init() can't use lua
  dt_pthread_mutex_lock(&darktable.lua_state.mutex);
  darktable.lua_state.exec_lock = true;
  dt_pthread_mutex_unlock(&darktable.lua_state.mutex);
}

void dt_lua_lock_internal(const char *function, const char *file, int line, gboolean silent)
{
  if(!silent && !darktable.lua_state.ending && pthread_equal(darktable.control->gui_thread, pthread_self()) != 0)
  {
    dt_print(DT_DEBUG_LUA, "LUA WARNING locking from the gui thread should be avoided\n");
    //g_assert(false);
  }

#ifdef _DEBUG
  dt_print(DT_DEBUG_LUA,"LUA DEBUG : thread %p waiting from %s:%d\n", g_thread_self(), function, line);
#endif
  dt_pthread_mutex_lock(&darktable.lua_state.mutex);
  while(darktable.lua_state.exec_lock == true) {
    dt_pthread_cond_wait(&darktable.lua_state.cond,&darktable.lua_state.mutex);
  }
  darktable.lua_state.exec_lock = true;
  dt_pthread_mutex_unlock(&darktable.lua_state.mutex);
#ifdef _DEBUG
  dt_print(DT_DEBUG_LUA,"LUA DEBUG : thread %p taken from %s:%d\n",  g_thread_self(), function, line);
#endif
}
void dt_lua_unlock_internal(const char *function, int line)
{
#ifdef _DEBUG
  dt_print(DT_DEBUG_LUA,"LUA DEBUG : thread %p released from %s:%d\n",g_thread_self(), function,line);
#endif
  dt_pthread_mutex_lock(&darktable.lua_state.mutex);
  darktable.lua_state.exec_lock = false;
  pthread_cond_signal(&darktable.lua_state.cond);
  dt_pthread_mutex_unlock(&darktable.lua_state.mutex);
}

static gboolean async_redraw(gpointer data)
{
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


// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
