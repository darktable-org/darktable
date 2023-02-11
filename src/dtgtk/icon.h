/*
    This file is part of darktable,
    Copyright (C) 2011-2020 darktable developers.

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
#define DTGTK_ICON(obj) G_TYPE_CHECK_INSTANCE_CAST(obj, dtgtk_icon_get_type(), GtkDarktableIcon)
#define DTGTK_ICON_CLASS(klass) G_TYPE_CHECK_CLASS_CAST(klass, dtgtk_icon_get_type(), GtkDarktableIconClass)
#define DTGTK_IS_ICON(obj) G_TYPE_CHECK_INSTANCE_TYPE(obj, dtgtk_icon_get_type())
#define DTGTK_IS_ICON_CLASS(klass) G_TYPE_CHECK_CLASS_TYPE(obj, dtgtk_icon_get_type())

typedef struct _GtkDarktableIcon
{
  GtkEventBox widget;
  DTGTKCairoPaintIconFunc icon;
  gint icon_flags;
  void *icon_data;
} GtkDarktableIcon;

typedef struct _GtkDarktableIconClass
{
  GtkEventBoxClass parent_class;
} GtkDarktableIconClass;

GType dtgtk_icon_get_type(void);

/** instantiate a new darktable icon control passing paint function as content */
GtkWidget *dtgtk_icon_new(DTGTKCairoPaintIconFunc paint, gint paintflags, void *paintdata);

/** set the paint function for a icon */
void dtgtk_icon_set_paint(GtkWidget *icon, DTGTKCairoPaintIconFunc paint, gint paintflags, void *paintdata);

G_END_DECLS

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
