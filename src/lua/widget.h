/*
   This file is part of darktable,
   copyright (c) 2015 Jeremy Rosen

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
#ifndef LUA_WIDGET_H
#define LUA_WIDGET_H
#include "lua/lua.h"
#include <gtk/gtk.h>
struct dt_lua_widget_type_t;
typedef struct {
  GtkWidget *widget;
  struct dt_lua_widget_type_t* type;
} dt_lua_widget_t;

typedef dt_lua_widget_t* lua_widget;

typedef struct dt_lua_widget_type_t{
  lua_widget (*gui_init)(lua_State *L);
  void (*gui_reset)(lua_widget widget);
  void (*gui_cleanup)(lua_State *L, lua_widget widget);
  const char * name;
  // private, do not override
  luaA_Type associated_type;
} dt_lua_widget_type_t;


/** pop a function from the top of the stack, 
    register as a callback named "name" for the object (not type) at index index
    */
void dt_lua_widget_setcallback(lua_State *L,int index,const char* name);
/** push the callback for name "name" on the stack, or nil if not available */
void dt_lua_widget_getcallback(lua_State *L,int index,const char* name);
/** triggers a callback for the object, 
    the callback always happen in a secondary thread with the object as unique parameter
    gpointer is the pointer to the object.
    object_tyep is the type of the lua type of the gpointer above

    this function can be called without the lua lock and from the gtk main thread (that's the whole point)
 */
void dt_lua_widget_trigger_callback(gpointer object,luaA_Type object_type,const char* name);


#define dt_lua_register_widget_type(L, widget_type, type_name)  \
  dt_lua_register_widget_type_type(L, widget_type, luaA_type_find(L,#type_name))
void dt_lua_register_widget_type_type(lua_State *L, dt_lua_widget_type_t* widget_type, luaA_Type type_id);


int dt_lua_init_widget(lua_State *L);
#endif
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
