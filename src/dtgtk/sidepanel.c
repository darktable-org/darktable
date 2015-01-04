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

static void get_preferred_width(GtkWidget *widget, gint *minimum_size, gint *natural_size)
{
  GtkDarktableSidePanel *panel = (GtkDarktableSidePanel *)widget;

  *minimum_size = *natural_size = panel->panel_width;
}

static void _side_panel_class_init(GtkDarktableSidePanelClass *klass)
{
  GtkWidgetClass *widget_class = (GtkWidgetClass *)klass;

  widget_class->get_preferred_width = get_preferred_width;
}

// public functions
GtkWidget *dtgtk_side_panel_new()
{
  GtkDarktableSidePanel *panel;

  panel = g_object_new(dtgtk_side_panel_get_type(), "orientation", GTK_ORIENTATION_VERTICAL, NULL);
  panel->panel_width = dt_conf_get_int("panel_width");

  gtk_widget_set_vexpand(GTK_WIDGET(panel), TRUE);
  gtk_widget_set_size_request(GTK_WIDGET(panel), panel->panel_width, -1);

  return (GtkWidget *)panel;
}

GType dtgtk_side_panel_get_type()
{
  static GType dtgtk_side_panel_type = 0;
  if(!dtgtk_side_panel_type)
  {
    static const GTypeInfo dtgtk_side_panel_info = {
      sizeof(GtkDarktableSidePanelClass), (GBaseInitFunc)NULL, (GBaseFinalizeFunc)NULL,
      (GClassInitFunc)_side_panel_class_init, NULL, /* class_finalize */
      NULL,                                         /* class_data */
      sizeof(GtkDarktableSidePanel), 0,             /* n_preallocs */
      (GInstanceInitFunc)NULL,
    };
    dtgtk_side_panel_type
        = g_type_register_static(GTK_TYPE_BOX, "GtkDarktableSidePanel", &dtgtk_side_panel_info, 0);
  }
  return dtgtk_side_panel_type;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
