/*
    This file is part of darktable,
    Copyright (C) 2012-2020 darktable developers.

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
  // ==== all below need to be freed and own their strings
  DT_ACTION_TYPE_SECTION,
  // ==== all above split off chains
  DT_ACTION_TYPE_COMMAND,
  DT_ACTION_TYPE_KEY_PRESSED,
  DT_ACTION_TYPE_PRESET,
  DT_ACTION_TYPE_FALLBACK,
  DT_ACTION_TYPE_VALUE_FALLBACK,
  // === all widgets below
  DT_ACTION_TYPE_PER_INSTANCE,
  DT_ACTION_TYPE_CLOSURE,
  // === dynamically assign widget type numbers from here
  DT_ACTION_TYPE_WIDGET,
} dt_action_type_t;

typedef struct dt_action_t
{
  dt_action_type_t type;
  const char *label;
  const char *label_translated;

  gpointer target; // widget, section, command
  struct dt_action_t *owner; // iop, lib, view, global
  struct dt_action_t *next;
} dt_action_t;

#define DT_ACTION(p) ((dt_action_t*)&p->actions)

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
  DT_ACTION_EFFECT_PREVIOUS = 1,
  DT_ACTION_EFFECT_NEXT = 2,
  DT_ACTION_EFFECT_FIRST = 4,
  DT_ACTION_EFFECT_LAST = 5,

  // Values
  DT_ACTION_EFFECT_POPUP = 0,
  DT_ACTION_EFFECT_UP = 1,
  DT_ACTION_EFFECT_DOWN = 2,
  DT_ACTION_EFFECT_RESET = 3,
  DT_ACTION_EFFECT_TOP = 4,
  DT_ACTION_EFFECT_BOTTOM = 5,

  // Togglebuttons
  DT_ACTION_EFFECT_TOGGLE = 0,
  DT_ACTION_EFFECT_ON = 1,
  DT_ACTION_EFFECT_OFF = 2,
  DT_ACTION_EFFECT_TOGGLE_CTRL = 3,
  DT_ACTION_EFFECT_ON_CTRL = 4,
  DT_ACTION_EFFECT_TOGGLE_RIGHT = 5,
  DT_ACTION_EFFECT_ON_RIGHT = 6,

  // Buttons
  DT_ACTION_EFFECT_ACTIVATE = 0,
  DT_ACTION_EFFECT_ACTIVATE_CTRL = 1,
  DT_ACTION_EFFECT_ACTIVATE_RIGHT = 2,
};
typedef gint dt_action_effect_t;

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
