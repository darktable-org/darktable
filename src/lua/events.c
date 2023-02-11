/*
   This file is part of darktable,
   Copyright (C) 2013-2023 darktable developers.

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
#include "lua/events.h"
#include "common/darktable.h"
#include "common/file_location.h"
#include "control/control.h"
#include "control/jobs/control_jobs.h"
#include "gui/accelerators.h"
#include "imageio/imageio_module.h"
#include "lua/call.h"
#include "lua/image.h"


void dt_lua_event_trigger(lua_State *L, const char *event, int nargs)
{
  // 1  event name
  // 2+ arguments

  // check that events are enabled
  lua_getfield(L, LUA_REGISTRYINDEX, "dt_lua_event_list");
  if(lua_isnil(L, -1))
  { // events have been disabled
    lua_pop(L, nargs+1);
    return;
  }

  // check that the event exists
  lua_getfield(L, -1, event);
  if(lua_isnil(L, -1))
  { // event doesn't exist
    lua_pop(L, nargs + 2);
    return;
  }

  // check that there are callbacks registered for the event
  lua_getfield(L, -1, "in_use");
  if(!lua_toboolean(L, -1))
  {
    lua_pop(L, nargs + 3);
    return;
  }

  // event exists - push the event handler, callback table,
  // event name, and extra arguments on the stack
  lua_getfield(L, -2, "on_event");
  lua_getfield(L, -3, "data");
  lua_pushstring(L, event);
  for(int i = 1; i <= nargs; i++) lua_pushvalue(L, -6 -nargs);

  // call the event handler
  dt_lua_treated_pcall(L,nargs+2,0);

  // clean up the stack
  lua_pop(L, nargs + 3);

  // redraw
  dt_lua_redraw_screen();
}

int dt_lua_event_trigger_wrapper(lua_State *L)
{
  // event name
  // extra parameters
  const char*event = luaL_checkstring(L,1);
  int nargs = lua_gettop(L) -1;
  dt_lua_event_trigger(L,event,nargs);
  return 0;
}

/*
 *
 * dt_lua_event_list is a table (array) of tables,
 * 1 for each type of event (shortcut, exit, post-image-import, etc.)
 *
 * Each event table (structure) contains the following:
 *   name -        string -       name of the event
 *   on event -    function -     function to call when event occurs
 *   on destroy -  function -     function to remove an event instance
 *   on register - function -     function to add an instance (callback)
 *   in use -      boolean -       whether any callbacks are registered
 *   data -        table (array) - callbacks to run when the event is triggered
 *   index -       table (array) - index tying names to callbacks
 *
 */

void dt_lua_event_add(lua_State *L, const char *evt_name)
{
  // 1 function to register a new event instance (callback)
  // 2 function to destroy an existing event instance
  // 3 event handler to call when event occurs

  // check we have the correct number of arguments
  int args = lua_gettop(L);

  if(args != 3)
  {
    lua_pop(L, args);
    dt_print(DT_DEBUG_LUA, "LUA ERROR : %s: wrong number of args for %s, expected 3, got %d\n", __FUNCTION__, evt_name, args);
    return;
  }

  // create a table for the new event
  lua_newtable(L);

  // name of the event
  lua_pushstring(L, evt_name);
  lua_setfield(L, -2, "name");

  // event handler
  if(lua_isfunction(L, -2))
  {
    lua_pushvalue(L, -2);
    lua_setfield(L, -2, "on_event");
  }
  else{
    dt_print(DT_DEBUG_LUA, "LUA ERROR :%s: function argument not found for on_event for event %s\n", __FUNCTION__, evt_name);
    return;
  }

  // event destruction handler
  if(lua_isfunction(L, -3))
  {
    lua_pushvalue(L, -3);
    lua_setfield(L, -2, "on_destroy");
  }
  else
  {
    dt_print(DT_DEBUG_LUA, "LUA ERROR : %s: function argument not found for on_destroy for event %s\n", __FUNCTION__, evt_name);
    return;
  }

  // event registration handler
  if(lua_isfunction(L, -4))
  {
    lua_pushvalue(L, -4);
    lua_setfield(L, -2, "on_register");

  }
  else
  {
    dt_print(DT_DEBUG_LUA, "LUA ERROR : %s: function argument not found for on_register for event %s\n", __FUNCTION__, evt_name);
    return;
  }

 // are there any events registered?
  lua_pushboolean(L, false);
  lua_setfield(L, -2, "in_use");

  // data table for containing callbacks to execute when event is triggered
  lua_newtable(L);
  lua_setfield(L, -2, "data");

  // index table for tying names to events
  lua_newtable(L);
  lua_setfield(L, -2, "index");

  // add the event to the event list
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

  lua_pop(L, 5);
}


/*
 * KEYED EVENTS
 * these are events that are triggered with a key
 * i.e they can be registered multiple time with a key parameter and only the handler with the corresponding
 * key will be triggered. there can be only one handler per key
 *
 * when registering, the fourth argument is the key
 * when triggering, the first argument is the key
 *
 * data tables is "event => {key => callback}"
 */

int dt_lua_event_keyed_register(lua_State *L)
{
  // 1 is the data table
  // 2 is the index table
  // 3 is the index name
  // 4 is the event name (checked)
  // 5 is the action to perform (checked)
  // 6 is the key itself

  // check the event type (shortcut)
  if(lua_isnoneornil(L, 6))
    return luaL_error(L, "no key provided when registering event %s", luaL_checkstring(L, 4));

  // check to make sure they key isn't already registered
  lua_getfield(L, 1, luaL_checkstring(L, 6));
  if(!lua_isnil(L, -1))
    return luaL_error(L, "key '%s' already registered for event %s ", luaL_checkstring(L, 6),
                      luaL_checkstring(L, 4));
  lua_pop(L, 1);
  // save the callback function to the data table referenced by the key name
  lua_pushvalue(L, 5);
  lua_setfield(L, 1, luaL_checkstring(L, 6));
  // save the index name, key name pair to the index table
  lua_pushvalue(L, 6);
  lua_setfield(L, 2, luaL_checkstring(L, 3));

  return 0;
}


int dt_lua_event_keyed_destroy(lua_State *L)
{
  // 1 is the data table
  // 2 is the index table
  // 3 is the index name
  // 4 is the event name (checked)

  // get the key name from the index
  lua_getfield(L, 2, luaL_checkstring(L, 3));

  // check to make sure the key was found
  if(lua_isnoneornil(L, -1))
    return luaL_error(L, "no key provided when destroying event %s", luaL_checkstring(L, 4));

  // remove the callback function from the data table using the key
  lua_pushnil(L);  // set the action to nil to remove it
  lua_setfield(L, 1, luaL_checkstring(L, -2));

  // remove the index entry
  lua_pushnil(L);  // remove the index
  lua_setfield(L, 2, luaL_checkstring(L, 3));
  return 0;
}


int dt_lua_event_keyed_trigger(lua_State *L)
{
  // 1 : the data table
  // 2 : the name of the event
  // 3 : the key
  // .. : other parameters

  // check to make sure we have a valid key and push the callback on the stack
  lua_getfield(L, 1, luaL_checkstring(L, 3));

  // if the key didn't return a callback it's an error
  if(lua_isnil(L, -1))
  {
    luaL_error(L, "event %s triggered for unregistered key %s", luaL_checkstring(L, 2),
               luaL_checkstring(L, 3));
  }

  // point the callback_marker at the callback function
  const int callback_marker = lua_gettop(L);

  // push the arguments on the stack
  for(int i = 2; i < callback_marker; i++)
  {
    lua_pushvalue(L, i);
  }

  // execute the callback
  dt_lua_treated_pcall(L,callback_marker-2,0);
  return 0;
}

/*
 * MULTIINSTANCE EVENTS
 * these events can be registered multiple time with multiple callbacks
 * all callbacks will be called in the order they were registered
 *
 * all callbacks will receive the same parameters
 * no values are returned
 *
 * data table is "event => { # => callback }
 */


int dt_lua_event_multiinstance_register(lua_State *L)
{
  // 1 is the data table
  // 2 is the index table
  // 3 is the index name
  // 4 is the event name (checked)
  // 5 is the action to perform (checked)(callback)

  // check for duplicate indexes
  for(int i = 1; i <= luaL_len(L, 2); i++)
  {
    lua_rawgeti(L, 2, i);
    if(strcmp(luaL_checkstring(L, -1), luaL_checkstring(L, 3)) == 0)
      luaL_error(L, "duplicate index name %s for event type %s\n", luaL_checkstring(L, 3), luaL_checkstring(L, 4));
    lua_pop(L, 1); // pop the string off the stack
  }

  // check that table sizes match
  if(luaL_len(L, 1) != luaL_len(L, 2))
    luaL_error(L, "index table and data table sizes differ.  %s events are corrupted.\n", luaL_checkstring(L, 4));

  // add the callback
  lua_seti(L, 1, luaL_len(L, 1) + 1); // add the callback to the data table
  lua_pop(L, 1);  // remove the event name from the stack
  /// add the index
  lua_seti(L, 2, luaL_len(L, 2) + 1); // add the index name to the index

  lua_pop(L, 2);  // clear the stack
  return 0;
}

int dt_lua_event_multiinstance_destroy(lua_State *L)
{
  // 1 is the data table
  // 2 is the index table
  // 3 is the index name
  // 4 is the event name (checked)

  // check that table sizes match
  if(luaL_len(L, 1) != luaL_len(L, 2))
    luaL_error(L, "index table and data table sizes differ.  %s events are corrupted.\n", luaL_checkstring(L, 4));

  // find the index name.  The key corresponds to the callback in the data table
  // set them both to nil to get rid of them

  int index = 0;
  for(int i = 1; i <= luaL_len(L, 2); i++)
  {
    lua_rawgeti(L, 2, i);
    if(strcmp(luaL_checkstring(L, -1), luaL_checkstring(L, 3)) == 0)
    {
      index = i;
      break;
    }
  }

  // get the size of the index table
  int size = luaL_len(L, 2);

  // if the index was found...
  if(index){
    // remove the index
    lua_pushnil(L);
    lua_rawseti(L, 1, index);

    // remove the callback
    lua_pushnil(L);
    lua_rawseti(L, 2, index);

    // if we removed an from the table that wasn't at the end
    // we have to shift all the items down to fill the table in
    if(index < size)
    {
      // start with the first entry after the one we remmoved
      for(int i = index + 1; i <= size; i++)
      {
        // move the index
        lua_rawgeti(L, 1, i);     // push the value on the stack
        lua_rawseti(L, 1, i - 1); // pull the value from the stack one place down
        lua_pushnil(L);           // remove the current entry with a nil
        lua_rawseti(L, 1, i);     // make the current one the hole

        // move the data
        lua_rawgeti(L, 2, i);     // push the value on the stack
        lua_rawseti(L, 2, i - 1); // pull the value from the stack one place down
        lua_pushnil(L);           // remove the current entry with a nil
        lua_rawseti(L, 2, i);     // make the current one the hole
      }
    }
  }

  // check that table sizes still match
  if(luaL_len(L, 1) != luaL_len(L, 2))
    luaL_error(L, "index table and data table sizes differ after event removal.  %s events are corrupted.\n", luaL_checkstring(L, 4));

  return 0;
}

int dt_lua_event_multiinstance_trigger(lua_State *L)
{
  // 1 : the data table
  // 2 : the name of the event
  // .. : other parameters

  // get the top of the argument list
  const int arg_top = lua_gettop(L);

  // step through the data table
  lua_pushnil(L);
  while(lua_next(L, 1)) // push the callback on the stack
  {
    // push the arguments on the stack
    for(int i = 2; i <= arg_top; i++)
    {
      lua_pushvalue(L, i);
    }

    // call the callback
    dt_lua_treated_pcall(L,arg_top-1,0);
  }
  return 0;
}



static int lua_register_event(lua_State *L)
{
  // 1 index name
  // 2 event name
  // 3 callback function
  // 4 key (shortcut event only)

  const char *evt_name = luaL_checkstring(L, 2);

  const int nparams = lua_gettop(L);

  // check the callback is a function
  luaL_checktype(L, 3, LUA_TFUNCTION);

  // get the event list
  lua_getfield(L, LUA_REGISTRYINDEX, "dt_lua_event_list");

  // get the table for this event and check to make sure it exists
  lua_getfield(L, -1, evt_name);
  if(lua_isnil(L, -1))
  {
    lua_pop(L, 3);
    return luaL_error(L, "unknown event type : %s\n", evt_name);
  }

  // push the event register function on the stack
  lua_getfield(L, -1, "on_register");
  // push the callback table on the stack
  lua_getfield(L, -2, "data");
  // push the index table on the stack
  lua_getfield(L, -3, "index");

  // push any arguments on the stack
  for(int i = 1; i <= nparams; i++){
    lua_pushvalue(L, i);
  }

  // call the register function
  lua_call(L, nparams + 2, 0);

  // mark the event as in use
  lua_pushboolean(L, true);
  lua_setfield(L, -2, "in_use");

  //clear the stack
  lua_pop(L, 2);
 return 0;
}

static int lua_destroy_event(lua_State *L)
{
  // 1 is the index name
  // 2 is event name

  const char *evt_name = luaL_checkstring(L, 2);

 const int nparams = lua_gettop(L);

  // get the event list
  lua_getfield(L, LUA_REGISTRYINDEX, "dt_lua_event_list");

  // get the table for this event and check to make sure it exists
  lua_getfield(L, -1, evt_name);
  if(lua_isnil(L, -1))
  {
    lua_pop(L, 2);
    return luaL_error(L, "unknown event type : %s\n", evt_name);
  }

  // push the event destroy function on the stack
  lua_getfield(L, -1, "on_destroy");
  // push the callback table on the stack
  lua_getfield(L, -2, "data");
  // push the index table on the stack
  lua_getfield(L, -3, "index");

  // push any arguments on the stack
  for(int i = 1; i <= nparams; i++){
    lua_pushvalue(L, i);
  }

   // call the register function
  lua_call(L, nparams + 2, 0);

  // check if there are any events left and set in use
  // if event type is shortcut, we have to count the tables
  // otherwise we can just take the size

  // get the index table
  lua_getfield(L, -1, "index");

  int count = 0;
  if(strcmp(evt_name, "shortcut") == 0)
  {
    // count table items
    lua_pushnil(L);           // push an empty key for lua_next
    if(lua_next(L, -2) != 0)  // get the next key, value pair and push it on the stack
    {
      count++;        // all we need is one key, value pair to determine in use
      lua_pop(L, 2);  // remove the kay, value pair from the stack
    }
  }
  else
    count = luaL_len(L, -1);  // check table size
  lua_pop(L, 1);              // pop the index table off the stack

  // push true if we have events registered otherwise false
  if(count)
    lua_pushboolean(L, true);
  else
    lua_pushboolean(L, false);

  // set in use
  lua_setfield(L, -2, "in_use");

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
  lua_pushstring(L, "destroy_event");
  lua_pushcfunction(L, &lua_destroy_event);
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
static void shortcut_callback(dt_action_t *action)
{
  dt_lua_async_call_alien(dt_lua_event_trigger_wrapper,
      0, NULL, NULL,
      LUA_ASYNC_TYPENAME,"const char*","shortcut",
      LUA_ASYNC_TYPENAME_WITH_FREE,"char*",strdup(action->label),g_cclosure_new(G_CALLBACK(&free),NULL,NULL),
      LUA_ASYNC_DONE);
}


static int register_shortcut_event(lua_State *L)
{
  // 1 is the data table
  // 2 is the index table
  // 3 is the index name
  // 4 is the event name (checked)
  // 5 is the action to perform (checked)
  // 6 is the key itself

  // duplicate the key for use elsewhere - do not free as it's in use elsewhere
  char *tmp = strdup(luaL_checkstring(L, 6));

  // register the event
  int result = dt_lua_event_keyed_register(L); // will raise an error in case of duplicate key

  // set up the accelerator path
  dt_action_register(&darktable.control->actions_lua, tmp, shortcut_callback,  0, 0);

  return result;
}

static int destroy_shortcut_event(lua_State *L)
{
  // 1 is the data table
  // 2 is the index table
  // 3 is the index name
  // 4 is the event name (checked)

  // get the key from the index
  lua_getfield(L, 2, luaL_checkstring(L, 3));

  // duplicate the key for use elsewhere - do not free, destroyed elsewhere
  char *tmp = strdup(luaL_checkstring(L, -1));

  // remove the key from the stack
  lua_pop(L, 1);

  // destroy the event
  int result = dt_lua_event_keyed_destroy(L); // will raise an error in case of duplicate key

  // remove the accelerator from the lua shortcuts
  dt_action_t *ac = dt_action_section(&darktable.control->actions_lua, tmp);
  dt_action_rename(ac, NULL);

  // free temporary buffer
  free(tmp);

  return result;
}

int dt_lua_init_events(lua_State *L)
{

  // events that don't really fit anywhere else
  lua_pushcfunction(L, register_shortcut_event);
  lua_pushcfunction(L, destroy_shortcut_event);
  lua_pushcfunction(L, dt_lua_event_keyed_trigger);
  dt_lua_event_add(L, "shortcut");

  lua_pushcfunction(L, dt_lua_event_multiinstance_register);
  lua_pushcfunction(L, dt_lua_event_multiinstance_destroy);
  lua_pushcfunction(L, dt_lua_event_multiinstance_trigger);
  dt_lua_event_add(L, "intermediate-export-image");

  lua_pushcfunction(L, dt_lua_event_multiinstance_register);
  lua_pushcfunction(L, dt_lua_event_multiinstance_destroy);
  lua_pushcfunction(L, dt_lua_event_multiinstance_trigger);
  dt_lua_event_add(L,"pre-import");

  lua_pushcfunction(L, dt_lua_event_multiinstance_register);
  lua_pushcfunction(L, dt_lua_event_multiinstance_destroy);
  lua_pushcfunction(L, dt_lua_event_multiinstance_trigger);
  dt_lua_event_add(L,"selection-changed");

  lua_pushcfunction(L, dt_lua_event_multiinstance_register);
  lua_pushcfunction(L, dt_lua_event_multiinstance_destroy);
  lua_pushcfunction(L, dt_lua_event_multiinstance_trigger);
  dt_lua_event_add(L, "darkroom-image-history-changed");

  lua_pushcfunction(L, dt_lua_event_multiinstance_register);
  lua_pushcfunction(L, dt_lua_event_multiinstance_destroy);
  lua_pushcfunction(L, dt_lua_event_multiinstance_trigger);
  dt_lua_event_add(L, "pixelpipe-processing-complete");

  return 0;
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
