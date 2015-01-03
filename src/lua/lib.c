/*
   This file is part of darktable,
   copyright (c) 2012 Jeremy Rosen

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
#include "lua/modules.h"
#include "lua/types.h"
#include "gui/gtk.h"

static int expanded_member(lua_State *L)
{
  dt_lib_module_t *module = *(dt_lib_module_t **)lua_touserdata(L, 1);
  if(lua_gettop(L) != 3)
  {
    lua_pushboolean(L, module->expandable());
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

gboolean dt_lua_lib_check(lua_State *L, struct dt_lib_module_t *self)
{
  return (self->widget != NULL);
}

void dt_lua_lib_check_error(lua_State *L, struct dt_lib_module_t *self)
{
  if(!self->widget)
  {
    luaL_error(L, "Attempt to access a non-visible module");
  }
}

static int name_member(lua_State *L)
{
  dt_lib_module_t *module = *(dt_lib_module_t **)lua_touserdata(L, 1);
  lua_pushstring(L, module->name());
  return 1;
}

static int expandable_member(lua_State *L)
{
  dt_lib_module_t *module = *(dt_lib_module_t **)lua_touserdata(L, 1);
  lua_pushboolean(L, module->expandable());
  return 1;
}

static int on_screen_member(lua_State *L)
{
  dt_lib_module_t *module = *(dt_lib_module_t **)lua_touserdata(L, 1);
  lua_pushboolean(L, module->widget != NULL);
  return 1;
}

#if 0
static int position_member(lua_State*L) {
  dt_lib_module_t * module = *(dt_lib_module_t**)lua_touserdata(L,1);
  lua_pushinteger(L,module->position());
  return 1;
}

static int container_member(lua_State*L) {
  dt_lib_module_t * module = *(dt_lib_module_t**)lua_touserdata(L,1);
  dt_ui_container_t container;
  container = module->container();
  luaA_push(L,dt_ui_container_t,&container);
  return 1;
}


static int views_member(lua_State*L) {
  dt_lib_module_t * module = *(dt_lib_module_t**)lua_touserdata(L,1);
  int i;
  lua_newtable(L);
  for(i=0; i<  darktable.view_manager->num_views ; i++) {
    if(darktable.view_manager->view[i].view(&darktable.view_manager->view[i]) & module->views()){
      dt_lua_module_entry_push(L,"view",(darktable.view_manager->view[i].module_name));
      luaL_ref(L,-2);
    }
  }
  return 1;
}

#endif
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
  dt_lua_type_register_parent_type(L, my_type, luaA_type_find(L, "dt_lib_module_t"));
  lua_pushcfunction(L, lib_tostring);
  dt_lua_type_setmetafield_type(L,my_type,"__tostring");
};

int dt_lua_init_early_lib(lua_State *L)
{

#if 0
  luaA_enum(L,dt_ui_container_t);
  luaA_enum_value(L,dt_ui_container_t,DT_UI_CONTAINER_PANEL_LEFT_TOP);
  luaA_enum_value(L,dt_ui_container_t,DT_UI_CONTAINER_PANEL_LEFT_CENTER);
  luaA_enum_value(L,dt_ui_container_t,DT_UI_CONTAINER_PANEL_LEFT_BOTTOM);
  luaA_enum_value(L,dt_ui_container_t,DT_UI_CONTAINER_PANEL_RIGHT_TOP);
  luaA_enum_value(L,dt_ui_container_t,DT_UI_CONTAINER_PANEL_RIGHT_CENTER);
  luaA_enum_value(L,dt_ui_container_t,DT_UI_CONTAINER_PANEL_RIGHT_BOTTOM);
  luaA_enum_value(L,dt_ui_container_t,DT_UI_CONTAINER_PANEL_TOP_LEFT);
  luaA_enum_value(L,dt_ui_container_t,DT_UI_CONTAINER_PANEL_TOP_CENTER);
  luaA_enum_value(L,dt_ui_container_t,DT_UI_CONTAINER_PANEL_TOP_RIGHT);
  luaA_enum_value(L,dt_ui_container_t,DT_UI_CONTAINER_PANEL_CENTER_TOP_LEFT);
  luaA_enum_value(L,dt_ui_container_t,DT_UI_CONTAINER_PANEL_CENTER_TOP_CENTER);
  luaA_enum_value(L,dt_ui_container_t,DT_UI_CONTAINER_PANEL_CENTER_TOP_RIGHT);
  luaA_enum_value(L,dt_ui_container_t,DT_UI_CONTAINER_PANEL_CENTER_BOTTOM_LEFT);
  luaA_enum_value(L,dt_ui_container_t,DT_UI_CONTAINER_PANEL_CENTER_BOTTOM_CENTER);
  luaA_enum_value(L,dt_ui_container_t,DT_UI_CONTAINER_PANEL_CENTER_BOTTOM_RIGHT);
  luaA_enum_value(L,dt_ui_container_t,DT_UI_CONTAINER_PANEL_BOTTOM);
#endif

  dt_lua_init_type(L, dt_lib_module_t);
  lua_pushcfunction(L, lib_reset);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const(L, dt_lib_module_t, "reset");
  lua_pushcfunction(L, version_member);
  dt_lua_type_register_const(L, dt_lib_module_t, "version");
  lua_pushcfunction(L, id_member);
  dt_lua_type_register_const(L, dt_lib_module_t, "id");
  lua_pushcfunction(L, name_member);
  dt_lua_type_register_const(L, dt_lib_module_t, "name");
  lua_pushcfunction(L, expandable_member);
  dt_lua_type_register_const(L, dt_lib_module_t, "expandable");
  lua_pushcfunction(L, expanded_member);
  lua_pushcclosure(L,dt_lua_gtk_wrap,1);
  dt_lua_type_register(L, dt_lib_module_t, "expanded");
#if 0
  lua_pushcfunction(L,position_member);
  dt_lua_type_register_const(L,dt_lib_module_t,"position");
  lua_pushcfunction(L,container_member);
  dt_lua_type_register_const(L,dt_lib_module_t,"container");
  lua_pushcfunction(L,views_member);
  dt_lua_type_register_const(L,dt_lib_module_t,"views");
#endif
  lua_pushcfunction(L, visible_member);
  lua_pushcclosure(L,dt_lua_gtk_wrap,1);
  dt_lua_type_register(L, dt_lib_module_t, "visible");
  lua_pushcfunction(L, on_screen_member);
  dt_lua_type_register_const(L, dt_lib_module_t, "on_screen");

  dt_lua_module_new(L, "lib"); // special case : will be attached to dt.gui in lua/gui.c:dt_lua_init_gui
  return 0;
}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
