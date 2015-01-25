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
} dt_lua_file_chooser_button_t;

typedef dt_lua_file_chooser_button_t* lua_file_chooser_button;

void file_chooser_button_init(lua_State* L);
static dt_lua_widget_type_t file_chooser_button_type = {
  .name = "file_chooser_button",
  .gui_init = file_chooser_button_init,
  .gui_cleanup = NULL,
};


void file_chooser_button_init(lua_State* L)
{
  lua_file_chooser_button file_chooser_button = malloc(sizeof(dt_lua_file_chooser_button_t));
	file_chooser_button->parent.widget = gtk_file_chooser_button_new(lua_tostring(L,2),lua_toboolean(L,1)?GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER:GTK_FILE_CHOOSER_ACTION_OPEN );

  file_chooser_button->parent.type = &file_chooser_button_type;
  luaA_push_type(L, file_chooser_button_type.associated_type, &file_chooser_button);
  g_object_ref_sink(file_chooser_button->parent.widget);

}

static int title_member(lua_State *L)
{
  lua_file_chooser_button file_chooser_button;
  luaA_to(L,lua_file_chooser_button,&file_chooser_button,1);
  if(lua_gettop(L) > 2) {
    const char * title = luaL_checkstring(L,3);
    gtk_file_chooser_button_set_title(GTK_FILE_CHOOSER_BUTTON(file_chooser_button->parent.widget),title);
    return 0;
  }
  lua_pushstring(L,gtk_file_chooser_button_get_title(GTK_FILE_CHOOSER_BUTTON(file_chooser_button->parent.widget)));
  return 1;
}

static int value_member(lua_State *L)
{
  lua_file_chooser_button file_chooser_button;
  luaA_to(L,lua_file_chooser_button,&file_chooser_button,1);
  if(lua_gettop(L) > 2) {
    const char * value = luaL_checkstring(L,3);
    gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(file_chooser_button->parent.widget),value);
    return 0;
  }
  lua_pushstring(L,gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(file_chooser_button->parent.widget)));
  return 1;
}


int dt_lua_init_widget_file_chooser_button(lua_State* L)
{
  dt_lua_init_widget_type(L,&file_chooser_button_type,lua_file_chooser_button);

  lua_pushcfunction(L,title_member);
  lua_pushcclosure(L,dt_lua_gtk_wrap,1);
  dt_lua_type_register(L, lua_file_chooser_button, "title");

  lua_pushcfunction(L,value_member);
  lua_pushcclosure(L,dt_lua_gtk_wrap,1);
  dt_lua_type_register(L, lua_file_chooser_button, "value");


  return 0;
}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
