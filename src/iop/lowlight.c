/*
    This file is part of darktable,
    copyright (c) 2011 Rostyslav Pidgornyi

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
#include <string.h>
#include <inttypes.h>
#include "common/colorspaces.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "gui/histogram.h"
#include "develop/develop.h"
#include "control/control.h"
#include "control/conf.h"
#include "gui/gtk.h"
#include "gui/draw.h"
#include "gui/presets.h"
#include "dtgtk/slider.h"

DT_MODULE(1)

#define DT_IOP_LOWLIGHT_INSET 5
#define DT_IOP_LOWLIGHT_RES 64
#define DT_IOP_LOWLIGHT_BANDS 6
#define DT_IOP_LOWLIGHT_LUT_RES 0x10000

typedef struct dt_iop_lowlight_params_t
{
  float blueness;
  float transition_x[DT_IOP_LOWLIGHT_BANDS], transition_y[DT_IOP_LOWLIGHT_BANDS];
}
dt_iop_lowlight_params_t;

typedef struct dt_iop_lowlight_gui_data_t
{
  dt_draw_curve_t *transition_curve;        // curve for gui to draw

  GtkDarktableSlider *scale_blueness;
  GtkDrawingArea *area;
  double mouse_x, mouse_y, mouse_pick;
  float mouse_radius;
  dt_iop_lowlight_params_t drag_params;
  int dragging;
  int x_move;
  float draw_xs[DT_IOP_LOWLIGHT_RES], draw_ys[DT_IOP_LOWLIGHT_RES];
  float draw_min_xs[DT_IOP_LOWLIGHT_RES], draw_min_ys[DT_IOP_LOWLIGHT_RES];
  float draw_max_xs[DT_IOP_LOWLIGHT_RES], draw_max_ys[DT_IOP_LOWLIGHT_RES];
}
dt_iop_lowlight_gui_data_t;

typedef struct dt_iop_lowlight_data_t
{
  float blueness;
  dt_draw_curve_t *curve;
  float lut[DT_IOP_LOWLIGHT_LUT_RES];
}
dt_iop_lowlight_data_t;

const char
*name()
{
  return _("lowlight vision");
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES;
}

int
groups ()
{
  return IOP_GROUP_EFFECT;
}


void init_key_accels()
{
  dtgtk_slider_init_accel(darktable.control->accels_darkroom,"<Darktable>/darkroom/plugins/lowlight/blue shift");
}
static float
lookup(const float *lut, const float i)
{
  const int bin0 = MIN(0xffff, MAX(0, DT_IOP_LOWLIGHT_LUT_RES *  i));
  const int bin1 = MIN(0xffff, MAX(0, DT_IOP_LOWLIGHT_LUT_RES *  i + 1));
  const float f = DT_IOP_LOWLIGHT_LUT_RES * i - bin0;
  return lut[bin1]*f + lut[bin0]*(1.-f);
}

void
process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *i, void *o, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_lowlight_data_t *d = (dt_iop_lowlight_data_t *)(piece->data);
  const int ch = piece->colors;

  // empiric coefficient
  const float c = 0.5f;
  const float threshold = 0.01f;

  // scotopic white, blue saturated
  float Lab_sw[3] = { 100.0f , 0 , -d->blueness };
  float XYZ_sw[3];

  dt_Lab_to_XYZ(Lab_sw, XYZ_sw);

#ifdef _OPENMP
  #pragma omp parallel for default(none) schedule(static) shared(roi_in, roi_out, d, i, o, XYZ_sw)
#endif
  for(int k=0; k<roi_out->width*roi_out->height; k++)
  {
    float *in = (float *)i + ch*k;
    float *out = (float *)o + ch*k;
    float XYZ[3], XYZ_s[3];
    float V;
    float w;

    dt_Lab_to_XYZ(in, XYZ);

    // calculate scotopic luminanse
    if (XYZ[0] > threshold)
    {
      // normal flow
      V = XYZ[1] * ( 1.33f * ( 1.0f + (XYZ[1]+XYZ[2])/XYZ[0]) - 1.68f );
    }
    else
    {
      // low red flow, avoids "snow" on dark noisy areas
      V = XYZ[1] * ( 1.33f * ( 1.0f + (XYZ[1]+XYZ[2])/threshold) - 1.68f );
    }

    // scale using empiric coefficient and fit inside limits
    V = fminf(1.0f,fmaxf(0.0f,c*V));

    // blending coefficient from curve
    w = lookup(d->lut,in[0]/100.f);

    XYZ_s[0] = V * XYZ_sw[0];
    XYZ_s[1] = V * XYZ_sw[1];
    XYZ_s[2] = V * XYZ_sw[2];

    XYZ[0] = w * XYZ[0] + (1.0f - w) * XYZ_s[0];
    XYZ[1] = w * XYZ[1] + (1.0f - w) * XYZ_s[1];
    XYZ[2] = w * XYZ[2] + (1.0f - w) * XYZ_s[2];

    dt_XYZ_to_Lab(XYZ,out);
  }
}

void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_lowlight_data_t *d = (dt_iop_lowlight_data_t *)(piece->data);
  dt_iop_lowlight_params_t *p = (dt_iop_lowlight_params_t *)p1;
  dt_draw_curve_set_point(d->curve, 0, p->transition_x[DT_IOP_LOWLIGHT_BANDS-2]-1.0, p->transition_y[0]);
  for(int k=0; k<DT_IOP_LOWLIGHT_BANDS; k++)
    dt_draw_curve_set_point(d->curve, k+1, p->transition_x[k], p->transition_y[k]);
  dt_draw_curve_set_point(d->curve, DT_IOP_LOWLIGHT_BANDS+1, p->transition_x[1]+1.0, p->transition_y[DT_IOP_LOWLIGHT_BANDS-1]);
  dt_draw_curve_calc_values(d->curve, 0.0, 1.0, DT_IOP_LOWLIGHT_LUT_RES, NULL, d->lut);
  d->blueness = p->blueness;
}

void init_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_lowlight_data_t *d = (dt_iop_lowlight_data_t *)malloc(sizeof(dt_iop_lowlight_data_t));
  dt_iop_lowlight_params_t *default_params = (dt_iop_lowlight_params_t *)self->default_params;
  piece->data = (void *)d;
  d->curve = dt_draw_curve_new(0.0, 1.0, CATMULL_ROM);
  (void)dt_draw_curve_add_point(d->curve, default_params->transition_x[DT_IOP_LOWLIGHT_BANDS-2]-1.0, default_params->transition_y[DT_IOP_LOWLIGHT_BANDS-2]);
  for(int k=0; k<DT_IOP_LOWLIGHT_BANDS; k++)
    (void)dt_draw_curve_add_point(d->curve, default_params->transition_x[k], default_params->transition_y[k]);
  (void)dt_draw_curve_add_point(d->curve, default_params->transition_x[1]+1.0, default_params->transition_y[1]);
}

void cleanup_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  // clean up everything again.
  dt_iop_lowlight_data_t *d = (dt_iop_lowlight_data_t *)(piece->data);
  dt_draw_curve_destroy(d->curve);
  free(piece->data);
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_lowlight_gui_data_t *g = (dt_iop_lowlight_gui_data_t *)self->gui_data;
  dt_iop_lowlight_params_t *p = (dt_iop_lowlight_params_t *)self->params;
  dtgtk_slider_set_value(g->scale_blueness, p->blueness);
  gtk_widget_queue_draw(self->widget);
}

void init(dt_iop_module_t *module)
{
  module->params = malloc(sizeof(dt_iop_lowlight_params_t));
  module->default_params = malloc(sizeof(dt_iop_lowlight_params_t));
  module->default_enabled = 0; // we're a rather slow and rare op.
  module->priority = 511; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_lowlight_params_t);
  module->gui_data = NULL;
  dt_iop_lowlight_params_t tmp;
  for(int k=0; k<DT_IOP_LOWLIGHT_BANDS; k++) tmp.transition_x[k] = k/(DT_IOP_LOWLIGHT_BANDS-1.0);
  for(int k=0; k<DT_IOP_LOWLIGHT_BANDS; k++) tmp.transition_y[k] = 0.5f;
  tmp.blueness = 0.0f;
  memcpy(module->params, &tmp, sizeof(dt_iop_lowlight_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_lowlight_params_t));
}

void cleanup(dt_iop_module_t *module)
{
  free(module->gui_data);
  module->gui_data = NULL;
  free(module->params);
  module->params = NULL;
}

void init_presets (dt_iop_module_t *self)
{
  dt_iop_lowlight_params_t p;

  DT_DEBUG_SQLITE3_EXEC(darktable.db, "begin", NULL, NULL, NULL);

  p.transition_x[0] = 0.000000;
  p.transition_x[1] = 0.200000;
  p.transition_x[2] = 0.400000;
  p.transition_x[3] = 0.600000;
  p.transition_x[4] = 0.800000;
  p.transition_x[5] = 1.000000;

  p.transition_y[0] = 1.000000;
  p.transition_y[1] = 1.000000;
  p.transition_y[2] = 1.000000;
  p.transition_y[3] = 1.000000;
  p.transition_y[4] = 1.000000;
  p.transition_y[5] = 1.000000;

  p.blueness = 0.0f;
  dt_gui_presets_add_generic(_("daylight"), self->op, &p, sizeof(p), 1);

  p.transition_x[0] = 0.000000;
  p.transition_x[1] = 0.200000;
  p.transition_x[2] = 0.400000;
  p.transition_x[3] = 0.600000;
  p.transition_x[4] = 0.800000;
  p.transition_x[5] = 1.000000;

  p.transition_y[0] = 0.600000;
  p.transition_y[1] = 0.800000;
  p.transition_y[2] = 0.950000;
  p.transition_y[3] = 0.980000;
  p.transition_y[4] = 1.000000;
  p.transition_y[5] = 1.000000;

  p.blueness = 30.0f;
  dt_gui_presets_add_generic(_("indoor bright"), self->op, &p, sizeof(p), 1);

  p.transition_x[0] = 0.000000;
  p.transition_x[1] = 0.200000;
  p.transition_x[2] = 0.400000;
  p.transition_x[3] = 0.600000;
  p.transition_x[4] = 0.800000;
  p.transition_x[5] = 1.000000;

  p.transition_y[0] = 0.300000;
  p.transition_y[1] = 0.500000;
  p.transition_y[2] = 0.700000;
  p.transition_y[3] = 0.850000;
  p.transition_y[4] = 0.970000;
  p.transition_y[5] = 1.000000;

  p.blueness = 30.0f;
  dt_gui_presets_add_generic(_("indoor dim"), self->op, &p, sizeof(p), 1);

  p.transition_x[0] = 0.000000;
  p.transition_x[1] = 0.200000;
  p.transition_x[2] = 0.400000;
  p.transition_x[3] = 0.600000;
  p.transition_x[4] = 0.800000;
  p.transition_x[5] = 1.000000;

  p.transition_y[0] = 0.050000;
  p.transition_y[1] = 0.200000;
  p.transition_y[2] = 0.400000;
  p.transition_y[3] = 0.700000;
  p.transition_y[4] = 0.920000;
  p.transition_y[5] = 1.000000;

  p.blueness = 40.0f;
  dt_gui_presets_add_generic(_("indoor dark"), self->op, &p, sizeof(p), 1);

  p.transition_x[0] = 0.000000;
  p.transition_x[1] = 0.200000;
  p.transition_x[2] = 0.400000;
  p.transition_x[3] = 0.600000;
  p.transition_x[4] = 0.800000;
  p.transition_x[5] = 1.000000;

  p.transition_y[0] = 0.070000;
  p.transition_y[1] = 0.100000;
  p.transition_y[2] = 0.180000;
  p.transition_y[3] = 0.350000;
  p.transition_y[4] = 0.750000;
  p.transition_y[5] = 1.000000;

  p.blueness = 50.0f;
  dt_gui_presets_add_generic(_("twilight"), self->op, &p, sizeof(p), 1);

  p.transition_x[0] = 0.000000;
  p.transition_x[1] = 0.200000;
  p.transition_x[2] = 0.400000;
  p.transition_x[3] = 0.600000;
  p.transition_x[4] = 0.800000;
  p.transition_x[5] = 1.000000;

  p.transition_y[0] = 0.000000;
  p.transition_y[1] = 0.450000;
  p.transition_y[2] = 0.750000;
  p.transition_y[3] = 0.930000;
  p.transition_y[4] = 0.990000;
  p.transition_y[5] = 1.000000;

  p.blueness = 30.0f;
  dt_gui_presets_add_generic(_("night street lit"), self->op, &p, sizeof(p), 1);

  p.transition_x[0] = 0.000000;
  p.transition_x[1] = 0.200000;
  p.transition_x[2] = 0.400000;
  p.transition_x[3] = 0.600000;
  p.transition_x[4] = 0.800000;
  p.transition_x[5] = 1.000000;

  p.transition_y[0] = 0.000000;
  p.transition_y[1] = 0.150000;
  p.transition_y[2] = 0.350000;
  p.transition_y[3] = 0.800000;
  p.transition_y[4] = 0.970000;
  p.transition_y[5] = 1.000000;

  p.blueness = 30.0f;
  dt_gui_presets_add_generic(_("night street"), self->op, &p, sizeof(p), 1);

  p.transition_x[0] = 0.000000;
  p.transition_x[1] = 0.150000;
  p.transition_x[2] = 0.400000;
  p.transition_x[3] = 0.600000;
  p.transition_x[4] = 0.800000;
  p.transition_x[5] = 1.000000;

  p.transition_y[0] = 0.000000;
  p.transition_y[1] = 0.020000;
  p.transition_y[2] = 0.050000;
  p.transition_y[3] = 0.200000;
  p.transition_y[4] = 0.550000;
  p.transition_y[5] = 1.000000;

  p.blueness = 40.0f;
  dt_gui_presets_add_generic(_("night street dark"), self->op, &p, sizeof(p), 1);

  p.transition_x[0] = 0.000000;
  p.transition_x[1] = 0.200000;
  p.transition_x[2] = 0.400000;
  p.transition_x[3] = 0.600000;
  p.transition_x[4] = 0.800000;
  p.transition_x[5] = 1.000000;

  p.transition_y[0] = 0.000000;
  p.transition_y[1] = 0.000000;
  p.transition_y[2] = 0.000000;
  p.transition_y[3] = 0.000000;
  p.transition_y[4] = 0.000000;
  p.transition_y[5] = 0.000000;


  p.blueness = 50.0f;
  dt_gui_presets_add_generic(_("night"), self->op, &p, sizeof(p), 1);

  DT_DEBUG_SQLITE3_EXEC(darktable.db, "commit", NULL, NULL, NULL);
}

// fills in new parameters based on mouse position (in 0,1)
static void
dt_iop_lowlight_get_params(dt_iop_lowlight_params_t *p, const double mouse_x, const double mouse_y, const float rad)
{
  for(int k=0; k<DT_IOP_LOWLIGHT_BANDS; k++)
  {
    const float f = expf(-(mouse_x - p->transition_x[k])*(mouse_x - p->transition_x[k])/(rad*rad));
    p->transition_y[k] = (1-f)*p->transition_y[k] + f*mouse_y;
  }
}

static gboolean
lowlight_expose(GtkWidget *widget, GdkEventExpose *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_lowlight_gui_data_t *c = (dt_iop_lowlight_gui_data_t *)self->gui_data;
  dt_iop_lowlight_params_t p = *(dt_iop_lowlight_params_t *)self->params;

  dt_draw_curve_set_point(c->transition_curve, 0, p.transition_x[DT_IOP_LOWLIGHT_BANDS-2]-1.0, p.transition_y[0]);
  for(int k=0; k<DT_IOP_LOWLIGHT_BANDS; k++) dt_draw_curve_set_point(c->transition_curve, k+1, p.transition_x[k], p.transition_y[k]);
  dt_draw_curve_set_point(c->transition_curve, DT_IOP_LOWLIGHT_BANDS+1, p.transition_x[1]+1.0, p.transition_y[DT_IOP_LOWLIGHT_BANDS-1]);

  const int inset = DT_IOP_LOWLIGHT_INSET;
  int width = widget->allocation.width, height = widget->allocation.height;
  cairo_surface_t *cst = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);

  cairo_set_source_rgb (cr, .2, .2, .2);
  cairo_paint(cr);

  cairo_translate(cr, inset, inset);
  width -= 2*inset;
  height -= 2*inset;

  cairo_set_line_width(cr, 1.0);
  cairo_set_source_rgb (cr, .1, .1, .1);
  cairo_rectangle(cr, 0, 0, width, height);
  cairo_stroke(cr);

  cairo_set_source_rgb (cr, .3, .3, .3);
  cairo_rectangle(cr, 0, 0, width, height);
  cairo_fill(cr);

  // draw grid
  cairo_set_line_width(cr, .4);
  cairo_set_source_rgb (cr, .1, .1, .1);
  dt_draw_grid(cr, 8, 0, 0, width, height);


  if(c->mouse_y > 0 || c->dragging)
  {
    // draw min/max curves:
    dt_iop_lowlight_get_params(&p, c->mouse_x, 1., c->mouse_radius);
    dt_draw_curve_set_point(c->transition_curve, 0, p.transition_x[DT_IOP_LOWLIGHT_BANDS-2]-1.0, p.transition_y[0]);
    for(int k=0; k<DT_IOP_LOWLIGHT_BANDS; k++)
      dt_draw_curve_set_point(c->transition_curve, k+1, p.transition_x[k], p.transition_y[k]);
    dt_draw_curve_set_point(c->transition_curve, DT_IOP_LOWLIGHT_BANDS+1, p.transition_x[1]+1.0, p.transition_y[DT_IOP_LOWLIGHT_BANDS-1]);
    dt_draw_curve_calc_values(c->transition_curve, 0.0, 1.0, DT_IOP_LOWLIGHT_RES, c->draw_min_xs, c->draw_min_ys);

    p = *(dt_iop_lowlight_params_t *)self->params;
    dt_iop_lowlight_get_params(&p, c->mouse_x, .0, c->mouse_radius);
    dt_draw_curve_set_point(c->transition_curve, 0, p.transition_x[DT_IOP_LOWLIGHT_BANDS-2]-1.0, p.transition_y[0]);
    for(int k=0; k<DT_IOP_LOWLIGHT_BANDS; k++)
      dt_draw_curve_set_point(c->transition_curve, k+1, p.transition_x[k], p.transition_y[k]);
    dt_draw_curve_set_point(c->transition_curve, DT_IOP_LOWLIGHT_BANDS+1, p.transition_x[1]+1.0, p.transition_y[DT_IOP_LOWLIGHT_BANDS-1]);
    dt_draw_curve_calc_values(c->transition_curve, 0.0, 1.0, DT_IOP_LOWLIGHT_RES, c->draw_max_xs, c->draw_max_ys);
  }

  cairo_save(cr);

  // draw x positions
  cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
  cairo_set_line_width(cr, 1.);
  const float arrw = 7.0f;
  for(int k=0; k<DT_IOP_LOWLIGHT_BANDS; k++)
  {
    cairo_move_to(cr, width*p.transition_x[k], height+inset-1);
    cairo_rel_line_to(cr, -arrw*.5f, 0);
    cairo_rel_line_to(cr, arrw*.5f, -arrw);
    cairo_rel_line_to(cr, arrw*.5f, arrw);
    cairo_close_path(cr);
    if(c->x_move == k) cairo_fill(cr);
    else               cairo_stroke(cr);
  }

  // draw selected cursor
  cairo_translate(cr, 0, height);

  // cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
  //cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
  cairo_set_line_width(cr, 2.);
  cairo_set_source_rgba(cr, .7, .7, .7, 1.0);

  p = *(dt_iop_lowlight_params_t *)self->params;
  dt_draw_curve_set_point(c->transition_curve, 0, p.transition_x[DT_IOP_LOWLIGHT_BANDS-2]-1.0, p.transition_y[0]);
  for(int k=0; k<DT_IOP_LOWLIGHT_BANDS; k++)
    dt_draw_curve_set_point(c->transition_curve, k+1, p.transition_x[k], p.transition_y[k]);
  dt_draw_curve_set_point(c->transition_curve, DT_IOP_LOWLIGHT_BANDS+1, p.transition_x[1]+1.0, p.transition_y[DT_IOP_LOWLIGHT_BANDS-1]);
  dt_draw_curve_calc_values(c->transition_curve, 0.0, 1.0, DT_IOP_LOWLIGHT_RES, c->draw_xs, c->draw_ys);
  cairo_move_to(cr, 0*width/(float)(DT_IOP_LOWLIGHT_RES-1), - height*c->draw_ys[0]);
  for(int k=1; k<DT_IOP_LOWLIGHT_RES; k++) cairo_line_to(cr, k*width/(float)(DT_IOP_LOWLIGHT_RES-1), - height*c->draw_ys[k]);
  cairo_stroke(cr);

  // draw dots on knots
  cairo_set_source_rgb(cr, 0.7, 0.7, 0.7);
  cairo_set_line_width(cr, 1.);
  for(int k=0; k<DT_IOP_LOWLIGHT_BANDS; k++)
  {
    cairo_arc(cr, width*p.transition_x[k], - height*p.transition_y[k], 3.0, 0.0, 2.0*M_PI);
    if(c->x_move == k) cairo_fill(cr);
    else               cairo_stroke(cr);
  }

  if(c->mouse_y > 0 || c->dragging)
  {
    // draw min/max, if selected
    cairo_set_source_rgba(cr, .7, .7, .7, .6);
    cairo_move_to(cr, 0, - height*c->draw_min_ys[0]);
    for(int k=1; k<DT_IOP_LOWLIGHT_RES; k++)    cairo_line_to(cr, k*width/(float)(DT_IOP_LOWLIGHT_RES-1), - height*c->draw_min_ys[k]);
    for(int k=DT_IOP_LOWLIGHT_RES-1; k>=0; k--) cairo_line_to(cr, k*width/(float)(DT_IOP_LOWLIGHT_RES-1), - height*c->draw_max_ys[k]);
    cairo_close_path(cr);
    cairo_fill(cr);
    // draw mouse focus circle
    cairo_set_source_rgba(cr, .9, .9, .9, .5);
    const float pos = DT_IOP_LOWLIGHT_RES * c->mouse_x;
    int k = (int)pos;
    const float f = k - pos;
    if(k >= DT_IOP_LOWLIGHT_RES-1) k = DT_IOP_LOWLIGHT_RES - 2;
    float ht = -height*(f*c->draw_ys[k] + (1-f)*c->draw_ys[k+1]);
    cairo_arc(cr, c->mouse_x*width, ht, c->mouse_radius*width, 0, 2.*M_PI);
    cairo_stroke(cr);
  }

  cairo_restore (cr);

  cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);

  // draw labels:
  cairo_text_extents_t ext;
  cairo_set_source_rgb(cr, .1, .1, .1);
  cairo_select_font_face (cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
  cairo_set_font_size (cr, .06*height);

  cairo_text_extents (cr, _("dark"), &ext);
  cairo_move_to (cr, .02*width+ext.height, .5*(height+ext.width));
  cairo_save (cr);
  cairo_rotate (cr, -M_PI*.5f);
  cairo_show_text(cr, _("dark"));
  cairo_restore (cr);

  cairo_text_extents (cr, _("bright"), &ext);
  cairo_move_to (cr, .98*width, .5*(height+ext.width));
  cairo_save (cr);
  cairo_rotate (cr, -M_PI*.5f);
  cairo_show_text(cr, _("bright"));
  cairo_restore (cr);

  cairo_text_extents (cr, _("day vision"), &ext);
  cairo_move_to (cr, .5*(width-ext.width), .08*height);
  cairo_show_text(cr, _("day vision"));

  cairo_text_extents (cr, _("night vision"), &ext);
  cairo_move_to (cr, .5*(width-ext.width), .97*height);
  cairo_show_text(cr, _("night vision"));


  cairo_destroy(cr);
  cairo_t *cr_pixmap = gdk_cairo_create(gtk_widget_get_window(widget));
  cairo_set_source_surface (cr_pixmap, cst, 0, 0);
  cairo_paint(cr_pixmap);
  cairo_destroy(cr_pixmap);
  cairo_surface_destroy(cst);
  return TRUE;
}

static gboolean
lowlight_motion_notify(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_lowlight_gui_data_t *c = (dt_iop_lowlight_gui_data_t *)self->gui_data;
  dt_iop_lowlight_params_t *p = (dt_iop_lowlight_params_t *)self->params;
  const int inset = DT_IOP_LOWLIGHT_INSET;
  int height = widget->allocation.height - 2*inset, width = widget->allocation.width - 2*inset;
  if(!c->dragging) c->mouse_x = CLAMP(event->x - inset, 0, width)/(float)width;
  c->mouse_y = 1.0 - CLAMP(event->y - inset, 0, height)/(float)height;
  if(c->dragging)
  {
    *p = c->drag_params;
    if(c->x_move >= 0)
    {
      const float mx = CLAMP(event->x - inset, 0, width)/(float)width;
      if(c->x_move > 0 && c->x_move < DT_IOP_LOWLIGHT_BANDS-1)
      {
        const float minx = p->transition_x[c->x_move-1]+0.001f;
        const float maxx = p->transition_x[c->x_move+1]-0.001f;
        p->transition_x[c->x_move] = fminf(maxx, fmaxf(minx, mx));
      }
    }
    else
    {
      dt_iop_lowlight_get_params(p, c->mouse_x, c->mouse_y + c->mouse_pick, c->mouse_radius);
    }
    dt_dev_add_history_item(darktable.develop, self, TRUE);
  }
  else if(event->y > height)
  {
    c->x_move = 0;
    float dist = fabsf(p->transition_x[0] - c->mouse_x);
    for(int k=1; k<DT_IOP_LOWLIGHT_BANDS; k++)
    {
      float d2 = fabsf(p->transition_x[k] - c->mouse_x);
      if(d2 < dist)
      {
        c->x_move = k;
        dist = d2;
      }
    }
  }
  else
  {
    c->x_move = -1;
  }
  gtk_widget_queue_draw(widget);
  gint x, y;
  gdk_window_get_pointer(event->window, &x, &y, NULL);
  return TRUE;
}

static gboolean
lowlight_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  if(event->button == 1)
  {
    dt_iop_module_t *self = (dt_iop_module_t *)user_data;
    dt_iop_lowlight_gui_data_t *c = (dt_iop_lowlight_gui_data_t *)self->gui_data;
    c->drag_params = *(dt_iop_lowlight_params_t *)self->params;
    const int inset = DT_IOP_LOWLIGHT_INSET;
    int height = widget->allocation.height - 2*inset, width = widget->allocation.width - 2*inset;
    c->mouse_pick = dt_draw_curve_calc_value(c->transition_curve, CLAMP(event->x - inset, 0, width)/(float)width);
    c->mouse_pick -= 1.0 - CLAMP(event->y - inset, 0, height)/(float)height;
    c->dragging = 1;
    return TRUE;
  }
  return FALSE;
}

static gboolean
lowlight_button_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  if(event->button == 1)
  {
    dt_iop_module_t *self = (dt_iop_module_t *)user_data;
    dt_iop_lowlight_gui_data_t *c = (dt_iop_lowlight_gui_data_t *)self->gui_data;
    c->dragging = 0;
    return TRUE;
  }
  return FALSE;
}

static gboolean
lowlight_leave_notify(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_lowlight_gui_data_t *c = (dt_iop_lowlight_gui_data_t *)self->gui_data;
  if(!c->dragging) c->mouse_x = c->mouse_y = -1.0;
  gtk_widget_queue_draw(widget);
  return TRUE;
}

static gboolean
lowlight_scrolled(GtkWidget *widget, GdkEventScroll *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_lowlight_gui_data_t *c = (dt_iop_lowlight_gui_data_t *)self->gui_data;
  if(event->direction == GDK_SCROLL_UP   && c->mouse_radius > 0.2/DT_IOP_LOWLIGHT_BANDS) c->mouse_radius *= 0.9; //0.7;
  if(event->direction == GDK_SCROLL_DOWN && c->mouse_radius < 1.0) c->mouse_radius *= (1.0/0.9); //1.42;
  gtk_widget_queue_draw(widget);
  return TRUE;
}

static void
blueness_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_lowlight_params_t *p = (dt_iop_lowlight_params_t *)self->params;
  p->blueness = dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_lowlight_gui_data_t));
  dt_iop_lowlight_gui_data_t *c = (dt_iop_lowlight_gui_data_t *)self->gui_data;
  dt_iop_lowlight_params_t *p = (dt_iop_lowlight_params_t *)self->params;

  c->transition_curve = dt_draw_curve_new(0.0, 1.0, CATMULL_ROM);
  (void)dt_draw_curve_add_point(c->transition_curve, p->transition_x[DT_IOP_LOWLIGHT_BANDS-2]-1.0, p->transition_y[DT_IOP_LOWLIGHT_BANDS-2]);
  for(int k=0; k<DT_IOP_LOWLIGHT_BANDS; k++) (void)dt_draw_curve_add_point(c->transition_curve, p->transition_x[k], p->transition_y[k]);
  (void)dt_draw_curve_add_point(c->transition_curve, p->transition_x[1]+1.0, p->transition_y[1]);

  c->mouse_x = c->mouse_y = c->mouse_pick = -1.0;
  c->dragging = 0;
  c->x_move = -1;
  c->mouse_radius = 1.0/DT_IOP_LOWLIGHT_BANDS;

  self->widget = GTK_WIDGET(gtk_vbox_new(FALSE, DT_GUI_IOP_MODULE_CONTROL_SPACING));

  c->area = GTK_DRAWING_AREA(gtk_drawing_area_new());
  gtk_drawing_area_size(c->area, 195, 195);

  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(c->area),FALSE, FALSE, 0);

  gtk_widget_add_events(GTK_WIDGET(c->area), GDK_POINTER_MOTION_MASK | GDK_POINTER_MOTION_HINT_MASK | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_LEAVE_NOTIFY_MASK);
  g_signal_connect (G_OBJECT (c->area), "expose-event",
                    G_CALLBACK (lowlight_expose), self);
  g_signal_connect (G_OBJECT (c->area), "button-press-event",
                    G_CALLBACK (lowlight_button_press), self);
  g_signal_connect (G_OBJECT (c->area), "button-release-event",
                    G_CALLBACK (lowlight_button_release), self);
  g_signal_connect (G_OBJECT (c->area), "motion-notify-event",
                    G_CALLBACK (lowlight_motion_notify), self);
  g_signal_connect (G_OBJECT (c->area), "leave-notify-event",
                    G_CALLBACK (lowlight_leave_notify), self);
  g_signal_connect (G_OBJECT (c->area), "scroll-event",
                    G_CALLBACK (lowlight_scrolled), self);

  c->scale_blueness = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 100.0, 5.0, p->blueness, 2));
  dtgtk_slider_set_default_value(c->scale_blueness, p->blueness);
  dtgtk_slider_set_label(c->scale_blueness,_("blue shift"));
  dtgtk_slider_set_unit(c->scale_blueness,"%");
  dtgtk_slider_set_format_type(c->scale_blueness,DARKTABLE_SLIDER_FORMAT_PERCENT);
  dtgtk_slider_set_accel(c->scale_blueness,darktable.control->accels_darkroom,"<Darktable>/darkroom/plugins/lowlight/blue shift");
  g_object_set(G_OBJECT(c->scale_blueness), "tooltip-text", _("blueness in shadows"), (char *)NULL);

  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(c->scale_blueness), TRUE, TRUE, 5);

  g_signal_connect (G_OBJECT (c->scale_blueness), "value-changed",
                    G_CALLBACK (blueness_callback), self);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  dt_iop_lowlight_gui_data_t *c = (dt_iop_lowlight_gui_data_t *)self->gui_data;
  dt_draw_curve_destroy(c->transition_curve);
  free(self->gui_data);
  self->gui_data = NULL;
}

// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
