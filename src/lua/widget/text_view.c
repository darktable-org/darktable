/*
   This file is part of darktable,
   copyright (c) 2015 Jeremy Rosen
   copyright (c) 2015 tobias ellinghaus

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

static void text_view_init(lua_State* L);
static void text_view_cleanup(lua_State* L,lua_widget widget);
static dt_lua_widget_type_t textview_type = {
  .name = "text_view",
  .gui_init = text_view_init,
  .gui_cleanup = text_view_cleanup,
  .alloc_size = sizeof(dt_lua_widget_t),
  .parent= &widget_type
};


static void text_view_init(lua_State* L)
{
  lua_text_view text_view;
  luaA_to(L,lua_text_view,&text_view,1);
  dt_gui_key_accel_block_on_focus_connect(GTK_WIDGET(text_view->widget));
}

static void text_view_cleanup(lua_State* L,lua_widget widget)
{
  dt_gui_key_accel_block_on_focus_disconnect(widget->widget);
}

static gchar* gtk_text_buffer_get_all_text(GtkTextBuffer *buffer)
{
  GtkTextIter start;
  GtkTextIter end;
  gtk_text_buffer_get_start_iter(buffer,&start);
  gtk_text_buffer_get_end_iter(buffer,&end);
  return gtk_text_buffer_get_text(buffer,&start,&end,false);
}

static int text_member(lua_State *L)
{
  lua_text_view textview;
  luaA_to(L,lua_text_view,&textview,1);
  GtkTextBuffer * textbuffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(textview->widget));
  if(lua_gettop(L) > 2) {
    const char * text = luaL_checkstring(L,3);
    gtk_text_buffer_set_text(textbuffer,text,-1);
    return 0;
  }
  lua_pushstring(L,gtk_text_buffer_get_all_text(textbuffer));
  return 1;
}

static int editable_member(lua_State *L)
{
  lua_text_view text_view;
  luaA_to(L,lua_text_view,&text_view,1);
  if(lua_gettop(L) > 2) {
    gboolean editable = lua_toboolean(L,3);
    gtk_text_view_set_editable(GTK_TEXT_VIEW(text_view->widget),editable);
    return 0;
  }
  lua_pushboolean(L,gtk_text_view_get_editable(GTK_TEXT_VIEW(text_view->widget)));
  return 1;
}

static int tostring_member(lua_State *L)
{
  lua_text_view widget;
  luaA_to(L, lua_text_view, &widget, 1);
  GtkTextBuffer * textbuffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(widget->widget));
  const gchar *text = gtk_text_buffer_get_all_text(textbuffer);
  gchar *res = g_strdup_printf("%s (\"%s\")", G_OBJECT_TYPE_NAME(widget->widget), text ? text : "");
  lua_pushstring(L, res);
  g_free(res);
  return 1;
}

int dt_lua_init_widget_text_view(lua_State* L)
{
  dt_lua_init_widget_type(L,&textview_type,lua_text_view,GTK_TYPE_TEXT_VIEW);

  lua_pushcfunction(L, tostring_member);
  dt_lua_gtk_wrap(L);
  dt_lua_type_setmetafield(L, lua_text_view, "__tostring");
  lua_pushcfunction(L,text_member);
  dt_lua_gtk_wrap(L);
  dt_lua_type_register(L, lua_text_view, "text");
  lua_pushcfunction(L,editable_member);
  dt_lua_gtk_wrap(L);
  dt_lua_type_register(L, lua_text_view, "editable");
  return 0;
}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
