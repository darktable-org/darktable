/*
    This file is part of darktable,
    Copyright (C) 2010-2022 darktable developers.

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

#include "bauhaus/bauhaus.h"
#include "dtgtk/paint.h"
#include "gui/draw.h"
#include <math.h>

#ifndef M_PI
#define M_PI 3.141592654
#endif

#define PREAMBLE(scaling, line_scaling, x_offset, y_offset) {  \
                  cairo_save(cr);                                                            \
                  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);                              \
                  const float s = ((w < h) ? w : h) * scaling;                               \
                  cairo_translate(cr, x + (w / 2.0) - (s / 2.0), y + (h / 2.0) - (s / 2.0)); \
                  cairo_scale(cr, s, s);                                                     \
                  cairo_translate(cr, x_offset, y_offset);                                   \
                  cairo_matrix_t matrix;                                                     \
                  cairo_get_matrix(cr, &matrix);                                             \
                  cairo_set_line_width(cr, (line_scaling * 1.618) / hypot(matrix.xx, matrix.yy)); }

#define FINISH { cairo_identity_matrix(cr); \
                 cairo_restore(cr); }

static void _rounded_rectangle(cairo_t *cr)  // create rounded rectangle to use in other icons
{
  const double degrees = M_PI / 180.0;

  cairo_new_sub_path (cr);
  cairo_arc (cr, 0.9, 0.1, 0.1, -90 * degrees, 0 * degrees);
  cairo_arc (cr, 0.9, 0.9, 0.1, 0 * degrees, 90 * degrees);
  cairo_arc (cr, 0.1, 0.9, 0.1, 90 * degrees, 180 * degrees);
  cairo_arc (cr, 0.1, 0.1, 0.1, 180 * degrees, 270 * degrees);
  cairo_close_path (cr);
}

void dtgtk_cairo_paint_empty(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0, 0)
  cairo_stroke(cr);
  FINISH
}

void dtgtk_cairo_paint_color(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0, 0)

  cairo_translate(cr, x, y);
  cairo_scale(cr, w, h);
  cairo_rectangle(cr, 0.1, 0.1, 0.8, 0.8);
  cairo_fill(cr);
  cairo_set_source_rgba(cr, 0, 0, 0, 0.6);
  cairo_stroke(cr);

  FINISH
}

void dtgtk_cairo_paint_presets(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0, 0)

  cairo_move_to(cr, 0.1, 0.1);
  cairo_line_to(cr, 0.9, 0.1);
  cairo_move_to(cr, 0.1, 0.5);
  cairo_line_to(cr, 0.9, 0.5);
  cairo_move_to(cr, 0.1, 0.9);
  cairo_line_to(cr, 0.9, 0.9);
  cairo_stroke(cr);

  FINISH
}

static void _draw_triangle(cairo_t *cr, const gint flags)  // create triangle for both following icons
{
  /* initialize rotation and flip matrices */
  cairo_matrix_t hflip_matrix;
  cairo_matrix_init(&hflip_matrix, -1, 0, 0, 1, 1, 0);

  double C = cosf(-(M_PI / 2.0f)), S = sinf(-(M_PI / 2.0f)); // -90 degrees
  C = flags & CPF_DIRECTION_DOWN ? cosf(-(M_PI * 1.5f)) : C;
  S = flags & CPF_DIRECTION_DOWN ? sinf(-(M_PI * 1.5f)) : S;
  cairo_matrix_t rotation_matrix;
  cairo_matrix_init(&rotation_matrix, C, S, -S, C, 0.5 - C * 0.5 + S * 0.5, 0.5 - S * 0.5 - C * 0.5);

  /* scale and transform*/
  if(flags & CPF_DIRECTION_UP || flags & CPF_DIRECTION_DOWN)
    cairo_transform(cr, &rotation_matrix);
  else if(flags & CPF_DIRECTION_LEFT) // Flip x transformation
    cairo_transform(cr, &hflip_matrix);

  cairo_move_to(cr, 0.05, 0.5);
  cairo_line_to(cr, 0.05, 0.1);
  cairo_line_to(cr, 0.45, 0.5);
  cairo_line_to(cr, 0.05, 0.9);
  cairo_line_to(cr, 0.05, 0.5);
}

void dtgtk_cairo_paint_triangle(cairo_t *cr, gint x, int y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0, 0)

  _draw_triangle(cr, flags);
  cairo_stroke(cr);

  FINISH
}

void dtgtk_cairo_paint_solid_triangle(cairo_t *cr, gint x, int y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0, 0)

  _draw_triangle(cr, flags);
  cairo_stroke_preserve(cr);
  cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
  cairo_fill(cr);

  FINISH
}

void dtgtk_cairo_paint_arrow(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0, 0)

  cairo_matrix_t hflip_matrix;
  cairo_matrix_init(&hflip_matrix, -1, 0, 0, 1, 1, 0);

  double C = cosf(-(M_PI / 2.0)), S = sinf(-(M_PI / 2.0)); // -90 degrees
  C = flags & CPF_DIRECTION_UP ? cosf(-(M_PI * 1.5f)) : C;
  S = flags & CPF_DIRECTION_UP ? sinf(-(M_PI * 1.5f)) : S;
  cairo_matrix_t rotation_matrix;
  cairo_matrix_init(&rotation_matrix, C, S, -S, C, 0.5 - C * 0.5 + S * 0.5, 0.5 - S * 0.5 - C * 0.5);

  if(flags & CPF_DIRECTION_UP || flags & CPF_DIRECTION_DOWN)
    cairo_transform(cr, &rotation_matrix);
  else if(flags & CPF_DIRECTION_RIGHT) // Flip x transformation
    cairo_transform(cr, &hflip_matrix);

  cairo_move_to(cr, 0.2, 0.1);
  cairo_line_to(cr, 0.9, 0.5);
  cairo_line_to(cr, 0.2, 0.9);
  cairo_stroke(cr);

  FINISH
}

void dtgtk_cairo_paint_solid_arrow(cairo_t *cr, gint x, int y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0, 0)

  /* initialize rotation and flip matrices */
  cairo_matrix_t hflip_matrix;
  cairo_matrix_init(&hflip_matrix, -1, 0, 0, 1, 1, 0);

  double C = cosf(-(M_PI / 2.0f)), S = sinf(-(M_PI / 2.0f)); // -90 degrees
  C = flags & CPF_DIRECTION_DOWN ? cosf(-(M_PI * 1.5f)) : C;
  S = flags & CPF_DIRECTION_DOWN ? sinf(-(M_PI * 1.5f)) : S;
  cairo_matrix_t rotation_matrix;
  cairo_matrix_init(&rotation_matrix, C, S, -S, C, 0.5 - C * 0.5 + S * 0.5, 0.5 - S * 0.5 - C * 0.5);

  /* scale and transform*/
  if(flags & CPF_DIRECTION_UP || flags & CPF_DIRECTION_DOWN)
    cairo_transform(cr, &rotation_matrix);
  else if(flags & CPF_DIRECTION_LEFT) // Flip x transformation
    cairo_transform(cr, &hflip_matrix);

  cairo_move_to(cr, 0.2, 0.1);
  cairo_line_to(cr, 0.9, 0.5);
  cairo_line_to(cr, 0.2, 0.9);
  cairo_fill(cr);

  FINISH
}

void dtgtk_cairo_paint_line_arrow(cairo_t *cr, gint x, int y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0, 0)

  cairo_move_to(cr, 0.1, 0.5);
  cairo_line_to(cr, 0.9, 0.5);
  cairo_stroke(cr);

  /* initialize flip matrices */
  cairo_matrix_t hflip_matrix;
  cairo_matrix_init(&hflip_matrix, -1, 0, 0, 1, 1, 0);

  /* scale and transform*/
  if(flags & CPF_DIRECTION_LEFT) // Flip x transformation
    cairo_transform(cr, &hflip_matrix);

  cairo_move_to(cr, 0.4, 0.1);
  cairo_line_to(cr, 0.0, 0.5);
  cairo_line_to(cr, 0.4, 0.9);
  cairo_fill(cr);

  FINISH
}

void dtgtk_cairo_paint_sortby(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1.2, 0, 0)

  cairo_move_to(cr, 0.1, 0.05);
  cairo_line_to(cr, 0.1, 0.95);
  cairo_move_to(cr, 0.0, 0.80);
  cairo_line_to(cr, 0.1, 0.95);
  cairo_line_to(cr, 0.2, 0.80);
  cairo_stroke(cr);

  if(flags & CPF_DIRECTION_UP)
  {
    cairo_move_to(cr, 0.35, 0.05);
    cairo_line_to(cr, 0.45, 0.05);
    cairo_move_to(cr, 0.35, 0.35);
    cairo_line_to(cr, 0.65, 0.35);
    cairo_move_to(cr, 0.35, 0.65);
    cairo_line_to(cr, 0.85, 0.65);
    cairo_move_to(cr, 0.35, 0.95);
    cairo_line_to(cr, 1.00, 0.95);
  }
  else
  {
    cairo_move_to(cr, 0.35, 0.05);
    cairo_line_to(cr, 1.00, 0.05);
    cairo_move_to(cr, 0.35, 0.35);
    cairo_line_to(cr, 0.85, 0.35);
    cairo_move_to(cr, 0.35, 0.65);
    cairo_line_to(cr, 0.65, 0.65);
    cairo_move_to(cr, 0.35, 0.95);
    cairo_line_to(cr, 0.45, 0.95);
  }
  cairo_stroke(cr);

  FINISH
}

void dtgtk_cairo_paint_flip(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0, 0)

  double C = cosf(-1.570796327f), S = sinf(-1.570796327f);
  cairo_matrix_t rotation_matrix;
  cairo_matrix_init(&rotation_matrix, C, S, -S, C, 0.5 - C * 0.5 + S * 0.5, 0.5 - S * 0.5 - C * 0.5);

  if((flags & CPF_DIRECTION_UP)) // Rotate -90 degrees
    cairo_transform(cr, &rotation_matrix);

  cairo_move_to(cr, 0.05, 0.4);
  cairo_line_to(cr, 0.05, 0);
  cairo_line_to(cr, 0.95, 0.4);
  cairo_line_to(cr, 0.05, 0.4);
  cairo_fill_preserve(cr);
  cairo_stroke(cr);

  cairo_move_to(cr, 0.05, 0.6);
  cairo_line_to(cr, 0.05, 1.0);
  cairo_line_to(cr, 0.95, 0.6);
  cairo_line_to(cr, 0.05, 0.6);
  cairo_stroke(cr);

  FINISH
}

void dtgtk_cairo_paint_reset(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0, 0)

  cairo_arc(cr, 0.5, 0.5, 0.46, 0, 6.2832);
  cairo_move_to(cr, 0.5, 0.32);
  cairo_line_to(cr, 0.5, 0.68);
  cairo_stroke(cr);

  FINISH
}

void dtgtk_cairo_paint_store(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0, 0)

  cairo_move_to(cr, 0.275, 0.1);
  cairo_line_to(cr, 0.1, 0.1);
  cairo_line_to(cr, 0.1, 0.9);
  cairo_line_to(cr, 0.9, 0.9);
  cairo_line_to(cr, 0.9, 0.175);
  cairo_line_to(cr, 0.825, 0.1);
  cairo_line_to(cr, 0.825, 0.5);
  cairo_line_to(cr, 0.275, 0.5);
  cairo_line_to(cr, 0.275, 0.1);

  cairo_stroke(cr);
  cairo_rectangle(cr, 0.5, 0.025, 0.17, 0.275);
  cairo_fill(cr);
  cairo_stroke(cr);

  FINISH
}

void dtgtk_cairo_paint_switch(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0, 0)

  cairo_arc(cr, 0.5, 0.5, 0.46, (-50 * 3.145 / 180), (230 * 3.145 / 180));
  cairo_move_to(cr, 0.5, 0.0);
  cairo_line_to(cr, 0.5, 0.5);
  cairo_stroke(cr);

  if(flags & CPF_FOCUS) // If focused add some diffuse light
  {
    cairo_arc(cr, 0.5, 0.5, 0.45, 0.0, 2*M_PI);
    cairo_clip(cr);
    cairo_paint_with_alpha(cr, 0.4);
  }

  FINISH
}

void dtgtk_cairo_paint_switch_inactive(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, .3f, 0, 0)

  cairo_arc(cr, 0.5, 0.5, 0.46, (-50 * 3.145 / 180), (230 * 3.145 / 180));
  cairo_move_to(cr, 0.5, 0.0);
  cairo_line_to(cr, 0.5, 0.5);
  cairo_stroke(cr);

  if(flags & CPF_FOCUS) // If focused add some diffuse light
  {
    cairo_arc(cr, 0.5, 0.5, 0.45, 0.0, 2*M_PI);
    cairo_clip(cr);
    cairo_paint_with_alpha(cr, 0.4);
  }

  FINISH
}

void dtgtk_cairo_paint_switch_on(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0, 0)

  cairo_arc(cr, 0.5, 0.5, 0.50, 0, 2 * M_PI);
  cairo_stroke(cr);

  cairo_arc(cr, 0.5, 0.5, 0.30, 0, 2 * M_PI);
  cairo_fill(cr);

  if(flags & CPF_FOCUS)
  {
    cairo_arc(cr, 0.5, 0.5, 0.50, 0.0, 2*M_PI);
    cairo_clip(cr);
    cairo_paint_with_alpha(cr, 0.5);
  }

  FINISH
}

void dtgtk_cairo_paint_switch_off(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0, 0)

  cairo_arc(cr, 0.5, 0.5, 0.50, 0, 2 * M_PI);
  cairo_stroke(cr);

  FINISH
}

void dtgtk_cairo_paint_switch_deprecated(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0, 0)

  cairo_move_to(cr, 0.1, 0.1);
  cairo_line_to(cr, 0.9, 0.9);
  cairo_move_to(cr, 0.1, 0.9);
  cairo_line_to(cr, 0.9, 0.1);
  cairo_stroke(cr);

  FINISH
}

void dtgtk_cairo_paint_plus(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  dtgtk_cairo_paint_plusminus(cr, x, y, w, h, flags | CPF_ACTIVE, data);
}

void dtgtk_cairo_paint_plusminus(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0, 0)

  cairo_arc(cr, 0.5, 0.5, 0.45, 0, 2 * M_PI);
  cairo_fill(cr);
  cairo_set_source_rgba(cr, 0.2, 0.2, 0.2, 1.0);

  if((flags & CPF_ACTIVE))
  {
    cairo_move_to(cr, 0.5, 0.25);
    cairo_line_to(cr, 0.5, 0.75);
    cairo_move_to(cr, 0.25, 0.5);
    cairo_line_to(cr, 0.75, 0.5);
    cairo_stroke(cr);
  }
  else
  {
    cairo_move_to(cr, 0.25, 0.5);
    cairo_line_to(cr, 0.75, 0.5);
    cairo_stroke(cr);
  }

  cairo_identity_matrix(cr);

  FINISH
}

void dtgtk_cairo_paint_square_plus(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0, 0)

  _rounded_rectangle(cr);
  cairo_fill(cr);

  cairo_set_source_rgba(cr, 0.2, 0.2, 0.2, 1.0);
  cairo_move_to(cr, 0.5, 0.25);
  cairo_line_to(cr, 0.5, 0.75);
  cairo_move_to(cr, 0.25, 0.5);
  cairo_line_to(cr, 0.75, 0.5);
  cairo_stroke(cr);

  FINISH
}

void dtgtk_cairo_paint_sorting(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0, 0)

  cairo_move_to(cr, 0.4, 0.1);
  cairo_line_to(cr, 0.4, 0.9);
  cairo_line_to(cr, 0.2, 0.7);
  cairo_move_to(cr, 0.6, 0.9);
  cairo_line_to(cr, 0.6, 0.1);
  cairo_line_to(cr, 0.8, 0.3);
  cairo_stroke(cr);

  FINISH
}

void dtgtk_cairo_paint_plus_simple(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0, 0)

  cairo_move_to(cr, 0.5, 0.1);
  cairo_line_to(cr, 0.5, 0.9);
  cairo_move_to(cr, 0.1, 0.5);
  cairo_line_to(cr, 0.9, 0.5);
  cairo_stroke(cr);

  FINISH
}

void dtgtk_cairo_paint_minus_simple(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0, 0)

  cairo_move_to(cr, 0.1, 0.5);
  cairo_line_to(cr, 0.9, 0.5);
  cairo_stroke(cr);

  FINISH
}

void dtgtk_cairo_paint_multiply_small(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0, 0)

  cairo_move_to(cr, 0.3, 0.3);
  cairo_line_to(cr, 0.7, 0.7);
  cairo_move_to(cr, 0.7, 0.3);
  cairo_line_to(cr, 0.3, 0.7);
  cairo_stroke(cr);

  FINISH
}

void dtgtk_cairo_paint_treelist(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0, 0)

  cairo_move_to(cr, 0.05, 0.05);
  cairo_line_to(cr, 0.125, 0.05);
  cairo_move_to(cr, 0.25, 0.35);
  cairo_line_to(cr, 0.325, 0.35);
  cairo_move_to(cr, 0.45, 0.65);
  cairo_line_to(cr, 0.525, 0.65);
  cairo_move_to(cr, 0.25, 0.95);
  cairo_line_to(cr, 0.325, 0.95);
  cairo_stroke(cr);

  cairo_move_to(cr, 0.35, 0.05);
  cairo_line_to(cr, 0.95, 0.05);
  cairo_move_to(cr, 0.55, 0.35);
  cairo_line_to(cr, 0.95, 0.35);
  cairo_move_to(cr, 0.75, 0.65);
  cairo_line_to(cr, 0.95, 0.65);
  cairo_move_to(cr, 0.55, 0.95);
  cairo_line_to(cr, 0.95, 0.95);
  cairo_stroke(cr);

  FINISH
}

void dtgtk_cairo_paint_invert(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(0.95, 1, 0, 0)

  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
  cairo_arc(cr, 0.5, 0.5, 0.46, 0, 2.0 * M_PI);
  cairo_stroke(cr);
  cairo_arc(cr, 0.5, 0.5, 0.46, 3.0 * M_PI / 2.0, M_PI / 2.0);
  cairo_fill(cr);

  FINISH
}

void dtgtk_cairo_paint_masks_eye(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0, 0)

  double dashed[] = { 0.2, 0.2 };
  int len = sizeof(dashed) / sizeof(dashed[0]);
  cairo_set_dash(cr, dashed, len, 0);

  cairo_arc(cr, 0.75, 0.75, 0.75, 2.8, 4.7124);
  cairo_stroke(cr);

  cairo_move_to(cr, 0.4, 0.1);
  cairo_line_to(cr, 0.3, 0.8);
  cairo_line_to(cr, 0.55, 0.716667);
  cairo_line_to(cr, 0.65, 1.016667);
  cairo_line_to(cr, 0.75, 0.983333);
  cairo_line_to(cr, 0.65, 0.683333);
  cairo_line_to(cr, 0.9, 0.6);
  cairo_line_to(cr, 0.4, 0.1);
  cairo_fill(cr);

  FINISH
}

void dtgtk_cairo_paint_masks_circle(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1.1, 1, 0, 0)

  cairo_arc(cr, 0.5, 0.5, 0.4, 0, 6.2832);
  cairo_stroke(cr);

  FINISH
}

void dtgtk_cairo_paint_masks_ellipse(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1.15, 1, 0, 0)

  cairo_save(cr);
  cairo_translate(cr, 0.1465, 0);
  cairo_scale(cr, 0.707, 1);
  cairo_arc(cr, 0.5, 0.5, 0.4, 0, 6.2832);
  cairo_restore(cr);
  cairo_stroke(cr);

  FINISH
}

void dtgtk_cairo_paint_masks_gradient(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, -0.05, -0.05)

  cairo_rectangle(cr, 0.1, 0.1, 0.9, 0.9);
  cairo_stroke_preserve(cr);
  cairo_pattern_t *pat = NULL;
  pat = cairo_pattern_create_linear(0.5, 0.1, 0.5, 0.9);
  cairo_pattern_add_color_stop_rgba(pat, 0.1, 0.6, 0.6, 0.6, 0.9);
  cairo_pattern_add_color_stop_rgba(pat, 0.9, 0.2, 0.2, 0.2, 0.9);
  cairo_set_source(cr, pat);
  cairo_fill(cr);
  cairo_pattern_destroy(pat);

  FINISH
}

void dtgtk_cairo_paint_masks_path(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1.05, 1, 0, 0)

  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
  cairo_move_to(cr, 0.1, 0.9);
  cairo_curve_to(cr, 0.1, 0.5, 0.9, 0.6, 0.9, 0.1);
  cairo_stroke(cr);
  cairo_move_to(cr, 0.5, 0.5);
  cairo_line_to(cr, 0.3, 0.1);
  cairo_set_line_width(cr, 0.1);
  cairo_stroke(cr);

  FINISH
}

void dtgtk_cairo_paint_masks_vertgradient(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0, 0)

  cairo_rectangle(cr, 0.1, 0.1, 0.9, 0.9);
  cairo_stroke_preserve(cr);
  cairo_pattern_t *pat = NULL;
  pat = cairo_pattern_create_linear(0.1, 0.5, 0.9, 0.5);
  cairo_pattern_add_color_stop_rgba(pat, 0.1, 0.6, 0.6, 0.6, 0.9);
  cairo_pattern_add_color_stop_rgba(pat, 1.0, 0.2, 0.2, 0.2, 0.9);
  cairo_rectangle(cr, 0.1, 0.1, 0.8, 0.8);
  cairo_set_source(cr, pat);
  cairo_fill(cr);
  cairo_pattern_destroy(pat);

  FINISH
}

void dtgtk_cairo_paint_masks_brush_and_inverse(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0, 0)

  cairo_arc(cr, 0.5, 0.5, 0.4, 0, 2.0 * M_PI);
  cairo_stroke(cr);
  cairo_arc(cr, 0.5, 0.5, 0.4, 3.0 * M_PI / 2.0, M_PI / 2.0);
  cairo_fill(cr);

  cairo_move_to(cr, -0.05, 1.0);
  cairo_arc_negative(cr, 0.25, 0.85, 0.15, 0.5 * M_PI, 1.12 * M_PI);
  cairo_arc(cr, -0.236, 0.72, 0.35, 0.08 * M_PI, 0.26 * M_PI);
  cairo_close_path(cr);
  cairo_fill(cr);

  cairo_set_line_width(cr, 0.01);
  cairo_arc(cr, 0.98, 0.0, 0.055, 1.2 * M_PI, 0.2 * M_PI);
  cairo_arc(cr, 0.48, 0.72, 0.09, 0.2 * M_PI, 1.2 * M_PI);
  cairo_close_path(cr);
  cairo_stroke(cr);

  FINISH
}

void dtgtk_cairo_paint_masks_brush(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(0.90, 1, 0., 0.)

  cairo_move_to(cr, -0.05, 1.0);
  cairo_arc_negative(cr, 0.25, 0.85, 0.15, 0.5 * M_PI, 1.12 * M_PI);
  cairo_arc(cr, -0.236, 0.72, 0.35, 0.08 * M_PI, 0.26 * M_PI);
  cairo_close_path(cr);
  cairo_stroke(cr);
  cairo_move_to(cr, 0, 1);
  cairo_arc_negative(cr, 0.20, 0.80, 0.10, 0.4 * M_PI, 1.9 * M_PI);
  cairo_stroke(cr);

  cairo_set_line_width(cr, 0.01);
  cairo_arc(cr, 0.98, 0.0, 0.055, 1.2 * M_PI, 0.2 * M_PI);
  cairo_arc(cr, 0.48, 0.72, 0.09, 0.2 * M_PI, 1.2 * M_PI);
  cairo_close_path(cr);
  cairo_fill(cr);

  FINISH
}

void dtgtk_cairo_paint_masks_uniform(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(0.95, 1, 0, 0)

  cairo_arc(cr, 0.5, 0.5, 0.5, -M_PI, M_PI);
  cairo_stroke(cr);

  FINISH
}

void dtgtk_cairo_paint_masks_drawn(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(0.9, 1, 0, 0)

  // main cylinder
  cairo_move_to(cr, 1.0, 1.0);
  cairo_line_to(cr, 0.9, 0.7);
  cairo_line_to(cr, 0.2, 0.0);
  cairo_line_to(cr, 0.0, 0.2);
  cairo_line_to(cr, 0.7, 0.9);
  cairo_line_to(cr, 1.0, 1.0);
  cairo_stroke(cr);

  // line
  cairo_move_to(cr, 0.8, 0.8);
  cairo_line_to(cr, 0.15, 0.15);
  cairo_stroke(cr);

  // junction
  cairo_move_to(cr, 0.9, 0.7);
  cairo_line_to(cr, 0.7, 0.9);
  cairo_stroke(cr);

  // tip
  cairo_move_to(cr, 1.05, 1.05);
  cairo_line_to(cr, 0.95, 0.95);
  cairo_stroke(cr);

  FINISH
}

/** draws an arc with a B&W gradient following the arc path.
 *  nb_steps must be adjusted depending on the displayed size of the element, 16 is fine for small buttons*/
void _gradient_arc(cairo_t *cr, double lw, int nb_steps, double x_center, double y_center, double radius,
                   double angle_from, double angle_to, double color_from, double color_to, double alpha)
{
  cairo_set_line_width(cr, lw);

  double *portions = malloc(sizeof(double) * (1 + nb_steps));

  // note: cairo angles seems to be shifted by M_PI relatively to the unit circle
  angle_from = angle_from + M_PI;
  angle_to = angle_to + M_PI;
  double step = (angle_to - angle_from) / nb_steps;
  for(int i = 0; i < nb_steps; i++) portions[i] = angle_from + i * step;
  portions[nb_steps] = angle_to;

  for(int i = 0; i < nb_steps; i++)
  {
    double color = color_from + i * (color_to - color_from) / nb_steps;
    cairo_set_source_rgba(cr, color, color, color, alpha);
    cairo_arc(cr, x_center, y_center, radius, portions[i], portions[i + 1]);
    cairo_stroke(cr);
  }
  free(portions);
}

void dtgtk_cairo_paint_masks_parametric(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(0.95, 1, 0, 0)

  cairo_pattern_t *p = cairo_get_source (cr);
  double r, g, b, a;
  double start;
  cairo_pattern_get_rgba (p, &r, &g, &b, &a);

  start = ((flags & CPF_PRELIGHT) && (r < 0.5)) ? 0.8 :  r / 4.0;
   _gradient_arc(cr, 0.125, 16, 0.5, 0.5, 0.5, -M_PI / 3.0, M_PI + M_PI / 3.0, start, r, a);

  // draw one tick up right
  cairo_move_to(cr, 1, 0.2);
  cairo_line_to(cr, 1.2, 0.2);
  cairo_line_to(cr, 1.1, 0.0);
  cairo_fill(cr);
  // draw another tick center right
  cairo_move_to(cr, 1.1, 0.6);
  cairo_line_to(cr, 1.325, 0.55);
  cairo_line_to(cr, 1.275, 0.75);
  cairo_fill(cr);

  FINISH
}

void dtgtk_cairo_paint_masks_drawn_and_parametric(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags,
                                                  void *data)
{
  PREAMBLE(1.05, 1, -0.1, -0.05)

  cairo_pattern_t *p = cairo_get_source (cr);
  double r, g, b, a;
  double start;
  cairo_pattern_get_rgba (p, &r, &g, &b, &a);

  start = ((flags & CPF_PRELIGHT) && (r < 0.5)) ? 0.8 :  r / 4.0;
  cairo_save(cr);
  _gradient_arc(cr, 0.125, 16, 0.75, 0.6, 0.4, -M_PI / 3.0, M_PI + M_PI / 3.0, start, r, a);

  // draw one tick up right
  cairo_move_to(cr, 1.2, 0.35);
  cairo_line_to(cr, 1.35, 0.35);
  cairo_line_to(cr, 1.275, 0.15);
  cairo_fill(cr);
  // draw another tick center right
  cairo_move_to(cr, 1.25, 0.7);
  cairo_line_to(cr, 1.4, 0.6);
  cairo_line_to(cr, 1.4, 0.8);
  cairo_fill(cr);
  cairo_restore(cr);

  cairo_scale(cr, 0.8, 0.8);
  cairo_translate(cr, 0.05, -0.05);

  // main cylinder
  cairo_move_to(cr, 1.0, 1.0);
  cairo_line_to(cr, 0.9, 0.7);
  cairo_line_to(cr, 0.2, 0.0);
  cairo_line_to(cr, 0.0, 0.2);
  cairo_line_to(cr, 0.7, 0.9);
  cairo_line_to(cr, 1.0, 1.0);
  cairo_stroke(cr);

  // line
  cairo_move_to(cr, 0.8, 0.8);
  cairo_line_to(cr, 0.25, 0.25);
  cairo_stroke(cr);

  // junction
  cairo_move_to(cr, 0.9, 0.7);
  cairo_line_to(cr, 0.7, 0.9);
  cairo_stroke(cr);

  // tip
  cairo_move_to(cr, 1.05, 1.05);
  cairo_line_to(cr, 0.95, 0.95);
  cairo_stroke(cr);

  FINISH
}

void dtgtk_cairo_paint_masks_raster(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0, 0)

  cairo_arc(cr, 0.5, 0.5, 0.5, 0, 2 * M_PI);
  cairo_clip(cr);
  cairo_new_path(cr);

  for(int i = 0; i < 4; i++)
    for(int j = 0; j < 4; j++)
      if((i + j) % 2)
      {
        cairo_rectangle(cr, i / 4.0, j / 4.0, 1.0 / 4.0, 1.0 / 4.0);
        cairo_fill(cr);
      }

  FINISH
}

void dtgtk_cairo_paint_masks_multi(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0, 0)

  cairo_arc(cr, 0.3, 0.3, 0.3, 0, 6.2832);
  cairo_stroke(cr);
  cairo_move_to(cr, 0.0, 1.0);
  cairo_curve_to(cr, 0.0, 0.5, 1.0, 0.6, 1.0, 0.0);
  cairo_stroke(cr);

  FINISH
}

void dtgtk_cairo_paint_masks_inverse(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0, 0)

  cairo_arc(cr, 0.5, 0.5, 0.46, 0, 2.0 * M_PI);
  cairo_stroke(cr);
  cairo_arc(cr, 0.5, 0.5, 0.46, 3.0 * M_PI / 2.0, M_PI / 2.0);
  cairo_fill(cr);

  FINISH
}

void dtgtk_cairo_paint_masks_union(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  // note : as the icon is not square, we don't want PREAMBLE macro
  // we want 2 round of radius R that intersect in the middle,
  // so the width needs R + R*0.8 + R*0.8 + R = R*3.6
  // with a safety belt of 5% to be sure the stroke is draw inside the area
  const float r = fminf(w / 3.6, h / 2.0) * 0.95;
  const float padding_left = (w - r * 3.6) / 2.0;
  cairo_arc(cr, padding_left + r, h / 2.0, r, 0, 2.0 * M_PI);
  cairo_arc(cr, padding_left + r * 2.6, h / 2.0, r, 0, 2.0 * M_PI);
  cairo_fill(cr);
}

void dtgtk_cairo_paint_masks_intersection(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  // note : as the icon is not square, we don't want PREAMBLE macro
  // we want 2 round of radius R that intersect in the middle,
  // so the width needs R + R*0.8 + R*0.8 + R = R*3.6
  // with a safety belt of *0.95 to be sure the stroke is draw inside the area
  const float r = fminf(w / 3.6, h / 2.0) * 0.95;
  const float padding_left = (w - r * 3.6) / 2.0;

  // we draw the outline of the 2 circles
  cairo_save(cr);
  cairo_set_line_width(cr, cairo_get_line_width(cr) * 0.5);
  cairo_arc(cr, padding_left + r, h / 2.0, r, 0, 2.0 * M_PI);
  cairo_stroke(cr);
  cairo_arc(cr, padding_left + r * 2.6, h / 2.0, r, 0, 2.0 * M_PI);
  cairo_stroke(cr);
  cairo_restore(cr);

  // we draw the intersection of the 2 circles we slightly different radius so they are more visible
  cairo_push_group(cr);
  cairo_arc(cr, padding_left + r * 1.3, h / 2.0, r * 0.85, 0, 2.0 * M_PI);
  cairo_fill(cr);
  cairo_set_operator(cr, CAIRO_OPERATOR_IN);
  cairo_arc(cr, padding_left + r * 2.3, h / 2.0, r * 0.85, 0, 2.0 * M_PI);
  cairo_fill(cr);
  cairo_pop_group_to_source(cr);
  cairo_paint(cr);
}

void dtgtk_cairo_paint_masks_difference(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  // note : as the icon is not square, we don't want PREAMBLE macro
  // we want 2 round of radius R that intersect in the middle,
  // so the width needs R + R*0.8 + R*0.8 + R = R*3.6
  // with a safety belt of *0.95 to be sure the stroke is draw inside the area
  const float r = fminf(w / 3.6, h / 2.0) * 0.95;
  const float padding_left = (w - r * 3.6) / 2.0;

  // we draw and fill the first circle
  cairo_arc(cr, padding_left + r, h / 2.0, r, 0, 2.0 * M_PI);
  cairo_fill(cr);

  // then erase the second circle
  cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
  cairo_arc(cr, padding_left + r * 2.6, h / 2.0, r, 0, 2.0 * M_PI);
  cairo_fill(cr);

  // last we draw the outline of the second circle
  cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
  cairo_set_line_width(cr, cairo_get_line_width(cr) * 0.5);
  cairo_arc(cr, padding_left + r * 2.6, h / 2.0, r, 0, 2.0 * M_PI);
  cairo_stroke(cr);
}

void dtgtk_cairo_paint_masks_exclusion(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  // note : as the icon is not square, we don't want PREAMBLE macro
  // we want 2 round of radius R that intersect in the middle,
  // so the width needs R + R*0.8 + R*0.8 + R = R*3.6
  // with a safety belt of *0.95 to be sure the stroke is draw inside the area
  const float r = fminf(w / 3.6, h / 2.0) * 0.95;
  const float padding_left = (w - r * 3.6) / 2.0;

  // we draw the first circle without the excluded area
  cairo_save(cr);
  cairo_set_line_width(cr, cairo_get_line_width(cr) * 0.5);
  cairo_arc(cr, padding_left + r, h / 2.0, r, 0, 2.0 * M_PI);
  cairo_fill(cr);
  cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
  cairo_arc(cr, padding_left + r * 2.3, h / 2.0, r * 0.85, 0, 2.0 * M_PI);
  cairo_fill(cr);
  cairo_restore(cr);

  // same for the second circle
  cairo_push_group(cr);
  cairo_arc(cr, padding_left + r * 2.6, h / 2.0, r, 0, 2.0 * M_PI);
  cairo_fill(cr);
  cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
  cairo_arc(cr, padding_left + r * 1.3, h / 2.0, r * 0.85, 0, 2.0 * M_PI);
  cairo_fill(cr);
  cairo_pop_group_to_source(cr);
  cairo_paint(cr);
}
void dtgtk_cairo_paint_masks_used(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0, 0)

  cairo_arc(cr, 0.5, 0.5, 0.35, 0, 2.0 * M_PI);
  cairo_move_to(cr, 0.5, 0.15);
  cairo_line_to(cr, 0.5, 0.5);
  cairo_stroke(cr);

  FINISH
}

void dtgtk_cairo_paint_eye(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  dtgtk_cairo_paint_eye_toggle(cr, x, y, w, h, flags & ~CPF_ACTIVE, data);
}

void dtgtk_cairo_paint_eye_toggle(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0, 0)

  cairo_arc(cr, 0.5, 0.5, 0.16, 0, 2 * M_PI);
  cairo_fill(cr);

  cairo_translate(cr, 0, 0.22);
  cairo_save(cr);
  cairo_scale(cr, 1.0, 0.55);
  cairo_arc(cr, 0.5, 0.5, 0.45, 0, 2 * M_PI);
  cairo_restore(cr);
  cairo_stroke(cr);

  if((flags & CPF_ACTIVE))
  {
    cairo_translate(cr, 0, -0.18);
    cairo_move_to(cr, 0.1, 0.9);
    cairo_line_to(cr, 0.9, 0.1);
    cairo_stroke(cr);
  }

  FINISH
}

void dtgtk_cairo_paint_timer(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0, 0)

  cairo_arc(cr, 0.5, 0.5, 0.5, (-80 * 3.145 / 180), (150 * 3.145 / 180));
  cairo_line_to(cr, 0.5, 0.5);
  cairo_stroke(cr);

  FINISH
}

void dtgtk_cairo_paint_grid(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0, 0)

  _rounded_rectangle(cr);

  // then create grid lines
  cairo_move_to(cr, 0.35, 0.15);
  cairo_line_to(cr, 0.35, 0.85);
  cairo_stroke(cr);

  cairo_move_to(cr, 0.65, 0.15);
  cairo_line_to(cr, 0.65, 0.85);
  cairo_stroke(cr);

  cairo_move_to(cr, 0.15, 0.35);
  cairo_line_to(cr, 0.85, 0.35);
  cairo_stroke(cr);

  cairo_move_to(cr, 0.15, 0.65);
  cairo_line_to(cr, 0.85, 0.65);
  cairo_stroke(cr);

  FINISH
}

void dtgtk_cairo_paint_focus_peaking(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1.2, 1, 0, 0)

  // stigmometer aka split focusing screen aka Dodin's prism
  cairo_arc(cr, 0.5, 0.5, 0.2, 0, 2. * M_PI);
  cairo_move_to(cr, 0.3, 0.5);
  cairo_line_to(cr, 0.7, 0.5);
  cairo_stroke(cr);

  // corners
  const double center = 0.5;
  const double width = 1.;
  const double height = width * 3. / 4.;
  const double offset_h = height / 2.;
  const double offset_w = width / 2.;

  const double tick_length = 0.2;
  const double radius = 0.1;

  const double left = center - offset_w;
  const double right = center + offset_w;
  const double top = center - offset_h;
  const double bottom = center + offset_h;

  const double degrees = M_PI / 180.0;

  /// north west
  cairo_move_to(cr, left, top + tick_length);
  cairo_arc (cr, left + radius, top + radius, radius, 180 * degrees, 270 * degrees);
  cairo_line_to(cr, left + tick_length, top);
  cairo_stroke(cr);

  // south west
  cairo_move_to(cr, left + tick_length, bottom);
  cairo_arc (cr, left + radius, bottom - radius, radius, 90 * degrees, 180 * degrees);
  cairo_line_to(cr, left, bottom - tick_length);
  cairo_stroke(cr);

  // south east
  cairo_move_to(cr, right, bottom - tick_length);
  cairo_arc (cr, right - radius, bottom - radius, radius, 0 * degrees, 90 * degrees);
  cairo_line_to(cr, right - tick_length, bottom);
  cairo_stroke(cr);

  // north east
  cairo_move_to(cr, right - tick_length, top);
  cairo_arc (cr, right - radius, top + radius, radius, -90 * degrees, 0 * degrees);
  cairo_line_to(cr, right, top + tick_length);
  cairo_stroke(cr);

  FINISH
}

void dtgtk_cairo_paint_camera(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0, 0)

  // lens
  cairo_arc(cr, 0.5, 0.5, 0.2, 0, 2. * M_PI);
  cairo_stroke(cr);
  cairo_arc(cr, 0.5, 0.5, 0.1, M_PI, M_PI+M_PI_2);
  cairo_stroke(cr);

  // body

  cairo_move_to(cr, 0, 0.25);
  cairo_line_to(cr, 0, 0.85);
  cairo_line_to(cr, 0.95, 0.85);
  cairo_line_to(cr, 0.95, 0.25);
  cairo_line_to(cr, 0.75, 0.25);
  cairo_line_to(cr, 0.65, 0.15);
  cairo_line_to(cr, 0.35, 0.15);
  cairo_line_to(cr, 0.25, 0.25);
  cairo_close_path(cr);
  cairo_stroke(cr);

  FINISH
}

void dtgtk_cairo_paint_histogram_scope(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0, 0)

  cairo_move_to(cr, 0.0, 1.0);
  cairo_curve_to(cr, 0.3, 1.0, 0.3, 0.0, 0.5, 0.0);
  cairo_curve_to(cr, 0.7, 0.0, 0.7, 1.0, 1.0, 1.0);
  cairo_fill(cr);

  FINISH
}

void dtgtk_cairo_paint_waveform_scope(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0, 0)

  cairo_pattern_t *p_src = cairo_get_source(cr);
  double r, g, b, a;
  cairo_pattern_get_rgba(p_src, &r, &g, &b, &a);

  cairo_pattern_t *pat;
  pat = cairo_pattern_create_linear(0.0, 0.0, 0.0, 1.0);

  cairo_pattern_add_color_stop_rgba(pat, 0.0, r, g, b, a * 0.0);
  cairo_pattern_add_color_stop_rgba(pat, 0.1, r, g, b, a * 0.1);
  cairo_pattern_add_color_stop_rgba(pat, 0.5, r, g, b, a * 1.0);
  cairo_pattern_add_color_stop_rgba(pat, 0.6, r, g, b, a * 1.0);
  cairo_pattern_add_color_stop_rgba(pat, 1.0, r, g, b, a * 0.2);

  cairo_rectangle(cr, 0.0, 0.0, 0.3, 0.9);
  cairo_set_source(cr, pat);
  cairo_fill(cr);

  cairo_save(cr);
  cairo_scale(cr, 1.0, -1.0);
  cairo_translate(cr, 0.0, -1.0);
  cairo_rectangle(cr, 0.25, 0.0, 0.5, 1.0);
  cairo_set_source(cr, pat);
  cairo_fill(cr);
  cairo_restore(cr);

  cairo_rectangle(cr, 0.7, 0.0, 0.3, 0.9);
  cairo_set_source(cr, pat);
  cairo_fill(cr);

  cairo_pattern_destroy(pat);

  FINISH
}

void dtgtk_cairo_paint_vectorscope(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0, 0)

  cairo_move_to(cr, 0.0, 0.3);
  cairo_curve_to(cr, 0.1, 0.0, 0.7, 0.3, 1.0, 0.7);
  cairo_curve_to(cr, 0.9, 0.8, 0.1, 0.8, 0.0, 0.3);
  cairo_fill(cr);

  FINISH
}

void dtgtk_cairo_paint_linear_scale(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0, 0)

  cairo_move_to(cr, 0.0, 1.0);
  cairo_line_to(cr, 1.0, 0.0);
  cairo_stroke(cr);

  FINISH
}

void dtgtk_cairo_paint_logarithmic_scale(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0, 0)

  cairo_move_to(cr, 0.0, 1.0);
  cairo_curve_to(cr, 0.0, 0.33, 0.66, 0.0, 1.0, 0.0);
  cairo_stroke(cr);

  FINISH
}

void dtgtk_cairo_paint_waveform_overlaid(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0, 0)

  cairo_pattern_t *p_src = cairo_get_source(cr);
  double r, g, b, a;
  cairo_pattern_get_rgba(p_src, &r, &g, &b, &a);

  cairo_pattern_t *pat;
  pat = cairo_pattern_create_linear(0.0, 0.0, 0.0, 1.0);

  cairo_pattern_add_color_stop_rgba(pat, 0.0, r, g * 0.7, b * 0.9, a * 0.2);
  cairo_pattern_add_color_stop_rgba(pat, 0.4, r * 0.9, g, b * 0.9, a * 0.8);
  cairo_pattern_add_color_stop_rgba(pat, 0.7, r, g * 0.9, b, a * 1.0);
  cairo_pattern_add_color_stop_rgba(pat, 1.0, r * 0.7, g * 0.5, b, a * 0.2);

  cairo_rectangle(cr, 0.0, 0.15, 1.0, 0.7);
  cairo_set_source(cr, pat);
  cairo_fill(cr);

  cairo_pattern_destroy(pat);

  FINISH
}

void dtgtk_cairo_paint_rgb_parade(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0, 0)

  cairo_pattern_t *pat;

  pat = cairo_pattern_create_linear(0.0, 0.0, 0.0, 1.0);
  cairo_pattern_add_color_stop_rgba(pat, 0.0, 0.8, 0.3, 0.3, 0.2);
  cairo_pattern_add_color_stop_rgba(pat, 0.4, 0.8, 0.3, 0.3, 0.7);
  cairo_pattern_add_color_stop_rgba(pat, 1.0, 0.8, 0.3, 0.3, 0.3);
  cairo_rectangle(cr, 0.0, 0.1, 1.0/3.0, 0.7);
  cairo_set_source(cr, pat);
  cairo_fill(cr);
  cairo_pattern_destroy(pat);

  pat = cairo_pattern_create_linear(0.0, 0.0, 0.0, 1.0);
  cairo_pattern_add_color_stop_rgba(pat, 0.0, 0.4, 0.8, 0.4, 0.1);
  cairo_pattern_add_color_stop_rgba(pat, 0.6, 0.4, 0.8, 0.4, 0.8);
  cairo_pattern_add_color_stop_rgba(pat, 1.0, 0.4, 0.8, 0.4, 0.4);
  cairo_rectangle(cr, 1.0/3.0, 0.2, 1.0/3.0, 0.7);
  cairo_set_source(cr, pat);
  cairo_fill(cr);
  cairo_pattern_destroy(pat);

  pat = cairo_pattern_create_linear(0.0, 0.0, 0.0, 1.0);
  cairo_pattern_add_color_stop_rgba(pat, 0.0, 0.4, 0.4, 0.8, 0.4);
  cairo_pattern_add_color_stop_rgba(pat, 0.5, 0.4, 0.4, 0.8, 0.9);
  cairo_pattern_add_color_stop_rgba(pat, 1.0, 0.4, 0.4, 0.8, 0.5);
  cairo_rectangle(cr, 2.0/3.0, 0.1, 1.0/3.0, 0.7);
  cairo_set_source(cr, pat);
  cairo_fill(cr);
  cairo_pattern_destroy(pat);

  FINISH
}

void dtgtk_cairo_paint_luv(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0, 0)

  // FIXME: change icon to "u*v*"
  cairo_move_to(cr, 0.0, 0.0);
  cairo_line_to(cr, 0.0, 1.0);
  cairo_line_to(cr, 0.7, 1.0);
  cairo_stroke(cr);

  cairo_move_to(cr, 0.5, 0.0);
  cairo_curve_to(cr, 0.5, 0.4, 1.0, 0.4, 1.0, 0.0);
  cairo_stroke(cr);

  cairo_move_to(cr, 0.5, 0.5);
  cairo_line_to(cr, 0.75, 0.8);
  cairo_line_to(cr, 1.0, 0.5);
  cairo_stroke(cr);

  FINISH
}

void dtgtk_cairo_paint_jzazbz(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0, 0)

  // FIXME: change icon to "AzBz" or just "z"
  cairo_move_to(cr, 0.4, 0.0);
  cairo_curve_to(cr, 0.45, 1.0, 0.1, 1.0, 0.0, 0.8);
  cairo_stroke(cr);

  cairo_move_to(cr, 0.6, 0.5);
  cairo_line_to(cr, 1.0, 0.5);
  cairo_line_to(cr, 0.6, 1.0);
  cairo_line_to(cr, 1.0, 1.0);
  cairo_stroke(cr);

  FINISH
}

void dtgtk_cairo_paint_ryb(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1.2, 1, -0.05, -0.05)

  cairo_set_source_rgba(cr, 1.0, 0.0, 0.0, 0.8);
  cairo_arc(cr, 0.5, 0.5, 0.5, -0.83333 * M_PI, -0.16667 * M_PI);
  cairo_line_to(cr, 0.5, 0.5);
  cairo_close_path(cr);
  cairo_fill(cr);
  cairo_stroke(cr);

  cairo_set_source_rgba(cr, 1.0, 1.0, 0.0, 0.8);
  cairo_arc(cr, 0.5, 0.5, 0.5, 0.5 * M_PI, -0.83333 * M_PI);
  cairo_line_to(cr, 0.5, 0.5);
  cairo_close_path(cr);
  cairo_fill(cr);
  cairo_stroke(cr);

  cairo_set_source_rgba(cr, 0.0, 0.0, 1.0, 0.8);
  cairo_arc(cr, 0.5, 0.5, 0.5, -0.16667 * M_PI, 0.5 * M_PI);
  cairo_line_to(cr, 0.5, 0.5);
  cairo_close_path(cr);
  cairo_fill(cr);
  cairo_stroke(cr);

  FINISH
}

void dtgtk_cairo_paint_color_harmony(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0.5, 0.5)

  const double degrees = M_PI / 180.0;
  typedef struct
  {
    const char *name;
    const int sectors;
    const float angle[4];
    const float length[4];
  } ch_t;
  ch_t *ch = (ch_t *)data;

  cairo_arc(cr, 0.0, 0.0, 0.5, 0.0 * degrees, 360.0 * degrees);
  cairo_stroke(cr);

  for(int i = 0; i < ch->sectors; i++)
  {
    float angle = ch->angle[i] * 360.0 * degrees;
    cairo_save(cr);
    cairo_rotate(cr, angle);
    cairo_move_to(cr, 0.0, 0.0);
    cairo_line_to(cr, 0.0, -0.5);
    cairo_stroke(cr);
    cairo_arc(cr, 0.0, -0.5, 0.15, 0.0 * degrees, 360.0 * degrees);
    cairo_fill(cr);
    cairo_restore(cr);
  }

  FINISH
}

void dtgtk_cairo_paint_filmstrip(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  gdouble sw = 0.6;
  gdouble bend = 0.3;

  PREAMBLE(1, 1, 0, 0)

  cairo_scale(cr, 0.7, 0.7);
  cairo_translate(cr, 0.15, 0.15);

  /* s curve left */
  cairo_move_to(cr, 0.0, 1.0);
  cairo_curve_to(cr, 0.0, 0.0 + bend, (1.0 - sw), 1.0 - bend, (1.0 - sw), 0.0);
  cairo_stroke(cr);

  /* s curve down */
  cairo_move_to(cr, 1.0, 0.0);
  cairo_curve_to(cr, 1.0, 1.0 - bend, sw, 0.0 + bend, sw, 1.0);
  cairo_stroke(cr);

  /* filmstrip start,stop and divider */
  cairo_move_to(cr, 0, 1.0);
  cairo_line_to(cr, sw, 1.0);
  cairo_stroke(cr);
  cairo_move_to(cr, 1 - sw, 0.0);
  cairo_line_to(cr, 1.0, 0.0);
  cairo_stroke(cr);

  cairo_move_to(cr, 1 - sw, 0.5);
  cairo_line_to(cr, sw, 0.5);
  cairo_stroke(cr);

  FINISH
}

void dtgtk_cairo_paint_directory(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0, 0);

  const double degrees = M_PI / 180.0;

  cairo_new_sub_path (cr);
  cairo_arc (cr, 0.85, 0.35, 0.1, -90 * degrees, 0 * degrees);
  cairo_arc (cr, 0.8, 0.75, 0.1, 0 * degrees, 90 * degrees);
  cairo_arc (cr, 0.2, 0.75, 0.1, 90 * degrees, 180 * degrees);
  cairo_arc (cr, 0.15, 0.35, 0.1, 180 * degrees, 270 * degrees);
  cairo_close_path (cr);
  cairo_stroke(cr);

  cairo_move_to(cr, 0.1, 0.3);
  cairo_arc (cr, 0.2, 0.15, 0.1, 180 * degrees, 270 * degrees);
  cairo_arc (cr, 0.45, 0.15, 0.1, -90 * degrees, 0 * degrees);
  cairo_curve_to(cr, 0.6, 0.15, 0.75, 0.25, 0.9, 0.25);
  cairo_fill(cr);

  FINISH
}

void dtgtk_cairo_paint_refresh(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0, 0)

  if(flags & 1)
  {
    cairo_translate(cr, 1, 0);
    cairo_scale(cr, -1, 1);
  }

  cairo_move_to(cr, 0.65, 0.1);
  cairo_line_to(cr, 0.5, 0.2);
  cairo_line_to(cr, 0.65, 0.3);
  cairo_stroke(cr);

  cairo_arc(cr, 0.5, 0.5, 0.35, (-80 * 3.145 / 180), (220 * 3.145 / 180));
  cairo_stroke(cr);

  FINISH
}

void dtgtk_cairo_paint_perspective(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0, 0)

  if((flags & 3) == 1)
  {
    cairo_move_to(cr, 0.1, 0.9);
    cairo_line_to(cr, 0.3, 0.1);
    cairo_line_to(cr, 0.7, 0.1);
    cairo_line_to(cr, 0.9, 0.9);
    cairo_line_to(cr, 0.1, 0.9);
    cairo_stroke(cr);
  }
  else if((flags & 3) == 2)
  {
    cairo_move_to(cr, 0.1, 0.9);
    cairo_line_to(cr, 0.9, 0.7);
    cairo_line_to(cr, 0.9, 0.3);
    cairo_line_to(cr, 0.1, 0.1);
    cairo_line_to(cr, 0.1, 0.9);
    cairo_stroke(cr);
  }
  else if((flags & 3) == 3)
  {
    cairo_move_to(cr, 0.1, 0.9);
    cairo_line_to(cr, 0.9, 0.7);
    cairo_line_to(cr, 0.8, 0.2);
    cairo_line_to(cr, 0.3, 0.1);
    cairo_line_to(cr, 0.1, 0.9);
    cairo_stroke(cr);
  }

  FINISH
}

void dtgtk_cairo_paint_structure(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0, 0)

  cairo_move_to(cr, 0.1, 0.1);
  cairo_line_to(cr, 0.0, 0.9);
  cairo_stroke(cr);
  cairo_move_to(cr, 0.366, 0.1);
  cairo_line_to(cr, 0.33, 0.9);
  cairo_stroke(cr);
  cairo_move_to(cr, 0.633, 0.1);
  cairo_line_to(cr, 0.66, 0.9);
  cairo_stroke(cr);
  cairo_move_to(cr, 0.9, 0.1);
  cairo_line_to(cr, 1.0, 0.9);
  cairo_stroke(cr);

  FINISH
}

void dtgtk_cairo_paint_draw_structure(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0, 0)

  cairo_move_to(cr, 0.1, 0.1);
  cairo_line_to(cr, 0.1, 0.9);
  cairo_line_to(cr, 0.9, 0.9);
  cairo_line_to(cr, 0.9, 0.1);
  cairo_line_to(cr, 0.1, 0.1);
  cairo_stroke(cr);

  cairo_arc(cr, 0.1, 0.1, 0.1, 0.0, 2 * M_PI);
  cairo_stroke(cr);
  cairo_arc(cr, 0.1, 0.9, 0.1, 0.0, 2 * M_PI);
  cairo_stroke(cr);
  cairo_arc(cr, 0.9, 0.9, 0.1, 0.0, 2 * M_PI);
  cairo_stroke(cr);
  cairo_arc(cr, 0.9, 0.1, 0.1, 0.0, 2 * M_PI);
  cairo_stroke(cr);

  FINISH
}

void dtgtk_cairo_paint_cancel(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0, 0)

  cairo_move_to(cr, 0.85, 0.15);
  cairo_line_to(cr, 0.15, 0.85);
  cairo_move_to(cr, 0.15, 0.15);
  cairo_line_to(cr, 0.85, 0.85);
  cairo_stroke(cr);

  FINISH
}

void dtgtk_cairo_paint_aspectflip(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0, 0)

  if(flags & 1)
  {
    cairo_translate(cr, 0, 1);
    cairo_scale(cr, 1, -1);
  }

  cairo_move_to(cr, 0.65, 0.0);
  cairo_line_to(cr, 0.5, 0.05);
  cairo_line_to(cr, 0.6, 0.25);
  cairo_stroke(cr);

  cairo_arc(cr, 0.5, 0.5, 0.45, (-80 * 3.145 / 180), (220 * 3.145 / 180));
  cairo_stroke(cr);

  FINISH
}

void dtgtk_cairo_paint_styles(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(0.5 * 1.1, 1, 0.5 + 0.06, 0.5 -0.10)

  cairo_arc(cr, 0.250, 0.45, 0.5, 0.0, 2.0 * M_PI);
  cairo_stroke(cr);
  cairo_arc(cr, -0.58, 0.65, 0.30, 0.0, 2.0 * M_PI);
  cairo_stroke(cr);
  cairo_arc(cr, -0.38, -0.27, 0.4, 0.0, 2.0 * M_PI);
  cairo_stroke(cr);

  /* if its a popup menu */
  if(flags)
  {
    cairo_move_to(cr, 0.475, -0.93);
    cairo_line_to(cr, 0.15, -0.20);
    cairo_line_to(cr, 0.85, -0.20);
    cairo_fill(cr);
  }

  FINISH
}

void dtgtk_cairo_paint_label(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0, 0)

  cairo_push_group(cr);

  double r = 0.4;

  /* fill base color */
  cairo_arc(cr, 0.5, 0.5, r, 0.0, 2.0 * M_PI);

  const dt_colorlabels_enum color = (flags & 7);

  if(color < DT_COLORLABELS_LAST)
  {
    set_color(cr, darktable.bauhaus->colorlabels[color]);
  }
  else
  {
    cairo_set_line_width(cr, 0.1);
    cairo_set_source_rgb(cr, 0.9, 0.9, 0.9);
    cairo_move_to(cr, 0.15, 0.85);
    cairo_line_to(cr, 0.85, 0.15);
    cairo_stroke(cr);

    // then erase some parts around cross line
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_set_line_width(cr, 0.05);
    cairo_move_to(cr, 0.1, 0.78);
    cairo_line_to(cr, 0.78, 0.15);
    cairo_move_to(cr, 0.20, 0.9);
    cairo_line_to(cr, 0.92, 0.15);
    cairo_set_source_rgba(cr, 0, 1.0, 0, 1.0);
    cairo_stroke(cr);
  }
  cairo_fill(cr);

  cairo_pop_group_to_source(cr);
  cairo_paint(cr);

  FINISH
}

void dtgtk_cairo_paint_label_sel(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  #define CPF_USER_DATA_INCLUDE CPF_USER_DATA
  #define CPF_USER_DATA_EXCLUDE CPF_USER_DATA << 1
  PREAMBLE(0.9, 1, 0, 0)

  const double r = 0.4;
  const dt_colorlabels_enum color = (flags & 7);

  if(color < DT_COLORLABELS_LAST)
  {
    set_color(cr, darktable.bauhaus->colorlabels[color]);
  }
  else
  {
    cairo_set_source_rgba(cr, 0.75, 0.75, 0.75, 1.0);
  }

  /* make blue color label icon more visible and well balanced with other colors */
  if(flags & CPF_LABEL_BLUE)
  {
    cairo_set_line_width(cr, 1.2 * cairo_get_line_width(cr));
  }

  /* then improve hover effect for same blue icon */
  if(flags & CPF_PRELIGHT)
  {
    cairo_set_line_width(cr, 1.2 * cairo_get_line_width(cr));
  }

  if(flags & CPF_USER_DATA_INCLUDE)
  {
    cairo_arc(cr, 0.5, 0.5, r, 0.0, 2.0 * M_PI);
    cairo_fill(cr);
  }
  else if(flags & CPF_USER_DATA_EXCLUDE)
  {
    /* fill base color */
    cairo_arc(cr, 0.5, 0.5, r, 0.0, 2.0 * M_PI);
    cairo_stroke(cr);
    cairo_move_to(cr, 0.1, 0.1);
    cairo_line_to(cr, 0.9, 0.9);
    cairo_move_to(cr, 0.9, 0.1);
    cairo_line_to(cr, 0.1, 0.9);
    cairo_stroke(cr);
  }
  else
  {
    /* fill base color */
    cairo_arc(cr, 0.5, 0.5, r, 0.0, 2.0 * M_PI);
    cairo_stroke(cr);
  }

  FINISH
}

void dtgtk_cairo_paint_reject(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(0.95, 1, 0, 0)

  // the reject icon
  cairo_arc(cr, 0.5, 0.5, 0.5, 0.0, 2.0 * M_PI);
  cairo_move_to(cr, 0.7, 0.3);
  cairo_line_to(cr, 0.3, 0.7);
  cairo_move_to(cr, 0.3, 0.3);
  cairo_line_to(cr, 0.7, 0.7);
  cairo_stroke(cr);

  if(flags & CPF_DIRECTION_RIGHT)
  {
    // that means the image is rejected, so we draw the cross in red bold
    cairo_set_source_rgb(cr, 1.0, 0, 0);
  }

  FINISH
}

void dtgtk_cairo_paint_remove(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(0.95, 1, 0, 0)

  // the reject icon
  _rounded_rectangle(cr);
  cairo_fill(cr);

  cairo_set_source_rgba(cr, 0.2, 0.2, 0.2, 0.8);
  cairo_move_to(cr, 0.7, 0.3);
  cairo_line_to(cr, 0.3, 0.7);
  cairo_move_to(cr, 0.3, 0.3);
  cairo_line_to(cr, 0.7, 0.7);
  cairo_stroke(cr);

  FINISH
}

void dtgtk_cairo_paint_star(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0, 0)

  // we create the path
  dt_draw_star(cr, 1 / 2., 1. / 2., 1. / 2., 1. / 5.);

  // we fill the star if needed (mouseover or activated)
  if(data)
  {
    GdkRGBA *bgc = (GdkRGBA *)data; // the inner star color is defined in data
    double r, g, b, a;
    if(cairo_pattern_get_rgba(cairo_get_source(cr), &r, &g, &b, &a) == CAIRO_STATUS_SUCCESS)
    {
      cairo_set_source_rgba(cr, bgc->red, bgc->green, bgc->blue, bgc->alpha);
      cairo_fill_preserve(cr);
      cairo_set_source_rgba(cr, r, g, b, a);
    }
  }

  cairo_stroke(cr);

  FINISH
}

void dtgtk_cairo_paint_unratestar(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0, 0)

  // we create background star
  dt_draw_star(cr, 1 / 2., 1. / 2., 1. / 2., 1. / 5.);

  double r, g, b, a;
  if(cairo_pattern_get_rgba(cairo_get_source(cr), &r, &g, &b, &a) == CAIRO_STATUS_SUCCESS)
  {
    cairo_set_source_rgba(cr, r, g, b, a*.3);
    cairo_stroke(cr);
    cairo_set_line_width(cr, 1.6 * cairo_get_line_width(cr));
    if((flags & CPF_PRELIGHT) || (flags & CPF_ACTIVE))
      cairo_set_source_rgba(cr, r, g, b, a);
    else
      cairo_set_source_rgba(cr, r, g, b, a*.7);

    cairo_move_to(cr, .1, .5);
    cairo_line_to(cr, .9, .5);
    cairo_stroke(cr);
  }

  FINISH
}

void dtgtk_cairo_paint_local_copy(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0, 0)

  if(flags & CPF_ACTIVE)
  {
    cairo_move_to(cr, 0.45, 0);
    cairo_line_to(cr, 0.85, 0);
    cairo_line_to(cr, 0.85, 0.40);
    cairo_close_path(cr);
    cairo_fill_preserve(cr);
    cairo_stroke(cr);
    cairo_rectangle(cr, 0.15, 0, 0.7, 1.0);
    cairo_stroke(cr);
  }
  else
  {
    /* fill base color */
    cairo_move_to(cr, 0, 0);
    cairo_line_to(cr, 1.0, 1.0);
    cairo_line_to(cr, 1.0, 0);
    cairo_close_path(cr);
    cairo_fill(cr);
  }

  FINISH
}

void dtgtk_cairo_paint_altered(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(0.5 * 0.95, 1, 0.5, 0.5)

  const float r = 1.;
  cairo_arc(cr, 0, 0, r, 0, 2.0f * M_PI);
  const float dx = r * cosf(M_PI / 8.0f), dy = r * sinf(M_PI / 8.0f);
  cairo_move_to(cr,  - dx,  - dy);
  cairo_curve_to(cr, 0, -2.0 * dy, 0, 2.0 * dy, dx, dy);
  cairo_move_to(cr, -.2 * dx,  .8 * dy);
  cairo_line_to(cr, -.8 * dx,  .8 * dy);
  cairo_move_to(cr,  .2 * dx, -.8 * dy);
  cairo_line_to(cr,  .8 * dx, -.8 * dy);
  cairo_move_to(cr,  .5 * dx, -.8 * dy - .3 * dx);
  cairo_line_to(cr,  .5 * dx, -.8 * dy + .3 * dx);
  cairo_stroke(cr);

  FINISH
}

void dtgtk_cairo_paint_audio(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0, 0)

  cairo_rectangle(cr, 0.05, 0.4, 0.2, 0.2);
  cairo_move_to(cr, 0.25, 0.6);
  cairo_line_to(cr, 0.45, 0.77);
  cairo_line_to(cr, 0.45, 0.23);
  cairo_line_to(cr, 0.25, 0.4);

  cairo_new_sub_path(cr);
  cairo_arc(cr, 0.2, 0.5, 0.45, -(35.0 / 180.0) * M_PI, (35.0 / 180.0) * M_PI);
  cairo_new_sub_path(cr);
  cairo_arc(cr, 0.2, 0.5, 0.6, -(35.0 / 180.0) * M_PI, (35.0 / 180.0) * M_PI);
  cairo_new_sub_path(cr);
  cairo_arc(cr, 0.2, 0.5, 0.75, -(35.0 / 180.0) * M_PI, (35.0 / 180.0) * M_PI);

  cairo_stroke(cr);

  FINISH
}

void dtgtk_cairo_paint_label_flower(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1.1, 1, 0, 0)

  const float r = 0.18;

  if(flags & CPF_LABEL_RED)
  {
    cairo_arc(cr, r, r, r, 0, 2.0f * M_PI);
    set_color(cr, darktable.bauhaus->colorlabels[DT_COLORLABELS_RED]);
    cairo_fill(cr);
  }

  if(flags & CPF_LABEL_YELLOW)
  {
    cairo_arc(cr, 1.0 - r, r, r, 0, 2.0f * M_PI);
    set_color(cr, darktable.bauhaus->colorlabels[DT_COLORLABELS_YELLOW]);
    cairo_fill(cr);
  }

  if(flags & CPF_LABEL_GREEN)
  {
    cairo_arc(cr, 0.5, 0.5, r, 0, 2.0f * M_PI);
    set_color(cr, darktable.bauhaus->colorlabels[DT_COLORLABELS_GREEN]);
    cairo_fill(cr);
  }

  if(flags & CPF_LABEL_BLUE)
  {
    cairo_arc(cr, r, 1.0 - r, r, 0, 2.0f * M_PI);
    set_color(cr, darktable.bauhaus->colorlabels[DT_COLORLABELS_BLUE]);
    cairo_fill(cr);
  }

  if(flags & CPF_LABEL_PURPLE)
  {
    cairo_arc(cr, 1.0 - r, 1.0 - r, r, 0, 2.0f * M_PI);
    set_color(cr, darktable.bauhaus->colorlabels[DT_COLORLABELS_PURPLE]);
    cairo_fill(cr);
  }

  FINISH
}

void dtgtk_cairo_paint_colorpicker(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0, 0.05)

  /* draw pipette */

  // drop
  cairo_move_to(cr, 0., 1. - 0.0);
  cairo_line_to(cr, 0.08, 1. - 0.15);
  cairo_line_to(cr, 0.16, 1. - 0.0);
  cairo_arc(cr, 0.08, 1. - 0.15 + 0.1926, 0.090666667, -0.49, 3.63);
  cairo_fill(cr);

  // cross line
  cairo_set_line_width(cr, 0.15);
  cairo_move_to(cr, 0.48, 1. - 0.831);
  cairo_line_to(cr, 0.739, 1. - 0.482);

  // shaft
  cairo_move_to(cr, 0.124, 1. - 0.297);
  cairo_line_to(cr, 0.823, 1. - 0.814);
  cairo_stroke(cr);

  // end
  cairo_set_line_width(cr, 0.35);
  cairo_move_to(cr, 0.823, 1. - 0.814);
  cairo_line_to(cr, 0.648, 1. - 0.685);
  cairo_stroke(cr);

  FINISH
}

void dtgtk_cairo_paint_colorpicker_set_values(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0, 0.05)

  /* draw pipette */

  // drop
  cairo_move_to(cr, 0., 1. - 0.0);
  cairo_line_to(cr, 0.08, 1. - 0.15);
  cairo_line_to(cr, 0.16, 1. - 0.0);
  cairo_arc(cr, 0.08, 1. - 0.15 + 0.1926, 0.090666667, -0.49, 3.63);
  cairo_fill(cr);

  // plus sign
  cairo_move_to(cr, 0.18, 0.00);
  cairo_line_to(cr, 0.18, 0.36);
  cairo_stroke(cr);
  cairo_move_to(cr, 0.00, 0.18);
  cairo_line_to(cr, 0.36, 0.18);
  cairo_stroke(cr);

  // cross line
  cairo_set_line_width(cr, 0.15);
  cairo_move_to(cr, 0.48, 1. - 0.831);
  cairo_line_to(cr, 0.739, 1. - 0.482);

  // shaft
  cairo_move_to(cr, 0.124, 1. - 0.297);
  cairo_line_to(cr, 0.823, 1. - 0.814);
  cairo_stroke(cr);

  // end
  cairo_set_line_width(cr, 0.35);
  cairo_move_to(cr, 0.823, 1. - 0.814);
  cairo_line_to(cr, 0.648, 1. - 0.685);
  cairo_stroke(cr);

  FINISH
}

void dtgtk_cairo_paint_showmask(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0, 0)

  /* draw circle */
  cairo_arc(cr, 0.5, 0.5, 0.35, -M_PI, M_PI);

  /* draw rectangle */
  cairo_rectangle(cr, 0.0, 0.0, 1.0, 1.0);
  cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
  cairo_fill(cr);
  cairo_stroke(cr);

  FINISH
}

void dtgtk_cairo_paint_preferences(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(0.5 * 0.95, 1, 0.5, 0.5)

  cairo_rotate(cr, M_PI / 12.);

  const float big_r = 1.f;
  const float tin_r = 0.8f;

  for(int i = 0; i < 12; i++)
  {
    const float radius = (i % 2 == 0) ? big_r : tin_r;
    cairo_arc(cr, 0.0, 0.0, radius, i * M_PI / 6., (i + 1) * M_PI / 6.);
  }
  cairo_close_path(cr);
  cairo_stroke(cr);

  cairo_arc(cr, 0.0, 0.0, 0.3, 0, 2. * M_PI);
  cairo_stroke(cr);

  FINISH
}

void dtgtk_cairo_paint_overlays(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(0.5 * 1.03, 1, 0.5, 0.5)

  dt_draw_star(cr, 0.0, 0.0, 1., 1.0/2.5);
  cairo_stroke(cr);

  FINISH
}

void dtgtk_cairo_paint_help(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(0.5 * 0.97, 1, 0.5, 0.5)

  cairo_arc(cr, 0.0, -0.5, 0.4, - M_PI, 0.25 * M_PI);
  cairo_arc_negative(cr, 0.7, 0.4, 0.7, -0.75 * M_PI, - M_PI);
  cairo_stroke(cr);
  cairo_arc(cr, 0.0, 0.85, 0.05, 0.0, 2.0 * M_PI);
  cairo_stroke(cr);

  FINISH
}

void dtgtk_cairo_paint_grouping(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0, 0)

  cairo_move_to(cr, 0.30, 0.15);
  cairo_line_to(cr, 0.95, 0.15);
  cairo_line_to(cr, 0.95, 0.65);
  cairo_move_to(cr, 0.20, 0.25);
  cairo_line_to(cr, 0.85, 0.25);
  cairo_line_to(cr, 0.85, 0.75);
  cairo_stroke(cr);
  if(flags & CPF_ACTIVE)
  {
    cairo_rectangle(cr, 0.05, 0.35, 0.7, 0.5);
    cairo_fill(cr);
  }
  else
  {
    cairo_rectangle(cr, 0.05, 0.35, 0.7, 0.5);
    cairo_stroke(cr);
  }

  FINISH
}

void dtgtk_cairo_paint_alignment(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0, 0)

  switch(flags >> (int)log2(CPF_SPECIAL_FLAG))
  {
    case 1: // Top left
      cairo_move_to(cr, 0.9, 0.1);
      cairo_line_to(cr, 0.1, 0.1);
      cairo_line_to(cr, 0.1, 0.9);
      break;

    case 2: // Top center
      cairo_move_to(cr, 0.1, 0.1);
      cairo_line_to(cr, 0.9, 0.1);
      break;

    case 4: // Top right
      cairo_move_to(cr, 0.1, 0.1);
      cairo_line_to(cr, 0.9, 0.1);
      cairo_line_to(cr, 0.9, 0.9);
      break;

    case 8: // left
      cairo_move_to(cr, 0.1, 0.1);
      cairo_line_to(cr, 0.1, 0.9);
      break;

    case 16: // center
      cairo_move_to(cr, 0.1, 0.5);
      cairo_line_to(cr, 0.9, 0.5);
      cairo_move_to(cr, 0.5, 0.1);
      cairo_line_to(cr, 0.5, 0.9);
      break;

    case 32: // right
      cairo_move_to(cr, 0.9, 0.1);
      cairo_line_to(cr, 0.9, 0.9);
      break;

    case 64: // bottom left
      cairo_move_to(cr, 0.9, 0.9);
      cairo_line_to(cr, 0.1, 0.9);
      cairo_line_to(cr, 0.1, 0.1);
      break;

    case 128: // bottom center
      cairo_move_to(cr, 0.1, 0.9);
      cairo_line_to(cr, 0.9, 0.9);
      break;

    case 256: // bottom right
      cairo_move_to(cr, 0.1, 0.9);
      cairo_line_to(cr, 0.9, 0.9);
      cairo_line_to(cr, 0.9, 0.1);
      break;
  }
  cairo_stroke(cr);

  FINISH
}

void dtgtk_cairo_paint_text_label(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0, 0)

  // draw an A
  cairo_move_to(cr, 0.1, 1.);
  cairo_line_to(cr, 0.5, 0.);
  cairo_line_to(cr, 0.9, 1.);

  cairo_move_to(cr, 0.25, 0.6);
  cairo_line_to(cr, 0.75, 0.6);
  cairo_stroke(cr);

  FINISH
}


void dtgtk_cairo_paint_or(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0, 0)

  cairo_move_to(cr, 0.1, 0.3);
  cairo_curve_to(cr, 0.1, 1.1, 0.9, 1.1, 0.9, 0.3);
  cairo_stroke(cr);

  FINISH
}

void dtgtk_cairo_paint_and(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0, 0)

  cairo_move_to(cr, 0.1, 0.9);
  cairo_curve_to(cr, 0.1, 0.1, 0.9, 0.1, 0.9, 0.9);
  cairo_stroke(cr);

  FINISH
}

void dtgtk_cairo_paint_andnot(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0, 0)

  cairo_move_to(cr, 0.1, 0.1);
  cairo_line_to(cr, 0.9, 0.9);
  cairo_stroke(cr);

  FINISH
}

void dtgtk_cairo_paint_dropdown(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0, 0)

  cairo_move_to(cr, 0.1, 0.3);
  cairo_line_to(cr, 0.5, 0.7);
  cairo_line_to(cr, 0.9, 0.3);
  cairo_stroke(cr);

  FINISH
}

void dtgtk_cairo_paint_bracket(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0, 0)

  cairo_rectangle(cr, 0.05, 0.05, 0.45, 0.45);
  cairo_stroke(cr);
  cairo_set_line_width(cr, 0.025);
  cairo_rectangle(cr, 0.55, 0.05, 0.45, 0.45);
  cairo_stroke(cr);
  cairo_set_line_width(cr, 0.05);
  cairo_rectangle(cr, 0.05, 0.55, 0.45, 0.45);
  cairo_stroke(cr);
  cairo_set_line_width(cr, 0.1);
  cairo_rectangle(cr, 0.55, 0.55, 0.45, 0.45);
  cairo_stroke(cr);

  FINISH
}

void dtgtk_cairo_paint_lock(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0, 0)

  // Adding the lock body
  cairo_rectangle(cr, 0.25, 0.5, .5, .45);
  cairo_fill(cr);

  // Adding the lock shank
  cairo_translate(cr, .5, .5);
  cairo_scale(cr, .2, .4);
  cairo_arc(cr, 0, 0, 1, M_PI, 0);
  cairo_stroke(cr);

  FINISH
}

void dtgtk_cairo_paint_check_mark(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0, 0)

  cairo_move_to(cr, 0.20, 0.45);
  cairo_line_to(cr, 0.45, 0.90);
  cairo_line_to(cr, 0.90, 0.20);
  cairo_stroke(cr);

  FINISH
}

void dtgtk_cairo_paint_overexposed(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0, 0)

  /* the triangle */
  cairo_move_to(cr, 1.0, 0);
  cairo_line_to(cr, 0, 1.0);
  cairo_line_to(cr, 1.0, 1.0);
  cairo_fill(cr);

  /* outer rect */
  _rounded_rectangle(cr);
  cairo_stroke(cr);

  FINISH
}


void dtgtk_cairo_paint_bulb(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(0.95, 1, 0, -0.05)

  const float line_width = 0.1;

  // glass
  cairo_arc_negative(cr, 0.5, 0.38, 0.4, 1., M_PI - 1.);
  cairo_close_path(cr);

  if(flags & CPF_ACTIVE)
  {
    cairo_stroke_preserve(cr);
    cairo_fill(cr);
  }
  else
  {
    cairo_stroke(cr);
    cairo_arc(cr, 0.5, 0.38, 0.2, -M_PI / 3., -M_PI / 6.);
    cairo_stroke(cr);
  }

  // screw
  cairo_move_to(cr, 0.33, 0.38 + 0.36 + 1 * line_width);
  cairo_line_to(cr, 0.67, 0.38 + 0.36 + 1 * line_width);
  cairo_stroke(cr);

  // nib
  cairo_arc(cr, 0.5, 0.38 + 0.36 + 2. * line_width, 2.0 * line_width, 0, M_PI);
  cairo_fill(cr);

  FINISH
}


void dtgtk_cairo_paint_rawoverexposed(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0, 0)

  cairo_save(cr);

  const float alpha = (flags & CPF_ACTIVE ? 1.0 : 0.4);

  // draw 4 CFA-like colored squares
  cairo_set_source_rgba(cr, 0.3, 0.3, 0.3, alpha); // red
  cairo_rectangle(cr, 0, 0, 0.5, 0.5);
  cairo_fill(cr);

  cairo_set_source_rgba(cr, 0.6, 0.6, 0.6, alpha); // green
  cairo_rectangle(cr, 0.5, 0, 0.5, 0.5);
  cairo_fill(cr);

  cairo_set_source_rgba(cr, 0.6, 0.6, 0.6, alpha); // green
  cairo_rectangle(cr, 0, 0.5, 0.5, 0.5);
  cairo_fill(cr);

  cairo_set_source_rgba(cr, 0.1, 0.1, 0.1, alpha); // blue
  cairo_rectangle(cr, 0.5, 0.5, 0.5, 0.5);
  cairo_fill(cr);

  cairo_restore(cr);

  /* outer rect */
  _rounded_rectangle(cr);
  cairo_stroke(cr);

  FINISH
}

void dtgtk_cairo_paint_gamut_check(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1.15, 1, 0, -0.05)

  // the triangle
  cairo_move_to(cr, 0.0, 1 - 0.067);
  cairo_line_to(cr, 0.5, 1 - 0.933);
  cairo_line_to(cr, 1.0, 1 - 0.067);
  cairo_close_path(cr);

  // exclamation mark
  // the dot
  cairo_new_sub_path(cr);
  cairo_move_to(cr, 0.42, 1 - 0.11);
  cairo_line_to(cr, 0.42, 1 - 0.25);
  cairo_line_to(cr, 0.58, 1 - 0.25);
  cairo_line_to(cr, 0.58, 1 - 0.11);
  cairo_close_path(cr);

  // the line
  cairo_new_sub_path(cr);
  cairo_move_to(cr, 0.447, 1 - 0.29);
  cairo_line_to(cr, 0.415, 1 - 0.552);
  cairo_line_to(cr, 0.415, 1 - 0.683);
  cairo_line_to(cr, 0.585, 1 - 0.683);
  cairo_line_to(cr, 0.585, 1 - 0.552);
  cairo_line_to(cr, 0.552, 1 - 0.29);
  cairo_close_path(cr);

  cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
  cairo_fill(cr);

  FINISH
}

void dtgtk_cairo_paint_softproof(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1.1, 1, 0, 0)

  // the horse shoe
  cairo_move_to(cr, 0.30, 1 - 0.0);
  cairo_curve_to(cr, 0.14, 1 - 0.09, 0.03, 1 - 0.88, 0.18, 1 - 1);
  cairo_curve_to(cr, 0.32, 1 - 1.08, 0.69, 1 - 0.63, 0.97, 1 - 0.32);
  cairo_close_path(cr);

  // triangle
  cairo_new_sub_path(cr);
  cairo_move_to(cr, 0.28, 1 - 0.07);
  cairo_line_to(cr, 0.37, 1 - 0.75);
  cairo_line_to(cr, 0.82, 1 - 0.42);
  cairo_close_path(cr);

  cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
  cairo_fill(cr);

  FINISH
}

void dtgtk_cairo_paint_display(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0, 0)

  cairo_rectangle(cr, 0, 0, 1, 3./4.);
  cairo_move_to(cr, 0.5, 3./4);
  cairo_line_to(cr, 0.5, 1);
  cairo_move_to(cr, 0.3, 1);
  cairo_line_to(cr, 0.7, 1);
  cairo_stroke(cr);

  FINISH
}

void dtgtk_cairo_paint_display2(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(0.55, 1, 0.5, 0.5)

  cairo_move_to(cr, -0.55, 0.9);
  cairo_rel_line_to(cr, 0.7, 0);
  cairo_stroke(cr);

  cairo_rectangle(cr, -0.9, -0.5, 1.4, 1.0);
  cairo_move_to(cr, -0.5, -0.7);
  cairo_rel_line_to(cr, 0, -0.2);
  cairo_rel_line_to(cr, 1.4, 0);
  cairo_rel_line_to(cr, 0, 1.0);
  cairo_rel_line_to(cr, -0.2, 0);
  cairo_stroke(cr);

  cairo_move_to(cr, -0.2, 0.6);
  cairo_rel_line_to(cr, 0, 0.2);
  cairo_stroke(cr);

  FINISH
}

void dtgtk_cairo_paint_rect_landscape(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0, 0)

  cairo_move_to(cr, 0.0, 0.3);
  cairo_line_to(cr, 1.0, 0.3);
  cairo_line_to(cr, 1.0, 0.7);
  cairo_line_to(cr, 0.0, 0.7);
  cairo_line_to(cr, 0.0, 0.3);
  cairo_stroke(cr);

  FINISH
}

void dtgtk_cairo_paint_rect_portrait(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0, 0)

  cairo_move_to(cr, 0.3, 0.0);
  cairo_line_to(cr, 0.7, 0.0);
  cairo_line_to(cr, 0.7, 1.0);
  cairo_line_to(cr, 0.3, 1.0);
  cairo_line_to(cr, 0.3, 0.0);
  cairo_stroke(cr);

  FINISH
}

void dtgtk_cairo_paint_polygon(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0, 0)

  cairo_move_to(cr, 0.0, 0.3);
  cairo_line_to(cr, 0.7, 0.0);
  cairo_line_to(cr, 0.5, 0.5);
  cairo_line_to(cr, 1.0, 0.6);
  cairo_line_to(cr, 0.6, 1.0);
  cairo_line_to(cr, 0.3, 0.8);
  cairo_line_to(cr, 0.0, 0.3);
  cairo_stroke(cr);

  FINISH
}

void dtgtk_cairo_paint_zoom(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0, 0)

  /* draw magnifying glass */

  // handle
  cairo_move_to(cr, 0.9, 1.0 - 0.1);
  cairo_line_to(cr, 0.65, 1.0 - 0.35);
  cairo_stroke(cr);

  // lens
  cairo_arc(cr, 0.35, 1.0 - 0.65, 0.3, -M_PI, M_PI);
  cairo_stroke(cr);

  FINISH
}

void dtgtk_cairo_paint_multiinstance(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0, 0)

  cairo_rectangle(cr, 0.35, 0.35, 0.6, 0.6);
  cairo_stroke(cr);
  cairo_rectangle(cr, 0.05, 0.05, 0.9, 0.9);
  cairo_rectangle(cr, 0.85, 0.25, -0.65, 0.65);
  cairo_clip(cr);
  cairo_rectangle(cr, 0.05, 0.05, 0.6, 0.6);
  cairo_stroke_preserve(cr);
  cairo_fill(cr);

  FINISH
}

void dtgtk_cairo_paint_modulegroup_active(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1.1, 1, 0, 0)

  cairo_arc(cr, 0.5, 0.5, 0.40, (-50 * 3.145 / 180), (230 * 3.145 / 180));
  cairo_move_to(cr, 0.5, 0.05);
  cairo_line_to(cr, 0.5, 0.40);
  cairo_stroke(cr);

  FINISH
}

void dtgtk_cairo_paint_modulegroup_favorites(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1.2, 1, 0, 0)

  const float r1 = 0.2;
  const float r2 = 0.4;
  const float d = 2.0 * M_PI * 0.1f;
  const float dx[10] = { sinf(0.0),   sinf(d),     sinf(2 * d), sinf(3 * d), sinf(4 * d),
                         sinf(5 * d), sinf(6 * d), sinf(7 * d), sinf(8 * d), sinf(9 * d) };
  const float dy[10] = { cosf(0.0),   cosf(d),     cosf(2 * d), cosf(3 * d), cosf(4 * d),
                         cosf(5 * d), cosf(6 * d), cosf(7 * d), cosf(8 * d), cosf(9 * d) };
  cairo_move_to(cr, 0.5 + r1 * dx[0], 0.5 - r1 * dy[0]);
  for(int k = 1; k < 10; k++)
    if(k & 1)
      cairo_line_to(cr, 0.5 + r2 * dx[k], 0.5 - r2 * dy[k]);
    else
      cairo_line_to(cr, 0.5 + r1 * dx[k], 0.5 - r1 * dy[k]);
  cairo_close_path(cr);
  cairo_stroke(cr);

  FINISH
}

void dtgtk_cairo_paint_modulegroup_basics(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1.1, 1, 0, 0)

  // the colors
  double r, g, b, a;
  cairo_pattern_get_rgba(cairo_get_source(cr), &r, &g, &b, &a);
  double na = a * 0.7;
  // the stroke width
  double sw = cairo_get_line_width(cr);

  // background sliders
  cairo_set_source_rgba(cr, r, g, b, na);
  cairo_set_line_width(cr, sw * 1.2);
  cairo_move_to(cr, 0.1, 0.2);
  cairo_line_to(cr, 0.9, 0.2);
  cairo_stroke(cr);
  cairo_move_to(cr, 0.1, 0.55);
  cairo_line_to(cr, 0.9, 0.55);
  cairo_stroke(cr);
  cairo_move_to(cr, 0.1, 0.9);
  cairo_line_to(cr, 0.9, 0.9);
  cairo_stroke(cr);

  // foreground sliders
  cairo_set_source_rgba(cr, r, g, b, a);
  cairo_set_line_width(cr, sw);
  cairo_move_to(cr, 0.1, 0.2);
  cairo_line_to(cr, 0.6, 0.2);
  cairo_stroke(cr);
  cairo_move_to(cr, 0.9, 0.55);
  cairo_line_to(cr, 0.3, 0.55);
  cairo_stroke(cr);
  cairo_move_to(cr, 0.1, 0.9);
  cairo_line_to(cr, 0.4, 0.9);
  cairo_stroke(cr);

  // cursors
  cairo_move_to(cr, 0.70, 0.0);
  cairo_line_to(cr, 0.81, 0.30);
  cairo_line_to(cr, 0.59, 0.30);
  cairo_close_path(cr);
  cairo_fill(cr);
  cairo_move_to(cr, 0.2, 0.35);
  cairo_line_to(cr, 0.31, 0.65);
  cairo_line_to(cr, 0.09, 0.65);
  cairo_close_path(cr);
  cairo_fill(cr);
  cairo_move_to(cr, 0.55, 0.70);
  cairo_line_to(cr, 0.66, 1.0);
  cairo_line_to(cr, 0.44, 1.0);
  cairo_close_path(cr);
  cairo_fill(cr);

  FINISH
}

void dtgtk_cairo_paint_modulegroup_basic(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1.1, 1, 0, 0)

  /* draw circle */
  cairo_arc(cr, 0.5, 0.5, 0.40, -M_PI, M_PI);
  cairo_stroke(cr);

  FINISH
}

void dtgtk_cairo_paint_modulegroup_tone(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1.1, 1, 0, 0)

  /* draw circle */
  cairo_arc(cr, 0.5, 0.5, 0.40, -M_PI, M_PI);
  cairo_stroke(cr);

  /* fill circle */
  cairo_pattern_t *pat = NULL;
  pat = cairo_pattern_create_linear(0, 0, 1, 0);
  cairo_pattern_add_color_stop_rgba(pat, 0, 1, 1, 1, 1);
  cairo_pattern_add_color_stop_rgba(pat, 1, 1, 1, 1, 0);
  cairo_set_source(cr, pat);
  cairo_arc(cr, 0.5, 0.5, 0.40, -M_PI, M_PI);
  cairo_fill(cr);
  cairo_pattern_destroy(pat);

  FINISH
}

void dtgtk_cairo_paint_modulegroup_color(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1.1, 1, 0, 0)

  /* draw circle */
  cairo_arc(cr, 0.5, 0.5, 0.40, -M_PI, M_PI);
  cairo_stroke(cr);

  /* fill circle */
  float a = 0.6;
  cairo_pattern_t *pat = NULL;
  pat = cairo_pattern_create_linear(0, 0, 1, 0);
  cairo_pattern_add_color_stop_rgba(pat, 0.0, 1, 0, 0, a);
  cairo_pattern_add_color_stop_rgba(pat, 0.1, 1, 0, 0, a);
  cairo_pattern_add_color_stop_rgba(pat, 0.5, 0, 1, 0, a);
  cairo_pattern_add_color_stop_rgba(pat, 0.9, 0, 0, 1, a);
  cairo_pattern_add_color_stop_rgba(pat, 1.0, 0, 0, 1, a);
  cairo_set_source(cr, pat);
  cairo_arc(cr, 0.5, 0.5, 0.40, -M_PI, M_PI);
  cairo_fill(cr);
  cairo_pattern_destroy(pat);

  FINISH
}

void dtgtk_cairo_paint_modulegroup_correct(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1.1, 1, 0, 0)

  /* draw circle */
  cairo_arc(cr, 0.42, 0.5, 0.40, 0, M_PI);
  cairo_stroke(cr);
  cairo_arc(cr, 0.58, 0.5, 0.40, M_PI, 0);
  cairo_stroke(cr);

  FINISH
}

void dtgtk_cairo_paint_modulegroup_effect(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1.1, 1, 0, 0)

  /* draw circle */
  cairo_arc(cr, 0.5, 0.5, 0.40, -M_PI, M_PI);
  cairo_stroke(cr);

  /* sparkles */

  cairo_move_to(cr, 0.378, 0.502);
  cairo_line_to(cr, 0.522, 0.549);
  cairo_line_to(cr, 0.564, 0.693);
  cairo_line_to(cr, 0.653, 0.569);
  cairo_line_to(cr, 0.802, 0.573);
  cairo_line_to(cr, 0.712, 0.449);
  cairo_line_to(cr, 0.762, 0.308);
  cairo_line_to(cr, 0.618, 0.356);
  cairo_line_to(cr, 0.500, 0.264);
  cairo_line_to(cr, 0.500, 0.417);
  cairo_close_path(cr);

  cairo_move_to(cr, 0.269, 0.717);
  cairo_line_to(cr, 0.322, 0.735);
  cairo_line_to(cr, 0.337, 0.787);
  cairo_line_to(cr, 0.370, 0.742);
  cairo_line_to(cr, 0.424, 0.743);
  cairo_line_to(cr, 0.391, 0.698);
  cairo_line_to(cr, 0.409, 0.646);
  cairo_line_to(cr, 0.357, 0.664);
  cairo_line_to(cr, 0.314, 0.630);
  cairo_line_to(cr, 0.314, 0.686);

  cairo_move_to(cr, 0.217, 0.366);
  cairo_line_to(cr, 0.271, 0.384);
  cairo_line_to(cr, 0.286, 0.437);
  cairo_line_to(cr, 0.319, 0.391);
  cairo_line_to(cr, 0.374, 0.393);
  cairo_line_to(cr, 0.341, 0.347);
  cairo_line_to(cr, 0.360, 0.295);
  cairo_line_to(cr, 0.306, 0.312);
  cairo_line_to(cr, 0.263, 0.279);
  cairo_line_to(cr, 0.263, 0.335);

  cairo_close_path(cr);

  cairo_stroke(cr);

  FINISH
}

void dtgtk_cairo_paint_modulegroup_grading(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1.2, 1, 0, 0)

  /* draw circle */
  cairo_arc(cr, 0.5, 0.5, 0.40, -M_PI, M_PI);
  cairo_stroke_preserve(cr);
  cairo_clip(cr);

  /* fill circle */
  float a = 0.6;
  cairo_pattern_t *pat = NULL;
  pat = cairo_pattern_create_linear(0, 0, 1, 0);
  cairo_pattern_add_color_stop_rgba(pat, 0.00, 1, 0, 0, a);
  cairo_pattern_add_color_stop_rgba(pat, 0.10, 1, 0, 0, a);
  cairo_pattern_add_color_stop_rgba(pat, 0.40, 1, 1, 0, a);
  cairo_pattern_add_color_stop_rgba(pat, 0.50, 0, 1, 0, a);
  cairo_pattern_add_color_stop_rgba(pat, 0.60, 0, 1, 1, a);
  cairo_pattern_add_color_stop_rgba(pat, 0.90, 0, 0, 1, a);
  cairo_pattern_add_color_stop_rgba(pat, 1.00, 0, 0, 1, a);
  cairo_set_source(cr, pat);
  cairo_arc(cr, 0.5, 0.5, 0.40, -M_PI, M_PI);
  cairo_fill(cr);
  cairo_pattern_destroy(pat);

  pat = cairo_pattern_create_linear(0, 0, 0, 1);
  cairo_pattern_add_color_stop_rgba(pat, 0.0, 1, 1, 1, a);
  cairo_pattern_add_color_stop_rgba(pat, 0.5, 0.5, 0.5, 0.5, a);
  cairo_pattern_add_color_stop_rgba(pat, 1.0, 0, 0, 0, a);
  cairo_set_source(cr, pat);
  cairo_arc(cr, 0.5, 0.5, 0.40, -M_PI, M_PI);

  cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
  cairo_fill_preserve(cr);
  cairo_set_operator(cr, CAIRO_OPERATOR_MULTIPLY);
  cairo_fill(cr);

  cairo_pattern_destroy(pat);

  FINISH
}

void dtgtk_cairo_paint_modulegroup_technical(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1.2, 1, 0, 0)

  /* draw circle */
  cairo_arc(cr, 0.5, 0.5, 0.4, -M_PI, M_PI);
  cairo_stroke(cr);

  /* draw thin lens */
  cairo_arc(cr, 0.05, 0.5, 0.55, -0.6107259643892086, 0.6107259643892086);
  cairo_arc(cr, 0.95, 0.5, 0.55, 2.5308666892005847, 3.7523186179790016);
  cairo_fill(cr);

  /* draw the rays */
  cairo_move_to(cr, 0.3, 0.3);
  cairo_line_to(cr, 0.5, 0.3);
  cairo_line_to(cr, 0.8, 0.5);
  cairo_stroke(cr);

  cairo_move_to(cr, 0.25, 0.5);
  cairo_line_to(cr, 0.80, 0.5);
  cairo_stroke(cr);

  cairo_move_to(cr, 0.3, 0.7);
  cairo_line_to(cr, 0.5, 0.7);
  cairo_line_to(cr, 0.80, 0.5);
  cairo_stroke(cr);

  FINISH
}

void dtgtk_cairo_paint_map_pin(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0, 0)

  cairo_move_to(cr, 0.2, 0.0);
  cairo_line_to(cr, 0.0, 1.0);
  cairo_line_to(cr, 0.7, 0.0);
  cairo_close_path(cr);
  cairo_fill(cr);

  FINISH
}

void dtgtk_cairo_paint_tool_clone(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0, 0)

  cairo_arc(cr, 0.65, 0.35, 0.35, 0, 2 * M_PI);
  cairo_stroke(cr);

  cairo_arc(cr, 0.35, 0.65, 0.35, 0, 2 * M_PI);
  cairo_stroke(cr);

  FINISH
}

void dtgtk_cairo_paint_tool_heal(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0, 0)

  cairo_set_line_width(cr, 0.01);
  cairo_move_to(cr, 0.35, 0.1);
  cairo_rel_line_to(cr, 0.3, 0.0);
  cairo_rel_line_to(cr, 0.0, 0.25);
  cairo_rel_line_to(cr, 0.25, 0.0);
  cairo_rel_line_to(cr, 0.0, 0.3);
  cairo_rel_line_to(cr, -0.25, 0.0);
  cairo_rel_line_to(cr, 0.0, 0.25);
  cairo_rel_line_to(cr, -0.3, 0.0);
  cairo_rel_line_to(cr, 0.0, -0.25);
  cairo_rel_line_to(cr, -0.25, 0.0);
  cairo_rel_line_to(cr, 0.0, -0.3);
  cairo_rel_line_to(cr, 0.25, 0.0);
  cairo_close_path(cr);

  cairo_rectangle(cr, 0., 0., 1., 1.);

  cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
  cairo_fill(cr);
  cairo_stroke(cr);

  FINISH
}

void dtgtk_cairo_paint_tool_fill(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1.08, 1, 0, 0)

  cairo_move_to(cr, 0.1, 0.1);
  cairo_line_to(cr, 0.2, 0.1);
  cairo_line_to(cr, 0.2, 0.9);
  cairo_line_to(cr, 0.8, 0.9);
  cairo_line_to(cr, 0.8, 0.1);
  cairo_line_to(cr, 0.9, 0.1);
  cairo_stroke(cr);
  cairo_rectangle(cr, 0.2, 0.4, .6, .5);
  cairo_fill(cr);
  cairo_stroke(cr);

  FINISH
}

void dtgtk_cairo_paint_tool_blur(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1.1, 1, 0, 0)

 cairo_move_to(cr, 0.5, 0.1);
 cairo_arc(cr, 0.5, 0.65, 0.28, -0.2 * M_PI, 1.2 * M_PI);
 cairo_close_path(cr);
 cairo_stroke(cr);
 cairo_set_line_width(cr, 0.1);
 cairo_arc(cr, 0.5, 0.65, 0.13, 0.65 * M_PI, 1.2 * M_PI);
 cairo_stroke(cr);

  FINISH
}

void dtgtk_cairo_paint_paste_forms(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0, 0)

  cairo_move_to(cr, 0.1, 0.6);
  cairo_line_to(cr, 0.9, 0.6);
  cairo_line_to(cr, 0.5, 1.0);
  cairo_close_path(cr);
  cairo_fill(cr);
  cairo_stroke(cr);

  cairo_move_to(cr, 0.4, 0.0);
  cairo_line_to(cr, 0.6, 0.0);
  cairo_line_to(cr, 0.6, 0.6);
  cairo_line_to(cr, 0.4, 0.6);
  cairo_fill(cr);
  cairo_stroke(cr);

  FINISH
}

void dtgtk_cairo_paint_cut_forms(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0, -0.07)

  cairo_set_line_width(cr, 0.1);
  if(flags & CPF_ACTIVE)
  {
    cairo_move_to(cr, 0.11, 0.25);
    cairo_line_to(cr, 0.89, 0.75);
    cairo_move_to(cr, 0.25, 0.11);
    cairo_line_to(cr, 0.75, 0.89);
    cairo_stroke(cr);

    cairo_arc(cr, 0.89, 0.53, 0.17, 0, 2 * M_PI);
    cairo_stroke(cr);

    cairo_arc(cr, 0.53, 0.89, 0.17, 0, 2 * M_PI);
    cairo_stroke(cr);
  }
  else
  {
    cairo_move_to(cr, 0.01, 0.35);
    cairo_line_to(cr, 0.99, 0.65);
    cairo_move_to(cr, 0.35, 0.01);
    cairo_line_to(cr, 0.65, 0.99);
    cairo_stroke(cr);

    cairo_arc(cr, 0.89, 0.53, 0.17, 0, 2 * M_PI);
    cairo_stroke(cr);

    cairo_arc(cr, 0.53, 0.89, 0.17, 0, 2 * M_PI);
    cairo_stroke(cr);
  }

  FINISH
}

void dtgtk_cairo_paint_display_wavelet_scale(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(0.93, 1, 0, 0)

  if(flags & CPF_ACTIVE)
  {
    float x1 = 0.2f;
    float y1 = 1.f;

    cairo_move_to(cr, x1, y1);

    const int steps = 4;
    const float delta = 1. / (float)steps;
    for(int i = 0; i < steps; i++)
    {
      y1 -= delta;
      cairo_line_to(cr, x1, y1);
      x1 += delta;
      if(x1 > .9) x1 = .9;
      cairo_line_to(cr, x1, y1);
    }
    cairo_stroke(cr);

    cairo_set_line_width(cr, 0.1);
    cairo_rectangle(cr, 0., 0., 1., 1.);
    cairo_stroke(cr);
  }
  else
  {
    cairo_move_to(cr, 0.08, 1.);
    cairo_curve_to(cr, 0.4, 0.05, 0.6, 0.05, 1., 1.);
    cairo_line_to(cr, 0.08, 1.);
    cairo_fill(cr);

    cairo_set_line_width(cr, 0.1);
    cairo_rectangle(cr, 0., 0., 1., 1.);
    cairo_stroke(cr);
  }

  FINISH
}

void dtgtk_cairo_paint_auto_levels(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0, 0)

  cairo_move_to(cr, .1, 0.3);
  cairo_line_to(cr, .1, 1.);
  cairo_stroke(cr);

  cairo_move_to(cr, .5, 0.1);
  cairo_line_to(cr, .5, 1.);
  cairo_stroke(cr);

  cairo_move_to(cr, .9, 0.3);
  cairo_line_to(cr, .9, 1.);
  cairo_stroke(cr);

  cairo_move_to(cr, 0., 1.0);
  cairo_line_to(cr, 1.0, 1.0);
  cairo_stroke(cr);

  FINISH
}

void _compass_star(cairo_t *cr, double cx, double cy, double size)
{
  const double a = size / 2.0;
  const double b = size / 10.0;

  cairo_move_to(cr, cx, cy - a);
  cairo_line_to(cr, cx + b, cy - b);
  cairo_line_to(cr, cx + a, cy);
  cairo_line_to(cr, cx + b, cy + b);
  cairo_line_to(cr, cx, cy + a);
  cairo_line_to(cr, cx - b, cy + b);
  cairo_line_to(cr, cx - a, cy);
  cairo_line_to(cr, cx - b, cy - b);
  cairo_close_path(cr);
  cairo_fill(cr);
}

void dtgtk_cairo_paint_compass_star(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0 , 0)

  _compass_star(cr, .5, .5, 1.);

  FINISH
}

void dtgtk_cairo_paint_wand(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0, 0)

  // the wand
  cairo_move_to(cr, 1., .85);
  cairo_line_to(cr, .85, 1.);
  cairo_line_to(cr, .2, .35);
  cairo_line_to(cr, .35, .2);
  cairo_close_path(cr);
  //cairo_stroke_preserve(cr);
  cairo_fill_preserve(cr);
  cairo_line_to(cr, .15, 0);
  cairo_line_to(cr, 0, .15);
  cairo_line_to(cr, .2, .35);
  cairo_stroke(cr);

  // the magic
  _compass_star(cr, .5, .1, .25);
  _compass_star(cr, .2, .65, .4);
  _compass_star(cr, .75, .25, .5);

  FINISH
}

void dtgtk_cairo_paint_lt_mode_grid(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1.4, 1, 0, 0)

  cairo_rectangle(cr, 0.05, 0.2, 0.9, 0.6);
  cairo_move_to(cr, 0.06, 0.5);
  cairo_line_to(cr, 0.94, 0.5);
  cairo_move_to(cr, 0.35, 0.21);
  cairo_line_to(cr, 0.35, 0.79);
  cairo_move_to(cr, 0.65, 0.21);
  cairo_line_to(cr, 0.65, 0.79);
  cairo_stroke(cr);

  cairo_set_line_width(cr, 0.08);
  cairo_rectangle(cr, 0.35, 0.5, 0.3, 0.3);
  cairo_stroke(cr);

  FINISH
}

void dtgtk_cairo_paint_lt_mode_zoom(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1.4, 1, 0, 0)

  cairo_rectangle(cr, 0.3, 0.2, 0.5, 0.34);

  cairo_move_to(cr, 0, 0.2);
  cairo_line_to(cr, 0.2, 0.2);
  cairo_line_to(cr, 0.2, 0.54);
  cairo_line_to(cr, 0, 0.54);

  cairo_move_to(cr, 1, 0.2);
  cairo_line_to(cr, 0.9, 0.2);
  cairo_line_to(cr, 0.9, 0.54);
  cairo_line_to(cr, 1, 0.54);

  cairo_move_to(cr, 0, 0.65);
  cairo_line_to(cr, 0.2, 0.65);
  cairo_line_to(cr, 0.2, 0.8);

  cairo_move_to(cr, 0.3, 0.8);
  cairo_line_to(cr, 0.3, 0.65);
  cairo_line_to(cr, 0.8, 0.65);
  cairo_line_to(cr, 0.8, 0.8);

  cairo_stroke(cr);

  FINISH
}

void dtgtk_cairo_paint_lt_mode_culling_fixed(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1.4, 1, 0, 0)

  cairo_rectangle(cr, 0.16, 0.22, 0.31, 0.56);
  cairo_rectangle(cr, 0.53, 0.22, 0.31, 0.56);
  cairo_stroke(cr);

  cairo_move_to(cr, 0.11, 0.4);
  cairo_line_to(cr, 0, 0.5);
  cairo_line_to(cr, 0.11, 0.6);
  cairo_fill(cr);

  cairo_move_to(cr, 0.89, 0.4);
  cairo_line_to(cr, 1, 0.5);
  cairo_line_to(cr, 0.89, 0.6);
  cairo_fill(cr);

// draw X
  cairo_move_to(cr, 0.22, 0.36);
  cairo_line_to(cr, 0.40, 0.64);
  cairo_move_to(cr, 0.22, 0.64);
  cairo_line_to(cr, 0.40, 0.36);

// draw Y
  cairo_move_to(cr, 0.59, 0.36);
  cairo_line_to(cr, 0.68, 0.5);
  cairo_line_to(cr, 0.77, 0.36);
  cairo_move_to(cr, 0.68, 0.5);
  cairo_line_to(cr, 0.68, 0.64);

  cairo_stroke(cr);

  FINISH
}

void dtgtk_cairo_paint_lt_mode_culling_dynamic(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1.4, 1, 0, 0)

  cairo_rectangle(cr, 0.16, 0.22, 0.31, 0.56);
  cairo_rectangle(cr, 0.53, 0.22, 0.31, 0.56);
  cairo_stroke(cr);

// draw X
  cairo_move_to(cr, 0.22, 0.36);
  cairo_line_to(cr, 0.40, 0.64);
  cairo_move_to(cr, 0.22, 0.64);
  cairo_line_to(cr, 0.40, 0.36);

// draw Y
  cairo_move_to(cr, 0.59, 0.36);
  cairo_line_to(cr, 0.68, 0.5);
  cairo_line_to(cr, 0.77, 0.36);
  cairo_move_to(cr, 0.68, 0.5);
  cairo_line_to(cr, 0.68, 0.64);

  cairo_stroke(cr);

  FINISH
}

void dtgtk_cairo_paint_lt_mode_fullpreview(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1.4, 1, 0, 0)

  cairo_rectangle(cr, 0.07, 0.22, 0.86, 0.56);
  cairo_stroke(cr);

  cairo_move_to(cr, 0.19, 0.29);
  cairo_line_to(cr, 0.19, 0.43);
  cairo_line_to(cr, 0.33, 0.29);
  cairo_fill(cr);

  cairo_move_to(cr, 0.19, 0.71);
  cairo_line_to(cr, 0.19, 0.57);
  cairo_line_to(cr, 0.33, 0.71);
  cairo_fill(cr);

  cairo_move_to(cr, 0.81, 0.71);
  cairo_line_to(cr, 0.81, 0.57);
  cairo_line_to(cr, 0.67, 0.71);
  cairo_fill(cr);

  cairo_move_to(cr, 0.81, 0.29);
  cairo_line_to(cr, 0.81, 0.43);
  cairo_line_to(cr, 0.67, 0.29);
  cairo_fill(cr);

  cairo_move_to(cr, 0.27, 0.37);
  cairo_line_to(cr, 0.30, 0.40);
  cairo_move_to(cr, 0.27, 0.63);
  cairo_line_to(cr, 0.30, 0.60);
  cairo_move_to(cr, 0.73, 0.63);
  cairo_line_to(cr, 0.70, 0.60);
  cairo_move_to(cr, 0.73, 0.37);
  cairo_line_to(cr, 0.70, 0.40);
  cairo_stroke(cr);

  FINISH
}

void dtgtk_cairo_paint_link(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0, 0)

  cairo_set_line_width(cr, .1);

  //arrow
  cairo_move_to(cr, .5, .5);
  cairo_line_to(cr, 1., 0.);
  cairo_stroke(cr);
  cairo_move_to(cr, .65, 0.);
  cairo_line_to(cr, 1., 0.);
  cairo_line_to(cr, 1., .35);
  cairo_stroke(cr);

  //rounded rectangle
  cairo_move_to(cr, .8, .6);
  cairo_line_to(cr, .8, .85);
  cairo_arc (cr, .65, .85, .15, 0., .5 * M_PI);
  cairo_line_to(cr, .15, 1.);
  cairo_arc (cr, .15, .85, .15, .5 * M_PI, M_PI);
  cairo_line_to(cr, 0., .35);
  cairo_arc (cr, .15, .35, .15, M_PI, 1.5 * M_PI);
  cairo_line_to(cr, .4, .2);
  cairo_stroke(cr);

  FINISH
}

void dtgtk_cairo_paint_shortcut(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1.15, 1, 0, 0)

  //keyboard outline
  cairo_set_line_width(cr, .05);
  cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);

  cairo_move_to(cr, .9, .27);
  cairo_line_to(cr, .1, .27);
  cairo_line_to(cr, .1, .73);
  cairo_line_to(cr, .9, .73);
  cairo_line_to(cr, .9, .27);

  cairo_stroke(cr);

  //keyboard buttons

  const double cr_linewidth=.04;
  const int toprow_keycount = 7;

  cairo_set_line_width(cr, cr_linewidth);
  cairo_set_line_join(cr, CAIRO_LINE_JOIN_MITER);
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);

  const double kheight = .73-.27;
  const double kwidth = .9 - .1;
  const double rspace = (kheight - cr_linewidth*3.)/4.;
  const double keylength = (kwidth-(cr_linewidth*2.0))/((double)toprow_keycount+((double)toprow_keycount/0.9));
  const double keyspace = keylength / 0.9;
  const double spacelength = kwidth / 2.0;

  // top row
  double keyrowwidth = keylength * toprow_keycount + keyspace*(toprow_keycount-1);
  double rowstartpos = .1 + (kwidth - keyrowwidth)/2;
  for(int i=0; i < 7; i++)
  {
    cairo_move_to(cr, rowstartpos + i*(keylength+keyspace), .27+rspace+cr_linewidth);
    cairo_line_to(cr, rowstartpos + i*(keylength+keyspace)+keylength, .27+rspace+cr_linewidth);
  }

  // middle row
  keyrowwidth = keylength * (toprow_keycount-1) + keyspace*(toprow_keycount-2);
  rowstartpos = .1 + (kwidth - keyrowwidth)/2;
  for(int i=0; i < 6; i++)
  {
    cairo_move_to(cr, rowstartpos + i*(keylength+keyspace), .27+(rspace+cr_linewidth)*2);
    cairo_line_to(cr, rowstartpos + i*(keylength+keyspace)+keylength, .27+(rspace+cr_linewidth)*2);
  }

  // 3rd (space) row
  keyrowwidth = spacelength;
  rowstartpos = .1 + (kwidth - keyrowwidth)/2;
  cairo_move_to(cr, rowstartpos , .27+(rspace+cr_linewidth)*3);
  cairo_line_to(cr, rowstartpos + spacelength, .27+(rspace+cr_linewidth)*3);

  cairo_stroke(cr);

  FINISH
}

void dtgtk_cairo_paint_pin(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0, 0)

  // the needle
  cairo_move_to(cr, 0.5, 0.5);
  cairo_line_to(cr, 0, 1.0);
  cairo_stroke(cr);

  // the main part
  cairo_move_to(cr, 0.13, 0.45);
  cairo_line_to(cr, 0.57, 0.88);
  cairo_line_to(cr, 0.67, 0.34);
  cairo_close_path(cr);
  cairo_fill(cr);
  cairo_move_to(cr, 0.61, 0);
  cairo_line_to(cr, 1.0, 0.4);
  cairo_line_to(cr, 0.34, 0.66);
  cairo_close_path(cr);
  cairo_fill(cr);

  FINISH
}

void dtgtk_cairo_paint_filtering_menu(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0, 0)

  // the filtering icon
  cairo_move_to(cr, 0.1, 0.0);
  cairo_line_to(cr, 0.8, 0.0);
  cairo_arc(cr, 0.8, 0.1, 0.1, -0.5 * M_PI, 0.2 * M_PI);
  cairo_move_to(cr, 0.85, 0.15);
  cairo_line_to(cr, 0.55, 0.5);
  cairo_line_to(cr, 0.55, 1.0);
  cairo_line_to(cr, 0.35, 0.8);
  cairo_line_to(cr, 0.35, 0.5);
  cairo_line_to(cr, 0.05, 0.15);
  cairo_arc(cr, 0.1, 0.1, 0.1, 0.8 * M_PI, 1.5 * M_PI);
  cairo_stroke(cr);

  // the "hamburger" menu icon
  cairo_move_to(cr, 0.75, 1.0);
  cairo_line_to(cr, 1.0, 1.0);
  cairo_stroke(cr);
  cairo_move_to(cr, 0.75, 0.8);
  cairo_line_to(cr, 1.0, 0.8);
  cairo_stroke(cr);
  cairo_move_to(cr, 0.75, 0.6);
  cairo_line_to(cr, 1.0, 0.6);
  cairo_stroke(cr);
  FINISH
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
