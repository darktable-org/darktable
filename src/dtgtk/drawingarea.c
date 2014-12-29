/*
    This file is part of darktable,
    copyright (c) 2014 LebedevRI.

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

#include "dtgtk/drawingarea.h"

static GtkSizeRequestMode get_request_mode(GtkWidget *widget)
{
  return GTK_SIZE_REQUEST_HEIGHT_FOR_WIDTH;
};

static void get_preferred_height_for_width(GtkWidget *widget, gint for_width, gint *min_height,
                                           gint *nat_height)
{
  GtkDarktableDrawingArea *da = (GtkDarktableDrawingArea *)widget;

  *min_height = *nat_height = for_width * da->aspect;
}

static void _drawing_area_class_init(GtkDarktableDrawingAreaClass *klass)
{
  GtkWidgetClass *widget_class = (GtkWidgetClass *)klass;

  widget_class->get_request_mode = get_request_mode;
  widget_class->get_preferred_height_for_width = get_preferred_height_for_width;
}

// public functions
GtkWidget *dtgtk_drawing_area_new_with_aspect_ratio(double aspect)
{
  GtkDarktableDrawingArea *da;
  da = g_object_new(dtgtk_drawing_area_get_type(), NULL);
  da->aspect = aspect;

  gtk_widget_set_hexpand(GTK_WIDGET(da), TRUE);

  return (GtkWidget *)da;
}

GType dtgtk_drawing_area_get_type()
{
  static GType dtgtk_drawing_area_type = 0;
  if(!dtgtk_drawing_area_type)
  {
    static const GTypeInfo dtgtk_drawing_area_info = {
      sizeof(GtkDarktableDrawingAreaClass), (GBaseInitFunc)NULL, (GBaseFinalizeFunc)NULL,
      (GClassInitFunc)_drawing_area_class_init, NULL, /* class_finalize */
      NULL,                                           /* class_data */
      sizeof(GtkDarktableDrawingArea), 0,             /* n_preallocs */
      (GInstanceInitFunc)NULL,
    };
    dtgtk_drawing_area_type = g_type_register_static(GTK_TYPE_DRAWING_AREA, "GtkDarktableDrawingArea",
                                                     &dtgtk_drawing_area_info, 0);
  }
  return dtgtk_drawing_area_type;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
