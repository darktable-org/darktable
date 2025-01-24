/*
    This file is part of darktable,
    Copyright (C) 2011-2024 darktable developers.

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
#include "bauhaus/bauhaus.h"
#include "common/colorspaces_inline_conversions.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/opencl.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop_gui.h"
#include "dtgtk/drawingarea.h"
#include "gui/accelerators.h"
#include "gui/draw.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "iop/iop_api.h"
#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

DT_MODULE_INTROSPECTION(1, dt_iop_lowlight_params_t)

#define DT_IOP_LOWLIGHT_INSET DT_PIXEL_APPLY_DPI(5)
#define DT_IOP_LOWLIGHT_RES 64
#define DT_IOP_LOWLIGHT_BANDS 6
#define DT_IOP_LOWLIGHT_LUT_RES 0x10000

typedef struct dt_iop_lowlight_params_t
{
  float blueness; // $MIN: 0.0 $MAX: 100.0 $DEFAULT: 0.0 $DESCRIPTION: "blue shift"
  float transition_x[DT_IOP_LOWLIGHT_BANDS];
  float transition_y[DT_IOP_LOWLIGHT_BANDS]; // $DEFAULT: 0.5
} dt_iop_lowlight_params_t;

typedef struct dt_iop_lowlight_gui_data_t
{
  dt_draw_curve_t *transition_curve; // curve for gui to draw

  GtkWidget *scale_blueness;
  GtkDrawingArea *area;
  double mouse_x, mouse_y, mouse_pick;
  float mouse_radius;
  dt_iop_lowlight_params_t drag_params;
  int dragging;
  int x_move;
  float draw_xs[DT_IOP_LOWLIGHT_RES], draw_ys[DT_IOP_LOWLIGHT_RES];
  float draw_min_xs[DT_IOP_LOWLIGHT_RES], draw_min_ys[DT_IOP_LOWLIGHT_RES];
  float draw_max_xs[DT_IOP_LOWLIGHT_RES], draw_max_ys[DT_IOP_LOWLIGHT_RES];
} dt_iop_lowlight_gui_data_t;

typedef struct dt_iop_lowlight_data_t
{
  float blueness;
  dt_draw_curve_t *curve;
  float lut[DT_IOP_LOWLIGHT_LUT_RES];
} dt_iop_lowlight_data_t;

typedef struct dt_iop_lowlight_global_data_t
{
  int kernel_lowlight;
} dt_iop_lowlight_global_data_t;


const char *name()
{
  return _("lowlight vision");
}

const char **description(dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("simulate human night vision"),
                                      _("creative"),
                                      _("non-linear, Lab, display-referred"),
                                      _("linear, XYZ"),
                                      _("non-linear, Lab, display-referred"));
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING;
}

int default_group()
{
  return IOP_GROUP_EFFECT | IOP_GROUP_EFFECTS;
}

dt_iop_colorspace_type_t default_colorspace(dt_iop_module_t *self,
                                            dt_dev_pixelpipe_t *pipe,
                                            dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_LAB;
}

static float lookup(const float *lut, const float i)
{
  const int bin0 = MIN(0xffff, MAX(0, DT_IOP_LOWLIGHT_LUT_RES * i));
  const int bin1 = MIN(0xffff, MAX(0, DT_IOP_LOWLIGHT_LUT_RES * i + 1));
  const float f = DT_IOP_LOWLIGHT_LUT_RES * i - bin0;
  return lut[bin1] * f + lut[bin0] * (1. - f);
}

void process(dt_iop_module_t *self,
             dt_dev_pixelpipe_iop_t *piece,
             const void *const restrict i,
             void *const restrict o,
             const dt_iop_roi_t *const roi_in,
             const dt_iop_roi_t *const roi_out)
{
  if(!dt_iop_have_required_input_format(4 /*we need full-color pixels*/, self, piece->colors,
                                         i, o, roi_in, roi_out))
    return;
  dt_iop_lowlight_data_t *d = piece->data;

  // empiric coefficient
  const float coeff = 0.5f;
  const float threshold = 0.01f;

  // scotopic white, blue saturated
  dt_aligned_pixel_t Lab_sw = { 100.0f, 0, -d->blueness };
  dt_aligned_pixel_t XYZ_sw;

  dt_Lab_to_XYZ(Lab_sw, XYZ_sw);

  const float *lut = d->lut;
  const size_t npixels = (size_t)roi_out->height * roi_out->width;

  DT_OMP_FOR()
  for(size_t k = 0; k < (size_t)npixels; k++)
  {
    const float *const in = (float *)i + 4 * k;
    float *const out = (float *)o + 4 * k;
    dt_aligned_pixel_t XYZ, XYZ_s;
    float V;

    dt_Lab_to_XYZ(in, XYZ);

    // calculate scotopic luminance
    if(XYZ[0] > threshold)
    {
      // normal flow
      V = XYZ[1] * (1.33f * (1.0f + (XYZ[1] + XYZ[2]) / XYZ[0]) - 1.68f);
    }
    else
    {
      // low red flow, avoids "snow" on dark noisy areas
      V = XYZ[1] * (1.33f * (1.0f + (XYZ[1] + XYZ[2]) / threshold) - 1.68f);
    }

    // scale using empiric coefficient and fit inside limits
//    V = fminf(1.0f, fmaxf(0.0f, coeff * V));
    V = CLIP(coeff * V);

    // blending coefficient from curve
    const float w = lookup(lut, in[0] / 100.f);

    for_each_channel(c)
      XYZ_s[c] = V * XYZ_sw[c];

    for_each_channel(c)
      XYZ[c] = w * XYZ[c] + (1.0f - w) * XYZ_s[c];

    dt_aligned_pixel_t res;
    dt_XYZ_to_Lab(XYZ, res);
    copy_pixel_nontemporal(out, res);
  }
}

#ifdef HAVE_OPENCL
int process_cl(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_lowlight_data_t *d = piece->data;
  dt_iop_lowlight_global_data_t *gd = self->global_data;

  cl_mem dev_m = NULL;
  cl_int err = DT_OPENCL_DEFAULT_ERROR;
  const int devid = piece->pipe->devid;

  const int width = roi_out->width;
  const int height = roi_out->height;

  // scotopic white, blue saturated
  dt_aligned_pixel_t Lab_sw = { 100.0f, 0.0f, -d->blueness };
  dt_aligned_pixel_t XYZ_sw;

  dt_Lab_to_XYZ(Lab_sw, XYZ_sw);

  dev_m = dt_opencl_copy_host_to_device(devid, d->lut, 256, 256, sizeof(float));
  if(dev_m == NULL) goto finish;

  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_lowlight, width, height,
    CLARG(dev_in), CLARG(dev_out), CLARG(width), CLARG(height), CLARG(XYZ_sw), CLARG(dev_m));

finish:
  dt_opencl_release_mem_object(dev_m);
  return err;
}
#endif


void init_global(dt_iop_module_so_t *self)
{
  const int program = 2; // basic.cl from programs.conf
  dt_iop_lowlight_global_data_t *gd = malloc(sizeof(dt_iop_lowlight_global_data_t));
  self->data = gd;
  gd->kernel_lowlight = dt_opencl_create_kernel(program, "lowlight");
}


void cleanup_global(dt_iop_module_so_t *self)
{
  dt_iop_lowlight_global_data_t *gd = self->data;
  dt_opencl_free_kernel(gd->kernel_lowlight);
  free(self->data);
  self->data = NULL;
}


void commit_params(dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_lowlight_data_t *d = piece->data;
  dt_iop_lowlight_params_t *p = (dt_iop_lowlight_params_t *)p1;
  dt_draw_curve_set_point(d->curve, 0, p->transition_x[DT_IOP_LOWLIGHT_BANDS - 2] - 1.0, p->transition_y[0]);
  for(int k = 0; k < DT_IOP_LOWLIGHT_BANDS; k++)
    dt_draw_curve_set_point(d->curve, k + 1, p->transition_x[k], p->transition_y[k]);
  dt_draw_curve_set_point(d->curve, DT_IOP_LOWLIGHT_BANDS + 1, p->transition_x[1] + 1.0,
                          p->transition_y[DT_IOP_LOWLIGHT_BANDS - 1]);
  dt_draw_curve_calc_values(d->curve, 0.0, 1.0, DT_IOP_LOWLIGHT_LUT_RES, NULL, d->lut);
  d->blueness = p->blueness;
}

void init_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_lowlight_data_t *d = malloc(sizeof(dt_iop_lowlight_data_t));
  const dt_iop_lowlight_params_t *const default_params = self->default_params;
  piece->data = (void *)d;
  d->curve = dt_draw_curve_new(0.0, 1.0, CATMULL_ROM);
  dt_draw_curve_add_point(d->curve, default_params->transition_x[DT_IOP_LOWLIGHT_BANDS - 2] - 1.0,
                                default_params->transition_y[DT_IOP_LOWLIGHT_BANDS - 2]);
  for(int k = 0; k < DT_IOP_LOWLIGHT_BANDS; k++)
    dt_draw_curve_add_point(d->curve, default_params->transition_x[k], default_params->transition_y[k]);
  dt_draw_curve_add_point(d->curve, default_params->transition_x[1] + 1.0,
                                default_params->transition_y[1]);
}

void cleanup_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  // clean up everything again.
  dt_iop_lowlight_data_t *d = piece->data;
  dt_draw_curve_destroy(d->curve);
  free(piece->data);
  piece->data = NULL;
}

void gui_update(dt_iop_module_t *self)
{
  dt_iop_lowlight_gui_data_t *g = self->gui_data;
  dt_iop_lowlight_params_t *p = self->params;
  dt_bauhaus_slider_set(g->scale_blueness, p->blueness);
  gtk_widget_queue_draw(GTK_WIDGET(g->area));;
}

void init(dt_iop_module_t *self)
{
  dt_iop_default_init(self);

  dt_iop_lowlight_params_t *d = self->default_params;

  for(int k = 0; k < DT_IOP_LOWLIGHT_BANDS; k++) d->transition_x[k] = k / (DT_IOP_LOWLIGHT_BANDS - 1.0);
}

void init_presets(dt_iop_module_so_t *self)
{
  dt_iop_lowlight_params_t p;

  dt_database_start_transaction(darktable.db);

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
  dt_gui_presets_add_generic(_("daylight"), self->op,
                             self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);

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
  dt_gui_presets_add_generic(_("indoor bright"), self->op,
                             self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);

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
  dt_gui_presets_add_generic(_("indoor dim"), self->op,
                             self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);

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
  dt_gui_presets_add_generic(_("indoor dark"), self->op,
                             self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);

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
  dt_gui_presets_add_generic(_("twilight"), self->op,
                             self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);

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
  dt_gui_presets_add_generic(_("night street lit"), self->op,
                             self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);

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
  dt_gui_presets_add_generic(_("night street"), self->op,
                             self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);

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
  dt_gui_presets_add_generic(_("night street dark"), self->op,
                             self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);

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
  dt_gui_presets_add_generic(_("night"), self->op,
                             self->version(), &p, sizeof(p), 1, DEVELOP_BLEND_CS_RGB_DISPLAY);

  dt_database_release_transaction(darktable.db);
}

// fills in new parameters based on mouse position (in 0,1)
static void dt_iop_lowlight_get_params(dt_iop_lowlight_params_t *p, const double mouse_x,
                                       const double mouse_y, const float rad)
{
  for(int k = 0; k < DT_IOP_LOWLIGHT_BANDS; k++)
  {
    const float f = expf(-(mouse_x - p->transition_x[k]) * (mouse_x - p->transition_x[k]) / (rad * rad));
    p->transition_y[k] = (1 - f) * p->transition_y[k] + f * mouse_y;
  }
}

static gboolean lowlight_draw(GtkWidget *widget, cairo_t *crf, dt_iop_module_t *self)
{
  dt_iop_lowlight_gui_data_t *g = self->gui_data;
  dt_iop_lowlight_params_t p = *(dt_iop_lowlight_params_t *)self->params;

  dt_draw_curve_set_point(g->transition_curve, 0, p.transition_x[DT_IOP_LOWLIGHT_BANDS - 2] - 1.0,
                          p.transition_y[0]);
  for(int k = 0; k < DT_IOP_LOWLIGHT_BANDS; k++)
    dt_draw_curve_set_point(g->transition_curve, k + 1, p.transition_x[k], p.transition_y[k]);
  dt_draw_curve_set_point(g->transition_curve, DT_IOP_LOWLIGHT_BANDS + 1, p.transition_x[1] + 1.0,
                          p.transition_y[DT_IOP_LOWLIGHT_BANDS - 1]);

  const int inset = DT_IOP_LOWLIGHT_INSET;
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  int width = allocation.width;
  int height = allocation.height - DT_RESIZE_HANDLE_SIZE;
  cairo_surface_t *cst = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);

  cairo_set_source_rgb(cr, .2, .2, .2);
  cairo_paint(cr);

  cairo_translate(cr, inset, inset);
  width -= 2 * inset;
  height -= 2 * inset;

  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.0));
  cairo_set_source_rgb(cr, .1, .1, .1);
  cairo_rectangle(cr, 0, 0, width, height);
  cairo_stroke(cr);

  cairo_set_source_rgb(cr, .3, .3, .3);
  cairo_rectangle(cr, 0, 0, width, height);
  cairo_fill(cr);

  // draw grid
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(.4));
  cairo_set_source_rgb(cr, .1, .1, .1);
  dt_draw_grid(cr, 8, 0, 0, width, height);


  if(g->mouse_y > 0 || g->dragging)
  {
    // draw min/max curves:
    dt_iop_lowlight_get_params(&p, g->mouse_x, 1., g->mouse_radius);
    dt_draw_curve_set_point(g->transition_curve, 0, p.transition_x[DT_IOP_LOWLIGHT_BANDS - 2] - 1.0,
                            p.transition_y[0]);
    for(int k = 0; k < DT_IOP_LOWLIGHT_BANDS; k++)
      dt_draw_curve_set_point(g->transition_curve, k + 1, p.transition_x[k], p.transition_y[k]);
    dt_draw_curve_set_point(g->transition_curve, DT_IOP_LOWLIGHT_BANDS + 1, p.transition_x[1] + 1.0,
                            p.transition_y[DT_IOP_LOWLIGHT_BANDS - 1]);
    dt_draw_curve_calc_values(g->transition_curve, 0.0, 1.0, DT_IOP_LOWLIGHT_RES, g->draw_min_xs,
                              g->draw_min_ys);

    p = *(dt_iop_lowlight_params_t *)self->params;
    dt_iop_lowlight_get_params(&p, g->mouse_x, .0, g->mouse_radius);
    dt_draw_curve_set_point(g->transition_curve, 0, p.transition_x[DT_IOP_LOWLIGHT_BANDS - 2] - 1.0,
                            p.transition_y[0]);
    for(int k = 0; k < DT_IOP_LOWLIGHT_BANDS; k++)
      dt_draw_curve_set_point(g->transition_curve, k + 1, p.transition_x[k], p.transition_y[k]);
    dt_draw_curve_set_point(g->transition_curve, DT_IOP_LOWLIGHT_BANDS + 1, p.transition_x[1] + 1.0,
                            p.transition_y[DT_IOP_LOWLIGHT_BANDS - 1]);
    dt_draw_curve_calc_values(g->transition_curve, 0.0, 1.0, DT_IOP_LOWLIGHT_RES, g->draw_max_xs,
                              g->draw_max_ys);
  }

  cairo_save(cr);

  // draw x positions
  cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.));
  const float arrw = DT_PIXEL_APPLY_DPI(7.0f);
  for(int k = 0; k < DT_IOP_LOWLIGHT_BANDS; k++)
  {
    cairo_move_to(cr, width * p.transition_x[k], height + inset - DT_PIXEL_APPLY_DPI(1));
    cairo_rel_line_to(cr, -arrw * .5f, 0);
    cairo_rel_line_to(cr, arrw * .5f, -arrw);
    cairo_rel_line_to(cr, arrw * .5f, arrw);
    cairo_close_path(cr);
    if(g->x_move == k)
      cairo_fill(cr);
    else
      cairo_stroke(cr);
  }

  // draw selected cursor
  cairo_translate(cr, 0, height);

  // cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
  // cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(2.));
  cairo_set_source_rgba(cr, .7, .7, .7, 1.0);

  p = *(dt_iop_lowlight_params_t *)self->params;
  dt_draw_curve_set_point(g->transition_curve, 0, p.transition_x[DT_IOP_LOWLIGHT_BANDS - 2] - 1.0,
                          p.transition_y[0]);
  for(int k = 0; k < DT_IOP_LOWLIGHT_BANDS; k++)
    dt_draw_curve_set_point(g->transition_curve, k + 1, p.transition_x[k], p.transition_y[k]);
  dt_draw_curve_set_point(g->transition_curve, DT_IOP_LOWLIGHT_BANDS + 1, p.transition_x[1] + 1.0,
                          p.transition_y[DT_IOP_LOWLIGHT_BANDS - 1]);
  dt_draw_curve_calc_values(g->transition_curve, 0.0, 1.0, DT_IOP_LOWLIGHT_RES, g->draw_xs, g->draw_ys);
  cairo_move_to(cr, 0 * width / (float)(DT_IOP_LOWLIGHT_RES - 1), -height * g->draw_ys[0]);
  for(int k = 1; k < DT_IOP_LOWLIGHT_RES; k++)
    cairo_line_to(cr, k * width / (float)(DT_IOP_LOWLIGHT_RES - 1), -height * g->draw_ys[k]);
  cairo_stroke(cr);

  // draw dots on knots
  cairo_set_source_rgb(cr, 0.7, 0.7, 0.7);
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.));
  for(int k = 0; k < DT_IOP_LOWLIGHT_BANDS; k++)
  {
    cairo_arc(cr, width * p.transition_x[k], -height * p.transition_y[k], DT_PIXEL_APPLY_DPI(3.0), 0.0,
              2.0 * M_PI);
    if(g->x_move == k)
      cairo_fill(cr);
    else
      cairo_stroke(cr);
  }

  if(g->mouse_y > 0 || g->dragging)
  {
    // draw min/max, if selected
    cairo_set_source_rgba(cr, .7, .7, .7, .6);
    cairo_move_to(cr, 0, -height * g->draw_min_ys[0]);
    for(int k = 1; k < DT_IOP_LOWLIGHT_RES; k++)
      cairo_line_to(cr, k * width / (float)(DT_IOP_LOWLIGHT_RES - 1), -height * g->draw_min_ys[k]);
    for(int k = DT_IOP_LOWLIGHT_RES - 1; k >= 0; k--)
      cairo_line_to(cr, k * width / (float)(DT_IOP_LOWLIGHT_RES - 1), -height * g->draw_max_ys[k]);
    cairo_close_path(cr);
    cairo_fill(cr);
    // draw mouse focus circle
    cairo_set_source_rgba(cr, .9, .9, .9, .5);
    const float pos = DT_IOP_LOWLIGHT_RES * g->mouse_x;
    int k = (int)pos;
    const float f = k - pos;
    if(k >= DT_IOP_LOWLIGHT_RES - 1) k = DT_IOP_LOWLIGHT_RES - 2;
    float ht = -height * (f * g->draw_ys[k] + (1 - f) * g->draw_ys[k + 1]);
    cairo_arc(cr, g->mouse_x * width, ht, g->mouse_radius * width, 0, 2. * M_PI);
    cairo_stroke(cr);
  }

  cairo_restore(cr);

  cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);

  // draw labels:
  PangoLayout *layout;
  PangoRectangle ink;
  PangoFontDescription *desc = pango_font_description_copy_static(darktable.bauhaus->pango_font_desc);
  pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
  pango_font_description_set_absolute_size(desc,(.06 * height) * PANGO_SCALE);
  layout = pango_cairo_create_layout(cr);
  pango_layout_set_font_description(layout, desc);
  cairo_set_source_rgb(cr, .1, .1, .1);

  pango_layout_set_text(layout, _("dark"), -1);
  pango_layout_get_pixel_extents(layout, &ink, NULL);
  cairo_move_to(cr, .02 * width - ink.y, .5 * (height + ink.width));
  cairo_save(cr);
  cairo_rotate(cr, -M_PI * .5f);
  pango_cairo_show_layout(cr, layout);
  cairo_restore(cr);

  pango_layout_set_text(layout, _("bright"), -1);
  pango_layout_get_pixel_extents(layout, &ink, NULL);
  cairo_move_to(cr, .98 * width - ink.height, .5 * (height + ink.width));
  cairo_save(cr);
  cairo_rotate(cr, -M_PI * .5f);
  pango_cairo_show_layout(cr, layout);
  cairo_restore(cr);


  pango_layout_set_text(layout, _("day vision"), -1);
  pango_layout_get_pixel_extents(layout, &ink, NULL);
  cairo_move_to(cr, .5 * (width - ink.width), .08 * height - ink.height);
  pango_cairo_show_layout(cr, layout);

  pango_layout_set_text(layout, _("night vision"), -1);
  pango_layout_get_pixel_extents(layout, &ink, NULL);
  cairo_move_to(cr, .5 * (width - ink.width), .97 * height - ink.height);
  pango_cairo_show_layout(cr, layout);

  pango_font_description_free(desc);
  g_object_unref(layout);
  cairo_destroy(cr);
  cairo_set_source_surface(crf, cst, 0, 0);
  cairo_paint(crf);
  cairo_surface_destroy(cst);
  return FALSE;
}

static gboolean lowlight_motion_notify(GtkWidget *widget, GdkEventMotion *event, dt_iop_module_t *self)
{
  dt_iop_lowlight_gui_data_t *g = self->gui_data;
  dt_iop_lowlight_params_t *p = self->params;
  const int inset = DT_IOP_LOWLIGHT_INSET;
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  int height = allocation.height - 2 * inset - DT_RESIZE_HANDLE_SIZE, width = allocation.width - 2 * inset;
  if(!g->dragging) g->mouse_x = CLAMP(event->x - inset, 0, width) / (float)width;
  g->mouse_y = 1.0 - CLAMP(event->y - inset, 0, height) / (float)height;
  if(g->dragging)
  {
    *p = g->drag_params;
    if(g->x_move >= 0)
    {
      const float mx = CLAMP(event->x - inset, 0, width) / (float)width;
      if(g->x_move > 0 && g->x_move < DT_IOP_LOWLIGHT_BANDS - 1)
      {
        const float minx = p->transition_x[g->x_move - 1] + 0.001f;
        const float maxx = p->transition_x[g->x_move + 1] - 0.001f;
        p->transition_x[g->x_move] = fminf(maxx, fmaxf(minx, mx));
      }
    }
    else
    {
      dt_iop_lowlight_get_params(p, g->mouse_x, g->mouse_y + g->mouse_pick, g->mouse_radius);
    }
    gtk_widget_queue_draw(widget);
    dt_dev_add_history_item_target(darktable.develop, self, TRUE, widget);
  }
  else if(event->y > height)
  {
    g->x_move = 0;
    float dist = fabs(p->transition_x[0] - g->mouse_x);
    for(int k = 1; k < DT_IOP_LOWLIGHT_BANDS; k++)
    {
      float d2 = fabs(p->transition_x[k] - g->mouse_x);
      if(d2 < dist)
      {
        g->x_move = k;
        dist = d2;
      }
    }
    gtk_widget_queue_draw(widget);
  }
  else
  {
    g->x_move = -1;
    gtk_widget_queue_draw(widget);
  }
  return TRUE;
}

static gboolean lowlight_button_press(GtkWidget *widget, GdkEventButton *event, dt_iop_module_t *self)
{
  dt_iop_lowlight_gui_data_t *g = self->gui_data;
  if(event->button == 1 && event->type == GDK_2BUTTON_PRESS)
  {
    // reset current curve
    dt_iop_lowlight_params_t *p = self->params;
    const dt_iop_lowlight_params_t *const d = self->default_params;
    for(int k = 0; k < DT_IOP_LOWLIGHT_BANDS; k++)
    {
      p->transition_x[k] = d->transition_x[k];
      p->transition_y[k] = d->transition_y[k];
    }
    dt_dev_add_history_item_target(darktable.develop, self, TRUE, widget);
    gtk_widget_queue_draw(GTK_WIDGET(g->area));
  }
  else if(event->button == 1)
  {
    g->drag_params = *(dt_iop_lowlight_params_t *)self->params;
    const int inset = DT_IOP_LOWLIGHT_INSET;
    GtkAllocation allocation;
    gtk_widget_get_allocation(widget, &allocation);
    int height = allocation.height - 2 * inset - DT_RESIZE_HANDLE_SIZE, width = allocation.width - 2 * inset;
    g->mouse_pick
        = dt_draw_curve_calc_value(g->transition_curve, CLAMP(event->x - inset, 0, width) / (float)width);
    g->mouse_pick -= 1.0 - CLAMP(event->y - inset, 0, height) / (float)height;
    g->dragging = 1;
    return TRUE;
  }
  return FALSE;
}

static gboolean lowlight_button_release(GtkWidget *widget, GdkEventButton *event, dt_iop_module_t *self)
{
  if(event->button == 1)
  {
    dt_iop_lowlight_gui_data_t *g = self->gui_data;
    g->dragging = 0;
    return TRUE;
  }
  return FALSE;
}

static gboolean lowlight_leave_notify(GtkWidget *widget, GdkEventCrossing *event, dt_iop_module_t *self)
{
  dt_iop_lowlight_gui_data_t *g = self->gui_data;
  if(!g->dragging) g->mouse_y = -1.0;
  gtk_widget_queue_draw(widget);
  return TRUE;
}

static gboolean lowlight_scrolled(GtkWidget *widget, GdkEventScroll *event, dt_iop_module_t *self)
{
  dt_iop_lowlight_gui_data_t *g = self->gui_data;

  if(dt_gui_ignore_scroll(event)) return FALSE;

  int delta_y;
  if(dt_gui_get_scroll_unit_delta(event, &delta_y))
  {
    g->mouse_radius = CLAMP(g->mouse_radius * (1.0 + 0.1 * delta_y), 0.2 / DT_IOP_LOWLIGHT_BANDS, 1.0);
    gtk_widget_queue_draw(widget);
  }

  return TRUE;
}

void gui_init(dt_iop_module_t *self)
{
  dt_iop_lowlight_gui_data_t *g = IOP_GUI_ALLOC(lowlight);
  const dt_iop_lowlight_params_t *const p = self->default_params;

  g->transition_curve = dt_draw_curve_new(0.0, 1.0, CATMULL_ROM);
  (void)dt_draw_curve_add_point(g->transition_curve, p->transition_x[DT_IOP_LOWLIGHT_BANDS - 2] - 1.0,
                                p->transition_y[DT_IOP_LOWLIGHT_BANDS - 2]);
  for(int k = 0; k < DT_IOP_LOWLIGHT_BANDS; k++)
    (void)dt_draw_curve_add_point(g->transition_curve, p->transition_x[k], p->transition_y[k]);
  (void)dt_draw_curve_add_point(g->transition_curve, p->transition_x[1] + 1.0, p->transition_y[1]);

  g->mouse_x = g->mouse_y = g->mouse_pick = -1.0;
  g->dragging = 0;
  g->x_move = -1;
  g->mouse_radius = 1.0 / DT_IOP_LOWLIGHT_BANDS;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  g->area = GTK_DRAWING_AREA(dt_ui_resize_wrap(NULL,
                                               0,
                                               "plugins/darkroom/lowlight/graphheight"));
  g_object_set_data(G_OBJECT(g->area), "iop-instance", self);
  dt_action_define_iop(self, NULL, N_("graph"), GTK_WIDGET(g->area), NULL);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->area), FALSE, FALSE, 0);

  g_signal_connect(G_OBJECT(g->area), "draw", G_CALLBACK(lowlight_draw), self);
  g_signal_connect(G_OBJECT(g->area), "button-press-event", G_CALLBACK(lowlight_button_press), self);
  g_signal_connect(G_OBJECT(g->area), "button-release-event", G_CALLBACK(lowlight_button_release), self);
  g_signal_connect(G_OBJECT(g->area), "motion-notify-event", G_CALLBACK(lowlight_motion_notify), self);
  g_signal_connect(G_OBJECT(g->area), "leave-notify-event", G_CALLBACK(lowlight_leave_notify), self);
  g_signal_connect(G_OBJECT(g->area), "scroll-event", G_CALLBACK(lowlight_scrolled), self);

  g->scale_blueness = dt_bauhaus_slider_from_params(self, "blueness");
  dt_bauhaus_slider_set_format(g->scale_blueness, "%");
  gtk_widget_set_tooltip_text(g->scale_blueness, _("blueness in shadows"));
}

void gui_cleanup(dt_iop_module_t *self)
{
  dt_iop_lowlight_gui_data_t *g = self->gui_data;
  dt_draw_curve_destroy(g->transition_curve);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
