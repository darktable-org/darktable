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

static dt_lua_widget_type_t separator_type = {
  .name = "separator",
  .gui_init = NULL,
  .gui_cleanup = NULL,
  .alloc_size = sizeof(dt_lua_widget_t),
  .parent= &widget_type

};

static int orientation_member(lua_State *L)
{
  lua_separator separator;
  luaA_to(L,lua_separator,&separator,1);
  dt_lua_orientation_t orientation;
  if(lua_gettop(L) > 2) {
    luaA_to(L,dt_lua_orientation_t,&orientation,3);
    gtk_orientable_set_orientation(GTK_ORIENTABLE(separator->widget),orientation);
    return 0;
  }
  orientation = gtk_orientable_get_orientation(GTK_ORIENTABLE(separator->widget));
  luaA_push(L,dt_lua_orientation_t,&orientation);
  return 1;
}


int dt_lua_init_widget_separator(lua_State* L)
{
  dt_lua_init_widget_type(L,&separator_type,lua_separator,GTK_TYPE_SEPARATOR);

  lua_pushcfunction(L,orientation_member);
  dt_lua_gtk_wrap(L);
  dt_lua_type_register(L, lua_separator, "orientation");
  return 0;
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

