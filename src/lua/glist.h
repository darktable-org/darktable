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

#pragma once

#include <glib.h>
#include <lua/lua.h>



// handle list of data who's elementes are type_name* (!!not type_name casted)
#define dt_lua_push_glist(L, list, type_name) dt_lua_push_glist_type(L, list, luaA_type_find(L,type_name))
void dt_lua_push_glist_type(lua_State *L, GList *list, luaA_Type elt_type);

// return a malloced list who's elements are malloced type_name*
#define dt_lua_to_glist(L, type_name, index) dt_lua_to_glist_type(L, luaA_type_find(L,type_name), index)
GList *dt_lua_to_glist_type(lua_State *L, luaA_Type elt_type, int index);



int dt_lua_init_glist(lua_State *L);

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

