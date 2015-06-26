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
#include "lua/lualib.h"
#include "lua/widget/widget.h"
#include "libs/lib.h"
#include "views/view.h"
#include "gui/accelerators.h"
#include "control/control.h"


typedef struct
{
  char *name;
  lua_widget widget;
  gboolean expandable;
  struct {
    dt_view_type_flags_t view;
    uint32_t container;
    int position;
  }  position_description[DT_VIEW_MAX_MODULES];
  uint32_t views;
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
  dt_lua_do_chunk_async(dt_lua_widget_trigger_callback,
      LUA_ASYNC_TYPENAME,"lua_widget",gui_data->widget,
      LUA_ASYNC_TYPENAME,"const char*","reset",
      LUA_ASYNC_DONE);
}

static void gui_cleanup_wrapper(struct dt_lib_module_t *self)
{
  self->widget = NULL;
}


uint32_t view_wrapper(struct dt_lib_module_t *self)
{
  lua_lib_data_t *gui_data =self->data;
  return gui_data->views;
}

uint32_t container_wrapper(struct dt_lib_module_t *self)
{
  const dt_view_t *cv = dt_view_manager_get_current_view(darktable.view_manager);
  uint32_t cur_view = cv->view(cv);
  lua_lib_data_t *gui_data =self->data;
  for(int index = 0 ;index < DT_VIEW_MAX_MODULES; index++) {
    if( gui_data->position_description[index].view == cur_view) {
      return gui_data->position_description[index].container;
    }
  }
  printf("ERROR in lualib, couldn't find a container, this should never happen\n");
  return 0;
}

int position_wrapper(struct dt_lib_module_t *self)
{
  const dt_view_t *cv = dt_view_manager_get_current_view(darktable.view_manager);
  uint32_t cur_view = cv->view(cv);
  lua_lib_data_t *gui_data =self->data;
  for(int index = 0 ;index < DT_VIEW_MAX_MODULES; index++) {
    if( gui_data->position_description[index].view == cur_view) {
      return gui_data->position_description[index].position;
    }
  }
  printf("ERROR in lualib, couldn't find a position, this should never happen\n");
  return 0;
}

static int async_lib_call(lua_State * L)
{
  //lua_getfield(L, LUA_REGISTRYINDEX, "dt_lua_libs");
  const char* event = lua_tostring(L,1);
  dt_lib_module_t * module = *(dt_lib_module_t**)lua_touserdata(L,2);
  dt_lua_module_entry_push(L,"lib",module->plugin_name);
  lua_getuservalue(L,-1);
  lua_getfield(L,-1,event);
  if(lua_isnoneornil(L,-1)) {
    return 0;
  }
  lua_pushvalue(L,2);
  lua_pushvalue(L,3);
  lua_pushvalue(L,4);
  dt_lua_do_chunk_raise(L,3,0);
  return 0;
}
static void view_enter_wrapper(struct dt_lib_module_t *self,struct dt_view_t *old_view,struct dt_view_t *new_view)
{
  dt_lua_do_chunk_async(async_lib_call,
      LUA_ASYNC_TYPENAME,"const char*","view_enter",
      LUA_ASYNC_TYPENAME,"dt_lua_lib_t",self,
      LUA_ASYNC_TYPENAME,"dt_lua_view_t",old_view,
      LUA_ASYNC_TYPENAME,"dt_lua_view_t",new_view,
      LUA_ASYNC_DONE);
}

static void view_leave_wrapper(struct dt_lib_module_t *self,struct dt_view_t *old_view,struct dt_view_t *new_view)
{
  dt_lua_do_chunk_async(async_lib_call,
      LUA_ASYNC_TYPENAME,"const char*","view_leave",
      LUA_ASYNC_TYPENAME,"dt_lua_lib_t",self,
      LUA_ASYNC_TYPENAME,"dt_lua_view_t",old_view,
      LUA_ASYNC_TYPENAME,"dt_lua_view_t",new_view,
      LUA_ASYNC_DONE);
}

static dt_lib_module_t ref_lib = {
  .module = NULL,
  .dt = &darktable,
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
  .init_key_accels = NULL,
  .connect_key_accels = NULL,
  .accel_closures = NULL,
  .reset_button = NULL,
  .presets_button = NULL,
  .view_enter = view_enter_wrapper,
  .view_leave = view_leave_wrapper,
};

static int register_lib(lua_State *L)
{
  dt_lib_module_t *lib = malloc(sizeof(dt_lib_module_t));
  memcpy(lib, &ref_lib, sizeof(dt_lib_module_t));
  lib->data = malloc(sizeof(lua_lib_data_t));
  lua_lib_data_t *data = lib->data;
  memset(data,0,sizeof(lua_lib_data_t));

  const char *plugin_name = luaL_checkstring(L, 1);
  g_strlcpy(lib->plugin_name, plugin_name, sizeof(lib->plugin_name));
  dt_lua_lib_register(L, lib);
  /* push the object on the stack to have its metadata */
  dt_lua_module_entry_push(L,"lib",lib->plugin_name);
  lua_getuservalue(L,-1);
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

  luaL_checktype(L,5,LUA_TTABLE);
  uint32_t views = 0;
  int nb_views =0;
  lua_pushnil(L);
  while(lua_next(L,5)) {
    dt_view_t *tmp_view;
    luaA_to(L,dt_lua_view_t,&tmp_view,-2);
    dt_view_type_flags_t view = darktable.view_manager->view[0].view(tmp_view);

    views |= view;

    luaL_checktype(L,-1,LUA_TTABLE);
    data->position_description[nb_views].view = view;

    // get the container
    lua_pushinteger(L,1);
    lua_gettable(L,-2);
    dt_ui_container_t container;
    luaA_to(L,dt_ui_container_t,&container,-1); 
    lua_pop(L,1);
    data->position_description[nb_views].container = container;

    // get the position
    lua_pushinteger(L,2);
    lua_gettable(L,-2);
    data->position_description[nb_views].position = luaL_checkinteger(L,-1);
    lua_pop(L,1);


    nb_views++;
    lua_pop(L,1);
  }
  data->views = views;

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



 
  if(lib->gui_reset)
  {
    dt_accel_register_lib(lib, NC_("accel", "reset lib parameters"), 0, 0);
  }
  if(lib->init) lib->init(lib);

  lib->gui_init(lib);
  if(lib->widget) g_object_ref(lib->widget);

  darktable.lib->plugins = g_list_insert_sorted(darktable.lib->plugins, lib, dt_lib_sort_plugins);
  dt_lib_init_presets(lib);
  if(darktable.gui && lib->init_key_accels) lib->init_key_accels(lib);

  dt_control_gui_mode_t oldmode = dt_conf_get_int("ui_last/view");
  dt_view_manager_switch(darktable.view_manager, oldmode);
  return 0;
}



int dt_lua_init_lualib(lua_State *L)
{
  dt_lua_push_darktable_lib(L);
  lua_pushstring(L, "register_lib");
  lua_pushcfunction(L, &register_lib);
  lua_pushcclosure(L,dt_lua_gtk_wrap,1);
  lua_settable(L, -3);
  lua_pop(L, 1);

  return 0;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
