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

#include <glib-object.h>
#include <gtk/gtk.h>

G_BEGIN_DECLS

#define DTGTK_TYPE_SIDE_PANEL (dtgtk_side_panel_get_type())
#define DTGTK_SIDE_PANEL(obj)                                                                                \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), DTGTK_TYPE_SIDE_PANEL, GtkDarktableSidePanel))
#define DTGTK_IS_SIDE_PANEL(obj) (G_TYPE_CHECK_INSTANCE_TYPE((obj), DTGTK_TYPE_SIDE_PANEL))
#define DTGTK_SIDE_PANEL_CLASS(klass)                                                                        \
  (G_TYPE_CHECK_CLASS_CAST((klass), DTGTK_TYPE_SIDE_PANEL, GtkDarktableSidePanelClass))
#define DTGTK_IS_SIDE_PANEL_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE((klass), DTGTK_TYPE_SIDE_PANEL))
#define DTGTK_SIDE_PANEL_GET_CLASS(obj)                                                                      \
  (G_TYPE_INSTANCE_GET_CLASS((obj), DTGTK_TYPE_SIDE_PANEL, GtkDarktableSidePanelClass))

typedef struct _GtkDarktableSidePanel
{
  GtkPaned panel;
} GtkDarktableSidePanel;

typedef struct _GtkDarktableSidePanelClass
{
  GtkPanedClass parent_class;

  /*< private >*/
  gint width;
} GtkDarktableSidePanelClass;

GType dtgtk_side_panel_get_type(void);

GtkWidget *dtgtk_side_panel_new();

G_END_DECLS

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

