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
} dt_lua_separator_t;

typedef dt_lua_separator_t* lua_separator;

static void separator_init(lua_State* L);
static dt_lua_widget_type_t separator_type = {
  .name = "separator",
  .gui_init = separator_init,
  .gui_cleanup = NULL,
};

static void separator_init(lua_State* L)
{
  lua_separator separator = malloc(sizeof(dt_lua_separator_t));
  dt_lua_orientation_t orientation;
  luaA_to(L,dt_lua_orientation_t,&orientation,1);
  separator->parent.widget = gtk_separator_new(orientation);
  separator->parent.type = &separator_type;
  luaA_push_type(L, separator_type.associated_type, &separator);
  g_object_ref_sink(separator->parent.widget);
}

int dt_lua_init_widget_separator(lua_State* L)
{
  dt_lua_init_widget_type(L,&separator_type,lua_separator);

  return 0;
}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
