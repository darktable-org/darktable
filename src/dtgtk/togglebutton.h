/*
    This file is part of darktable,
    Copyright (C) 2010-2020 darktable developers.

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

#define DTGTK_TYPE_TOGGLEBUTTON dtgtk_togglebutton_get_type()
G_DECLARE_FINAL_TYPE(GtkDarktableToggleButton, dtgtk_togglebutton, DTGTK, TOGGLEBUTTON, GtkToggleButton)

struct _GtkDarktableToggleButton
{
  GtkToggleButton widget;
  DTGTKCairoPaintIconFunc icon;
  gint icon_flags;
  void *icon_data;
  GdkRGBA bg;
  GtkWidget *canvas;
};

/** instantiate a new darktable toggle button */
GtkWidget *dtgtk_togglebutton_new(DTGTKCairoPaintIconFunc paint, gint paintflag, void *paintdata);

/** Set the paint function and paint flags */
void dtgtk_togglebutton_set_paint(GtkDarktableToggleButton *button, DTGTKCairoPaintIconFunc paint,
                                  gint paintflags, void *paintdata);

G_END_DECLS

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

