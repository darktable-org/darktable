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
    dt_selection_clear(darktable.selection);
    luaL_checktype(L,-1,LUA_TTABLE);
    lua_pushnil(L);
    while (lua_next(L, -2) != 0)
    {
      /* uses 'key' (at index -2) and 'value' (at index -1) */
      int imgid;
      luaA_to(L,dt_lua_image_t,&imgid,-1);
      dt_selection_toggle(darktable.selection,imgid);

      lua_pop(L,1);
    }
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
  int32_t mouse_over_id = -1;
  DT_CTL_GET_GLOBAL(mouse_over_id, lib_image_mouse_over_id);
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


int dt_lua_init_gui(lua_State * L)
{

  /* images */
  dt_lua_push_darktable_lib(L);
  luaA_Type type_id = dt_lua_init_singleton(L,"gui_lib");
  lua_setfield(L,-2,"gui");
  lua_pop(L,1);

  lua_pushcfunction(L,selection_cb);
  dt_lua_register_type_callback_stack_typeid(L,type_id,"selection");
  dt_lua_register_type_callback_typeid(L,type_id,hovered_cb,NULL,"hovered",NULL);
  dt_lua_register_type_callback_typeid(L,type_id,act_on_cb,NULL,"action_images",NULL);
  return 0;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
