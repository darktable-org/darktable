/*
    This file is part of darktable,
    Copyright (C) 2009-2023 darktable developers.

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

/** some common drawing routines. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "common/curve_tools.h"
#include "common/darktable.h"
#include "common/splines.h"
#include "control/conf.h"
#include "develop/develop.h"
#include <cairo.h>
#include <glib.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <gui/gtk.h>

#ifndef M_PI
#define M_PI 3.141592654
#endif

/** wrapper around nikon curve. */
typedef struct dt_draw_curve_t
{
  CurveData c;
  CurveSample csample;
} dt_draw_curve_t;

/** set color based on gui overlay preference */
static inline void dt_draw_set_color_overlay(cairo_t *cr,
                                             const gboolean bright,
                                             const double alpha)
{
  const double amt =
    bright
    ? 0.5 + darktable.gui->overlay_contrast * 0.5
    : (1.0 - darktable.gui->overlay_contrast) * 0.5;

  cairo_set_source_rgba(cr,
                        darktable.gui->overlay_red * amt,
                        darktable.gui->overlay_green * amt,
                        darktable.gui->overlay_blue * amt, alpha);
}

/** draws a rating star
 */
static inline void dt_draw_star(cairo_t *cr,
                                const float x,
                                const float y,
                                const float r1,
                                const float r2)
{
  const float d = 2.0 * M_PI * 0.1f;

  const float dx[10] = { sinf(0.0),   sinf(d),     sinf(2 * d), sinf(3 * d), sinf(4 * d),
                         sinf(5 * d), sinf(6 * d), sinf(7 * d), sinf(8 * d), sinf(9 * d) };

  const float dy[10] = { cosf(0.0),   cosf(d),     cosf(2 * d), cosf(3 * d), cosf(4 * d),
                         cosf(5 * d), cosf(6 * d), cosf(7 * d), cosf(8 * d), cosf(9 * d) };

  cairo_move_to(cr, x + r1 * dx[0], y - r1 * dy[0]);
  for(int k = 1; k < 10; k++)
    if(k & 1)
      cairo_line_to(cr, x + r2 * dx[k], y - r2 * dy[k]);
    else
      cairo_line_to(cr, x + r1 * dx[k], y - r1 * dy[k]);
  cairo_close_path(cr);
}

static inline void dt_draw_line(cairo_t *cr,
                                const float left,
                                const float top,
                                const float right,
                                const float bottom)
{
  cairo_move_to(cr, left, top);
  cairo_line_to(cr, right, bottom);
}

static inline void dt_draw_grid(cairo_t *cr,
                                const int num,
                                const int left,
                                const int top,
                                const int right,
                                const int bottom)
{
  float width = right - left;
  float height = bottom - top;

  for(int k = 1; k < num; k++)
  {
    dt_draw_line(cr,
                 left + k / (float)num * width,
                 top,
                 left + k / (float)num * width,
                 bottom);
    cairo_stroke(cr);
    dt_draw_line(cr,
                 left,
                 top + k / (float)num * height,
                 right,
                 top + k / (float)num * height);
    cairo_stroke(cr);
  }
}

static inline float dt_curve_to_mouse(const float x,
                                      const float zoom_factor,
                                      const float offset)
{
  return (x - offset) * zoom_factor;
}

/* left, right, top, bottom are in curve coordinates [0..1] */
static inline void dt_draw_grid_zoomed(cairo_t *cr,
                                       const int num,
                                       const float left,
                                       const float top,
                                       const float right,
                                       const float bottom,
                                       const float width,
                                       const float height,
                                       const float zoom_factor,
                                       const float zoom_offset_x,
                                       const float zoom_offset_y)
{
  for(int k = 1; k < num; k++)
  {
    dt_draw_line
      (cr,
       dt_curve_to_mouse(left + k / (float)num, zoom_factor, zoom_offset_x) * width,
       dt_curve_to_mouse(top, zoom_factor, zoom_offset_y) * -height,
       dt_curve_to_mouse(left + k / (float)num, zoom_factor, zoom_offset_x) * width,
       dt_curve_to_mouse(bottom, zoom_factor, zoom_offset_y) * -height);
    cairo_stroke(cr);

    dt_draw_line
      (cr,
       dt_curve_to_mouse(left, zoom_factor, zoom_offset_x) * width,
       dt_curve_to_mouse(top + k / (float)num, zoom_factor, zoom_offset_y) * -height,
       dt_curve_to_mouse(right, zoom_factor, zoom_offset_x) * width,
       dt_curve_to_mouse(top + k / (float)num, zoom_factor, zoom_offset_y) * -height);
    cairo_stroke(cr);
  }
}

#ifdef _OPENMP
#pragma omp declare simd uniform(base)
#endif
static inline float dt_log_scale_axis(const float x,
                                      const float base)
{
  return logf(x * (base - 1.0f) + 1.f) / logf(base);
}

static inline void dt_draw_loglog_grid(cairo_t *cr,
                                       const int num,
                                       const int left,
                                       const int top,
                                       const int right,
                                       const int bottom,
                                       const float base)
{
  const float width = right - left;
  const float height = bottom - top;

  for(int k = 1; k < num; k++)
  {
    const float x = dt_log_scale_axis(k / (float)num, base);

    dt_draw_line(cr, left + x * width, top, left + x * width, bottom);
    cairo_stroke(cr);
    dt_draw_line(cr, left, top + x * height, right, top + x * height);
    cairo_stroke(cr);
  }
}

static inline void dt_draw_semilog_x_grid(cairo_t *cr,
                                          const int num,
                                          const int left,
                                          const int top,
                                          const int right,
                                          const int bottom,
                                          const float base)
{
  const float width = right - left;
  const float height = bottom - top;

  for(int k = 1; k < num; k++)
  {
    const float x = dt_log_scale_axis(k / (float)num, base);
    dt_draw_line(cr, left + x * width, top, left + x * width, bottom);
    cairo_stroke(cr);
    dt_draw_line(cr,
                 left,
                 top + k / (float)num * height,
                 right,
                 top + k / (float)num * height);
    cairo_stroke(cr);
  }
}

static inline void dt_draw_semilog_y_grid(cairo_t *cr,
                                          const int num,
                                          const int left,
                                          const int top,
                                          const int right,
                                          const int bottom,
                                          const float base)
{
  const float width = right - left;
  const float height = bottom - top;

  for(int k = 1; k < num; k++)
  {
    const float x = dt_log_scale_axis(k / (float)num, base);
    dt_draw_line(cr,
                 left + k / (float)num * width,
                 top,
                 left + k / (float)num * width,
                 bottom);
    cairo_stroke(cr);
    dt_draw_line(cr, left, top + x * height, right, top + x * height);
    cairo_stroke(cr);
  }
}


static inline void dt_draw_waveform_lines(cairo_t *cr,
                                          const int left,
                                          const int top,
                                          const int right,
                                          const int bottom,
                                          const gboolean horizontal)
{
  const float width = right - left;
  const float height = bottom - top;
  const int num = 9, middle = 5, white = 1;
  // FIXME: should this vary with ppd?
  const double dashes = 4.0;

  cairo_save(cr);

  // FIXME: should be using DT_PIXEL_APPLY_DPI()?
  const double wd = cairo_get_line_width(cr);

  for(int k = 1; k < num; k++)
  {
    cairo_set_dash(cr, &dashes, k == white || k == middle, 0);
    cairo_set_line_width(cr, k == white ? wd * 3 : k == middle ? wd * 2 : wd);
    if(horizontal)
      dt_draw_line(cr,
                   left,
                   top + k / (float)num * height,
                   right,
                   top + k / (float)num * height);
    else
      dt_draw_line(cr,
                   right - k / (float)num * width,
                   top,
                   right - k / (float)num * width,
                   bottom);
    cairo_stroke(cr);
  }

  cairo_restore(cr);
}

static inline void dt_draw_vertical_lines(cairo_t *cr,
                                          const int num,
                                          const int left,
                                          const int top,
                                          const int right,
                                          const int bottom)
{
  const float width = right - left;

  for(int k = 1; k < num; k++)
  {
    cairo_move_to(cr, left + k / (float)num * width, top);
    cairo_line_to(cr, left + k / (float)num * width, bottom);
    cairo_stroke(cr);
  }
}

static inline void dt_draw_horizontal_lines(cairo_t *cr,
                                            const int num,
                                            const int left,
                                            const int top,
                                            const int right,
                                            const int bottom)
{
  const float height = bottom - top;

  for(int k = 1; k < num; k++)
  {
    cairo_move_to(cr, left, top + k / (float)num * height);
    cairo_line_to(cr, right, top + k / (float)num * height);
    cairo_stroke(cr);
  }
}

static inline void dt_draw_endmarker(cairo_t *cr,
                                     const int width,
                                     const int height,
                                     const int left)
{
  // fibonacci spiral:
  float v[14] = { -8., 3., -8., 0., -13., 0., -13, 3., -13., 8., -8., 8., 0., 0. };

  for(int k = 0; k < 14; k += 2)
    v[k] = v[k] * 0.01 + 0.5;
  for(int k = 1; k < 14; k += 2)
    v[k] = v[k] * 0.03 + 0.5;
  for(int k = 0; k < 14; k += 2)
    v[k] *= width;
  for(int k = 1; k < 14; k += 2)
    v[k] *= height;

  if(left)
    for(int k = 0; k < 14; k += 2)
      v[k] = width - v[k];

  cairo_set_line_width(cr, 2.);
  cairo_set_source_rgb(cr, 0.3, 0.3, 0.3);
  cairo_move_to(cr, v[0], v[1]);
  cairo_curve_to(cr, v[2], v[3], v[4], v[5], v[6], v[7]);
  cairo_curve_to(cr, v[8], v[9], v[10], v[11], v[12], v[13]);

  for(int k = 0; k < 14; k += 2)
    v[k] = width - v[k];
  for(int k = 1; k < 14; k += 2)
    v[k] = height - v[k];

  cairo_curve_to(cr, v[10], v[11], v[8], v[9], v[6], v[7]);
  cairo_curve_to(cr, v[4], v[5], v[2], v[3], v[0], v[1]);
  cairo_stroke(cr);
}

static inline dt_draw_curve_t *dt_draw_curve_new(const float min,
                                                 const float max,
                                                 const unsigned int type)
{
  dt_draw_curve_t *c = (dt_draw_curve_t *)malloc(sizeof(dt_draw_curve_t));
  c->csample.m_samplingRes = 0x10000;
  c->csample.m_outputRes = 0x10000;
  c->csample.m_Samples = (uint16_t *)malloc(sizeof(uint16_t) * 0x10000);

  c->c.m_spline_type = type;
  c->c.m_numAnchors = 0;
  c->c.m_min_x = 0.0;
  c->c.m_max_x = 1.0;
  c->c.m_min_y = 0.0;
  c->c.m_max_y = 1.0;
  return c;
}

static inline void dt_draw_curve_destroy(dt_draw_curve_t *c)
{
  free(c->csample.m_Samples);
  free(c);
}

static inline void dt_draw_curve_set_point(dt_draw_curve_t *c,
                                           const int num,
                                           const float x,
                                           const float y)
{
  c->c.m_anchors[num].x = x;
  c->c.m_anchors[num].y = y;
}

static inline void dt_draw_curve_smaple_values(dt_draw_curve_t *c,
                                               const float min,
                                               const float max,
                                               const int res,
                                               float *x,
                                               float *y)
{
  if(x)
  {
#ifdef _OPENMP
#pragma omp parallel for SIMD() default(none) \
  dt_omp_firstprivate(res) shared(x) schedule(static)
#endif
    for(int k = 0; k < res; k++)
      x[k] = k * (1.0f / res);
  }
  if(y)
  {
#ifdef _OPENMP
#pragma omp parallel for SIMD() default(none) \
  dt_omp_firstprivate(min, max, res) shared(y, c) schedule(static)
#endif
    for(int k = 0; k < res; k++)
      y[k] = min + (max - min) * c->csample.m_Samples[k] * (1.0f / 0x10000);
  }
}

static inline void dt_draw_curve_calc_values(dt_draw_curve_t *c,
                                             const float min,
                                             const float max,
                                             const int res,
                                             float *x,
                                             float *y)
{
  c->csample.m_samplingRes = res;
  c->csample.m_outputRes = 0x10000;
  CurveDataSample(&c->c, &c->csample);
  dt_draw_curve_smaple_values(c, min, max, res, x, y);
}

static inline void dt_draw_curve_calc_values_V2_nonperiodic(dt_draw_curve_t *c,
                                                            const float min,
                                                            const float max,
                                                            const int res,
                                                            float *x,
                                                            float *y)
{
  c->csample.m_samplingRes = res;
  c->csample.m_outputRes = 0x10000;
  CurveDataSampleV2(&c->c, &c->csample);
  dt_draw_curve_smaple_values(c, min, max, res, x, y);
}

static inline void dt_draw_curve_calc_values_V2_periodic(dt_draw_curve_t *c,
                                                         const float min,
                                                         const float max,
                                                         const int res,
                                                         float *x,
                                                         float *y)
{
  c->csample.m_samplingRes = res;
  c->csample.m_outputRes = 0x10000;
  CurveDataSampleV2Periodic(&c->c, &c->csample);
  dt_draw_curve_smaple_values(c, min, max, res, x, y);
}

static inline void dt_draw_curve_calc_values_V2(dt_draw_curve_t *c,
                                                const float min,
                                                const float max,
                                                const int res,
                                                float *x,
                                                float *y,
                                                const gboolean periodic)
{
  if(periodic)
    dt_draw_curve_calc_values_V2_periodic(c, min, max, res, x, y);
  else
    dt_draw_curve_calc_values_V2_nonperiodic(c, min, max, res, x, y);
 }

static inline float dt_draw_curve_calc_value(dt_draw_curve_t *c, const float x)
{
  float xa[20], ya[20];
  float val = 0.f;
  float *ypp = NULL;

  for(int i = 0; i < c->c.m_numAnchors; i++)
  {
    xa[i] = c->c.m_anchors[i].x;
    ya[i] = c->c.m_anchors[i].y;
  }
  ypp = interpolate_set(c->c.m_numAnchors, xa, ya, c->c.m_spline_type);
  if(ypp)
  {
    val = interpolate_val(c->c.m_numAnchors, xa, x, ya, ypp, c->c.m_spline_type);
    free(ypp);
  }
  return MIN(MAX(val, c->c.m_min_y), c->c.m_max_y);
}

static inline int dt_draw_curve_add_point(dt_draw_curve_t *c,
                                          const float x,
                                          const float y)
{
  c->c.m_anchors[c->c.m_numAnchors].x = x;
  c->c.m_anchors[c->c.m_numAnchors].y = y;
  c->c.m_numAnchors++;
  return 0;
}

// linear x linear y
static inline void dt_draw_histogram_8_linxliny(cairo_t *cr,
                                                const uint32_t *hist,
                                                const int32_t channels,
                                                const int32_t channel)
{
  cairo_move_to(cr, 0, 0);

  for(int k = 0; k < 256; k++)
    cairo_line_to(cr, k, hist[channels * k + channel]);

  cairo_line_to(cr, 255, 0);
  cairo_close_path(cr);
  cairo_fill(cr);
}

static inline void dt_draw_histogram_8_zoomed(cairo_t *cr,
                                              const uint32_t *hist,
                                              const int32_t channels,
                                              const int32_t channel,
                                              const float zoom_factor,
                                              const float zoom_offset_x,
                                              const float zoom_offset_y,
                                              const gboolean linear)
{
  cairo_move_to(cr, -zoom_offset_x, -zoom_offset_y);

  for(int k = 0; k < 256; k++)
  {
    const float value =
      ((float)hist[channels * k + channel] - zoom_offset_y) * zoom_factor;
    const float hist_value = value < 0 ? 0.f : value;

    cairo_line_to(cr,
                  ((float)k - zoom_offset_x) * zoom_factor,
                  linear ? hist_value : logf(1.0f + hist_value));
  }
  cairo_line_to(cr, (255.f - zoom_offset_x), -zoom_offset_y * zoom_factor);
  cairo_close_path(cr);
  cairo_fill(cr);
}

// log x (scalable) & linear y
static inline void dt_draw_histogram_8_logxliny(cairo_t *cr,
                                                const uint32_t *hist,
                                                const int32_t channels,
                                                const int32_t channel,
                                                const float base_log)
{
  cairo_move_to(cr, 0, 0);

  for(int k = 0; k < 256; k++)
  {
    const float x =
      logf((float)k / 255.0f * (base_log - 1.0f) + 1.0f) / logf(base_log) * 255.0f;
    const float y = hist[channels * k + channel];
    cairo_line_to(cr, x, y);
  }
  cairo_line_to(cr, 255, 0);
  cairo_close_path(cr);
  cairo_fill(cr);
}

// log x (scalable) & log y
static inline void dt_draw_histogram_8_logxlogy(cairo_t *cr,
                                                const uint32_t *hist,
                                                const int32_t channels,
                                                const int32_t channel,
                                                const float base_log)
{
  cairo_move_to(cr, 0, 0);
  for(int k = 0; k < 256; k++)
  {
    const float x =
      logf((float)k / 255.0f * (base_log - 1.0f) + 1.0f) / logf(base_log) * 255.0f;
    const float y = logf(1.0 + hist[channels * k + channel]);
    cairo_line_to(cr, x, y);
  }
  cairo_line_to(cr, 255, 0);
  cairo_close_path(cr);
  cairo_fill(cr);
}

// linear x log y
static inline void dt_draw_histogram_8_linxlogy(cairo_t *cr,
                                                const uint32_t *hist,
                                                const int32_t channels,
                                                const int32_t channel)
{
  cairo_move_to(cr, 0, 0);
  for(int k = 0; k < 256; k++)
    cairo_line_to(cr, k, logf(1.0 + hist[channels * k + channel]));
  cairo_line_to(cr, 255, 0);
  cairo_close_path(cr);
  cairo_fill(cr);
}

// log x (scalable)
static inline void dt_draw_histogram_8_log_base(cairo_t *cr,
                                                const uint32_t *hist,
                                                const int32_t channels,
                                                const int32_t channel,
                                                const gboolean linear,
                                                const float base_log)
{

  if(linear) // linear y
    dt_draw_histogram_8_logxliny(cr, hist, channels, channel, base_log);
  else // log y
    dt_draw_histogram_8_logxlogy(cr, hist, channels, channel, base_log);
}

// linear x
static inline void dt_draw_histogram_8(cairo_t *cr,
                                       const uint32_t *hist,
                                       const int32_t channels,
                                       const int32_t channel,
                                       const gboolean linear)
{
  if(linear) // linear y
    dt_draw_histogram_8_linxliny(cr, hist, channels, channel);
  else // log y
    dt_draw_histogram_8_linxlogy(cr, hist, channels, channel);
}

/** transform a data blob from cairo's premultiplied rgba/bgra to
 * GdkPixbuf's un-premultiplied bgra/rgba */
static inline void dt_draw_cairo_to_gdk_pixbuf(uint8_t *data,
                                               const unsigned int width,
                                               const unsigned int height)
{
  for(uint32_t y = 0; y < height; y++)
    for(uint32_t x = 0; x < width; x++)
    {
      uint8_t *r, *g, *b, *a, tmp;
      r = &data[(y * width + x) * 4 + 0];
      g = &data[(y * width + x) * 4 + 1];
      b = &data[(y * width + x) * 4 + 2];
      a = &data[(y * width + x) * 4 + 3];

      // switch r and b
      tmp = *r;
      *r = *b;
      *b = tmp;

      // cairo uses premultiplied alpha, reverse that
      if(*a != 0)
      {
        const float inv_a = 255.0 / *a;
        *r *= inv_a;
        *g *= inv_a;
        *b *= inv_a;
      }
    }
}

static inline void dt_cairo_perceptual_gradient(cairo_pattern_t *grad,
                                                const double alpha)
{
  // Create a linear gradient from black to white
  cairo_pattern_add_color_stop_rgba(grad, 0.0, 0.0, 0.0, 0.0, alpha);
  cairo_pattern_add_color_stop_rgba(grad, 1.0, 1.0, 1.0, 1.0, alpha);
}

static inline GdkPixbuf *dt_draw_paint_to_pixbuf
  (GtkWidget *widget,
   const guint pixbuf_size,
   const int flags,
   void (*dtgtk_cairo_paint_fct)(cairo_t *cr,
                                 const gint x,
                                 const gint y,
                                 const gint w,
                                 const gint h,
                                 const gint flags,
                                 void *data))
{
  GdkRGBA fg_color;
  GtkStyleContext *context = gtk_widget_get_style_context(widget);
  GtkStateFlags state = gtk_widget_get_state_flags(widget);
  gtk_style_context_get_color(context, state, &fg_color);

  const int dim = DT_PIXEL_APPLY_DPI(pixbuf_size);
  cairo_surface_t *cst = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, dim, dim);
  cairo_t *cr = cairo_create(cst);
  gdk_cairo_set_source_rgba(cr, &fg_color);
  (*dtgtk_cairo_paint_fct)(cr, 0, 0, dim, dim, flags, NULL);
  cairo_destroy(cr);

  uint8_t *data = cairo_image_surface_get_data(cst);
  dt_draw_cairo_to_gdk_pixbuf(data, dim, dim);
  const size_t size = (size_t)dim * dim * 4;
  uint8_t *buf = (uint8_t *)malloc(size);
  memcpy(buf, data, size);
  GdkPixbuf *pixbuf = gdk_pixbuf_new_from_data(buf, GDK_COLORSPACE_RGB,
                                               TRUE, 8, dim, dim, dim * 4,
                                               (GdkPixbufDestroyNotify)free, NULL);
  cairo_surface_destroy(cst);
  return pixbuf;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
