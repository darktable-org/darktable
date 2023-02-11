/*
 *    This file is part of darktable,
 *    Copyright (C) 2019-2020 darktable developers.
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
#include "chart/common.h"

#include <cairo.h>
#include <gtk/gtk.h>

void draw_no_image(cairo_t *cr, GtkWidget *widget);
void draw_line(cairo_t *cr, point_t start, point_t end);
void draw_cross(cairo_t *cr, point_t center);
void draw_box(cairo_t *cr, box_t box, const float *homography);

void clear_background(cairo_t *cr);
void center_image(cairo_t *cr, image_t *image);
void draw_image(cairo_t *cr, image_t *image);
void draw_boundingbox(cairo_t *cr, point_t *bb);
void draw_f_boxes(cairo_t *cr, const float *homography, chart_t *chart);
void draw_d_boxes(cairo_t *cr, const float *homography, chart_t *chart);
void draw_color_boxes_outline(cairo_t *cr, const float *homography, chart_t *chart);
void draw_color_boxes_inside(cairo_t *cr, const float *homography, chart_t *chart, float shrink, float line_width,
                               gboolean colored);
void stroke_boxes(cairo_t *cr, float line_width);

void set_offset_and_scale(image_t *image, float width, float height);
cairo_surface_t *cairo_surface_create_from_xyz_data(const float *const image, const int width, const int height);

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
