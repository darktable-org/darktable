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
#include "lua/view.h"
#include "lua/modules.h"
#include "lua/types.h"
#include "lua/events.h"
#include "control/jobs/control_jobs.h"

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
  dt_lua_type_register_parent_type(L, my_type, luaA_type_find(L, "dt_view_t"));
  lua_pushcfunction(L, view_tostring);
  dt_lua_type_setmetafield_type(L,my_type,"__tostring");
};


typedef struct
{
  dt_view_t *old_view;
  dt_view_t *new_view;
} view_changed_callback_data_t;


static int32_t view_changed_callback_job(dt_job_t *job)
{
  dt_lua_lock();
  view_changed_callback_data_t *t = dt_control_job_get_params(job);
  dt_lua_module_entry_push(darktable.lua_state.state, "view", t->old_view->module_name);
  dt_lua_module_entry_push(darktable.lua_state.state, "view", t->new_view->module_name);
  free(t);
  dt_lua_event_trigger(darktable.lua_state.state, "view-changed", 2);
  dt_lua_unlock();
  return 0;
}

static void on_view_changed(gpointer instance, dt_view_t *old_view, dt_view_t *new_view, gpointer user_data)
{
  dt_job_t *job = dt_control_job_create(&view_changed_callback_job, "lua: on view changed");
  if(job)
  {
    view_changed_callback_data_t *t
        = (view_changed_callback_data_t *)calloc(1, sizeof(view_changed_callback_data_t));
    if(!t)
    {
      dt_control_job_dispose(job);
    }
    else
    {
      dt_control_job_set_params(job, t);
      t->old_view = old_view;
      t->new_view = new_view;
      dt_control_add_job(darktable.control, DT_JOB_QUEUE_USER_FG, job);
    }
  }
}

int dt_lua_init_early_view(lua_State *L)
{

  dt_lua_init_type(L, dt_view_t);
  lua_pushcfunction(L, id_member);
  dt_lua_type_register_const(L, dt_view_t, "id");
  lua_pushcfunction(L, name_member);
  dt_lua_type_register_const(L, dt_view_t, "name");

  dt_lua_module_new(L, "view"); // special case : will be attached to dt.gui in lua/gui.c:dt_lua_init_gui


  return 0;
}
int dt_lua_init_view(lua_State *L)
{
  lua_pushcfunction(L, dt_lua_event_multiinstance_register);
  lua_pushcfunction(L, dt_lua_event_multiinstance_trigger);
  dt_lua_event_add(L, "view-changed");
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_VIEWMANAGER_VIEW_CHANGED,
                            G_CALLBACK(on_view_changed), NULL);
  return 0;
}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
