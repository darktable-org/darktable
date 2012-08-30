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
#ifndef DT_LUA_STMT_H
#define DT_LUA_STMT_H
#include <sqlite3.h>
#include "lua/dt_lua.h"
#include "lua/types.h"

/*
lua type for a sqlite3_stmt that will be auto-finalized on __gc
	
   */

void dt_lua_stmt_push(lua_State * L,sqlite3_stmt *stmt);
sqlite3_stmt* dt_lua_stmt_check(lua_State * L,int index);
int dt_lua_init_stmt(lua_State * L);

#endif

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
