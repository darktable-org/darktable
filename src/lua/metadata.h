/*
   This file is part of darktable,
   Copyright (C) 2026 darktable developers.

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

#include "lua/lua.h"

int dt_lua_init_metadata(lua_State *L);

/** build the tag name of a script's per-image storage key,
 * Xmp.darktable.lua_<script>_<key>, from the script name at
 * script_index and the key at key_index on the lua stack. raises a lua
 * error if either contains anything but letters, digits and underscores.
 * the result must be g_free()d */
char *dt_lua_metadata_script_tagname(lua_State *L,
                                     const int script_index,
                                     const int key_index);

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

