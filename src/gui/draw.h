#ifndef DT_GUI_DRAW_H
#define DT_GUI_DRAW_H
/** some common drawing routines. */

#ifdef HAVE_CONFIG_H
  #include "config.h"
#endif

#include <stdlib.h>
#ifdef HAVE_GEGL
  #ifndef USE_GEGL_CURVE
    #define USE_GEGL_CURVE
  #endif
#endif
#ifdef USE_GEGL_CURVE
  #include <gegl.h>
#else // this is a dirty hack, this way nikon_curve will not even be compiled if we don't need it:
  #include "common/nikon_curve.c"
#endif
#include <cairo.h>

/** wrapper around nikon curve or gegl. */
typedef struct dt_draw_curve_t
{
#ifdef USE_GEGL_CURVE
  GeglCurve *c;
#else
  CurveData c;
  CurveSample csample;
#endif
}
dt_draw_curve_t;

static inline void dt_draw_grid(cairo_t *cr, const int num, const int width, const int height)
{
  for(int k=1;k<num;k++)
  {
    cairo_move_to(cr, k/(float)num*width, 0); cairo_line_to(cr, k/(float)num*width, height);
    cairo_stroke(cr);
    cairo_move_to(cr, 0, k/(float)num*height); cairo_line_to(cr, width, k/(float)num*height);
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
#ifdef USE_GEGL_CURVE
  c->c = gegl_curve_new(min, max);
  g_object_ref(c->c);
#else
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
#endif
  return c;
}

static inline void dt_draw_curve_destroy(dt_draw_curve_t *c)
{
#ifdef USE_GEGL_CURVE
  g_object_unref(c->c);
  free(c);
#else
  free(c->csample.m_Samples);
  free(c);
#endif
}

static inline void dt_draw_curve_set_point(dt_draw_curve_t *c, const int num, const float x, const float y)
{
#ifdef USE_GEGL_CURVE
  gegl_curve_set_point(c->c, num, x, y);
#else
  c->c.m_anchors[num].x = x;
  c->c.m_anchors[num].y = y;
#endif
}

static inline void dt_draw_curve_calc_values(dt_draw_curve_t *c, const float min, const float max, const int res, double *x, double *y)
{
#ifdef USE_GEGL_CURVE
  gegl_curve_calc_values(c->c, min, max, res, x, y);
#else
  c->csample.m_samplingRes = res;
  c->csample.m_outputRes = 0x10000;
  CurveDataSample(&c->c, &c->csample);
  for(int k=0;k<res;k++)
  {
    x[k] = k*(1.0/res);
    y[k] = min + (max-min)*c->csample.m_Samples[k]*(1.0/0x10000);
  }
#endif
}

static inline float dt_draw_curve_calc_value(dt_draw_curve_t *c, const float x)
{
#ifdef USE_GEGL_CURVE
  return gegl_curve_calc_value(c->c, x);
#else
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
#endif
}

static inline int dt_draw_curve_add_point(dt_draw_curve_t *c, const float x, const float y)
{
#ifdef USE_GEGL_CURVE
  return gegl_curve_add_point(c->c, x, y);
#else
  c->c.m_anchors[c->c.m_numAnchors].x = x;
  c->c.m_anchors[c->c.m_numAnchors].y = y;
  c->c.m_numAnchors++;
  return 0;
#endif
}

#endif
