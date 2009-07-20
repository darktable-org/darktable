#ifndef DT_GUI_DRAW_H
#define DT_GUI_DRAW_H
/** some common drawing routines. */

// TODO: remove gegl ifndef HAVE_GEGL and use nikon curve!
#include <gegl.h>
#include <cairo.h>

/** wrapper around nikon curve or gegl. */
typedef struct dt_draw_curve_t
{
  GeglCurve *c;
}
dt_draw_curve_t;

void dt_draw_grid(cairo_t *cr, const int num, const int width, const int height)
{
  for(int k=1;k<num;k++)
  {
    cairo_move_to(cr, k/(float)num*width, 0); cairo_line_to(cr, k/(float)num*width, height);
    cairo_stroke(cr);
    cairo_move_to(cr, 0, k/(float)num*height); cairo_line_to(cr, width, k/(float)num*height);
    cairo_stroke(cr);
  }
}

static inline dt_draw_curve_t *dt_draw_curve_new(const float min, const float max)
{
  dt_draw_curve_t *c = (dt_draw_curve_t *)malloc(sizeof(dt_draw_curve_t));
  c->c = gegl_curve_new(min, max);
  g_object_ref(c->c);
  return c;
}

static inline void dt_draw_curve_destroy(dt_draw_curve_t *c)
{
  g_object_unref(c->c);
  free(c);
}

static inline void dt_draw_curve_set_point(dt_draw_curve_t *c, const int num, const float x, const float y)
{
  gegl_curve_set_point(c->c, num, x, y);
}

static inline void dt_draw_curve_calc_values(dt_draw_curve_t *c, const float min, const float max, const int res, double *x, double *y)
{
  gegl_curve_calc_values(c->c, min, max, res, x, y);
}

static inline float dt_draw_curve_calc_value(dt_draw_curve_t *c, const float x)
{
  return gegl_curve_calc_value(c->c, x);
}

static inline int dt_draw_curve_add_point(dt_draw_curve_t *c, const float x, const float y)
{
  return gegl_curve_add_point(c->c, x, y);
}

#endif
