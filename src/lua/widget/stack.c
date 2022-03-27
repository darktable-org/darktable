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

static dt_lua_widget_type_t stack_type = {
  .name = "stack",
  .gui_init = NULL,
  .gui_cleanup = NULL,
  .alloc_size = sizeof(dt_lua_container_t),
  .parent= &container_type
};

static int active_member(lua_State*L)
{
  lua_stack stack;
  luaA_to(L,lua_stack,&stack,1);
  if(lua_gettop(L) > 2) {
    GList * children = gtk_container_get_children(GTK_CONTAINER(stack->widget));
    int length = g_list_length(children);
    if(lua_isnumber(L,3)) {
      int index = lua_tointeger(L,3) ;
      if(index < 1 || index > length) {
        g_list_free(children);
        return luaL_error(L,"Invalid index for stack widget : %d\n",index);
      }
      gtk_stack_set_visible_child(GTK_STACK(stack->widget),g_list_nth_data(children,index-1));
    } else if(dt_lua_isa(L,3,lua_widget)) {
      lua_widget child;
      luaA_to(L,lua_widget,&child,3);
      if(!g_list_find(children,child->widget)) {
        g_list_free(children);
        return luaL_error(L,"Active child of stack widget is not in the stack\n");
      }
      gtk_stack_set_visible_child(GTK_STACK(stack->widget),child->widget);
    } else {
      g_list_free(children);
      return luaL_error(L,"Invalid type for stack active child\n");
    }
    g_list_free(children);
    return 0;
  }
  GtkWidget * child = gtk_stack_get_visible_child(GTK_STACK(stack->widget));
  if(child)
    luaA_push(L,lua_widget,&child);
  else
    lua_pushnil(L);
  return 1;
}

static int h_size_fixed_member(lua_State *L)
{
  lua_stack stack;
  luaA_to(L, lua_stack, &stack, 1);
  if(lua_gettop(L) > 2) {
    gboolean resize = lua_toboolean(L,3);
    gtk_stack_set_hhomogeneous(GTK_STACK(stack->widget), resize);
    return 0;
  }
  lua_pushboolean(L,gtk_stack_get_hhomogeneous(GTK_STACK(stack->widget)));
  return 1;
}


static int v_size_fixed_member(lua_State *L)
{
  lua_stack stack;
  luaA_to(L, lua_stack, &stack, 1);
  if(lua_gettop(L) > 2) {
    gboolean resize = lua_toboolean(L,3);
    gtk_stack_set_vhomogeneous(GTK_STACK(stack->widget), resize);
    return 0;
  }
  lua_pushboolean(L,gtk_stack_get_vhomogeneous(GTK_STACK(stack->widget)));
  return 1;
}

int dt_lua_init_widget_stack(lua_State* L)
{
  dt_lua_init_widget_type(L,&stack_type,lua_stack,GTK_TYPE_STACK);

  lua_pushcfunction(L,active_member);
  dt_lua_gtk_wrap(L);
  dt_lua_type_register(L, lua_stack, "active");
  lua_pushcfunction(L,h_size_fixed_member);
  dt_lua_gtk_wrap(L);
  dt_lua_type_register(L, lua_stack, "h_size_fixed");
  lua_pushcfunction(L,v_size_fixed_member);
  dt_lua_gtk_wrap(L);
  dt_lua_type_register(L, lua_stack, "v_size_fixed");
  return 0;
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

