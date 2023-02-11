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

#pragma once

#include <glib-object.h>
#include <gtk/gtk.h>

G_BEGIN_DECLS

#define DTGTK_TYPE_DRAWING_AREA (dtgtk_drawing_area_get_type())
#define DTGTK_DRAWING_AREA(obj)                                                                              \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), DTGTK_TYPE_DRAWING_AREA, GtkDarktableDrawingArea))
#define DTGTK_IS_DRAWING_AREA(obj) (G_TYPE_CHECK_INSTANCE_TYPE((obj), DTGTK_TYPE_DRAWING_AREA))
#define DTGTK_DRAWING_AREA_CLASS(klass)                                                                      \
  (G_TYPE_CHECK_CLASS_CAST((klass), DTGTK_TYPE_DRAWING_AREA, GtkDarktableDrawingAreaClass))
#define DTGTK_IS_DRAWING_AREA_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE((klass), DTGTK_TYPE_DRAWING_AREA))
#define DTGTK_DRAWING_AREA_GET_CLASS(obj)                                                                    \
  (G_TYPE_INSTANCE_GET_CLASS((obj), DTGTK_TYPE_DRAWING_AREA, GtkDarktableDrawingAreaClass))

typedef struct _GtkDarktableDrawingArea
{
  GtkDrawingArea area;

  /*
   * drawing area aspect ratio.
   * width = as much as possible
   * height = width * aspect
   *
   * e.g. 1   => square
   *      0.5 => height is 2 times smaller than width
   *      2   => height is 2 times bigger than width
   */
  double aspect;
} GtkDarktableDrawingArea;

typedef struct _GtkDarktableDrawingAreaClass
{
  GtkDrawingAreaClass parent_class;
} GtkDarktableDrawingAreaClass;

GType dtgtk_drawing_area_get_type(void);

GtkWidget *dtgtk_drawing_area_new_with_aspect_ratio(double aspect);
void dtgtk_drawing_area_set_aspect_ratio(GtkWidget *w, double aspect);

G_END_DECLS

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
