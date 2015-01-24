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
  GList * text;
} dt_lua_combo_box_text_t;

typedef dt_lua_combo_box_text_t* lua_combo_box_text;

void combo_box_text_init(lua_State* L);
void combo_box_text_cleanup(lua_State* L,lua_widget widget);
static dt_lua_widget_type_t combo_box_text_type = {
  .name = "combo_box_text",
  .gui_init = combo_box_text_init,
  .gui_reset = NULL,
  .gui_cleanup = combo_box_text_cleanup,
};

void combo_box_text_init(lua_State* L)
{
  lua_combo_box_text combo_box_text = malloc(sizeof(dt_lua_combo_box_text_t));
  combo_box_text->text = NULL;
  if(lua_toboolean(L,1)) {
    combo_box_text->parent.widget = gtk_combo_box_text_new_with_entry();
  } else {
    combo_box_text->parent.widget = gtk_combo_box_text_new();
  }
  dt_gui_key_accel_block_on_focus_connect(gtk_bin_get_child(GTK_BIN(combo_box_text->parent.widget)));
  combo_box_text->parent.type = &combo_box_text_type;
  luaA_push_type(L, combo_box_text_type.associated_type, &combo_box_text);
  g_object_ref_sink(combo_box_text->parent.widget);
}

void combo_box_text_cleanup(lua_State* L,lua_widget widget)
{
  lua_combo_box_text combo_box_text = (lua_combo_box_text)widget;
  g_list_free_full(combo_box_text->text,free);
  dt_gui_key_accel_block_on_focus_disconnect(gtk_bin_get_child(GTK_BIN(combo_box_text->parent.widget)));
}


static int combo_box_text_len(lua_State*L)
{
  lua_combo_box_text combo_box_text;
  luaA_to(L,lua_combo_box_text,&combo_box_text,1);
  lua_pushinteger(L,g_list_length(combo_box_text->text));
  return 1;
}

static int combo_box_text_numindex(lua_State*L)
{
  lua_combo_box_text combo_box_text;
  luaA_to(L,lua_combo_box_text,&combo_box_text,1);
  int key = lua_tointeger(L,2);
  int length = g_list_length(combo_box_text->text);
  if(lua_gettop(L) > 2) {
    if(key <= 0 || key > length +1) {
      return luaL_error(L,"Invalid index for combo box : %d\n",key);
    } else if(key == length +1) {
      const char * string = luaL_checkstring(L,3);
      gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo_box_text->parent.widget),string);
      combo_box_text->text = g_list_append(combo_box_text->text,strdup(string));
    } else if(lua_isnil(L,3)){
      gtk_combo_box_text_remove(GTK_COMBO_BOX_TEXT(combo_box_text->parent.widget),key-1);
      GList * removed_elt = g_list_nth(combo_box_text->text,key-1);
      combo_box_text->text = g_list_remove_link(combo_box_text->text,removed_elt);
      g_list_free_full(removed_elt,free);
    } else {
      const char * string = luaL_checkstring(L,3);
      gtk_combo_box_text_remove(GTK_COMBO_BOX_TEXT(combo_box_text->parent.widget),key-1);
      gtk_combo_box_text_insert_text(GTK_COMBO_BOX_TEXT(combo_box_text->parent.widget),key-1,string);
      GList * removed_elt = g_list_nth(combo_box_text->text,key-1);
      combo_box_text->text = g_list_remove_link(combo_box_text->text,removed_elt);
      combo_box_text->text = g_list_insert(combo_box_text->text,strdup(string),key-1);
      g_list_free_full(removed_elt,free);
    }
    return 0;
  }
  if(key <= 0 || key > length) {
    return luaL_error(L,"Invalid index for combo box : %d\n",key);
  }
  lua_pushstring(L,g_list_nth_data(combo_box_text->text,key-1));
  return 1;
}

static int value_member(lua_State*L)
{
  lua_combo_box_text combo_box_text;
  luaA_to(L,lua_combo_box_text,&combo_box_text,1);
  int length = g_list_length(combo_box_text->text);
  if(lua_gettop(L) > 2) {
    if(lua_isnil(L,3)) {
      gtk_combo_box_set_active(GTK_COMBO_BOX(combo_box_text->parent.widget),-1);
    } else if(lua_isnumber(L,3)) {
      int index = lua_tointeger(L,3) ;
      if(index < 1 || index > length) {
        return luaL_error(L,"Invalid index for combo box : %d\n",index);
      }
      gtk_combo_box_set_active(GTK_COMBO_BOX(combo_box_text->parent.widget),index-1);
    } else if(lua_isstring(L,3)&& GTK_IS_ENTRY(gtk_bin_get_child(GTK_BIN(combo_box_text->parent.widget)))) {
      const char * string = lua_tostring(L,3);
      GtkEntry *entry = GTK_ENTRY(gtk_bin_get_child(GTK_BIN(combo_box_text->parent.widget)));
      gtk_entry_set_text(entry,string);
    } else {
      return luaL_error(L,"Invalid type for combo box value\n");
    }
    return 0;
  }
  lua_pushstring(L,gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(combo_box_text->parent.widget)));
  return 1;
}

int dt_lua_init_widget_combo_box_text(lua_State* L)
{
  dt_lua_init_gpointer_type(L,lua_combo_box_text);
  dt_lua_register_widget_type(L,&combo_box_text_type,lua_combo_box_text);

  lua_pushcfunction(L,combo_box_text_len);
  lua_pushcclosure(L,dt_lua_gtk_wrap,1);
  lua_pushcfunction(L,combo_box_text_numindex);
  lua_pushcclosure(L,dt_lua_gtk_wrap,1);
  dt_lua_type_register_number(L,lua_combo_box_text);
  lua_pushcfunction(L,value_member);
  lua_pushcclosure(L,dt_lua_gtk_wrap,1);
  dt_lua_type_register(L, lua_combo_box_text, "value");

  return 0;
}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
