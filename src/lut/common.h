#pragma once

#include "lut/colorchart.h"

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

  point_t bb[4];

  chart_t **chart;
  gboolean draw_colored;
} image_t;

point_t transform_coords(point_t p, point_t *bb);
