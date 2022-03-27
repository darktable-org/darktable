/*
   This file is part of darktable,
   Copyright (C) 2015-2021 darktable developers.

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

static void container_init(lua_State* L);
static void container_cleanup(lua_State* L,lua_widget widget);
dt_lua_widget_type_t container_type = {
  .name = "container",
  .gui_init = container_init,
  .gui_cleanup = container_cleanup,
  .alloc_size = sizeof(dt_lua_container_t),
  .parent= &widget_type
};

static int container_reset(lua_State* L)
{
  lua_container container;
  luaA_to(L,lua_container,&container,1);
  lua_getiuservalue(L, 1, 1);
  GList *children = gtk_container_get_children(GTK_CONTAINER(container->widget));
  for(const GList *curelt = children; curelt; curelt = g_list_next(curelt))
  {
    lua_pushcfunction(L,dt_lua_widget_trigger_callback);
    GtkWidget* cur_widget = curelt->data;
    luaA_push(L,lua_widget,&cur_widget);
    lua_pushstring(L,"reset");
    lua_call(L,2,0);
  }
  lua_pop(L,1);
  g_list_free(children);
  return 0;
}

static void on_child_added(GtkContainer *container,GtkWidget *child,lua_container user_data)
{
  dt_lua_async_call_alien(dt_lua_widget_trigger_callback,
      0,NULL,NULL,
      LUA_ASYNC_TYPENAME,"lua_widget",user_data,
      LUA_ASYNC_TYPENAME,"const char*","add",
      LUA_ASYNC_TYPENAME,"lua_widget",child,
      LUA_ASYNC_DONE);
}

static void on_child_removed(GtkContainer *container,GtkWidget *child,lua_container user_data)
{
  dt_lua_async_call_alien(dt_lua_widget_trigger_callback,
      0,NULL,NULL,
      LUA_ASYNC_TYPENAME,"lua_widget",user_data,
      LUA_ASYNC_TYPENAME,"const char*","remove",
      LUA_ASYNC_TYPENAME,"lua_widget",child,
      LUA_ASYNC_DONE);
}


static void container_cleanup(lua_State* L,lua_widget widget)
{
  g_signal_handlers_disconnect_by_func(widget->widget, G_CALLBACK(on_child_removed), widget);
  dt_gui_container_remove_children(GTK_CONTAINER(widget->widget));
}


static int child_added(lua_State *L)
{
  lua_widget widget;
  luaA_to(L, lua_widget,&widget,2);
  lua_getiuservalue(L, 1, 1);
  luaA_push(L,lua_widget,&widget);
  lua_pushvalue(L,2);
  lua_settable(L,-3);
  return 0;
}

static int child_removed(lua_State *L)
{
  lua_widget widget;
  luaA_to(L, lua_widget,&widget,2),
  lua_getiuservalue(L, 1, 1);
  luaA_push(L,lua_widget,&widget);
  lua_pushnil(L);
  lua_settable(L,-3);
  return 0;
}

static void container_init(lua_State* L)
{
  lua_container container;
  luaA_to(L,lua_container,&container,-1);
  lua_pushcfunction(L,container_reset);
  dt_lua_widget_set_callback(L,-2,"reset");
  lua_pushcfunction(L,child_added);
  dt_lua_widget_set_callback(L,-2,"add");
  lua_pushcfunction(L,child_removed);
  dt_lua_widget_set_callback(L,-2,"remove");
  g_signal_connect(container->widget, "add", G_CALLBACK(on_child_added), container);
  g_signal_connect(container->widget, "remove", G_CALLBACK(on_child_removed), container);
}

static int container_len(lua_State*L)
{
  lua_container container;
  luaA_to(L,lua_container,&container,1);
  int children = dt_gui_container_num_children(GTK_CONTAINER(container->widget));
  lua_pushinteger(L,children);
  return 1;
}

static int container_numindex(lua_State*L)
{
  lua_container container;
  luaA_to(L,lua_container,&container,1);
  GList * children = gtk_container_get_children(GTK_CONTAINER(container->widget));
  int index = lua_tointeger(L,2) -1;
  int length = g_list_length(children);
  if(lua_gettop(L) >2) {
    if(!lua_isnil(L,3) &&  index == length) {
      lua_widget widget;
      luaA_to(L, lua_widget,&widget,3);
      gtk_container_add(GTK_CONTAINER(container->widget),widget->widget);
      // the following lines add the widget to the container's user_value to guarantee it's referenced on the lua side
      // they should be done by child_added, but
      // there can be a race with lua's GC, so do it now.
      // child_added doing it a second time is harmless
      lua_getiuservalue(L, 1, 1);
      luaA_push(L,lua_widget,&widget);
      lua_pushvalue(L,3);
      lua_settable(L,-3);
      lua_pop(L,1);
    } else if(lua_isnil(L,3) && index < length) {
      GtkWidget *searched_widget = g_list_nth_data(children,index);
      gtk_container_remove(GTK_CONTAINER(container->widget),searched_widget);
    } else {
      luaL_error(L,"Incorrect index or value when setting the child of a container : you can only append, not change a child in the middle");
    }
    g_list_free(children);
    return 0;
  } else {
    if(index < 0 || index >= length)
    {
      lua_pushnil(L);
    }
    else
    {
      GtkWidget *searched_widget = g_list_nth_data(children, index);
      luaA_push(L, lua_widget, &searched_widget);
    }
    g_list_free(children);
    return 1;
  }
}

int dt_lua_init_widget_container(lua_State* L)
{
  dt_lua_init_widget_type(L,&container_type,lua_container,GTK_TYPE_CONTAINER);

  lua_pushcfunction(L,container_len);
  dt_lua_gtk_wrap(L);
  lua_pushcfunction(L,container_numindex);
  dt_lua_gtk_wrap(L);
  dt_lua_type_register_number(L,lua_container);

  return 0;
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

