/*
   This file is part of darktable,
   Copyright (C) 2015-2020 darktable developers.

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

#include "lua/widget/widget.h"

typedef dt_lua_widget_t* lua_separator;
typedef dt_lua_widget_t* lua_label;
typedef dt_lua_widget_t* lua_section_label;
typedef dt_lua_widget_t* lua_file_chooser_button;
typedef dt_lua_widget_t* lua_entry;
typedef dt_lua_widget_t* lua_combobox;
typedef dt_lua_widget_t* lua_check_button;
typedef dt_lua_widget_t* lua_button;
typedef dt_lua_widget_t* lua_slider;
typedef dt_lua_widget_t* lua_text_view;

// containers can be inherited
extern dt_lua_widget_type_t container_type;
typedef dt_lua_widget_t dt_lua_container_t;

typedef dt_lua_container_t* lua_container;
typedef dt_lua_container_t* lua_box;
typedef dt_lua_container_t* lua_stack;

// Various functions to init various widget types
int dt_lua_init_widget_box(lua_State* L);
int dt_lua_init_widget_button(lua_State* L);
int dt_lua_init_widget_check_button(lua_State* L);
int dt_lua_init_widget_label(lua_State* L);
int dt_lua_init_widget_section_label(lua_State* L);
int dt_lua_init_widget_entry(lua_State* L);
int dt_lua_init_widget_file_chooser_button(lua_State* L);
int dt_lua_init_widget_separator(lua_State* L);
int dt_lua_init_widget_combobox(lua_State* L);
int dt_lua_init_widget_container(lua_State* L);
int dt_lua_init_widget_stack(lua_State* L);
int dt_lua_init_widget_slider(lua_State* L);
int dt_lua_init_widget_text_view(lua_State* L);

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

