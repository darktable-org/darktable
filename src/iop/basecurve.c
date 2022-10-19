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
#include "bauhaus/bauhaus.h"
#include "common/colorspaces_inline_conversions.h"
#include "common/debug.h"
#include "common/math.h"
#include "common/opencl.h"
#include "common/rgb_norms.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "develop/imageop_gui.h"
#include "develop/tiling.h"
#include "dtgtk/drawingarea.h"
#include "gui/draw.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "gui/accelerators.h"
#include "iop/iop_api.h"

#include <assert.h>
#include <gtk/gtk.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#define DT_GUI_CURVE_EDITOR_INSET DT_PIXEL_APPLY_DPI(5)
#define DT_IOP_TONECURVE_RES 256
#define MAXNODES 20


DT_MODULE_INTROSPECTION(6, dt_iop_basecurve_params_t)

typedef struct dt_iop_basecurve_node_t
{
  float x; // $MIN: 0.0 $MAX: 1.0
  float y; // $MIN: 0.0 $MAX: 1.0
} dt_iop_basecurve_node_t;

typedef struct dt_iop_basecurve_params_t
{
  // three curves (c, ., .) with max number of nodes
  // the other two are reserved, maybe we'll have cam rgb at some point.
  dt_iop_basecurve_node_t basecurve[3][MAXNODES];
  int basecurve_nodes[3]; // $MIN: 0 $MAX: MAXNODES $DEFAULT: 0
  int basecurve_type[3];  // $MIN: 0 $MAX: MONOTONE_HERMITE $DEFAULT: MONOTONE_HERMITE
  int exposure_fusion;    /* number of exposure fusion steps
                             $DEFAULT: 0 $DESCRIPTION: "fusion" */
  float exposure_stops;   /* number of stops between fusion images
                             $MIN: 0.01 $MAX: 4.0 $DEFAULT: 1.0 $DESCRIPTION: "exposure shift" */
  float exposure_bias;    /* whether to do exposure-fusion with over or under-exposure
                             $MIN: -1.0 $MAX: 1.0 $DEFAULT: 1.0 $DESCRIPTION: "exposure bias" */
  dt_iop_rgb_norms_t preserve_colors; /* $DEFAULT: DT_RGB_NORM_LUMINANCE $DESCRIPTION: "preserve colors" */
} dt_iop_basecurve_params_t;

typedef struct dt_iop_basecurve_params5_t
{
  // three curves (c, ., .) with max number of nodes
  // the other two are reserved, maybe we'll have cam rgb at some point.
  dt_iop_basecurve_node_t basecurve[3][MAXNODES];
  int basecurve_nodes[3];
  int basecurve_type[3];
  int exposure_fusion;    // number of exposure fusion steps
  float exposure_stops;   // number of stops between fusion images
  float exposure_bias;    // whether to do exposure-fusion with over or under-exposure
} dt_iop_basecurve_params5_t;

typedef struct dt_iop_basecurve_params3_t
{
  // three curves (c, ., .) with max number of nodes
  // the other two are reserved, maybe we'll have cam rgb at some point.
  dt_iop_basecurve_node_t basecurve[3][MAXNODES];
  int basecurve_nodes[3];
  int basecurve_type[3];
  int exposure_fusion;  // number of exposure fusion steps
  float exposure_stops; // number of stops between fusion images
} dt_iop_basecurve_params3_t;

// same but semantics/defaults changed
typedef dt_iop_basecurve_params3_t dt_iop_basecurve_params4_t;

typedef struct dt_iop_basecurve_params2_t
{
  // three curves (c, ., .) with max number of nodes
  // the other two are reserved, maybe we'll have cam rgb at some point.
  dt_iop_basecurve_node_t basecurve[3][MAXNODES];
  int basecurve_nodes[3];
  int basecurve_type[3];
} dt_iop_basecurve_params2_t;

typedef struct dt_iop_basecurve_params1_t
{
  float tonecurve_x[6], tonecurve_y[6];
  int tonecurve_preset;
} dt_iop_basecurve_params1_t;

int legacy_params(dt_iop_module_t *self, const void *const old_params, const int old_version,
                  void *new_params, const int new_version)
{
  if(old_version == 1 && new_version == 6)
  {
    dt_iop_basecurve_params1_t *o = (dt_iop_basecurve_params1_t *)old_params;
    dt_iop_basecurve_params_t *n = (dt_iop_basecurve_params_t *)new_params;

    // start with a fresh copy of default parameters
    // unfortunately default_params aren't inited at this stage.
    *n = (dt_iop_basecurve_params_t){ {
                                        { { 0.0, 0.0 }, { 1.0, 1.0 } },
                                      },
                                      { 2, 3, 3 },
                                      { MONOTONE_HERMITE, MONOTONE_HERMITE, MONOTONE_HERMITE } , 0, 1};
    for(int k = 0; k < 6; k++) n->basecurve[0][k].x = o->tonecurve_x[k];
    for(int k = 0; k < 6; k++) n->basecurve[0][k].y = o->tonecurve_y[k];
    n->basecurve_nodes[0] = 6;
    n->basecurve_type[0] = CUBIC_SPLINE;
    n->exposure_fusion = 0;
    n->exposure_stops = 1;
    n->exposure_bias = 1.0;
    n->preserve_colors = DT_RGB_NORM_NONE;
    return 0;
  }
  if(old_version == 2 && new_version == 6)
  {
    dt_iop_basecurve_params2_t *o = (dt_iop_basecurve_params2_t *)old_params;
    dt_iop_basecurve_params_t *n = (dt_iop_basecurve_params_t *)new_params;
    memcpy(n, o, sizeof(dt_iop_basecurve_params2_t));
    n->exposure_fusion = 0;
    n->exposure_stops = 1;
    n->exposure_bias = 1.0;
    n->preserve_colors = DT_RGB_NORM_NONE;
    return 0;
  }
  if(old_version == 3 && new_version == 6)
  {
    dt_iop_basecurve_params3_t *o = (dt_iop_basecurve_params3_t *)old_params;
    dt_iop_basecurve_params_t *n = (dt_iop_basecurve_params_t *)new_params;
    memcpy(n, o, sizeof(dt_iop_basecurve_params3_t));
    n->exposure_stops = (o->exposure_fusion == 0 && o->exposure_stops == 0) ? 1.0f : o->exposure_stops;
    n->exposure_bias = 1.0;
    n->preserve_colors = DT_RGB_NORM_NONE;
    return 0;
  }
  if(old_version == 4 && new_version == 6)
  {
    dt_iop_basecurve_params4_t *o = (dt_iop_basecurve_params4_t *)old_params;
    dt_iop_basecurve_params_t *n = (dt_iop_basecurve_params_t *)new_params;
    memcpy(n, o, sizeof(dt_iop_basecurve_params4_t));
    n->exposure_bias = 1.0;
    n->preserve_colors = DT_RGB_NORM_NONE;
    return 0;
  }
  if(old_version == 5 && new_version == 6)
  {
    dt_iop_basecurve_params5_t *o = (dt_iop_basecurve_params5_t *)old_params;
    dt_iop_basecurve_params_t *n = (dt_iop_basecurve_params_t *)new_params;
    memcpy(n, o, sizeof(dt_iop_basecurve_params5_t));
    n->preserve_colors = DT_RGB_NORM_NONE;
    return 0;
  }
  return 1;
}

typedef struct dt_iop_basecurve_gui_data_t
{
  dt_draw_curve_t *minmax_curve; // curve for gui to draw
  int minmax_curve_type, minmax_curve_nodes;
  GtkBox *hbox;
  GtkDrawingArea *area;
  GtkWidget *fusion, *exposure_step, *exposure_bias;
  GtkWidget *cmb_preserve_colors;
  double mouse_x, mouse_y;
  int selected;
  double selected_offset, selected_y, selected_min, selected_max;
  float draw_xs[DT_IOP_TONECURVE_RES], draw_ys[DT_IOP_TONECURVE_RES];
  float draw_min_xs[DT_IOP_TONECURVE_RES], draw_min_ys[DT_IOP_TONECURVE_RES];
  float draw_max_xs[DT_IOP_TONECURVE_RES], draw_max_ys[DT_IOP_TONECURVE_RES];
  float loglogscale;
  GtkWidget *logbase;
} dt_iop_basecurve_gui_data_t;

static const char neutral[] = N_("neutral");
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
  int iso_min;
  float iso_max;
  dt_iop_basecurve_params_t params;
  int autoapply;
  int filter;
} basecurve_preset_t;

#define m MONOTONE_HERMITE

static const basecurve_preset_t basecurve_camera_presets[] = {
  // copy paste your measured basecurve line at the top here, like so (note the exif data and the last 1):
  // clang-format off

  // nikon d750 by Edouard Gomez
  {"Nikon D750", "NIKON CORPORATION", "NIKON D750", 0, FLT_MAX, {{{{0.000000, 0.000000}, {0.018124, 0.026126}, {0.143357, 0.370145}, {0.330116, 0.730507}, {0.457952, 0.853462}, {0.734950, 0.965061}, {0.904758, 0.985699}, {1.000000, 1.000000}}}, {8}, {m}, 0, 0, 0, DT_RGB_NORM_LUMINANCE}, 0, 1},
  // contributed by Stefan Kauerauf
  {"Nikon D5100", "NIKON CORPORATION", "NIKON D5100", 0, FLT_MAX, {{{{0.000000, 0.000000}, {0.001113, 0.000506}, {0.002842, 0.001338}, {0.005461, 0.002470}, {0.011381, 0.006099}, {0.013303, 0.007758}, {0.034638, 0.041119}, {0.044441, 0.063882}, {0.070338, 0.139639}, {0.096068, 0.210915}, {0.137693, 0.310295}, {0.206041, 0.432674}, {0.255508, 0.504447}, {0.302770, 0.569576}, {0.425625, 0.726755}, {0.554526, 0.839541}, {0.621216, 0.882839}, {0.702662, 0.927072}, {0.897426, 0.990984}, {1.000000, 1.000000}}}, {20}, {m}, 0, 0, 0, DT_RGB_NORM_LUMINANCE}, 0, 1},
  // nikon d7000 by Edouard Gomez
  {"Nikon D7000", "NIKON CORPORATION", "NIKON D7000", 0, FLT_MAX, {{{{0.000000, 0.000000}, {0.001943, 0.003040}, {0.019814, 0.028810}, {0.080784, 0.210476}, {0.145700, 0.383873}, {0.295961, 0.654041}, {0.651915, 0.952819}, {1.000000, 1.000000}}}, {8}, {m}, 0, 0, 0, DT_RGB_NORM_LUMINANCE}, 0, 1},
  // nikon d7200 standard by Ralf Brown (firmware 1.00)
  {"Nikon D7200", "NIKON CORPORATION", "NIKON D7200", 0, FLT_MAX, {{{{0.000000, 0.000000}, {0.001604, 0.001334}, {0.007401, 0.005237}, {0.009474, 0.006890}, {0.017348, 0.017176}, {0.032782, 0.044336}, {0.048033, 0.086548}, {0.075803, 0.168331}, {0.109539, 0.273539}, {0.137373, 0.364645}, {0.231651, 0.597511}, {0.323797, 0.736475}, {0.383796, 0.805797}, {0.462284, 0.872247}, {0.549844, 0.918328}, {0.678855, 0.962361}, {0.817445, 0.990406}, {1.000000, 1.000000}}}, {18}, {m}, 0, 0, 0, DT_RGB_NORM_LUMINANCE}, 0, 1},
  // nikon d7500 by Anders Bennehag (firmware C 1.00, LD 2.016)
  {"NIKON D7500", "NIKON CORPORATION", "NIKON D7500", 0, FLT_MAX, {{{{0.000000, 0.000000}, {0.000892, 0.001062}, {0.002280, 0.001768}, {0.013983, 0.011368}, {0.032597, 0.044700}, {0.050065, 0.097131}, {0.084129, 0.219954}, {0.120975, 0.336806}, {0.170730, 0.473752}, {0.258677, 0.647113}, {0.409997, 0.827417}, {0.499979, 0.889468}, {0.615564, 0.941960}, {0.665272, 0.957736}, {0.832126, 0.991968}, {1.000000, 1.000000}}}, {16}, {m}, 0, 0, 0, DT_RGB_NORM_LUMINANCE}, 0, 1},
  // sony rx100m2 by Günther R.
  { "Sony DSC-RX100M2", "SONY", "DSC-RX100M2", 0, FLT_MAX, { { { { 0.000000, 0.000000 }, { 0.015106, 0.008116 }, { 0.070077, 0.093725 }, { 0.107484, 0.170723 }, { 0.191528, 0.341093 }, { 0.257996, 0.458453 }, { 0.305381, 0.537267 }, { 0.326367, 0.569257 }, { 0.448067, 0.723742 }, { 0.509627, 0.777966 }, { 0.676751, 0.898797 }, { 1.000000, 1.000000 } } }, { 12 }, { m } , 0, 0, 0, DT_RGB_NORM_LUMINANCE}, 0, 1 },
  // contributed by matthias bodenbinder
  { "Canon EOS 6D", "Canon", "Canon EOS 6D", 0, FLT_MAX, { { { { 0.000000, 0.002917 }, { 0.000751, 0.001716 }, { 0.006011, 0.004438 }, { 0.020286, 0.021725 }, { 0.048084, 0.085918 }, { 0.093914, 0.233804 }, { 0.162284, 0.431375 }, { 0.257701, 0.629218 }, { 0.384673, 0.800332 }, { 0.547709, 0.917761 }, { 0.751315, 0.988132 }, { 1.000000, 0.999943 } } }, { 12 }, { m } , 0, 0, 0, DT_RGB_NORM_LUMINANCE}, 0, 1 },
  // contributed by Dan Torop
  { "Fujifilm X100S", "Fujifilm", "X100S", 0, FLT_MAX, { { { { 0.000000, 0.000000 }, { 0.009145, 0.007905 }, { 0.026570, 0.032201 }, { 0.131526, 0.289717 }, { 0.175858, 0.395263 }, { 0.350981, 0.696899 }, { 0.614997, 0.959451 }, { 1.000000, 1.000000 } } }, { 8 }, { m } , 0, 0, 0, DT_RGB_NORM_LUMINANCE}, 0, 1 },
  { "Fujifilm X100T", "Fujifilm", "X100T", 0, FLT_MAX, { { { { 0.000000, 0.000000 }, { 0.009145, 0.007905 }, { 0.026570, 0.032201 }, { 0.131526, 0.289717 }, { 0.175858, 0.395263 }, { 0.350981, 0.696899 }, { 0.614997, 0.959451 }, { 1.000000, 1.000000 } } }, { 8 }, { m } , 0, 0, 0, DT_RGB_NORM_LUMINANCE}, 0, 1 },
  // contributed by Johannes Hanika
  { "Canon EOS 5D Mark II", "Canon", "Canon EOS 5D Mark II", 0, FLT_MAX, { { { { 0.000000, 0.000366 }, { 0.006560, 0.003504 }, { 0.027310, 0.029834 }, { 0.045915, 0.070230 }, { 0.206554, 0.539895 }, { 0.442337, 0.872409 }, { 0.673263, 0.971703 }, { 1.000000, 0.999832 } } }, { 8 }, { m } , 0, 0, 0, DT_RGB_NORM_LUMINANCE}, 0, 1 },
  // contributed by chrik5
  { "Pentax K-5", "Pentax", "Pentax K-5", 0, FLT_MAX, { { { { 0.000000, 0.000000 }, { 0.004754, 0.002208 }, { 0.009529, 0.004214 }, { 0.023713, 0.013508 }, { 0.031866, 0.020352 }, { 0.046734, 0.034063 }, { 0.059989, 0.052413 }, { 0.088415, 0.096030 }, { 0.136610, 0.190629 }, { 0.174480, 0.256484 }, { 0.205192, 0.307430 }, { 0.228896, 0.348447 }, { 0.286411, 0.428680 }, { 0.355314, 0.513527 }, { 0.440014, 0.607651 }, { 0.567096, 0.732791 }, { 0.620597, 0.775968 }, { 0.760355, 0.881828 }, { 0.875139, 0.960682 }, { 1.000000, 1.000000 } } }, { 20 }, { m } , 0, 0, 0, DT_RGB_NORM_LUMINANCE}, 0, 1 },
  // contributed by Togan Muftuoglu - ed: slope is too aggressive on shadows
  //{ "Nikon D90", "NIKON", "D90", 0, FLT_MAX, { { { { 0.000000, 0.000000 }, { 0.015520, 0.012248 }, { 0.097950, 0.251013 }, { 0.301515, 0.621951 }, { 0.415513, 0.771384 }, { 0.547326, 0.843079 }, { 0.819769, 0.956678 }, { 1.000000, 1.000000 } } }, { 8 }, { m } }, 0, 1 },
  // contributed by Edouard Gomez
  {"Nikon D90", "NIKON CORPORATION", "NIKON D90", 0, FLT_MAX, {{{{0.000000, 0.000000}, {0.011702, 0.012659}, {0.122918, 0.289973}, {0.153642, 0.342731}, {0.246855, 0.510114}, {0.448958, 0.733820}, {0.666759, 0.894290}, {1.000000, 1.000000}}}, {8}, {m}, 0, 0, 0, DT_RGB_NORM_LUMINANCE}, 0, 1},
  // contributed by Pascal Obry
  { "Nikon D800", "NIKON", "NIKON D800", 0, FLT_MAX, { { { { 0.000000, 0.000000 }, { 0.001773, 0.001936 }, { 0.009671, 0.009693 }, { 0.016754, 0.020617 }, { 0.024884, 0.037309 }, { 0.048174, 0.107768 }, { 0.056932, 0.139532 }, { 0.085504, 0.233303 }, { 0.130378, 0.349747 }, { 0.155476, 0.405445 }, { 0.175245, 0.445918 }, { 0.217657, 0.516873 }, { 0.308475, 0.668608 }, { 0.375381, 0.754058 }, { 0.459858, 0.839909 }, { 0.509567, 0.881543 }, { 0.654394, 0.960877 }, { 0.783380, 0.999161 }, { 0.859310, 1.000000 }, { 1.000000, 1.000000 } } }, { 20 }, { m } , 0, 0, 0, DT_RGB_NORM_LUMINANCE}, 0, 1 },
  // contributed by Lukas Schrangl
  {"Olympus OM-D E-M10 II", "OLYMPUS CORPORATION    ", "E-M10MarkII     ", 0, FLT_MAX, {{{{0.000000, 0.000000}, {0.005707, 0.004764}, {0.018944, 0.024456}, {0.054501, 0.129992}, {0.075665, 0.211873}, {0.119641, 0.365771}, {0.173148, 0.532024}, {0.247979, 0.668989}, {0.357597, 0.780138}, {0.459003, 0.839829}, {0.626844, 0.904426}, {0.769425, 0.948541}, {0.820429, 0.964715}, {1.000000, 1.000000}}}, {14}, {m}, 0, 0, 0, DT_RGB_NORM_LUMINANCE}, 0, 1},
  // clang-format on
};
static const int basecurve_camera_presets_cnt = sizeof(basecurve_camera_presets) / sizeof(basecurve_preset_t);

static const basecurve_preset_t basecurve_presets[] = {
  // clang-format off
  // smoother cubic spline curve
  { N_("cubic spline"), "", "", 0, FLT_MAX, { { { { 0.0, 0.0}, { 1.0, 1.0 }, { 0., 0.}, { 0., 0.}, { 0., 0.}, { 0., 0.}, { 0., 0.}, { 0., 0.} } }, { 2 }, { CUBIC_SPLINE }, 0, 0, 0, DT_RGB_NORM_LUMINANCE }, 0, 0 },
  { neutral,         "", "",                      0, FLT_MAX, { { { { 0.000000, 0.000000 }, { 0.005000, 0.002500 }, { 0.150000, 0.300000 }, { 0.400000, 0.700000 }, { 0.750000, 0.950000 }, { 1.000000, 1.000000 } } }, { 6 }, { m } , 0, 0, 0, DT_RGB_NORM_LUMINANCE}, 0, 1 },
  { canon_eos,       "Canon", "",                 0, FLT_MAX, { { { { 0.000000, 0.000000 }, { 0.028226, 0.029677 }, { 0.120968, 0.232258 }, { 0.459677, 0.747581 }, { 0.858871, 0.967742 }, { 1.000000, 1.000000 } } }, { 6 }, { m }, 0, 0, 0, DT_RGB_NORM_LUMINANCE }, 0, 0 },
  { canon_eos_alt,   "Canon", "EOS 5D Mark%",      0, FLT_MAX, { { { { 0.000000, 0.000000 }, { 0.026210, 0.029677 }, { 0.108871, 0.232258 }, { 0.350806, 0.747581 }, { 0.669355, 0.967742 }, { 1.000000, 1.000000 } } }, { 6 }, { m }, 0, 0, 0, DT_RGB_NORM_LUMINANCE }, 0, 0 },
  { nikon,           "NIKON", "",                 0, FLT_MAX, { { { { 0.000000, 0.000000 }, { 0.036290, 0.036532 }, { 0.120968, 0.228226 }, { 0.459677, 0.759678 }, { 0.858871, 0.983468 }, { 1.000000, 1.000000 } } }, { 6 }, { m }, 0, 0, 0, DT_RGB_NORM_LUMINANCE }, 0, 0 },
  { nikon_alt,       "NIKON", "%D____%",            0, FLT_MAX, { { { { 0.000000, 0.000000 }, { 0.012097, 0.007322 }, { 0.072581, 0.130742 }, { 0.310484, 0.729291 }, { 0.611321, 0.951613 }, { 1.000000, 1.000000 } } }, { 6 }, { m }, 0, 0, 0, DT_RGB_NORM_LUMINANCE }, 0, 0 },
  { sony_alpha,      "SONY", "",                  0, FLT_MAX, { { { { 0.000000, 0.000000 }, { 0.031949, 0.036532 }, { 0.105431, 0.228226 }, { 0.434505, 0.759678 }, { 0.855738, 0.983468 }, { 1.000000, 1.000000 } } }, { 6 }, { m }, 0, 0, 0, DT_RGB_NORM_LUMINANCE }, 0, 0 },
  { pentax,          "PENTAX", "",                0, FLT_MAX, { { { { 0.000000, 0.000000 }, { 0.032258, 0.024596 }, { 0.120968, 0.166419 }, { 0.205645, 0.328527 }, { 0.604839, 0.790171 }, { 1.000000, 1.000000 } } }, { 6 }, { m }, 0, 0, 0, DT_RGB_NORM_LUMINANCE }, 0, 0 },
  { ricoh,           "RICOH", "",                 0, FLT_MAX, { { { { 0.000000, 0.000000 }, { 0.032259, 0.024596 }, { 0.120968, 0.166419 }, { 0.205645, 0.328527 }, { 0.604839, 0.790171 }, { 1.000000, 1.000000 } } }, { 6 }, { m }, 0, 0, 0, DT_RGB_NORM_LUMINANCE }, 0, 0 },
  { olympus,         "OLYMPUS", "",               0, FLT_MAX, { { { { 0.000000, 0.000000 }, { 0.033962, 0.028226 }, { 0.249057, 0.439516 }, { 0.501887, 0.798387 }, { 0.750943, 0.955645 }, { 1.000000, 1.000000 } } }, { 6 }, { m }, 0, 0, 0, DT_RGB_NORM_LUMINANCE }, 0, 0 },
  { olympus_alt,     "OLYMPUS", "E-M%",            0, FLT_MAX, { { { { 0.000000, 0.000000 }, { 0.012097, 0.010322 }, { 0.072581, 0.167742 }, { 0.310484, 0.711291 }, { 0.645161, 0.956855 }, { 1.000000, 1.000000 } } }, { 6 }, { m }, 0, 0, 0, DT_RGB_NORM_LUMINANCE }, 0, 0 },
  { panasonic,       "Panasonic", "",             0, FLT_MAX, { { { { 0.000000, 0.000000 }, { 0.036290, 0.024596 }, { 0.120968, 0.166419 }, { 0.205645, 0.328527 }, { 0.604839, 0.790171 }, { 1.000000, 1.000000 } } }, { 6 }, { m }, 0, 0, 0, DT_RGB_NORM_LUMINANCE }, 0, 0 },
  { leica,           "Leica", "",                 0, FLT_MAX, { { { { 0.000000, 0.000000 }, { 0.036291, 0.024596 }, { 0.120968, 0.166419 }, { 0.205645, 0.328527 }, { 0.604839, 0.790171 }, { 1.000000, 1.000000 } } }, { 6 }, { m }, 0, 0, 0, DT_RGB_NORM_LUMINANCE }, 0, 0 },
  { kodak_easyshare, "EASTMAN KODAK COMPANY", "", 0, FLT_MAX, { { { { 0.000000, 0.000000 }, { 0.044355, 0.020967 }, { 0.133065, 0.154322 }, { 0.209677, 0.300301 }, { 0.572581, 0.753477 }, { 1.000000, 1.000000 } } }, { 6 }, { m }, 0, 0, 0, DT_RGB_NORM_LUMINANCE }, 0, 0 },
  { konica_minolta,  "MINOLTA", "",               0, FLT_MAX, { { { { 0.000000, 0.000000 }, { 0.020161, 0.010322 }, { 0.112903, 0.167742 }, { 0.500000, 0.711291 }, { 0.899194, 0.956855 }, { 1.000000, 1.000000 } } }, { 6 }, { m }, 0, 0, 0, DT_RGB_NORM_LUMINANCE }, 0, 0 },
  { samsung,         "SAMSUNG", "",               0, FLT_MAX, { { { { 0.000000, 0.000000 }, { 0.040323, 0.029677 }, { 0.133065, 0.232258 }, { 0.447581, 0.747581 }, { 0.842742, 0.967742 }, { 1.000000, 1.000000 } } }, { 6 }, { m }, 0, 0, 0, DT_RGB_NORM_LUMINANCE }, 0, 0 },
  { fujifilm,        "FUJIFILM", "",              0, FLT_MAX, { { { { 0.000000, 0.000000 }, { 0.028226, 0.029677 }, { 0.104839, 0.232258 }, { 0.387097, 0.747581 }, { 0.754032, 0.967742 }, { 1.000000, 1.000000 } } }, { 6 }, { m }, 0, 0, 0, DT_RGB_NORM_LUMINANCE }, 0, 0 },
  { nokia,           "Nokia", "",                 0, FLT_MAX, { { { { 0.000000, 0.000000 }, { 0.041825, 0.020161 }, { 0.117871, 0.153226 }, { 0.319392, 0.500000 }, { 0.638783, 0.842742 }, { 1.000000, 1.000000 } } }, { 6 }, { m }, 0, 0, 0, DT_RGB_NORM_LUMINANCE }, 0, 0 },
  // clang-format on
};
#undef m
static const int basecurve_presets_cnt = sizeof(basecurve_presets) / sizeof(basecurve_preset_t);

typedef struct dt_iop_basecurve_data_t
{
  dt_draw_curve_t *curve; // curve for pixelpipe piece and pixel processing
  int basecurve_type;
  int basecurve_nodes;
  float table[0x10000];      // precomputed look-up table for tone curve
  float unbounded_coeffs[3]; // approximation for extrapolation
  int exposure_fusion;
  float exposure_stops;
  float exposure_bias;
  int preserve_colors;
} dt_iop_basecurve_data_t;

typedef struct dt_iop_basecurve_global_data_t
{
  int kernel_basecurve_lut;
  int kernel_basecurve_zero;
  int kernel_basecurve_legacy_lut;
  int kernel_basecurve_compute_features;
  int kernel_basecurve_blur_h;
  int kernel_basecurve_blur_v;
  int kernel_basecurve_expand;
  int kernel_basecurve_reduce;
  int kernel_basecurve_detail;
  int kernel_basecurve_adjust_features;
  int kernel_basecurve_blend_gaussian;
  int kernel_basecurve_blend_laplacian;
  int kernel_basecurve_normalize;
  int kernel_basecurve_reconstruct;
  int kernel_basecurve_finalize;
} dt_iop_basecurve_global_data_t;



const char *name()
{
  return _("base curve");
}

const char **description(struct dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("apply a view transform based on personal or camera manufacturer look,\n"
                                        "for corrective purposes, to prepare images for display"),
                                      _("corrective"),
                                      _("linear, RGB, display-referred"),
                                      _("non-linear, RGB"),
                                      _("non-linear, RGB, display-referred"));
}

int default_group()
{
  return IOP_GROUP_BASIC | IOP_GROUP_TECHNICAL;
}

int flags()
{
  return IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

static void set_presets(dt_iop_module_so_t *self, const basecurve_preset_t *presets, int count, gboolean camera)
{
  const gboolean autoapply_percamera = dt_conf_get_bool("plugins/darkroom/basecurve/auto_apply_percamera_presets");

  const gboolean force_autoapply = (autoapply_percamera || !camera);

  // transform presets above to db entries.
  for(int k = 0; k < count; k++)
  {
    // disable exposure fusion if not explicitly inited in params struct definition above:
    dt_iop_basecurve_params_t tmp = presets[k].params;
    if(tmp.exposure_fusion == 0 && tmp.exposure_stops == 0.0f)
    {
      tmp.exposure_fusion = 0;
      tmp.exposure_stops = 1.0f;
      tmp.exposure_bias = 1.0f;
    }
    // add the preset.
    dt_gui_presets_add_generic(_(presets[k].name), self->op, self->version(),
                               &tmp, sizeof(dt_iop_basecurve_params_t), 1,
                               DEVELOP_BLEND_CS_RGB_DISPLAY);
    // and restrict it to model, maker, iso, and raw images
    dt_gui_presets_update_mml(_(presets[k].name), self->op, self->version(),
                              presets[k].maker, presets[k].model, "");
    dt_gui_presets_update_iso(_(presets[k].name), self->op, self->version(),
                              presets[k].iso_min, presets[k].iso_max);
    dt_gui_presets_update_ldr(_(presets[k].name), self->op, self->version(), FOR_RAW);
    // make it auto-apply for matching images:
    dt_gui_presets_update_autoapply(_(presets[k].name), self->op, self->version(),
                                    force_autoapply ? 1 : presets[k].autoapply);
    // hide all non-matching presets in case the model string is set.
    // When force_autoapply was given always filter (as these are per-camera presets)
    dt_gui_presets_update_filter(_(presets[k].name), self->op, self->version(), camera || presets[k].filter);
  }
}

void init_presets(dt_iop_module_so_t *self)
{
  // sql begin
  dt_database_start_transaction(darktable.db);

  set_presets(self, basecurve_presets, basecurve_presets_cnt, FALSE);
  set_presets(self, basecurve_camera_presets, basecurve_camera_presets_cnt, TRUE);

  // sql commit
  dt_database_release_transaction(darktable.db);
}

static float exposure_increment(float stops, int e, float fusion, float bias)
{
  float offset = stops * fusion * (bias - 1.0f) / 2.0f;
  return powf(2.0f, stops * e + offset);
}

#ifdef HAVE_OPENCL
static
int gauss_blur_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece,
                  cl_mem dev_in, cl_mem dev_out, cl_mem dev_tmp,
                  const int width, const int height)
{
  dt_iop_basecurve_global_data_t *gd = (dt_iop_basecurve_global_data_t *)self->global_data;

  cl_int err = DT_OPENCL_DEFAULT_ERROR;
  const int devid = piece->pipe->devid;

  /* horizontal blur */
  size_t sizes[] = { ROUNDUPDWD(width, devid), ROUNDUPDHT(height, devid), 1 };
  dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_blur_h, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_blur_h, 1, sizeof(cl_mem), (void *)&dev_tmp);
  dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_blur_h, 2, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_blur_h, 3, sizeof(int), (void *)&height);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_basecurve_blur_h, sizes);
  if(err != CL_SUCCESS) return FALSE;

  /* vertical blur */
  size_t sizes2[] = { ROUNDUPDWD(width, devid), ROUNDUPDHT(height, devid), 1 };
  dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_blur_v, 0, sizeof(cl_mem), (void *)&dev_tmp);
  dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_blur_v, 1, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_blur_v, 2, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_blur_v, 3, sizeof(int), (void *)&height);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_basecurve_blur_v, sizes2);
  if(err != CL_SUCCESS) return FALSE;

  return TRUE;
}

static
int gauss_expand_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece,
                    cl_mem dev_in, cl_mem dev_out, cl_mem dev_tmp,
                    const int width, const int height)
{
  dt_iop_basecurve_global_data_t *gd = (dt_iop_basecurve_global_data_t *)self->global_data;

  cl_int err = DT_OPENCL_DEFAULT_ERROR;
  const int devid = piece->pipe->devid;

  size_t sizes[] = { ROUNDUPDWD(width, devid), ROUNDUPDHT(height, devid), 1 };
  dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_expand, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_expand, 1, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_expand, 2, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_expand, 3, sizeof(int), (void *)&height);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_basecurve_expand, sizes);
  if(err != CL_SUCCESS) return FALSE;

  return gauss_blur_cl(self, piece, dev_out, dev_out, dev_tmp, width, height);
}


static
int gauss_reduce_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece,
                    cl_mem dev_in, cl_mem dev_coarse, cl_mem dev_detail,
                    cl_mem dev_tmp1, cl_mem dev_tmp2,
                    const int width, const int height)
{
  dt_iop_basecurve_global_data_t *gd = (dt_iop_basecurve_global_data_t *)self->global_data;

  cl_int err = DT_OPENCL_DEFAULT_ERROR;
  const int devid = piece->pipe->devid;

  {
    if(!gauss_blur_cl(self, piece, dev_in, dev_tmp1, dev_tmp2, width, height))
      return FALSE;

    const int cw = (width - 1) / 2 + 1;
    const int ch = (height - 1) / 2 + 1;

    size_t sizes[] = { ROUNDUPDWD(cw, devid), ROUNDUPDHT(ch, devid), 1 };
    dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_reduce, 0, sizeof(cl_mem), (void *)&dev_tmp1);
    dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_reduce, 1, sizeof(cl_mem), (void *)&dev_coarse);
    dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_reduce, 2, sizeof(int), (void *)&cw);
    dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_reduce, 3, sizeof(int), (void *)&ch);
    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_basecurve_reduce, sizes);
    if(err != CL_SUCCESS) return FALSE;
  }

  if(dev_detail != NULL)
  {
    if(!gauss_expand_cl(self, piece, dev_coarse, dev_tmp1, dev_tmp2, width, height))
      return FALSE;

    size_t sizes[] = { ROUNDUPDWD(width, devid), ROUNDUPDHT(height, devid), 1 };
    dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_detail, 0, sizeof(cl_mem), (void *)&dev_in);
    dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_detail, 1, sizeof(cl_mem), (void *)&dev_tmp1);
    dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_detail, 2, sizeof(cl_mem), (void *)&dev_detail);
    dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_detail, 3, sizeof(int), (void *)&width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_detail, 4, sizeof(int), (void *)&height);
    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_basecurve_detail, sizes);
    if(err != CL_SUCCESS) return FALSE;
  }

  return TRUE;
}

static
int process_cl_fusion(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
                      const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_basecurve_data_t *d = (dt_iop_basecurve_data_t *)piece->data;
  dt_iop_basecurve_global_data_t *gd = (dt_iop_basecurve_global_data_t *)self->global_data;
  const dt_iop_order_iccprofile_info_t *const work_profile = dt_ioppr_get_iop_work_profile_info(piece->module, piece->module->dev->iop);

  cl_int err = DT_OPENCL_DEFAULT_ERROR;

  const int num_levels_max = 8;
  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;
  const int rad = MIN(width, (int)ceilf(256 * roi_in->scale / piece->iscale));

  cl_mem dev_profile_info = NULL;
  cl_mem dev_profile_lut = NULL;
  dt_colorspaces_iccprofile_info_cl_t *profile_info_cl;
  cl_float *profile_lut_cl = NULL;

  cl_mem *dev_col = calloc(num_levels_max, sizeof(cl_mem));
  cl_mem *dev_comb = calloc(num_levels_max, sizeof(cl_mem));

  cl_mem dev_tmp1 = NULL;
  cl_mem dev_tmp2 = NULL;
  cl_mem dev_m = NULL;
  cl_mem dev_coeffs = NULL;

  const int use_work_profile = (work_profile == NULL) ? 0 : 1;
  const int preserve_colors = d->preserve_colors;

  err = dt_ioppr_build_iccprofile_params_cl(work_profile, devid, &profile_info_cl, &profile_lut_cl,
                                            &dev_profile_info, &dev_profile_lut);
  if(err != CL_SUCCESS) goto error;

  int num_levels = num_levels_max;

  dev_tmp1 = dt_opencl_alloc_device(devid, width, height, sizeof(float) * 4);
  if(dev_tmp1 == NULL) goto error;

  dev_tmp2 = dt_opencl_alloc_device(devid, width, height, sizeof(float) * 4);
  if(dev_tmp2 == NULL) goto error;

  // allocate buffers for wavelet transform and blending
  for(int k = 0, step = 1, w = width, h = height; k < num_levels; k++)
  {
    // coarsest step is some % of image width.
    dev_col[k] = dt_opencl_alloc_device(devid, w, h, sizeof(float) * 4);
    if(dev_col[k] == NULL) goto error;

    dev_comb[k] = dt_opencl_alloc_device(devid, w, h, sizeof(float) * 4);
    if(dev_comb[k] == NULL) goto error;

    size_t sizes[] = { ROUNDUPDWD(w, devid), ROUNDUPDHT(h, devid), 1 };
    dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_zero, 0, sizeof(cl_mem), (void *)&dev_comb[k]);
    dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_zero, 1, sizeof(int), (void *)&w);
    dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_zero, 2, sizeof(int), (void *)&h);
    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_basecurve_zero, sizes);
    if(err != CL_SUCCESS) goto error;

    w = (w - 1) / 2 + 1;
    h = (h - 1) / 2 + 1;
    step *= 2;

    if(step > rad || w < 4 || h < 4)
    {
      num_levels = k + 1;
      break;
    }
  }

  dev_m = dt_opencl_copy_host_to_device(devid, d->table, 256, 256, sizeof(float));
  if(dev_m == NULL) goto error;

  dev_coeffs = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * 3, d->unbounded_coeffs);
  if(dev_coeffs == NULL) goto error;


  for(int e = 0; e < d->exposure_fusion + 1; e++)
  {
    // for every exposure fusion image: push by some ev, apply base curve and compute features
    {
      const float mul = exposure_increment(d->exposure_stops, e, d->exposure_fusion, d->exposure_bias);

      size_t sizes[] = { ROUNDUPDWD(width, devid), ROUNDUPDHT(height, devid), 1 };
      if(d->preserve_colors == DT_RGB_NORM_NONE)
      {
        dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_legacy_lut, 0, sizeof(cl_mem), (void *)&dev_in);
        dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_legacy_lut, 1, sizeof(cl_mem), (void *)&dev_tmp1);
        dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_legacy_lut, 2, sizeof(int), (void *)&width);
        dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_legacy_lut, 3, sizeof(int), (void *)&height);
        dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_legacy_lut, 4, sizeof(float), (void *)&mul);
        dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_legacy_lut, 5, sizeof(cl_mem), (void *)&dev_m);
        dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_legacy_lut, 6, sizeof(cl_mem), (void *)&dev_coeffs);
        err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_basecurve_legacy_lut, sizes);
        if(err != CL_SUCCESS) goto error;
      }
      else
      {
        dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_lut, 0, sizeof(cl_mem), (void *)&dev_in);
        dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_lut, 1, sizeof(cl_mem), (void *)&dev_tmp1);
        dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_lut, 2, sizeof(int), (void *)&width);
        dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_lut, 3, sizeof(int), (void *)&height);
        dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_lut, 4, sizeof(float), (void *)&mul);
        dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_lut, 5, sizeof(cl_mem), (void *)&dev_m);
        dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_lut, 6, sizeof(cl_mem), (void *)&dev_coeffs);
        dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_lut, 7, sizeof(int), (void *)&preserve_colors);
        dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_lut, 8, sizeof(cl_mem), (void *)&dev_profile_info);
        dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_lut, 9, sizeof(cl_mem), (void *)&dev_profile_lut);
        dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_lut, 10, sizeof(int), (void *)&use_work_profile);
        err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_basecurve_lut, sizes);
      }

      dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_compute_features, 0, sizeof(cl_mem), (void *)&dev_tmp1);
      dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_compute_features, 1, sizeof(cl_mem), (void *)&dev_col[0]);
      dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_compute_features, 2, sizeof(int), (void *)&width);
      dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_compute_features, 3, sizeof(int), (void *)&height);
      err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_basecurve_compute_features, sizes);
      if(err != CL_SUCCESS) goto error;
    }

    // create gaussian pyramid of color buffer
    if(!gauss_reduce_cl(self, piece, dev_col[0], dev_col[1], dev_out, dev_tmp1, dev_tmp2, width, height))
      goto error;

    // adjust features
    {
      size_t sizes[] = { ROUNDUPDWD(width, devid), ROUNDUPDHT(height, devid), 1 };
      dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_adjust_features, 0, sizeof(cl_mem), (void *)&dev_col[0]);
      dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_adjust_features, 1, sizeof(cl_mem), (void *)&dev_out);
      dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_adjust_features, 2, sizeof(cl_mem), (void *)&dev_tmp1);
      dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_adjust_features, 3, sizeof(int), (void *)&width);
      dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_adjust_features, 4, sizeof(int), (void *)&height);
      err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_basecurve_adjust_features, sizes);
      if(err != CL_SUCCESS) goto error;

      size_t origin[] = { 0, 0, 0 };
      size_t region[] = { width, height, 1 };
      err = dt_opencl_enqueue_copy_image(devid, dev_tmp1, dev_col[0], origin, origin, region);
      if(err != CL_SUCCESS) goto error;
    }

    for(int k = 1, w = width, h = height; k < num_levels; k++)
    {
      if(!gauss_reduce_cl(self, piece, dev_col[k-1], dev_col[k], NULL, dev_tmp1, dev_tmp2, w, h))
        goto error;

      w = (w - 1) / 2 + 1;
      h = (h - 1) / 2 + 1;
    }

    // update pyramid coarse to fine
    for(int k = num_levels - 1; k >= 0; k--)
    {
      int w = width;
      int h = height;

      for(int i = 0; i < k; i++)
      {
        w = (w - 1) / 2 + 1;
        h = (h - 1) / 2 + 1;
      }

      // dev_col[k+1] -> dev_tmp2[k]
      if(k != num_levels - 1)
        if(!gauss_expand_cl(self, piece, dev_col[k+1], dev_tmp2, dev_tmp1, w, h))
          goto error;

      // blend images into output pyramid
      if(k == num_levels - 1)
      {
        // blend gaussian base
        size_t sizes[] = { ROUNDUPDWD(w, devid), ROUNDUPDHT(h, devid), 1 };
        dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_blend_gaussian, 0, sizeof(cl_mem), (void *)&dev_comb[k]);
        dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_blend_gaussian, 1, sizeof(cl_mem), (void *)&dev_col[k]);
        dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_blend_gaussian, 2, sizeof(cl_mem), (void *)&dev_tmp1);
        dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_blend_gaussian, 3, sizeof(int), (void *)&w);
        dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_blend_gaussian, 4, sizeof(int), (void *)&h);
        err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_basecurve_blend_gaussian, sizes);
        if(err != CL_SUCCESS) goto error;

        size_t origin[] = { 0, 0, 0 };
        size_t region[] = { w, h, 1 };
        err = dt_opencl_enqueue_copy_image(devid, dev_tmp1, dev_comb[k], origin, origin, region);
        if(err != CL_SUCCESS) goto error;
      }
      else
      {
        // blend laplacian
        size_t sizes[] = { ROUNDUPDWD(w, devid), ROUNDUPDHT(h, devid), 1 };
        dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_blend_laplacian, 0, sizeof(cl_mem), (void *)&dev_comb[k]);
        dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_blend_laplacian, 1, sizeof(cl_mem), (void *)&dev_col[k]);
        dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_blend_laplacian, 2, sizeof(cl_mem), (void *)&dev_tmp2);
        dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_blend_laplacian, 3, sizeof(cl_mem), (void *)&dev_tmp1);
        dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_blend_laplacian, 4, sizeof(int), (void *)&w);
        dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_blend_laplacian, 5, sizeof(int), (void *)&h);
        err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_basecurve_blend_laplacian, sizes);
        if(err != CL_SUCCESS) goto error;

        size_t origin[] = { 0, 0, 0 };
        size_t region[] = { w, h, 1 };
        err = dt_opencl_enqueue_copy_image(devid, dev_tmp1, dev_comb[k], origin, origin, region);
        if(err != CL_SUCCESS) goto error;
      }
    }
  }

  // normalize and reconstruct output pyramid buffer coarse to fine
  for(int k = num_levels - 1; k >= 0; k--)
  {
    int w = width;
    int h = height;

    for(int i = 0; i < k; i++)
    {
      w = (w - 1) / 2 + 1;
      h = (h - 1) / 2 + 1;
    }

    {
      // normalize both gaussian base and laplacian
      size_t sizes[] = { ROUNDUPDWD(w, devid), ROUNDUPDHT(h, devid), 1 };
      dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_normalize, 0, sizeof(cl_mem), (void *)&dev_comb[k]);
      dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_normalize, 1, sizeof(cl_mem), (void *)&dev_tmp1);
      dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_normalize, 2, sizeof(int), (void *)&w);
      dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_normalize, 3, sizeof(int), (void *)&h);
      err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_basecurve_normalize, sizes);
      if(err != CL_SUCCESS) goto error;

      // dev_tmp1[k] -> dev_comb[k]
      size_t origin[] = { 0, 0, 0 };
      size_t region[] = { w, h, 1 };
      err = dt_opencl_enqueue_copy_image(devid, dev_tmp1, dev_comb[k], origin, origin, region);
      if(err != CL_SUCCESS) goto error;
    }

    if(k < num_levels - 1)
    {
      // reconstruct output image

      // dev_comb[k+1] -> dev_tmp1
      if(!gauss_expand_cl(self, piece, dev_comb[k+1], dev_tmp1, dev_tmp2, w, h))
        goto error;

      // dev_comb[k] + dev_tmp1 -> dev_tmp2
      size_t sizes[] = { ROUNDUPDWD(w, devid), ROUNDUPDHT(h, devid), 1 };
      dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_reconstruct, 0, sizeof(cl_mem), (void *)&dev_comb[k]);
      dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_reconstruct, 1, sizeof(cl_mem), (void *)&dev_tmp1);
      dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_reconstruct, 2, sizeof(cl_mem), (void *)&dev_tmp2);
      dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_reconstruct, 3, sizeof(int), (void *)&w);
      dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_reconstruct, 4, sizeof(int), (void *)&h);
      err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_basecurve_reconstruct, sizes);
      if(err != CL_SUCCESS) goto error;

      // dev_tmp2 -> dev_comb[k]
      size_t origin[] = { 0, 0, 0 };
      size_t region[] = { w, h, 1 };
      err = dt_opencl_enqueue_copy_image(devid, dev_tmp2, dev_comb[k], origin, origin, region);
      if(err != CL_SUCCESS) goto error;
    }
  }

  // copy output buffer
  {
    size_t sizes[] = { ROUNDUPDWD(width, devid), ROUNDUPDHT(height, devid), 1 };
    dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_finalize, 0, sizeof(cl_mem), (void *)&dev_in);
    dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_finalize, 1, sizeof(cl_mem), (void *)&dev_comb[0]);
    dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_finalize, 2, sizeof(cl_mem), (void *)&dev_out);
    dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_finalize, 3, sizeof(int), (void *)&width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_finalize, 4, sizeof(int), (void *)&height);
    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_basecurve_finalize, sizes);
    if(err != CL_SUCCESS) goto error;
  }

  for(int k = 0; k < num_levels_max; k++)
  {
    dt_opencl_release_mem_object(dev_col[k]);
    dt_opencl_release_mem_object(dev_comb[k]);
  }
  dt_ioppr_free_iccprofile_params_cl(&profile_info_cl, &profile_lut_cl, &dev_profile_info, &dev_profile_lut);
  dt_opencl_release_mem_object(dev_m);
  dt_opencl_release_mem_object(dev_coeffs);
  dt_opencl_release_mem_object(dev_tmp1);
  dt_opencl_release_mem_object(dev_tmp2);
  free(dev_comb);
  free(dev_col);
  return TRUE;

error:
  for(int k = 0; k < num_levels_max; k++)
  {
    dt_opencl_release_mem_object(dev_col[k]);
    dt_opencl_release_mem_object(dev_comb[k]);
  }
  dt_ioppr_free_iccprofile_params_cl(&profile_info_cl, &profile_lut_cl, &dev_profile_info, &dev_profile_lut);
  dt_opencl_release_mem_object(dev_m);
  dt_opencl_release_mem_object(dev_coeffs);
  dt_opencl_release_mem_object(dev_tmp1);
  dt_opencl_release_mem_object(dev_tmp2);
  free(dev_comb);
  free(dev_col);
  dt_print(DT_DEBUG_OPENCL, "[opencl_basecurve_fusion] couldn't enqueue kernel! %s\n", cl_errstr(err));
  return FALSE;
}

static
int process_cl_lut(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
                   const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_basecurve_data_t *d = (dt_iop_basecurve_data_t *)piece->data;
  dt_iop_basecurve_global_data_t *gd = (dt_iop_basecurve_global_data_t *)self->global_data;
  const dt_iop_order_iccprofile_info_t *const work_profile = dt_ioppr_get_iop_work_profile_info(piece->module, piece->module->dev->iop);

  cl_mem dev_m = NULL;
  cl_mem dev_coeffs = NULL;
  cl_int err = DT_OPENCL_DEFAULT_ERROR;

  cl_mem dev_profile_info = NULL;
  cl_mem dev_profile_lut = NULL;
  dt_colorspaces_iccprofile_info_cl_t *profile_info_cl;
  cl_float *profile_lut_cl = NULL;

  const int use_work_profile = (work_profile == NULL) ? 0 : 1;
  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;
  const int preserve_colors = d->preserve_colors;

  const float mul = 1.0f;

  size_t sizes[] = { ROUNDUPDWD(width, devid), ROUNDUPDHT(height, devid), 1 };
  dev_m = dt_opencl_copy_host_to_device(devid, d->table, 256, 256, sizeof(float));
  if(dev_m == NULL) goto error;

  err = dt_ioppr_build_iccprofile_params_cl(work_profile, devid, &profile_info_cl, &profile_lut_cl,
                                            &dev_profile_info, &dev_profile_lut);
  if(err != CL_SUCCESS) goto error;

  dev_coeffs = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * 3, d->unbounded_coeffs);

  if(dev_coeffs == NULL) goto error;

  // read data/kernels/basecurve.cl for a description of "legacy" vs current
  // Conditional is moved outside of the OpenCL operations for performance.
  if(d->preserve_colors == DT_RGB_NORM_NONE)
  {
    dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_legacy_lut, 0, sizeof(cl_mem), (void *)&dev_in);
    dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_legacy_lut, 1, sizeof(cl_mem), (void *)&dev_out);
    dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_legacy_lut, 2, sizeof(int), (void *)&width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_legacy_lut, 3, sizeof(int), (void *)&height);
    dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_legacy_lut, 4, sizeof(float), (void *)&mul);
    dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_legacy_lut, 5, sizeof(cl_mem), (void *)&dev_m);
    dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_legacy_lut, 6, sizeof(cl_mem), (void *)&dev_coeffs);
    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_basecurve_legacy_lut, sizes);
    if(err != CL_SUCCESS) goto error;
  }
  else
  {
    //FIXME:  There are still conditionals on d->preserve_colors within this flow that could impact performance
    dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_lut, 0, sizeof(cl_mem), (void *)&dev_in);
    dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_lut, 1, sizeof(cl_mem), (void *)&dev_out);
    dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_lut, 2, sizeof(int), (void *)&width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_lut, 3, sizeof(int), (void *)&height);
    dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_lut, 4, sizeof(float), (void *)&mul);
    dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_lut, 5, sizeof(cl_mem), (void *)&dev_m);
    dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_lut, 6, sizeof(cl_mem), (void *)&dev_coeffs);
    dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_lut, 7, sizeof(int), (void *)&preserve_colors);
    dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_lut, 8, sizeof(cl_mem), (void *)&dev_profile_info);
    dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_lut, 9, sizeof(cl_mem), (void *)&dev_profile_lut);
    dt_opencl_set_kernel_arg(devid, gd->kernel_basecurve_lut, 10, sizeof(int), (void *)&use_work_profile);
    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_basecurve_lut, sizes);
    if(err != CL_SUCCESS) goto error;
  }

  dt_opencl_release_mem_object(dev_m);
  dt_opencl_release_mem_object(dev_coeffs);
  dt_ioppr_free_iccprofile_params_cl(&profile_info_cl, &profile_lut_cl, &dev_profile_info, &dev_profile_lut);
  return TRUE;

error:
  dt_opencl_release_mem_object(dev_m);
  dt_opencl_release_mem_object(dev_coeffs);
  dt_ioppr_free_iccprofile_params_cl(&profile_info_cl, &profile_lut_cl, &dev_profile_info, &dev_profile_lut);
  dt_print(DT_DEBUG_OPENCL, "[opencl_basecurve_lut] couldn't enqueue kernel! %s\n", cl_errstr(err));
  return FALSE;
}

int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_basecurve_data_t *d = (dt_iop_basecurve_data_t *)piece->data;

  if(d->exposure_fusion)
    return process_cl_fusion(self, piece, dev_in, dev_out, roi_in, roi_out);
  else
    return process_cl_lut(self, piece, dev_in, dev_out, roi_in, roi_out);
}

#endif

void tiling_callback(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                     const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out,
                     struct dt_develop_tiling_t *tiling)
{
  dt_iop_basecurve_data_t *const d = (dt_iop_basecurve_data_t *)piece->data;

  if(d->exposure_fusion)
  {
    const int rad = MIN(roi_in->width, (int)ceilf(256 * roi_in->scale / piece->iscale));

    tiling->factor = 6.666f;                 // in + out + col[] + comb[] + 2*tmp
    tiling->maxbuf = 1.0f;
    tiling->overhead = 0;
    tiling->xalign = 1;
    tiling->yalign = 1;
    tiling->overlap = rad;
  }
  else
  {
    tiling->factor = 2.0f;                   // in + out
    tiling->maxbuf = 1.0f;
    tiling->overhead = 0;
    tiling->xalign = 1;
    tiling->yalign = 1;
    tiling->overlap = 0;
  }
}

// See comments of opencl version in data/kernels/basecurve.cl for description of the meaning of "legacy"
static inline void apply_legacy_curve(
    const float *const in,
    float *const out,
    const int width,
    const int height,
    const float mul,
    const float *const table,
    const float *const unbounded_coeffs)
{
  const size_t npixels = (size_t)width * height;
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(npixels) \
  dt_omp_sharedconst(in, out, mul, table, unbounded_coeffs) \
  schedule(static)
#endif
  for(size_t k = 0; k < 4*npixels; k += 4)
  {
    for(int i = 0; i < 3; i++)
    {
      const float f = in[k+i] * mul;
      // use base curve for values < 1, else use extrapolation.
      if(f < 1.0f)
        out[k+i] = fmaxf(table[CLAMP((int)(f * 0x10000ul), 0, 0xffff)], 0.f);
      else
        out[k+i] = fmaxf(dt_iop_eval_exp(unbounded_coeffs, f), 0.f);
    }
    out[k+3] = in[k+3];
  }
}

// See description of the equivalent OpenCL function in data/kernels/basecurve.cl
static inline void apply_curve(
    const float *const in,
    float *const out,
    const int width,
    const int height,
    const int preserve_colors,
    const float mul,
    const float *const table,
    const float *const unbounded_coeffs,
    const dt_iop_order_iccprofile_info_t *const work_profile)
{
  const size_t npixels = (size_t)width * height;
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(npixels, preserve_colors, work_profile) \
  dt_omp_sharedconst(in, out, mul, table, unbounded_coeffs) \
  schedule(static)
#endif
  for(size_t k = 0; k < 4*npixels; k += 4)
  {
    float ratio = 1.f;
    // FIXME: Determine if we can get rid of the conditionals within this function in some way to improve performance.
    // However, solving this one is much harder than the conditional for legacy vs. current
    const float lum = mul * dt_rgb_norm(in+k, preserve_colors, work_profile);
    if(lum > 0.f)
    {
      const float curve_lum = (lum < 1.0f)
        ? table[CLAMP((int)(lum * 0x10000ul), 0, 0xffff)]
        : dt_iop_eval_exp(unbounded_coeffs, lum);
      ratio = mul * curve_lum / lum;
    }
    for(size_t c = 0; c < 3; c++)
    {
      out[k+c] = fmaxf(ratio * in[k+c], 0.f);
    }
    out[k+3] = in[k+3];
  }
}

static inline void compute_features(
    float *const col,
    const int wd,
    const int ht)
{
  // features are product of
  // 1) well exposedness
  // 2) saturation
  // 3) local contrast (handled in laplacian form later)
  const size_t npixels = (size_t)wd * ht;
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(col, npixels) \
  schedule(static)
#endif
  for(size_t x = 0; x < 4*npixels; x += 4)
  {
    const float max = MAX(col[x], MAX(col[x+1], col[x+2]));
    const float min = MIN(col[x], MIN(col[x+1], col[x+2]));
    const float sat = .1f + .1f*(max-min)/MAX(1e-4f, max);

    const float c = 0.54f;
    float v = fabsf(col[x]-c);
    v = MAX(fabsf(col[x+1]-c), v);
    v = MAX(fabsf(col[x+2]-c), v);
    const float var = 0.5;
    const float exp = .2f + dt_fast_expf(-v*v/(var*var));
    col[x+3] = sat * exp;
  }
}

static inline void gauss_blur(
    const float *const input,
    float *const output,
    const size_t wd,
    const size_t ht)
{
  const float w[5] = { 1.f / 16.f, 4.f / 16.f, 6.f / 16.f, 4.f / 16.f, 1.f / 16.f };
  float *tmp = dt_alloc_align_float((size_t)4 * wd * ht);
  memset(tmp, 0, sizeof(float) * 4 * wd * ht);
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(ht, input, w, wd) \
  shared(tmp) \
  schedule(static)
#endif
  for(int j=0;j<ht;j++)
  { // horizontal pass
    // left borders
    for(int i=0;i<2;i++) for(int c=0;c<4;c++)
      for(int ii=-2;ii<=2;ii++)
        tmp[4*(j*wd+i)+c] += input[4*(j*wd+MAX(-i-ii,i+ii))+c] * w[ii+2];
    // most pixels
    for(int i=2;i<wd-2;i++) for(int c=0;c<4;c++)
      for(int ii=-2;ii<=2;ii++)
        tmp[4*(j*wd+i)+c] += input[4*(j*wd+i+ii)+c] * w[ii+2];
    // right borders
    for(int i=wd-2;i<wd;i++) for(int c=0;c<4;c++)
      for(int ii=-2;ii<=2;ii++)
        tmp[4*(j*wd+i)+c] += input[4*(j*wd+MIN(i+ii, wd-(i+ii-wd+1) ))+c] * w[ii+2];
  }
  memset(output, 0, sizeof(float) * 4 * wd * ht);
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(ht, output, w, wd) \
  shared(tmp) \
  schedule(static)
#endif
  for(int i=0;i<wd;i++)
  { // vertical pass
    for(int j=0;j<2;j++) for(int c=0;c<4;c++)
      for(int jj=-2;jj<=2;jj++)
        output[4*(j*wd+i)+c] += tmp[4*(MAX(-j-jj,j+jj)*wd+i)+c] * w[jj+2];
    for(int j=2;j<ht-2;j++) for(int c=0;c<4;c++)
      for(int jj=-2;jj<=2;jj++)
        output[4*(j*wd+i)+c] += tmp[4*((j+jj)*wd+i)+c] * w[jj+2];
    for(int j=ht-2;j<ht;j++) for(int c=0;c<4;c++)
      for(int jj=-2;jj<=2;jj++)
        output[4*(j*wd+i)+c] += tmp[4*(MIN(j+jj, ht-(j+jj-ht+1))*wd+i)+c] * w[jj+2];
  }
  dt_free_align(tmp);
}

static inline void gauss_expand(
    const float *const input, // coarse input
    float *const fine,        // upsampled, blurry output
    const size_t wd,          // fine res
    const size_t ht)
{
  const size_t cw = (wd-1)/2+1;
  // fill numbers in even pixels, zero odd ones
  memset(fine, 0, sizeof(float) * 4 * wd * ht);
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(cw, fine, ht, input, wd) \
  schedule(static) \
  collapse(2)
#endif
  for(int j=0;j<ht;j+=2)
    for(int i=0;i<wd;i+=2)
      for(int c=0;c<4;c++)
        fine[4*(j*wd+i)+c] = 4.0f * input[4*(j/2*cw + i/2)+c];

  // convolve with same kernel weights mul by 4:
  gauss_blur(fine, fine, wd, ht);
}

// XXX FIXME: we'll need to pad up the image to get a good boundary condition!
// XXX FIXME: downsampling will not result in an energy conserving pattern (every 4 pixels one sample)
// XXX FIXME: neither will a mirror boundary condition (mirrors in subsampled values at random density)
// TODO: copy laplacian code from local laplacian filters, it's faster.
static inline void gauss_reduce(
    const float *const input, // fine input buffer
    float *const coarse,      // coarse scale, blurred input buf
    float *const detail,      // detail/laplacian, fine scale, or 0
    const size_t wd,
    const size_t ht)
{
  // blur, store only coarse res
  const size_t cw = (wd-1)/2+1, ch = (ht-1)/2+1;

  float *blurred = dt_alloc_align_float((size_t)4 * wd * ht);
  gauss_blur(input, blurred, wd, ht);
  for(size_t j=0;j<ch;j++) for(size_t i=0;i<cw;i++)
    for(int c=0;c<4;c++) coarse[4*(j*cw+i)+c] = blurred[4*(2*j*wd+2*i)+c];
  dt_free_align(blurred);

  if(detail)
  {
    // compute laplacian/details: expand coarse buffer into detail
    // buffer subtract expanded buffer from input in place
    gauss_expand(coarse, detail, wd, ht);
    for(size_t k=0;k<wd*ht*4;k++)
      detail[k] = input[k] - detail[k];
  }
}

void process_fusion(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
                    void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const float *const in = (const float *)ivoid;
  float *const out = (float *)ovoid;
  dt_iop_basecurve_data_t *const d = (dt_iop_basecurve_data_t *)(piece->data);
  const dt_iop_order_iccprofile_info_t *const work_profile = dt_ioppr_get_iop_work_profile_info(piece->module, piece->module->dev->iop);

  // allocate temporary buffer for wavelet transform + blending
  const int wd = roi_in->width, ht = roi_in->height;
  int num_levels = 8;
  float **col = malloc(sizeof(float *) * num_levels);
  float **comb = malloc(sizeof(float *) * num_levels);
  int w = wd, h = ht;
  const int rad = MIN(wd, (int)ceilf(256 * roi_in->scale / piece->iscale));
  int step = 1;
  for(int k = 0; k < num_levels; k++)
  {
    // coarsest step is some % of image width.
    col[k]  = dt_alloc_align_float((size_t)4 * w * h);
    comb[k] = dt_alloc_align_float((size_t)4 * w * h);
    memset(comb[k], 0, sizeof(float) * 4 * w * h);
    w = (w - 1) / 2 + 1;
    h = (h - 1) / 2 + 1;
    step *= 2;
    if(step > rad || w < 4 || h < 4)
    {
      num_levels = k + 1;
      break;
    }
  }

  for(int e = 0; e < d->exposure_fusion + 1; e++)
  {
    // for every exposure fusion image:
    // push by some ev, apply base curve:
    if(d->preserve_colors == DT_RGB_NORM_NONE)
      apply_legacy_curve(in, col[0], wd, ht, exposure_increment(d->exposure_stops, e, d->exposure_fusion, d->exposure_bias),
                         d->table, d->unbounded_coeffs);
    else
      apply_curve(in, col[0], wd, ht, d->preserve_colors, exposure_increment(d->exposure_stops, e, d->exposure_fusion, d->exposure_bias),
                  d->table, d->unbounded_coeffs, work_profile);

    // compute features
    compute_features(col[0], wd, ht);

    // create gaussian pyramid of colour buffer
    w = wd;
    h = ht;
    gauss_reduce(col[0], col[1], out, w, h);
#ifdef _OPENMP
#pragma omp parallel for default(none) \
    dt_omp_firstprivate(ht, out, wd) \
    shared(col) \
    schedule(static)
#endif
    for(size_t k = 0; k < 4ul * wd * ht; k += 4)
      col[0][k + 3] *= .1f + sqrtf(out[k] * out[k] + out[k + 1] * out[k + 1] + out[k + 2] * out[k + 2]);

// #define DEBUG_VIS2
#ifdef DEBUG_VIS2 // transform weights in channels
    for(size_t k = 0; k < 4ul * w * h; k += 4) col[0][k + e] = col[0][k + 3];
#endif

// #define DEBUG_VIS
#ifdef DEBUG_VIS // DEBUG visualise weight buffer
    for(size_t k = 0; k < 4ul * w * h; k += 4) comb[0][k + e] = col[0][k + 3];
    continue;
#endif

    for(int k = 1; k < num_levels; k++)
    {
      gauss_reduce(col[k - 1], col[k], 0, w, h);
      w = (w - 1) / 2 + 1;
      h = (h - 1) / 2 + 1;
    }

    // update pyramid coarse to fine
    for(int k = num_levels - 1; k >= 0; k--)
    {
      w = wd;
      h = ht;
      for(int i = 0; i < k; i++)
      {
        w = (w - 1) / 2 + 1;
        h = (h - 1) / 2 + 1;
      }
      // abuse output buffer as temporary memory:
      if(k != num_levels - 1)
        gauss_expand(col[k + 1], out, w, h);
#ifdef _OPENMP
#pragma omp parallel for default(none) \
      dt_omp_firstprivate(out) \
      shared(col, comb, w, h, num_levels, k) \
      schedule(static)
#endif
      for(size_t x = 0; x < (size_t)4 * h * w; x += 4)
      {
        // blend images into output pyramid
        if(k == num_levels - 1) // blend gaussian base
#ifdef DEBUG_VIS2
          ;
#else
        {
        for(int c = 0; c < 3; c++)
          comb[k][x + c] += col[k][x + 3] * col[k][x + c];
        }
#endif
        else // laplacian
        {
          for(int c = 0; c < 3; c++)
            comb[k][x + c] += col[k][x + 3] * (col[k][x + c] - out[x + c]);
        }
        comb[k][x + 3] += col[k][x + 3];
      }
    }
  }

#ifndef DEBUG_VIS // DEBUG: switch off when visualising weight buf
  // normalise and reconstruct output pyramid buffer coarse to fine
  for(int k = num_levels - 1; k >= 0; k--)
  {
    w = wd;
    h = ht;
    for(int i = 0; i < k; i++)
    {
      w = (w - 1) / 2 + 1;
      h = (h - 1) / 2 + 1;
    }

    // normalise both gaussian base and laplacians:
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(comb, w, h, k) schedule(static)
#endif
    for(size_t i = 0; i < (size_t)4 * w * h; i += 4)
      if(comb[k][i + 3] > 1e-8f)
        for(int c = 0; c < 3; c++) comb[k][i + c] /= comb[k][i + 3];

    if(k < num_levels - 1)
    { // reconstruct output image
      gauss_expand(comb[k + 1], out, w, h);
#ifdef _OPENMP
#pragma omp parallel for default(none) \
      dt_omp_firstprivate(out, w, h, k) \
      shared(comb) \
      schedule(static)
#endif
      for(size_t x = 0; x < (size_t)4 * h * w; x += 4)
        {
        for(int c = 0; c < 3; c++)
          comb[k][x + c] += out[x + c];
        }
    }
  }
#endif
  // copy output buffer
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(col, in, ht, out, wd) \
  shared(comb) \
  schedule(static)
#endif
  for(size_t k = 0; k < (size_t)4 * wd * ht; k += 4)
  {
    out[k + 0] = fmaxf(comb[0][k + 0], 0.f);
    out[k + 1] = fmaxf(comb[0][k + 1], 0.f);
    out[k + 2] = fmaxf(comb[0][k + 2], 0.f);
    out[k + 3] = in[k + 3]; // pass on 4th channel
  }

  // free temp buffers
  for(int k = 0; k < num_levels; k++)
  {
    dt_free_align(col[k]);
    dt_free_align(comb[k]);
  }
  free(col);
  free(comb);
}

void process_lut(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
                 void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const float *const in = (const float *)ivoid;
  float *const out = (float *)ovoid;
  //const int ch = piece->colors; <-- it appears someone was trying to make this handle monochrome data,
  //however the for loops only handled RGBA - FIXME, determine what possible data formats and channel
  //configurations we might encounter here and handle those too
  dt_iop_basecurve_data_t *const d = (dt_iop_basecurve_data_t *)(piece->data);
  const dt_iop_order_iccprofile_info_t *const work_profile = dt_ioppr_get_iop_work_profile_info(piece->module, piece->module->dev->iop);

  const int wd = roi_in->width, ht = roi_in->height;

  // Compared to previous implementation, we've at least moved this conditional outside of the image processing loops
  // so that it is evaluated only once.  See FIXME comments in apply_curve for more potential performance improvements
  if(d->preserve_colors == DT_RGB_NORM_NONE)
    apply_legacy_curve(in, out, wd, ht, 1.0, d->table, d->unbounded_coeffs);
  else
    apply_curve(in, out, wd, ht, d->preserve_colors, 1.0, d->table, d->unbounded_coeffs, work_profile);
}


void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_basecurve_data_t *const d = (dt_iop_basecurve_data_t *)(piece->data);

  // are we doing exposure fusion?
  if(d->exposure_fusion)
    process_fusion(self, piece, ivoid, ovoid, roi_in, roi_out);
  else
    process_lut(self, piece, ivoid, ovoid, roi_in, roi_out);
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_basecurve_data_t *d = (dt_iop_basecurve_data_t *)(piece->data);
  dt_iop_basecurve_params_t *p = (dt_iop_basecurve_params_t *)p1;

  d->exposure_fusion = p->exposure_fusion;
  d->exposure_stops = p->exposure_stops;
  d->exposure_bias = p->exposure_bias;
  d->preserve_colors = p->preserve_colors;

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
  // create part of the pixelpipe
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
  dt_iop_basecurve_params_t *p = (dt_iop_basecurve_params_t *)self->params;
  dt_iop_basecurve_gui_data_t *g = (dt_iop_basecurve_gui_data_t *)self->gui_data;

  gtk_widget_set_visible(g->exposure_step, p->exposure_fusion != 0);
  gtk_widget_set_visible(g->exposure_bias, p->exposure_fusion != 0);

  dt_iop_cancel_history_update(self);
  // gui curve is read directly from params during expose event.
  gtk_widget_queue_draw(self->widget);
}

static float eval_grey(float x)
{
  // "log base" is a combined scaling and offset change so that x->[0,1], with
  // the left side of the histogram expanded (slider->right) or not (slider left, linear)
  return x;
}

void init(dt_iop_module_t *module)
{
  dt_iop_default_init(module);
  dt_iop_basecurve_params_t *d = module->default_params;
  d->basecurve[0][1].x = d->basecurve[0][1].y = 1.0;
  d->basecurve_nodes[0] = 2;
}

void init_global(dt_iop_module_so_t *module)
{
  const int program = 18; // basecurve.cl, from programs.conf
  dt_iop_basecurve_global_data_t *gd
      = (dt_iop_basecurve_global_data_t *)malloc(sizeof(dt_iop_basecurve_global_data_t));
  module->data = gd;
  gd->kernel_basecurve_lut = dt_opencl_create_kernel(program, "basecurve_lut");
  gd->kernel_basecurve_zero = dt_opencl_create_kernel(program, "basecurve_zero");
  gd->kernel_basecurve_legacy_lut = dt_opencl_create_kernel(program, "basecurve_legacy_lut");
  gd->kernel_basecurve_compute_features = dt_opencl_create_kernel(program, "basecurve_compute_features");
  gd->kernel_basecurve_blur_h = dt_opencl_create_kernel(program, "basecurve_blur_h");
  gd->kernel_basecurve_blur_v = dt_opencl_create_kernel(program, "basecurve_blur_v");
  gd->kernel_basecurve_expand = dt_opencl_create_kernel(program, "basecurve_expand");
  gd->kernel_basecurve_reduce = dt_opencl_create_kernel(program, "basecurve_reduce");
  gd->kernel_basecurve_detail = dt_opencl_create_kernel(program, "basecurve_detail");
  gd->kernel_basecurve_adjust_features = dt_opencl_create_kernel(program, "basecurve_adjust_features");
  gd->kernel_basecurve_blend_gaussian = dt_opencl_create_kernel(program, "basecurve_blend_gaussian");
  gd->kernel_basecurve_blend_laplacian = dt_opencl_create_kernel(program, "basecurve_blend_laplacian");
  gd->kernel_basecurve_normalize = dt_opencl_create_kernel(program, "basecurve_normalize");
  gd->kernel_basecurve_reconstruct = dt_opencl_create_kernel(program, "basecurve_reconstruct");
  gd->kernel_basecurve_finalize = dt_opencl_create_kernel(program, "basecurve_finalize");
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_basecurve_global_data_t *gd = (dt_iop_basecurve_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_basecurve_lut);
  dt_opencl_free_kernel(gd->kernel_basecurve_zero);
  dt_opencl_free_kernel(gd->kernel_basecurve_legacy_lut);
  dt_opencl_free_kernel(gd->kernel_basecurve_compute_features);
  dt_opencl_free_kernel(gd->kernel_basecurve_blur_h);
  dt_opencl_free_kernel(gd->kernel_basecurve_blur_v);
  dt_opencl_free_kernel(gd->kernel_basecurve_expand);
  dt_opencl_free_kernel(gd->kernel_basecurve_reduce);
  dt_opencl_free_kernel(gd->kernel_basecurve_detail);
  dt_opencl_free_kernel(gd->kernel_basecurve_adjust_features);
  dt_opencl_free_kernel(gd->kernel_basecurve_blend_gaussian);
  dt_opencl_free_kernel(gd->kernel_basecurve_blend_laplacian);
  dt_opencl_free_kernel(gd->kernel_basecurve_normalize);
  dt_opencl_free_kernel(gd->kernel_basecurve_reconstruct);
  dt_opencl_free_kernel(gd->kernel_basecurve_finalize);
  free(module->data);
  module->data = NULL;
}

static gboolean dt_iop_basecurve_enter_notify(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  gtk_widget_queue_draw(widget);
  return TRUE;
}

static gboolean dt_iop_basecurve_leave_notify(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  gtk_widget_queue_draw(widget);
  return TRUE;
}

static float to_log(const float x, const float base)
{
  if(base > 0.0f)
    return logf(x * base + 1.0f) / logf(base + 1.0f);
  else
    return x;
}

static float to_lin(const float x, const float base)
{
  if(base > 0.0f)
    return (powf(base - 1.0f, x) - 1.0f) / base;
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

  float unbounded_coeffs[3];
  const float xm = basecurve[nodes - 1].x;
  {
    const float x[4] = { 0.7f * xm, 0.8f * xm, 0.9f * xm, 1.0f * xm };
    const float y[4] = { c->draw_ys[CLAMP((int)(x[0] * DT_IOP_TONECURVE_RES), 0, DT_IOP_TONECURVE_RES - 1)],
                         c->draw_ys[CLAMP((int)(x[1] * DT_IOP_TONECURVE_RES), 0, DT_IOP_TONECURVE_RES - 1)],
                         c->draw_ys[CLAMP((int)(x[2] * DT_IOP_TONECURVE_RES), 0, DT_IOP_TONECURVE_RES - 1)],
                         c->draw_ys[CLAMP((int)(x[3] * DT_IOP_TONECURVE_RES), 0, DT_IOP_TONECURVE_RES - 1)] };
    dt_iop_estimate_exp(x, y, 4, unbounded_coeffs);
  }

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
  if(c->selected >= 0)
  {
    char text[30];
    // draw information about current selected node
    PangoLayout *layout;
    PangoRectangle ink;
    PangoFontDescription *desc = pango_font_description_copy_static(darktable.bauhaus->pango_font_desc);
    pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
    pango_font_description_set_absolute_size(desc, PANGO_SCALE);
    layout = pango_cairo_create_layout(cr);
    pango_layout_set_font_description(layout, desc);

    const float x_node_value = basecurve[c->selected].x * 100;
    const float y_node_value = basecurve[c->selected].y * 100;
    const float d_node_value = y_node_value - x_node_value;
    // scale conservatively to 100% of width:
    snprintf(text, sizeof(text), "100.00 / 100.00 ( +100.00)");
    pango_layout_set_text(layout, text, -1);
    pango_layout_get_pixel_extents(layout, &ink, NULL);
    pango_font_description_set_absolute_size(desc, (double)width / ink.width * PANGO_SCALE);
    pango_layout_set_font_description(layout, desc);

    snprintf(text, sizeof(text), "%.2f / %.2f ( %+.2f)", x_node_value, y_node_value, d_node_value);

    cairo_set_source_rgb(cr, 0.1, 0.1, 0.1);
    pango_layout_set_text(layout, text, -1);
    pango_layout_get_pixel_extents(layout, &ink, NULL);
    cairo_move_to(cr, 0.98f * width - ink.width - ink.x, -0.02 * height - ink.height - ink.y);
    pango_cairo_show_layout(cr, layout);
    cairo_stroke(cr);
    pango_font_description_free(desc);
    g_object_unref(layout);
  }
  cairo_scale(cr, 1.0f, -1.0f);

  // draw grid
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(.4));
  cairo_set_source_rgb(cr, .1, .1, .1);
  if(c->loglogscale)
    dt_draw_loglog_grid(cr, 4, 0, 0, width, height, c->loglogscale + 1.0f);
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
    const float xx = k / (DT_IOP_TONECURVE_RES - 1.0f);
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

static inline int _add_node(dt_iop_basecurve_node_t *basecurve, int *nodes, float x, float y)
{
  int selected = -1;
  if(basecurve[0].x > x)
    selected = 0;
  else
  {
    for(int k = 1; k < *nodes; k++)
    {
      if(basecurve[k].x > x)
      {
        selected = k;
        break;
      }
    }
  }
  if(selected == -1) selected = *nodes;
  for(int i = *nodes; i > selected; i--)
  {
    basecurve[i].x = basecurve[i - 1].x;
    basecurve[i].y = basecurve[i - 1].y;
  }
  // found a new point
  basecurve[selected].x = x;
  basecurve[selected].y = y;
  (*nodes)++;
  return selected;
}

static void dt_iop_basecurve_sanity_check(dt_iop_module_t *self, GtkWidget *widget)
{
  dt_iop_basecurve_gui_data_t *c = (dt_iop_basecurve_gui_data_t *)self->gui_data;
  dt_iop_basecurve_params_t *p = (dt_iop_basecurve_params_t *)self->params;

  int ch = 0;
  int nodes = p->basecurve_nodes[ch];
  dt_iop_basecurve_node_t *basecurve = p->basecurve[ch];

  if(nodes <= 2) return;

  const float mx = basecurve[c->selected].x;

  // delete vertex if order has changed
  // for all points, x coordinate of point must be strictly larger than
  // the x coordinate of the previous point
  if((c->selected > 0 && (basecurve[c->selected - 1].x >= mx))
     || (c->selected < nodes - 1 && (basecurve[c->selected + 1].x <= mx)))
  {
    for(int k = c->selected; k < nodes - 1; k++)
    {
      basecurve[k].x = basecurve[k + 1].x;
      basecurve[k].y = basecurve[k + 1].y;
    }
    c->selected = -2; // avoid re-insertion of that point immediately after this
    p->basecurve_nodes[ch]--;
  }
}

static gboolean _move_point_internal(dt_iop_module_t *self, GtkWidget *widget, float dx, float dy, guint state);

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
  const double old_m_x = c->mouse_x;
  const double old_m_y = c->mouse_y;
  c->mouse_x = event->x - inset;
  c->mouse_y = event->y - inset;

  const float mx = CLAMP(c->mouse_x, 0, width) / (float)width;
  const float my = 1.0f - CLAMP(c->mouse_y, 0, height) / (float)height;
  const float linx = to_lin(mx, c->loglogscale), liny = to_lin(my, c->loglogscale);

  if(event->state & GDK_BUTTON1_MASK)
  {
    // got a vertex selected:
    if(c->selected >= 0)
    {
      // this is used to translate mause position in loglogscale to make this behavior unified with linear scale.
      const float translate_mouse_x = old_m_x / width - to_log(basecurve[c->selected].x, c->loglogscale);
      const float translate_mouse_y = 1 - old_m_y / height - to_log(basecurve[c->selected].y, c->loglogscale);
      // dx & dy are in linear coordinates
      const float dx = to_lin(c->mouse_x / width - translate_mouse_x, c->loglogscale)
                       - to_lin(old_m_x / width - translate_mouse_x, c->loglogscale);
      const float dy = to_lin(1 - c->mouse_y / height - translate_mouse_y, c->loglogscale)
                       - to_lin(1 - old_m_y / height - translate_mouse_y, c->loglogscale);

      return _move_point_internal(self, widget, dx, dy, event->state);
    }
    else if(nodes < MAXNODES && c->selected >= -1)
    {
      // no vertex was close, create a new one!
      c->selected = _add_node(basecurve, &p->basecurve_nodes[ch], linx, liny);
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
  if(c->selected >= 0) gtk_widget_grab_focus(widget);
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
  int nodes = p->basecurve_nodes[ch];
  dt_iop_basecurve_node_t *basecurve = p->basecurve[ch];

  if(event->button == 1)
  {
    if(event->type == GDK_BUTTON_PRESS && dt_modifier_is(event->state, GDK_CONTROL_MASK)
      && nodes < MAXNODES && c->selected == -1)
    {
      // if we are not on a node -> add a new node at the current x of the pointer and y of the curve at that x
      const int inset = DT_GUI_CURVE_EDITOR_INSET;
      GtkAllocation allocation;
      gtk_widget_get_allocation(widget, &allocation);
      int width = allocation.width - 2 * inset;
      c->mouse_x = event->x - inset;
      c->mouse_y = event->y - inset;

      const float mx = CLAMP(c->mouse_x, 0, width) / (float)width;
      const float linx = to_lin(mx, c->loglogscale);

      // don't add a node too close to others in x direction, it can crash dt
      int selected = -1;
      if(basecurve[0].x > linx)
        selected = 0;
      else
      {
        for(int k = 1; k < nodes; k++)
        {
          if(basecurve[k].x > linx)
          {
            selected = k;
            break;
          }
        }
      }
      if(selected == -1) selected = nodes;
      // > 0 -> check distance to left neighbour
      // < nodes -> check distance to right neighbour
      if(!((selected > 0 && linx - basecurve[selected - 1].x <= 0.025) ||
           (selected < nodes && basecurve[selected].x - linx <= 0.025)))
      {
        // evaluate the curve at the current x position
        const float y = dt_draw_curve_calc_value(c->minmax_curve, linx);

        if(y >= 0.0 && y <= 1.0) // never add something outside the viewport, you couldn't change it afterwards
        {
          // create a new node
          selected = _add_node(basecurve, &p->basecurve_nodes[ch], linx, y);

          // maybe set the new one as being selected
          float min = .04f;
          min *= min; // comparing against square
          for(int k = 0; k < nodes; k++)
          {
            float other_y = to_log(basecurve[k].y, c->loglogscale);
            float dist = (y - other_y) * (y - other_y);
            if(dist < min) c->selected = selected;
          }

          dt_dev_add_history_item(darktable.develop, self, TRUE);
          gtk_widget_queue_draw(self->widget);
        }
      }
      return TRUE;
    }
    else if(event->type == GDK_2BUTTON_PRESS)
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
  }
  else if(event->button == 3 && c->selected >= 0)
  {
    if(c->selected == 0 || c->selected == nodes - 1)
    {
      float reset_value = c->selected == 0 ? 0 : 1;
      basecurve[c->selected].y = basecurve[c->selected].x = reset_value;
      gtk_widget_queue_draw(self->widget);
      dt_dev_add_history_item(darktable.develop, self, TRUE);
      return TRUE;
    }

    for(int k = c->selected; k < nodes - 1; k++)
    {
      basecurve[k].x = basecurve[k + 1].x;
      basecurve[k].y = basecurve[k + 1].y;
    }
    basecurve[nodes - 1].x = basecurve[nodes - 1].y = 0;
    c->selected = -2; // avoid re-insertion of that point immediately after this
    p->basecurve_nodes[ch]--;
    gtk_widget_queue_draw(self->widget);
    dt_dev_add_history_item(darktable.develop, self, TRUE);
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

static gboolean _move_point_internal(dt_iop_module_t *self, GtkWidget *widget, float dx, float dy, guint state)
{
  dt_iop_basecurve_params_t *p = (dt_iop_basecurve_params_t *)self->params;
  dt_iop_basecurve_gui_data_t *c = (dt_iop_basecurve_gui_data_t *)self->gui_data;

  int ch = 0;
  dt_iop_basecurve_node_t *basecurve = p->basecurve[ch];

  float multiplier = dt_accel_get_speed_multiplier(widget, state);
  dx *= multiplier;
  dy *= multiplier;

  basecurve[c->selected].x = CLAMP(basecurve[c->selected].x + dx, 0.0f, 1.0f);
  basecurve[c->selected].y = CLAMP(basecurve[c->selected].y + dy, 0.0f, 1.0f);

  dt_iop_basecurve_sanity_check(self, widget);

  gtk_widget_queue_draw(widget);
  dt_iop_queue_history_update(self, FALSE);
  return TRUE;
}

#define BASECURVE_DEFAULT_STEP (0.001f)

static gboolean _scrolled(GtkWidget *widget, GdkEventScroll *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_basecurve_gui_data_t *c = (dt_iop_basecurve_gui_data_t *)self->gui_data;

  if(dt_gui_ignore_scroll(event)) return FALSE;

  if(c->selected < 0) return TRUE;

  gdouble delta_y;
  if(dt_gui_get_scroll_delta(event, &delta_y))
  {
    delta_y *= -BASECURVE_DEFAULT_STEP;
    return _move_point_internal(self, widget, 0.0, delta_y, event->state);
  }

  return TRUE;
}

static gboolean dt_iop_basecurve_key_press(GtkWidget *widget, GdkEventKey *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_basecurve_gui_data_t *c = (dt_iop_basecurve_gui_data_t *)self->gui_data;

  if(c->selected < 0) return TRUE;

  int handled = 0;
  float dx = 0.0f, dy = 0.0f;
  if(event->keyval == GDK_KEY_Up || event->keyval == GDK_KEY_KP_Up)
  {
    handled = 1;
    dy = BASECURVE_DEFAULT_STEP;
  }
  else if(event->keyval == GDK_KEY_Down || event->keyval == GDK_KEY_KP_Down)
  {
    handled = 1;
    dy = -BASECURVE_DEFAULT_STEP;
  }
  else if(event->keyval == GDK_KEY_Right || event->keyval == GDK_KEY_KP_Right)
  {
    handled = 1;
    dx = BASECURVE_DEFAULT_STEP;
  }
  else if(event->keyval == GDK_KEY_Left || event->keyval == GDK_KEY_KP_Left)
  {
    handled = 1;
    dx = -BASECURVE_DEFAULT_STEP;
  }

  if(!handled) return FALSE;

  return _move_point_internal(self, widget, dx, dy, event->state);
}

#undef BASECURVE_DEFAULT_STEP

void gui_changed(dt_iop_module_t *self, GtkWidget *w, void *previous)
{
  dt_iop_basecurve_params_t *p = (dt_iop_basecurve_params_t *)self->params;
  dt_iop_basecurve_gui_data_t *g = (dt_iop_basecurve_gui_data_t *)self->gui_data;

  if(w == g->fusion)
  {
    int prev = *(int *)previous;
    if(p->exposure_fusion != 0 && prev == 0)
    {
      gtk_widget_set_visible(g->exposure_step, TRUE);
      gtk_widget_set_visible(g->exposure_bias, TRUE);
    }
    if(p->exposure_fusion == 0 && prev != 0)
    {
      gtk_widget_set_visible(g->exposure_step, FALSE);
      gtk_widget_set_visible(g->exposure_bias, FALSE);
    }
  }
}

static void logbase_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_basecurve_gui_data_t *g = (dt_iop_basecurve_gui_data_t *)self->gui_data;
  g->loglogscale = eval_grey(dt_bauhaus_slider_get(g->logbase));
  gtk_widget_queue_draw(GTK_WIDGET(g->area));
}

void gui_init(struct dt_iop_module_t *self)
{
  dt_iop_basecurve_gui_data_t *c = IOP_GUI_ALLOC(basecurve);
  dt_iop_basecurve_params_t *p = (dt_iop_basecurve_params_t *)self->default_params;

  c->minmax_curve = dt_draw_curve_new(0.0, 1.0, p->basecurve_type[0]);
  c->minmax_curve_type = p->basecurve_type[0];
  c->minmax_curve_nodes = p->basecurve_nodes[0];
  for(int k = 0; k < p->basecurve_nodes[0]; k++)
    (void)dt_draw_curve_add_point(c->minmax_curve, p->basecurve[0][k].x, p->basecurve[0][k].y);
  c->mouse_x = c->mouse_y = -1.0;
  c->selected = -1;
  c->loglogscale = 0;
  self->timeout_handle = 0;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  c->area = GTK_DRAWING_AREA(dtgtk_drawing_area_new_with_aspect_ratio(1.0));
  gtk_widget_set_tooltip_text(GTK_WIDGET(c->area), _("abscissa: input, ordinate: output. works on RGB channels"));
  g_object_set_data(G_OBJECT(c->area), "iop-instance", self);
  dt_action_define_iop(self, NULL, N_("curve"), GTK_WIDGET(c->area), NULL);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(c->area), TRUE, TRUE, 0);

  c->cmb_preserve_colors = dt_bauhaus_combobox_from_params(self, "preserve_colors");
  gtk_widget_set_tooltip_text(c->cmb_preserve_colors, _("method to preserve colors when applying contrast"));

  c->fusion = dt_bauhaus_combobox_from_params(self, "exposure_fusion");
  dt_bauhaus_combobox_add(c->fusion, _("none"));
  dt_bauhaus_combobox_add(c->fusion, _("two exposures"));
  dt_bauhaus_combobox_add(c->fusion, _("three exposures"));
  gtk_widget_set_tooltip_text(c->fusion, _("fuse this image stopped up/down a couple of times with itself, to "
                                           "compress high dynamic range. expose for the highlights before use."));

  c->exposure_step = dt_bauhaus_slider_from_params(self, "exposure_stops");
  dt_bauhaus_slider_set_digits(c->exposure_step, 3);
  gtk_widget_set_tooltip_text(c->exposure_step, _("how many stops to shift the individual exposures apart"));
  gtk_widget_set_no_show_all(c->exposure_step, TRUE);
  gtk_widget_set_visible(c->exposure_step, p->exposure_fusion != 0 ? TRUE : FALSE);

  // initially set to 1 (consistency with previous versions), but double-click resets to 0
  // to get a quick way to reach 0 with the mouse.
  c->exposure_bias = dt_bauhaus_slider_from_params(self, "exposure_bias");
  dt_bauhaus_slider_set_default(c->exposure_bias, 0.0f);
  dt_bauhaus_slider_set_digits(c->exposure_bias, 3);
  gtk_widget_set_tooltip_text(c->exposure_bias, _("whether to shift exposure up or down "
                                                  "(-1: reduce highlight, +1: reduce shadows)"));
  gtk_widget_set_no_show_all(c->exposure_bias, TRUE);
  gtk_widget_set_visible(c->exposure_bias, p->exposure_fusion != 0 ? TRUE : FALSE);
  c->logbase = dt_bauhaus_slider_new_with_range(self, 0.0f, 40.0f, 0, 0.0f, 2);
  dt_bauhaus_widget_set_label(c->logbase, NULL, N_("scale for graph"));
  gtk_box_pack_start(GTK_BOX(self->widget), c->logbase , TRUE, TRUE, 0);  g_signal_connect(G_OBJECT(c->logbase), "value-changed", G_CALLBACK(logbase_callback), self);

  gtk_widget_add_events(GTK_WIDGET(c->area), GDK_POINTER_MOTION_MASK | darktable.gui->scroll_mask
                                           | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK
                                           | GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK);
  gtk_widget_set_can_focus(GTK_WIDGET(c->area), TRUE);
  g_signal_connect(G_OBJECT(c->area), "draw", G_CALLBACK(dt_iop_basecurve_draw), self);
  g_signal_connect(G_OBJECT(c->area), "button-press-event", G_CALLBACK(dt_iop_basecurve_button_press), self);
  g_signal_connect(G_OBJECT(c->area), "motion-notify-event", G_CALLBACK(dt_iop_basecurve_motion_notify), self);
  g_signal_connect(G_OBJECT(c->area), "leave-notify-event", G_CALLBACK(dt_iop_basecurve_leave_notify), self);
  g_signal_connect(G_OBJECT(c->area), "enter-notify-event", G_CALLBACK(dt_iop_basecurve_enter_notify), self);
  g_signal_connect(G_OBJECT(c->area), "configure-event", G_CALLBACK(area_resized), self);
  g_signal_connect(G_OBJECT(c->area), "scroll-event", G_CALLBACK(_scrolled), self);
  g_signal_connect(G_OBJECT(c->area), "key-press-event", G_CALLBACK(dt_iop_basecurve_key_press), self);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  dt_iop_basecurve_gui_data_t *c = (dt_iop_basecurve_gui_data_t *)self->gui_data;
  dt_draw_curve_destroy(c->minmax_curve);
  dt_iop_cancel_history_update(self);

  IOP_GUI_FREE;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

