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

#include "paint.h"
#include <gtk/gtk.h>

G_BEGIN_DECLS

#define DTGTK_TYPE_PAINT_CELL dtgtk_paint_cell_get_type()
G_DECLARE_FINAL_TYPE(GtkDarktablePaintCell, dtgtk_paint_cell,
                     DTGTK, PAINT_CELL, GtkCellRenderer)

struct _GtkDarktablePaintCell
{
  GtkCellRenderer parent;
  DTGTKCairoPaintIconFunc paint;
  gint paint_flags;
  void *paint_data;
};

/** Cell renderer that draws a dtgtk cairo paint function directly
 *  into the tree view's cairo context — the same approach
 *  dtgtk_button uses for its icon. avoids the GtkCellRendererPixbuf
 *  intermediate, which softens edges at icon sizes.
 *
 *  Visibility is controlled via the standard GtkCellRenderer "visible"
 *  property, typically bound to a model column via
 *  gtk_tree_view_column_new_with_attributes(..., "visible", COL, NULL). */
GtkCellRenderer *dtgtk_paint_cell_new(DTGTKCairoPaintIconFunc paint,
                                      gint paint_flags,
                                      void *paint_data);

G_END_DECLS

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
