/*
   This file is part of darktable,
   Copyright (C) 2013-2020 darktable developers.

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
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "common/darktable.h"
#include "common/file_location.h"
#include "lua/configuration.h"
#include "lua/lua.h"

static int check_version(lua_State *L)
{
  const char *module_name = "<unnamed module>";
  if(!lua_isnil(L,1)) module_name = luaL_checkstring(L, 1);
  gboolean valid = false;
  for(int i = 2; i <= lua_gettop(L); i++)
  {
    lua_pushinteger(L, 1);
    lua_gettable(L, i);
    int major = luaL_checkinteger(L, -1);
    lua_pop(L, 1);

    lua_pushinteger(L, 2);
    lua_gettable(L, i);
    int minor = luaL_checkinteger(L, -1);
    lua_pop(L, 1);

    /*
       patch number is not needed to check for compatibility
       but let's take the good habits
       lua_pushinteger(L,3);
       lua_gettable(L,i);
       int patch= luaL_checkinteger(L,-1);
       lua_pop(L,1);
     */
    if(major == LUA_API_VERSION_MAJOR && minor <= LUA_API_VERSION_MINOR)
    {
      valid = true;
    }
  }
  if(valid)
  {
    // nothing
  }
  else if(strlen(LUA_API_VERSION_SUFFIX) == 0)
  {
    luaL_error(L, "Module %s is not compatible with API %d.%d.%d", module_name, LUA_API_VERSION_MAJOR,
               LUA_API_VERSION_MINOR, LUA_API_VERSION_PATCH);
  }
  else
  {
    dt_print(DT_DEBUG_LUA, "LUA ERROR Module %s is not compatible with API %d.%d.%d-%s\n", module_name,
             LUA_API_VERSION_MAJOR, LUA_API_VERSION_MINOR, LUA_API_VERSION_PATCH, LUA_API_VERSION_SUFFIX);
  }
  return 0;
}


typedef enum
{
  os_windows,
  os_macos,
  os_linux,
  os_unix
} lua_os_type;
#if defined(_WIN32)
static const lua_os_type cur_os = os_windows;
#elif defined(__MACH__) || defined(__APPLE__)
static const lua_os_type cur_os = os_macos;
#elif defined(__linux__)
static const lua_os_type cur_os = os_linux;
#else
static const lua_os_type cur_os = os_unix;
#endif

int dt_lua_init_configuration(lua_State *L)
{
  char tmp_path[PATH_MAX] = { 0 };

  dt_lua_push_darktable_lib(L);

  dt_lua_goto_subtable(L, "configuration");
  // build the table containing the configuration info

  lua_pushstring(L, "tmp_dir");
  dt_loc_get_tmp_dir(tmp_path, sizeof(tmp_path));
  lua_pushstring(L, tmp_path);
  lua_settable(L, -3);

  lua_pushstring(L, "config_dir");
  dt_loc_get_user_config_dir(tmp_path, sizeof(tmp_path));
  lua_pushstring(L, tmp_path);
  lua_settable(L, -3);

  lua_pushstring(L, "cache_dir");
  dt_loc_get_user_cache_dir(tmp_path, sizeof(tmp_path));
  lua_pushstring(L, tmp_path);
  lua_settable(L, -3);

  lua_pushstring(L, "version");
  lua_pushstring(L, darktable_package_version);
  lua_settable(L, -3);

  lua_pushstring(L, "verbose");
  lua_pushboolean(L, darktable.unmuted & DT_DEBUG_LUA);
  lua_settable(L, -3);

  lua_pushstring(L, "has_gui");
  lua_pushboolean(L, darktable.gui != NULL);
  lua_settable(L, -3);

  lua_pushstring(L, "api_version_major");
  lua_pushinteger(L, LUA_API_VERSION_MAJOR);
  lua_settable(L, -3);

  lua_pushstring(L, "api_version_minor");
  lua_pushinteger(L, LUA_API_VERSION_MINOR);
  lua_settable(L, -3);

  lua_pushstring(L, "api_version_patch");
  lua_pushinteger(L, LUA_API_VERSION_PATCH);
  lua_settable(L, -3);

  lua_pushstring(L, "api_version_suffix");
  lua_pushstring(L, LUA_API_VERSION_SUFFIX);
  lua_settable(L, -3);

  lua_pushstring(L, "api_version_string");
  if(LUA_API_VERSION_SUFFIX[0] == '\0')
  {
    lua_pushfstring(L, "%d.%d.%d", LUA_API_VERSION_MAJOR, LUA_API_VERSION_MINOR, LUA_API_VERSION_PATCH);
  }
  else
  {
    lua_pushfstring(L, "%d.%d.%d-%s", LUA_API_VERSION_MAJOR, LUA_API_VERSION_MINOR, LUA_API_VERSION_PATCH,
                    LUA_API_VERSION_SUFFIX);
  }
  lua_settable(L, -3);

  lua_pushstring(L, "check_version");
  lua_pushcfunction(L, check_version);
  lua_settable(L, -3);

  luaA_enum(L, lua_os_type);
  luaA_enum_value_name(L, lua_os_type, os_windows, "windows");
  luaA_enum_value_name(L, lua_os_type, os_macos, "macos");
  luaA_enum_value_name(L, lua_os_type, os_linux, "linux");
  luaA_enum_value_name(L, lua_os_type, os_unix, "unix");
  lua_pushstring(L, "running_os");
  luaA_push(L, lua_os_type, &cur_os);
  lua_settable(L, -3);

  lua_pop(L, 1); // remove the configuration table from the stack
  return 0;
}



// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
