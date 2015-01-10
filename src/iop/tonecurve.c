/*
    This file is part of darktable,
    copyright (c) 2009--2014 johannes hanika.
    copyright (c) 2014 Ulrich Pegelow.

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
#include <stdint.h>

#include "develop/develop.h"
#include "develop/imageop.h"
#include "control/control.h"
#include "bauhaus/bauhaus.h"
#include "gui/draw.h"
#include "dtgtk/drawingarea.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "common/opencl.h"
#include "libs/colorpicker.h"

#define DT_GUI_CURVE_EDITOR_INSET DT_PIXEL_APPLY_DPI(1)
#define DT_GUI_CURVE_INFL .3f

#define DT_IOP_TONECURVE_RES 64
#define DT_IOP_TONECURVE_MAXNODES 20

DT_MODULE_INTROSPECTION(4, dt_iop_tonecurve_params_t)

static gboolean dt_iop_tonecurve_draw(GtkWidget *widget, cairo_t *crf, gpointer user_data);
static gboolean dt_iop_tonecurve_motion_notify(GtkWidget *widget, GdkEventMotion *event, gpointer user_data);
static gboolean dt_iop_tonecurve_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data);
static gboolean dt_iop_tonecurve_leave_notify(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data);
static gboolean dt_iop_tonecurve_enter_notify(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data);


#define DT_IOP_TONECURVE_RES 64
#define DT_IOP_TONECURVE_MAXNODES 20

typedef enum tonecurve_channel_t
{
  ch_L = 0,
  ch_a = 1,
  ch_b = 2,
  ch_max = 3
} tonecurve_channel_t;

typedef struct dt_iop_tonecurve_node_t
{
  float x;
  float y;
} dt_iop_tonecurve_node_t;


// parameter structure of tonecurve 1st version, needed for use in legacy_params()
typedef struct dt_iop_tonecurve_params1_t
{
  float tonecurve_x[6], tonecurve_y[6];
  int tonecurve_preset;
} dt_iop_tonecurve_params1_t;

// parameter structure of tonecurve 3rd version, needed for use in legacy_params()
typedef struct dt_iop_tonecurve_params3_t
{
  dt_iop_tonecurve_node_t tonecurve[3][DT_IOP_TONECURVE_MAXNODES]; // three curves (L, a, b) with max number
                                                                   // of nodes
  int tonecurve_nodes[3];
  int tonecurve_type[3];
  int tonecurve_autoscale_ab;
  int tonecurve_preset;
} dt_iop_tonecurve_params3_t;

typedef struct dt_iop_tonecurve_params_t
{
  dt_iop_tonecurve_node_t tonecurve[3][DT_IOP_TONECURVE_MAXNODES]; // three curves (L, a, b) with max number
                                                                   // of nodes
  int tonecurve_nodes[3];
  int tonecurve_type[3];
  int tonecurve_autoscale_ab;
  int tonecurve_preset;
  int tonecurve_unbound_ab;
} dt_iop_tonecurve_params_t;


typedef struct dt_iop_tonecurve_gui_data_t
{
  dt_draw_curve_t *minmax_curve[3]; // curves for gui to draw
  int minmax_curve_nodes[3];
  int minmax_curve_type[3];
  GtkBox *hbox;
  GtkDrawingArea *area;
  GtkSizeGroup *sizegroup;
  GtkWidget *autoscale_ab;
  GtkNotebook *channel_tabs;
  tonecurve_channel_t channel;
  double mouse_x, mouse_y;
  int selected;
  float draw_xs[DT_IOP_TONECURVE_RES], draw_ys[DT_IOP_TONECURVE_RES];
  float draw_min_xs[DT_IOP_TONECURVE_RES], draw_min_ys[DT_IOP_TONECURVE_RES];
  float draw_max_xs[DT_IOP_TONECURVE_RES], draw_max_ys[DT_IOP_TONECURVE_RES];
} dt_iop_tonecurve_gui_data_t;

typedef struct dt_iop_tonecurve_data_t
{
  dt_draw_curve_t *curve[3];     // curves for gegl nodes and pixel processing
  int curve_nodes[3];            // number of nodes
  int curve_type[3];             // curve style (e.g. CUBIC_SPLINE)
  float table[3][0x10000];       // precomputed look-up tables for tone curve
  float unbounded_coeffs_L[3];   // approximation for extrapolation of L
  float unbounded_coeffs_ab[12]; // approximation for extrapolation of ab (left and right)
  int autoscale_ab;
  int unbound_ab;
} dt_iop_tonecurve_data_t;

typedef struct dt_iop_tonecurve_global_data_t
{
  int kernel_tonecurve;
} dt_iop_tonecurve_global_data_t;


const char *name()
{
  return _("tone curve");
}


int groups()
{
  return IOP_GROUP_TONE;
}

int flags()
{
  return IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING;
}

int legacy_params(dt_iop_module_t *self, const void *const old_params, const int old_version,
                  void *new_params, const int new_version)
{
  if(old_version == 1 && new_version == 4)
  {
    dt_iop_tonecurve_params1_t *o = (dt_iop_tonecurve_params1_t *)old_params;
    dt_iop_tonecurve_params_t *n = (dt_iop_tonecurve_params_t *)new_params;

    // start with a fresh copy of default parameters
    // unfortunately default_params aren't inited at this stage.
    *n = (dt_iop_tonecurve_params_t){ { { { 0.0, 0.0 }, { 1.0, 1.0 } },
                                        { { 0.0, 0.0 }, { 0.5, 0.5 }, { 1.0, 1.0 } },
                                        { { 0.0, 0.0 }, { 0.5, 0.5 }, { 1.0, 1.0 } } },
                                      { 2, 3, 3 },
                                      { MONOTONE_HERMITE, MONOTONE_HERMITE, MONOTONE_HERMITE },
                                      1,
                                      0,
                                      1 };
    for(int k = 0; k < 6; k++) n->tonecurve[ch_L][k].x = o->tonecurve_x[k];
    for(int k = 0; k < 6; k++) n->tonecurve[ch_L][k].y = o->tonecurve_y[k];
    n->tonecurve_nodes[ch_L] = 6;
    n->tonecurve_type[ch_L] = CUBIC_SPLINE;
    n->tonecurve_autoscale_ab = 1;
    n->tonecurve_preset = o->tonecurve_preset;
    n->tonecurve_unbound_ab = 0;
    return 0;
  }
  else if(old_version == 2 && new_version == 4)
  {
    // version 2 never really materialized so there should be no legacy history stacks of that version around
    return 1;
  }
  else if(old_version == 3 && new_version == 4)
  {
    dt_iop_tonecurve_params3_t *o = (dt_iop_tonecurve_params3_t *)old_params;
    dt_iop_tonecurve_params_t *n = (dt_iop_tonecurve_params_t *)new_params;

    memcpy(n->tonecurve, o->tonecurve, sizeof(n->tonecurve));
    memcpy(n->tonecurve_nodes, o->tonecurve_nodes, sizeof(n->tonecurve_nodes));
    memcpy(n->tonecurve_type, o->tonecurve_type, sizeof(n->tonecurve_type));
    n->tonecurve_autoscale_ab = o->tonecurve_autoscale_ab;
    n->tonecurve_preset = o->tonecurve_preset;
    n->tonecurve_unbound_ab = 0;
    return 0;
  }

  return 1;
}

#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_tonecurve_data_t *d = (dt_iop_tonecurve_data_t *)piece->data;
  dt_iop_tonecurve_global_data_t *gd = (dt_iop_tonecurve_global_data_t *)self->data;
  cl_mem dev_L = NULL;
  cl_mem dev_a = NULL;
  cl_mem dev_b = NULL;
  cl_mem dev_coeffs_L = NULL;
  cl_mem dev_coeffs_ab = NULL;
  cl_int err = -999;

  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;
  const int autoscale_ab = d->autoscale_ab;
  const int unbound_ab = d->unbound_ab;
  const float low_approximation = d->table[0][(int)(0.01f * 0xfffful)];

  dev_L = dt_opencl_copy_host_to_device(devid, d->table[ch_L], 256, 256, sizeof(float));
  if(dev_L == NULL) goto error;

  dev_a = dt_opencl_copy_host_to_device(devid, d->table[ch_a], 256, 256, sizeof(float));
  if(dev_a == NULL) goto error;

  dev_b = dt_opencl_copy_host_to_device(devid, d->table[ch_b], 256, 256, sizeof(float));
  if(dev_b == NULL) goto error;

  dev_coeffs_L = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * 3, d->unbounded_coeffs_L);
  if(dev_coeffs_L == NULL) goto error;

  dev_coeffs_ab = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * 12, d->unbounded_coeffs_ab);
  if(dev_coeffs_ab == NULL) goto error;

  size_t sizes[] = { ROUNDUPWD(width), ROUNDUPHT(height), 1 };
  dt_opencl_set_kernel_arg(devid, gd->kernel_tonecurve, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_tonecurve, 1, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(devid, gd->kernel_tonecurve, 2, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_tonecurve, 3, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_tonecurve, 4, sizeof(cl_mem), (void *)&dev_L);
  dt_opencl_set_kernel_arg(devid, gd->kernel_tonecurve, 5, sizeof(cl_mem), (void *)&dev_a);
  dt_opencl_set_kernel_arg(devid, gd->kernel_tonecurve, 6, sizeof(cl_mem), (void *)&dev_b);
  dt_opencl_set_kernel_arg(devid, gd->kernel_tonecurve, 7, sizeof(int), (void *)&autoscale_ab);
  dt_opencl_set_kernel_arg(devid, gd->kernel_tonecurve, 8, sizeof(int), (void *)&unbound_ab);
  dt_opencl_set_kernel_arg(devid, gd->kernel_tonecurve, 9, sizeof(cl_mem), (void *)&dev_coeffs_L);
  dt_opencl_set_kernel_arg(devid, gd->kernel_tonecurve, 10, sizeof(cl_mem), (void *)&dev_coeffs_ab);
  dt_opencl_set_kernel_arg(devid, gd->kernel_tonecurve, 11, sizeof(float), (void *)&low_approximation);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_tonecurve, sizes);

  if(err != CL_SUCCESS) goto error;
  dt_opencl_release_mem_object(dev_L);
  dt_opencl_release_mem_object(dev_a);
  dt_opencl_release_mem_object(dev_b);
  dt_opencl_release_mem_object(dev_coeffs_L);
  dt_opencl_release_mem_object(dev_coeffs_ab);
  return TRUE;

error:
  if(dev_L != NULL) dt_opencl_release_mem_object(dev_L);
  if(dev_a != NULL) dt_opencl_release_mem_object(dev_a);
  if(dev_b != NULL) dt_opencl_release_mem_object(dev_b);
  if(dev_coeffs_L != NULL) dt_opencl_release_mem_object(dev_coeffs_L);
  if(dev_coeffs_ab != NULL) dt_opencl_release_mem_object(dev_coeffs_ab);
  dt_print(DT_DEBUG_OPENCL, "[opencl_tonecurve] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *i, void *o,
             const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  const int ch = piece->colors;
  dt_iop_tonecurve_data_t *d = (dt_iop_tonecurve_data_t *)(piece->data);

  const float xm_L = 1.0f / d->unbounded_coeffs_L[0];
  const float xm_ar = 1.0f / d->unbounded_coeffs_ab[0];
  const float xm_al = 1.0f - 1.0f / d->unbounded_coeffs_ab[3];
  const float xm_br = 1.0f / d->unbounded_coeffs_ab[6];
  const float xm_bl = 1.0f - 1.0f / d->unbounded_coeffs_ab[9];
  const float low_approximation = d->table[0][(int)(0.01f * 0xfffful)];

  const int width = roi_out->width;
  const int height = roi_out->height;
  const int autoscale_ab = d->autoscale_ab;
  const int unbound_ab = d->unbound_ab;

#ifdef _OPENMP
#pragma omp parallel for default(none) shared(i, o, d) schedule(static)
#endif
  for(int k = 0; k < height; k++)
  {
    float *in = ((float *)i) + (size_t)k * ch * width;
    float *out = ((float *)o) + (size_t)k * ch * width;

    for(int j = 0; j < width; j++, in += ch, out += ch)
    {
      const float L_in = in[0] / 100.0f;

      out[0] = (L_in < xm_L) ? d->table[ch_L][CLAMP((int)(L_in * 0xfffful), 0, 0xffff)]
                             : dt_iop_eval_exp(d->unbounded_coeffs_L, L_in);

      if(autoscale_ab == 0)
      {
        const float a_in = (in[1] + 128.0f) / 256.0f;
        const float b_in = (in[2] + 128.0f) / 256.0f;

        if(unbound_ab == 0)
        {
          // old style handling of a/b curves: only lut lookup with clamping
          out[1] = d->table[ch_a][CLAMP((int)(a_in * 0xfffful), 0, 0xffff)];
          out[2] = d->table[ch_b][CLAMP((int)(b_in * 0xfffful), 0, 0xffff)];
        }
        else
        {
          // new style handling of a/b curves: lut lookup with two-sided extrapolation;
          // mind the x-axis reversal for the left-handed side
          out[1] = (a_in > xm_ar)
                       ? dt_iop_eval_exp(d->unbounded_coeffs_ab, a_in)
                       : ((a_in < xm_al) ? dt_iop_eval_exp(d->unbounded_coeffs_ab + 3, 1.0f - a_in)
                                         : d->table[ch_a][CLAMP((int)(a_in * 0xfffful), 0, 0xffff)]);
          out[2] = (b_in > xm_br)
                       ? dt_iop_eval_exp(d->unbounded_coeffs_ab + 6, b_in)
                       : ((b_in < xm_bl) ? dt_iop_eval_exp(d->unbounded_coeffs_ab + 9, 1.0f - b_in)
                                         : d->table[ch_b][CLAMP((int)(b_in * 0xfffful), 0, 0xffff)]);
        }
      }
      else
      {
        // in Lab: correct compressed Luminance for saturation:
        if(L_in > 0.01f)
        {
          out[1] = in[1] * out[0] / in[0];
          out[2] = in[2] * out[0] / in[0];
        }
        else
        {
          out[1] = in[1] * low_approximation;
          out[2] = in[2] * low_approximation;
        }
      }

      out[3] = in[3];
    }
  }
}

static const struct
{
  const char *name;
  const char *maker;
  const char *model;
  int iso_min;
  int iso_max;
  struct dt_iop_tonecurve_params_t preset;
} preset_camera_curves[] = {
    // This is where you can paste the line provided by dt-curve-tool
    // Here is a valid example for you to compare
    // clang-format off
    // nikon d750 by Edouard Gomez
    {"NIKON D750", "NIKON CORPORATION", "NIKON D750", 0, 51200, {{{{0.000000, 0.000000}, {0.083508, 0.073677}, {0.212191, 0.274799}, {0.397095, 0.594035}, {0.495025, 0.714660}, {0.683565, 0.878550}, {0.854059, 0.950927}, {1.000000, 1.000000}, },{{0.000000, 0.000000}, {0.125000, 0.125000}, {0.250000, 0.250000}, {0.375000, 0.375000}, {0.500000, 0.500000}, {0.625000, 0.625000}, {0.750000, 0.750000}, {0.875000, 0.875000}, },{{0.000000, 0.000000}, {0.125000, 0.125000}, {0.250000, 0.250000}, {0.375000, 0.375000}, {0.500000, 0.500000}, {0.625000, 0.625000}, {0.750000, 0.750000}, {0.875000, 0.875000}, },}, {8, 8, 8}, {2, 2, 2}, 1, 0, 0}},
    // nikon d7000 by Edouard Gomez
    {"NIKON D7000", "NIKON CORPORATION", "NIKON D7000", 0, 51200, {{{{0.000000, 0.000000}, {0.110633, 0.111192}, {0.209771, 0.286963}, {0.355888, 0.561236}, {0.454987, 0.673098}, {0.769212, 0.920485}, {0.800468, 0.933428}, {1.000000, 1.000000}, },{{0.000000, 0.000000}, {0.125000, 0.125000}, {0.250000, 0.250000}, {0.375000, 0.375000}, {0.500000, 0.500000}, {0.625000, 0.625000}, {0.750000, 0.750000}, {0.875000, 0.875000}, },{{0.000000, 0.000000}, {0.125000, 0.125000}, {0.250000, 0.250000}, {0.375000, 0.375000}, {0.500000, 0.500000}, {0.625000, 0.625000}, {0.750000, 0.750000}, {0.875000, 0.875000}, },}, {8, 8, 8}, {2, 2, 2}, 1, 0, 0}},
    // clang-format on
  };

void init_presets(dt_iop_module_so_t *self)
{
  dt_iop_tonecurve_params_t p;
  memset(&p, 0, sizeof(p));
  p.tonecurve_nodes[ch_L] = 6;
  p.tonecurve_nodes[ch_a] = 7;
  p.tonecurve_nodes[ch_b] = 7;
  p.tonecurve_type[ch_L] = CUBIC_SPLINE;
  p.tonecurve_type[ch_a] = CUBIC_SPLINE;
  p.tonecurve_type[ch_b] = CUBIC_SPLINE;
  p.tonecurve_preset = 0;
  p.tonecurve_autoscale_ab = 1;
  p.tonecurve_unbound_ab = 1;

  float linear_L[6] = { 0.0, 0.08, 0.4, 0.6, 0.92, 1.0 };
  float linear_ab[7] = { 0.0, 0.08, 0.3, 0.5, 0.7, 0.92, 1.0 };

  // linear a, b curves for presets
  for(int k = 0; k < 7; k++) p.tonecurve[ch_a][k].x = linear_ab[k];
  for(int k = 0; k < 7; k++) p.tonecurve[ch_a][k].y = linear_ab[k];
  for(int k = 0; k < 7; k++) p.tonecurve[ch_b][k].x = linear_ab[k];
  for(int k = 0; k < 7; k++) p.tonecurve[ch_b][k].y = linear_ab[k];


  // More useful low contrast curve (based on Samsung NX -2 Contrast)
  p.tonecurve[ch_L][0].x = 0.000000;
  p.tonecurve[ch_L][1].x = 0.003862;
  p.tonecurve[ch_L][2].x = 0.076613;
  p.tonecurve[ch_L][3].x = 0.169355;
  p.tonecurve[ch_L][4].x = 0.774194;
  p.tonecurve[ch_L][5].x = 1.000000;
  p.tonecurve[ch_L][0].y = 0.000000;
  p.tonecurve[ch_L][1].y = 0.007782;
  p.tonecurve[ch_L][2].y = 0.156182;
  p.tonecurve[ch_L][3].y = 0.290352;
  p.tonecurve[ch_L][4].y = 0.773852;
  p.tonecurve[ch_L][5].y = 1.000000;
  dt_gui_presets_add_generic(_("low contrast"), self->op, self->version(), &p, sizeof(p), 1);

  for(int k = 0; k < 6; k++) p.tonecurve[ch_L][k].x = linear_L[k];
  for(int k = 0; k < 6; k++) p.tonecurve[ch_L][k].y = linear_L[k];
  dt_gui_presets_add_generic(_("linear"), self->op, self->version(), &p, sizeof(p), 1);

  for(int k = 0; k < 6; k++) p.tonecurve[ch_L][k].x = linear_L[k];
  for(int k = 0; k < 6; k++) p.tonecurve[ch_L][k].y = linear_L[k];
  p.tonecurve[ch_L][1].y -= 0.03;
  p.tonecurve[ch_L][4].y += 0.03;
  p.tonecurve[ch_L][2].y -= 0.03;
  p.tonecurve[ch_L][3].y += 0.03;
  for(int k = 1; k < 5; k++) p.tonecurve[ch_L][k].x = powf(p.tonecurve[ch_L][k].x, 2.2f);
  for(int k = 1; k < 5; k++) p.tonecurve[ch_L][k].y = powf(p.tonecurve[ch_L][k].y, 2.2f);
  dt_gui_presets_add_generic(_("med contrast"), self->op, self->version(), &p, sizeof(p), 1);

  for(int k = 0; k < 6; k++) p.tonecurve[ch_L][k].x = linear_L[k];
  for(int k = 0; k < 6; k++) p.tonecurve[ch_L][k].y = linear_L[k];
  p.tonecurve[ch_L][1].y -= 0.06;
  p.tonecurve[ch_L][4].y += 0.06;
  p.tonecurve[ch_L][2].y -= 0.10;
  p.tonecurve[ch_L][3].y += 0.10;
  for(int k = 1; k < 5; k++) p.tonecurve[ch_L][k].x = powf(p.tonecurve[ch_L][k].x, 2.2f);
  for(int k = 1; k < 5; k++) p.tonecurve[ch_L][k].y = powf(p.tonecurve[ch_L][k].y, 2.2f);
  dt_gui_presets_add_generic(_("high contrast"), self->op, self->version(), &p, sizeof(p), 1);

  for (int k=0; k<sizeof(preset_camera_curves)/sizeof(preset_camera_curves[0]); k++)
  {
    // insert the preset
    dt_gui_presets_add_generic(preset_camera_curves[k].name, self->op, self->version(),
                               &preset_camera_curves[k].preset, sizeof(p), 1);

    // restrict it to model, maker
    dt_gui_presets_update_mml(preset_camera_curves[k].name, self->op, self->version(),
                              preset_camera_curves[k].maker, preset_camera_curves[k].model, "");

    // restrict it to  iso
    dt_gui_presets_update_iso(preset_camera_curves[k].name, self->op, self->version(),
                              preset_camera_curves[k].iso_min, preset_camera_curves[k].iso_max);

    // restrict it to raw images
    dt_gui_presets_update_ldr(preset_camera_curves[k].name, self->op, self->version(), FOR_RAW);

    // hide all non-matching presets in case the model string is set.
    dt_gui_presets_update_filter(preset_camera_curves[k].name, self->op, self->version(), 1);
  }
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_tonecurve_data_t *d = (dt_iop_tonecurve_data_t *)(piece->data);
  dt_iop_tonecurve_params_t *p = (dt_iop_tonecurve_params_t *)p1;
  for(int ch = 0; ch < ch_max; ch++)
  {
    // take care of possible change of curve type or number of nodes (not yet implemented in UI)
    if(d->curve_type[ch] != p->tonecurve_type[ch] || d->curve_nodes[ch] != p->tonecurve_nodes[ch])
    {
      dt_draw_curve_destroy(d->curve[ch]);
      d->curve[ch] = dt_draw_curve_new(0.0, 1.0, p->tonecurve_type[ch]);
      d->curve_nodes[ch] = p->tonecurve_nodes[ch];
      d->curve_type[ch] = p->tonecurve_type[ch];
      for(int k = 0; k < p->tonecurve_nodes[ch]; k++)
        (void)dt_draw_curve_add_point(d->curve[ch], p->tonecurve[ch][k].x, p->tonecurve[ch][k].y);
    }
    else
    {
      for(int k = 0; k < p->tonecurve_nodes[ch]; k++)
        dt_draw_curve_set_point(d->curve[ch], k, p->tonecurve[ch][k].x, p->tonecurve[ch][k].y);
    }
    dt_draw_curve_calc_values(d->curve[ch], 0.0f, 1.0f, 0x10000, NULL, d->table[ch]);
  }
  for(int k = 0; k < 0x10000; k++) d->table[ch_L][k] *= 100.0f;
  for(int k = 0; k < 0x10000; k++) d->table[ch_a][k] = d->table[ch_a][k] * 256.0f - 128.0f;
  for(int k = 0; k < 0x10000; k++) d->table[ch_b][k] = d->table[ch_b][k] * 256.0f - 128.0f;

  d->autoscale_ab = p->tonecurve_autoscale_ab;
  d->unbound_ab = p->tonecurve_unbound_ab;

  // extrapolation for L-curve (right hand side only):
  const float xm_L = p->tonecurve[ch_L][p->tonecurve_nodes[ch_L] - 1].x;
  const float x_L[4] = { 0.7f * xm_L, 0.8f * xm_L, 0.9f * xm_L, 1.0f * xm_L };
  const float y_L[4] = { d->table[ch_L][CLAMP((int)(x_L[0] * 0x10000ul), 0, 0xffff)],
                         d->table[ch_L][CLAMP((int)(x_L[1] * 0x10000ul), 0, 0xffff)],
                         d->table[ch_L][CLAMP((int)(x_L[2] * 0x10000ul), 0, 0xffff)],
                         d->table[ch_L][CLAMP((int)(x_L[3] * 0x10000ul), 0, 0xffff)] };
  dt_iop_estimate_exp(x_L, y_L, 4, d->unbounded_coeffs_L);

  // extrapolation for a-curve right side:
  const float xm_ar = p->tonecurve[ch_a][p->tonecurve_nodes[ch_a] - 1].x;
  const float x_ar[4] = { 0.7f * xm_ar, 0.8f * xm_ar, 0.9f * xm_ar, 1.0f * xm_ar };
  const float y_ar[4] = { d->table[ch_a][CLAMP((int)(x_ar[0] * 0x10000ul), 0, 0xffff)],
                          d->table[ch_a][CLAMP((int)(x_ar[1] * 0x10000ul), 0, 0xffff)],
                          d->table[ch_a][CLAMP((int)(x_ar[2] * 0x10000ul), 0, 0xffff)],
                          d->table[ch_a][CLAMP((int)(x_ar[3] * 0x10000ul), 0, 0xffff)] };
  dt_iop_estimate_exp(x_ar, y_ar, 4, d->unbounded_coeffs_ab);

  // extrapolation for a-curve left side (we need to mirror the x-axis):
  const float xm_al = 1.0f - p->tonecurve[ch_a][0].x;
  const float x_al[4] = { 0.7f * xm_al, 0.8f * xm_al, 0.9f * xm_al, 1.0f * xm_al };
  const float y_al[4] = { d->table[ch_a][CLAMP((int)((1.0f - x_al[0]) * 0x10000ul), 0, 0xffff)],
                          d->table[ch_a][CLAMP((int)((1.0f - x_al[1]) * 0x10000ul), 0, 0xffff)],
                          d->table[ch_a][CLAMP((int)((1.0f - x_al[2]) * 0x10000ul), 0, 0xffff)],
                          d->table[ch_a][CLAMP((int)((1.0f - x_al[3]) * 0x10000ul), 0, 0xffff)] };
  dt_iop_estimate_exp(x_al, y_al, 4, d->unbounded_coeffs_ab + 3);

  // extrapolation for b-curve right side:
  const float xm_br = p->tonecurve[ch_b][p->tonecurve_nodes[ch_b] - 1].x;
  const float x_br[4] = { 0.7f * xm_br, 0.8f * xm_br, 0.9f * xm_br, 1.0f * xm_br };
  const float y_br[4] = { d->table[ch_b][CLAMP((int)(x_br[0] * 0x10000ul), 0, 0xffff)],
                          d->table[ch_b][CLAMP((int)(x_br[1] * 0x10000ul), 0, 0xffff)],
                          d->table[ch_b][CLAMP((int)(x_br[2] * 0x10000ul), 0, 0xffff)],
                          d->table[ch_b][CLAMP((int)(x_br[3] * 0x10000ul), 0, 0xffff)] };
  dt_iop_estimate_exp(x_br, y_br, 4, d->unbounded_coeffs_ab + 6);

  // extrapolation for b-curve left side (we need to mirror the x-axis):
  const float xm_bl = 1.0f - p->tonecurve[ch_b][0].x;
  const float x_bl[4] = { 0.7f * xm_bl, 0.8f * xm_bl, 0.9f * xm_bl, 1.0f * xm_bl };
  const float y_bl[4] = { d->table[ch_b][CLAMP((int)((1.0f - x_bl[0]) * 0x10000ul), 0, 0xffff)],
                          d->table[ch_b][CLAMP((int)((1.0f - x_bl[1]) * 0x10000ul), 0, 0xffff)],
                          d->table[ch_b][CLAMP((int)((1.0f - x_bl[2]) * 0x10000ul), 0, 0xffff)],
                          d->table[ch_b][CLAMP((int)((1.0f - x_bl[3]) * 0x10000ul), 0, 0xffff)] };
  dt_iop_estimate_exp(x_bl, y_bl, 4, d->unbounded_coeffs_ab + 9);
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  // create part of the gegl pipeline
  dt_iop_tonecurve_data_t *d = (dt_iop_tonecurve_data_t *)malloc(sizeof(dt_iop_tonecurve_data_t));
  dt_iop_tonecurve_params_t *default_params = (dt_iop_tonecurve_params_t *)self->default_params;
  piece->data = (void *)d;
  d->autoscale_ab = 1;
  d->unbound_ab = 1;
  for(int ch = 0; ch < ch_max; ch++)
  {
    d->curve[ch] = dt_draw_curve_new(0.0, 1.0, default_params->tonecurve_type[ch]);
    d->curve_nodes[ch] = default_params->tonecurve_nodes[ch];
    d->curve_type[ch] = default_params->tonecurve_type[ch];
    for(int k = 0; k < default_params->tonecurve_nodes[ch]; k++)
      (void)dt_draw_curve_add_point(d->curve[ch], default_params->tonecurve[ch][k].x,
                                    default_params->tonecurve[ch][k].y);
  }
  for(int k = 0; k < 0x10000; k++) d->table[ch_L][k] = 100.0f * k / 0x10000;          // identity for L
  for(int k = 0; k < 0x10000; k++) d->table[ch_a][k] = 256.0f * k / 0x10000 - 128.0f; // identity for a
  for(int k = 0; k < 0x10000; k++) d->table[ch_b][k] = 256.0f * k / 0x10000 - 128.0f; // identity for b
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  // clean up everything again.
  dt_iop_tonecurve_data_t *d = (dt_iop_tonecurve_data_t *)(piece->data);
  for(int ch = 0; ch < ch_max; ch++) dt_draw_curve_destroy(d->curve[ch]);
  free(piece->data);
  piece->data = NULL;
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_tonecurve_gui_data_t *g = (dt_iop_tonecurve_gui_data_t *)self->gui_data;
  dt_iop_tonecurve_params_t *p = (dt_iop_tonecurve_params_t *)self->params;
  dt_bauhaus_combobox_set(g->autoscale_ab, 1 - p->tonecurve_autoscale_ab);
  // that's all, gui curve is read directly from params during expose event.
  gtk_widget_queue_draw(self->widget);
}


void init(dt_iop_module_t *module)
{
  module->params = malloc(sizeof(dt_iop_tonecurve_params_t));
  module->default_params = malloc(sizeof(dt_iop_tonecurve_params_t));
  module->default_enabled = 0;
  module->request_histogram |= (DT_REQUEST_ON);
  module->priority = 666; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_tonecurve_params_t);
  module->gui_data = NULL;
  dt_iop_tonecurve_params_t tmp = (dt_iop_tonecurve_params_t){
    { { // three curves (L, a, b) with a number of nodes
        { 0.0, 0.0 },
        { 1.0, 1.0 }
      },
      { { 0.0, 0.0 }, { 0.5, 0.5 }, { 1.0, 1.0 } },
      { { 0.0, 0.0 }, { 0.5, 0.5 }, { 1.0, 1.0 } } },
    { 2, 3, 3 }, // number of nodes per curve
    // { CATMULL_ROM, CATMULL_ROM, CATMULL_ROM},  // curve types
    { MONOTONE_HERMITE, MONOTONE_HERMITE, MONOTONE_HERMITE },
    // { CUBIC_SPLINE, CUBIC_SPLINE, CUBIC_SPLINE},
    1, // autoscale_ab
    0,
    1 // unbound_ab
  };
  memcpy(module->params, &tmp, sizeof(dt_iop_tonecurve_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_tonecurve_params_t));
}

void init_global(dt_iop_module_so_t *module)
{
  const int program = 2; // basic.cl, from programs.conf
  dt_iop_tonecurve_global_data_t *gd
      = (dt_iop_tonecurve_global_data_t *)malloc(sizeof(dt_iop_tonecurve_global_data_t));
  module->data = gd;
  gd->kernel_tonecurve = dt_opencl_create_kernel(program, "tonecurve");
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_tonecurve_global_data_t *gd = (dt_iop_tonecurve_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_tonecurve);
  free(module->data);
  module->data = NULL;
}

void cleanup(dt_iop_module_t *module)
{
  free(module->gui_data);
  module->gui_data = NULL;
  free(module->params);
  module->params = NULL;
}

static void autoscale_ab_callback(GtkWidget *widget, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_tonecurve_gui_data_t *g = (dt_iop_tonecurve_gui_data_t *)self->gui_data;
  dt_iop_tonecurve_params_t *p = (dt_iop_tonecurve_params_t *)self->params;
  p->tonecurve_autoscale_ab = 1 - dt_bauhaus_combobox_get(g->autoscale_ab);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void tab_switch(GtkNotebook *notebook, GtkWidget *page, guint page_num, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_tonecurve_gui_data_t *c = (dt_iop_tonecurve_gui_data_t *)self->gui_data;
  if(self->dt->gui->reset) return;
  c->channel = (tonecurve_channel_t)page_num;
  gtk_widget_queue_draw(self->widget);
}

static gboolean area_resized(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
  GtkRequisition r;
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  r.width = allocation.width;
  r.height = allocation.width;
  gtk_widget_get_preferred_size(widget, &r, NULL);
  return TRUE;
}

static void pick_toggled(GtkToggleButton *togglebutton, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;

  self->request_color_pick
      = (gtk_toggle_button_get_active(togglebutton) ? DT_REQUEST_COLORPICK_MODULE : DT_REQUEST_COLORPICK_OFF);

  /* set the area sample size */
  if(self->request_color_pick != DT_REQUEST_COLORPICK_OFF)
  {
    dt_lib_colorpicker_set_point(darktable.lib, 0.5, 0.5);
    dt_dev_reprocess_all(self->dev);
  }
  else
    dt_control_queue_redraw();

  if(self->off) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->off), 1);
  dt_iop_request_focus(self);
}


static gboolean scrolled(GtkWidget *widget, GdkEventScroll *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_tonecurve_params_t *p = (dt_iop_tonecurve_params_t *)self->params;
  dt_iop_tonecurve_gui_data_t *c = (dt_iop_tonecurve_gui_data_t *)self->gui_data;

  int ch = c->channel;
  dt_iop_tonecurve_node_t *tonecurve = p->tonecurve[ch];
  int autoscale_ab = p->tonecurve_autoscale_ab;

  // if autoscale_ab is on: do not modify a and b curves
  if(autoscale_ab && ch != ch_L) return TRUE;

  if(c->selected >= 0)
  {
    if(event->direction == GDK_SCROLL_UP)
      tonecurve[c->selected].y = MAX(0.0f, tonecurve[c->selected].y + 0.001f);
    if(event->direction == GDK_SCROLL_DOWN)
      tonecurve[c->selected].y = MIN(1.0f, tonecurve[c->selected].y - 0.001f);
    dt_dev_add_history_item(darktable.develop, self, TRUE);
    gtk_widget_queue_draw(widget);
  }
  return TRUE;
}


void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_tonecurve_gui_data_t));
  dt_iop_tonecurve_gui_data_t *c = (dt_iop_tonecurve_gui_data_t *)self->gui_data;
  dt_iop_tonecurve_params_t *p = (dt_iop_tonecurve_params_t *)self->params;

  for(int ch = 0; ch < ch_max; ch++)
  {
    c->minmax_curve[ch] = dt_draw_curve_new(0.0, 1.0, p->tonecurve_type[ch]);
    c->minmax_curve_nodes[ch] = p->tonecurve_nodes[ch];
    c->minmax_curve_type[ch] = p->tonecurve_type[ch];
    for(int k = 0; k < p->tonecurve_nodes[ch]; k++)
      (void)dt_draw_curve_add_point(c->minmax_curve[ch], p->tonecurve[ch][k].x, p->tonecurve[ch][k].y);
  }

  c->channel = ch_L;
  c->mouse_x = c->mouse_y = -1.0;
  c->selected = -1;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  // tabs
  c->channel_tabs = GTK_NOTEBOOK(gtk_notebook_new());

  gtk_notebook_append_page(GTK_NOTEBOOK(c->channel_tabs),
                           GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0)), gtk_label_new(_("  L  ")));
  g_object_set(
      G_OBJECT(gtk_notebook_get_tab_label(c->channel_tabs, gtk_notebook_get_nth_page(c->channel_tabs, -1))),
      "tooltip-text", _("tonecurve for L channel"), NULL);
  gtk_notebook_append_page(GTK_NOTEBOOK(c->channel_tabs),
                           GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0)), gtk_label_new(_("  a  ")));
  g_object_set(
      G_OBJECT(gtk_notebook_get_tab_label(c->channel_tabs, gtk_notebook_get_nth_page(c->channel_tabs, -1))),
      "tooltip-text", _("tonecurve for a channel"), NULL);
  gtk_notebook_append_page(GTK_NOTEBOOK(c->channel_tabs),
                           GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0)), gtk_label_new(_("  b  ")));
  g_object_set(
      G_OBJECT(gtk_notebook_get_tab_label(c->channel_tabs, gtk_notebook_get_nth_page(c->channel_tabs, -1))),
      "tooltip-text", _("tonecurve for b channel"), NULL);

  gtk_widget_show_all(GTK_WIDGET(gtk_notebook_get_nth_page(c->channel_tabs, c->channel)));
  gtk_notebook_set_current_page(GTK_NOTEBOOK(c->channel_tabs), c->channel);

  GtkWidget *tb = dtgtk_togglebutton_new(dtgtk_cairo_paint_colorpicker, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER);
  gtk_widget_set_size_request(GTK_WIDGET(tb), DT_PIXEL_APPLY_DPI(14), DT_PIXEL_APPLY_DPI(14));
  g_object_set(G_OBJECT(tb), "tooltip-text", _("pick GUI color from image"), (char *)NULL);

  GtkWidget *notebook = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_start(GTK_BOX(notebook), GTK_WIDGET(c->channel_tabs), FALSE, FALSE, 0);
  gtk_box_pack_end(GTK_BOX(notebook), GTK_WIDGET(tb), FALSE, FALSE, 0);

  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), vbox, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(notebook), TRUE, TRUE, 0);

  g_signal_connect(G_OBJECT(c->channel_tabs), "switch_page", G_CALLBACK(tab_switch), self);

  c->area = GTK_DRAWING_AREA(dtgtk_drawing_area_new_with_aspect_ratio(1.0));
  gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(c->area), TRUE, TRUE, 0);
  g_object_set(G_OBJECT(c->area), "tooltip-text", _("double click to reset curve"), (char *)NULL);

  gtk_widget_add_events(GTK_WIDGET(c->area), GDK_POINTER_MOTION_MASK | GDK_POINTER_MOTION_HINT_MASK
                                             | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK
                                             | GDK_LEAVE_NOTIFY_MASK | GDK_SCROLL_MASK);
  g_signal_connect(G_OBJECT(c->area), "draw", G_CALLBACK(dt_iop_tonecurve_draw), self);
  g_signal_connect(G_OBJECT(c->area), "button-press-event", G_CALLBACK(dt_iop_tonecurve_button_press), self);
  g_signal_connect(G_OBJECT(c->area), "motion-notify-event", G_CALLBACK(dt_iop_tonecurve_motion_notify), self);
  g_signal_connect(G_OBJECT(c->area), "leave-notify-event", G_CALLBACK(dt_iop_tonecurve_leave_notify), self);
  g_signal_connect(G_OBJECT(c->area), "enter-notify-event", G_CALLBACK(dt_iop_tonecurve_enter_notify), self);
  g_signal_connect(G_OBJECT(c->area), "configure-event", G_CALLBACK(area_resized), self);
  g_signal_connect(G_OBJECT(tb), "toggled", G_CALLBACK(pick_toggled), self);
  g_signal_connect(G_OBJECT(c->area), "scroll-event", G_CALLBACK(scrolled), self);

  c->autoscale_ab = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(c->autoscale_ab, NULL, _("scale chroma"));
  dt_bauhaus_combobox_add(c->autoscale_ab, _("automatic"));
  dt_bauhaus_combobox_add(c->autoscale_ab, C_("scale", "manual"));
  gtk_box_pack_start(GTK_BOX(self->widget), c->autoscale_ab, TRUE, TRUE, 0);
  g_object_set(G_OBJECT(c->autoscale_ab), "tooltip-text",
               _("if set to auto, a and b curves have no effect and are not displayed. chroma values (a and "
                 "b) of each pixel are then adjusted based on L curve data."),
               (char *)NULL);
  g_signal_connect(G_OBJECT(c->autoscale_ab), "value-changed", G_CALLBACK(autoscale_ab_callback), self);

  c->sizegroup = GTK_SIZE_GROUP(gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL));
  gtk_size_group_add_widget(c->sizegroup, GTK_WIDGET(c->area));
  gtk_size_group_add_widget(c->sizegroup, GTK_WIDGET(c->channel_tabs));
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  dt_iop_tonecurve_gui_data_t *c = (dt_iop_tonecurve_gui_data_t *)self->gui_data;
  // this one we need to unref manually. not so the initially unowned widgets.
  g_object_unref(c->sizegroup);
  dt_draw_curve_destroy(c->minmax_curve[ch_L]);
  dt_draw_curve_destroy(c->minmax_curve[ch_a]);
  dt_draw_curve_destroy(c->minmax_curve[ch_b]);
  free(self->gui_data);
  self->gui_data = NULL;
}


static gboolean dt_iop_tonecurve_enter_notify(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_tonecurve_gui_data_t *c = (dt_iop_tonecurve_gui_data_t *)self->gui_data;
  c->mouse_x = fabs(c->mouse_x);
  c->mouse_y = fabs(c->mouse_y);
  gtk_widget_queue_draw(widget);
  return TRUE;
}

static gboolean dt_iop_tonecurve_leave_notify(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_tonecurve_gui_data_t *c = (dt_iop_tonecurve_gui_data_t *)self->gui_data;
  // weird sign dance for fluxbox:
  c->mouse_x = -fabs(c->mouse_x);
  c->mouse_y = -fabs(c->mouse_y);
  gtk_widget_queue_draw(widget);
  return TRUE;
}


static void picker_scale(const float *in, float *out)
{
  out[0] = CLAMP(in[0] / 100.0f, 0.0f, 1.0f);
  out[1] = CLAMP((in[1] + 128.0f) / 256.0f, 0.0f, 1.0f);
  out[2] = CLAMP((in[2] + 128.0f) / 256.0f, 0.0f, 1.0f);
}

static gboolean dt_iop_tonecurve_draw(GtkWidget *widget, cairo_t *crf, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_tonecurve_gui_data_t *c = (dt_iop_tonecurve_gui_data_t *)self->gui_data;
  dt_iop_tonecurve_params_t *p = (dt_iop_tonecurve_params_t *)self->params;
  dt_develop_t *dev = darktable.develop;

  const float color_labels_left[3][3]
      = { { 0.3f, 0.3f, 0.3f }, { 0.0f, 0.34f, 0.27f }, { 0.0f, 0.27f, 0.58f } };

  const float color_labels_right[3][3]
      = { { 0.3f, 0.3f, 0.3f }, { 0.53f, 0.08f, 0.28f }, { 0.81f, 0.66f, 0.0f } };

  int ch = c->channel;
  int nodes = p->tonecurve_nodes[ch];
  dt_iop_tonecurve_node_t *tonecurve = p->tonecurve[ch];
  int autoscale_ab = p->tonecurve_autoscale_ab;
  if(c->minmax_curve_type[ch] != p->tonecurve_type[ch] || c->minmax_curve_nodes[ch] != p->tonecurve_nodes[ch])
  {
    dt_draw_curve_destroy(c->minmax_curve[ch]);
    c->minmax_curve[ch] = dt_draw_curve_new(0.0, 1.0, p->tonecurve_type[ch]);
    c->minmax_curve_nodes[ch] = p->tonecurve_nodes[ch];
    c->minmax_curve_type[ch] = p->tonecurve_type[ch];
    for(int k = 0; k < p->tonecurve_nodes[ch]; k++)
      (void)dt_draw_curve_add_point(c->minmax_curve[ch], p->tonecurve[ch][k].x, p->tonecurve[ch][k].y);
  }
  else
  {
    for(int k = 0; k < p->tonecurve_nodes[ch]; k++)
      dt_draw_curve_set_point(c->minmax_curve[ch], k, p->tonecurve[ch][k].x, p->tonecurve[ch][k].y);
  }
  dt_draw_curve_t *minmax_curve = c->minmax_curve[ch];
  dt_draw_curve_calc_values(minmax_curve, 0.0, 1.0, DT_IOP_TONECURVE_RES, c->draw_xs, c->draw_ys);

  const float xm = tonecurve[nodes - 1].x;
  const float x[4] = { 0.7f * xm, 0.8f * xm, 0.9f * xm, 1.0f * xm };
  const float y[4] = { c->draw_ys[CLAMP((int)(x[0] * DT_IOP_TONECURVE_RES), 0, DT_IOP_TONECURVE_RES - 1)],
                       c->draw_ys[CLAMP((int)(x[1] * DT_IOP_TONECURVE_RES), 0, DT_IOP_TONECURVE_RES - 1)],
                       c->draw_ys[CLAMP((int)(x[2] * DT_IOP_TONECURVE_RES), 0, DT_IOP_TONECURVE_RES - 1)],
                       c->draw_ys[CLAMP((int)(x[3] * DT_IOP_TONECURVE_RES), 0, DT_IOP_TONECURVE_RES - 1)] };
  float unbounded_coeffs[3];
  dt_iop_estimate_exp(x, y, 4, unbounded_coeffs);

  const int inset = DT_GUI_CURVE_EDITOR_INSET;
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  int width = allocation.width, height = allocation.height;
  cairo_surface_t *cst = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);
  // clear bg
  cairo_set_source_rgb(cr, .2, .2, .2);
  cairo_paint(cr);

  cairo_translate(cr, inset, inset);
  width -= 2 * inset;
  height -= 2 * inset;

#if 0
  // draw shadow around
  float alpha = 1.0f;
  for(int k=0; k<inset; k++)
  {
    cairo_rectangle(cr, -k, -k, width + 2*k, height + 2*k);
    cairo_set_source_rgba(cr, 0, 0, 0, alpha);
    alpha *= 0.6f;
    cairo_fill(cr);
  }
#else
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.0));
  cairo_set_source_rgb(cr, .1, .1, .1);
  cairo_rectangle(cr, 0, 0, width, height);
  cairo_stroke(cr);
#endif

  cairo_set_source_rgb(cr, .3, .3, .3);
  cairo_rectangle(cr, 0, 0, width, height);
  cairo_fill(cr);

  // draw color labels
  const int cells = 8;
  for(int j = 0; j < cells; j++)
  {
    for(int i = 0; i < cells; i++)
    {
      const float f = (cells - 1 - j + i) / (2.0f * cells - 2.0f);
      cairo_set_source_rgba(cr, (1.0f - f) * color_labels_left[ch][0] + f * color_labels_right[ch][0],
                            (1.0f - f) * color_labels_left[ch][1] + f * color_labels_right[ch][1],
                            (1.0f - f) * color_labels_left[ch][2] + f * color_labels_right[ch][2],
                            .5f); // blend over to make colors darker, so the overlay is more visible
      cairo_rectangle(cr, width * i / (float)cells, height * j / (float)cells, width / (float)cells,
                      height / (float)cells);
      cairo_fill(cr);
    }
  }

  // draw grid
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(.4));
  cairo_set_source_rgb(cr, .1, .1, .1);
  dt_draw_grid(cr, 4, 0, 0, width, height);

  // if autoscale_ab is on: do not display a and b curves
  if(autoscale_ab && ch != ch_L) goto finally;

  // draw nodes positions
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.));
  cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
  cairo_translate(cr, 0, height);

  for(int k = 0; k < nodes; k++)
  {
    cairo_arc(cr, tonecurve[k].x * width, -tonecurve[k].y * height, DT_PIXEL_APPLY_DPI(3), 0, 2. * M_PI);
    cairo_stroke(cr);
  }

  // draw selected cursor
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.));

  // draw histogram in background
  // only if module is enabled
  if(self->enabled)
  {
    uint32_t *hist;
    float hist_max;
    float *raw_mean, *raw_min, *raw_max;
    float *raw_mean_output;
    float picker_mean[3], picker_min[3], picker_max[3];
    char text[256];

    raw_mean = self->picked_color;
    raw_min = self->picked_color_min;
    raw_max = self->picked_color_max;
    raw_mean_output = self->picked_output_color;

    hist = self->histogram;
    hist_max = dev->histogram_type == DT_DEV_HISTOGRAM_LINEAR ? self->histogram_max[ch]
                                                              : logf(1.0 + self->histogram_max[ch]);
    if(hist && hist_max > 0.0f)
    {
      cairo_save(cr);
      cairo_scale(cr, width / 63.0, -(height - DT_PIXEL_APPLY_DPI(5)) / hist_max);
      cairo_set_source_rgba(cr, .2, .2, .2, 0.5);
      dt_draw_histogram_8(cr, hist, ch, dev->histogram_type == DT_DEV_HISTOGRAM_LINEAR); // TODO: make draw
                                                                                         // handle waveform
                                                                                         // histograms
      cairo_restore(cr);
    }

    if(self->request_color_pick != DT_REQUEST_COLORPICK_OFF)
    {
      // the global live samples ...
      GSList *samples = darktable.lib->proxy.colorpicker.live_samples;
      dt_colorpicker_sample_t *sample = NULL;
      while(samples)
      {
        sample = samples->data;

        picker_scale(sample->picked_color_lab_mean, picker_mean);
        picker_scale(sample->picked_color_lab_min, picker_min);
        picker_scale(sample->picked_color_lab_max, picker_max);

        cairo_set_source_rgba(cr, 0.5, 0.7, 0.5, 0.15);
        cairo_rectangle(cr, width * picker_min[ch], 0, width * fmax(picker_max[ch] - picker_min[ch], 0.0f),
                        -height);
        cairo_fill(cr);
        cairo_set_source_rgba(cr, 0.5, 0.7, 0.5, 0.5);
        cairo_move_to(cr, width * picker_mean[ch], 0);
        cairo_line_to(cr, width * picker_mean[ch], -height);
        cairo_stroke(cr);

        samples = g_slist_next(samples);
      }

      // ... and the local sample
      if(raw_max[0] >= 0.0f)
      {
        picker_scale(raw_mean, picker_mean);
        picker_scale(raw_min, picker_min);
        picker_scale(raw_max, picker_max);

        cairo_set_source_rgba(cr, 0.7, 0.5, 0.5, 0.33);
        cairo_rectangle(cr, width * picker_min[ch], 0, width * fmax(picker_max[ch] - picker_min[ch], 0.0f),
                        -height);
        cairo_fill(cr);
        cairo_set_source_rgba(cr, 0.9, 0.7, 0.7, 0.5);
        cairo_move_to(cr, width * picker_mean[ch], 0);
        cairo_line_to(cr, width * picker_mean[ch], -height);
        cairo_stroke(cr);

        snprintf(text, sizeof(text), "%.1f → %.1f", raw_mean[ch], raw_mean_output[ch]);

        cairo_set_source_rgb(cr, 0.1, 0.1, 0.1);
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr, DT_PIXEL_APPLY_DPI(0.06) * height);
        cairo_move_to(cr, 0.02f * width, -0.94 * height);
        cairo_show_text(cr, text);
        cairo_stroke(cr);
      }
    }
  }

  if(c->selected >= 0)
  {
    cairo_set_source_rgb(cr, .9, .9, .9);
    cairo_arc(cr, tonecurve[c->selected].x * width, -tonecurve[c->selected].y * height, DT_PIXEL_APPLY_DPI(4),
              0, 2. * M_PI);
    cairo_stroke(cr);
  }

  // draw curve
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(2.));
  cairo_set_source_rgb(cr, .9, .9, .9);
  // cairo_set_line_cap  (cr, CAIRO_LINE_CAP_SQUARE);
  cairo_move_to(cr, 0, -height * c->draw_ys[0]);
  for(int k = 1; k < DT_IOP_TONECURVE_RES; k++)
  {
    const float xx = k / (DT_IOP_TONECURVE_RES - 1.0);
    if(xx > xm)
    {
      const float yy = dt_iop_eval_exp(unbounded_coeffs, xx);
      cairo_line_to(cr, xx * width, -height * yy);
    }
    else
    {
      cairo_line_to(cr, xx * width, -height * c->draw_ys[k]);
    }
  }
  cairo_stroke(cr);

finally:
  cairo_destroy(cr);
  cairo_set_source_surface(crf, cst, 0, 0);
  cairo_paint(crf);
  cairo_surface_destroy(cst);
  return TRUE;
}

static gboolean dt_iop_tonecurve_motion_notify(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_tonecurve_gui_data_t *c = (dt_iop_tonecurve_gui_data_t *)self->gui_data;
  dt_iop_tonecurve_params_t *p = (dt_iop_tonecurve_params_t *)self->params;

  int ch = c->channel;
  int nodes = p->tonecurve_nodes[ch];
  dt_iop_tonecurve_node_t *tonecurve = p->tonecurve[ch];
  int autoscale_ab = p->tonecurve_autoscale_ab;

  // if autoscale_ab is on: do not modify a and b curves
  if(autoscale_ab && ch != ch_L) goto finally;

  const int inset = DT_GUI_CURVE_EDITOR_INSET;
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  int height = allocation.height - 2 * inset, width = allocation.width - 2 * inset;
  c->mouse_x = CLAMP(event->x - inset, 0, width);
  c->mouse_y = CLAMP(event->y - inset, 0, height);

  const float mx = c->mouse_x / (float)width;
  const float my = 1.0f - c->mouse_y / (float)height;

  if(event->state & GDK_BUTTON1_MASK)
  {
    // got a vertex selected:
    if(c->selected >= 0)
    {
      tonecurve[c->selected].x = mx;
      tonecurve[c->selected].y = my;

      // delete vertex if order has changed:
      if(nodes > 2)
        if((c->selected > 0 && tonecurve[c->selected - 1].x >= mx)
           || (c->selected < nodes - 1 && tonecurve[c->selected + 1].x <= mx))
        {
          for(int k = c->selected; k < nodes - 1; k++)
          {
            tonecurve[k].x = tonecurve[k + 1].x;
            tonecurve[k].y = tonecurve[k + 1].y;
          }
          c->selected = -2; // avoid re-insertion of that point immediately after this
          p->tonecurve_nodes[ch]--;
        }
      dt_dev_add_history_item(darktable.develop, self, TRUE);
    }
    else if(nodes < 20 && c->selected >= -1)
    {
      // no vertex was close, create a new one!
      if(tonecurve[0].x > mx)
        c->selected = 0;
      else
        for(int k = 1; k < nodes; k++)
        {
          if(tonecurve[k].x > mx)
          {
            c->selected = k;
            break;
          }
        }
      if(c->selected == -1) c->selected = nodes;
      for(int i = nodes; i > c->selected; i--)
      {
        tonecurve[i].x = tonecurve[i - 1].x;
        tonecurve[i].y = tonecurve[i - 1].y;
      }
      // found a new point
      tonecurve[c->selected].x = mx;
      tonecurve[c->selected].y = my;
      p->tonecurve_nodes[ch]++;
      dt_dev_add_history_item(darktable.develop, self, TRUE);
    }
  }
  else
  {
    // minimum area around the node to select it:
    float min = .04f;
    min *= min; // comparing against square
    int nearest = -1;
    for(int k = 0; k < nodes; k++)
    {
      float dist = (my - tonecurve[k].y) * (my - tonecurve[k].y)
                   + (mx - tonecurve[k].x) * (mx - tonecurve[k].x);
      if(dist < min)
      {
        min = dist;
        nearest = k;
      }
    }
    c->selected = nearest;
  }
finally:
  gtk_widget_queue_draw(widget);
  return TRUE;
}

static gboolean dt_iop_tonecurve_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_tonecurve_params_t *p = (dt_iop_tonecurve_params_t *)self->params;
  dt_iop_tonecurve_params_t *d = (dt_iop_tonecurve_params_t *)self->default_params;
  dt_iop_tonecurve_gui_data_t *c = (dt_iop_tonecurve_gui_data_t *)self->gui_data;

  int ch = c->channel;
  int autoscale_ab = p->tonecurve_autoscale_ab;

  if(event->button == 1 && event->type == GDK_2BUTTON_PRESS)
  {
    // reset current curve
    // if autoscale_ab is on: allow only reset of L curve
    if(!(autoscale_ab && ch != ch_L))
    {
      p->tonecurve_nodes[ch] = d->tonecurve_nodes[ch];
      p->tonecurve_type[ch] = d->tonecurve_type[ch];
      for(int k = 0; k < d->tonecurve_nodes[ch]; k++)
      {
        p->tonecurve[ch][k].x = d->tonecurve[ch][k].x;
        p->tonecurve[ch][k].y = d->tonecurve[ch][k].y;
      }
      c->selected = -2; // avoid motion notify re-inserting immediately.
      dt_dev_add_history_item(darktable.develop, self, TRUE);
      gtk_widget_queue_draw(self->widget);
    }
    else
    {
      if(ch != ch_L)
      {
        p->tonecurve_autoscale_ab = 0;
        c->selected = -2; // avoid motion notify re-inserting immediately.
        dt_bauhaus_combobox_set(c->autoscale_ab, 1 - p->tonecurve_autoscale_ab);
        dt_dev_add_history_item(darktable.develop, self, TRUE);
        gtk_widget_queue_draw(self->widget);
      }
    }
    return TRUE;
  }
  return FALSE;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
