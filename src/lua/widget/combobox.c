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
#include "bauhaus/bauhaus.h"
#include "lua/types.h"
#include "gui/gtk.h"

typedef struct {
  dt_lua_widget_t parent;
} dt_lua_combobox_t;

typedef dt_lua_combobox_t* lua_combobox;

static void combobox_init(lua_State* L);
static dt_lua_widget_type_t combobox_type = {
  .name = "combobox",
  .gui_init = combobox_init,
  .gui_cleanup = NULL,
};

static void combobox_init(lua_State* L)
{
  lua_combobox combobox = malloc(sizeof(dt_lua_combobox_t));
  combobox->parent.widget = dt_bauhaus_combobox_new(NULL);
  if(lua_toboolean(L,1)) {
    dt_bauhaus_combobox_set_editable(combobox->parent.widget,1);
  }
  combobox->parent.type = &combobox_type;
  luaA_push_type(L, combobox_type.associated_type, &combobox);
  g_object_ref_sink(combobox->parent.widget);
}

static int combobox_len(lua_State*L)
{
  lua_combobox combobox;
  luaA_to(L,lua_combobox,&combobox,1);
  lua_pushinteger(L,dt_bauhaus_combobox_length(combobox->parent.widget));
  return 1;
}

static int combobox_numindex(lua_State*L)
{
  lua_combobox combobox;
  luaA_to(L,lua_combobox,&combobox,1);
  int key = lua_tointeger(L,2);
  int length = dt_bauhaus_combobox_length(combobox->parent.widget);
  if(lua_gettop(L) > 2) {
    if(key <= 0 || key > length +1) {
      return luaL_error(L,"Invalid index for combobox : %d\n",key);
    } else if(key == length +1) {
      const char * string = luaL_checkstring(L,3);
      dt_bauhaus_combobox_add(combobox->parent.widget,string);
    } else if(lua_isnil(L,3)){
      dt_bauhaus_combobox_remove_at(combobox->parent.widget,key-1);
    } else {
      const char * string = luaL_checkstring(L,3);
      dt_bauhaus_combobox_remove_at(combobox->parent.widget,key-1);
      dt_bauhaus_combobox_insert(combobox->parent.widget,string,key-1);
    }
    return 0;
  }
  if(key <= 0 || key > length) {
    return luaL_error(L,"Invalid index for combo box : %d\n",key);
  }
  const GList *labels = dt_bauhaus_combobox_get_labels(combobox->parent.widget);
  lua_pushstring(L,g_list_nth_data((GList*)labels,key-1));
  return 1;
}

static int value_member(lua_State*L)
{
  lua_combobox combobox;
  luaA_to(L,lua_combobox,&combobox,1);
  int length = dt_bauhaus_combobox_length(combobox->parent.widget);
  if(lua_gettop(L) > 2) {
    if(lua_isnil(L,3)) {
      dt_bauhaus_combobox_set(combobox->parent.widget,-1);
    } else if(lua_isnumber(L,3)) {
      int index = lua_tointeger(L,3) ;
      if(index < 1 || index > length) {
        return luaL_error(L,"Invalid index for combo box : %d\n",index);
      }
      dt_bauhaus_combobox_set(combobox->parent.widget,index -1);
    } else if(lua_isstring(L,3)&& dt_bauhaus_combobox_get_editable(combobox->parent.widget)) {
      const char * string = lua_tostring(L,3);
      dt_bauhaus_combobox_set_text(combobox->parent.widget,string);
    } else {
      return luaL_error(L,"Invalid type for combo box value\n");
    }
    return 0;
  }
  lua_pushstring(L,dt_bauhaus_combobox_get_text(combobox->parent.widget));
  return 1;
}

int dt_lua_init_widget_combobox(lua_State* L)
{
  dt_lua_init_widget_type(L,&combobox_type,lua_combobox);

  lua_pushcfunction(L,combobox_len);
  lua_pushcclosure(L,dt_lua_gtk_wrap,1);
  lua_pushcfunction(L,combobox_numindex);
  lua_pushcclosure(L,dt_lua_gtk_wrap,1);
  dt_lua_type_register_number(L,lua_combobox);
  lua_pushcfunction(L,value_member);
  lua_pushcclosure(L,dt_lua_gtk_wrap,1);
  dt_lua_type_register(L, lua_combobox, "value");

  return 0;
}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
