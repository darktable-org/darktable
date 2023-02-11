/*
    This file is part of darktable,
    Copyright (C) 2019-2020 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

#include <glib.h>
#include <stdint.h>

#include "common/undo.h"

static gboolean _lighttable_undo_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                          GdkModifierType modifier, gpointer data)
{
  dt_undo_do_undo(darktable.undo, DT_UNDO_LIGHTTABLE);
  return TRUE;
}

static gboolean _lighttable_redo_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                          GdkModifierType modifier, gpointer data)
{
  dt_undo_do_redo(darktable.undo, DT_UNDO_LIGHTTABLE);
  return TRUE;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
