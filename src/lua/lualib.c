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
#include "lua/lualib.h"
#include "control/control.h"
#include "gui/accelerators.h"
#include "libs/lib.h"
#include "lua/widget/widget.h"
#include "views/view.h"

typedef struct position_description_t
{
  const char *view;
  uint32_t container;
  int position;
} position_description_t;

typedef struct
{
  char *name;
  lua_widget widget;
  gboolean expandable;
  GList *position_descriptions;
  const char **views;
} lua_lib_data_t;

static int expandable_wrapper(struct dt_lib_module_t *self)
{
  return ((lua_lib_data_t *)self->data)->expandable;
}

static int version_wrapper()
{
  return 0;
}

static const char *name_wrapper(struct dt_lib_module_t *self)
{
  return ((lua_lib_data_t *)self->data)->name;
}


static void gui_init_wrapper(struct dt_lib_module_t *self)
{
  lua_lib_data_t *gui_data =self->data;
  self->widget = gui_data->widget->widget;
}

static void gui_reset_wrapper(struct dt_lib_module_t *self)
{
  lua_lib_data_t *gui_data =self->data;
  dt_lua_async_call_alien(dt_lua_widget_trigger_callback,
      0, NULL,NULL,
      LUA_ASYNC_TYPENAME,"lua_widget",gui_data->widget,
      LUA_ASYNC_TYPENAME,"const char*","reset",
      LUA_ASYNC_DONE);
}

static void gui_cleanup_wrapper(struct dt_lib_module_t *self)
{
  lua_lib_data_t *gui_data =self->data;
  free(gui_data->name);
  free(gui_data->views);
  g_list_free(gui_data->position_descriptions);
  free(self->data);
  self->widget = NULL;
}


static const char **view_wrapper(struct dt_lib_module_t *self)
{
  lua_lib_data_t *gui_data = self->data;
  return (gui_data->views);
}

static position_description_t *get_position_description(lua_lib_data_t *gui_data, const dt_view_t *cur_view)
{
  for(GList *iter = gui_data->position_descriptions; iter; iter = g_list_next(iter))
  {
    position_description_t *position_description = (position_description_t *)iter->data;
    if(!strcmp(position_description->view, cur_view->module_name)) return position_description;
  }
  return NULL;
}

uint32_t container_wrapper(struct dt_lib_module_t *self)
{
  const dt_view_t *cur_view = dt_view_manager_get_current_view(darktable.view_manager);
  lua_lib_data_t *gui_data = self->data;
  position_description_t *position_description = get_position_description(gui_data, cur_view);
  if(position_description) return position_description->container;
  printf("ERROR in lualib, couldn't find a container for `%s', this should never happen\n", gui_data->name);
  return 0;
}

int position_wrapper(const struct dt_lib_module_t *self)
{
  const dt_view_t *cur_view = dt_view_manager_get_current_view(darktable.view_manager);
  lua_lib_data_t *gui_data = self->data;
  position_description_t *position_description = get_position_description(gui_data, cur_view);
  if(position_description) return position_description->position;
  printf("ERROR in lualib, couldn't find a position for `%s', this should never happen\n", gui_data->name);
  /*
     No position found. This can happen if we are called while current view is not one
     of our views. just return 0
     */
  return 0;
}

static int async_lib_call(lua_State * L)
{
  //lua_getfield(L, LUA_REGISTRYINDEX, "dt_lua_libs");
  const char* event = lua_tostring(L,1);
  dt_lib_module_t * module = *(dt_lib_module_t**)lua_touserdata(L,2);
  dt_lua_module_entry_push(L,"lib",module->plugin_name);
  lua_getiuservalue(L, -1, 1);
  lua_getfield(L,-1,event);
  if(lua_isnoneornil(L,-1)) {
    lua_pop(L,7);
    return 0;
  }
  lua_pushvalue(L,2);
  lua_pushvalue(L,3);
  lua_pushvalue(L,4);
  lua_call(L,3,0);
  lua_pop(L,6);
  return 0;
}
static void view_enter_wrapper(struct dt_lib_module_t *self,struct dt_view_t *old_view,struct dt_view_t *new_view)
{
  dt_lua_async_call_alien(async_lib_call,
      0, NULL,NULL,
      LUA_ASYNC_TYPENAME,"const char*","view_enter",
      LUA_ASYNC_TYPENAME,"dt_lua_lib_t",self,
      LUA_ASYNC_TYPENAME,"dt_lua_view_t",old_view,
      LUA_ASYNC_TYPENAME,"dt_lua_view_t",new_view,
      LUA_ASYNC_DONE);
}

static void view_leave_wrapper(struct dt_lib_module_t *self,struct dt_view_t *old_view,struct dt_view_t *new_view)
{
  dt_lua_async_call_alien(async_lib_call,
      0, NULL,NULL,
      LUA_ASYNC_TYPENAME,"const char*","view_leave",
      LUA_ASYNC_TYPENAME,"dt_lua_lib_t",self,
      LUA_ASYNC_TYPENAME,"dt_lua_view_t",old_view,
      LUA_ASYNC_TYPENAME,"dt_lua_view_t",new_view,
      LUA_ASYNC_DONE);
}

static dt_lib_module_t ref_lib = {
  .module = NULL,
  .data = NULL,
  .plugin_name ={ 0 },
  .widget = NULL,
  .expander = NULL,
  .version = version_wrapper,
  .name = name_wrapper,
  .views = view_wrapper,
  .container = container_wrapper,
  .expandable = expandable_wrapper,
  .init = NULL,
  .gui_init = gui_init_wrapper,
  .gui_cleanup = gui_cleanup_wrapper,
  .gui_reset = gui_reset_wrapper,
  .gui_post_expose = NULL,
  .mouse_leave = NULL,
  .mouse_moved = NULL,
  .button_released = NULL,
  .button_pressed = NULL,
  .scrolled = NULL,
  .configure = NULL,
  .position = position_wrapper,
  .legacy_params = NULL,
  .get_params = NULL,
  .set_params = NULL,
  .init_presets = NULL,
  .reset_button = NULL,
  .presets_button = NULL,
  .view_enter = view_enter_wrapper,
  .view_leave = view_leave_wrapper,
};

static int register_lib(lua_State *L)
{
  dt_lib_module_t *lib = malloc(sizeof(dt_lib_module_t));
  memcpy(lib, &ref_lib, sizeof(dt_lib_module_t));
  lib->data = calloc(1, sizeof(lua_lib_data_t));
  lua_lib_data_t *data = lib->data;

  const char *plugin_name = luaL_checkstring(L, 1);
  g_strlcpy(lib->plugin_name, plugin_name, sizeof(lib->plugin_name));
  dt_lua_lib_register(L, lib);
  /* push the object on the stack to have its metadata */
  dt_lua_module_entry_push(L,"lib",lib->plugin_name);
  lua_getiuservalue(L, -1, 1);
  lua_pushvalue(L, 1);
  lua_setfield(L, -2, "plugin_name");
  const char *name = luaL_checkstring(L, 2);
  lua_pushvalue(L, 2);
  lua_setfield(L, -2, "name");
  data->name = strdup(name);
  data->widget = NULL;

  luaL_checktype(L,3,LUA_TBOOLEAN);
  data->expandable = lua_toboolean(L,3);

  luaL_checktype(L,4,LUA_TBOOLEAN);
  if(!lua_toboolean(L,4)) {
    lib->gui_reset = NULL;
  }

  luaL_checktype(L, 5, LUA_TTABLE);
  lua_pushnil(L);
  while(lua_next(L, 5))
  {
    dt_view_t *tmp_view;
    luaA_to(L, dt_lua_view_t, &tmp_view, -2);

    luaL_checktype(L, -1, LUA_TTABLE);
    position_description_t *position_description = malloc(sizeof(position_description_t));
    data->position_descriptions = g_list_append(data->position_descriptions, position_description);

    position_description->view = tmp_view->module_name;

    // get the container
    lua_pushinteger(L,1);
    lua_gettable(L,-2);
    dt_ui_container_t container;
    luaA_to(L,dt_ui_container_t,&container,-1);
    lua_pop(L,1);
    position_description->container = container;

    // get the position
    lua_pushinteger(L,2);
    lua_gettable(L,-2);
    position_description->position = luaL_checkinteger(L,-1);
    lua_pop(L,1);

    lua_pop(L, 1);
  }
  data->views = calloc(g_list_length(data->position_descriptions) + 1, sizeof(char *));
  int i = 0;
  for(GList *iter = data->position_descriptions; iter; iter = g_list_next(iter))
  {
    position_description_t *position_description = (position_description_t *)iter->data;
    data->views[i++] = position_description->view;
  }

  lua_widget widget;
  luaA_to(L,lua_widget,&widget,6);
  dt_lua_widget_bind(L,widget);
  data->widget = widget;

  if(lua_isfunction(L,7)) {
    lua_pushvalue(L, 7);
    lua_setfield(L, -2, "view_enter");
  } else {
    lib->view_enter = NULL;
  }

  if(lua_isfunction(L,8)) {
    lua_pushvalue(L, 8);
    lua_setfield(L, -2, "view_leave");
  } else {
    lib->view_leave = NULL;
  }

  lua_pop(L,2);

  if(lib->init) lib->init(lib);

  lib->gui_init(lib);
  if(lib->widget) g_object_ref(lib->widget);

  darktable.lib->plugins = g_list_insert_sorted(darktable.lib->plugins, lib, dt_lib_sort_plugins);
  dt_lib_init_presets(lib);

  dt_view_manager_switch_by_view(darktable.view_manager, dt_view_manager_get_current_view(darktable.view_manager));
  return 0;
}



int dt_lua_init_lualib(lua_State *L)
{
  dt_lua_push_darktable_lib(L);
  lua_pushstring(L, "register_lib");
  lua_pushcfunction(L, &register_lib);
  dt_lua_gtk_wrap(L);
  lua_settable(L, -3);
  lua_pop(L, 1);

  return 0;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
