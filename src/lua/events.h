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
#ifndef DT_LUA_EVENTS_H
#define DT_LUA_EVENTS_H
#include <lualib.h>
#include <lua.h>
#include <lauxlib.h>


/**
  Add a new event to the lua API
* evt_name : the id to use for the event
EXPECTED STACK
* -2 : closure to be called on darktable.register_event
  in charge of saving the info that will be needed when the event triggers
  including the actual callbacks to use
  * can raise lua_error
  * 1  : a table to save info for the event (including the callback to user
  * 2  : event name
  * 3  : callback to use
  * 4+ : extra parameters given in register_event
  * should not return anything
* -1 : closure to be called when event is triggered,
  in charge of using the data provided by the register function to call the callbacks
  * 1 : the table filled by the registration function
  * ... : extra parameters passed when the event was triggered
*/
void dt_lua_event_add(lua_State *L, const char *evt_name);


/**
  Trigger an event that has been previously added
  * event : the id the the event was registered under
  * nargs : the number of significant items on the stack.
    these items will be passed as extra parameters to the event's callback
  */
void dt_lua_event_trigger(lua_State *L, const char *event, int nargs);


/////////////////////
//    HELPERS      //
/////////////////////
// these pairs of functions can usually be used "as is" for registering events


/**
  MULTIINSTANCE EVENT
  an event that can be registered (by the lua side) multiple times.
  All callbacks will be called when the event is triggered

  the register function does not expect any extra parameter from the lua side

  the trigger will pass the arguments as is to the lua callback

  */
int dt_lua_event_multiinstance_register(lua_State *L);
int dt_lua_event_multiinstance_trigger(lua_State *L);


/**
  KEYED EVENT
  an event that is registered with a string key.
  It can be registered only once per key.

  the register function wants one extra parameter from lua : the key to register with

  the trigger function wants the bottom most arg to be the key to trigger
  */
int dt_lua_event_keyed_register(lua_State *L);
int dt_lua_event_keyed_trigger(lua_State *L);



/**
  initialize events, called at DT start
  */
int dt_lua_init_early_events(lua_State *L);
int dt_lua_init_events(lua_State *L);
#endif
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
