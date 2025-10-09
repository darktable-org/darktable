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
#include "common/darktable.h"

G_DEFINE_TYPE(GtkDarktableDrawingArea, dtgtk_drawing_area, GTK_TYPE_DRAWING_AREA);

static GtkSizeRequestMode dtgtk_drawing_area_get_request_mode(GtkWidget *widget)
{
  return GTK_SIZE_REQUEST_HEIGHT_FOR_WIDTH;
};

static void _widget_measure(GtkWidget* widget,
                            GtkOrientation orientation,
                            int for_size,
                            int* minimum,
                            int* natural,
                            int* minimum_baseline,
                            int* natural_baseline)
{
  if(orientation == GTK_ORIENTATION_VERTICAL)
  {
    GtkDarktableDrawingArea *da = DTGTK_DRAWING_AREA(widget);

    if(da->height == 0)
    {
      // initialize with height = width
      *minimum = *natural = for_size;
    }
    else if(da->height == -1)
    {
      // initialize with aspect ratio
      *minimum = *natural = for_size * da->aspect;
    }
    else
    {
      *minimum = *natural = da->height;
    }
  }
}

static guint _drawing_area_draw_signal = 0;

static void _widget_snapshot(GtkWidget* widget,
                             GtkSnapshot* snapshot)
{
  graphene_rect_t bounds;
  graphene_rect_init(&bounds, 0, 0, gtk_widget_get_width(widget), gtk_widget_get_height(widget));
  cairo_t* cr = gtk_snapshot_append_cairo(snapshot, &bounds);

  g_signal_emit(widget, _drawing_area_draw_signal, 0, cr);

  cairo_destroy(cr);
}

static void dtgtk_drawing_area_class_init(GtkDarktableDrawingAreaClass *class)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(class);

  widget_class->measure = _widget_measure;
  widget_class->snapshot = _widget_snapshot;

  _drawing_area_draw_signal =
      g_signal_new ("draw",
                    G_TYPE_FROM_CLASS(class),
                    G_SIGNAL_RUN_LAST,
                    0,                                // class offset
                    NULL, NULL,                       // accumulator, data
                    g_cclosure_marshal_VOID__POINTER, // marshaller
                    G_TYPE_NONE, 1,                   // return type
                    G_TYPE_POINTER);                  // parameter: cairo_t*

  dt_add_legacy_signals(widget_class);
}

static void dtgtk_drawing_area_init(GtkDarktableDrawingArea *da)
{
}

// public functions
GtkWidget *dtgtk_drawing_area_new(void)
{
  GtkDarktableDrawingArea *da;
  da = g_object_new(dtgtk_drawing_area_get_type(), NULL);
  da->aspect = 1.0f; // square by default
  da->height = -1;

  return (GtkWidget *)da;
}


GtkWidget *dtgtk_drawing_area_new_with_aspect_ratio(double aspect)
{
  GtkDarktableDrawingArea *da;
  da = g_object_new(dtgtk_drawing_area_get_type(), NULL);
  da->aspect = aspect;
  da->height = -1;

  return (GtkWidget *)da;
}

GtkWidget *dtgtk_drawing_area_new_with_height(int height)
{
  GtkDarktableDrawingArea *da;
  da = g_object_new(dtgtk_drawing_area_get_type(), NULL);
  da->aspect = 1.0f; // not used
  da->height = height;

  return (GtkWidget *)da;
}

void dtgtk_drawing_area_set_aspect_ratio(GtkWidget *widget, double aspect)
{
  GtkDarktableDrawingArea *da = DTGTK_DRAWING_AREA(widget);
  da->aspect = aspect;
  da->height = -1;
  gtk_widget_queue_resize(widget);
}

void dtgtk_drawing_area_set_height(GtkWidget *widget, int height)
{
  GtkDarktableDrawingArea *da = DTGTK_DRAWING_AREA(widget);
  da->aspect = 1.0f; // not used
  da->height = height < 0 ? 0 : height;
  gtk_widget_queue_resize(widget);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

