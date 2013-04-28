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

#include "lua/colorlabels.h"
#include "lua/types.h"
#include "common/colorlabels.h"

/********************************************
  image labels handling
 *******************************************/

static int colorlabel_index(lua_State *L){
  int imgid;
  luaA_to(L,dt_lua_colorlabel_t,&imgid,-2);
  lua_pushboolean(L,dt_colorlabels_check_label(imgid,lua_tointeger(L,-1)));
  return 1;
}

static int colorlabel_newindex(lua_State *L){
  int imgid;
  luaA_to(L,dt_lua_colorlabel_t,&imgid,-3);
  if(lua_toboolean(L,-1)) { // no testing of type so we can benefit from all types of values
    dt_colorlabels_set_label(imgid,lua_tointeger(L,-2));
  } else {
    dt_colorlabels_remove_label(imgid,lua_tointeger(L,-2));
  }
  return 0;
}

static int colorlabel_eq(lua_State*L) {
  int imgid=*((int*)lua_touserdata(L,-1));
  int imgid2=*((int*)lua_touserdata(L,-2));
  lua_pushboolean(L,imgid==imgid2);
  return 1;
}

int dt_lua_init_colorlabels(lua_State * L) {
  dt_lua_init_type(L,dt_lua_colorlabel_t);
  dt_lua_register_type_callback_list(L,dt_lua_colorlabel_t,colorlabel_index,colorlabel_newindex,dt_colorlabels_name);
  luaL_getmetatable(L,"dt_lua_colorlabel_t");
  lua_pushcfunction(L,colorlabel_eq);
	lua_setfield(L,-2,"__eq");
  lua_pop(L,1); //remove metatable

  return 0;
}


// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
