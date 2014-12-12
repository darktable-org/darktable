/*
    This file is part of darktable,
    copyright (c) 2013 Jeremy Rosen

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
#ifndef DT_LUA_LIB_H
#define DT_LUA_LIB_H
#include <lua/lua.h>
#include "libs/lib.h"


struct dt_lib_module_t;

void dt_lua_lib_register(lua_State *L, struct dt_lib_module_t *self);

void dt_lua_lib_check_error(lua_State *L, struct dt_lib_module_t *self);
gboolean dt_lua_lib_check(lua_State *L, struct dt_lib_module_t *self);



int dt_lua_init_early_lib(lua_State *L);

#endif
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
