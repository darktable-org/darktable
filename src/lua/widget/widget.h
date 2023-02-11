/*
   This file is part of darktable,
   Copyright (C) 2015-2021 darktable developers.

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

#pragma once

#include "lua/call.h"
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
  gboolean visible;
  size_t  alloc_size;
  struct dt_lua_widget_type_t *parent;
  // private, do not override
  luaA_Type associated_type;
  GType gtk_type;
} dt_lua_widget_type_t;

extern dt_lua_widget_type_t widget_type;



/** pop a function from the top of the stack,
    register as a callback named "name" for the object (not type) at index index
    */
void dt_lua_widget_set_callback(lua_State *L,int index,const char* name);
/** push the callback for name "name" on the stack, or nil if not available */
void dt_lua_widget_get_callback(lua_State *L,int index,const char* name);
/** triggers a callback for the object,
  * first param : the lua_storage to trigger
  * second param : the name of the event to fire
  * other params : passed to the callback
  returns nothing, might raise exceptions

  this function is meant to be called via dt_lua_async_call if needed

 */
int dt_lua_widget_trigger_callback(lua_State *L);

/* wrapper to automatically implement a callback on a GTK signal */
#define dt_lua_widget_register_gtk_callback(L,widget_type,signal_name,lua_name,callback) \
  dt_lua_widget_register_gtk_callback_type(L,luaA_type_find(L, #widget_type),signal_name,lua_name,callback)
void dt_lua_widget_register_gtk_callback_type(lua_State *L,luaA_Type type_id,const char* signal_name, const char* lua_name,GCallback callback);


#define dt_lua_init_widget_type(L, widget_type,lua_type,gtk_type)  \
  dt_lua_init_widget_type_type(L, widget_type, #lua_type,gtk_type)
luaA_Type dt_lua_init_widget_type_type(lua_State *L, dt_lua_widget_type_t* widget_type,const char* lua_type,GType gtk_type);

/**
  Bind a lua widget, i.e prevent it from being destroyed by the lua GC.
  after that, the lua object is guaranteed to exist until it is unbound or
  the associated GtkWidget is destroyed

  You want to call that on widget you add to the UI so they stay alive.
  */
void dt_lua_widget_bind(lua_State *L, lua_widget widget);
void dt_lua_widget_unbind(lua_State *L, lua_widget widget);


int dt_lua_init_widget(lua_State *L);

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
