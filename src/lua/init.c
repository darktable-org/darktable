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
#include "lua/lua.h"
#include "lua/init.h"
#include "common/darktable.h"


// closed on GC of the dt lib, usually when the lua interpreter closes
static int dt_luacleanup(lua_State*L) {
  const int init_gui = (darktable.gui != NULL);
  if(!init_gui)
    dt_cleanup();
  return 0;
}


/**
  hardcoded list of types to register
  other types can be added dynamically
 */
static lua_CFunction init_funcs[] = {
  NULL
};


void dt_lua_init_early(lua_State*L){
  if(!L)
    L= luaL_newstate();
  darktable.lua_state= L;
  luaL_openlibs(darktable.lua_state);
  dt_lua_push_darktable_lib(L);
  // set the metatable
  lua_newtable(L);
  lua_pushcfunction(L,dt_luacleanup);
  lua_setfield(L,-2,"__gc");
  lua_setmetatable(L,-2);

  lua_pop(L,1);


}


void dt_lua_init(lua_State*L,const int init_gui){

  // init the lua environment
  lua_CFunction* cur_type = init_funcs;
  while(*cur_type) {
    (*cur_type)(L);
    cur_type++;
  }
  dt_lua_push_darktable_lib(L);
  // build the table containing the configuration info 
  
  lua_getglobal(L,"package");
  dt_lua_goto_subtable(L,"loaded");
  lua_pushstring(L,"darktable");
  dt_lua_push_darktable_lib(L);
  lua_settable(L,-3);
  lua_pop(L,1);

}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
