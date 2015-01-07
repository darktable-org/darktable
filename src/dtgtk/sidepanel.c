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

#include "dtgtk/sidepanel.h"
#include "develop/imageop.h"

#include <gtk/gtk.h>

G_DEFINE_TYPE(GtkDarktableSidePanel, dtgtk_side_panel, GTK_TYPE_BOX);

static void dtgtk_side_panel_get_preferred_width(GtkWidget *widget, gint *minimum_size, gint *natural_size)
{
  GtkDarktableSidePanelClass *class = DTGTK_SIDE_PANEL_GET_CLASS(widget);

  *minimum_size = *natural_size = class->width;
}

static void dtgtk_side_panel_class_init(GtkDarktableSidePanelClass *class)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(class);

  widget_class->get_preferred_width = dtgtk_side_panel_get_preferred_width;

  class->width = dt_conf_get_int("panel_width");
}

static void dtgtk_side_panel_init(GtkDarktableSidePanel *panel)
{
  gtk_widget_set_vexpand(GTK_WIDGET(panel), TRUE);
}

// public functions
GtkWidget *dtgtk_side_panel_new()
{
  return g_object_new(dtgtk_side_panel_get_type(), "orientation", GTK_ORIENTATION_VERTICAL, NULL);
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
