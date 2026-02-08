/*
    This file is part of darktable,
    Copyright (C) 2010-2026 darktable developers.

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

#include "bauhaus/bauhaus.h"
#include "common/colorspaces.h"
#include "common/darktable.h"
#include "common/interpolation.h"
#include "common/opencl.h"
#include "common/image_cache.h"
#include "common/math.h"
#include "common/imagebuf.h"
#include "common/gaussian.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "develop/imageop_gui.h"
#include "develop/masks.h"
#include "develop/blend.h"
#include "common/colorspaces_inline_conversions.h"
#include "imageio/imageio_rawspeed.h" // for dt_rawspeed_crop_dcraw_filters
#include "develop/tiling.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"

#include <complex.h>
#include <glib.h>
#include <memory.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

DT_MODULE_INTROSPECTION(6, dt_iop_demosaic_params_t)

#define DT_DEMOSAIC_XTRANS 1024 // masks for non-Bayer demosaic ops
#define DT_DEMOSAIC_DUAL 2048   // masks for dual demosaicing methods
#define DT_REDUCESIZE_MIN 64

// these are highly depending on CPU architecture (cache size)
#define DT_RCD_TILESIZE 112
#define DT_LMMSE_TILESIZE 136

typedef enum dt_iop_demosaic_method_t
{
  // methods for Bayer images
  DT_IOP_DEMOSAIC_PPG = 0,   // $DESCRIPTION: "PPG"
  DT_IOP_DEMOSAIC_AMAZE = 1, // $DESCRIPTION: "AMaZE"
  DT_IOP_DEMOSAIC_VNG4 = 2,  // $DESCRIPTION: "VNG4"
  DT_IOP_DEMOSAIC_RCD = 5,   // $DESCRIPTION: "RCD"
  DT_IOP_DEMOSAIC_LMMSE = 6, // $DESCRIPTION: "LMMSE"
  DT_IOP_DEMOSAIC_RCD_DUAL = DT_DEMOSAIC_DUAL | DT_IOP_DEMOSAIC_RCD, // $DESCRIPTION: "RCD (dual)"
  DT_IOP_DEMOSAIC_AMAZE_DUAL = DT_DEMOSAIC_DUAL | DT_IOP_DEMOSAIC_AMAZE, // $DESCRIPTION: "AMaZE (dual)""
  DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME = 3, // $DESCRIPTION: "passthrough (monochrome)"
  DT_IOP_DEMOSAIC_PASSTHROUGH_COLOR = 4, // $DESCRIPTION: "photosite color (debug)"
  // methods for x-trans images
  DT_IOP_DEMOSAIC_VNG = DT_DEMOSAIC_XTRANS | 0,           // $DESCRIPTION: "VNG"
  DT_IOP_DEMOSAIC_MARKESTEIJN = DT_DEMOSAIC_XTRANS | 1,   // $DESCRIPTION: "Markesteijn 1-pass"
  DT_IOP_DEMOSAIC_MARKESTEIJN_3 = DT_DEMOSAIC_XTRANS | 2, // $DESCRIPTION: "Markesteijn 3-pass"
  DT_IOP_DEMOSAIC_FDC = DT_DEMOSAIC_XTRANS | 4,           // $DESCRIPTION: "frequency domain chroma"
  DT_IOP_DEMOSAIC_MARKEST3_DUAL = DT_DEMOSAIC_DUAL | DT_IOP_DEMOSAIC_MARKESTEIJN_3, // $DESCRIPTION: "Markesteijn 3-pass (dual)"
  DT_IOP_DEMOSAIC_PASSTHR_MONOX = DT_DEMOSAIC_XTRANS | 3, // $DESCRIPTION: "passthrough (monochrome)"
  DT_IOP_DEMOSAIC_PASSTHR_COLORX = DT_DEMOSAIC_XTRANS | 5, // $DESCRIPTION: "photosite color (debug)"
  // dummy for true monochromes
  DT_IOP_DEMOSAIC_MONO = 7,                                // $DESCRIPTION: "Monochrome"
} dt_iop_demosaic_method_t;

static const char *_method_str(const int method)
{
  switch(method)
  {
    case DT_IOP_DEMOSAIC_PPG:
      return "PPG";
    case DT_IOP_DEMOSAIC_AMAZE:
      return "AMAZE";
    case DT_IOP_DEMOSAIC_VNG4:
      return "VNG4";
    case DT_IOP_DEMOSAIC_RCD:
      return "RCD";
    case DT_IOP_DEMOSAIC_LMMSE:
      return "LMMSE";
    case DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME:
      return "PASSTHROUGH_MONOCHROME";
    case DT_IOP_DEMOSAIC_PASSTHROUGH_COLOR:
      return "PASSTHROUGH_COLOR";
    case DT_IOP_DEMOSAIC_VNG:
      return "VNG";
    case DT_IOP_DEMOSAIC_MARKESTEIJN:
      return "MARKESTEIJN";
    case DT_IOP_DEMOSAIC_MARKESTEIJN_3:
      return "MARKESTEIJN_3";
    case DT_IOP_DEMOSAIC_FDC:
      return "FDC";
    case DT_IOP_DEMOSAIC_PASSTHR_MONOX:
      return "PASSTHROUGH_MONOCHROME";
    case DT_IOP_DEMOSAIC_PASSTHR_COLORX:
      return "PASSTHROUGH_COLOR";
    case DT_IOP_DEMOSAIC_MONO:
      return "DT_IOP_DEMOSAIC_MONO";
    default:
      return "UNKNOWN";
  }
}

typedef enum dt_iop_demosaic_greeneq_t
{
  DT_IOP_GREEN_EQ_NO = 0,    // $DESCRIPTION: "disabled"
  DT_IOP_GREEN_EQ_LOCAL = 1, // $DESCRIPTION: "local average"
  DT_IOP_GREEN_EQ_FULL = 2,  // $DESCRIPTION: "full average"
  DT_IOP_GREEN_EQ_BOTH = 3   // $DESCRIPTION: "full and local average"
} dt_iop_demosaic_greeneq_t;

typedef enum dt_iop_demosaic_smooth_t
{
  DT_DEMOSAIC_SMOOTH_OFF = 0, // $DESCRIPTION: "disabled"
  DT_DEMOSAIC_SMOOTH_1 = 1,   // $DESCRIPTION: "once"
  DT_DEMOSAIC_SMOOTH_2 = 2,   // $DESCRIPTION: "twice"
  DT_DEMOSAIC_SMOOTH_3 = 3,   // $DESCRIPTION: "three times"
  DT_DEMOSAIC_SMOOTH_4 = 4,   // $DESCRIPTION: "four times"
  DT_DEMOSAIC_SMOOTH_5 = 5,   // $DESCRIPTION: "five times"
} dt_iop_demosaic_smooth_t;

typedef enum dt_iop_demosaic_lmmse_t
{
  DT_LMMSE_REFINE_0 = 0,   // $DESCRIPTION: "basic"
  DT_LMMSE_REFINE_1 = 1,   // $DESCRIPTION: "median"
  DT_LMMSE_REFINE_2 = 2,   // $DESCRIPTION: "3x median"
  DT_LMMSE_REFINE_3 = 3,   // $DESCRIPTION: "refine & medians"
  DT_LMMSE_REFINE_4 = 4,   // $DESCRIPTION: "2x refine + medians"
} dt_iop_demosaic_lmmse_t;

typedef struct dt_iop_demosaic_params_t
{
  dt_iop_demosaic_greeneq_t green_eq;           // $DEFAULT: DT_IOP_GREEN_EQ_NO $DESCRIPTION: "match greens"
  float median_thrs;                            // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "edge threshold"
  dt_iop_demosaic_smooth_t color_smoothing;     // $DEFAULT: DT_DEMOSAIC_SMOOTH_OFF $DESCRIPTION: "color smoothing"
  dt_iop_demosaic_method_t demosaicing_method;  // $DEFAULT: DT_IOP_DEMOSAIC_RCD $DESCRIPTION: "method"
  dt_iop_demosaic_lmmse_t lmmse_refine;         // $DEFAULT: DT_LMMSE_REFINE_1 $DESCRIPTION: "LMMSE refine"
  float dual_thrs;                              // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.2 $DESCRIPTION: "dual threshold"
  float cs_radius;                              // $MIN: 0.0 $MAX: 1.5 $DEFAULT: 0.0 $DESCRIPTION: "radius"
  float cs_thrs;                                // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.40 $DESCRIPTION: "contrast sensitivity"
  float cs_boost;                               // $MIN: 0.0 $MAX: 1.5 $DEFAULT: 0.0 $DESCRIPTION: "corner boost"
  int cs_iter;                                  // $MIN: 1 $MAX: 25 $DEFAULT: 8 $DESCRIPTION: "iterations"
  float cs_center;                              // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "sharp center"
  gboolean cs_enabled;                          // $DEFAULT: FALSE $DESCRIPTION: "capture sharpen"
} dt_iop_demosaic_params_t;

typedef struct dt_iop_demosaic_gui_data_t
{
  GtkWidget *median_thrs;
  GtkWidget *greeneq;
  GtkWidget *color_smoothing;
  GtkWidget *demosaic_method_bayer;
  GtkWidget *demosaic_method_xtrans;
  GtkWidget *demosaic_method_bayerfour;
  GtkWidget *demosaic_method_mono;
  GtkWidget *dual_thrs;
  GtkWidget *lmmse_refine;
  GtkWidget *cs_thrs;
  GtkWidget *cs_radius;
  GtkWidget *cs_boost;
  GtkWidget *cs_iter;
  GtkWidget *cs_center;
  GtkWidget *cs_enabled;
  dt_gui_collapsible_section_t capture;
  gboolean cs_mask;
  gboolean dual_mask;
  gboolean cs_boost_mask;
  gboolean autoradius;
  gboolean autothrs;
  float new_radius;
  float new_thrs;
} dt_iop_demosaic_gui_data_t;

typedef struct dt_iop_demosaic_global_data_t
{
  // demosaic pattern
  int kernel_green_eq_lavg;
  int kernel_green_eq_favg_reduce_first;
  int kernel_green_eq_favg_reduce_second;
  int kernel_green_eq_favg_apply;
  int kernel_pre_median;
  int kernel_passthrough_monochrome;
  int kernel_passthrough_color;
  int kernel_ppg_green;
  int kernel_ppg_redblue;
  int kernel_zoom_half_size;
  int kernel_border_interpolate;
  int kernel_color_smoothing;
  int kernel_zoom_passthrough_monochrome;
  int kernel_vng_lin_interpolate;
  int kernel_zoom_third_size;
  int kernel_vng_interpolate;
  int kernel_markesteijn_initial_copy;
  int kernel_markesteijn_green_minmax;
  int kernel_markesteijn_interpolate_green;
  int kernel_markesteijn_solitary_green;
  int kernel_markesteijn_recalculate_green;
  int kernel_markesteijn_red_and_blue;
  int kernel_markesteijn_interpolate_twoxtwo;
  int kernel_markesteijn_convert_yuv;
  int kernel_markesteijn_differentiate;
  int kernel_markesteijn_homo_threshold;
  int kernel_markesteijn_homo_set;
  int kernel_markesteijn_homo_sum;
  int kernel_markesteijn_homo_max;
  int kernel_markesteijn_homo_max_corr;
  int kernel_markesteijn_homo_quench;
  int kernel_markesteijn_zero;
  int kernel_markesteijn_accu;
  int kernel_markesteijn_final;
  int kernel_rcd_populate;
  int kernel_rcd_write_output;
  int kernel_rcd_step_1_1;
  int kernel_rcd_step_1_2;
  int kernel_rcd_step_2_1;
  int kernel_rcd_step_3_1;
  int kernel_rcd_step_4_1;
  int kernel_rcd_step_4_2;
  int kernel_rcd_step_5_1;
  int kernel_rcd_step_5_2;
  int kernel_rcd_border_redblue;
  int kernel_rcd_border_green;
  int kernel_demosaic_box3;
  int kernel_write_blended_dual;
  int gaussian_9x9_mul;
  int gaussian_9x9_div;
  int prefill_clip_mask;
  int prepare_blend;
  int modify_blend;
  int show_blend_mask;
  int capture_result;
  int final_blend;
  float *gauss_coeffs;
} dt_iop_demosaic_global_data_t;

typedef struct dt_iop_demosaic_data_t
{
  dt_iop_demosaic_greeneq_t green_eq;
  dt_iop_demosaic_smooth_t color_smoothing;
  uint32_t demosaicing_method;
  dt_iop_demosaic_lmmse_t lmmse_refine;
  float median_thrs;
  double CAM_to_RGB[3][4];
  float dual_thrs;
  float cs_radius;
  float cs_thrs;
  float cs_boost;
  int cs_iter;
  float cs_center;
  gboolean cs_enabled;
} dt_iop_demosaic_data_t;

static gboolean _get_thumb_quality(const int width, const int height)
{
  // we check if we need ultra-high quality thumbnail for this size
  const dt_mipmap_size_t level = dt_mipmap_cache_get_matching_size(width, height);
  const char *min = dt_conf_get_string_const("plugins/lighttable/thumbnail_hq_min_level");
  const dt_mipmap_size_t min_s = dt_mipmap_cache_get_min_mip_from_pref(min);

  return (level >= min_s);
}

// can we avoid full demosaicing and use a fast interpolator instead?
static gboolean _demosaic_full(const dt_dev_pixelpipe_iop_t *const piece,
                               const dt_image_t *const img,
                               const dt_iop_roi_t *const roi_out)
{
  if((img->flags & DT_IMAGE_4BAYER)   // half_size_f doesn't support 4bayer images
      || dt_image_is_mono_sraw(img)
      || piece->pipe->want_detail_mask)
    return TRUE;

  if(piece->pipe->type & DT_DEV_PIXELPIPE_THUMBNAIL)
    return _get_thumb_quality(roi_out->width, roi_out->height);

  if(piece->pipe->type & DT_DEV_PIXELPIPE_PREVIEW)
    return roi_out->scale > (piece->pipe->dsc.filters == 9u ? 0.667f : 0.5f);

  return TRUE;
}

// Implemented in demosaicing/amaze.cc
void amaze_demosaic(const float *const in,
                    float *out,
                    const int width,
                    const int height,
                    const uint32_t filters,
                    const float procmin);

#include "iop/demosaicing/basics.c"
#include "iop/demosaicing/vng.c"
#include "iop/demosaicing/xtrans.c"
#include "iop/demosaicing/passthrough.c"
#include "iop/demosaicing/ppg.c"
#include "iop/demosaicing/rcd.c"
#include "iop/demosaicing/lmmse.c"
#include "iop/demosaicing/capture.c"
#include "iop/demosaicing/dual.c"

const char *name()
{
  return _("demosaic");
}

const char **description(dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("reconstruct full RGB pixels from a sensor color filter array reading"),
                                      _("mandatory"),
                                      _("linear, raw, scene-referred"),
                                      _("linear, raw"),
                                      _("linear, RGB, scene-referred"));
}

int default_group()
{
  return IOP_GROUP_BASIC | IOP_GROUP_TECHNICAL;
}

int flags()
{
  return IOP_FLAGS_ONE_INSTANCE | IOP_FLAGS_FENCE | IOP_FLAGS_WRITE_DETAILS;
}

dt_iop_colorspace_type_t default_colorspace(dt_iop_module_t *self,
                                            dt_dev_pixelpipe_t *pipe,
                                            dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RAW;
}

int legacy_params(dt_iop_module_t *self,
                  const void *const old_params,
                  const int old_version,
                  void **new_params,
                  int32_t *new_params_size,
                  int *new_version)
{
  typedef struct dt_iop_demosaic_params_v6_t
  {
    dt_iop_demosaic_greeneq_t green_eq;
    float median_thrs;
    dt_iop_demosaic_smooth_t color_smoothing;
    dt_iop_demosaic_method_t demosaicing_method;
    dt_iop_demosaic_lmmse_t lmmse_refine;
    float dual_thrs;
    float cs_radius;
    float cs_thrs;
    float cs_boost;
    int cs_iter;
    float cs_center;
    gboolean cs_enabled;
  } dt_iop_demosaic_params_v6_t;

  typedef struct dt_iop_demosaic_params_v5_t
  {
    dt_iop_demosaic_greeneq_t green_eq;
    float median_thrs;
    dt_iop_demosaic_smooth_t color_smoothing;
    dt_iop_demosaic_method_t demosaicing_method;
    dt_iop_demosaic_lmmse_t lmmse_refine;
    float dual_thrs;
    float cs_radius;
    float cs_thrs;
    float cs_boost;
    int cs_iter;
    float cs_center;
  } dt_iop_demosaic_params_v5_t;

  typedef struct dt_iop_demosaic_params_v4_t
  {
    dt_iop_demosaic_greeneq_t green_eq;
    float median_thrs;
    dt_iop_demosaic_smooth_t color_smoothing;
    dt_iop_demosaic_method_t demosaicing_method;
    dt_iop_demosaic_lmmse_t lmmse_refine;
    float dual_thrs;
  } dt_iop_demosaic_params_v4_t;

  if(old_version == 2)
  {
    typedef struct dt_iop_demosaic_params_v2_t
    {
      dt_iop_demosaic_greeneq_t green_eq;
      float median_thrs;
    } dt_iop_demosaic_params_v2_t;

    const dt_iop_demosaic_params_v2_t *o = (dt_iop_demosaic_params_v2_t *)old_params;
    dt_iop_demosaic_params_v4_t *n = malloc(sizeof(dt_iop_demosaic_params_v4_t));
    n->green_eq = o->green_eq;
    n->median_thrs = o->median_thrs;
    n->color_smoothing = DT_DEMOSAIC_SMOOTH_OFF;
    n->demosaicing_method = DT_IOP_DEMOSAIC_PPG;
    n->lmmse_refine = DT_LMMSE_REFINE_1;
    n->dual_thrs = 0.20f;

    *new_params = n;
    *new_params_size = sizeof(dt_iop_demosaic_params_v4_t);
    *new_version = 4;
    return 0;
  }

  if(old_version == 3)
  {
    typedef struct dt_iop_demosaic_params_v3_t
    {
      dt_iop_demosaic_greeneq_t green_eq;
      float median_thrs;
      uint32_t color_smoothing;
      dt_iop_demosaic_method_t demosaicing_method;
      dt_iop_demosaic_lmmse_t lmmse_refine;
    } dt_iop_demosaic_params_v3_t;

    const dt_iop_demosaic_params_v3_t *o = (dt_iop_demosaic_params_v3_t *)old_params;
    dt_iop_demosaic_params_v4_t *n = malloc(sizeof(dt_iop_demosaic_params_v4_t));
    memcpy(n, o, sizeof *o);
    n->dual_thrs = 0.20f;

    *new_params = n;
    *new_params_size = sizeof(dt_iop_demosaic_params_v4_t);
    *new_version = 4;
    return 0;
  }

  if(old_version == 4)
  {
    const dt_iop_demosaic_params_v4_t *o = (dt_iop_demosaic_params_v4_t *)old_params;
    dt_iop_demosaic_params_v5_t *n = malloc(sizeof(dt_iop_demosaic_params_v5_t));
    memcpy(n, o, sizeof *o);
    n->cs_radius = 0.0f;
    n->cs_thrs = 0.4f;
    n->cs_boost = 0.0f;
    n->cs_iter = 0;
    n->cs_center = 0.0f;

    *new_params = n;
    *new_params_size = sizeof(dt_iop_demosaic_params_v5_t);
    *new_version = 5;
    return 0;
  }

  if(old_version == 5)
  {
    const dt_iop_demosaic_params_v5_t *o = (dt_iop_demosaic_params_v5_t *)old_params;
    dt_iop_demosaic_params_v6_t *n = malloc(sizeof(dt_iop_demosaic_params_v6_t));
    memcpy(n, o, sizeof *o);

    n->cs_iter = o->cs_iter > 0 ? o->cs_iter : 8;
    n->cs_enabled = o->cs_iter > 0;

    *new_params = n;
    *new_params_size = sizeof(dt_iop_demosaic_params_v6_t);
    *new_version = 6;
   return 0;
  }

  return 1;
}

dt_iop_colorspace_type_t input_colorspace(dt_iop_module_t *self,
                                          dt_dev_pixelpipe_t *pipe,
                                          dt_dev_pixelpipe_iop_t *piece)
{
  return dt_image_is_mono_sraw(&self->dev->image_storage) ? IOP_CS_RGB : IOP_CS_RAW;
}

dt_iop_colorspace_type_t output_colorspace(dt_iop_module_t *self,
                                           dt_dev_pixelpipe_t *pipe,
                                           dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

void distort_mask(dt_iop_module_t *self,
                  dt_dev_pixelpipe_iop_t *piece,
                  const float *const in,
                  float *const out,
                  const dt_iop_roi_t *const roi_in,
                  const dt_iop_roi_t *const roi_out)
{
  if(roi_out->scale != roi_in->scale)
  {
    const dt_interpolation_t *itor = dt_interpolation_new(DT_INTERPOLATION_USERPREF_WARP);
    dt_interpolation_resample_roi_1c(itor, out, roi_out, in, roi_in);
  }
  else
    dt_iop_copy_image_roi(out, in, 1, roi_in, roi_out);
}

void modify_roi_out(dt_iop_module_t *self,
                    dt_dev_pixelpipe_iop_t *piece,
                    dt_iop_roi_t *roi_out,
                    const dt_iop_roi_t *const roi_in)
{
  *roi_out = *roi_in;
  roi_out->x = 0;
  roi_out->y = 0;
}

void modify_roi_in(dt_iop_module_t *self,
                   dt_dev_pixelpipe_iop_t *piece,
                   const dt_iop_roi_t *roi_out,
                   dt_iop_roi_t *roi_in)
{
  *roi_in = *roi_out;
  roi_in->x = MAX(0, roi_out->x / roi_out->scale);
  roi_in->y = MAX(0, roi_out->y / roi_out->scale);

  roi_in->width = MAX(8, roi_out->width / roi_out->scale);
  roi_in->height = MAX(8, roi_out->height / roi_out->scale);
  roi_in->scale = 1.0f;
}

static gboolean _tiling_requirements(dt_iop_module_t *self,
                                     dt_dev_pixelpipe_iop_t *const piece,
                                     const dt_iop_roi_t *const roi_in,
                                     const dt_iop_roi_t *const roi_out,
                                     const gboolean dual,
                                     const gboolean greens,
                                     const gboolean direct,
                                     const gboolean capture,
                                     const gboolean gpu,
                                     const dt_iop_demosaic_method_t method,
                                     int *overlap,
                                     int *valid_rows,
                                     int *tile_height,
                                     int *num_tiles)
{
  dt_dev_pixelpipe_t *const pipe = piece->pipe;
  const int devid = pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;

  const int64_t allmem = gpu ? dt_opencl_get_device_available(devid) : dt_get_available_mem();
  const int64_t psize = sizeof(float) * width * height;
  const int64_t opsize = sizeof(float) * roi_out->width * roi_out->height * 4;
  // in & out buffers
  int64_t avail = allmem - psize - opsize;
  // for temporary output if not direct
  if(!direct) avail -= psize * 4;
  // green equilibration
  if(greens) avail -= psize;

  int64_t untiled_avail = avail;
  if(dual) untiled_avail -= psize * 5;

  int perpix = 4;
  int border = 8;
  switch(method)
  {
    case DT_IOP_DEMOSAIC_MARKESTEIJN_3:
      perpix = 48;
      border = 18;
      break;
    case DT_IOP_DEMOSAIC_MARKESTEIJN:
      perpix = 25;
      border = 12;
      break;
    case DT_IOP_DEMOSAIC_FDC:
      perpix = 25;
      border = 12;
      break;
    case DT_IOP_DEMOSAIC_RCD:
      perpix = gpu ? 10 : 2;
      border = 10;
      break;
    case DT_IOP_DEMOSAIC_LMMSE:
      perpix = 2;
      border = 10;
      break;
    case DT_IOP_DEMOSAIC_PPG:
      perpix = 4;
      border = 8;
      break;
    case DT_IOP_DEMOSAIC_AMAZE:
      perpix = 2;
      border = 6;
      break;
    case DT_IOP_DEMOSAIC_VNG4:
      perpix = 4;
      border = 6;
      break;
    case DT_IOP_DEMOSAIC_VNG:
      perpix = 4;
      border = 6;
      break;
    case DT_IOP_DEMOSAIC_MONO:
      perpix = 1;
      border = 0;
      break;
    case DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME:
    case DT_IOP_DEMOSAIC_PASSTHROUGH_COLOR:
    case DT_IOP_DEMOSAIC_PASSTHR_MONOX:
    case DT_IOP_DEMOSAIC_PASSTHR_COLORX:
      perpix = 1;
      border = 0;
      break;

    default:
      perpix = 4;
      border = 8;
  }

  if(capture)
  {
    perpix = MAX(perpix, 6);
    border = MAX(border, 18);
  }

  const int per_row = perpix * sizeof(float) * width;
  const int max_rows = avail / per_row / (gpu ? 5 : 4);
  const int untiled_max_rows = untiled_avail / per_row;

  dt_print(DT_DEBUG_PIPE | DT_DEBUG_TILING | DT_DEBUG_VERBOSE,
            "%s requirements: allmem=%dMB avail=%dMB untiled_avail=%dMB perpix=%d max_row=%d tile_max_row=%d",
            gpu ? "OpenCL" : "CPU", (int)(allmem / DT_MEGA), (int)(untiled_avail / DT_MEGA), (int)(avail / DT_MEGA),
            perpix, untiled_max_rows, max_rows);

  if(untiled_max_rows >= height) return FALSE;

  const int vsnapper = piece->filters == 9 ? 6 : 2;
  const int t_height = vsnapper * (max_rows / vsnapper);
  const int v_height = t_height - 2*border;
  const int n_tiles = (height + v_height - 1) / v_height;

  *overlap = border;
  *valid_rows = v_height;
  *tile_height = t_height;
  *num_tiles = n_tiles;
  return TRUE;
}

void process(dt_iop_module_t *self,
             dt_dev_pixelpipe_iop_t *const piece,
             const void *const i,
             void *const o,
             const dt_iop_roi_t *const roi_in,
             const dt_iop_roi_t *const roi_out)
{
  const dt_image_t *img = &self->dev->image_storage;
  dt_dev_pixelpipe_t *const pipe = piece->pipe;

  dt_dev_clear_scharr_mask(pipe);

  const gboolean run_fast = pipe->type & (DT_DEV_PIXELPIPE_FAST | DT_DEV_PIXELPIPE_PREVIEW);
  const gboolean fullpipe = pipe->type & DT_DEV_PIXELPIPE_FULL;

  const uint8_t(*const xtrans)[6] = piece->xtrans;
  const dt_iop_demosaic_data_t *d = piece->data;
  const dt_iop_demosaic_gui_data_t *g = self->gui_data;
  const uint32_t filters = piece->filters;

  const gboolean fullscale = _demosaic_full(piece, img, roi_out);
  const gboolean is_xtrans = filters == 9u;
  const gboolean is_4bayer = img->flags & DT_IMAGE_4BAYER;
  const gboolean is_bayer = !is_4bayer && !is_xtrans && filters != 0;
  const gboolean true_monochrome = dt_image_is_mono_sraw(img);

  const int demosaicing_method = d->demosaicing_method;
  int method = demosaicing_method & ~DT_DEMOSAIC_DUAL;
  const int width = roi_in->width;
  const int height = roi_in->height;

  if((width < 16 || height < 16)
      && (method != DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME && method != DT_IOP_DEMOSAIC_PASSTHROUGH_COLOR))
    method = is_xtrans ? DT_IOP_DEMOSAIC_VNG : DT_IOP_DEMOSAIC_VNG4;

  gboolean show_dual = FALSE;
  gboolean show_capture = FALSE;
  gboolean show_sigma = FALSE;
  if(self->dev->gui_attached && fullpipe)
  {
    if(g->dual_mask)
    {
      show_dual = TRUE;
      pipe->mask_display = DT_DEV_PIXELPIPE_DISPLAY_MASK;
    }
    if(g->cs_mask && d->cs_enabled)
    {
      show_capture = TRUE;
      pipe->mask_display = DT_DEV_PIXELPIPE_DISPLAY_MASK;
    }
    if(g->cs_boost_mask && d->cs_enabled)
    {
      show_sigma = TRUE;
      pipe->mask_display = DT_DEV_PIXELPIPE_DISPLAY_MASK;
    }
  }

  float *in = (float *)i;
  if(!fullscale)
  {
    dt_print_pipe(DT_DEBUG_PIPE, "demosaic approx zoom", pipe, self, DT_DEVICE_CPU, roi_in, roi_out);
    if(method == DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME || method == DT_IOP_DEMOSAIC_PASSTHROUGH_COLOR)
      dt_iop_clip_and_zoom_demosaic_passthrough_monochrome_f((float *)o, in, roi_out, roi_in, roi_out->width, width);
    else if(is_xtrans)
      dt_iop_clip_and_zoom_demosaic_third_size_xtrans_f((float *)o, in, roi_out, roi_in, roi_out->width, width, xtrans);
    else
      dt_iop_clip_and_zoom_demosaic_half_size_f((float *)o, in, roi_out, roi_in, roi_out->width, width, filters);

    return;
  }

  const gboolean demosaic_mask = pipe->mask_display == DT_DEV_PIXELPIPE_DISPLAY_PASSTHRU;
  const gboolean no_masking = pipe->mask_display == DT_DEV_PIXELPIPE_DISPLAY_NONE;
  const gboolean dual = (demosaicing_method & DT_DEMOSAIC_DUAL) && !run_fast && !show_sigma && !show_capture && !demosaic_mask;
  const gboolean direct = roi_out->width == width && roi_out->height == height && feqf(roi_in->scale, roi_out->scale, 1e-8f);
  const gboolean passthru = method == DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME
                         || method == DT_IOP_DEMOSAIC_PASSTHROUGH_COLOR;
  const gboolean do_capture = !passthru &&  !is_4bayer && !show_dual && !run_fast && d->cs_enabled;
  const gboolean greens = is_bayer && d->green_eq != DT_IOP_GREEN_EQ_NO && no_masking && !run_fast && !true_monochrome;

  const float procmax = dt_iop_get_processed_maximum(piece);
  const float procmin = dt_iop_get_processed_minimum(piece);
  const int exif_iso = img->exif_iso;

  if(do_capture)
  {
    if(_noise_requested(self, piece))
      _capture_noise(self, piece);
    if(_radius_requested(self, piece))
      _capture_radius(self, piece, in, width, height, xtrans, filters);
  }

  int overlap = 0;
  int tile_height = height;
  int valid_rows = height;
  int num_tiles = 1;
  const gboolean tiling = _tiling_requirements(self, piece, roi_in, roi_out, dual, greens, direct, do_capture, FALSE, method,
                          &overlap, &valid_rows, &tile_height, &num_tiles);

  const gboolean bad_tiling = tiling && (valid_rows < 30);
  if(bad_tiling)
  {
    valid_rows = 30;
    tile_height = valid_rows + 2* overlap;
    num_tiles = (height + valid_rows - 1) / valid_rows;
    dt_print(DT_DEBUG_ALWAYS, "high demosaicing memory load");
  }

  if(tiling || (darktable.unmuted & DT_DEBUG_VERBOSE))
    dt_print(DT_DEBUG_PIPE, "CPU %s%s %s demosaic %s%s%s%s%s%s. tiles=%d tileheight=%d overlap=%d",
      tiling ? "tiled " : "",
      direct ? "direct" : "scaled",
      is_xtrans ? "xtrans" : is_bayer ? "bayer" : "bayer4",
      _method_str(method),
      dual ? "_DUAL" : "",
      do_capture ? " capture" : "",
      greens ? " greens" : "",
      no_masking ? "" : " showmask",
      bad_tiling ? ", high memory" : "",
      num_tiles, tile_height, overlap);

  float *out = direct ? (float *)o : dt_iop_image_alloc(width, height, 4);
  if(!out)
  {
    dt_print(DT_DEBUG_ALWAYS, "can't create output buffer for demosaic");
    dt_control_log(_("can't allocate demosaic buffer"));
    return;
  }

  float *green_in = NULL;
  if(greens)
  {
    const float threshold = 0.0001f * exif_iso;
    green_in = dt_iop_image_alloc(width, height, 1);
    if(green_in)
    {
      in = green_in;
      float *aux = NULL;
      switch(d->green_eq)
      {
        case DT_IOP_GREEN_EQ_FULL:
          green_equilibration_favg(in, (float *)i, width, height, filters);
          break;
        case DT_IOP_GREEN_EQ_LOCAL:
          green_equilibration_lavg(in, (float *)i, width, height, filters, threshold);
          break;
        case DT_IOP_GREEN_EQ_BOTH:
          aux = dt_iop_image_alloc(width, height, 1);
          if(aux)
          {
            green_equilibration_favg(aux, (float *)i, width, height, filters);
            green_equilibration_lavg(in, aux, width, height, filters, threshold);
            dt_free_align(aux);
          }
          break;
        default:
          break;
      }
    }
    else
    {
      dt_print(DT_DEBUG_ALWAYS, "can't create green buffer for demosaic");
      dt_control_log(_("can't equilibrate greens"));
    }
  }

  float *t_out = tiling ? dt_iop_image_alloc(width, tile_height, 4) : out;
  if(!t_out)
  {
    dt_free_align(green_in);
    if(!direct) dt_free_align(out);
    dt_print(DT_DEBUG_ALWAYS, "can't create output buffer for demosaic");
    dt_control_log(_("can't allocate demosaic buffer"));
    return;
  }

  const int ch = true_monochrome ? 4 : 1;

  for(int tile_nr = 0; tile_nr < num_tiles; tile_nr++)
  {
    const int group = tile_nr * valid_rows ;
    const int last_in = MIN(height, group + valid_rows + overlap);
    const int topline = group - overlap;
    const int first_in = MAX(0, topline);
    const int t_rows = tiling ? last_in - first_in : height;

    const int missing = topline < 0 ? -topline : 0;
    const int first_out = overlap - missing;
    const int out_height = t_rows - first_out;

    if(out_height > 0)
    {
      if(tiling)
        dt_print(DT_DEBUG_TILING, "tile=%.3d/%.3d, group=%.5d first=%.5d last=%.5d rows=%.4d",
               tile_nr, num_tiles, group, first_in, last_in, t_rows);

      float *t_in = in + width * first_in * ch;

      if(demosaic_mask)
        demosaic_box3(t_out, t_in, width, t_rows, filters, xtrans);
      else if(method == DT_IOP_DEMOSAIC_MONO)
        dt_iop_image_copy_by_size(t_out, t_in, width, t_rows, 4);
      else if(method == DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME)
        passthrough_monochrome(t_out, t_in, width, t_rows);
      else if(method == DT_IOP_DEMOSAIC_PASSTHROUGH_COLOR)
        passthrough_color(t_out, t_in, width, t_rows, filters, xtrans);
      else if(is_xtrans)
      {
        const int passes = method == DT_IOP_DEMOSAIC_MARKESTEIJN_3 ? 3 : 1;
        if(method == DT_IOP_DEMOSAIC_FDC)
          xtrans_fdc_interpolate(t_out, t_in, width, t_rows, xtrans, exif_iso);
        else if(method == DT_IOP_DEMOSAIC_MARKESTEIJN || method == DT_IOP_DEMOSAIC_MARKESTEIJN_3)
          xtrans_markesteijn_interpolate(t_out, t_in, width, t_rows, xtrans, passes);
        else
          vng_interpolate(t_out, t_in, width, t_rows, filters, xtrans, FALSE);
      }
      else
      {
        if(method == DT_IOP_DEMOSAIC_VNG4 || is_4bayer)
        {
          vng_interpolate(t_out, t_in, width, t_rows, filters, xtrans, FALSE);
          if(is_4bayer)
          {
            dt_colorspaces_cygm_to_rgb(t_out, width * t_rows, d->CAM_to_RGB);
            dt_colorspaces_cygm_to_rgb(pipe->dsc.processed_maximum, 1, d->CAM_to_RGB);
          }
        }
        else if(method == DT_IOP_DEMOSAIC_RCD)
          rcd_demosaic(t_out, t_in, width, t_rows, filters, procmax);
        else if(method == DT_IOP_DEMOSAIC_LMMSE)
          lmmse_demosaic(t_out, t_in, width, t_rows, filters, d->lmmse_refine, procmax);
        else if(method != DT_IOP_DEMOSAIC_AMAZE)
          demosaic_ppg(t_out, t_in, width, t_rows, filters, d->median_thrs);
        else
          amaze_demosaic(t_in, t_out, width, t_rows, filters, procmin);
      }

      if(do_capture)
        _capture_sharpen(self, piece, t_in, t_out, width, t_rows, roi_in->x, roi_in->y + first_in, show_capture, show_sigma, xtrans, filters);

      if(dual)
        dual_demosaic(piece, t_out, t_in, width, t_rows, filters, xtrans, show_dual, d->dual_thrs);

      if(tiling)
        dt_iop_image_copy(out + group * width * 4, t_out + first_out * width * 4, width * 4 * out_height);
    }
  }

  if(tiling) dt_free_align(t_out);
  dt_free_align(green_in);

  if(pipe->want_detail_mask)
    dt_dev_write_scharr_mask(piece, out, roi_in, TRUE);

  if(d->color_smoothing != DT_DEMOSAIC_SMOOTH_OFF && no_masking && !run_fast)
    color_smoothing(out, width, height, d->color_smoothing);

  if(!direct)
  {
    dt_iop_clip_and_zoom_roi((float *)o, out, roi_out, roi_in);
    dt_free_align(out);
  }
}

#ifdef HAVE_OPENCL
int process_cl(dt_iop_module_t *self,
               dt_dev_pixelpipe_iop_t *const piece,
               cl_mem dev_in,
               cl_mem dev_out,
               const dt_iop_roi_t *const roi_in,
               const dt_iop_roi_t *const roi_out)
{
  const dt_image_t *img = &self->dev->image_storage;
  dt_dev_pixelpipe_t *const pipe = piece->pipe;
  const gboolean run_fast = pipe->type & (DT_DEV_PIXELPIPE_FAST | DT_DEV_PIXELPIPE_PREVIEW);
  const gboolean fullpipe = pipe->type & DT_DEV_PIXELPIPE_FULL;
  const gboolean true_monochrome = dt_image_is_mono_sraw(img);

  uint8_t(*const xtrans)[6] = piece->xtrans;
  cl_mem dev_xtrans = NULL;

  const uint32_t filters = piece->filters;
  const gboolean fullscale = _demosaic_full(piece, img, roi_out);
  const gboolean is_xtrans = filters == 9u;
  const gboolean is_bayer = !is_xtrans && filters != 0 && !true_monochrome;

  dt_dev_clear_scharr_mask(pipe);

  const dt_iop_demosaic_data_t *d = piece->data;
  const dt_iop_demosaic_gui_data_t *g = self->gui_data;
  const dt_iop_demosaic_global_data_t *gd = self->global_data;

  const int demosaicing_method = d->demosaicing_method;
  int method = demosaicing_method & ~DT_DEMOSAIC_DUAL;

  // We do a PPG to RCD demosaicer fallback here as the used driver is known to fail.
  // Also could "return DT_OPENCL_DT_EXCEPTION" for a cpu fallback
  if(method == DT_IOP_DEMOSAIC_PPG
     && dt_opencl_exception(pipe->devid, DT_OPENCL_AMD_APP))
    method = DT_IOP_DEMOSAIC_RCD;

  const int iwidth = roi_in->width;
  const int iheight = roi_in->height;

  if((iwidth < 16 || iheight < 16)
    &&  (method != DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME
      && method != DT_IOP_DEMOSAIC_PASSTHROUGH_COLOR
      && method != DT_IOP_DEMOSAIC_MONO))
    method = is_xtrans ? DT_IOP_DEMOSAIC_VNG : DT_IOP_DEMOSAIC_VNG4;

  gboolean show_dual = FALSE;
  gboolean show_capture = FALSE;
  gboolean show_sigma = FALSE;
  if(self->dev->gui_attached && fullpipe)
  {
    if(g->dual_mask)
    {
      show_dual = TRUE;
      pipe->mask_display = DT_DEV_PIXELPIPE_DISPLAY_MASK;
    }
    if(g->cs_mask && d->cs_enabled)
    {
      show_capture = TRUE;
      pipe->mask_display = DT_DEV_PIXELPIPE_DISPLAY_MASK;
    }
    if(g->cs_boost_mask && d->cs_enabled)
    {
      show_sigma = TRUE;
      pipe->mask_display = DT_DEV_PIXELPIPE_DISPLAY_MASK;
    }
  }

  const int devid = pipe->devid;
  cl_int err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
  if(dev_in == NULL || dev_out == NULL)
  {
    dt_print(DT_DEBUG_PIPE, "demosaic in/output missing");
    return err;
  }

  if(is_xtrans)
  {
    dev_xtrans = dt_opencl_copy_host_to_device_constant(devid, sizeof(piece->xtrans), xtrans);
    if(!dev_xtrans) return err;
  }

  if(!fullscale)
  {
    dt_print_pipe(DT_DEBUG_PIPE, "demosaic approx zoom", pipe, self, devid, roi_in, roi_out);
    if(is_xtrans)
    {
      // sample third-size image
      err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_zoom_third_size, roi_out->width, roi_out->height,
          CLARG(dev_in), CLARG(dev_out), CLARG(roi_out->width), CLARG(roi_out->height),
          CLARG(iwidth), CLARG(iheight), CLARG(roi_out->scale), CLARG(dev_xtrans));
      dt_opencl_release_mem_object(dev_xtrans);
      return err;
    }
    else if(method == DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME)
      return dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_zoom_passthrough_monochrome, roi_out->width, roi_out->height,
          CLARG(dev_in), CLARG(dev_out), CLARG(roi_out->width), CLARG(roi_out->height),
          CLARG(iwidth), CLARG(iheight), CLARG(roi_out->scale));
    else // bayer
      return dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_zoom_half_size, roi_out->width, roi_out->height,
          CLARG(dev_in), CLARG(dev_out), CLARG(roi_out->width), CLARG(roi_out->height),
          CLARG(iwidth), CLARG(iheight), CLARG(roi_out->scale), CLARG(filters));
  }

  const gboolean direct = roi_out->width == iwidth && roi_out->height == iheight && feqf(roi_in->scale, roi_out->scale, 1e-8f);
  const gboolean no_masking = pipe->mask_display == DT_DEV_PIXELPIPE_DISPLAY_NONE;
  const gboolean demosaic_mask = pipe->mask_display == DT_DEV_PIXELPIPE_DISPLAY_PASSTHRU;
  const gboolean dual = (demosaicing_method & DT_DEMOSAIC_DUAL) && !run_fast && !show_sigma && !show_capture && !demosaic_mask;
  const gboolean passthru = method == DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME
                         || method == DT_IOP_DEMOSAIC_PASSTHROUGH_COLOR;
  const gboolean do_capture = !passthru && !run_fast && !show_dual && d->cs_enabled;
  const gboolean greens = is_bayer && d->green_eq != DT_IOP_GREEN_EQ_NO && no_masking && !run_fast && !true_monochrome;

  if(do_capture)
  {
    if(_noise_requested(self, piece))
      _capture_noise(self, piece);
    if(_radius_requested(self, piece))
      _capture_radius_cl(self, piece, dev_in, iwidth, iheight, xtrans, filters, true_monochrome);
  }

  gboolean tiling = FALSE;
  cl_mem out_image = NULL;
  cl_mem t_in = NULL;
  cl_mem t_out = NULL;
  cl_mem t_high = NULL;
  cl_mem t_low = NULL;
  cl_mem in_image = greens ? dt_opencl_alloc_device(devid, iwidth, iheight, sizeof(float)) : dev_in;
  if(!in_image)
  {
    dt_print(DT_DEBUG_PIPE | DT_DEBUG_OPENCL, "demosaic couldn't allocate greens buffer");
    err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
    goto finish;
  }

  if(greens)
  {
    err = green_equilibration_cl(self, piece, dev_in, in_image, iwidth, iheight, filters);
    if(err != CL_SUCCESS)
    {
      dt_print(DT_DEBUG_PIPE | DT_DEBUG_OPENCL, "demosaic couldn't equilibrate greens");
      goto finish;
    }
  }

  int overlap = 0;
  int tile_height = iheight;
  int valid_rows = iheight;
  int num_tiles = 1;
  tiling = _tiling_requirements(self, piece, roi_in, roi_out, dual, greens, direct, do_capture, TRUE, method,
                          &overlap, &valid_rows, &tile_height, &num_tiles);

  if(tiling || (darktable.unmuted & DT_DEBUG_VERBOSE))
  {
    dt_print(DT_DEBUG_PIPE | DT_DEBUG_OPENCL, "OpenCL:%d %s%s %s demosaic %s%s%s%s%s. tiles=%d tileheight=%d overlap=%d",
      devid,
      tiling ? "tiled " : "",
      direct ? "direct" : "scaled",
      is_xtrans ? "xtrans" : is_bayer ? "bayer" : "bayer4",
      _method_str(method),
      dual ? "_DUAL" : "",
      do_capture ? " capture" : "",
      greens ? " greens" : "",
      no_masking ? "" : " showmask",
      num_tiles, tile_height, overlap);
  }

  const gboolean overtiling = (iheight / MAX(1, valid_rows)) > 200;
  // In case of excessive overtiling we do an early fallback
  if(tiling && overtiling)
  {
    err = DT_OPENCL_PROCESS_CL;
    dt_print(DT_DEBUG_PIPE | DT_DEBUG_OPENCL, "OpenCL demosaic falling back to cpu because of overtiling, valid_rows=%d", valid_rows);
    goto finish;
  }

  const int in_channels = true_monochrome ? 4 : 1;
  out_image = direct ? dev_out : dt_opencl_alloc_device(devid, iwidth, iheight, sizeof(float) * 4);
  t_in = tiling  ? dt_opencl_alloc_device(devid, iwidth, tile_height, sizeof(float) * in_channels) : in_image;
  t_out = tiling ? dt_opencl_alloc_device(devid, iwidth, tile_height, sizeof(float) * 4) : out_image;
  t_high = dual  ? dt_opencl_alloc_device(devid, iwidth, tile_height, sizeof(float) * 4) : t_out;
  t_low = dual   ? dt_opencl_alloc_device(devid, iwidth, tile_height, sizeof(float) * 4) : NULL;

  err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
  if(!out_image || !t_in || !t_out || !t_high || (dual && !t_low))
  {
    dt_print(DT_DEBUG_PIPE | DT_DEBUG_OPENCL,
      "demosaic couldn't allocate internal tile buffers");
    goto finish;
  }

  for(int tile_nr = 0; tile_nr < num_tiles; tile_nr++)
  {
    const int group = tile_nr * valid_rows ;
    const int last_in = MIN(iheight, group + valid_rows + overlap);
    const int topline = group - overlap;
    const int first_in = MAX(0, topline);
    const int t_rows = tiling ? last_in - first_in : iheight;

    const int missing = topline < 0 ? -topline : 0;
    const int first_out = overlap - missing;
    const int out_height = t_rows - first_out;
    err = CL_SUCCESS;

    if(out_height > 0)
    {
      if(tiling)
      {
        dt_print(DT_DEBUG_TILING,
              "tile=%.3d/%.3d, group=%.5d first=%.5d last=%.5d rows=%.4d",
               tile_nr, num_tiles, group, first_in, last_in, t_rows);

        size_t insrc[]  = { 0, first_in, 0 };
        size_t tdest[]  = { 0, 0, 0 };
        size_t iarea[]  = { iwidth, t_rows, 1 };
        err = dt_opencl_enqueue_copy_image(devid, in_image, t_in, insrc, tdest, iarea);
        if(err != CL_SUCCESS) goto finish;
      }

      if(demosaic_mask)
        err = demosaic_box3_cl(self, piece, t_in, t_high, dev_xtrans, iwidth, t_rows, filters);
      else if(method == DT_IOP_DEMOSAIC_MONO)
      {
        size_t insrc[]  = { 0, 0, 0 };
        size_t tdest[]  = { 0, 0, 0 };
        size_t iarea[]  = { iwidth, t_rows, 1 };
        err = dt_opencl_enqueue_copy_image(devid, t_in, t_high, insrc, tdest, iarea);
      }
      else if(passthru || method == DT_IOP_DEMOSAIC_PPG)
        err = process_default_cl(self, piece, t_in, t_high, dev_xtrans, iwidth, t_rows, method, filters);
      else if(method == DT_IOP_DEMOSAIC_RCD)
        err = process_rcd_cl(self, piece, t_in, t_high, iwidth, t_rows, filters);
      else if(method == DT_IOP_DEMOSAIC_VNG4 || method == DT_IOP_DEMOSAIC_VNG)
        err = process_vng_cl(self, piece, t_in, t_high, dev_xtrans, xtrans, iwidth, t_rows, filters, 0, FALSE);
      else if(method == DT_IOP_DEMOSAIC_MARKESTEIJN || method == DT_IOP_DEMOSAIC_MARKESTEIJN_3)
        err = process_markesteijn_cl(self, piece, t_in, t_high, dev_xtrans, xtrans, iwidth, t_rows);

      if(err != CL_SUCCESS) goto finish;

      if(do_capture)
      {
        err = _capture_sharpen_cl(self, piece, t_in, t_high, dev_xtrans, iwidth, t_rows, roi_in->x, roi_in->y + first_in, show_capture, show_sigma, xtrans, filters);
        if(err != CL_SUCCESS) goto finish;
      }

      if(dual)
      {
        err = process_vng_cl(self, piece, t_in, t_low, dev_xtrans, xtrans, iwidth, t_rows, filters, 0, TRUE);
        if(err == CL_SUCCESS)
          err = color_smoothing_cl(self, piece, t_low, t_low, iwidth, t_rows, DT_DEMOSAIC_SMOOTH_2);
        if(err == CL_SUCCESS)
          err = dual_demosaic_cl(self, piece, t_high, t_low, t_out, iwidth, t_rows, show_dual);
        if(err != CL_SUCCESS) goto finish;
      }

      if(tiling)
      {
        size_t tsrc[]   = { 0, first_out, 0 };
        size_t odest[]  = { 0, group, 0 };
        size_t oarea[]  = { iwidth, out_height, 1 };
        err = dt_opencl_enqueue_copy_image(devid, t_out, out_image, tsrc, odest, oarea);
        if(err != CL_SUCCESS) goto finish;
      }
    }
  }

  if(greens)  // release early for less cl memory load
  {
    dt_opencl_release_mem_object(in_image);
    in_image = NULL;
  }

  if(tiling)
  {
    dt_opencl_release_mem_object(t_in);
    dt_opencl_release_mem_object(t_out);
    t_in = NULL;
    t_out = NULL;
  }

  if(dual)
  {
    dt_opencl_release_mem_object(t_high);
    dt_opencl_release_mem_object(t_low);
    t_high = NULL;
    t_low = NULL;
  }

  if(pipe->want_detail_mask)
  {
    err = dt_dev_write_scharr_mask_cl(piece, out_image, roi_in, TRUE);
    if(err != CL_SUCCESS) goto finish;
  }

  if(d->color_smoothing != DT_DEMOSAIC_SMOOTH_OFF && no_masking && !run_fast)
  {
    err = color_smoothing_cl(self, piece, out_image, out_image, iwidth, iheight, d->color_smoothing);
    if(err != CL_SUCCESS) goto finish;
  }

  if(!direct)
     err = dt_iop_clip_and_zoom_roi_cl(devid, dev_out, out_image, roi_out, roi_in);

finish:
  dt_opencl_release_mem_object(dev_xtrans);

  if(greens) dt_opencl_release_mem_object(in_image);
  if(!direct) dt_opencl_release_mem_object(out_image);
  if(tiling)
  {
    dt_opencl_release_mem_object(t_in);
    dt_opencl_release_mem_object(t_out);
  }
  if(dual)
  {
    dt_opencl_release_mem_object(t_high);
    dt_opencl_release_mem_object(t_low);
  }
  return err;
}
#endif
#undef MIN_TILE_ROWS


void init_global(dt_iop_module_so_t *self)
{
  const int program = 0; // from programs.conf
  dt_iop_demosaic_global_data_t *gd = malloc(sizeof(dt_iop_demosaic_global_data_t));
  self->data = gd;

  gd->kernel_zoom_half_size = dt_opencl_create_kernel(program, "clip_and_zoom_demosaic_half_size");
  gd->kernel_ppg_green = dt_opencl_create_kernel(program, "ppg_demosaic_green");
  gd->kernel_green_eq_lavg = dt_opencl_create_kernel(program, "green_equilibration_lavg");
  gd->kernel_green_eq_favg_reduce_first = dt_opencl_create_kernel(program, "green_equilibration_favg_reduce_first");
  gd->kernel_green_eq_favg_reduce_second = dt_opencl_create_kernel(program, "green_equilibration_favg_reduce_second");
  gd->kernel_green_eq_favg_apply = dt_opencl_create_kernel(program, "green_equilibration_favg_apply");
  gd->kernel_pre_median = dt_opencl_create_kernel(program, "pre_median");
  gd->kernel_ppg_redblue = dt_opencl_create_kernel(program, "ppg_demosaic_redblue");
  gd->kernel_border_interpolate = dt_opencl_create_kernel(program, "border_interpolate");
  gd->kernel_color_smoothing = dt_opencl_create_kernel(program, "color_smoothing");

  const int other = 14; // from programs.conf
  gd->kernel_passthrough_monochrome = dt_opencl_create_kernel(other, "passthrough_monochrome");
  gd->kernel_passthrough_color = dt_opencl_create_kernel(other, "passthrough_color");
  gd->kernel_zoom_passthrough_monochrome = dt_opencl_create_kernel(other, "clip_and_zoom_demosaic_passthrough_monochrome");

  const int vng = 15; // from programs.conf
  gd->kernel_vng_lin_interpolate = dt_opencl_create_kernel(vng, "vng_lin_interpolate");
  gd->kernel_zoom_third_size = dt_opencl_create_kernel(vng, "clip_and_zoom_demosaic_third_size_xtrans");
  gd->kernel_vng_interpolate = dt_opencl_create_kernel(vng, "vng_interpolate");

  const int markesteijn = 16; // from programs.conf
  gd->kernel_markesteijn_initial_copy = dt_opencl_create_kernel(markesteijn, "markesteijn_initial_copy");
  gd->kernel_markesteijn_green_minmax = dt_opencl_create_kernel(markesteijn, "markesteijn_green_minmax");
  gd->kernel_markesteijn_interpolate_green = dt_opencl_create_kernel(markesteijn, "markesteijn_interpolate_green");
  gd->kernel_markesteijn_solitary_green = dt_opencl_create_kernel(markesteijn, "markesteijn_solitary_green");
  gd->kernel_markesteijn_recalculate_green = dt_opencl_create_kernel(markesteijn, "markesteijn_recalculate_green");
  gd->kernel_markesteijn_red_and_blue = dt_opencl_create_kernel(markesteijn, "markesteijn_red_and_blue");
  gd->kernel_markesteijn_interpolate_twoxtwo = dt_opencl_create_kernel(markesteijn, "markesteijn_interpolate_twoxtwo");
  gd->kernel_markesteijn_convert_yuv = dt_opencl_create_kernel(markesteijn, "markesteijn_convert_yuv");
  gd->kernel_markesteijn_differentiate = dt_opencl_create_kernel(markesteijn, "markesteijn_differentiate");
  gd->kernel_markesteijn_homo_threshold = dt_opencl_create_kernel(markesteijn, "markesteijn_homo_threshold");
  gd->kernel_markesteijn_homo_set = dt_opencl_create_kernel(markesteijn, "markesteijn_homo_set");
  gd->kernel_markesteijn_homo_sum = dt_opencl_create_kernel(markesteijn, "markesteijn_homo_sum");
  gd->kernel_markesteijn_homo_max = dt_opencl_create_kernel(markesteijn, "markesteijn_homo_max");
  gd->kernel_markesteijn_homo_max_corr = dt_opencl_create_kernel(markesteijn, "markesteijn_homo_max_corr");
  gd->kernel_markesteijn_homo_quench = dt_opencl_create_kernel(markesteijn, "markesteijn_homo_quench");
  gd->kernel_markesteijn_zero = dt_opencl_create_kernel(markesteijn, "markesteijn_zero");
  gd->kernel_markesteijn_accu = dt_opencl_create_kernel(markesteijn, "markesteijn_accu");
  gd->kernel_markesteijn_final = dt_opencl_create_kernel(markesteijn, "markesteijn_final");

  const int rcd = 31; // from programs.conf
  gd->kernel_rcd_populate = dt_opencl_create_kernel(rcd, "rcd_populate");
  gd->kernel_rcd_write_output = dt_opencl_create_kernel(rcd, "rcd_write_output");
  gd->kernel_rcd_step_1_1 = dt_opencl_create_kernel(rcd, "rcd_step_1_1");
  gd->kernel_rcd_step_1_2 = dt_opencl_create_kernel(rcd, "rcd_step_1_2");
  gd->kernel_rcd_step_2_1 = dt_opencl_create_kernel(rcd, "rcd_step_2_1");
  gd->kernel_rcd_step_3_1 = dt_opencl_create_kernel(rcd, "rcd_step_3_1");
  gd->kernel_rcd_step_4_1 = dt_opencl_create_kernel(rcd, "rcd_step_4_1");
  gd->kernel_rcd_step_4_2 = dt_opencl_create_kernel(rcd, "rcd_step_4_2");
  gd->kernel_rcd_step_5_1 = dt_opencl_create_kernel(rcd, "rcd_step_5_1");
  gd->kernel_rcd_step_5_2 = dt_opencl_create_kernel(rcd, "rcd_step_5_2");
  gd->kernel_rcd_border_redblue = dt_opencl_create_kernel(rcd, "rcd_border_redblue");
  gd->kernel_rcd_border_green = dt_opencl_create_kernel(rcd, "rcd_border_green");
  gd->kernel_demosaic_box3 = dt_opencl_create_kernel(rcd, "demosaic_box3");
  gd->kernel_write_blended_dual  = dt_opencl_create_kernel(rcd, "write_blended_dual");

  const int capt = 38; // capture.cl, from programs.conf
  gd->gaussian_9x9_mul = dt_opencl_create_kernel(capt, "kernel_9x9_mul");
  gd->gaussian_9x9_div = dt_opencl_create_kernel(capt, "kernel_9x9_div");
  gd->prefill_clip_mask = dt_opencl_create_kernel(capt, "prefill_clip_mask");
  gd->prepare_blend = dt_opencl_create_kernel(capt, "prepare_blend");
  gd->modify_blend = dt_opencl_create_kernel(capt, "modify_blend");
  gd->show_blend_mask = dt_opencl_create_kernel(capt, "show_blend_mask");
  gd->capture_result = dt_opencl_create_kernel(capt, "capture_result");
  gd->final_blend = dt_opencl_create_kernel(capt, "final_blend");

  gd->gauss_coeffs = dt_alloc_align_float(CAPTURE_KERNEL_ALIGN * (UCHAR_MAX+1));
  for(int i = 0; i <= UCHAR_MAX; i++)
    _calc_9x9_gauss_coeffs(&gd->gauss_coeffs[i * CAPTURE_KERNEL_ALIGN], MAX(1e-7f, (float)i * CAPTURE_GAUSS_FRACTION));
}

void cleanup_global(dt_iop_module_so_t *self)
{
  dt_iop_demosaic_global_data_t *gd = self->data;
  dt_opencl_free_kernel(gd->kernel_zoom_half_size);
  dt_opencl_free_kernel(gd->kernel_ppg_green);
  dt_opencl_free_kernel(gd->kernel_pre_median);
  dt_opencl_free_kernel(gd->kernel_green_eq_lavg);
  dt_opencl_free_kernel(gd->kernel_green_eq_favg_reduce_first);
  dt_opencl_free_kernel(gd->kernel_green_eq_favg_reduce_second);
  dt_opencl_free_kernel(gd->kernel_green_eq_favg_apply);
  dt_opencl_free_kernel(gd->kernel_ppg_redblue);
  dt_opencl_free_kernel(gd->kernel_border_interpolate);
  dt_opencl_free_kernel(gd->kernel_color_smoothing);
  dt_opencl_free_kernel(gd->kernel_passthrough_monochrome);
  dt_opencl_free_kernel(gd->kernel_passthrough_color);
  dt_opencl_free_kernel(gd->kernel_zoom_passthrough_monochrome);
  dt_opencl_free_kernel(gd->kernel_vng_lin_interpolate);
  dt_opencl_free_kernel(gd->kernel_zoom_third_size);
  dt_opencl_free_kernel(gd->kernel_vng_interpolate);
  dt_opencl_free_kernel(gd->kernel_markesteijn_initial_copy);
  dt_opencl_free_kernel(gd->kernel_markesteijn_green_minmax);
  dt_opencl_free_kernel(gd->kernel_markesteijn_interpolate_green);
  dt_opencl_free_kernel(gd->kernel_markesteijn_solitary_green);
  dt_opencl_free_kernel(gd->kernel_markesteijn_recalculate_green);
  dt_opencl_free_kernel(gd->kernel_markesteijn_red_and_blue);
  dt_opencl_free_kernel(gd->kernel_markesteijn_interpolate_twoxtwo);
  dt_opencl_free_kernel(gd->kernel_markesteijn_convert_yuv);
  dt_opencl_free_kernel(gd->kernel_markesteijn_differentiate);
  dt_opencl_free_kernel(gd->kernel_markesteijn_homo_threshold);
  dt_opencl_free_kernel(gd->kernel_markesteijn_homo_set);
  dt_opencl_free_kernel(gd->kernel_markesteijn_homo_sum);
  dt_opencl_free_kernel(gd->kernel_markesteijn_homo_max);
  dt_opencl_free_kernel(gd->kernel_markesteijn_homo_max_corr);
  dt_opencl_free_kernel(gd->kernel_markesteijn_homo_quench);
  dt_opencl_free_kernel(gd->kernel_markesteijn_zero);
  dt_opencl_free_kernel(gd->kernel_markesteijn_accu);
  dt_opencl_free_kernel(gd->kernel_markesteijn_final);
  dt_opencl_free_kernel(gd->kernel_rcd_populate);
  dt_opencl_free_kernel(gd->kernel_rcd_write_output);
  dt_opencl_free_kernel(gd->kernel_rcd_step_1_1);
  dt_opencl_free_kernel(gd->kernel_rcd_step_1_2);
  dt_opencl_free_kernel(gd->kernel_rcd_step_2_1);
  dt_opencl_free_kernel(gd->kernel_rcd_step_3_1);
  dt_opencl_free_kernel(gd->kernel_rcd_step_4_1);
  dt_opencl_free_kernel(gd->kernel_rcd_step_4_2);
  dt_opencl_free_kernel(gd->kernel_rcd_step_5_1);
  dt_opencl_free_kernel(gd->kernel_rcd_step_5_2);
  dt_opencl_free_kernel(gd->kernel_rcd_border_redblue);
  dt_opencl_free_kernel(gd->kernel_rcd_border_green);
  dt_opencl_free_kernel(gd->kernel_demosaic_box3);
  dt_opencl_free_kernel(gd->kernel_write_blended_dual);
  dt_opencl_free_kernel(gd->gaussian_9x9_mul);
  dt_opencl_free_kernel(gd->gaussian_9x9_div);
  dt_opencl_free_kernel(gd->prefill_clip_mask);
  dt_opencl_free_kernel(gd->prepare_blend);
  dt_opencl_free_kernel(gd->modify_blend);
  dt_opencl_free_kernel(gd->show_blend_mask);
  dt_opencl_free_kernel(gd->capture_result);
  dt_opencl_free_kernel(gd->final_blend);
  dt_free_align(gd->gauss_coeffs);
  free(self->data);
  self->data = NULL;
  _cleanup_lmmse_gamma();
}

void commit_params(dt_iop_module_t *self,
                   dt_iop_params_t *params,
                   dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  const dt_iop_demosaic_params_t *const p = (dt_iop_demosaic_params_t *)params;
  dt_iop_demosaic_data_t *d = piece->data;
  const dt_image_t *img = &pipe->image;
  const gboolean true_monochrome = dt_image_is_mono_sraw(img);
  if(!(dt_image_is_raw(img) || true_monochrome))
    piece->enabled = FALSE;
  d->green_eq = p->green_eq;
  d->color_smoothing = p->color_smoothing;
  d->median_thrs = p->median_thrs;
  d->dual_thrs = p->dual_thrs;
  d->lmmse_refine = p->lmmse_refine;
  dt_iop_demosaic_method_t use_method = p->demosaicing_method;
  d->cs_radius = p->cs_radius;
  d->cs_thrs = p->cs_thrs;
  d->cs_boost = p->cs_boost;
  d->cs_center = p->cs_center;
  d->cs_enabled = p->cs_enabled;
  // magic function to have CS iterations with fine granularity for values <= 11 and then rising up to 50
  d->cs_iter = p->cs_iter + (int)(0.000065f * powf((float)p->cs_iter, 4.0f));
  const gboolean xmethod = use_method & DT_DEMOSAIC_XTRANS;
  const gboolean is_dual = use_method & DT_DEMOSAIC_DUAL;
  const gboolean bayer4  = self->dev->image_storage.flags & DT_IMAGE_4BAYER;
  const gboolean bayer   = self->dev->image_storage.buf_dsc.filters != 9u && !bayer4 && !true_monochrome;
  const gboolean xtrans  = self->dev->image_storage.buf_dsc.filters == 9u;
  const gboolean passing = use_method == DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME
                        || use_method == DT_IOP_DEMOSAIC_PASSTHROUGH_COLOR;

  // if there is a xtrans/bayer mismatch due to presets/historycopy we still want to keep dual mode
  if(bayer && xmethod)
    use_method = is_dual ? DT_IOP_DEMOSAIC_RCD_DUAL : DT_IOP_DEMOSAIC_RCD;
  if(xtrans && !xmethod)
    use_method = is_dual ? DT_IOP_DEMOSAIC_MARKEST3_DUAL : DT_IOP_DEMOSAIC_MARKESTEIJN;
  if(bayer4 && !passing)
    use_method = DT_IOP_DEMOSAIC_VNG4;
  if(true_monochrome)
    use_method = DT_IOP_DEMOSAIC_MONO;

  if(use_method == DT_IOP_DEMOSAIC_PASSTHR_MONOX)
    use_method = DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME;
  if(use_method == DT_IOP_DEMOSAIC_PASSTHR_COLORX)
    use_method = DT_IOP_DEMOSAIC_PASSTHROUGH_COLOR;

  if(use_method != DT_IOP_DEMOSAIC_PPG)
    d->median_thrs = 0.0f;

  if(passing || bayer4 || true_monochrome)
  {
    d->green_eq = DT_IOP_GREEN_EQ_NO;
    d->color_smoothing = DT_DEMOSAIC_SMOOTH_OFF;
  }

  if(use_method & DT_DEMOSAIC_DUAL)
    d->color_smoothing = DT_DEMOSAIC_SMOOTH_OFF;

  d->demosaicing_method = use_method;

  // as some demosaicers don't have OpenCL implementations
  switch(d->demosaicing_method)
  {
    case DT_IOP_DEMOSAIC_AMAZE:
      piece->process_cl_ready = FALSE;
      break;
    case DT_IOP_DEMOSAIC_LMMSE:
      piece->process_cl_ready = FALSE;
      break;
    case DT_IOP_DEMOSAIC_AMAZE_DUAL:
      piece->process_cl_ready = FALSE;
      break;
    case DT_IOP_DEMOSAIC_FDC:
      piece->process_cl_ready = FALSE;
      break;
    default:
      piece->process_cl_ready = TRUE;
  }

  if(bayer4)
  {
    // 4Bayer images not implemented in OpenCL yet
    piece->process_cl_ready = FALSE;

    // Get and store the matrix to go from camera to RGB for 4Bayer images
    if(!dt_colorspaces_conversion_matrices_rgb(self->dev->image_storage.adobe_XYZ_to_CAM,
                                               NULL, d->CAM_to_RGB,
                                               self->dev->image_storage.d65_color_matrix, NULL))
    {
      const char *camera = self->dev->image_storage.camera_makermodel;
      dt_print(DT_DEBUG_ALWAYS, "[colorspaces] `%s' color matrix not found for 4bayer image!", camera);
      dt_control_log(_("`%s' color matrix not found for 4bayer image!"), camera);
    }
  }
}

void init_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_demosaic_data_t));
}

void cleanup_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void reload_defaults(dt_iop_module_t *self)
{
  dt_iop_demosaic_params_t *d = self->default_params;
  dt_iop_demosaic_gui_data_t *g = self->gui_data;
  const dt_image_t *img = &self->dev->image_storage;

  if(dt_image_is_monochrome(img))
    d->demosaicing_method = dt_image_is_mono_sraw(img) ? DT_IOP_DEMOSAIC_MONO : DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME;
  else if(img->buf_dsc.filters == 9u)
    d->demosaicing_method = DT_IOP_DEMOSAIC_MARKESTEIJN;
  else if(img->flags & DT_IMAGE_4BAYER)
    d->demosaicing_method = DT_IOP_DEMOSAIC_VNG4;
  else
    d->demosaicing_method = DT_IOP_DEMOSAIC_RCD;

  d->cs_thrs = _get_variance_threshold(self);

  self->hide_enable_button = TRUE;

  self->default_enabled = dt_image_is_raw(img) || dt_image_is_mono_sraw(img);
  if(self->widget)
    gtk_stack_set_visible_child_name(GTK_STACK(self->widget), self->default_enabled ? "raw" : "non_raw");

  if(g)
  {
    g->autoradius = FALSE;
    g->autothrs = FALSE;
  }
}

void gui_changed(dt_iop_module_t *self, GtkWidget *w, void *previous)
{
  dt_iop_demosaic_gui_data_t *g = self->gui_data;
  dt_iop_demosaic_params_t *p = self->params;

  const dt_image_t *oimg = &self->dev->image_storage;
  const gboolean true_monochrome = dt_image_is_mono_sraw(oimg);
  const gboolean bayer4 = oimg->flags & DT_IMAGE_4BAYER;
  const gboolean bayer  = oimg->buf_dsc.filters != 9u && !bayer4 && !true_monochrome;
  const gboolean xtrans = oimg->buf_dsc.filters == 9u;

  dt_iop_demosaic_method_t use_method = p->demosaicing_method;
  const gboolean xmethod = use_method & DT_DEMOSAIC_XTRANS;
  const gboolean is_dual = use_method & DT_DEMOSAIC_DUAL;

  if(bayer && xmethod)
    use_method = is_dual ? DT_IOP_DEMOSAIC_RCD_DUAL : DT_IOP_DEMOSAIC_RCD;
  if(xtrans && !xmethod)
    use_method = is_dual ? DT_IOP_DEMOSAIC_MARKEST3_DUAL : DT_IOP_DEMOSAIC_MARKESTEIJN;
  if(true_monochrome)
    use_method = DT_IOP_DEMOSAIC_MONO;

  const gboolean bayerpassing =
      use_method == DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME
   || use_method == DT_IOP_DEMOSAIC_PASSTHROUGH_COLOR;

  if(bayer4 && !bayerpassing)
    use_method = DT_IOP_DEMOSAIC_VNG4;

  const gboolean isppg = use_method == DT_IOP_DEMOSAIC_PPG;
  const gboolean isdual = (use_method & DT_DEMOSAIC_DUAL) && !bayer4 && !true_monochrome;
  const gboolean islmmse = use_method == DT_IOP_DEMOSAIC_LMMSE;
  const gboolean passing = bayerpassing
    || use_method == DT_IOP_DEMOSAIC_PASSTHR_MONOX
    || use_method == DT_IOP_DEMOSAIC_PASSTHR_COLORX;

  const gboolean capture_support = !passing && !bayer4;
  const gboolean do_capture = capture_support && p->cs_enabled;

  gtk_widget_set_visible(g->demosaic_method_bayer, bayer);
  gtk_widget_set_visible(g->demosaic_method_bayerfour, bayer4);
  gtk_widget_set_visible(g->demosaic_method_xtrans, xtrans);
  gtk_widget_set_visible(g->demosaic_method_mono, true_monochrome);

  gtk_widget_set_visible(g->cs_radius, do_capture);
  gtk_widget_set_visible(g->cs_thrs, do_capture);
  gtk_widget_set_visible(g->cs_boost, do_capture);
  gtk_widget_set_visible(g->cs_center, do_capture && p->cs_boost);
  gtk_widget_set_visible(g->cs_iter, do_capture);

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->cs_enabled), p->cs_enabled);
  gtk_widget_set_visible(g->cs_enabled, capture_support);
  gtk_widget_set_visible(GTK_WIDGET(g->capture.expander), do_capture);

  // we might have a wrong method due to xtrans/bayer - mode mismatch
  const gboolean method_mismatch = use_method != p->demosaicing_method;
  if(method_mismatch)
  {
    if(bayer)
      dt_bauhaus_combobox_set_from_value(g->demosaic_method_bayer, use_method);
    else if(xtrans)
      dt_bauhaus_combobox_set_from_value(g->demosaic_method_xtrans, use_method);
    else if(bayer4)
      dt_bauhaus_combobox_set_from_value(g->demosaic_method_bayerfour, use_method);
    else
      dt_bauhaus_combobox_set_from_value(g->demosaic_method_mono, use_method);

    /*  If auto-applied from a preset/style from a different sensor type it's demosaicer
        method was added to the current sensor specific demosaicer combobox.
        As we know the bad method here, we can remove it from the combobox by testing for it's position.
    */
    GtkWidget *demosaicers =  bayer  ? g->demosaic_method_bayer :
                              xtrans ? g->demosaic_method_xtrans :
                              bayer4 ? g->demosaic_method_bayerfour : g->demosaic_method_mono;
    const int pos = dt_bauhaus_combobox_get_from_value(demosaicers, p->demosaicing_method);
    if(pos >= 0) dt_bauhaus_combobox_remove_at(demosaicers, pos);
  }

  p->demosaicing_method = use_method;

  gtk_widget_set_visible(g->median_thrs, bayer && isppg);
  gtk_widget_set_visible(g->greeneq, !passing && !bayer4 && !xtrans && !true_monochrome);
  gtk_widget_set_visible(g->color_smoothing, !passing && !bayer4 && !isdual && !true_monochrome);
  gtk_widget_set_visible(g->dual_thrs, isdual);
  gtk_widget_set_visible(g->lmmse_refine, islmmse);

  const gboolean monomode = use_method == DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME
                        ||  use_method == DT_IOP_DEMOSAIC_PASSTHR_MONOX;
  const gboolean was_monomode = oimg->flags & DT_IMAGE_MONOCHROME_BAYER;
  if((w == g->demosaic_method_bayer || w == g->demosaic_method_xtrans) && monomode != was_monomode)
  {
    dt_image_t *img = dt_image_cache_get(self->dev->image_storage.id, 'w');
    if(monomode)
      img->flags |= DT_IMAGE_MONOCHROME_BAYER;
    else
      img->flags &= ~DT_IMAGE_MONOCHROME_BAYER;

    const int mask_bw = dt_image_monochrome_flags(img);
    dt_image_cache_write_release(img, DT_IMAGE_CACHE_RELAXED);
    dt_imageio_update_monochrome_workflow_tag(self->dev->image_storage.id, mask_bw);
    dt_dev_reload_image(self->dev, self->dev->image_storage.id);
  }

  if(!w || w != g->dual_thrs)
  {
    dt_bauhaus_widget_set_quad_active(g->dual_thrs, FALSE);
    g->dual_mask = FALSE;
  }
  if(!w || w != g->cs_thrs)
  {
    dt_bauhaus_widget_set_quad_active(g->cs_thrs, FALSE);
    g->cs_mask = FALSE;
  }
}

void gui_update(dt_iop_module_t *self)
{
  gui_changed(self, NULL, NULL);
  gtk_stack_set_visible_child_name(GTK_STACK(self->widget), self->default_enabled ? "raw" : "non_raw");
  dt_iop_demosaic_gui_data_t *g = self->gui_data;
  g->new_radius = 0.0f;
  g->new_thrs = 0.0f;
  g->autoradius = FALSE;
  g->autothrs = FALSE;
}

static void _dual_thrs_callback(GtkWidget *quad, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_demosaic_gui_data_t *g = self->gui_data;

  g->dual_mask = dt_bauhaus_widget_get_quad_active(quad);

  dt_bauhaus_widget_set_quad_active(g->cs_thrs, FALSE);
  g->cs_mask = FALSE;
  dt_bauhaus_widget_set_quad_active(g->cs_boost, FALSE);
  g->cs_boost_mask = FALSE;

  dt_dev_reprocess_center(self->dev);
}

static void _cs_thrs_callback(GtkWidget *quad, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_demosaic_gui_data_t *g = self->gui_data;
  g->cs_mask = dt_bauhaus_widget_get_quad_active(quad);

  dt_bauhaus_widget_set_quad_active(g->dual_thrs, FALSE);
  g->dual_mask = FALSE;
  dt_bauhaus_widget_set_quad_active(g->cs_boost, FALSE);
  g->cs_boost_mask = FALSE;

  dt_dev_reprocess_center(self->dev);
}

static void _cs_boost_callback(GtkWidget *quad, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_demosaic_gui_data_t *g = self->gui_data;
  g->cs_boost_mask = dt_bauhaus_widget_get_quad_active(quad);

  dt_bauhaus_widget_set_quad_active(g->dual_thrs, FALSE);
  g->dual_mask = FALSE;
  dt_bauhaus_widget_set_quad_active(g->cs_thrs, FALSE);
  g->cs_mask = FALSE;

  dt_dev_reprocess_center(self->dev);
}

static void _cs_radius_callback(GtkWidget *quad, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_demosaic_gui_data_t *g = self->gui_data;
  g->new_radius = -1.0f;
  dt_dev_reprocess_center(self->dev);
}

static void _ui_pipe_done(gpointer instance, dt_iop_module_t *self)
{
  dt_iop_demosaic_gui_data_t *g = self->gui_data;
  if(!g || darktable.gui->reset) return;

  ++darktable.gui->reset;
  const gboolean new_radius = g->new_radius > 0.0f;
  const gboolean new_thrs = g->new_thrs > 0.0f;
  if(new_radius)
     dt_bauhaus_slider_set_val(g->cs_radius, g->new_radius);

  if(new_thrs)
    dt_bauhaus_slider_set_val(g->cs_thrs, g->new_thrs);

  --darktable.gui->reset;

  if(new_radius || new_thrs)
  {
    dt_print(DT_DEBUG_PIPE, "demosaic UI pipe sets radius=%.3f thrs=%.3f",
      g->new_radius, g->new_thrs);
    g->new_radius = g->new_thrs = 0.0f;

    dt_dev_add_history_item(darktable.develop, self, TRUE);
  }
}

static void _preset_applied_callback(gpointer instance, dt_iop_module_t *self)
{
  const dt_iop_demosaic_params_t *p = self->params;
  if(p->cs_enabled && (p->cs_radius <= 0.0f || p->cs_thrs <= 0.0f))
  {
    dt_print(DT_DEBUG_PIPE, "demosaic auto preset applied, radius=%.3f thrs=%.3f",
      p->cs_radius, p->cs_thrs);
    dt_dev_reprocess_center(self->dev);
  }
}

void gui_focus(dt_iop_module_t *self, const gboolean in)
{
  dt_iop_demosaic_gui_data_t *g = self->gui_data;
  if(!in)
  {
    const gboolean was_masking = g->dual_mask || g->cs_mask || g->cs_boost_mask;
    dt_bauhaus_widget_set_quad_active(g->dual_thrs, FALSE);
    g->dual_mask = FALSE;
    dt_bauhaus_widget_set_quad_active(g->cs_thrs, FALSE);
    g->cs_mask = FALSE;
    dt_bauhaus_widget_set_quad_active(g->cs_boost, FALSE);
    g->cs_boost_mask = FALSE;

    if(was_masking) dt_dev_reprocess_center(self->dev);
  }
}

void gui_init(dt_iop_module_t *self)
{
  dt_iop_demosaic_gui_data_t *g = IOP_GUI_ALLOC(demosaic);

  GtkWidget *box_raw = self->widget = dt_gui_vbox();

  g->demosaic_method_bayer = dt_bauhaus_combobox_from_params(self, "demosaicing_method");

  const int xtranspos = dt_bauhaus_combobox_get_from_value(g->demosaic_method_bayer, DT_DEMOSAIC_XTRANS);

  for(int i=0;i<8;i++) dt_bauhaus_combobox_remove_at(g->demosaic_method_bayer, xtranspos);
  gtk_widget_set_tooltip_text(g->demosaic_method_bayer, _("Bayer sensor demosaicing method, PPG and RCD are fast, AMaZE and LMMSE are slow.\nLMMSE is suited best for high ISO images.\ndual demosaicers increase processing time by blending a VNG variant in a second pass."));

  g->demosaic_method_xtrans = dt_bauhaus_combobox_from_params(self, "demosaicing_method");
  for(int i=0;i<xtranspos;i++) dt_bauhaus_combobox_remove_at(g->demosaic_method_xtrans, 0);
  dt_bauhaus_combobox_remove_at(g->demosaic_method_xtrans, 7);
  gtk_widget_set_tooltip_text(g->demosaic_method_xtrans, _("X-Trans sensor demosaicing method, Markesteijn 3-pass and frequency domain chroma are slow.\ndual demosaicers increase processing time by blending a VNG variant in a second pass."));

  g->demosaic_method_bayerfour = dt_bauhaus_combobox_from_params(self, "demosaicing_method");
  for(int i=0;i<8;i++) dt_bauhaus_combobox_remove_at(g->demosaic_method_bayerfour, xtranspos);
  for(int i=0;i<2;i++) dt_bauhaus_combobox_remove_at(g->demosaic_method_bayerfour, 0);
  for(int i=0;i<4;i++) dt_bauhaus_combobox_remove_at(g->demosaic_method_bayerfour, 1);
  gtk_widget_set_tooltip_text(g->demosaic_method_bayerfour, _("Bayer4 sensor demosaicing methods."));

  g->demosaic_method_mono = dt_bauhaus_combobox_from_params(self, "demosaicing_method");
  for(int i=0;i<7;i++) dt_bauhaus_combobox_remove_at(g->demosaic_method_mono, xtranspos);
  for(int i=0;i<xtranspos;i++) dt_bauhaus_combobox_remove_at(g->demosaic_method_mono, 0);
  gtk_widget_set_tooltip_text(g->demosaic_method_mono, _("monochrome sensor demosaicing methods."));

  g->dual_thrs = dt_bauhaus_slider_from_params(self, "dual_thrs");
  dt_bauhaus_slider_set_digits(g->dual_thrs, 2);
  gtk_widget_set_tooltip_text(g->dual_thrs, _("contrast threshold for dual demosaic.\nset to 0.0 for high frequency content\n"
                                                "set to 1.0 for flat content"));
  dt_bauhaus_widget_set_quad(g->dual_thrs, self, dtgtk_cairo_paint_showmask, TRUE, _dual_thrs_callback,
                             _("toggle mask visualization"));

  g->median_thrs = dt_bauhaus_slider_from_params(self, "median_thrs");
  dt_bauhaus_slider_set_digits(g->median_thrs, 3);
  gtk_widget_set_tooltip_text(g->median_thrs, _("threshold for edge-aware median.\nset to 0.0 to switch off\n"
                                                "set to 1.0 to ignore edges"));
  g->lmmse_refine = dt_bauhaus_combobox_from_params(self, "lmmse_refine");
  gtk_widget_set_tooltip_text(g->lmmse_refine, _("LMMSE refinement steps. the median steps average the output,\nrefine adds some recalculation of red & blue channels"));

  g->color_smoothing = dt_bauhaus_combobox_from_params(self, "color_smoothing");
  gtk_widget_set_tooltip_text(g->color_smoothing, _("how many color smoothing median steps after demosaicing"));

  g->greeneq = dt_bauhaus_combobox_from_params(self, "green_eq");
  gtk_widget_set_tooltip_text(g->greeneq, _("green channels matching method. only apply for some old sensors"));

  g->cs_enabled = dt_bauhaus_toggle_from_params(self, "cs_enabled");
  gtk_widget_set_tooltip_text(g->cs_enabled, _("capture sharpen recovers details lost due to in-camera blurring\n"
                                            "which can be caused by diffraction, the anti-aliasing filter or other\n"
                                            "sources of gaussian-type blur"));

  dt_gui_new_collapsible_section(&g->capture, "plugins/darkroom/demosaic/expand_capture",
                                 _("capture sharpen controls"), GTK_BOX(box_raw), DT_ACTION(self));
  self->widget = GTK_WIDGET(g->capture.container);

  g->cs_iter = dt_bauhaus_slider_from_params(self, "cs_iter");
  gtk_widget_set_tooltip_text(g->cs_iter, _("set effect strength by iterations"));

  g->cs_radius = dt_bauhaus_slider_from_params(self, "cs_radius");
  dt_bauhaus_slider_set_digits(g->cs_radius, 2);
  dt_bauhaus_slider_set_format(g->cs_radius, _(_(" px")));
  gtk_widget_set_tooltip_text(g->cs_radius, _("capture sharpen radius should reflect the overall gaussian type blur\n"
                                              "of the camera sensor, possibly the anti-aliasing filter and the lens.\n"
                                              "increasing this too far will soon lead to artifacts like halos and\n"
                                              "ringing especially when used with a large 'iterations' setting.\n\n"
                                              "Note: a radius set to zero will be recalculated automatically the next run. use for presets"));
  dt_bauhaus_widget_set_quad(g->cs_radius, self, dtgtk_cairo_paint_reset, FALSE, _cs_radius_callback,
                                            _("calculate the capture sharpen radius from available raw sensor data.\n"
                                              "for best results avoid cropping or darkroom zooming in"));

  g->cs_thrs = dt_bauhaus_slider_from_params(self, "cs_thrs");
  gtk_widget_set_tooltip_text(g->cs_thrs, _("restrict capture sharpening to areas with high local contrast,\n"
                                            "increase to exclude flat areas in very dark or noisy images,\n"
                                            "decrease for well exposed and low noise images.\n\n"
                                            "Note: a threshold set to zero will be reset to defaults the next run. use for presets"));
  dt_bauhaus_widget_set_quad(g->cs_thrs, self, dtgtk_cairo_paint_showmask, TRUE, _cs_thrs_callback, _("visualize sharpened areas"));

  g->cs_boost = dt_bauhaus_slider_from_params(self, "cs_boost");
  dt_bauhaus_slider_set_digits(g->cs_boost, 2);
  dt_bauhaus_slider_set_format(g->cs_boost, _(_(" px")));
  gtk_widget_set_tooltip_text(g->cs_boost, _("further increase sharpen radius at image corners,\n"
                                             "the sharp center of the image will not be affected"));
  dt_bauhaus_widget_set_quad(g->cs_boost, self, dtgtk_cairo_paint_showmask, TRUE, _cs_boost_callback, _("visualize the overall radius"));

  g->cs_center = dt_bauhaus_slider_from_params(self, "cs_center");
  dt_bauhaus_slider_set_format(g->cs_center, "%");
  dt_bauhaus_slider_set_digits(g->cs_center, 0);
  gtk_widget_set_tooltip_text(g->cs_center, _("adjust to the sharp image center"));

  // start building top level widget
  self->widget = gtk_stack_new();
  gtk_stack_set_homogeneous(GTK_STACK(self->widget), FALSE);

  GtkWidget *label_non_raw = dt_ui_label_new(_("not applicable"));
  gtk_widget_set_tooltip_text(label_non_raw, _("demosaicing is only used for color raw images"));

  gtk_stack_add_named(GTK_STACK(self->widget), label_non_raw, "non_raw");
  gtk_stack_add_named(GTK_STACK(self->widget), box_raw, "raw");
  DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_DEVELOP_UI_PIPE_FINISHED, _ui_pipe_done);
  g->new_radius = 0.0f;
  g->new_thrs = 0.0f;

  DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_PRESET_APPLIED, _preset_applied_callback);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
