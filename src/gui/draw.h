/*
    This file is part of darktable,
    copyright (c) 2009--2010 johannes hanika.

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
#ifndef DT_GUI_DRAW_H
#define DT_GUI_DRAW_H
/** some common drawing routines. */

#ifdef HAVE_CONFIG_H
  #include "config.h"
#endif

#include <stdlib.h>
#include <math.h>
// this is a dirty hack, this way nikon_curve will not even be compiled if we don't need it:
#ifdef DT_CONTROL_H
  #include "common/nikon_curve.c"
#else
  #include "common/nikon_curve.h"
#endif
#include <cairo.h>

/** wrapper around nikon curve or gegl. */
typedef struct dt_draw_curve_t
{
  CurveData c;
  CurveSample csample;
}
dt_draw_curve_t;

static inline void dt_draw_grid(cairo_t *cr, const int num, const int left, const int top, const int right, const int bottom)
{
  float width = right - left;
  float height = bottom - top;

  for(int k=1;k<num;k++)
  {
    cairo_move_to(cr, left + k/(float)num*width, top); cairo_line_to(cr, left +  k/(float)num*width, bottom);
    cairo_stroke(cr);
    cairo_move_to(cr, left, top + k/(float)num*height); cairo_line_to(cr, right, top + k/(float)num*height);
    cairo_stroke(cr);
  }
}

static inline void dt_draw_endmarker(cairo_t *cr, const int width, const int height, const int left)
{
  // fibonacci spiral:
  float v[14] = { -8., 3.,
                  -8., 0., -13., 0., -13, 3.,
                  -13., 8., -8., 8., 0., 0.};
  for(int k=0;k<14;k+=2) v[k] = v[k]*0.01 + 0.5;
  for(int k=1;k<14;k+=2) v[k] = v[k]*0.03 + 0.5;
  for(int k=0;k<14;k+=2) v[k] *= width;
  for(int k=1;k<14;k+=2) v[k] *= height;
  if(left)
    for(int k=0;k<14;k+=2) v[k] = width - v[k];
  cairo_set_line_width(cr, 2.);
  cairo_set_source_rgb(cr, 0.3, 0.3, 0.3);
  cairo_move_to (cr, v[0], v[1]);
  cairo_curve_to(cr, v[2], v[3], v[4], v[5], v[6], v[7]);
  cairo_curve_to(cr, v[8], v[9], v[10], v[11], v[12], v[13]);
  for(int k=0;k<14;k+=2) v[k] = width - v[k];
  for(int k=1;k<14;k+=2) v[k] = height - v[k];
  cairo_curve_to(cr, v[10], v[11], v[8], v[9], v[6], v[7]);
  cairo_curve_to(cr, v[4], v[5], v[2], v[3], v[0], v[1]);
  cairo_stroke(cr);
}

static inline dt_draw_curve_t *dt_draw_curve_new(const float min, const float max)
{
  dt_draw_curve_t *c = (dt_draw_curve_t *)malloc(sizeof(dt_draw_curve_t));
  c->csample.m_samplingRes = 0x10000;
  c->csample.m_outputRes = 0x10000;
  c->csample.m_Samples = (uint16_t *)malloc(sizeof(uint16_t)*0x10000);

  c->c.m_curveType = TONE_CURVE;
  c->c.m_numAnchors = 0;
  c->c.m_gamma = 1.0;
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

static inline void dt_draw_curve_set_point(dt_draw_curve_t *c, const int num, const float x, const float y)
{
  c->c.m_anchors[num].x = x;
  c->c.m_anchors[num].y = y;
}

static inline void dt_draw_curve_calc_values(dt_draw_curve_t *c, const float min, const float max, const int res, float *x, float *y)
{
  c->csample.m_samplingRes = res;
  c->csample.m_outputRes = 0x10000;
  CurveDataSample(&c->c, &c->csample);
  if(x) for(int k=0;k<res;k++) x[k] = k*(1.0f/res);
  if(y) for(int k=0;k<res;k++)
    y[k] = min + (max-min)*c->csample.m_Samples[k]*(1.0f/0x10000);
}

static inline float dt_draw_curve_calc_value(dt_draw_curve_t *c, const float x)
{
  double xa[20], ya[20];
  for(int i=0; i<c->c.m_numAnchors; i++)
  {
    xa[i] = c->c.m_anchors[i].x;
    ya[i] = c->c.m_anchors[i].y;
  }
  double ypval = 0, yppval = 0;
  double *ypp = spline_cubic_set(c->c.m_numAnchors, xa, ya, 2, 0.0, 2, 0.0);
  double val = spline_cubic_val(c->c.m_numAnchors, xa, x, ya, ypp, &ypval, &yppval);
  free(ypp);
  return MIN(MAX(val, c->c.m_min_y), c->c.m_max_y);
}

static inline int dt_draw_curve_add_point(dt_draw_curve_t *c, const float x, const float y)
{
  c->c.m_anchors[c->c.m_numAnchors].x = x;
  c->c.m_anchors[c->c.m_numAnchors].y = y;
  c->c.m_numAnchors++;
  return 0;
}



#endif
