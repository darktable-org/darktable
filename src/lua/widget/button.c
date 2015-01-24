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
} dt_lua_button_t;

typedef dt_lua_button_t* lua_button;

static void button_init(lua_State* L);
static dt_lua_widget_type_t button_type = {
  .name = "button",
  .gui_init = button_init,
  .gui_cleanup = NULL,
};

static void lua_button_callback(GtkButton *widget, gpointer user_data)
{
  dt_lua_widget_trigger_callback_async((lua_widget)user_data,"clicked");
}

static void button_init(lua_State* L)
{
  const char * new_value = NULL;
  if(!lua_isnoneornil(L,1)){
    new_value = luaL_checkstring(L,1);
  }
  lua_button button = malloc(sizeof(dt_lua_button_t));
  if(new_value) {
    button->parent.widget = gtk_button_new_with_label(new_value);
  } else {
    button->parent.widget = gtk_button_new();
  }
  if(!lua_isnoneornil(L,2)){
    dt_lua_widget_set_callback(L,2,"clicked");
  }


  g_signal_connect(button->parent.widget, "clicked", G_CALLBACK(lua_button_callback), button);
  button->parent.type = &button_type;
  luaA_push_type(L, button_type.associated_type, &button);
  g_object_ref_sink(button->parent.widget);
}



static int label_member(lua_State *L)
{
  lua_button button;
  luaA_to(L,lua_button,&button,1);
  if(lua_gettop(L) > 2) {
    const char * label = luaL_checkstring(L,3);
    gtk_button_set_label(GTK_BUTTON(button->parent.widget),label);
    return 0;
  }
  lua_pushstring(L,gtk_button_get_label(GTK_BUTTON(button->parent.widget)));
  return 1;
}

static int clicked_member(lua_State *L)
{
  if(lua_gettop(L) > 2) {
    dt_lua_widget_set_callback(L,1,"clicked");
    return 0;
  }
  dt_lua_widget_get_callback(L,1,"clicked");
  return 1;
}

int dt_lua_init_widget_button(lua_State* L)
{
  dt_lua_init_gpointer_type(L,lua_button);
  dt_lua_register_widget_type(L,&button_type,lua_button);

  lua_pushcfunction(L,label_member);
  lua_pushcclosure(L,dt_lua_gtk_wrap,1);
  dt_lua_type_register(L, lua_button, "label");
  lua_pushcfunction(L,clicked_member);
  dt_lua_type_register(L, lua_button, "clicked_callback");

  return 0;
}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
