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
#include "lua/widget/widget.h"
#include "lua/types.h"
#include "gui/gtk.h"

typedef struct {
  dt_lua_widget_t parent;
} dt_lua_entry_t;

typedef dt_lua_entry_t* lua_entry;

static void entry_init(lua_State* L);
static void entry_cleanup(lua_State* L,lua_widget widget);
static dt_lua_widget_type_t entry_type = {
  .name = "entry",
  .gui_init = entry_init,
  .gui_cleanup = entry_cleanup,
};


static void entry_init(lua_State* L)
{
  lua_entry entry = malloc(sizeof(dt_lua_entry_t));
	entry->parent.widget = gtk_entry_new();
  dt_gui_key_accel_block_on_focus_connect(GTK_WIDGET(entry->parent.widget));
  entry->parent.type = &entry_type;
  luaA_push_type(L, entry_type.associated_type, &entry);
  g_object_ref_sink(entry->parent.widget);
}

static void entry_cleanup(lua_State* L,lua_widget widget)
{
  dt_gui_key_accel_block_on_focus_disconnect(widget->widget);
}


static int text_member(lua_State *L)
{
  lua_entry entry;
  luaA_to(L,lua_entry,&entry,1);
  if(lua_gettop(L) > 2) {
    const char * text = luaL_checkstring(L,3);
    gtk_entry_set_text(GTK_ENTRY(entry->parent.widget),text);
    return 0;
  }
  lua_pushstring(L,gtk_entry_get_text(GTK_ENTRY(entry->parent.widget)));
  return 1;
}

static int is_password_member(lua_State *L)
{
  lua_entry entry;
  luaA_to(L,lua_entry,&entry,1);
  if(lua_gettop(L) > 2) {
    const gboolean visibility = lua_toboolean(L,3);
    gtk_entry_set_visibility(GTK_ENTRY(entry->parent.widget),visibility);
    return 0;
  }
  lua_pushboolean(L,gtk_entry_get_visibility(GTK_ENTRY(entry->parent.widget)));
  return 1;
}

static int placeholder_member(lua_State *L)
{
  lua_entry entry;
  luaA_to(L,lua_entry,&entry,1);
  if(lua_gettop(L) > 2) {
    const char * placeholder = luaL_checkstring(L,3);
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry->parent.widget),placeholder);
    return 0;
  }
  lua_pushstring(L,gtk_entry_get_placeholder_text(GTK_ENTRY(entry->parent.widget)));
  return 1;
}

static int editable_member(lua_State *L)
{
  lua_entry entry;
  luaA_to(L,lua_entry,&entry,1);
  gboolean editable;
  if(lua_gettop(L) > 2) {
    editable = lua_toboolean(L,3);
    g_object_set(G_OBJECT(entry->parent.widget),"editable",editable,NULL);
    return 0;
  }
  g_object_get(G_OBJECT(entry->parent.widget),"editable",&editable,NULL);
  lua_pushboolean(L,editable);
  return 1;
}

int dt_lua_init_widget_entry(lua_State* L)
{
  dt_lua_init_widget_type(L,&entry_type,lua_entry);

  lua_pushcfunction(L,text_member);
  lua_pushcclosure(L,dt_lua_gtk_wrap,1);
  dt_lua_type_register(L, lua_entry, "text");

  lua_pushcfunction(L,is_password_member);
  lua_pushcclosure(L,dt_lua_gtk_wrap,1);
  dt_lua_type_register(L, lua_entry, "is_password");

  lua_pushcfunction(L,placeholder_member);
  lua_pushcclosure(L,dt_lua_gtk_wrap,1);
  dt_lua_type_register(L, lua_entry, "placeholder");

  lua_pushcfunction(L,editable_member);
  lua_pushcclosure(L,dt_lua_gtk_wrap,1);
  dt_lua_type_register(L, lua_entry, "editable");

  return 0;
}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
