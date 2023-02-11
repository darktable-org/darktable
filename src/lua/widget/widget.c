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

#include "control/control.h"
#include "lua/call.h"
#include "lua/modules.h"
#include "lua/types.h"
#include "lua/widget/common.h"
#include "stdarg.h"

/**
  TODO
  use name to save/restore states as pref like other widgets
  have a way to save presets
  luastorage can't save presets
dt_ui_section_label : make new lua widget - Done
widget names : implement for CSS ? - Done
  */

#pragma GCC diagnostic ignored "-Wshadow"

dt_lua_widget_type_t widget_type = {
  .name = "widget",
  .gui_init = NULL,
  .gui_cleanup = NULL,
  .alloc_size = sizeof(dt_lua_widget_t),
  .parent = NULL
};


static void cleanup_widget_sub(lua_State *L, dt_lua_widget_type_t *widget_type, lua_widget widget);
static void cleanup_widget_sub(lua_State *L, dt_lua_widget_type_t *widget_type, lua_widget widget) {
  if(widget_type->parent)
    cleanup_widget_sub(L, widget_type->parent, widget);
  if(widget_type->gui_cleanup) {
    widget_type->gui_cleanup(L, widget);
  }
}

static void init_widget_sub(lua_State *L, dt_lua_widget_type_t *widget_type);
static void init_widget_sub(lua_State *L, dt_lua_widget_type_t *widget_type) {
  if(widget_type->parent)
    init_widget_sub(L, widget_type->parent);
  if(widget_type->gui_init)
    widget_type->gui_init(L);
}

static void on_destroy(GtkWidget *widget, gpointer user_data)
{
}

static gboolean on_destroy_wrapper(gpointer user_data)
{
  gtk_widget_destroy((GtkWidget*) user_data);
  return false;
}

static int widget_gc(lua_State *L)
{
  lua_widget lwidget;
  luaA_to(L, lua_widget, &lwidget, 1);
  if(!lwidget) return 0; // object has been destroyed
  if(gtk_widget_get_parent(lwidget->widget)) {
    luaL_error(L, "Destroying a widget which is still parented, this should never happen (%s at %p)\n", lwidget->type->name, lwidget);
  }
  cleanup_widget_sub(L, lwidget->type, lwidget);
  dt_lua_widget_unbind(L, lwidget);
  // no need to drop, the pointer table is weak and the widget is already being GC, so it's not in the table anymore
  //dt_lua_type_gpointer_drop(L,lwidget);
  //dt_lua_type_gpointer_drop(L,lwidget->widget);
  g_idle_add(on_destroy_wrapper, lwidget->widget);
  free(lwidget);
  return 0;
}

static int get_widget_params(lua_State *L)
{
  struct dt_lua_widget_type_t *widget_type = lua_touserdata(L, lua_upvalueindex(1));
  if(G_TYPE_IS_ABSTRACT(widget_type->gtk_type)){
    luaL_error(L, "Trying to create a widget of an abstract type : %s\n", widget_type->name);
  }
  lua_widget widget= malloc(widget_type->alloc_size);
  widget->widget = gtk_widget_new(widget_type->gtk_type, NULL);
  gtk_widget_show(widget->widget);// widgets are invisible by default
  g_object_ref_sink(widget->widget);
  widget->type = widget_type;
  luaA_push_type(L, widget_type->associated_type, &widget);
  dt_lua_type_gpointer_alias_type(L, widget_type->associated_type, widget, widget->widget);
  init_widget_sub(L, widget_type);

  luaL_getmetafield(L, -1, "__gtk_signals");
  lua_pushnil(L); /* first key */
  while(lua_next(L, -2) != 0)
  {
    g_signal_connect(widget->widget, lua_tostring(L,-2), G_CALLBACK(lua_touserdata(L,-1)), widget);
    lua_pop(L,1);
  }
  lua_pop(L,1);
  g_signal_connect(widget->widget, "destroy", G_CALLBACK(on_destroy), widget);
  return 1;
}



luaA_Type dt_lua_init_widget_type_type(lua_State *L, dt_lua_widget_type_t *widget_type,const char *lua_type, GType gtk_type)
{
  luaA_Type type_id = dt_lua_init_gpointer_type_type(L, luaA_type_add(L, lua_type, sizeof(gpointer)));
  widget_type->associated_type = type_id;
  widget_type->gtk_type = gtk_type;
  dt_lua_type_register_parent_type(L, type_id, widget_type->parent->associated_type);

  lua_newtable(L);
  dt_lua_type_setmetafield_type(L, type_id, "__gtk_signals");
  // add to the table
  lua_pushlightuserdata(L, widget_type);
  lua_pushcclosure(L, get_widget_params, 1);
  dt_lua_gtk_wrap(L);
  dt_lua_module_entry_new(L, -1, "widget", widget_type->name);
  lua_pop(L, 1);
  return type_id;
};



static int new_widget(lua_State *L)
{
  const char *entry_name = luaL_checkstring(L, 1);
  dt_lua_module_entry_push(L, "widget", entry_name);
  lua_insert(L, 2);
  lua_call(L, lua_gettop(L) - 2, 1);
  return 1;
}

void dt_lua_widget_set_callback(lua_State *L, int index, const char *name)
{
  luaL_argcheck(L, dt_lua_isa(L, index, lua_widget), index, "lua_widget expected");
  luaL_checktype(L, -1, LUA_TFUNCTION);
  lua_getiuservalue(L, index, 1);
  lua_pushvalue(L, -2);
  lua_setfield(L, -2, name);
  lua_pop(L, 2);
}

void dt_lua_widget_get_callback(lua_State *L, int index, const char *name)
{
  luaL_argcheck(L, dt_lua_isa(L, index, lua_widget), index, "lua_widget expected");
  lua_getiuservalue(L, index, 1);
  lua_getfield(L, -1, name);
  lua_remove(L, -2);
}

int dt_lua_widget_trigger_callback(lua_State *L)
{
  int nargs = lua_gettop(L) - 2;
  lua_widget widget;
  luaA_to(L, lua_widget, &widget, 1);
  const char* name = lua_tostring(L, 2);
  lua_getiuservalue(L, 1, 1);
  lua_getfield(L, -1, name);
  if(!lua_isnil(L, -1)) {
    lua_pushvalue(L, 1);
    for(int i = 0 ; i < nargs ; i++) {
      lua_pushvalue(L, i + 3);
    }
    dt_lua_treated_pcall(L, nargs + 1, 0);
    dt_lua_redraw_screen();
  }
  return 0;
}

static int reset_member(lua_State *L)
{
  if(lua_gettop(L) > 2) {
    dt_lua_widget_set_callback(L, 1, "reset");
    return 0;
  }
  dt_lua_widget_get_callback(L, 1, "reset");
  return 1;
}


static int tooltip_member(lua_State *L)
{
  lua_widget widget;
  luaA_to(L, lua_widget, &widget, 1);
  if(lua_gettop(L) > 2) {
    if(lua_isnil(L, 3)) {
      gtk_widget_set_tooltip_text(widget->widget, NULL);
    } else {
      const char * text = luaL_checkstring(L, 3);
      gtk_widget_set_tooltip_text(widget->widget, text);
    }
    return 0;
  }
  char* result = gtk_widget_get_tooltip_text(widget->widget);
  lua_pushstring(L, result);
  free(result);
  return 1;
}

static int name_member(lua_State *L)
{
  lua_widget widget;
  luaA_to(L, lua_widget, &widget, 1);
  if(lua_gettop(L) > 2) {
    if(lua_isnil(L, 3)) {
      gtk_widget_set_name(widget->widget, NULL);
    } else {
      const gchar * text = luaL_checkstring(L, 3);
      gtk_widget_set_name(widget->widget, text);
    }
    return 0;
  }
  const gchar* result = gtk_widget_get_name(widget->widget);
  lua_pushstring(L, result);
  return 1;
}

static int visible_member(lua_State *L)
{
  lua_widget widget;
  gboolean value;
  luaA_to(L, lua_widget, &widget, 1);
  if(lua_gettop(L) > 2) {
    value = lua_toboolean(L, 3);
    //gtk_widget_set_visible(widget->widget, value);
    if(value)
    {
      gtk_widget_show(widget->widget);
      // enable gtk_widget_show_all() in case it was disabled by
      // setting a widget to hidden
      gtk_widget_set_no_show_all(widget->widget, FALSE);
    }
    else
    {
      gtk_widget_hide(widget->widget);
      // dont let gtk_widget_show_all() unhide the widget
      gtk_widget_set_no_show_all(widget->widget, TRUE);
    }
  }
  value = gtk_widget_get_visible(widget->widget);
  lua_pushboolean(L, value);
  return 1;
}

static int sensitive_member(lua_State *L)
{
  lua_widget widget;
  luaA_to(L, lua_widget, &widget, 1);
  if(lua_gettop(L) > 2) {
    gboolean value = lua_toboolean(L, 3);
    gtk_widget_set_sensitive(widget->widget, value);
    return 0;
  }
  gboolean result = gtk_widget_get_sensitive(widget->widget);
  lua_pushboolean(L, result);
  return 1;
}

int dt_lua_widget_tostring_member(lua_State *L)
{
  lua_widget widget;
  luaA_to(L, lua_widget, &widget, 1);
  lua_pushstring(L, G_OBJECT_TYPE_NAME(widget->widget));
  return 1;
}

static int gtk_signal_member(lua_State *L)
{

  const char *signal = lua_tostring(L, lua_upvalueindex(1));
  if(lua_gettop(L) > 2) {
    dt_lua_widget_set_callback(L, 1, signal);
    return 0;
  }
  dt_lua_widget_get_callback(L, 1, signal);
  return 1;
}

void dt_lua_widget_register_gtk_callback_type(lua_State *L, luaA_Type type_id, const char *signal_name, const char *lua_name, GCallback callback)
{
  lua_pushstring(L, signal_name);
  lua_pushcclosure(L, gtk_signal_member, 1);
  dt_lua_type_register_type(L, type_id, lua_name);

  luaL_newmetatable(L, luaA_typename(L, type_id));
  lua_getfield(L, -1, "__gtk_signals");
  lua_pushlightuserdata(L, callback);
  lua_setfield(L, -2, signal_name);
  lua_pop(L, 2);

}

int widget_call(lua_State *L)
{
  lua_pushnil(L); /* first key */
  while(lua_next(L, 2) != 0)
  {
    lua_pushvalue(L, -2);
    lua_pushvalue(L, -2);
    lua_settable(L, 1);
    lua_pop(L, 1);
  }
  lua_pop(L, 1);
  return 1;
}

void dt_lua_widget_bind(lua_State *L, lua_widget widget)
{
  /* check that widget isn't already parented */
  if(gtk_widget_get_parent (widget->widget) != NULL) {
    luaL_error(L, "Attempting to bind a widget which already has a parent\n");
  }

  /* store it as a toplevel widget */
  lua_getfield(L, LUA_REGISTRYINDEX, "dt_lua_widget_bind_table");
  lua_pushlightuserdata(L, widget);
  luaA_push(L, lua_widget, &widget);
  lua_settable(L, -3);
  lua_pop(L, 1);
}

void dt_lua_widget_unbind(lua_State *L, lua_widget widget)
{
  lua_getfield(L, LUA_REGISTRYINDEX, "dt_lua_widget_bind_table");
  lua_pushlightuserdata(L, widget);
  lua_pushnil(L);
  lua_settable(L, -3);
  lua_pop(L, 1);
}


int dt_lua_init_widget(lua_State* L)
{

  lua_newtable(L);
  lua_setfield(L, LUA_REGISTRYINDEX, "dt_lua_widget_bind_table");

  dt_lua_module_new(L, "widget");

  widget_type.associated_type = dt_lua_init_gpointer_type(L, lua_widget);
  lua_pushcfunction(L, tooltip_member);
  dt_lua_gtk_wrap(L);
  dt_lua_type_register(L, lua_widget, "tooltip");
  lua_pushcfunction(L, name_member);
  dt_lua_gtk_wrap(L);
  dt_lua_type_register(L, lua_widget, "name");
  lua_pushcfunction(L, visible_member);
  dt_lua_gtk_wrap(L);
  dt_lua_type_register(L, lua_widget, "visible");
  lua_pushcfunction(L, widget_gc);
  dt_lua_gtk_wrap(L);
  dt_lua_type_setmetafield(L, lua_widget, "__gc");
  lua_pushcfunction(L, reset_member);
  dt_lua_type_register(L, lua_widget, "reset_callback");
  lua_pushcfunction(L, widget_call);
  dt_lua_type_setmetafield(L, lua_widget, "__call");
  lua_pushcfunction(L, sensitive_member);
  dt_lua_gtk_wrap(L);
  dt_lua_type_register(L, lua_widget, "sensitive");
  lua_pushcfunction(L, dt_lua_widget_tostring_member);
  dt_lua_gtk_wrap(L);
  dt_lua_type_setmetafield(L, lua_widget, "__tostring");

  dt_lua_init_widget_container(L);

  dt_lua_init_widget_box(L);
  dt_lua_init_widget_button(L);
  dt_lua_init_widget_check_button(L);
  dt_lua_init_widget_combobox(L);
  dt_lua_init_widget_label(L);
  dt_lua_init_widget_section_label(L);
  dt_lua_init_widget_entry(L);
  dt_lua_init_widget_file_chooser_button(L);
  dt_lua_init_widget_separator(L);
  dt_lua_init_widget_slider(L);
  dt_lua_init_widget_stack(L);
  dt_lua_init_widget_text_view(L);

  dt_lua_push_darktable_lib(L);
  lua_pushstring(L, "new_widget");
  lua_pushcfunction(L, &new_widget);
  lua_settable(L, -3);
  lua_pop(L, 1);
  return 0;
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
