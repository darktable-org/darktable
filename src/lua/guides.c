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
#include "gui/guides.h"
#include "common/darktable.h"
#include "lua/cairo.h"
#include "lua/call.h"
#include "lua/lua.h"
#include "lua/widget/widget.h"

typedef struct callback_data_t
{
  int draw_callback_id;
  int gui_callback_id;
} callback_data_t;

static void _guides_draw_callback(cairo_t *cr, const float x, const float y,
                                  const float w, const float h,
                                  const float zoom_scale, void *user_data)
{
  callback_data_t *d = (callback_data_t *)user_data;

  dt_lua_lock_silent(); // this code is called from the C side so we have to lock

  lua_State *L = darktable.lua_state.state;
  lua_rawgeti(L, LUA_REGISTRYINDEX, d->draw_callback_id);

  luaA_push(L, dt_lua_cairo_t, &cr);
  lua_pushnumber(L, x);
  lua_pushnumber(L, y);
  lua_pushnumber(L, w);
  lua_pushnumber(L, h);
  lua_pushnumber(L, zoom_scale);

  // this will be called directly from the gui thread so we can just execute it, without caring about the gtk lock
  dt_lua_treated_pcall(L,6,0);

  dt_lua_type_gpointer_drop(L,cr);

  dt_lua_unlock();
}

static GtkWidget *_guides_gui_callback(dt_iop_module_t *self, void *user_data)
{
  callback_data_t *d = (callback_data_t *)user_data;
  dt_lua_lock_silent(); // this code is called from the C side so we have to lock
  lua_State *L = darktable.lua_state.state;
  lua_rawgeti(L, LUA_REGISTRYINDEX, d->gui_callback_id);
  dt_lua_treated_pcall(L,0,1);

//   dt_lua_debug_stack(L);
  lua_widget widget;
  luaA_to(L, lua_widget, &widget, -1);
  dt_lua_widget_bind(L, widget);
  lua_pop(L,1);

  dt_lua_unlock();

  return widget->widget;
}

static int register_guide(lua_State *L)
{
  int draw_callback_id = -1, gui_callback_id = -1;
  dt_guides_widget_callback gui_callback = NULL;

  lua_settop(L, 3);

  const char *name = luaL_checkstring(L, 1);

  if(!lua_isnil(L, 3))
  {
    luaL_checktype(L, 3, LUA_TFUNCTION);
    gui_callback = _guides_gui_callback;
    gui_callback_id = luaL_ref(L, LUA_REGISTRYINDEX);
  }
  else
    lua_pop(L, 1); // get rid of the nil

  if(lua_isnil(L, 2))
    return luaL_error(L, "missing draw callback");

  luaL_checktype(L, 2, LUA_TFUNCTION);
  draw_callback_id = luaL_ref(L, LUA_REGISTRYINDEX);

  callback_data_t *user_data = (callback_data_t *)malloc(sizeof(callback_data_t));
  user_data->draw_callback_id = draw_callback_id;
  user_data->gui_callback_id = gui_callback_id;

  dt_guides_add_guide(name, _guides_draw_callback, gui_callback, user_data, free);
  return 0;
}

int dt_lua_init_guides(lua_State *L)
{
  dt_lua_push_darktable_lib(L);

  dt_lua_goto_subtable(L, "guides");
  // build the table containing the guides

  lua_pushstring(L, "register_guide");
  lua_pushcfunction(L, register_guide);
  lua_settable(L, -3);

  lua_pop(L, 1); // remove the configuration table from the stack
  return 0;
}



// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
