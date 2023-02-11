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

#include "common/styles.h"
#include <lua/lua.h>

// initializes dt_style_t

/**
  (0,+1)
  takes an image, a name, an optional description and creates a style. returns the new style object
  used by dt_lua_image_t
  */
int dt_lua_style_create_from_image(lua_State *L);

/**
  (-2,0)
  takes an image and a style in any order, apply the style to the image
  */
int dt_lua_style_apply(lua_State *L);

int dt_lua_init_styles(lua_State *L);

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
