/*
    This file is part of darktable,
    Copyright (C) 2021 darktable developers.

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

typedef enum dt_action_type_t
{
  DT_ACTION_TYPE_CATEGORY,
  DT_ACTION_TYPE_GLOBAL,
  DT_ACTION_TYPE_VIEW,
  DT_ACTION_TYPE_LIB,
  DT_ACTION_TYPE_IOP,
  DT_ACTION_TYPE_BLEND,
  // ==== all below need to be freed and own their strings
  DT_ACTION_TYPE_SECTION,
  // ==== all above split off chains
  DT_ACTION_TYPE_IOP_INSTANCE,
  DT_ACTION_TYPE_IOP_SECTION,
  DT_ACTION_TYPE_COMMAND,
  DT_ACTION_TYPE_PRESET,
  DT_ACTION_TYPE_FALLBACK,
  DT_ACTION_TYPE_VALUE_FALLBACK,
  // === all widgets below
  DT_ACTION_TYPE_PER_INSTANCE,
  DT_ACTION_TYPE_WIDGET,
  // === dynamically assign widget type numbers from here
} dt_action_type_t;

typedef struct dt_action_t
{
  dt_action_type_t type;
  const char *id;
  const char *label;

  gpointer target; // widget, section, command
  struct dt_action_t *owner; // iop, lib, view, global
  struct dt_action_t *next;
} dt_action_t;

#define DT_ACTION(p) (p?(dt_action_t*)&p->actions:NULL)

enum
{
  DT_ACTION_ELEMENT_DEFAULT = 0,
};
typedef gint dt_action_element_t;

enum
{
  DT_ACTION_EFFECT_DEFAULT_MOVE = -1,
  DT_ACTION_EFFECT_DEFAULT_KEY = 0,
  DT_ACTION_EFFECT_DEFAULT_UP = 1,
  DT_ACTION_EFFECT_DEFAULT_DOWN = 2,

  // Generic
  DT_ACTION_EFFECT_NEXT = DT_ACTION_EFFECT_DEFAULT_UP,
  DT_ACTION_EFFECT_PREVIOUS = DT_ACTION_EFFECT_DEFAULT_DOWN,
  DT_ACTION_EFFECT_LAST = 4,
  DT_ACTION_EFFECT_FIRST = 5,
  DT_ACTION_EFFECT_COMBO_SEPARATOR = 6,

  // Values
  DT_ACTION_EFFECT_POPUP = DT_ACTION_EFFECT_DEFAULT_KEY,
  DT_ACTION_EFFECT_UP = DT_ACTION_EFFECT_DEFAULT_UP,
  DT_ACTION_EFFECT_DOWN = DT_ACTION_EFFECT_DEFAULT_DOWN,
  DT_ACTION_EFFECT_RESET = 3,
  DT_ACTION_EFFECT_TOP = 4,
  DT_ACTION_EFFECT_BOTTOM = 5,
  DT_ACTION_EFFECT_SET = 6,

  // Togglebuttons
  DT_ACTION_EFFECT_TOGGLE = DT_ACTION_EFFECT_DEFAULT_KEY,
  DT_ACTION_EFFECT_ON = DT_ACTION_EFFECT_DEFAULT_UP,
  DT_ACTION_EFFECT_OFF = DT_ACTION_EFFECT_DEFAULT_DOWN,
  DT_ACTION_EFFECT_TOGGLE_CTRL = 3,
  DT_ACTION_EFFECT_ON_CTRL = 4,
  DT_ACTION_EFFECT_TOGGLE_RIGHT = 5,
  DT_ACTION_EFFECT_ON_RIGHT = 6,

  DT_ACTION_EFFECT_HOLD = DT_ACTION_EFFECT_DEFAULT_KEY,

  // Buttons
  DT_ACTION_EFFECT_ACTIVATE = DT_ACTION_EFFECT_DEFAULT_KEY,
  DT_ACTION_EFFECT_ACTIVATE_CTRL = DT_ACTION_EFFECT_DEFAULT_UP,
  DT_ACTION_EFFECT_ACTIVATE_RIGHT = DT_ACTION_EFFECT_DEFAULT_DOWN,

  // Entries
  DT_ACTION_EFFECT_FOCUS = DT_ACTION_EFFECT_DEFAULT_KEY,
  DT_ACTION_EFFECT_START = DT_ACTION_EFFECT_DEFAULT_UP,
  DT_ACTION_EFFECT_END = DT_ACTION_EFFECT_DEFAULT_DOWN,
  DT_ACTION_EFFECT_CLEAR = 3,
};
typedef gint dt_action_effect_t;

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
