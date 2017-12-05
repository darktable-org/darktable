/*
 *    This file is part of darktable,
 *    copyright (c) 2016 tobias ellinghaus.
 *
 *    darktable is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 *
 *    darktable is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "chart/colorchart.h"

enum
{
  TOP_LEFT = 0,
  TOP_RIGHT = 1,
  BOTTOM_RIGHT = 2,
  BOTTOM_LEFT = 3
};

typedef struct image_t
{
  GtkWidget *drawing_area;

  cairo_surface_t *surface;
  cairo_pattern_t *image;
  int width, height;
  float *xyz;
  float scale;
  int offset_x, offset_y;
  float shrink;

  point_t bb[4];

  chart_t **chart;
  gboolean draw_colored;
} image_t;

point_t transform_coords(point_t p, point_t *bb);

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces
// modified;
