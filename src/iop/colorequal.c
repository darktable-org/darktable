/*
    This file is part of darktable,
    Copyright (C) 2022-2024 darktable developers.

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

/* Midi mapping is supported, here is the reference for loupedeck+
midi:D7=iop/colorequal/page;hue
midi:D#7=iop/colorequal/page
midi:E7=iop/colorequal/page;brightness
None;midi:CC1=iop/colorequal/hue/red
None;midi:CC2=iop/colorequal/hue/orange
None;midi:CC3=iop/colorequal/hue/yellow
None;midi:CC4=iop/colorequal/hue/green
None;midi:CC5=iop/colorequal/hue/cyan
None;midi:CC6=iop/colorequal/hue/blue
None;midi:CC7=iop/colorequal/hue/lavender
None;midi:CC8=iop/colorequal/hue/magenta
None;midi:CC9=iop/colorequal/saturation/red
None;midi:CC10=iop/colorequal/saturation/orange
None;midi:CC11=iop/colorequal/saturation/yellow
None;midi:CC12=iop/colorequal/saturation/green
None;midi:CC13=iop/colorequal/saturation/cyan
None;midi:CC14=iop/colorequal/saturation/blue
None;midi:CC15=iop/colorequal/saturation/lavender
None;midi:CC16=iop/colorequal/saturation/magenta
None;midi:CC17=iop/colorequal/brightness/red
None;midi:CC18=iop/colorequal/brightness/orange
None;midi:CC19=iop/colorequal/brightness/yellow
None;midi:CC20=iop/colorequal/brightness/green
None;midi:CC21=iop/colorequal/brightness/cyan
None;midi:CC22=iop/colorequal/brightness/blue
None;midi:CC23=iop/colorequal/brightness/lavender
None;midi:CC24=iop/colorequal/brightness/magenta
*/

//#include "common/extra_optimizations.h" // results in crashes on some systems

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "bauhaus/bauhaus.h"
#include "common/chromatic_adaptation.h"
#include "common/darktable_ucs_22_helpers.h"
#include "common/darktable.h"
#include "common/eigf.h"
#include "common/interpolation.h"
#include "common/opencl.h"
#include "common/color_picker.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/blend.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "develop/imageop_gui.h"
#include "develop/tiling.h"
#include "dtgtk/drawingarea.h"
#include "dtgtk/expander.h"
#include "gui/accelerators.h"
#include "gui/color_picker_proxy.h"
#include "gui/draw.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "gui/color_picker_proxy.h"
#include "iop/iop_api.h"
#include "iop/choleski.h"
#include "common/colorspaces_inline_conversions.h"

#ifdef _OPENMP
#include <omp.h>
#endif

#define NODES 8
#define SATSIZE 4096
#define SLIDER_BRIGHTNESS 0.65f // 65 %

#define SAT_EFFECT 2.0f
#define BRIGHT_EFFECT 8.0f

DT_MODULE_INTROSPECTION(4, dt_iop_colorequal_params_t)

typedef struct dt_iop_colorequal_params_t
{
  float threshold;          // $MIN: 0.0 $MAX: 0.3 $DEFAULT: 0.1 $DESCRIPTION: "saturation threshold"
  float smoothing_hue;      // $MIN: 0.05 $MAX: 2.0 $DEFAULT: 1.0 $DESCRIPTION: "hue curve"
  float contrast;           // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "contrast"

  float white_level;        // $MIN: -2.0 $MAX: 16.0 $DEFAULT: 1.0 $DESCRIPTION: "white level"
  float chroma_size;        // $MIN: 1.0 $MAX: 10.0 $DEFAULT: 1.5 $DESCRIPTION: "hue analysis radius"
  float param_size;         // $MIN: 1.0 $MAX: 128. $DEFAULT: 1.0 $DESCRIPTION: "effect radius"
  gboolean use_filter;      // $DEFAULT: TRUE $DESCRIPTION: "use guided filter"

  // Note: what follows is tedious because each param needs to be declared separately.
  // A more efficient way would be to use 3 arrays of 8 elements,
  // but then GUI sliders would need to be wired manually to the correct array index.
  // So we do it the tedious way here, and let the introspection magic connect sliders to params automatically,
  // then we pack the params in arrays in commit_params().

  float sat_red;         // $MIN: 0. $MAX: 2. $DEFAULT: 1.0 $DESCRIPTION: "red"
  float sat_orange;      // $MIN: 0. $MAX: 2. $DEFAULT: 1.0 $DESCRIPTION: "orange"
  float sat_yellow;      // $MIN: 0. $MAX: 2. $DEFAULT: 1.0 $DESCRIPTION: "yellow"
  float sat_green;       // $MIN: 0. $MAX: 2. $DEFAULT: 1.0 $DESCRIPTION: "green"
  float sat_cyan;        // $MIN: 0. $MAX: 2. $DEFAULT: 1.0 $DESCRIPTION: "cyan"
  float sat_blue;        // $MIN: 0. $MAX: 2. $DEFAULT: 1.0 $DESCRIPTION: "blue"
  float sat_lavender;    // $MIN: 0. $MAX: 2. $DEFAULT: 1.0 $DESCRIPTION: "lavender"
  float sat_magenta;     // $MIN: 0. $MAX: 2. $DEFAULT: 1.0 $DESCRIPTION: "magenta"

  float hue_red;         // $MIN: -180. $MAX: 180. $DEFAULT: 0.0 $DESCRIPTION: "red"
  float hue_orange;      // $MIN: -180. $MAX: 180. $DEFAULT: 0.0 $DESCRIPTION: "orange"
  float hue_yellow;      // $MIN: -180. $MAX: 180. $DEFAULT: 0.0 $DESCRIPTION: "yellow"
  float hue_green;       // $MIN: -180. $MAX: 180. $DEFAULT: 0.0 $DESCRIPTION: "green"
  float hue_cyan;        // $MIN: -180. $MAX: 180. $DEFAULT: 0.0 $DESCRIPTION: "cyan"
  float hue_blue;        // $MIN: -180. $MAX: 180. $DEFAULT: 0.0 $DESCRIPTION: "blue"
  float hue_lavender;    // $MIN: -180. $MAX: 180. $DEFAULT: 0.0 $DESCRIPTION: "lavender"
  float hue_magenta;     // $MIN: -180. $MAX: 180. $DEFAULT: 0.0 $DESCRIPTION: "magenta"

  float bright_red;      // $MIN: 0. $MAX: 2. $DEFAULT: 1.0 $DESCRIPTION: "red"
  float bright_orange;   // $MIN: 0. $MAX: 2. $DEFAULT: 1.0 $DESCRIPTION: "orange"
  float bright_yellow;   // $MIN: 0. $MAX: 2. $DEFAULT: 1.0 $DESCRIPTION: "yellow"
  float bright_green;    // $MIN: 0. $MAX: 2. $DEFAULT: 1.0 $DESCRIPTION: "green"
  float bright_cyan;     // $MIN: 0. $MAX: 2. $DEFAULT: 1.0 $DESCRIPTION: "cyan"
  float bright_blue;     // $MIN: 0. $MAX: 2. $DEFAULT: 1.0 $DESCRIPTION: "blue"
  float bright_lavender; // $MIN: 0. $MAX: 2. $DEFAULT: 1.0 $DESCRIPTION: "lavender"
  float bright_magenta;  // $MIN: 0. $MAX: 2. $DEFAULT: 1.0 $DESCRIPTION: "magenta"

  float hue_shift;       // $MIN: -23. $MAX: 23. $DEFAULT: 0.0 $DESCRIPTION: "node placement"
} dt_iop_colorequal_params_t;

typedef enum dt_iop_colorequal_channel_t
{
  HUE = 0,
  SATURATION = 1,
  BRIGHTNESS = 2,
  NUM_CHANNELS = 3,
  GRAD_SWITCH = 4,
  SATURATION_GRAD = SATURATION + GRAD_SWITCH,
  BRIGHTNESS_GRAD = BRIGHTNESS + GRAD_SWITCH
} dt_iop_colorequal_channel_t;


typedef struct dt_iop_colorequal_data_t
{
  float *LUT_saturation;
  float *LUT_hue;
  float *LUT_brightness;
  float *gamut_LUT;
  gboolean lut_inited;
  float white_level;
  float chroma_size;
  float chroma_feathering;
  float param_size;
  float param_feathering;
  gboolean use_filter;
  dt_iop_order_iccprofile_info_t *work_profile;
  float hue_shift;
  float threshold;
  float max_brightness;
  float contrast;
} dt_iop_colorequal_data_t;

typedef struct dt_iop_colorequal_global_data_t
{
  int ce_init_covariance;
  int ce_finish_covariance;
  int ce_prepare_prefilter;
  int ce_apply_prefilter;
  int ce_prepare_correlations;
  int ce_finish_correlations;
  int ce_final_guide;
  int ce_apply_guided;
  int ce_sample_input;
  int ce_process_data;
  int ce_write_output;
  int ce_write_visual;
  int ce_draw_weight;
  int ce_bilinear1;
  int ce_bilinear2;
  int ce_bilinear4;
} dt_iop_colorequal_global_data_t;


const char *name()
{
  return _("color equalizer");
}

const char *aliases()
{
  return _("color zones|hsl");
}

const char **description(dt_iop_module_t *self)
{
  return dt_iop_set_description
    (self, _("change saturation, hue and brightness depending on local hue"),
     _("corrective and creative"),
     _("linear, RGB, scene-referred"),
     _("quasi-linear, RGB"),
     _("quasi-linear, RGB, scene-referred"));
}

int default_group()
{
  return IOP_GROUP_COLOR;
}

int flags()
{
  return IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING;
}

dt_iop_colorspace_type_t default_colorspace(dt_iop_module_t *self,
                                            dt_dev_pixelpipe_t *pipe,
                                            dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

typedef struct dt_iop_colorequal_gui_data_t
{
  GtkWidget *white_level;
  GtkWidget *sat_red, *sat_orange, *sat_yellow, *sat_green;
  GtkWidget *sat_cyan, *sat_blue, *sat_lavender, *sat_magenta;
  GtkWidget *hue_red, *hue_orange, *hue_yellow, *hue_green;
  GtkWidget *hue_cyan, *hue_blue, *hue_lavender, *hue_magenta;
  GtkWidget *bright_red, *bright_orange, *bright_yellow, *bright_green;
  GtkWidget *bright_cyan, *bright_blue, *bright_lavender, *bright_magenta;

  GtkWidget *smoothing_hue, *threshold, *contrast;
  GtkWidget *chroma_size, *param_size, *use_filter;

  GtkWidget *hue_shift;
  gboolean picking;

  // Array-like re-indexing of the above for efficient uniform
  // handling in loops. Populate the array in gui_init()
  GtkWidget *sat_sliders[NODES];
  GtkWidget *hue_sliders[NODES];
  GtkWidget *bright_sliders[NODES];
  int page_num;

  GtkNotebook *notebook;
  GtkDrawingArea *area;
  GtkStack *stack;
  dt_gui_collapsible_section_t cs;
  float *LUT;
  dt_iop_colorequal_channel_t channel;

  dt_iop_order_iccprofile_info_t *work_profile;
  dt_iop_order_iccprofile_info_t *white_adapted_profile;

  unsigned char *b_data[NUM_CHANNELS];
  cairo_surface_t *b_surface[NUM_CHANNELS];

  float graph_height;
  float max_saturation;
  gboolean gradients_cached;

  float *gamut_LUT;

  int mask_mode;
  gboolean dragging;
  gboolean on_node;
  int selected;
  float points[NODES+1][2];
} dt_iop_colorequal_gui_data_t;

void init_global(dt_iop_module_so_t *self)
{
  const int program = 37; // colorequal.cl, from programs.conf

  dt_iop_colorequal_global_data_t *gd = malloc(sizeof(dt_iop_colorequal_global_data_t));
  self->data = gd;

  gd->ce_init_covariance = dt_opencl_create_kernel(program, "init_covariance");
  gd->ce_finish_covariance = dt_opencl_create_kernel(program, "finish_covariance");
  gd->ce_prepare_prefilter = dt_opencl_create_kernel(program, "prepare_prefilter");
  gd->ce_apply_prefilter = dt_opencl_create_kernel(program, "apply_prefilter");
  gd->ce_prepare_correlations = dt_opencl_create_kernel(program, "prepare_correlations");
  gd->ce_finish_correlations = dt_opencl_create_kernel(program, "finish_correlations");
  gd->ce_final_guide = dt_opencl_create_kernel(program, "final_guide");
  gd->ce_apply_guided = dt_opencl_create_kernel(program, "apply_guided");
  gd->ce_sample_input = dt_opencl_create_kernel(program, "sample_input");
  gd->ce_process_data = dt_opencl_create_kernel(program, "process_data");
  gd->ce_write_output = dt_opencl_create_kernel(program, "write_output");
  gd->ce_write_visual = dt_opencl_create_kernel(program, "write_visual");
  gd->ce_draw_weight = dt_opencl_create_kernel(program, "draw_weight");
  gd->ce_bilinear1 = dt_opencl_create_kernel(program, "bilinear1");
  gd->ce_bilinear2 = dt_opencl_create_kernel(program, "bilinear2");
  gd->ce_bilinear4 = dt_opencl_create_kernel(program, "bilinear4");
}

void cleanup_global(dt_iop_module_so_t *self)
{
  dt_iop_colorequal_global_data_t *gd = self->data;
  dt_opencl_free_kernel(gd->ce_init_covariance);
  dt_opencl_free_kernel(gd->ce_finish_covariance);
  dt_opencl_free_kernel(gd->ce_prepare_prefilter);
  dt_opencl_free_kernel(gd->ce_apply_prefilter);
  dt_opencl_free_kernel(gd->ce_prepare_correlations);
  dt_opencl_free_kernel(gd->ce_finish_correlations);
  dt_opencl_free_kernel(gd->ce_final_guide);
  dt_opencl_free_kernel(gd->ce_apply_guided);
  dt_opencl_free_kernel(gd->ce_sample_input);
  dt_opencl_free_kernel(gd->ce_process_data);
  dt_opencl_free_kernel(gd->ce_write_output);
  dt_opencl_free_kernel(gd->ce_write_visual);
  dt_opencl_free_kernel(gd->ce_draw_weight);
  dt_opencl_free_kernel(gd->ce_bilinear1);
  dt_opencl_free_kernel(gd->ce_bilinear2);
  dt_opencl_free_kernel(gd->ce_bilinear4);

  free(self->data);
  self->data = NULL;
}


static inline float _get_scaling(const float sigma)
{
  return MAX(1.0f, MIN(4.0f, floorf(sigma - 1.5f)));
}

void tiling_callback(dt_iop_module_t *self,
                     dt_dev_pixelpipe_iop_t *piece,
                     const dt_iop_roi_t *roi_in,
                     const dt_iop_roi_t *roi_out,
                     dt_develop_tiling_t *tiling)
{
  dt_iop_colorequal_data_t *data = piece->data;

  tiling->maxbuf = 1.0f;
  tiling->xalign = 1;
  tiling->yalign = 1;
  tiling->overhead = (2 * SATSIZE + 4 * LUT_ELEM) * sizeof(float);
  const int maxradius = MAX(data->chroma_size, data->param_size);
  tiling->overlap = 16 + maxradius; // safe feathering

  tiling->factor = 4.5f;  // in/out buffers plus mainloop incl gaussian
  if(data->use_filter)
  {
    // calculate relative size of downsampled buffers
    const float sigma = (float)maxradius * MAX(0.5f, roi_in->scale / piece->iscale);
    const float scaling = _get_scaling(sigma);
    tiling->factor += scaling == 1.0f
                      ? 3.0f
                      : (1.0f + 4.0f / sqrf(scaling));
  }
}


int legacy_params(dt_iop_module_t *self,
                  const void *const old_params,
                  const int old_version,
                  void **new_params,
                  int32_t *new_params_size,
                  int *new_version)
{
  if(old_version == 1)
  {
    const dt_iop_colorequal_params_t *o = old_params;
    dt_iop_colorequal_params_t *n = malloc(sizeof(dt_iop_colorequal_params_t));

    memcpy(n, o, sizeof(dt_iop_colorequal_params_t) - sizeof(float));
    n->hue_shift = 0.0f;
    *new_params = n;
    *new_params_size = sizeof(dt_iop_colorequal_params_t);
    *new_version = 2;
    return 0;
  }

  if(old_version == 2)
  {
    const dt_iop_colorequal_params_t *o = old_params;
    dt_iop_colorequal_params_t *n = malloc(sizeof(dt_iop_colorequal_params_t));

    memcpy(n, o, sizeof(dt_iop_colorequal_params_t) - sizeof(float));
    n->threshold = 0.024f;  // in v1/2 we had an inflection point of 0.1

    // brightness and saturation slider ranges have been expanded by 4:3 so we correct here
    const float *sodata = &o->sat_red;
    const float *bodata = &o->bright_red;
    float *sndata = &n->sat_red;
    float *bndata = &n->bright_red;
    for(int i = 0; i < NODES; i++)
    {
      sndata[i] = 1.0f + 0.75f * (sodata[i] - 1.0f);
      bndata[i] = 1.0f + 0.75f * (bodata[i] - 1.0f);
    }

    *new_params = n;
    *new_params_size = sizeof(dt_iop_colorequal_params_t);
    *new_version = 3;
    return 0;
  }

  if(old_version == 3)
  {
    const dt_iop_colorequal_params_t *o = old_params;
    dt_iop_colorequal_params_t *n = malloc(sizeof(dt_iop_colorequal_params_t));

    memcpy(n, o, sizeof(dt_iop_colorequal_params_t) - sizeof(float));
    n->threshold = o->threshold + 0.1f;
    n->contrast = -5.0f * MAX(0.0f, o->threshold - 0.024f); // sort of magic from what we had

    *new_params = n;
    *new_params_size = sizeof(dt_iop_colorequal_params_t);
    *new_version = 4;
    return 0;
  }

  return 1;
}

void _mean_gaussian(float *const buf,
                    const int width,
                    const int height,
                    const uint32_t ch,
                    const float sigma)
{
  // We use unbounded signals, so don't care for the internal value clipping
  const float range = 1.0e9;
  const dt_aligned_pixel_t max = {range, range, range, range};
  const dt_aligned_pixel_t min = {-range, -range, -range, -range};
  dt_gaussian_t *g = dt_gaussian_init(width, height, ch, max, min, sigma, DT_IOP_GAUSSIAN_ZERO);
  if(!g) return;
  if(ch == 4)
    dt_gaussian_blur_4c(g, buf, buf);
  else
    dt_gaussian_blur(g, buf, buf);
  dt_gaussian_free(g);
}


// sRGB primary red records at 20° of hue in darktable UCS 22, so we offset the whole hue range
// such that red is the origin hues in the GUI. This is consistent with HSV/HSL color wheels UI.
#define ANGLE_SHIFT +20.f
static inline float _deg_to_rad(const float angle)
{
  return (angle + ANGLE_SHIFT) * M_PI_F / 180.f;
}

/* We use precalculated data for the logistic weighting function for performance and stability
   and do linear interpolation at runtime. Avoids banding effects and allows a sharp transition.
*/
static float satweights[2 * SATSIZE + 1];
static float lastcontrast = NAN;
static void _init_satweights(const float contrast)
{
  if(lastcontrast == contrast)
    return;
  lastcontrast = contrast;
  const double factor = -60.0 - 40.0 * (double)contrast;
  for(int i = -SATSIZE; i < SATSIZE + 1; i++)
  {
    const double val = 0.5 / (double)SATSIZE * (double)i;
    satweights[i+SATSIZE] = (float)(1.0 / (1.0 + exp(factor * val)));
  }
}

static inline float _get_satweight(const float sat)
{
  const float isat = (float)SATSIZE * (1.0f + CLAMP(sat, -1.0f, 1.0f-(1.0f/SATSIZE)));
  const float base = floorf(isat);
  const int i = base;
  return satweights[i] + (isat - base) * (satweights[i+1] - satweights[i]);
}

DT_OMP_DECLARE_SIMD(aligned(UV))
static float *const _init_covariance(const size_t pixels, const float *const restrict UV)
{
  // Init the symmetric covariance matrix of the guide (4 elements by pixel) :
  // covar = [[ covar(U, U), covar(U, V)],
  //          [ covar(V, U), covar(V, V)]]
  // with covar(x, y) = avg(x * y) - avg(x) * avg(y), corr(x, y) = x * y
  // so here, we init it with x * y, compute all the avg() at the next step
  // and subtract avg(x) * avg(y) later
  float *const restrict covariance = dt_alloc_align_float(pixels * 4);
  if(!covariance)
    return covariance;

  DT_OMP_FOR()
  for(size_t k = 0; k < pixels; k++)
  {
    // corr(U, U)
    covariance[4 * k + 0] = UV[2 * k + 0] * UV[2 * k + 0];
    // corr(U, V)
    covariance[4 * k + 1] = covariance[4 * k + 2] = UV[2 * k] * UV[2 * k + 1];
    // corr(V, V)
    covariance[4 * k + 3] = UV[2 * k + 1] * UV[2 * k + 1];
  }
  return covariance;
}

DT_OMP_DECLARE_SIMD(aligned(UV, covariance: 64))
static void _finish_covariance(const size_t pixels,
                               const float *const restrict UV,
                               float *const restrict covariance)
{
  // Finish the UV covariance matrix computation by subtracting avg(x) * avg(y)
  // to avg(x * y) already computed
  DT_OMP_FOR()
  for(size_t k = 0; k < pixels; k++)
  {
    // covar(U, U) = var(U)
    covariance[4 * k + 0] -= UV[2 * k + 0] * UV[2 * k + 0];
    // covar(U, V)
    covariance[4 * k + 1] -= UV[2 * k + 0] * UV[2 * k + 1];
    covariance[4 * k + 2] -= UV[2 * k + 0] * UV[2 * k + 1];
    // covar(V, V) = var(V)
    covariance[4 * k + 3] -= UV[2 * k + 1] * UV[2 * k + 1];
  }
}

DT_OMP_DECLARE_SIMD(aligned(UV, covariance, a, b: 64))
static void _prepare_prefilter(const size_t pixels,
                               const float *const restrict UV,
                               const float *const restrict covariance,
                               float *const restrict a,
                               float *const restrict b,
                               const float eps)
{
  DT_OMP_FOR()
  for(size_t k = 0; k < pixels; k++)
  {
    // Extract the 2×2 covariance matrix sigma = cov(U, V) at current pixel
    // and add the variance threshold : sigma' = sigma + epsilon * Identity
    dt_aligned_pixel_t Sigma = {covariance[4 * k + 0] + eps,
                                covariance[4 * k + 1],
                                covariance[4 * k + 2],
                                covariance[4 * k + 3] + eps};

    // Invert the 2×2 sigma matrix algebraically
    // see https://www.mathcentre.ac.uk/resources/uploaded/sigma-matrices7-2009-1.pdf
    const float det = Sigma[0] * Sigma[3] - Sigma[1] * Sigma[2];

    // a(chan) = dot_product(cov(chan, uv), sigma_inv)
    if(fabsf(det) > 4.f * FLT_EPSILON)
    {
      dt_aligned_pixel_t sigma_inv = { Sigma[3] / det, -Sigma[1] / det,
                                      -Sigma[2] / det,  Sigma[0] / det };
      // find a_1, a_2 s.t. U' = a_1 * U + a_2 * V
      a[4 * k + 0] = (covariance[4 * k + 0] * sigma_inv[0]
                    + covariance[4 * k + 1] * sigma_inv[1]);
      a[4 * k + 1] = (covariance[4 * k + 0] * sigma_inv[2]
                    + covariance[4 * k + 1] * sigma_inv[3]);

      // find a_3, a_4 s.t. V' = a_3 * U + a_4 V
      a[4 * k + 2] = (covariance[4 * k + 2] * sigma_inv[0]
                    + covariance[4 * k + 3] * sigma_inv[1]);
      a[4 * k + 3] = (covariance[4 * k + 2] * sigma_inv[2]
                    + covariance[4 * k + 3] * sigma_inv[3]);
    }
    else
    {
      // determinant too close to 0: singular matrix
      a[4 * k + 0] = a[4 * k + 1] = a[4 * k + 2] = a[4 * k + 3] = 0.f;
    }

    b[2 * k + 0] = UV[2 * k + 0]  - a[4 * k + 0] * UV[2 * k + 0]  - a[4 * k + 1] * UV[2 * k + 1];
    b[2 * k + 1] = UV[2 * k + 1]  - a[4 * k + 2] * UV[2 * k + 0]  - a[4 * k + 3] * UV[2 * k + 1];
  }
}

DT_OMP_DECLARE_SIMD(aligned(a, b, saturation, UV: 64))
static void _apply_prefilter(size_t npixels,
                             float sat_shift,
                             float *const restrict UV,
                             const float *const restrict saturation,
                             const float *const restrict a,
                             const float *const restrict b)
{
  DT_OMP_FOR_SIMD()
  for(size_t k = 0; k < npixels; k++)
  {
    // For each correction factor, we re-express it as a[0] * U + a[1] * V + b
    const float uv[2] = { UV[2 * k + 0], UV[2 * k + 1] };
    const float cv[2] = { a[4 * k + 0] * uv[0] + a[4 * k + 1] * uv[1] + b[2 * k + 0],
                          a[4 * k + 2] * uv[0] + a[4 * k + 3] * uv[1] + b[2 * k + 1] };

    // we avoid chroma blurring into achromatic areas by interpolating
    // input UV vs corrected UV
    const float satweight = _get_satweight(saturation[k] - sat_shift);
    UV[2 * k + 0] = interpolatef(satweight, cv[0], uv[0]);
    UV[2 * k + 1] = interpolatef(satweight, cv[1], uv[1]);
  }
}

static void _prefilter_chromaticity(float *const restrict UV,
                                    float *const restrict saturation,
                                    const int width,
                                    const int height,
                                    const float sigma,
                                    const float eps,
                                    const float sat_shift)
{
  // We guide the 3-channels corrections with the 2-channels
  // chromaticity coordinates UV aka we express corrections = a * UV +
  // b where a is a 2×2 matrix and b a constant Therefore the guided
  // filter computation is a bit more complicated than the typical
  // 1-channel case.  We use by-the-book 3-channels fast guided filter
  // as in http://kaiminghe.com/eccv10/ but obviously reduced to 2.
  // We know that it tends to oversmooth the input where its intensity
  // is close to 0, but this is actually desirable here since
  // chromaticity -> 0 means neutral greys and we want to discard them
  // as much as possible from any color equalization.

  // possibly downsample for speed-up
  const size_t pixels = (size_t)width * height;
  const float scaling = _get_scaling(sigma);
  const float gsigma = MAX(0.2f, sigma / scaling);
  const int ds_height = height / scaling;
  const int ds_width = width / scaling;
  const size_t ds_pixels = (size_t)ds_width * ds_height;
  const gboolean resized = width != ds_width || height != ds_height;

  float *ds_UV = UV;
  if(resized)
  {
    ds_UV = dt_alloc_align_float(ds_pixels * 2);
    if(!ds_UV)
      return;	//out of memory, can't run the prefilter
    interpolate_bilinear(UV, width, height, ds_UV, ds_width, ds_height, 2);
  }

  float *const restrict covariance = _init_covariance(ds_pixels, ds_UV);
  if(!covariance)
  {
    if(ds_UV != UV) dt_free_align(ds_UV);
    return;
  }

  // Compute the local averages of everything over the window size We
  // use a gaussian blur as a weighted local average because it's a
  // radial function so it will not favour vertical and horizontal
  // edges over diagonal ones as the by-the-book box blur (unweighted
  // local average) would.

  _mean_gaussian(ds_UV, ds_width, ds_height, 2, gsigma);
  _mean_gaussian(covariance, ds_width, ds_height, 4, gsigma);

  _finish_covariance(ds_pixels, ds_UV, covariance);

  // Compute a and b the params of the guided filters
  float *const restrict ds_a = dt_alloc_align_float(4 * ds_pixels);
  float *const restrict ds_b = dt_alloc_align_float(2 * ds_pixels);

  if(ds_a && ds_b)
    _prepare_prefilter(ds_pixels, ds_UV, covariance, ds_a, ds_b, eps);

  dt_free_align(covariance);
  if(ds_UV != UV) dt_free_align(ds_UV);
  if(!ds_a || !ds_b)
  {
    dt_free_align(ds_a);
    dt_free_align(ds_b);
    return;
  }

  // Compute the averages of a and b for each filter
  _mean_gaussian(ds_a, ds_width, ds_height, 4, gsigma);
  _mean_gaussian(ds_b, ds_width, ds_height, 2, gsigma);

  // Upsample a and b to real-size image
  float *a = ds_a;
  float *b = ds_b;

  if(resized)
  {
    a = dt_alloc_align_float(pixels * 4);
    b = dt_alloc_align_float(pixels * 2);
    if(a && b)
    {
      interpolate_bilinear(ds_a, ds_width, ds_height, a, width, height, 4);
      interpolate_bilinear(ds_b, ds_width, ds_height, b, width, height, 2);
      dt_free_align(ds_a);
      dt_free_align(ds_b);
    }
    else
    {
      dt_free_align(ds_a);
      dt_free_align(ds_b);
      return;
    }
  }

  // Apply the guided filter
  _apply_prefilter(pixels, sat_shift, UV, saturation, a, b);

  dt_free_align(a);
  dt_free_align(b);
}

static void _guide_with_chromaticity(float *const restrict UV,
                              float *const restrict corrections,
                              float *const restrict saturation,
                              float *const restrict b_corrections,
                              float *const restrict gradients,
                              const int width,
                              const int height,
                              const float sigma,
                              const float eps,
                              const float bright_shift,
                              const float sat_shift)
{
  // We guide the 3-channels corrections with the 2-channels
  // chromaticity coordinates UV aka we express corrections = a * UV +
  // b where a is a 2×2 matrix and b a constant Therefore the guided
  // filter computation is a bit more complicated than the typical
  // 1-channel case.  We use by-the-book 3-channels fast guided filter
  // as in http://kaiminghe.com/eccv10/ but obviously reduced to 2.
  // We know that it tends to oversmooth the input where its intensity
  // is close to 0, but this is actually desirable here since
  // chromaticity -> 0 means neutral greys and we want to discard them
  // as much as possible from any color equalization.

  // Downsample for speed-up
  const size_t pixels = (size_t)width * height;
  const float scaling = _get_scaling(sigma);
  const float gsigma = MAX(0.2f, sigma / scaling);
  const int ds_height = height / scaling;
  const int ds_width = width / scaling;
  const size_t ds_pixels = (size_t)ds_width * ds_height;
  const gboolean resized = width != ds_width || height != ds_height;

  float *ds_UV = UV;
  float *ds_corrections = corrections;
  float *ds_b_corrections = b_corrections;
  if(resized)
  {
    ds_UV = dt_alloc_align_float(ds_pixels * 2);
    ds_corrections = dt_alloc_align_float(ds_pixels * 2);
    ds_b_corrections = dt_alloc_align_float(ds_pixels);
    if(ds_UV && ds_corrections && ds_b_corrections)
    {
      interpolate_bilinear(UV, width, height, ds_UV, ds_width, ds_height, 2);
      interpolate_bilinear(corrections, width, height, ds_corrections, ds_width, ds_height, 2);
      interpolate_bilinear(b_corrections, width, height, ds_b_corrections, ds_width, ds_height, 1);
    }
    else
    {
      dt_free_align(ds_UV);
      dt_free_align(ds_corrections);
      dt_free_align(ds_b_corrections);
      return;
    }
  }

  float *const restrict covariance = _init_covariance(ds_pixels, ds_UV);
  // Get the correlations between corrections and UV
  float *const restrict correlations = dt_alloc_align_float(ds_pixels * 4);
  if(!covariance || !correlations)
  {
    // ran out of memory, so we won't be able to apply the guided filter.
    // clean up and return now
    if(resized)
    {
      dt_free_align(ds_UV);
      dt_free_align(ds_corrections);
      dt_free_align(ds_b_corrections);
    }
    dt_free_align(covariance);
    dt_free_align(correlations);
    return;
  }

  DT_OMP_FOR_SIMD(aligned(ds_UV, ds_corrections, ds_b_corrections, correlations: 64))
  for(size_t k = 0; k < ds_pixels; k++)
  {
    // corr(sat, U)
    correlations[4 * k + 0] = ds_UV[2 * k + 0] * ds_corrections[2 * k + 1];
    // corr(sat, V)
    correlations[4 * k + 1] = ds_UV[2 * k + 1] * ds_corrections[2 * k + 1];

    // corr(bright, U)
    correlations[4 * k + 2] = ds_UV[2 * k + 0] * ds_b_corrections[k];
    // corr(bright, V)
    correlations[4 * k + 3] = ds_UV[2 * k + 1] * ds_b_corrections[k];
  }

  // Compute the local averages of everything over the window size We
  // use a gaussian blur as a weighted local average because it's a
  // radial function so it will not favour vertical and horizontal
  // edges over diagonal ones as the by-the-book box blur (unweighted
  // local average) would.
  // We use unbounded signals, so don't care for the internal value clipping
  _mean_gaussian(ds_UV, ds_width, ds_height, 2, gsigma);
  _mean_gaussian(covariance, ds_width, ds_height, 4, gsigma);
  _mean_gaussian(ds_corrections, ds_width, ds_height, 2, gsigma);
  _mean_gaussian(ds_b_corrections, ds_width, ds_height, 1, 0.1f * gsigma);
  _mean_gaussian(correlations, ds_width, ds_height, 4, gsigma);

  _finish_covariance(ds_pixels, ds_UV, covariance);

  // Finish the guide * guided correlation computation
  DT_OMP_FOR_SIMD(aligned(ds_UV, ds_corrections, correlations: 64))
  for(size_t k = 0; k < ds_pixels; k++)
  {
    correlations[4 * k + 0] -= ds_UV[2 * k + 0] * ds_corrections[2 * k + 1];
    correlations[4 * k + 1] -= ds_UV[2 * k + 1] * ds_corrections[2 * k + 1];

    correlations[4 * k + 2] -= ds_UV[2 * k + 0] * ds_b_corrections[k];
    correlations[4 * k + 3] -= ds_UV[2 * k + 1] * ds_b_corrections[k];
  }

  // Compute a and b the params of the guided filters
  float *const restrict ds_a = dt_alloc_align_float(4 * ds_pixels);
  float *const restrict ds_b = dt_alloc_align_float(2 * ds_pixels);
  if (!ds_a || !ds_b)
  {
    dt_free_align(ds_a);
    dt_free_align(ds_b);
    dt_free_align(correlations);
    dt_free_align(covariance);
    if(resized)
    {
      dt_free_align(ds_corrections);
      dt_free_align(ds_b_corrections);
      dt_free_align(ds_UV);
    }
    return;
  }

  DT_OMP_FOR_SIMD(aligned(ds_UV, covariance, correlations, ds_corrections, ds_b_corrections, ds_a, ds_b: 64))
  for(size_t k = 0; k < ds_pixels; k++)
  {
    // Extract the 2×2 covariance matrix sigma = cov(U, V) at current pixel
    // and add the covariance threshold : sigma' = sigma + epsilon * Identity
    const dt_aligned_pixel_t Sigma
        = { covariance[4 * k + 0] + eps,
            covariance[4 * k + 1],
            covariance[4 * k + 2],
            covariance[4 * k + 3] + eps };

    // Invert the 2×2 sigma matrix algebraically
    // see https://www.mathcentre.ac.uk/resources/uploaded/sigma-matrices7-2009-1.pdf
    const float det = Sigma[0] * Sigma[3] - Sigma[1] * Sigma[2];
    // Note : epsilon prevents determinant == 0 so the invert exists all the time
    if(fabsf(det) > 4.f * FLT_EPSILON)
    {
      const dt_aligned_pixel_t sigma_inv = { Sigma[3] / det, -Sigma[1] / det, -Sigma[2] / det, Sigma[0] / det };
      ds_a[4 * k + 0] = (correlations[4 * k + 0] * sigma_inv[0] + correlations[4 * k + 1] * sigma_inv[1]);
      ds_a[4 * k + 1] = (correlations[4 * k + 0] * sigma_inv[2] + correlations[4 * k + 1] * sigma_inv[3]);
      ds_a[4 * k + 2] = (correlations[4 * k + 2] * sigma_inv[0] + correlations[4 * k + 3] * sigma_inv[1]);
      ds_a[4 * k + 3] = (correlations[4 * k + 2] * sigma_inv[2] + correlations[4 * k + 3] * sigma_inv[3]);
    }
    else
    {
      ds_a[4 * k + 0] = ds_a[4 * k + 1] = ds_a[4 * k + 2] = ds_a[4 * k + 3] = 0.f;
    }
    // b = avg(chan) - dot_product(a_chan * avg(UV))
    ds_b[2 * k + 0] = ds_corrections[2 * k + 1] - ds_a[4 * k + 0] * ds_UV[2 * k + 0]  - ds_a[4 * k + 1] * ds_UV[2 * k + 1];
    ds_b[2 * k + 1] = ds_b_corrections[k]       - ds_a[4 * k + 2] * ds_UV[2 * k + 0]  - ds_a[4 * k + 3] * ds_UV[2 * k + 1];
  }

  if(resized)
  {
    dt_free_align(ds_corrections);
    dt_free_align(ds_b_corrections);
    dt_free_align(ds_UV);
  }
  dt_free_align(correlations);
  dt_free_align(covariance);

  // Compute the averages of a and b for each filter and blur
  _mean_gaussian(ds_a, ds_width, ds_height, 4, gsigma);
  _mean_gaussian(ds_b, ds_width, ds_height, 2, gsigma);

  // Upsample a and b to real-size image
  float *a = ds_a;
  float *b = ds_b;
  if(resized)
  {
    a = dt_alloc_align_float(pixels * 4);
    b = dt_alloc_align_float(pixels * 2);
    if(a && b)
    {
      interpolate_bilinear(ds_a, ds_width, ds_height, a, width, height, 4);
      interpolate_bilinear(ds_b, ds_width, ds_height, b, width, height, 2);
      dt_free_align(ds_a);
      dt_free_align(ds_b);
    }
    else
    {
      dt_free_align(ds_a);
      dt_free_align(ds_b);
      return;
    }
  }

  // Apply the guided filter
  DT_OMP_FOR_SIMD(aligned(a, b, corrections, saturation, gradients, UV: 64))
  for(size_t k = 0; k < pixels; k++)
  {
    // For each correction factor, we re-express it as a[0] * U + a[1] * V + b
    const float uv[2] = { UV[2 * k + 0], UV[2 * k + 1] };
    const float cv[2] = { a[4 * k + 0] * uv[0] + a[4 * k + 1] * uv[1] + b[2 * k + 0],
                          a[4 * k + 2] * uv[0] + a[4 * k + 3] * uv[1] + b[2 * k + 1] };
    corrections[2 * k + 1] = interpolatef(_get_satweight(saturation[k] - sat_shift), cv[0], 1.0f);
    const float gradient_weight = 1.0f - CLIP(gradients[k]);
    b_corrections[k] = interpolatef(gradient_weight * _get_satweight(saturation[k] - bright_shift), cv[1], 0.0f);
  }
  dt_free_align(a);
  dt_free_align(b);
}

static void _prepare_process(const float roi_scale,
                             dt_iop_colorequal_data_t *d,

                             // parameters to be setup
                             float *white,
                             float *sat_shift,
                             float *max_brightness_shift,
                             float *corr_max_brightness_shift,
                             float *bright_shift,
                             float *gradient_amp,
                             float *hue_sigma,
                             float *par_sigma,
                             float *sat_sigma,
                             float *scharr_sigma)
{
  *white = Y_to_dt_UCS_L_star(d->white_level);

  /* We use the logistic weighting function to diminish effects in the guided filter for locations
     with low chromacity. The logistic function is precalculated for a inflection point of zero
     so we have to shift the input value (saturation) for both brightness and saturation corrections.
     The default can be shifted by the threshold slider.
     Depending on the maximum for the eight brightness sliders we increase the brightness shift, the value
     of 0.01 has been found by a lot of testing to be safe.
     As increased param_size leads to propagation of brightness into achromatic parts we have to correct for that too.
  */
  *sat_shift = d->threshold;
  *max_brightness_shift = 0.01f * d->max_brightness;
  *corr_max_brightness_shift = *max_brightness_shift * MIN(5.0f, sqrtf(d->param_size));
  *bright_shift = *sat_shift + *corr_max_brightness_shift;

  /* We want information about sharp transitions of saturation for halo suppression.
     As the scharr operator is faster and more stable for scale changes we use
       it instead of local variance.
     We reduce chroma noise effects by using a minimum threshold and by sqaring the gradient.
     The gradient_amp corrects a gradient of 0.5 to be 1.0 and takes care
       of maximum changed brightness and roi_scale.
  */
  *gradient_amp = 4.0f * sqrtf(d->max_brightness) * sqrf(roi_scale);
  *hue_sigma = 0.5f * d->chroma_size * roi_scale;
  *par_sigma = 0.5f * d->param_size * roi_scale;
  *sat_sigma = MAX(0.5f, roi_scale);
  *scharr_sigma = MAX(0.5f, roi_scale);

  _init_satweights(d->contrast);
}

void process(dt_iop_module_t *self,
             dt_dev_pixelpipe_iop_t *piece,
             const void *const i,
             void *const o,
             const dt_iop_roi_t *const roi_in,
             const dt_iop_roi_t *const roi_out)
{
  if(!dt_iop_have_required_input_format(4 /*we need full-color pixels*/,
                                        self, piece->colors, i, o, roi_in, roi_out))
    return;

  const int width = roi_out->width;
  const int height = roi_out->height;
  const size_t npixels = (size_t)width * height;
  float *restrict UV = NULL;
  float *restrict corrections = NULL;
  float *restrict b_corrections = NULL;
  float *restrict Lscharr = NULL;
  float *restrict saturation = NULL;
  if(!dt_iop_alloc_image_buffers(self, roi_in, roi_out,
                                 2/*ch per pix*/ | DT_IMGSZ_OUTPUT | DT_IMGSZ_FULL, &UV,
                                 2               | DT_IMGSZ_OUTPUT | DT_IMGSZ_FULL, &corrections,
                                 1               | DT_IMGSZ_OUTPUT | DT_IMGSZ_FULL, &b_corrections,
                                 1               | DT_IMGSZ_OUTPUT | DT_IMGSZ_FULL, &Lscharr,
                                 1               | DT_IMGSZ_OUTPUT | DT_IMGSZ_FULL, &saturation,
                                 0/*end of list*/))
  {
    // Uh oh, we didn't have enough memory!  Any buffers that had
    // already been allocated have been freed, and the module's
    // trouble flag has been set.  We can simply pass through the
    // input image and return now, since there isn't anything else we
    // need to clean up at this point.
    dt_iop_copy_image_roi(o, i, piece->colors, roi_in, roi_out);
    return;
  }
  dt_iop_colorequal_data_t *d = piece->data;
  dt_iop_colorequal_gui_data_t *g = self->gui_data;
  const gboolean fullpipe = piece->pipe->type & DT_DEV_PIXELPIPE_FULL;
  const int mask_mode = g && fullpipe ? g->mask_mode : 0;
  const gboolean run_fast = piece->pipe->type & DT_DEV_PIXELPIPE_FAST;

  const float *const restrict in = (float*)i;
  float *const restrict out = (float*)o;

  // STEP 0: prepare the RGB <-> XYZ D65 matrices
  // see colorbalancergb.c process() for the details, it's exactly the same
  const struct dt_iop_order_iccprofile_info_t *const work_profile =
    dt_ioppr_get_pipe_current_profile_info(self, piece->pipe);
  if(work_profile == NULL) return; // no point

  dt_colormatrix_t input_matrix;
  dt_colormatrix_t output_matrix;
  dt_colormatrix_mul(input_matrix, XYZ_D50_to_D65_CAT16, work_profile->matrix_in);
  dt_colormatrix_mul(output_matrix, work_profile->matrix_out, XYZ_D65_to_D50_CAT16);

  float white, sat_shift, max_brightness_shift, corr_max_brightness_shift, bright_shift, gradient_amp, hue_sigma, par_sigma, sat_sigma, scharr_sigma;
  _prepare_process(roi_in->scale / piece->iscale, d,
    &white, &sat_shift, &max_brightness_shift, &corr_max_brightness_shift, &bright_shift, &gradient_amp, &hue_sigma, &par_sigma, &sat_sigma, &scharr_sigma);

  // STEP 1: convert image from RGB to darktable UCS LUV and calc saturation
  DT_OMP_FOR()
  for(size_t k = 0; k < npixels; k++)
  {
    const float *const restrict pix_in = DT_IS_ALIGNED_PIXEL(in + k * 4);
    float *const restrict uv = UV + k * 2;

    // Convert to XYZ D65
    dt_aligned_pixel_t XYZ_D65 = { 0.0f, 0.0f, 0.0f, 0.0f };
    dot_product(pix_in, input_matrix, XYZ_D65);
    // Convert to dt UCS 22 UV and store UV
    dt_aligned_pixel_t xyY = { 0.0f, 0.0f, 0.0f, 0.0f };
    dt_D65_XYZ_to_xyY(XYZ_D65, xyY);

    // calc saturation from input data
    const float dmin = fminf(pix_in[0], fminf(pix_in[1], pix_in[2]));
    const float dmax = fmaxf(pix_in[0], fmaxf(pix_in[1], pix_in[2]));
    const float delta = dmax - dmin;
    saturation[k] = (dmax > NORM_MIN && delta > NORM_MIN) ? delta / dmax : 0.0f;

    xyY_to_dt_UCS_UV(xyY, uv);
    Lscharr[k] = Y_to_dt_UCS_L_star(xyY[2]);
  }

  _mean_gaussian(saturation, width, height, 1, sat_sigma);

  // STEP 2 : smoothen UV to avoid discontinuities in hue
  if(d->use_filter && !run_fast)
    _prefilter_chromaticity(UV, saturation, width, height, hue_sigma, d->chroma_feathering, sat_shift);

  // STEP 3 : carry-on with conversion from LUV to HSB
  DT_OMP_FOR()
  for(int row = 0; row < height; row++)
  {
    for(int col = 0; col < width; col++)
    {
      const size_t k = (size_t)row * width + col;

      const float *const restrict pix_in = DT_IS_ALIGNED_PIXEL(in + k * 4);
      float *const restrict pix_out = DT_IS_ALIGNED_PIXEL(out + k * 4);
      float *const restrict corrections_out = corrections + k * 2;

      float *const restrict uv = UV + k * 2;

      // Finish the conversion to dt UCS JCH then HSB
      dt_aligned_pixel_t JCH = { 0.0f, 0.0f, 0.0f, 0.0f };
      dt_UCS_LUV_to_JCH(Lscharr[k], white, uv, JCH);
      dt_UCS_JCH_to_HSB(JCH, pix_out);

      // As tmp[k] is not used any longer as L(uminance) we re-use it for the saturation gradient
      if(d->use_filter)
      {
        const int vrow = MIN(height - 2, MAX(1, row));
        const int vcol = MIN(width - 2, MAX(1, col));
        const size_t kk = vrow * width + vcol;
        Lscharr[k] = gradient_amp * sqrf(MAX(0.0f, scharr_gradient(&saturation[kk], width) - 0.02f));
      }

      // Get the boosts - if chroma = 0, we have a neutral grey so set everything to 0
      if(JCH[1] > NORM_MIN)
      {
        const float hue = pix_out[0];
        const float sat = pix_out[1];
        corrections_out[0] = lookup_gamut(d->LUT_hue, hue);
        corrections_out[1] = lookup_gamut(d->LUT_saturation, hue);
        b_corrections[k] = sat * (lookup_gamut(d->LUT_brightness, hue) - 1.0f);
      }
      else
      {
        corrections_out[0] = 0.0f;
        corrections_out[1] = 1.0f;
        b_corrections[k] = 0.0f;
      }

      // Copy alpha
      pix_out[3] = pix_in[3];
    }
  }

  if(d->use_filter && !run_fast)
  {
    // blur the saturation gradients
    _mean_gaussian(Lscharr, width, height, 1, scharr_sigma);

    // STEP 4: apply a guided filter on the corrections, guided with UV chromaticity, to ensure spatially-contiguous corrections.
    // Even if the hue is not perfectly constant this will help avoiding chroma noise.
    _guide_with_chromaticity(UV, corrections, saturation, b_corrections, Lscharr, width, height, par_sigma, d->param_feathering, bright_shift, sat_shift);
  }

  if(mask_mode == 0)
  {
    // STEP 5: apply the corrections and convert back to RGB
    DT_OMP_FOR()
    for(size_t k = 0; k < npixels; k++)
    {
      const float *const restrict corrections_out = corrections + k * 2;
      float *const restrict pix_out = DT_IS_ALIGNED_PIXEL(out + k * 4);

      // Apply the corrections
      pix_out[0] += corrections_out[0]; // WARNING: hue is an offset
      // pix_out[1] (saturation) and pix_out[2] (brightness) are gains
      pix_out[1] = MAX(0.0f, pix_out[1] * (1.0f + SAT_EFFECT * (corrections_out[1] - 1.0f)));
      pix_out[2] = MAX(0.0f, pix_out[2] * (1.0f + BRIGHT_EFFECT * b_corrections[k]));

      // Sanitize gamut
      gamut_map_HSB(pix_out, d->gamut_LUT, white);

      // Convert back to XYZ D65
      dt_aligned_pixel_t XYZ_D65 = { 0.f };
      dt_UCS_HSB_to_XYZ(pix_out, white, XYZ_D65);

      // And back to pipe RGB through XYZ D50
      dot_product(XYZ_D65, output_matrix, pix_out);
    }
  }
  else
  {
    piece->pipe->mask_display = DT_DEV_PIXELPIPE_DISPLAY_PASSTHRU;
    const int mode = mask_mode - 1;
    DT_OMP_FOR()
    for(size_t k = 0; k < npixels; k++)
    {
      float *const restrict pix_out = DT_IS_ALIGNED_PIXEL(out + k * 4);
      const float *const restrict corrections_out = corrections + k * 2;

      const float val = sqrtf(pix_out[2] * white);
      float corr = 0.0f;
      switch(mode)
      {
        case BRIGHTNESS:
          corr = BRIGHT_EFFECT * b_corrections[k];
          break;
        case SATURATION:
          corr = SAT_EFFECT * (corrections_out[1] - 1.0f);
          break;
        case BRIGHTNESS_GRAD:
          corr = _get_satweight(saturation[k] - bright_shift) - 0.5f;
          break;
        case SATURATION_GRAD:
          corr = _get_satweight(saturation[k] - sat_shift) - 0.5f;
          break;

        default:  // HUE
          corr = 0.2f * corrections_out[0];
      }

      const gboolean neg = corr < 0.0f;
      corr = fabsf(corr);
      corr = corr < 2e-3 ? 0.0f : corr;
      pix_out[0] = MAX(0.0f, neg ? val - corr : val);
      pix_out[1] = MAX(0.0f, val - corr);
      pix_out[2] = MAX(0.0f, neg ? val : val - corr);

      if(mode == BRIGHTNESS && Lscharr[k] > 0.1f)
      {
        pix_out[0] = pix_out[2] = 0.0f;
        pix_out[1] = Lscharr[k];
      }
    }

    if((mode == BRIGHTNESS_GRAD) || (mode == SATURATION_GRAD))
    {
      const float scaler = (float)width * 16.0f;
      const float eps = 0.5f / (float)height;
      for(int col = 0; col < 16 * width; col++)
      {
        const float weight = _get_satweight((float)col / scaler - (mode == SATURATION_GRAD ? sat_shift : bright_shift));
        if(weight > eps && weight < 1.0f - eps)
        {
          const int row = (int)((1.0f - weight) * (float)(height-1));
          const size_t k = row * width + col / 16;
          out[4*k] = out[4*k+2] = 0.0f;
          out[4*k+1] = 1.0f;
        }
      }
    }
  }

  dt_free_align(corrections);
  dt_free_align(b_corrections);
  dt_free_align(saturation);
  dt_free_align(UV);
  dt_free_align(Lscharr);
}

#if HAVE_OPENCL

int _mean_gaussian_cl(const int devid,
                      cl_mem image,
                      const int width,
                      const int height,
                      const int ch,
                      const float sigma)
{
  const float range = 1.0e9;
  const dt_aligned_pixel_t max = {range, range, range, range};
  const dt_aligned_pixel_t min = {-range, -range, -range, -range};

  dt_gaussian_cl_t *g = dt_gaussian_init_cl(devid, width, height, ch, max, min, sigma, DT_IOP_GAUSSIAN_ZERO);
  if(!g) return DT_OPENCL_PROCESS_CL;

  cl_int err = dt_gaussian_blur_cl_buffer(g, image, image);
  dt_gaussian_free_cl(g);
  return err;
}

static cl_mem _init_covariance_cl(const int devid,
                                  dt_iop_colorequal_global_data_t *gd,
                                  cl_mem UV,
                                  const int width,
                                  const int height)
{
  cl_mem covariance = dt_opencl_alloc_device_buffer(devid, 4 * sizeof(float) * width * height);
  if(covariance == NULL) return NULL;

  cl_int err = dt_opencl_enqueue_kernel_2d_args(devid, gd->ce_init_covariance, width, height,
                    CLARG(covariance), CLARG(UV), CLARG(width), CLARG(height));
  if(err != CL_SUCCESS)
  {
    dt_opencl_release_mem_object(covariance);
    covariance = NULL;
  }
  return covariance;
}

static int _prefilter_chromaticity_cl(const int devid,
                                      dt_iop_colorequal_global_data_t *gd,
                                      cl_mem UV,
                                      cl_mem saturation,
                                      cl_mem weight,
                                      const int width,
                                      const int height,
                                      const float sigma,
                                      const float eps,
                                      const float sat_shift)
{
  cl_int err = CL_MEM_OBJECT_ALLOCATION_FAILURE;

  const float scaling = _get_scaling(sigma);
  const float gsigma = MAX(0.2f, sigma / scaling);
  const int ds_height = height / scaling;
  const int ds_width = width / scaling;
  const gboolean resized = width != ds_width || height != ds_height;

  const size_t bsize = (size_t) width * height * sizeof(float);
  const size_t ds_bsize = (size_t) ds_width * ds_height * sizeof(float);

  cl_mem ds_UV = UV;
  cl_mem covariance = NULL;
  cl_mem ds_a = NULL;
  cl_mem ds_b = NULL;
  cl_mem a = NULL;
  cl_mem b = NULL;

  if(resized)
  {
    ds_UV = dt_opencl_alloc_device_buffer(devid, 2 * ds_bsize);
    if(ds_UV == NULL) return err;

    err = dt_opencl_enqueue_kernel_2d_args(devid, gd->ce_bilinear2, ds_width, ds_height,
          CLARG(UV), CLARG(width), CLARG(height), CLARG(ds_UV), CLARG(ds_width), CLARG(ds_height));
    if(err != CL_SUCCESS) goto error;
  }

  covariance = _init_covariance_cl(devid, gd, ds_UV, ds_width, ds_height);
  if(covariance == NULL)
  {
    err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
    goto error;
  }

  err = _mean_gaussian_cl(devid, ds_UV, ds_width, ds_height, 2, gsigma);
  if(err != CL_SUCCESS) goto error;

  err = _mean_gaussian_cl(devid, covariance, ds_width, ds_height, 4, gsigma);
  if(err != CL_SUCCESS) goto error;

  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->ce_finish_covariance, ds_width, ds_height,
          CLARG(covariance), CLARG(ds_UV), CLARG(ds_width), CLARG(ds_height));
  if(err != CL_SUCCESS) goto error;

  ds_a = dt_opencl_alloc_device_buffer(devid, 4 * ds_bsize);
  ds_b = dt_opencl_alloc_device_buffer(devid, 2 * ds_bsize);
  if(ds_a == NULL || ds_b == NULL)
  {
    err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
    goto error;
  }

  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->ce_prepare_prefilter, ds_width, ds_height,
          CLARG(ds_UV), CLARG(covariance), CLARG(ds_a), CLARG(ds_b), CLARG(eps),
          CLARG(ds_width), CLARG(ds_height));
  if(err != CL_SUCCESS) goto error;

  dt_opencl_release_mem_object(covariance);
  covariance = NULL;
  if(resized)
  {
    dt_opencl_release_mem_object(ds_UV);
    ds_UV = NULL;
  }

  err = _mean_gaussian_cl(devid, ds_a, ds_width, ds_height, 4, gsigma);
  if(err != CL_SUCCESS) goto error;

  err = _mean_gaussian_cl(devid, ds_b, ds_width, ds_height, 2, gsigma);
  if(err != CL_SUCCESS) goto error;

  a = ds_a;
  b = ds_b;
  if(resized)
  {
    a = dt_opencl_alloc_device_buffer(devid, 4 * bsize);
    b = dt_opencl_alloc_device_buffer(devid, 2 * bsize);
    if(a == NULL || b == NULL)
    {
      err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
      goto error;
    }

    err = dt_opencl_enqueue_kernel_2d_args(devid, gd->ce_bilinear4, width, height,
          CLARG(ds_a), CLARG(ds_width), CLARG(ds_height), CLARG(a), CLARG(width), CLARG(height));
    if(err != CL_SUCCESS) goto error;

    err = dt_opencl_enqueue_kernel_2d_args(devid, gd->ce_bilinear2, width, height,
          CLARG(ds_b), CLARG(ds_width), CLARG(ds_height), CLARG(b), CLARG(width), CLARG(height));
    if(err != CL_SUCCESS) goto error;

    dt_opencl_release_mem_object(ds_a);
    ds_a = NULL;
    dt_opencl_release_mem_object(ds_b);
    ds_b = NULL;
  }

  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->ce_apply_prefilter, width, height,
          CLARG(UV), CLARG(saturation), CLARG(a), CLARG(b), CLARG(weight),
          CLARG(sat_shift), CLARG(width), CLARG(height));

error:
  if(resized)
  {
    dt_opencl_release_mem_object(a);
    dt_opencl_release_mem_object(b);
    dt_opencl_release_mem_object(ds_UV);
  }
  dt_opencl_release_mem_object(covariance);
  dt_opencl_release_mem_object(ds_a);
  dt_opencl_release_mem_object(ds_b);
  return err;
}

static int _guide_with_chromaticity_cl(const int devid,
                                      dt_iop_colorequal_global_data_t *gd,
                                      cl_mem UV,
                                      cl_mem corrections,
                                      cl_mem saturation,
                                      cl_mem b_corrections,
                                      cl_mem scharr,
                                      cl_mem weight,
                                      const int width,
                                      const int height,
                                      const float sigma,
                                      const float eps,
                                      const float bright_shift,
                                      const float sat_shift)
{
  const float scaling = _get_scaling(sigma);
  const float gsigma =  MAX(0.2f, sigma / scaling);
  const int ds_height = height / scaling;
  const int ds_width = width / scaling;
  const gboolean resized = width != ds_width || height != ds_height;

  const size_t bsize = (size_t) width * height * sizeof(float);
  const size_t ds_bsize = (size_t) ds_width * ds_height * sizeof(float);

  cl_int err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
  cl_mem ds_UV = UV;
  cl_mem ds_corrections = corrections;
  cl_mem ds_b_corrections = b_corrections;
  cl_mem covariance = NULL;
  cl_mem correlations = NULL;
  cl_mem ds_a = NULL;
  cl_mem ds_b = NULL;
  cl_mem a = NULL;
  cl_mem b = NULL;

  if(resized)
  {
    ds_UV = dt_opencl_alloc_device_buffer(devid, 2 * ds_bsize);
    ds_corrections = dt_opencl_alloc_device_buffer(devid, 2 * ds_bsize);
    ds_b_corrections = dt_opencl_alloc_device_buffer(devid, ds_bsize);
    if(ds_UV == NULL || ds_corrections == NULL || ds_b_corrections == NULL) goto error;

    err = dt_opencl_enqueue_kernel_2d_args(devid, gd->ce_bilinear2, ds_width, ds_height,
          CLARG(UV), CLARG(width), CLARG(height), CLARG(ds_UV), CLARG(ds_width), CLARG(ds_height));
    if(err != CL_SUCCESS) goto error;

    err = dt_opencl_enqueue_kernel_2d_args(devid, gd->ce_bilinear2, ds_width, ds_height,
          CLARG(corrections), CLARG(width), CLARG(height), CLARG(ds_corrections), CLARG(ds_width), CLARG(ds_height));
    if(err != CL_SUCCESS) goto error;

    err = dt_opencl_enqueue_kernel_2d_args(devid, gd->ce_bilinear1, ds_width, ds_height,
          CLARG(b_corrections), CLARG(width), CLARG(height), CLARG(ds_b_corrections), CLARG(ds_width), CLARG(ds_height));
    if(err != CL_SUCCESS) goto error;
  }

  covariance = _init_covariance_cl(devid, gd, ds_UV, ds_width, ds_height);
  correlations = dt_opencl_alloc_device_buffer(devid, 4 * ds_bsize);
  if(covariance == NULL || correlations == NULL)
  {
    err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
    goto error;
  }

  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->ce_prepare_correlations, ds_width, ds_height,
          CLARG(ds_corrections), CLARG(ds_b_corrections), CLARG(ds_UV), CLARG(correlations),
          CLARG(ds_width), CLARG(ds_height));
  if(err != CL_SUCCESS) goto error;

  err = _mean_gaussian_cl(devid, ds_UV, ds_width, ds_height, 2, gsigma);
  if(err != CL_SUCCESS) goto error;
  err = _mean_gaussian_cl(devid, covariance, ds_width, ds_height, 4, gsigma);
  if(err != CL_SUCCESS) goto error;
  err = _mean_gaussian_cl(devid, ds_corrections, ds_width, ds_height, 2, gsigma);
  if(err != CL_SUCCESS) goto error;
  err = _mean_gaussian_cl(devid, ds_b_corrections, ds_width, ds_height, 1, 0.1f * gsigma);
  if(err != CL_SUCCESS) goto error;
  err = _mean_gaussian_cl(devid, correlations, ds_width, ds_height, 4, gsigma);
  if(err != CL_SUCCESS) goto error;

  ds_a = dt_opencl_alloc_device_buffer(devid, 4 * ds_bsize);
  ds_b = dt_opencl_alloc_device_buffer(devid, 2 * ds_bsize);
  if(ds_a == NULL || ds_b == NULL)
  {
    err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
    goto error;
  }

  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->ce_finish_correlations, ds_width, ds_height,
          CLARG(ds_corrections), CLARG(ds_b_corrections), CLARG(ds_UV),
          CLARG(correlations), CLARG(covariance),
          CLARG(ds_width), CLARG(ds_height));
  if(err != CL_SUCCESS) goto error;

  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->ce_final_guide, ds_width, ds_height,
          CLARG(covariance), CLARG(correlations), CLARG(ds_corrections), CLARG(ds_b_corrections),
          CLARG(ds_UV), CLARG(ds_a), CLARG(ds_b), CLARG(eps),
          CLARG(ds_width), CLARG(ds_height));
  if(err != CL_SUCCESS) goto error;

  if(resized)
  {
    dt_opencl_release_mem_object(ds_UV);
    ds_UV = NULL;
    dt_opencl_release_mem_object(ds_corrections);
    ds_corrections = NULL;
    dt_opencl_release_mem_object(ds_b_corrections);
    ds_b_corrections = NULL;
  }
  dt_opencl_release_mem_object(correlations);
  correlations = NULL;
  dt_opencl_release_mem_object(covariance);
  covariance = NULL;

  err = _mean_gaussian_cl(devid, ds_a, ds_width, ds_height, 4, gsigma);
  if(err != CL_SUCCESS) goto error;
  err = _mean_gaussian_cl(devid, ds_b, ds_width, ds_height, 2, gsigma);
  if(err != CL_SUCCESS) goto error;

  a = ds_a;
  b = ds_b;
  if(resized)
  {
    a = dt_opencl_alloc_device_buffer(devid, 4 * bsize);
    b = dt_opencl_alloc_device_buffer(devid, 2 * bsize);
    if(a == NULL || b == NULL)
    {
      err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
      goto error;
    }

    err = dt_opencl_enqueue_kernel_2d_args(devid, gd->ce_bilinear4, width, height,
          CLARG(ds_a), CLARG(ds_width), CLARG(ds_height), CLARG(a), CLARG(width), CLARG(height));
    if(err != CL_SUCCESS) goto error;

    err = dt_opencl_enqueue_kernel_2d_args(devid, gd->ce_bilinear2, width, height,
          CLARG(ds_b), CLARG(ds_width), CLARG(ds_height), CLARG(b), CLARG(width), CLARG(height));
    if(err != CL_SUCCESS) goto error;
  }

  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->ce_apply_guided, width, height,
          CLARG(UV), CLARG(saturation), CLARG(scharr), CLARG(a), CLARG(b),
          CLARG(corrections), CLARG(b_corrections), CLARG(weight),
          CLARG(sat_shift), CLARG(bright_shift), CLARG(width), CLARG(height));

error:
  if(resized)
  {
    dt_opencl_release_mem_object(ds_UV);
    dt_opencl_release_mem_object(ds_corrections);
    dt_opencl_release_mem_object(ds_b_corrections);
    dt_opencl_release_mem_object(a);
    dt_opencl_release_mem_object(b);
  }
  dt_opencl_release_mem_object(correlations);
  dt_opencl_release_mem_object(covariance);
  dt_opencl_release_mem_object(ds_a);
  dt_opencl_release_mem_object(ds_b);

  return err;
}

int process_cl(dt_iop_module_t *self,
               dt_dev_pixelpipe_iop_t *piece,
               cl_mem dev_in,
               cl_mem dev_out,
               const dt_iop_roi_t *const roi_in,
               const dt_iop_roi_t *const roi_out)
{
  dt_iop_colorequal_data_t *d = (dt_iop_colorequal_data_t *)piece->data;
  dt_iop_colorequal_global_data_t *const gd = (dt_iop_colorequal_global_data_t *)self->global_data;

  // Get working color profile
  const struct dt_iop_order_iccprofile_info_t *const work_profile
      = dt_ioppr_get_pipe_current_profile_info(self, piece->pipe);
  if(piece->colors != 4 || work_profile == NULL)
    return DT_OPENCL_PROCESS_CL;

  cl_int err = CL_MEM_OBJECT_ALLOCATION_FAILURE;

  const int devid = piece->pipe->devid;
  const int width = roi_out->width;
  const int height = roi_out->height;
  const size_t bsize = (size_t) width * height * sizeof(float);

  dt_iop_colorequal_gui_data_t *g = (dt_iop_colorequal_gui_data_t *)self->gui_data;
  const gboolean fullpipe = piece->pipe->type & DT_DEV_PIXELPIPE_FULL;
  const int mask_mode = g && fullpipe ? g->mask_mode : 0;
  const int guiding = d->use_filter;
  const gboolean run_fast = piece->pipe->type & DT_DEV_PIXELPIPE_FAST;

  float white, sat_shift, max_brightness_shift, corr_max_brightness_shift, bright_shift, gradient_amp, hue_sigma, par_sigma, sat_sigma, scharr_sigma;
  _prepare_process(roi_in->scale / piece->iscale, d,
    &white, &sat_shift, &max_brightness_shift, &corr_max_brightness_shift, &bright_shift, &gradient_amp, &hue_sigma, &par_sigma, &sat_sigma, &scharr_sigma);

  dt_colormatrix_t input_matrix;
  dt_colormatrix_t output_matrix;
  dt_colormatrix_mul(input_matrix, XYZ_D50_to_D65_CAT16, work_profile->matrix_in);
  dt_colormatrix_mul(output_matrix, work_profile->matrix_out, XYZ_D65_to_D50_CAT16);

  cl_mem input_matrix_cl = dt_opencl_copy_host_to_device_constant(devid, 12 * sizeof(float), input_matrix);
  cl_mem output_matrix_cl = dt_opencl_copy_host_to_device_constant(devid, 12 * sizeof(float), output_matrix);
  cl_mem gamut_LUT = dt_opencl_copy_host_to_device_constant(devid, LUT_ELEM * sizeof(float), d->gamut_LUT);
  cl_mem LUT_saturation = dt_opencl_copy_host_to_device_constant(devid, LUT_ELEM * sizeof(float), d->LUT_saturation);
  cl_mem LUT_hue = dt_opencl_copy_host_to_device_constant(devid, LUT_ELEM * sizeof(float), d->LUT_hue);
  cl_mem LUT_brightness = dt_opencl_copy_host_to_device_constant(devid, LUT_ELEM * sizeof(float), d->LUT_brightness);
  cl_mem weight = dt_opencl_copy_host_to_device_constant(devid, (2 * SATSIZE + 1) * sizeof(float), satweights);

  cl_mem pixout = dt_opencl_alloc_device_buffer(devid, 4 * bsize);
  cl_mem UV = dt_opencl_alloc_device_buffer(devid, 2 * bsize);
  cl_mem corrections = dt_opencl_alloc_device_buffer(devid, 2 * bsize);
  cl_mem b_corrections = dt_opencl_alloc_device_buffer(devid, bsize);
  cl_mem Lscharr = dt_opencl_alloc_device_buffer(devid, bsize);
  cl_mem saturation = dt_opencl_alloc_device_buffer(devid, bsize);

  if(input_matrix_cl == NULL || output_matrix_cl == NULL || gamut_LUT == NULL || LUT_saturation == NULL
      || LUT_hue == NULL || LUT_brightness == NULL || weight == NULL
      || pixout == NULL || UV == NULL || corrections == NULL || b_corrections == NULL
      || Lscharr == NULL || saturation == NULL)
    goto error;

  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->ce_sample_input, width, height,
          CLARG(dev_in), CLARG(saturation), CLARG(Lscharr), CLARG(UV), CLARG(pixout),
          CLARG(input_matrix_cl), CLARG(width),  CLARG(height));
  if(err != CL_SUCCESS) goto error;

  err = _mean_gaussian_cl(devid, saturation, width, height, 1, sat_sigma);
  if(err != CL_SUCCESS) goto error;

  // STEP 2 : smoothen UV to avoid discontinuities in hue
  if(guiding && !run_fast)
  {
    err = _prefilter_chromaticity_cl(devid, gd, UV, saturation, weight, width, height, hue_sigma, d->chroma_feathering, sat_shift);
    if(err != CL_SUCCESS) goto error;
  }

  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->ce_process_data, width, height,
                CLARG(UV), CLARG(Lscharr), CLARG(saturation),
                CLARG(corrections), CLARG(b_corrections), CLARG(pixout),
                CLARG(LUT_saturation), CLARG(LUT_hue), CLARG(LUT_brightness),
                CLARG(white), CLARG(gradient_amp), CLARG(guiding),
                CLARG(width),  CLARG(height));
  if(err != CL_SUCCESS) goto error;

  if(guiding && !run_fast)
  {
    err = _mean_gaussian_cl(devid, Lscharr, width, height, 1, scharr_sigma);
    if(err != CL_SUCCESS) goto error;

    err = _guide_with_chromaticity_cl(devid, gd, UV, corrections, saturation, b_corrections, Lscharr, weight, width, height, par_sigma, d->param_feathering, bright_shift, sat_shift);
    if(err != CL_SUCCESS) goto error;
  }

  if(!mask_mode)
  {
    err = dt_opencl_enqueue_kernel_2d_args(devid, gd->ce_write_output, width, height,
          CLARG(dev_out), CLARG(pixout),
          CLARG(corrections), CLARG(b_corrections),
          CLARG(output_matrix_cl), CLARG(gamut_LUT), CLARG(white),
          CLARG(width), CLARG(height));
  }
  else
  {
    piece->pipe->mask_display = DT_DEV_PIXELPIPE_DISPLAY_PASSTHRU;
    const int mode = mask_mode - 1;
    err = dt_opencl_enqueue_kernel_2d_args(devid, gd->ce_write_visual, width, height,
          CLARG(dev_out), CLARG(pixout), CLARG(corrections), CLARG(b_corrections), CLARG(saturation), CLARG(Lscharr),
          CLARG(weight), CLARG(bright_shift), CLARG(sat_shift), CLARG(white), CLARG(mode),
          CLARG(width), CLARG(height));
    if(err != CL_SUCCESS) goto error;

    if(mode == BRIGHTNESS_GRAD || mode == SATURATION_GRAD)
    {
      err = dt_opencl_enqueue_kernel_1d_args(devid, gd->ce_draw_weight, width,
          CLARG(dev_out), CLARG(weight), CLARG(bright_shift), CLARG(sat_shift), CLARG(mode), CLARG(width), CLARG(height));
    }
  }

error:
  dt_opencl_release_mem_object(input_matrix_cl);
  dt_opencl_release_mem_object(output_matrix_cl);
  dt_opencl_release_mem_object(gamut_LUT);
  dt_opencl_release_mem_object(LUT_saturation);
  dt_opencl_release_mem_object(LUT_hue);
  dt_opencl_release_mem_object(LUT_brightness);
  dt_opencl_release_mem_object(weight);
  dt_opencl_release_mem_object(UV);
  dt_opencl_release_mem_object(pixout);
  dt_opencl_release_mem_object(corrections);
  dt_opencl_release_mem_object(b_corrections);
  dt_opencl_release_mem_object(Lscharr);
  dt_opencl_release_mem_object(saturation);
  return err;
}

#endif // OpenCL

static inline float _get_hue_node(const int k, const float hue_shift)
{
  // Get the angular coordinate of the k-th hue node, including hue shift
  return _deg_to_rad(((float)k) * 360.f / ((float)NODES) + hue_shift);
}

static inline float _cosine_coeffs(const float l,
                                   const float c)
{
  return expf(-l * l / c);
}

static inline void _periodic_RBF_interpolate(float nodes[NODES],
                                             const float smoothing,
                                             float *const LUT,
                                             const float hue_shift,
                                             const gboolean clip)
{
  // Perform a periodic interpolation across hue angles using radial-basis functions
  // see https://eng.aurelienpierre.com/2022/06/interpolating-hue-angles/#Refined-approach
  // for the theory and Python demo

  // Number of terms for the cosine series
  const int m = (int)ceilf(3.f * sqrtf(smoothing));

  float DT_ALIGNED_ARRAY A[NODES][NODES] = { { 0.f } };

  // Build the A matrix with nodes
  for(int i = 0; i < NODES; i++)
    for(int j = 0; j < NODES; j++)
    {
      for(int l = 0; l < m; l++)
      {
        A[i][j] += _cosine_coeffs(l, smoothing) * \
          cosf(((float)l) * fabsf(_get_hue_node(i, hue_shift) - _get_hue_node(j, hue_shift)));
      }
      A[i][j] = expf(A[i][j]);
    }

  // Solve A * x = y for lambdas
  pseudo_solve((float *)A, nodes, NODES, NODES, FALSE);

  // Interpolate data for all x : generate the LUT
  // WARNING: the LUT spans from [-pi; pi[ for consistency with the output of atan2f()
  for(int i = 0; i < LUT_ELEM; i++)
  {
    // i is directly the hue angle in degree since we sample the LUT
    // every degree.  We use un-offset angles here, since thue hue
    // offset is merely a GUI thing, only relevant for user-defined
    // nodes.
    const float hue = (float)i * 360.0f / (float)LUT_ELEM * M_PI_F / 180.f - M_PI_F;
    LUT[i] = 0.f;

    for(int k = 0; k < NODES; k++)
    {
      float result = 0;
      for(int l = 0; l < m; l++)
      {
        result += _cosine_coeffs(l, smoothing)
          * cosf(((float)l) * fabsf(hue - _get_hue_node(k, hue_shift)));
      }
      LUT[i] += nodes[k] * expf(result);
    }

    if(clip) LUT[i] = fmaxf(0.f, LUT[i]);
  }
}

void init_pipe(dt_iop_module_t *self,
               dt_dev_pixelpipe_t *pipe,
               dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = dt_calloc_aligned(sizeof(dt_iop_colorequal_data_t));
  dt_iop_colorequal_data_t *d = piece->data;
  d->LUT_saturation = dt_alloc_align_float(LUT_ELEM);
  d->LUT_hue = dt_alloc_align_float(LUT_ELEM);
  d->LUT_brightness = dt_alloc_align_float(LUT_ELEM);
  d->gamut_LUT = dt_alloc_align_float(LUT_ELEM);
  d->lut_inited = FALSE;
  d->work_profile = NULL;
}


void cleanup_pipe(dt_iop_module_t *self,
                  dt_dev_pixelpipe_t *pipe,
                  dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_colorequal_data_t *d = piece->data;
  dt_free_align(d->LUT_saturation);
  dt_free_align(d->LUT_hue);
  dt_free_align(d->LUT_brightness);
  dt_free_align(d->gamut_LUT);
  dt_free_align(piece->data);
  piece->data = NULL;
}

static inline void _pack_saturation(dt_iop_colorequal_params_t *p,
                                    float array[NODES])
{
  array[0] = p->sat_red;
  array[1] = p->sat_orange;
  array[2] = p->sat_yellow;
  array[3] = p->sat_green;
  array[4] = p->sat_cyan;
  array[5] = p->sat_blue;
  array[6] = p->sat_lavender;
  array[7] = p->sat_magenta;
}

static inline void _pack_hue(dt_iop_colorequal_params_t *p,
                             float array[NODES])
{
  array[0] = p->hue_red;
  array[1] = p->hue_orange;
  array[2] = p->hue_yellow;
  array[3] = p->hue_green;
  array[4] = p->hue_cyan;
  array[5] = p->hue_blue;
  array[6] = p->hue_lavender;
  array[7] = p->hue_magenta;

  for(int i = 0; i < NODES; i++)
    array[i] = array[i] / 180.f * M_PI_F; // Convert to radians
}

static inline void _pack_brightness(dt_iop_colorequal_params_t *p,
                                    float array[NODES])
{
  array[0] = p->bright_red;
  array[1] = p->bright_orange;
  array[2] = p->bright_yellow;
  array[3] = p->bright_green;
  array[4] = p->bright_cyan;
  array[5] = p->bright_blue;
  array[6] = p->bright_lavender;
  array[7] = p->bright_magenta;
}


void commit_params(dt_iop_module_t *self,
                   dt_iop_params_t *p1,
                   dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_colorequal_params_t *p = (dt_iop_colorequal_params_t *)p1;
  dt_iop_colorequal_data_t *d = piece->data;

  d->white_level = exp2f(p->white_level);
  d->chroma_size = p->chroma_size;
  d->chroma_feathering = powf(10.f, -5.0f);
  d->param_size = p->param_size;
  d->param_feathering = powf(10.f, -6.0f);
  d->use_filter = p->use_filter;
  d->hue_shift = p->hue_shift;
  // default inflection point at a sat of 6%; allow selection up to ~60%
  d->threshold = -0.015f + 0.3f * sqrf(5.0f * p->threshold);
  d->contrast = p->contrast;
  float DT_ALIGNED_ARRAY sat_values[NODES];
  float DT_ALIGNED_ARRAY hue_values[NODES];
  float DT_ALIGNED_ARRAY bright_values[NODES];

  // FIXME only calc LUTs if necessary
  _pack_saturation(p, sat_values);
  _periodic_RBF_interpolate(sat_values,
                            M_PI_F,
                            d->LUT_saturation, d->hue_shift, TRUE);

  _pack_hue(p, hue_values);
  _periodic_RBF_interpolate(hue_values,
                            1.f / p->smoothing_hue * M_PI_F,
                            d->LUT_hue, d->hue_shift, FALSE);

  _pack_brightness(p, bright_values);

  d->max_brightness = 1.0f;
  for(int c = 0; c < NODES; c++)
    d->max_brightness = fmaxf(d->max_brightness, bright_values[c]);

  _periodic_RBF_interpolate(bright_values,
                            M_PI_F,
                            d->LUT_brightness, d->hue_shift, TRUE);

  // Check if the RGB working profile has changed in pipe
  // WARNING: this function is not triggered upon working profile change,
  // so the gamut boundaries are wrong until we change some param in this module
  struct dt_iop_order_iccprofile_info_t *const work_profile =
    dt_ioppr_get_pipe_current_profile_info(self, piece->pipe);
  if(work_profile == NULL)
    return;
  if(work_profile != d->work_profile)
  {
    d->lut_inited = FALSE;
    d->work_profile = work_profile;
  }

  // find the maximum chroma allowed by the current working gamut in
  // conjunction to hue this will be used to prevent users to mess up
  // their images by pushing chroma out of gamut
  if(!d->lut_inited)
  {
    dt_colormatrix_t input_matrix;
    dt_colormatrix_mul(input_matrix, XYZ_D50_to_D65_CAT16, work_profile->matrix_in);
    dt_UCS_22_build_gamut_LUT(input_matrix, d->gamut_LUT);
    d->lut_inited = TRUE;
  }
}


static inline void _build_dt_UCS_HSB_gradients
  (dt_aligned_pixel_t HSB,
   dt_aligned_pixel_t RGB,
   const struct dt_iop_order_iccprofile_info_t *work_profile,
   const float *const gamut_LUT)
{
  // Generate synthetic HSB gradients and convert to display RGB

  // First, gamut-map to ensure the requested HSB color is available in display gamut
  gamut_map_HSB(HSB, gamut_LUT, 1.f);

  // Then, convert to XYZ D65
  dt_aligned_pixel_t XYZ_D65 = { 1.0f, 1.0f, 1.0f, 1.0f };
  dt_UCS_HSB_to_XYZ(HSB, 1.f, XYZ_D65);

  if(work_profile)
  {
    dt_ioppr_xyz_to_rgb_matrix(XYZ_D65, RGB,
                               work_profile->matrix_out_transposed,
                               work_profile->lut_out,
                               work_profile->unbounded_coeffs_out,
                               work_profile->lutsize,
                               work_profile->nonlinearlut);
  }
  else
  {
    // Fall back to sRGB output and slow white point conversion
    dt_aligned_pixel_t XYZ_D50;
    XYZ_D65_to_D50(XYZ_D65, XYZ_D50);
    dt_XYZ_to_sRGB(XYZ_D50, RGB);
  }

  dt_vector_clip(RGB);
}

static inline void _draw_sliders_saturation_gradient
  (const float sat_min,
   const float sat_max,
   const float hue,
   const float brightness,
   GtkWidget *const slider,
   const struct dt_iop_order_iccprofile_info_t *work_profile,
   const float *const gamut_LUT)
{
  const float range = sat_max - sat_min;

  for(int i = 0; i < DT_BAUHAUS_SLIDER_MAX_STOPS; i++)
  {
    const float stop = ((float)i / (float)(DT_BAUHAUS_SLIDER_MAX_STOPS - 1));
    const float sat = sat_min + stop * range;
    dt_aligned_pixel_t RGB = { 1.0f, 1.0f, 1.0f, 1.0f };
    _build_dt_UCS_HSB_gradients((dt_aligned_pixel_t){ hue, sat, brightness, 0.f },
                                RGB, work_profile, gamut_LUT);
    dt_bauhaus_slider_set_stop(slider, stop, RGB[0], RGB[1], RGB[2]);
  }
}

static inline void _draw_sliders_hue_gradient
  (const float sat,
   const float hue,
   const float brightness,
   GtkWidget *const slider,
   const struct dt_iop_order_iccprofile_info_t *work_profile,
   const float *const gamut_LUT)
{
  const float hue_min = hue - M_PI_F;

  for(int i = 0; i < DT_BAUHAUS_SLIDER_MAX_STOPS; i++)
  {
    const float stop = ((float)i / (float)(DT_BAUHAUS_SLIDER_MAX_STOPS - 1));
    const float hue_temp = hue_min + stop * 2.f * M_PI_F;
    dt_aligned_pixel_t RGB = {  1.0f, 1.0f, 1.0f, 1.0f };
    _build_dt_UCS_HSB_gradients((dt_aligned_pixel_t){ hue_temp, sat, brightness, 0.f },
                                RGB, work_profile, gamut_LUT);
    dt_bauhaus_slider_set_stop(slider, stop, RGB[0], RGB[1], RGB[2]);
  }
}

static inline void _draw_sliders_brightness_gradient
  (const float sat,
   const float hue,
   GtkWidget *const slider,
   const struct dt_iop_order_iccprofile_info_t *work_profile,
   const float *const gamut_LUT)
{
  for(int i = 0; i < DT_BAUHAUS_SLIDER_MAX_STOPS; i++)
  {
    const float stop = ((float)i / (float)(DT_BAUHAUS_SLIDER_MAX_STOPS - 1))
      * (1.f - 0.001f);
    dt_aligned_pixel_t RGB = {  1.0f, 1.0f, 1.0f, 1.0f };
    _build_dt_UCS_HSB_gradients((dt_aligned_pixel_t){ hue, sat, stop + 0.001f, 0.f },
                                RGB, work_profile, gamut_LUT);
    dt_bauhaus_slider_set_stop(slider, stop, RGB[0], RGB[1], RGB[2]);
  }
}

static inline void _init_sliders(dt_iop_module_t *self)
{
  dt_iop_colorequal_gui_data_t *g = self->gui_data;
  dt_iop_colorequal_params_t *p = self->params;

  // Saturation sliders
  for(int k = 0; k < NODES; k++)
  {
    GtkWidget *const slider = g->sat_sliders[k];
    _draw_sliders_saturation_gradient(0.f, g->max_saturation, _get_hue_node(k, p->hue_shift),
                                      SLIDER_BRIGHTNESS, slider,
                                      g->white_adapted_profile,
                                      g->gamut_LUT);
    dt_bauhaus_slider_set_format(slider, "%");
    dt_bauhaus_slider_set_offset(slider, -100.0f);
    dt_bauhaus_slider_set_digits(slider, 2);
    gtk_widget_queue_draw(slider);
  }

  // Hue sliders
  for(int k = 0; k < NODES; k++)
  {
    GtkWidget *const slider = g->hue_sliders[k];
    _draw_sliders_hue_gradient(g->max_saturation, _get_hue_node(k, p->hue_shift),
                               SLIDER_BRIGHTNESS, slider,
                               g->white_adapted_profile,
                               g->gamut_LUT);
    dt_bauhaus_slider_set_format(slider, "°");
    dt_bauhaus_slider_set_digits(slider, 2);
    gtk_widget_queue_draw(slider);
  }

  // Brightness sliders
  for(int k = 0; k < NODES; k++)
  {
    GtkWidget *const slider = g->bright_sliders[k];
    _draw_sliders_brightness_gradient(g->max_saturation, _get_hue_node(k, p->hue_shift),
                                      slider,
                                      g->white_adapted_profile,
                                      g->gamut_LUT);
    dt_bauhaus_slider_set_format(slider, "%");
    dt_bauhaus_slider_set_offset(slider, -100.0f);
    dt_bauhaus_slider_set_digits(slider, 2);
    gtk_widget_queue_draw(slider);
  }
}

static void _init_graph_backgrounds(dt_iop_colorequal_gui_data_t *g,
                                    const float graph_width,
                                    const float graph_height,
                                    const float *const restrict gamut_LUT)
{
  const int gwidth = graph_width;
  const int gheight = graph_height;
  const int stride = cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, gwidth);
  const float max_saturation = g->max_saturation;

  for(int c = 0; c < NUM_CHANNELS; c++)
  {
    if(g->b_data[c])
      free(g->b_data[c]);
    g->b_data[c] = malloc(stride * gheight);

    if(g->b_surface[c])
      cairo_surface_destroy(g->b_surface[c]);
    g->b_surface[c] = cairo_image_surface_create_for_data(g->b_data[c], CAIRO_FORMAT_RGB24, gwidth, gheight, stride);
  }

  DT_OMP_FOR(collapse(2))
  for(int i = 0; i < gheight; i++)
  {
    for(int j = 0; j < gwidth; j++)
    {
      const size_t idx = i * stride + j * 4;
      const float x = 360.0f * (float)(gwidth - j - 1) / (graph_width - 1.0f) - 90.0f;
      const float y = 1.0f - (float)i / (graph_height - 1.0f);
      const float hue = (x < -180.0f) ? _deg_to_rad(x +180.0f) : _deg_to_rad(x);
      const float hhue = hue - (y - 0.5f) * 2.f * M_PI_F;

      dt_aligned_pixel_t RGB;
      dt_aligned_pixel_t HSB[NUM_CHANNELS] = {{ hhue, max_saturation,     SLIDER_BRIGHTNESS,              1.0f },
                                              { hue,  max_saturation * y, SLIDER_BRIGHTNESS,              1.0f },
                                              { hue,  max_saturation,     1.25f * SLIDER_BRIGHTNESS * y,  1.0f } };

      for(int k = 0; k < NUM_CHANNELS; k++)
      {
        _build_dt_UCS_HSB_gradients(HSB[k], RGB, g->white_adapted_profile, gamut_LUT);
        for_three_channels(c)
          g->b_data[k][idx + c] = roundf(CLIP(RGB[c]) * 255.f);
      }
    }
  }
  g->gradients_cached = TRUE;
}

void reload_defaults(dt_iop_module_t *self)
{
  // we might be called from presets update infrastructure => there is no image
  if(!self->dev || !dt_is_valid_imgid(self->dev->image_storage.id)) return;

  dt_iop_colorequal_gui_data_t *g = self->gui_data;
  if(g)
  {
    // reset masking
    dt_bauhaus_widget_set_quad_active(g->param_size, FALSE);
    dt_bauhaus_widget_set_quad_active(g->threshold, FALSE);
    g->mask_mode = 0;
  }
}

void init_presets(dt_iop_module_so_t *self)
{
  // bleach bypass
  dt_iop_colorequal_params_t p1 =
    { .threshold       = 0.0f,
      .smoothing_hue   = 1.0f,
      .contrast        = 0.0f,
      .white_level     = 1.0f,
      .chroma_size     = 1.5f,
      .param_size      = 1.0f,
      .use_filter      = TRUE,

      .sat_red         = 1.0f - 0.2215f,
      .sat_orange      = 1.0f - 0.1772f,
      .sat_yellow      = 1.0f - 0.3861f,
      .sat_green       = 1.0f - 0.3924f,
      .sat_cyan        = 1.0f - 0.4557f,
      .sat_blue        = 1.0f - 0.4177f,
      .sat_lavender    = 1.0f - 0.2468f,
      .sat_magenta     = 1.0f - 0.2532f,

      .hue_red         = 15.46f,
      .hue_orange      = 0.0f,
      .hue_yellow      = -2.21f,
      .hue_green       = 28.72f,
      .hue_cyan        = 16.57f,
      .hue_blue        = 0.0f,
      .hue_lavender    = 0.0f,
      .hue_magenta     = 0.0f,

      .bright_red      = 1.0f - 0.250f,
      .bright_orange   = 1.0f - 0.250f,
      .bright_yellow   = 1.0f - 0.250f,
      .bright_green    = 1.0f - 0.350f,
      .bright_cyan     = 1.0f - 0.350f,
      .bright_blue     = 1.0f - 0.250f,
      .bright_lavender = 1.0f - 0.250f,
      .bright_magenta  = 1.0f - 0.250f,

      .hue_shift       = 0.0f
    };

  dt_gui_presets_add_generic(_("bleach bypass"), self->op,
                             self->version(), &p1, sizeof(p1),
                             1, DEVELOP_BLEND_CS_RGB_SCENE);

  // Kodachrome 64 like
  dt_iop_colorequal_params_t p2 =
    { .threshold       = 0.193f,
      .smoothing_hue   = 1.0f,
      .contrast        = -0.345f,
      .white_level     = 1.0f,
      .chroma_size     = 1.5f,
      .param_size      = 19.0f,
      .use_filter      = TRUE,

      .sat_red         = 1.0f + 0.2390f,
      .sat_orange      = 1.0f + 0.0377f,
      .sat_yellow      = 1.0f - 0.1761f,
      .sat_green       = 1.0f - 0.1635f,
      .sat_cyan        = 1.0f - 0.0126f,
      .sat_blue        = 1.0f + 0.0126f,
      .sat_lavender    = 1.0f - 0.0000f,
      .sat_magenta     = 1.0f + 0.1384f,

      .hue_red         = -2.20f,
      .hue_orange      = -17.56f,
      .hue_yellow      = -3.29f,
      .hue_green       = 32.93f,
      .hue_cyan        = 14.27f,
      .hue_blue        = 6.59f,
      .hue_lavender    = -7.68f,
      .hue_magenta     = 0.0f,

      .bright_red      = 1.0f - 0.0063f,
      .bright_orange   = 1.0f + 0.1824f,
      .bright_yellow   = 1.0f - 0.1950f,
      .bright_green    = 1.0f - 0.2390f,
      .bright_cyan     = 1.0f - 0.2453f,
      .bright_blue     = 1.0f + 0.0377f,
      .bright_lavender = 1.0f - 0.1572f,
      .bright_magenta  = 1.0f - 0.1384f,

      .hue_shift       = 0.0f
    };

  dt_gui_presets_add_generic(_("Kodachrome 64 like"), self->op,
                             self->version(), &p2, sizeof(p2),
                             1, DEVELOP_BLEND_CS_RGB_SCENE);

  // Kodak Portra 400
  dt_iop_colorequal_params_t p3 =
    { .threshold       = 0.199f,
      .smoothing_hue   = 1.0f,
      .contrast        = -0.375f,
      .white_level     = 1.0f,
      .chroma_size     = 1.5f,
      .param_size      = 1.0f,
      .use_filter      = TRUE,

      .sat_red         = 1.0f + 0.0692f,
      .sat_orange      = 1.0f + 0.0503f,
      .sat_yellow      = 1.0f - 0.0000f,
      .sat_green       = 1.0f - 0.0000f,
      .sat_cyan        = 1.0f - 0.0000f,
      .sat_blue        = 1.0f - 0.0000f,
      .sat_lavender    = 1.0f - 0.0000f,
      .sat_magenta     = 1.0f - 0.0000f,

      .hue_red         = 9.88f,
      .hue_orange      = -4.39,
      .hue_yellow      = 15.37f,
      .hue_green       = 8.78f,
      .hue_cyan        = 2.20f,
      .hue_blue        = -19.76f,
      .hue_lavender    = -3.29f,
      .hue_magenta     = 0.0f,

      .bright_red      = 1.0f + 0.0881f,
      .bright_orange   = 1.0f + 0.0629f,
      .bright_yellow   = 1.0f + 0.0629f,
      .bright_green    = 1.0f - 0.1069f,
      .bright_cyan     = 1.0f - 0.1069f,
      .bright_blue     = 1.0f - 0.1006f,
      .bright_lavender = 1.0f - 0.0189f,
      .bright_magenta  = 1.0f - 0.0000f,

      .hue_shift       = -23.0f
    };

  dt_gui_presets_add_generic(_("Kodak Portra 400 like"), self->op,
                             self->version(), &p3, sizeof(p3),
                             1, DEVELOP_BLEND_CS_RGB_SCENE);

  // Teal & Orange
  dt_iop_colorequal_params_t p4 =
    { .threshold       = 0.184f,
      .smoothing_hue   = 0.52f,
      .contrast        = -0.3f,
      .white_level     = 1.0f,
      .chroma_size     = 1.5f,
      .param_size      = 1.0f,
      .use_filter      = TRUE,

      .sat_red         = 1.0f + 0.1572f,
      .sat_orange      = 1.0f - 0.0063f,
      .sat_yellow      = 1.0f - 0.3270f,
      .sat_green       = 1.0f - 0.0377f,
      .sat_cyan        = 1.0f - 0.0000f,
      .sat_blue        = 1.0f + 0.0126f,
      .sat_lavender    = 1.0f - 0.0000f,
      .sat_magenta     = 1.0f - 0.0000f,

      .hue_red         = 15.37f,
      .hue_orange      = -24.15f,
      .hue_yellow      = 75.74f,
      .hue_green       = 42.81f,
      .hue_cyan        = 2.20f,
      .hue_blue        = -36.22f,
      .hue_lavender    = 2.20f,
      .hue_magenta     = 42.81f,

      .bright_red      = 1.0f - 0.0000f,
      .bright_orange   = 1.0f - 0.0000f,
      .bright_yellow   = 1.0f - 0.0000f,
      .bright_green    = 1.0f - 0.0000f,
      .bright_cyan     = 1.0f - 0.0000f,
      .bright_blue     = 1.0f - 0.0000f,
      .bright_lavender = 1.0f - 0.0000f,
      .bright_magenta  = 1.0f - 0.0000f,

      .hue_shift       = 0.0f
    };

  dt_gui_presets_add_generic(_("teal & orange"), self->op,
                             self->version(), &p4, sizeof(p4),
                             1, DEVELOP_BLEND_CS_RGB_SCENE);
}

void gui_focus(dt_iop_module_t *self, gboolean in)
{
  dt_iop_colorequal_gui_data_t *g = self->gui_data;
  if(!in)
  {
    dt_iop_color_picker_reset(self, FALSE);
    const gboolean buttons = g->mask_mode != 0;
    dt_bauhaus_widget_set_quad_active(g->param_size, FALSE);
    dt_bauhaus_widget_set_quad_active(g->threshold, FALSE);
    dt_bauhaus_widget_set_quad_active(g->hue_shift, FALSE);
    g->picking = FALSE;
    g->mask_mode = 0;
    if(buttons) dt_dev_reprocess_center(self->dev);
  }
}

#define MINJZ 0.0001f
static inline float _get_hueval(const float hue)
{
  const float b = hue - ANGLE_SHIFT / 360.0f;
  return b < 0.0f ? b + 1.0f : b;
}

static void _draw_color_picker(dt_iop_module_t *self,
                               cairo_t *cr,
                               dt_iop_colorequal_params_t *p,
                               dt_iop_colorequal_gui_data_t *g,
                               const double width,
                               const double height)
{
  if(!(self->request_color_pick == DT_REQUEST_COLORPICK_MODULE))
    return;

  // only visualize for decent brightness & saturation
  if(self->picked_color[0] < MINJZ || self->picked_color[1] < MINJZ)
    return;

  float mean_alpha = 0.6f;

  float hav  = self->picked_color[2];
  float hmax = self->picked_color_max[2];
  float hmin = self->picked_color_min[2];

  const float hava  = self->picked_color[3];
  const float hmina = self->picked_color_min[3];
  const float hmaxa = self->picked_color_max[3];

  if(hmax - hmin > hmaxa - hmina)
  {
    hmax = hmaxa < 0.5f ? hmaxa + 0.5f  : hmaxa - 0.5f;
    hmin = hmina < 0.5f ? hmina + 0.5f  : hmina - 0.5f;
    hav  = hava < 0.5f  ? hava + 0.5f   : hava - 0.5f;
  }

  const float xmin = width * _get_hueval(hmin);
  const float xmax = width * _get_hueval(hmax);
  if(xmax != xmin)
  {
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.3);
    mean_alpha -= 0.3f;
    if(xmax > xmin)
      cairo_rectangle(cr, xmin, 0.0, xmax - xmin, height);
    else
    {
      cairo_rectangle(cr, 0.0, 0.0, xmax, height);
      cairo_rectangle(cr, xmin, 0.0, width - xmin, height);
    }
    cairo_fill(cr);
  }

  cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, mean_alpha);
  const float xav = width * _get_hueval(hav);
  cairo_move_to(cr, xav, 0.0);
  cairo_line_to(cr, xav, height);
  cairo_stroke(cr);
}
#undef MINJZ

static gboolean _iop_colorequalizer_draw(GtkWidget *widget,
                                         cairo_t *crf,
                                         dt_iop_module_t *self)
{
  dt_iop_colorequal_gui_data_t *g = self->gui_data;
  dt_iop_colorequal_params_t *p = self->params;

  // Cache the graph objects to avoid recomputing all the view at each redraw
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  GtkStyleContext *context = gtk_widget_get_style_context(widget);

  cairo_surface_t *cst = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                                       allocation.width,
                                                       allocation.height);
  PangoFontDescription *desc =
    pango_font_description_copy_static(darktable.bauhaus->pango_font_desc);
  cairo_t *cr = cairo_create(cst);
  PangoLayout *layout = pango_cairo_create_layout(cr);

  const gint font_size = pango_font_description_get_size(desc);
  pango_font_description_set_size(desc, 0.95 * font_size);
  pango_layout_set_font_description(layout, desc);
  pango_cairo_context_set_resolution(pango_layout_get_context(layout), darktable.gui->dpi);

  char text[256];

  // Get the text line height for spacing
  PangoRectangle ink;
  snprintf(text, sizeof(text), "X");
  pango_layout_set_text(layout, text, -1);
  pango_layout_get_pixel_extents(layout, &ink, NULL);
  const float line_height = ink.height;

  const float inset = DT_PIXEL_APPLY_DPI(4);
  const float margin_top = inset;
  const float margin_bottom = line_height + 2.0 * inset;
  const float margin_left = 0.0;
  const float margin_right = 0.0;

  const float graph_width =
    allocation.width - margin_right - margin_left;   // align the right border on sliders
  const float graph_height =
    allocation.height - margin_bottom - margin_top; // give room to nodes
  g->graph_height = graph_height;

  gtk_render_background(context, cr, 0.0, 0.0, allocation.width, allocation.height);

  // draw x gradient as axis legend
  cairo_pattern_t *grad = cairo_pattern_create_linear(margin_left, 0.0, graph_width, 0.0);
  if(g->gamut_LUT)
  {
    for(int k = 0; k < 360; k++)
    {
      const float hue = _deg_to_rad((float)k);
      dt_aligned_pixel_t RGB = { 1.f };
      _build_dt_UCS_HSB_gradients((dt_aligned_pixel_t){ hue, g->max_saturation,
                                                        SLIDER_BRIGHTNESS, 1.0f },
        RGB, g->white_adapted_profile, g->gamut_LUT);
      cairo_pattern_add_color_stop_rgba(grad, (double)k / 360.0, RGB[0], RGB[1], RGB[2], 1.0);
    }
  }

  cairo_set_line_width(cr, 0.0);
  cairo_rectangle(cr, margin_left, graph_height + 2 * inset, graph_width, line_height);
  cairo_set_source(cr, grad);
  cairo_fill(cr);
  cairo_pattern_destroy(grad);

  // set the graph as the origin of the coordinates
  cairo_translate(cr, margin_left, margin_top);

  // possibly recalculate and draw background
  if(!g->gradients_cached)
    _init_graph_backgrounds(g, graph_width, graph_height, g->gamut_LUT);

  cairo_rectangle(cr, 0.0, 0.0, graph_width, graph_height);
  cairo_set_source_surface(cr, g->b_surface[g->channel], 0.0, 0.0);
  cairo_fill(cr);

  cairo_rectangle(cr, 0, 0, graph_width, graph_height);
  cairo_clip(cr);

  // draw grid
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(0.5));
  set_color(cr, darktable.bauhaus->graph_border);
  dt_draw_grid(cr, 8, 0, 0, graph_width, graph_height);

  // draw ground level
  set_color(cr, darktable.bauhaus->graph_fg);
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.0));
  dt_draw_line(cr, 0.0, 0.5 * graph_height, graph_width, 0.5 * graph_height);
  cairo_stroke(cr);

  GdkRGBA fg_color = darktable.bauhaus->graph_fg;
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(2.0));
  set_color(cr, fg_color);

  // Build the curve LUT and plotting params for the current channel
  g->LUT = dt_alloc_align_float(LUT_ELEM);
  float DT_ALIGNED_ARRAY values[NODES];
  float smoothing;
  float offset;
  float factor;
  gboolean clip;

  switch(g->channel)
  {
    case SATURATION:
    {
      _pack_saturation(p, values);
      smoothing = 1.0f;
      clip = TRUE;
      offset = 1.f;
      factor = 0.5f;
      break;
    }
    case HUE:
    {
      _pack_hue(p, values);
      smoothing = p->smoothing_hue;
      clip = FALSE;
      offset = 0.5f;
      factor = 1.f / (2.f * M_PI_F);
      break;
    }
    case BRIGHTNESS:
    default:
    {
      _pack_brightness(p, values);
      smoothing = 1.0f;
      clip = TRUE;
      offset = 1.0f;
      factor = 0.5f;
      break;
    }
  }

  _periodic_RBF_interpolate(values, 1.f / smoothing * M_PI_F, g->LUT, 0.0f, clip);

  const float dx = p->hue_shift / 360.0f;
  const int first = -dx * 360;
  for(int k = first; k < (360 + first); k++)
  {
    const float x = ((float)k / (float)(360 - 1) + dx) * graph_width;
    float hue = _deg_to_rad(k);
    hue = (hue < M_PI_F) ? hue : -2.f * M_PI_F + hue; // The LUT is defined in [-pi; pi[
    const float y = (offset - lookup_gamut(g->LUT, hue) * factor) * graph_height;

    if(k == first)
      cairo_move_to(cr, x, y);
    else
      cairo_line_to(cr, x, y);
  }
  cairo_stroke(cr);

  // draw nodes positions
  for(int k = 0; k < NODES + 1; k++)
  {
    float hue = _get_hue_node(k, 0.0f); // in radians
    const float xn = (k / ((float)NODES) + dx    ) * graph_width;
    hue = (hue < M_PI_F) ? hue : -2.f * M_PI_F + hue; // The LUT is defined in [-pi; pi[
    const float yn = (offset - lookup_gamut(g->LUT, hue) * factor) * graph_height;

    // fill bars
    cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(6));
    set_color(cr, darktable.bauhaus->color_fill);
    dt_draw_line(cr, xn, 0.5 * graph_height, xn, yn);
    cairo_stroke(cr);

    // bullets
    cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(3));
    cairo_arc(cr, xn, yn, DT_PIXEL_APPLY_DPI(4), 0, 2. * M_PI);
    set_color(cr, darktable.bauhaus->graph_fg);
    cairo_stroke_preserve(cr);

    // record nodes positions for motion events
    g->points[k][0] = xn;
    g->points[k][1] = yn;

    if(g->on_node && g->selected == k % NODES)
      set_color(cr, darktable.bauhaus->graph_fg);
    else
      set_color(cr, darktable.bauhaus->graph_bg);

    cairo_fill(cr);
  }

  dt_free_align(g->LUT);

  if(self->enabled && dt_iop_has_focus(self) && g->picking)
    _draw_color_picker(self, cr, p, g, (double)graph_width, (double)graph_height);

  cairo_restore(cr);
  // restore font size
  pango_font_description_set_size(desc, font_size);
  pango_layout_set_font_description(layout, desc);

  cairo_destroy(cr);
  cairo_set_source_surface(crf, cst, 0, 0);
  cairo_paint(crf);
  cairo_surface_destroy(cst);
  g_object_unref(layout);
  pango_font_description_free(desc);
  return FALSE;
}

static void _pipe_RGB_to_Ych(dt_iop_module_t *self,
                             dt_dev_pixelpipe_t *pipe,
                             const dt_aligned_pixel_t RGB,
                             dt_aligned_pixel_t Ych)
{
  const struct dt_iop_order_iccprofile_info_t *const work_profile =
    dt_ioppr_get_pipe_current_profile_info(self, pipe);
  if(work_profile == NULL) return; // no point

  dt_aligned_pixel_t XYZ_D50 = { 0.0f, 0.0f, 0.0f, 0.0f };
  dt_aligned_pixel_t XYZ_D65 = { 0.0f, 0.0f, 0.0f, 0.0f };

  dt_ioppr_rgb_matrix_to_xyz(RGB, XYZ_D50, work_profile->matrix_in_transposed,
                             work_profile->lut_in,
                             work_profile->unbounded_coeffs_in,
                             work_profile->lutsize,
                             work_profile->nonlinearlut);
  XYZ_D50_to_D65(XYZ_D50, XYZ_D65);
  XYZ_to_Ych(XYZ_D65, Ych);

  if(Ych[2] < 0.f)
    Ych[2] = 2.f * M_PI_F + Ych[2];
}

void color_picker_apply(dt_iop_module_t *self,
                        GtkWidget *picker,
                        dt_dev_pixelpipe_t *pipe)
{
  dt_iop_colorequal_gui_data_t *g = self->gui_data;
  dt_iop_colorequal_params_t *p = self->params;

  if(picker == g->white_level)
  {
    dt_aligned_pixel_t max_Ych = { 0.0f, 0.0f, 0.0f, 0.0f };
    _pipe_RGB_to_Ych(self, pipe, (const float *)self->picked_color_max, max_Ych);

    ++darktable.gui->reset;
    p->white_level = log2f(max_Ych[0]);
    dt_bauhaus_slider_set(g->white_level, p->white_level);
    --darktable.gui->reset;

    gui_changed(self, picker, NULL);
    dt_dev_add_history_item(darktable.develop, self, TRUE);
  }
  else
    gtk_widget_queue_draw(GTK_WIDGET(g->area));
}

static void _picker_callback(GtkWidget *quad, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_colorequal_gui_data_t *g = self->gui_data;

  g->picking = dt_bauhaus_widget_get_quad_active(quad);

  gtk_widget_queue_draw(GTK_WIDGET(g->area));
}

static void _masking_callback_p(GtkWidget *quad, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_colorequal_gui_data_t *g = self->gui_data;
  dt_bauhaus_widget_set_quad_active(g->threshold, FALSE);
  g->mask_mode = (dt_bauhaus_widget_get_quad_active(quad)) ? g->channel + 1 : 0;
  dt_dev_reprocess_center(self->dev);
}

static void _masking_callback_t(GtkWidget *quad, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_colorequal_gui_data_t *g = self->gui_data;
  dt_bauhaus_widget_set_quad_active(g->param_size, FALSE);
  g->mask_mode = (dt_bauhaus_widget_get_quad_active(quad)) ? GRAD_SWITCH + g->channel + 1 : 0;
  dt_dev_reprocess_center(self->dev);
}

static void _channel_tabs_switch_callback(GtkNotebook *notebook,
                                          GtkWidget *page,
                                          guint page_num,
                                          dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_colorequal_gui_data_t *g = self->gui_data;

  // The 4th tab is options, in which case we do nothing
  // For the first 3 tabs, update color channel and redraw the graph
  if(page_num < NUM_CHANNELS)
  {
    g->channel = (dt_iop_colorequal_channel_t)page_num;
  }

  g->page_num = page_num;

  const int old_mask_mode = g->mask_mode;
  const gboolean masking_p = dt_bauhaus_widget_get_quad_active(g->param_size);
  const gboolean masking_t = dt_bauhaus_widget_get_quad_active(g->threshold);
  gui_update(self);

  dt_bauhaus_widget_set_quad_active(g->param_size, masking_p);
  dt_bauhaus_widget_set_quad_active(g->threshold, masking_t);

  g->mask_mode = masking_p ? g->channel + 1 : (masking_t ? GRAD_SWITCH + g->channel + 1 : 0);
  if(g->mask_mode != old_mask_mode)
    dt_dev_reprocess_center(self->dev);

  gtk_widget_queue_draw(GTK_WIDGET(g->area));
}

static GtkWidget *_get_slider(dt_iop_colorequal_gui_data_t *g, int selected)
{
  GtkWidget *w = NULL;

  switch(g->channel)
  {
    case(SATURATION):
      w = g->sat_sliders[selected];
      break;
    case(HUE):
      w = g->hue_sliders[selected];
      break;
    case(BRIGHTNESS):
    default:
      w = g->bright_sliders[selected];
      break;
  }

  gtk_widget_realize(w);
  return w;
}

static void _area_set_value(dt_iop_colorequal_gui_data_t *g,
                            const float graph_height,
                            const float pos)
{
  float factor = .0f;
  float max = .0f;

  GtkWidget *w = _get_slider(g, g->selected);

  if(w)
  {
    switch(g->channel)
    {
       case(SATURATION):
         factor = 0.5f;
         max = 100.0f;
         break;
       case(HUE):
         factor = 1.f / (2.f * M_PI_F);
         max = (100.0f / 180.0f) * 100.0f;
         break;
       case(BRIGHTNESS):
       default:
         factor = 0.5f;
         max = 100.0f;
         break;
    }

    const float val = (0.5f - (pos / graph_height)) * max / factor;
    dt_bauhaus_slider_set_val(w, val);
  }
}

static void _area_set_pos(dt_iop_colorequal_gui_data_t *g,
                          const float pos)
{
  const float graph_height = MAX(1.0f, g->graph_height);
  const float y = CLAMP(pos, 0.0f, graph_height);

  _area_set_value(g, graph_height, y);
}

static void _area_reset_nodes(dt_iop_colorequal_gui_data_t *g)
{
  const float graph_height = MAX(1.0f, g->graph_height);
  const float y = graph_height / 2.0f;

  if(g->on_node)
  {
    _area_set_value(g, graph_height, y);
  }
  else
  {
    for(int k=0; k<NODES; k++)
    {
      g->selected = k;
      _area_set_value(g, graph_height, y);
    }
    g->on_node = FALSE;
  }
}

static gboolean _area_scrolled_callback(GtkWidget *widget,
                                        GdkEventScroll *event,
                                        dt_iop_module_t *self)
{
  dt_iop_colorequal_gui_data_t *g = self->gui_data;

  GtkWidget *w = dt_modifier_is(event->state, GDK_MOD1_MASK)
               ? GTK_WIDGET(g->notebook)
               : _get_slider(g, g->selected);
  return gtk_widget_event(w, (GdkEvent*)event);
}

static gboolean _area_motion_notify_callback(GtkWidget *widget,
                                             GdkEventMotion *event,
                                             dt_iop_module_t *self)
{
  dt_iop_colorequal_gui_data_t *g = self->gui_data;

  if(g->dragging && g->on_node)
    _area_set_pos(g, event->y);
  else
  {
    // look if close to a node
    const float epsilon = DT_PIXEL_APPLY_DPI(10.0);
    const int oldsel = g->selected;
    const int oldon = g->on_node;
    g->selected = (int)(((float)event->x - g->points[0][0])
                        / (g->points[1][0] - g->points[0][0]) + 0.5f) % NODES;
    g->on_node = fabsf(g->points[g->selected][1] - (float)event->y) < epsilon;
    darktable.control->element = g->selected;
    if(oldsel != g->selected || oldon != g->on_node)
      gtk_widget_queue_draw(GTK_WIDGET(g->area));
  }

  return TRUE;
}

static gboolean _area_button_press_callback(GtkWidget *widget,
                                            GdkEventButton *event,
                                            dt_iop_module_t *self)
{
  dt_iop_colorequal_gui_data_t *g = self->gui_data;

  if(event->button == 2
     || (event->button == 1 // Ctrl+Click alias for macOS
         && dt_modifier_is(event->state, GDK_CONTROL_MASK)))
  {
    dt_conf_set_bool("plugins/darkroom/colorequal/show_sliders",
                     gtk_notebook_get_n_pages(g->notebook) != 4);
    gui_update(self);
  }
  else if(event->button == 1)
  {
    if(event->type == GDK_2BUTTON_PRESS)
    {
      _area_reset_nodes(g);
      return TRUE;
    }
    else
    {
      g->dragging = TRUE;
    }
  }
  else
    return gtk_widget_event(_get_slider(g, g->selected), (GdkEvent*)event);

  return FALSE;
}

static gboolean _area_button_release_callback(GtkWidget *widget,
                                              GdkEventButton *event,
                                              dt_iop_module_t *self)
{
  dt_iop_colorequal_gui_data_t *g = self->gui_data;

  if(event->button == 1)
  {
    g->dragging = FALSE;
    return TRUE;
  }

  return FALSE;
}

static gboolean _area_size_callback(GtkWidget *widget,
                                    GdkEventButton *event,
                                    dt_iop_module_t *self)
{
  dt_iop_colorequal_gui_data_t *g = self->gui_data;
  g->gradients_cached = FALSE;
  return FALSE;
}

void gui_changed(dt_iop_module_t *self, GtkWidget *w, void *previous)
{
  dt_iop_colorequal_gui_data_t *g = self->gui_data;
  dt_iop_colorequal_params_t *p = self->params;

  // Get the current display profile
  struct dt_iop_order_iccprofile_info_t *work_profile =
    dt_ioppr_get_pipe_output_profile_info(self->dev->full.pipe);

  // Check if it is different than the one in cache, and update it if needed
  if(work_profile != g->work_profile)
  {
    // Re-init the profiles
    dt_free_align(g->white_adapted_profile);
    g->white_adapted_profile = D65_adapt_iccprofile(work_profile);
    g->work_profile = work_profile;
    g->gradients_cached = FALSE;

    // Regenerate the display gamut LUT - Default to Rec709 D65 aka linear sRGB
    dt_colormatrix_t input_matrix = { { 0.4124564f, 0.3575761f, 0.1804375f, 0.f },
                                      { 0.2126729f, 0.7151522f, 0.0721750f, 0.f },
                                      { 0.0193339f, 0.1191920f, 0.9503041f, 0.f },
                                      { 0.f } };
    if(g->white_adapted_profile != NULL)
      memcpy(input_matrix, g->white_adapted_profile->matrix_in, sizeof(dt_colormatrix_t));
    else
      dt_print(DT_DEBUG_PIPE, "[colorequal] display color space falls back to sRGB");

    dt_UCS_22_build_gamut_LUT(input_matrix, g->gamut_LUT);
    g->max_saturation = get_minimum_saturation(g->gamut_LUT, 0.2f, 1.f);
  }

  // Show guided filter controls only if in use
  const gboolean guiding = p->use_filter;
  gtk_widget_set_visible(GTK_WIDGET(g->threshold), guiding);
  gtk_widget_set_visible(GTK_WIDGET(g->contrast), guiding);
  gtk_widget_set_visible(GTK_WIDGET(g->chroma_size), guiding);
  gtk_widget_set_visible(GTK_WIDGET(g->param_size), guiding);

  // Only show hue smoothing where effective
  gtk_widget_set_visible(GTK_WIDGET(g->smoothing_hue), g->channel == HUE);

  if(w == g->use_filter && !guiding)
    g->mask_mode = 0;

  if((work_profile != g->work_profile) || (w == g->hue_shift))
    _init_sliders(self);

  gtk_widget_queue_draw(GTK_WIDGET(g->area));
}

void gui_cleanup(dt_iop_module_t *self)
{
  dt_iop_colorequal_gui_data_t *g = self->gui_data;
  self->request_color_pick = DT_REQUEST_COLORPICK_OFF;

  if(g->white_adapted_profile)
  {
    dt_free_align(g->white_adapted_profile);
    g->white_adapted_profile = NULL;
  }

  dt_free_align(g->gamut_LUT);

  // Destroy the background cache
  for(dt_iop_colorequal_channel_t chan = 0; chan < NUM_CHANNELS; chan++)
  {
    if(g->b_data[chan])
      free(g->b_data[chan]);
    if(g->b_surface[chan])
      cairo_surface_destroy(g->b_surface[chan]);
  }

  dt_conf_set_int("plugins/darkroom/colorequal/gui_page",
                  gtk_notebook_get_current_page (g->notebook));
}

void gui_update(dt_iop_module_t *self)
{
  dt_iop_colorequal_params_t *p = self->params;
  dt_iop_colorequal_gui_data_t *g = self->gui_data;
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->use_filter), p->use_filter);
  gui_changed(self, NULL, NULL);

  gboolean show_sliders = dt_conf_get_bool("plugins/darkroom/colorequal/show_sliders");

  // reset masking
  g->mask_mode = 0;
  dt_bauhaus_widget_set_quad_active(g->param_size, FALSE);
  dt_bauhaus_widget_set_quad_active(g->threshold, FALSE);

  const int nbpage = gtk_notebook_get_n_pages(g->notebook);
  if((nbpage == 4) ^ show_sliders)
  {
    if(show_sliders)
      gtk_widget_show(dt_ui_notebook_page(g->notebook, N_("options"), _("options")));
    else
      gtk_notebook_remove_page(g->notebook, 3);

    GtkDarktableExpander *exp = DTGTK_EXPANDER(g->cs.expander);
    gtk_widget_set_visible(dtgtk_expander_get_header(exp), !show_sliders);
    gtk_widget_set_name(GTK_WIDGET(g->cs.container), show_sliders ? NULL : "collapsible");
    gtk_revealer_set_reveal_child(GTK_REVEALER(exp->frame), show_sliders || exp->expanded);
  }

  // display widgets depending on the selected notebook page
  gtk_widget_set_visible(GTK_WIDGET(g->area), g->page_num < 3);
  gtk_widget_set_visible(GTK_WIDGET(g->hue_shift), g->page_num < 3);

  const char numstr[] = {'0' + (show_sliders ? g->page_num : 3), 0};
  gtk_stack_set_visible_child_name(g->stack, numstr);
}

static float _action_process_colorequal(gpointer target,
                                        dt_action_element_t element,
                                        dt_action_effect_t effect,
                                        float move_size)
{
  dt_iop_module_t *self = g_object_get_data(G_OBJECT(target), "iop-instance");
  dt_iop_colorequal_gui_data_t *g = self->gui_data;

  GtkWidget *w = _get_slider(g, element);
  const int index = dt_action_widget(w)->type - DT_ACTION_TYPE_WIDGET - 1;
  const dt_action_def_t *def = darktable.control->widget_definitions->pdata[index];

  return def->process(w, DT_ACTION_ELEMENT_DEFAULT, effect, move_size);
}

static const dt_action_element_def_t _action_elements_colorequal[]
  = { { N_("red"      ), dt_action_effect_value },
      { N_("orange"   ), dt_action_effect_value },
      { N_("yellow"   ), dt_action_effect_value },
      { N_("green"    ), dt_action_effect_value },
      { N_("cyan"     ), dt_action_effect_value },
      { N_("blue"     ), dt_action_effect_value },
      { N_("lavender" ), dt_action_effect_value },
      { N_("magenta"  ), dt_action_effect_value },
      { NULL } };

static const dt_action_def_t _action_def_coloreq
  = { N_("color equalizer"),
      _action_process_colorequal,
      _action_elements_colorequal };

void gui_init(dt_iop_module_t *self)
{
  dt_iop_colorequal_gui_data_t *g = IOP_GUI_ALLOC(colorequal);

  // Init the color profiles and cache them
  struct dt_iop_order_iccprofile_info_t *work_profile = NULL;
  if(self->dev)
    work_profile = dt_ioppr_get_pipe_output_profile_info(self->dev->full.pipe);
  if(g->white_adapted_profile)
    dt_free_align(g->white_adapted_profile);
  g->white_adapted_profile = D65_adapt_iccprofile(work_profile);
  g->work_profile = work_profile;
  g->gradients_cached = FALSE;
  g->on_node = FALSE;
  for(dt_iop_colorequal_channel_t chan = 0; chan < NUM_CHANNELS; chan++)
  {
    g->b_data[chan] = NULL;
    g->b_surface[chan] = NULL;
  }

  // Init the display gamut LUT - Default to Rec709 D65 aka linear sRGB
  g->gamut_LUT = dt_alloc_align_float(LUT_ELEM);
  dt_colormatrix_t input_matrix = { { 0.4124564f, 0.3575761f, 0.1804375f, 0.f },
                                    { 0.2126729f, 0.7151522f, 0.0721750f, 0.f },
                                    { 0.0193339f, 0.1191920f, 0.9503041f, 0.f },
                                    { 0.f } };
  if(g->white_adapted_profile)
    memcpy(input_matrix, g->white_adapted_profile->matrix_in, sizeof(dt_colormatrix_t));

  dt_UCS_22_build_gamut_LUT(input_matrix, g->gamut_LUT);
  g->max_saturation = get_minimum_saturation(g->gamut_LUT, 0.2f, 1.f);

  // start building top level widget
  static dt_action_def_t notebook_def = { };
  g->notebook = dt_ui_notebook_new(&notebook_def);
  dt_action_define_iop(self, NULL, N_("page"), GTK_WIDGET(g->notebook), &notebook_def);
  g_signal_connect(G_OBJECT(g->notebook), "switch_page",
                   G_CALLBACK(_channel_tabs_switch_callback), self);

  // graph
  g->area = GTK_DRAWING_AREA
    (dt_ui_resize_wrap(NULL,
                       0,
                       "plugins/darkroom/colorequal/graphheight"));

  g_object_set_data(G_OBJECT(g->area), "iop-instance", self);
  dt_action_define_iop(self, NULL, N_("graph"), GTK_WIDGET(g->area), &_action_def_coloreq);
  gtk_widget_set_tooltip_text(GTK_WIDGET(g->area), _("double-click to reset the curve\nmiddle-click to toggle sliders visibility\nalt+scroll to change page"));
  gtk_widget_set_can_focus(GTK_WIDGET(g->area), TRUE);
  gtk_widget_add_events(GTK_WIDGET(g->area),
                        GDK_BUTTON_PRESS_MASK
                        | GDK_POINTER_MOTION_MASK
                        | GDK_BUTTON_RELEASE_MASK
                        | GDK_SCROLL_MASK
                        | GDK_SMOOTH_SCROLL_MASK);
  g_signal_connect(G_OBJECT(g->area), "draw", G_CALLBACK(_iop_colorequalizer_draw), self);
  g_signal_connect(G_OBJECT(g->area), "button-press-event",
                   G_CALLBACK(_area_button_press_callback), self);
  g_signal_connect(G_OBJECT(g->area), "button-release-event",
                   G_CALLBACK(_area_button_release_callback), self);
  g_signal_connect(G_OBJECT(g->area), "motion-notify-event",
                   G_CALLBACK(_area_motion_notify_callback), self);
  g_signal_connect(G_OBJECT(g->area), "scroll-event",
                   G_CALLBACK(_area_scrolled_callback), self);
  g_signal_connect(G_OBJECT(g->area), "size_allocate",
                   G_CALLBACK(_area_size_callback), self);

  GtkWidget *box = self->widget = dt_gui_vbox(g->notebook, g->area);
  g->hue_shift = dt_color_picker_new_with_cst(self, DT_COLOR_PICKER_POINT_AREA | DT_COLOR_PICKER_DENOISE,
                 dt_bauhaus_slider_from_params(self, "hue_shift"), IOP_CS_JZCZHZ);
  dt_bauhaus_slider_set_format(g->hue_shift, "°");
  dt_bauhaus_slider_set_digits(g->hue_shift, 0);
  gtk_widget_set_tooltip_text(g->hue_shift,
                              _("shift nodes to lower or higher hue"));

  dt_bauhaus_widget_set_quad_tooltip(g->hue_shift,
    _("pick hue from image and visualize it\nctrl+click to select an area"));
  g_signal_connect(G_OBJECT(g->hue_shift), "quad-pressed", G_CALLBACK(_picker_callback), self);
  gtk_widget_set_name(g->hue_shift, "keep-active");
  g->picking = FALSE;

  g->stack = GTK_STACK(gtk_stack_new());
  dt_gui_box_add(box, g->stack);
  dt_action_define_iop(self, NULL, N_("sliders"), GTK_WIDGET(g->stack), NULL);
  gtk_stack_set_homogeneous(g->stack, FALSE);
  // this should really be set in gui_update depending on whether sliders are
  // shown to prevent the module size changing when changing tabs
  // (as is the custom elsewhere and less confusing)
  // but since graph is hidden anyway under the options tab this is apparently
  // not a requirement here

  dt_iop_module_t *sect = NULL;
#define GROUP_SLIDERS(num, page, tooltip)                  \
  dt_ui_notebook_page(g->notebook, page, tooltip);         \
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0); \
  gtk_stack_add_named(g->stack, self->widget, num);        \
  sect = DT_IOP_SECTION_FOR_PARAMS(self, page);

  GROUP_SLIDERS("0", N_("hue"), _("change hue hue-wise"))
  g->hue_sliders[0] = g->hue_red =
    dt_bauhaus_slider_from_params(sect, "hue_red");
  g->hue_sliders[1] = g->hue_orange =
    dt_bauhaus_slider_from_params(sect, "hue_orange");
  g->hue_sliders[2] = g->hue_yellow =
    dt_bauhaus_slider_from_params(sect, "hue_yellow");
  g->hue_sliders[3] = g->hue_green =
    dt_bauhaus_slider_from_params(sect, "hue_green");
  g->hue_sliders[4] = g->hue_cyan =
    dt_bauhaus_slider_from_params(sect, "hue_cyan");
  g->hue_sliders[5] = g->hue_blue =
    dt_bauhaus_slider_from_params(sect, "hue_blue");
  g->hue_sliders[6] = g->hue_lavender =
    dt_bauhaus_slider_from_params(sect, "hue_lavender");
  g->hue_sliders[7] = g->hue_magenta =
    dt_bauhaus_slider_from_params(sect, "hue_magenta");

  GROUP_SLIDERS("1", N_("saturation"), _("change saturation hue-wise"))
  g->sat_sliders[0] = g->sat_red =
    dt_bauhaus_slider_from_params(sect, "sat_red");
  g->sat_sliders[1] = g->sat_orange =
    dt_bauhaus_slider_from_params(sect, "sat_orange");
  g->sat_sliders[2] = g->sat_yellow =
    dt_bauhaus_slider_from_params(sect, "sat_yellow");
  g->sat_sliders[3] = g->sat_green =
    dt_bauhaus_slider_from_params(sect, "sat_green");
  g->sat_sliders[4] = g->sat_cyan =
    dt_bauhaus_slider_from_params(sect, "sat_cyan");
  g->sat_sliders[5] = g->sat_blue =
    dt_bauhaus_slider_from_params(sect, "sat_blue");
  g->sat_sliders[6] = g->sat_lavender =
    dt_bauhaus_slider_from_params(sect, "sat_lavender");
  g->sat_sliders[7] = g->sat_magenta =
    dt_bauhaus_slider_from_params(sect, "sat_magenta");

  GROUP_SLIDERS("2", N_("brightness"), _("change brightness hue-wise"))
  g->bright_sliders[0] = g->bright_red =
    dt_bauhaus_slider_from_params(sect, "bright_red");
  g->bright_sliders[1] = g->bright_orange =
    dt_bauhaus_slider_from_params(sect, "bright_orange");
  g->bright_sliders[2] = g->bright_yellow =
    dt_bauhaus_slider_from_params(sect, "bright_yellow");
  g->bright_sliders[3] = g->bright_green =
    dt_bauhaus_slider_from_params(sect, "bright_green");
  g->bright_sliders[4] = g->bright_cyan =
    dt_bauhaus_slider_from_params(sect, "bright_cyan");
  g->bright_sliders[5] = g->bright_blue =
    dt_bauhaus_slider_from_params(sect, "bright_blue");
  g->bright_sliders[6] = g->bright_lavender =
    dt_bauhaus_slider_from_params(sect, "bright_lavender");
  g->bright_sliders[7] = g->bright_magenta =
    dt_bauhaus_slider_from_params(sect, "bright_magenta");

  GtkWidget *options = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);
  gtk_stack_add_named(g->stack, options, "3");
  dt_gui_new_collapsible_section
    (&g->cs,
     "plugins/darkroom/colorequal/expand_options",
     _("options"),
     GTK_BOX(options),
     DT_ACTION(self));
  self->widget = GTK_WIDGET(g->cs.container);

  g->white_level = dt_color_picker_new(self, DT_COLOR_PICKER_AREA,
                                       dt_bauhaus_slider_from_params(self, "white_level"));
  dt_bauhaus_slider_set_soft_range(g->white_level, -2., +2.);
  dt_bauhaus_slider_set_format(g->white_level, _(" EV"));
  gtk_widget_set_tooltip_text(g->white_level,
                              _("the white level set manually or via the picker restricts brightness corrections\n"
                                "to stay below the defined level. the default is fine for most images."));

  g->smoothing_hue = dt_bauhaus_slider_from_params(self, "smoothing_hue");
  gtk_widget_set_tooltip_text(g->smoothing_hue,
                              _("change for sharper or softer hue curve"));

  g->use_filter = dt_bauhaus_toggle_from_params(self, "use_filter");
  gtk_widget_set_tooltip_text(g->use_filter,
                              _("restrict effect by using a guided filter based on hue and saturation"));

  g->chroma_size = dt_bauhaus_slider_from_params(self, "chroma_size");
  dt_bauhaus_slider_set_digits(g->chroma_size, 1);
  dt_bauhaus_slider_set_format(g->chroma_size, _(_(" px")));
  gtk_widget_set_tooltip_text(g->chroma_size,
                              _("set radius of the guided filter chroma analysis (hue).\n"
                                "increase if there is large local variance of hue or strong chroma noise."));

  g->threshold = dt_bauhaus_slider_from_params(self, "threshold");
  dt_bauhaus_slider_set_digits(g->threshold, 3);
  dt_bauhaus_slider_set_format(g->threshold, "%");
  dt_bauhaus_widget_set_quad_paint(g->threshold, dtgtk_cairo_paint_showmask, 0, NULL);
  dt_bauhaus_widget_set_quad_toggle(g->threshold, TRUE);
  dt_bauhaus_widget_set_quad_active(g->threshold, FALSE);
  g_signal_connect(G_OBJECT(g->threshold), "quad-pressed", G_CALLBACK(_masking_callback_t), self);
  dt_bauhaus_widget_set_quad_tooltip(g->threshold,
    _("visualize weighting function on changed output and view weighting curve.\n"
      "red shows possibly changed data, blueish parts will not be changed."));

  gtk_widget_set_tooltip_text(g->threshold,
                              _("set saturation threshold for the guided filter.\n"
                                " - decrease to allow changes in areas with low chromaticity\n"
                                " - increase to restrict changes to higher chromaticities\n"
                                "   increases contrast and avoids brightness changes in low chromaticity areas"));

  g->contrast = dt_bauhaus_slider_from_params(self, "contrast");
  dt_bauhaus_slider_set_digits(g->contrast, 3);
  gtk_widget_set_tooltip_text(g->contrast,
                              _("set saturation contrast for the guided filter.\n"
                                " - increase to favor sharp transitions between saturations leading to higher contrast\n"
                                " - decrease for smoother transitions"));

  g->param_size = dt_bauhaus_slider_from_params(self, "param_size");
  dt_bauhaus_slider_set_digits(g->param_size, 1);
  dt_bauhaus_slider_set_format(g->param_size, _(_(" px")));
  gtk_widget_set_tooltip_text(g->param_size, _("set radius of applied parameters for the guided filter"));

  dt_bauhaus_widget_set_quad_paint(g->param_size, dtgtk_cairo_paint_showmask, 0, NULL);
  dt_bauhaus_widget_set_quad_toggle(g->param_size, TRUE);
  dt_bauhaus_widget_set_quad_active(g->param_size, FALSE);
  g_signal_connect(G_OBJECT(g->param_size), "quad-pressed", G_CALLBACK(_masking_callback_p), self);
  dt_bauhaus_widget_set_quad_tooltip(g->param_size,
    _("visualize changed output for the selected tab.\n"
      "red shows increased values, blue decreased."));

  _init_sliders(self);

  // restore the previously saved active tab
  const guint active_page = dt_conf_get_int("plugins/darkroom/colorequal/gui_page");
  if(active_page < 3)
  {
    gtk_widget_show(gtk_notebook_get_nth_page(g->notebook, active_page));
    gtk_notebook_set_current_page(g->notebook, active_page);
  }
  g->channel = (active_page >= NUM_CHANNELS) ? SATURATION : active_page;
  g->page_num = active_page;

  self->widget = GTK_WIDGET(box);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
