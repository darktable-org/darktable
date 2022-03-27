/*
   This file is part of darktable,
   Copyright (C) 2015-2020 darktable developers.

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
#include "gui/gtk.h"
#include "lua/types.h"
#include "lua/widget/common.h"

static void entry_init(lua_State* L);
static void entry_cleanup(lua_State* L,lua_widget widget);
static dt_lua_widget_type_t entry_type = {
  .name = "entry",
  .gui_init = entry_init,
  .gui_cleanup = entry_cleanup,
  .alloc_size = sizeof(dt_lua_widget_t),
  .parent= &widget_type
};


static void entry_init(lua_State* L)
{
  lua_entry entry;
  luaA_to(L,lua_entry,&entry,1);
}

static void entry_cleanup(lua_State* L,lua_widget widget)
{
}


static int text_member(lua_State *L)
{
  lua_entry entry;
  luaA_to(L,lua_entry,&entry,1);
  if(lua_gettop(L) > 2) {
    const char * text = luaL_checkstring(L,3);
    gtk_entry_set_text(GTK_ENTRY(entry->widget),text);
    return 0;
  }
  lua_pushstring(L,gtk_entry_get_text(GTK_ENTRY(entry->widget)));
  return 1;
}

static int is_password_member(lua_State *L)
{
  lua_entry entry;
  luaA_to(L,lua_entry,&entry,1);
  if(lua_gettop(L) > 2) {
    const gboolean visibility = lua_toboolean(L,3);
    gtk_entry_set_visibility(GTK_ENTRY(entry->widget),!visibility);
    return 0;
  }
  lua_pushboolean(L,gtk_entry_get_visibility(GTK_ENTRY(entry->widget)));
  return 1;
}

static int placeholder_member(lua_State *L)
{
  lua_entry entry;
  luaA_to(L,lua_entry,&entry,1);
  if(lua_gettop(L) > 2) {
    const char * placeholder = luaL_checkstring(L,3);
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry->widget),placeholder);
    return 0;
  }
  lua_pushstring(L,gtk_entry_get_placeholder_text(GTK_ENTRY(entry->widget)));
  return 1;
}

static int editable_member(lua_State *L)
{
  lua_entry entry;
  luaA_to(L,lua_entry,&entry,1);
  gboolean editable;
  if(lua_gettop(L) > 2) {
    editable = lua_toboolean(L,3);
    g_object_set(G_OBJECT(entry->widget), "editable", editable, (gchar *)0);
    return 0;
  }
  g_object_get(G_OBJECT(entry->widget),"editable",&editable,NULL);
  lua_pushboolean(L,editable);
  return 1;
}

static int tostring_member(lua_State *L)
{
  lua_entry widget;
  luaA_to(L, lua_entry, &widget, 1);
  const gchar *text = gtk_entry_get_text(GTK_ENTRY(widget->widget));
  gchar *res = g_strdup_printf("%s (\"%s\")", G_OBJECT_TYPE_NAME(widget->widget), text ? text : "");
  lua_pushstring(L, res);
  g_free(res);
  return 1;
}

int dt_lua_init_widget_entry(lua_State* L)
{
  dt_lua_init_widget_type(L,&entry_type,lua_entry,GTK_TYPE_ENTRY);

  lua_pushcfunction(L, tostring_member);
  dt_lua_gtk_wrap(L);
  dt_lua_type_setmetafield(L, lua_entry, "__tostring");

  lua_pushcfunction(L,text_member);
  dt_lua_gtk_wrap(L);
  dt_lua_type_register(L, lua_entry, "text");

  lua_pushcfunction(L,is_password_member);
  dt_lua_gtk_wrap(L);
  dt_lua_type_register(L, lua_entry, "is_password");

  lua_pushcfunction(L,placeholder_member);
  dt_lua_gtk_wrap(L);
  dt_lua_type_register(L, lua_entry, "placeholder");

  lua_pushcfunction(L,editable_member);
  dt_lua_gtk_wrap(L);
  dt_lua_type_register(L, lua_entry, "editable");

  return 0;
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

