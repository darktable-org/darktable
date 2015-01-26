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

typedef struct {
  dt_lua_widget_t parent;
} dt_lua_check_button_t;

typedef dt_lua_check_button_t* lua_check_button;

static void check_button_init(lua_State* L);
static dt_lua_widget_type_t check_button_type = {
  .name = "check_button",
  .gui_init = check_button_init,
  .gui_cleanup = NULL,
};

static void clicked_callback(GtkButton *widget, gpointer user_data)
{
  dt_lua_widget_trigger_callback_async((lua_widget)user_data,"clicked");
}

static void check_button_init(lua_State* L)
{
  lua_settop(L,3);
  const char * new_value = NULL;
  if(!lua_isnil(L,1)){
    new_value = luaL_checkstring(L,1);
  }
  lua_check_button check_button = malloc(sizeof(dt_lua_check_button_t));
  if(new_value) {
    check_button->parent.widget = gtk_check_button_new_with_label(new_value);
  } else {
    check_button->parent.widget = gtk_check_button_new();
  }
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(check_button->parent.widget),lua_toboolean(L,2));

  check_button->parent.type = &check_button_type;
  luaA_push_type(L, check_button_type.associated_type, &check_button);
  g_object_ref_sink(check_button->parent.widget);

  if(!lua_isnil(L,3)){
    lua_pushvalue(L,3);
    dt_lua_widget_set_callback(L,-2,"clicked");
  }
}

static int label_member(lua_State *L)
{
  lua_check_button check_button;
  luaA_to(L,lua_check_button,&check_button,1);
  if(lua_gettop(L) > 2) {
    const char * label = luaL_checkstring(L,3);
    gtk_button_set_label(GTK_BUTTON(check_button->parent.widget),label);
    return 0;
  }
  lua_pushstring(L,gtk_button_get_label(GTK_BUTTON(check_button->parent.widget)));
  return 1;
}

static int value_member(lua_State *L)
{
  lua_check_button check_button;
  luaA_to(L,lua_check_button,&check_button,1);
  if(lua_gettop(L) > 2) {
    luaL_checktype(L,3,LUA_TBOOLEAN);
    gboolean value = lua_toboolean(L,3);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(check_button->parent.widget),value);
    return 0;
  }
  lua_pushboolean(L,gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(check_button->parent.widget)));
  return 1;
}

int dt_lua_init_widget_check_button(lua_State* L)
{
  dt_lua_init_widget_type(L,&check_button_type,lua_check_button);

  lua_pushcfunction(L,value_member);
  lua_pushcclosure(L,dt_lua_gtk_wrap,1);
  dt_lua_type_register(L, lua_check_button, "value");
  lua_pushcfunction(L,label_member);
  lua_pushcclosure(L,dt_lua_gtk_wrap,1);
  dt_lua_type_register(L, lua_check_button, "label");
  dt_lua_widget_register_gtk_callback(L,lua_check_button,"clicked","clicked_callback",G_CALLBACK(clicked_callback));

  return 0;
}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
