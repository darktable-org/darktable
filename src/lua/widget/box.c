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
#include "bauhaus/bauhaus.h"
#include "gui/gtk.h"
#include "lua/types.h"
#include "lua/widget/common.h"

static void box_init(lua_State* L);
static dt_lua_widget_type_t box_type = {
  .name = "box",
  .gui_init = box_init,
  .gui_cleanup = NULL,
  .alloc_size = sizeof(dt_lua_container_t),
  .parent= &container_type
};

static void box_init(lua_State* L)
{
  lua_box box;
  luaA_to(L,lua_box,&box,-1);
  gtk_orientable_set_orientation(GTK_ORIENTABLE(box->widget),GTK_ORIENTATION_VERTICAL);
}


static int orientation_member(lua_State *L)
{
  lua_box box;
  luaA_to(L,lua_box,&box,1);
  dt_lua_orientation_t orientation;
  if(lua_gettop(L) > 2) {
    luaA_to(L,dt_lua_orientation_t,&orientation,3);
    gtk_orientable_set_orientation(GTK_ORIENTABLE(box->widget),orientation);
    if(gtk_orientable_get_orientation(GTK_ORIENTABLE(box->widget)) == GTK_ORIENTATION_HORIZONTAL)
    {
      GList *children = gtk_container_get_children(GTK_CONTAINER(box->widget));
      for(const GList *l = children; l; l = g_list_next(l))
      {
        gtk_box_set_child_packing(GTK_BOX(box->widget), GTK_WIDGET(l->data), TRUE, TRUE, 0, GTK_PACK_START);
      }
      g_list_free(children);
    }
    return 0;
  }
  orientation = gtk_orientable_get_orientation(GTK_ORIENTABLE(box->widget));
  luaA_push(L,dt_lua_orientation_t,&orientation);
  return 1;
}

void _get_packing_info(lua_box box, gboolean *expand, gboolean *fill, guint *padding)
{
  GList *children = gtk_container_get_children(GTK_CONTAINER(box->widget));
  for(const GList *l = children; l; l = g_list_next(l))
  {
    gtk_box_query_child_packing(GTK_BOX(box->widget), GTK_WIDGET(l->data), expand, fill, padding, NULL);
    break;
  }
  g_list_free(children);
}

void _set_packing_info(lua_box box, gboolean expand, gboolean fill, guint padding)
{
  GList *children = gtk_container_get_children(GTK_CONTAINER(box->widget));
  for(const GList *l = children; l; l = g_list_next(l))
  {
    gtk_box_set_child_packing(GTK_BOX(box->widget), GTK_WIDGET(l->data), expand, fill, padding, GTK_PACK_START);
  }
  g_list_free(children);
}

static int packing_expand_member(lua_State *L)
{
  lua_box box;
  luaA_to(L, lua_box, &box, 1);
  gboolean old_fill;
  gboolean old_expand;
  guint old_padding;
  _get_packing_info(box, &old_expand, &old_fill, &old_padding);
  if(lua_gettop(L) > 2) {
    gboolean expand = lua_toboolean(L, 3);
    if(gtk_orientable_get_orientation(GTK_ORIENTABLE(box->widget)) == GTK_ORIENTATION_HORIZONTAL)
      _set_packing_info(box, expand, old_fill, old_padding);
    return 0;
  }
  lua_pushboolean(L, old_expand);
  return 1;
}

static int packing_fill_member(lua_State *L)
{
  lua_box box;
  luaA_to(L, lua_box, &box, 1);
  gboolean old_fill;
  gboolean old_expand;
  guint old_padding;
  _get_packing_info(box, &old_expand, &old_fill, &old_padding);
  if(lua_gettop(L) > 2) {
    gboolean fill = lua_toboolean(L, 3);
    if(gtk_orientable_get_orientation(GTK_ORIENTABLE(box->widget)) == GTK_ORIENTATION_HORIZONTAL)
      _set_packing_info(box, old_expand, fill, old_padding);
    return 0;
  }
  lua_pushboolean(L, old_fill);
  return 1;
}

static int packing_padding_member(lua_State *L)
{
  lua_box box;
  luaA_to(L, lua_box, &box, 1);
  gboolean old_fill;
  gboolean old_expand;
  guint old_padding;
  _get_packing_info(box, &old_expand, &old_fill, &old_padding);
  if(lua_gettop(L) > 2) {
    guint padding = lua_tointeger(L, 3);
    if(gtk_orientable_get_orientation(GTK_ORIENTABLE(box->widget)) == GTK_ORIENTATION_HORIZONTAL)
      _set_packing_info(box, old_expand, old_fill, padding);
    return 0;
  }
  lua_pushboolean(L, old_padding);
  return 1;
}

int dt_lua_init_widget_box(lua_State* L)
{
  dt_lua_init_widget_type(L,&box_type,lua_box,GTK_TYPE_BOX);

  lua_pushcfunction(L,orientation_member);
  dt_lua_gtk_wrap(L);
  dt_lua_type_register(L, lua_box, "orientation");
  lua_pushcfunction(L,packing_expand_member);
  dt_lua_gtk_wrap(L);
  dt_lua_type_register(L, lua_box, "packing_expand");
  lua_pushcfunction(L,packing_fill_member);
  dt_lua_gtk_wrap(L);
  dt_lua_type_register(L, lua_box, "packing_fill");
  lua_pushcfunction(L,packing_padding_member);
  dt_lua_gtk_wrap(L);
  dt_lua_type_register(L, lua_box, "packing_padding");
  return 0;
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

