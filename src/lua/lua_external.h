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
#ifndef LUA_LUA_EXTERNAL_H
#define LUA_LUA_EXTERNAL_H

/* this file can safely be included when lua is disabled */

#ifdef USE_LUA
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
#include <lautoc.h>

void dt_lua_init(lua_State*L,const int init_gui);
void dt_lua_init_early(lua_State*L);

#else
/* defines to easily have a few lua types when lua is not available */
typedef struct lua_State lua_State;
typedef int (*lua_CFunction)(lua_State *L);
typedef int luaA_Type;
#define LUAA_INVALID_TYPE -1


inline void dt_lua_init(lua_State*L,const int init_gui){};
inline void dt_lua_init_early(lua_State*L){};

#endif



#endif
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;

