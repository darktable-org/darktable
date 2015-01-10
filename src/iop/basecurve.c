/*
    This file is part of darktable,
    copyright (c) 2009--2011 johannes hanika.

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
#include "develop/develop.h"
#include "develop/imageop.h"
#include "control/control.h"
#include "common/debug.h"
#include "common/opencl.h"
#include "gui/gtk.h"
#include "gui/draw.h"
#include "gui/presets.h"
#include "bauhaus/bauhaus.h"
#include "dtgtk/drawingarea.h"
#include <gtk/gtk.h>
#include <inttypes.h>
#include <stdlib.h>
#include <math.h>
#include <assert.h>
#include <string.h>

#define DT_GUI_CURVE_EDITOR_INSET DT_PIXEL_APPLY_DPI(5)
#define DT_GUI_CURVE_INFL .3f
#define DT_IOP_TONECURVE_RES 256
#define MAXNODES 20


DT_MODULE_INTROSPECTION(2, dt_iop_basecurve_params_t)

typedef struct dt_iop_basecurve_node_t
{
  float x;
  float y;
} dt_iop_basecurve_node_t;

typedef struct dt_iop_basecurve_params_t
{
  // three curves (c, ., .) with max number of nodes
  // the other two are reserved, maybe we'll have cam rgb at some point.
  dt_iop_basecurve_node_t basecurve[3][MAXNODES];
  int basecurve_nodes[3];
  int basecurve_type[3];
} dt_iop_basecurve_params_t;

typedef struct dt_iop_basecurve_params1_t
{
  float tonecurve_x[6], tonecurve_y[6];
  int tonecurve_preset;
} dt_iop_basecurve_params1_t;

int legacy_params(dt_iop_module_t *self, const void *const old_params, const int old_version,
                  void *new_params, const int new_version)
{
  if(old_version == 1 && new_version == 2)
  {
    dt_iop_basecurve_params1_t *o = (dt_iop_basecurve_params1_t *)old_params;
    dt_iop_basecurve_params_t *n = (dt_iop_basecurve_params_t *)new_params;

    // start with a fresh copy of default parameters
    // unfortunately default_params aren't inited at this stage.
    *n = (dt_iop_basecurve_params_t){ {
                                        { { 0.0, 0.0 }, { 1.0, 1.0 } },
                                      },
                                      { 2, 3, 3 },
                                      { MONOTONE_HERMITE, MONOTONE_HERMITE, MONOTONE_HERMITE } };
    for(int k = 0; k < 6; k++) n->basecurve[0][k].x = o->tonecurve_x[k];
    for(int k = 0; k < 6; k++) n->basecurve[0][k].y = o->tonecurve_y[k];
    n->basecurve_nodes[0] = 6;
    n->basecurve_type[0] = CUBIC_SPLINE;
    return 0;
  }
  return 1;
}

static const char natural[] = N_("natural");
static const char canon_eos[] = N_("canon eos like");
static const char canon_eos_alt[] = N_("canon eos like alternate");
static const char nikon[] = N_("nikon like");
static const char nikon_alt[] = N_("nikon like alternate");
static const char sony_alpha[] = N_("sony alpha like");
static const char pentax[] = N_("pentax like");
static const char ricoh[] = N_("ricoh like");
static const char olympus[] = N_("olympus like");
static const char olympus_alt[] = N_("olympus like alternate");
static const char panasonic[] = N_("panasonic like");
static const char leica[] = N_("leica like");
static const char kodak_easyshare[] = N_("kodak easyshare like");
static const char konica_minolta[] = N_("konica minolta like");
static const char samsung[] = N_("samsung like");
static const char fujifilm[] = N_("fujifilm like");
static const char nokia[] = N_("nokia like");

typedef struct basecurve_preset_t
{
  const char *name;
  const char *maker;
  const char *model;
  int iso_min, iso_max;
  dt_iop_basecurve_params_t params;
  int autoapply;
  int filter;
} basecurve_preset_t;

#define m MONOTONE_HERMITE
static const basecurve_preset_t basecurve_presets[] = {
  // copy paste your measured basecurve line at the top here, like so (note the exif data and the last 1):

  // smoother cubic spline curve
  { N_("cubic spline"), "", "", 0, 51200, { { { { 0.0, 0.0}, { 1.0, 1.0 }, { 0., 0.}, { 0., 0.}, { 0., 0.}, { 0., 0.}, { 0., 0.}, { 0., 0.} } }, { 2 }, { CUBIC_SPLINE } }, 0, 0 },
  // nikon d750 by Edouard Gomez
  {"NIKON D750", "NIKON CORPORATION", "NIKON D750", 0, 51200, {{{{0.000000, 0.000000}, {0.018124, 0.026126}, {0.143357, 0.370145}, {0.330116, 0.730507}, {0.457952, 0.853462}, {0.734950, 0.965061}, {0.904758, 0.985699}, {1.000000, 1.000000}}}, {8}, {m}}, 0, 1},
  // nikon d7000 by Edouard Gomez
  {"NIKON D7000", "NIKON CORPORATION", "NIKON D7000", 0, 51200, {{{{0.000000, 0.000000}, {0.001943, 0.003040}, {0.019814, 0.028810}, {0.080784, 0.210476}, {0.145700, 0.383873}, {0.295961, 0.654041}, {0.651915, 0.952819}, {1.000000, 1.000000}}}, {8}, {m}}, 0, 1},
  // sony rx100m2 by Günther R.
  { "DSC-RX100M2", "SONY", "DSC-RX100M2", 0, 51200, { { { { 0.000000, 0.000000 }, { 0.015106, 0.008116 }, { 0.070077, 0.093725 }, { 0.107484, 0.170723 }, { 0.191528, 0.341093 }, { 0.257996, 0.458453 }, { 0.305381, 0.537267 }, { 0.326367, 0.569257 }, { 0.448067, 0.723742 }, { 0.509627, 0.777966 }, { 0.676751, 0.898797 }, { 1.000000, 1.000000 } } }, { 12 }, { m } }, 0, 1 },
  // contributed by matthias bodenbinder
  { "Canon EOS 6D", "Canon", "Canon EOS 6D", 0, 51200, { { { { 0.000000, 0.002917 }, { 0.000751, 0.001716 }, { 0.006011, 0.004438 }, { 0.020286, 0.021725 }, { 0.048084, 0.085918 }, { 0.093914, 0.233804 }, { 0.162284, 0.431375 }, { 0.257701, 0.629218 }, { 0.384673, 0.800332 }, { 0.547709, 0.917761 }, { 0.751315, 0.988132 }, { 1.000000, 0.999943 } } }, { 12 }, { m } }, 0, 1 },
  // contributed by Dan Torop
  { "Fujifilm X100S", "Fujifilm", "X100S", 0, 51200, { { { { 0.000000, 0.000000 }, { 0.009145, 0.007905 }, { 0.026570, 0.032201 }, { 0.131526, 0.289717 }, { 0.175858, 0.395263 }, { 0.350981, 0.696899 }, { 0.614997, 0.959451 }, { 1.000000, 1.000000 } } }, { 8 }, { m } }, 0, 1 },
  // contributed by Johannes Hanika
  { "Canon EOS 5D Mark II", "Canon", "Canon EOS 5D Mark II", 0, 51200, { { { { 0.000000, 0.000366 }, { 0.006560, 0.003504 }, { 0.027310, 0.029834 }, { 0.045915, 0.070230 }, { 0.206554, 0.539895 }, { 0.442337, 0.872409 }, { 0.673263, 0.971703 }, { 1.000000, 0.999832 } } }, { 8 }, { m } }, 0, 1 },
  // contributed by chrik5
  { "Pentax K-5", "Pentax", "Pentax K-5", 0, 51200, { { { { 0.000000, 0.000000 }, { 0.004754, 0.002208 }, { 0.009529, 0.004214 }, { 0.023713, 0.013508 }, { 0.031866, 0.020352 }, { 0.046734, 0.034063 }, { 0.059989, 0.052413 }, { 0.088415, 0.096030 }, { 0.136610, 0.190629 }, { 0.174480, 0.256484 }, { 0.205192, 0.307430 }, { 0.228896, 0.348447 }, { 0.286411, 0.428680 }, { 0.355314, 0.513527 }, { 0.440014, 0.607651 }, { 0.567096, 0.732791 }, { 0.620597, 0.775968 }, { 0.760355, 0.881828 }, { 0.875139, 0.960682 }, { 1.000000, 1.000000 } } }, { 20 }, { m } }, 0, 1 },
  // contributed by Togan Muftuoglu
  { "Nikon D90", "NIKON", "D90", 0, 51200, { { { { 0.000000, 0.000000 }, { 0.015520, 0.012248 }, { 0.097950, 0.251013 }, { 0.301515, 0.621951 }, { 0.415513, 0.771384 }, { 0.547326, 0.843079 }, { 0.819769, 0.956678 }, { 1.000000, 1.000000 } } }, { 8 }, { m } }, 0, 1 },
  // contributed by Pascal Obry
  { "Nikon D800", "NIKON", "D800", 0, 51200, { { { { 0.000000, 0.000000 }, { 0.001773, 0.001936 }, { 0.009671, 0.009693 }, { 0.016754, 0.020617 }, { 0.024884, 0.037309 }, { 0.048174, 0.107768 }, { 0.056932, 0.139532 }, { 0.085504, 0.233303 }, { 0.130378, 0.349747 }, { 0.155476, 0.405445 }, { 0.175245, 0.445918 }, { 0.217657, 0.516873 }, { 0.308475, 0.668608 }, { 0.375381, 0.754058 }, { 0.459858, 0.839909 }, { 0.509567, 0.881543 }, { 0.654394, 0.960877 }, { 0.783380, 0.999161 }, { 0.859310, 1.000000 }, { 1.000000, 1.000000 } } }, { 20 }, { m } }, 0, 1 },

  { natural,         "", "",                      0, 51200, { { { { 0.000000, 0.000000 }, { 0.040000, 0.025000 }, { 0.500000, 0.750000 }, { 1.000000, 1.000000 } } }, { 4 }, { m } }, 0, 1 },
  { canon_eos,       "Canon", "",                 0, 51200, { { { { 0.000000, 0.000000 }, { 0.028226, 0.029677 }, { 0.120968, 0.232258 }, { 0.459677, 0.747581 }, { 0.858871, 0.967742 }, { 1.000000, 1.000000 } } }, { 6 }, { m } }, 1, 0 },
  { canon_eos_alt,   "Canon", "EOS 5D Mark",      0, 51200, { { { { 0.000000, 0.000000 }, { 0.026210, 0.029677 }, { 0.108871, 0.232258 }, { 0.350806, 0.747581 }, { 0.669355, 0.967742 }, { 1.000000, 1.000000 } } }, { 6 }, { m } }, 1, 0 },
  { nikon,           "NIKON", "",                 0, 51200, { { { { 0.000000, 0.000000 }, { 0.036290, 0.036532 }, { 0.120968, 0.228226 }, { 0.459677, 0.759678 }, { 0.858871, 0.983468 }, { 1.000000, 1.000000 } } }, { 6 }, { m } }, 1, 0 },
  { nikon_alt,       "NIKON", "D____",            0, 51200, { { { { 0.000000, 0.000000 }, { 0.012097, 0.007322 }, { 0.072581, 0.130742 }, { 0.310484, 0.729291 }, { 0.611321, 0.951613 }, { 1.000000, 1.000000 } } }, { 6 }, { m } }, 1, 0 },
  { sony_alpha,      "SONY", "",                  0, 51200, { { { { 0.000000, 0.000000 }, { 0.031949, 0.036532 }, { 0.105431, 0.228226 }, { 0.434505, 0.759678 }, { 0.855738, 0.983468 }, { 1.000000, 1.000000 } } }, { 6 }, { m } }, 1, 0 },
  { pentax,          "PENTAX", "",                0, 51200, { { { { 0.000000, 0.000000 }, { 0.032258, 0.024596 }, { 0.120968, 0.166419 }, { 0.205645, 0.328527 }, { 0.604839, 0.790171 }, { 1.000000, 1.000000 } } }, { 6 }, { m } }, 1, 0 },
  { ricoh,           "RICOH", "",                 0, 51200, { { { { 0.000000, 0.000000 }, { 0.032259, 0.024596 }, { 0.120968, 0.166419 }, { 0.205645, 0.328527 }, { 0.604839, 0.790171 }, { 1.000000, 1.000000 } } }, { 6 }, { m } }, 1, 0 },
  { olympus,         "OLYMPUS", "",               0, 51200, { { { { 0.000000, 0.000000 }, { 0.033962, 0.028226 }, { 0.249057, 0.439516 }, { 0.501887, 0.798387 }, { 0.750943, 0.955645 }, { 1.000000, 1.000000 } } }, { 6 }, { m } }, 1, 0 },
  { olympus_alt,     "OLYMPUS", "E-M",            0, 51200, { { { { 0.000000, 0.000000 }, { 0.012097, 0.010322 }, { 0.072581, 0.167742 }, { 0.310484, 0.711291 }, { 0.645161, 0.956855 }, { 1.000000, 1.000000 } } }, { 6 }, { m } }, 1, 0 },
  { panasonic,       "Panasonic", "",             0, 51200, { { { { 0.000000, 0.000000 }, { 0.036290, 0.024596 }, { 0.120968, 0.166419 }, { 0.205645, 0.328527 }, { 0.604839, 0.790171 }, { 1.000000, 1.000000 } } }, { 6 }, { m } }, 1, 0 },
  { leica,           "Leica", "",                 0, 51200, { { { { 0.000000, 0.000000 }, { 0.036291, 0.024596 }, { 0.120968, 0.166419 }, { 0.205645, 0.328527 }, { 0.604839, 0.790171 }, { 1.000000, 1.000000 } } }, { 6 }, { m } }, 1, 0 },
  { kodak_easyshare, "EASTMAN KODAK COMPANY", "", 0, 51200, { { { { 0.000000, 0.000000 }, { 0.044355, 0.020967 }, { 0.133065, 0.154322 }, { 0.209677, 0.300301 }, { 0.572581, 0.753477 }, { 1.000000, 1.000000 } } }, { 6 }, { m } }, 1, 0 },
  { konica_minolta,  "MINOLTA", "",               0, 51200, { { { { 0.000000, 0.000000 }, { 0.020161, 0.010322 }, { 0.112903, 0.167742 }, { 0.500000, 0.711291 }, { 0.899194, 0.956855 }, { 1.000000, 1.000000 } } }, { 6 }, { m } }, 1, 0 },
  { samsung,         "SAMSUNG", "",               0, 51200, { { { { 0.000000, 0.000000 }, { 0.040323, 0.029677 }, { 0.133065, 0.232258 }, { 0.447581, 0.747581 }, { 0.842742, 0.967742 }, { 1.000000, 1.000000 } } }, { 6 }, { m } }, 1, 0 },
  { fujifilm,        "FUJIFILM", "",              0, 51200, { { { { 0.000000, 0.000000 }, { 0.028226, 0.029677 }, { 0.104839, 0.232258 }, { 0.387097, 0.747581 }, { 0.754032, 0.967742 }, { 1.000000, 1.000000 } } }, { 6 }, { m } }, 1, 0 },
  { nokia,           "Nokia", "",                 0, 51200, { { { { 0.000000, 0.000000 }, { 0.041825, 0.020161 }, { 0.117871, 0.153226 }, { 0.319392, 0.500000 }, { 0.638783, 0.842742 }, { 1.000000, 1.000000 } } }, { 6 }, { m } }, 1, 0 },
};
#undef m
static const int basecurve_presets_cnt = sizeof(basecurve_presets) / sizeof(basecurve_preset_t);

typedef struct dt_iop_basecurve_gui_data_t
{
  dt_draw_curve_t *minmax_curve; // curve for gui to draw
  int minmax_curve_type, minmax_curve_nodes;
  GtkBox *hbox;
  GtkDrawingArea *area;
  GtkWidget *scale;
  double mouse_x, mouse_y;
  int selected;
  double selected_offset, selected_y, selected_min, selected_max;
  float draw_xs[DT_IOP_TONECURVE_RES], draw_ys[DT_IOP_TONECURVE_RES];
  float draw_min_xs[DT_IOP_TONECURVE_RES], draw_min_ys[DT_IOP_TONECURVE_RES];
  float draw_max_xs[DT_IOP_TONECURVE_RES], draw_max_ys[DT_IOP_TONECURVE_RES];
  int loglogscale;
} dt_iop_basecurve_gui_data_t;

typedef struct dt_iop_basecurve_data_t
{
  dt_draw_curve_t *curve; // curve for gegl nodes and pixel processing
  int basecurve_type;
  int basecurve_nodes;
  float table[0x10000];      // precomputed look-up table for tone curve
  float unbounded_coeffs[3]; // approximation for extrapolation
} dt_iop_basecurve_data_t;

typedef struct dt_iop_basecurve_global_data_t
{
  int kernel_basecurve;
} dt_iop_basecurve_global_data_t;


const char *name()
{
  return _("base curve");
}

int groups()
{
  return IOP_GROUP_BASIC;
}


int flags()
{
  return IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_ONE_INSTANCE;
}


void init_presets(dt_iop_module_so_t *self)
{
  // transform presets above to db entries.
  // sql begin
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "begin", NULL, NULL, NULL);
  for(int k = 0; k < basecurve_presets_cnt; k++)
  {
    // add the preset.
    dt_gui_presets_add_generic(_(basecurve_presets[k].name), self->op, self->version(),
                               &basecurve_presets[k].params, sizeof(dt_iop_basecurve_params_t), 1);
    // and restrict it to model, maker, iso, and raw images
    dt_gui_presets_update_mml(_(basecurve_presets[k].name), self->op, self->version(),
                              basecurve_presets[k].maker, basecurve_presets[k].model, "");
    dt_gui_presets_update_iso(_(basecurve_presets[k].name), self->op, self->version(),
                              basecurve_presets[k].iso_min, basecurve_presets[k].iso_max);
    dt_gui_presets_update_ldr(_(basecurve_presets[k].name), self->op, self->version(), FOR_RAW);
    // make it auto-apply for matching images:
    dt_gui_presets_update_autoapply(_(basecurve_presets[k].name), self->op, self->version(),
                                    basecurve_presets[k].autoapply);
    // hide all non-matching presets in case the model string is set.
    dt_gui_presets_update_filter(_(basecurve_presets[k].name), self->op, self->version(),
                                 basecurve_presets[k].filter);
  }
  // sql commit
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "commit", NULL, NULL, NULL);
}

#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_basecurve_data_t *d = (dt_iop_basecurve_data_t *)piece->data;
  dt_iop_basecurve_global_data_t *gd = (dt_iop_basecurve_global_data_t *)self->data;

  cl_mem dev_m = NULL;
  cl_mem dev_coeffs = NULL;
  cl_int err = -999;
  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;

  size_t sizes[] = { ROUNDUPWD(width), ROUNDUPHT(height), 1 };
  dev_m = dt_opencl_copy_host_to_device(devid, d->table, 256, 256, sizeof(float));
  if(dev_m == NULL) goto error;

  dev_coeffs = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * 3, d->unbounded_coeffs);
  if(dev_coeffs == NULL) goto error;
  dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve, 1, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve, 2, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve, 3, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve, 4, sizeof(cl_mem), (void *)&dev_m);
  dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve, 5, sizeof(cl_mem), (void *)&dev_coeffs);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_basecurve, sizes);

  if(err != CL_SUCCESS) goto error;
  dt_opencl_release_mem_object(dev_m);
  dt_opencl_release_mem_object(dev_coeffs);
  return TRUE;

error:
  if(dev_m != NULL) dt_opencl_release_mem_object(dev_m);
  if(dev_coeffs != NULL) dt_opencl_release_mem_object(dev_coeffs);
  dt_print(DT_DEBUG_OPENCL, "[opencl_basecurve] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *i, void *o,
             const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  float *in = (float *)i;
  float *out = (float *)o;
  const int ch = piece->colors;
  dt_iop_basecurve_data_t *d = (dt_iop_basecurve_data_t *)(piece->data);
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(roi_out, out, d, in) schedule(static)
#endif
  for(size_t k = 0; k < (size_t)roi_out->width * roi_out->height; k++)
  {
    float *inp = in + ch * k;
    float *outp = out + ch * k;
    for(int i = 0; i < 3; i++)
    {
      // use base curve for values < 1, else use extrapolation.
      if(inp[i] < 1.0f)
        outp[i] = d->table[CLAMP((int)(inp[i] * 0x10000ul), 0, 0xffff)];
      else
        outp[i] = dt_iop_eval_exp(d->unbounded_coeffs, inp[i]);
    }

    outp[3] = inp[3];
  }
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_basecurve_data_t *d = (dt_iop_basecurve_data_t *)(piece->data);
  dt_iop_basecurve_params_t *p = (dt_iop_basecurve_params_t *)p1;

  const int ch = 0;
  // take care of possible change of curve type or number of nodes (not yet implemented in UI)
  if(d->basecurve_type != p->basecurve_type[ch] || d->basecurve_nodes != p->basecurve_nodes[ch])
  {
    if(d->curve) // catch initial init_pipe case
      dt_draw_curve_destroy(d->curve);
    d->curve = dt_draw_curve_new(0.0, 1.0, p->basecurve_type[ch]);
    d->basecurve_nodes = p->basecurve_nodes[ch];
    d->basecurve_type = p->basecurve_type[ch];
    for(int k = 0; k < p->basecurve_nodes[ch]; k++)
    {
      // printf("p->basecurve[%i][%i].x = %f;\n", ch, k, p->basecurve[ch][k].x);
      // printf("p->basecurve[%i][%i].y = %f;\n", ch, k, p->basecurve[ch][k].y);
      (void)dt_draw_curve_add_point(d->curve, p->basecurve[ch][k].x, p->basecurve[ch][k].y);
    }
  }
  else
  {
    for(int k = 0; k < p->basecurve_nodes[ch]; k++)
      dt_draw_curve_set_point(d->curve, k, p->basecurve[ch][k].x, p->basecurve[ch][k].y);
  }
  dt_draw_curve_calc_values(d->curve, 0.0f, 1.0f, 0x10000, NULL, d->table);

  // now the extrapolation stuff:
  const float xm = p->basecurve[0][p->basecurve_nodes[0] - 1].x;
  const float x[4] = { 0.7f * xm, 0.8f * xm, 0.9f * xm, 1.0f * xm };
  const float y[4] = { d->table[CLAMP((int)(x[0] * 0x10000ul), 0, 0xffff)],
                       d->table[CLAMP((int)(x[1] * 0x10000ul), 0, 0xffff)],
                       d->table[CLAMP((int)(x[2] * 0x10000ul), 0, 0xffff)],
                       d->table[CLAMP((int)(x[3] * 0x10000ul), 0, 0xffff)] };
  dt_iop_estimate_exp(x, y, 4, d->unbounded_coeffs);
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  // create part of the gegl pipeline
  piece->data = calloc(1, sizeof(dt_iop_basecurve_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  // clean up everything again.
  dt_iop_basecurve_data_t *d = (dt_iop_basecurve_data_t *)(piece->data);
  dt_draw_curve_destroy(d->curve);
  free(piece->data);
  piece->data = NULL;
}

void gui_update(struct dt_iop_module_t *self)
{
  // nothing to do, gui curve is read directly from params during expose event.
  gtk_widget_queue_draw(self->widget);
}

void init(dt_iop_module_t *module)
{
  module->params = malloc(sizeof(dt_iop_basecurve_params_t));
  module->default_params = malloc(sizeof(dt_iop_basecurve_params_t));
  module->default_enabled = 0;
  module->priority = 300; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_basecurve_params_t);
  module->gui_data = NULL;
  dt_iop_basecurve_params_t tmp = (dt_iop_basecurve_params_t){
    {
      { // three curves (L, a, b) with a number of nodes
        { 0.0, 0.0 },
        { 1.0, 1.0 }
      },
    },
    { 2, 0, 0 }, // number of nodes per curve
    { MONOTONE_HERMITE, MONOTONE_HERMITE, MONOTONE_HERMITE },
  };
  memcpy(module->params, &tmp, sizeof(dt_iop_basecurve_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_basecurve_params_t));
}

void cleanup(dt_iop_module_t *module)
{
  free(module->gui_data);
  module->gui_data = NULL;
  free(module->params);
  module->params = NULL;
}

void init_global(dt_iop_module_so_t *module)
{
  const int program = 2; // basic.cl, from programs.conf
  dt_iop_basecurve_global_data_t *gd
      = (dt_iop_basecurve_global_data_t *)malloc(sizeof(dt_iop_basecurve_global_data_t));
  module->data = gd;
  gd->kernel_basecurve = dt_opencl_create_kernel(program, "basecurve");
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_basecurve_global_data_t *gd = (dt_iop_basecurve_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_basecurve);
  free(module->data);
  module->data = NULL;
}

static gboolean dt_iop_basecurve_enter_notify(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_basecurve_gui_data_t *c = (dt_iop_basecurve_gui_data_t *)self->gui_data;
  c->mouse_x = fabs(c->mouse_x);
  c->mouse_y = fabs(c->mouse_y);
  gtk_widget_queue_draw(widget);
  return TRUE;
}

static gboolean dt_iop_basecurve_leave_notify(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_basecurve_gui_data_t *c = (dt_iop_basecurve_gui_data_t *)self->gui_data;
  // sign swapping for fluxbox
  c->mouse_x = -fabs(c->mouse_x);
  c->mouse_y = -fabs(c->mouse_y);
  gtk_widget_queue_draw(widget);
  return TRUE;
}

static float to_log(const float x, float base)
{
  if(base)
    return logf(x * (base - 1.0f) + 1.0f) / logf(base);
  else
    return x;
}
static float to_lin(const float x, float base)
{
  if(base)
    return (powf(base, x) - 1.0f) / (base - 1.0f);
  else
    return x;
}

static gboolean dt_iop_basecurve_draw(GtkWidget *widget, cairo_t *crf, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_basecurve_gui_data_t *c = (dt_iop_basecurve_gui_data_t *)self->gui_data;
  dt_iop_basecurve_params_t *p = (dt_iop_basecurve_params_t *)self->params;

  int nodes = p->basecurve_nodes[0];
  dt_iop_basecurve_node_t *basecurve = p->basecurve[0];
  if(c->minmax_curve_type != p->basecurve_type[0] || c->minmax_curve_nodes != p->basecurve_nodes[0])
  {
    dt_draw_curve_destroy(c->minmax_curve);
    c->minmax_curve = dt_draw_curve_new(0.0, 1.0, p->basecurve_type[0]);
    c->minmax_curve_nodes = p->basecurve_nodes[0];
    c->minmax_curve_type = p->basecurve_type[0];
    for(int k = 0; k < p->basecurve_nodes[0]; k++)
      (void)dt_draw_curve_add_point(c->minmax_curve, p->basecurve[0][k].x, p->basecurve[0][k].y);
  }
  else
  {
    for(int k = 0; k < p->basecurve_nodes[0]; k++)
      dt_draw_curve_set_point(c->minmax_curve, k, p->basecurve[0][k].x, p->basecurve[0][k].y);
  }
  dt_draw_curve_t *minmax_curve = c->minmax_curve;
  dt_draw_curve_calc_values(minmax_curve, 0.0, 1.0, DT_IOP_TONECURVE_RES, c->draw_xs, c->draw_ys);

  const float xm = basecurve[nodes - 1].x;
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

  cairo_translate(cr, 0, height);
  cairo_scale(cr, 1.0f, -1.0f);

  // draw grid
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(.4));
  cairo_set_source_rgb(cr, .1, .1, .1);
  if(c->loglogscale)
    dt_draw_loglog_grid(cr, 4, 0, 0, width, height, c->loglogscale);
  else
    dt_draw_grid(cr, 4, 0, 0, width, height);

  // draw nodes positions
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.));
  cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
  for(int k = 0; k < nodes; k++)
  {
    const float x = to_log(basecurve[k].x, c->loglogscale), y = to_log(basecurve[k].y, c->loglogscale);
    cairo_arc(cr, x * width, y * height, DT_PIXEL_APPLY_DPI(3), 0, 2. * M_PI);
    cairo_stroke(cr);
  }

  // draw selected cursor
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.));

  if(c->selected >= 0)
  {
    cairo_set_source_rgb(cr, .9, .9, .9);
    const float x = to_log(basecurve[c->selected].x, c->loglogscale),
                y = to_log(basecurve[c->selected].y, c->loglogscale);
    cairo_arc(cr, x * width, y * height, DT_PIXEL_APPLY_DPI(4), 0, 2. * M_PI);
    cairo_stroke(cr);
  }

  // draw curve
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(2.));
  cairo_set_source_rgb(cr, .9, .9, .9);
  // cairo_set_line_cap  (cr, CAIRO_LINE_CAP_SQUARE);
  cairo_move_to(cr, 0, height * to_log(c->draw_ys[0], c->loglogscale));
  for(int k = 1; k < DT_IOP_TONECURVE_RES; k++)
  {
    const float xx = k / (DT_IOP_TONECURVE_RES - 1.0);
    if(xx > xm)
    {
      const float yy = dt_iop_eval_exp(unbounded_coeffs, xx);
      const float x = to_log(xx, c->loglogscale), y = to_log(yy, c->loglogscale);
      cairo_line_to(cr, x * width, height * y);
    }
    else
    {
      const float yy = c->draw_ys[k];
      const float x = to_log(xx, c->loglogscale), y = to_log(yy, c->loglogscale);
      cairo_line_to(cr, x * width, height * y);
    }
  }
  cairo_stroke(cr);

  cairo_destroy(cr);
  cairo_set_source_surface(crf, cst, 0, 0);
  cairo_paint(crf);
  cairo_surface_destroy(cst);
  return TRUE;
}

static gboolean dt_iop_basecurve_motion_notify(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_basecurve_gui_data_t *c = (dt_iop_basecurve_gui_data_t *)self->gui_data;
  dt_iop_basecurve_params_t *p = (dt_iop_basecurve_params_t *)self->params;
  int ch = 0;
  int nodes = p->basecurve_nodes[ch];
  dt_iop_basecurve_node_t *basecurve = p->basecurve[ch];

  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  const int inset = DT_GUI_CURVE_EDITOR_INSET;
  int height = allocation.height - 2 * inset, width = allocation.width - 2 * inset;
  c->mouse_x = CLAMP(event->x - inset, 0, width);
  c->mouse_y = CLAMP(event->y - inset, 0, height);

  const float mx = c->mouse_x / (float)width;
  const float my = 1.0f - c->mouse_y / (float)height;
  const float linx = to_lin(mx, c->loglogscale), liny = to_lin(my, c->loglogscale);

  if(event->state & GDK_BUTTON1_MASK)
  {
    // got a vertex selected:
    if(c->selected >= 0)
    {
      basecurve[c->selected].x = linx;
      basecurve[c->selected].y = liny;

      // delete vertex if order has changed:
      if(nodes > 2)
        if((c->selected > 0 && basecurve[c->selected - 1].x >= linx)
           || (c->selected < nodes - 1 && basecurve[c->selected + 1].x <= linx))
        {
          for(int k = c->selected; k < nodes - 1; k++)
          {
            basecurve[k].x = basecurve[k + 1].x;
            basecurve[k].y = basecurve[k + 1].y;
          }
          c->selected = -2; // avoid re-insertion of that point immediately after this
          p->basecurve_nodes[ch]--;
        }
      dt_dev_add_history_item(darktable.develop, self, TRUE);
    }
    else if(nodes < 20 && c->selected >= -1)
    {
      // no vertex was close, create a new one!
      if(basecurve[0].x > linx)
        c->selected = 0;
      else
        for(int k = 1; k < nodes; k++)
        {
          if(basecurve[k].x > linx)
          {
            c->selected = k;
            break;
          }
        }
      if(c->selected == -1) c->selected = nodes;
      for(int i = nodes; i > c->selected; i--)
      {
        basecurve[i].x = basecurve[i - 1].x;
        basecurve[i].y = basecurve[i - 1].y;
      }
      // found a new point
      basecurve[c->selected].x = linx;
      basecurve[c->selected].y = liny;
      p->basecurve_nodes[ch]++;
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
      float dist
          = (my - to_log(basecurve[k].y, c->loglogscale)) * (my - to_log(basecurve[k].y, c->loglogscale))
            + (mx - to_log(basecurve[k].x, c->loglogscale)) * (mx - to_log(basecurve[k].x, c->loglogscale));
      if(dist < min)
      {
        min = dist;
        nearest = k;
      }
    }
    c->selected = nearest;
  }
  gtk_widget_queue_draw(widget);
  return TRUE;
}

static gboolean dt_iop_basecurve_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_basecurve_params_t *p = (dt_iop_basecurve_params_t *)self->params;
  dt_iop_basecurve_params_t *d = (dt_iop_basecurve_params_t *)self->default_params;
  dt_iop_basecurve_gui_data_t *c = (dt_iop_basecurve_gui_data_t *)self->gui_data;

  int ch = 0;

  if(event->button == 1 && event->type == GDK_2BUTTON_PRESS)
  {
    // reset current curve
    p->basecurve_nodes[ch] = d->basecurve_nodes[ch];
    p->basecurve_type[ch] = d->basecurve_type[ch];
    for(int k = 0; k < d->basecurve_nodes[ch]; k++)
    {
      p->basecurve[ch][k].x = d->basecurve[ch][k].x;
      p->basecurve[ch][k].y = d->basecurve[ch][k].y;
    }
    c->selected = -2; // avoid motion notify re-inserting immediately.
    dt_dev_add_history_item(darktable.develop, self, TRUE);
    gtk_widget_queue_draw(self->widget);
    return TRUE;
  }
  return FALSE;
}

static gboolean area_resized(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  GtkRequisition r;
  r.width = allocation.width;
  r.height = allocation.width;
  gtk_widget_get_preferred_size(widget, &r, NULL);
  return TRUE;
}

static gboolean scrolled(GtkWidget *widget, GdkEventScroll *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_basecurve_params_t *p = (dt_iop_basecurve_params_t *)self->params;
  dt_iop_basecurve_gui_data_t *c = (dt_iop_basecurve_gui_data_t *)self->gui_data;

  int ch = 0;
  dt_iop_basecurve_node_t *basecurve = p->basecurve[ch];

  if(c->selected >= 0)
  {
    if(event->direction == GDK_SCROLL_UP)
      basecurve[c->selected].y = MAX(0.0f, basecurve[c->selected].y + 0.001f);
    if(event->direction == GDK_SCROLL_DOWN)
      basecurve[c->selected].y = MIN(1.0f, basecurve[c->selected].y - 0.001f);
    dt_dev_add_history_item(darktable.develop, self, TRUE);
    gtk_widget_queue_draw(widget);
  }
  return TRUE;
}

static void scale_callback(GtkWidget *widget, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_basecurve_gui_data_t *g = (dt_iop_basecurve_gui_data_t *)self->gui_data;
  if(dt_bauhaus_combobox_get(widget))
    g->loglogscale = 64;
  else
    g->loglogscale = 0;
  gtk_widget_queue_draw(GTK_WIDGET(g->area));
}

void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_basecurve_gui_data_t));
  dt_iop_basecurve_gui_data_t *c = (dt_iop_basecurve_gui_data_t *)self->gui_data;
  dt_iop_basecurve_params_t *p = (dt_iop_basecurve_params_t *)self->params;

  c->minmax_curve = dt_draw_curve_new(0.0, 1.0, p->basecurve_type[0]);
  c->minmax_curve_type = p->basecurve_type[0];
  c->minmax_curve_nodes = p->basecurve_nodes[0];
  for(int k = 0; k < p->basecurve_nodes[0]; k++)
    (void)dt_draw_curve_add_point(c->minmax_curve, p->basecurve[0][k].x, p->basecurve[0][k].y);
  c->mouse_x = c->mouse_y = -1.0;
  c->selected = -1;
  c->loglogscale = 0;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);
  c->area = GTK_DRAWING_AREA(dtgtk_drawing_area_new_with_aspect_ratio(1.0));
  g_object_set(G_OBJECT(c->area), "tooltip-text",
               _("abscissa: input, ordinate: output. works on RGB channels"), (char *)NULL);

  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(c->area), TRUE, TRUE, 0);

  c->scale = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(c->scale, NULL, _("scale"));
  dt_bauhaus_combobox_add(c->scale, _("linear"));
  dt_bauhaus_combobox_add(c->scale, _("logarithmic"));
  g_object_set(c->scale, "tooltip-text",
               _("scale to use in the graph. use logarithmic scale for more precise control near the blacks"),
               (char *)NULL);
  gtk_box_pack_start(GTK_BOX(self->widget), c->scale, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(c->scale), "value-changed", G_CALLBACK(scale_callback), self);

  gtk_widget_add_events(GTK_WIDGET(c->area), GDK_POINTER_MOTION_MASK | GDK_POINTER_MOTION_HINT_MASK
                                             | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK
                                             | GDK_LEAVE_NOTIFY_MASK | GDK_SCROLL_MASK);
  g_signal_connect(G_OBJECT(c->area), "draw", G_CALLBACK(dt_iop_basecurve_draw), self);
  g_signal_connect(G_OBJECT(c->area), "button-press-event", G_CALLBACK(dt_iop_basecurve_button_press), self);
  g_signal_connect(G_OBJECT(c->area), "motion-notify-event", G_CALLBACK(dt_iop_basecurve_motion_notify), self);
  g_signal_connect(G_OBJECT(c->area), "leave-notify-event", G_CALLBACK(dt_iop_basecurve_leave_notify), self);
  g_signal_connect(G_OBJECT(c->area), "enter-notify-event", G_CALLBACK(dt_iop_basecurve_enter_notify), self);
  g_signal_connect(G_OBJECT(c->area), "configure-event", G_CALLBACK(area_resized), self);
  g_signal_connect(G_OBJECT(c->area), "scroll-event", G_CALLBACK(scrolled), self);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  dt_iop_basecurve_gui_data_t *c = (dt_iop_basecurve_gui_data_t *)self->gui_data;
  dt_draw_curve_destroy(c->minmax_curve);
  free(self->gui_data);
  self->gui_data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
