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

/*
 * LUA API VERSIONNING
 * This API versionning follows sementic versionning as defined in
 * http://semver.org
 * only stable releases are considered "released"
   => no need to increase API version with every commit
   however, beware of stable releases and API changes
 */
// LAST RELEASED VERSION : 1.4 was 1.0.0
/* incompatible API change */
#define API_VERSION_MAJOR  1
/* backward compatible API change */
#define API_VERSION_MINOR  1
/* bugfixes that should not change anything to the API */
#define API_VERSION_PATCH  0
/* suffix for unstable version */
#define API_VERSION_SUFFIX "-dev"

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

  lua_pushstring(L,"api_version_major");
  lua_pushnumber(L,API_VERSION_MAJOR);
  lua_settable(L,-3);

  lua_pushstring(L,"api_version_minor");
  lua_pushnumber(L,API_VERSION_MINOR);
  lua_settable(L,-3);

  lua_pushstring(L,"api_version_patch");
  lua_pushnumber(L,API_VERSION_PATCH);
  lua_settable(L,-3);

  lua_pushstring(L,"api_version_suffix");
  lua_pushstring(L,API_VERSION_SUFFIX);
  lua_settable(L,-3);

  lua_pushstring(L,"api_version_string");
  if (strcmp(API_VERSION_SUFFIX, "") == 0) {
    lua_pushfstring(L,"%d.%d.%d",API_VERSION_MAJOR,API_VERSION_MINOR,API_VERSION_PATCH);
  } else {
    lua_pushfstring(L,"%d.%d.%d-%s",API_VERSION_MAJOR,API_VERSION_MINOR,API_VERSION_PATCH,API_VERSION_SUFFIX);
  }
  lua_settable(L,-3);

  lua_pop(L,1); //remove the configuration table from the stack
  return 0;

}



// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
