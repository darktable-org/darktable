/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU General Public License for more details.
*/

// our includes go first:
#include "bauhaus/bauhaus.h"
#include "common/chromatic_adaptation.h"
#include "common/color_picker.h"
#include "common/darktable_ucs_22_helpers.h"
#include "common/dtpthread.h"
#include "common/eigf.h"
#include "common/fast_guided_filter.h"
#include "common/guided_filter.h"
#include "common/gamut_mapping.h"
#include "common/imagebuf.h"
#include "common/math.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "develop/openmp_maths.h"
#include "develop/tiling.h"
#include "dtgtk/button.h"
#include "dtgtk/drawingarea.h"
#include "gui/color_picker_proxy.h"
#include "gui/draw.h"
#include "gui/gtk.h"
#include "gui/accelerators.h"
#include "gui/presets.h"
#include "iop/iop_api.h"

#define DT_IOP_SATCURVE_MAXNODES 20
#define DT_IOP_SATCURVE_RES 256
#define DT_IOP_SATCURVE_HIST_RES 256
#define DT_IOP_SATCURVE_INSET DT_PIXEL_APPLY_DPI(5)
#define DT_IOP_SATCURVE_GRADIENT_SIZE DT_PIXEL_APPLY_DPI(8)
#define DT_IOP_SATCURVE_GRADIENT_GAP DT_PIXEL_APPLY_DPI(2)
#define DT_IOP_SATCURVE_MIN_X_DISTANCE 0.0025f
#define DT_IOP_SATCURVE_GAMUT_STEPS 92
#define DT_IOP_SATCURVE_CHANNELS 2
// squared normalized distance below which a curve node is considered grabbed by
// the pointer (radius ~0.05). Shared by the button-press and motion handlers so
// a node that is hovered is also the one a click picks up and drags.
#define DT_IOP_SATCURVE_NODE_GRAB_SQ 0.0025f

DT_MODULE_INTROSPECTION(1, dt_iop_satcurve_params_t)

typedef enum dt_iop_satcurve_formula_t
{
  DT_IOP_SATCURVE_JZAZBZ = 0,
  DT_IOP_SATCURVE_DTUCS = 1
} dt_iop_satcurve_formula_t;

typedef enum dt_iop_satcurve_channel_t
{
  DT_IOP_SATCURVE_CHANNEL_SATURATION = 0,
  DT_IOP_SATCURVE_CHANNEL_BRILLIANCE = 1
} dt_iop_satcurve_channel_t;

typedef struct dt_iop_satcurve_node_t
{
  float x, y;
} dt_iop_satcurve_node_t;

typedef struct dt_iop_satcurve_channel_params_t
{
  dt_iop_satcurve_node_t curve[DT_IOP_SATCURVE_MAXNODES];
  int curve_num_nodes;
  int curve_type;
} dt_iop_satcurve_channel_params_t;

typedef struct dt_iop_satcurve_params_t
{
  dt_iop_satcurve_channel_params_t channel[DT_IOP_SATCURVE_CHANNELS];
  dt_iop_satcurve_formula_t formula; // $DEFAULT: 1 $DESCRIPTION: "saturation formula"

  // fast scalar guided filter controls
  gboolean use_guided_filter; // $DEFAULT: FALSE $DESCRIPTION: "use guided filter"
  float gf_radius;            // $MIN: 0.5 $MAX: 200.0 $DEFAULT: 10.0 $DESCRIPTION: "filter radius"
  float gf_feathering;        // $MIN: 0.1 $MAX: 50.0 $DEFAULT: 1.0 $DESCRIPTION: "edge feathering"
  int gf_iterations;          // $MIN: 1 $MAX: 10 $DEFAULT: 1 $DESCRIPTION: "iterations"
} dt_iop_satcurve_params_t;

typedef struct dt_iop_satcurve_channel_data_t
{
  dt_draw_curve_t *curve;
  int curve_num_nodes;
  int curve_type;
  float *lut;
} dt_iop_satcurve_channel_data_t;

typedef struct dt_iop_satcurve_data_t
{
  dt_iop_satcurve_formula_t formula;
  dt_iop_satcurve_channel_data_t channel[DT_IOP_SATCURVE_CHANNELS];
  float *gamut_lut;
  gboolean lut_inited;
  const struct dt_iop_order_iccprofile_info_t *work_profile;

  gboolean use_guided_filter;
  float gf_radius;
  float gf_feathering;
  int gf_iterations;
} dt_iop_satcurve_data_t;

typedef struct dt_iop_satcurve_gui_channel_t
{
  dt_draw_curve_t *curve;
  int curve_num_nodes;
  int curve_type;
  float draw_ys[DT_IOP_SATCURVE_RES];
} dt_iop_satcurve_gui_channel_t;

typedef struct dt_iop_satcurve_gui_data_t
{
  GtkDrawingArea *area;
  GtkWidget *colorpicker;
  GtkWidget *formula;
  GtkWidget *show_saturation_mask;
  GtkNotebook *notebook;

  GtkWidget *use_guided_filter;
  GtkWidget *gf_radius;
  GtkWidget *gf_feathering;
  GtkWidget *gf_iterations;
  dt_gui_collapsible_section_t gf_section;

  dt_iop_satcurve_gui_channel_t channel[DT_IOP_SATCURVE_CHANNELS];
  dt_iop_satcurve_channel_t active_channel;
  int selected;
  gboolean dragging;

  float histogram[DT_IOP_SATCURVE_HIST_RES];
  float histogram_max;
  dt_pthread_mutex_t histogram_lock;

  gboolean picker_valid;
  float picked_s;
  float picked_s_min;
  float picked_s_max;

  gboolean mask_display;
} dt_iop_satcurve_gui_data_t;

typedef struct dt_iop_satcurve_global_data_t
{
  int kernel_satcurvergb;
  int kernel_satcurve_histogram;
  int kernel_satcurve_mask;
  int kernel_satcurve_scalar_mask;
  int kernel_satcurve_mask_from_scalar;
  int kernel_fgf_resample;
  int kernel_fgf_quantize;
  int kernel_fgf_covar_products;
  int kernel_fgf_solve_ab;
  int kernel_fgf_apply_blend;
} dt_iop_satcurve_global_data_t;

typedef struct dt_iop_satcurve_factors_t
{
  float sat_factor;
  float bri_factor;
} dt_iop_satcurve_factors_t;

const char *name()
{
  return _("saturation curve");
}

const char *aliases()
{
  return _("sat vs sat|saturation versus saturation|brilliance curve|satcurve");
}

const char **description(dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("remap saturation and brilliance with curves"),
                                _("corrective or creative"),
                                _("linear, RGB, scene-referred"),
                                _("linear, RGB, scene-referred"),
                                _("linear, RGB, scene-referred"));
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING;
}

int default_group()
{
  return IOP_GROUP_COLOR | IOP_GROUP_GRADING;
}

dt_iop_colorspace_type_t default_colorspace(dt_iop_module_t *self,
                                            dt_dev_pixelpipe_t *pipe,
                                            dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

static inline void reset_channel_curve(dt_iop_satcurve_channel_params_t *c)
{
  c->curve_num_nodes = 2;
  c->curve_type = MONOTONE_HERMITE;
  c->curve[0] = (dt_iop_satcurve_node_t){0.f, .5f};
  c->curve[1] = (dt_iop_satcurve_node_t){1.f, .5f};
}

static inline void reset_params(dt_iop_satcurve_params_t *p)
{
  reset_channel_curve(&p->channel[DT_IOP_SATCURVE_CHANNEL_SATURATION]);
  reset_channel_curve(&p->channel[DT_IOP_SATCURVE_CHANNEL_BRILLIANCE]);
  p->formula = DT_IOP_SATCURVE_DTUCS;

  p->use_guided_filter = FALSE;
  p->gf_radius = 10.0f;
  p->gf_feathering = 1.0f;
  p->gf_iterations = 1;
}

static inline float lookup_lut(const float *lut, const float x)
{
  const float position = CLAMP(x, 0.f, 1.f) * (DT_IOP_SATCURVE_RES - 1);
  const int i = MIN((int)position, DT_IOP_SATCURVE_RES - 2);
  return lut[i] + (position - i) * (lut[i + 1] - lut[i]);
}

static inline float curve_to_factor(const float c)
{
  return MAX(2.f * c, 0.f);
}

static inline float satcurve_lookup_gamut(const float *const gamut_lut, const float h)
{
  const float position = (h + M_PI_F) * (float)LUT_ELEM / DT_2PI_F;
  const int bin0 = ((int)floorf(position)) % LUT_ELEM;
  const int bin1 = (bin0 + 1) % LUT_ELEM;
  const float f = position - floorf(position);
  return gamut_lut[bin0] * (1.f - f) + gamut_lut[bin1] * f;
}

static inline float satcurve_soft_clip(const float x, const float knee, const float maximum)
{
  if (x <= knee)
    return x;

  const float range = maximum - knee;
  if (range <= 0.f)
    return maximum;

  return knee + range * (1.f - expf(-(x - knee) / range));
}

static inline float clip_jz_chroma(const float Jz, const float Cz, const float ch, const float sh)
{
  const float d0 = 1.6295499532821566e-11f;
  const float dd = -0.56f;
  float Iz = (Jz + d0) / (1.f + dd - dd * (Jz + d0));
  Iz = MAX(Iz, 0.f);

  static const dt_colormatrix_t AI_trans = {
      {1.f, 1.f, 1.f, 0.f},
      {.1386050432715393f, -.1386050432715393f, -.0960192420263190f, 0.f},
      {.0580473161561189f, -.0580473161561189f, -.8118918960560390f, 0.f}};

  dt_aligned_pixel_t izab = {Iz, Cz * ch, Cz * sh, 0.f}, lms;
  dt_apply_transposed_color_matrix(izab, AI_trans, lms);

  float max_c = Cz;
  if (lms[0] < 0.f)
    max_c = MIN(max_c, -Iz / (AI_trans[1][0] * ch + AI_trans[2][0] * sh));
  if (lms[1] < 0.f)
    max_c = MIN(max_c, -Iz / (AI_trans[1][1] * ch + AI_trans[2][1] * sh));
  if (lms[2] < 0.f)
    max_c = MIN(max_c, -Iz / (AI_trans[1][2] * ch + AI_trans[2][2] * sh));

  return MAX(max_c, 0.f);
}

static inline float pixel_s_in_norm_jzazbz(const float *const restrict rgb_in,
                                           const dt_colormatrix_t inputmatrix_trans,
                                           const float *const restrict gamut_lut,
                                           float *const restrict h_out)
{
  dt_aligned_pixel_t rgb, xyz, jab;
  copy_pixel(rgb, rgb_in);
  dt_vector_clipneg(rgb);
  dt_apply_transposed_color_matrix(rgb, inputmatrix_trans, xyz);
  dt_XYZ_2_JzAzBz(xyz, jab);

  const float Jz = MAX(jab[0], 0.f);
  const float Cz = dt_fast_hypotf(jab[1], jab[2]);
  const float h = atan2f(jab[2], jab[1]);
  const float gamut = MAX(satcurve_lookup_gamut(gamut_lut, h), FLT_MIN);
  const float s_in = Jz > 0.f ? Cz / Jz : 0.f;

  if (h_out)
    *h_out = h;
  return s_in / gamut;
}

// maximum HSB saturation reachable at the gamut boundary for a given J, h in dt UCS;
// fit constants (15.93.../0.652.../0.600...) come from the gamut-boundary polynomial
// model used throughout darktable_ucs_22_helpers. Single source of truth for this
// computation -- do not re-derive it inline at call sites.
static inline float satcurve_ucs_gamut_saturation(const float J, const float h,
                                                  const float L_white,
                                                  const float *const restrict gamut_lut)
{
  const float max_colorfulness = MAX(satcurve_lookup_gamut(gamut_lut, h), FLT_MIN);
  const float max_chroma = 15.932993652962535f * powf(J / L_white, 0.6523997524738018f) * powf(max_colorfulness, 0.6007557017508491f) / L_white;

  const dt_aligned_pixel_t JCH_gamut_boundary = {J, max_chroma, h, 0.f};
  dt_aligned_pixel_t HSB_gamut_boundary;
  dt_UCS_JCH_to_HSB(JCH_gamut_boundary, HSB_gamut_boundary);

  return MAX(HSB_gamut_boundary[1], FLT_MIN);
}

static inline float pixel_s_in_norm_ucs(const float *const restrict rgb_in,
                                        const dt_colormatrix_t inputmatrix_trans,
                                        const float *const restrict gamut_lut,
                                        const float L_white,
                                        float *const restrict h_out)
{
  dt_aligned_pixel_t rgb, xyz, xyY, JCH, HCB;

  copy_pixel(rgb, rgb_in);
  dt_vector_clipneg(rgb);
  dt_apply_transposed_color_matrix(rgb, inputmatrix_trans, xyz);

  dt_D65_XYZ_to_xyY(xyz, xyY);
  xyY_to_dt_UCS_JCH(xyY, L_white, JCH);
  dt_UCS_JCH_to_HCB(JCH, HCB);

  const float gamut_s = satcurve_ucs_gamut_saturation(JCH[0], JCH[2], L_white, gamut_lut);

  if (h_out)
    *h_out = JCH[2];

  const float saturation = HCB[2] > 0.f ? HCB[1] / HCB[2] : 0.f;
  return saturation / gamut_s;
}

static inline float pixel_s_in_norm(const dt_iop_satcurve_formula_t formula,
                                    const float *const restrict rgb_in,
                                    const dt_colormatrix_t inputmatrix_trans,
                                    const float *const restrict gamut_lut,
                                    const float L_white,
                                    float *const restrict h_out)
{
  if (formula == DT_IOP_SATCURVE_DTUCS)
    return pixel_s_in_norm_ucs(rgb_in, inputmatrix_trans, gamut_lut, L_white, h_out);

  return pixel_s_in_norm_jzazbz(rgb_in, inputmatrix_trans, gamut_lut, h_out);
}

static inline dt_iop_satcurve_channel_params_t *get_active_channel_params(dt_iop_module_t *self)
{
  dt_iop_satcurve_gui_data_t *g = self->gui_data;
  dt_iop_satcurve_params_t *p = self->params;
  return &p->channel[g->active_channel];
}

static inline dt_iop_satcurve_gui_channel_t *get_active_gui_channel(dt_iop_module_t *self)
{
  dt_iop_satcurve_gui_data_t *g = self->gui_data;
  return &g->channel[g->active_channel];
}

static inline gboolean channel_is_neutral(const dt_iop_satcurve_channel_data_t *c)
{
  return c->curve_num_nodes == 2 && fabsf(c->lut[0] - .5f) < 1e-6f && fabsf(c->lut[DT_IOP_SATCURVE_RES - 1] - .5f) < 1e-6f;
}

static inline dt_iop_satcurve_factors_t eval_curve_factors(const dt_iop_satcurve_data_t *d,
                                                           const float s_in_norm)
{
  const float sat_c = CLAMP(lookup_lut(d->channel[DT_IOP_SATCURVE_CHANNEL_SATURATION].lut, s_in_norm), 0.f, 1.f);
  const float bri_c = CLAMP(lookup_lut(d->channel[DT_IOP_SATCURVE_CHANNEL_BRILLIANCE].lut, s_in_norm), 0.f, 1.f);

  return (dt_iop_satcurve_factors_t){
      .sat_factor = curve_to_factor(sat_c),
      .bri_factor = curve_to_factor(bri_c)};
}

// applies log1p compression and publishes bin counts to the GUI histogram;
// shared tail for both the CPU path (_update_sat_histogram) and the GPU path
// (process_cl), which only differ in how they arrive at per-bin pixel counts
static void _commit_sat_histogram(dt_iop_module_t *self, const int *const bins)
{
  dt_iop_satcurve_gui_data_t *g = self->gui_data;
  if (!g)
    return;

  float local_hist[DT_IOP_SATCURVE_HIST_RES];
  float max_val = 0.f;
  for (int i = 0; i < DT_IOP_SATCURVE_HIST_RES; i++)
  {
    local_hist[i] = log1pf((float)bins[i]);
    max_val = MAX(max_val, local_hist[i]);
  }

  dt_pthread_mutex_lock(&g->histogram_lock);
  memcpy(g->histogram, local_hist, sizeof(local_hist));
  g->histogram_max = MAX(max_val, 1e-6f);
  dt_pthread_mutex_unlock(&g->histogram_lock);

  dt_control_queue_redraw_widget(GTK_WIDGET(g->area));
}

static void _update_sat_histogram(dt_iop_module_t *self,
                                  const dt_iop_satcurve_data_t *d,
                                  const dt_colormatrix_t inputmatrix_trans,
                                  const float *const restrict in,
                                  const size_t npixels)
{
  dt_iop_satcurve_gui_data_t *g = self->gui_data;
  if (!g)
    return;

  int local_hist[DT_IOP_SATCURVE_HIST_RES] = {0};
  const float L_white = Y_to_dt_UCS_L_star(1.f);

  DT_OMP_FOR(reduction(+ : local_hist[:DT_IOP_SATCURVE_HIST_RES]))
  for (size_t k = 0; k < npixels; k++)
  {
    const float s_in_norm = pixel_s_in_norm(d->formula, in + 4 * k, inputmatrix_trans, d->gamut_lut, L_white, NULL);
    const int bin = CLAMP((int)(s_in_norm * (DT_IOP_SATCURVE_HIST_RES - 1)),
                          0, DT_IOP_SATCURVE_HIST_RES - 1);
    local_hist[bin]++;
  }

  _commit_sat_histogram(self, local_hist);
}

static inline void apply_sat_and_brilliance_ucs(const dt_iop_satcurve_data_t *d,
                                                dt_aligned_pixel_t xyz,
                                                const float L_white,
                                                const float s_in_norm)
{
  dt_aligned_pixel_t xyY, JCH, HCB, HSB;

  dt_D65_XYZ_to_xyY(xyz, xyY);
  xyY_to_dt_UCS_JCH(xyY, L_white, JCH);
  dt_UCS_JCH_to_HCB(JCH, HCB);

  HSB[0] = HCB[0];
  HSB[1] = HCB[2] > 0.f ? HCB[1] / HCB[2] : 0.f;
  HSB[2] = HCB[2];
  HSB[3] = 0.f;

  const float gamut_s = satcurve_ucs_gamut_saturation(JCH[0], JCH[2], L_white, d->gamut_lut);

  // Compute curve factors based on the passed s_in_norm parameter
  const dt_iop_satcurve_factors_t f = eval_curve_factors(d, s_in_norm);

  HSB[1] = MAX(HSB[1] * f.sat_factor, 0.f);
  HSB[1] = satcurve_soft_clip(HSB[1], .8f * gamut_s, gamut_s);

  dt_UCS_HSB_to_JCH(HSB, JCH);
  dt_UCS_JCH_to_HCB(JCH, HCB);

  HCB[1] = MAX(HCB[1] * f.bri_factor, 0.f);
  HCB[2] = MAX(HCB[2] * f.bri_factor, 0.f);

  dt_UCS_HCB_to_JCH(HCB, JCH);

  const float gamut_s_out = satcurve_ucs_gamut_saturation(JCH[0], JCH[2], L_white, d->gamut_lut);

  dt_aligned_pixel_t HSB_out;
  HSB_out[0] = HCB[0];
  HSB_out[1] = HCB[2] > 0.f ? HCB[1] / HCB[2] : 0.f;
  HSB_out[2] = HCB[2];
  HSB_out[3] = 0.f;

  HSB_out[1] = satcurve_soft_clip(HSB_out[1], .8f * gamut_s_out, gamut_s_out);

  dt_UCS_HSB_to_JCH(HSB_out, JCH);
  dt_UCS_JCH_to_xyY(JCH, L_white, xyY);
  dt_xyY_to_XYZ(xyY, xyz);
}

static inline void apply_sat_and_brilliance_jzazbz(const dt_iop_satcurve_data_t *d,
                                                   dt_aligned_pixel_t xyz,
                                                   const float s_in_norm)
{
  dt_aligned_pixel_t jab;
  dt_XYZ_2_JzAzBz(xyz, jab);

  const float Jz = MAX(jab[0], 0.f);
  const float Cz = dt_fast_hypotf(jab[1], jab[2]);
  const float h = atan2f(jab[2], jab[1]);
  const float ch = cosf(h), sh = sinf(h);
  const float gamut = MAX(satcurve_lookup_gamut(d->gamut_lut, h), FLT_MIN);

  // Compute curve factors based on the passed s_in_norm parameter
  const dt_iop_satcurve_factors_t f = eval_curve_factors(d, s_in_norm);

  const float s_out = satcurve_soft_clip(MAX(s_in_norm * f.sat_factor, 0.f), .8f, 1.f) * gamut;

  const float r = dt_fast_hypotf(Jz, Cz);
  const float inv_norm = 1.f / sqrtf(1.f + s_out * s_out);

  float Jz_tmp = r * inv_norm;
  float Cz_tmp = clip_jz_chroma(Jz_tmp, r * s_out * inv_norm, ch, sh);

  Jz_tmp *= f.bri_factor;
  Cz_tmp = clip_jz_chroma(Jz_tmp, Cz_tmp * f.bri_factor, ch, sh);

  jab[0] = Jz_tmp;
  jab[1] = Cz_tmp * ch;
  jab[2] = Cz_tmp * sh;

  dt_JzAzBz_2_XYZ(jab, xyz);
}

// Compute the normalized saturation for each pixel into a separate buffer,
// analogous to compute_luminance_mask() in toneequal.c.
static inline void compute_saturation_mask(const dt_iop_satcurve_data_t *d,
                                           const dt_colormatrix_t inputmatrix_trans,
                                           const float L_white,
                                           const float *const restrict in,
                                           float *const restrict mask,
                                           const size_t npixels)
{
  DT_OMP_FOR()
  for (size_t k = 0; k < npixels; k++)
  {
    mask[k] = pixel_s_in_norm(d->formula, in + 4 * k, inputmatrix_trans,
                              d->gamut_lut, L_white, NULL);
  }
}

// Render the saturation mask as a color preview. Zero saturation is shown as
// white, full saturation as purple/magenta, matching the chroma gradient end
// color {0.5, 0.0, 0.5} used in blend_gui.c. A sqrt-like gamma makes low
// values easier to see, analogous to display_luminance_mask() in toneequal.c.
static inline void display_saturation_mask(const float *const restrict in,
                                           const float *const restrict mask,
                                           float *const restrict out,
                                           const size_t npixels)
{
  DT_OMP_FOR()
  for (size_t k = 0; k < npixels; k++)
  {
    // white at t = 0, magenta {0.5, 0.0, 0.5} at t = 1
    const float t = sqrtf(CLAMP(mask[k], 0.f, 1.f));
    out[4 * k + 0] = 1.0f - 0.5f * t;
    out[4 * k + 1] = 1.0f - t;
    out[4 * k + 2] = 1.0f - 0.5f * t;
    out[4 * k + 3] = in[4 * k + 3];
  }
}

// Applies the fast scalar guided filter (edge-aware surface blur) in-place on a
// saturation mask, using the guided-filter parameters cached in d. No-op if
// use_guided_filter is off or the parameters are degenerate.
static inline void apply_guided_filter_to_mask(const dt_iop_satcurve_data_t *d,
                                               float *const restrict mask,
                                               const int width,
                                               const int height)
{
  if (!d->use_guided_filter || d->gf_radius < 0.5f || d->gf_iterations <= 0)
    return;

  fast_surface_blur(mask,
                    width,
                    height,
                    d->gf_radius,
                    d->gf_feathering,
                    d->gf_iterations,
                    DT_GF_BLENDING_LINEAR,
                    1.0f,
                    0.0f,
                    exp2f(-14.0f),
                    4.0f);
}

void process(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece,
             const void *const ivoid, void *const ovoid,
             const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_satcurve_data_t *d = piece->data;
  dt_iop_satcurve_gui_data_t *g = self->gui_data;

  const dt_iop_order_iccprofile_info_t *work_profile = dt_ioppr_get_pipe_current_profile_info(self, piece->pipe);
  if (!work_profile || piece->colors != 4)
  {
    dt_iop_image_copy_by_size(ovoid, ivoid, roi_out->width, roi_out->height, piece->colors);
    return;
  }

  dt_colormatrix_t inputmatrix = {{0.0f}};
  dt_colormatrix_t outputmatrix = {{0.0f}};
  dt_colormatrix_t inputmatrix_trans;
  dt_colormatrix_t outputmatrix_trans;

  dt_colormatrix_mul(inputmatrix, XYZ_D50_to_D65_CAT16, work_profile->matrix_in);
  dt_colormatrix_transpose(inputmatrix_trans, inputmatrix);
  dt_colormatrix_mul(outputmatrix, work_profile->matrix_out, XYZ_D65_to_D50_CAT16);
  dt_colormatrix_transpose(outputmatrix_trans, outputmatrix);

  const float *const restrict in = DT_IS_ALIGNED((const float *)ivoid);
  float *const restrict out = DT_IS_ALIGNED((float *)ovoid);
  const size_t npixels = (size_t)roi_out->width * roi_out->height;
  const float L_white = Y_to_dt_UCS_L_star(1.f);

  if (self->dev->gui_attached && dt_pipe_is_full(piece->pipe) && dt_iop_has_focus(self) && piece->pipe == self->dev->full.pipe)
    _update_sat_histogram(self, d, inputmatrix_trans, in, npixels);

  // Display a grayscale preview of the normalized saturation mask.
  if (self->dev->gui_attached && dt_pipe_is_full(piece->pipe) && g && g->mask_display)
  {
    float *const restrict mask = dt_alloc_align_float(npixels);
    if (mask)
    {
      compute_saturation_mask(d, inputmatrix_trans, L_white, in, mask, npixels);

      // if the guided filter is enabled, show the filtered mask that is
      // actually used to modulate the correction, not the raw saturation
      apply_guided_filter_to_mask(d, mask, roi_out->width, roi_out->height);

      display_saturation_mask(in, mask, out, npixels);
      dt_free_align(mask);
      piece->pipe->mask_display = DT_DEV_PIXELPIPE_DISPLAY_PASSTHRU;

      return;
    }
  }

  const gboolean neutral_sat = channel_is_neutral(&d->channel[DT_IOP_SATCURVE_CHANNEL_SATURATION]);
  const gboolean neutral_bri = channel_is_neutral(&d->channel[DT_IOP_SATCURVE_CHANNEL_BRILLIANCE]);

  if (neutral_sat && neutral_bri)
  {
    dt_iop_image_copy_by_size(ovoid, ivoid, roi_out->width, roi_out->height, piece->colors);
    return;
  }

  // Compute filtered saturation mask as input signal for the curves
  float *restrict gf_mask = NULL;

  if (d->use_guided_filter && d->gf_radius >= 0.5f && d->gf_iterations > 0)
  {
    gf_mask = dt_alloc_align_float(npixels);
    if (gf_mask)
    {
      compute_saturation_mask(d, inputmatrix_trans, L_white, in, gf_mask, npixels);
      apply_guided_filter_to_mask(d, gf_mask, roi_out->width, roi_out->height);
    }
    else
    {
      dt_control_log(_("saturation curve: guided filter failed to allocate memory, disabling it for this run"));
    }
  }

  DT_OMP_FOR()
  for (size_t k = 0; k < npixels; k++)
  {
    dt_aligned_pixel_t rgb, xyz, pixout;
    copy_pixel(rgb, in + 4 * k);
    dt_vector_clipneg(rgb);
    dt_apply_transposed_color_matrix(rgb, inputmatrix_trans, xyz);

    // Determine input saturation: either from the filtered mask or directly from the pixel
    const float s_in_norm = gf_mask ? CLAMP(gf_mask[k], 0.f, 1.f)
                                    : pixel_s_in_norm(d->formula, in + 4 * k, inputmatrix_trans, d->gamut_lut, L_white, NULL);

    if (d->formula == DT_IOP_SATCURVE_DTUCS)
      apply_sat_and_brilliance_ucs(d, xyz, L_white, s_in_norm);
    else
      apply_sat_and_brilliance_jzazbz(d, xyz, s_in_norm);

    dt_apply_transposed_color_matrix(xyz, outputmatrix_trans, pixout);
    dt_vector_clipneg(pixout);
    pixout[3] = in[4 * k + 3];

    copy_pixel_nontemporal(out + 4 * k, pixout);
  }
  dt_omploop_sfence();

  if (gf_mask)
    dt_free_align(gf_mask);
}

#if HAVE_OPENCL

// Forward declaration
static cl_int _satcurve_guided_filter_cl(const dt_iop_satcurve_global_data_t *gd,
                                         const int devid,
                                         cl_mem mask_in, cl_mem mask_out,
                                         const int width, const int height,
                                         const int radius, const float feathering,
                                         const int iterations);

static cl_int _cl_fgf_box_mean(const int devid, const int width, const int height,
                               const int radius, cl_mem in, cl_mem out, cl_mem temp)
{
  cl_int err = dt_opencl_enqueue_kernel_1d_args(
      devid, darktable.opencl->guided_filter->kernel_guided_filter_box_mean_x, height,
      CLARG(width), CLARG(height), CLARG(in), CLARG(temp), CLARG(radius));
  if (err != CL_SUCCESS)
    return err;

  return dt_opencl_enqueue_kernel_1d_args(
      devid, darktable.opencl->guided_filter->kernel_guided_filter_box_mean_y, width,
      CLARG(width), CLARG(height), CLARG(temp), CLARG(out), CLARG(radius));
}

static cl_int _satcurve_guided_filter_cl(const dt_iop_satcurve_global_data_t *gd,
                                         const int devid,
                                         cl_mem mask_in, cl_mem mask_out,
                                         const int width, const int height,
                                         const int radius, const float feathering,
                                         const int iterations)
{
  const float scaling = 4.0f;
  const int ds_radius = (radius < 4) ? 1 : (int)(radius / scaling);
  const int ds_width = (int)((float)width / scaling);
  const int ds_height = (int)((float)height / scaling);

  if (ds_width < 1 || ds_height < 1)
    return dt_opencl_enqueue_copy_image(devid, mask_in, mask_out,
                                        (size_t[]){0, 0, 0}, (size_t[]){0, 0, 0}, (size_t[]){width, height, 1});

  cl_mem ds_image = dt_opencl_alloc_device(devid, ds_width, ds_height, sizeof(float));
  cl_mem ds_blend_tmp = dt_opencl_alloc_device(devid, ds_width, ds_height, sizeof(float));
  cl_mem ds_g = dt_opencl_alloc_device(devid, ds_width, ds_height, sizeof(float));
  cl_mem ds_gg = dt_opencl_alloc_device(devid, ds_width, ds_height, sizeof(float));
  cl_mem ds_gp = dt_opencl_alloc_device(devid, ds_width, ds_height, sizeof(float));
  cl_mem mean_g = dt_opencl_alloc_device(devid, ds_width, ds_height, sizeof(float));
  cl_mem mean_p = dt_opencl_alloc_device(devid, ds_width, ds_height, sizeof(float));
  cl_mem mean_gg = dt_opencl_alloc_device(devid, ds_width, ds_height, sizeof(float));
  cl_mem mean_gp = dt_opencl_alloc_device(devid, ds_width, ds_height, sizeof(float));
  cl_mem a = dt_opencl_alloc_device(devid, ds_width, ds_height, sizeof(float));
  cl_mem b = dt_opencl_alloc_device(devid, ds_width, ds_height, sizeof(float));
  cl_mem temp_ds = dt_opencl_alloc_device(devid, ds_width, ds_height, sizeof(float));
  cl_mem a_full = dt_opencl_alloc_device(devid, width, height, sizeof(float));
  cl_mem b_full = dt_opencl_alloc_device(devid, width, height, sizeof(float));

  cl_int err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
  if (!ds_image || !ds_blend_tmp || !ds_g || !ds_gg || !ds_gp || !mean_g || !mean_p || !mean_gg || !mean_gp || !a || !b || !temp_ds || !a_full || !b_full)
    goto cleanup;

  err = dt_opencl_enqueue_kernel_2d_args(
      devid, gd->kernel_fgf_resample, ds_width, ds_height,
      CLARG(mask_in), CLARG(width), CLARG(height),
      CLARG(ds_image), CLARG(ds_width), CLARG(ds_height));
  if (err != CL_SUCCESS)
    goto cleanup;

  for (int i = 0; i < iterations; i++)
  {
    const float qmin = exp2f(-14.0f);
    const float qmax = 4.0f;
    const float quantization = 0.0f;

    err = dt_opencl_enqueue_kernel_2d_args(
        devid, gd->kernel_fgf_quantize, ds_width, ds_height,
        CLARG(ds_image), CLARG(ds_g), CLARG(ds_width), CLARG(ds_height),
        CLARG(quantization), CLARG(qmin), CLARG(qmax));
    if (err != CL_SUCCESS)
      goto cleanup;

    err = dt_opencl_enqueue_kernel_2d_args(
        devid, gd->kernel_fgf_covar_products, ds_width, ds_height,
        CLARG(ds_g), CLARG(ds_image), CLARG(ds_gg), CLARG(ds_gp),
        CLARG(ds_width), CLARG(ds_height));
    if (err != CL_SUCCESS)
      goto cleanup;

    if ((err = _cl_fgf_box_mean(devid, ds_width, ds_height, ds_radius, ds_g, mean_g, temp_ds)) != CL_SUCCESS)
      goto cleanup;
    if ((err = _cl_fgf_box_mean(devid, ds_width, ds_height, ds_radius, ds_image, mean_p, temp_ds)) != CL_SUCCESS)
      goto cleanup;
    if ((err = _cl_fgf_box_mean(devid, ds_width, ds_height, ds_radius, ds_gg, mean_gg, temp_ds)) != CL_SUCCESS)
      goto cleanup;
    if ((err = _cl_fgf_box_mean(devid, ds_width, ds_height, ds_radius, ds_gp, mean_gp, temp_ds)) != CL_SUCCESS)
      goto cleanup;

    err = dt_opencl_enqueue_kernel_2d_args(
        devid, gd->kernel_fgf_solve_ab, ds_width, ds_height,
        CLARG(mean_g), CLARG(mean_p), CLARG(mean_gg), CLARG(mean_gp),
        CLARG(a), CLARG(b), CLARG(ds_width), CLARG(ds_height), CLARG(feathering));
    if (err != CL_SUCCESS)
      goto cleanup;

    if ((err = _cl_fgf_box_mean(devid, ds_width, ds_height, ds_radius, a, a, temp_ds)) != CL_SUCCESS)
      goto cleanup;
    if ((err = _cl_fgf_box_mean(devid, ds_width, ds_height, ds_radius, b, b, temp_ds)) != CL_SUCCESS)
      goto cleanup;

    if (i != iterations - 1)
    {
      err = dt_opencl_enqueue_kernel_2d_args(
          devid, gd->kernel_fgf_apply_blend, ds_width, ds_height,
          CLARG(ds_image), CLARG(ds_blend_tmp), CLARG(a), CLARG(b),
          CLARG(ds_width), CLARG(ds_height));
      if (err != CL_SUCCESS)
        goto cleanup;

      const cl_mem swap = ds_image;
      ds_image = ds_blend_tmp;
      ds_blend_tmp = swap;
    }
  }

  err = dt_opencl_enqueue_kernel_2d_args(
      devid, gd->kernel_fgf_resample, width, height,
      CLARG(a), CLARG(ds_width), CLARG(ds_height),
      CLARG(a_full), CLARG(width), CLARG(height));
  if (err != CL_SUCCESS)
    goto cleanup;

  err = dt_opencl_enqueue_kernel_2d_args(
      devid, gd->kernel_fgf_resample, width, height,
      CLARG(b), CLARG(ds_width), CLARG(ds_height),
      CLARG(b_full), CLARG(width), CLARG(height));
  if (err != CL_SUCCESS)
    goto cleanup;

  err = dt_opencl_enqueue_kernel_2d_args(
      devid, gd->kernel_fgf_apply_blend, width, height,
      CLARG(mask_in), CLARG(mask_out), CLARG(a_full), CLARG(b_full),
      CLARG(width), CLARG(height));

cleanup:
  dt_opencl_release_mem_object(ds_image);
  dt_opencl_release_mem_object(ds_blend_tmp);
  dt_opencl_release_mem_object(ds_g);
  dt_opencl_release_mem_object(ds_gg);
  dt_opencl_release_mem_object(ds_gp);
  dt_opencl_release_mem_object(mean_g);
  dt_opencl_release_mem_object(mean_p);
  dt_opencl_release_mem_object(mean_gg);
  dt_opencl_release_mem_object(mean_gp);
  dt_opencl_release_mem_object(a);
  dt_opencl_release_mem_object(b);
  dt_opencl_release_mem_object(temp_ds);
  dt_opencl_release_mem_object(a_full);
  dt_opencl_release_mem_object(b_full);
  return err;
}

void tiling_callback(dt_iop_module_t *self,
                     dt_dev_pixelpipe_iop_t *piece,
                     const dt_iop_roi_t *roi_in,
                     const dt_iop_roi_t *roi_out,
                     struct dt_develop_tiling_t *tiling)
{
  dt_iop_satcurve_data_t *d = piece->data;

  const float ioratio = (float)roi_out->width * roi_out->height / ((float)roi_in->width * roi_in->height);
  const gboolean gf_active = d->use_guided_filter && d->gf_radius >= 0.5f && d->gf_iterations > 0;

  tiling->factor = 1.0f + ioratio;
  tiling->factor_cl = tiling->factor;
  tiling->maxbuf = 1.0f;
  tiling->maxbuf_cl = 1.0f;
  tiling->overhead = 0;
  tiling->overlap = 0;
  tiling->align = 1;

  if (!gf_active)
    return;

  // OpenCL allocations for guided filter:
  // mask_scalar_cl  : full-res float -> 0.25
  // mask_filtered_cl: full-res float -> 0.25
  // ds_image etc.   : downsampled float (12x) -> 0.75
  // a_full, b_full  : full-res float -> 0.50
  // Total overhead factor: 1.75
  tiling->factor_cl += 1.75f;

  const int base_overlap = (int)ceilf(d->gf_radius);
  const int scaled_overlap = base_overlap * d->gf_iterations;
  const int resample_margin = 2;

  tiling->overlap = MAX(tiling->overlap, scaled_overlap + resample_margin);
}

int process_cl(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece,
               cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_satcurve_data_t *d = piece->data;
  dt_iop_satcurve_gui_data_t *g = self->gui_data;
  const dt_iop_satcurve_global_data_t *gd = self->global_data;

  cl_int err = DT_OPENCL_DEFAULT_ERROR;
  if (piece->colors != 4)
    return err;

  const dt_iop_order_iccprofile_info_t *const work_profile =
      dt_ioppr_get_pipe_current_profile_info(self, piece->pipe);
  if (work_profile == NULL)
    return err;

  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;

  // not const anymore: may be downgraded to FALSE below if the guided-filter
  // scratch buffers fail to allocate, mirroring the CPU fallback in process()
  gboolean gf_active = d->use_guided_filter && d->gf_radius >= 0.5f && d->gf_iterations > 0;

  const gboolean want_mask =
      self->dev->gui_attached && dt_pipe_is_full(piece->pipe) && g && g->mask_display;

  cl_mem input_matrix_cl = NULL;
  cl_mem output_matrix_cl = NULL;
  cl_mem sat_lut_cl = NULL;
  cl_mem bri_lut_cl = NULL;
  cl_mem gamut_lut_cl = NULL;
  cl_mem hist_bins_cl = NULL;
  cl_mem mask_scalar_cl = NULL;
  cl_mem mask_filtered_cl = NULL;

  dt_colormatrix_t input_matrix = {{0.0f}};
  dt_colormatrix_t output_matrix = {{0.0f}};
  dt_colormatrix_mul(input_matrix, XYZ_D50_to_D65_CAT16, work_profile->matrix_in);
  dt_colormatrix_mul(output_matrix, work_profile->matrix_out, XYZ_D65_to_D50_CAT16);

  input_matrix_cl = dt_opencl_copy_host_to_device_constant(devid, 12 * sizeof(float), input_matrix);
  gamut_lut_cl = dt_opencl_copy_host_to_device_constant(devid, LUT_ELEM * sizeof(float), d->gamut_lut);

  if (input_matrix_cl == NULL || gamut_lut_cl == NULL)
  {
    err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
    goto error;
  }

  const float L_white = Y_to_dt_UCS_L_star(1.f);

  // Histogram update: runs independently of want_mask/neutral, mirroring
  // _update_sat_histogram() on the CPU path, which also runs before every
  // early return.
  const gboolean want_histogram =
      self->dev->gui_attached && dt_pipe_is_full(piece->pipe) && dt_iop_has_focus(self) && piece->pipe == self->dev->full.pipe;

  if (want_histogram)
  {
    int hist_bins_host[DT_IOP_SATCURVE_HIST_RES] = {0};
    const size_t hist_bins_size = sizeof(int) * DT_IOP_SATCURVE_HIST_RES;

    hist_bins_cl = dt_opencl_alloc_device_buffer(devid, hist_bins_size);
    if (hist_bins_cl && dt_opencl_write_buffer_to_device(devid, hist_bins_host, hist_bins_cl, 0,
                                                         hist_bins_size, TRUE) == CL_SUCCESS)
    {
      const cl_int hist_err = dt_opencl_enqueue_kernel_2d_args(
          devid, gd->kernel_satcurve_histogram, width, height,
          CLARG(dev_in), CLARG(width), CLARG(height),
          CLARG(input_matrix_cl), CLARG(gamut_lut_cl),
          CLARG(d->formula), CLARG(L_white),
          CLARG(hist_bins_cl));

      if (hist_err == CL_SUCCESS && dt_opencl_read_buffer_from_device(devid, hist_bins_host, hist_bins_cl, 0,
                                                                      hist_bins_size, TRUE) == CL_SUCCESS)
        _commit_sat_histogram(self, hist_bins_host);
    }

    dt_opencl_release_mem_object(hist_bins_cl);
    hist_bins_cl = NULL;
  }

  // Mask preview mode
  if (want_mask)
  {
    if (gf_active)
    {
      mask_scalar_cl = dt_opencl_alloc_device(devid, width, height, sizeof(float));
      mask_filtered_cl = dt_opencl_alloc_device(devid, width, height, sizeof(float));
      if (mask_scalar_cl == NULL || mask_filtered_cl == NULL)
      {
        // fallback: show the unfiltered mask instead of aborting the preview
        dt_control_log(_("saturation curve: guided filter failed to allocate memory, "
                         "disabling it for this preview"));
        dt_opencl_release_mem_object(mask_scalar_cl);
        dt_opencl_release_mem_object(mask_filtered_cl);
        mask_scalar_cl = NULL;
        mask_filtered_cl = NULL;
        gf_active = FALSE;
      }
    }

    if (!gf_active)
    {
      err = dt_opencl_enqueue_kernel_2d_args(
          devid, gd->kernel_satcurve_mask, width, height,
          CLARG(dev_in), CLARG(dev_out),
          CLARG(width), CLARG(height),
          CLARG(input_matrix_cl), CLARG(gamut_lut_cl),
          CLARG(d->formula), CLARG(L_white));

      if (err == CL_SUCCESS)
        piece->pipe->mask_display = DT_DEV_PIXELPIPE_DISPLAY_PASSTHRU;
      goto error;
    }

    err = dt_opencl_enqueue_kernel_2d_args(
        devid, gd->kernel_satcurve_scalar_mask, width, height,
        CLARG(dev_in), CLARG(mask_scalar_cl),
        CLARG(width), CLARG(height),
        CLARG(input_matrix_cl), CLARG(gamut_lut_cl),
        CLARG(d->formula), CLARG(L_white));
    if (err != CL_SUCCESS)
      goto error;

    err = _satcurve_guided_filter_cl(gd, devid, mask_scalar_cl, mask_filtered_cl,
                                     width, height, (int)d->gf_radius, d->gf_feathering,
                                     d->gf_iterations);
    if (err != CL_SUCCESS)
      goto error;

    err = dt_opencl_enqueue_kernel_2d_args(
        devid, gd->kernel_satcurve_mask_from_scalar, width, height,
        CLARG(mask_filtered_cl), CLARG(dev_in), CLARG(dev_out),
        CLARG(width), CLARG(height));

    if (err == CL_SUCCESS)
      piece->pipe->mask_display = DT_DEV_PIXELPIPE_DISPLAY_PASSTHRU;
    goto error;
  }

  const gboolean neutral_sat = channel_is_neutral(&d->channel[DT_IOP_SATCURVE_CHANNEL_SATURATION]);
  const gboolean neutral_bri = channel_is_neutral(&d->channel[DT_IOP_SATCURVE_CHANNEL_BRILLIANCE]);
  if (neutral_sat && neutral_bri)
  {
    err = dt_opencl_enqueue_copy_image(piece->pipe->devid, dev_in, dev_out,
                                       (size_t[]){0, 0, 0},
                                       (size_t[]){0, 0, 0},
                                       (size_t[]){roi_in->width, roi_in->height, 1});
    goto error;
  }

  output_matrix_cl = dt_opencl_copy_host_to_device_constant(devid, 12 * sizeof(float), output_matrix);
  sat_lut_cl = dt_opencl_copy_host_to_device_constant(
      devid, DT_IOP_SATCURVE_RES * sizeof(float),
      d->channel[DT_IOP_SATCURVE_CHANNEL_SATURATION].lut);
  bri_lut_cl = dt_opencl_copy_host_to_device_constant(
      devid, DT_IOP_SATCURVE_RES * sizeof(float),
      d->channel[DT_IOP_SATCURVE_CHANNEL_BRILLIANCE].lut);

  if (output_matrix_cl == NULL || sat_lut_cl == NULL || bri_lut_cl == NULL)
  {
    err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
    goto error;
  }

  // 1. Prepare filtered saturation mask if guided filter is enabled
  if (gf_active)
  {
    mask_scalar_cl = dt_opencl_alloc_device(devid, width, height, sizeof(float));
    mask_filtered_cl = dt_opencl_alloc_device(devid, width, height, sizeof(float));
    if (mask_scalar_cl == NULL || mask_filtered_cl == NULL)
    {
      // fallback: proceed without guided filter instead of aborting the
      // whole correction, mirroring process() on the CPU
      dt_control_log(_("saturation curve: guided filter failed to allocate memory, "
                       "disabling it for this run"));
      dt_opencl_release_mem_object(mask_scalar_cl);
      dt_opencl_release_mem_object(mask_filtered_cl);
      mask_scalar_cl = NULL;
      mask_filtered_cl = NULL;
      gf_active = FALSE;
    }
  }

  if (gf_active)
  {
    err = dt_opencl_enqueue_kernel_2d_args(
        devid, gd->kernel_satcurve_scalar_mask, width, height,
        CLARG(dev_in), CLARG(mask_scalar_cl),
        CLARG(width), CLARG(height),
        CLARG(input_matrix_cl), CLARG(gamut_lut_cl),
        CLARG(d->formula), CLARG(L_white));
    if (err != CL_SUCCESS)
      goto error;

    err = _satcurve_guided_filter_cl(gd, devid, mask_scalar_cl, mask_filtered_cl,
                                     width, height, (int)d->gf_radius, d->gf_feathering,
                                     d->gf_iterations);
    if (err != CL_SUCCESS)
      goto error;
  }

  const int use_gf = gf_active ? 1 : 0;
  cl_mem guided_mask_arg = gf_active ? mask_filtered_cl : dev_in;

  // 2. Main processing pass: evaluate curves using the smoothed mask
  err = dt_opencl_enqueue_kernel_2d_args(
      devid, gd->kernel_satcurvergb, width, height,
      CLARG(dev_in), CLARG(dev_out),
      CLARG(guided_mask_arg), CLARG(use_gf),
      CLARG(width), CLARG(height),
      CLARG(input_matrix_cl), CLARG(output_matrix_cl),
      CLARG(sat_lut_cl), CLARG(bri_lut_cl), CLARG(gamut_lut_cl),
      CLARG(d->formula), CLARG(L_white));

  if (err != CL_SUCCESS)
    goto error;

error:
  dt_opencl_release_mem_object(input_matrix_cl);
  dt_opencl_release_mem_object(output_matrix_cl);
  dt_opencl_release_mem_object(sat_lut_cl);
  dt_opencl_release_mem_object(bri_lut_cl);
  dt_opencl_release_mem_object(gamut_lut_cl);
  dt_opencl_release_mem_object(hist_bins_cl);
  dt_opencl_release_mem_object(mask_scalar_cl);
  dt_opencl_release_mem_object(mask_filtered_cl);
  return err;
}

void init_global(dt_iop_module_so_t *self)
{
  const int program = 42;     // satcurve.cl in programs.conf
  const int program_fgf = 43; // fast_guided_filter.cl in programs.conf

  dt_iop_satcurve_global_data_t *gd = malloc(sizeof(dt_iop_satcurve_global_data_t));
  self->data = gd;
  gd->kernel_satcurvergb = dt_opencl_create_kernel(program, "satcurvergb");
  gd->kernel_satcurve_histogram = dt_opencl_create_kernel(program, "satcurve_histogram");
  gd->kernel_satcurve_mask = dt_opencl_create_kernel(program, "satcurve_mask");
  gd->kernel_satcurve_scalar_mask = dt_opencl_create_kernel(program, "satcurve_scalar_mask");
  gd->kernel_satcurve_mask_from_scalar = dt_opencl_create_kernel(program, "satcurve_mask_from_scalar");
  gd->kernel_fgf_resample = dt_opencl_create_kernel(program_fgf, "fastguided_resample");
  gd->kernel_fgf_quantize = dt_opencl_create_kernel(program_fgf, "fastguided_quantize");
  gd->kernel_fgf_covar_products = dt_opencl_create_kernel(program_fgf, "fastguided_covar_products");
  gd->kernel_fgf_solve_ab = dt_opencl_create_kernel(program_fgf, "fastguided_solve_ab");
  gd->kernel_fgf_apply_blend = dt_opencl_create_kernel(program_fgf, "fastguided_apply_blend");
}

void cleanup_global(dt_iop_module_so_t *self)
{
  const dt_iop_satcurve_global_data_t *gd = self->data;
  dt_opencl_free_kernel(gd->kernel_satcurvergb);
  dt_opencl_free_kernel(gd->kernel_satcurve_histogram);
  dt_opencl_free_kernel(gd->kernel_satcurve_mask);
  dt_opencl_free_kernel(gd->kernel_satcurve_scalar_mask);
  dt_opencl_free_kernel(gd->kernel_satcurve_mask_from_scalar);
  dt_opencl_free_kernel(gd->kernel_fgf_resample);
  dt_opencl_free_kernel(gd->kernel_fgf_quantize);
  dt_opencl_free_kernel(gd->kernel_fgf_covar_products);
  dt_opencl_free_kernel(gd->kernel_fgf_solve_ab);
  dt_opencl_free_kernel(gd->kernel_fgf_apply_blend);
  free(self->data);
  self->data = NULL;
}
#endif

void init_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_satcurve_data_t *d = dt_calloc_align_type(dt_iop_satcurve_data_t, 1);

  for (int ch = 0; ch < DT_IOP_SATCURVE_CHANNELS; ch++)
  {
    d->channel[ch].lut = dt_alloc_align_float(DT_IOP_SATCURVE_RES);
    d->channel[ch].curve = dt_draw_curve_new(0.f, 1.f, MONOTONE_HERMITE);
    d->channel[ch].curve_type = MONOTONE_HERMITE;
    d->channel[ch].curve_num_nodes = 0;
  }

  d->gamut_lut = dt_alloc_align_float(LUT_ELEM);
  d->lut_inited = FALSE;
  d->work_profile = NULL;
  piece->data = d;
}

void cleanup_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_satcurve_data_t *d = piece->data;
  if (!d)
    return;

  for (int ch = 0; ch < DT_IOP_SATCURVE_CHANNELS; ch++)
  {
    if (d->channel[ch].curve)
      dt_draw_curve_destroy(d->channel[ch].curve);
    dt_free_align(d->channel[ch].lut);
  }

  dt_free_align(d->gamut_lut);
  dt_free_align(d);
  piece->data = NULL;
}

static void build_jzazbz_gamut_lut(dt_iop_satcurve_data_t *d,
                                   const dt_iop_order_iccprofile_info_t *work_profile)
{
  dt_colormatrix_t inputmatrix = {{0.0f}};
  dt_colormatrix_mul(inputmatrix, XYZ_D50_to_D65_CAT16, work_profile->matrix_in);

  dt_colormatrix_t inputmatrix_trans;
  dt_colormatrix_transpose(inputmatrix_trans, inputmatrix);

  float *sampler = dt_calloc_align_float(LUT_ELEM);

  DT_OMP_FOR(reduction(max : sampler[:LUT_ELEM]) collapse(3))
  for (int r = 0; r < DT_IOP_SATCURVE_GAMUT_STEPS; r++)
    for (int g = 0; g < DT_IOP_SATCURVE_GAMUT_STEPS; g++)
      for (int b = 0; b < DT_IOP_SATCURVE_GAMUT_STEPS; b++)
      {
        const dt_aligned_pixel_t rgb = {
            r / (float)(DT_IOP_SATCURVE_GAMUT_STEPS - 1),
            g / (float)(DT_IOP_SATCURVE_GAMUT_STEPS - 1),
            b / (float)(DT_IOP_SATCURVE_GAMUT_STEPS - 1),
            0.f};

        dt_aligned_pixel_t xyz, jab;
        dt_apply_transposed_color_matrix(rgb, inputmatrix_trans, xyz);
        dt_XYZ_2_JzAzBz(xyz, jab);

        const float sat = jab[0] > 1e-6f ? dt_fast_hypotf(jab[1], jab[2]) / jab[0] : 0.f;
        int index = roundf((LUT_ELEM - 1) * (atan2f(jab[2], jab[1]) + M_PI_F) / DT_2PI_F);
        index = index < 0 ? LUT_ELEM - 1 : (index >= LUT_ELEM ? 0 : index);
        sampler[index] = MAX(sampler[index], sat);
      }

  for (size_t i = 2; i < LUT_ELEM - 2; i++)
    d->gamut_lut[i] = (sampler[i - 2] + sampler[i - 1] + sampler[i] + sampler[i + 1] + sampler[i + 2]) / 5.f;
  d->gamut_lut[0] = (sampler[LUT_ELEM - 2] + sampler[LUT_ELEM - 1] + sampler[0] + sampler[1] + sampler[2]) / 5.f;
  d->gamut_lut[1] = (sampler[LUT_ELEM - 1] + sampler[0] + sampler[1] + sampler[2] + sampler[3]) / 5.f;
  d->gamut_lut[LUT_ELEM - 2] = (sampler[LUT_ELEM - 4] + sampler[LUT_ELEM - 3] + sampler[LUT_ELEM - 2] + sampler[LUT_ELEM - 1] + sampler[0]) / 5.f;
  d->gamut_lut[LUT_ELEM - 1] = (sampler[LUT_ELEM - 3] + sampler[LUT_ELEM - 2] + sampler[LUT_ELEM - 1] + sampler[0] + sampler[1]) / 5.f;

  dt_free_align(sampler);
}

static void build_gamut_lut(dt_iop_satcurve_data_t *d,
                            const dt_iop_order_iccprofile_info_t *work_profile)
{
  if (d->formula == DT_IOP_SATCURVE_DTUCS)
  {
    dt_colormatrix_t inputmatrix = {{0.0f}};
    dt_colormatrix_mul(inputmatrix, XYZ_D50_to_D65_CAT16, work_profile->matrix_in);
    dt_UCS_22_build_gamut_LUT(inputmatrix, d->gamut_lut);
  }
  else
  {
    build_jzazbz_gamut_lut(d, work_profile);
  }
}

static void sync_channel_curve(dt_iop_satcurve_channel_data_t *dst,
                               const dt_iop_satcurve_channel_params_t *src)
{
  if (dst->curve_type != src->curve_type || dst->curve_num_nodes != src->curve_num_nodes)
  {
    if (dst->curve)
      dt_draw_curve_destroy(dst->curve);
    dst->curve = dt_draw_curve_new(0.f, 1.f, src->curve_type);
    dst->curve_type = src->curve_type;
    dst->curve_num_nodes = src->curve_num_nodes;

    for (int i = 0; i < src->curve_num_nodes; i++)
      dt_draw_curve_add_point(dst->curve, src->curve[i].x, src->curve[i].y);
  }
  else
  {
    for (int i = 0; i < src->curve_num_nodes; i++)
      dt_draw_curve_set_point(dst->curve, i, src->curve[i].x, src->curve[i].y);
  }

  dt_draw_curve_calc_values(dst->curve, 0.f, 1.f, DT_IOP_SATCURVE_RES, NULL, dst->lut);
}

void commit_params(dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_satcurve_data_t *d = piece->data;
  const dt_iop_satcurve_params_t *p = (const dt_iop_satcurve_params_t *)p1;

  if (d->formula != p->formula)
  {
    d->formula = p->formula;
    d->lut_inited = FALSE;
  }

  for (int ch = 0; ch < DT_IOP_SATCURVE_CHANNELS; ch++)
    sync_channel_curve(&d->channel[ch], &p->channel[ch]);

  d->use_guided_filter = p->use_guided_filter;
  d->gf_radius = p->gf_radius;
  d->gf_feathering = p->gf_feathering;
  d->gf_iterations = p->gf_iterations;

  const dt_iop_order_iccprofile_info_t *work_profile =
      dt_ioppr_get_pipe_current_profile_info(self, piece->pipe);

  if (work_profile && (!d->lut_inited || work_profile != d->work_profile))
  {
    build_gamut_lut(d, work_profile);
    d->work_profile = work_profile;
    d->lut_inited = TRUE;
  }
}

void init_presets(dt_iop_module_so_t *self)
{
  dt_iop_satcurve_params_t p = {0};

  reset_params(&p);
  dt_gui_presets_add_generic(_("neutral"), self->op, self->version(),
                             &p, sizeof(p), TRUE, DEVELOP_BLEND_CS_RGB_SCENE);

  reset_params(&p);
  p.channel[DT_IOP_SATCURVE_CHANNEL_SATURATION].curve[0].y = .60f;
  p.channel[DT_IOP_SATCURVE_CHANNEL_SATURATION].curve[1].y = .55f;
  dt_gui_presets_add_generic(_("gentle saturation"), self->op, self->version(),
                             &p, sizeof(p), TRUE, DEVELOP_BLEND_CS_RGB_SCENE);

  reset_params(&p);
  p.channel[DT_IOP_SATCURVE_CHANNEL_SATURATION].curve_num_nodes = 3;
  p.channel[DT_IOP_SATCURVE_CHANNEL_SATURATION].curve[1] = (dt_iop_satcurve_node_t){.45f, .62f};
  p.channel[DT_IOP_SATCURVE_CHANNEL_SATURATION].curve[2] = (dt_iop_satcurve_node_t){1.f, .42f};
  dt_gui_presets_add_generic(_("protect saturated colors"), self->op, self->version(),
                             &p, sizeof(p), TRUE, DEVELOP_BLEND_CS_RGB_SCENE);

  reset_params(&p);
  p.channel[DT_IOP_SATCURVE_CHANNEL_BRILLIANCE].curve[0].y = .56f;
  p.channel[DT_IOP_SATCURVE_CHANNEL_BRILLIANCE].curve[1].y = .54f;
  dt_gui_presets_add_generic(_("gentle brilliance"), self->op, self->version(),
                             &p, sizeof(p), TRUE, DEVELOP_BLEND_CS_RGB_SCENE);

  reset_params(&p);
  p.channel[DT_IOP_SATCURVE_CHANNEL_BRILLIANCE].curve_num_nodes = 3;
  p.channel[DT_IOP_SATCURVE_CHANNEL_BRILLIANCE].curve[1] = (dt_iop_satcurve_node_t){.40f, .58f};
  p.channel[DT_IOP_SATCURVE_CHANNEL_BRILLIANCE].curve[2] = (dt_iop_satcurve_node_t){1.f, .48f};
  dt_gui_presets_add_generic(_("bright mids, protect extremes"), self->op, self->version(),
                             &p, sizeof(p), TRUE, DEVELOP_BLEND_CS_RGB_SCENE);
}

static inline int add_node_to_channel(dt_iop_satcurve_channel_params_t *c, const float x, const float y)
{
  if (c->curve_num_nodes >= DT_IOP_SATCURVE_MAXNODES)
    return -1;

  int at = 0;
  while (at < c->curve_num_nodes && c->curve[at].x < x)
    at++;

  if ((at && x - c->curve[at - 1].x < DT_IOP_SATCURVE_MIN_X_DISTANCE) || (at < c->curve_num_nodes && c->curve[at].x - x < DT_IOP_SATCURVE_MIN_X_DISTANCE))
    return -1;

  for (int i = c->curve_num_nodes; i > at; i--)
    c->curve[i] = c->curve[i - 1];
  c->curve[at] = (dt_iop_satcurve_node_t){x, y};
  c->curve_num_nodes++;
  return at;
}

static void _get_graph_geometry(const GtkAllocation *a, float *x0, float *y0, float *w, float *h)
{
  *x0 = DT_IOP_SATCURVE_INSET;
  *y0 = DT_IOP_SATCURVE_INSET;
  *w = MAX(1, a->width - 2 * DT_IOP_SATCURVE_INSET);
  *h = MAX(1, a->height - 2 * DT_IOP_SATCURVE_INSET - DT_IOP_SATCURVE_GRADIENT_GAP - DT_IOP_SATCURVE_GRADIENT_SIZE);
}

static void _draw_sat_histogram(cairo_t *cr, dt_iop_satcurve_gui_data_t *g, const int w, const int h)
{
  dt_pthread_mutex_lock(&g->histogram_lock);

  const float hist_max = g->histogram_max;
  cairo_save(cr);
  cairo_set_source_rgba(cr, .8, .8, .8, .30);
  cairo_move_to(cr, 0, h);

  for (int i = 0; i < DT_IOP_SATCURVE_HIST_RES; i++)
  {
    const float x = w * i / (float)(DT_IOP_SATCURVE_HIST_RES - 1);
    const float y = h * (1.f - g->histogram[i] / hist_max);
    cairo_line_to(cr, x, y);
  }

  cairo_line_to(cr, w, h);
  cairo_close_path(cr);
  cairo_fill(cr);
  cairo_restore(cr);

  dt_pthread_mutex_unlock(&g->histogram_lock);
}

static void _draw_sat_picker(cairo_t *cr, dt_iop_module_t *self, const int w, const int h)
{
  dt_iop_satcurve_gui_data_t *g = self->gui_data;
  if (!g->picker_valid)
    return;
  if (self->request_color_pick != DT_REQUEST_COLORPICK_MODULE)
    return;

  const float x_mean = CLAMP(g->picked_s, 0.f, 1.f) * w;
  const float x_min = CLAMP(g->picked_s_min, 0.f, 1.f) * w;
  const float x_max = CLAMP(g->picked_s_max, 0.f, 1.f) * w;

  cairo_save(cr);

  cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.20);
  cairo_rectangle(cr, x_min, 0, MAX(1.f, x_max - x_min), h);
  cairo_fill(cr);

  cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.2));
  cairo_move_to(cr, x_mean, 0);
  cairo_line_to(cr, x_mean, h);
  cairo_stroke(cr);

  cairo_restore(cr);
}

static void _draw_channel_curve(cairo_t *cr,
                                const dt_iop_satcurve_channel_params_t *cp,
                                const dt_iop_satcurve_gui_channel_t *gc,
                                const int selected,
                                const gboolean active,
                                const int w, const int h,
                                const double r, const double g, const double b)
{
  cairo_save(cr);

  cairo_set_source_rgba(cr, r, g, b, active ? .95 : .50);
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(active ? 2.0 : 1.2));
  cairo_move_to(cr, 0, h * (1.f - gc->draw_ys[0]));
  for (int i = 1; i < DT_IOP_SATCURVE_RES; i++)
  {
    const float x = w * i / (float)(DT_IOP_SATCURVE_RES - 1);
    const float y = h * (1.f - gc->draw_ys[i]);
    cairo_line_to(cr, x, y);
  }
  cairo_stroke(cr);

  if (active)
  {
    cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.0));
    for (int i = 0; i < cp->curve_num_nodes; i++)
    {
      const float px = w * cp->curve[i].x;
      const float py = h * (1.f - cp->curve[i].y);
      cairo_arc(cr, px, py, DT_PIXEL_APPLY_DPI(i == selected ? 5 : 3), 0, DT_2PI_F);
      cairo_stroke(cr);
    }
  }

  cairo_restore(cr);
}

static void _sync_gui_curve(dt_iop_satcurve_gui_channel_t *gc,
                            const dt_iop_satcurve_channel_params_t *cp)
{
  if (gc->curve_type != cp->curve_type || gc->curve_num_nodes != cp->curve_num_nodes)
  {
    if (gc->curve)
      dt_draw_curve_destroy(gc->curve);
    gc->curve = dt_draw_curve_new(0.f, 1.f, cp->curve_type);
    gc->curve_type = cp->curve_type;
    gc->curve_num_nodes = cp->curve_num_nodes;

    for (int i = 0; i < cp->curve_num_nodes; i++)
      dt_draw_curve_add_point(gc->curve, cp->curve[i].x, cp->curve[i].y);
  }
  else
  {
    for (int i = 0; i < cp->curve_num_nodes; i++)
      dt_draw_curve_set_point(gc->curve, i, cp->curve[i].x, cp->curve[i].y);
  }

  dt_draw_curve_calc_values(gc->curve, 0.f, 1.f, DT_IOP_SATCURVE_RES, NULL, gc->draw_ys);
}

static gboolean area_draw(GtkWidget *widget, cairo_t *cr, dt_iop_module_t *self)
{
  dt_iop_satcurve_gui_data_t *g = self->gui_data;
  dt_iop_satcurve_params_t *p = self->params;

  GtkAllocation a;
  gtk_widget_get_allocation(widget, &a);

  float gx0, gy0, gw, gh;
  _get_graph_geometry(&a, &gx0, &gy0, &gw, &gh);

  cairo_set_source_rgb(cr, .15, .15, .15);
  cairo_paint(cr);

  cairo_save(cr);
  cairo_translate(cr, gx0, gy0);

  for (int ch = 0; ch < DT_IOP_SATCURVE_CHANNELS; ch++)
    _sync_gui_curve(&g->channel[ch], &p->channel[ch]);

  _draw_sat_histogram(cr, g, (int)gw, (int)gh);
  _draw_sat_picker(cr, self, (int)gw, (int)gh);

  cairo_set_source_rgba(cr, 1, 1, 1, .08);
  cairo_set_line_width(cr, 1.0);
  for (int i = 0; i <= 4; i++)
  {
    const float y = gh * i / 4.f;
    cairo_move_to(cr, 0, y);
    cairo_line_to(cr, gw, y);
  }
  for (int i = 0; i <= 4; i++)
  {
    const float x = gw * i / 4.f;
    cairo_move_to(cr, x, 0);
    cairo_line_to(cr, x, gh);
  }
  cairo_stroke(cr);

  _draw_channel_curve(cr,
                      &p->channel[DT_IOP_SATCURVE_CHANNEL_SATURATION],
                      &g->channel[DT_IOP_SATCURVE_CHANNEL_SATURATION],
                      g->active_channel == DT_IOP_SATCURVE_CHANNEL_SATURATION ? g->selected : -1,
                      g->active_channel == DT_IOP_SATCURVE_CHANNEL_SATURATION,
                      (int)gw, (int)gh,
                      .90, .90, .90);

  _draw_channel_curve(cr,
                      &p->channel[DT_IOP_SATCURVE_CHANNEL_BRILLIANCE],
                      &g->channel[DT_IOP_SATCURVE_CHANNEL_BRILLIANCE],
                      g->active_channel == DT_IOP_SATCURVE_CHANNEL_BRILLIANCE ? g->selected : -1,
                      g->active_channel == DT_IOP_SATCURVE_CHANNEL_BRILLIANCE,
                      (int)gw, (int)gh,
                      1.00, .65, .20);

  cairo_restore(cr);

  // Saturation axis gradient: white at zero saturation, purple/magenta
  // {0.5, 0.0, 0.5} at full saturation, matching the mask preview and the
  // chroma gradient end color in blend_gui.c.
  cairo_pattern_t *grad = cairo_pattern_create_linear(gx0, 0.0, gx0 + gw, 0.0);
  cairo_pattern_add_color_stop_rgba(grad, 0.0, 1.0, 1.0, 1.0, 1.0);
  cairo_pattern_add_color_stop_rgba(grad, 1.0, 0.5, 0.0, 0.5, 1.0);
  cairo_rectangle(cr, gx0, gy0 + gh + DT_IOP_SATCURVE_GRADIENT_GAP, gw, DT_IOP_SATCURVE_GRADIENT_SIZE);
  cairo_set_source(cr, grad);
  cairo_fill(cr);
  cairo_pattern_destroy(grad);

  return FALSE;
}

// Returns the index of the curve node nearest to (x, y) in normalized graph
// coordinates, within the shared grab radius, or -1 if none is close enough.
// Picking the nearest (rather than the first within range) avoids ambiguities
// when two nodes sit close together. This is what makes a node moveable on
// hover: the motion handler drags g->selected, the scroll handler nudges it,
// and the draw code renders it larger.
static inline int _node_hit_at(const dt_iop_satcurve_channel_params_t *cp,
                               const float x, const float y)
{
  int nearest = -1;
  float best = DT_IOP_SATCURVE_NODE_GRAB_SQ;
  for (int i = 0; i < cp->curve_num_nodes; i++)
  {
    const float dx = x - cp->curve[i].x;
    const float dy = y - cp->curve[i].y;
    const float d2 = dx * dx + dy * dy;
    if (d2 < best)
    {
      best = d2;
      nearest = i;
    }
  }
  return nearest;
}

static gboolean area_button(GtkWidget *widget, GdkEventButton *event, dt_iop_module_t *self)
{
  DT_GUARD_GUI_UPDATE(FALSE);

  dt_iop_satcurve_gui_data_t *g = self->gui_data;
  dt_iop_satcurve_channel_params_t *cp = get_active_channel_params(self);
  dt_iop_satcurve_gui_channel_t *gc = get_active_gui_channel(self);

  GtkAllocation a;
  gtk_widget_get_allocation(widget, &a);

  if (event->y >= a.height - DT_RESIZE_HANDLE_SIZE)
    return FALSE;

  float gx0, gy0, w, h;
  _get_graph_geometry(&a, &gx0, &gy0, &w, &h);

  const float x = CLAMP((event->x - gx0) / w, 0.f, 1.f);
  const float y = CLAMP(1.f - (event->y - gy0) / h, 0.f, 1.f);

  int hit = _node_hit_at(cp, x, y);

  if (event->type == GDK_2BUTTON_PRESS && event->button == GDK_BUTTON_PRIMARY)
  {
    reset_channel_curve(cp);
    g->selected = -1;
    dt_dev_add_history_item(darktable.develop, self, TRUE);
    gtk_widget_queue_draw(widget);
    return TRUE;
  }

  if (event->button == GDK_BUTTON_SECONDARY && hit >= 0)
  {
    if ((event->state & GDK_CONTROL_MASK) || cp->curve_num_nodes <= 2)
    {
      cp->curve[hit].y = .5f;
    }
    else
    {
      for (int i = hit; i < cp->curve_num_nodes - 1; i++)
        cp->curve[i] = cp->curve[i + 1];
      cp->curve_num_nodes--;
    }
    g->selected = -1;
    dt_dev_add_history_item(darktable.develop, self, TRUE);
    gtk_widget_queue_draw(widget);
    return TRUE;
  }

  if (event->button == GDK_BUTTON_PRIMARY)
  {
    if (hit < 0 && cp->curve_num_nodes < DT_IOP_SATCURVE_MAXNODES)
      hit = add_node_to_channel(cp, x, CLAMP(dt_draw_curve_calc_value(gc->curve, x), 0.f, 1.f));

    g->selected = hit;
    g->dragging = hit >= 0;
    return TRUE;
  }

  return FALSE;
}

static gboolean area_scroll(GtkWidget *widget, GdkEventScroll *event,
                            dt_iop_module_t *self)
{
  DT_GUARD_GUI_UPDATE(FALSE);

  dt_iop_satcurve_gui_data_t *g = self->gui_data;
  dt_iop_satcurve_channel_params_t *cp = get_active_channel_params(self);

  if (g->selected < 0)
    return FALSE;

  int delta_y = 0;
  if (dt_gui_get_scroll_unit_delta((const GdkEvent *)event, &delta_y))
  {
    const float step = 0.02f * (event->state & GDK_CONTROL_MASK ? 5.0f : 1.0f);
    const int n = g->selected;
    cp->curve[n].y = CLAMP(cp->curve[n].y - delta_y * step, 0.f, 1.f);

    dt_dev_add_history_item(darktable.develop, self, TRUE);
    gtk_widget_queue_draw(widget);
  }

  return TRUE;
}

static gboolean area_motion(GtkWidget *widget, GdkEventMotion *event, dt_iop_module_t *self)
{
  DT_GUARD_GUI_UPDATE(FALSE);

  dt_iop_satcurve_gui_data_t *g = self->gui_data;
  dt_iop_satcurve_channel_params_t *cp = get_active_channel_params(self);

  GtkAllocation a;
  gtk_widget_get_allocation(widget, &a);

  float gx0, gy0, w, h;
  _get_graph_geometry(&a, &gx0, &gy0, &w, &h);

  // Dragging a node that was grabbed by a button press.
  if (g->dragging && g->selected >= 0)
  {
    const float x = CLAMP((event->x - gx0) / w, 0.f, 1.f);
    const int n = g->selected;

    if (n == 0)
      cp->curve[n].x = 0.f;
    else if (n == cp->curve_num_nodes - 1)
      cp->curve[n].x = 1.f;
    else
      cp->curve[n].x = CLAMP(x,
                             cp->curve[n - 1].x + DT_IOP_SATCURVE_MIN_X_DISTANCE,
                             cp->curve[n + 1].x - DT_IOP_SATCURVE_MIN_X_DISTANCE);

    cp->curve[n].y = CLAMP(1.f - (event->y - gy0) / h, 0.f, 1.f);
    dt_dev_add_history_item(darktable.develop, self, FALSE);
    gtk_widget_queue_draw(widget);
    return TRUE;
  }

  // Otherwise, pick up the node under the pointer so it becomes moveable: a
  // subsequent press drags it, the scroll wheel nudges it, and it is drawn
  // highlighted. Hit-test only inside the graph rectangle -- clamping would
  // otherwise grab an endpoint node from the inset/gradient margin.
  const gboolean inside = event->x >= gx0 && event->x <= gx0 + w && event->y >= gy0 && event->y <= gy0 + h;

  int hit = -1;
  if (inside)
  {
    const float x = (event->x - gx0) / w;
    const float y = 1.f - (event->y - gy0) / h;
    hit = _node_hit_at(cp, x, y);
  }

  if (hit != g->selected)
  {
    g->selected = hit;
    gtk_widget_queue_draw(widget);
    if (g->selected >= 0)
      gtk_widget_grab_focus(widget);
  }

  return FALSE;
}

static gboolean area_release(GtkWidget *widget, GdkEventButton *event, dt_iop_module_t *self)
{
  DT_GUARD_GUI_UPDATE(FALSE);

  dt_iop_satcurve_gui_data_t *g = self->gui_data;
  if (g->dragging)
  {
    g->dragging = FALSE;
    dt_dev_add_history_item(darktable.develop, self, TRUE);
  }
  return TRUE;
}

static const float *_get_gamut_lut_for_picker(dt_iop_module_t *self)
{
  static float unity_gamut_lut[LUT_ELEM];
  static gboolean unity_inited = FALSE;

  if (!unity_inited)
  {
    for (int i = 0; i < LUT_ELEM; i++)
      unity_gamut_lut[i] = 1.f;
    unity_inited = TRUE;
  }

  for (GList *nodes = self->dev->full.pipe ? self->dev->full.pipe->nodes : NULL;
       nodes;
       nodes = g_list_next(nodes))
  {
    dt_dev_pixelpipe_iop_t *piece = nodes->data;
    if (piece->module == self && piece->data)
    {
      const dt_iop_satcurve_data_t *d = piece->data;
      if (d->lut_inited && d->gamut_lut)
        return d->gamut_lut;
    }
  }

  return unity_gamut_lut;
}

void color_picker_apply(dt_iop_module_t *self, GtkWidget *widget, dt_dev_pixelpipe_t *pipe)
{
  dt_iop_satcurve_gui_data_t *g = self->gui_data;
  if (!g)
    return;

  const dt_iop_order_iccprofile_info_t *work_profile =
      dt_ioppr_get_iop_work_profile_info(self, self->dev->iop);
  if (!work_profile)
    return;

  dt_colormatrix_t inputmatrix = {{0.0f}};
  dt_colormatrix_t inputmatrix_trans;
  dt_colormatrix_mul(inputmatrix, XYZ_D50_to_D65_CAT16, work_profile->matrix_in);
  dt_colormatrix_transpose(inputmatrix_trans, inputmatrix);

  const float *const gamut_lut = _get_gamut_lut_for_picker(self);
  const float L_white = Y_to_dt_UCS_L_star(1.f);

  const dt_aligned_pixel_t mean_rgb = {self->picked_color[0], self->picked_color[1], self->picked_color[2], 0.f};
  const dt_aligned_pixel_t min_rgb = {self->picked_color_min[0], self->picked_color_min[1], self->picked_color_min[2], 0.f};
  const dt_aligned_pixel_t max_rgb = {self->picked_color_max[0], self->picked_color_max[1], self->picked_color_max[2], 0.f};

  const dt_iop_satcurve_formula_t formula = ((const dt_iop_satcurve_params_t *)self->params)->formula;

  g->picked_s = pixel_s_in_norm(formula, mean_rgb, inputmatrix_trans, gamut_lut, L_white, NULL);
  g->picked_s_min = pixel_s_in_norm(formula, min_rgb, inputmatrix_trans, gamut_lut, L_white, NULL);
  g->picked_s_max = pixel_s_in_norm(formula, max_rgb, inputmatrix_trans, gamut_lut, L_white, NULL);
  g->picker_valid = TRUE;

  gtk_widget_queue_draw(GTK_WIDGET(g->area));
}

// Toggle the saturation-mask preview, following the standard pattern used by
// other mask preview controls in darktable.
static void show_saturation_mask_callback(GtkToggleButton *button, dt_iop_module_t *self)
{
  DT_GUARD_GUI_UPDATE();

  dt_iop_satcurve_gui_data_t *g = self->gui_data;
  if (!g)
    return;

  dt_iop_request_focus(self);

  if (self->request_mask_display)
  {
    dt_control_log(_("cannot display saturation masks while the blending mask is displayed"));
    gtk_toggle_button_set_active(button, FALSE);
    g->mask_display = FALSE;
    return;
  }

  g->mask_display = gtk_toggle_button_get_active(button);
  if (g->mask_display && !self->enabled)
  {
    self->enabled = TRUE;
    dt_dev_add_history_item(darktable.develop, self, TRUE);
  }

  dt_iop_refresh_center(self);
  dt_iop_color_picker_reset(self, TRUE);
}

void gui_focus(dt_iop_module_t *self, gboolean in)
{
  dt_iop_satcurve_gui_data_t *g = self->gui_data;
  if (!in)
  {
    dt_iop_color_picker_reset(self, FALSE);

    // Reset the mask preview when leaving the module.
    if (g && g->mask_display)
    {
      g->mask_display = FALSE;
      if (g->show_saturation_mask)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->show_saturation_mask), FALSE);
    }
  }
}

static void _channel_tabs_switch_callback(GtkNotebook *notebook,
                                          GtkWidget *page,
                                          guint page_num,
                                          dt_iop_module_t *self)
{
  DT_GUARD_GUI_UPDATE();

  dt_iop_satcurve_gui_data_t *g = self->gui_data;
  if (!g)
    return;

  g->active_channel = (page_num == 1)
                          ? DT_IOP_SATCURVE_CHANNEL_BRILLIANCE
                          : DT_IOP_SATCURVE_CHANNEL_SATURATION;
  g->selected = -1;

  gtk_widget_queue_draw(GTK_WIDGET(g->area));
}

void gui_changed(dt_iop_module_t *self, GtkWidget *widget, void *previous)
{
  dt_iop_satcurve_gui_data_t *g = self->gui_data;
  dt_iop_satcurve_params_t *p = self->params;
  if (!g)
    return;

  // g->formula is a _from_params widget: the framework already commits
  // the history item internally after this callback returns (Path A).
  // No manual dt_dev_add_history_item() needed here.
  if (!widget || widget == g->formula)
    gtk_widget_queue_draw(GTK_WIDGET(g->area));

  if (!widget || widget == g->use_guided_filter)
  {
    gtk_widget_set_sensitive(g->gf_radius, p->use_guided_filter);
    gtk_widget_set_sensitive(g->gf_feathering, p->use_guided_filter);
    gtk_widget_set_sensitive(g->gf_iterations, p->use_guided_filter);
  }
}

void gui_update(dt_iop_module_t *self)
{
  dt_iop_satcurve_gui_data_t *g = self->gui_data;
  const dt_iop_satcurve_params_t *p = self->params;
  if (!g)
    return;

  if (g->formula)
    dt_bauhaus_combobox_set(g->formula, p->formula);

  if (g->notebook)
    gtk_notebook_set_current_page(GTK_NOTEBOOK(g->notebook),
                                  g->active_channel == DT_IOP_SATCURVE_CHANNEL_BRILLIANCE ? 1 : 0);

  gtk_widget_set_sensitive(g->gf_radius, p->use_guided_filter);
  gtk_widget_set_sensitive(g->gf_feathering, p->use_guided_filter);
  gtk_widget_set_sensitive(g->gf_iterations, p->use_guided_filter);

  if (g->area)
    gtk_widget_queue_draw(GTK_WIDGET(g->area));
}

void gui_cleanup(dt_iop_module_t *self)
{
  dt_iop_satcurve_gui_data_t *g = self->gui_data;
  if (!g)
    return;

  for (int ch = 0; ch < DT_IOP_SATCURVE_CHANNELS; ch++)
    if (g->channel[ch].curve)
      dt_draw_curve_destroy(g->channel[ch].curve);

  dt_pthread_mutex_destroy(&g->histogram_lock);
}

void gui_init(dt_iop_module_t *self)
{
  dt_iop_satcurve_gui_data_t *g = IOP_GUI_ALLOC(satcurve);
  const dt_iop_satcurve_params_t *p = self->default_params;

  g->selected = -1;
  g->dragging = FALSE;
  g->active_channel = DT_IOP_SATCURVE_CHANNEL_SATURATION;
  g->mask_display = FALSE;

  for (int ch = 0; ch < DT_IOP_SATCURVE_CHANNELS; ch++)
  {
    g->channel[ch].curve_type = p->channel[ch].curve_type;
    g->channel[ch].curve_num_nodes = p->channel[ch].curve_num_nodes;
    g->channel[ch].curve = dt_draw_curve_new(0.f, 1.f, p->channel[ch].curve_type);

    for (int i = 0; i < p->channel[ch].curve_num_nodes; i++)
      dt_draw_curve_add_point(g->channel[ch].curve,
                              p->channel[ch].curve[i].x,
                              p->channel[ch].curve[i].y);
  }

  memset(g->histogram, 0, sizeof(g->histogram));
  g->histogram_max = 1e-6f;
  dt_pthread_mutex_init(&g->histogram_lock, NULL);

  g->picker_valid = FALSE;
  g->picked_s = g->picked_s_min = g->picked_s_max = 0.f;

  self->widget = dt_gui_vbox();

  static dt_action_def_t notebook_def = {};

  g->notebook = dt_ui_notebook_new(&notebook_def);
  gtk_notebook_set_show_border(g->notebook, FALSE);
  gtk_notebook_set_scrollable(g->notebook, FALSE);
  gtk_notebook_popup_disable(g->notebook);

  GtkWidget *page_sat = dt_ui_notebook_page(g->notebook, N_("saturation"), NULL);
  GtkWidget *page_bri = dt_ui_notebook_page(g->notebook, N_("brilliance"), NULL);
  gtk_widget_set_size_request(page_sat, -1, 0);
  gtk_widget_set_size_request(page_bri, -1, 0);

  gtk_notebook_set_current_page(g->notebook, 0);

  g_signal_connect(G_OBJECT(g->notebook), "switch-page",
                   G_CALLBACK(_channel_tabs_switch_callback), self);

  dt_action_define_iop(self, NULL, N_("page"), GTK_WIDGET(g->notebook), &notebook_def);

  dt_gui_box_add(self->widget, GTK_WIDGET(g->notebook));

  g->area = GTK_DRAWING_AREA(dt_ui_resize_wrap(NULL, 0, "plugins/darkroom/satcurve/graph_height"));
  gtk_widget_set_size_request(GTK_WIDGET(g->area), -1, DT_PIXEL_APPLY_DPI(180));
  gtk_widget_add_events(GTK_WIDGET(g->area),
                        GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK | GDK_SCROLL_MASK);
  g_signal_connect(G_OBJECT(g->area), "draw", G_CALLBACK(area_draw), self);
  g_signal_connect(G_OBJECT(g->area), "button-press-event", G_CALLBACK(area_button), self);
  g_signal_connect(G_OBJECT(g->area), "button-release-event", G_CALLBACK(area_release), self);
  g_signal_connect(G_OBJECT(g->area), "motion-notify-event", G_CALLBACK(area_motion), self);
  g_signal_connect(G_OBJECT(g->area), "scroll-event", G_CALLBACK(area_scroll), self);
  dt_gui_box_add(self->widget, GTK_WIDGET(g->area));

  GtkWidget *main_vbox = self->widget;

  GtkWidget *controls_hbox = dt_gui_hbox();
  gtk_box_set_spacing(GTK_BOX(controls_hbox), DT_PIXEL_APPLY_DPI(5));

  g->colorpicker = dt_color_picker_new_with_cst(self, DT_COLOR_PICKER_POINT, NULL, IOP_CS_RGB);
  gtk_widget_set_tooltip_text(g->colorpicker, _("pick saturation from image"));

  // Toggle button for the saturation-mask preview.
  g->show_saturation_mask = dtgtk_togglebutton_new(dtgtk_cairo_paint_showmask, 0, NULL);
  gtk_widget_set_tooltip_text(g->show_saturation_mask, _("display saturation mask"));
  g_signal_connect(G_OBJECT(g->show_saturation_mask), "toggled",
                   G_CALLBACK(show_saturation_mask_callback), self);

  gtk_box_pack_start(GTK_BOX(controls_hbox), g->colorpicker, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(controls_hbox), g->show_saturation_mask, FALSE, FALSE, 0);

  // Temporarily set packing target to controls_hbox
  self->widget = controls_hbox;

  g->formula = dt_bauhaus_combobox_from_params(self, "formula");
  dt_bauhaus_combobox_add(g->formula, _("JzAzBz"));
  dt_bauhaus_combobox_add(g->formula, _("darktable UCS"));
  gtk_widget_set_tooltip_text(g->formula,
                              _("choose the perceptual saturation definition used by both curves"));

  // Expand to fill remaining horizontal space and align the widget to the right end
  gtk_widget_set_hexpand(g->formula, TRUE);
  gtk_widget_set_halign(g->formula, GTK_ALIGN_END);

  // Restore main container packing target
  self->widget = main_vbox;
  dt_gui_box_add(self->widget, controls_hbox);

  GtkWidget *gf_box = dt_gui_vbox();
  dt_gui_new_collapsible_section(&g->gf_section,
                                 "plugins/darkroom/satcurve/expand_guided_filter",
                                 _("guided filter"),
                                 GTK_BOX(gf_box), DT_ACTION(self));

  dt_iop_module_t *gf_section =
      DT_IOP_SECTION_FOR_PARAMS(self, NULL, g->gf_section.container);

  g->use_guided_filter = dt_bauhaus_toggle_from_params(gf_section, "use_guided_filter");
  gtk_widget_set_tooltip_text(g->use_guided_filter,
                              _("spatially modulate the strength of the curves with an edge-aware "
                                "guided filter, based on local saturation, to reduce halos"));

  g->gf_radius = dt_bauhaus_slider_from_params(gf_section, "gf_radius");
  dt_bauhaus_slider_set_format(g->gf_radius, _("px"));
  dt_bauhaus_slider_set_digits(g->gf_radius, 1);
  gtk_widget_set_tooltip_text(g->gf_radius, _("size of the neighbourhood used to guide the filter"));

  g->gf_feathering = dt_bauhaus_slider_from_params(gf_section, "gf_feathering");
  dt_bauhaus_slider_set_digits(g->gf_feathering, 1);
  gtk_widget_set_tooltip_text(g->gf_feathering,
                              _("precision of the feathering: higher values produce softer, "
                                "less edge-sensitive transitions"));

  g->gf_iterations = dt_bauhaus_slider_from_params(gf_section, "gf_iterations");
  dt_bauhaus_slider_set_digits(g->gf_iterations, 0);
  gtk_widget_set_tooltip_text(g->gf_iterations,
                              _("number of times the guided filter is applied recursively; "
                                "increases smoothing but costs more time"));

  dt_gui_box_add(self->widget, gf_box);
}

void init(dt_iop_module_t *self)
{
  self->params = calloc(1, sizeof(dt_iop_satcurve_params_t));
  self->default_params = calloc(1, sizeof(dt_iop_satcurve_params_t));
  self->params_size = sizeof(dt_iop_satcurve_params_t);
  self->gui_data = NULL;

  reset_params(self->params);
  reset_params(self->default_params);
}

void cleanup(dt_iop_module_t *self)
{
  free(self->params);
  self->params = NULL;
  free(self->default_params);
  self->default_params = NULL;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on