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
#include "bauhaus/bauhaus.h"
#include "gui/gtk.h"
#include "lua/types.h"
#include "lua/widget/common.h"

static void combobox_init(lua_State *L);
static dt_lua_widget_type_t combobox_type = {
  .name = "combobox",
  .gui_init = combobox_init,
  .gui_cleanup = NULL,
  .alloc_size = sizeof(dt_lua_widget_t),
  .parent= &widget_type
};


static void combobox_init(lua_State*L)
{
  lua_combobox combobox;
  luaA_to(L,lua_combobox,&combobox,-1);
  dt_bauhaus_combobox_from_widget(DT_BAUHAUS_WIDGET(combobox->widget),NULL);

}

static int combobox_len(lua_State*L)
{
  lua_combobox combobox;
  luaA_to(L,lua_combobox,&combobox,1);
  lua_pushinteger(L,dt_bauhaus_combobox_length(combobox->widget));
  return 1;
}

static int combobox_numindex(lua_State*L)
{
  lua_combobox combobox;
  luaA_to(L,lua_combobox,&combobox,1);
  int key = lua_tointeger(L,2);
  int length = dt_bauhaus_combobox_length(combobox->widget);
  if(lua_gettop(L) > 2) {
    if(key <= 0 || key > length +1) {
      return luaL_error(L,"Invalid index for combobox : %d\n",key);
    } else if(key == length +1) {
      const char * string = luaL_checkstring(L,3);
      dt_bauhaus_combobox_add(combobox->widget,string);
    } else if(lua_isnil(L,3)){
      dt_bauhaus_combobox_remove_at(combobox->widget,key-1);
    } else {
      const char * string = luaL_checkstring(L,3);
      dt_bauhaus_combobox_remove_at(combobox->widget,key-1);
      dt_bauhaus_combobox_insert(combobox->widget,string,key-1);
    }
    return 0;
  }
  if(key <= 0 || key > length)
  {
    lua_pushnil(L);
    return 1;
  }
  lua_pushstring(L, dt_bauhaus_combobox_get_entry(combobox->widget, key - 1));
  return 1;
}

static int label_member(lua_State *L)
{
  lua_combobox combobox;
  luaA_to(L,lua_combobox,&combobox,1);
  if(lua_gettop(L) > 2) {
    char tmp[256];
    luaA_to(L,char_256,&tmp,3);
    dt_bauhaus_widget_set_label(combobox->widget,NULL,tmp);
    return 0;
  }
  lua_pushstring(L,dt_bauhaus_widget_get_label(combobox->widget));
  return 1;
}

static int editable_member(lua_State *L)
{
  lua_combobox combobox;
  luaA_to(L,lua_combobox,&combobox,1);
  if(lua_gettop(L) > 2) {
    gboolean editable = lua_toboolean(L,3);
    dt_bauhaus_combobox_set_editable(combobox->widget,editable);
    return 0;
  }
  lua_pushboolean(L,dt_bauhaus_combobox_get_editable(combobox->widget));
  return 1;
}

static int value_member(lua_State*L)
{
  lua_combobox combobox;
  luaA_to(L,lua_combobox,&combobox,1);
  int length = dt_bauhaus_combobox_length(combobox->widget);
  if(lua_gettop(L) > 2) {
    if(lua_isnil(L,3)) {
      dt_bauhaus_combobox_set(combobox->widget,-1);
    } else if(lua_isnumber(L,3)) {
      int index = lua_tointeger(L,3) ;
      if(index < 1 || index > length) {
        return luaL_error(L,"Invalid index for combo box : %d\n",index);
      }
      dt_bauhaus_combobox_set(combobox->widget,index -1);
    } else if(lua_isstring(L,3)&& dt_bauhaus_combobox_get_editable(combobox->widget)) {
      const char * string = lua_tostring(L,3);
      dt_bauhaus_combobox_set_text(combobox->widget,string);
    } else {
      return luaL_error(L,"Invalid type for combo box value\n");
    }
    return 0;
  }
  lua_pushstring(L,dt_bauhaus_combobox_get_text(combobox->widget));
  return 1;
}

static int selected_member(lua_State*L)
{
  lua_combobox combobox;
  luaA_to(L,lua_combobox,&combobox,1);
  int length = dt_bauhaus_combobox_length(combobox->widget);
  if(lua_gettop(L) > 2) {
    if(lua_isnil(L,3)) {
      dt_bauhaus_combobox_set(combobox->widget,-1);
    } else if(lua_isnumber(L,3)) {
      int index = lua_tointeger(L,3) ;
      if(index < 0 || index > length) {
        return luaL_error(L,"Invalid index for combo box : %d\n",index);
      }
      dt_bauhaus_combobox_set(combobox->widget,index -1);
    } else {
      return luaL_error(L,"Invalid type for combo box selected\n");
    }
    return 0;
  }
  lua_pushinteger(L,dt_bauhaus_combobox_get(combobox->widget) + 1);
  return 1;
}

static void changed_callback(GtkButton *widget, gpointer user_data)
{
  dt_lua_async_call_alien(dt_lua_widget_trigger_callback,
      0,NULL,NULL,
      LUA_ASYNC_TYPENAME,"lua_widget",user_data,
      LUA_ASYNC_TYPENAME,"const char*","value-changed",
      LUA_ASYNC_DONE);
}

static int tostring_member(lua_State *L)
{
  lua_combobox widget;
  luaA_to(L, lua_combobox, &widget, 1);
  const gchar *text = dt_bauhaus_widget_get_label(widget->widget);
  gchar *res = g_strdup_printf("%s (\"%s\")", G_OBJECT_TYPE_NAME(widget->widget), text ? text : "");
  lua_pushstring(L, res);
  g_free(res);
  return 1;
}

int dt_lua_init_widget_combobox(lua_State* L)
{
  dt_lua_init_widget_type(L,&combobox_type,lua_combobox,DT_BAUHAUS_WIDGET_TYPE);

  lua_pushcfunction(L, tostring_member);
  dt_lua_gtk_wrap(L);
  dt_lua_type_setmetafield(L, lua_combobox, "__tostring");
  lua_pushcfunction(L,combobox_len);
  dt_lua_gtk_wrap(L);
  lua_pushcfunction(L,combobox_numindex);
  dt_lua_gtk_wrap(L);
  dt_lua_type_register_number(L,lua_combobox);
  lua_pushcfunction(L,value_member);
  dt_lua_gtk_wrap(L);
  dt_lua_type_register(L, lua_combobox, "value");
  lua_pushcfunction(L,selected_member);
  dt_lua_gtk_wrap(L);
  dt_lua_type_register(L, lua_combobox, "selected");
  dt_lua_widget_register_gtk_callback(L,lua_combobox,"value-changed","changed_callback",G_CALLBACK(changed_callback));
  lua_pushcfunction(L,label_member);
  dt_lua_gtk_wrap(L);
  dt_lua_type_register(L, lua_combobox, "label");

  lua_pushcfunction(L,editable_member);
  dt_lua_gtk_wrap(L);
  dt_lua_type_register(L, lua_combobox, "editable");

  return 0;
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

