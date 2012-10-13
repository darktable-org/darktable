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
#include "lua/gui.h"
#include "lua/image.h"

/***********************************************************************
  Creating the images global variable
 **********************************************************************/

static int selection_cb(lua_State *L) {
	GList *image = dt_collection_get_selected(darktable.collection);
	if(lua_gettop(L) > 0) {
		dt_selection_clear(darktable.selection);
		luaL_checktype(L,-1,LUA_TTABLE);
		lua_pushnil(L);
		while (lua_next(L, -2) != 0) {
			/* uses 'key' (at index -2) and 'value' (at index -1) */
			int imgid = dt_lua_image_get(L,-1);
			dt_selection_toggle(darktable.selection,imgid);

			lua_pop(L,1);
		}
	}
	lua_newtable(L);
	while(image){
		dt_lua_image_push(L,(long int)image->data);
		luaL_ref(L,-2);
		image = g_list_delete_link(image, image);
	}
	return 1;
}


int dt_lua_init_gui(lua_State * L) {
	
  /* images */
  dt_lua_push_darktable_lib(L);
  dt_lua_goto_subtable(L,"gui");
  lua_pushcfunction(L,selection_cb);
  lua_setfield(L,-2,"selection");
  return 0;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
