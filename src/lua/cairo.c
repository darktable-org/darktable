/*
 *   This file is part of darktable,
 *   Copyright (C) 2015-2020 darktable developers.
 *
 *   darktable is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   darktable is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with darktable.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "lua/cairo.h"
#include "common/darktable.h"
#include "gui/draw.h"
#include "lua/call.h"
#include "lua/lua.h"
#include "lua/types.h"


static int _draw_line(lua_State *L)
{
  cairo_t *cr;
  luaA_to(L, dt_lua_cairo_t, &cr, 1);
  lua_Number left = luaL_checknumber(L, 2);
  lua_Number top = luaL_checknumber(L, 3);
  lua_Number right = luaL_checknumber(L, 4);
  lua_Number bottom = luaL_checknumber(L, 5);

  dt_draw_line(cr, left, top, right, bottom);

  return 0;
}

static int _save(lua_State *L)
{
  cairo_t *cr;
  luaA_to(L, dt_lua_cairo_t, &cr, 1);

  cairo_save(cr);

  return 0;
}

static int _restore(lua_State *L)
{
  cairo_t *cr;
  luaA_to(L, dt_lua_cairo_t, &cr, 1);

  cairo_restore(cr);

  return 0;
}

static int _new_sub_path(lua_State *L)
{
  cairo_t *cr;
  luaA_to(L, dt_lua_cairo_t, &cr, 1);

  cairo_new_sub_path(cr);

  return 0;
}

static int _scale(lua_State *L)
{
  cairo_t *cr;
  luaA_to(L, dt_lua_cairo_t, &cr, 1);
  lua_Number x = luaL_checknumber(L, 2);
  lua_Number y = luaL_checknumber(L, 3);

  cairo_scale(cr, x, y);

  return 0;
}

static int _translate(lua_State *L)
{
  cairo_t *cr;
  luaA_to(L, dt_lua_cairo_t, &cr, 1);
  lua_Number x = luaL_checknumber(L, 2);
  lua_Number y = luaL_checknumber(L, 3);

  cairo_translate(cr, x, y);

  return 0;
}

static int _rotate(lua_State *L)
{
  cairo_t *cr;
  luaA_to(L, dt_lua_cairo_t, &cr, 1);
  lua_Number a = luaL_checknumber(L, 2);

  cairo_rotate(cr, a);

  return 0;
}

static int _move_to(lua_State *L)
{
  cairo_t *cr;
  luaA_to(L, dt_lua_cairo_t, &cr, 1);
  lua_Number x = luaL_checknumber(L, 2);
  lua_Number y = luaL_checknumber(L, 3);

  cairo_move_to(cr, x, y);

  return 0;
}

static int _line_to(lua_State *L)
{
  cairo_t *cr;
  luaA_to(L, dt_lua_cairo_t, &cr, 1);
  lua_Number x = luaL_checknumber(L, 2);
  lua_Number y = luaL_checknumber(L, 3);

  cairo_line_to(cr, x, y);

  return 0;
}

static int _arc(lua_State *L)
{
  cairo_t *cr;
  luaA_to(L, dt_lua_cairo_t, &cr, 1);
  lua_Number x = luaL_checknumber(L, 2);
  lua_Number y = luaL_checknumber(L, 3);
  lua_Number r = luaL_checknumber(L, 4);
  lua_Number a1 = luaL_checknumber(L, 5);
  lua_Number a2 = luaL_checknumber(L, 6);

  cairo_arc(cr, x, y, r, a1, a2);

  return 0;
}

static int _arc_negative(lua_State *L)
{
  cairo_t *cr;
  luaA_to(L, dt_lua_cairo_t, &cr, 1);
  lua_Number x = luaL_checknumber(L, 2);
  lua_Number y = luaL_checknumber(L, 3);
  lua_Number r = luaL_checknumber(L, 4);
  lua_Number a1 = luaL_checknumber(L, 5);
  lua_Number a2 = luaL_checknumber(L, 6);

  cairo_arc_negative(cr, x, y, r, a1, a2);

  return 0;
}

static int _rectangle(lua_State *L)
{
  cairo_t *cr;
  luaA_to(L, dt_lua_cairo_t, &cr, 1);
  lua_Number x = luaL_checknumber(L, 2);
  lua_Number y = luaL_checknumber(L, 3);
  lua_Number w = luaL_checknumber(L, 4);
  lua_Number h = luaL_checknumber(L, 5);

  cairo_rectangle(cr, x, y, w, h);

  return 0;
}

int dt_lua_init_cairo(lua_State *L)
{
  int cairo_type = dt_lua_init_gpointer_type(L, dt_lua_cairo_t);

  lua_pushcfunction(L, _draw_line);
  dt_lua_gtk_wrap(L);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, cairo_type, "draw_line");

  lua_pushcfunction(L, _save);
  dt_lua_gtk_wrap(L);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, cairo_type, "save");

  lua_pushcfunction(L, _restore);
  dt_lua_gtk_wrap(L);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, cairo_type, "restore");

  lua_pushcfunction(L, _new_sub_path);
  dt_lua_gtk_wrap(L);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, cairo_type, "new_sub_path");

  lua_pushcfunction(L, _scale);
  dt_lua_gtk_wrap(L);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, cairo_type, "scale");

  lua_pushcfunction(L, _translate);
  dt_lua_gtk_wrap(L);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, cairo_type, "translate");

  lua_pushcfunction(L, _rotate);
  dt_lua_gtk_wrap(L);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, cairo_type, "rotate");

  lua_pushcfunction(L, _move_to);
  dt_lua_gtk_wrap(L);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, cairo_type, "move_to");

  lua_pushcfunction(L, _line_to);
  dt_lua_gtk_wrap(L);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, cairo_type, "line_to");

  lua_pushcfunction(L, _arc);
  dt_lua_gtk_wrap(L);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, cairo_type, "arc");

  lua_pushcfunction(L, _arc_negative);
  dt_lua_gtk_wrap(L);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, cairo_type, "arc_negative");

  lua_pushcfunction(L, _rectangle);
  dt_lua_gtk_wrap(L);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, cairo_type, "rectangle");

  return 0;
}



// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
