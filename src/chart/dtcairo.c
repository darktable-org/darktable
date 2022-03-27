/*
 *    This file is part of darktable,
 *    Copyright (C) 2019-2021 darktable developers.
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

#include "chart/dtcairo.h"
#include "chart/common.h"

void draw_no_image(cairo_t *cr, GtkWidget *widget)
{
  guint width = gtk_widget_get_allocated_width(widget);
  guint height = gtk_widget_get_allocated_height(widget);
  cairo_set_line_width(cr, 5);
  cairo_set_source_rgb(cr, 1, 0, 0);
  cairo_move_to(cr, 0, 0);
  cairo_line_to(cr, width, height);
  cairo_move_to(cr, width, 0);
  cairo_line_to(cr, 0, height);
  cairo_stroke(cr);
}

void draw_line(cairo_t *cr, point_t start, point_t end)
{
  cairo_move_to(cr, start.x, start.y);
  cairo_line_to(cr, end.x, end.y);
}

void draw_cross(cairo_t *cr, point_t center)
{
  cairo_move_to(cr, center.x - 10, center.y);
  cairo_line_to(cr, center.x + 10, center.y);
  cairo_move_to(cr, center.x, center.y - 10);
  cairo_line_to(cr, center.x, center.y + 10);
}

void draw_box(cairo_t *cr, box_t box, const float *homography)
{
  point_t p[4];
  p[TOP_LEFT] = p[TOP_RIGHT] = p[BOTTOM_RIGHT] = p[BOTTOM_LEFT] = box.p;
  p[TOP_RIGHT].x += box.w;
  p[BOTTOM_RIGHT].x += box.w;
  p[BOTTOM_RIGHT].y += box.h;
  p[BOTTOM_LEFT].y += box.h;

  for(int i = 0; i < 4; i++) p[i] = apply_homography(p[i], homography);

  //   cairo_new_sub_path(cr);
  cairo_move_to(cr, p[TOP_LEFT].x, p[TOP_LEFT].y);
  for(int i = 1; i < 4; i++)
  {
    point_t corner = p[i];
    cairo_line_to(cr, corner.x, corner.y);
  }
  cairo_close_path(cr);
}

void clear_background(cairo_t *cr)
{
  cairo_set_source_rgb(cr, 0, 0, 0);
  cairo_paint(cr);
}

void center_image(cairo_t *cr, image_t *image)
{
  cairo_translate(cr, image->offset_x, image->offset_y);
}

void draw_image(cairo_t *cr, image_t *image)
{
  cairo_set_source(cr, image->image);
  cairo_paint(cr);
}

void draw_boundingbox(cairo_t *cr, point_t *bb)
{
  for(int i = 0; i < 4; i++) draw_line(cr, bb[i], bb[(i + 1) % 4]);
}

void draw_f_boxes(cairo_t *cr, const float *homography, chart_t *chart)
{
  for(GList *iter = chart->f_list; iter; iter = g_list_next(iter))
  {
    f_line_t *f = iter->data;
    for(int i = 0; i < 4; i++)
    {
      point_t p = apply_homography(f->p[i], homography);
      draw_cross(cr, p);
    }
  }
}

static void _draw_boxes(cairo_t *cr, const float *homography, GHashTable *table)
{
  GHashTableIter table_iter;
  gpointer key, value;

  g_hash_table_iter_init(&table_iter, table);
  while(g_hash_table_iter_next(&table_iter, &key, &value))
  {
    box_t *box = (box_t *)value;
    draw_box(cr, *box, homography);
  }
}

void draw_d_boxes(cairo_t *cr, const float *homography, chart_t *chart)
{
  _draw_boxes(cr, homography, chart->d_table);
}

void draw_color_boxes_outline(cairo_t *cr, const float *homography, chart_t *chart)
{
  _draw_boxes(cr, homography, chart->box_table);
}

void draw_color_boxes_inside(cairo_t *cr, const float *homography, chart_t *chart, float shrink, float line_width,
                               gboolean colored)
{
  GHashTableIter table_iter;
  gpointer key, value;

  float x_shrink = shrink * chart->box_shrink / chart->bb_w, y_shrink = shrink * chart->box_shrink / chart->bb_h;

  cairo_set_line_width(cr, line_width);
  cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);

  g_hash_table_iter_init(&table_iter, chart->box_table);
  while(g_hash_table_iter_next(&table_iter, &key, &value))
  {
    box_t *box = (box_t *)value;
    box_t inner_box = *box;
    inner_box.p.x += x_shrink;
    inner_box.p.y += y_shrink;
    inner_box.w -= 2.0 * x_shrink;
    inner_box.h -= 2.0 * y_shrink;
    draw_box(cr, inner_box, homography);

    if(colored) cairo_set_source_rgb(cr, box->rgb[0], box->rgb[1], box->rgb[2]);

    cairo_stroke(cr);
  }
}

void stroke_boxes(cairo_t *cr, float line_width)
{
  cairo_set_line_width(cr, line_width * 2.5);
  cairo_set_source_rgb(cr, 1, 1, 1);
  cairo_stroke_preserve(cr);

  cairo_set_line_width(cr, line_width);
  cairo_set_source_rgb(cr, 0, 0, 0);
  cairo_stroke(cr);
}

void set_offset_and_scale(image_t *image, float width, float height)
{
  if(!image->image) return;

  cairo_matrix_t matrix;
  const float s_w = (float)image->width / width;
  const float s_h = (float)image->height / height;
  image->scale = MAX(s_w, s_h);
  cairo_matrix_init_scale(&matrix, image->scale, image->scale);
  cairo_pattern_set_matrix(image->image, &matrix);

  image->offset_x = (width - (image->width / image->scale)) / 2.0 + 0.5;
  image->offset_y = (height - (image->height / image->scale)) / 2.0 + 0.5;
}

static cairo_user_data_key_t source_data_buffer_key;

cairo_surface_t *cairo_surface_create_from_xyz_data(const float *const image, const int width, const int height)
{
  unsigned char *rgbbuf = (unsigned char *)malloc(sizeof(unsigned char) * height * width * 4);

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(height, image, width) \
  shared(rgbbuf) \
  schedule(static)
#endif
  for(int y = 0; y < height; y++)
  {
    const float *iter = image + y * width * 3;
    for(int x = 0; x < width; x++, iter += 3)
    {
      dt_aligned_pixel_t sRGB;
      int32_t pixel = 0;
      dt_XYZ_to_sRGB_clipped(iter, sRGB);
      for(int c = 0; c < 3; c++) pixel |= ((int)(sRGB[c] * 255) & 0xff) << (16 - c * 8);
      *((int *)(&rgbbuf[(x + (size_t)y * width) * 4])) = pixel;
    }
  }

  cairo_format_t format = CAIRO_FORMAT_RGB24;
  const int stride = cairo_format_stride_for_width(format, width);
  cairo_surface_t *surface = cairo_image_surface_create_for_data(rgbbuf, format, width, height, stride);
  cairo_surface_set_user_data(surface, &source_data_buffer_key, rgbbuf, free);

  return surface;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

