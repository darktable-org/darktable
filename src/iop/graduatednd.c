/*
    This file is part of darktable,
    copyright (c) 2010-2012 Henrik Andersson.

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
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <stdlib.h>
#include <math.h>
#include <assert.h>
#include <string.h>
#ifdef HAVE_GEGL
#include <gegl.h>
#endif
#include "bauhaus/bauhaus.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/tiling.h"
#include "control/control.h"
#include "common/colorspaces.h"
#include "common/debug.h"
#include "common/opencl.h"
#include "dtgtk/gradientslider.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include <xmmintrin.h>

#define CLIP(x) ((x < 0.0f) ? 0.0f : (x > 1.0f) ? 1.0f : x)

DT_MODULE_INTROSPECTION(1, dt_iop_graduatednd_params_t)

typedef struct dt_iop_graduatednd_params_t
{
  float density;
  float compression;
  float rotation;
  float offset;
  float hue;
  float saturation;
} dt_iop_graduatednd_params_t;

typedef struct dt_iop_graduatednd_global_data_t
{
  int kernel_graduatedndp;
  int kernel_graduatedndm;
} dt_iop_graduatednd_global_data_t;


void init_presets(dt_iop_module_so_t *self)
{
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "begin", NULL, NULL, NULL);

  dt_gui_presets_add_generic(_("neutral gray ND2 (soft)"), self->op, self->version(),
                             &(dt_iop_graduatednd_params_t){ 1, 0, 0, 50, 0, 0 },
                             sizeof(dt_iop_graduatednd_params_t), 1);
  dt_gui_presets_add_generic(_("neutral gray ND4 (soft)"), self->op, self->version(),
                             &(dt_iop_graduatednd_params_t){ 2, 0, 0, 50, 0, 0 },
                             sizeof(dt_iop_graduatednd_params_t), 1);
  dt_gui_presets_add_generic(_("neutral gray ND8 (soft)"), self->op, self->version(),
                             &(dt_iop_graduatednd_params_t){ 3, 0, 0, 50, 0, 0 },
                             sizeof(dt_iop_graduatednd_params_t), 1);
  dt_gui_presets_add_generic(_("neutral gray ND2 (hard)"), self->op, self->version(),
                             &(dt_iop_graduatednd_params_t){ 1, 75, 0, 50, 0, 0 },
                             sizeof(dt_iop_graduatednd_params_t), 1);
  dt_gui_presets_add_generic(_("neutral gray ND4 (hard)"), self->op, self->version(),
                             &(dt_iop_graduatednd_params_t){ 2, 75, 0, 50, 0, 0 },
                             sizeof(dt_iop_graduatednd_params_t), 1);
  dt_gui_presets_add_generic(_("neutral gray ND8 (hard)"), self->op, self->version(),
                             &(dt_iop_graduatednd_params_t){ 3, 75, 0, 50, 0, 0 },
                             sizeof(dt_iop_graduatednd_params_t), 1);
  dt_gui_presets_add_generic(_("orange ND2 (soft)"), self->op, self->version(),
                             &(dt_iop_graduatednd_params_t){ 1, 0, 0, 50, 0.102439, 0.8 },
                             sizeof(dt_iop_graduatednd_params_t), 1);
  dt_gui_presets_add_generic(_("yellow ND2 (soft)"), self->op, self->version(),
                             &(dt_iop_graduatednd_params_t){ 1, 0, 0, 50, 0.151220, 0.5 },
                             sizeof(dt_iop_graduatednd_params_t), 1);
  dt_gui_presets_add_generic(_("purple ND2 (soft)"), self->op, self->version(),
                             &(dt_iop_graduatednd_params_t){ 1, 0, 0, 50, 0.824390, 0.5 },
                             sizeof(dt_iop_graduatednd_params_t), 1);
  dt_gui_presets_add_generic(_("green ND2 (soft)"), self->op, self->version(),
                             &(dt_iop_graduatednd_params_t){ 1, 0, 0, 50, 0.302439, 0.5 },
                             sizeof(dt_iop_graduatednd_params_t), 1);
  dt_gui_presets_add_generic(_("red ND2 (soft)"), self->op, self->version(),
                             &(dt_iop_graduatednd_params_t){ 1, 0, 0, 50, 0, 0.5 },
                             sizeof(dt_iop_graduatednd_params_t), 1);
  dt_gui_presets_add_generic(_("blue ND2 (soft)"), self->op, self->version(),
                             &(dt_iop_graduatednd_params_t){ 1, 0, 0, 50, 0.663415, 0.5 },
                             sizeof(dt_iop_graduatednd_params_t), 1);
  dt_gui_presets_add_generic(_("brown ND4 (soft)"), self->op, self->version(),
                             &(dt_iop_graduatednd_params_t){ 2, 0, 0, 50, 0.082927, 0.25 },
                             sizeof(dt_iop_graduatednd_params_t), 1);

  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "commit", NULL, NULL, NULL);
}

typedef struct dt_iop_graduatednd_gui_data_t
{
  GtkBox *vbox;
  GtkWidget *label1, *label2, *label3, *label5, *label6; // density, compression, rotation, hue, saturation
  GtkWidget *scale1, *scale2, *scale3;                   // density, compression, rotation
  GtkWidget *gslider1, *gslider2;                        // hue, saturation

  int selected;
  int dragging;

  gboolean define;
  float xa, ya, xb, yb, oldx, oldy;
} dt_iop_graduatednd_gui_data_t;

typedef struct dt_iop_graduatednd_data_t
{
  float density;     // The density of filter 0-8 EV
  float compression; // Default 0% = soft and 100% = hard
  float rotation;    // 2*PI -180 - +180
  float offset;      // Default 50%, centered, can be offsetted...
  float hue;         // the hue
  float saturation;  // the saturation
} dt_iop_graduatednd_data_t;

const char *name()
{
  return _("graduated density");
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING
         | IOP_FLAGS_TILING_FULL_ROI;
}

int groups()
{
  return IOP_GROUP_EFFECT;
}

void init_key_accels(dt_iop_module_so_t *self)
{
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "density"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "compression"));
}

void connect_key_accels(dt_iop_module_t *self)
{
  dt_iop_graduatednd_gui_data_t *g = (dt_iop_graduatednd_gui_data_t *)self->gui_data;

  dt_accel_connect_slider_iop(self, "density", GTK_WIDGET(g->scale1));
  dt_accel_connect_slider_iop(self, "compression", GTK_WIDGET(g->scale2));
}

static inline float f(const float t, const float c, const float x)
{
  return (t / (1.0f + powf(c, -x * 6.0f)) + (1.0f - t) * (x * .5f + .5f));
}

typedef struct dt_iop_vector_2d_t
{
  double x;
  double y;
} dt_iop_vector_2d_t;

// determine the distance between the segment [(xa,ya)(xb,yb)] and the point (xc,yc)
static float dist_seg(float xa, float ya, float xb, float yb, float xc, float yc)
{
  if(xa == xb && ya == yb) return (xc - xa) * (xc - xa) + (yc - ya) * (yc - ya);

  float sx = xb - xa;
  float sy = yb - ya;

  float ux = xc - xa;
  float uy = yc - ya;

  float dp = sx * ux + sy * uy;
  if(dp < 0) return (xc - xa) * (xc - xa) + (yc - ya) * (yc - ya);

  float sn2 = sx * sx + sy * sy;
  if(dp > sn2) return (xc - xb) * (xc - xb) + (yc - yb) * (yc - yb);

  float ah2 = dp * dp / sn2;
  float un2 = ux * ux + uy * uy;
  return un2 - ah2;
}

static int set_grad_from_points(struct dt_iop_module_t *self, float xa, float ya, float xb, float yb,
                                float *rotation, float *offset)
{
  // we want absolute positions
  float pts[4]
      = { xa * self->dev->preview_pipe->backbuf_width, ya * self->dev->preview_pipe->backbuf_height,
          xb * self->dev->preview_pipe->backbuf_width, yb * self->dev->preview_pipe->backbuf_height };
  dt_dev_distort_backtransform_plus(self->dev, self->dev->preview_pipe, self->priority + 1, 9999999, pts, 2);
  dt_dev_pixelpipe_iop_t *piece = dt_dev_distort_get_iop_pipe(self->dev, self->dev->preview_pipe, self);
  pts[0] /= (float)piece->buf_out.width;
  pts[2] /= (float)piece->buf_out.width;
  pts[1] /= (float)piece->buf_out.height;
  pts[3] /= (float)piece->buf_out.height;

  // we first need to find the rotation angle
  // weird dichotomic solution : we may use something more cool ...
  float v1 = -M_PI;
  float v2 = M_PI;
  float sinv, cosv, r1, r2, v, r;

  sinv = sinf(v1), cosv = cosf(v1);
  r1 = pts[1] * cosv - pts[0] * sinv + pts[2] * sinv - pts[3] * cosv;

  // we search v2 so r2 as not the same sign as r1
  float pas = M_PI / 16.0;
  do
  {
    v2 += pas;
    sinv = sinf(v2), cosv = cosf(v2);
    r2 = pts[1] * cosv - pts[0] * sinv + pts[2] * sinv - pts[3] * cosv;
    if(r1 * r2 < 0) break;
  } while(v2 <= M_PI);

  if(v2 == M_PI) return 9;

  int iter = 0;
  do
  {
    v = (v1 + v2) / 2.0;
    sinv = sinf(v), cosv = cosf(v);
    r = pts[1] * cosv - pts[0] * sinv + pts[2] * sinv - pts[3] * cosv;

    if(r < 0.01 && r > -0.01) break;

    if(r * r2 < 0)
      v1 = v;
    else
    {
      r2 = r;
      v2 = v;
    }

  } while(iter++ < 1000);
  if(iter >= 1000) return 8;

  // be careful to the gnd direction
  if(pts[2] - pts[0] > 0 && v > M_PI * 0.5) v = v - M_PI;
  if(pts[2] - pts[0] > 0 && v < -M_PI * 0.5) v = M_PI + v;

  if(pts[2] - pts[0] < 0 && v < M_PI * 0.5 && v >= 0) v = v - M_PI;
  if(pts[2] - pts[0] < 0 && v > -M_PI * 0.5 && v < 0) v = v + M_PI;

  *rotation = -v * 180.0 / M_PI;

  // and now we go for the offset (more easy)
  sinv = sinf(v);
  cosv = cosf(v);
  float ofs = -2.0 * sinv * pts[0] + sinv - cosv + 1.0 + 2.0 * cosv * pts[1];
  *offset = ofs * 50.0;

  return 1;
}

static int set_points_from_grad(struct dt_iop_module_t *self, float *xa, float *ya, float *xb, float *yb,
                                float rotation, float offset)
{
  // we get the extremities of the line
  const float v = (-rotation / 180) * M_PI;
  const float sinv = sin(v);
  float pts[4];

  dt_dev_pixelpipe_iop_t *piece = dt_dev_distort_get_iop_pipe(self->dev, self->dev->preview_pipe, self);
  if(!piece) return 0;
  float wp = piece->buf_out.width, hp = piece->buf_out.height;

  // if sinv=0 then this is just the offset
  if(sinv == 0)
  {
    if(v == 0)
    {
      pts[0] = wp * 0.1;
      pts[2] = wp * 0.9;
      pts[1] = pts[3] = hp * offset / 100.0;
    }
    else
    {
      pts[2] = wp * 0.1;
      pts[0] = wp * 0.9;
      pts[1] = pts[3] = hp * (1.0 - offset / 100.0);
    }
  }
  else
  {
    // otherwise we determine the extremities
    const float cosv = cos(v);
    float xx1 = (sinv - cosv + 1.0 - offset / 50.0) * wp * 0.5 / sinv;
    float xx2 = (sinv + cosv + 1.0 - offset / 50.0) * wp * 0.5 / sinv;
    float yy1 = 0;
    float yy2 = hp;
    float a = hp / (xx2 - xx1);
    float b = -xx1 * a;
    // now ensure that the line isn't outside image borders
    if(xx2 > wp)
    {
      yy2 = a * wp + b;
      xx2 = wp;
    }
    if(xx2 < 0)
    {
      yy2 = b;
      xx2 = 0;
    }
    if(xx1 > wp)
    {
      yy1 = a * wp + b;
      xx1 = wp;
    }
    if(xx1 < 0)
    {
      yy1 = b;
      xx1 = 0;
    }

    // we want extremities not to be on image border
    xx2 -= (xx2 - xx1) * 0.1;
    xx1 += (xx2 - xx1) * 0.1;
    yy2 -= (yy2 - yy1) * 0.1;
    yy1 += (yy2 - yy1) * 0.1;

    // now we have to decide which point is where, depending of the angle
    /*xx1 /= wd;
    xx2 /= wd;
    yy1 /= ht;
    yy2 /= ht;*/
    if(v < M_PI * 0.5 && v > -M_PI * 0.5)
    {
      // we want xa < xb
      if(xx1 < xx2)
      {
        pts[0] = xx1;
        pts[1] = yy1;
        pts[2] = xx2;
        pts[3] = yy2;
      }
      else
      {
        pts[2] = xx1;
        pts[3] = yy1;
        pts[0] = xx2;
        pts[1] = yy2;
      }
    }
    else
    {
      // we want xb < xa
      if(xx2 < xx1)
      {
        pts[0] = xx1;
        pts[1] = yy1;
        pts[2] = xx2;
        pts[3] = yy2;
      }
      else
      {
        pts[2] = xx1;
        pts[3] = yy1;
        pts[0] = xx2;
        pts[1] = yy2;
      }
    }
  }
  // now we want that points to take care of distort modules

  if(!dt_dev_distort_transform_plus(self->dev, self->dev->preview_pipe, self->priority + 1, 999999, pts, 2))
    return 0;
  *xa = pts[0] / self->dev->preview_pipe->backbuf_width;
  *ya = pts[1] / self->dev->preview_pipe->backbuf_height;
  *xb = pts[2] / self->dev->preview_pipe->backbuf_width;
  *yb = pts[3] / self->dev->preview_pipe->backbuf_height;
  return 1;
}

void gui_post_expose(struct dt_iop_module_t *self, cairo_t *cr, int32_t width, int32_t height,
                     int32_t pointerx, int32_t pointery)
{
  dt_develop_t *dev = self->dev;
  dt_iop_graduatednd_gui_data_t *g = (dt_iop_graduatednd_gui_data_t *)self->gui_data;
  dt_iop_graduatednd_params_t *p = (dt_iop_graduatednd_params_t *)self->params;

  float wd = dev->preview_pipe->backbuf_width;
  float ht = dev->preview_pipe->backbuf_height;
  float zoom_y = dt_control_get_dev_zoom_y();
  float zoom_x = dt_control_get_dev_zoom_x();
  dt_dev_zoom_t zoom = dt_control_get_dev_zoom();
  int closeup = dt_control_get_dev_closeup();
  float zoom_scale = dt_dev_get_zoom_scale(dev, zoom, closeup ? 2 : 1, 1);

  cairo_translate(cr, width / 2.0, height / 2.0f);
  cairo_scale(cr, zoom_scale, zoom_scale);
  cairo_translate(cr, -.5f * wd - zoom_x * wd, -.5f * ht - zoom_y * ht);

  // we get the extremities of the line
  if(g->define == 0)
  {
    if(!set_points_from_grad(self, &g->xa, &g->ya, &g->xb, &g->yb, p->rotation, p->offset)) return;
    g->define = 1;
  }

  float xa = g->xa * wd, xb = g->xb * wd, ya = g->ya * ht, yb = g->yb * ht;
  // the lines
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
  if(g->selected == 3 || g->dragging == 3)
    cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(5.0) / zoom_scale);
  else
    cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(3.0) / zoom_scale);
  cairo_set_source_rgba(cr, .3, .3, .3, .8);

  cairo_move_to(cr, xa, ya);
  cairo_line_to(cr, xb, yb);
  cairo_stroke(cr);

  if(g->selected == 3 || g->dragging == 3)
    cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(2.0) / zoom_scale);
  else
    cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.0) / zoom_scale);
  cairo_set_source_rgba(cr, .8, .8, .8, .8);
  cairo_move_to(cr, xa, ya);
  cairo_line_to(cr, xb, yb);
  cairo_stroke(cr);

  // the extremities
  float x1, y1, x2, y2;
  float l = sqrt((xb - xa) * (xb - xa) + (yb - ya) * (yb - ya));
  const float ext = wd * 0.01f / zoom_scale;
  x1 = xa + (xb - xa) * ext / l;
  y1 = ya + (yb - ya) * ext / l;
  x2 = (xa + x1) / 2.0;
  y2 = (ya + y1) / 2.0;
  y2 += (x1 - xa);
  x2 -= (y1 - ya);
  cairo_move_to(cr, xa, ya);
  cairo_line_to(cr, x1, y1);
  cairo_line_to(cr, x2, y2);
  cairo_close_path(cr);
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.0) / zoom_scale);
  if(g->selected == 1 || g->dragging == 1)
    cairo_set_source_rgba(cr, .8, .8, .8, 1.0);
  else
    cairo_set_source_rgba(cr, .8, .8, .8, .5);
  cairo_fill_preserve(cr);
  if(g->selected == 1 || g->dragging == 1)
    cairo_set_source_rgba(cr, .3, .3, .3, 1.0);
  else
    cairo_set_source_rgba(cr, .3, .3, .3, .5);
  cairo_stroke(cr);

  x1 = xb - (xb - xa) * ext / l;
  y1 = yb - (yb - ya) * ext / l;
  x2 = (xb + x1) / 2.0;
  y2 = (yb + y1) / 2.0;
  y2 += (xb - x1);
  x2 -= (yb - y1);
  cairo_move_to(cr, xb, yb);
  cairo_line_to(cr, x1, y1);
  cairo_line_to(cr, x2, y2);
  cairo_close_path(cr);
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.0) / zoom_scale);
  if(g->selected == 2 || g->dragging == 2)
    cairo_set_source_rgba(cr, .8, .8, .8, 1.0);
  else
    cairo_set_source_rgba(cr, .8, .8, .8, .5);
  cairo_fill_preserve(cr);
  if(g->selected == 2 || g->dragging == 2)
    cairo_set_source_rgba(cr, .3, .3, .3, 1.0);
  else
    cairo_set_source_rgba(cr, .3, .3, .3, .5);
  cairo_stroke(cr);
}

int mouse_moved(struct dt_iop_module_t *self, double x, double y, double pressure, int which)
{
  dt_iop_graduatednd_gui_data_t *g = (dt_iop_graduatednd_gui_data_t *)self->gui_data;
  dt_dev_zoom_t zoom = dt_control_get_dev_zoom();
  int closeup = dt_control_get_dev_closeup();
  float zoom_scale = dt_dev_get_zoom_scale(self->dev, zoom, closeup ? 2 : 1, 1);
  float pzx, pzy;
  dt_dev_get_pointer_zoom_pos(self->dev, x, y, &pzx, &pzy);
  pzx += 0.5f;
  pzy += 0.5f;

  // are we dragging something ?
  if(g->dragging > 0)
  {
    if(g->dragging == 1)
    {
      // we are dragging xa,ya
      g->xa = pzx;
      g->ya = pzy;
    }
    else if(g->dragging == 2)
    {
      // we are dragging xb,yb
      g->xb = pzx;
      g->yb = pzy;
    }
    else if(g->dragging == 3)
    {
      // we are dragging the entire line
      g->xa += pzx - g->oldx;
      g->xb += pzx - g->oldx;
      g->ya += pzy - g->oldy;
      g->yb += pzy - g->oldy;
      g->oldx = pzx;
      g->oldy = pzy;
    }
  }
  else
  {
    g->selected = 0;
    const float ext = DT_PIXEL_APPLY_DPI(0.02f) / zoom_scale;
    // are we near extermity ?
    if(pzy > g->ya - ext && pzy < g->ya + ext && pzx > g->xa - ext && pzx < g->xa + ext)
    {
      g->selected = 1;
    }
    else if(pzy > g->yb - ext && pzy < g->yb + ext && pzx > g->xb - ext && pzx < g->xb + ext)
    {
      g->selected = 2;
    }
    else if(dist_seg(g->xa, g->ya, g->xb, g->yb, pzx, pzy) < ext * ext * 0.5)
      g->selected = 3;
  }

  dt_control_queue_redraw_center();
  return 1;
}

int button_pressed(struct dt_iop_module_t *self, double x, double y, double pressure, int which, int type,
                   uint32_t state)
{
  dt_iop_graduatednd_gui_data_t *g = (dt_iop_graduatednd_gui_data_t *)self->gui_data;
  float pzx, pzy;
  dt_dev_get_pointer_zoom_pos(self->dev, x, y, &pzx, &pzy);
  pzx += 0.5f;
  pzy += 0.5f;

  if(which == 3)
  {
    g->dragging = 2;
    g->xa = pzx;
    g->ya = pzy;
    g->xb = pzx;
    g->yb = pzy;
    g->oldx = pzx;
    g->oldy = pzy;
    return 1;
  }
  else if(g->selected > 0 && which == 1)
  {
    g->dragging = g->selected;
    g->oldx = pzx;
    g->oldy = pzy;
    return 1;
  }
  g->dragging = 0;
  return 0;
}

int button_released(struct dt_iop_module_t *self, double x, double y, int which, uint32_t state)
{
  dt_iop_graduatednd_gui_data_t *g = (dt_iop_graduatednd_gui_data_t *)self->gui_data;
  dt_iop_graduatednd_params_t *p = (dt_iop_graduatednd_params_t *)self->params;
  if(g->dragging > 0)
  {
    // dt_iop_graduatednd_params_t *p   = (dt_iop_graduatednd_params_t *)self->params;
    // float wd = self->dev->preview_pipe->backbuf_width;
    // float ht = self->dev->preview_pipe->backbuf_height;
    float pzx, pzy;
    dt_dev_get_pointer_zoom_pos(self->dev, x, y, &pzx, &pzy);
    pzx += 0.5f;
    pzy += 0.5f;

    float r = 0.0, o = 0.0;
    // float pts[4];
    // dt_dev_distort_backtransform(self->dev,pts,2);
    set_grad_from_points(self, g->xa, g->ya, g->xb, g->yb, &r, &o);

    // if this is a "line dragging, we reset extremities, to be sure they are not outside the image
    if(g->dragging == 3)
    {
      /*
       * whole line dragging should not change rotation, so we should reuse
       * old rotation to avoid rounding issues
       */
      r = p->rotation;
      set_points_from_grad(self, &g->xa, &g->ya, &g->xb, &g->yb, r, o);
    }
    self->dt->gui->reset = 1;
    dt_bauhaus_slider_set(g->scale3, r);
    // dt_bauhaus_slider_set(g->scale4,o);
    self->dt->gui->reset = 0;
    p->rotation = r;
    p->offset = o;
    g->dragging = 0;
    dt_dev_add_history_item(darktable.develop, self, TRUE);
  }

  g->dragging = 0;
  return 0;
}

int scrolled(dt_iop_module_t *self, double x, double y, int up, uint32_t state)
{
  dt_iop_graduatednd_gui_data_t *g = (dt_iop_graduatednd_gui_data_t *)self->gui_data;
  dt_iop_graduatednd_params_t *p = (dt_iop_graduatednd_params_t *)self->params;
  if((state & GDK_CONTROL_MASK) == GDK_CONTROL_MASK)
  {
    float dens;
    if(up)
      dens = fminf(8.0, p->density + 0.1);
    else
      dens = fmaxf(-8.0, p->density - 0.1);
    if(dens != p->density)
    {
      dt_bauhaus_slider_set(g->scale1, dens);
    }
    return 1;
  }
  if((state & GDK_SHIFT_MASK) == GDK_SHIFT_MASK)
  {
    float comp;
    if(up)
      comp = fminf(100.0, p->compression + 1.0);
    else
      comp = fmaxf(0.0, p->compression - 1.0);
    if(comp != p->compression)
    {
      dt_bauhaus_slider_set(g->scale2, comp);
    }
    return 1;
  }
  return 0;
}

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *ivoid, void *ovoid,
             const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  const dt_iop_graduatednd_data_t *data = (dt_iop_graduatednd_data_t *)piece->data;
  const int ch = piece->colors;

  const int ix = (roi_in->x);
  const int iy = (roi_in->y);
  const float iw = piece->buf_in.width * roi_out->scale;
  const float ih = piece->buf_in.height * roi_out->scale;
  const float hw = iw / 2.0;
  const float hh = ih / 2.0;
  const float hw_inv = 1.0 / hw;
  const float hh_inv = 1.0 / hh;
  const float v = (-data->rotation / 180) * M_PI;
  const float sinv = sin(v);
  const float cosv = cos(v);
  const float filter_radie = sqrt((hh * hh) + (hw * hw)) / hh;
  const float offset = data->offset / 100.0 * 2;

  float color[3];
  hsl2rgb(color, data->hue, data->saturation, 0.5);
  if(data->density < 0)
    for(int l = 0; l < 3; l++) color[l] = 1.0 - color[l];

#if 1
  const float filter_compression = 1.0 / filter_radie
                                   / (1.0 - (0.5 + (data->compression / 100.0) * 0.9 / 2.0)) * 0.5;
#else
  const float compression = data->compression / 100.0f;
  const float t = 1.0f - .8f / (.8f + compression);
  const float c = 1.0f + 1000.0f * powf(4.0, compression);
#endif


  if(data->density > 0)
  {
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(roi_out, color, data, ivoid, ovoid) schedule(static)
#endif
    for(int y = 0; y < roi_out->height; y++)
    {
      size_t k = (size_t)roi_out->width * y * ch;
      const float *in = (float *)ivoid + k;
      float *out = (float *)ovoid + k;

      float length = (sinv * (-1.0 + ix * hw_inv) - cosv * (-1.0 + (iy + y) * hh_inv) - 1.0 + offset)
                     * filter_compression;
      const float length_inc = sinv * hw_inv * filter_compression;

      __m128 c = _mm_set_ps(0, color[2], color[1], color[0]);
      __m128 c1 = _mm_sub_ps(_mm_set1_ps(1.0f), c);

      for(int x = 0; x < roi_out->width; x++, in += ch, out += ch)
      {
#if 1
        // !!! approximation is ok only when highest density is 8
        // for input x = (data->density * CLIP( 0.5+length ), calculate 2^x as (e^(ln2*x/8))^8
        // use exp2f approximation to calculate e^(ln2*x/8)
        // in worst case - density==8,CLIP(0.5-length) == 1.0 it gives 0.6% of error
        const float t = 0.693147181f /* ln2 */ * (data->density * CLIP(0.5f + length) / 8.0f);
        float d1 = t * t * 0.5f;
        float d2 = d1 * t * 0.333333333f;
        float d3 = d2 * t * 0.25f;
        float d = 1 + t + d1 + d2 + d3; /* taylor series for e^x till x^4 */
        // printf("%d %d  %f\n",y,x,d);
        __m128 density = _mm_set1_ps(d);
        density = _mm_mul_ps(density, density);
        density = _mm_mul_ps(density, density);
        density = _mm_mul_ps(density, density);
#else
        // use fair exp2f
        __m128 density = _mm_set1_ps(exp2f(data->density * CLIP(0.5f + length)));
#endif

        /* max(0,in / (c + (1-c)*density)) */
        _mm_stream_ps(out, _mm_max_ps(_mm_set1_ps(0.0f),
                                      _mm_div_ps(_mm_load_ps(in), _mm_add_ps(c, _mm_mul_ps(c1, density)))));

        length += length_inc;
      }
    }
  }
  else
  {
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(roi_out, color, data, ivoid, ovoid) schedule(static)
#endif
    for(int y = 0; y < roi_out->height; y++)
    {
      size_t k = (size_t)roi_out->width * y * ch;
      const float *in = (float *)ivoid + k;
      float *out = (float *)ovoid + k;

      float length = (sinv * (-1.0f + ix * hw_inv) - cosv * (-1.0f + (iy + y) * hh_inv) - 1.0f + offset)
                     * filter_compression;
      const float length_inc = sinv * hw_inv * filter_compression;

      __m128 c = _mm_set_ps(0, color[2], color[1], color[0]);
      __m128 c1 = _mm_sub_ps(_mm_set1_ps(1.0f), c);

      for(int x = 0; x < roi_out->width; x++, in += ch, out += ch)
      {
#if 1
        // !!! approximation is ok only when lowest density is -8
        // for input x = (-data->density * CLIP( 0.5-length ), calculate 2^x as (e^(ln2*x/8))^8
        // use exp2f approximation to calculate e^(ln2*x/8)
        // in worst case - density==-8,CLIP(0.5-length) == 1.0 it gives 0.6% of error
        const float t = 0.693147181f /* ln2 */ * (-data->density * CLIP(0.5f - length) / 8.0f);
        float d1 = t * t * 0.5f;
        float d2 = d1 * t * 0.333333333f;
        float d3 = d2 * t * 0.25f;
        float d = 1 + t + d1 + d2 + d3; /* taylor series for e^x till x^4 */
        __m128 density = _mm_set1_ps(d);
        density = _mm_mul_ps(density, density);
        density = _mm_mul_ps(density, density);
        density = _mm_mul_ps(density, density);
#else
        __m128 density = _mm_set1_ps(exp2f(-data->density * CLIP(0.5f - length)));
#endif

        /* max(0,in * (c + (1-c)*density)) */
        _mm_stream_ps(out, _mm_max_ps(_mm_set1_ps(0.0f),
                                      _mm_mul_ps(_mm_load_ps(in), _mm_add_ps(c, _mm_mul_ps(c1, density)))));

        length += length_inc;
      }
    }
  }
  _mm_sfence();

  if(piece->pipe->mask_display) dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);
}


#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_graduatednd_data_t *data = (dt_iop_graduatednd_data_t *)piece->data;
  dt_iop_graduatednd_global_data_t *gd = (dt_iop_graduatednd_global_data_t *)self->data;

  cl_int err = -999;
  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;

  const int ix = (roi_in->x);
  const int iy = (roi_in->y);
  const float iw = piece->buf_in.width * roi_out->scale;
  const float ih = piece->buf_in.height * roi_out->scale;
  const float hw = iw / 2.0;
  const float hh = ih / 2.0;
  const float hw_inv = 1.0 / hw;
  const float hh_inv = 1.0 / hh;
  const float v = (-data->rotation / 180) * M_PI;
  const float sinv = sin(v);
  const float cosv = cos(v);
  const float filter_radie = sqrt((hh * hh) + (hw * hw)) / hh;
  const float offset = data->offset / 100.0 * 2;
  const float density = data->density;

  float color[4] = { 0.0f };
  hsl2rgb(color, data->hue, data->saturation, 0.5);
  if(density < 0)
    for(int l = 0; l < 3; l++) color[l] = 1.0f - color[l];

#if 1
  const float filter_compression = 1.0 / filter_radie
                                   / (1.0 - (0.5 + (data->compression / 100.0) * 0.9 / 2.0)) * 0.5;
#else
  const float compression = data->compression / 100.0f;
  const float t = 1.0f - .8f / (.8f + compression);
  const float c = 1.0f + 1000.0f * powf(4.0, compression);
#endif

  const float length_base = (sinv * (-1.0 + ix * hw_inv) - cosv * (-1.0 + iy * hh_inv) - 1.0 + offset)
                            * filter_compression;
  const float length_inc_y = -cosv * hh_inv * filter_compression;
  const float length_inc_x = sinv * hw_inv * filter_compression;

  size_t sizes[] = { ROUNDUPWD(width), ROUNDUPHT(height), 1 };

  int kernel = density > 0 ? gd->kernel_graduatedndp : gd->kernel_graduatedndm;

  dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, kernel, 4, 4 * sizeof(float), (void *)color);
  dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(float), (void *)&density);
  dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(float), (void *)&length_base);
  dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(float), (void *)&length_inc_x);
  dt_opencl_set_kernel_arg(devid, kernel, 8, sizeof(float), (void *)&length_inc_y);
  err = dt_opencl_enqueue_kernel_2d(devid, kernel, sizes);
  if(err != CL_SUCCESS) goto error;
  return TRUE;

error:
  dt_print(DT_DEBUG_OPENCL, "[opencl_graduatednd] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif

void init_global(dt_iop_module_so_t *module)
{
  const int program = 8; // extended.cl, from programs.conf
  dt_iop_graduatednd_global_data_t *gd
      = (dt_iop_graduatednd_global_data_t *)malloc(sizeof(dt_iop_graduatednd_global_data_t));
  module->data = gd;
  gd->kernel_graduatedndp = dt_opencl_create_kernel(program, "graduatedndp");
  gd->kernel_graduatedndm = dt_opencl_create_kernel(program, "graduatedndm");
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_graduatednd_global_data_t *gd = (dt_iop_graduatednd_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_graduatedndp);
  dt_opencl_free_kernel(gd->kernel_graduatedndm);
  free(module->data);
  module->data = NULL;
}


static void density_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_graduatednd_params_t *p = (dt_iop_graduatednd_params_t *)self->params;
  p->density = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void compression_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_graduatednd_params_t *p = (dt_iop_graduatednd_params_t *)self->params;
  p->compression = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void rotation_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_graduatednd_params_t *p = (dt_iop_graduatednd_params_t *)self->params;
  dt_iop_graduatednd_gui_data_t *g = (dt_iop_graduatednd_gui_data_t *)self->gui_data;
  // float wd = self->dev->preview_pipe->backbuf_width;
  // float ht = self->dev->preview_pipe->backbuf_height;
  p->rotation = dt_bauhaus_slider_get(slider);
  set_points_from_grad(self, &g->xa, &g->ya, &g->xb, &g->yb, p->rotation, p->offset);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_graduatednd_params_t *p = (dt_iop_graduatednd_params_t *)p1;
#ifdef HAVE_GEGL
  fprintf(stderr, "[velvia] TODO: implement gegl version!\n");
// pull in new params to gegl
#else
  dt_iop_graduatednd_data_t *d = (dt_iop_graduatednd_data_t *)piece->data;
  d->density = p->density;
  d->compression = p->compression;
  d->rotation = p->rotation;
  d->offset = p->offset;
  d->hue = p->hue;
  d->saturation = p->saturation;
#endif
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
#ifdef HAVE_GEGL
  // create part of the gegl pipeline
  piece->data = NULL;
#else
  piece->data = calloc(1, sizeof(dt_iop_graduatednd_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
#endif
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
#ifdef HAVE_GEGL
  // clean up everything again.
  (void)gegl_node_remove_child(pipe->gegl, piece->input);
// no free necessary, no data is alloc'ed
#else
  free(piece->data);
  piece->data = NULL;
#endif
}

static inline void update_saturation_slider_end_color(GtkWidget *slider, float hue)
{
  float rgb[3];
  hsl2rgb(rgb, hue, 1.0, 0.5);
  dt_bauhaus_slider_set_stop(slider, 1.0, rgb[0], rgb[1], rgb[2]);
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_module_t *module = (dt_iop_module_t *)self;
  dt_iop_graduatednd_gui_data_t *g = (dt_iop_graduatednd_gui_data_t *)self->gui_data;
  dt_iop_graduatednd_params_t *p = (dt_iop_graduatednd_params_t *)module->params;
  dt_bauhaus_slider_set(g->scale1, p->density);
  dt_bauhaus_slider_set(g->scale2, p->compression);
  dt_bauhaus_slider_set(g->scale3, p->rotation);
  dt_bauhaus_slider_set(g->gslider1, p->hue);
  dt_bauhaus_slider_set(g->gslider2, p->saturation);

  // float wd = self->dev->preview_pipe->backbuf_width;
  // float ht = self->dev->preview_pipe->backbuf_height;
  g->define = 0;
  // set_points_from_grad(self,&g->xa,&g->ya,&g->xb,&g->yb,p->rotation,p->offset);
  update_saturation_slider_end_color(g->gslider2, p->hue);
}

void init(dt_iop_module_t *module)
{
  module->params = malloc(sizeof(dt_iop_graduatednd_params_t));
  module->default_params = malloc(sizeof(dt_iop_graduatednd_params_t));
  module->default_enabled = 0;
  module->priority = 283; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_graduatednd_params_t);
  module->gui_data = NULL;
  dt_iop_graduatednd_params_t tmp = (dt_iop_graduatednd_params_t){ 1.0, 0, 0, 50, 0, 0 };
  memcpy(module->params, &tmp, sizeof(dt_iop_graduatednd_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_graduatednd_params_t));
}

void cleanup(dt_iop_module_t *module)
{
  free(module->gui_data);
  module->gui_data = NULL;
  free(module->params);
  module->params = NULL;
}

static void hue_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_graduatednd_params_t *p = (dt_iop_graduatednd_params_t *)self->params;
  dt_iop_graduatednd_gui_data_t *g = (dt_iop_graduatednd_gui_data_t *)self->gui_data;

  const float hue = dt_bauhaus_slider_get(g->gslider1);
  // fprintf(stderr," hue: %f, saturation: %f\n",hue,dtgtk_gradient_slider_get_value(g->gslider2));

  update_saturation_slider_end_color(g->gslider2, hue);

  if(self->dt->gui->reset) return;
  gtk_widget_queue_draw(GTK_WIDGET(g->gslider2));

  p->hue = hue;
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void saturation_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_graduatednd_params_t *p = (dt_iop_graduatednd_params_t *)self->params;

  p->saturation = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}


void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_graduatednd_gui_data_t));
  dt_iop_graduatednd_gui_data_t *g = (dt_iop_graduatednd_gui_data_t *)self->gui_data;
  dt_iop_graduatednd_params_t *p = (dt_iop_graduatednd_params_t *)self->params;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  /* density */
  g->scale1 = dt_bauhaus_slider_new_with_range(self, -8.0, 8.0, 0.1, p->density, 2);
  dt_bauhaus_slider_set_format(g->scale1, "%.2fev");
  dt_bauhaus_widget_set_label(g->scale1, NULL, _("density"));
  g_object_set(G_OBJECT(g->scale1), "tooltip-text", _("the density in EV for the filter"), (char *)NULL);
  g_signal_connect(G_OBJECT(g->scale1), "value-changed", G_CALLBACK(density_callback), self);

  /* compression */
  g->scale2 = dt_bauhaus_slider_new_with_range(self, 0.0, 100.0, 1.0, p->compression, 0);
  dt_bauhaus_slider_set_format(g->scale2, "%.0f%%");
  dt_bauhaus_widget_set_label(g->scale2, NULL, _("compression"));
  /* xgettext:no-c-format */
  g_object_set(G_OBJECT(g->scale2), "tooltip-text", _("compression of graduation:\n0% = soft, 100% = hard"),
               (char *)NULL);
  g_signal_connect(G_OBJECT(g->scale2), "value-changed", G_CALLBACK(compression_callback), self);

  /* rotation */
  g->scale3 = dt_bauhaus_slider_new_with_range(self, -180, 180, 0.5, p->rotation, 2);
  dt_bauhaus_widget_set_label(g->scale3, NULL, _("rotation"));
  dt_bauhaus_slider_set_format(g->scale3, "%.2f°");
  g_object_set(G_OBJECT(g->scale3), "tooltip-text", _("rotation of filter -180 to 180 degrees"), (char *)NULL);
  g_signal_connect(G_OBJECT(g->scale3), "value-changed", G_CALLBACK(rotation_callback), self);

  /* add widgets to ui */
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->scale1), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->scale2), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->scale3), TRUE, TRUE, 0);

  /* hue slider */
  g->gslider1 = dt_bauhaus_slider_new_with_range_and_feedback(self, 0.0f, 1.0f, 0.01f, 0.0f, 2, 0);
  dt_bauhaus_slider_set_stop(g->gslider1, 0.0f, 1.0f, 0.0f, 0.0f);
  // dt_bauhaus_slider_set_format(g->gslider1, "");
  dt_bauhaus_widget_set_label(g->gslider1, NULL, _("hue"));
  dt_bauhaus_slider_set_stop(g->gslider1, 0.166f, 1.0f, 1.0f, 0.0f);
  dt_bauhaus_slider_set_stop(g->gslider1, 0.322f, 0.0f, 1.0f, 0.0f);
  dt_bauhaus_slider_set_stop(g->gslider1, 0.498f, 0.0f, 1.0f, 1.0f);
  dt_bauhaus_slider_set_stop(g->gslider1, 0.664f, 0.0f, 0.0f, 1.0f);
  dt_bauhaus_slider_set_stop(g->gslider1, 0.830f, 1.0f, 0.0f, 1.0f);
  dt_bauhaus_slider_set_stop(g->gslider1, 1.0f, 1.0f, 0.0f, 0.0f);
  g_object_set(G_OBJECT(g->gslider1), "tooltip-text", _("select the hue tone of filter"), (char *)NULL);
  g_signal_connect(G_OBJECT(g->gslider1), "value-changed", G_CALLBACK(hue_callback), self);

  gtk_box_pack_start(GTK_BOX(self->widget), g->gslider1, TRUE, TRUE, 0);

  /* saturation slider */
  g->gslider2 = dt_bauhaus_slider_new_with_range(self, 0.0f, 1.0f, 0.01f, 0.0f, 2);
  dt_bauhaus_widget_set_label(g->gslider2, NULL, _("saturation"));
  dt_bauhaus_slider_set_stop(g->gslider2, 0.0f, 0.2f, 0.2f, 0.2f);
  dt_bauhaus_slider_set_stop(g->gslider2, 1.0f, 1.0f, 1.0f, 1.0f);
  g_object_set(G_OBJECT(g->gslider2), "tooltip-text", _("select the saturation of filter"), (char *)NULL);
  g_signal_connect(G_OBJECT(g->gslider2), "value-changed", G_CALLBACK(saturation_callback), self);

  gtk_box_pack_start(GTK_BOX(self->widget), g->gslider2, TRUE, TRUE, 0);
  g->selected = 0;
  g->dragging = 0;
  g->define = 0;
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
