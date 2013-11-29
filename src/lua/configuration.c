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
#include "common/darktable.h"
#include "common/file_location.h"
#include "lua/lua.h"


int dt_lua_init_configuration(lua_State*L)
{
  char tmp_path[PATH_MAX];

  dt_lua_push_darktable_lib(L);

  dt_lua_goto_subtable(L,"configuration");
  // build the table containing the configuration info

  lua_pushstring(L,"tmp_dir");
  dt_loc_get_tmp_dir(tmp_path, PATH_MAX);
  lua_pushstring(L,tmp_path);
  lua_settable(L,-3);

  lua_pushstring(L,"config_dir");
  dt_loc_get_user_config_dir(tmp_path, PATH_MAX);
  lua_pushstring(L,tmp_path);
  lua_settable(L,-3);

  lua_pushstring(L,"cache_dir");
  dt_loc_get_user_cache_dir(tmp_path, PATH_MAX);
  lua_pushstring(L,tmp_path);
  lua_settable(L,-3);

  lua_pushstring(L,"version");
  lua_pushstring(L,PACKAGE_VERSION);
  lua_settable(L,-3);

  lua_pushstring(L,"verbose");
  lua_pushboolean(L,darktable.unmuted & DT_DEBUG_LUA);
  lua_settable(L,-3);

  lua_pushstring(L,"has_gui");
  lua_pushboolean(L,darktable.gui != NULL);
  lua_settable(L,-3);

  lua_pop(L,1); //remove the configuration table from the stack
  return 0;

}



// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
