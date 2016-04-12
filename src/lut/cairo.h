#pragma once
#include "lut/common.h"
#include "lut/colorchart.h"

#include <gtk/gtk.h>
#include <cairo.h>

void draw_no_image(cairo_t *cr, GtkWidget *widget);
void draw_line(cairo_t *cr, point_t start, point_t end);
void draw_cross(cairo_t *cr, point_t center);
void draw_box(cairo_t *cr, box_t box, point_t *bb);

void clear_background(cairo_t *cr);
void center_image(cairo_t *cr, image_t *image);
void draw_image(cairo_t *cr, image_t *image);
void draw_boundingbox(cairo_t *cr, point_t *bb);
void draw_f_boxes(cairo_t *cr, point_t *bb, chart_t *chart);
void draw_d_boxes(cairo_t *cr, point_t *bb, chart_t *chart);
void draw_color_boxes_outline(cairo_t *cr, point_t *bb, chart_t *chart);
void draw_color_boxes_inside(cairo_t *cr, point_t *bb, chart_t *chart, float line_width, gboolean colored);
void stroke_boxes(cairo_t *cr, float line_width);

void set_offset_and_scale(image_t *image, float width, float height);
cairo_surface_t *cairo_surface_create_from_xyz_data(const float * const image, const int width, const int height);
