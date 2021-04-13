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
  // === all widgets below
  DT_ACTION_TYPE_PER_INSTANCE,
  DT_ACTION_TYPE_CLOSURE,
  DT_ACTION_TYPE_WIDGET,
  DT_ACTION_TYPE_SLIDER,
  DT_ACTION_TYPE_COMBO,
  DT_ACTION_TYPE_TOGGLE,
  DT_ACTION_TYPE_BUTTON,
  // FIXME these will be dynamically assigned to modules that expose a shortcuttable widget
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

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
