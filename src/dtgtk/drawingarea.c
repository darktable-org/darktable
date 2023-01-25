/*
    This file is part of darktable,
    Copyright (C) 2014-2020 darktable developers.

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

G_DEFINE_TYPE(GtkDarktableDrawingArea, dtgtk_drawing_area, GTK_TYPE_DRAWING_AREA);

static GtkSizeRequestMode dtgtk_drawing_area_get_request_mode(GtkWidget *widget)
{
  return GTK_SIZE_REQUEST_HEIGHT_FOR_WIDTH;
};

static void dtgtk_drawing_area_get_preferred_height_for_width(GtkWidget *widget, gint for_width,
                                                              gint *min_height, gint *nat_height)
{
  GtkDarktableDrawingArea *da = DTGTK_DRAWING_AREA(widget);

  *min_height = *nat_height = for_width * da->aspect;
}

static void dtgtk_drawing_area_class_init(GtkDarktableDrawingAreaClass *class)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(class);

  widget_class->get_request_mode = dtgtk_drawing_area_get_request_mode;
  widget_class->get_preferred_height_for_width = dtgtk_drawing_area_get_preferred_height_for_width;
}

static void dtgtk_drawing_area_init(GtkDarktableDrawingArea *da)
{
}

// public functions
GtkWidget *dtgtk_drawing_area_new_with_aspect_ratio(double aspect)
{
  GtkDarktableDrawingArea *da;
  da = g_object_new(dtgtk_drawing_area_get_type(), NULL);
  da->aspect = aspect;

  return (GtkWidget *)da;
}

void dtgtk_drawing_area_set_aspect_ratio(GtkWidget *widget, double aspect)
{
  GtkDarktableDrawingArea *da = DTGTK_DRAWING_AREA(widget);
  da->aspect = aspect;
  gtk_widget_queue_resize(widget);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

