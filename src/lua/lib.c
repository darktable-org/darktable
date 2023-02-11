/*
   This file is part of darktable,
   Copyright (C) 2014-2020 darktable developers.

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
#include "lua/lib.h"
#include "gui/gtk.h"
#include "lua/call.h"
#include "lua/modules.h"
#include "lua/types.h"

static int expanded_member(lua_State *L)
{
  dt_lib_module_t *module = *(dt_lib_module_t **)lua_touserdata(L, 1);
  if(lua_gettop(L) != 3)
  {
    lua_pushboolean(L, module->expandable(module));
    return 1;
  }
  else
  {
    dt_lib_gui_set_expanded(module, lua_toboolean(L, 3));
    return 0;
  }
}

static int visible_member(lua_State *L)
{
  dt_lib_module_t *module = *(dt_lib_module_t **)lua_touserdata(L, 1);
  if(lua_gettop(L) != 3)
  {
    lua_pushboolean(L, dt_lib_is_visible(module));
    return 1;
  }
  else
  {
    dt_lib_set_visible(module, lua_toboolean(L, 3));
    return 0;
  }
}

static int version_member(lua_State *L)
{
  dt_lib_module_t *module = *(dt_lib_module_t **)lua_touserdata(L, 1);
  lua_pushinteger(L, module->version());
  return 1;
}

static int id_member(lua_State *L)
{
  dt_lib_module_t *module = *(dt_lib_module_t **)lua_touserdata(L, 1);
  lua_pushstring(L, module->plugin_name);
  return 1;
}

static int name_member(lua_State *L)
{
  dt_lib_module_t *module = *(dt_lib_module_t **)lua_touserdata(L, 1);
  lua_pushstring(L, module->name(module));
  return 1;
}

static int expandable_member(lua_State *L)
{
  dt_lib_module_t *module = *(dt_lib_module_t **)lua_touserdata(L, 1);
  lua_pushboolean(L, module->expandable(module));
  return 1;
}

static int on_screen_member(lua_State *L)
{
  dt_lib_module_t *module = *(dt_lib_module_t **)lua_touserdata(L, 1);
  lua_pushboolean(L, module->widget != NULL);
  return 1;
}

static int position_member(lua_State*L)
{
  dt_lib_module_t * module = *(dt_lib_module_t**)lua_touserdata(L, 1);
  lua_pushinteger(L, module->position(module));
  return 1;
}

static int container_member(lua_State*L)
{
  dt_lib_module_t * module = *(dt_lib_module_t**)lua_touserdata(L, 1);
  dt_ui_container_t container;
  container = module->container(module);
  luaA_push(L, dt_ui_container_t, &container);
  return 1;
}


static int views_member(lua_State*L)
{
  dt_lib_module_t * module = *(dt_lib_module_t**)lua_touserdata(L, 1);
  lua_newtable(L);
  int table_index = 1;
  for(GList *iter = darktable.view_manager->views; iter; iter = g_list_next(iter))
  {
    const dt_view_t *view = (const dt_view_t *)iter->data;
    if(dt_lib_is_visible_in_view(module, view))
    {
      dt_lua_module_entry_push(L, "view", (view->module_name));
      lua_seti(L, -2, table_index);
      table_index++;
    }
  }
  return 1;
}

static int lib_reset(lua_State *L)
{
  dt_lib_module_t *module = *(dt_lib_module_t **)lua_touserdata(L, 1);
  if(module->widget && module->gui_reset)
  {
    module->gui_reset(module);
  }
  return 0;
}

static int lib_tostring(lua_State *L)
{
  dt_lib_module_t *module = *(dt_lib_module_t **)lua_touserdata(L, -1);
  lua_pushstring(L, module->plugin_name);
  return 1;
}

void dt_lua_lib_register(lua_State *L, dt_lib_module_t *module)
{
  dt_lua_module_entry_new_singleton(L, "lib", module->plugin_name, module);
  int my_type = dt_lua_module_entry_get_type(L, "lib", module->plugin_name);
  dt_lua_type_register_parent_type(L, my_type, luaA_type_find(L, "dt_lua_lib_t"));
  lua_pushcfunction(L, lib_tostring);
  dt_lua_type_setmetafield_type(L,my_type,"__tostring");
};

int dt_lua_init_early_lib(lua_State *L)
{

  luaA_enum(L,dt_ui_container_t);
  luaA_enum_value(L, dt_ui_container_t, DT_UI_CONTAINER_PANEL_LEFT_TOP);
  luaA_enum_value(L, dt_ui_container_t, DT_UI_CONTAINER_PANEL_LEFT_CENTER);
  luaA_enum_value(L, dt_ui_container_t, DT_UI_CONTAINER_PANEL_LEFT_BOTTOM);
  luaA_enum_value(L, dt_ui_container_t, DT_UI_CONTAINER_PANEL_RIGHT_TOP);
  luaA_enum_value(L, dt_ui_container_t, DT_UI_CONTAINER_PANEL_RIGHT_CENTER);
  luaA_enum_value(L, dt_ui_container_t, DT_UI_CONTAINER_PANEL_RIGHT_BOTTOM);
  luaA_enum_value(L, dt_ui_container_t, DT_UI_CONTAINER_PANEL_TOP_LEFT);
  luaA_enum_value(L, dt_ui_container_t, DT_UI_CONTAINER_PANEL_TOP_CENTER);
  luaA_enum_value(L, dt_ui_container_t, DT_UI_CONTAINER_PANEL_TOP_RIGHT);
  luaA_enum_value(L, dt_ui_container_t, DT_UI_CONTAINER_PANEL_CENTER_TOP_LEFT);
  luaA_enum_value(L, dt_ui_container_t, DT_UI_CONTAINER_PANEL_CENTER_TOP_CENTER);
  luaA_enum_value(L, dt_ui_container_t, DT_UI_CONTAINER_PANEL_CENTER_TOP_RIGHT);
  luaA_enum_value(L, dt_ui_container_t, DT_UI_CONTAINER_PANEL_CENTER_BOTTOM_LEFT);
  luaA_enum_value(L, dt_ui_container_t, DT_UI_CONTAINER_PANEL_CENTER_BOTTOM_CENTER);
  luaA_enum_value(L, dt_ui_container_t, DT_UI_CONTAINER_PANEL_CENTER_BOTTOM_RIGHT);
  luaA_enum_value(L, dt_ui_container_t, DT_UI_CONTAINER_PANEL_BOTTOM);

  dt_lua_init_type(L, dt_lua_lib_t);
  lua_pushcfunction(L, lib_reset);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const(L, dt_lua_lib_t, "reset");
  lua_pushcfunction(L, version_member);
  dt_lua_type_register_const(L, dt_lua_lib_t, "version");
  lua_pushcfunction(L, id_member);
  dt_lua_type_register_const(L, dt_lua_lib_t, "id");
  lua_pushcfunction(L, name_member);
  dt_lua_type_register_const(L, dt_lua_lib_t, "name");
  lua_pushcfunction(L, expandable_member);
  dt_lua_type_register_const(L, dt_lua_lib_t, "expandable");
  lua_pushcfunction(L, expanded_member);
  dt_lua_gtk_wrap(L);
  dt_lua_type_register(L, dt_lua_lib_t, "expanded");
  lua_pushcfunction(L, position_member);
  dt_lua_type_register_const(L, dt_lua_lib_t, "position");
  lua_pushcfunction(L, container_member);
  dt_lua_type_register_const(L, dt_lua_lib_t, "container");
  lua_pushcfunction(L, views_member);
  dt_lua_type_register_const(L, dt_lua_lib_t, "views");
  lua_pushcfunction(L, visible_member);
  dt_lua_gtk_wrap(L);
  dt_lua_type_register(L, dt_lua_lib_t, "visible");
  lua_pushcfunction(L, on_screen_member);
  dt_lua_type_register_const(L, dt_lua_lib_t, "on_screen");

  dt_lua_module_new(L, "lib"); // special case : will be attached to dt.gui in lua/gui.c:dt_lua_init_gui
  return 0;
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
