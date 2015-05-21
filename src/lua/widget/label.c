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
#include "lua/widget/common.h"
#include "lua/types.h"

static dt_lua_widget_type_t label_type = {
  .name = "label",
  .gui_init = NULL,
  .gui_cleanup = NULL,
  .alloc_size = sizeof(dt_lua_widget_t),
  .parent= &widget_type
};


static int label_member(lua_State *L)
{
  lua_label label;
  luaA_to(L,lua_label,&label,1);
  if(lua_gettop(L) > 2) {
    const char * text = luaL_checkstring(L,3);
    gtk_label_set_text(GTK_LABEL(label->widget),text);
    return 0;
  }
  lua_pushstring(L,gtk_label_get_text(GTK_LABEL(label->widget)));
  return 1;
}

static int selectable_member(lua_State *L)
{
  lua_label label;
  luaA_to(L,lua_label,&label,1);
  if(lua_gettop(L) > 2) {
    gboolean selectable = lua_toboolean(L,3);
    gtk_label_set_selectable(GTK_LABEL(label->widget),selectable);
    return 0;
  }
  lua_pushboolean(L,gtk_label_get_selectable(GTK_LABEL(label->widget)));
  return 1;
}

int dt_lua_init_widget_label(lua_State* L)
{
  dt_lua_init_widget_type(L,&label_type,lua_label,GTK_TYPE_LABEL);

  lua_pushcfunction(L,label_member);
  lua_pushcclosure(L,dt_lua_gtk_wrap,1);
  dt_lua_type_register(L, lua_label, "label");
  lua_pushcfunction(L,selectable_member);
  lua_pushcclosure(L,dt_lua_gtk_wrap,1);
  dt_lua_type_register(L, lua_label, "selectable");
  return 0;
}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
