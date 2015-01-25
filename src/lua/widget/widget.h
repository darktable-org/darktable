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
  void (*gui_init)(lua_State *L);
  void (*gui_cleanup)(lua_State *L, lua_widget widget);
  const char * name;
  // private, do not override
  luaA_Type associated_type;
} dt_lua_widget_type_t;


// Types added to the lua type system and useable externally
typedef GtkOrientation dt_lua_orientation_t;

/** pop a function from the top of the stack, 
    register as a callback named "name" for the object (not type) at index index
    */
void dt_lua_widget_set_callback(lua_State *L,int index,const char* name);
/** push the callback for name "name" on the stack, or nil if not available */
void dt_lua_widget_get_callback(lua_State *L,int index,const char* name);
/** triggers a callback for the object, 
    the callback always happen in a secondary thread with the object as unique parameter
    gpointer is the pointer to the object.
    object_tyep is the type of the lua type of the gpointer above

    the async function can be called without the lua lock and from the gtk main thread (that's the whole point)
 */
void dt_lua_widget_trigger_callback(lua_State*L,lua_widget object,const char* name);
void dt_lua_widget_trigger_callback_async(lua_widget object,const char* name);

/* wrapper to automatically implement a callback on a GTK signal */
#define dt_lua_widget_register_gtk_callback(L,widget_type,signal_name,lua_name,callback) \
  dt_lua_widget_register_gtk_callback_type(L,luaA_type_find(L, #widget_type),signal_name,lua_name,callback)
void dt_lua_widget_register_gtk_callback_type(lua_State *L,luaA_Type type_id,const char* signal_name, const char* lua_name,GCallback callback); 


#define dt_lua_init_widget_type(L, widget_type,lua_type)  \
  dt_lua_init_widget_type_type(L, widget_type, #lua_type)
luaA_Type dt_lua_init_widget_type_type(lua_State *L, dt_lua_widget_type_t* widget_type,const char* lua_type);


int dt_lua_init_widget(lua_State *L);
#endif
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
