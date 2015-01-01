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
#include "common/darktable.h"
#include "common/file_location.h"
#include "control/control.h"
#include "control/jobs/control_jobs.h"
#include "lua/call.h"
#include "lua/events.h"
#include "lua/image.h"
#include "gui/accelerators.h"
#include "common/imageio_module.h"


void dt_lua_event_trigger(lua_State *L, const char *event, int nargs)
{
  lua_getfield(L, LUA_REGISTRYINDEX, "dt_lua_event_list");
  if(lua_isnil(L, -1))
  { // events have been disabled
    lua_pop(L, nargs+1);
    return;
  }
  lua_getfield(L, -1, event);
  if(lua_isnil(L, -1))
  { // event doesn't exist
    lua_pop(L, nargs + 2);
    return;
  }
  lua_getfield(L, -1, "in_use");
  if(!lua_toboolean(L, -1))
  {
    lua_pop(L, nargs + 3);
    return;
  }
  lua_getfield(L, -2, "on_event");
  lua_getfield(L, -3, "data");
  lua_pushstring(L, event);
  for(int i = 1; i <= nargs; i++) lua_pushvalue(L, -7);
  dt_lua_do_chunk(L, nargs + 2, 0);
  lua_pop(L, nargs + 3);
  dt_lua_redraw_screen();
}


void dt_lua_event_add(lua_State *L, const char *evt_name)
{
  lua_newtable(L);

  lua_pushstring(L, evt_name);
  lua_setfield(L, -2, "name");

  lua_pushvalue(L, -2);
  lua_setfield(L, -2, "on_event");

  lua_pushvalue(L, -3);
  lua_setfield(L, -2, "on_register");

  lua_pushboolean(L, false);
  lua_setfield(L, -2, "in_use");

  lua_newtable(L);
  lua_setfield(L, -2, "data");

  lua_getfield(L, LUA_REGISTRYINDEX, "dt_lua_event_list");

  lua_getfield(L, -1, evt_name);
  if(!lua_isnil(L, -1))
  {
    luaL_error(L, "double registration of event %s", evt_name);
    // triggered early, so should cause an unhandled exception.
    // This is normal, this error is used as an assert
  }
  lua_pop(L, 1);

  lua_pushvalue(L, -2);
  lua_setfield(L, -2, evt_name);

  lua_pop(L, 4);
}


/*
 * KEYED EVENTS
 * these are events that are triggered with a key
 * i.e the can be registered multiple time with a key parameter and only the handler with the corresponding
 * key will be triggered. there can be only one handler per key
 *
 * when registering, the third argument is the key
 * when triggering, the first argument is the key
 *
 * data tables is "event => {key => callback}"
 */
int dt_lua_event_keyed_register(lua_State *L)
{
  // 1 is the data table
  // 2 is the event name (checked)
  // 3 is the action to perform (checked)
  // 4 is the key itself
  if(lua_isnoneornil(L, 4))
    return luaL_error(L, "no key provided when registering event %s", luaL_checkstring(L, 2));
  lua_getfield(L, 1, luaL_checkstring(L, 4));
  if(!lua_isnil(L, -1))
    return luaL_error(L, "key '%s' already registered for event %s ", luaL_checkstring(L, 4),
                      luaL_checkstring(L, 2));
  lua_pop(L, 1);

  lua_pushvalue(L, 3);
  lua_setfield(L, 1, luaL_checkstring(L, 4));

  return 0;
}


int dt_lua_event_keyed_trigger(lua_State *L)
{
  // 1 : the data table
  // 2 : the name of the event
  // 3 : the key
  // .. : other parameters
  lua_getfield(L, 1, luaL_checkstring(L, 3));
  if(lua_isnil(L, -1))
  {
    luaL_error(L, "event %s triggered for unregistered key %s", luaL_checkstring(L, 2),
               luaL_checkstring(L, 3));
  }
  const int callback_marker = lua_gettop(L);
  for(int i = 2; i < callback_marker; i++)
  {
    lua_pushvalue(L, i);
  }
  dt_lua_do_chunk_silent(L, callback_marker - 2, 0);
  return 0;
}

/*
 * MULTIINSTANCE EVENTS
 * these events can be registered multiple time with multiple callbacks
 * all callbacks will be called in the order they were registered
 *
 * all callbacks will receive the same parameters
 * the result is all return values from all callbacks
 *
 * data table is "event => { # => callback }
 */


int dt_lua_event_multiinstance_register(lua_State *L)
{
  // 1 is the data table
  // 2 is the event name (checked)
  // 3 is the action to perform (checked)

  // simply add the callback to the data table
  luaL_ref(L, 1);
  lua_pop(L, 2);
  return 0;
}

int dt_lua_event_multiinstance_trigger(lua_State *L)
{
  // 1 : the data table
  // 2 : the name of the event
  // .. : other parameters
  const int arg_top = lua_gettop(L);
  lua_pushnil(L);
  while(lua_next(L, 1))
  {
    for(int i = 2; i <= arg_top; i++)
    {
      lua_pushvalue(L, i);
    }
    dt_lua_do_chunk_silent(L, arg_top - 1, 0);
  }
  return 0;
}



static int lua_register_event(lua_State *L)
{
  // 1 is event name
  const char *evt_name = luaL_checkstring(L, 1);
  const int nparams = lua_gettop(L);
  // 2 is event handler
  luaL_checktype(L, 2, LUA_TFUNCTION);
  lua_getfield(L, LUA_REGISTRYINDEX, "dt_lua_event_list");
  lua_getfield(L, -1, evt_name);
  if(lua_isnil(L, -1))
  {
    lua_pop(L, 2);
    return luaL_error(L, "unknown event type : %s\n", evt_name);
  }
  lua_getfield(L, -1, "on_register");
  lua_getfield(L, -2, "data");
  for(int i = 1; i <= nparams; i++) lua_pushvalue(L, i);
  dt_lua_do_chunk_raise(L, nparams + 1, 0);
  lua_pushboolean(L, true);
  lua_setfield(L, -2, "in_use");
  lua_pop(L, 2);
  return 0;
}



int dt_lua_init_early_events(lua_State *L)
{
  lua_newtable(L);
  lua_setfield(L, LUA_REGISTRYINDEX, "dt_lua_event_list");
  dt_lua_push_darktable_lib(L);
  lua_pushstring(L, "register_event");
  lua_pushcfunction(L, &lua_register_event);
  lua_settable(L, -3);
  lua_pop(L, 1);
  return 0;
}

/****************************
 * MSIC EVENTS REGISTRATION *
 ****************************/


/*
 * shortcut events
 * keyed event with a tuned registration to handle shortcuts
 */
typedef struct
{
  char *name;
} shortcut_callback_data;
static int32_t shortcut_callback_job(dt_job_t *job)
{
  dt_lua_lock();
  shortcut_callback_data *t = dt_control_job_get_params(job);
  lua_pushstring(darktable.lua_state.state, t->name);
  free(t->name);
  free(t);
  dt_lua_event_trigger(darktable.lua_state.state, "shortcut", 1);
  dt_lua_unlock();
  return 0;
}
static gboolean shortcut_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                  GdkModifierType modifier, gpointer p)
{
  dt_job_t *job = dt_control_job_create(&shortcut_callback_job, "lua: on shortcut");
  if(job)
  {
    shortcut_callback_data *t = (shortcut_callback_data *)calloc(1, sizeof(shortcut_callback_data));
    if(!t)
    {
      dt_control_job_dispose(job);
    }
    else
    {
      dt_control_job_set_params(job, t);
      t->name = strdup(p);
      dt_control_add_job(darktable.control, DT_JOB_QUEUE_USER_FG, job);
    }
  }
  return TRUE;
}


static void closure_destroy(gpointer data, GClosure *closure)
{
  free(data);
}
static int register_shortcut_event(lua_State *L)
{
  // 1 is the data table
  // 2 is the event name (checked)
  // 3 is the action to perform (checked)
  // 4 is the key itself

  char *tmp = strdup(luaL_checkstring(L, 4));
  int result = dt_lua_event_keyed_register(L); // will raise an error in case of duplicate key
  dt_accel_register_lua(tmp, 0, 0);
  dt_accel_connect_lua(tmp, g_cclosure_new(G_CALLBACK(shortcut_callback), tmp, closure_destroy));
  return result;
}

/*
   called on a signal, from a secondary thread
   => we have the gdk lock, but the main UI thread can run if we release it
   */

static void on_export_image_tmpfile(gpointer instance, int imgid, char *filename,
                                    dt_imageio_module_format_t *format, dt_imageio_module_data_t *fdata,
                                    dt_imageio_module_storage_t *storage, dt_imageio_module_data_t *sdata,
                                    gpointer user_data)
{
  dt_lua_lock();
  luaA_push(darktable.lua_state.state, dt_lua_image_t, &imgid);
  lua_pushstring(darktable.lua_state.state, filename);
  luaA_push_type(darktable.lua_state.state, format->parameter_lua_type, fdata);
  if(storage)
  {
    luaA_push_type(darktable.lua_state.state, storage->parameter_lua_type, sdata);
  }
  else
  {
    lua_pushnil(darktable.lua_state.state);
  }
  dt_lua_event_trigger(darktable.lua_state.state, "intermediate-export-image", 4);
  dt_lua_unlock();
}



int dt_lua_init_events(lua_State *L)
{

  // events that don't really fit anywhere else
  lua_pushcfunction(L, register_shortcut_event);
  lua_pushcfunction(L, dt_lua_event_keyed_trigger);
  dt_lua_event_add(L, "shortcut");

  lua_pushcfunction(L, dt_lua_event_multiinstance_register);
  lua_pushcfunction(L, dt_lua_event_multiinstance_trigger);
  dt_lua_event_add(L, "intermediate-export-image");
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_IMAGE_EXPORT_TMPFILE,
                            G_CALLBACK(on_export_image_tmpfile), NULL);
  return 0;
}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
