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

#include <stdlib.h>
#include "lua/history.h"
#include "lua/types.h"
#include "common/history.h"

static int history_item_tostring(lua_State*L) {
	dt_history_item_t * item =luaL_checkudata(L,-1,"dt_history_item_t");
	lua_pushfstring(L,"%d : %s (%s)",item->num,item->name,item->op);
  return 1;
}

static int history_item_gc(lua_State*L) {
	dt_history_item_t * item =luaL_checkudata(L,-1,"dt_history_item_t");
	free(item->name);
	free(item->op);
	return 0;
}


int dt_lua_init_history(lua_State * L) {
  /* history */
  dt_lua_init_type(L,dt_history_item_t);
  luaA_struct(L,dt_history_item_t);
  luaA_struct_member(L,dt_history_item_t,num,const int);
  luaA_struct_member(L,dt_history_item_t,op,const char*);
  luaA_struct_member(L,dt_history_item_t,name,const char*);

  dt_lua_register_type_callback_type(L,dt_history_item_t,NULL,NULL,dt_history_item_t);

  luaL_getmetatable(L,"dt_history_item_t");
  lua_pushcfunction(L,history_item_gc);
  lua_setfield(L,-2,"__gc");
  lua_pushcfunction(L,history_item_tostring);
  lua_setfield(L,-2,"__tostring");
  lua_pop(L,1); //remove metatable
  return 0;
}


// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
