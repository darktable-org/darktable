/*
    This file is part of darktable,
    copyright (c) 2015 LebedevRI.

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
#ifndef DTGTK_SIDE_PANEL_H
#define DTGTK_SIDE_PANEL_H

#include "develop/imageop.h"
#include <gtk/gtk.h>

G_BEGIN_DECLS
#define DTGTK_SIDE_PANEL(obj)                                                                                \
  G_TYPE_CHECK_INSTANCE_CAST(obj, dtgtk_side_panel_get_type(), GtkDarktableSidePanel)
#define DTGTK_SIDE_PANEL_CLASS(klass)                                                                        \
  GTK_CHECK_CLASS_CAST(klass, dtgtk_side_panel_get_type(), GtkDarktableButtonClass)
#define DTGTK_IS_SIDE_PANEL(obj) G_TYPE_CHECK_INSTANCE_TYPE(obj, dtgtk_side_panel_get_type())
#define DTGTK_IS_SIDE_PANEL_CLASS(klass) GTK_CHECK_CLASS_TYPE(obj, dtgtk_side_panel_get_type())

typedef struct _GtkDarktableSidePanel
{
  GtkBox panel;
  gint panel_width;
} GtkDarktableSidePanel;

typedef struct _GtkDarktableSidePanelClass
{
  GtkBoxClass parent_class;
} GtkDarktableSidePanelClass;

GType dtgtk_side_panel_get_type(void);

GtkWidget *dtgtk_side_panel_new();

G_END_DECLS

#endif

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
