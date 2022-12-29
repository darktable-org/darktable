/*
    This file is part of darktable,
    Copyright (C) 2011-2021 darktable developers.

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

const char **description(struct dt_iop_module_t *self)
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

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
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

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const i, void *const o,
             const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_lowlight_data_t *d = (dt_iop_lowlight_data_t *)(piece->data);
  const int ch = piece->colors;

  // empiric coefficient
  const float c = 0.5f;
  const float threshold = 0.01f;

  // scotopic white, blue saturated
  dt_aligned_pixel_t Lab_sw = { 100.0f, 0, -d->blueness };
  dt_aligned_pixel_t XYZ_sw;

  dt_Lab_to_XYZ(Lab_sw, XYZ_sw);

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(ch, i, o, roi_out, threshold, c) \
  shared(d, XYZ_sw) \
  schedule(static)
#endif
  for(size_t k = 0; k < (size_t)roi_out->width * roi_out->height; k++)
  {
    float *in = (float *)i + ch * k;
    float *out = (float *)o + ch * k;
    dt_aligned_pixel_t XYZ, XYZ_s;
    float V;
    float w;

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
    V = fminf(1.0f, fmaxf(0.0f, c * V));

    // blending coefficient from curve
    w = lookup(d->lut, in[0] / 100.f);

    XYZ_s[0] = V * XYZ_sw[0];
    XYZ_s[1] = V * XYZ_sw[1];
    XYZ_s[2] = V * XYZ_sw[2];

    XYZ[0] = w * XYZ[0] + (1.0f - w) * XYZ_s[0];
    XYZ[1] = w * XYZ[1] + (1.0f - w) * XYZ_s[1];
    XYZ[2] = w * XYZ[2] + (1.0f - w) * XYZ_s[2];

    dt_XYZ_to_Lab(XYZ, out);

    out[3] = in[3];
  }
}

#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_lowlight_data_t *d = (dt_iop_lowlight_data_t *)piece->data;
  dt_iop_lowlight_global_data_t *gd = (dt_iop_lowlight_global_data_t *)self->global_data;

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
  if(dev_m == NULL) goto error;

  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_lowlight, width, height,
    CLARG(dev_in), CLARG(dev_out), CLARG(width), CLARG(height), CLARG(XYZ_sw), CLARG(dev_m));
  if(err != CL_SUCCESS) goto error;

  dt_opencl_release_mem_object(dev_m);
  return TRUE;

error:
  dt_opencl_release_mem_object(dev_m);
  dt_print(DT_DEBUG_OPENCL, "[opencl_lowlight] couldn't enqueue kernel! %s\n", cl_errstr(err));
  return FALSE;
}
#endif


void init_global(dt_iop_module_so_t *module)
{
  const int program = 2; // basic.cl from programs.conf
  dt_iop_lowlight_global_data_t *gd
      = (dt_iop_lowlight_global_data_t *)malloc(sizeof(dt_iop_lowlight_global_data_t));
  module->data = gd;
  gd->kernel_lowlight = dt_opencl_create_kernel(program, "lowlight");
}


void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_lowlight_global_data_t *gd = (dt_iop_lowlight_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_lowlight);
  free(module->data);
  module->data = NULL;
}


void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_lowlight_data_t *d = (dt_iop_lowlight_data_t *)(piece->data);
  dt_iop_lowlight_params_t *p = (dt_iop_lowlight_params_t *)p1;
  dt_draw_curve_set_point(d->curve, 0, p->transition_x[DT_IOP_LOWLIGHT_BANDS - 2] - 1.0, p->transition_y[0]);
  for(int k = 0; k < DT_IOP_LOWLIGHT_BANDS; k++)
    dt_draw_curve_set_point(d->curve, k + 1, p->transition_x[k], p->transition_y[k]);
  dt_draw_curve_set_point(d->curve, DT_IOP_LOWLIGHT_BANDS + 1, p->transition_x[1] + 1.0,
                          p->transition_y[DT_IOP_LOWLIGHT_BANDS - 1]);
  dt_draw_curve_calc_values(d->curve, 0.0, 1.0, DT_IOP_LOWLIGHT_LUT_RES, NULL, d->lut);
  d->blueness = p->blueness;
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_lowlight_data_t *d = (dt_iop_lowlight_data_t *)malloc(sizeof(dt_iop_lowlight_data_t));
  dt_iop_lowlight_params_t *default_params = (dt_iop_lowlight_params_t *)self->default_params;
  piece->data = (void *)d;
  d->curve = dt_draw_curve_new(0.0, 1.0, CATMULL_ROM);
  (void)dt_draw_curve_add_point(d->curve, default_params->transition_x[DT_IOP_LOWLIGHT_BANDS - 2] - 1.0,
                                default_params->transition_y[DT_IOP_LOWLIGHT_BANDS - 2]);
  for(int k = 0; k < DT_IOP_LOWLIGHT_BANDS; k++)
    (void)dt_draw_curve_add_point(d->curve, default_params->transition_x[k], default_params->transition_y[k]);
  (void)dt_draw_curve_add_point(d->curve, default_params->transition_x[1] + 1.0,
                                default_params->transition_y[1]);
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  // clean up everything again.
  dt_iop_lowlight_data_t *d = (dt_iop_lowlight_data_t *)(piece->data);
  dt_draw_curve_destroy(d->curve);
  free(piece->data);
  piece->data = NULL;
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_lowlight_gui_data_t *g = (dt_iop_lowlight_gui_data_t *)self->gui_data;
  dt_iop_lowlight_params_t *p = (dt_iop_lowlight_params_t *)self->params;
  dt_bauhaus_slider_set(g->scale_blueness, p->blueness);
  dt_iop_cancel_history_update(self);
  gtk_widget_queue_draw(self->widget);
}

void init(dt_iop_module_t *module)
{
  dt_iop_default_init(module);

  dt_iop_lowlight_params_t *d = module->default_params;

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

static gboolean lowlight_draw(GtkWidget *widget, cairo_t *crf, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_lowlight_gui_data_t *c = (dt_iop_lowlight_gui_data_t *)self->gui_data;
  dt_iop_lowlight_params_t p = *(dt_iop_lowlight_params_t *)self->params;

  dt_draw_curve_set_point(c->transition_curve, 0, p.transition_x[DT_IOP_LOWLIGHT_BANDS - 2] - 1.0,
                          p.transition_y[0]);
  for(int k = 0; k < DT_IOP_LOWLIGHT_BANDS; k++)
    dt_draw_curve_set_point(c->transition_curve, k + 1, p.transition_x[k], p.transition_y[k]);
  dt_draw_curve_set_point(c->transition_curve, DT_IOP_LOWLIGHT_BANDS + 1, p.transition_x[1] + 1.0,
                          p.transition_y[DT_IOP_LOWLIGHT_BANDS - 1]);

  const int inset = DT_IOP_LOWLIGHT_INSET;
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  int width = allocation.width, height = allocation.height;
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


  if(c->mouse_y > 0 || c->dragging)
  {
    // draw min/max curves:
    dt_iop_lowlight_get_params(&p, c->mouse_x, 1., c->mouse_radius);
    dt_draw_curve_set_point(c->transition_curve, 0, p.transition_x[DT_IOP_LOWLIGHT_BANDS - 2] - 1.0,
                            p.transition_y[0]);
    for(int k = 0; k < DT_IOP_LOWLIGHT_BANDS; k++)
      dt_draw_curve_set_point(c->transition_curve, k + 1, p.transition_x[k], p.transition_y[k]);
    dt_draw_curve_set_point(c->transition_curve, DT_IOP_LOWLIGHT_BANDS + 1, p.transition_x[1] + 1.0,
                            p.transition_y[DT_IOP_LOWLIGHT_BANDS - 1]);
    dt_draw_curve_calc_values(c->transition_curve, 0.0, 1.0, DT_IOP_LOWLIGHT_RES, c->draw_min_xs,
                              c->draw_min_ys);

    p = *(dt_iop_lowlight_params_t *)self->params;
    dt_iop_lowlight_get_params(&p, c->mouse_x, .0, c->mouse_radius);
    dt_draw_curve_set_point(c->transition_curve, 0, p.transition_x[DT_IOP_LOWLIGHT_BANDS - 2] - 1.0,
                            p.transition_y[0]);
    for(int k = 0; k < DT_IOP_LOWLIGHT_BANDS; k++)
      dt_draw_curve_set_point(c->transition_curve, k + 1, p.transition_x[k], p.transition_y[k]);
    dt_draw_curve_set_point(c->transition_curve, DT_IOP_LOWLIGHT_BANDS + 1, p.transition_x[1] + 1.0,
                            p.transition_y[DT_IOP_LOWLIGHT_BANDS - 1]);
    dt_draw_curve_calc_values(c->transition_curve, 0.0, 1.0, DT_IOP_LOWLIGHT_RES, c->draw_max_xs,
                              c->draw_max_ys);
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
    if(c->x_move == k)
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
  dt_draw_curve_set_point(c->transition_curve, 0, p.transition_x[DT_IOP_LOWLIGHT_BANDS - 2] - 1.0,
                          p.transition_y[0]);
  for(int k = 0; k < DT_IOP_LOWLIGHT_BANDS; k++)
    dt_draw_curve_set_point(c->transition_curve, k + 1, p.transition_x[k], p.transition_y[k]);
  dt_draw_curve_set_point(c->transition_curve, DT_IOP_LOWLIGHT_BANDS + 1, p.transition_x[1] + 1.0,
                          p.transition_y[DT_IOP_LOWLIGHT_BANDS - 1]);
  dt_draw_curve_calc_values(c->transition_curve, 0.0, 1.0, DT_IOP_LOWLIGHT_RES, c->draw_xs, c->draw_ys);
  cairo_move_to(cr, 0 * width / (float)(DT_IOP_LOWLIGHT_RES - 1), -height * c->draw_ys[0]);
  for(int k = 1; k < DT_IOP_LOWLIGHT_RES; k++)
    cairo_line_to(cr, k * width / (float)(DT_IOP_LOWLIGHT_RES - 1), -height * c->draw_ys[k]);
  cairo_stroke(cr);

  // draw dots on knots
  cairo_set_source_rgb(cr, 0.7, 0.7, 0.7);
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.));
  for(int k = 0; k < DT_IOP_LOWLIGHT_BANDS; k++)
  {
    cairo_arc(cr, width * p.transition_x[k], -height * p.transition_y[k], DT_PIXEL_APPLY_DPI(3.0), 0.0,
              2.0 * M_PI);
    if(c->x_move == k)
      cairo_fill(cr);
    else
      cairo_stroke(cr);
  }

  if(c->mouse_y > 0 || c->dragging)
  {
    // draw min/max, if selected
    cairo_set_source_rgba(cr, .7, .7, .7, .6);
    cairo_move_to(cr, 0, -height * c->draw_min_ys[0]);
    for(int k = 1; k < DT_IOP_LOWLIGHT_RES; k++)
      cairo_line_to(cr, k * width / (float)(DT_IOP_LOWLIGHT_RES - 1), -height * c->draw_min_ys[k]);
    for(int k = DT_IOP_LOWLIGHT_RES - 1; k >= 0; k--)
      cairo_line_to(cr, k * width / (float)(DT_IOP_LOWLIGHT_RES - 1), -height * c->draw_max_ys[k]);
    cairo_close_path(cr);
    cairo_fill(cr);
    // draw mouse focus circle
    cairo_set_source_rgba(cr, .9, .9, .9, .5);
    const float pos = DT_IOP_LOWLIGHT_RES * c->mouse_x;
    int k = (int)pos;
    const float f = k - pos;
    if(k >= DT_IOP_LOWLIGHT_RES - 1) k = DT_IOP_LOWLIGHT_RES - 2;
    float ht = -height * (f * c->draw_ys[k] + (1 - f) * c->draw_ys[k + 1]);
    cairo_arc(cr, c->mouse_x * width, ht, c->mouse_radius * width, 0, 2. * M_PI);
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
  return TRUE;
}

static gboolean lowlight_motion_notify(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_lowlight_gui_data_t *c = (dt_iop_lowlight_gui_data_t *)self->gui_data;
  dt_iop_lowlight_params_t *p = (dt_iop_lowlight_params_t *)self->params;
  const int inset = DT_IOP_LOWLIGHT_INSET;
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  int height = allocation.height - 2 * inset, width = allocation.width - 2 * inset;
  if(!c->dragging) c->mouse_x = CLAMP(event->x - inset, 0, width) / (float)width;
  c->mouse_y = 1.0 - CLAMP(event->y - inset, 0, height) / (float)height;
  if(c->dragging)
  {
    *p = c->drag_params;
    if(c->x_move >= 0)
    {
      const float mx = CLAMP(event->x - inset, 0, width) / (float)width;
      if(c->x_move > 0 && c->x_move < DT_IOP_LOWLIGHT_BANDS - 1)
      {
        const float minx = p->transition_x[c->x_move - 1] + 0.001f;
        const float maxx = p->transition_x[c->x_move + 1] - 0.001f;
        p->transition_x[c->x_move] = fminf(maxx, fmaxf(minx, mx));
      }
    }
    else
    {
      dt_iop_lowlight_get_params(p, c->mouse_x, c->mouse_y + c->mouse_pick, c->mouse_radius);
    }
    gtk_widget_queue_draw(widget);
    dt_iop_queue_history_update(self, FALSE);
  }
  else if(event->y > height)
  {
    c->x_move = 0;
    float dist = fabs(p->transition_x[0] - c->mouse_x);
    for(int k = 1; k < DT_IOP_LOWLIGHT_BANDS; k++)
    {
      float d2 = fabs(p->transition_x[k] - c->mouse_x);
      if(d2 < dist)
      {
        c->x_move = k;
        dist = d2;
      }
    }
    gtk_widget_queue_draw(widget);
  }
  else
  {
    c->x_move = -1;
    gtk_widget_queue_draw(widget);
  }
  return TRUE;
}

static gboolean lowlight_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(event->button == 1 && event->type == GDK_2BUTTON_PRESS)
  {
    // reset current curve
    dt_iop_lowlight_params_t *p = (dt_iop_lowlight_params_t *)self->params;
    dt_iop_lowlight_params_t *d = (dt_iop_lowlight_params_t *)self->default_params;
    for(int k = 0; k < DT_IOP_LOWLIGHT_BANDS; k++)
    {
      p->transition_x[k] = d->transition_x[k];
      p->transition_y[k] = d->transition_y[k];
    }
    dt_dev_add_history_item(darktable.develop, self, TRUE, TRUE);
    gtk_widget_queue_draw(self->widget);
  }
  else if(event->button == 1)
  {
    dt_iop_lowlight_gui_data_t *c = (dt_iop_lowlight_gui_data_t *)self->gui_data;
    c->drag_params = *(dt_iop_lowlight_params_t *)self->params;
    const int inset = DT_IOP_LOWLIGHT_INSET;
    GtkAllocation allocation;
    gtk_widget_get_allocation(widget, &allocation);
    int height = allocation.height - 2 * inset, width = allocation.width - 2 * inset;
    c->mouse_pick
        = dt_draw_curve_calc_value(c->transition_curve, CLAMP(event->x - inset, 0, width) / (float)width);
    c->mouse_pick -= 1.0 - CLAMP(event->y - inset, 0, height) / (float)height;
    c->dragging = 1;
    return TRUE;
  }
  return FALSE;
}

static gboolean lowlight_button_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
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

static gboolean lowlight_leave_notify(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_lowlight_gui_data_t *c = (dt_iop_lowlight_gui_data_t *)self->gui_data;
  if(!c->dragging) c->mouse_y = -1.0;
  gtk_widget_queue_draw(widget);
  return TRUE;
}

static gboolean lowlight_scrolled(GtkWidget *widget, GdkEventScroll *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_lowlight_gui_data_t *c = (dt_iop_lowlight_gui_data_t *)self->gui_data;

  if(dt_gui_ignore_scroll(event)) return FALSE;

  int delta_y;
  if(dt_gui_get_scroll_unit_deltas(event, NULL, &delta_y))
  {
    if(dt_modifier_is(event->state, GDK_CONTROL_MASK))
    {
      //adjust aspect
      const int aspect = dt_conf_get_int("plugins/darkroom/lowlight/aspect_percent");
      dt_conf_set_int("plugins/darkroom/lowlight/aspect_percent", aspect + delta_y);
      dtgtk_drawing_area_set_aspect_ratio(widget, aspect / 100.0);
    }
    else
    {
      c->mouse_radius = CLAMP(c->mouse_radius * (1.0 + 0.1 * delta_y), 0.2 / DT_IOP_LOWLIGHT_BANDS, 1.0);
      gtk_widget_queue_draw(widget);
    }
  }

  return TRUE;
}

void gui_init(struct dt_iop_module_t *self)
{
  dt_iop_lowlight_gui_data_t *c = IOP_GUI_ALLOC(lowlight);
  dt_iop_lowlight_params_t *p = (dt_iop_lowlight_params_t *)self->default_params;

  c->transition_curve = dt_draw_curve_new(0.0, 1.0, CATMULL_ROM);
  (void)dt_draw_curve_add_point(c->transition_curve, p->transition_x[DT_IOP_LOWLIGHT_BANDS - 2] - 1.0,
                                p->transition_y[DT_IOP_LOWLIGHT_BANDS - 2]);
  for(int k = 0; k < DT_IOP_LOWLIGHT_BANDS; k++)
    (void)dt_draw_curve_add_point(c->transition_curve, p->transition_x[k], p->transition_y[k]);
  (void)dt_draw_curve_add_point(c->transition_curve, p->transition_x[1] + 1.0, p->transition_y[1]);

  c->mouse_x = c->mouse_y = c->mouse_pick = -1.0;
  c->dragging = 0;
  c->x_move = -1;
  self->timeout_handle = 0;
  c->mouse_radius = 1.0 / DT_IOP_LOWLIGHT_BANDS;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  const float aspect = dt_conf_get_int("plugins/darkroom/lowlight/aspect_percent") / 100.0;
  c->area = GTK_DRAWING_AREA(dtgtk_drawing_area_new_with_aspect_ratio(aspect));
  g_object_set_data(G_OBJECT(c->area), "iop-instance", self);
  dt_action_define_iop(self, NULL, N_("graph"), GTK_WIDGET(c->area), NULL);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(c->area), FALSE, FALSE, 0);

  gtk_widget_add_events(GTK_WIDGET(c->area), GDK_POINTER_MOTION_MASK | darktable.gui->scroll_mask
                                           | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK
                                           | GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK);
  g_signal_connect(G_OBJECT(c->area), "draw", G_CALLBACK(lowlight_draw), self);
  g_signal_connect(G_OBJECT(c->area), "button-press-event", G_CALLBACK(lowlight_button_press), self);
  g_signal_connect(G_OBJECT(c->area), "button-release-event", G_CALLBACK(lowlight_button_release), self);
  g_signal_connect(G_OBJECT(c->area), "motion-notify-event", G_CALLBACK(lowlight_motion_notify), self);
  g_signal_connect(G_OBJECT(c->area), "leave-notify-event", G_CALLBACK(lowlight_leave_notify), self);
  g_signal_connect(G_OBJECT(c->area), "scroll-event", G_CALLBACK(lowlight_scrolled), self);

  c->scale_blueness = dt_bauhaus_slider_from_params(self, "blueness");
  dt_bauhaus_slider_set_format(c->scale_blueness, "%");
  gtk_widget_set_tooltip_text(c->scale_blueness, _("blueness in shadows"));
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  dt_iop_lowlight_gui_data_t *c = (dt_iop_lowlight_gui_data_t *)self->gui_data;
  dt_draw_curve_destroy(c->transition_curve);
  dt_iop_cancel_history_update(self);

  IOP_GUI_FREE;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

