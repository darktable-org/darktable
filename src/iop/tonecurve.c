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
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "bauhaus/bauhaus.h"
#include "common/opencl.h"
#include "common/colorspaces_inline_conversions.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "dtgtk/drawingarea.h"
#include "dtgtk/paint.h"
#include "gui/draw.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "gui/color_picker_proxy.h"
#include "iop/iop_api.h"
#include "libs/colorpicker.h"

#define DT_GUI_CURVE_EDITOR_INSET DT_PIXEL_APPLY_DPI(1)
#define DT_GUI_CURVE_INFL .3f

#define DT_IOP_TONECURVE_RES 256
#define DT_IOP_TONECURVE_MAXNODES 20
#define DT_IOP_TONECURVE_MAX_CH 4
#define DT_IOP_TONECURVE_BINS 256

DT_MODULE_INTROSPECTION(5, dt_iop_tonecurve_params_t)

static gboolean dt_iop_tonecurve_draw(GtkWidget *widget, cairo_t *crf, gpointer user_data);
static gboolean dt_iop_tonecurve_motion_notify(GtkWidget *widget, GdkEventMotion *event, gpointer user_data);
static gboolean dt_iop_tonecurve_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data);
static gboolean dt_iop_tonecurve_leave_notify(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data);
static gboolean dt_iop_tonecurve_enter_notify(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data);
static gboolean dt_iop_tonecurve_key_press(GtkWidget *widget, GdkEventKey *event, gpointer user_data);

typedef enum tonecurve_channel_t
{
  ch_L = 0,
  ch_a = 1,
  ch_C = 1,
  ch_b = 2,
  ch_CL = 2,
//  ch_Ch = 2
  ch_LC = 3,
} tonecurve_channel_t;

typedef enum tonecurve_log_t
{
  linxliny = 0,
  logxlogy = 1,
  logxliny = 2,
  linxlogy = 3,
} tonecurve_log_t;

typedef struct dt_iop_tonecurve_node_t
{
  float x;
  float y;
} dt_iop_tonecurve_node_t;

typedef enum dt_iop_tonecurve_mode_t
{
  DT_S_SCALE_MANUAL_LAB = 0,       // three curves (L, a, b)
  DT_S_SCALE_AUTOMATIC_LAB = 1,    // automatically adjust saturation based on L_out/L_in
  DT_S_SCALE_AUTOMATIC_XYZ = 2,    // automatically adjust saturation by
                                   // transforming the curve C to C' like:
                                   // L_out=C(L_in) -> Y_out=C'(Y_in) and applying C' to the X and Z
                                   // channels, too (and then transforming it back to Lab of course)
  DT_S_SCALE_AUTOMATIC_RGB = 3,    // similar to above but use an rgb working space
  DT_S_SCALE_MANUAL_RGB = 4,
  DT_S_SCALE_MANUAL_LCH = 5,       // manual LCh instead of manual Lab
} dt_iop_tonecurve_mode_t;

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

typedef struct dt_iop_tonecurve_params4_t
{
  dt_iop_tonecurve_node_t tonecurve[3][DT_IOP_TONECURVE_MAXNODES]; // three curves (L, a, b) with max number
                                                                   // of nodes
  int tonecurve_nodes[3];
  int tonecurve_type[3];
  int tonecurve_autoscale_ab;
  int tonecurve_preset;
  int tonecurve_unbound_ab;
} dt_iop_tonecurve_params4_t;

typedef struct dt_iop_tonecurve_params_t
{
  dt_iop_tonecurve_node_t tonecurve[DT_IOP_TONECURVE_MAX_CH][DT_IOP_TONECURVE_MAXNODES];
  int tonecurve_nodes[DT_IOP_TONECURVE_MAX_CH];
  int tonecurve_type[DT_IOP_TONECURVE_MAX_CH];
  int tonecurve_tc_mode;
  int tonecurve_preset;
  int tonecurve_unbound_ab;
  int rgb_norm;
  float rgb_norm_exp;
} dt_iop_tonecurve_params_t;

typedef struct dt_iop_tonecurve_gui_data_t
{
  dt_draw_curve_t *minmax_curve[DT_IOP_TONECURVE_MAX_CH]; // curves for gui to draw
  int minmax_curve_nodes[DT_IOP_TONECURVE_MAX_CH];
  int minmax_curve_type[DT_IOP_TONECURVE_MAX_CH];
  GtkBox *hbox;
  GtkDrawingArea *area;
  GtkSizeGroup *sizegroup;
  GtkWidget *tc_mode;
  GtkNotebook *channel_tabs;
  GtkWidget *colorpicker;
  dt_iop_color_picker_t color_picker;
  GtkWidget *interpolator;
  GtkWidget *scale;
  tonecurve_channel_t channel;
  double mouse_x, mouse_y;
  int selected;
  float draw_xs[DT_IOP_TONECURVE_RES], draw_ys[DT_IOP_TONECURVE_RES];
//  float draw_min_xs[DT_IOP_TONECURVE_RES], draw_min_ys[DT_IOP_TONECURVE_RES];  // not used
//  float draw_max_xs[DT_IOP_TONECURVE_RES], draw_max_ys[DT_IOP_TONECURVE_RES];  // not used
  float loglogscale[DT_IOP_TONECURVE_MAX_CH];
  int scale_mode[DT_IOP_TONECURVE_MAX_CH];
  GtkWidget *logbase;
  GtkWidget *rgb_norm;
  GtkWidget *rgb_norm_exp;
  gboolean got_focus;
  // local histogram
  uint32_t local_histogram[DT_IOP_TONECURVE_BINS * DT_IOP_TONECURVE_MAX_CH];
  // maximum levels in histogram, one per channel
  uint32_t local_histogram_max[DT_IOP_TONECURVE_MAX_CH];
} dt_iop_tonecurve_gui_data_t;

typedef struct dt_iop_tonecurve_data_t
{
  dt_draw_curve_t *curve[DT_IOP_TONECURVE_MAX_CH];     // curves for pipe piece and pixel processing
  int curve_nodes[DT_IOP_TONECURVE_MAX_CH];            // number of nodes
  int curve_type[DT_IOP_TONECURVE_MAX_CH];             // curve style (e.g. CUBIC_SPLINE)
  float table[DT_IOP_TONECURVE_MAX_CH][0x10000];       // precomputed look-up tables for tone curve
  float unbounded_coeffs[DT_IOP_TONECURVE_MAX_CH][6];   // approximation coef for extrapolation
  int tc_mode;
  int unbound_ab;
  int rgb_norm;
  float rgb_norm_exp;
} dt_iop_tonecurve_data_t;

static const struct
{
//  const char *tab_label; doesn't work with _("") translation method
  int chx; // if chx = chy identity = diagonal else  identity = y=0.5
  int chy;
  float c_min_pipe_value;  // pipe value
  float c_max_pipe_value;  // pipe value
  float c_min_display_value;  // display value
  float c_max_display_value;  // display value
  float c_lut_factor; // can be used to save multiplication/division
  int c_histogram_enabled;    // 0 = disabled, 1 = enabled
  int c_log_enabled;   // 0 = disabled, 1 = enabled
  int c_extrapolation; // 1 = Single; 2 = Double
  int c_nb_nodes;
  dt_iop_tonecurve_node_t c_nodes[DT_IOP_TONECURVE_MAXNODES];
} curve_detail[] = {
  // 0 - L(Lab) - L(LCh)
  {0, 0, 0.0f, 100.0f, 0.0f, 100.0f, 100.0f, 1, 1, 1, 2, { { 0.0f, 0.0f }, { 1.0f, 1.0f } }},
  // 1 - a (Lab)
  {1, 1, -128.0f, 127.0f, -128.0f, 127.0f, 256.0f, 1, 0, 2, 3, { { 0.0, 0.0 }, { 0.5, 0.5 }, { 1.0, 1.0 } }},
  // 2 - b (Lab)
  {2, 2, -128.0f, 127.0f, -128.0f, 127.0f, 256.0f, 1, 0, 2, 3, { { 0.0, 0.0 }, { 0.5, 0.5 }, { 1.0, 1.0 } }},
  // 3 - C(C) (LCh)
  {1, 1, 0.0f, 182.019f, 0.0f, 100.0f, 1.0f, 1, 1, 1, 2, { { 0.0f, 0.0f }, { 1.0f, 1.0f } }},
  // 4 - C(L) (LCh)
  {0, 1, 0.0f, 182.019f, 0.0f, 100.0f, 1.0f, 1, 1, 1, 2, { { 0.0f, 0.5f }, { 1.0f, 0.5f } }},
  // 5 - L(C) (LCh)
  {1, 0, 0.0f, 100.0f, 0.0f, 100.0f, 1.0f, 1, 1, 1, 2, { { 0.0f, 0.5f }, { 1.0f, 0.5f } }},
  // 4 - C(h) (LCh)
//  {2, 1, 0.0f, 1.0f, 0.0f, 360.0f, 1.0f, 1, 0, 1, 2, { { 0.0f, 0.5f }, { 1.0f, 0.5f } }},
  // 6 - L(RGB) - L(XYZ)
  {0, 0, 0.0f, 100.0f, 0.0f, 100.0f, 100.0f, 1, 1, 1, 2, { { 0.0f, 0.0f }, { 1.0f, 1.0f } }},
  // 7 - L(LRGB)
  {0, 0, 0.0f, 100.0f, 0.0f, 100.0f, 1.0f, 1, 1, 1, 2, { { 0.0f, 0.0f }, { 1.0f, 1.0f } }},
  // 8 - R (LRGB)
  {1, 1, 0.0f, 100.0f, 0.0f, 100.0f, 1.0f, 1, 1, 1, 2, { { 0.0f, 0.0f}, { 1.0f, 1.0f } }},
  // 9 - G (LRGB)
  {2, 2, 0.0f, 100.0f, 0.0f, 100.0f, 1.0f, 1, 1, 1, 2, { { 0.0f, 0.0f }, { 1.0f, 1.0f } }},
  // 10 - B (LRGB)
  {3, 3, 0.0f, 100.0f, 0.0f, 100.0f, 1.0f, 1, 1, 1, 2, { { 0.0f, 0.0f }, { 1.0f, 1.0f } }}
};

typedef enum dt_iop_tonecurve_UI_colorspace_t // could use the standard dt colorspace enum
{                                           // but not sure they have the same meaning
  DT_TC_LAB = 0,
  DT_TC_XYZ = 1,
  DT_TC_RGB = 2,
  DT_TC_LCH = 3,
} dt_iop_tonecurve_UI_colorspace_t;

static const struct
{
  int nb_ch;
  int colorspace;
  int curve_detail_i[DT_IOP_TONECURVE_MAX_CH];
} mode_curves[] = {
  {3, DT_TC_LAB, {0, 1, 2}},  // DT_S_SCALE_MANUAL_LAB
  {1, DT_TC_LAB, {0}},  // DT_S_SCALE_AUTOMATIC_LAB
  {1, DT_TC_LAB, {6}}, // DT_S_SCALE_AUTOMATIC_XYZ
  {1, DT_TC_LAB, {6}}, // DT_S_SCALE_AUTOMATIC_RGB
  {4, DT_TC_RGB, {7, 8, 9, 10}}, // DT_S_SCALE_MANUAL_RGB
  {4, DT_TC_LCH, {0, 3, 4, 5}}  // DT_S_SCALE_MANUAL_LCH
};

typedef struct dt_iop_tonecurve_global_data_t
{
  float picked_color[3];
  float picked_color_min[3];
  float picked_color_max[3];
  float picked_output_color[3];
  int kernel_tonecurve;
} dt_iop_tonecurve_global_data_t;


const char *name()
{
  return _("tone curve");
}

int default_group()
{
  return IOP_GROUP_TONE;
}

int flags()
{
  return IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return iop_cs_Lab;
}

int legacy_params(dt_iop_module_t *self, const void *const old_params, const int old_version,
                  void *new_params, const int new_version)
{
  if(old_version == 1 && new_version == 5)
  {
    dt_iop_tonecurve_params1_t *o = (dt_iop_tonecurve_params1_t *)old_params;
    dt_iop_tonecurve_params_t *n = (dt_iop_tonecurve_params_t *)new_params;

    // start with a fresh copy of default parameters
    // unfortunately default_params aren't inited at this stage.
    *n = (dt_iop_tonecurve_params_t){ { { { 0.0, 0.0 }, { 1.0, 1.0 } },
                                        { { 0.0, 0.0 }, { 0.5, 0.5 }, { 1.0, 1.0 } },
                                        { { 0.0, 0.0 }, { 0.5, 0.5 }, { 1.0, 1.0 } },
                                        { { 0.0, 0.0 }, { 1.0, 1.0 } } },
                                      { 2, 3, 3, 2 },
                                      { CUBIC_SPLINE, CUBIC_SPLINE, CUBIC_SPLINE, CUBIC_SPLINE },
                                      DT_S_SCALE_AUTOMATIC_LAB,
                                      0,
                                      0,
                                      2};
    for(int k = 0; k < 6; k++) n->tonecurve[ch_L][k].x = o->tonecurve_x[k];
    for(int k = 0; k < 6; k++) n->tonecurve[ch_L][k].y = o->tonecurve_y[k];
    n->tonecurve_nodes[ch_L] = 6;
//    n->tonecurve_type[ch_L] = CUBIC_SPLINE;
//    n->tonecurve_tc_mode = DT_S_SCALE_AUTOMATIC_LAB;
    n->tonecurve_preset = o->tonecurve_preset;
//    n->tonecurve_unbound_ab = 0;
//    n->rgb_norm = 0;
//    n->rgb_norm_exp = 2.0f;
    return 0;
  }
  else if(old_version == 2 && new_version == 5)
  {
    // version 2 never really materialized so there should be no legacy history stacks of that version around
    return 1;
  }
  else if(old_version == 3 && new_version == 5)
  {
    dt_iop_tonecurve_params3_t *o = (dt_iop_tonecurve_params3_t *)old_params;
    dt_iop_tonecurve_params_t *n = (dt_iop_tonecurve_params_t *)new_params;

    memcpy(n->tonecurve, o->tonecurve, sizeof(o->tonecurve));
    memcpy(n->tonecurve_nodes, o->tonecurve_nodes, sizeof(o->tonecurve_nodes));
    memcpy(n->tonecurve_type, o->tonecurve_type, sizeof(o->tonecurve_type));
    n->tonecurve_tc_mode = o->tonecurve_autoscale_ab;
    n->tonecurve_preset = o->tonecurve_preset;
    n->tonecurve_unbound_ab = 0;
    n->rgb_norm = 0;
    n->rgb_norm_exp = 2.0f;
    return 0;
  }
  else if(old_version == 4 && new_version == 5)
  {
    dt_iop_tonecurve_params3_t *o = (dt_iop_tonecurve_params3_t *)old_params;
    dt_iop_tonecurve_params_t *n = (dt_iop_tonecurve_params_t *)new_params;

    memcpy(n->tonecurve, o->tonecurve, sizeof(o->tonecurve));
    memcpy(n->tonecurve_nodes, o->tonecurve_nodes, sizeof(o->tonecurve_nodes));
    memcpy(n->tonecurve_type, o->tonecurve_type, sizeof(o->tonecurve_type));
    n->tonecurve_tc_mode = o->tonecurve_autoscale_ab;
    n->tonecurve_preset = o->tonecurve_preset;
    n->tonecurve_unbound_ab = 0;
    n->rgb_norm = 0;
    n->rgb_norm_exp = 2.0f;
    return 0;
  }

  return 1;
}

typedef enum dt_rgb_norm_t //
{
  DT_RGB_NORM_L = 1,
  DT_RGB_NORM_AVG = 2,
  DT_RGB_NORM_LP = 3,
  DT_RGB_NORM_BP = 4,
  DT_RGB_NORM_WYP = 5,
} dt_rgb_norm_t;

float dt_rgb_norm_vect(float rgb[4], int rgb_norm, float norm_exp)
{
// RGB values are in[0,1]
// ensure (norm >= 0.0f) in GUI controls for better perf

  if(rgb[0] < 0.0f || rgb[1] < 0.0f || rgb[2] < 0.0f) return -1;

  switch(rgb_norm)
  {
  case DT_RGB_NORM_L:  // norm L infinite = max
    return fmaxf(rgb[0], fmaxf(rgb[1], rgb[2]));
  case DT_RGB_NORM_AVG:  // norm L average(rgb)
      return (rgb[0] + rgb[1] + rgb[2]) / 3.0f;
  case DT_RGB_NORM_LP:  // general Lp norm (pseudo-norm if p < 1) - slow variant
    if (norm_exp == 1.0f) return (rgb[0] + rgb[1] + rgb[2]);
    else if (norm_exp == 2.0f) return sqrtf((rgb[0] * rgb[0] + rgb[1] * rgb[1] + rgb[2] * rgb[2]));
    return powf((powf(rgb[0], norm_exp) + powf(rgb[1], norm_exp) + powf(rgb[2], norm_exp)), 1.0f/norm_exp);
  case DT_RGB_NORM_BP:  // basic power norm
    {
      float R, G, B;
      R = rgb[0] * rgb[0];
      G = rgb[1] * rgb[1];
      B = rgb[2] * rgb[2];
      return (rgb[0] * R + rgb[1] * G + rgb[2] * B) / (R + G + B);
    }
  case DT_RGB_NORM_WYP: // weighted yellow power norm
    {
      const float coeff_R = 2.21533456f;  // 1.22^4
      const float coeff_G = 2.0736f; // 1.20^4
      const float coeff_B = 0.11316496f; //0.58^4
      const float rgb4[3] = {rgb[0] * rgb[0] * rgb[0] * rgb[0] * coeff_R,
                            rgb[1] * rgb[1] * rgb[1] * rgb[1] * coeff_G,
                            rgb[2] * rgb[2] * rgb[2] * rgb[2] * coeff_B};
      const float rgb5[3] = {rgb4[0] * rgb[0] * 1.22f, rgb4[1] * rgb[1] * 1.20f, rgb4[2] * rgb[2] * 0.58f};
      return 0.83743219f * (rgb5[0] + rgb5[1] + rgb5[2]) / (rgb4[0] + rgb4[1] + rgb4[2]);
    }
  default: {return -1;}
  }
}

typedef void((*worker_t)(const float *pixel, uint32_t *histogram));

void histogram_Lab(const float *pixel, uint32_t *histogram)
{
//  histogram[4 * (uint8_t)CLAMP(pixel[0]*2.55f, 0.0f, 255.0f)]++;
  float rgb[3] = {0, 0, 0};
  dt_Lab_to_prophotorgb(pixel, rgb);
  for(int c=0; c<3; c++)
  {
    const float fake_rgb[3] = {rgb[c], rgb[c], rgb[c]};
    float lab[3];
    // convert colors to pseudo-Lab for UI:
    // only the [0] element of the vector is relevant (pseudo-L)
    dt_prophotorgb_to_Lab(fake_rgb, lab);
    // histogram_RGB can be put to 255 bins
    histogram[4 * (uint8_t)CLAMP(lab[0]*2.55f, 0.0f, 255.0f) + c+1]++;
  }
}

void histogram_LCh(const float *pixel, uint32_t *histogram)
{
  float LCh[3] = {0, 0, 0};
  dt_Lab_2_LCH(pixel, LCh);
//  histogram[4 * (uint8_t)CLAMP(LCh[0]*2.55f, 0.0f, 255.0f)]++;  // L (use the standard one instead)
  const uint8_t chroma_i = 4 * (uint8_t)CLAMP(LCh[1]*1.40095, 0.0f, 255.0f);
  histogram[chroma_i + 1]++;  // chroma; 255 / 182.019 = 1.40095
  histogram[4 * (uint8_t)CLAMP(LCh[0]*2.55f, 0.0f, 255.0f) + 2]++;  // L
//  histogram[4 * (uint8_t)CLAMP(LCh[2]*255.0f, 0.0f, 255.0f) + 2]++;  // hue
  histogram[chroma_i + 3]++;  // chroma; 255 / 182.019 = 1.40095
}

void histogram_worker(const void *const pixel, const int height, const int width, uint32_t *histogram, const worker_t Worker)
{
  const int nthreads = omp_get_max_threads();

  const size_t bins_total = (size_t)4 * DT_IOP_TONECURVE_BINS;
  const size_t buf_size = bins_total * sizeof(uint32_t);
  void *partial_hists = calloc(nthreads, buf_size);

#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none) shared(partial_hists)
#endif
  for(int j = 0; j < height; j++)
  {
    uint32_t *thread_hist = (uint32_t *)partial_hists + bins_total * omp_get_thread_num();
    float *in = (float *)pixel + 4 * (width * j);
    for(int i = 0; i < width; i++, in += 4)
    {
      Worker(in, thread_hist);
    }
  }

#ifdef _OPENMP
  memset((void*)histogram, 0, buf_size);
  uint32_t *hist = (uint32_t *)histogram;

#pragma omp parallel for schedule(static) default(none) shared(hist, partial_hists)
  for(size_t k = 0; k < bins_total; k++)
  {
    for(size_t n = 0; n < nthreads; n++)
    {
      const uint32_t *thread_hist = (uint32_t *)partial_hists + bins_total * n;
      hist[k] += thread_hist[k];
    }
  }
#else
  memmove((void *)histogram, partial_hists, buf_size);
#endif
  free(partial_hists);
}

#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_tonecurve_data_t *d = (dt_iop_tonecurve_data_t *)piece->data;
  dt_dev_pixelpipe_t *pipe = (dt_dev_pixelpipe_t *)(piece->pipe);
  dt_iop_tonecurve_global_data_t *gd = (dt_iop_tonecurve_global_data_t *)self->data;
  dt_iop_tonecurve_gui_data_t *g = (dt_iop_tonecurve_gui_data_t *)self->gui_data;
  cl_mem dev_ch[DT_IOP_TONECURVE_MAX_CH];
  cl_mem dev_coeffs[DT_IOP_TONECURVE_MAX_CH];
  for(int ch = 0; ch < DT_IOP_TONECURVE_MAX_CH; ch++)
  {
    dev_ch[ch] = NULL;
    dev_coeffs[ch] = NULL;
  }
  cl_int err = -999;
  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;
  const int tc_mode = d->tc_mode;
  const int unbound_ab = d->unbound_ab;
  const float low_approximation = d->table[0][(int)(0.01f * 0x10000ul)];
  const int rgb_norm = d->rgb_norm;
  const float rgb_norm_exp = d->rgb_norm_exp;
  const gboolean histogram_needed = g && g->got_focus
      && pipe->type != DT_DEV_PIXELPIPE_PREVIEW
      && (tc_mode == DT_S_SCALE_MANUAL_RGB || tc_mode == DT_S_SCALE_MANUAL_LCH) ? TRUE : FALSE;
//printf("process_cl - tc_mode %d, pipe_type %d, histo needed %s\n", tc_mode, pipe->type, histogram_needed ? "Yes" : "No");
//if (g) printf("process_cl - focus %s\n", (g->got_focus)?"Yes":"No");
  for(int ch = 0; ch < DT_IOP_TONECURVE_MAX_CH; ch++)
  {
    dev_ch[ch] = dt_opencl_copy_host_to_device(devid, d->table[ch], 256, 256, sizeof(float));
    if(dev_ch[ch] == NULL) goto error;
    dev_coeffs[ch] = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * 6, d->unbounded_coeffs[ch]);
    if(dev_coeffs[ch] == NULL) goto error;
  }

  size_t sizes[] = { ROUNDUPWD(width), ROUNDUPHT(height), 1 };
  dt_opencl_set_kernel_arg(devid, gd->kernel_tonecurve, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_tonecurve, 1, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(devid, gd->kernel_tonecurve, 2, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_tonecurve, 3, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_tonecurve, 4, sizeof(int), (void *)&tc_mode);
  dt_opencl_set_kernel_arg(devid, gd->kernel_tonecurve, 5, sizeof(int), (void *)&unbound_ab);
  dt_opencl_set_kernel_arg(devid, gd->kernel_tonecurve, 6, sizeof(float), (void *)&low_approximation);
  dt_opencl_set_kernel_arg(devid, gd->kernel_tonecurve, 7, sizeof(float), (void *)&rgb_norm);
  dt_opencl_set_kernel_arg(devid, gd->kernel_tonecurve, 8, sizeof(float), (void *)&rgb_norm_exp);
  for(int ch = 0; ch < DT_IOP_TONECURVE_MAX_CH; ch++)
  {
    dt_opencl_set_kernel_arg(devid, gd->kernel_tonecurve, 9 + ch, sizeof(cl_mem), (void *)&dev_ch[ch]);
    dt_opencl_set_kernel_arg(devid, gd->kernel_tonecurve, 9 + DT_IOP_TONECURVE_MAX_CH + ch, sizeof(cl_mem), (void *)&dev_coeffs[ch]);
  }

  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_tonecurve, sizes);

  if(err != CL_SUCCESS) goto error;
  if (histogram_needed)
  {
    float *tmpbuf = NULL;
    float *pixel;
    pixel = tmpbuf = dt_alloc_align(64, (size_t)width * height * 4 * sizeof(float));
    if(!pixel) goto error;
    err = dt_opencl_copy_device_to_host(devid, pixel, dev_in, width, height, 4 * sizeof(float));
    if(err != CL_SUCCESS)
    {
      if(tmpbuf) dt_free_align(tmpbuf);
      goto error;
    }
    if (tc_mode == DT_S_SCALE_MANUAL_RGB)
      histogram_worker(pixel, height, width, &g->local_histogram[0], histogram_Lab);
    else if (tc_mode == DT_S_SCALE_MANUAL_LCH)
      histogram_worker(pixel, height, width, &g->local_histogram[0], histogram_LCh);
    if(tmpbuf) dt_free_align(tmpbuf);
//uint32_t sum_histo = 0;
//for(int c=0; c < 4*DT_IOP_TONECURVE_BINS; c++) sum_histo += g->local_histogram[c];
//printf("process_cl - height %d, width %d, sum histo %d\n", height, width, sum_histo);
  }
  for(int ch = 0; ch < DT_IOP_TONECURVE_MAX_CH; ch++)
  {
    dt_opencl_release_mem_object(dev_ch[ch]);
    dt_opencl_release_mem_object(dev_coeffs[ch]);
  }
  return TRUE;

error:
  for(int ch = 0; ch < DT_IOP_TONECURVE_MAX_CH; ch++)
  {
    dt_opencl_release_mem_object(dev_ch[ch]);
    dt_opencl_release_mem_object(dev_coeffs[ch]);
  }
  dt_print(DT_DEBUG_OPENCL, "[opencl_tonecurve] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const i, void *const o,
             const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const int ch = piece->colors;
  dt_iop_tonecurve_data_t *d = (dt_iop_tonecurve_data_t *)(piece->data);
  dt_dev_pixelpipe_t *pipe = (dt_dev_pixelpipe_t *)(piece->pipe);
  dt_iop_tonecurve_gui_data_t *g = (dt_iop_tonecurve_gui_data_t *)self->gui_data;
  const float xm[DT_IOP_TONECURVE_MAX_CH][2] = {
    {1.0f / d->unbounded_coeffs[0][0], 1.0f - 1.0f / d->unbounded_coeffs[0][3]},
    {1.0f / d->unbounded_coeffs[1][0], 1.0f - 1.0f / d->unbounded_coeffs[1][3]},
    {1.0f / d->unbounded_coeffs[2][0], 1.0f - 1.0f / d->unbounded_coeffs[2][3]},
    {1.0f / d->unbounded_coeffs[3][0], 1.0f - 1.0f / d->unbounded_coeffs[3][3]} };
  const float low_approximation = d->table[0][(int)(0.01f * 0x10000ul)];
  const int width = roi_out->width;
  const int height = roi_out->height;
  const int tc_mode = d->tc_mode;
  const int unbound_ab = d->unbound_ab;
  const int rgb_norm = d->rgb_norm;
  const float rgb_norm_exp = d->rgb_norm_exp;
  const gboolean histogram_needed = g && g->got_focus
      && pipe->type != DT_DEV_PIXELPIPE_PREVIEW
      && (tc_mode == DT_S_SCALE_MANUAL_RGB || tc_mode == DT_S_SCALE_MANUAL_LCH) ? TRUE : FALSE;
//printf("process - tc_mode %d, pipe_type %d, histo needed %s\n", tc_mode, pipe->type, histogram_needed ? "Yes" : "No");
//if (g) printf("process - focus %s\n", (g->got_focus)?"Yes":"No");

#ifdef _OPENMP
#pragma omp parallel for default(none) shared(d,g) schedule(static)
#endif
  for(int k = 0; k < height; k++)
  {
    float *in = ((float *)i) + (size_t)k * ch * width;
    float *out = ((float *)o) + (size_t)k * ch * width;

    for(int j = 0; j < width; j++, in += ch, out += ch)
    {
      if(tc_mode == DT_S_SCALE_MANUAL_LAB)
      {
        const float L_in = in[0] / 100.0f;
        out[0] = (L_in < xm[ch_L][0]) ? d->table[ch_L][CLAMP((int)(L_in * 0x10000ul), 0, 0xffff)]
                               : dt_iop_eval_exp(d->unbounded_coeffs[0], L_in);
        const float a_in = (in[1] + 128.0f) / 256.0f;
        const float b_in = (in[2] + 128.0f) / 256.0f;

        if(unbound_ab == 0)
        {
          // old style handling of a/b curves: only lut lookup with clamping
          out[1] = d->table[ch_a][CLAMP((int)(a_in * 0x10000ul), 0, 0xffff)];
          out[2] = d->table[ch_b][CLAMP((int)(b_in * 0x10000ul), 0, 0xffff)];
        }
        else
        {
          // new style handling of a/b curves: lut lookup with two-sided extrapolation;
          // mind the x-axis reversal for the left-handed side
          out[1] = (a_in > xm[ch_a][0])
                       ? dt_iop_eval_exp(d->unbounded_coeffs[ch_a], a_in)
                       : ((a_in < xm[ch_a][1]) ? dt_iop_eval_exp(d->unbounded_coeffs[ch_a] + 3, 1.0f - a_in)
                                         : d->table[ch_a][CLAMP((int)(a_in * 0x10000ul), 0, 0xffff)]);
          out[2] = (b_in > xm[ch_b][0])
                       ? dt_iop_eval_exp(d->unbounded_coeffs[ch_b], b_in)
                       : ((b_in < xm[ch_b][1]) ? dt_iop_eval_exp(d->unbounded_coeffs[ch_b] + 3, 1.0f - b_in)
                                         : d->table[ch_b][CLAMP((int)(b_in * 0x10000ul), 0, 0xffff)]);
        }
      }
      else if(tc_mode == DT_S_SCALE_MANUAL_LCH)
      {
        const float L_in = in[0] / 100.0f;
        out[0] = (L_in < xm[0][0]) ? d->table[ch_L][CLAMP((int)(L_in * 0x10000ul), 0, 0xffff)]
                    : dt_iop_eval_exp(d->unbounded_coeffs[0], L_in);
        float LCh[3] = {0, 0, 0};
        out[1] = in[1];
        out[2] = out[2];
        dt_Lab_2_LCH(in, LCh);
        const float chroma = LCh[1] / 182.019f;
        if (chroma > 0.0f)
        {
          LCh[1] = (chroma < xm[ch_C][0]) ? d->table[ch_C][CLAMP((int)(chroma * 0x10000ul), 0, 0xffff)]
              : dt_iop_eval_exp(d->unbounded_coeffs[ch_C], chroma);  // C(C)
          LCh[1] = (L_in < xm[ch_CL][0]) ? LCh[1] * d->table[ch_CL][CLAMP((int)(L_in * 0x10000ul), 0, 0xffff)] * 2.0f
              : LCh[1] * dt_iop_eval_exp(d->unbounded_coeffs[ch_CL], L_in) * 2.0f;  // C(L)
          out[0] = (LCh[1] < xm[ch_LC][0]) ? out[0] * d->table[ch_LC][CLAMP((int)(LCh[1] * 0x10000ul), 0, 0xffff)] * 2.0f
              : out[0] * dt_iop_eval_exp(d->unbounded_coeffs[ch_LC], LCh[1]) * 2.0f;  // L(C)
//          LCh[1] = LCh[1] * d->table[ch_Ch][CLAMP((int)(LCh[2] * 0x10000ul), 0, 0xffff)] * 2.0f;  // C(h)
          out[1] = in[1] * (LCh[1] / chroma);
          out[2] = in[2] * (LCh[1] / chroma);
        }
      }
      else if(tc_mode == DT_S_SCALE_AUTOMATIC_LAB)
      {
        const float L_in = in[0] / 100.0f;
        out[0] = (L_in < xm[0][0]) ? d->table[ch_L][CLAMP((int)(L_in * 0x10000ul), 0, 0xffff)]
                               : dt_iop_eval_exp(d->unbounded_coeffs[0], L_in);
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
      else if(tc_mode == DT_S_SCALE_AUTOMATIC_XYZ)
      {
        float XYZ[3];
        dt_Lab_to_XYZ(in, XYZ);
        for(int c=0; c<3; c++)
          XYZ[c] = (XYZ[c] < xm[ch_L][0]) ? d->table[ch_L][CLAMP((int)(XYZ[c] * 0x10000ul), 0, 0xffff)]
                                   : dt_iop_eval_exp(d->unbounded_coeffs[0], XYZ[c]);
        dt_XYZ_to_Lab(XYZ, out);
      }
      else if(tc_mode == DT_S_SCALE_AUTOMATIC_RGB)
      {
        float rgb[3] = {0, 0, 0};
        dt_Lab_to_prophotorgb(in, rgb);
        if (rgb_norm != 0)
        {
          float norm = dt_rgb_norm_vect( rgb, rgb_norm, rgb_norm_exp); // compute the norm == luminance estimator
          if (norm < 0) goto no_norm;
          const float rgb_ratios[3] = {rgb[0] / norm, rgb[1] / norm, rgb[2] / norm}; // these ratios are the actual colors, independent from the luminance
          norm = (norm < xm[ch_L][0]) ? d->table[ch_L][CLAMP((int)(norm * 0x10000ul), 0, 0xffff)]
                                     : dt_iop_eval_exp(d->unbounded_coeffs[0], norm); // compute the curve on the luminance
          for(int c=0; c<3; c++) rgb[c] = (norm * rgb_ratios[c]); // restore the colors from the original ratios and the new luminance
        }
        else
        {
no_norm:
          for(int c=0; c<3; c++)
            rgb[c] = (rgb[c] < xm[ch_L][0]) ? d->table[ch_L][CLAMP((int)(rgb[c] * 0x10000ul), 0, 0xffff)]
                                    : dt_iop_eval_exp(d->unbounded_coeffs[0], rgb[c]);
        }
        dt_prophotorgb_to_Lab(rgb, out);
      }
      else if (tc_mode == DT_S_SCALE_MANUAL_RGB)
      {
        float rgb[3] = {0, 0, 0};
        dt_Lab_to_prophotorgb(in, rgb);
        for(int c=0; c<3; c++)
        {
          rgb[c] = (rgb[c] < xm[ch_L][0]) ? d->table[ch_L][CLAMP((int)(rgb[c] * 0x10000ul), 0, 0xffff)]
                                   : dt_iop_eval_exp(d->unbounded_coeffs[0], rgb[c]);
          rgb[c] = (rgb[c] < xm[c+1][0]) ? d->table[c+1][CLAMP((int)(rgb[c] * 0x10000ul), 0, 0xffff)]
                                   : dt_iop_eval_exp(d->unbounded_coeffs[c+1], rgb[c]);
        }
        dt_prophotorgb_to_Lab(rgb, out);
      }
      out[3] = in[3];
    }
  }
  if (histogram_needed)
  {
    if (tc_mode == DT_S_SCALE_MANUAL_RGB)
      histogram_worker(i, height, width, &g->local_histogram[0], histogram_Lab);
    else if (tc_mode == DT_S_SCALE_MANUAL_LCH)
      histogram_worker(i, height, width, &g->local_histogram[0], histogram_LCh);
//uint32_t sum_histo = 0;
//for(int c=0; c < 4*DT_IOP_TONECURVE_BINS; c++)  sum_histo += g->local_histogram[c];
//printf("process - height %d, width %d, sum histo %d\n", height, width, sum_histo);
  }
}

static const struct
{
  const char *name;
  const char *maker;
  const char *model;
  int iso_min;
  float iso_max;
  struct dt_iop_tonecurve_params_t preset;
} preset_camera_curves[] = {
  // This is where you can paste the line provided by dt-curve-tool
  // Here is a valid example for you to compare
  // clang-format off
    // nikon d750 by Edouard Gomez
    {"Nikon D750", "NIKON CORPORATION", "NIKON D750", 0, FLT_MAX, {{{{0.000000, 0.000000}, {0.083508, 0.073677}, {0.212191, 0.274799}, {0.397095, 0.594035}, {0.495025, 0.714660}, {0.683565, 0.878550}, {0.854059, 0.950927}, {1.000000, 1.000000}, },{{0.000000, 0.000000}, {0.125000, 0.125000}, {0.250000, 0.250000}, {0.375000, 0.375000}, {0.500000, 0.500000}, {0.625000, 0.625000}, {0.750000, 0.750000}, {0.875000, 0.875000}, },{{0.000000, 0.000000}, {0.125000, 0.125000}, {0.250000, 0.250000}, {0.375000, 0.375000}, {0.500000, 0.500000}, {0.625000, 0.625000}, {0.750000, 0.750000}, {0.875000, 0.875000}, },}, {8, 8, 8}, {2, 2, 2}, 1, 0, 0}},
    // nikon d5100 contributed by Stefan Kauerauf
    {"NIKON D5100", "NIKON CORPORATION", "NIKON D5100", 0, FLT_MAX, {{{{0.000000, 0.000000}, {0.000957, 0.000176}, {0.002423, 0.000798}, {0.005893, 0.003685}, {0.013219, 0.006619}, {0.023372, 0.011954}, {0.037580, 0.017817}, {0.069695, 0.035353}, {0.077276, 0.040315}, {0.123707, 0.082707}, {0.145249, 0.112105}, {0.189168, 0.186135}, {0.219576, 0.243677}, {0.290201, 0.385251}, {0.428150, 0.613355}, {0.506199, 0.700256}, {0.622833, 0.805488}, {0.702763, 0.870959}, {0.935053, 0.990139}, {1.000000, 1.000000}, },{{0.000000, 0.000000}, {0.050000, 0.050000}, {0.100000, 0.100000}, {0.150000, 0.150000}, {0.200000, 0.200000}, {0.250000, 0.250000}, {0.300000, 0.300000}, {0.350000, 0.350000}, {0.400000, 0.400000}, {0.450000, 0.450000}, {0.500000, 0.500000}, {0.550000, 0.550000}, {0.600000, 0.600000}, {0.650000, 0.650000}, {0.700000, 0.700000}, {0.750000, 0.750000}, {0.800000, 0.800000}, {0.850000, 0.850000}, {0.900000, 0.900000}, {0.950000, 0.950000}, },{{0.000000, 0.000000}, {0.050000, 0.050000}, {0.100000, 0.100000}, {0.150000, 0.150000}, {0.200000, 0.200000}, {0.250000, 0.250000}, {0.300000, 0.300000}, {0.350000, 0.350000}, {0.400000, 0.400000}, {0.450000, 0.450000}, {0.500000, 0.500000}, {0.550000, 0.550000}, {0.600000, 0.600000}, {0.650000, 0.650000}, {0.700000, 0.700000}, {0.750000, 0.750000}, {0.800000, 0.800000}, {0.850000, 0.850000}, {0.900000, 0.900000}, {0.950000, 0.950000}, },}, {20, 20, 20}, {2, 2, 2}, 1, 0, 0}},
    // nikon d7000 by Edouard Gomez
    {"Nikon D7000", "NIKON CORPORATION", "NIKON D7000", 0, FLT_MAX, {{{{0.000000, 0.000000}, {0.110633, 0.111192}, {0.209771, 0.286963}, {0.355888, 0.561236}, {0.454987, 0.673098}, {0.769212, 0.920485}, {0.800468, 0.933428}, {1.000000, 1.000000}, },{{0.000000, 0.000000}, {0.125000, 0.125000}, {0.250000, 0.250000}, {0.375000, 0.375000}, {0.500000, 0.500000}, {0.625000, 0.625000}, {0.750000, 0.750000}, {0.875000, 0.875000}, },{{0.000000, 0.000000}, {0.125000, 0.125000}, {0.250000, 0.250000}, {0.375000, 0.375000}, {0.500000, 0.500000}, {0.625000, 0.625000}, {0.750000, 0.750000}, {0.875000, 0.875000}, },}, {8, 8, 8}, {2, 2, 2}, 1, 0, 0}},
    // nikon d7200 standard by Ralf Brown (firmware 1.00)
    {"Nikon D7200", "NIKON CORPORATION", "NIKON D7200", 0, FLT_MAX, {{{{0.000000, 0.000000}, {0.000618, 0.003286}, {0.001639, 0.003705}, {0.005227, 0.005101}, {0.013299, 0.011192}, {0.016048, 0.013130}, {0.037941, 0.027014}, {0.058195, 0.041339}, {0.086531, 0.069088}, {0.116679, 0.107283}, {0.155629, 0.159422}, {0.205477, 0.246265}, {0.225923, 0.287343}, {0.348056, 0.509104}, {0.360629, 0.534732}, {0.507562, 0.762089}, {0.606899, 0.865692}, {0.734828, 0.947468}, {0.895488, 0.992021}, {1.000000, 1.000000}, },{{0.000000, 0.000000}, {0.050000, 0.050000}, {0.100000, 0.100000}, {0.150000, 0.150000}, {0.200000, 0.200000}, {0.250000, 0.250000}, {0.300000, 0.300000}, {0.350000, 0.350000}, {0.400000, 0.400000}, {0.450000, 0.450000}, {0.500000, 0.500000}, {0.550000, 0.550000}, {0.600000, 0.600000}, {0.650000, 0.650000}, {0.700000, 0.700000}, {0.750000, 0.750000}, {0.800000, 0.800000}, {0.850000, 0.850000}, {0.900000, 0.900000}, {0.950000, 0.950000}, },{{0.000000, 0.000000}, {0.050000, 0.050000}, {0.100000, 0.100000}, {0.150000, 0.150000}, {0.200000, 0.200000}, {0.250000, 0.250000}, {0.300000, 0.300000}, {0.350000, 0.350000}, {0.400000, 0.400000}, {0.450000, 0.450000}, {0.500000, 0.500000}, {0.550000, 0.550000}, {0.600000, 0.600000}, {0.650000, 0.650000}, {0.700000, 0.700000}, {0.750000, 0.750000}, {0.800000, 0.800000}, {0.850000, 0.850000}, {0.900000, 0.900000}, {0.950000, 0.950000}, },}, {20, 20, 20}, {2, 2, 2}, 1, 0, 0}},
    // nikon d7500 by Anders Bennehag (firmware C 1.00, LD 2.016)
    {"NIKON D7500", "NIKON CORPORATION", "NIKON D7500", 0, FLT_MAX, {{{{0.000000, 0.000000}, {0.000421, 0.003412}, {0.003775, 0.004001}, {0.013762, 0.008704}, {0.016698, 0.010230}, {0.034965, 0.018732}, {0.087311, 0.049808}, {0.101389, 0.060789}, {0.166845, 0.145269}, {0.230944, 0.271288}, {0.333399, 0.502609}, {0.353207, 0.542549}, {0.550014, 0.819535}, {0.731749, 0.944033}, {0.783283, 0.960546}, {1.000000, 1.000000}, },{{0.000000, 0.000000}, {0.062500, 0.062500}, {0.125000, 0.125000}, {0.187500, 0.187500}, {0.250000, 0.250000}, {0.312500, 0.312500}, {0.375000, 0.375000}, {0.437500, 0.437500}, {0.500000, 0.500000}, {0.562500, 0.562500}, {0.625000, 0.625000}, {0.687500, 0.687500}, {0.750000, 0.750000}, {0.812500, 0.812500}, {0.875000, 0.875000}, {0.937500, 0.937500}, },{{0.000000, 0.000000}, {0.062500, 0.062500}, {0.125000, 0.125000}, {0.187500, 0.187500}, {0.250000, 0.250000}, {0.312500, 0.312500}, {0.375000, 0.375000}, {0.437500, 0.437500}, {0.500000, 0.500000}, {0.562500, 0.562500}, {0.625000, 0.625000}, {0.687500, 0.687500}, {0.750000, 0.750000}, {0.812500, 0.812500}, {0.875000, 0.875000}, {0.937500, 0.937500}, },}, {16, 16, 16}, {2, 2, 2}, 1, 0, 0}},
    // nikon d90 by Edouard Gomez
    {"Nikon D90", "NIKON CORPORATION", "NIKON D90", 0, FLT_MAX, {{{{0.000000, 0.000000}, {0.002915, 0.006453}, {0.023324, 0.021601}, {0.078717, 0.074963}, {0.186589, 0.242230}, {0.364432, 0.544956}, {0.629738, 0.814127}, {1.000000, 1.000000}, },{{0.000000, 0.000000}, {0.125000, 0.125000}, {0.250000, 0.250000}, {0.375000, 0.375000}, {0.500000, 0.500000}, {0.625000, 0.625000}, {0.750000, 0.750000}, {0.875000, 0.875000}, },{{0.000000, 0.000000}, {0.125000, 0.125000}, {0.250000, 0.250000}, {0.375000, 0.375000}, {0.500000, 0.500000}, {0.625000, 0.625000}, {0.750000, 0.750000}, {0.875000, 0.875000}, },}, {8, 8, 8}, {2, 2, 2}, 1, 0, 0}},
    // Olympus OM-D E-M10 II by Lukas Schrangl
    {"Olympus OM-D E-M10 II", "OLYMPUS CORPORATION    ", "E-M10MarkII     ", 0, FLT_MAX, {{{{0.000000, 0.000000}, {0.004036, 0.000809}, {0.015047, 0.009425}, {0.051948, 0.042053}, {0.071777, 0.066635}, {0.090018, 0.086722}, {0.110197, 0.118773}, {0.145817, 0.171861}, {0.207476, 0.278652}, {0.266832, 0.402823}, {0.428061, 0.696319}, {0.559728, 0.847113}, {0.943576, 0.993482}, {1.000000, 1.000000}, },{{0.000000, 0.000000}, {0.071429, 0.071429}, {0.142857, 0.142857}, {0.214286, 0.214286}, {0.285714, 0.285714}, {0.357143, 0.357143}, {0.428571, 0.428571}, {0.500000, 0.500000}, {0.571429, 0.571429}, {0.642857, 0.642857}, {0.714286, 0.714286}, {0.785714, 0.785714}, {0.857143, 0.857143}, {0.928571, 0.928571}, },{{0.000000, 0.000000}, {0.071429, 0.071429}, {0.142857, 0.142857}, {0.214286, 0.214286}, {0.285714, 0.285714}, {0.357143, 0.357143}, {0.428571, 0.428571}, {0.500000, 0.500000}, {0.571429, 0.571429}, {0.642857, 0.642857}, {0.714286, 0.714286}, {0.785714, 0.785714}, {0.857143, 0.857143}, {0.928571, 0.928571}, },}, {14, 14, 14}, {2, 2, 2}, 1, 0, 0}},
  // clang-format on
};

void init_presets(dt_iop_module_so_t *self)
{
  dt_iop_tonecurve_params_t p;
  memset(&p, 0, sizeof(p));
  p.tonecurve_nodes[ch_L] = 6;
  p.tonecurve_nodes[ch_a] = 7;
  p.tonecurve_nodes[ch_b] = 7;
  p.tonecurve_nodes[3] = 2;
  p.tonecurve_type[ch_L] = CUBIC_SPLINE;
  p.tonecurve_type[ch_a] = CUBIC_SPLINE;
  p.tonecurve_type[ch_b] = CUBIC_SPLINE;
  p.tonecurve_type[3] = CUBIC_SPLINE;
  p.tonecurve_preset = 0;
  p.tonecurve_tc_mode = DT_S_SCALE_AUTOMATIC_RGB;
  p.tonecurve_unbound_ab = 1;
  p.rgb_norm = 0;
  p.rgb_norm_exp = 2.0f;

  float linear_ab[7] = { 0.0, 0.08, 0.3, 0.5, 0.7, 0.92, 1.0 };

  // linear a, b curves for presets
  for(int k = 0; k < 7; k++) p.tonecurve[ch_a][k].x = linear_ab[k];
  for(int k = 0; k < 7; k++) p.tonecurve[ch_a][k].y = linear_ab[k];
  for(int k = 0; k < 7; k++) p.tonecurve[ch_b][k].x = linear_ab[k];
  for(int k = 0; k < 7; k++) p.tonecurve[ch_b][k].y = linear_ab[k];

  // forth channel
  p.tonecurve[3][0].x = p.tonecurve[3][0].y = 0.0f;
  p.tonecurve[3][1].x = p.tonecurve[3][1].y = 1.0f;

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
  dt_gui_presets_add_generic(_("contrast compression"), self->op, self->version(), &p, sizeof(p), 1);

  p.tonecurve_nodes[ch_L] = 7;
  float linear_L[7] = { 0.0, 0.08, 0.17, 0.50, 0.83, 0.92, 1.0 };

  // Linear - no contrast
  for(int k = 0; k < 7; k++) p.tonecurve[ch_L][k].x = linear_L[k];
  for(int k = 0; k < 7; k++) p.tonecurve[ch_L][k].y = linear_L[k];
  dt_gui_presets_add_generic(_("gamma 1.0 (linear)"), self->op, self->version(), &p, sizeof(p), 1);

  // Linear contrast
  for(int k = 0; k < 7; k++) p.tonecurve[ch_L][k].x = linear_L[k];
  for(int k = 0; k < 7; k++) p.tonecurve[ch_L][k].y = linear_L[k];
  p.tonecurve[ch_L][1].y -= 0.020;
  p.tonecurve[ch_L][2].y -= 0.030;
  p.tonecurve[ch_L][4].y += 0.030;
  p.tonecurve[ch_L][5].y += 0.020;
  dt_gui_presets_add_generic(_("contrast - med (linear)"), self->op, self->version(), &p, sizeof(p), 1);

  for(int k = 0; k < 7; k++) p.tonecurve[ch_L][k].x = linear_L[k];
  for(int k = 0; k < 7; k++) p.tonecurve[ch_L][k].y = linear_L[k];
  p.tonecurve[ch_L][1].y -= 0.040;
  p.tonecurve[ch_L][2].y -= 0.060;
  p.tonecurve[ch_L][4].y += 0.060;
  p.tonecurve[ch_L][5].y += 0.040;
  dt_gui_presets_add_generic(_("contrast - high (linear)"), self->op, self->version(), &p, sizeof(p), 1);

  // Gamma contrast
  for(int k = 0; k < 7; k++) p.tonecurve[ch_L][k].x = linear_L[k];
  for(int k = 0; k < 7; k++) p.tonecurve[ch_L][k].y = linear_L[k];
  p.tonecurve[ch_L][1].y -= 0.020;
  p.tonecurve[ch_L][2].y -= 0.030;
  p.tonecurve[ch_L][4].y += 0.030;
  p.tonecurve[ch_L][5].y += 0.020;
  for(int k = 1; k < 6; k++) p.tonecurve[ch_L][k].x = powf(p.tonecurve[ch_L][k].x, 2.2f);
  for(int k = 1; k < 6; k++) p.tonecurve[ch_L][k].y = powf(p.tonecurve[ch_L][k].y, 2.2f);
  dt_gui_presets_add_generic(_("contrast - med (gamma 2.2)"), self->op, self->version(), &p, sizeof(p), 1);

  for(int k = 0; k < 7; k++) p.tonecurve[ch_L][k].x = linear_L[k];
  for(int k = 0; k < 7; k++) p.tonecurve[ch_L][k].y = linear_L[k];
  p.tonecurve[ch_L][1].y -= 0.040;
  p.tonecurve[ch_L][2].y -= 0.060;
  p.tonecurve[ch_L][4].y += 0.060;
  p.tonecurve[ch_L][5].y += 0.040;
  for(int k = 1; k < 6; k++) p.tonecurve[ch_L][k].x = powf(p.tonecurve[ch_L][k].x, 2.2f);
  for(int k = 1; k < 6; k++) p.tonecurve[ch_L][k].y = powf(p.tonecurve[ch_L][k].y, 2.2f);
  dt_gui_presets_add_generic(_("contrast - high (gamma 2.2)"), self->op, self->version(), &p, sizeof(p), 1);

  /** for pure power-like functions, we need more nodes close to the bounds**/
  p.tonecurve_type[ch_L] = MONOTONE_HERMITE;

  for(int k = 0; k < 7; k++) p.tonecurve[ch_L][k].x = linear_L[k];
  for(int k = 0; k < 7; k++) p.tonecurve[ch_L][k].y = linear_L[k];

  // Gamma 2.0 - no contrast
  for(int k = 1; k < 6; k++) p.tonecurve[ch_L][k].y = powf(linear_L[k], 2.0f);
  dt_gui_presets_add_generic(_("gamma 2.0"), self->op, self->version(), &p, sizeof(p), 1);

  // Gamma 0.5 - no contrast
  for(int k = 1; k < 6; k++) p.tonecurve[ch_L][k].y = powf(linear_L[k], 0.5f);
  dt_gui_presets_add_generic(_("gamma 0.5"), self->op, self->version(), &p, sizeof(p), 1);

  // Log2 - no contrast
  for(int k = 1; k < 6; k++) p.tonecurve[ch_L][k].y = logf(linear_L[k] + 1.0f) / logf(2.0f);
  dt_gui_presets_add_generic(_("logarithm (base 2)"), self->op, self->version(), &p, sizeof(p), 1);

  // Exp2 - no contrast
  for(int k = 1; k < 6; k++) p.tonecurve[ch_L][k].y = powf(2.0f, linear_L[k]) - 1.0f;
  dt_gui_presets_add_generic(_("exponential (base 2)"), self->op, self->version(), &p, sizeof(p), 1);

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

  const int tc_mode = p->tonecurve_tc_mode;
  const int nb_ch = mode_curves[tc_mode].nb_ch;

  if(pipe->type == DT_DEV_PIXELPIPE_PREVIEW)
    piece->request_histogram |= (DT_REQUEST_ON);
  else
    piece->request_histogram &= ~(DT_REQUEST_ON);

  for(int ch = 0; ch < nb_ch; ch++)
  {
    const int curve_detail_i = mode_curves[tc_mode].curve_detail_i[ch];
    const int lut_factor = curve_detail[curve_detail_i].c_lut_factor;
    const int extrapolation = curve_detail[curve_detail_i].c_extrapolation;
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

    if (extrapolation == 1)
    {
      if (lut_factor != 1.0f)
        for(int k = 0; k < 0x10000; k++) d->table[ch][k] *= lut_factor;
    }
    else if (extrapolation == 2)
    {
      if (lut_factor != 1.0f)
        for(int k = 0; k < 0x10000; k++) d->table[ch][k] = d->table[ch][k] * lut_factor - lut_factor / 2.0f;
    }
  }

  if(p->tonecurve_tc_mode == DT_S_SCALE_AUTOMATIC_XYZ)
  {
    // derive curve for XYZ:
    for(int k=0;k<0x10000;k++)
    {
      float XYZ[3] = {k/(float)0x10000, k/(float)0x10000, k/(float)0x10000};
      float Lab[3] = {0.0};
      dt_XYZ_to_Lab(XYZ, Lab);
      Lab[0] = d->table[ch_L][CLAMP((int)(Lab[0]/100.0f * 0x10000), 0, 0xffff)];
      dt_Lab_to_XYZ(Lab, XYZ);
      d->table[ch_L][k] = XYZ[1]; // now mapping Y_in to Y_out
    }
  }
  else if(p->tonecurve_tc_mode == DT_S_SCALE_AUTOMATIC_RGB)
  {
    // derive curve for rgb:
    for(int k=0;k<0x10000;k++)
    {
      float rgb[3] = {k/(float)0x10000, k/(float)0x10000, k/(float)0x10000};
      float Lab[3] = {0.0};
      dt_prophotorgb_to_Lab(rgb, Lab);
      Lab[0] = d->table[ch_L][CLAMP((int)(Lab[0]/100.0f * 0x10000), 0, 0xffff)];
      dt_Lab_to_prophotorgb(Lab, rgb);
      d->table[ch_L][k] = rgb[1]; // now mapping G_in to G_out
    }
  }
  else if(p->tonecurve_tc_mode == DT_S_SCALE_MANUAL_RGB)
  {
    // derive curve for lrgb:
    for (int ch=0;ch<4;ch++)
    {
      for(int k=0;k<0x10000;k++)
      {
        float rgb[3] = {k/(float)0x10000, k/(float)0x10000, k/(float)0x10000};
        float Lab[3] = {0.0};
        dt_prophotorgb_to_Lab(rgb, Lab);
        Lab[0] = d->table[ch][CLAMP((int)(Lab[0]/100.0f * 0x10000), 0, 0xffff)] * 100.0f;
        dt_Lab_to_prophotorgb(Lab, rgb);
        d->table[ch][k] = rgb[1]; // now mapping L, R, G, B_in to L, R, G, B_out
      }
    }
  }

  d->tc_mode = p->tonecurve_tc_mode;
  d->unbound_ab = p->tonecurve_unbound_ab;
  d->rgb_norm = p->rgb_norm;
  d->rgb_norm_exp = p->rgb_norm_exp;

  for(int ch = 0; ch < nb_ch; ch++)
  {
    const int curve_detail_i = mode_curves[tc_mode].curve_detail_i[ch];
    const int extrapolation = curve_detail[curve_detail_i].c_extrapolation;

    if (extrapolation == 1)
    {
      // extrapolation for single curve (right hand side only):
      const float xm_L = p->tonecurve[ch][p->tonecurve_nodes[ch] - 1].x;
      const float x_L[4] = { 0.7f * xm_L, 0.8f * xm_L, 0.9f * xm_L, 1.0f * xm_L };
      const float y_L[4] = { d->table[ch][CLAMP((int)(x_L[0] * 0x10000ul), 0, 0xffff)],
                             d->table[ch][CLAMP((int)(x_L[1] * 0x10000ul), 0, 0xffff)],
                             d->table[ch][CLAMP((int)(x_L[2] * 0x10000ul), 0, 0xffff)],
                             d->table[ch][CLAMP((int)(x_L[3] * 0x10000ul), 0, 0xffff)] };
      dt_iop_estimate_exp(x_L, y_L, 4, d->unbounded_coeffs[ch]);
    }
    else if (extrapolation == 2)
    {
      // extrapolation for double curve right side:
      const float xm_ar = p->tonecurve[ch][p->tonecurve_nodes[ch] - 1].x;
      const float x_ar[4] = { 0.7f * xm_ar, 0.8f * xm_ar, 0.9f * xm_ar, 1.0f * xm_ar };
      const float y_ar[4] = { d->table[ch][CLAMP((int)(x_ar[0] * 0x10000ul), 0, 0xffff)],
                              d->table[ch][CLAMP((int)(x_ar[1] * 0x10000ul), 0, 0xffff)],
                              d->table[ch][CLAMP((int)(x_ar[2] * 0x10000ul), 0, 0xffff)],
                              d->table[ch][CLAMP((int)(x_ar[3] * 0x10000ul), 0, 0xffff)] };
      dt_iop_estimate_exp(x_ar, y_ar, 4, d->unbounded_coeffs[ch]);
      // extrapolation for double curve left side (we need to mirror the x-axis):
      const float xm_al = 1.0f - p->tonecurve[ch][0].x;
      const float x_al[4] = { 0.7f * xm_al, 0.8f * xm_al, 0.9f * xm_al, 1.0f * xm_al };
      const float y_al[4] = { d->table[ch][CLAMP((int)((1.0f - x_al[0]) * 0x10000ul), 0, 0xffff)],
                              d->table[ch][CLAMP((int)((1.0f - x_al[1]) * 0x10000ul), 0, 0xffff)],
                              d->table[ch][CLAMP((int)((1.0f - x_al[2]) * 0x10000ul), 0, 0xffff)],
                              d->table[ch][CLAMP((int)((1.0f - x_al[3]) * 0x10000ul), 0, 0xffff)] };
      dt_iop_estimate_exp(x_al, y_al, 4, d->unbounded_coeffs[ch] + 3);
    }
  }
piece->process_cl_ready = 1;
}
/*
static float eval_grey(float x)
{
  // estimate the log base to remap the grey x to 0.5
  return x;
}*/

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  // create part of the pixelpipe
  dt_iop_tonecurve_data_t *d = (dt_iop_tonecurve_data_t *)malloc(sizeof(dt_iop_tonecurve_data_t));
  dt_iop_tonecurve_params_t *default_params = (dt_iop_tonecurve_params_t *)self->default_params;
  piece->data = (void *)d;
  const int tc_mode = d->tc_mode = DT_S_SCALE_AUTOMATIC_LAB;
  d->unbound_ab = 1;
  for(int ch = 0; ch < DT_IOP_TONECURVE_MAX_CH; ch++)
  {
    d->curve[ch] = dt_draw_curve_new(0.0, 1.0, default_params->tonecurve_type[ch]);
    d->curve_nodes[ch] = default_params->tonecurve_nodes[ch];
    d->curve_type[ch] = default_params->tonecurve_type[ch];
    for(int k = 0; k < default_params->tonecurve_nodes[ch]; k++)
      (void)dt_draw_curve_add_point(d->curve[ch], default_params->tonecurve[ch][k].x,
                                    default_params->tonecurve[ch][k].y);
  }
  const int nb_ch = mode_curves[tc_mode].nb_ch;
  for(int ch = 0; ch < nb_ch; ch++)
  {
    const int curve_detail_i = mode_curves[tc_mode].curve_detail_i[ch];
    const int lut_factor = curve_detail[curve_detail_i].c_lut_factor;
    const int extrapolation = curve_detail[curve_detail_i].c_extrapolation;
    if (curve_detail[curve_detail_i].chx == curve_detail[curve_detail_i].chy)
    {
      if (extrapolation == 1)
        for(int k = 0; k < 0x10000; k++) d->table[ch][k] = lut_factor * k / 0x10000;          // identity for x'=f(x)
      else if (extrapolation == 2)
        for(int k = 0; k < 0x10000; k++) d->table[ch][k] = lut_factor * k / 0x10000 - lut_factor / 2.0f; // identity for x'=f(x)
    }
    else
      for(int k = 0; k < 0x10000; k++) d->table[ch][k] = 0.5f;  // identity for x'=f(y)
  }
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  // clean up everything again.
  dt_iop_tonecurve_data_t *d = (dt_iop_tonecurve_data_t *)(piece->data);
  for(int ch = 0; ch < DT_IOP_TONECURVE_MAX_CH; ch++) dt_draw_curve_destroy(d->curve[ch]);
  free(piece->data);
  piece->data = NULL;
}

void gui_reset(struct dt_iop_module_t *self)
{
  printf("gui_reset\n");
  dt_iop_tonecurve_gui_data_t *g = (dt_iop_tonecurve_gui_data_t *)self->gui_data;
  dt_iop_tonecurve_params_t *p = (dt_iop_tonecurve_params_t *)self->params;
  dt_bauhaus_combobox_set(g->interpolator, p->tonecurve_type[ch_L]);
  self->request_color_pick = DT_REQUEST_COLORPICK_OFF;
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->colorpicker), 0);

  dt_bauhaus_combobox_set(g->scale, 0); // linear
  for (int ch = 0; ch < DT_IOP_TONECURVE_MAX_CH; ch++)
  {
    g->loglogscale[ch] = 0;
    g->scale_mode[ch] = linxliny;
  }
  g->channel = (tonecurve_channel_t)ch_L;
  gtk_widget_queue_draw(self->widget);
}

void tabs_update(struct dt_iop_module_t *self, int reset_nodes)
{
  printf("tab_update\n");
  dt_iop_tonecurve_gui_data_t *g = (dt_iop_tonecurve_gui_data_t *)self->gui_data;
  dt_iop_tonecurve_params_t *p = (dt_iop_tonecurve_params_t *)self->params;
  dt_iop_tonecurve_params_t *d = (dt_iop_tonecurve_params_t *)self->default_params;

  const int nb_ch = mode_curves[p->tonecurve_tc_mode].nb_ch;
  char* tab_label[DT_IOP_TONECURVE_MAX_CH];
  char* tab_tooltip[DT_IOP_TONECURVE_MAX_CH];
  tab_label[0] = _("  L  ");
  tab_tooltip[0] = _("tonecurve for L channel");

  switch(p->tonecurve_tc_mode)
  {
    case DT_S_SCALE_AUTOMATIC_LAB:
    {
      gtk_widget_set_visible(g->rgb_norm, FALSE);
      gtk_widget_set_visible(g->rgb_norm_exp, FALSE);
      break;
    }
    case DT_S_SCALE_MANUAL_LAB:
    {
      tab_label[1] = _("  a  ");
      tab_tooltip[1] = _("tonecurve for a channel");
      tab_label[2] = _("  b  ");
      tab_tooltip[2] = _("tonecurve for b channel");
      gtk_widget_set_visible(g->rgb_norm, FALSE);
      gtk_widget_set_visible(g->rgb_norm_exp, FALSE);
      break;
    }
    case DT_S_SCALE_AUTOMATIC_XYZ:
    {
      gtk_widget_set_visible(g->rgb_norm, FALSE);
      gtk_widget_set_visible(g->rgb_norm_exp, FALSE);
      break;
    }
    case DT_S_SCALE_AUTOMATIC_RGB:
    {
      gtk_widget_set_visible(g->rgb_norm, TRUE);
      if (dt_bauhaus_combobox_get(g->rgb_norm) == DT_RGB_NORM_LP)
        gtk_widget_set_visible(g->rgb_norm_exp, TRUE);
      else
        gtk_widget_set_visible(g->rgb_norm_exp, FALSE);
      break;
    }
    case DT_S_SCALE_MANUAL_RGB:
    {
      tab_label[1] = _("  R  ");
      tab_tooltip[1] = _("tonecurve for R channel");
      tab_label[2] = _("  G  ");
      tab_tooltip[2] = _("tonecurve for G channel");
      tab_label[3] = _("  B  ");
      tab_tooltip[3] = _("tonecurve for B channel");
      gtk_widget_set_visible(g->rgb_norm, FALSE);
      gtk_widget_set_visible(g->rgb_norm_exp, FALSE);
      break;
    }
    case DT_S_SCALE_MANUAL_LCH:
    {
      tab_label[1] = _(" C(C) ");
      tab_tooltip[1] = _("tonecurve for C(C)");
      tab_label[2] = _(" C(L) ");
      tab_tooltip[2] = _("tonecurve for C(L) - histogram(L)");
      tab_label[3] = _(" L(C) ");
      tab_tooltip[3] = _("tonecurve for L(C) - histogram(C)");
//      tab_label[2] = _(" C(h) ");
//      tab_tooltip[2] = _("tonecurve for C(h) - histogram(h)");
      gtk_widget_set_visible(g->rgb_norm, FALSE);
      gtk_widget_set_visible(g->rgb_norm_exp, FALSE);
      break;
    }
  }
  for(int ch=1; ch<nb_ch; ch++)
  {
    const int curve_detail_i = mode_curves[p->tonecurve_tc_mode].curve_detail_i[ch];
    // set default nodes
    const int nb_nodes = curve_detail[curve_detail_i].c_nb_nodes;
    for (int j=0; j<nb_nodes; j++)
    {
      d->tonecurve[ch][j].x = curve_detail[curve_detail_i].c_nodes[j].x;
      d->tonecurve[ch][j].y = curve_detail[curve_detail_i].c_nodes[j].y;
      if (reset_nodes)
      {
        p->tonecurve[ch][j].x = curve_detail[curve_detail_i].c_nodes[j].x;
        p->tonecurve[ch][j].y = curve_detail[curve_detail_i].c_nodes[j].y;
      }
    }
    d->tonecurve_nodes[ch] = nb_nodes;
    if (reset_nodes) p->tonecurve_nodes[ch] = nb_nodes;
  }
  if (nb_ch>1)
  {
    for(int ch=0; ch < nb_ch; ch++)
    {
      GtkWidget *tabid = gtk_notebook_get_nth_page(g->channel_tabs, ch);
      gtk_notebook_set_tab_label_text(g->channel_tabs, tabid, tab_label[ch]);
      gtk_widget_set_tooltip_text(gtk_notebook_get_tab_label(g->channel_tabs, tabid), tab_tooltip[ch]);
      gtk_widget_show(tabid);
    }
    for(int ch=nb_ch; ch < DT_IOP_TONECURVE_MAX_CH; ch++)
    {
      GtkWidget *tabid = gtk_notebook_get_nth_page(g->channel_tabs, ch);
      gtk_widget_hide(tabid);
    }
    gtk_notebook_set_show_tabs(g->channel_tabs, TRUE);
  }
  else
  {
    gtk_notebook_set_show_tabs(g->channel_tabs, FALSE);
  }
  if(curve_detail[mode_curves[p->tonecurve_tc_mode].curve_detail_i[g->channel]].c_log_enabled)
  {
    dt_bauhaus_combobox_set(g->scale, g->scale_mode[g->channel]);
    gtk_widget_set_visible(g->scale, TRUE);
    if (g->scale_mode[g->channel] != linxliny)
    {
      gtk_widget_set_visible(g->logbase, TRUE);
      dt_bauhaus_slider_set(g->logbase, g->loglogscale[g->channel]);
    }
    else gtk_widget_set_visible(g->logbase, FALSE);
  }
  else
  {
    gtk_widget_set_visible(g->scale, FALSE);
    gtk_widget_set_visible(g->logbase, FALSE);
  }
}

void gui_update(struct dt_iop_module_t *self)
{
  printf("gui_update\n");
  dt_iop_tonecurve_gui_data_t *g = (dt_iop_tonecurve_gui_data_t *)self->gui_data;
  dt_iop_tonecurve_params_t *p = (dt_iop_tonecurve_params_t *)self->params;

  switch(p->tonecurve_tc_mode)
  {
    case DT_S_SCALE_MANUAL_LAB:
    {
      dt_bauhaus_combobox_set(g->tc_mode, 1);
      break;
    }
    case DT_S_SCALE_AUTOMATIC_LAB:
    {
      dt_bauhaus_combobox_set(g->tc_mode, 0);
      g->channel = (tonecurve_channel_t)ch_L;
      break;
    }
    case DT_S_SCALE_AUTOMATIC_XYZ:
    {
      dt_bauhaus_combobox_set(g->tc_mode, 2);
      g->channel = (tonecurve_channel_t)ch_L;
      break;
    }
    case DT_S_SCALE_AUTOMATIC_RGB:
    {
      dt_bauhaus_combobox_set(g->tc_mode, 3);
      g->channel = (tonecurve_channel_t)ch_L;
      break;
    }
    case DT_S_SCALE_MANUAL_RGB:
    {
      dt_bauhaus_combobox_set(g->tc_mode, 4);
      break;
    }
    case DT_S_SCALE_MANUAL_LCH:
    {
      dt_bauhaus_combobox_set(g->tc_mode, 5);
      break;
    }
  }
  dt_bauhaus_combobox_set(g->rgb_norm, p->rgb_norm);
  dt_bauhaus_combobox_set(g->rgb_norm_exp, p->rgb_norm_exp);
  tabs_update(self, FALSE);

  dt_bauhaus_combobox_set(g->interpolator, p->tonecurve_type[ch_L]);

  gtk_widget_queue_draw(GTK_WIDGET(g->area));

  // that's all, gui curve is read directly from params during expose event.
  gtk_widget_queue_draw(self->widget);

  if (self->request_color_pick == DT_REQUEST_COLORPICK_OFF)
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->colorpicker), 0);
}

void init(dt_iop_module_t *module)
{
setvbuf(stdout, NULL, _IONBF, 0);
printf("init\n");
  module->params = calloc(1, sizeof(dt_iop_tonecurve_params_t));
  module->default_params = calloc(1, sizeof(dt_iop_tonecurve_params_t));
  module->default_enabled = 0;
  module->request_histogram |= (DT_REQUEST_ON);
  module->params_size = sizeof(dt_iop_tonecurve_params_t);
  module->gui_data = NULL;
  dt_iop_tonecurve_params_t tmp = (dt_iop_tonecurve_params_t){
    { { { 0.0, 0.0 }, { 1.0, 1.0 } },
      { { 0.0, 0.0 }, { 1.0, 1.0 } },
      { { 0.0, 0.0 }, { 1.0, 1.0 } },
      { { 0.0, 0.0 }, { 1.0, 1.0 } } },
    { 2, 2, 2, 2 }, // number of nodes per curve
    // { CATMULL_ROM, CATMULL_ROM, CATMULL_ROM},  // curve types
    { MONOTONE_HERMITE, MONOTONE_HERMITE, MONOTONE_HERMITE, MONOTONE_HERMITE },
    // { CUBIC_SPLINE, CUBIC_SPLINE, CUBIC_SPLINE},
    DT_S_SCALE_AUTOMATIC_RGB, // tc_mode
    0,
    1, // unbound_ab
    0, //rgb_norm
    2, //rgb_norm_exp
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
  free(module->params);
  module->params = NULL;
}

static void scale_callback(GtkWidget *widget, dt_iop_module_t *self)
{
  printf("scale_callback\n");
  if(darktable.gui->reset) return;
//  dt_iop_tonecurve_params_t *p = (dt_iop_tonecurve_params_t *)self->params;
  dt_iop_tonecurve_gui_data_t *g = (dt_iop_tonecurve_gui_data_t *)self->gui_data;
  const int ch = g->channel;
  switch(dt_bauhaus_combobox_get(widget))
  {
    case 0:
    {
      // linear
      g->loglogscale[ch] = 0;
      g->scale_mode[ch] = linxliny;
      gtk_widget_set_visible(g->logbase, FALSE);
      break;
    }
    case 1:
    {
      // log log
//      g->loglogscale = eval_grey(dt_bauhaus_slider_get(g->logbase));
//      g->loglogscale[ch] = dt_bauhaus_slider_get(g->logbase);
      g->scale_mode[ch] = logxlogy;
      gtk_widget_set_visible(g->logbase, TRUE);
      dt_bauhaus_slider_set(g->logbase, g->loglogscale[ch]);
      break;
    }
    case 2:
    {
      // x: log, y: linear
//      g->loglogscale = eval_grey(dt_bauhaus_slider_get(g->logbase));
//      g->loglogscale[ch] = dt_bauhaus_slider_get(g->logbase);
      g->scale_mode[ch] = logxliny;
      gtk_widget_set_visible(g->logbase, TRUE);
      dt_bauhaus_slider_set(g->logbase, g->loglogscale[ch]);
      break;
    }
    case 3:
    {
      // x: linear, y: log
//      g->loglogscale = eval_grey(dt_bauhaus_slider_get(g->logbase));
//      g->loglogscale[ch] = dt_bauhaus_slider_get(g->logbase);
      g->scale_mode[ch] = linxlogy;
      gtk_widget_set_visible(g->logbase, TRUE);
      dt_bauhaus_slider_set(g->logbase, g->loglogscale[ch]);
      break;
    }
  }
/*  if(g->scale_mode[ch] != linxliny && curve_detail[mode_curves[p->tonecurve_tc_mode].curve_detail_i[ch]].c_log_enabled)
  {
    gtk_widget_set_visible(g->logbase, TRUE);
    dt_bauhaus_slider_set(g->logbase, g->loglogscale[ch]);
  }
  else
    gtk_widget_set_visible(g->logbase, FALSE); */
  gtk_widget_queue_draw(GTK_WIDGET(g->area));
}

static void rgb_norm_callback(GtkWidget *widget, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_tonecurve_gui_data_t *g = (dt_iop_tonecurve_gui_data_t *)self->gui_data;
  dt_iop_tonecurve_params_t *p = (dt_iop_tonecurve_params_t *)self->params;
  p->rgb_norm = dt_bauhaus_combobox_get(widget);
  if (p->rgb_norm == DT_RGB_NORM_LP)
    gtk_widget_set_visible(g->rgb_norm_exp, TRUE);
  else
    gtk_widget_set_visible(g->rgb_norm_exp, FALSE);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
  gtk_widget_queue_draw(self->widget);
}

static void rgb_norm_exp_callback(GtkWidget *slider, dt_iop_module_t *self)
{
  if(self->dt->gui->reset) return;
  dt_iop_tonecurve_gui_data_t *g = (dt_iop_tonecurve_gui_data_t *)self->gui_data;
  dt_iop_tonecurve_params_t *p = (dt_iop_tonecurve_params_t *)self->params;
  if (dt_bauhaus_combobox_get(g->rgb_norm) == DT_RGB_NORM_LP)
  {
    p->rgb_norm_exp = dt_bauhaus_slider_get(slider);
    dt_dev_add_history_item(darktable.develop, self, TRUE);
    gtk_widget_queue_draw(self->widget);
  }
}

static void logbase_callback(GtkWidget *slider, dt_iop_module_t *self)
{
  printf("logbase_callback\n");
  if(self->dt->gui->reset) return;
  dt_iop_tonecurve_gui_data_t *g = (dt_iop_tonecurve_gui_data_t *)self->gui_data;

  if (dt_bauhaus_combobox_get(g->scale) != 0)
  {
//    g->loglogscale = eval_grey(dt_bauhaus_slider_get(g->logbase));
    g->loglogscale[g->channel] = dt_bauhaus_slider_get(g->logbase);
    gtk_widget_queue_draw(GTK_WIDGET(g->area));
  }
}

static void tc_mode_callback(GtkWidget *widget, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_tonecurve_gui_data_t *g = (dt_iop_tonecurve_gui_data_t *)self->gui_data;
  dt_iop_tonecurve_params_t *p = (dt_iop_tonecurve_params_t *)self->params;
  const int combo = dt_bauhaus_combobox_get(widget);

  g->channel = (tonecurve_channel_t)ch_L;
  gtk_notebook_set_current_page(GTK_NOTEBOOK(g->channel_tabs), ch_L);

  switch(combo)
  {
    case 0:
      p->tonecurve_tc_mode = DT_S_SCALE_AUTOMATIC_LAB;
      break;
    case 1:
      p->tonecurve_tc_mode = DT_S_SCALE_MANUAL_LAB;
      break;
    case 2:
      p->tonecurve_tc_mode = DT_S_SCALE_AUTOMATIC_XYZ;
      break;
    case 3:
        p->tonecurve_tc_mode = DT_S_SCALE_AUTOMATIC_RGB;
        break;
    case 4:
      p->tonecurve_tc_mode = DT_S_SCALE_MANUAL_RGB;
      break;
    case 5:
      p->tonecurve_tc_mode = DT_S_SCALE_MANUAL_LCH;
      break;
  }
  tabs_update(self, TRUE);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
  gtk_widget_queue_draw(self->widget);
}

static void interpolator_callback(GtkWidget *widget, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_tonecurve_params_t *p = (dt_iop_tonecurve_params_t *)self->params;
  dt_iop_tonecurve_gui_data_t *g = (dt_iop_tonecurve_gui_data_t *)self->gui_data;
  const int combo = dt_bauhaus_combobox_get(widget);
  for(int ch=0; ch<DT_IOP_TONECURVE_MAX_CH; ch++)
  {
    if(combo == 0) p->tonecurve_type[ch] = CUBIC_SPLINE;
    if(combo == 1) p->tonecurve_type[ch] = CATMULL_ROM;
    if(combo == 2) p->tonecurve_type[ch] = MONOTONE_HERMITE;
  }
  dt_dev_add_history_item(darktable.develop, self, TRUE);
  gtk_widget_queue_draw(GTK_WIDGET(g->area));
}

static void tab_switch(GtkNotebook *notebook, GtkWidget *page, guint page_num, gpointer user_data)
{
  printf("tab_switch\n");
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_tonecurve_gui_data_t *g = (dt_iop_tonecurve_gui_data_t *)self->gui_data;
  if(self->dt->gui->reset) return;
  dt_iop_tonecurve_params_t *p = (dt_iop_tonecurve_params_t *)self->params;
  const int ch = g->channel = (tonecurve_channel_t)page_num;
  if(curve_detail[mode_curves[p->tonecurve_tc_mode].curve_detail_i[ch]].c_log_enabled)
  {
    dt_bauhaus_combobox_set(g->scale, g->scale_mode[ch]);
    gtk_widget_set_visible(g->scale, TRUE);
    if (g->scale_mode[ch] != linxliny)
    {
      gtk_widget_set_visible(g->logbase, TRUE);
      dt_bauhaus_slider_set(g->logbase, g->loglogscale[ch]);
    }
    else gtk_widget_set_visible(g->logbase, FALSE);
  }
  else
  {
    gtk_widget_set_visible(g->scale, FALSE);
    gtk_widget_set_visible(g->logbase, FALSE);
  }
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

static float to_log(const float x, const float base, const int log_enabled, const int scale_mode, const int is_ordinate)
{
  if(scale_mode != linxliny && log_enabled)
  {
    if ( scale_mode == logxliny && is_ordinate == 1)
    {
      // we don't want log on ordinate axis in semilog x
      return x;
    }
    else if ( scale_mode == linxlogy && is_ordinate == 0)
    {
      // we don't want log on abcissa axis in semilog y
      return x;
    }
    else
    {
      return logf(x * (base - 1.0f) + 1.0f) / logf(base);
    }
  }
  else
  {
    return x;
  }
}

static float to_lin(const float x, const float base, const int log_enabled, const int scale_mode, const int is_ordinate)
{
  if(scale_mode != linxliny && log_enabled)
  {
    if (scale_mode == logxliny && is_ordinate == 1)
    {
      // we don't want log on ordinate axis in semilog x
      return x;
    }
    else if (scale_mode == linxlogy && is_ordinate == 0)
    {
      // we don't want log on abcissa axis in semilog y
      return x;
    }
    else
    {
      return (powf(base, x) - 1.0f) / (base - 1.0f);
    }
  }
  else
  {
    return x;
  }
}

static void _iop_color_picker_apply(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_tonecurve_global_data_t *gd = (dt_iop_tonecurve_global_data_t *)self->data;

  for(int k=0; k<3; k++)
  {
    gd->picked_color[k] = self->picked_color[k];
    gd->picked_color_min[k] = self->picked_color_min[k];
    gd->picked_color_max[k] = self->picked_color_max[k];
    gd->picked_output_color[k] = self->picked_output_color[k];
  }
  dt_control_queue_redraw_widget(self->widget);
}

static int _iop_color_picker_get_set(dt_iop_module_t *self, GtkWidget *button)
{
  dt_iop_color_picker_t *picker = self->picker;
  const int current_picker = picker->current_picker;

  picker->current_picker = 1;

  if(current_picker == picker->current_picker)
    return DT_COLOR_PICKER_ALREADY_SELECTED;
  else
    return picker->current_picker;
}

static void _iop_color_picker_update(dt_iop_module_t *self)
{
  dt_iop_tonecurve_gui_data_t *g = (dt_iop_tonecurve_gui_data_t *)self->gui_data;
  const int old_reset = darktable.gui->reset;
  darktable.gui->reset = 1;

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->colorpicker), g->color_picker.current_picker == 1);

  darktable.gui->reset = old_reset;
  dt_control_queue_redraw_widget(self->widget);
}

static void dt_iop_tonecurve_sanity_check(dt_iop_module_t *self, GtkWidget *widget)
{
  dt_iop_tonecurve_gui_data_t *c = (dt_iop_tonecurve_gui_data_t *)self->gui_data;
  dt_iop_tonecurve_params_t *p = (dt_iop_tonecurve_params_t *)self->params;

  const int ch = c->channel;
  const int nodes = p->tonecurve_nodes[ch];
  dt_iop_tonecurve_node_t *tonecurve = p->tonecurve[ch];
  int nb_ch = mode_curves[p->tonecurve_tc_mode].nb_ch;

  // if channel not included in this mode
  if (ch > nb_ch - 1) return;

  if(nodes <= 2) return;

  const float mx = tonecurve[c->selected].x;

  // delete vertex if order has changed
  // for all points, x coordinate of point must be strictly larger than
  // the x coordinate of the previous point
  if((c->selected > 0 && (tonecurve[c->selected - 1].x >= mx))
     || (c->selected < nodes - 1 && (tonecurve[c->selected + 1].x <= mx)))
  {
    for(int k = c->selected; k < nodes - 1; k++)
    {
      tonecurve[k].x = tonecurve[k + 1].x;
      tonecurve[k].y = tonecurve[k + 1].y;
    }
    c->selected = -2; // avoid re-insertion of that point immediately after this
    p->tonecurve_nodes[ch]--;
  }
}

static gboolean _move_point_internal(dt_iop_module_t *self, GtkWidget *widget, float dx, float dy, guint state)
{
  dt_iop_tonecurve_params_t *p = (dt_iop_tonecurve_params_t *)self->params;
  dt_iop_tonecurve_gui_data_t *c = (dt_iop_tonecurve_gui_data_t *)self->gui_data;

  int ch = c->channel;
  dt_iop_tonecurve_node_t *tonecurve = p->tonecurve[ch];

  float multiplier;

  GdkModifierType modifiers = gtk_accelerator_get_default_mod_mask();
  if((state & modifiers) == GDK_SHIFT_MASK)
  {
    multiplier = dt_conf_get_float("darkroom/ui/scale_rough_step_multiplier");
  }
  else if((state & modifiers) == GDK_CONTROL_MASK)
  {
    multiplier = dt_conf_get_float("darkroom/ui/scale_precise_step_multiplier");
  }
  else
  {
    multiplier = dt_conf_get_float("darkroom/ui/scale_step_multiplier");
  }

  dx *= multiplier;
  dy *= multiplier;

  tonecurve[c->selected].x = CLAMP(tonecurve[c->selected].x + dx, 0.0f, 1.0f);
  tonecurve[c->selected].y = CLAMP(tonecurve[c->selected].y + dy, 0.0f, 1.0f);

  dt_iop_tonecurve_sanity_check(self, widget);

  dt_dev_add_history_item(darktable.develop, self, TRUE);
  gtk_widget_queue_draw(widget);

  return TRUE;
}

#define TONECURVE_DEFAULT_STEP (0.001f)

static gboolean _scrolled(GtkWidget *widget, GdkEventScroll *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_tonecurve_params_t *p = (dt_iop_tonecurve_params_t *)self->params;
  dt_iop_tonecurve_gui_data_t *c = (dt_iop_tonecurve_gui_data_t *)self->gui_data;

  int ch = c->channel;
  int nb_ch = mode_curves[p->tonecurve_tc_mode].nb_ch;

  // if channel not included in this mode
  if (ch > nb_ch - 1) return TRUE;

  if(c->selected < 0) return TRUE;

  gdouble delta_y;
  if(dt_gui_get_scroll_deltas(event, NULL, &delta_y))
  {
    delta_y *= -TONECURVE_DEFAULT_STEP;
    return _move_point_internal(self, widget, 0.0, delta_y, event->state);
  }

  return TRUE;
}

static gboolean dt_iop_tonecurve_key_press(GtkWidget *widget, GdkEventKey *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_tonecurve_params_t *p = (dt_iop_tonecurve_params_t *)self->params;
  dt_iop_tonecurve_gui_data_t *c = (dt_iop_tonecurve_gui_data_t *)self->gui_data;

  int ch = c->channel;
  int nb_ch = mode_curves[p->tonecurve_tc_mode].nb_ch;

  // if channel not included in this mode
  if (ch > nb_ch - 1) return TRUE;

  if(c->selected < 0) return TRUE;

  int handled = 0;
  float dx = 0.0f, dy = 0.0f;
  if(event->keyval == GDK_KEY_Up || event->keyval == GDK_KEY_KP_Up)
  {
    handled = 1;
    dy = TONECURVE_DEFAULT_STEP;
  }
  else if(event->keyval == GDK_KEY_Down || event->keyval == GDK_KEY_KP_Down)
  {
    handled = 1;
    dy = -TONECURVE_DEFAULT_STEP;
  }
  else if(event->keyval == GDK_KEY_Right || event->keyval == GDK_KEY_KP_Right)
  {
    handled = 1;
    dx = TONECURVE_DEFAULT_STEP;
  }
  else if(event->keyval == GDK_KEY_Left || event->keyval == GDK_KEY_KP_Left)
  {
    handled = 1;
    dx = -TONECURVE_DEFAULT_STEP;
  }

  if(!handled) return TRUE;

  return _move_point_internal(self, widget, dx, dy, event->state);
}

#undef TONECURVE_DEFAULT_STEP

void gui_focus(struct dt_iop_module_t *self, gboolean in)
{
  dt_iop_tonecurve_gui_data_t *c = (dt_iop_tonecurve_gui_data_t *)self->gui_data;
  c->got_focus = in;
}

void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_tonecurve_gui_data_t));
  dt_iop_tonecurve_gui_data_t *c = (dt_iop_tonecurve_gui_data_t *)self->gui_data;
  dt_iop_tonecurve_params_t *p = (dt_iop_tonecurve_params_t *)self->params;

  for(int ch = 0; ch < DT_IOP_TONECURVE_MAX_CH; ch++)
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
  for( int ch = 0; ch < DT_IOP_TONECURVE_MAX_CH; ch++)
  {
    c->loglogscale[ch] = 0;
    c->scale_mode[ch] = linxliny;
  }
  c->got_focus = FALSE;
  for (int i = 0; i < DT_IOP_TONECURVE_MAX_CH * DT_IOP_TONECURVE_BINS; i++) c->local_histogram[i] = 0;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);
  dt_gui_add_help_link(self->widget, dt_get_help_url(self->op));

  c->tc_mode = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(c->tc_mode, NULL, _("color space"));
  dt_bauhaus_combobox_add(c->tc_mode, _("Lab, linked channels"));
  dt_bauhaus_combobox_add(c->tc_mode, _("Lab, independent channels"));
  dt_bauhaus_combobox_add(c->tc_mode, _("XYZ, linked channels"));
  dt_bauhaus_combobox_add(c->tc_mode, _("RGB, linked channels"));
  dt_bauhaus_combobox_add(c->tc_mode, _("RGB, independent channels"));
  dt_bauhaus_combobox_add(c->tc_mode, _("LCh, independent channels"));
  gtk_box_pack_start(GTK_BOX(self->widget), c->tc_mode, TRUE, TRUE, 0);
  gtk_widget_set_tooltip_text(c->tc_mode, _("if set to Lab linked channels, a and b curves have no effect and are "
                                                 "not displayed. chroma values (a and b) of each pixel are "
                                                 "then adjusted based on L curve data. XYZ is similar "
                                                 "but applies the saturation changes in XYZ space."));
  g_signal_connect(G_OBJECT(c->tc_mode), "value-changed", G_CALLBACK(tc_mode_callback), self);

  // tabs
  c->channel_tabs = GTK_NOTEBOOK(gtk_notebook_new());

  gtk_notebook_append_page(GTK_NOTEBOOK(c->channel_tabs),
                           GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0)), gtk_label_new(_("  L  ")));
  gtk_widget_set_tooltip_text(gtk_notebook_get_tab_label(c->channel_tabs, gtk_notebook_get_nth_page(c->channel_tabs, -1)),
                              _("tonecurve for L channel"));
  gtk_notebook_append_page(GTK_NOTEBOOK(c->channel_tabs),
                           GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0)), gtk_label_new(_("  a  ")));
  gtk_widget_set_tooltip_text(gtk_notebook_get_tab_label(c->channel_tabs, gtk_notebook_get_nth_page(c->channel_tabs, -1)),
                              _("tonecurve for a channel"));
  gtk_notebook_append_page(GTK_NOTEBOOK(c->channel_tabs),
                           GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0)), gtk_label_new(_("  b  ")));
  gtk_widget_set_tooltip_text(gtk_notebook_get_tab_label(c->channel_tabs, gtk_notebook_get_nth_page(c->channel_tabs, -1)),
                              _("tonecurve for b channel"));
  gtk_notebook_append_page(GTK_NOTEBOOK(c->channel_tabs),
                           GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0)), gtk_label_new("spare"));
  gtk_widget_set_tooltip_text(gtk_notebook_get_tab_label(c->channel_tabs, gtk_notebook_get_nth_page(c->channel_tabs, -1)),
                              "spare");

  gtk_widget_show_all(GTK_WIDGET(gtk_notebook_get_nth_page(c->channel_tabs, c->channel)));
  gtk_notebook_set_current_page(GTK_NOTEBOOK(c->channel_tabs), c->channel);

  GtkWidget *tb = dtgtk_togglebutton_new(dtgtk_cairo_paint_colorpicker, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
  gtk_widget_set_size_request(GTK_WIDGET(tb), DT_PIXEL_APPLY_DPI(14), DT_PIXEL_APPLY_DPI(14));
  gtk_widget_set_tooltip_text(tb, _("pick GUI color from image\nctrl+click to select an area"));
  c->colorpicker = tb;

  GtkWidget *notebook = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_start(GTK_BOX(notebook), GTK_WIDGET(c->channel_tabs), FALSE, FALSE, 0);
  gtk_box_pack_end(GTK_BOX(notebook), GTK_WIDGET(tb), FALSE, FALSE, 0);

  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), vbox, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(notebook), TRUE, TRUE, 0);

  g_signal_connect(G_OBJECT(c->channel_tabs), "switch_page", G_CALLBACK(tab_switch), self);

  c->area = GTK_DRAWING_AREA(dtgtk_drawing_area_new_with_aspect_ratio(1.0));
  gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(c->area), TRUE, TRUE, 0);

  // FIXME: that tooltip goes in the way of the numbers when you hover a node to get a reading
  //gtk_widget_set_tooltip_text(GTK_WIDGET(c->area), _("double click to reset curve"));

  gtk_widget_add_events(GTK_WIDGET(c->area), GDK_POINTER_MOTION_MASK | GDK_POINTER_MOTION_HINT_MASK
                                                 | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK
                                                 | GDK_LEAVE_NOTIFY_MASK | GDK_SCROLL_MASK
                                                 | darktable.gui->scroll_mask);
  gtk_widget_set_can_focus(GTK_WIDGET(c->area), TRUE);
  g_signal_connect(G_OBJECT(c->area), "draw", G_CALLBACK(dt_iop_tonecurve_draw), self);
  g_signal_connect(G_OBJECT(c->area), "button-press-event", G_CALLBACK(dt_iop_tonecurve_button_press), self);
  g_signal_connect(G_OBJECT(c->area), "motion-notify-event", G_CALLBACK(dt_iop_tonecurve_motion_notify), self);
  g_signal_connect(G_OBJECT(c->area), "leave-notify-event", G_CALLBACK(dt_iop_tonecurve_leave_notify), self);
  g_signal_connect(G_OBJECT(c->area), "enter-notify-event", G_CALLBACK(dt_iop_tonecurve_enter_notify), self);
  g_signal_connect(G_OBJECT(c->area), "configure-event", G_CALLBACK(area_resized), self);
  g_signal_connect(G_OBJECT(tb), "button-press-event", G_CALLBACK(dt_iop_color_picker_callback_button_press), &c->colorpicker);
  g_signal_connect(G_OBJECT(c->area), "scroll-event", G_CALLBACK(_scrolled), self);
  g_signal_connect(G_OBJECT(c->area), "key-press-event", G_CALLBACK(dt_iop_tonecurve_key_press), self);

  /* From src/common/curve_tools.h :
    #define CUBIC_SPLINE 0
    #define CATMULL_ROM 1
    #define MONOTONE_HERMITE 2
  */
  c->interpolator = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(c->interpolator, NULL, _("interpolation method"));
  dt_bauhaus_combobox_add(c->interpolator, _("cubic spline"));
  dt_bauhaus_combobox_add(c->interpolator, _("centripetal spline"));
  dt_bauhaus_combobox_add(c->interpolator, _("monotonic spline"));
  gtk_box_pack_start(GTK_BOX(self->widget), c->interpolator , TRUE, TRUE, 0);
  gtk_widget_set_tooltip_text(c->interpolator, _("change this method if you see oscillations or cusps in the curve\n"
                                                 "- cubic spline is better to produce smooth curves but oscillates when nodes are too close\n"
                                                 "- centripetal is better to avoids cusps and oscillations with close nodes but is less smooth\n"
                                                 "- monotonic is better for accuracy of pure analytical functions (log, gamma, exp)\n"));
  g_signal_connect(G_OBJECT(c->interpolator), "value-changed", G_CALLBACK(interpolator_callback), self);

  c->scale = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(c->scale, NULL, _("scale"));
  dt_bauhaus_combobox_add(c->scale, _("linear"));
  dt_bauhaus_combobox_add(c->scale, _("log-log (xy)"));
  dt_bauhaus_combobox_add(c->scale, _("semi-log (x)"));
  dt_bauhaus_combobox_add(c->scale, _("semi-log (y)"));
  gtk_widget_set_tooltip_text(c->scale, _("scale to use in the graph. use logarithmic scale for "
                                          "more precise control near the blacks"));
  gtk_box_pack_start(GTK_BOX(self->widget), c->scale, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(c->scale), "value-changed", G_CALLBACK(scale_callback), self);


  c->logbase = dt_bauhaus_slider_new_with_range(self, 2.0f, 64.f, 0.5f, 2.0f, 2);
  dt_bauhaus_widget_set_label(c->logbase, NULL, _("base of the logarithm"));
  gtk_box_pack_start(GTK_BOX(self->widget), c->logbase , TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(c->logbase), "value-changed", G_CALLBACK(logbase_callback), self);

  c->rgb_norm = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(c->rgb_norm, NULL, _("rgb norm"));
  dt_bauhaus_combobox_add(c->rgb_norm, _("none"));
  dt_bauhaus_combobox_add(c->rgb_norm, _("L infinite (maxRGB)"));
  dt_bauhaus_combobox_add(c->rgb_norm, _("L average (avgRGB)"));
  dt_bauhaus_combobox_add(c->rgb_norm, _("L power"));
  dt_bauhaus_combobox_add(c->rgb_norm, _("basic power"));
  dt_bauhaus_combobox_add(c->rgb_norm, _("weighted yellow power"));
  gtk_widget_set_tooltip_text(c->rgb_norm, _("apply normalization factor"));
  gtk_box_pack_start(GTK_BOX(self->widget), c->rgb_norm, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(c->rgb_norm), "value-changed", G_CALLBACK(rgb_norm_callback), self);

  c->rgb_norm_exp = dt_bauhaus_slider_new_with_range(self, 0.1f, 5.0f, 0.1f, 2.0f, 2);
  dt_bauhaus_widget_set_label(c->rgb_norm_exp, NULL, _("rgb norm power"));
  gtk_box_pack_start(GTK_BOX(self->widget), c->rgb_norm_exp , TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(c->rgb_norm_exp), "value-changed", G_CALLBACK(rgb_norm_exp_callback), self);

  c->sizegroup = GTK_SIZE_GROUP(gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL));
  gtk_size_group_add_widget(c->sizegroup, GTK_WIDGET(c->area));
  gtk_size_group_add_widget(c->sizegroup, GTK_WIDGET(c->channel_tabs));

  dt_iop_init_picker(&c->color_picker,
              self,
              DT_COLOR_PICKER_POINT_AREA,
              _iop_color_picker_get_set,
              _iop_color_picker_apply,
              _iop_color_picker_update);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  dt_iop_tonecurve_gui_data_t *c = (dt_iop_tonecurve_gui_data_t *)self->gui_data;
  // this one we need to unref manually. not so the initially unowned widgets.
  g_object_unref(c->sizegroup);
  for(int ch = 0; ch < DT_IOP_TONECURVE_MAX_CH; ch++) dt_draw_curve_destroy(c->minmax_curve[ch]);
  free(self->gui_data);
  self->gui_data = NULL;
}

static gboolean dt_iop_tonecurve_enter_notify(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  gtk_widget_queue_draw(widget);
  return TRUE;
}

static gboolean dt_iop_tonecurve_leave_notify(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  gtk_widget_queue_draw(widget);
  return TRUE;
}

static void picker_scale(const float *in, float *out, int tc_mode)
{
  float inl[DT_IOP_TONECURVE_MAX_CH];
  const int colorspace = mode_curves[tc_mode].colorspace;
  const int nb_ch = mode_curves[tc_mode].nb_ch;
  for(int ch = 0; ch < DT_IOP_TONECURVE_MAX_CH ; ch++) inl[ch] = in[ch];
  if (colorspace == DT_TC_LCH)
    dt_Lab_2_LCH(in, inl);
  else if (colorspace == DT_TC_RGB)
  {
    inl[0] = in[0];
    for(int ch = 0; ch < 3; ch++)
    {
      float rgb[3];
      float lab[3];
      dt_Lab_to_prophotorgb(in, rgb);
      const float fake_rgb[3] = {rgb[ch], rgb[ch], rgb[ch]};
      dt_prophotorgb_to_Lab(fake_rgb, lab);
      inl[ch+1] = lab[0];
    }
  }
  for(int ch = 0; ch < nb_ch ; ch++)
  {
    const int j = mode_curves[tc_mode].curve_detail_i[ch];
    if (curve_detail[j].c_extrapolation == 2)
    {
      inl[ch] = CLAMP((inl[ch] - curve_detail[j].c_min_pipe_value) / (curve_detail[j].c_max_pipe_value - curve_detail[j].c_min_pipe_value + 1.0f), 0.0f, 1.0f);
    }
    else
    {
      inl[ch] = CLAMP(inl[ch] / curve_detail[j].c_max_pipe_value, 0.0f, 1.0f);
    }
  }
  for(int ch = 0; ch < nb_ch ; ch++) out[ch] = inl[ch];
}

static gboolean dt_iop_tonecurve_draw(GtkWidget *widget, cairo_t *crf, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_tonecurve_gui_data_t *c = (dt_iop_tonecurve_gui_data_t *)self->gui_data;
  dt_iop_tonecurve_params_t *p = (dt_iop_tonecurve_params_t *)self->params;

  dt_develop_t *dev = darktable.develop;

  int ch = c->channel;
  int nodes = p->tonecurve_nodes[ch];
  dt_iop_tonecurve_node_t *tonecurve = p->tonecurve[ch];
  const int tc_mode = p->tonecurve_tc_mode;
  const int curve_detail_i = mode_curves[tc_mode].curve_detail_i[ch];
  const int chx = curve_detail[curve_detail_i].chx;
  const int chy = curve_detail[curve_detail_i].chy;
  const int log_enabled = curve_detail[curve_detail_i].c_log_enabled;
  const float logscale = c->loglogscale[ch];

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

  float unbounded_coeffs[3];
  const float xm = tonecurve[nodes - 1].x;
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
  char text[256];

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
  if (tc_mode == DT_S_SCALE_MANUAL_LAB)  // Lab
  {
    const int cells = 8;
    const float color_labels_left[3][3] =
          { { 0.3f, 0.3f, 0.3f }, { 0.0f, 0.34f, 0.27f }, { 0.0f, 0.27f, 0.58f } };
    const float color_labels_right[3][3] =
          { { 0.3f, 0.3f, 0.3f }, { 0.53f, 0.08f, 0.28f }, { 0.81f, 0.66f, 0.0f } };
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
  } /*
  else if (tc_mode == DT_S_SCALE_MANUAL_LCH && ch == ch_Ch) // Ch
  {
    const int cells = 64;
    const float color_labels[5][3] =
          { { 0.53f, 0.08f, 0.28f }, { 0.81f, 0.66f, 0.0f }, { 0.0f, 0.34f, 0.27f }, { 0.0f, 0.27f, 0.58f }, { 0.53f, 0.08f, 0.28f } };
    for(int i = 0; i < cells; i++)
    {
      const int j = i * 4 / cells;
      const float f = fmod((float)i, (float)cells / 4.0f) / ((float)cells / 4.0f);
      cairo_set_source_rgba(cr, (1.0f - f) * color_labels[j][0] + f * color_labels[j+1][0],
                            (1.0f - f) * color_labels[j][1] + f * color_labels[j+1][1],
                            (1.0f - f) * color_labels[j][2] + f * color_labels[j+1][2],
                            1.0f);
      cairo_rectangle(cr, width * i / (float)cells, height * (cells - 1) / (float)cells, width / (float)cells,
                    height / (float)cells);
      cairo_fill(cr);
    }
  } */

  // draw grid
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(.4));
  cairo_set_source_rgb(cr, .1, .1, .1);

  if ((c->scale_mode[ch] != linxliny) && log_enabled )
  {
    if (c->scale_mode[ch] == logxlogy)
    {
      dt_draw_loglog_grid(cr, 4, 0, height, width, 0, logscale);
    }
    else if (c->scale_mode[ch] == logxliny)
    {
      dt_draw_semilog_x_grid(cr, 4, 0, height, width, 0, logscale);
    }
    else if (c->scale_mode[ch] == linxlogy)
    {
      dt_draw_semilog_y_grid(cr, 4, 0, height, width, 0, logscale);
    }
  }
  else
  {
    dt_draw_grid(cr, 4, 0, 0, width, height);
  }

  // draw identity line
  if (chx == chy)
  {
    cairo_move_to(cr, 0, height);
    cairo_line_to(cr, width, 0);
  }
  else
  {
    const float height2 = to_log(height / 2.0f, logscale, log_enabled, c->scale_mode[ch], 1);
    cairo_move_to(cr, 0, height2);
    cairo_line_to(cr, width, height2);
  }
  cairo_stroke(cr);

  // if channel not included in this mode
  if(ch > mode_curves[tc_mode].nb_ch - 1)  goto finally;

  // draw nodes positions
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.));
  cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
  cairo_translate(cr, 0, height);

  for(int k = 0; k < nodes; k++)
  {
    const float x = to_log(tonecurve[k].x, logscale, log_enabled, c->scale_mode[ch], 0),
                y = to_log(tonecurve[k].y, logscale, log_enabled, c->scale_mode[ch], 1);

    cairo_arc(cr, x * width, -y * height, DT_PIXEL_APPLY_DPI(3), 0, 2. * M_PI);
    cairo_stroke(cr);
  }

  // draw selected cursor
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.));

  if(c->selected >= 0)
  {
    cairo_set_source_rgb(cr, .9, .9, .9);
    const float x = to_log(tonecurve[c->selected].x, logscale, log_enabled, c->scale_mode[ch], 0),
                y = to_log(tonecurve[c->selected].y, logscale, log_enabled, c->scale_mode[ch], 1);

    cairo_arc(cr, x * width, -y * height, DT_PIXEL_APPLY_DPI(4), 0, 2. * M_PI);
    cairo_stroke(cr);
  }

  // draw histogram in background
  // only if module is enabled
  if(self->enabled )
  {
    float hist_max;
    float *raw_mean, *raw_min, *raw_max;
    float *raw_mean_output;
    float picker_mean[DT_IOP_TONECURVE_MAX_CH];
    float picker_min[DT_IOP_TONECURVE_MAX_CH];
    float picker_max[DT_IOP_TONECURVE_MAX_CH];
    float picker_mean_output[DT_IOP_TONECURVE_MAX_CH];

    raw_mean = self->picked_color;
    raw_min = self->picked_color_min;
    raw_max = self->picked_color_max;
    raw_mean_output = self->picked_output_color;

    uint32_t *hist;
    hist = self->histogram;
    if (hist && curve_detail[curve_detail_i].c_histogram_enabled)
    {
      float hist_color[3] = {0.2f, 0.2f, 0.2f};;
      uint32_t *histogram_max;
        if (tc_mode == DT_S_SCALE_MANUAL_RGB || DT_S_SCALE_MANUAL_LCH)
        {
//uint32_t sum_histo = 0;
//for(int i=0; i < 4*DT_IOP_TONECURVE_BINS; i++) sum_histo += c->local_histogram[i];
//printf("dt_iop_tonecurve_draw - sum histo %d, L0 %d\n", sum_histo);
          c->local_histogram_max[ch] = 0;
          for(int i = 0; i < 4 * DT_IOP_TONECURVE_BINS; i+=4)
          {
            if (ch == 0) c->local_histogram[i] = hist[i]; // get channel L
            c->local_histogram_max[ch] = (c->local_histogram[i+ch] > c->local_histogram_max[ch])
                    ? c->local_histogram[i+ch] : c->local_histogram_max[ch];
          }
          hist = &c->local_histogram[0];
          histogram_max = &c->local_histogram_max[0];
          if (tc_mode == DT_S_SCALE_MANUAL_RGB && ch!=0 ) hist_color[ch-1] = 0.4f;
        }
      else
      { // other cases we use the standard histogram
        hist = self->histogram;
        histogram_max = &self->histogram_max[0];
      }
      if ((c->scale_mode[ch] != linxliny) && curve_detail[curve_detail_i].c_log_enabled)
      {
        hist_max = c->scale_mode[ch] == logxliny ? histogram_max[ch]
                        : logf(histogram_max[ch] / 255.0f * (logscale - 1.0f) + 1.0f) / logf(logscale) * 255.0f;
      }
      else
      {
        hist_max = dev->histogram_type == DT_DEV_HISTOGRAM_LINEAR ? histogram_max[ch]
                        : logf(1.0 + histogram_max[ch]);
      }
      if(hist && hist_max > 0.0f)
      {
        cairo_save(cr);
        cairo_scale(cr, width / 255.0, -(height - DT_PIXEL_APPLY_DPI(5)) / hist_max);
        cairo_set_source_rgba(cr, hist_color[0], hist_color[1], hist_color[2], 0.5);
        if ((c->scale_mode[ch] != linxliny) && curve_detail[curve_detail_i].c_log_enabled)
        {
          if (c->scale_mode[ch] == logxliny)
            dt_draw_histogram_8_logx_liny(cr, hist, 4, ch, logscale);
          else if (c->scale_mode[ch] == logxlogy)
            dt_draw_histogram_8_logx_logy(cr, hist, 4, ch, logscale);
          else if (c->scale_mode[ch] == linxlogy)
            dt_draw_histogram_8_linx_logy(cr, hist, 4, ch, logscale);
        }
        else
        {
          // TODO: make draw handle waveform histograms
          dt_draw_histogram_8(cr, hist, 4, ch, dev->histogram_type == DT_DEV_HISTOGRAM_LINEAR);
        }
        cairo_restore(cr);
      }
    }

    if(self->request_color_pick == DT_REQUEST_COLORPICK_MODULE)
    {
      // the global live samples ...
      GSList *samples = darktable.lib->proxy.colorpicker.live_samples;
      dt_colorpicker_sample_t *sample = NULL;
      while(samples)
      {
        sample = samples->data;

        picker_scale(sample->picked_color_lab_mean, picker_mean, tc_mode);
        picker_scale(sample->picked_color_lab_min, picker_min, tc_mode);
        picker_scale(sample->picked_color_lab_max, picker_max, tc_mode);
        // Convert abcissa to log coordinates if needed
        picker_min[ch] = to_log(picker_min[ch], logscale, log_enabled, c->scale_mode[ch], 0);
        picker_max[ch] = to_log(picker_max[ch], logscale, log_enabled, c->scale_mode[ch], 0);
        picker_mean[ch] = to_log(picker_mean[ch], logscale, log_enabled, c->scale_mode[ch], 0);

        const int ch2 = (chx == chy) ? ch : chx; // display the reference value for the measure
        cairo_set_source_rgba(cr, 0.5, 0.7, 0.5, 0.15);
        cairo_rectangle(cr, width * picker_min[ch2], 0, width * fmax(picker_max[ch2] - picker_min[ch2], 0.0f),
                        -height);
        cairo_fill(cr);
        cairo_set_source_rgba(cr, 0.5, 0.7, 0.5, 0.5);
        cairo_move_to(cr, width * picker_mean[ch2], 0);
        cairo_line_to(cr, width * picker_mean[ch2], -height);
        cairo_stroke(cr);

        samples = g_slist_next(samples);
      }

      // ... and the local sample
      if(raw_max[0] >= 0.0f)
      {
        PangoLayout *layout;
        PangoRectangle ink;
        PangoFontDescription *desc = pango_font_description_copy_static(darktable.bauhaus->pango_font_desc);
        pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
        pango_font_description_set_absolute_size(desc, PANGO_SCALE);
        layout = pango_cairo_create_layout(cr);
        pango_layout_set_font_description(layout, desc);
        picker_scale(raw_mean, picker_mean, tc_mode);
        picker_scale(raw_min, picker_min, tc_mode);
        picker_scale(raw_max, picker_max, tc_mode);
        const float picker_mean2 = picker_mean[chy]; // keep memory for value display
        // Convert abcissa to log coordinates if needed
        picker_min[ch] = to_log(picker_min[ch], logscale, log_enabled, c->scale_mode[ch], 0);
        picker_max[ch] = to_log(picker_max[ch], logscale, log_enabled, c->scale_mode[ch], 0);
        picker_mean[ch] = to_log(picker_mean[ch], logscale, log_enabled, c->scale_mode[ch], 0);

        // scale conservatively to 100% of width:
        snprintf(text, sizeof(text), "100.00 / 100.00 ( +100.00)");
        pango_layout_set_text(layout, text, -1);
        pango_layout_get_pixel_extents(layout, &ink, NULL);
        pango_font_description_set_absolute_size(desc, width*1.0/ink.width * PANGO_SCALE);
        pango_layout_set_font_description(layout, desc);
        const int ch2 = (chx == chy) ? ch : chx; // display the reference value for the measure
                                                  // for colorspace <> Lab, min & max may not be valid
        cairo_set_source_rgba(cr, 0.7, 0.5, 0.5, 0.33);
        cairo_rectangle(cr, width * picker_min[ch2], 0, width * fmax(picker_max[ch2] - picker_min[ch2], 0.0f),
                        -height);
        cairo_fill(cr);
        cairo_set_source_rgba(cr, 0.9, 0.7, 0.7, 0.5);
        cairo_move_to(cr, width * picker_mean[ch2], 0);
        cairo_line_to(cr, width * picker_mean[ch2], -height);
        cairo_stroke(cr);

        picker_scale(raw_mean_output, picker_mean_output, tc_mode);
        float max_scale_value = curve_detail[mode_curves[tc_mode].curve_detail_i[chy]].c_max_display_value;
        const float min_scale_value = curve_detail[mode_curves[tc_mode].curve_detail_i[chy]].c_min_display_value;
        if (curve_detail[mode_curves[tc_mode].curve_detail_i[chy]].c_extrapolation == 2)
        {
          max_scale_value = max_scale_value - min_scale_value + 1.0f;
          snprintf(text, sizeof(text), "%.1f → %.1f", picker_mean2 * max_scale_value + min_scale_value, picker_mean_output[chy] * max_scale_value + min_scale_value);
        }
        else
          snprintf(text, sizeof(text), "%.1f → %.1f", picker_mean2 * max_scale_value, picker_mean_output[chy] * max_scale_value);

        cairo_set_source_rgb(cr, 0.1, 0.1, 0.1);
        cairo_set_font_size(cr, DT_PIXEL_APPLY_DPI(0.04) * height);
        pango_layout_set_text(layout, text, -1);
        pango_layout_get_pixel_extents(layout, &ink, NULL);
        cairo_move_to(cr, 0.02f * width, -0.94 * height - ink.height - ink.y);
        pango_cairo_show_layout(cr, layout);
        cairo_stroke(cr);
        pango_font_description_free(desc);
        g_object_unref(layout);
      }
    }
  }

  if(c->selected >= 0)
  {
    // draw information about current selected node
    PangoLayout *layout;
    PangoRectangle ink;
    PangoFontDescription *desc = pango_font_description_copy_static(darktable.bauhaus->pango_font_desc);
    pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
    pango_font_description_set_absolute_size(desc, PANGO_SCALE);
    layout = pango_cairo_create_layout(cr);
    pango_layout_set_font_description(layout, desc);

    // scale conservatively to 100% of width:
    snprintf(text, sizeof(text), "100.00 / 100.00 ( +100.00)");
    pango_layout_set_text(layout, text, -1);
    pango_layout_get_pixel_extents(layout, &ink, NULL);
    pango_font_description_set_absolute_size(desc, width*1.0/ink.width * PANGO_SCALE);
    pango_layout_set_font_description(layout, desc);

    const float min_scale_value = curve_detail[mode_curves[tc_mode].curve_detail_i[chy]].c_min_display_value;
    const float max_scale_value = curve_detail[mode_curves[tc_mode].curve_detail_i[chy]].c_max_display_value;

    const float x_node_value = tonecurve[c->selected].x * (max_scale_value - min_scale_value) + min_scale_value;
    const float y_node_value = tonecurve[c->selected].y * (max_scale_value - min_scale_value) + min_scale_value;
    const float d_node_value = y_node_value - x_node_value;

    if (chx == chy)
    {
      snprintf(text, sizeof(text), "%.1f / %.1f ( %+.1f)", x_node_value, y_node_value, d_node_value);
    }
    else // x & y are not meaningful x & y are different data. Keep only the variation
    {
      snprintf(text, sizeof(text), "( %+.1f%%)", (y_node_value - 50.0f) * 2.0f);
    }
    cairo_set_source_rgb(cr, 0.1, 0.1, 0.1);
    pango_layout_set_text(layout, text, -1);
    pango_layout_get_pixel_extents(layout, &ink, NULL);
    cairo_move_to(cr, 0.98f * width - ink.width - ink.x, -0.02 * height - ink.height - ink.y);
    pango_cairo_show_layout(cr, layout);
    cairo_stroke(cr);
    pango_font_description_free(desc);
    g_object_unref(layout);

    // enlarge selected node
    cairo_set_source_rgb(cr, .9, .9, .9);
    const float x = to_log(tonecurve[c->selected].x, logscale, log_enabled, c->scale_mode[ch], 0),
                y = to_log(tonecurve[c->selected].y, logscale, log_enabled, c->scale_mode[ch], 1);

    cairo_arc(cr, x * width, -y * height, DT_PIXEL_APPLY_DPI(4),
              0, 2. * M_PI);
    cairo_stroke(cr);
  }

  // draw curve
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(2.));
  cairo_set_source_rgb(cr, .9, .9, .9);
  // cairo_set_line_cap  (cr, CAIRO_LINE_CAP_SQUARE);

  const float y_offset = to_log(c->draw_ys[0], logscale, log_enabled, c->scale_mode[ch], 1);
  cairo_move_to(cr, 0, -height * y_offset);

  for(int k = 1; k < DT_IOP_TONECURVE_RES; k++)
  {
    const float xx = k / (DT_IOP_TONECURVE_RES - 1.0f);
    float yy;

    if(xx > xm)
    {
      yy = dt_iop_eval_exp(unbounded_coeffs, xx);
    }
    else
    {
      yy = c->draw_ys[k];
    }

    const float x = to_log(xx, logscale, log_enabled, c->scale_mode[ch], 0),
                y = to_log(yy, logscale, log_enabled, c->scale_mode[ch], 1);

    cairo_line_to(cr, x * width, -height * y);
  }
  cairo_stroke(cr);

finally:
  cairo_destroy(cr);
  cairo_set_source_surface(crf, cst, 0, 0);
  cairo_paint(crf);
  cairo_surface_destroy(cst);
  return TRUE;
}

static inline int _add_node(dt_iop_tonecurve_node_t *tonecurve, int *nodes, float x, float y)
{
  int selected = -1;
  if(tonecurve[0].x > x)
    selected = 0;
  else
  {
    for(int k = 1; k < *nodes; k++)
    {
      if(tonecurve[k].x > x)
      {
        selected = k;
        break;
      }
    }
  }
  if(selected == -1) selected = *nodes;
  for(int i = *nodes; i > selected; i--)
  {
    tonecurve[i].x = tonecurve[i - 1].x;
    tonecurve[i].y = tonecurve[i - 1].y;
  }
  // found a new point
  tonecurve[selected].x = x;
  tonecurve[selected].y = y;
  (*nodes)++;
  return selected;
}

static gboolean dt_iop_tonecurve_motion_notify(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_tonecurve_gui_data_t *c = (dt_iop_tonecurve_gui_data_t *)self->gui_data;
  dt_iop_tonecurve_params_t *p = (dt_iop_tonecurve_params_t *)self->params;

  const int ch = c->channel;
  const float logscale = c->loglogscale[ch];
  int nodes = p->tonecurve_nodes[ch];
  dt_iop_tonecurve_node_t *tonecurve = p->tonecurve[ch];
  const int tc_mode = p->tonecurve_tc_mode;
  const int log_enabled = curve_detail[mode_curves[tc_mode].curve_detail_i[ch]].c_log_enabled;

  if (ch >= mode_curves[tc_mode].nb_ch) goto finally;

  const int inset = DT_GUI_CURVE_EDITOR_INSET;
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  int height = allocation.height - 2 * inset, width = allocation.width - 2 * inset;
  double old_m_x = c->mouse_x;
  double old_m_y = c->mouse_y;
  c->mouse_x = event->x - inset;
  c->mouse_y = event->y - inset;

  const float mx = CLAMP(c->mouse_x, 0, width) / width;
  const float my = 1.0f - CLAMP(c->mouse_y, 0, height) / height;
  const float linx = to_lin(mx, logscale, log_enabled, c->scale_mode[ch], 0),
              liny = to_lin(my, logscale, log_enabled, c->scale_mode[ch], 1);

  if(event->state & GDK_BUTTON1_MASK)
  {
    // got a vertex selected:
    if(c->selected >= 0)
    {
      // this is used to translate mause position in loglogscale to make this behavior unified with linear scale.
      const float translate_mouse_x = old_m_x / width - to_log(tonecurve[c->selected].x, logscale, log_enabled, c->scale_mode[ch], 0);
      const float translate_mouse_y = 1 - old_m_y / height - to_log(tonecurve[c->selected].y, logscale, log_enabled, c->scale_mode[ch], 1);
      // dx & dy are in linear coordinates
      const float dx = to_lin(c->mouse_x / width - translate_mouse_x, logscale, log_enabled, c->scale_mode[ch], 0)
                       - to_lin(old_m_x / width - translate_mouse_x, logscale, log_enabled, c->scale_mode[ch], 0);
      const float dy = to_lin(1 - c->mouse_y / height - translate_mouse_y, logscale, log_enabled, c->scale_mode[ch], 1)
                       - to_lin(1 - old_m_y / height - translate_mouse_y, logscale, log_enabled, c->scale_mode[ch], 1);
      return _move_point_internal(self, widget, dx, dy, event->state);
    }
    else if(nodes < DT_IOP_TONECURVE_MAXNODES && c->selected >= -1)
    {
      // no vertex was close, create a new one!
      c->selected = _add_node(tonecurve, &p->tonecurve_nodes[ch], linx, liny);
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
          = (my - to_log(tonecurve[k].y, logscale, log_enabled, c->scale_mode[ch], 1)) * (my - to_log(tonecurve[k].y, logscale, log_enabled, c->scale_mode[ch], 1))
            + (mx - to_log(tonecurve[k].x, logscale, log_enabled, c->scale_mode[ch], 0)) * (mx - to_log(tonecurve[k].x, logscale, log_enabled, c->scale_mode[ch], 0));
      if(dist < min)
      {
        min = dist;
        nearest = k;
      }
    }
    c->selected = nearest;
  }
finally:
  if(c->selected >= 0) gtk_widget_grab_focus(widget);
  gtk_widget_queue_draw(widget);
  return TRUE;
}

static gboolean dt_iop_tonecurve_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_tonecurve_params_t *p = (dt_iop_tonecurve_params_t *)self->params;
  dt_iop_tonecurve_params_t *d = (dt_iop_tonecurve_params_t *)self->default_params;
  dt_iop_tonecurve_gui_data_t *c = (dt_iop_tonecurve_gui_data_t *)self->gui_data;

  const int ch = c->channel;
  const float logscale = c->loglogscale[ch];
  int nodes = p->tonecurve_nodes[ch];
  dt_iop_tonecurve_node_t *tonecurve = p->tonecurve[ch];
  const int tc_mode = p->tonecurve_tc_mode;
  const int log_enabled = curve_detail[mode_curves[tc_mode].curve_detail_i[ch]].c_log_enabled;

  if(event->button == 1)
  {
    if(event->type == GDK_BUTTON_PRESS && (event->state & GDK_CONTROL_MASK) == GDK_CONTROL_MASK
       && nodes < DT_IOP_TONECURVE_MAXNODES && c->selected == -1)
    {
      // if we are not on a node -> add a new node at the current x of the pointer and y of the curve at that x
      const int inset = DT_GUI_CURVE_EDITOR_INSET;
      GtkAllocation allocation;
      gtk_widget_get_allocation(widget, &allocation);
      int width = allocation.width - 2 * inset;
      c->mouse_x = event->x - inset;
      c->mouse_y = event->y - inset;

      const float mx = CLAMP(c->mouse_x, 0, width) / (float)width;
      const float linx = to_lin(mx, logscale, log_enabled, c->scale_mode[ch], 0);

      // don't add a node too close to others in x direction, it can crash dt
      int selected = -1;
      if(tonecurve[0].x > mx)
        selected = 0;
      else
      {
        for(int k = 1; k < nodes; k++)
        {
          if(tonecurve[k].x > mx)
          {
            selected = k;
            break;
          }
        }
      }
      if(selected == -1) selected = nodes;
      // > 0 -> check distance to left neighbour
      // < nodes -> check distance to right neighbour
      if(!((selected > 0 && linx - tonecurve[selected - 1].x <= 0.025) ||
           (selected < nodes && tonecurve[selected].x - linx <= 0.025)))
      {
        // evaluate the curve at the current x position
        const float y = dt_draw_curve_calc_value(c->minmax_curve[ch], linx);

        if(y >= 0.0 && y <= 1.0) // never add something outside the viewport, you couldn't change it afterwards
        {
          // create a new node
          selected = _add_node(tonecurve, &p->tonecurve_nodes[ch], linx, y);

          // maybe set the new one as being selected
          float min = .04f;
          min *= min; // comparing against square
          for(int k = 0; k < nodes; k++)
          {
            float other_y = to_log(tonecurve[k].y, logscale, log_enabled, c->scale_mode[ch], 1);
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
      p->tonecurve_nodes[ch] = d->tonecurve_nodes[ch];
      p->tonecurve_type[ch] = d->tonecurve_type[ch];
      for(int k = 0; k < d->tonecurve_nodes[ch]; k++)
      {
        p->tonecurve[ch][k].x = d->tonecurve[ch][k].x;
        p->tonecurve[ch][k].y = d->tonecurve[ch][k].y;
      }
      c->selected = -2; // avoid motion notify re-inserting immediately.
      dt_bauhaus_combobox_set(c->interpolator, p->tonecurve_type[ch_L]);
      dt_dev_add_history_item(darktable.develop, self, TRUE);
      gtk_widget_queue_draw(self->widget);
      return TRUE;
    }
  }
  else if(event->button == 3 && c->selected >= 0)
  {
    if(c->selected == 0 || c->selected == nodes - 1)
    {
      int k = c->selected == 0 ? 0 : d->tonecurve_nodes[ch] - 1;
      p->tonecurve[ch][c->selected].x = d->tonecurve[ch][k].x;
      p->tonecurve[ch][c->selected].y = d->tonecurve[ch][k].y;
      gtk_widget_queue_draw(self->widget);
      dt_dev_add_history_item(darktable.develop, self, TRUE);
      return TRUE;
    }

    for(int k = c->selected; k < nodes - 1; k++)
    {
      tonecurve[k].x = tonecurve[k + 1].x;
      tonecurve[k].y = tonecurve[k + 1].y;
    }
    c->selected = -2; // avoid re-insertion of that point immediately after this
    p->tonecurve_nodes[ch]--;
    gtk_widget_queue_draw(self->widget);
    dt_dev_add_history_item(darktable.develop, self, TRUE);
    return TRUE;
  }
  return FALSE;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
