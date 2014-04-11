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
#include <glib.h>
#include "common/collection.h"
#include "common/selection.h"
#include "common/darktable.h"
#include "control/control.h"
#include "control/settings.h"
#include "lua/gui.h"
#include "lua/image.h"
#include "lua/types.h"

/***********************************************************************
  Creating the images global variable
 **********************************************************************/

static int selection_cb(lua_State *L)
{
  GList *image = dt_collection_get_selected(darktable.collection, -1);
  if(lua_gettop(L) > 0)
  {
    GList * new_selection = NULL;
    luaL_checktype(L,-1,LUA_TTABLE);
    lua_pushnil(L);
    while (lua_next(L, -2) != 0)
    {
      /* uses 'key' (at index -2) and 'value' (at index -1) */
      int imgid;
      luaA_to(L,dt_lua_image_t,&imgid,-1);
      new_selection = g_list_prepend(new_selection,GINT_TO_POINTER(imgid));
      lua_pop(L,1);
    }
    new_selection = g_list_reverse(new_selection);
    dt_lua_unlock(true);// we need the gdk lock to update ui information
    dt_selection_clear(darktable.selection);
    dt_selection_select_list(darktable.selection,new_selection);
    dt_lua_lock();
    g_list_free(new_selection);
  }
  lua_newtable(L);
  while(image)
  {
    luaA_push(L,dt_lua_image_t,&image->data);
    luaL_ref(L,-2);
    image = g_list_delete_link(image, image);
  }
  return 1;
}

static int hovered_cb(lua_State *L)
{
  int32_t mouse_over_id = dt_control_get_mouse_over_id();
  if(mouse_over_id ==-1) {
    lua_pushnil(L);
  } else {
    luaA_push(L,dt_lua_image_t,&mouse_over_id);
  }
  return 1;
}

static int act_on_cb(lua_State *L)
{

  int32_t imgid = dt_view_get_image_to_act_on();
  lua_newtable(L);
  if(imgid != -1) {
    luaA_push(L,dt_lua_image_t,&imgid);
    luaL_ref(L,-2);
    return 1;
  } else {
    GList *image = dt_collection_get_selected(darktable.collection, -1);
    while(image)
    {
      luaA_push(L,dt_lua_image_t,&image->data);
      luaL_ref(L,-2);
      image = g_list_delete_link(image, image);
    }
    return 1;
  }
}


static int current_view_cb(lua_State *L)
{
  if(lua_gettop(L) > 0)
  {
    luaL_argcheck(L,dt_lua_isa(L,1,dt_view_t),1,"dt_view_t expected");
    dt_view_t * module = *(dt_view_t**)lua_touserdata(L,1);
    printf("switch to %d\n",module->view(module));
    int i = 0;
    while(i< darktable.view_manager->num_views && module != &darktable.view_manager->view[i]) i++;
    if(i == darktable.view_manager->num_views) return luaL_error(L,"should never happen : %s %d\n",__FILE__,__LINE__);
    dt_ctl_switch_mode_to(i);
  }
  const dt_view_t* current_view = dt_view_manager_get_current_view(darktable.view_manager);
  dt_lua_module_push_entry(L,"view",current_view->module_name);
  return 1;
}

int dt_lua_init_gui(lua_State * L)
{

  /* images */
  dt_lua_push_darktable_lib(L);
  luaA_Type type_id = dt_lua_init_singleton(L,"gui_lib",NULL);
  lua_setfield(L,-2,"gui");
  lua_pop(L,1);

  luaA_enum(L,dt_ui_container_t);
  luaA_enum_value(L,dt_ui_container_t,DT_UI_CONTAINER_PANEL_LEFT_TOP,false);
  luaA_enum_value(L,dt_ui_container_t,DT_UI_CONTAINER_PANEL_LEFT_CENTER,false);
  luaA_enum_value(L,dt_ui_container_t,DT_UI_CONTAINER_PANEL_LEFT_BOTTOM,false);
  luaA_enum_value(L,dt_ui_container_t,DT_UI_CONTAINER_PANEL_RIGHT_TOP,false);
  luaA_enum_value(L,dt_ui_container_t,DT_UI_CONTAINER_PANEL_RIGHT_CENTER,false);
  luaA_enum_value(L,dt_ui_container_t,DT_UI_CONTAINER_PANEL_RIGHT_BOTTOM,false);
  luaA_enum_value(L,dt_ui_container_t,DT_UI_CONTAINER_PANEL_TOP_LEFT,false);
  luaA_enum_value(L,dt_ui_container_t,DT_UI_CONTAINER_PANEL_TOP_CENTER,false);
  luaA_enum_value(L,dt_ui_container_t,DT_UI_CONTAINER_PANEL_TOP_RIGHT,false);
  luaA_enum_value(L,dt_ui_container_t,DT_UI_CONTAINER_PANEL_CENTER_TOP_LEFT,false);
  luaA_enum_value(L,dt_ui_container_t,DT_UI_CONTAINER_PANEL_CENTER_TOP_CENTER,false);
  luaA_enum_value(L,dt_ui_container_t,DT_UI_CONTAINER_PANEL_CENTER_TOP_RIGHT,false);
  luaA_enum_value(L,dt_ui_container_t,DT_UI_CONTAINER_PANEL_CENTER_BOTTOM_LEFT,false);
  luaA_enum_value(L,dt_ui_container_t,DT_UI_CONTAINER_PANEL_CENTER_BOTTOM_CENTER,false);
  luaA_enum_value(L,dt_ui_container_t,DT_UI_CONTAINER_PANEL_CENTER_BOTTOM_RIGHT,false);
  luaA_enum_value(L,dt_ui_container_t,DT_UI_CONTAINER_PANEL_BOTTOM,false);

  lua_pushcfunction(L,selection_cb);
  dt_lua_register_type_callback_stack_typeid(L,type_id,"selection");
  dt_lua_register_type_callback_typeid(L,type_id,hovered_cb,NULL,"hovered",NULL);
  dt_lua_register_type_callback_typeid(L,type_id,act_on_cb,NULL,"action_images",NULL);
  lua_pushcfunction(L,current_view_cb);
  dt_lua_register_type_callback_stack_typeid(L,type_id,"current_view");
  return 0;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
