/*
   This file is part of darktable,
   Copyright (C) 2013-2021 darktable developers.

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

#pragma once

#include <lua/lua.h>

/*
 * LUA API VERSIONING
 * This API versioning follows semantic versioning as defined in
 * http://semver.org
 * only stable releases are considered "released"
 *   => no need to increase API version with every commit,
 *   however, beware of stable releases and API changes
 */
// 1.6 was 2.0.1
// 1.6.1 was 2.0.2
// 2.0.0 was 3.0.0
// 2.2.0 was 4.0.0 ( removed the ugly yield functions make scripts incompatible)
// 2.4.0 was 5.0.0 (going to lua 5.3 is a major API bump)
// 3.2.0 was 6.0.0 (removed facebook, flickr, and picasa from types.dt_imageio_storage_module_t)
// 3.6.0 was 7.0.0 (added naming to events, selections, and actions)
// 3.8.0 was 8.0.0 (moved to lua 5.4 and added some events)
// 4.2.0 was 9.0.0 (view toolbox functions and snapshot filename removed)
/* incompatible API change */
#define LUA_API_VERSION_MAJOR 9
/* backward compatible API change */
#define LUA_API_VERSION_MINOR 0
/* bugfixes that should not change anything to the API */
#define LUA_API_VERSION_PATCH 0
/* suffix for unstable version */
#define LUA_API_VERSION_SUFFIX ""

/** initialize lua stuff at DT start time */
int dt_lua_init_configuration(lua_State *L);

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
