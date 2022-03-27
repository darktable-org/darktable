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

#include "lua/glist.h"
#include <lauxlib.h>
#include <stdlib.h>

void dt_lua_push_glist_type(lua_State *L, GList *list, luaA_Type elt_type)
{
  lua_newtable(L);
  int index_table = 1;
  for(const GList *elt = list; elt; elt = g_list_next(elt))
  {
    luaA_push_type(L, elt_type, elt->data);
    lua_seti(L, -2, index_table);
    index_table++;
  }
}

GList *dt_lua_to_glist_type(lua_State *L, luaA_Type elt_type, int index)
{
  // recreate list of images
  GList *list = NULL;
  size_t type_size = luaA_typesize(L, elt_type);
  lua_pushnil(L); /* first key */
  while(lua_next(L, index - 1) != 0)
  {
    /* uses 'key' (at index -2) and 'value' (at index -1) */
    void *obj = malloc(type_size);
    luaA_to_type(L, elt_type, obj, -1);
    lua_pop(L, 1);
    list = g_list_prepend(list, (gpointer)obj);
  }
  list = g_list_reverse(list);
  return list;
}

int dt_lua_init_glist(lua_State *L)
{
  return 0;
}


// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

