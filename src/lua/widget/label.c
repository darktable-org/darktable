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
} dt_lua_label_t;

typedef dt_lua_label_t* lua_label;

static void label_init(lua_State* L);
static dt_lua_widget_type_t label_type = {
  .name = "label",
  .gui_init = label_init,
  .gui_cleanup = NULL,
};


static void label_init(lua_State* L)
{
  const char * new_value = NULL;
  if(!lua_isnoneornil(L,1)){
    new_value = luaL_checkstring(L,1);
  }
  lua_label label = malloc(sizeof(dt_lua_label_t));
  if(new_value) {
    label->parent.widget = gtk_label_new(new_value);
  } else {
    label->parent.widget = gtk_label_new(NULL);
  }
  label->parent.type = &label_type;
  luaA_push_type(L, label_type.associated_type, &label);
  g_object_ref_sink(label->parent.widget);

}

static int label_member(lua_State *L)
{
  lua_label label;
  luaA_to(L,lua_label,&label,1);
  if(lua_gettop(L) > 2) {
    const char * text = luaL_checkstring(L,3);
    gtk_label_set_text(GTK_LABEL(label->parent.widget),text);
    return 0;
  }
  lua_pushstring(L,gtk_label_get_text(GTK_LABEL(label->parent.widget)));
  return 1;
}

int dt_lua_init_widget_label(lua_State* L)
{
  dt_lua_init_widget_type(L,&label_type,lua_label);

  lua_pushcfunction(L,label_member);
  lua_pushcclosure(L,dt_lua_gtk_wrap,1);
  dt_lua_type_register(L, lua_label, "label");
  return 0;
}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
