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
#include "lua/types.h"
#include "lua/widget/common.h"

static dt_lua_widget_type_t button_type = {
  .name = "button",
  .gui_init = NULL,
  .gui_cleanup = NULL,
  .alloc_size = sizeof(dt_lua_widget_t),
  .parent= &widget_type
};

static void clicked_callback(GtkButton *widget, gpointer user_data)
{
  dt_lua_async_call_alien(dt_lua_widget_trigger_callback,
      0,NULL,NULL,
      LUA_ASYNC_TYPENAME,"lua_widget",user_data,
      LUA_ASYNC_TYPENAME,"const char*","clicked",
      LUA_ASYNC_DONE);
}




static int label_member(lua_State *L)
{
  lua_button button;
  luaA_to(L,lua_button,&button,1);
  if(lua_gettop(L) > 2) {
    const char * label = luaL_checkstring(L,3);
    gtk_button_set_label(GTK_BUTTON(button->widget),label);
    return 0;
  }
  lua_pushstring(L,gtk_button_get_label(GTK_BUTTON(button->widget)));
  return 1;
}

static int tostring_member(lua_State *L)
{
  lua_button widget;
  luaA_to(L, lua_button, &widget, 1);
  const gchar *text = gtk_button_get_label(GTK_BUTTON(widget->widget));
  gchar *res = g_strdup_printf("%s (\"%s\")", G_OBJECT_TYPE_NAME(widget->widget), text ? text : "");
  lua_pushstring(L, res);
  g_free(res);
  return 1;
}

int dt_lua_init_widget_button(lua_State* L)
{
  dt_lua_init_widget_type(L,&button_type,lua_button,GTK_TYPE_BUTTON);

  lua_pushcfunction(L, tostring_member);
  dt_lua_gtk_wrap(L);
  dt_lua_type_setmetafield(L, lua_button, "__tostring");
  lua_pushcfunction(L,label_member);
  dt_lua_gtk_wrap(L);
  dt_lua_type_register(L, lua_button, "label");
  dt_lua_widget_register_gtk_callback(L,lua_button,"clicked","clicked_callback",G_CALLBACK(clicked_callback));

  return 0;
}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
