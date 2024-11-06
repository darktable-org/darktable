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

DT_MODULE_INTROSPECTION(4, dt_iop_demosaic_params_t)

#define DT_DEMOSAIC_XTRANS 1024 // masks for non-Bayer demosaic ops
#define DT_DEMOSAIC_DUAL 2048   // masks for dual demosaicing methods
#define DT_REDUCESIZE_MIN 64

#define DT_XTRANS_SNAPPER 3
#define DT_BAYER_SNAPPER 2

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
  DT_IOP_DEMOSAIC_RCD_VNG = DT_DEMOSAIC_DUAL | DT_IOP_DEMOSAIC_RCD, // $DESCRIPTION: "RCD + VNG4"
  DT_IOP_DEMOSAIC_AMAZE_VNG = DT_DEMOSAIC_DUAL | DT_IOP_DEMOSAIC_AMAZE, // $DESCRIPTION: "AMaZE + VNG4"
  DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME = 3, // $DESCRIPTION: "passthrough (monochrome)"
  DT_IOP_DEMOSAIC_PASSTHROUGH_COLOR = 4, // $DESCRIPTION: "photosite color (debug)"
  // methods for x-trans images
  DT_IOP_DEMOSAIC_VNG = DT_DEMOSAIC_XTRANS | 0,           // $DESCRIPTION: "VNG"
  DT_IOP_DEMOSAIC_MARKESTEIJN = DT_DEMOSAIC_XTRANS | 1,   // $DESCRIPTION: "Markesteijn 1-pass"
  DT_IOP_DEMOSAIC_MARKESTEIJN_3 = DT_DEMOSAIC_XTRANS | 2, // $DESCRIPTION: "Markesteijn 3-pass"
  DT_IOP_DEMOSAIC_FDC = DT_DEMOSAIC_XTRANS | 4,           // $DESCRIPTION: "frequency domain chroma"
  DT_IOP_DEMOSAIC_MARKEST3_VNG = DT_DEMOSAIC_DUAL | DT_IOP_DEMOSAIC_MARKESTEIJN_3, // $DESCRIPTION: "Markesteijn 3-pass + VNG"
  DT_IOP_DEMOSAIC_PASSTHR_MONOX = DT_DEMOSAIC_XTRANS | 3, // $DESCRIPTION: "passthrough (monochrome)"
  DT_IOP_DEMOSAIC_PASSTHR_COLORX = DT_DEMOSAIC_XTRANS | 5, // $DESCRIPTION: "photosite color (debug)"
} dt_iop_demosaic_method_t;

typedef enum dt_iop_demosaic_greeneq_t
{
  DT_IOP_GREEN_EQ_NO = 0,    // $DESCRIPTION: "disabled"
  DT_IOP_GREEN_EQ_LOCAL = 1, // $DESCRIPTION: "local average"
  DT_IOP_GREEN_EQ_FULL = 2,  // $DESCRIPTION: "full average"
  DT_IOP_GREEN_EQ_BOTH = 3   // $DESCRIPTION: "full and local average"
} dt_iop_demosaic_greeneq_t;

typedef enum dt_iop_demosaic_qual_flags_t
{
  // either perform full scale demosaicing or choose simple half scale
  // or third scale interpolation instead
  DT_DEMOSAIC_DEFAULT                 = 0,
  DT_DEMOSAIC_FULL_SCALE              = 1 << 0,
  DT_DEMOSAIC_ONLY_VNG_LINEAR         = 1 << 1,
} dt_iop_demosaic_qual_flags_t;

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
  float dual_thrs;                              // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.20 $DESCRIPTION: "dual threshold"
} dt_iop_demosaic_params_t;

typedef struct dt_iop_demosaic_gui_data_t
{
  GtkWidget *median_thrs;
  GtkWidget *greeneq;
  GtkWidget *color_smoothing;
  GtkWidget *demosaic_method_bayer;
  GtkWidget *demosaic_method_xtrans;
  GtkWidget *demosaic_method_bayerfour;
  GtkWidget *dual_thrs;
  GtkWidget *lmmse_refine;
  gboolean visual_mask;
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
  int kernel_downsample;
  int kernel_border_interpolate;
  int kernel_color_smoothing;
  int kernel_zoom_passthrough_monochrome;
  int kernel_vng_border_interpolate;
  int kernel_vng_lin_interpolate;
  int kernel_zoom_third_size;
  int kernel_vng_green_equilibrate;
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
  int kernel_write_blended_dual;
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
} dt_iop_demosaic_data_t;

static gboolean _get_thumb_quality(const int width, const int height)
{
  // we check if we need ultra-high quality thumbnail for this size
  const dt_mipmap_size_t level = dt_mipmap_cache_get_matching_size(darktable.mipmap_cache, width, height);
  const char *min = dt_conf_get_string_const("plugins/lighttable/thumbnail_hq_min_level");
  const dt_mipmap_size_t min_s = dt_mipmap_cache_get_min_mip_from_pref(min);

  return (level >= min_s);
}

// set flags for demosaic quality based on factors besides demosaic
// method (e.g. config, scale, pixelpipe type)
static dt_iop_demosaic_qual_flags_t demosaic_qual_flags(const dt_dev_pixelpipe_iop_t *const piece,
                                                        const dt_image_t *const img,
                                                        const dt_iop_roi_t *const roi_out)
{
  const uint32_t filters = piece->pipe->dsc.filters;
  const gboolean is_xtrans = filters == 9u;

  dt_iop_demosaic_qual_flags_t flags = DT_DEMOSAIC_DEFAULT;
  switch(piece->pipe->type & DT_DEV_PIXELPIPE_ANY)
  {
    case DT_DEV_PIXELPIPE_FULL:
      flags |= DT_DEMOSAIC_FULL_SCALE;
      break;
    case DT_DEV_PIXELPIPE_EXPORT:
      flags |= DT_DEMOSAIC_FULL_SCALE;
      break;
    case DT_DEV_PIXELPIPE_THUMBNAIL:
      flags |= (piece->pipe->want_detail_mask || _get_thumb_quality(roi_out->width, roi_out->height))
                  ? DT_DEMOSAIC_FULL_SCALE
                  : DT_DEMOSAIC_DEFAULT;
      break;
    default: // make C not complain about missing enum members
      break;
  }

  // For sufficiently small scaling, one or more repetitition of the
  // CFA pattern can be merged into a single pixel, hence it is
  // possible to skip the full demosaic and perform a quick downscale.
  // Note even though the X-Trans CFA is 6x6, for this purposes we can
  // see each 6x6 tile as four fairly similar 3x3 tiles
  if(roi_out->scale > (is_xtrans ? 0.667f : 0.5f))
    flags |= DT_DEMOSAIC_FULL_SCALE;

  // half_size_f doesn't support 4bayer images
  if(img->flags & DT_IMAGE_4BAYER)
    flags |= DT_DEMOSAIC_FULL_SCALE;

  // we check if we can stop at the linear interpolation step in VNG
  // instead of going the full way
  if((flags & DT_DEMOSAIC_FULL_SCALE) && (roi_out->scale < (is_xtrans ? 0.5f : 0.667f)))
    flags |= DT_DEMOSAIC_ONLY_VNG_LINEAR;

  return flags;
}

// Implemented in demosaicing/amaze.cc
void amaze_demosaic(dt_dev_pixelpipe_iop_t *piece,
                    const float *const in,
                    float *out,
                    const dt_iop_roi_t *const roi_in,
                    const dt_iop_roi_t *const roi_out,
                    const uint32_t filters);

#include "iop/demosaicing/basics.c"
#include "iop/demosaicing/vng.c"
#include "iop/demosaicing/xtrans.c"
#include "iop/demosaicing/passthrough.c"
#include "iop/demosaicing/ppg.c"
#include "iop/demosaicing/rcd.c"
#include "iop/demosaicing/lmmse.c"
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
  return IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_ONE_INSTANCE | IOP_FLAGS_FENCE;
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
    n->color_smoothing = 0;
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

  return 1;
}

dt_iop_colorspace_type_t input_colorspace(dt_iop_module_t *self,
                                          dt_dev_pixelpipe_t *pipe,
                                          dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RAW;
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
  const struct dt_interpolation *itor = dt_interpolation_new(DT_INTERPOLATION_USERPREF_WARP);
  dt_interpolation_resample_roi_1c(itor, out, roi_out, in, roi_in);
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
  // need 1:1, demosaic and then sub-sample. or directly sample half-size
  roi_in->x /= roi_out->scale;
  roi_in->y /= roi_out->scale;
  roi_in->width /= roi_out->scale;
  roi_in->height /= roi_out->scale;
  roi_in->scale = 1.0f;

  dt_iop_demosaic_data_t *d = piece->data;
  const dt_iop_demosaic_method_t method = d->demosaicing_method;
  const gboolean passthrough = method == DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME ||
                               method == DT_IOP_DEMOSAIC_PASSTHR_MONOX ||
                               method == DT_IOP_DEMOSAIC_PASSTHROUGH_COLOR ||
                               method == DT_IOP_DEMOSAIC_PASSTHR_COLORX;
  // set position to closest top/left sensor pattern snap
  if(!passthrough)
  {
    const int aligner = (piece->pipe->dsc.filters != 9u) ? DT_BAYER_SNAPPER : DT_XTRANS_SNAPPER;
    const int dx = roi_in->x % aligner;
    const int dy = roi_in->y % aligner;

/*
    // This implements snapping to closest position, meant for optimized xtrans position
    // but with problems at extreme zoom levels
    const int shift_x = (dx > aligner / 2) ? aligner - dx : -dx;
    const int shift_y = (dy > aligner / 2) ? aligner - dy : -dy;

    roi_in->x += shift_x;
    roi_in->y += shift_y;
*/

    // currently we always snap to left & upper
    roi_in->x -= dx;
    roi_in->y -= dy;
  }

  // clamp to full buffer fixing numeric inaccuracies
  roi_in->x = MAX(0, roi_in->x);
  roi_in->y = MAX(0, roi_in->y);
  roi_in->width = MIN(roi_in->width, piece->buf_in.width);
  roi_in->height = MIN(roi_in->height, piece->buf_in.height);
}

void tiling_callback(dt_iop_module_t *self,
                     dt_dev_pixelpipe_iop_t *piece,
                     const dt_iop_roi_t *roi_in,
                     const dt_iop_roi_t *roi_out,
                     dt_develop_tiling_t *tiling)
{
  dt_iop_demosaic_data_t *d = piece->data;

  const float ioratio = (float)roi_out->width * roi_out->height / ((float)roi_in->width * roi_in->height);
  const float smooth = d->color_smoothing ? ioratio : 0.0f;
  const gboolean is_xtrans = piece->pipe->dsc.filters == 9u;
  const float greeneq = (!is_xtrans && (d->green_eq != DT_IOP_GREEN_EQ_NO)) ? 0.25f : 0.0f;
  const dt_iop_demosaic_method_t demosaicing_method = d->demosaicing_method & ~DT_DEMOSAIC_DUAL;

  const int qual_flags = demosaic_qual_flags(piece, &self->dev->image_storage, roi_out);
  const int full_scale = qual_flags & DT_DEMOSAIC_FULL_SCALE;

  // check if output buffer has same dimension as input buffer (thus avoiding one
  // additional temporary buffer)
  const gboolean unscaled = roi_out->width == roi_in->width && roi_out->height == roi_in->height && feqf(roi_in->scale, roi_out->scale, 1e-8f);
  const gboolean is_opencl = piece->pipe->devid > DT_DEVICE_CPU;
  // define aligners
  tiling->xalign = is_xtrans ? DT_XTRANS_SNAPPER : DT_BAYER_SNAPPER;
  tiling->yalign = is_xtrans ? DT_XTRANS_SNAPPER : DT_BAYER_SNAPPER;

  tiling->maxbuf = 1.0f;
  tiling->overhead = 0;

  if(demosaicing_method == DT_IOP_DEMOSAIC_PPG ||
     demosaicing_method == DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME ||
     demosaicing_method == DT_IOP_DEMOSAIC_PASSTHROUGH_COLOR ||
     demosaicing_method == DT_IOP_DEMOSAIC_AMAZE)
  {
    // Bayer pattern with PPG, Passthrough or Amaze
    tiling->factor = 1.0f + ioratio;         // in + out

    if(full_scale && unscaled)
      tiling->factor += MAX(1.0f + greeneq, smooth);  // + tmp + geeneq | + smooth
    else if(full_scale)
      tiling->factor += MAX(2.0f + greeneq, smooth);  // + tmp + aux + greeneq | + smooth
    else
      tiling->factor += smooth;                        // + smooth

    tiling->overhead = 0;
    tiling->overlap = 5; // take care of border handling
  }
  else if(demosaicing_method == DT_IOP_DEMOSAIC_MARKESTEIJN ||
          demosaicing_method == DT_IOP_DEMOSAIC_MARKESTEIJN_3 ||
          demosaicing_method == DT_IOP_DEMOSAIC_FDC)
  {
    // X-Trans pattern full Markesteijn processing
    const int ndir = demosaicing_method == DT_IOP_DEMOSAIC_MARKESTEIJN_3 ? 8 : 4;
    const int overlap = demosaicing_method == DT_IOP_DEMOSAIC_MARKESTEIJN_3 ? 18 : 12;

    tiling->factor = 1.0f + ioratio;
    tiling->factor += ndir * 1.0f      // rgb
                      + ndir * 0.25f   // drv
                      + ndir * 0.125f  // homo + homosum
                      + 1.0f;          // aux

    if(full_scale && unscaled)
      tiling->factor += MAX(1.0f + greeneq, smooth);
    else if(full_scale)
      tiling->factor += MAX(2.0f + greeneq, smooth);
    else
      tiling->factor += smooth;

    tiling->overlap = overlap;
  }
  else if(demosaicing_method == DT_IOP_DEMOSAIC_RCD)
  {
    tiling->factor = 1.0f + ioratio;
    if(full_scale && unscaled)
      tiling->factor += MAX(1.0f + greeneq, smooth);  // + tmp + geeneq | + smooth
    else if(full_scale)
      tiling->factor += MAX(2.0f + greeneq, smooth);  // + tmp + aux + greeneq | + smooth
    else
      tiling->factor += smooth;                        // + smooth

    tiling->overhead = is_opencl ? 0 : sizeof(float) * DT_RCD_TILESIZE * DT_RCD_TILESIZE * 8 * dt_get_num_threads();
    tiling->overlap = 10;
    tiling->factor_cl = tiling->factor + 3.0f;
  }
  else if(demosaicing_method == DT_IOP_DEMOSAIC_LMMSE)
  {
    tiling->factor = 1.0f + ioratio;
    if(full_scale && unscaled)
      tiling->factor += MAX(1.0f + greeneq, smooth);  // + tmp + geeneq | + smooth
    else if(full_scale)
      tiling->factor += MAX(2.0f + greeneq, smooth);  // + tmp + aux + greeneq | + smooth
    else
      tiling->factor += smooth;                        // + smooth
    tiling->overhead = sizeof(float) * DT_LMMSE_TILESIZE * DT_LMMSE_TILESIZE * 6 * dt_get_num_threads();
    tiling->overlap = 10;
  }
  else
  {
    // VNG
    tiling->factor = 1.0f + ioratio;

    if(full_scale && unscaled)
      tiling->factor += MAX(1.0f + greeneq, smooth);
    else if(full_scale)
      tiling->factor += MAX(2.0f + greeneq, smooth);
    else
      tiling->factor += smooth;

    tiling->overlap = 6;
  }

  if(d->demosaicing_method & DT_DEMOSAIC_DUAL)
  {
    // make sure VNG4 is also possible
    tiling->factor += 1.0f;
    tiling->overlap = MAX(6, tiling->overlap);
  }
  return;
}

void process(dt_iop_module_t *self,
             dt_dev_pixelpipe_iop_t *piece,
             const void *const i,
             void *const o,
             const dt_iop_roi_t *const roi_in,
             const dt_iop_roi_t *const roi_out)
{
  const dt_image_t *img = &self->dev->image_storage;

  dt_dev_clear_scharr_mask(piece->pipe);

  dt_iop_roi_t roo = *roi_out;
  roo.x = roo.y = 0;

  const gboolean run_fast = piece->pipe->type & DT_DEV_PIXELPIPE_FAST;
  const gboolean fullpipe = piece->pipe->type & DT_DEV_PIXELPIPE_FULL;

  const uint8_t(*const xtrans)[6] = (const uint8_t(*const)[6])piece->pipe->dsc.xtrans;

  const dt_iop_demosaic_data_t *d = piece->data;
  const dt_iop_demosaic_gui_data_t *g = self->gui_data;

  const int qual_flags = demosaic_qual_flags(piece, img, roi_out);
  const gboolean fullscale = qual_flags & DT_DEMOSAIC_FULL_SCALE;
  const gboolean is_xtrans = piece->pipe->dsc.filters == 9u;
  const gboolean is_4bayer = img->flags & DT_IMAGE_4BAYER;
  const gboolean is_bayer = !is_xtrans && piece->pipe->dsc.filters != 0;

  int demosaicing_method = d->demosaicing_method;
  const int width = roi_in->width;
  const int height = roi_in->height;

  if(width < 16 || height < 16)
    demosaicing_method = is_xtrans ? DT_IOP_DEMOSAIC_MARKEST3_VNG : DT_IOP_DEMOSAIC_VNG4;

  gboolean showmask = FALSE;
  if(self->dev->gui_attached && fullpipe)
  {
    if(g->visual_mask)
    {
      showmask = TRUE;
      piece->pipe->mask_display = DT_DEV_PIXELPIPE_DISPLAY_MASK;
    }
    // take care of passthru modes
    if(piece->pipe->mask_display == DT_DEV_PIXELPIPE_DISPLAY_PASSTHRU)
      demosaicing_method = is_xtrans ? DT_IOP_DEMOSAIC_MARKESTEIJN : DT_IOP_DEMOSAIC_RCD;
  }

  float *in  = (float *)i;
  float *out = (float *)o;

  if(!fullscale)
  {
    dt_print_pipe(DT_DEBUG_PIPE, "demosaic approx zoom", piece->pipe, self, DT_DEVICE_CPU, roi_in, roi_out);
    if(demosaicing_method == DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME || demosaicing_method == DT_IOP_DEMOSAIC_PASSTHROUGH_COLOR)
      dt_iop_clip_and_zoom_demosaic_passthrough_monochrome_f(out, in, &roo, roi_in, roi_out->width, width);
    else if(is_xtrans)
      dt_iop_clip_and_zoom_demosaic_third_size_xtrans_f(out, in, &roo, roi_in, roi_out->width, width, xtrans);
    else
      dt_iop_clip_and_zoom_demosaic_half_size_f(out, in, &roo, roi_in, roi_out->width, width, piece->pipe->dsc.filters);

    return;
  }

  const int base_demosaicing_method = demosaicing_method & ~DT_DEMOSAIC_DUAL;
  const gboolean dual = (demosaicing_method & DT_DEMOSAIC_DUAL) && !run_fast;

  const gboolean direct = roi_out->width == width && roi_out->height == height && feqf(roi_in->scale, roi_out->scale, 1e-8f);

  if(!direct)
  {
    // demosaic and then clip and zoom
    // we demosaic at 1:1 the size of input roi, so make sure
    // we fit these bounds exactly, to avoid crashes.
    // FIXME: instead of fighting with roi_out data for the CPU demosaicers we should rework all
    // CPU demosaicer code using only roi_in as we do already for OpenCL code path.
    roo.width = width;
    roo.height = height;
    roo.scale = 1.0f;
    out = dt_alloc_align_float((size_t)4 * width * height);
  }

  if(is_bayer && d->green_eq != DT_IOP_GREEN_EQ_NO)
  {
    const float threshold = 0.0001f * img->exif_iso;
    in = dt_alloc_align_float((size_t)height * width);
    float *aux = NULL;

    switch(d->green_eq)
    {
      case DT_IOP_GREEN_EQ_FULL:
        green_equilibration_favg(in, (float *)i, width, height, piece->pipe->dsc.filters, roi_in->x, roi_in->y);
        break;
      case DT_IOP_GREEN_EQ_LOCAL:
        green_equilibration_lavg(in, (float *)i, width, height, piece->pipe->dsc.filters, roi_in->x, roi_in->y, threshold);
        break;
      case DT_IOP_GREEN_EQ_BOTH:
        aux = dt_alloc_align_float((size_t)height * width);
        green_equilibration_favg(aux, (float *)i, width, height, piece->pipe->dsc.filters, roi_in->x, roi_in->y);
        green_equilibration_lavg(in, aux, width, height, piece->pipe->dsc.filters, roi_in->x, roi_in->y, threshold);
        dt_free_align(aux);
        break;
      default:
        break;
    }
  }

  if(demosaicing_method == DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME)
    passthrough_monochrome(out, in, &roo, roi_in);
  else if(demosaicing_method == DT_IOP_DEMOSAIC_PASSTHROUGH_COLOR)
    passthrough_color(out, in, &roo, roi_in, piece->pipe->dsc.filters, xtrans);
  else if(is_xtrans)
  {
    const int passes = base_demosaicing_method == DT_IOP_DEMOSAIC_MARKESTEIJN_3 ? 3 : 1;
    if(demosaicing_method == DT_IOP_DEMOSAIC_MARKEST3_VNG)
      vng_interpolate(out, in, &roo, roi_in, piece->pipe->dsc.filters, xtrans, qual_flags & DT_DEMOSAIC_ONLY_VNG_LINEAR);
    else if(demosaicing_method == DT_IOP_DEMOSAIC_FDC)
      xtrans_fdc_interpolate(self, out, in, &roo, roi_in, xtrans);
    else if(base_demosaicing_method == DT_IOP_DEMOSAIC_MARKESTEIJN || base_demosaicing_method == DT_IOP_DEMOSAIC_MARKESTEIJN_3)
      xtrans_markesteijn_interpolate(out, in, &roo, roi_in, xtrans, passes);
    else
      vng_interpolate(out, in, &roo, roi_in, piece->pipe->dsc.filters, xtrans, qual_flags & DT_DEMOSAIC_ONLY_VNG_LINEAR);
  }
  else
  {
    if(demosaicing_method == DT_IOP_DEMOSAIC_VNG4 || is_4bayer)
    {
      vng_interpolate(out, in, &roo, roi_in, piece->pipe->dsc.filters, xtrans, qual_flags & DT_DEMOSAIC_ONLY_VNG_LINEAR);
      if(is_4bayer)
      {
        dt_colorspaces_cygm_to_rgb(out, width * height, d->CAM_to_RGB);
        dt_colorspaces_cygm_to_rgb(piece->pipe->dsc.processed_maximum, 1, d->CAM_to_RGB);
      }
    }
    else if(base_demosaicing_method == DT_IOP_DEMOSAIC_RCD)
      rcd_demosaic(piece, out, in, roi_in, piece->pipe->dsc.filters);
    else if(demosaicing_method == DT_IOP_DEMOSAIC_LMMSE)
      lmmse_demosaic(piece, out, in, roi_in, piece->pipe->dsc.filters, d->lmmse_refine);
    else if(base_demosaicing_method != DT_IOP_DEMOSAIC_AMAZE)
      demosaic_ppg(out, in, &roo, roi_in, piece->pipe->dsc.filters, d->median_thrs);
    else
      amaze_demosaic(piece, in, out, roi_in, &roo, piece->pipe->dsc.filters);
  }

  if(piece->pipe->want_detail_mask)
    dt_dev_write_scharr_mask(piece, out, roi_in, TRUE);

  if(dual)
    dual_demosaic(piece, out, in, &roo, roi_in, piece->pipe->dsc.filters, xtrans, showmask, d->dual_thrs);

  if((float *)i != in) dt_free_align(in);

  if(d->color_smoothing)
    color_smoothing(out, roi_in, d->color_smoothing);

  dt_print_pipe(DT_DEBUG_PIPE, direct ? "demosaic inplace" : "demosaic clip_and_zoom", piece->pipe, self, DT_DEVICE_CPU, roi_in, roi_out);
  if(!direct)
  {
    dt_iop_clip_and_zoom_roi((float *)o, out, roi_out, &roo);
    dt_free_align(out);
  }
}

#ifdef HAVE_OPENCL
int process_cl(dt_iop_module_t *self,
               dt_dev_pixelpipe_iop_t *piece,
               cl_mem dev_in,
               cl_mem dev_out,
               const dt_iop_roi_t *const roi_in,
               const dt_iop_roi_t *const roi_out)
{
  const dt_image_t *img = &self->dev->image_storage;
  const gboolean run_fast = piece->pipe->type & DT_DEV_PIXELPIPE_FAST;
  const gboolean fullpipe = piece->pipe->type & DT_DEV_PIXELPIPE_FULL;
  const int qual_flags = demosaic_qual_flags(piece, img, roi_out);
  const gboolean fullscale = qual_flags & DT_DEMOSAIC_FULL_SCALE;
  const gboolean is_xtrans = piece->pipe->dsc.filters == 9u;
  const gboolean is_bayer = !is_xtrans && piece->pipe->dsc.filters != 0;

  dt_dev_clear_scharr_mask(piece->pipe);

  const dt_iop_demosaic_data_t *d = piece->data;
  const dt_iop_demosaic_gui_data_t *g = self->gui_data;
  const dt_iop_demosaic_global_data_t *gd = self->global_data;

  int demosaicing_method = d->demosaicing_method;
  const int width = roi_in->width;
  const int height = roi_in->height;

  if(width < 16 || height < 16)
    demosaicing_method = is_xtrans ? DT_IOP_DEMOSAIC_MARKEST3_VNG : DT_IOP_DEMOSAIC_VNG4;

  gboolean showmask = FALSE;
  if(self->dev->gui_attached && fullpipe)
  {
    if(g->visual_mask)
    {
      showmask = TRUE;
      piece->pipe->mask_display = DT_DEV_PIXELPIPE_DISPLAY_MASK;
    }
    // take care of passthru modes
    if(piece->pipe->mask_display == DT_DEV_PIXELPIPE_DISPLAY_PASSTHRU)
      demosaicing_method = is_xtrans ? DT_IOP_DEMOSAIC_MARKESTEIJN : DT_IOP_DEMOSAIC_RCD;
  }

  const int devid = piece->pipe->devid;

  cl_int err = CL_MEM_OBJECT_ALLOCATION_FAILURE;

  if(dev_in  == NULL || dev_out == NULL) return err;

  if(!fullscale)
  {
    dt_print_pipe(DT_DEBUG_PIPE, "demosaic approx zoom", piece->pipe, self, piece->pipe->devid, roi_in, roi_out);
    const int zero = 0;
    if(is_xtrans)
    {
      cl_mem dev_xtrans = dt_opencl_copy_host_to_device_constant(devid, sizeof(piece->pipe->dsc.xtrans), piece->pipe->dsc.xtrans);
      if(dev_xtrans == NULL) return err;
      // sample third-size image
      err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_zoom_third_size, roi_out->width, roi_out->height,
          CLARG(dev_in), CLARG(dev_out), CLARG(roi_out->width), CLARG(roi_out->height), CLARG(roi_in->x), CLARG(roi_in->y),
          CLARG(width), CLARG(height), CLARG(roi_out->scale), CLARG(dev_xtrans));
      dt_opencl_release_mem_object(dev_xtrans);
      return err;
    }
    else if(demosaicing_method == DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME)
      return dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_zoom_passthrough_monochrome, roi_out->width, roi_out->height,
          CLARG(dev_in), CLARG(dev_out), CLARG(roi_out->width), CLARG(roi_out->height), CLARG(zero), CLARG(zero), CLARG(width),
          CLARG(height), CLARG(roi_out->scale), CLARG(piece->pipe->dsc.filters));
    else // bayer
      return dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_zoom_half_size, roi_out->width, roi_out->height,
          CLARG(dev_in), CLARG(dev_out), CLARG(roi_out->width), CLARG(roi_out->height), CLARG(zero), CLARG(zero), CLARG(width),
          CLARG(height), CLARG(roi_out->scale), CLARG(piece->pipe->dsc.filters));
  }

  const gboolean direct = roi_out->width == width && roi_out->height == height && feqf(roi_in->scale, roi_out->scale, 1e-8f);
  const int base_demosaicing_method = demosaicing_method & ~DT_DEMOSAIC_DUAL;
  const gboolean dual = (demosaicing_method & DT_DEMOSAIC_DUAL) && !run_fast;

  cl_mem out_image = direct ? dev_out : dt_opencl_alloc_device(devid, width, height, sizeof(float) * 4);
  cl_mem in_image = dev_in;

  if(out_image == NULL)
    goto finish;

  if(is_bayer && d->green_eq != DT_IOP_GREEN_EQ_NO)
  {
    in_image = dt_opencl_alloc_device(devid, width, height, sizeof(float));
    if(in_image == NULL) goto finish;

    err = green_equilibration_cl(self, piece, dev_in, in_image, roi_in);
    if(err != CL_SUCCESS) goto finish;
  }

  if(demosaicing_method == DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME ||
     demosaicing_method == DT_IOP_DEMOSAIC_PPG ||
     demosaicing_method == DT_IOP_DEMOSAIC_PASSTHROUGH_COLOR)
  {
    err = process_default_cl(self, piece, in_image, out_image, roi_in, demosaicing_method);
    if(err != CL_SUCCESS) return err;
  }
  else if(base_demosaicing_method == DT_IOP_DEMOSAIC_RCD)
  {
    err = process_rcd_cl(self, piece, in_image, out_image, roi_in);
    if(err != CL_SUCCESS) goto finish;
  }
  else if(demosaicing_method == DT_IOP_DEMOSAIC_VNG4 || demosaicing_method == DT_IOP_DEMOSAIC_VNG)
  {
    err = process_vng_cl(self, piece, in_image, out_image, roi_in, FALSE);
    if(err != CL_SUCCESS) goto finish;
  }
  else if(demosaicing_method == DT_IOP_DEMOSAIC_MARKEST3_VNG)
  {
    err = process_vng_cl(self, piece, in_image, out_image, roi_in, qual_flags & DT_DEMOSAIC_ONLY_VNG_LINEAR);
    if(err != CL_SUCCESS) goto finish;
  }
  else if(base_demosaicing_method == DT_IOP_DEMOSAIC_MARKESTEIJN || base_demosaicing_method == DT_IOP_DEMOSAIC_MARKESTEIJN_3)
  {
    err = process_markesteijn_cl(self, piece, in_image, out_image, roi_in);
    if(err != CL_SUCCESS) goto finish;
  }
  else
  {
    dt_print(DT_DEBUG_OPENCL, "[opencl_demosaic] demosaicing method %d not yet supported by opencl code", demosaicing_method);
    err = DT_OPENCL_PROCESS_CL;
    goto finish;
  }

  if(piece->pipe->want_detail_mask)
  {
    err = dt_dev_write_scharr_mask_cl(piece, out_image, roi_in, TRUE);
    if(err != CL_SUCCESS) goto finish;
  }

  if(dual)
  {
    err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
    cl_mem low_image = dt_opencl_alloc_device(devid, width, height, sizeof(float) * 4);
    if(low_image)
    {
      err = process_vng_cl(self, piece, in_image, low_image, roi_in, FALSE);
      if(err == CL_SUCCESS)
        err = color_smoothing_cl(self, piece, low_image, low_image, roi_in, DT_DEMOSAIC_SMOOTH_2);
      if(err == CL_SUCCESS)
        err = dual_demosaic_cl(self, piece, out_image, low_image, out_image, roi_in, showmask);
      dt_opencl_release_mem_object(low_image);
    }
    if(err != CL_SUCCESS) goto finish;
  }

  if(in_image != dev_in)  // release early for less cl memory load
  {
    dt_opencl_release_mem_object(in_image);
    in_image = NULL;
  }

  if(d->color_smoothing)
  {
    err = color_smoothing_cl(self, piece, out_image, out_image, roi_in, d->color_smoothing);
    if(err != CL_SUCCESS) goto finish;
  }

  dt_print_pipe(DT_DEBUG_PIPE, direct ? "demosaic inplace" : "demosaic clip_and_zoom", piece->pipe, self, piece->pipe->devid, roi_in, roi_out);
  if(!direct)
    err = dt_iop_clip_and_zoom_roi_cl(devid, dev_out, out_image, roi_out, roi_in);

finish:

  if(in_image != dev_in) dt_opencl_release_mem_object(in_image);
  if(out_image != dev_out) dt_opencl_release_mem_object(out_image);

  return err;
}
#endif

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
  gd->kernel_downsample = dt_opencl_create_kernel(program, "clip_and_zoom");
  gd->kernel_border_interpolate = dt_opencl_create_kernel(program, "border_interpolate");
  gd->kernel_color_smoothing = dt_opencl_create_kernel(program, "color_smoothing");

  const int other = 14; // from programs.conf
  gd->kernel_passthrough_monochrome = dt_opencl_create_kernel(other, "passthrough_monochrome");
  gd->kernel_passthrough_color = dt_opencl_create_kernel(other, "passthrough_color");
  gd->kernel_zoom_passthrough_monochrome = dt_opencl_create_kernel(other, "clip_and_zoom_demosaic_passthrough_monochrome");

  const int vng = 15; // from programs.conf
  gd->kernel_vng_border_interpolate = dt_opencl_create_kernel(vng, "vng_border_interpolate");
  gd->kernel_vng_lin_interpolate = dt_opencl_create_kernel(vng, "vng_lin_interpolate");
  gd->kernel_zoom_third_size = dt_opencl_create_kernel(vng, "clip_and_zoom_demosaic_third_size_xtrans");
  gd->kernel_vng_green_equilibrate = dt_opencl_create_kernel(vng, "vng_green_equilibrate");
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
  gd->kernel_write_blended_dual  = dt_opencl_create_kernel(rcd, "write_blended_dual");
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
  dt_opencl_free_kernel(gd->kernel_downsample);
  dt_opencl_free_kernel(gd->kernel_border_interpolate);
  dt_opencl_free_kernel(gd->kernel_color_smoothing);
  dt_opencl_free_kernel(gd->kernel_passthrough_monochrome);
  dt_opencl_free_kernel(gd->kernel_passthrough_color);
  dt_opencl_free_kernel(gd->kernel_zoom_passthrough_monochrome);
  dt_opencl_free_kernel(gd->kernel_vng_border_interpolate);
  dt_opencl_free_kernel(gd->kernel_vng_lin_interpolate);
  dt_opencl_free_kernel(gd->kernel_zoom_third_size);
  dt_opencl_free_kernel(gd->kernel_vng_green_equilibrate);
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
  dt_opencl_free_kernel(gd->kernel_write_blended_dual);
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

  if(!dt_image_is_raw(&pipe->image))
    piece->enabled = FALSE;
  d->green_eq = p->green_eq;
  d->color_smoothing = p->color_smoothing;
  d->median_thrs = p->median_thrs;
  d->dual_thrs = p->dual_thrs;
  d->lmmse_refine = p->lmmse_refine;
  dt_iop_demosaic_method_t use_method = p->demosaicing_method;

  const gboolean xmethod = use_method & DT_DEMOSAIC_XTRANS;
  const gboolean bayer4  = self->dev->image_storage.flags & DT_IMAGE_4BAYER;
  const gboolean bayer   = self->dev->image_storage.buf_dsc.filters != 9u && !bayer4;
  const gboolean xtrans  = self->dev->image_storage.buf_dsc.filters == 9u;

  if(bayer && xmethod)
    use_method = DT_IOP_DEMOSAIC_RCD;
  if(xtrans && !xmethod)
    use_method = DT_IOP_DEMOSAIC_MARKESTEIJN;
  // we don't have to fully check for available bayer4 modes here as process() takes care of this
  if(bayer4)
    use_method &= ~DT_DEMOSAIC_DUAL;

  if(use_method == DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME || use_method == DT_IOP_DEMOSAIC_PASSTHR_MONOX)
    use_method = DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME;
  if(use_method == DT_IOP_DEMOSAIC_PASSTHROUGH_COLOR || use_method == DT_IOP_DEMOSAIC_PASSTHR_COLORX)
    use_method = DT_IOP_DEMOSAIC_PASSTHROUGH_COLOR;

  const gboolean passing = use_method == DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME
                        || use_method == DT_IOP_DEMOSAIC_PASSTHROUGH_COLOR;

  if(use_method != DT_IOP_DEMOSAIC_PPG)
    d->median_thrs = 0.0f;

  if(passing || bayer4)
  {
    d->green_eq = DT_IOP_GREEN_EQ_NO;
    d->color_smoothing = 0;
  }

  if(use_method & DT_DEMOSAIC_DUAL)
  {
    dt_dev_pixelpipe_usedetails(piece->pipe);
    d->color_smoothing = 0;
  }
  d->demosaicing_method = use_method;

  // OpenCL only supported by some of the demosaicing methods
  switch(d->demosaicing_method)
  {
    case DT_IOP_DEMOSAIC_PPG:
      piece->process_cl_ready = TRUE;
      break;
    case DT_IOP_DEMOSAIC_AMAZE:
      piece->process_cl_ready = FALSE;
      break;
    case DT_IOP_DEMOSAIC_VNG4:
      piece->process_cl_ready = TRUE;
      break;
    case DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME:
      piece->process_cl_ready = TRUE;
      break;
    case DT_IOP_DEMOSAIC_PASSTHROUGH_COLOR:
      piece->process_cl_ready = TRUE;
      break;
    case DT_IOP_DEMOSAIC_RCD:
      piece->process_cl_ready = TRUE;
      break;
    case DT_IOP_DEMOSAIC_LMMSE:
      piece->process_cl_ready = FALSE;
      break;
    case DT_IOP_DEMOSAIC_RCD_VNG:
      piece->process_cl_ready = TRUE;
      break;
    case DT_IOP_DEMOSAIC_AMAZE_VNG:
      piece->process_cl_ready = FALSE;
      break;
    case DT_IOP_DEMOSAIC_MARKEST3_VNG:
      piece->process_cl_ready = TRUE;
      break;
    case DT_IOP_DEMOSAIC_VNG:
      piece->process_cl_ready = TRUE;
      break;
    case DT_IOP_DEMOSAIC_MARKESTEIJN:
      piece->process_cl_ready = TRUE;
      break;
    case DT_IOP_DEMOSAIC_MARKESTEIJN_3:
      piece->process_cl_ready = TRUE;
      break;
    case DT_IOP_DEMOSAIC_FDC:
      piece->process_cl_ready = FALSE;
      break;
    default:
      piece->process_cl_ready = FALSE;
  }


  // green-equilibrate over full image excludes tiling
  // The details mask calculation required for dual demosaicing does not allow tiling.
  if((d->green_eq == DT_IOP_GREEN_EQ_FULL
      || d->green_eq == DT_IOP_GREEN_EQ_BOTH)
      || use_method & DT_DEMOSAIC_DUAL
      || piece->pipe->want_detail_mask)
  {
    piece->process_tiling_ready = FALSE;
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

  if(dt_image_is_monochrome(&self->dev->image_storage))
    d->demosaicing_method = DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME;
  else if(self->dev->image_storage.buf_dsc.filters == 9u)
    d->demosaicing_method = DT_IOP_DEMOSAIC_MARKESTEIJN;
  else
    d->demosaicing_method = self->dev->image_storage.flags & DT_IMAGE_4BAYER
                            ? DT_IOP_DEMOSAIC_VNG4
                            : DT_IOP_DEMOSAIC_RCD;

  self->hide_enable_button = TRUE;

  self->default_enabled = dt_image_is_raw(&self->dev->image_storage);
  if(self->widget)
    gtk_stack_set_visible_child_name(GTK_STACK(self->widget), self->default_enabled ? "raw" : "non_raw");
}

void gui_changed(dt_iop_module_t *self, GtkWidget *w, void *previous)
{
  dt_iop_demosaic_gui_data_t *g = self->gui_data;
  dt_iop_demosaic_params_t *p = self->params;

  const gboolean bayer4 = self->dev->image_storage.flags & DT_IMAGE_4BAYER;
  const gboolean bayer  = self->dev->image_storage.buf_dsc.filters != 9u && !bayer4;
  const gboolean xtrans = self->dev->image_storage.buf_dsc.filters == 9u;

  dt_iop_demosaic_method_t use_method = p->demosaicing_method;
  const gboolean xmethod = use_method & DT_DEMOSAIC_XTRANS;

  if(bayer && xmethod)
    use_method = DT_IOP_DEMOSAIC_RCD;
  if(xtrans && !xmethod)
    use_method = DT_IOP_DEMOSAIC_MARKESTEIJN;

  const gboolean bayerpassing =
      use_method == DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME
   || use_method == DT_IOP_DEMOSAIC_PASSTHROUGH_COLOR;

  if(bayer4 && !(bayerpassing || (use_method == DT_IOP_DEMOSAIC_VNG4)))
    use_method = DT_IOP_DEMOSAIC_VNG4;

  const gboolean isppg = use_method == DT_IOP_DEMOSAIC_PPG;
  const gboolean isdual = (use_method & DT_DEMOSAIC_DUAL) && !bayer4;
  const gboolean islmmse = use_method == DT_IOP_DEMOSAIC_LMMSE;
  const gboolean passing = bayerpassing
    || use_method == DT_IOP_DEMOSAIC_PASSTHR_MONOX
    || use_method == DT_IOP_DEMOSAIC_PASSTHR_COLORX;

  gtk_widget_set_visible(g->demosaic_method_bayer, bayer);
  gtk_widget_set_visible(g->demosaic_method_bayerfour, bayer4);
  gtk_widget_set_visible(g->demosaic_method_xtrans, xtrans);

  // we might have a wrong method dur to xtrans/bayer - mode mismatch
  if(bayer)
    dt_bauhaus_combobox_set_from_value(g->demosaic_method_bayer, use_method);
  else if(xtrans)
    dt_bauhaus_combobox_set_from_value(g->demosaic_method_xtrans, use_method);
  else
    dt_bauhaus_combobox_set_from_value(g->demosaic_method_bayerfour, use_method);

  p->demosaicing_method = use_method;

  gtk_widget_set_visible(g->median_thrs, bayer && isppg);
  gtk_widget_set_visible(g->greeneq, !passing && !bayer4 && !xtrans);
  gtk_widget_set_visible(g->color_smoothing, !passing && !bayer4 && !isdual);
  gtk_widget_set_visible(g->dual_thrs, isdual);
  gtk_widget_set_visible(g->lmmse_refine, islmmse);

  dt_image_t *img = dt_image_cache_get(darktable.image_cache, self->dev->image_storage.id, 'w');
  int mono_changed = img->flags & DT_IMAGE_MONOCHROME_BAYER;
  if(p->demosaicing_method == DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME
       || p->demosaicing_method == DT_IOP_DEMOSAIC_PASSTHR_MONOX)
    img->flags |= DT_IMAGE_MONOCHROME_BAYER;
  else
    img->flags &= ~DT_IMAGE_MONOCHROME_BAYER;
  const int mask_bw = dt_image_monochrome_flags(img);
  mono_changed ^= img->flags & DT_IMAGE_MONOCHROME_BAYER;
  dt_image_cache_write_release(darktable.image_cache, img, DT_IMAGE_CACHE_RELAXED);

  if(mono_changed)
  {
    dt_imageio_update_monochrome_workflow_tag(self->dev->image_storage.id, mask_bw);
    dt_dev_reload_image(self->dev, self->dev->image_storage.id);
  }

  // as the dual modes change behaviour for previous pipeline modules we do a reprocess
  if(isdual && (w == g->demosaic_method_bayer || w == g->demosaic_method_xtrans))
    dt_dev_reprocess_center(self->dev);
}

void gui_update(dt_iop_module_t *self)
{
  dt_iop_demosaic_gui_data_t *g = self->gui_data;
  dt_bauhaus_widget_set_quad_active(g->dual_thrs, FALSE);

  g->visual_mask = FALSE;
  gui_changed(self, NULL, NULL);

  gtk_stack_set_visible_child_name(GTK_STACK(self->widget), self->default_enabled ? "raw" : "non_raw");
}

static void _visualize_callback(GtkWidget *quad, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_demosaic_gui_data_t *g = self->gui_data;

  g->visual_mask = dt_bauhaus_widget_get_quad_active(quad);
  dt_dev_reprocess_center(self->dev);
}

void gui_focus(dt_iop_module_t *self, gboolean in)
{
  dt_iop_demosaic_gui_data_t *g = self->gui_data;
  if(!in)
  {
    const gboolean was_dualmask = g->visual_mask;
    dt_bauhaus_widget_set_quad_active(g->dual_thrs, FALSE);
    g->visual_mask = FALSE;
    if(was_dualmask) dt_dev_reprocess_center(self->dev);
  }
}

void gui_init(dt_iop_module_t *self)
{
  dt_iop_demosaic_gui_data_t *g = IOP_GUI_ALLOC(demosaic);

  GtkWidget *box_raw = self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  g->demosaic_method_bayer = dt_bauhaus_combobox_from_params(self, "demosaicing_method");

  const int xtranspos = dt_bauhaus_combobox_get_from_value(g->demosaic_method_bayer, DT_DEMOSAIC_XTRANS);

  for(int i=0;i<7;i++) dt_bauhaus_combobox_remove_at(g->demosaic_method_bayer, xtranspos);
  gtk_widget_set_tooltip_text(g->demosaic_method_bayer, _("Bayer sensor demosaicing method, PPG and RCD are fast, AMaZE and LMMSE are slow.\nLMMSE is suited best for high ISO images.\ndual demosaicers double processing time."));

  g->demosaic_method_xtrans = dt_bauhaus_combobox_from_params(self, "demosaicing_method");
  for(int i=0;i<xtranspos;i++) dt_bauhaus_combobox_remove_at(g->demosaic_method_xtrans, 0);
  gtk_widget_set_tooltip_text(g->demosaic_method_xtrans, _("X-Trans sensor demosaicing method, Markesteijn 3-pass and frequency domain chroma are slow.\ndual demosaicers double processing time."));

  g->demosaic_method_bayerfour = dt_bauhaus_combobox_from_params(self, "demosaicing_method");
  for(int i=0;i<7;i++) dt_bauhaus_combobox_remove_at(g->demosaic_method_bayerfour, xtranspos);
  for(int i=0;i<2;i++) dt_bauhaus_combobox_remove_at(g->demosaic_method_bayerfour, 0);
  for(int i=0;i<4;i++) dt_bauhaus_combobox_remove_at(g->demosaic_method_bayerfour, 1);
  gtk_widget_set_tooltip_text(g->demosaic_method_bayerfour, _("Bayer4 sensor demosaicing methods."));

  g->median_thrs = dt_bauhaus_slider_from_params(self, "median_thrs");
  dt_bauhaus_slider_set_digits(g->median_thrs, 3);
  gtk_widget_set_tooltip_text(g->median_thrs, _("threshold for edge-aware median.\nset to 0.0 to switch off\n"
                                                "set to 1.0 to ignore edges"));

  g->dual_thrs = dt_bauhaus_slider_from_params(self, "dual_thrs");
  dt_bauhaus_slider_set_digits(g->dual_thrs, 2);
  gtk_widget_set_tooltip_text(g->dual_thrs, _("contrast threshold for dual demosaic.\nset to 0.0 for high frequency content\n"
                                                "set to 1.0 for flat content"));
  dt_bauhaus_widget_set_quad_paint(g->dual_thrs, dtgtk_cairo_paint_showmask, 0, NULL);
  dt_bauhaus_widget_set_quad_toggle(g->dual_thrs, TRUE);
  dt_bauhaus_widget_set_quad_active(g->dual_thrs, FALSE);
  g_signal_connect(G_OBJECT(g->dual_thrs), "quad-pressed", G_CALLBACK(_visualize_callback), self);
  dt_bauhaus_widget_set_quad_tooltip(g->dual_thrs, _("toggle mask visualization"));

  g->lmmse_refine = dt_bauhaus_combobox_from_params(self, "lmmse_refine");
  gtk_widget_set_tooltip_text(g->lmmse_refine, _("LMMSE refinement steps. the median steps average the output,\nrefine adds some recalculation of red & blue channels"));

  g->color_smoothing = dt_bauhaus_combobox_from_params(self, "color_smoothing");
  gtk_widget_set_tooltip_text(g->color_smoothing, _("how many color smoothing median steps after demosaicing"));

  g->greeneq = dt_bauhaus_combobox_from_params(self, "green_eq");
  gtk_widget_set_tooltip_text(g->greeneq, _("green channels matching method"));

  // start building top level widget
  self->widget = gtk_stack_new();
  gtk_stack_set_homogeneous(GTK_STACK(self->widget), FALSE);

  GtkWidget *label_non_raw = dt_ui_label_new(_("not applicable"));
  gtk_widget_set_tooltip_text(label_non_raw, _("demosaicing is only used for color raw images"));

  gtk_stack_add_named(GTK_STACK(self->widget), label_non_raw, "non_raw");
  gtk_stack_add_named(GTK_STACK(self->widget), box_raw, "raw");
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
