/*
   This file is part of darktable,
   Copyright (C) 2014-2021 darktable developers.

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
#include "lua/view.h"
#include "control/jobs/control_jobs.h"
#include "lua/events.h"
#include "lua/modules.h"
#include "lua/types.h"


static int id_member(lua_State *L)
{
  dt_view_t *module = *(dt_view_t **)lua_touserdata(L, 1);
  lua_pushstring(L, module->module_name);
  return 1;
}

static int name_member(lua_State *L)
{
  dt_view_t *module = *(dt_view_t **)lua_touserdata(L, 1);
  lua_pushstring(L, module->name(module));
  return 1;
}

static int view_tostring(lua_State *L)
{
  dt_view_t *module = *(dt_view_t **)lua_touserdata(L, -1);
  lua_pushstring(L, module->module_name);
  return 1;
}

void dt_lua_register_view(lua_State *L, dt_view_t *module)
{
  dt_lua_module_entry_new_singleton(L, "view", module->module_name, module);
  int my_type = dt_lua_module_entry_get_type(L, "view", module->module_name);
  dt_lua_type_register_parent_type(L, my_type, luaA_type_find(L, "dt_lua_view_t"));
  lua_pushcfunction(L, view_tostring);
  dt_lua_type_setmetafield_type(L,my_type,"__tostring");
};


static void on_view_changed(gpointer instance, dt_view_t *old_view, dt_view_t *new_view, gpointer user_data)
{
  dt_lua_async_call_alien(dt_lua_event_trigger_wrapper,
      0,NULL,NULL,
      LUA_ASYNC_TYPENAME,"const char*","view-changed",
      LUA_ASYNC_TYPENAME,"unknown",old_view,
      LUA_ASYNC_TYPENAME,"unknown",new_view,
      LUA_ASYNC_DONE);
}

int dt_lua_init_early_view(lua_State *L)
{

  dt_lua_init_type(L, dt_lua_view_t);
  lua_pushcfunction(L, id_member);
  dt_lua_type_register_const(L, dt_lua_view_t, "id");
  lua_pushcfunction(L, name_member);
  dt_lua_type_register_const(L, dt_lua_view_t, "name");

  dt_lua_module_new(L, "view"); // special case : will be attached to dt.gui in lua/gui.c:dt_lua_init_gui


  return 0;
}
int dt_lua_init_view(lua_State *L)
{
  lua_pushcfunction(L, dt_lua_event_multiinstance_register);
  lua_pushcfunction(L, dt_lua_event_multiinstance_destroy);
  lua_pushcfunction(L, dt_lua_event_multiinstance_trigger);
  dt_lua_event_add(L, "view-changed");
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_VIEWMANAGER_VIEW_CHANGED,
                            G_CALLBACK(on_view_changed), NULL);
  return 0;
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
