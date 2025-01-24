/*
    This file is part of darktable,
    Copyright (C) 2010-2021 darktable developers.

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
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "bauhaus/bauhaus.h"
#include "common/colorspaces.h"
#include "common/debug.h"
#include "common/math.h"
#include "common/opencl.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "develop/imageop_gui.h"
#include "develop/tiling.h"
#include "dtgtk/gradientslider.h"
#include "gui/accelerators.h"
#include "gui/color_picker_proxy.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "iop/iop_api.h"

DT_MODULE_INTROSPECTION(1, dt_iop_graduatednd_params_t)

typedef struct dt_iop_graduatednd_params_t
{
  float density;     // $MIN: -8.0 $MAX: 8.0 $DEFAULT: 1.0 $DESCRIPTION: "density" The density of filter 0-8 EV
  float hardness;    // $MIN: 0.0 $MAX: 100.0 $DEFAULT: 0.0 $DESCRIPTION: "hardness" 0% = soft and 100% = hard
  float rotation;    // $MIN: -180.0 $MAX: 180.0 $DEFAULT: 0.0 $DESCRIPTION: "rotation" 2*PI -180 - +180
  float offset;      // $DEFAULT: 50.0 $DESCRIPTION: "offset" centered, can be offsetted...
  float hue;         // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "hue"
  float saturation;  // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "saturation"
} dt_iop_graduatednd_params_t;

typedef struct dt_iop_graduatednd_global_data_t
{
  int kernel_graduatedndp;
  int kernel_graduatedndm;
} dt_iop_graduatednd_global_data_t;


void init_presets(dt_iop_module_so_t *self)
{
  dt_database_start_transaction(darktable.db);

  dt_gui_presets_add_generic(_("neutral gray ND2 (soft)"), self->op, self->version(),
                             &(dt_iop_graduatednd_params_t){ 1, 0, 0, 50, 0, 0 },
                             sizeof(dt_iop_graduatednd_params_t), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);
  dt_gui_presets_add_generic(_("neutral gray ND4 (soft)"), self->op, self->version(),
                             &(dt_iop_graduatednd_params_t){ 2, 0, 0, 50, 0, 0 },
                             sizeof(dt_iop_graduatednd_params_t), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);
  dt_gui_presets_add_generic(_("neutral gray ND8 (soft)"), self->op, self->version(),
                             &(dt_iop_graduatednd_params_t){ 3, 0, 0, 50, 0, 0 },
                             sizeof(dt_iop_graduatednd_params_t), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);
  dt_gui_presets_add_generic(_("neutral gray ND2 (hard)"), self->op, self->version(),
                             &(dt_iop_graduatednd_params_t){ 1, 75, 0, 50, 0, 0 },
                             sizeof(dt_iop_graduatednd_params_t), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);

  dt_gui_presets_add_generic(_("neutral gray ND4 (hard)"), self->op, self->version(),
                             &(dt_iop_graduatednd_params_t){ 2, 75, 0, 50, 0, 0 },
                             sizeof(dt_iop_graduatednd_params_t), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);
  dt_gui_presets_add_generic(_("neutral gray ND8 (hard)"), self->op, self->version(),
                             &(dt_iop_graduatednd_params_t){ 3, 75, 0, 50, 0, 0 },
                             sizeof(dt_iop_graduatednd_params_t), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);

  dt_gui_presets_add_generic(_("orange ND2 (soft)"), self->op, self->version(),
                             &(dt_iop_graduatednd_params_t){ 1, 0, 0, 50, 0.102439, 0.8 },
                             sizeof(dt_iop_graduatednd_params_t), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);
  dt_gui_presets_add_generic(_("yellow ND2 (soft)"), self->op, self->version(),
                             &(dt_iop_graduatednd_params_t){ 1, 0, 0, 50, 0.151220, 0.5 },
                             sizeof(dt_iop_graduatednd_params_t), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);

  dt_gui_presets_add_generic(_("purple ND2 (soft)"), self->op, self->version(),
                             &(dt_iop_graduatednd_params_t){ 1, 0, 0, 50, 0.824390, 0.5 },
                             sizeof(dt_iop_graduatednd_params_t), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);

  dt_gui_presets_add_generic(_("green ND2 (soft)"), self->op, self->version(),
                             &(dt_iop_graduatednd_params_t){ 1, 0, 0, 50, 0.302439, 0.5 },
                             sizeof(dt_iop_graduatednd_params_t), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);

  dt_gui_presets_add_generic(_("red ND2 (soft)"), self->op, self->version(),
                             &(dt_iop_graduatednd_params_t){ 1, 0, 0, 50, 0, 0.5 },
                             sizeof(dt_iop_graduatednd_params_t), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);
  dt_gui_presets_add_generic(_("blue ND2 (soft)"), self->op, self->version(),
                             &(dt_iop_graduatednd_params_t){ 1, 0, 0, 50, 0.663415, 0.5 },
                             sizeof(dt_iop_graduatednd_params_t), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);

  dt_gui_presets_add_generic(_("brown ND4 (soft)"), self->op, self->version(),
                             &(dt_iop_graduatednd_params_t){ 2, 0, 0, 50, 0.082927, 0.25 },
                             sizeof(dt_iop_graduatednd_params_t), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);

  dt_database_release_transaction(darktable.db);
}

typedef struct dt_iop_graduatednd_gui_data_t
{
  GtkWidget *density, *hardness, *rotation, *hue, *saturation;

  int selected;
  int dragging;

  gboolean define;
  float xa, ya, xb, yb, oldx, oldy;
} dt_iop_graduatednd_gui_data_t;

typedef struct dt_iop_graduatednd_data_t
{
  float density;     // The density of filter 0-8 EV
  float hardness; // Default 0% = soft and 100% = hard
  float rotation;    // 2*PI -180 - +180
  float offset;      // Default 50%, centered, can be offsetted...
  float color[4];    // RGB color of gradient
  float color1[4];   // inverted color (1 - c)
} dt_iop_graduatednd_data_t;


const char *name()
{
  return _("graduated density");
}

const char **description(dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("simulate an optical graduated neutral density filter"),
                                      _("corrective and creative"),
                                      _("linear or non-linear, RGB, scene-referred"),
                                      _("non-linear, RGB"),
                                      _("non-linear, RGB, display-referred"));
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING
         | IOP_FLAGS_TILING_FULL_ROI;
}

int default_group()
{
  return IOP_GROUP_EFFECT | IOP_GROUP_GRADING;
}

dt_iop_colorspace_type_t default_colorspace(dt_iop_module_t *self,
                                            dt_dev_pixelpipe_t *pipe,
                                            dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
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
static float _dist_seg(
  	float xa,
        float ya,
        float xb,
        float yb,
        float xc,
        float yc)
{
  if(xa == xb && ya == yb) return (xc - xa) * (xc - xa) + (yc - ya) * (yc - ya);

  const float sx = xb - xa;
  const float sy = yb - ya;

  const float ux = xc - xa;
  const float uy = yc - ya;

  const float dp = sx * ux + sy * uy;
  if(dp < 0) return (xc - xa) * (xc - xa) + (yc - ya) * (yc - ya);

  const float sn2 = sx * sx + sy * sy;
  if(dp > sn2) return (xc - xb) * (xc - xb) + (yc - yb) * (yc - yb);

  const float ah2 = dp * dp / sn2;
  const float un2 = ux * ux + uy * uy;
  return un2 - ah2;
}

static int _set_grad_from_points(
	dt_iop_module_t *self,
        float xa,
        float ya,
        float xb,
        float yb,
        float *rotation,
        float *offset)
{
  // we want absolute positions
  float wd, ht;
  dt_dev_get_preview_size(self->dev, &wd, &ht);
  float pts[4]
      = { xa * wd, ya * ht,
          xb * wd, yb * ht };
  dt_dev_distort_backtransform_plus(self->dev, self->dev->preview_pipe, self->iop_order, DT_DEV_TRANSFORM_DIR_FORW_EXCL, pts, 2);
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
  const float pas = M_PI / 16.0;

  do
  {
    v2 += pas;
    sinv = sinf(v2), cosv = cosf(v2);
    r2 = pts[1] * cosv - pts[0] * sinv + pts[2] * sinv - pts[3] * cosv;
    if(r1 * r2 < 0) break;
  } while(v2 <= M_PI);

  if(v2 == (float)M_PI) return 9;

  // set precision for the iterative check
  const float eps = .0001f;

  int iter = 0;
  do
  {
    v = (v1 + v2) / 2.0;
    sinv = sinf(v), cosv = cosf(v);
    r = pts[1] * cosv - pts[0] * sinv + pts[2] * sinv - pts[3] * cosv;

    if(r < eps && r > -eps) break;

    if(r * r2 < 0)
      v1 = v;
    else
    {
      r2 = r;
      v2 = v;
    }

  } while(iter++ < 1000);

  if(iter >= 1000) return 8; // generally in less than 20 iterations all is good, so we are over conservative

  // be careful to the gnd direction

  const float diff_x = pts[2] - pts[0];
  const float MPI2 = (M_PI / 2.0f);

  if(diff_x > eps)
  {
    if(v >=  MPI2) v -= M_PI;
    if(v <  -MPI2) v += M_PI;
  }
  else if(diff_x < -eps)
  {
    if(v <  MPI2 && v >= 0) v -= M_PI;
    if(v > -MPI2 && v < 0)  v += M_PI;
  }
  else // let's pretend that we are at PI/2
  {
    const float diff_y = pts[3] - pts[1];
    if(diff_y <= 0.0f) v = -MPI2;
    else               v = MPI2;
  }

  *rotation = -v * 180.0f / M_PI;

  // and now we go for the offset (more easy)
  sinv = sinf(v);
  cosv = cosf(v);
  const float ofs = (-2.0f * sinv * pts[0]) + sinv - cosv + 1.0f + (2.0f * cosv * pts[1]);

  *offset = ofs * 50.0f;

  return 1;
}

static int _set_points_from_grad(
	dt_iop_module_t *self,
        float *xa,
        float *ya,
        float *xb,
        float *yb,
        float rotation,
        float offset)
{
  // we get the extremities of the line
  const float v = (-rotation / 180) * M_PI;
  const float sinv = sinf(v);
  dt_boundingbox_t pts;

  dt_dev_pixelpipe_iop_t *piece = dt_dev_distort_get_iop_pipe(self->dev, self->dev->preview_pipe, self);
  if(!piece) return 0;
  float wp = piece->buf_out.width, hp = piece->buf_out.height;

  // if sinv=0 then this is just the offset

  if(sinv == 0.0f) // horizontal
  {
    if(rotation == 0.0f)
    {
      pts[0] = wp * 0.1f;
      pts[2] = wp * 0.9f;
      pts[1] = pts[3] = hp * offset / 100.0f;
    }
    else
    {
      pts[2] = wp * 0.1f;
      pts[0] = wp * 0.9f;
      pts[1] = pts[3] = hp * (1.0f - offset / 100.0f);
    }
  }
  else if(fabsf(sinv) == 1) // vertical
  {
    if(rotation == 90)
    {
      pts[0] = pts[2] = wp * offset / 100.0f;
      pts[3] = hp * 0.1f;
      pts[1] = hp * 0.9f;
    }
    else
    {
      pts[0] = pts[2] = wp * (1.0 - offset / 100.0f);
      pts[1] = hp * 0.1f;
      pts[3] = hp * 0.9f;
    }
  }
  else
  {
    // otherwise we determine the extremities
    const float cosv = cosf(v);
    float xx1 = (sinv - cosv + 1.0f - offset / 50.0f) * wp * 0.5f / sinv;
    float xx2 = (sinv + cosv + 1.0f - offset / 50.0f) * wp * 0.5f / sinv;
    float yy1 = 0.0f;
    float yy2 = hp;
    const float a = hp / (xx2 - xx1);
    const float b = -xx1 * a;

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

    if(rotation < 90.0f && rotation > -90.0f)
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

  if(!dt_dev_distort_transform_plus(self->dev, self->dev->preview_pipe, self->iop_order, DT_DEV_TRANSFORM_DIR_FORW_EXCL, pts, 2))
    return 0;
  float wd, ht;
  dt_dev_get_preview_size(self->dev, &wd, &ht);
  *xa = pts[0] / wd;
  *ya = pts[1] / ht;
  *xb = pts[2] / wd;
  *yb = pts[3] / ht;
  return 1;
}

static inline void _update_saturation_slider_end_color(GtkWidget *slider, float hue)
{
  dt_aligned_pixel_t rgb;
  hsl2rgb(rgb, hue, 1.0, 0.5);
  dt_bauhaus_slider_set_stop(slider, 1.0, rgb[0], rgb[1], rgb[2]);
}

void color_picker_apply(dt_iop_module_t *self, GtkWidget *picker,
                        dt_dev_pixelpipe_t *pipe)
{
  dt_iop_graduatednd_gui_data_t *g = self->gui_data;
  dt_iop_graduatednd_params_t *p = self->params;

  // convert picker RGB 2 HSL
  float H = .0f, S = .0f, L = .0f;
  rgb2hsl(self->picked_color, &H, &S, &L);

  if(fabsf(p->hue - H) < 0.0001f && fabsf(p->saturation - S) < 0.0001f)
  {
    // interrupt infinite loops
    return;
  }

  p->hue        = H;
  p->saturation = S;

  ++darktable.gui->reset;
  dt_bauhaus_slider_set(g->hue, p->hue);
  dt_bauhaus_slider_set(g->saturation, p->saturation);
  _update_saturation_slider_end_color(g->saturation, p->hue);
  --darktable.gui->reset;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

void gui_reset(dt_iop_module_t *self)
{
  dt_iop_color_picker_reset(self, TRUE);
}

void gui_post_expose(dt_iop_module_t *self,
                     cairo_t *cr,
                     const float wd,
                     const float ht,
                     const float pointerx,
                     const float pointery,
                     const float zoom_scale)
{
  dt_iop_graduatednd_gui_data_t *g = self->gui_data;
  dt_iop_graduatednd_params_t *p = self->params;

  // we get the extremities of the line
  if(g->define == 0)
  {
    if(!_set_points_from_grad(self, &g->xa, &g->ya, &g->xb, &g->yb, p->rotation, p->offset)) return;
    g->define = 1;
  }

  const float xa = g->xa * wd, xb = g->xb * wd, ya = g->ya * ht, yb = g->yb * ht;
  // the lines
  const double lwidth = (dt_iop_canvas_not_sensitive(darktable.develop) ? 0.5 : 1.0) / zoom_scale;
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
  if(g->selected == 3 || g->dragging == 3)
    cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(5.0) * lwidth);
  else
    cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(3.0) * lwidth);
  dt_draw_set_color_overlay(cr, FALSE, 0.8);

  cairo_move_to(cr, xa, ya);
  cairo_line_to(cr, xb, yb);
  cairo_stroke(cr);

  if(g->selected == 3 || g->dragging == 3)
    cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(2.0) * lwidth);
  else
    cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.0) * lwidth);
  dt_draw_set_color_overlay(cr, TRUE, 0.8);
  cairo_move_to(cr, xa, ya);
  cairo_line_to(cr, xb, yb);
  cairo_stroke(cr);

  if(dt_iop_canvas_not_sensitive(darktable.develop))
    return;
  // the extremities
  float x1, y1, x2, y2;
  const float l = sqrtf((xb - xa) * (xb - xa) + (yb - ya) * (yb - ya));
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
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.0) * lwidth);
  if(g->selected == 1 || g->dragging == 1)
    dt_draw_set_color_overlay(cr, TRUE, 1.0);
  else
    dt_draw_set_color_overlay(cr, TRUE, 0.5);
  cairo_fill_preserve(cr);
  if(g->selected == 1 || g->dragging == 1)
    dt_draw_set_color_overlay(cr, FALSE, 1.0);
  else
    dt_draw_set_color_overlay(cr, FALSE, 0.5);
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
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.0) * lwidth);
  if(g->selected == 2 || g->dragging == 2)
    dt_draw_set_color_overlay(cr, TRUE, 1.0);
  else
    dt_draw_set_color_overlay(cr, TRUE, 0.5);
  cairo_fill_preserve(cr);
  if(g->selected == 2 || g->dragging == 2)
    dt_draw_set_color_overlay(cr, FALSE, 1.0);
  else
    dt_draw_set_color_overlay(cr, FALSE, 0.5);
  cairo_stroke(cr);
}

int mouse_moved(dt_iop_module_t *self,
                const float pzx,
                const float pzy,
                const double pressure,
                const int which,
                const float zoom_scale)
{
  dt_iop_graduatednd_gui_data_t *g = self->gui_data;
  gboolean handled = FALSE;

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
    handled = TRUE;
  }
  else
  {
    g->selected = 0;
    const float ext = DT_PIXEL_APPLY_DPI(0.02f) / zoom_scale;
    // are we near extremity ?
    if(pzy > g->ya - ext && pzy < g->ya + ext && pzx > g->xa - ext && pzx < g->xa + ext)
    {
      g->selected = 1;
    }
    else if(pzy > g->yb - ext && pzy < g->yb + ext && pzx > g->xb - ext && pzx < g->xb + ext)
    {
      g->selected = 2;
    }
    else if(_dist_seg(g->xa, g->ya, g->xb, g->yb, pzx, pzy) < ext * ext * 0.5)
      g->selected = 3;
  }

  dt_control_queue_redraw_center();
  return handled;
}

int button_pressed(dt_iop_module_t *self,
                   const float pzx,
                   const float pzy,
                   const double pressure,
                   const int which,
                   const int type,
                   const uint32_t state,
                   const float zoom_scale)
{
  dt_iop_graduatednd_gui_data_t *g = self->gui_data;

  if(which == 3)
  {
    // creating a line with right click
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

int button_released(dt_iop_module_t *self,
                    const float pzx,
                    const float pzy,
                    const int which,
                    const uint32_t state,
                    const float zoom_scale)
{
  dt_iop_graduatednd_gui_data_t *g = self->gui_data;
  dt_iop_graduatednd_params_t *p = self->params;
  if(g->dragging > 0)
  {
    float r = 0.0, o = 0.0;
    _set_grad_from_points(self, g->xa, g->ya, g->xb, g->yb, &r, &o);

    // if this is a "line dragging, we reset extremities, to be sure they are not outside the image
    if(g->dragging == 3)
    {
      /*
       * whole line dragging should not change rotation, so we should reuse
       * old rotation to avoid rounding issues
       */

      r = p->rotation;
      _set_points_from_grad(self, &g->xa, &g->ya, &g->xb, &g->yb, r, o);
    }
    ++darktable.gui->reset;
    dt_bauhaus_slider_set(g->rotation, r);
    --darktable.gui->reset;
    p->rotation = r;
    p->offset = o;
    g->dragging = 0;
    dt_dev_add_history_item(darktable.develop, self, TRUE);
  }

  g->dragging = 0;
  return 0;
}

int scrolled(
	dt_iop_module_t *self,
        float x,
        float y,
        int up,
        uint32_t state)
{
  dt_iop_graduatednd_gui_data_t *g = self->gui_data;
  dt_iop_graduatednd_params_t *p = self->params;
  if(dt_modifier_is(state, GDK_CONTROL_MASK))
  {
    float dens;
    if(up)
      dens = fminf(8.0, p->density + 0.1);
    else
      dens = fmaxf(-8.0, p->density - 0.1);
    if(dens != p->density)
    {
      dt_bauhaus_slider_set(g->density, dens);
    }
    return 1;
  }
  if(dt_modifier_is(state, GDK_SHIFT_MASK))
  {
    float comp;
    if(up)
      comp = fminf(100.0, p->hardness + 1.0);
    else
      comp = fmaxf(0.0, p->hardness - 1.0);
    if(comp != p->hardness)
    {
      dt_bauhaus_slider_set(g->hardness, comp);
    }
    return 1;
  }
  return 0;
}

DT_OMP_DECLARE_SIMD(simdlen(4))
static inline float _density_times_length(const float dens, const float length)
{
  return (dens * CLAMP(0.5f + length, 0.0f, 1.0f) / 8.0f);
}

DT_OMP_DECLARE_SIMD(simdlen(4))
static inline float _compute_density(const float dens, const float length)
{
#if 1
  // !!! approximation is ok only when highest density is 8
  // for input x = (data->density * CLIP( 0.5+length ), calculate 2^x as (e^(ln2*x/8))^8
  // use exp2f approximation to calculate e^(ln2*x/8)
  // in worst case - density==8,CLIP(0.5-length) == 1.0 it gives 0.6% of error
  const float t = DT_M_LN2f * _density_times_length(dens,length);
  const float d1 = t * t * 0.5f;
  const float d2 = d1 * t * 0.333333333f;
  const float d3 = d2 * t * 0.25f;
  const float d = 1 + t + d1 + d2 + d3; /* taylor series for e^x till x^4 */
  float density = d * d;
  density = density * density;
  density = density * density;
#else
  // use fair exp2f
  // for GCC10 on recent hardware, exp2f is actually faster than the above approximation,
  // but it does not vectorize so it is slower overall
  const float density = exp2f(dens * CLIP(0.5f + length));
#endif
  return density;
}

void process(dt_iop_module_t *self,
             dt_dev_pixelpipe_iop_t *piece,
             const void *const ivoid,
             void *const ovoid,
             const dt_iop_roi_t *const roi_in,
             const dt_iop_roi_t *const roi_out)
{
  if(!dt_iop_have_required_input_format(4 /*we need full-color pixels*/, self, piece->colors,
                                         ivoid, ovoid, roi_in, roi_out))
    return;	// input buffer has been copied to output unchanged and the trouble flag set

  const dt_iop_graduatednd_data_t *const data = (const dt_iop_graduatednd_data_t *const)piece->data;
  const int ix = (roi_in->x);
  const int iy = (roi_in->y);
  const float iw = piece->buf_in.width * roi_out->scale;
  const float ih = piece->buf_in.height * roi_out->scale;
  const float hw = iw / 2.0f;
  const float hh = ih / 2.0f;
  const float hw_inv = 1.0f / hw;
  const float hh_inv = 1.0f / hh;
  const float v = (-data->rotation / 180) * M_PI;
  const float sinv = sinf(v);
  const float cosv = cosf(v);
  const float cosv_hh_inv = cosv * hh_inv;
  const float filter_radie = sqrtf((hh * hh) + (hw * hw)) / hh;
  const float offset = data->offset / 100.0f * 2;

  const float filter_hardness = (1.0f / filter_radie)
                                / (1.0f - (0.5f + (data->hardness / 100.0f) * 0.9f / 2.0f)) * 0.5f;

  const int width = roi_out->width;
  const int height = roi_out->height;
  const float length_base = sinv * (-1.0f + ix * hw_inv) + cosv - 1.0f + offset;
  const float length_inc = sinv * hw_inv * filter_hardness;
  const float density = data->density;
  // preload data into aligned storage; the compiler will optimize
  // these into registers when it vectorizes
  const dt_aligned_pixel_t color = { data->color[0], data->color[1], data->color[2], data->color[3] };
  const dt_aligned_pixel_t color1 = { data->color1[0], data->color1[1], data->color1[2], data->color1[3] };
  const dt_aligned_pixel_t zero = { 0.0f, 0.0f, 0.0f, 0.0f };

  if(density > 0)
  {
    DT_OMP_FOR()
    for(int y = 0; y < height; y++)
    {
      const size_t k = (size_t)4 * width * y;
      const float *const restrict in = (float *)ivoid + k;
      float *const restrict out = (float *)ovoid + k;

      float length = (length_base - (iy + y) * cosv_hh_inv) * filter_hardness;
      const dt_aligned_pixel_t counts = { 0.0f, 1.0f, 2.0f, 3.0f };

      // process pixels four at a time so that the compiler can
      // vectorize the density computation
      for(int x = 0; x+3 < width; x += 4)
      {
        dt_aligned_pixel_t curr_density;
        dt_aligned_pixel_t lengths;
        for_four_channels(i)
        {
          lengths[i] = length + counts[i] * length_inc;
          curr_density[i] = _compute_density(density, lengths[i]);
        }
        for(int i = 0; i < 4; i++)
        {
          dt_aligned_pixel_t res;	// the compiler will optimize this into a register
          for_each_channel(l, aligned(in : 16))
          {
            res[l] = MAX(zero[l], (in[4*(x+i)+l] / (color[l] + color1[l] * curr_density[i])));
          }
          // use streaming writes to eliminate the memory reads from loading cache lines
          copy_pixel_nontemporal(out + 4*(x+i), res);
        }
        length += 4 * length_inc;
      }
      // handle the left-over pixels
      for(int x = width & ~3; x < width; x++)
      {
        const float curr_density = _compute_density(density, length);
        dt_aligned_pixel_t res;	// the compiler will optimize this into a register
        for_each_channel(l, aligned(in : 16))
        {
          res[l] = MAX(zero[l], (in[4*x+l] / (color[l] + color1[l] * curr_density)));
        }
        // use streaming writes to eliminate the memory reads from loading cache lines
        copy_pixel_nontemporal(out + 4*x, res);
        length += length_inc;
      }
    }
    // ensure that the nontemporal writes have finished before continuing
    dt_omploop_sfence();
  }
  else
  {
    DT_OMP_FOR()
    for(int y = 0; y < height; y++)
    {
      const size_t k = (size_t)4 * width * y;
      const float *const restrict in = (float *)ivoid + k;
      float *const restrict out = (float *)ovoid + k;
      float length = (length_base - (iy + y) * cosv_hh_inv) * filter_hardness;
      const dt_aligned_pixel_t counts = { 0.0f, 1.0f, 2.0f, 3.0f };

      // process pixels four at a time so that the compiler can
      // vectorize the density computation
      for(int x = 0; x+3 < width; x += 4)
      {
        dt_aligned_pixel_t curr_density;
        dt_aligned_pixel_t lengths;
        for_four_channels(i)
        {
          lengths[i] = -(length + counts[i] * length_inc);
          curr_density[i] = _compute_density(-density, lengths[i]);
        }
        for(int i = 0; i < 4; i++)
        {
          dt_aligned_pixel_t res;	// the compiler will optimize this into a register
          for_each_channel(l, aligned(in : 16))
          {
            res[l] = MAX(zero[l], (in[4*(x+i)+l] * (color[l] + color1[l] * curr_density[i])));
          }
          // use streaming writes to eliminate the memory reads from loading cache lines
          copy_pixel_nontemporal(out + 4*(x+i), res);
        }
        length += 4*length_inc;
      }
      // handle the left-over pixels
      for(int x = width & ~3; x < width; x++)
      {
        const float curr_density = _compute_density(-density, -length);
        dt_aligned_pixel_t res;	// the compiler will optimize this into a register
        for_each_channel(l, aligned(in : 16))
        {
          res[l] = MAX(zero[l], (in[4*x+l] * (color[l] + color1[l] * curr_density)));
        }
        // use streaming writes to eliminate the memory reads from loading cache lines
        copy_pixel_nontemporal(out + 4*x, res);
        length += length_inc;
      }
    }
    // ensure that the nontemporal writes have finished before continuing
    dt_omploop_sfence();
  }

  if(piece->pipe->mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK)
    dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);
}

#ifdef HAVE_OPENCL
int process_cl(dt_iop_module_t *self,
               dt_dev_pixelpipe_iop_t *piece,
               cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in,
               const dt_iop_roi_t *const roi_out)
{
  dt_iop_graduatednd_data_t *data = piece->data;
  dt_iop_graduatednd_global_data_t *gd = self->global_data;

  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;

  const int ix = (roi_in->x);
  const int iy = (roi_in->y);
  const float iw = piece->buf_in.width * roi_out->scale;
  const float ih = piece->buf_in.height * roi_out->scale;
  const float hw = iw / 2.0f;
  const float hh = ih / 2.0f;
  const float hw_inv = 1.0f / hw;
  const float hh_inv = 1.0f / hh;
  const float v = (-data->rotation / 180) * M_PI;
  const float sinv = sinf(v);
  const float cosv = cosf(v);
  const float filter_radie = sqrtf((hh * hh) + (hw * hw)) / hh;
  const float offset = data->offset / 100.0f * 2;
  const float density = data->density;

#if 1
  const float filter_hardness = 1.0 / filter_radie
                                   / (1.0 - (0.5 + (data->hardness / 100.0) * 0.9 / 2.0)) * 0.5;
#else
  const float hardness = data->hardness / 100.0f;
  const float t = 1.0f - .8f / (.8f + hardness);
  const float c = 1.0f + 1000.0f * powf(4.0, hardness);
#endif

  const float length_base = (sinv * (-1.0 + ix * hw_inv) - cosv * (-1.0 + iy * hh_inv) - 1.0 + offset)
                            * filter_hardness;
  const float length_inc_y = -cosv * hh_inv * filter_hardness;
  const float length_inc_x = sinv * hw_inv * filter_hardness;


  int kernel = density > 0 ? gd->kernel_graduatedndp : gd->kernel_graduatedndm;

  return dt_opencl_enqueue_kernel_2d_args(devid, kernel, width, height,
    CLARG(dev_in), CLARG(dev_out), CLARG(width), CLARG(height), CLARRAY(4, data->color), CLARG(density),
    CLARG(length_base), CLARG(length_inc_x), CLARG(length_inc_y));
}
#endif

void init_global(dt_iop_module_so_t *self)
{
  const int program = 8; // extended.cl, from programs.conf
  dt_iop_graduatednd_global_data_t *gd = malloc(sizeof(dt_iop_graduatednd_global_data_t));
  self->data = gd;
  gd->kernel_graduatedndp = dt_opencl_create_kernel(program, "graduatedndp");
  gd->kernel_graduatedndm = dt_opencl_create_kernel(program, "graduatedndm");
}

void cleanup_global(dt_iop_module_so_t *self)
{
  dt_iop_graduatednd_global_data_t *gd = self->data;
  dt_opencl_free_kernel(gd->kernel_graduatedndp);
  dt_opencl_free_kernel(gd->kernel_graduatedndm);
  free(self->data);
  self->data = NULL;
}

void gui_changed(dt_iop_module_t *self, GtkWidget *w, void *previous)
{
  dt_iop_graduatednd_params_t *p = self->params;
  dt_iop_graduatednd_gui_data_t *g = self->gui_data;
  if(w == g->rotation)
  {
    _set_points_from_grad(self, &g->xa, &g->ya, &g->xb, &g->yb, p->rotation, p->offset);
  }
  else if(w == g->hue)
  {
    _update_saturation_slider_end_color(g->saturation, p->hue);
    gtk_widget_queue_draw(g->saturation);
  }
}

void commit_params(dt_iop_module_t *self,
                   dt_iop_params_t *p1,
                   dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_graduatednd_params_t *p = (dt_iop_graduatednd_params_t *)p1;
  dt_iop_graduatednd_data_t *d = piece->data;

  d->density = p->density;
  d->hardness = p->hardness;
  d->rotation = p->rotation;
  d->offset = p->offset;

  hsl2rgb(d->color, p->hue, p->saturation, 0.5);
  d->color[3] = 0.0f;

  if(d->density < 0)
    for(int l = 0; l < 4; l++) d->color[l] = 1.0 - d->color[l];

  for(int l = 0; l < 4; l++) d->color1[l] = 1.0 - d->color[l];
}

void init_pipe(dt_iop_module_t *self,
               dt_dev_pixelpipe_t *pipe,
               dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1, sizeof(dt_iop_graduatednd_data_t));
}

void cleanup_pipe(dt_iop_module_t *self,
                  dt_dev_pixelpipe_t *pipe,
                  dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void gui_update(dt_iop_module_t *self)
{
  dt_iop_graduatednd_gui_data_t *g = self->gui_data;
  dt_iop_graduatednd_params_t *p = self->params;

  dt_iop_color_picker_reset(self, TRUE);

  g->define = 0;
  _update_saturation_slider_end_color(g->saturation, p->hue);
}

void gui_init(dt_iop_module_t *self)
{
  dt_iop_graduatednd_gui_data_t *g = IOP_GUI_ALLOC(graduatednd);

  g->density = dt_bauhaus_slider_from_params(self, "density");
  dt_bauhaus_slider_set_format(g->density, _(" EV"));
  gtk_widget_set_tooltip_text(g->density, _("the density in EV for the filter"));

  g->hardness = dt_bauhaus_slider_from_params(self, "hardness");
  dt_bauhaus_slider_set_format(g->hardness, "%");
  /* xgettext:no-c-format */
  gtk_widget_set_tooltip_text(g->hardness, _("hardness of graduation:\n0% = soft, 100% = hard"));

  g->rotation = dt_bauhaus_slider_from_params(self, "rotation");
  dt_bauhaus_slider_set_format(g->rotation, "°");
  gtk_widget_set_tooltip_text(g->rotation, _("rotation of filter -180 to 180 degrees"));

  g->hue = dt_color_picker_new(self, DT_COLOR_PICKER_POINT, dt_bauhaus_slider_from_params(self, "hue"));
  dt_bauhaus_slider_set_feedback(g->hue, 0);
  dt_bauhaus_slider_set_factor(g->hue, 360.0f);
  dt_bauhaus_slider_set_format(g->hue, "°");
  dt_bauhaus_slider_set_stop(g->hue, 0.0f, 1.0f, 0.0f, 0.0f);
  dt_bauhaus_slider_set_stop(g->hue, 0.166f, 1.0f, 1.0f, 0.0f);
  dt_bauhaus_slider_set_stop(g->hue, 0.322f, 0.0f, 1.0f, 0.0f);
  dt_bauhaus_slider_set_stop(g->hue, 0.498f, 0.0f, 1.0f, 1.0f);
  dt_bauhaus_slider_set_stop(g->hue, 0.664f, 0.0f, 0.0f, 1.0f);
  dt_bauhaus_slider_set_stop(g->hue, 0.830f, 1.0f, 0.0f, 1.0f);
  dt_bauhaus_slider_set_stop(g->hue, 1.0f, 1.0f, 0.0f, 0.0f);
  gtk_widget_set_tooltip_text(g->hue, _("select the hue tone of filter"));

  g->saturation = dt_bauhaus_slider_from_params(self, "saturation");
  dt_bauhaus_slider_set_format(g->saturation, "%");
  dt_bauhaus_slider_set_stop(g->saturation, 0.0f, 0.2f, 0.2f, 0.2f);
  dt_bauhaus_slider_set_stop(g->saturation, 1.0f, 1.0f, 1.0f, 1.0f);
  gtk_widget_set_tooltip_text(g->saturation, _("select the saturation of filter"));

  g->selected = 0;
  g->dragging = 0;
  g->define = 0;
}

GSList *mouse_actions(dt_iop_module_t *self)
{
  GSList *lm = NULL;
  lm = dt_mouse_action_create_format(lm, DT_MOUSE_ACTION_LEFT_DRAG, 0,
                                     _("[%s on nodes] change line rotation"), self->name());
  lm = dt_mouse_action_create_format(lm, DT_MOUSE_ACTION_LEFT_DRAG, 0, _("[%s on line] move line"), self->name());
  lm = dt_mouse_action_create_format(lm, DT_MOUSE_ACTION_SCROLL, GDK_CONTROL_MASK,
                                     _("[%s on line] change density"), self->name());
  lm = dt_mouse_action_create_format(lm, DT_MOUSE_ACTION_SCROLL, GDK_SHIFT_MASK,
                                     _("[%s on line] change hardness"), self->name());
  return lm;
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

