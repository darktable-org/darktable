/*
    This file is part of darktable,
    Copyright (C) 2010-2024 darktable developers.

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
#include "common/iop_profile.h"
#include "common/math.h"
#include "bauhaus/bauhaus.h"
#include "dtgtk/button.h"
#include "dtgtk/paint.h"
#include "common/colorspaces.h"
#include "common/colorspaces_inline_conversions.h"
#include "common/chromatic_adaptation.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "gui/color_picker_proxy.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"
#include "libs/lib.h"
#include "common/color_harmony.h"
#include "common/color_ryb.h"
#include "common/opencl.h"
#include "common/gaussian.h"
#include "control/conf.h"

#include <gtk/gtk.h>
#include <math.h>
#include <stdlib.h>

#define COLORHARMONIZER_HUE_BINS 360
#define COLORHARMONIZER_MAX_NODES 4
// Resolution for the RYB↔UCS lookup tables (0.5° steps).
#define COLORHARMONIZER_RYB_INVERSE_STEPS 720

// Precomputed hue conversion tables, built once in init_global.
// Indexed by hue fraction × COLORHARMONIZER_RYB_INVERSE_STEPS.
static float s_ucs_to_ryb_lut[COLORHARMONIZER_RYB_INVERSE_STEPS];
static float s_ryb_to_ucs_lut[COLORHARMONIZER_RYB_INVERSE_STEPS];

DT_MODULE_INTROSPECTION(6, dt_iop_colorharmonizer_params_t)

typedef enum dt_iop_colorharmonizer_rule_t
{
  DT_COLORHARMONIZER_MONOCHROMATIC = 0,           // $DESCRIPTION: "monochromatic"
  DT_COLORHARMONIZER_ANALOGOUS = 1,               // $DESCRIPTION: "analogous"
  DT_COLORHARMONIZER_ANALOGOUS_COMPLEMENTARY = 2, // $DESCRIPTION: "analogous complementary"
  DT_COLORHARMONIZER_COMPLEMENTARY = 3,           // $DESCRIPTION: "complementary"
  DT_COLORHARMONIZER_SPLIT_COMPLEMENTARY = 4,     // $DESCRIPTION: "split complementary"
  DT_COLORHARMONIZER_DYAD = 5,                    // $DESCRIPTION: "dyad"
  DT_COLORHARMONIZER_TRIAD = 6,                   // $DESCRIPTION: "triad"
  DT_COLORHARMONIZER_TETRAD = 7,                  // $DESCRIPTION: "tetrad"
  DT_COLORHARMONIZER_SQUARE = 8,                  // $DESCRIPTION: "square"
  DT_COLORHARMONIZER_CUSTOM = 9                   // $DESCRIPTION: "custom"
} dt_iop_colorharmonizer_rule_t;

typedef struct dt_iop_colorharmonizer_params_t
{
  dt_iop_colorharmonizer_rule_t rule; // $DEFAULT: DT_COLORHARMONIZER_COMPLEMENTARY $DESCRIPTION: "harmony rule"
  float anchor_hue;                   // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.1 $DESCRIPTION: "anchor hue"
  float pull_strength;                // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "pull strength"
  float neutral_protection;           // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.5 $DESCRIPTION: "neutral color protection"
  float pull_width;                   // $MIN: 0.25 $MAX: 4.0 $DEFAULT: 1.0 $DESCRIPTION: "pull width"
  float custom_hue[4];                // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "custom node hue"
  int   num_custom_nodes;             // $MIN: 2 $MAX: 4 $DEFAULT: 4 $DESCRIPTION: "nodes"
  float node_saturation[4];           // $MIN: 0.0 $MAX: 2.0 $DEFAULT: 1.0 $DESCRIPTION: "node saturation"
  float smoothing;                    // $MIN: 0.0 $MAX: 2.0 $DEFAULT: 0.0 $DESCRIPTION: "smoothing"
} dt_iop_colorharmonizer_params_t;

// Pipe data: params plus harmony node positions precomputed in commit_params so
// that process() and process_cl() don't pay the RYB↔UCS conversion cost per run.
typedef struct dt_iop_colorharmonizer_data_t
{
  dt_iop_colorharmonizer_params_t params;
  float nodes[COLORHARMONIZER_MAX_NODES]; // UCS hue [0,1) of each harmony node
  int   num_nodes;
} dt_iop_colorharmonizer_data_t;

typedef struct dt_iop_colorharmonizer_gui_data_t
{
  GtkWidget *rule, *anchor_hue, *pull_strength, *neutral_protection, *pull_width, *smoothing;
  GtkWidget *set_from_vectorscope, *sync_to_vectorscope, *auto_detect;
  GtkWidget *num_custom_nodes_slider;                    // node count (custom mode only)
  GtkWidget *swatches_area;                              // horizontal swatches row (non-custom mode)
  GtkWidget *node_swatch[COLORHARMONIZER_MAX_NODES];     // swatches inside swatches_area
  GtkWidget *custom_swatch[COLORHARMONIZER_MAX_NODES];   // swatches inside custom rows
  GtkWidget *custom_hue_slider[COLORHARMONIZER_MAX_NODES];
  GtkWidget *custom_row[COLORHARMONIZER_MAX_NODES];
  GtkWidget *sat_row[COLORHARMONIZER_MAX_NODES];         // saturation rows
  GtkWidget *sat_slider[COLORHARMONIZER_MAX_NODES];      // per-node saturation sliders (chroma gradient)
  dt_gui_collapsible_section_t sat_section;              // collapsible "Saturation" section
  float      hue_histogram[COLORHARMONIZER_HUE_BINS];
  gboolean   histogram_valid;
  GMutex     histogram_lock;
} dt_iop_colorharmonizer_gui_data_t;

typedef struct dt_iop_colorharmonizer_global_data_t
{
  int kernel_colorharmonizer_map;
  int kernel_colorharmonizer_apply;
} dt_iop_colorharmonizer_global_data_t;

const char *name()
{
  return _("color harmonizer");
}

const char **description(dt_iop_module_t *self)
{
  return dt_iop_set_description
    (self,
     _("harmonize colors toward a selected palette in perceptual space"),
     _("creative color grading"),
     _("linear, RGB, scene-referred"),
     _("darktable UCS / JCH (perceptual)"),
     _("linear, RGB, scene-referred"));
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING;
}

int default_group()
{
  return IOP_GROUP_COLOR;
}

dt_iop_colorspace_type_t default_colorspace(dt_iop_module_t *self,
                                            dt_dev_pixelpipe_t *pipe,
                                            dt_dev_pixelpipe_iop_t *piece)
{
  // We work in RGB pipeline, convert internally to darktable UCS.
  return IOP_CS_RGB;
}

int legacy_params(dt_iop_module_t *self,
                  const void *const old_params,
                  const int old_version,
                  void **new_params,
                  int32_t *new_params_size,
                  int *new_version)
{
  if(old_version == 2)
  {
    typedef struct
    {
      dt_iop_colorharmonizer_rule_t rule;
      float anchor_hue;
      float pull_strength;
      float neutral_protection;
      float pull_width;
    } v2_params_t;

    typedef struct
    {
      dt_iop_colorharmonizer_rule_t rule;
      float anchor_hue;
      float pull_strength;
      float neutral_protection;
      float pull_width;
      float custom_hue[4];
    } v3_params_t;

    const v2_params_t *o = old_params;
    v3_params_t *n = malloc(sizeof(v3_params_t));
    if(!n) return 1;

    n->rule            = o->rule;
    n->anchor_hue      = o->anchor_hue;
    n->pull_strength = o->pull_strength;
    n->neutral_protection = o->neutral_protection;
    n->pull_width      = o->pull_width;
    n->custom_hue[0]   = 0.0f;
    n->custom_hue[1]   = 0.25f;
    n->custom_hue[2]   = 0.5f;
    n->custom_hue[3]   = 0.75f;

    *new_params      = n;
    *new_params_size = sizeof(v3_params_t);
    *new_version     = 3;
    return 0;
  }
  if(old_version == 3)
  {
    typedef struct
    {
      dt_iop_colorharmonizer_rule_t rule;
      float anchor_hue;
      float pull_strength;
      float neutral_protection;
      float pull_width;
      float custom_hue[4];
    } v3_params_t;

    const v3_params_t *o = old_params;
    dt_iop_colorharmonizer_params_t *n = malloc(sizeof(dt_iop_colorharmonizer_params_t));
    if(!n) return 1;

    n->rule             = o->rule;
    n->anchor_hue       = o->anchor_hue;
    n->pull_strength  = o->pull_strength;
    n->neutral_protection  = o->neutral_protection;
    n->pull_width       = o->pull_width;
    for(int i = 0; i < 4; i++) n->custom_hue[i] = o->custom_hue[i];
    n->num_custom_nodes = 4;
    for(int i = 0; i < 4; i++) n->node_saturation[i] = 1.0f;

    *new_params      = n;
    *new_params_size = sizeof(dt_iop_colorharmonizer_params_t);
    *new_version     = 5;
    return 0;
  }
  if(old_version == 4)
  {
    typedef struct
    {
      dt_iop_colorharmonizer_rule_t rule;
      float anchor_hue;
      float pull_strength;
      float neutral_protection;
      float pull_width;
      float custom_hue[4];
      int   num_custom_nodes;
    } v4_params_t;

    const v4_params_t *o = old_params;
    dt_iop_colorharmonizer_params_t *n = malloc(sizeof(dt_iop_colorharmonizer_params_t));
    if(!n) return 1;

    n->rule             = o->rule;
    n->anchor_hue       = o->anchor_hue;
    n->pull_strength  = o->pull_strength;
    n->neutral_protection  = o->neutral_protection;
    n->pull_width       = o->pull_width;
    for(int i = 0; i < 4; i++) n->custom_hue[i] = o->custom_hue[i];
    n->num_custom_nodes = o->num_custom_nodes;
    for(int i = 0; i < 4; i++) n->node_saturation[i] = 1.0f;

    *new_params      = n;
    *new_params_size = sizeof(dt_iop_colorharmonizer_params_t);
    *new_version     = 5;
    return 0;
  }
  if(old_version == 5)
  {
    dt_iop_colorharmonizer_params_t *n = malloc(sizeof(dt_iop_colorharmonizer_params_t));
    if(!n) return 1;
    memcpy(n, old_params, sizeof(dt_iop_colorharmonizer_params_t) - sizeof(float));
    n->smoothing = 0.0f;

    *new_params      = n;
    *new_params_size = sizeof(dt_iop_colorharmonizer_params_t);
    *new_version     = 6;
    return 0;
  }
  return 1;
}

// Compute a hue shift toward the nearest harmony node, scaled by Gaussian proximity.
//
// We use the nearest-node's angular difference (diff_winning) multiplied by
// the peak Gaussian weight (max_w). This ensures:
//   - A pixel already at a node gets zero shift (diff_winning = 0).
//   - A pixel far from all nodes gets near-zero shift (max_w ≈ 0).
//   - The correction magnitude tapers smoothly as the pixel moves away from its node.
//
//   Narrow zone (< 1): Gaussian drops off quickly → only hues very close to a
//                       node are attracted; distant hues are barely shifted.
//   Default zone (1):  Gaussian tapers to ~14 % at the midpoint between nodes.
//   Wide zone   (> 1): Gaussian stays high across the full hue circle → broad,
//                       global correction; all hues are pulled noticeably.
static inline float get_weighted_hue_shift(float px_hue, const float *nodes, int num_nodes,
                                           float pull_width_factor,
                                           int *out_winning_idx, float *out_max_weight)
{
  if(num_nodes <= 0)
  {
    if(out_winning_idx) *out_winning_idx = 0;
    if(out_max_weight) *out_max_weight = 0.0f;
    return 0.0f;
  }
  const float sigma = pull_width_factor * 0.5f / (float)num_nodes;
  const float inv_2sigma2 = 1.0f / (2.0f * sigma * sigma);

  float max_w       = 0.0f;
  int   winning_idx = 0;
  float diff_winning = 0.0f;

  for(int i = 0; i < num_nodes; i++)
  {
    float d = fabsf(px_hue - nodes[i]);
    if(d > 0.5f) d = 1.0f - d;

    const float w = expf(-d * d * inv_2sigma2);
    float diff = nodes[i] - px_hue;
    if(diff > 0.5f)       diff -= 1.0f;
    else if(diff < -0.5f) diff += 1.0f;

    if(w > max_w)
    {
      max_w       = w;
      winning_idx = i;
      diff_winning = diff;
    }
  }

  if(out_winning_idx) *out_winning_idx = winning_idx;
  if(out_max_weight)  *out_max_weight  = max_w;
  return diff_winning * max_w;
}

static inline float wrap_hue(float h)
{
  h = fmodf(h, 1.0f);
  if (h < 0.0f) h += 1.0f;
  return h;
}

// Forward declarations: defined later in the file, needed here for get_harmony_nodes.
static float _ucs_hue_to_ryb_hue(float ucs_hue);
static float _ucs_to_ryb_fast(float ucs);
static float _ryb_to_ucs_fast(float yrb);
// Compute harmony node positions in UCS hue space [0,1).
// For predefined rules the geometry comes from dt_color_harmony_get_sector_angles()
// (color_harmony.h) so that the nodes used for processing are exactly aligned
// with the guide overlay shown in the vectorscope.
static inline void get_harmony_nodes(dt_iop_colorharmonizer_rule_t rule, float anchor_hue,
                                     const float *custom_hue, int custom_n,
                                     float *nodes, int *num_nodes)
{
  if(rule == DT_COLORHARMONIZER_CUSTOM)
  {
    const int n = CLAMP(custom_n, 1, COLORHARMONIZER_MAX_NODES);
    for(int i = 0; i < n; i++)
      nodes[i] = custom_hue ? custom_hue[i] : 0.0f;
    *num_nodes = n;
    return;
  }

  // Convert the UCS anchor hue to an integer RYB rotation (degrees) and look up
  // the vectorscope geometry table to obtain absolute RYB node positions.
  const int rotation = (int)roundf(_ucs_to_ryb_fast(anchor_hue) * 360.0f) % 360;
  float node_angles[COLORHARMONIZER_MAX_NODES];
  dt_color_harmony_get_sector_angles((dt_color_harmony_type_t)(rule + 1), rotation,
                                     node_angles, num_nodes);
  // Convert each RYB node back to UCS for use in the processing pipeline.
  for(int i = 0; i < *num_nodes; i++)
    nodes[i] = _ryb_to_ucs_fast(node_angles[i]);
}

void commit_params(dt_iop_module_t *self,
                   dt_iop_params_t *p1,
                   dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_colorharmonizer_params_t *p = p1;
  dt_iop_colorharmonizer_data_t   *d = piece->data;
  d->params = *p;
  get_harmony_nodes(p->rule, p->anchor_hue, p->custom_hue, p->num_custom_nodes,
                    d->nodes, &d->num_nodes);
}

static void _update_histogram(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const float *ivoid, const dt_iop_roi_t *roi_in)
{
  if(!self->gui_data || !((piece->pipe->type & DT_DEV_PIXELPIPE_PREVIEW) == DT_DEV_PIXELPIPE_PREVIEW))
    return;

  dt_iop_colorharmonizer_gui_data_t *g = self->gui_data;
  const dt_iop_order_iccprofile_info_t *const work_profile = dt_ioppr_get_pipe_work_profile_info(piece->pipe);
  if(!work_profile) return;

  const int ch = piece->colors;
  const float L_white = Y_to_dt_UCS_L_star(1.0f);
  float local_histo[COLORHARMONIZER_HUE_BINS] = { 0.0f };

  for(int j = 0; j < roi_in->height; j++)
  {
    const float *src = ((const float *)ivoid) + (size_t)ch * roi_in->width * j;
    for(int i = 0; i < roi_in->width; i++, src += ch)
    {
      dt_aligned_pixel_t px_rgb = { fmaxf(src[0], 0.0f), fmaxf(src[1], 0.0f),
                                    fmaxf(src[2], 0.0f), 0.0f };
      dt_aligned_pixel_t px_JCH;
      dt_ioppr_rgb_matrix_to_dt_UCS_JCH(px_rgb, px_JCH, work_profile->matrix_in_transposed, L_white);

      const float chroma = px_JCH[1];
      if(chroma > 0.01f)
      {
        const float hue = (px_JCH[2] + M_PI_F) / (2.f * M_PI_F);
        const int bin = (int)(hue * COLORHARMONIZER_HUE_BINS) % COLORHARMONIZER_HUE_BINS;
        local_histo[bin] += chroma;
      }
    }
  }

  g_mutex_lock(&g->histogram_lock);
  memcpy(g->hue_histogram, local_histo, sizeof(local_histo));
  g->histogram_valid = TRUE;
  g_mutex_unlock(&g->histogram_lock);
}

void process(dt_iop_module_t *self,
             dt_dev_pixelpipe_iop_t *piece,
             const void *const ivoid,
             void *const ovoid,
             const dt_iop_roi_t *const roi_in,
             const dt_iop_roi_t *const roi_out)
{
  const dt_iop_colorharmonizer_data_t   *d = piece->data;
  const dt_iop_colorharmonizer_params_t *p = &d->params;
  const size_t ch = piece->colors;

  if(!dt_iop_have_required_input_format(4, self, piece->colors, ivoid, ovoid, roi_in, roi_out))
    return;

  const float *const nodes = d->nodes;
  const int num_nodes = d->num_nodes;

  const dt_iop_order_iccprofile_info_t *const work_profile = dt_ioppr_get_pipe_work_profile_info(piece->pipe);
  if(!work_profile) return;

  const float L_white = Y_to_dt_UCS_L_star(1.0f);

  const float pull_strength = p->pull_strength;
  const float pull_width    = p->pull_width;
  const float np_t          = p->neutral_protection;
  // cutoff is the chroma level at which neutral_protection halves the correction
  // (chroma_weight = 0.5 when chroma == cutoff). The cubic response concentrates
  // the protective effect toward very low-chroma values.
  const float cutoff        = np_t * np_t * np_t * 0.03f;

  if(p->smoothing <= 0.0f)
  {
    // Fused single pass: forward conversion → correction → inverse conversion.
    // No intermediate buffer needed; each pixel is touched exactly once.
    DT_OMP_FOR()
    for(int j = 0; j < roi_out->height; j++)
    {
      const float *in  = ((const float *)ivoid) + (size_t)ch * roi_in->width * j;
      float       *out = ((float *)ovoid)        + (size_t)ch * roi_out->width * j;

      for(int i = 0; i < roi_out->width; i++, in += ch, out += ch)
      {
        const dt_aligned_pixel_t px_rgb = { fmaxf(in[0], 0.0f), fmaxf(in[1], 0.0f),
                                            fmaxf(in[2], 0.0f), 0.0f };
        dt_aligned_pixel_t px_JCH;
        dt_ioppr_rgb_matrix_to_dt_UCS_JCH(px_rgb, px_JCH, work_profile->matrix_in_transposed, L_white);

        const float hue    = (px_JCH[2] + M_PI_F) / (2.f * M_PI_F);
        const float chroma = px_JCH[1];

        int   winning_idx = 0;
        float max_weight  = 0.0f;
        const float hue_shift = get_weighted_hue_shift(hue, nodes, num_nodes, pull_width,
                                                       &winning_idx, &max_weight);
        const float sat_delta = (p->node_saturation[winning_idx] - 1.0f) * max_weight;

        // Smooth hyperbolic ramp: approaches 0 for neutral colors, ~1 for saturated colors.
        const float chroma_weight = chroma / (chroma + cutoff + 1e-5f);

        px_JCH[2] = wrap_hue(hue + hue_shift * pull_strength * chroma_weight) * 2.f * M_PI_F - M_PI_F;
        px_JCH[1] = fmaxf(chroma * (1.0f + sat_delta * chroma_weight), 0.0f);

        dt_aligned_pixel_t px_xyY, px_xyz_d65, px_xyz;
        dt_UCS_JCH_to_xyY(px_JCH, L_white, px_xyY);
        dt_xyY_to_XYZ(px_xyY, px_xyz_d65);
        XYZ_D65_to_D50(px_xyz_d65, px_xyz);

        dt_aligned_pixel_t px_rgb_out;
        dt_apply_transposed_color_matrix(px_xyz, work_profile->matrix_out_transposed, px_rgb_out);
        for_each_channel(c) out[c] = px_rgb_out[c];
        out[3] = in[3];
      }
    }
  }
  else
  {
    // Two-pass with JCH caching: cache the forward conversion from Pass 1
    // so Pass 2 only performs the inverse (JCH → RGB).
    const size_t npx = (size_t)roi_out->width * roi_out->height;
    float *jch_cache   = dt_alloc_align_float(3 * npx);
    float *corrections = dt_alloc_align_float(2 * npx);
    if(!jch_cache || !corrections)
    {
      dt_free_align(jch_cache);
      dt_free_align(corrections);
      return;
    }

    // Pass 1: forward RGB → JCH (cached) + compute per-pixel corrections.
    DT_OMP_FOR()
    for(int j = 0; j < roi_out->height; j++)
    {
      const float *in  = ((const float *)ivoid) + (size_t)ch * roi_in->width * j;
      const size_t row = (size_t)j * roi_out->width;

      for(int i = 0; i < roi_out->width; i++, in += ch)
      {
        const dt_aligned_pixel_t px_rgb = { fmaxf(in[0], 0.0f), fmaxf(in[1], 0.0f),
                                            fmaxf(in[2], 0.0f), 0.0f };
        dt_aligned_pixel_t px_JCH;
        dt_ioppr_rgb_matrix_to_dt_UCS_JCH(px_rgb, px_JCH, work_profile->matrix_in_transposed, L_white);

        const float hue = (px_JCH[2] + M_PI_F) / (2.f * M_PI_F);

        const size_t k = row + i;
        jch_cache[k * 3]     = px_JCH[0];  // J
        jch_cache[k * 3 + 1] = px_JCH[1];  // chroma
        jch_cache[k * 3 + 2] = hue;        // normalized hue [0,1)

        int   winning_idx = 0;
        float max_weight  = 0.0f;
        const float hue_shift = get_weighted_hue_shift(hue, nodes, num_nodes, pull_width,
                                                       &winning_idx, &max_weight);
        corrections[k * 2]     = hue_shift;
        corrections[k * 2 + 1] = (p->node_saturation[winning_idx] - 1.0f) * max_weight;
      }
    }

    // Gaussian blur: smooth corrections spatially to soften zone-boundary transitions.
    // sigma scales with the preview↔full-res ratio so the blur radius stays consistent
    // across pipeline scales; 1.5 px is the minimum useful radius, and wider attraction
    // zones warrant proportionally more smoothing.
    const float sigma = p->smoothing * fmaxf(1.5f, 8.0f * roi_in->scale / piece->iscale)
                        * fmaxf(1.0f, pull_width);
    dt_gaussian_mean_blur(corrections, roi_out->width, roi_out->height, 2, sigma);

    // Pass 2: apply blurred corrections using cached JCH (inverse only).
    DT_OMP_FOR()
    for(int j = 0; j < roi_out->height; j++)
    {
      const float *in  = ((const float *)ivoid) + (size_t)ch * roi_in->width  * j;
      float       *out = ((float *)ovoid)        + (size_t)ch * roi_out->width * j;
      const size_t row = (size_t)j * roi_out->width;

      for(int i = 0; i < roi_out->width; i++, in += ch, out += ch)
      {
        const size_t k     = row + i;
        const float J      = jch_cache[k * 3];
        const float chroma = jch_cache[k * 3 + 1];
        const float hue    = jch_cache[k * 3 + 2];

        const float chroma_weight = chroma / (chroma + cutoff + 1e-5f);

        const float new_hue = wrap_hue(hue + corrections[k * 2] * pull_strength * chroma_weight);
        dt_aligned_pixel_t px_JCH = { J,
                                      fmaxf(chroma * (1.0f + corrections[k * 2 + 1] * chroma_weight), 0.0f),
                                      new_hue * 2.f * M_PI_F - M_PI_F,
                                      0.0f };

        dt_aligned_pixel_t px_xyY, px_xyz_d65, px_xyz;
        dt_UCS_JCH_to_xyY(px_JCH, L_white, px_xyY);
        dt_xyY_to_XYZ(px_xyY, px_xyz_d65);
        XYZ_D65_to_D50(px_xyz_d65, px_xyz);

        dt_aligned_pixel_t px_rgb_out;
        dt_apply_transposed_color_matrix(px_xyz, work_profile->matrix_out_transposed, px_rgb_out);
        for_each_channel(c) out[c] = px_rgb_out[c];
        out[3] = in[3];
      }
    }

    dt_free_align(jch_cache);
    dt_free_align(corrections);
  }

  // Build a chroma-weighted hue histogram from the input for auto-detection.
  _update_histogram(self, piece, ivoid, roi_in);
}

void init(dt_iop_module_t *self)
{
  dt_iop_default_init(self);

  // Set sensible defaults for custom harmony nodes (evenly spread).
  // Other fields use introspection defaults from the param annotations.
  dt_iop_colorharmonizer_params_t *p = self->default_params;
  p->custom_hue[0] = 0.0f;
  p->custom_hue[1] = 0.25f;
  p->custom_hue[2] = 0.5f;
  p->custom_hue[3] = 0.75f;
  memcpy(self->params, self->default_params, self->params_size);
}

void cleanup(dt_iop_module_t *self)
{
  dt_iop_default_cleanup(self);
}

void init_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1, sizeof(dt_iop_colorharmonizer_data_t));
}

void cleanup_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

// Linearly interpolate between two hue values on the circle, returning a result in [0,1).
static inline float _hue_lerp(float a, float b, float t)
{
  if(b - a >  0.5f) b -= 1.0f;
  else if(a - b >  0.5f) a -= 1.0f;
  float r = a + t * (b - a);
  if(r < 0.0f) r += 1.0f;
  return r;
}

// O(1) UCS→RYB lookup with linear interpolation between table entries.
static inline float _ucs_to_ryb_fast(float ucs)
{
  const float pos  = ucs * COLORHARMONIZER_RYB_INVERSE_STEPS;
  const int   i0   = (int)pos % COLORHARMONIZER_RYB_INVERSE_STEPS;
  const int   i1   = (i0 + 1) % COLORHARMONIZER_RYB_INVERSE_STEPS;
  return _hue_lerp(s_ucs_to_ryb_lut[i0], s_ucs_to_ryb_lut[i1], pos - (int)pos);
}

// O(1) RYB→UCS lookup with linear interpolation between table entries.
static inline float _ryb_to_ucs_fast(float ryb)
{
  const float pos  = ryb * COLORHARMONIZER_RYB_INVERSE_STEPS;
  const int   i0   = (int)pos % COLORHARMONIZER_RYB_INVERSE_STEPS;
  const int   i1   = (i0 + 1) % COLORHARMONIZER_RYB_INVERSE_STEPS;
  return _hue_lerp(s_ryb_to_ucs_lut[i0], s_ryb_to_ucs_lut[i1], pos - (int)pos);
}

// Build both lookup tables once at module load.
// Forward table (UCS→RYB): _ucs_hue_to_ryb_hue involves a gamut binary search, so
// precomputing it avoids repeating that cost at every GUI or processing call.
// Inverse table (RYB→UCS): nearest-match scan over the forward table — O(N²) but
// N=720 and it only runs once, so the total cost is negligible.
static void _build_hue_luts(void)
{
  for(int i = 0; i < COLORHARMONIZER_RYB_INVERSE_STEPS; i++)
    s_ucs_to_ryb_lut[i] = _ucs_hue_to_ryb_hue(i / (float)COLORHARMONIZER_RYB_INVERSE_STEPS);

  for(int j = 0; j < COLORHARMONIZER_RYB_INVERSE_STEPS; j++)
  {
    const float target = j / (float)COLORHARMONIZER_RYB_INVERSE_STEPS;
    float best_dist = 1.0f, best_ucs = 0.0f;
    for(int i = 0; i < COLORHARMONIZER_RYB_INVERSE_STEPS; i++)
    {
      float d = fabsf(s_ucs_to_ryb_lut[i] - target);
      if(d > 0.5f) d = 1.0f - d;
      if(d < best_dist) { best_dist = d; best_ucs = i / (float)COLORHARMONIZER_RYB_INVERSE_STEPS; }
    }
    s_ryb_to_ucs_lut[j] = best_ucs;
  }
}

void init_global(dt_iop_module_so_t *self)
{
  _build_hue_luts();
  const int program = 40; // colorharmonizer.cl in programs.conf
  dt_iop_colorharmonizer_global_data_t *gd = malloc(sizeof(dt_iop_colorharmonizer_global_data_t));
  self->data = gd;
  gd->kernel_colorharmonizer_map    = dt_opencl_create_kernel(program, "colorharmonizer_map");
  gd->kernel_colorharmonizer_apply  = dt_opencl_create_kernel(program, "colorharmonizer_apply");
}

void cleanup_global(dt_iop_module_so_t *self)
{
  dt_iop_colorharmonizer_global_data_t *gd = self->data;
  dt_opencl_free_kernel(gd->kernel_colorharmonizer_map);
  dt_opencl_free_kernel(gd->kernel_colorharmonizer_apply);
  free(self->data);
  self->data = NULL;
}

#ifdef HAVE_OPENCL
int process_cl(dt_iop_module_t *self,
               dt_dev_pixelpipe_iop_t *piece,
               cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in,
               const dt_iop_roi_t *const roi_out)
{
  const dt_iop_colorharmonizer_data_t   *const d  = piece->data;
  const dt_iop_colorharmonizer_params_t *const p  = &d->params;
  const dt_iop_colorharmonizer_global_data_t *const gd = self->global_data;

  cl_int err = DT_OPENCL_DEFAULT_ERROR;

  if(piece->colors != 4)
    return err;

  const int devid  = piece->pipe->devid;
  const int width  = roi_out->width;
  const int height = roi_out->height;

  const dt_iop_order_iccprofile_info_t *const work_profile = dt_ioppr_get_pipe_work_profile_info(piece->pipe);
  if(!work_profile) return err;

  dt_colormatrix_t input_matrix, output_matrix;
  dt_colormatrix_mul(input_matrix,  XYZ_D50_to_D65_CAT16, work_profile->matrix_in);
  dt_colormatrix_mul(output_matrix, work_profile->matrix_out, XYZ_D65_to_D50_CAT16);

  float nodes[COLORHARMONIZER_MAX_NODES];
  memcpy(nodes, d->nodes, sizeof(nodes));
  const int num_nodes = d->num_nodes;
  float node_saturation[COLORHARMONIZER_MAX_NODES];
  for(int i = 0; i < COLORHARMONIZER_MAX_NODES; i++) node_saturation[i] = p->node_saturation[i];

  const float L_white = Y_to_dt_UCS_L_star(1.0f);

  cl_mem input_matrix_cl    = dt_opencl_copy_host_to_device_constant(devid, 12 * sizeof(float), input_matrix);
  cl_mem output_matrix_cl   = dt_opencl_copy_host_to_device_constant(devid, 12 * sizeof(float), output_matrix);
  cl_mem nodes_cl           = dt_opencl_copy_host_to_device_constant(devid, COLORHARMONIZER_MAX_NODES * sizeof(float), nodes);
  cl_mem node_saturation_cl = dt_opencl_copy_host_to_device_constant(devid, COLORHARMONIZER_MAX_NODES * sizeof(float), node_saturation);

  // Correction map buffer (float2 per pixel: hue_delta, sat_delta).
  const size_t f2sz = (size_t)width * height * 2 * sizeof(float);
  cl_mem corrections_cl = dt_opencl_alloc_device_buffer(devid, f2sz);

  // JCH cache buffer (float4 per pixel: J, chroma, hue, alpha).
  // Written by the map kernel so the apply kernel skips the forward conversion.
  const size_t jchsz = (size_t)width * height * 4 * sizeof(float);
  cl_mem jch_cl = dt_opencl_alloc_device_buffer(devid, jchsz);

  if(!input_matrix_cl || !output_matrix_cl || !nodes_cl || !node_saturation_cl
     || !corrections_cl || !jch_cl)
  {
    err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
    goto error;
  }

  // Step 1: compute per-pixel correction maps + cache JCH.
  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_colorharmonizer_map, width, height,
    CLARG(dev_in),
    CLARG(corrections_cl),
    CLARG(jch_cl),
    CLARG(width), CLARG(height),
    CLARG(input_matrix_cl),
    CLARG(nodes_cl), CLARG(num_nodes),
    CLARG(p->pull_width),
    CLARG(node_saturation_cl),
    CLARG(L_white));
  if(err != CL_SUCCESS) goto error;

  // Step 2: Gaussian-blur corrections spatially.
  if(p->smoothing > 0.0f)
  {
    const float sigma = p->smoothing * fmaxf(1.5f, 8.0f * roi_in->scale / piece->iscale)
                        * fmaxf(1.0f, p->pull_width);
    if((err = dt_gaussian_mean_blur_cl(devid, corrections_cl, width, height, 2, sigma)) != CL_SUCCESS) goto error;
  }

  // Step 3: apply smoothed corrections from cached JCH → output.
  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_colorharmonizer_apply, width, height,
    CLARG(dev_out),
    CLARG(width), CLARG(height),
    CLARG(output_matrix_cl),
    CLARG(jch_cl),
    CLARG(corrections_cl),
    CLARG(p->pull_strength), CLARG(p->neutral_protection),
    CLARG(L_white));

  if(err == CL_SUCCESS && self->gui_data && (piece->pipe->type & DT_DEV_PIXELPIPE_PREVIEW) == DT_DEV_PIXELPIPE_PREVIEW)
  {
    float *host_in = dt_alloc_align_float((size_t)width * height * 4);
    if(host_in)
    {
      if(dt_opencl_copy_device_to_host(devid, host_in, dev_in, width, height, 4 * sizeof(float)) == CL_SUCCESS)
      {
        _update_histogram(self, piece, host_in, roi_in);
      }
      dt_free_align(host_in);
    }
  }

error:
  dt_opencl_release_mem_object(input_matrix_cl);
  dt_opencl_release_mem_object(output_matrix_cl);
  dt_opencl_release_mem_object(nodes_cl);
  dt_opencl_release_mem_object(node_saturation_cl);
  dt_opencl_release_mem_object(corrections_cl);
  dt_opencl_release_mem_object(jch_cl);
  return err;
}
#endif

// Convert a darktable UCS JCH pixel to gamma-corrected sRGB.
// Chain: JCH → XYZ(D65) → linear sRGB → sRGB gamma
// Uses the native D65 sRGB matrix; skips the unnecessary D65→D50 adaptation step.
static inline void _jch_to_srgb(const dt_aligned_pixel_t JCH, const float L_white,
                                 dt_aligned_pixel_t sRGB)
{
  dt_aligned_pixel_t XYZ_D65, linear;
  dt_UCS_JCH_to_XYZ(JCH, L_white, XYZ_D65);
  dt_XYZ_to_Rec709_D65(XYZ_D65, linear);
  for_each_channel(c)
    sRGB[c] = linear[c] <= 0.0031308f ? 12.92f * linear[c]
                                      : 1.055f * powf(linear[c], 1.f / 2.4f) - 0.055f;
}

// Find the maximum sRGB-safe chroma for a UCS hue [0,1) at J = 0.65 (mid-brightness).
// Uses 16 iterations of binary search; result fits within [0, 1] sRGB without clamping.
static float _find_max_chroma(const float hue)
{
  const float L_white = Y_to_dt_UCS_L_star(1.0f);
  const float H = hue * 2.f * M_PI_F - M_PI_F;  // [0,1) → [-π, π]
  const float J = 0.65f;                         // ≈ Y=0.29, mid-brightness

  float C_lo = 0.f, C_hi = 2.f;
  for(int iter = 0; iter < 16; iter++)
  {
    const float C_mid = (C_lo + C_hi) * 0.5f;
    dt_aligned_pixel_t JCH = { J, C_mid, H, 0.f };
    dt_aligned_pixel_t sRGB;
    _jch_to_srgb(JCH, L_white, sRGB);
    if(sRGB[0] >= 0.f && sRGB[1] >= 0.f && sRGB[2] >= 0.f
       && sRGB[0] <= 1.f && sRGB[1] <= 1.f && sRGB[2] <= 1.f)
      C_lo = C_mid;
    else
      C_hi = C_mid;
  }
  return C_lo;
}

// Render a normalized UCS hue value [0,1) to display sRGB at mid-brightness.
// Backs off 15% from the gamut boundary so swatches look vivid but not clipped.
static void _hue_to_srgb(float hue, float *r, float *g, float *b)
{
  const float L_white = Y_to_dt_UCS_L_star(1.0f);
  const float H = hue * 2.f * M_PI_F - M_PI_F;
  const float J = 0.65f;

  dt_aligned_pixel_t JCH = { J, _find_max_chroma(hue) * 0.85f, H, 0.f };
  dt_aligned_pixel_t sRGB;
  _jch_to_srgb(JCH, L_white, sRGB);
  *r = CLAMP(sRGB[0], 0.f, 1.f);
  *g = CLAMP(sRGB[1], 0.f, 1.f);
  *b = CLAMP(sRGB[2], 0.f, 1.f);
}

static void _paint_hue_slider(GtkWidget *slider)
{
  for(int i = 0; i < DT_BAUHAUS_SLIDER_MAX_STOPS; i++)
  {
    const float stop = ((float)i / (float)(DT_BAUHAUS_SLIDER_MAX_STOPS - 1));
    float r, g, b;
    // slider positions are RYB fractions; convert to UCS for the color swatch
    _hue_to_srgb(_ryb_to_ucs_fast(stop), &r, &g, &b);
    dt_bauhaus_slider_set_stop(slider, stop, r, g, b);
  }
}

static void _paint_all_hue_sliders(const dt_iop_colorharmonizer_gui_data_t *const g)
{
  _paint_hue_slider(g->anchor_hue);
  for(int i = 0; i < COLORHARMONIZER_MAX_NODES; i++)
    _paint_hue_slider(g->custom_hue_slider[i]);
}

// Paint a saturation slider with a gray→full-color gradient at the given UCS hue.
static void _paint_sat_slider(GtkWidget *slider, const float hue)
{
  const float L_white = Y_to_dt_UCS_L_star(1.0f);
  const float J = 0.65f;
  const float H = hue * 2.f * M_PI_F - M_PI_F;

  const float C_lo = _find_max_chroma(hue);
  // C_swatch is the chroma used for the swatch (= the "neutral / 100%" reference color).
  // The saturation slider ranges 0–2, so stop=0.5 maps to value=1.0 (neutral).
  // We make stop=0.5 show the swatch color and stop=1.0 reach the gamut boundary (C_lo),
  // so the right end looks as vivid as the swatch instead of appearing washed-out.
  const float C_swatch = C_lo * 0.85f;

  for(int i = 0; i < DT_BAUHAUS_SLIDER_MAX_STOPS; i++)
  {
    const float stop = (float)i / (float)(DT_BAUHAUS_SLIDER_MAX_STOPS - 1);
    // Linear from 0 → C_swatch over [0, 0.5], then C_swatch → C_lo over [0.5, 1.0].
    const float C = stop <= 0.5f ? stop * 2.f * C_swatch
                                 : C_swatch + (stop - 0.5f) * 2.f * (C_lo - C_swatch);
    dt_aligned_pixel_t JCH = { J, C, H, 0.f };
    dt_aligned_pixel_t sRGB;
    _jch_to_srgb(JCH, L_white, sRGB);
    dt_bauhaus_slider_set_stop(slider, stop,
                               CLAMP(sRGB[0], 0.f, 1.f),
                               CLAMP(sRGB[1], 0.f, 1.f),
                               CLAMP(sRGB[2], 0.f, 1.f));
  }
  gtk_widget_queue_draw(slider);
}

static void _paint_all_sat_sliders(const dt_iop_colorharmonizer_gui_data_t *const g,
                                    const dt_iop_colorharmonizer_params_t *const p)
{
  float nodes[COLORHARMONIZER_MAX_NODES];
  int num_nodes = 0;
  get_harmony_nodes(p->rule, p->anchor_hue, p->custom_hue, p->num_custom_nodes, nodes, &num_nodes);
  for(int i = 0; i < num_nodes; i++)
    _paint_sat_slider(g->sat_slider[i], nodes[i]);
}

// Convert a UCS anchor_hue [0,1) to the RYB hue fraction [0,1) that the vectorscope uses.
// Path: UCS hue → gamut-clipped sRGB → linearize → RGB HSV hue → RYB hue.
static float _ucs_hue_to_ryb_hue(const float ucs_hue)
{
  float r, g, b;
  _hue_to_srgb(ucs_hue, &r, &g, &b);
  const dt_aligned_pixel_t srgb = { r, g, b, 0.f };
  dt_aligned_pixel_t lrgb;
  dt_sRGB_to_linear_sRGB(srgb, lrgb);
  dt_aligned_pixel_t HCV;
  dt_RGB_2_HCV(lrgb, HCV);
  return dt_rgb_hue_to_ryb_hue(HCV[0]);
}


static void _push_to_vectorscope(dt_iop_module_t *self)
{
  dt_iop_colorharmonizer_params_t *p = self->params;
  dt_color_harmony_guide_t guide;
  dt_lib_histogram_get_harmony(darktable.lib, &guide);

  if(p->rule == DT_COLORHARMONIZER_CUSTOM)
  {
    // Custom: provide absolute-angle nodes; type is left as NONE so the
    // vectorscope's own UI shows no standard rule selected.
    guide.type     = DT_COLOR_HARMONY_NONE;
    guide.custom_n = p->num_custom_nodes;
    for(int i = 0; i < p->num_custom_nodes; i++)
      guide.custom_angles[i] = _ucs_to_ryb_fast(p->custom_hue[i]);
  }
  else
  {
    guide.type        = (dt_color_harmony_type_t)(p->rule + 1);
    guide.rotation    = (int)roundf(_ucs_to_ryb_fast(p->anchor_hue) * 360.f) % 360;
    guide.custom_n    = 0;
  }

  dt_lib_histogram_set_scope(darktable.lib, 0); // 0 = DT_LIB_HISTOGRAM_SCOPE_VECTORSCOPE
  dt_lib_histogram_set_type(darktable.lib, 2);  // 2 = DT_LIB_HISTOGRAM_VECTORSCOPE_RYB
  dt_lib_histogram_set_harmony(darktable.lib, &guide);
}

static void _anchor_hue_slider_changed(GtkWidget *widget, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_color_picker_reset(self, TRUE);
  dt_iop_colorharmonizer_params_t *p = self->params;
  p->anchor_hue = _ryb_to_ucs_fast(dt_bauhaus_slider_get(widget) / 360.0f);
  gui_changed(self, widget, NULL);
  dt_dev_add_history_item(self->dev, self, TRUE);
}

// Apply a vectorscope harmony guide (rule + rotation) to params and refresh the GUI.
// Precondition: guide->type != DT_COLOR_HARMONY_NONE.
static void _apply_harmony_guide(dt_iop_module_t *self,
                                  dt_iop_colorharmonizer_params_t *p,
                                  dt_iop_colorharmonizer_gui_data_t *g,
                                  const dt_color_harmony_guide_t *guide)
{
  p->rule      = (dt_iop_colorharmonizer_rule_t)(guide->type - 1);
  p->anchor_hue = _ryb_to_ucs_fast(guide->rotation / 360.0f);

  ++darktable.gui->reset;
  dt_bauhaus_combobox_set(g->rule, p->rule);
  dt_bauhaus_slider_set(g->anchor_hue, (float)guide->rotation);
  --darktable.gui->reset;

  gui_changed(self, NULL, NULL);
  dt_dev_add_history_item(self->dev, self, TRUE);
}

static void _sync_custom_sliders(dt_iop_colorharmonizer_gui_data_t *g,
                                  dt_iop_colorharmonizer_params_t *p);

static void _on_vectorscope_harmony_changed(const dt_color_harmony_guide_t *guide, void *user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(!self || !self->gui_data) return;
  if(!self->enabled) return;

  if(guide->type == DT_COLOR_HARMONY_NONE)
  {
    // Custom harmony: the vectorscope rotated custom_angles in place (e.g. via scroll);
    // sync the updated node positions back into params and refresh the UI.
    if(guide->custom_n <= 0) return;
    dt_iop_colorharmonizer_params_t *p = self->params;
    dt_iop_colorharmonizer_gui_data_t *g = self->gui_data;
    const int n = MIN(guide->custom_n, p->num_custom_nodes);
    for(int i = 0; i < n; i++)
      p->custom_hue[i] = _ryb_to_ucs_fast(guide->custom_angles[i]);
    _sync_custom_sliders(g, p);
    for(int i = 0; i < COLORHARMONIZER_MAX_NODES; i++)
    {
      gtk_widget_queue_draw(g->custom_swatch[i]);
      gtk_widget_queue_draw(g->node_swatch[i]);
    }
    _paint_all_sat_sliders(g, p);
    dt_dev_add_history_item(self->dev, self, TRUE);
    return;
  }

  // _push_to_vectorscope() is called inside gui_changed() when sync is enabled, but since
  // we fire the callback only from user interaction paths (not from dt_vec_set_harmony),
  // the resulting push-back will not cause another callback invocation.
  _apply_harmony_guide(self, self->params, self->gui_data, guide);
}

static void _sync_to_vectorscope_toggled(GtkToggleButton *button, dt_iop_module_t *self)
{
  dt_iop_colorharmonizer_gui_data_t *g = self->gui_data;
  const gboolean active = gtk_toggle_button_get_active(button);
  dt_conf_set_bool("plugins/darkroom/colorharmonizer/sync_to_vectorscope", active);

  if(active)
  {
    _push_to_vectorscope(self);
    dt_lib_histogram_set_harmony_callback(darktable.lib, _on_vectorscope_harmony_changed, self);
  }
  else
  {
    dt_lib_histogram_set_harmony_callback(darktable.lib, NULL, NULL);
    // Custom guides only live in memory — clear them from the vectorscope when
    // sync is disabled so they don't linger as stale overlays.
    dt_iop_colorharmonizer_params_t *p = self->params;
    if(p->rule == DT_COLORHARMONIZER_CUSTOM)
    {
      dt_color_harmony_guide_t guide;
      dt_lib_histogram_get_harmony(darktable.lib, &guide);
      guide.custom_n = 0;
      guide.type     = DT_COLOR_HARMONY_NONE;
      dt_lib_histogram_set_harmony(darktable.lib, &guide);
    }
  }

  if(g && g->set_from_vectorscope)
    gtk_widget_set_sensitive(g->set_from_vectorscope, !active);
}

static gboolean _swatch_draw_callback(GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
  dt_iop_module_t *self = user_data;
  dt_iop_colorharmonizer_params_t *p = self->params;
  const int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), "swatch-index"));

  float hue = 0.0f;
  gboolean show_color = TRUE;

  if(p->rule == DT_COLORHARMONIZER_CUSTOM)
  {
    hue = p->custom_hue[idx];
  }
  else
  {
    float nodes[COLORHARMONIZER_MAX_NODES];
    int num_nodes = 0;
    get_harmony_nodes(p->rule, p->anchor_hue, NULL, COLORHARMONIZER_MAX_NODES, nodes, &num_nodes);
    if(idx < num_nodes)
      hue = nodes[idx];
    else
      show_color = FALSE; // unused slot: draw neutral grey
  }

  if(!show_color)
  {
    cairo_set_source_rgb(cr, 0.25, 0.25, 0.25);
    cairo_paint(cr);
    return TRUE;
  }

  float r, g, b;
  _hue_to_srgb(hue, &r, &g, &b);
  cairo_set_source_rgb(cr, r, g, b);
  cairo_paint(cr);
  return TRUE;
}

static void _custom_hue_changed(GtkWidget *widget, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;

  // Deactivate any active color picker when the slider is moved directly.
  dt_iop_color_picker_reset(self, TRUE);

  dt_iop_colorharmonizer_params_t *p = self->params;
  dt_iop_colorharmonizer_gui_data_t *g = self->gui_data;
  const int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), "slider-index"));

  p->custom_hue[idx] = _ryb_to_ucs_fast(dt_bauhaus_slider_get(widget) / 360.0f);

  gtk_widget_queue_draw(g->custom_swatch[idx]);
  gtk_widget_queue_draw(g->node_swatch[idx]);
  _paint_all_sat_sliders(g, p);
  dt_dev_add_history_item(self->dev, self, TRUE);

  if(g->sync_to_vectorscope
     && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g->sync_to_vectorscope)))
    _push_to_vectorscope(self);
}

static void _node_saturation_changed(GtkWidget *widget, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_colorharmonizer_params_t *p = self->params;
  const int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), "slider-index"));
  p->node_saturation[idx] = dt_bauhaus_slider_get(widget);
  dt_dev_add_history_item(self->dev, self, TRUE);
}

// Initialize custom nodes when switching from a predefined rule to custom mode.
// Reads the current vectorscope guide's sector positions and converts them to UCS.
static void _init_custom_nodes_from_rule(dt_iop_module_t *self,
                                          dt_iop_colorharmonizer_gui_data_t *g,
                                          dt_iop_colorharmonizer_params_t *p,
                                          dt_iop_colorharmonizer_rule_t old_rule)
{
  dt_color_harmony_guide_t guide;
  dt_lib_histogram_get_harmony(darktable.lib, &guide);
  float node_angles[COLORHARMONIZER_MAX_NODES];
  int num_nodes = 0;
  dt_lib_histogram_get_sector_angles(darktable.lib, guide.type, guide.rotation, node_angles, &num_nodes);

  if(num_nodes < 1)
  {
    // Fallback: guide not yet available (sync off); derive from rule geometry in UCS.
    get_harmony_nodes(old_rule, p->anchor_hue, NULL, 0, node_angles, &num_nodes);
    for(int i = 0; i < num_nodes; i++) p->custom_hue[i] = node_angles[i];
  }
  else
  {
    for(int i = 0; i < num_nodes && i < COLORHARMONIZER_MAX_NODES; i++)
      p->custom_hue[i] = _ryb_to_ucs_fast(node_angles[i]);
  }

  p->num_custom_nodes = CLAMP(num_nodes, 2, COLORHARMONIZER_MAX_NODES);
  if(num_nodes < 2) p->custom_hue[1] = p->custom_hue[0]; // monochromatic fallback

  ++darktable.gui->reset;
  dt_bauhaus_slider_set(g->num_custom_nodes_slider, p->num_custom_nodes);
  --darktable.gui->reset;
}

// Update widget visibility based on current mode (custom vs. predefined) and node count.
static void _update_visibility(dt_iop_colorharmonizer_gui_data_t *g,
                               dt_iop_colorharmonizer_params_t *p,
                               gboolean is_custom)
{
  gtk_widget_set_visible(g->anchor_hue, !is_custom);
  gtk_widget_set_visible(g->swatches_area, !is_custom);
  gtk_widget_set_visible(g->num_custom_nodes_slider, is_custom);

  int num_nodes = 0;

  if(is_custom)
  {
    num_nodes = CLAMP(p->num_custom_nodes, 2, COLORHARMONIZER_MAX_NODES);
    for(int i = 0; i < COLORHARMONIZER_MAX_NODES; i++)
      gtk_widget_set_visible(g->custom_row[i], i < num_nodes);
  }
  else
  {
    for(int i = 0; i < COLORHARMONIZER_MAX_NODES; i++)
      gtk_widget_set_visible(g->custom_row[i], FALSE);

    float nodes[COLORHARMONIZER_MAX_NODES];
    get_harmony_nodes(p->rule, p->anchor_hue, NULL, COLORHARMONIZER_MAX_NODES, nodes, &num_nodes);
    for(int i = 0; i < COLORHARMONIZER_MAX_NODES; i++)
      gtk_widget_set_visible(g->node_swatch[i], i < num_nodes);
  }

  for(int i = 0; i < COLORHARMONIZER_MAX_NODES; i++)
    gtk_widget_set_visible(g->sat_row[i], i < num_nodes);
}

// Sync custom hue slider display values from params.
static void _sync_custom_sliders(dt_iop_colorharmonizer_gui_data_t *g,
                                  dt_iop_colorharmonizer_params_t *p)
{
  ++darktable.gui->reset;
  for(int i = 0; i < COLORHARMONIZER_MAX_NODES; i++)
    dt_bauhaus_slider_set(g->custom_hue_slider[i], _ucs_to_ryb_fast(p->custom_hue[i]) * 360.0f);
  --darktable.gui->reset;
}

// Push the current harmony to the vectorscope if sync is enabled and the changed
// widget is one that affects the harmony guide (rule, hue sliders, or full refresh).
static void _sync_vectorscope_if_enabled(dt_iop_module_t *self,
                                          dt_iop_colorharmonizer_gui_data_t *g,
                                          GtkWidget *widget)
{
  if(!g->sync_to_vectorscope
     || !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g->sync_to_vectorscope)))
    return;

  if(self->enabled)
  {
    gboolean is_hue_widget = (widget == g->anchor_hue);
    if(!is_hue_widget)
      for(int i = 0; i < COLORHARMONIZER_MAX_NODES; i++)
        if(widget == g->custom_hue_slider[i]) { is_hue_widget = TRUE; break; }

    if(!widget || widget == g->rule || is_hue_widget || widget == g->num_custom_nodes_slider)
    {
      if(!widget) dt_iop_request_focus(self);
      _push_to_vectorscope(self);
    }
  }
}

void gui_changed(dt_iop_module_t *self, GtkWidget *widget, void *previous)
{
  dt_iop_colorharmonizer_gui_data_t *g = self->gui_data;
  if(!g) return;

  dt_iop_colorharmonizer_params_t *p = self->params;
  const gboolean is_custom = (p->rule == DT_COLORHARMONIZER_CUSTOM);

  // When switching to custom mode, seed the nodes from the current vectorscope guide.
  if(widget == g->rule && is_custom && previous)
  {
    const dt_iop_colorharmonizer_rule_t old_rule = *(dt_iop_colorharmonizer_rule_t *)previous;
    if(old_rule != DT_COLORHARMONIZER_CUSTOM)
      _init_custom_nodes_from_rule(self, g, p, old_rule);
  }

  _update_visibility(g, p, is_custom);

  for(int i = 0; i < COLORHARMONIZER_MAX_NODES; i++)
  {
    gtk_widget_queue_draw(g->node_swatch[i]);
    gtk_widget_queue_draw(g->custom_swatch[i]);
  }

  if(is_custom)
    _sync_custom_sliders(g, p);

  _paint_all_hue_sliders(g);
  _paint_all_sat_sliders(g, p);
  _sync_vectorscope_if_enabled(self, g, widget);
}

void gui_update(dt_iop_module_t *self)
{
  dt_iop_colorharmonizer_gui_data_t *g = self->gui_data;
  dt_iop_colorharmonizer_params_t *p = self->params;

  // _from_params sliders/combos (rule, pull_strength, pull_width, neutral_protection,
  // smoothing, num_custom_nodes) auto-sync before gui_update() is called; only custom
  // widgets that need coordinate conversion or aren't _from_params need manual sync.
  ++darktable.gui->reset;
  dt_bauhaus_slider_set(g->anchor_hue, _ucs_to_ryb_fast(p->anchor_hue) * 360.0f);
  for(int i = 0; i < COLORHARMONIZER_MAX_NODES; i++)
  {
    dt_bauhaus_slider_set(g->custom_hue_slider[i], _ucs_to_ryb_fast(p->custom_hue[i]) * 360.0f);
    dt_bauhaus_slider_set(g->sat_slider[i], p->node_saturation[i]);
  }
  --darktable.gui->reset;

  dt_gui_update_collapsible_section(&g->sat_section);
  gui_changed(self, NULL, NULL);
  dt_iop_color_picker_reset(self, TRUE);
}

// Convert a picked pixel color (pipeline RGB) to a normalized darktable UCS hue [0, 1).
// Returns FALSE only if the work profile is unavailable.
static gboolean _picked_color_to_hue(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, float *out_hue)
{
  const dt_iop_order_iccprofile_info_t *const work_profile = pipe->work_profile_info;
  if(!work_profile) return FALSE;

  dt_aligned_pixel_t px_rgb, px_xyz;
  for(int c = 0; c < 3; c++) px_rgb[c] = fmaxf(self->picked_color[c], 0.0f);
  px_rgb[3] = 0.0f;

  const float (*const matrix_in)[4] = (const float (*)[4])work_profile->matrix_in;
  px_xyz[0] = matrix_in[0][0]*px_rgb[0] + matrix_in[0][1]*px_rgb[1] + matrix_in[0][2]*px_rgb[2];
  px_xyz[1] = matrix_in[1][0]*px_rgb[0] + matrix_in[1][1]*px_rgb[1] + matrix_in[1][2]*px_rgb[2];
  px_xyz[2] = matrix_in[2][0]*px_rgb[0] + matrix_in[2][1]*px_rgb[1] + matrix_in[2][2]*px_rgb[2];
  px_xyz[3] = 0.0f;

  const float L_white = Y_to_dt_UCS_L_star(1.0f);
  dt_aligned_pixel_t px_xyz_d65, px_xyY, px_JCH;
  XYZ_D50_to_D65(px_xyz, px_xyz_d65);
  dt_D65_XYZ_to_xyY(px_xyz_d65, px_xyY);
  xyY_to_dt_UCS_JCH(px_xyY, L_white, px_JCH);

  *out_hue = (px_JCH[2] + M_PI_F) / (2.f * M_PI_F);
  return TRUE;
}

void color_picker_apply(dt_iop_module_t *self, GtkWidget *picker, dt_dev_pixelpipe_t *pipe)
{
  dt_iop_colorharmonizer_gui_data_t *g = self->gui_data;
  dt_iop_colorharmonizer_params_t *p = self->params;

  float hue = 0.0f;
  if(!_picked_color_to_hue(self, pipe, &hue)) return;

  if(picker == g->anchor_hue)
  {
    p->anchor_hue = hue;
    ++darktable.gui->reset;
    dt_bauhaus_slider_set(g->anchor_hue, _ucs_to_ryb_fast(hue) * 360.0f);
    --darktable.gui->reset;

    for(int i = 0; i < COLORHARMONIZER_MAX_NODES; i++)
    {
      gtk_widget_queue_draw(g->custom_swatch[i]);
      gtk_widget_queue_draw(g->node_swatch[i]);
    }

    dt_dev_add_history_item(self->dev, self, TRUE);
    gui_changed(self, g->anchor_hue, NULL);
    return;
  }

  for(int i = 0; i < COLORHARMONIZER_MAX_NODES; i++)
  {
    if(picker == g->custom_hue_slider[i])
    {
      p->custom_hue[i] = hue;
      ++darktable.gui->reset;
      dt_bauhaus_slider_set(g->custom_hue_slider[i], _ucs_to_ryb_fast(hue) * 360.0f);
      --darktable.gui->reset;
      gtk_widget_queue_draw(g->custom_swatch[i]);
      dt_dev_add_history_item(self->dev, self, TRUE);
      gui_changed(self, g->custom_hue_slider[i], NULL);
      return;
    }
  }
}

// Score how well a rule+anchor covers the image's hue histogram.
// Returns a coverage fraction in [0,1]: the fraction of chromatic energy that
// falls within the Gaussian attraction zones of the harmony nodes.
// Uses the same sigma formula as the main algorithm (pull_width_factor = 1.0).
static float _score_harmony(const float *histo, int num_bins,
                              dt_iop_colorharmonizer_rule_t rule, float anchor_hue)
{
  float nodes[COLORHARMONIZER_MAX_NODES];
  int num_nodes = 1;
  get_harmony_nodes(rule, anchor_hue, NULL, COLORHARMONIZER_MAX_NODES, nodes, &num_nodes);

  if(num_nodes <= 0) return 0.0f;
  const float sigma = 0.5f / (float)num_nodes;
  const float inv_2sigma2 = 1.0f / (2.0f * sigma * sigma);

  float total   = 0.0f;
  float covered = 0.0f;

  for(int b = 0; b < num_bins; b++)
  {
    if(histo[b] <= 0.0f) continue;
    const float h = (b + 0.5f) / (float)num_bins;

    float max_w = 0.0f;
    for(int i = 0; i < num_nodes; i++)
    {
      float d = fabsf(h - nodes[i]);
      if(d > 0.5f) d = 1.0f - d;
      const float w = expf(-d * d * inv_2sigma2);
      if(w > max_w) max_w = w;
    }

    covered += histo[b] * max_w;
    total   += histo[b];
  }

  return (total > 1e-6f) ? (covered / total) : 0.0f;
}

// Analyse a chroma-weighted hue histogram and return the harmony rule and
// anchor hue that best explain the existing color distribution (i.e. the
// combination that already covers the most chromatic energy).
static void _auto_detect_harmony(const float *histo, int num_bins,
                                  dt_iop_colorharmonizer_rule_t *best_rule,
                                  float *best_anchor)
{
  // Smooth the histogram with three passes of a circular box filter to
  // suppress noise from individual pixels before scoring.
  float smooth[COLORHARMONIZER_HUE_BINS];
  memcpy(smooth, histo, num_bins * sizeof(float));
  for(int pass = 0; pass < 3; pass++)
  {
    float tmp[COLORHARMONIZER_HUE_BINS];
    for(int b = 0; b < num_bins; b++)
    {
      const int prev = (b - 1 + num_bins) % num_bins;
      const int next = (b + 1) % num_bins;
      tmp[b] = (smooth[prev] + smooth[b] + smooth[next]) * (1.0f / 3.0f);
    }
    memcpy(smooth, tmp, num_bins * sizeof(float));
  }

  float best_score = -1.0f;
  *best_rule   = DT_COLORHARMONIZER_COMPLEMENTARY;
  *best_anchor = 0.0f;

  // Only test predefined (non-custom) rules
  const int num_rules = DT_COLORHARMONIZER_SQUARE + 1;
  const int num_steps = 360; // 1° resolution — 9 × 360 = 3240 combinations, still fast

  for(int r = 0; r < num_rules; r++)
  {
    for(int a = 0; a < num_steps; a++)
    {
      const float anchor = (float)a / (float)num_steps;
      const float score  = _score_harmony(smooth, num_bins,
                                          (dt_iop_colorharmonizer_rule_t)r, anchor);
      if(score > best_score)
      {
        best_score   = score;
        *best_rule   = (dt_iop_colorharmonizer_rule_t)r;
        *best_anchor = anchor;
      }
    }
  }
}

static void _auto_detect_callback(GtkButton *button, dt_iop_module_t *self)
{
  dt_iop_colorharmonizer_gui_data_t *g = self->gui_data;
  dt_iop_colorharmonizer_params_t   *p = self->params;

  g_mutex_lock(&g->histogram_lock);
  if(!g->histogram_valid)
  {
    g_mutex_unlock(&g->histogram_lock);
    dt_control_log(_("no histogram available yet — wait for the preview to finish processing"));
    return;
  }
  float histo[COLORHARMONIZER_HUE_BINS];
  memcpy(histo, g->hue_histogram, sizeof(histo));
  g_mutex_unlock(&g->histogram_lock);

  dt_iop_colorharmonizer_rule_t best_rule;
  float best_anchor;
  _auto_detect_harmony(histo, COLORHARMONIZER_HUE_BINS, &best_rule, &best_anchor);

  p->rule       = best_rule;
  p->anchor_hue = best_anchor;

  ++darktable.gui->reset;
  dt_bauhaus_combobox_set(g->rule, p->rule);
  dt_bauhaus_slider_set(g->anchor_hue, _ucs_to_ryb_fast(p->anchor_hue) * 360.0f);
  --darktable.gui->reset;

  gui_changed(self, NULL, NULL);
  dt_dev_add_history_item(self->dev, self, TRUE);

  if(g->sync_to_vectorscope
     && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g->sync_to_vectorscope)))
    _push_to_vectorscope(self);
}

static void _set_from_vectorscope_callback(GtkButton *button, dt_iop_module_t *self)
{
  dt_color_harmony_guide_t guide;

  dt_lib_histogram_get_harmony(darktable.lib, &guide);

  if(guide.type != DT_COLOR_HARMONY_NONE)
    _apply_harmony_guide(self, self->params, self->gui_data, &guide);

  // select vectorscope in histogram view
  dt_lib_histogram_set_scope(darktable.lib, 0); // 0 = DT_LIB_HISTOGRAM_SCOPE_VECTORSCOPE
}

// Create a colored hue swatch (24×24 drawing area) with the standard draw callback.
static GtkWidget *_create_swatch(int index, dt_iop_module_t *self)
{
  GtkWidget *swatch = gtk_drawing_area_new();
  gtk_widget_set_size_request(swatch, DT_PIXEL_APPLY_DPI(24), DT_PIXEL_APPLY_DPI(24));
  g_object_set_data(G_OBJECT(swatch), "swatch-index", GINT_TO_POINTER(index));
  g_signal_connect(G_OBJECT(swatch), "draw", G_CALLBACK(_swatch_draw_callback), self);
  return swatch;
}

void gui_init(dt_iop_module_t *self)
{
  dt_iop_colorharmonizer_gui_data_t *g = IOP_GUI_ALLOC(colorharmonizer);
  GtkWidget *main_box = self->widget = dt_gui_vbox();

  GtkWidget *rule_row = dt_gui_hbox();
  dt_gui_box_add(self->widget, rule_row);
  self->widget = rule_row;
  g->rule = dt_bauhaus_combobox_from_params(self, "rule");
  gtk_widget_set_hexpand(g->rule, TRUE);
  self->widget = main_box;

  g->auto_detect = dtgtk_button_new(dtgtk_cairo_paint_camera, CPF_NONE, NULL);
  gtk_widget_set_tooltip_text(g->auto_detect,
    _("analyze the image's hue distribution and automatically suggest the harmony rule\n"
      "and anchor hue that best match its existing color palette.\n"
      "\n"
      "the detection scores every rule and anchor combination against a chroma-weighted\n"
      "histogram of the preview image, then selects the combination that already covers\n"
      "the most chromatic energy — i.e. requires the least correction.\n"
      "\n"
      "the result replaces the current rule and anchor hue. use pull strength to control\n"
      "how strongly the remaining off-palette colors are pulled toward the detected palette."));
  g_signal_connect(G_OBJECT(g->auto_detect), "clicked", G_CALLBACK(_auto_detect_callback), self);
  dt_gui_box_add(rule_row, g->auto_detect);

  gtk_widget_set_tooltip_text(g->rule,
    _("harmony rule that defines which hues are considered 'in harmony'.\n"
      "\n"
      "monochromatic: a single hue family — only one node.\n"
      "analogous: three adjacent hues spaced 30° apart — naturalistic, cohesive.\n"
      "analogous complementary: analogous triad plus its complement — rich but balanced.\n"
      "complementary: two hues opposite on the wheel — high contrast, vibrant.\n"
      "split complementary: one hue and the two hues flanking its complement — vivid yet less stark.\n"
      "dyad: two hues separated by 60° — gentle contrast.\n"
      "triad: three hues evenly spaced 120° apart — balanced, colorful.\n"
      "tetrad: four hues in two complementary pairs spaced 60° — complex, needs restraint.\n"
      "square: four hues evenly spaced 90° apart — strong and varied.\n"
      "custom: define all four harmony node hues independently."));

  {
    GtkWidget *slider = dt_bauhaus_slider_new_with_range(self, 0.0f, 360.0f, 0.5f, 0.0f, 1);
    dt_bauhaus_widget_set_label(slider, NULL, N_("anchor hue"));
    dt_bauhaus_slider_set_feedback(slider, 0);
    dt_bauhaus_slider_set_format(slider, "°");
    dt_bauhaus_slider_set_digits(slider, 1);
    g_signal_connect(G_OBJECT(slider), "value-changed",
                     G_CALLBACK(_anchor_hue_slider_changed), self);
    g->anchor_hue = dt_color_picker_new(self, DT_COLOR_PICKER_POINT_AREA, slider);
    dt_bauhaus_widget_set_quad_tooltip(g->anchor_hue,
      _("pick hue from image.\n"
        "ctrl+click to select an area"));
    gtk_widget_set_tooltip_text(g->anchor_hue,
      _("the primary 'key' hue of the harmony — the first node from which all others are derived.\n"
        "\n"
        "drag the slider or use the color picker (eyedropper) to sample a dominant hue directly\n"
        "from the image. the remaining harmony nodes are computed automatically from this hue\n"
        "and the selected rule.\n"
        "\n"
        "tip: pick a skin tone, a sky, or another dominant subject color to anchor the palette."));
    dt_gui_box_add(self->widget, g->anchor_hue);
  }

  // Node count slider — shown in custom mode only; placed before swatches/rows so it
  // controls how many are visible.
  g->num_custom_nodes_slider = dt_bauhaus_slider_from_params(self, "num_custom_nodes");
  dt_bauhaus_slider_set_digits(g->num_custom_nodes_slider, 0);
  gtk_widget_set_tooltip_text(g->num_custom_nodes_slider,
    _("number of active harmony nodes in custom mode (2–4).\n"
      "nodes are numbered from the top; inactive nodes are hidden."));

  // Horizontal swatches area — shown in non-custom mode (read-only node colors)
  g->swatches_area = dt_gui_hbox();
  for(int i = 0; i < COLORHARMONIZER_MAX_NODES; i++)
  {
    g->node_swatch[i] = _create_swatch(i, self);
    gtk_widget_set_hexpand(g->node_swatch[i], TRUE);
    dt_gui_box_add(g->swatches_area, g->node_swatch[i]);
  }
  dt_gui_box_add(self->widget, g->swatches_area);

  // Vertical swatch rows — one per harmony node (max 4), shown in custom mode only
  for(int i = 0; i < COLORHARMONIZER_MAX_NODES; i++)
  {
    GtkWidget *row = dt_gui_hbox();

    // Color swatch (small square showing the node's hue)
    g->custom_swatch[i] = _create_swatch(i, self);
    dt_gui_box_add(row, g->custom_swatch[i]);

    // Hue slider with color picker
    GtkWidget *slider = dt_bauhaus_slider_new_with_range_and_feedback(
        self, 0.0f, 360.0f, 0.5f, 0.0f, 1, 0);
    dt_bauhaus_slider_set_format(slider, "°");
    g_object_set_data(G_OBJECT(slider), "slider-index", GINT_TO_POINTER(i));
    g_signal_connect(G_OBJECT(slider), "value-changed",
                     G_CALLBACK(_custom_hue_changed), self);
    gtk_widget_set_tooltip_text(slider,
      _("hue of this harmony node (0°–360°).\n"
        "only active in 'custom' harmony mode.\n"
        "use the color picker to sample the desired hue from the image."));

    // Wrap slider with color picker quad button
    g->custom_hue_slider[i] = dt_color_picker_new(self, DT_COLOR_PICKER_POINT_AREA, slider);
    dt_bauhaus_widget_set_quad_tooltip(g->custom_hue_slider[i],
      _("pick hue from image.\n"
        "ctrl+click to select an area"));

    gtk_widget_set_hexpand(g->custom_hue_slider[i], TRUE);
    dt_gui_box_add(row, g->custom_hue_slider[i]);

    g->custom_row[i] = row;
    dt_gui_box_add(self->widget, row);
  }

  GtkWidget *sync_row = dt_gui_hbox();

  g->sync_to_vectorscope = gtk_check_button_new_with_label(_("vectorscope two-way sync"));
  gtk_widget_set_tooltip_text(g->sync_to_vectorscope,
    _("when enabled, the vectorscope harmony overlay is kept in sync with\n"
      "the harmony rule and anchor hue controls in the module.\n"
      "disable to adjust the module without disturbing the vectorscope display, and vice-versa."));

  const char *conf_key = "plugins/darkroom/colorharmonizer/sync_to_vectorscope";
  if(!dt_conf_key_exists(conf_key))
    dt_conf_set_bool(conf_key, TRUE);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->sync_to_vectorscope), dt_conf_get_bool(conf_key));

  g_signal_connect(G_OBJECT(g->sync_to_vectorscope), "toggled",
                   G_CALLBACK(_sync_to_vectorscope_toggled), self);

  g->set_from_vectorscope = dtgtk_button_new(dtgtk_cairo_paint_refresh, CPF_NONE, NULL);
  gtk_widget_set_tooltip_text(g->set_from_vectorscope,
    _("import the harmony rule and anchor hue currently displayed in the vectorscope.\n"
      "also switches the histogram panel to the vectorscope view if it is not already active."));
  g_signal_connect(G_OBJECT(g->set_from_vectorscope), "clicked",
                   G_CALLBACK(_set_from_vectorscope_callback), self);

  gtk_widget_set_hexpand(g->sync_to_vectorscope, TRUE);
  dt_gui_box_add(sync_row, g->sync_to_vectorscope);
  dt_gui_box_add(sync_row, g->set_from_vectorscope);
  dt_gui_box_add(self->widget, sync_row);


  // "set from vectorscope" is redundant when sync is active; disable it initially
  // (sync checkbox starts checked, so the button starts insensitive)
  gtk_widget_set_sensitive(g->set_from_vectorscope,
      !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g->sync_to_vectorscope)));

  g->histogram_valid = FALSE;
  g_mutex_init(&g->histogram_lock);

  g->pull_strength = dt_bauhaus_slider_from_params(self, "pull_strength");
  gtk_widget_set_tooltip_text(g->pull_strength,
    _("how strongly off-harmony hues are pulled toward the nearest harmony node.\n"
      "\n"
      "0: no effect — colors are left unchanged.\n"
      "low values (0.1–0.3): subtle nudge, colors lean toward the palette without\n"
      "  losing their original character.\n"
      "high values (0.7–1.0): aggressive correction, most hues converge noticeably\n"
      "  onto the harmony nodes.\n"
      "\n"
      "the shift is weighted by each pixel's chroma: fully desaturated pixels (grays)\n"
      "are never affected regardless of this setting."));

  g->pull_width = dt_bauhaus_slider_from_params(self, "pull_width");
  dt_bauhaus_slider_set_digits(g->pull_width, 2);
  gtk_widget_set_tooltip_text(g->pull_width,
    _("controls the angular reach of each harmony node's attraction.\n"
      "\n"
      "the attraction is Gaussian — strongest at the node center and tapering smoothly\n"
      "outward. zone width scales the standard deviation of this Gaussian linearly.\n"
      "\n"
      "narrow (< 1): only hues very close to a node are pulled — selective, precise.\n"
      "  useful when colors are already near-harmonic and need only a gentle correction.\n"
      "default (1): each node's influence tapers to near-zero at the midpoint between\n"
      "  adjacent nodes — clean separation with smooth transitions.\n"
      "wide (> 1): attraction zones overlap — broader, more global hue shift.\n"
      "  useful for strongly discordant images or when a painterly look is desired."));

  g->neutral_protection = dt_bauhaus_slider_from_params(self, "neutral_protection");
  dt_bauhaus_slider_set_digits(g->neutral_protection, 2);
  gtk_widget_set_tooltip_text(g->neutral_protection,
    _("shields low-chroma (desaturated) colors from the hue correction.\n"
      "\n"
      "0: no protection — grays, skin highlights, and muted tones are all shifted.\n"
      "low values (0.1–0.3): only near-neutral grays are exempted; pastels are still affected.\n"
      "mid values (0.4–0.6): muted and pastel colors are increasingly spared.\n"
      "high values (0.7–1.0): only vivid, saturated colors are corrected; everything else\n"
      "  is left close to its original hue.\n"
      "\n"
      "the weighting is smooth and hyperbolic — there is no hard cutoff. protection grows\n"
      "gradually from zero and the slider response is distributed evenly across its range."));

  g->smoothing = dt_bauhaus_slider_from_params(self, "smoothing");
  dt_bauhaus_slider_set_digits(g->smoothing, 2);
  gtk_widget_set_tooltip_text(g->smoothing,
    _("controls the intensity of smoothing applied to the correction field.\n"
      "\n"
      "0: disable smoothing (recommended) — transitions are handled by smooth interpolation\n"
      "  in hue space, preserving the maximum amount of image detail.\n"
      "low values (0.1–0.5): subtle spatial averaging to reduce color noise in shadows.\n"
      "high values (> 1.0): broad spatial blending; creates a soft, painterly look but may\n"
      "  blur the grading across image edges, causing some visual separation."));

  dt_gui_new_collapsible_section(&g->sat_section,
                                 "plugins/darkroom/colorharmonizer/expand_saturation",
                                 _("saturation"),
                                 GTK_BOX(main_box),
                                 DT_ACTION(self));
  self->widget = GTK_WIDGET(g->sat_section.container);

  static const char *sat_labels[] = { N_("hue 1"), N_("hue 2"), N_("hue 3"), N_("hue 4") };
  for(int i = 0; i < COLORHARMONIZER_MAX_NODES; i++)
  {
    GtkWidget *slider = dt_bauhaus_slider_new_with_range(self, 0.0f, 2.0f, 0.01f, 1.0f, 2);
    dt_bauhaus_widget_set_label(slider, NULL, _(sat_labels[i]));
    dt_bauhaus_slider_set_factor(slider, 100.f);
    dt_bauhaus_slider_set_format(slider, "%");
    dt_bauhaus_slider_set_digits(slider, 0);
    g_object_set_data(G_OBJECT(slider), "slider-index", GINT_TO_POINTER(i));
    g_signal_connect(G_OBJECT(slider), "value-changed",
                     G_CALLBACK(_node_saturation_changed), self);
    gtk_widget_set_tooltip_text(slider,
      _("saturation multiplier for colors near this harmony node.\n"
        "100% = unchanged. below 100% desaturates; above 100% boosts saturation.\n"
        "the effect is weighted by the pixel's proximity to this node\n"
        "and respects the 'neutral hue protection' setting."));
    g->sat_slider[i] = slider;
    g->sat_row[i] = slider;
    dt_gui_box_add(self->widget, slider);
  }

  self->widget = main_box;
}

void gui_focus(dt_iop_module_t *self, gboolean in)
{
  dt_iop_colorharmonizer_gui_data_t *g = self->gui_data;
  if(!g) return;
  const gboolean sync = g->sync_to_vectorscope
      && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g->sync_to_vectorscope));
  if(in)
  {
    if(sync && self->enabled)
      _push_to_vectorscope(self);
    // Ensure the callback is registered on first focus-in.
    // darktable.lib is not yet initialized when gui_init runs, so we defer
    // the initial registration until the module is actually shown.
    dt_lib_histogram_set_harmony_callback(darktable.lib,
        sync ? _on_vectorscope_harmony_changed : NULL,
        sync ? self : NULL);
  }
  else
  {
    // Unregister while not focused so other modules can register their own callback.
    dt_lib_histogram_set_harmony_callback(darktable.lib, NULL, NULL);
  }
}

void gui_cleanup(dt_iop_module_t *self)
{
  dt_iop_colorharmonizer_gui_data_t *g = self->gui_data;
  if(g)
  {
    dt_lib_histogram_set_harmony_callback(darktable.lib, NULL, NULL);
    g_mutex_clear(&g->histogram_lock);
  }
  dt_iop_default_cleanup(self);
}
