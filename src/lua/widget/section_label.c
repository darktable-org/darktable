/*
   This file is part of darktable,
   Copyright (C) 2017-2020 darktable developers.

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
#include "gui/gtk.h"

static void section_label_init(lua_State* L);
static dt_lua_widget_type_t section_label_type = {
  .name = "section_label",
  .gui_init = section_label_init,
  .gui_cleanup = NULL,
  .alloc_size = sizeof(dt_lua_widget_t),
  .parent= &widget_type
};

static void section_label_init(lua_State* L)
{
  lua_section_label label;
  luaA_to(L,lua_section_label,&label,1);
  dt_ui_section_label_set(label->widget);
}


static int section_label_member(lua_State *L)
{
  lua_section_label label;
  luaA_to(L,lua_section_label,&label,1);
  if(lua_gettop(L) > 2) {
    const char * text = luaL_checkstring(L,3);
    gtk_label_set_text(GTK_LABEL(label->widget),text);
    return 0;
  }
  lua_pushstring(L,gtk_label_get_text(GTK_LABEL(label->widget)));
  return 1;
}


static int tostring_member(lua_State *L)
{
  lua_section_label widget;
  luaA_to(L, lua_section_label, &widget, 1);
  const gchar *text = gtk_label_get_text(GTK_LABEL(widget->widget));
  gchar *res = g_strdup_printf("%s (\"%s\")", G_OBJECT_TYPE_NAME(widget->widget), text ? text : "");
  lua_pushstring(L, res);
  g_free(res);
  return 1;
}

int dt_lua_init_widget_section_label(lua_State* L)
{
  dt_lua_init_widget_type(L,&section_label_type,lua_section_label,GTK_TYPE_LABEL);

  lua_pushcfunction(L, tostring_member);
  dt_lua_gtk_wrap(L);
  dt_lua_type_setmetafield(L, lua_section_label, "__tostring");
  lua_pushcfunction(L,section_label_member);
  dt_lua_gtk_wrap(L);
  dt_lua_type_register(L, lua_section_label, "label");
  return 0;
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
