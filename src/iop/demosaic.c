/*
    This file is part of darktable,
    Copyright (C) 2010-2022 darktable developers.

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

#include "rcd_demosaic.c"
#include "lmmse_demosaic.c"

DT_MODULE_INTROSPECTION(4, dt_iop_demosaic_params_t)

#define DT_DEMOSAIC_XTRANS 1024 // masks for non-Bayer demosaic ops
#define DT_DEMOSAIC_DUAL 2048   // masks for dual demosaicing methods
#define DT_REDUCESIZE_MIN 64

#define DT_XTRANS_SNAPPER 3
#define DT_BAYER_SNAPPER 2

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
  DT_DEMOSAIC_FULL_SCALE              = 1 << 0,
  DT_DEMOSAIC_ONLY_VNG_LINEAR         = 1 << 1,
  DT_DEMOSAIC_XTRANS_FULL             = 1 << 2,
  DT_DEMOSAIC_MEDIUM_QUAL             = 1 << 3
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
  dt_iop_demosaic_greeneq_t green_eq; // $DEFAULT: DT_IOP_GREEN_EQ_NO $DESCRIPTION: "match greens"
  float median_thrs; // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "edge threshold"
  dt_iop_demosaic_smooth_t color_smoothing; // $DEFAULT: DT_DEMOSAIC_SMOOTH_OFF $DESCRIPTION: "color smoothing"
  dt_iop_demosaic_method_t demosaicing_method; // $DEFAULT: DT_IOP_DEMOSAIC_RCD $DESCRIPTION: "method"
  dt_iop_demosaic_lmmse_t lmmse_refine; // $DEFAULT: DT_LMMSE_REFINE_1 $DESCRIPTION: "LMMSE refine"
  float dual_thrs; // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.20 $DESCRIPTION: "dual threshold"
} dt_iop_demosaic_params_t;

typedef struct dt_iop_demosaic_gui_data_t
{
  GtkWidget *median_thrs;
  GtkWidget *greeneq;
  GtkWidget *color_smoothing;
  GtkWidget *demosaic_method_bayer;
  GtkWidget *demosaic_method_xtrans;
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
  float *lmmse_gamma_in;
  float *lmmse_gamma_out;
} dt_iop_demosaic_global_data_t;

typedef struct dt_iop_demosaic_data_t
{
  uint32_t green_eq;
  uint32_t color_smoothing;
  uint32_t demosaicing_method;
  uint32_t lmmse_refine;
  float median_thrs;
  double CAM_to_RGB[3][4];
  float dual_thrs;
} dt_iop_demosaic_data_t;

// Implemented on amaze_demosaic_RT.cc
void amaze_demosaic_RT(
    dt_dev_pixelpipe_iop_t *piece,
    const float *const in,
    float *out,
    const dt_iop_roi_t *const roi_in,
    const dt_iop_roi_t *const roi_out,
    const uint32_t filters);


const char *name()
{
  return _("demosaic");
}

const char **description(struct dt_iop_module_t *self)
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

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RAW;
}

int legacy_params(
        dt_iop_module_t *self,
        const void *const old_params,
        const int old_version,
        void *new_params,
        const int new_version)
{
  typedef struct dt_iop_demosaic_params_t dt_iop_demosaic_params_v4_t;
  typedef struct dt_iop_demosaic_params_v3_t
  {
    dt_iop_demosaic_greeneq_t green_eq;
    float median_thrs;
    uint32_t color_smoothing;
    dt_iop_demosaic_method_t demosaicing_method;
    dt_iop_demosaic_lmmse_t lmmse_refine;
  } dt_iop_demosaic_params_v3_t;

  if(old_version == 3 && new_version == 4)
  {
    dt_iop_demosaic_params_v3_t *o = (dt_iop_demosaic_params_v3_t *)old_params;
    dt_iop_demosaic_params_v4_t *n = (dt_iop_demosaic_params_v4_t *)new_params;
    memcpy(n, o, sizeof *o);
    n->dual_thrs = 0.20f;
    return 0;
  }

  if(old_version == 2 && new_version == 3)
  {
    dt_iop_demosaic_params_t *o = (dt_iop_demosaic_params_t *)old_params;
    dt_iop_demosaic_params_t *n = (dt_iop_demosaic_params_t *)new_params;
    n->green_eq = o->green_eq;
    n->median_thrs = o->median_thrs;
    n->color_smoothing = 0;
    n->demosaicing_method = DT_IOP_DEMOSAIC_PPG;
    n->lmmse_refine = DT_LMMSE_REFINE_1;
    return 0;
  }
  return 1;
}

int input_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe,
                     dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RAW;
}

int output_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe,
                      dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

#define SWAP(a, b)                                                                                           \
  {                                                                                                          \
    const float tmp = (b);                                                                                   \
    (b) = (a);                                                                                               \
    (a) = tmp;                                                                                               \
  }

#ifdef _OPENMP
  #pragma omp declare simd aligned(in, out)
#endif
static void pre_median_b(
        float *out,
        const float *const in,
        const dt_iop_roi_t *const roi,
        const uint32_t filters,
        const int num_passes,
        const float threshold)
{
  dt_iop_image_copy_by_size(out, in, roi->width, roi->height, 1);

  // now green:
  const int lim[5] = { 0, 1, 2, 1, 0 };
  for(int pass = 0; pass < num_passes; pass++)
  {
#ifdef _OPENMP
#pragma omp parallel for default(none) \
    dt_omp_firstprivate(filters, in, lim, roi, threshold) \
    shared(out) \
    schedule(static)
#endif
    for(int row = 3; row < roi->height - 3; row++)
    {
      float med[9];
      int col = 3;
      if(FC(row, col, filters) != 1 && FC(row, col, filters) != 3) col++;
      float *pixo = out + (size_t)roi->width * row + col;
      const float *pixi = in + (size_t)roi->width * row + col;
      for(; col < roi->width - 3; col += 2)
      {
        int cnt = 0;
        for(int k = 0, i = 0; i < 5; i++)
        {
          for(int j = -lim[i]; j <= lim[i]; j += 2)
          {
            if(fabsf(pixi[roi->width * (i - 2) + j] - pixi[0]) < threshold)
            {
              med[k++] = pixi[roi->width * (i - 2) + j];
              cnt++;
            }
            else
              med[k++] = 64.0f + pixi[roi->width * (i - 2) + j];
          }
        }
        for(int i = 0; i < 8; i++)
          for(int ii = i + 1; ii < 9; ii++)
            if(med[i] > med[ii]) SWAP(med[i], med[ii]);
        pixo[0] = fmaxf(0.0f, (cnt == 1 ? med[4] - 64.0f : med[(cnt - 1) / 2]));
        // pixo[0] = med[(cnt-1)/2];
        pixo += 2;
        pixi += 2;
      }
    }
  }
}

static void pre_median(
        float *out,
        const float *const in,
        const dt_iop_roi_t *const roi,
        const uint32_t filters,
        const int num_passes,
        const float threshold)
{
  pre_median_b(out, in, roi, filters, num_passes, threshold);
}

#define SWAPmed(I, J)                                                                                        \
  if(med[I] > med[J]) SWAP(med[I], med[J])

static void color_smoothing(
        float *out,
        const dt_iop_roi_t *const roi_out,
        const int num_passes)
{
  const int width4 = 4 * roi_out->width;

  for(int pass = 0; pass < num_passes; pass++)
  {
    for(int c = 0; c < 3; c += 2)
    {
      {
        float *outp = out;
        for(int j = 0; j < roi_out->height; j++)
          for(int i = 0; i < roi_out->width; i++, outp += 4) outp[3] = outp[c];
      }
#ifdef _OPENMP
#pragma omp parallel for default(none) \
      dt_omp_firstprivate(roi_out, width4) \
      shared(out, c) \
      schedule(static)
#endif
      for(int j = 1; j < roi_out->height - 1; j++)
      {
        float *outp = out + (size_t)4 * j * roi_out->width + 4;
        for(int i = 1; i < roi_out->width - 1; i++, outp += 4)
        {
          float med[9] = {
            outp[-width4 - 4 + 3] - outp[-width4 - 4 + 1], outp[-width4 + 0 + 3] - outp[-width4 + 0 + 1],
            outp[-width4 + 4 + 3] - outp[-width4 + 4 + 1], outp[-4 + 3] - outp[-4 + 1],
            outp[+0 + 3] - outp[+0 + 1], outp[+4 + 3] - outp[+4 + 1],
            outp[+width4 - 4 + 3] - outp[+width4 - 4 + 1], outp[+width4 + 0 + 3] - outp[+width4 + 0 + 1],
            outp[+width4 + 4 + 3] - outp[+width4 + 4 + 1],
          };
          /* optimal 9-element median search */
          SWAPmed(1, 2);
          SWAPmed(4, 5);
          SWAPmed(7, 8);
          SWAPmed(0, 1);
          SWAPmed(3, 4);
          SWAPmed(6, 7);
          SWAPmed(1, 2);
          SWAPmed(4, 5);
          SWAPmed(7, 8);
          SWAPmed(0, 3);
          SWAPmed(5, 8);
          SWAPmed(4, 7);
          SWAPmed(3, 6);
          SWAPmed(1, 4);
          SWAPmed(2, 5);
          SWAPmed(4, 7);
          SWAPmed(4, 2);
          SWAPmed(6, 4);
          SWAPmed(4, 2);
          outp[c] = fmaxf(med[4] + outp[1], 0.0f);
        }
      }
    }
  }
}
#undef SWAP

static void green_equilibration_lavg(
        float *out,
        const float *const in,
        const int width,
        const int height,
        const uint32_t filters,
        const int x,
        const int y,
        const float thr)
{
  const float maximum = 1.0f;

  int oj = 2, oi = 2;
  if(FC(oj + y, oi + x, filters) != 1) oj++;
  if(FC(oj + y, oi + x, filters) != 1) oi++;
  if(FC(oj + y, oi + x, filters) != 1) oj--;

  dt_iop_image_copy_by_size(out, in, width, height, 1);

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(height, in, thr, width, maximum) \
  shared(out, oi, oj) \
  schedule(static) collapse(2)
#endif
  for(size_t j = oj; j < height - 2; j += 2)
  {
    for(size_t i = oi; i < width - 2; i += 2)
    {
      const float o1_1 = in[(j - 1) * width + i - 1];
      const float o1_2 = in[(j - 1) * width + i + 1];
      const float o1_3 = in[(j + 1) * width + i - 1];
      const float o1_4 = in[(j + 1) * width + i + 1];
      const float o2_1 = in[(j - 2) * width + i];
      const float o2_2 = in[(j + 2) * width + i];
      const float o2_3 = in[j * width + i - 2];
      const float o2_4 = in[j * width + i + 2];

      const float m1 = (o1_1 + o1_2 + o1_3 + o1_4) / 4.0f;
      const float m2 = (o2_1 + o2_2 + o2_3 + o2_4) / 4.0f;

      // prevent divide by zero and ...
      // guard against m1/m2 becoming too large (due to m2 being too small) which results in hot pixels
      // also m1 must be checked to be positive
      if((m2 > 0.0f) && (m1 > 0.0f) && (m1 / m2 < maximum * 2.0f))
      {
        const float c1 = (fabsf(o1_1 - o1_2) + fabsf(o1_1 - o1_3) + fabsf(o1_1 - o1_4) + fabsf(o1_2 - o1_3)
                          + fabsf(o1_3 - o1_4) + fabsf(o1_2 - o1_4)) / 6.0f;
        const float c2 = (fabsf(o2_1 - o2_2) + fabsf(o2_1 - o2_3) + fabsf(o2_1 - o2_4) + fabsf(o2_2 - o2_3)
                          + fabsf(o2_3 - o2_4) + fabsf(o2_2 - o2_4)) / 6.0f;
        if((in[j * width + i] < maximum * 0.95f) && (c1 < maximum * thr) && (c2 < maximum * thr))
        {
          out[j * width + i] = fmaxf(0.0f, in[j * width + i] * m1 / m2);
        }
      }
    }
  }
}

static void green_equilibration_favg(
        float *out,
        const float *const in,
        const int width,
        const int height,
        const uint32_t filters,
        const int x,
        const int y)
{
  int oj = 0, oi = 0;
  // const float ratio_max = 1.1f;
  double sum1 = 0.0, sum2 = 0.0, gr_ratio;

  if((FC(oj + y, oi + x, filters) & 1) != 1) oi++;
  const int g2_offset = oi ? -1 : 1;
  dt_iop_image_copy_by_size(out, in, width, height, 1);
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(g2_offset, height, in, width) \
  reduction(+ : sum1, sum2) \
  shared(oi, oj) \
  schedule(static) collapse(2)
#endif
  for(size_t j = oj; j < (height - 1); j += 2)
  {
    for(size_t i = oi; i < (width - 1 - g2_offset); i += 2)
    {
      sum1 += in[j * width + i];
      sum2 += in[(j + 1) * width + i + g2_offset];
    }
  }

  if(sum1 > 0.0 && sum2 > 0.0)
    gr_ratio = sum2 / sum1;
  else
    return;

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(g2_offset, height, in, width) \
  shared(out, oi, oj, gr_ratio) \
  schedule(static) collapse(2)
#endif
  for(int j = oj; j < (height - 1); j += 2)
  {
    for(int i = oi; i < (width - 1 - g2_offset); i += 2)
    {
      out[(size_t)j * width + i] = fmaxf(0.0f, in[(size_t)j * width + i] * gr_ratio);
    }
  }
}


//
// x-trans specific demosaicing algorithms
//

// xtrans_interpolate adapted from dcraw 9.20

// tile size, optimized to keep data in L2 cache
#define TS 122

/** Lookup for allhex[], making sure that row/col aren't negative **/
static inline const short *_hexmap(
        const int row,
        const int col,
        short (*const allhex)[3][8])
{
  // Row and column offsets may be negative, but C's modulo function
  // is not useful here with a negative dividend. To be safe, add a
  // fairly large multiple of 3. In current code row and col will
  // never be less than -9 (1-pass) or -14 (3-pass).
  int irow = row + 600;
  int icol = col + 600;
  assert(irow >= 0 && icol >= 0);
  return allhex[irow % 3][icol % 3];
}

/*
   Frank Markesteijn's algorithm for Fuji X-Trans sensors
*/
static void xtrans_markesteijn_interpolate(
        float *out,
        const float *const in,
        const dt_iop_roi_t *const roi_out,
        const dt_iop_roi_t *const roi_in,
        const uint8_t (*const xtrans)[6],
        const int passes)
{
  static const short orth[12] = { 1, 0, 0, 1, -1, 0, 0, -1, 1, 0, 0, 1 },
                     patt[2][16] = { { 0, 1, 0, -1, 2, 0, -1, 0, 1, 1, 1, -1, 0, 0, 0, 0 },
                                     { 0, 1, 0, -2, 1, 0, -2, 0, 1, 1, -2, -2, 1, -1, -1, 1 } },
                     dir[4] = { 1, TS, TS + 1, TS - 1 };

  short allhex[3][3][8];
  // sgrow/sgcol is the offset in the sensor matrix of the solitary
  // green pixels (initialized here only to avoid compiler warning)
  unsigned short sgrow = 0, sgcol = 0;

  const int width = roi_out->width;
  const int height = roi_out->height;
  const int ndir = 4 << (passes > 1);

  const size_t buffer_size = (size_t)TS * TS * (ndir * 4 + 3) * sizeof(float);
  size_t padded_buffer_size;
  char *const all_buffers = (char *)dt_alloc_perthread(buffer_size, sizeof(char), &padded_buffer_size);
  if(!all_buffers)
  {
    dt_print(DT_DEBUG_ALWAYS, "[demosaic] not able to allocate Markesteijn buffers\n");
    return;
  }

  /* Map a green hexagon around each non-green pixel and vice versa:    */
  for(int row = 0; row < 3; row++)
    for(int col = 0; col < 3; col++)
      for(int ng = 0, d = 0; d < 10; d += 2)
      {
        const int g = FCxtrans(row, col, NULL, xtrans) == 1;
        if(FCxtrans(row + orth[d], col + orth[d + 2], NULL, xtrans) == 1)
          ng = 0;
        else
          ng++;
        // if there are four non-green pixels adjacent in cardinal
        // directions, this is the solitary green pixel
        if(ng == 4)
        {
          sgrow = row;
          sgcol = col;
        }
        if(ng == g + 1)
          for(int c = 0; c < 8; c++)
          {
            const int v = orth[d] * patt[g][c * 2] + orth[d + 1] * patt[g][c * 2 + 1];
            const int h = orth[d + 2] * patt[g][c * 2] + orth[d + 3] * patt[g][c * 2 + 1];
            // offset within TSxTS buffer
            allhex[row][col][c ^ (g * 2 & d)] = h + v * TS;
          }
      }

  // extra passes propagates out errors at edges, hence need more padding
  const int pad_tile = (passes == 1) ? 12 : 17;
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(all_buffers, padded_buffer_size, dir, height, in, ndir, pad_tile, passes, roi_in, width, xtrans) \
  shared(sgrow, sgcol, allhex, out) \
  schedule(static)
#endif
  // step through TSxTS cells of image, each tile overlapping the
  // prior as interpolation needs a substantial border
  for(int top = -pad_tile; top < height - pad_tile; top += TS - (pad_tile*2))
  {
    char *const buffer = dt_get_perthread(all_buffers, padded_buffer_size);
    // rgb points to ndir TSxTS tiles of 3 channels (R, G, and B)
    float(*rgb)[TS][TS][3] = (float(*)[TS][TS][3])buffer;
    // yuv points to 3 channel (Y, u, and v) TSxTS tiles
    // note that channels come before tiles to allow for a
    // vectorization optimization when building drv[] from yuv[]
    float (*const yuv)[TS][TS] = (float(*)[TS][TS])(buffer + TS * TS * (ndir * 3) * sizeof(float));
    // drv points to ndir TSxTS tiles, each a single channel of derivatives
    float (*const drv)[TS][TS] = (float(*)[TS][TS])(buffer + TS * TS * (ndir * 3 + 3) * sizeof(float));
    // gmin and gmax reuse memory which is used later by yuv buffer;
    // each points to a TSxTS tile of single channel data
    float (*const gmin)[TS] = (float(*)[TS])(buffer + TS * TS * (ndir * 3) * sizeof(float));
    float (*const gmax)[TS] = (float(*)[TS])(buffer + TS * TS * (ndir * 3 + 1) * sizeof(float));
    // homo and homosum reuse memory which is used earlier in the
    // loop; each points to ndir single-channel TSxTS tiles
    uint8_t (*const homo)[TS][TS] = (uint8_t(*)[TS][TS])(buffer + TS * TS * (ndir * 3) * sizeof(float));
    uint8_t (*const homosum)[TS][TS] = (uint8_t(*)[TS][TS])(buffer + TS * TS * (ndir * 3) * sizeof(float)
                                                            + TS * TS * ndir * sizeof(uint8_t));

    for(int left = -pad_tile; left < width - pad_tile; left += TS - (pad_tile*2))
    {
      int mrow = MIN(top + TS, height + pad_tile);
      int mcol = MIN(left + TS, width + pad_tile);

      // Copy current tile from in to image buffer. If border goes
      // beyond edges of image, fill with mirrored/interpolated edges.
      // The extra border avoids discontinuities at image edges.
      for(int row = top; row < mrow; row++)
        for(int col = left; col < mcol; col++)
        {
          float(*const pix) = rgb[0][row - top][col - left];
          if((col >= 0) && (row >= 0) && (col < width) && (row < height))
          {
            const int f = FCxtrans(row, col, roi_in, xtrans);
            for(int c = 0; c < 3; c++) pix[c] = (c == f) ? in[roi_in->width * row + col] : 0.f;
          }
          else
          {
            // mirror a border pixel if beyond image edge
            const int c = FCxtrans(row, col, roi_in, xtrans);
            for(int cc = 0; cc < 3; cc++)
            {
              if(cc != c)
                pix[cc] = 0.0f;
              else
              {
#define TRANSLATE(n, size) ((n >= size) ? (2 * size - n - 2) : abs(n))
                const int cy = TRANSLATE(row, height), cx = TRANSLATE(col, width);
                if(c == FCxtrans(cy, cx, roi_in, xtrans))
                  pix[c] = in[roi_in->width * cy + cx];
                else
                {
                  // interpolate if mirror pixel is a different color
                  float sum = 0.0f;
                  uint8_t count = 0;
                  for(int y = row - 1; y <= row + 1; y++)
                    for(int x = col - 1; x <= col + 1; x++)
                    {
                      const int yy = TRANSLATE(y, height), xx = TRANSLATE(x, width);
                      const int ff = FCxtrans(yy, xx, roi_in, xtrans);
                      if(ff == c)
                      {
                        sum += in[roi_in->width * yy + xx];
                        count++;
                      }
                    }
                  pix[c] = sum / count;
                }
              }
            }
          }
        }

      // duplicate rgb[0] to rgb[1], rgb[2], and rgb[3]
      for(int c = 1; c <= 3; c++) memcpy(rgb[c], rgb[0], sizeof(*rgb));

      // note that successive calculations are inset within the tile
      // so as to give enough border data, and there needs to be a 6
      // pixel border initially to allow allhex to find neighboring
      // pixels

      /* Set green1 and green3 to the minimum and maximum allowed values:   */
      // Run through each red/blue or blue/red pair, setting their g1
      // and g3 values to the min/max of green pixels surrounding the
      // pair. Use a 3 pixel border as gmin/gmax is used by
      // interpolate green which has a 3 pixel border.
      const int pad_g1_g3 = 3;
      for(int row = top + pad_g1_g3; row < mrow - pad_g1_g3; row++)
      {
        // setting max to 0.0f signifies that this is a new pair, which
        // requires a new min/max calculation of its neighboring greens
        float min = FLT_MAX, max = 0.0f;
        for(int col = left + pad_g1_g3; col < mcol - pad_g1_g3; col++)
        {
          // if in row of horizontal red & blue pairs (or processing
          // vertical red & blue pairs near image bottom), reset min/max
          // between each pair
          if(FCxtrans(row, col, roi_in, xtrans) == 1)
          {
            min = FLT_MAX, max = 0.0f;
            continue;
          }
          // if at start of red & blue pair, calculate min/max of green
          // pixels surrounding it; note that while normally using == to
          // compare floats is suspect, here the check is if 0.0f has
          // explicitly been assigned to max (which signifies a new
          // red/blue pair)
          if(max == 0.0f)
          {
            float (*const pix)[3] = &rgb[0][row - top][col - left];
            const short *const hex = _hexmap(row,col,allhex);
            for(int c = 0; c < 6; c++)
            {
              const float val = pix[hex[c]][1];
              if(min > val) min = val;
              if(max < val) max = val;
            }
          }
          gmin[row - top][col - left] = min;
          gmax[row - top][col - left] = max;
          // handle vertical red/blue pairs
          switch((row - sgrow) % 3)
          {
            // hop down a row to second pixel in vertical pair
            case 1:
              if(row < mrow - 4) row++, col--;
              break;
            // then if not done with the row hop up and right to next
            // vertical red/blue pair, resetting min/max
            case 2:
              min = FLT_MAX, max = 0.0f;
              if((col += 2) < mcol - 4 && row > top + 3) row--;
          }
        }
      }

      /* Interpolate green horizontally, vertically, and along both diagonals: */
      // need a 3 pixel border here as 3*hex[] can have a 3 unit offset
      const int pad_g_interp = 3;
      for(int row = top + pad_g_interp; row < mrow - pad_g_interp; row++)
        for(int col = left + pad_g_interp; col < mcol - pad_g_interp; col++)
        {
          float color[8];
          const int f = FCxtrans(row, col, roi_in, xtrans);
          if(f == 1) continue;
          float (*const pix)[3] = &rgb[0][row - top][col - left];
          const short *const hex = _hexmap(row,col,allhex);
          // TODO: these constants come from integer math constants in
          // dcraw -- calculate them instead from interpolation math
          color[0] = 0.6796875f * (pix[hex[1]][1] + pix[hex[0]][1])
                     - 0.1796875f * (pix[2 * hex[1]][1] + pix[2 * hex[0]][1]);
          color[1] = 0.87109375f * pix[hex[3]][1] + pix[hex[2]][1] * 0.13f
                     + 0.359375f * (pix[0][f] - pix[-hex[2]][f]);
          for(int c = 0; c < 2; c++)
            color[2 + c] = 0.640625f * pix[hex[4 + c]][1] + 0.359375f * pix[-2 * hex[4 + c]][1]
                           + 0.12890625f * (2 * pix[0][f] - pix[3 * hex[4 + c]][f] - pix[-3 * hex[4 + c]][f]);
          for(int c = 0; c < 4; c++)
            rgb[c ^ !((row - sgrow) % 3)][row - top][col - left][1]
                = CLAMPS(color[c], gmin[row - top][col - left], gmax[row - top][col - left]);
        }

      for(int pass = 0; pass < passes; pass++)
      {
        if(pass == 1)
        {
          // if on second pass, copy rgb[0] to [3] into rgb[4] to [7],
          // and process that second set of buffers
          memcpy(rgb + 4, rgb, sizeof(*rgb) * 4);
          rgb += 4;
        }

        /* Recalculate green from interpolated values of closer pixels: */
        if(pass)
        {
          const int pad_g_recalc = 6;
          for(int row = top + pad_g_recalc; row < mrow - pad_g_recalc; row++)
            for(int col = left + pad_g_recalc; col < mcol - pad_g_recalc; col++)
            {
              const int f = FCxtrans(row, col, roi_in, xtrans);
              if(f == 1) continue;
              const short *const hex = _hexmap(row,col,allhex);
              for(int d = 3; d < 6; d++)
              {
                float(*rfx)[3] = &rgb[(d - 2) ^ !((row - sgrow) % 3)][row - top][col - left];
                const float val = rfx[-2 * hex[d]][1]
                            + 2 * rfx[hex[d]][1] - rfx[-2 * hex[d]][f]
                            - 2 * rfx[hex[d]][f] + 3 * rfx[0][f];
                rfx[0][1] = CLAMPS(val / 3.0f, gmin[row - top][col - left], gmax[row - top][col - left]);
              }
            }
        }

        /* Interpolate red and blue values for solitary green pixels:   */
        const int pad_rb_g = (passes == 1) ? 6 : 5;
        for(int row = (top - sgrow + pad_rb_g + 2) / 3 * 3 + sgrow; row < mrow - pad_rb_g; row += 3)
          for(int col = (left - sgcol + pad_rb_g + 2) / 3 * 3 + sgcol; col < mcol - pad_rb_g; col += 3)
          {
            float(*rfx)[3] = &rgb[0][row - top][col - left];
            int h = FCxtrans(row, col + 1, roi_in, xtrans);
            float diff[6] = { 0.0f };
            // interplated color: first index is red/blue, second is
            // pass, is double actual result
            float color[2][6];
            // Six passes, alternating hori/vert interp (i),
            // starting with R or B (h) depending on which is closest.
            // Passes 0,1 to rgb[0], rgb[1] of hori/vert interp. Pass
            // 3,5 to rgb[2], rgb[3] of best of interp hori/vert
            // results. Each pass which outputs moves on to the next
            // rgb[] for input of interp greens.
            for(int i = 1, d = 0; d < 6; d++, i ^= TS ^ 1, h ^= 2)
            {
              // look 1 and 2 pixels distance from solitary green to
              // red then blue or blue then red
              for(int c = 0; c < 2; c++, h ^= 2)
              {
                // rate of change in greens between current pixel and
                // interpolated pixels 1 or 2 distant: a quick
                // derivative which will be divided by two later to be
                // rate of luminance change for red/blue between known
                // red/blue neighbors and the current unknown pixel
                const float g = 2 * rfx[0][1] - rfx[i << c][1] - rfx[-(i << c)][1];
                // color is halved before being stored in rgb, hence
                // this becomes green rate of change plus the average
                // of the near red or blue pixels on current axis
                color[h != 0][d] = g + rfx[i << c][h] + rfx[-(i << c)][h];
                // Note that diff will become the slope for both red
                // and blue differentials in the current direction.
                // For 2nd and 3rd hori+vert passes, create a sum of
                // steepness for both cardinal directions.
                if(d > 1)
                  diff[d] += sqrf(rfx[i << c][1] - rfx[-(i << c)][1] - rfx[i << c][h] + rfx[-(i << c)][h])
                             + sqrf(g);
              }
              if((d < 2) || (d & 1))
              { // output for passes 0, 1, 3, 5
                // for 0, 1 just use hori/vert, for 3, 5 use best of x/y dir
                const int d_out = d - ((d > 1) && (diff[d-1] < diff[d]));
                rfx[0][0] = color[0][d_out] / 2.f;
                rfx[0][2] = color[1][d_out] / 2.f;
                rfx += TS * TS;
              }
            }
          }

        /* Interpolate red for blue pixels and vice versa:              */
        const int pad_rb_br = (passes == 1) ? 6 : 5;
        for(int row = top + pad_rb_br; row < mrow - pad_rb_br; row++)
          for(int col = left + pad_rb_br; col < mcol - pad_rb_br; col++)
          {
            const int f = 2 - FCxtrans(row, col, roi_in, xtrans);
            if(f == 1) continue;
            float(*rfx)[3] = &rgb[0][row - top][col - left];
            const int c = (row - sgrow) % 3 ? TS : 1;
            const int h = 3 * (c ^ TS ^ 1);
            for(int d = 0; d < 4; d++, rfx += TS * TS)
            {
              const int i = d > 1 || ((d ^ c) & 1) ||
                ((fabsf(rfx[0][1]-rfx[c][1]) + fabsf(rfx[0][1]-rfx[-c][1])) <
                 2.f*(fabsf(rfx[0][1]-rfx[h][1]) + fabsf(rfx[0][1]-rfx[-h][1]))) ? c:h;
              rfx[0][f] = (rfx[i][f] + rfx[-i][f] + 2.f * rfx[0][1] - rfx[i][1] - rfx[-i][1]) / 2.f;
            }
          }

        /* Fill in red and blue for 2x2 blocks of green:                */
        const int pad_g22 = (passes == 1) ? 8 : 4;
        for(int row = top + pad_g22; row < mrow - pad_g22; row++)
        {
          if((row - sgrow) % 3)
            for(int col = left + pad_g22; col < mcol - pad_g22; col++)
              if((col - sgcol) % 3)
              {
                float(*rfx)[3] = &rgb[0][row - top][col - left];
                const short *const hex = _hexmap(row,col,allhex);
                for(int d = 0; d < ndir; d += 2, rfx += TS * TS)
                  if(hex[d] + hex[d + 1])
                  {
                    const float g = 3.f * rfx[0][1] - 2.f * rfx[hex[d]][1] - rfx[hex[d + 1]][1];
                    for(int c = 0; c < 4; c += 2)
                      rfx[0][c] = (g + 2.f * rfx[hex[d]][c] + rfx[hex[d + 1]][c]) / 3.f;
                  }
                  else
                  {
                    const float g = 2.f * rfx[0][1] - rfx[hex[d]][1] - rfx[hex[d + 1]][1];
                    for(int c = 0; c < 4; c += 2)
                      rfx[0][c] = (g + rfx[hex[d]][c] + rfx[hex[d + 1]][c]) / 2.f;
                  }
              }
        }
      } // end of multipass loop

      // jump back to the first set of rgb buffers (this is a nop
      // unless on the second pass)
      rgb = (float(*)[TS][TS][3])buffer;
      // from here on out, mainly are working within the current tile
      // rather than in reference to the image, so don't offset
      // mrow/mcol by top/left of tile
      mrow -= top;
      mcol -= left;

      /* Convert to perceptual colorspace and differentiate in all directions:  */
      // Original dcraw algorithm uses CIELab as perceptual space
      // (presumably coming from original AHD) and converts taking
      // camera matrix into account. Now use YPbPr which requires much
      // less code and is nearly indistinguishable. It assumes the
      // camera RGB is roughly linear.
      for(int d = 0; d < ndir; d++)
      {
        const int pad_yuv = (passes == 1) ? 8 : 13;
        for(int row = pad_yuv; row < mrow - pad_yuv; row++)
          for(int col = pad_yuv; col < mcol - pad_yuv; col++)
          {
            const float *rx = rgb[d][row][col];
            // use ITU-R BT.2020 YPbPr, which is great, but could use
            // a better/simpler choice? note that imageop.h provides
            // dt_iop_RGB_to_YCbCr which uses Rec. 601 conversion,
            // which appears less good with specular highlights
            const float y = 0.2627f * rx[0] + 0.6780f * rx[1] + 0.0593f * rx[2];
            yuv[0][row][col] = y;
            yuv[1][row][col] = (rx[2] - y) * 0.56433f;
            yuv[2][row][col] = (rx[0] - y) * 0.67815f;
          }
        // Note that f can offset by a column (-1 or +1) and by a row
        // (-TS or TS). The row-wise offsets cause the undefined
        // behavior sanitizer to warn of an out of bounds index, but
        // as yfx is multi-dimensional and there is sufficient
        // padding, that is not actually so.
        const int f = dir[d & 3];
        const int pad_drv = (passes == 1) ? 9 : 14;
        for(int row = pad_drv; row < mrow - pad_drv; row++)
          for(int col = pad_drv; col < mcol - pad_drv; col++)
          {
            const float(*yfx)[TS][TS] = (float(*)[TS][TS]) & yuv[0][row][col];
            drv[d][row][col] = sqrf(2 * yfx[0][0][0] - yfx[0][0][f] - yfx[0][0][-f])
                               + sqrf(2 * yfx[1][0][0] - yfx[1][0][f] - yfx[1][0][-f])
                               + sqrf(2 * yfx[2][0][0] - yfx[2][0][f] - yfx[2][0][-f]);
          }
      }

      /* Build homogeneity maps from the derivatives:                   */
      memset(homo, 0, sizeof(uint8_t) * ndir * TS * TS);
      const int pad_homo = (passes == 1) ? 10 : 15;
      for(int row = pad_homo; row < mrow - pad_homo; row++)
        for(int col = pad_homo; col < mcol - pad_homo; col++)
        {
          float tr = FLT_MAX;
          for(int d = 0; d < ndir; d++)
            if(tr > drv[d][row][col]) tr = drv[d][row][col];
          tr *= 8;
          for(int d = 0; d < ndir; d++)
            for(int v = -1; v <= 1; v++)
              for(int h = -1; h <= 1; h++)
                homo[d][row][col] += ((drv[d][row + v][col + h] <= tr) ? 1 : 0);
        }

      /* Build 5x5 sum of homogeneity maps for each pixel & direction */
      for(int d = 0; d < ndir; d++)
        for(int row = pad_tile; row < mrow - pad_tile; row++)
        {
          // start before first column where homo[d][row][col+2] != 0,
          // so can know v5sum and homosum[d][row][col] will be 0
          int col = pad_tile-5;
          uint8_t v5sum[5] = { 0 };
          homosum[d][row][col] = 0;
          // calculate by rolling through column sums
          for(col++; col < mcol - pad_tile; col++)
          {
            uint8_t colsum = 0;
            for(int v = -2; v <= 2; v++) colsum += homo[d][row + v][col + 2];
            homosum[d][row][col] = homosum[d][row][col - 1] - v5sum[col % 5] + colsum;
            v5sum[col % 5] = colsum;
          }
        }

      /* Average the most homogeneous pixels for the final result:       */
      for(int row = pad_tile; row < mrow - pad_tile; row++)
        for(int col = pad_tile; col < mcol - pad_tile; col++)
        {
          uint8_t hm[8] = { 0 };
          uint8_t maxval = 0;
          for(int d = 0; d < ndir; d++)
          {
            hm[d] = homosum[d][row][col];
            maxval = (maxval < hm[d] ? hm[d] : maxval);
          }
          maxval -= maxval >> 3;
          for(int d = 0; d < ndir - 4; d++)
          {
            if(hm[d] < hm[d + 4])
              hm[d] = 0;
            else if(hm[d] > hm[d + 4])
              hm[d + 4] = 0;
          }
          dt_aligned_pixel_t avg = { 0.0f };
          for(int d = 0; d < ndir; d++)
          {
            if(hm[d] >= maxval)
            {
              for(int c = 0; c < 3; c++) avg[c] += rgb[d][row][col][c];
              avg[3]++;
            }
          }
          for(int c = 0; c < 3; c++)
            out[4 * (width * (row + top) + col + left) + c] = avg[c]/avg[3];
        }
    }
  }
  dt_free_align(all_buffers);
}

#undef TS

#define TS 122
static void xtrans_fdc_interpolate(
        struct dt_iop_module_t *self,
        float *out,
        const float *const in,
        const dt_iop_roi_t *const roi_out,
        const dt_iop_roi_t *const roi_in,
        const uint8_t (*const xtrans)[6])
{
  static const short orth[12] = { 1, 0, 0, 1, -1, 0, 0, -1, 1, 0, 0, 1 },
                     patt[2][16] = { { 0, 1, 0, -1, 2, 0, -1, 0, 1, 1, 1, -1, 0, 0, 0, 0 },
                                     { 0, 1, 0, -2, 1, 0, -2, 0, 1, 1, -2, -2, 1, -1, -1, 1 } },
                     dir[4] = { 1, TS, TS + 1, TS - 1 };

  static const float directionality[8] = { 1.0f, 0.0f, 0.5f, 0.5f, 1.0f, 0.0f, 0.5f, 0.5f };

  short allhex[3][3][8];
  // sgrow/sgcol is the offset in the sensor matrix of the solitary
  // green pixels (initialized here only to avoid compiler warning)
  unsigned short sgrow = 0, sgcol = 0;

  const int width = roi_out->width;
  const int height = roi_out->height;
  static const int ndir = 4;

  static const float complex Minv[3][8] = {
    { 1.000000e+00f, 2.500000e-01f - 4.330127e-01f * _Complex_I, -2.500000e-01f - 4.330127e-01f * _Complex_I,
      -1.000000e+00f, 7.500000e-01f - 1.299038e+00f * _Complex_I, -2.500000e-01f + 4.330127e-01f * _Complex_I,
      7.500000e-01f + 1.299038e+00f * _Complex_I, 2.500000e-01f + 4.330127e-01f * _Complex_I },
    { 1.000000e+00f, -2.000000e-01f + 3.464102e-01f * _Complex_I, 2.000000e-01f + 3.464102e-01f * _Complex_I,
      8.000000e-01f, 0.0f, 2.000000e-01f - 3.464102e-01f * _Complex_I, 0.0f,
      -2.000000e-01f - 3.464102e-01f * _Complex_I },
    { 1.000000e+00f, 2.500000e-01f - 4.330127e-01f * _Complex_I, -2.500000e-01f - 4.330127e-01f * _Complex_I,
      -1.000000e+00f, -7.500000e-01f + 1.299038e+00f * _Complex_I, -2.500000e-01f + 4.330127e-01f * _Complex_I,
      -7.500000e-01f - 1.299038e+00f * _Complex_I, 2.500000e-01f + 4.330127e-01f * _Complex_I },
  };

  static const float complex modarr[6][6][8] = {
    { { 1.000000e+00f + 0.000000e+00f * _Complex_I, 1.000000e+00f + 0.000000e+00f * _Complex_I,
        1.000000e+00f + 0.000000e+00f * _Complex_I, 1.000000e+00f + 0.000000e+00f * _Complex_I,
        1.000000e+00f + 0.000000e+00f * _Complex_I, 1.000000e+00f + 0.000000e+00f * _Complex_I,
        1.000000e+00f + 0.000000e+00f * _Complex_I, 1.000000e+00f + 0.000000e+00f * _Complex_I },
      { -1.000000e+00f - 1.224647e-16f * _Complex_I, 5.000000e-01f + 8.660254e-01f * _Complex_I,
        -1.000000e+00f - 1.224647e-16f * _Complex_I, 5.000000e-01f - 8.660254e-01f * _Complex_I,
        -5.000000e-01f + 8.660254e-01f * _Complex_I, 1.000000e+00f + 0.000000e+00f * _Complex_I,
        -5.000000e-01f + 8.660254e-01f * _Complex_I, -5.000000e-01f - 8.660254e-01f * _Complex_I },
      { 1.000000e+00f + 2.449294e-16f * _Complex_I, -5.000000e-01f + 8.660254e-01f * _Complex_I,
        1.000000e+00f + 2.449294e-16f * _Complex_I, -5.000000e-01f - 8.660254e-01f * _Complex_I,
        -5.000000e-01f - 8.660254e-01f * _Complex_I, 1.000000e+00f + 0.000000e+00f * _Complex_I,
        -5.000000e-01f - 8.660254e-01f * _Complex_I, -5.000000e-01f + 8.660254e-01f * _Complex_I },
      { -1.000000e+00f - 3.673940e-16f * _Complex_I, -1.000000e+00f + 1.224647e-16f * _Complex_I,
        -1.000000e+00f - 3.673940e-16f * _Complex_I, -1.000000e+00f - 1.224647e-16f * _Complex_I,
        1.000000e+00f - 2.449294e-16f * _Complex_I, 1.000000e+00f + 0.000000e+00f * _Complex_I,
        1.000000e+00f - 2.449294e-16f * _Complex_I, 1.000000e+00f + 2.449294e-16f * _Complex_I },
      { 1.000000e+00f + 4.898587e-16f * _Complex_I, -5.000000e-01f - 8.660254e-01f * _Complex_I,
        1.000000e+00f + 4.898587e-16f * _Complex_I, -5.000000e-01f + 8.660254e-01f * _Complex_I,
        -5.000000e-01f + 8.660254e-01f * _Complex_I, 1.000000e+00f + 0.000000e+00f * _Complex_I,
        -5.000000e-01f + 8.660254e-01f * _Complex_I, -5.000000e-01f - 8.660254e-01f * _Complex_I },
      { -1.000000e+00f - 6.123234e-16f * _Complex_I, 5.000000e-01f - 8.660254e-01f * _Complex_I,
        -1.000000e+00f - 6.123234e-16f * _Complex_I, 5.000000e-01f + 8.660254e-01f * _Complex_I,
        -5.000000e-01f - 8.660254e-01f * _Complex_I, 1.000000e+00f + 0.000000e+00f * _Complex_I,
        -5.000000e-01f - 8.660254e-01f * _Complex_I, -5.000000e-01f + 8.660254e-01f * _Complex_I } },
    { { 5.000000e-01f + 8.660254e-01f * _Complex_I, -1.000000e+00f + 1.224647e-16f * _Complex_I,
        5.000000e-01f - 8.660254e-01f * _Complex_I, -1.000000e+00f + 1.224647e-16f * _Complex_I,
        1.000000e+00f + 0.000000e+00f * _Complex_I, -5.000000e-01f + 8.660254e-01f * _Complex_I,
        -5.000000e-01f + 8.660254e-01f * _Complex_I, -5.000000e-01f + 8.660254e-01f * _Complex_I },
      { -5.000000e-01f - 8.660254e-01f * _Complex_I, -5.000000e-01f - 8.660254e-01f * _Complex_I,
        -5.000000e-01f + 8.660254e-01f * _Complex_I, -5.000000e-01f + 8.660254e-01f * _Complex_I,
        -5.000000e-01f + 8.660254e-01f * _Complex_I, -5.000000e-01f + 8.660254e-01f * _Complex_I,
        -5.000000e-01f - 8.660254e-01f * _Complex_I, 1.000000e+00f + 0.000000e+00f * _Complex_I },
      { 5.000000e-01f + 8.660254e-01f * _Complex_I, 5.000000e-01f - 8.660254e-01f * _Complex_I,
        5.000000e-01f - 8.660254e-01f * _Complex_I, 5.000000e-01f + 8.660254e-01f * _Complex_I,
        -5.000000e-01f - 8.660254e-01f * _Complex_I, -5.000000e-01f + 8.660254e-01f * _Complex_I,
        1.000000e+00f - 2.449294e-16f * _Complex_I, -5.000000e-01f - 8.660254e-01f * _Complex_I },
      { -5.000000e-01f - 8.660254e-01f * _Complex_I, 1.000000e+00f - 2.449294e-16f * _Complex_I,
        -5.000000e-01f + 8.660254e-01f * _Complex_I, 1.000000e+00f + 0.000000e+00f * _Complex_I,
        1.000000e+00f - 2.449294e-16f * _Complex_I, -5.000000e-01f + 8.660254e-01f * _Complex_I,
        -5.000000e-01f + 8.660254e-01f * _Complex_I, -5.000000e-01f + 8.660254e-01f * _Complex_I },
      { 5.000000e-01f + 8.660254e-01f * _Complex_I, 5.000000e-01f + 8.660254e-01f * _Complex_I,
        5.000000e-01f - 8.660254e-01f * _Complex_I, 5.000000e-01f - 8.660254e-01f * _Complex_I,
        -5.000000e-01f + 8.660254e-01f * _Complex_I, -5.000000e-01f + 8.660254e-01f * _Complex_I,
        -5.000000e-01f - 8.660254e-01f * _Complex_I, 1.000000e+00f + 2.449294e-16f * _Complex_I },
      { -5.000000e-01f - 8.660254e-01f * _Complex_I, -5.000000e-01f + 8.660254e-01f * _Complex_I,
        -5.000000e-01f + 8.660254e-01f * _Complex_I, -5.000000e-01f - 8.660254e-01f * _Complex_I,
        -5.000000e-01f - 8.660254e-01f * _Complex_I, -5.000000e-01f + 8.660254e-01f * _Complex_I,
        1.000000e+00f - 2.266216e-15f * _Complex_I, -5.000000e-01f - 8.660254e-01f * _Complex_I } },
    { { -5.000000e-01f + 8.660254e-01f * _Complex_I, 1.000000e+00f - 2.449294e-16f * _Complex_I,
        -5.000000e-01f - 8.660254e-01f * _Complex_I, 1.000000e+00f - 2.449294e-16f * _Complex_I,
        1.000000e+00f + 0.000000e+00f * _Complex_I, -5.000000e-01f - 8.660254e-01f * _Complex_I,
        -5.000000e-01f - 8.660254e-01f * _Complex_I, -5.000000e-01f - 8.660254e-01f * _Complex_I },
      { 5.000000e-01f - 8.660254e-01f * _Complex_I, 5.000000e-01f + 8.660254e-01f * _Complex_I,
        5.000000e-01f + 8.660254e-01f * _Complex_I, 5.000000e-01f - 8.660254e-01f * _Complex_I,
        -5.000000e-01f + 8.660254e-01f * _Complex_I, -5.000000e-01f - 8.660254e-01f * _Complex_I,
        1.000000e+00f - 2.449294e-16f * _Complex_I, -5.000000e-01f + 8.660254e-01f * _Complex_I },
      { -5.000000e-01f + 8.660254e-01f * _Complex_I, -5.000000e-01f + 8.660254e-01f * _Complex_I,
        -5.000000e-01f - 8.660254e-01f * _Complex_I, -5.000000e-01f - 8.660254e-01f * _Complex_I,
        -5.000000e-01f - 8.660254e-01f * _Complex_I, -5.000000e-01f - 8.660254e-01f * _Complex_I,
        -5.000000e-01f + 8.660254e-01f * _Complex_I, 1.000000e+00f + 0.000000e+00f * _Complex_I },
      { 5.000000e-01f - 8.660254e-01f * _Complex_I, -1.000000e+00f + 3.673940e-16f * _Complex_I,
        5.000000e-01f + 8.660254e-01f * _Complex_I, -1.000000e+00f + 1.224647e-16f * _Complex_I,
        1.000000e+00f - 2.449294e-16f * _Complex_I, -5.000000e-01f - 8.660254e-01f * _Complex_I,
        -5.000000e-01f - 8.660254e-01f * _Complex_I, -5.000000e-01f - 8.660254e-01f * _Complex_I },
      { -5.000000e-01f + 8.660254e-01f * _Complex_I, -5.000000e-01f - 8.660254e-01f * _Complex_I,
        -5.000000e-01f - 8.660254e-01f * _Complex_I, -5.000000e-01f + 8.660254e-01f * _Complex_I,
        -5.000000e-01f + 8.660254e-01f * _Complex_I, -5.000000e-01f - 8.660254e-01f * _Complex_I,
        1.000000e+00f - 4.898587e-16f * _Complex_I, -5.000000e-01f + 8.660254e-01f * _Complex_I },
      { 5.000000e-01f - 8.660254e-01f * _Complex_I, 5.000000e-01f - 8.660254e-01f * _Complex_I,
        5.000000e-01f + 8.660254e-01f * _Complex_I, 5.000000e-01f + 8.660254e-01f * _Complex_I,
        -5.000000e-01f - 8.660254e-01f * _Complex_I, -5.000000e-01f - 8.660254e-01f * _Complex_I,
        -5.000000e-01f + 8.660254e-01f * _Complex_I, 1.000000e+00f + 1.133108e-15f * _Complex_I } },
    { { -1.000000e+00f + 1.224647e-16f * _Complex_I, -1.000000e+00f + 3.673940e-16f * _Complex_I,
        -1.000000e+00f - 1.224647e-16f * _Complex_I, -1.000000e+00f + 3.673940e-16f * _Complex_I,
        1.000000e+00f + 0.000000e+00f * _Complex_I, 1.000000e+00f - 2.449294e-16f * _Complex_I,
        1.000000e+00f - 2.449294e-16f * _Complex_I, 1.000000e+00f - 2.449294e-16f * _Complex_I },
      { 1.000000e+00f + 0.000000e+00f * _Complex_I, -5.000000e-01f - 8.660254e-01f * _Complex_I,
        1.000000e+00f + 2.449294e-16f * _Complex_I, -5.000000e-01f + 8.660254e-01f * _Complex_I,
        -5.000000e-01f + 8.660254e-01f * _Complex_I, 1.000000e+00f - 2.449294e-16f * _Complex_I,
        -5.000000e-01f + 8.660254e-01f * _Complex_I, -5.000000e-01f - 8.660254e-01f * _Complex_I },
      { -1.000000e+00f - 1.224647e-16f * _Complex_I, 5.000000e-01f - 8.660254e-01f * _Complex_I,
        -1.000000e+00f - 3.673940e-16f * _Complex_I, 5.000000e-01f + 8.660254e-01f * _Complex_I,
        -5.000000e-01f - 8.660254e-01f * _Complex_I, 1.000000e+00f - 2.449294e-16f * _Complex_I,
        -5.000000e-01f - 8.660254e-01f * _Complex_I, -5.000000e-01f + 8.660254e-01f * _Complex_I },
      { 1.000000e+00f + 2.449294e-16f * _Complex_I, 1.000000e+00f - 4.898587e-16f * _Complex_I,
        1.000000e+00f + 4.898587e-16f * _Complex_I, 1.000000e+00f - 2.449294e-16f * _Complex_I,
        1.000000e+00f - 2.449294e-16f * _Complex_I, 1.000000e+00f - 2.449294e-16f * _Complex_I,
        1.000000e+00f - 4.898587e-16f * _Complex_I, 1.000000e+00f + 0.000000e+00f * _Complex_I },
      { -1.000000e+00f - 3.673940e-16f * _Complex_I, 5.000000e-01f + 8.660254e-01f * _Complex_I,
        -1.000000e+00f - 6.123234e-16f * _Complex_I, 5.000000e-01f - 8.660254e-01f * _Complex_I,
        -5.000000e-01f + 8.660254e-01f * _Complex_I, 1.000000e+00f - 2.449294e-16f * _Complex_I,
        -5.000000e-01f + 8.660254e-01f * _Complex_I, -5.000000e-01f - 8.660254e-01f * _Complex_I },
      { 1.000000e+00f + 4.898587e-16f * _Complex_I, -5.000000e-01f + 8.660254e-01f * _Complex_I,
        1.000000e+00f + 7.347881e-16f * _Complex_I, -5.000000e-01f - 8.660254e-01f * _Complex_I,
        -5.000000e-01f - 8.660254e-01f * _Complex_I, 1.000000e+00f - 2.449294e-16f * _Complex_I,
        -5.000000e-01f - 8.660254e-01f * _Complex_I, -5.000000e-01f + 8.660254e-01f * _Complex_I } },
    { { -5.000000e-01f - 8.660254e-01f * _Complex_I, 1.000000e+00f - 4.898587e-16f * _Complex_I,
        -5.000000e-01f + 8.660254e-01f * _Complex_I, 1.000000e+00f - 4.898587e-16f * _Complex_I,
        1.000000e+00f + 0.000000e+00f * _Complex_I, -5.000000e-01f + 8.660254e-01f * _Complex_I,
        -5.000000e-01f + 8.660254e-01f * _Complex_I, -5.000000e-01f + 8.660254e-01f * _Complex_I },
      { 5.000000e-01f + 8.660254e-01f * _Complex_I, 5.000000e-01f + 8.660254e-01f * _Complex_I,
        5.000000e-01f - 8.660254e-01f * _Complex_I, 5.000000e-01f - 8.660254e-01f * _Complex_I,
        -5.000000e-01f + 8.660254e-01f * _Complex_I, -5.000000e-01f + 8.660254e-01f * _Complex_I,
        -5.000000e-01f - 8.660254e-01f * _Complex_I, 1.000000e+00f - 2.449294e-16f * _Complex_I },
      { -5.000000e-01f - 8.660254e-01f * _Complex_I, -5.000000e-01f + 8.660254e-01f * _Complex_I,
        -5.000000e-01f + 8.660254e-01f * _Complex_I, -5.000000e-01f - 8.660254e-01f * _Complex_I,
        -5.000000e-01f - 8.660254e-01f * _Complex_I, -5.000000e-01f + 8.660254e-01f * _Complex_I,
        1.000000e+00f - 4.898587e-16f * _Complex_I, -5.000000e-01f - 8.660254e-01f * _Complex_I },
      { 5.000000e-01f + 8.660254e-01f * _Complex_I, -1.000000e+00f + 6.123234e-16f * _Complex_I,
        5.000000e-01f - 8.660254e-01f * _Complex_I, -1.000000e+00f + 3.673940e-16f * _Complex_I,
        1.000000e+00f - 2.449294e-16f * _Complex_I, -5.000000e-01f + 8.660254e-01f * _Complex_I,
        -5.000000e-01f + 8.660254e-01f * _Complex_I, -5.000000e-01f + 8.660254e-01f * _Complex_I },
      { -5.000000e-01f - 8.660254e-01f * _Complex_I, -5.000000e-01f - 8.660254e-01f * _Complex_I,
        -5.000000e-01f + 8.660254e-01f * _Complex_I, -5.000000e-01f + 8.660254e-01f * _Complex_I,
        -5.000000e-01f + 8.660254e-01f * _Complex_I, -5.000000e-01f + 8.660254e-01f * _Complex_I,
        -5.000000e-01f - 8.660254e-01f * _Complex_I, 1.000000e+00f + 0.000000e+00f * _Complex_I },
      { 5.000000e-01f + 8.660254e-01f * _Complex_I, 5.000000e-01f - 8.660254e-01f * _Complex_I,
        5.000000e-01f - 8.660254e-01f * _Complex_I, 5.000000e-01f + 8.660254e-01f * _Complex_I,
        -5.000000e-01f - 8.660254e-01f * _Complex_I, -5.000000e-01f + 8.660254e-01f * _Complex_I,
        1.000000e+00f - 7.347881e-16f * _Complex_I, -5.000000e-01f - 8.660254e-01f * _Complex_I } },
    { { 5.000000e-01f - 8.660254e-01f * _Complex_I, -1.000000e+00f + 6.123234e-16f * _Complex_I,
        5.000000e-01f + 8.660254e-01f * _Complex_I, -1.000000e+00f + 6.123234e-16f * _Complex_I,
        1.000000e+00f + 0.000000e+00f * _Complex_I, -5.000000e-01f - 8.660254e-01f * _Complex_I,
        -5.000000e-01f - 8.660254e-01f * _Complex_I, -5.000000e-01f - 8.660254e-01f * _Complex_I },
      { -5.000000e-01f + 8.660254e-01f * _Complex_I, -5.000000e-01f - 8.660254e-01f * _Complex_I,
        -5.000000e-01f - 8.660254e-01f * _Complex_I, -5.000000e-01f + 8.660254e-01f * _Complex_I,
        -5.000000e-01f + 8.660254e-01f * _Complex_I, -5.000000e-01f - 8.660254e-01f * _Complex_I,
        1.000000e+00f - 2.266216e-15f * _Complex_I, -5.000000e-01f + 8.660254e-01f * _Complex_I },
      { 5.000000e-01f - 8.660254e-01f * _Complex_I, 5.000000e-01f - 8.660254e-01f * _Complex_I,
        5.000000e-01f + 8.660254e-01f * _Complex_I, 5.000000e-01f + 8.660254e-01f * _Complex_I,
        -5.000000e-01f - 8.660254e-01f * _Complex_I, -5.000000e-01f - 8.660254e-01f * _Complex_I,
        -5.000000e-01f + 8.660254e-01f * _Complex_I, 1.000000e+00f - 1.133108e-15f * _Complex_I },
      { -5.000000e-01f + 8.660254e-01f * _Complex_I, 1.000000e+00f - 7.347881e-16f * _Complex_I,
        -5.000000e-01f - 8.660254e-01f * _Complex_I, 1.000000e+00f - 4.898587e-16f * _Complex_I,
        1.000000e+00f - 2.449294e-16f * _Complex_I, -5.000000e-01f - 8.660254e-01f * _Complex_I,
        -5.000000e-01f - 8.660254e-01f * _Complex_I, -5.000000e-01f - 8.660254e-01f * _Complex_I },
      { 5.000000e-01f - 8.660254e-01f * _Complex_I, 5.000000e-01f + 8.660254e-01f * _Complex_I,
        5.000000e-01f + 8.660254e-01f * _Complex_I, 5.000000e-01f - 8.660254e-01f * _Complex_I,
        -5.000000e-01f + 8.660254e-01f * _Complex_I, -5.000000e-01f - 8.660254e-01f * _Complex_I,
        1.000000e+00f - 7.347881e-16f * _Complex_I, -5.000000e-01f + 8.660254e-01f * _Complex_I },
      { -5.000000e-01f + 8.660254e-01f * _Complex_I, -5.000000e-01f + 8.660254e-01f * _Complex_I,
        -5.000000e-01f - 8.660254e-01f * _Complex_I, -5.000000e-01f - 8.660254e-01f * _Complex_I,
        -5.000000e-01f - 8.660254e-01f * _Complex_I, -5.000000e-01f - 8.660254e-01f * _Complex_I,
        -5.000000e-01f + 8.660254e-01f * _Complex_I, 1.000000e+00f + 0.000000e+00f * _Complex_I } },
  };

  static const float complex harr[4][13][13]
      = { { { 1.326343e-03f - 1.299441e-18f * _Complex_I, 7.091837e-04f - 1.228342e-03f * _Complex_I,
              -6.278557e-04f - 1.087478e-03f * _Complex_I, -1.157216e-03f + 9.920263e-19f * _Complex_I,
              -4.887166e-04f + 8.464820e-04f * _Complex_I, 5.758687e-04f + 9.974338e-04f * _Complex_I,
              1.225183e-03f - 9.002496e-19f * _Complex_I, 5.758687e-04f - 9.974338e-04f * _Complex_I,
              -4.887166e-04f - 8.464820e-04f * _Complex_I, -1.157216e-03f + 7.085902e-19f * _Complex_I,
              -6.278557e-04f + 1.087478e-03f * _Complex_I, 7.091837e-04f + 1.228342e-03f * _Complex_I,
              1.326343e-03f - 6.497206e-19f * _Complex_I },
            { -1.980815e-03f + 1.698059e-18f * _Complex_I, -1.070384e-03f + 1.853959e-03f * _Complex_I,
              7.924697e-04f + 1.372598e-03f * _Complex_I, 1.876584e-03f - 1.378892e-18f * _Complex_I,
              1.225866e-03f - 2.123262e-03f * _Complex_I, -1.569320e-03f - 2.718142e-03f * _Complex_I,
              -3.273971e-03f + 2.004729e-18f * _Complex_I, -1.569320e-03f + 2.718142e-03f * _Complex_I,
              1.225866e-03f + 2.123262e-03f * _Complex_I, 1.876584e-03f - 9.192611e-19f * _Complex_I,
              7.924697e-04f - 1.372598e-03f * _Complex_I, -1.070384e-03f - 1.853959e-03f * _Complex_I,
              -1.980815e-03f + 7.277398e-19f * _Complex_I },
            { 1.457023e-03f - 1.070603e-18f * _Complex_I, 8.487143e-04f - 1.470016e-03f * _Complex_I,
              -6.873776e-04f - 1.190573e-03f * _Complex_I, -2.668335e-03f + 1.633884e-18f * _Complex_I,
              -2.459813e-03f + 4.260521e-03f * _Complex_I, 3.238772e-03f + 5.609717e-03f * _Complex_I,
              7.074895e-03f - 3.465699e-18f * _Complex_I, 3.238772e-03f - 5.609717e-03f * _Complex_I,
              -2.459813e-03f - 4.260521e-03f * _Complex_I, -2.668335e-03f + 9.803302e-19f * _Complex_I,
              -6.873776e-04f + 1.190573e-03f * _Complex_I, 8.487143e-04f + 1.470016e-03f * _Complex_I,
              1.457023e-03f - 3.568678e-19f * _Complex_I },
            { -1.017660e-03f + 6.231370e-19f * _Complex_I, -5.415171e-04f + 9.379351e-04f * _Complex_I,
              7.255109e-04f + 1.256622e-03f * _Complex_I, 3.699792e-03f - 1.812375e-18f * _Complex_I,
              4.090356e-03f - 7.084704e-03f * _Complex_I, -6.006283e-03f - 1.040319e-02f * _Complex_I,
              -1.391431e-02f + 5.112034e-18f * _Complex_I, -6.006283e-03f + 1.040319e-02f * _Complex_I,
              4.090356e-03f + 7.084704e-03f * _Complex_I, 3.699792e-03f - 9.061876e-19f * _Complex_I,
              7.255109e-04f - 1.256622e-03f * _Complex_I, -5.415171e-04f - 9.379351e-04f * _Complex_I,
              -1.017660e-03f + 1.246274e-19f * _Complex_I },
            { 9.198983e-04f - 4.506202e-19f * _Complex_I, 6.815900e-04f - 1.180548e-03f * _Complex_I,
              -1.287335e-03f - 2.229729e-03f * _Complex_I, -5.023856e-03f + 1.845735e-18f * _Complex_I,
              -5.499048e-03f + 9.524630e-03f * _Complex_I, 9.797672e-03f + 1.697006e-02f * _Complex_I,
              2.504795e-02f - 6.134977e-18f * _Complex_I, 9.797672e-03f - 1.697006e-02f * _Complex_I,
              -5.499048e-03f - 9.524630e-03f * _Complex_I, -5.023856e-03f + 6.152449e-19f * _Complex_I,
              -1.287335e-03f + 2.229729e-03f * _Complex_I, 6.815900e-04f + 1.180548e-03f * _Complex_I,
              9.198983e-04f + 0.000000e+00f * _Complex_I },
            { -7.972663e-04f + 2.929109e-19f * _Complex_I, -1.145605e-03f + 1.984247e-03f * _Complex_I,
              1.983334e-03f + 3.435235e-03f * _Complex_I, 6.730096e-03f - 1.648398e-18f * _Complex_I,
              6.782033e-03f - 1.174683e-02f * _Complex_I, -1.392077e-02f - 2.411147e-02f * _Complex_I,
              -3.906939e-02f + 4.784620e-18f * _Complex_I, -1.392077e-02f + 2.411147e-02f * _Complex_I,
              6.782033e-03f + 1.174683e-02f * _Complex_I, 6.730096e-03f + 0.000000e+00f * _Complex_I,
              1.983334e-03f - 3.435235e-03f * _Complex_I, -1.145605e-03f - 1.984247e-03f * _Complex_I,
              -7.972663e-04f - 9.763696e-20f * _Complex_I },
            { 8.625458e-04f - 2.112628e-19f * _Complex_I, 1.431113e-03f - 2.478760e-03f * _Complex_I,
              -2.310309e-03f - 4.001572e-03f * _Complex_I, -7.706486e-03f + 9.437723e-19f * _Complex_I,
              -7.220186e-03f + 1.250573e-02f * _Complex_I, 1.587118e-02f + 2.748969e-02f * _Complex_I,
              4.765675e-02f + 0.000000e+00f * _Complex_I, 1.587118e-02f - 2.748969e-02f * _Complex_I,
              -7.220186e-03f - 1.250573e-02f * _Complex_I, -7.706486e-03f - 9.437723e-19f * _Complex_I,
              -2.310309e-03f + 4.001572e-03f * _Complex_I, 1.431113e-03f + 2.478760e-03f * _Complex_I,
              8.625458e-04f + 2.112628e-19f * _Complex_I },
            { -7.972663e-04f + 9.763696e-20f * _Complex_I, -1.145605e-03f + 1.984247e-03f * _Complex_I,
              1.983334e-03f + 3.435235e-03f * _Complex_I, 6.730096e-03f + 0.000000e+00f * _Complex_I,
              6.782033e-03f - 1.174683e-02f * _Complex_I, -1.392077e-02f - 2.411147e-02f * _Complex_I,
              -3.906939e-02f - 4.784620e-18f * _Complex_I, -1.392077e-02f + 2.411147e-02f * _Complex_I,
              6.782033e-03f + 1.174683e-02f * _Complex_I, 6.730096e-03f + 1.648398e-18f * _Complex_I,
              1.983334e-03f - 3.435235e-03f * _Complex_I, -1.145605e-03f - 1.984247e-03f * _Complex_I,
              -7.972663e-04f - 2.929109e-19f * _Complex_I },
            { 9.198983e-04f + 0.000000e+00f * _Complex_I, 6.815900e-04f - 1.180548e-03f * _Complex_I,
              -1.287335e-03f - 2.229729e-03f * _Complex_I, -5.023856e-03f - 6.152449e-19f * _Complex_I,
              -5.499048e-03f + 9.524630e-03f * _Complex_I, 9.797672e-03f + 1.697006e-02f * _Complex_I,
              2.504795e-02f + 6.134977e-18f * _Complex_I, 9.797672e-03f - 1.697006e-02f * _Complex_I,
              -5.499048e-03f - 9.524630e-03f * _Complex_I, -5.023856e-03f - 1.845735e-18f * _Complex_I,
              -1.287335e-03f + 2.229729e-03f * _Complex_I, 6.815900e-04f + 1.180548e-03f * _Complex_I,
              9.198983e-04f + 4.506202e-19f * _Complex_I },
            { -1.017660e-03f - 1.246274e-19f * _Complex_I, -5.415171e-04f + 9.379351e-04f * _Complex_I,
              7.255109e-04f + 1.256622e-03f * _Complex_I, 3.699792e-03f + 9.061876e-19f * _Complex_I,
              4.090356e-03f - 7.084704e-03f * _Complex_I, -6.006283e-03f - 1.040319e-02f * _Complex_I,
              -1.391431e-02f - 5.112034e-18f * _Complex_I, -6.006283e-03f + 1.040319e-02f * _Complex_I,
              4.090356e-03f + 7.084704e-03f * _Complex_I, 3.699792e-03f + 1.812375e-18f * _Complex_I,
              7.255109e-04f - 1.256622e-03f * _Complex_I, -5.415171e-04f - 9.379351e-04f * _Complex_I,
              -1.017660e-03f - 6.231370e-19f * _Complex_I },
            { 1.457023e-03f + 3.568678e-19f * _Complex_I, 8.487143e-04f - 1.470016e-03f * _Complex_I,
              -6.873776e-04f - 1.190573e-03f * _Complex_I, -2.668335e-03f - 9.803302e-19f * _Complex_I,
              -2.459813e-03f + 4.260521e-03f * _Complex_I, 3.238772e-03f + 5.609717e-03f * _Complex_I,
              7.074895e-03f + 3.465699e-18f * _Complex_I, 3.238772e-03f - 5.609717e-03f * _Complex_I,
              -2.459813e-03f - 4.260521e-03f * _Complex_I, -2.668335e-03f - 1.633884e-18f * _Complex_I,
              -6.873776e-04f + 1.190573e-03f * _Complex_I, 8.487143e-04f + 1.470016e-03f * _Complex_I,
              1.457023e-03f + 1.070603e-18f * _Complex_I },
            { -1.980815e-03f - 7.277398e-19f * _Complex_I, -1.070384e-03f + 1.853959e-03f * _Complex_I,
              7.924697e-04f + 1.372598e-03f * _Complex_I, 1.876584e-03f + 9.192611e-19f * _Complex_I,
              1.225866e-03f - 2.123262e-03f * _Complex_I, -1.569320e-03f - 2.718142e-03f * _Complex_I,
              -3.273971e-03f - 2.004729e-18f * _Complex_I, -1.569320e-03f + 2.718142e-03f * _Complex_I,
              1.225866e-03f + 2.123262e-03f * _Complex_I, 1.876584e-03f + 1.378892e-18f * _Complex_I,
              7.924697e-04f - 1.372598e-03f * _Complex_I, -1.070384e-03f - 1.853959e-03f * _Complex_I,
              -1.980815e-03f - 1.698059e-18f * _Complex_I },
            { 1.326343e-03f + 6.497206e-19f * _Complex_I, 7.091837e-04f - 1.228342e-03f * _Complex_I,
              -6.278557e-04f - 1.087478e-03f * _Complex_I, -1.157216e-03f - 7.085902e-19f * _Complex_I,
              -4.887166e-04f + 8.464820e-04f * _Complex_I, 5.758687e-04f + 9.974338e-04f * _Complex_I,
              1.225183e-03f + 9.002496e-19f * _Complex_I, 5.758687e-04f - 9.974338e-04f * _Complex_I,
              -4.887166e-04f - 8.464820e-04f * _Complex_I, -1.157216e-03f - 9.920263e-19f * _Complex_I,
              -6.278557e-04f + 1.087478e-03f * _Complex_I, 7.091837e-04f + 1.228342e-03f * _Complex_I,
              1.326343e-03f + 1.299441e-18f * _Complex_I } },
          { { 9.129120e-04f - 8.943958e-19f * _Complex_I, -5.925973e-04f - 1.026409e-03f * _Complex_I,
              -5.989682e-04f + 1.037443e-03f * _Complex_I, 1.158755e-03f - 8.514393e-19f * _Complex_I,
              -8.992493e-04f - 1.557545e-03f * _Complex_I, -1.283187e-03f + 2.222546e-03f * _Complex_I,
              2.730635e-03f - 1.337625e-18f * _Complex_I, -1.283187e-03f - 2.222546e-03f * _Complex_I,
              -8.992493e-04f + 1.557545e-03f * _Complex_I, 1.158755e-03f - 2.838131e-19f * _Complex_I,
              -5.989682e-04f - 1.037443e-03f * _Complex_I, -5.925973e-04f + 1.026409e-03f * _Complex_I,
              9.129120e-04f + 0.000000e+00f * _Complex_I },
            { -5.588854e-04f - 9.680179e-04f * _Complex_I, -6.474856e-04f + 1.121478e-03f * _Complex_I,
              1.536588e-03f - 1.129066e-18f * _Complex_I, -9.123802e-04f - 1.580289e-03f * _Complex_I,
              -1.541434e-03f + 2.669842e-03f * _Complex_I, 4.379825e-03f - 9.925627e-18f * _Complex_I,
              -2.394173e-03f - 4.146830e-03f * _Complex_I, -2.189912e-03f + 3.793039e-03f * _Complex_I,
              3.082869e-03f - 3.493222e-18f * _Complex_I, -9.123802e-04f - 1.580289e-03f * _Complex_I,
              -7.682939e-04f + 1.330724e-03f * _Complex_I, 1.294971e-03f + 0.000000e+00f * _Complex_I,
              -5.588854e-04f - 9.680179e-04f * _Complex_I },
            { -5.883876e-04f + 1.019117e-03f * _Complex_I, 1.714796e-03f - 1.260012e-18f * _Complex_I,
              -1.180365e-03f - 2.044451e-03f * _Complex_I, -1.483082e-03f + 2.568774e-03f * _Complex_I,
              4.933362e-03f - 2.416651e-18f * _Complex_I, -3.296542e-03f - 5.709779e-03f * _Complex_I,
              -3.546477e-03f + 6.142678e-03f * _Complex_I, 6.593085e-03f - 1.614840e-18f * _Complex_I,
              -2.466681e-03f - 4.272417e-03f * _Complex_I, -1.483082e-03f + 2.568774e-03f * _Complex_I,
              2.360729e-03f + 0.000000e+00f * _Complex_I, -8.573982e-04f - 1.485057e-03f * _Complex_I,
              -5.883876e-04f + 1.019117e-03f * _Complex_I },
            { 1.483526e-03f - 1.090077e-18f * _Complex_I, -1.074793e-03f - 1.861596e-03f * _Complex_I,
              -1.447448e-03f + 2.507053e-03f * _Complex_I, 3.952416e-03f - 1.936126e-18f * _Complex_I,
              -3.496688e-03f - 6.056441e-03f * _Complex_I, -4.898024e-03f + 8.483627e-03f * _Complex_I,
              1.070518e-02f - 2.622012e-18f * _Complex_I, -4.898024e-03f - 8.483627e-03f * _Complex_I,
              -3.496688e-03f + 6.056441e-03f * _Complex_I, 3.952416e-03f + 0.000000e+00f * _Complex_I,
              -1.447448e-03f - 2.507053e-03f * _Complex_I, -1.074793e-03f + 1.861596e-03f * _Complex_I,
              1.483526e-03f + 3.633590e-19f * _Complex_I },
            { -9.966429e-04f - 1.726236e-03f * _Complex_I, -1.478281e-03f + 2.560458e-03f * _Complex_I,
              4.306274e-03f - 2.109466e-18f * _Complex_I, -3.294955e-03f - 5.707029e-03f * _Complex_I,
              -5.436890e-03f + 9.416970e-03f * _Complex_I, 1.556418e-02f - 3.812124e-18f * _Complex_I,
              -8.842875e-03f - 1.531631e-02f * _Complex_I, -7.782088e-03f + 1.347897e-02f * _Complex_I,
              1.087378e-02f + 0.000000e+00f * _Complex_I, -3.294955e-03f - 5.707029e-03f * _Complex_I,
              -2.153137e-03f + 3.729342e-03f * _Complex_I, 2.956562e-03f + 3.350104e-18f * _Complex_I,
              -9.966429e-04f - 1.726236e-03f * _Complex_I },
            { -1.291288e-03f + 2.236576e-03f * _Complex_I, 3.942788e-03f - 8.935208e-18f * _Complex_I,
              -2.798347e-03f - 4.846880e-03f * _Complex_I, -4.448869e-03f + 7.705666e-03f * _Complex_I,
              1.522441e-02f - 3.728906e-18f * _Complex_I, -1.175443e-02f - 2.035927e-02f * _Complex_I,
              -1.417872e-02f + 2.455826e-02f * _Complex_I, 2.350886e-02f + 0.000000e+00f * _Complex_I,
              -7.612206e-03f - 1.318473e-02f * _Complex_I, -4.448869e-03f + 7.705666e-03f * _Complex_I,
              5.596695e-03f + 1.370795e-18f * _Complex_I, -1.971394e-03f - 3.414555e-03f * _Complex_I,
              -1.291288e-03f + 2.236576e-03f * _Complex_I },
            { 2.779286e-03f - 1.361458e-18f * _Complex_I, -2.194126e-03f - 3.800338e-03f * _Complex_I,
              -3.057720e-03f + 5.296126e-03f * _Complex_I, 9.725261e-03f - 2.382002e-18f * _Complex_I,
              -8.649261e-03f - 1.498096e-02f * _Complex_I, -1.417667e-02f + 2.455472e-02f * _Complex_I,
              3.552610e-02f + 0.000000e+00f * _Complex_I, -1.417667e-02f - 2.455472e-02f * _Complex_I,
              -8.649261e-03f + 1.498096e-02f * _Complex_I, 9.725261e-03f + 2.382002e-18f * _Complex_I,
              -3.057720e-03f - 5.296126e-03f * _Complex_I, -2.194126e-03f + 3.800338e-03f * _Complex_I,
              2.779286e-03f + 1.361458e-18f * _Complex_I },
            { -1.291288e-03f - 2.236576e-03f * _Complex_I, -1.971394e-03f + 3.414555e-03f * _Complex_I,
              5.596695e-03f - 1.370795e-18f * _Complex_I, -4.448869e-03f - 7.705666e-03f * _Complex_I,
              -7.612206e-03f + 1.318473e-02f * _Complex_I, 2.350886e-02f + 0.000000e+00f * _Complex_I,
              -1.417872e-02f - 2.455826e-02f * _Complex_I, -1.175443e-02f + 2.035927e-02f * _Complex_I,
              1.522441e-02f + 3.728906e-18f * _Complex_I, -4.448869e-03f - 7.705666e-03f * _Complex_I,
              -2.798347e-03f + 4.846880e-03f * _Complex_I, 3.942788e-03f + 8.935208e-18f * _Complex_I,
              -1.291288e-03f - 2.236576e-03f * _Complex_I },
            { -9.966429e-04f + 1.726236e-03f * _Complex_I, 2.956562e-03f - 3.350104e-18f * _Complex_I,
              -2.153137e-03f - 3.729342e-03f * _Complex_I, -3.294955e-03f + 5.707029e-03f * _Complex_I,
              1.087378e-02f + 0.000000e+00f * _Complex_I, -7.782088e-03f - 1.347897e-02f * _Complex_I,
              -8.842875e-03f + 1.531631e-02f * _Complex_I, 1.556418e-02f + 3.812124e-18f * _Complex_I,
              -5.436890e-03f - 9.416970e-03f * _Complex_I, -3.294955e-03f + 5.707029e-03f * _Complex_I,
              4.306274e-03f + 2.109466e-18f * _Complex_I, -1.478281e-03f - 2.560458e-03f * _Complex_I,
              -9.966429e-04f + 1.726236e-03f * _Complex_I },
            { 1.483526e-03f - 3.633590e-19f * _Complex_I, -1.074793e-03f - 1.861596e-03f * _Complex_I,
              -1.447448e-03f + 2.507053e-03f * _Complex_I, 3.952416e-03f + 0.000000e+00f * _Complex_I,
              -3.496688e-03f - 6.056441e-03f * _Complex_I, -4.898024e-03f + 8.483627e-03f * _Complex_I,
              1.070518e-02f + 2.622012e-18f * _Complex_I, -4.898024e-03f - 8.483627e-03f * _Complex_I,
              -3.496688e-03f + 6.056441e-03f * _Complex_I, 3.952416e-03f + 1.936126e-18f * _Complex_I,
              -1.447448e-03f - 2.507053e-03f * _Complex_I, -1.074793e-03f + 1.861596e-03f * _Complex_I,
              1.483526e-03f + 1.090077e-18f * _Complex_I },
            { -5.883876e-04f - 1.019117e-03f * _Complex_I, -8.573982e-04f + 1.485057e-03f * _Complex_I,
              2.360729e-03f + 0.000000e+00f * _Complex_I, -1.483082e-03f - 2.568774e-03f * _Complex_I,
              -2.466681e-03f + 4.272417e-03f * _Complex_I, 6.593085e-03f + 1.614840e-18f * _Complex_I,
              -3.546477e-03f - 6.142678e-03f * _Complex_I, -3.296542e-03f + 5.709779e-03f * _Complex_I,
              4.933362e-03f + 2.416651e-18f * _Complex_I, -1.483082e-03f - 2.568774e-03f * _Complex_I,
              -1.180365e-03f + 2.044451e-03f * _Complex_I, 1.714796e-03f + 1.260012e-18f * _Complex_I,
              -5.883876e-04f - 1.019117e-03f * _Complex_I },
            { -5.588854e-04f + 9.680179e-04f * _Complex_I, 1.294971e-03f + 0.000000e+00f * _Complex_I,
              -7.682939e-04f - 1.330724e-03f * _Complex_I, -9.123802e-04f + 1.580289e-03f * _Complex_I,
              3.082869e-03f + 3.493222e-18f * _Complex_I, -2.189912e-03f - 3.793039e-03f * _Complex_I,
              -2.394173e-03f + 4.146830e-03f * _Complex_I, 4.379825e-03f + 9.925627e-18f * _Complex_I,
              -1.541434e-03f - 2.669842e-03f * _Complex_I, -9.123802e-04f + 1.580289e-03f * _Complex_I,
              1.536588e-03f + 1.129066e-18f * _Complex_I, -6.474856e-04f - 1.121478e-03f * _Complex_I,
              -5.588854e-04f + 9.680179e-04f * _Complex_I },
            { 9.129120e-04f + 0.000000e+00f * _Complex_I, -5.925973e-04f - 1.026409e-03f * _Complex_I,
              -5.989682e-04f + 1.037443e-03f * _Complex_I, 1.158755e-03f + 2.838131e-19f * _Complex_I,
              -8.992493e-04f - 1.557545e-03f * _Complex_I, -1.283187e-03f + 2.222546e-03f * _Complex_I,
              2.730635e-03f + 1.337625e-18f * _Complex_I, -1.283187e-03f - 2.222546e-03f * _Complex_I,
              -8.992493e-04f + 1.557545e-03f * _Complex_I, 1.158755e-03f + 8.514393e-19f * _Complex_I,
              -5.989682e-04f - 1.037443e-03f * _Complex_I, -5.925973e-04f + 1.026409e-03f * _Complex_I,
              9.129120e-04f + 8.943958e-19f * _Complex_I } },
          { { 8.228091e-04f + 0.000000e+00f * _Complex_I, -5.365069e-04f + 9.292572e-04f * _Complex_I,
              -6.011501e-04f - 1.041223e-03f * _Complex_I, 1.249890e-03f - 3.061346e-19f * _Complex_I,
              -7.632708e-04f + 1.322024e-03f * _Complex_I, -9.846035e-04f - 1.705383e-03f * _Complex_I,
              2.080486e-03f - 1.019144e-18f * _Complex_I, -9.846035e-04f + 1.705383e-03f * _Complex_I,
              -7.632708e-04f - 1.322024e-03f * _Complex_I, 1.249890e-03f - 9.184039e-19f * _Complex_I,
              -6.011501e-04f + 1.041223e-03f * _Complex_I, -5.365069e-04f - 9.292572e-04f * _Complex_I,
              8.228091e-04f - 8.061204e-19f * _Complex_I },
            { -5.616336e-04f - 9.727779e-04f * _Complex_I, 1.382894e-03f + 0.000000e+00f * _Complex_I,
              -8.694311e-04f + 1.505899e-03f * _Complex_I, -9.721139e-04f - 1.683751e-03f * _Complex_I,
              2.446785e-03f - 2.772471e-18f * _Complex_I, -1.605471e-03f + 2.780758e-03f * _Complex_I,
              -1.832781e-03f - 3.174469e-03f * _Complex_I, 3.210942e-03f - 7.276687e-18f * _Complex_I,
              -1.223392e-03f + 2.118978e-03f * _Complex_I, -9.721139e-04f - 1.683751e-03f * _Complex_I,
              1.738862e-03f - 1.277695e-18f * _Complex_I, -6.914471e-04f + 1.197621e-03f * _Complex_I,
              -5.616336e-04f - 9.727779e-04f * _Complex_I },
            { -5.723872e-04f + 9.914038e-04f * _Complex_I, -8.302721e-04f - 1.438073e-03f * _Complex_I,
              2.445280e-03f + 0.000000e+00f * _Complex_I, -1.378399e-03f + 2.387458e-03f * _Complex_I,
              -1.882898e-03f - 3.261274e-03f * _Complex_I, 4.921549e-03f - 1.205432e-18f * _Complex_I,
              -2.760152e-03f + 4.780723e-03f * _Complex_I, -2.460774e-03f - 4.262186e-03f * _Complex_I,
              3.765795e-03f - 1.844708e-18f * _Complex_I, -1.378399e-03f + 2.387458e-03f * _Complex_I,
              -1.222640e-03f - 2.117675e-03f * _Complex_I, 1.660544e-03f - 1.220148e-18f * _Complex_I,
              -5.723872e-04f + 9.914038e-04f * _Complex_I },
            { 1.226482e-03f + 3.004015e-19f * _Complex_I, -9.600816e-04f + 1.662910e-03f * _Complex_I,
              -1.495900e-03f - 2.590974e-03f * _Complex_I, 3.833507e-03f + 0.000000e+00f * _Complex_I,
              -3.167257e-03f + 5.485850e-03f * _Complex_I, -4.303595e-03f - 7.454046e-03f * _Complex_I,
              9.412791e-03f - 2.305469e-18f * _Complex_I, -4.303595e-03f + 7.454046e-03f * _Complex_I,
              -3.167257e-03f - 5.485850e-03f * _Complex_I, 3.833507e-03f - 1.877877e-18f * _Complex_I,
              -1.495900e-03f + 2.590974e-03f * _Complex_I, -9.600816e-04f - 1.662910e-03f * _Complex_I,
              1.226482e-03f - 9.012046e-19f * _Complex_I },
            { -9.898007e-04f - 1.714385e-03f * _Complex_I, 3.215120e-03f + 3.643077e-18f * _Complex_I,
              -2.507621e-03f + 4.343327e-03f * _Complex_I, -3.557798e-03f - 6.162286e-03f * _Complex_I,
              1.105198e-02f + 0.000000e+00f * _Complex_I, -7.691179e-03f + 1.332151e-02f * _Complex_I,
              -8.705793e-03f - 1.507888e-02f * _Complex_I, 1.538236e-02f - 3.767591e-18f * _Complex_I,
              -5.525988e-03f + 9.571292e-03f * _Complex_I, -3.557798e-03f - 6.162286e-03f * _Complex_I,
              5.015242e-03f - 2.456760e-18f * _Complex_I, -1.607560e-03f + 2.784375e-03f * _Complex_I,
              -9.898007e-04f - 1.714385e-03f * _Complex_I },
            { -1.414655e-03f + 2.450254e-03f * _Complex_I, -2.341263e-03f - 4.055186e-03f * _Complex_I,
              6.915775e-03f + 1.693876e-18f * _Complex_I, -5.086403e-03f + 8.809908e-03f * _Complex_I,
              -8.062191e-03f - 1.396412e-02f * _Complex_I, 2.415333e-02f + 0.000000e+00f * _Complex_I,
              -1.451128e-02f + 2.513428e-02f * _Complex_I, -1.207667e-02f - 2.091740e-02f * _Complex_I,
              1.612438e-02f - 3.949335e-18f * _Complex_I, -5.086403e-03f + 8.809908e-03f * _Complex_I,
              -3.457887e-03f - 5.989237e-03f * _Complex_I, 4.682526e-03f - 1.061161e-17f * _Complex_I,
              -1.414655e-03f + 2.450254e-03f * _Complex_I },
            { 3.039574e-03f + 1.488962e-18f * _Complex_I, -2.598226e-03f + 4.500260e-03f * _Complex_I,
              -3.750909e-03f - 6.496765e-03f * _Complex_I, 1.119776e-02f + 2.742661e-18f * _Complex_I,
              -9.210579e-03f + 1.595319e-02f * _Complex_I, -1.464762e-02f - 2.537042e-02f * _Complex_I,
              3.672076e-02f + 0.000000e+00f * _Complex_I, -1.464762e-02f + 2.537042e-02f * _Complex_I,
              -9.210579e-03f - 1.595319e-02f * _Complex_I, 1.119776e-02f - 2.742661e-18f * _Complex_I,
              -3.750909e-03f + 6.496765e-03f * _Complex_I, -2.598226e-03f - 4.500260e-03f * _Complex_I,
              3.039574e-03f - 1.488962e-18f * _Complex_I },
            { -1.414655e-03f - 2.450254e-03f * _Complex_I, 4.682526e-03f + 1.061161e-17f * _Complex_I,
              -3.457887e-03f + 5.989237e-03f * _Complex_I, -5.086403e-03f - 8.809908e-03f * _Complex_I,
              1.612438e-02f + 3.949335e-18f * _Complex_I, -1.207667e-02f + 2.091740e-02f * _Complex_I,
              -1.451128e-02f - 2.513428e-02f * _Complex_I, 2.415333e-02f + 0.000000e+00f * _Complex_I,
              -8.062191e-03f + 1.396412e-02f * _Complex_I, -5.086403e-03f - 8.809908e-03f * _Complex_I,
              6.915775e-03f - 1.693876e-18f * _Complex_I, -2.341263e-03f + 4.055186e-03f * _Complex_I,
              -1.414655e-03f - 2.450254e-03f * _Complex_I },
            { -9.898007e-04f + 1.714385e-03f * _Complex_I, -1.607560e-03f - 2.784375e-03f * _Complex_I,
              5.015242e-03f + 2.456760e-18f * _Complex_I, -3.557798e-03f + 6.162286e-03f * _Complex_I,
              -5.525988e-03f - 9.571292e-03f * _Complex_I, 1.538236e-02f + 3.767591e-18f * _Complex_I,
              -8.705793e-03f + 1.507888e-02f * _Complex_I, -7.691179e-03f - 1.332151e-02f * _Complex_I,
              1.105198e-02f + 0.000000e+00f * _Complex_I, -3.557798e-03f + 6.162286e-03f * _Complex_I,
              -2.507621e-03f - 4.343327e-03f * _Complex_I, 3.215120e-03f - 3.643077e-18f * _Complex_I,
              -9.898007e-04f + 1.714385e-03f * _Complex_I },
            { 1.226482e-03f + 9.012046e-19f * _Complex_I, -9.600816e-04f + 1.662910e-03f * _Complex_I,
              -1.495900e-03f - 2.590974e-03f * _Complex_I, 3.833507e-03f + 1.877877e-18f * _Complex_I,
              -3.167257e-03f + 5.485850e-03f * _Complex_I, -4.303595e-03f - 7.454046e-03f * _Complex_I,
              9.412791e-03f + 2.305469e-18f * _Complex_I, -4.303595e-03f + 7.454046e-03f * _Complex_I,
              -3.167257e-03f - 5.485850e-03f * _Complex_I, 3.833507e-03f + 0.000000e+00f * _Complex_I,
              -1.495900e-03f + 2.590974e-03f * _Complex_I, -9.600816e-04f - 1.662910e-03f * _Complex_I,
              1.226482e-03f - 3.004015e-19f * _Complex_I },
            { -5.723872e-04f - 9.914038e-04f * _Complex_I, 1.660544e-03f + 1.220148e-18f * _Complex_I,
              -1.222640e-03f + 2.117675e-03f * _Complex_I, -1.378399e-03f - 2.387458e-03f * _Complex_I,
              3.765795e-03f + 1.844708e-18f * _Complex_I, -2.460774e-03f + 4.262186e-03f * _Complex_I,
              -2.760152e-03f - 4.780723e-03f * _Complex_I, 4.921549e-03f + 1.205432e-18f * _Complex_I,
              -1.882898e-03f + 3.261274e-03f * _Complex_I, -1.378399e-03f - 2.387458e-03f * _Complex_I,
              2.445280e-03f + 0.000000e+00f * _Complex_I, -8.302721e-04f + 1.438073e-03f * _Complex_I,
              -5.723872e-04f - 9.914038e-04f * _Complex_I },
            { -5.616336e-04f + 9.727779e-04f * _Complex_I, -6.914471e-04f - 1.197621e-03f * _Complex_I,
              1.738862e-03f + 1.277695e-18f * _Complex_I, -9.721139e-04f + 1.683751e-03f * _Complex_I,
              -1.223392e-03f - 2.118978e-03f * _Complex_I, 3.210942e-03f + 7.276687e-18f * _Complex_I,
              -1.832781e-03f + 3.174469e-03f * _Complex_I, -1.605471e-03f - 2.780758e-03f * _Complex_I,
              2.446785e-03f + 2.772471e-18f * _Complex_I, -9.721139e-04f + 1.683751e-03f * _Complex_I,
              -8.694311e-04f - 1.505899e-03f * _Complex_I, 1.382894e-03f + 0.000000e+00f * _Complex_I,
              -5.616336e-04f + 9.727779e-04f * _Complex_I },
            { 8.228091e-04f + 8.061204e-19f * _Complex_I, -5.365069e-04f + 9.292572e-04f * _Complex_I,
              -6.011501e-04f - 1.041223e-03f * _Complex_I, 1.249890e-03f + 9.184039e-19f * _Complex_I,
              -7.632708e-04f + 1.322024e-03f * _Complex_I, -9.846035e-04f - 1.705383e-03f * _Complex_I,
              2.080486e-03f + 1.019144e-18f * _Complex_I, -9.846035e-04f + 1.705383e-03f * _Complex_I,
              -7.632708e-04f - 1.322024e-03f * _Complex_I, 1.249890e-03f + 3.061346e-19f * _Complex_I,
              -6.011501e-04f + 1.041223e-03f * _Complex_I, -5.365069e-04f - 9.292572e-04f * _Complex_I,
              8.228091e-04f + 0.000000e+00f * _Complex_I } },
          { { 1.221201e-03f + 5.982162e-19f * _Complex_I, -1.773498e-03f - 6.515727e-19f * _Complex_I,
              1.246697e-03f + 3.053526e-19f * _Complex_I, -8.215306e-04f - 1.006085e-19f * _Complex_I,
              7.609372e-04f + 0.000000e+00f * _Complex_I, -4.863927e-04f + 5.956592e-20f * _Complex_I,
              4.882100e-04f - 1.195770e-19f * _Complex_I, -4.863927e-04f + 1.786978e-19f * _Complex_I,
              7.609372e-04f - 3.727517e-19f * _Complex_I, -8.215306e-04f + 5.030424e-19f * _Complex_I,
              1.246697e-03f - 9.160579e-19f * _Complex_I, -1.773498e-03f + 1.520336e-18f * _Complex_I,
              1.221201e-03f - 1.196432e-18f * _Complex_I },
            { 7.406884e-04f - 1.282910e-03f * _Complex_I, -1.025411e-03f + 1.776065e-03f * _Complex_I,
              7.186273e-04f - 1.244699e-03f * _Complex_I, -4.025606e-04f + 6.972554e-04f * _Complex_I,
              5.908383e-04f - 1.023362e-03f * _Complex_I, -1.125190e-03f + 1.948886e-03f * _Complex_I,
              1.432695e-03f - 2.481501e-03f * _Complex_I, -1.125190e-03f + 1.948886e-03f * _Complex_I,
              5.908383e-04f - 1.023362e-03f * _Complex_I, -4.025606e-04f + 6.972554e-04f * _Complex_I,
              7.186273e-04f - 1.244699e-03f * _Complex_I, -1.025411e-03f + 1.776065e-03f * _Complex_I,
              7.406884e-04f - 1.282910e-03f * _Complex_I },
            { -7.162255e-04f - 1.240539e-03f * _Complex_I, 8.961176e-04f + 1.552121e-03f * _Complex_I,
              -6.705589e-04f - 1.161442e-03f * _Complex_I, 6.187140e-04f + 1.071644e-03f * _Complex_I,
              -1.165433e-03f - 2.018589e-03f * _Complex_I, 1.948120e-03f + 3.374242e-03f * _Complex_I,
              -2.297663e-03f - 3.979669e-03f * _Complex_I, 1.948120e-03f + 3.374242e-03f * _Complex_I,
              -1.165433e-03f - 2.018589e-03f * _Complex_I, 6.187140e-04f + 1.071644e-03f * _Complex_I,
              -6.705589e-04f - 1.161442e-03f * _Complex_I, 8.961176e-04f + 1.552121e-03f * _Complex_I,
              -7.162255e-04f - 1.240539e-03f * _Complex_I },
            { -1.280260e-03f - 7.839331e-19f * _Complex_I, 1.987108e-03f + 9.734024e-19f * _Complex_I,
              -2.614019e-03f - 9.603749e-19f * _Complex_I, 3.635167e-03f + 8.903590e-19f * _Complex_I,
              -4.954867e-03f - 6.067962e-19f * _Complex_I, 6.653220e-03f + 0.000000e+00f * _Complex_I,
              -7.600546e-03f + 9.307984e-19f * _Complex_I, 6.653220e-03f - 1.629569e-18f * _Complex_I,
              -4.954867e-03f + 1.820389e-18f * _Complex_I, 3.635167e-03f - 1.780718e-18f * _Complex_I,
              -2.614019e-03f + 1.600625e-18f * _Complex_I, 1.987108e-03f - 1.460104e-18f * _Complex_I,
              -1.280260e-03f + 1.097506e-18f * _Complex_I },
            { -5.756945e-04f + 9.971322e-04f * _Complex_I, 1.268614e-03f - 2.197304e-03f * _Complex_I,
              -2.421407e-03f + 4.194000e-03f * _Complex_I, 4.045715e-03f - 7.007384e-03f * _Complex_I,
              -5.527367e-03f + 9.573681e-03f * _Complex_I, 6.837207e-03f - 1.184239e-02f * _Complex_I,
              -7.288212e-03f + 1.262355e-02f * _Complex_I, 6.837207e-03f - 1.184239e-02f * _Complex_I,
              -5.527367e-03f + 9.573681e-03f * _Complex_I, 4.045715e-03f - 7.007384e-03f * _Complex_I,
              -2.421407e-03f + 4.194000e-03f * _Complex_I, 1.268614e-03f - 2.197304e-03f * _Complex_I,
              -5.756945e-04f + 9.971322e-04f * _Complex_I },
            { 7.349896e-04f + 1.273039e-03f * _Complex_I, -1.748057e-03f - 3.027723e-03f * _Complex_I,
              3.332671e-03f + 5.772355e-03f * _Complex_I, -6.051736e-03f - 1.048191e-02f * _Complex_I,
              9.842376e-03f + 1.704749e-02f * _Complex_I, -1.401169e-02f - 2.426897e-02f * _Complex_I,
              1.598601e-02f + 2.768858e-02f * _Complex_I, -1.401169e-02f - 2.426897e-02f * _Complex_I,
              9.842376e-03f + 1.704749e-02f * _Complex_I, -6.051736e-03f - 1.048191e-02f * _Complex_I,
              3.332671e-03f + 5.772355e-03f * _Complex_I, -1.748057e-03f - 3.027723e-03f * _Complex_I,
              7.349896e-04f + 1.273039e-03f * _Complex_I },
            { 1.400383e-03f + 1.028985e-18f * _Complex_I, -3.545886e-03f - 2.171229e-18f * _Complex_I,
              7.289370e-03f + 3.570761e-18f * _Complex_I, -1.418908e-02f - 5.212982e-18f * _Complex_I,
              2.520839e-02f + 6.174275e-18f * _Complex_I, -3.934772e-02f - 4.818706e-18f * _Complex_I,
              4.797481e-02f + 0.000000e+00f * _Complex_I, -3.934772e-02f + 4.818706e-18f * _Complex_I,
              2.520839e-02f - 6.174275e-18f * _Complex_I, -1.418908e-02f + 5.212982e-18f * _Complex_I,
              7.289370e-03f - 3.570761e-18f * _Complex_I, -3.545886e-03f + 2.171229e-18f * _Complex_I,
              1.400383e-03f - 1.028985e-18f * _Complex_I },
            { 7.349896e-04f - 1.273039e-03f * _Complex_I, -1.748057e-03f + 3.027723e-03f * _Complex_I,
              3.332671e-03f - 5.772355e-03f * _Complex_I, -6.051736e-03f + 1.048191e-02f * _Complex_I,
              9.842376e-03f - 1.704749e-02f * _Complex_I, -1.401169e-02f + 2.426897e-02f * _Complex_I,
              1.598601e-02f - 2.768858e-02f * _Complex_I, -1.401169e-02f + 2.426897e-02f * _Complex_I,
              9.842376e-03f - 1.704749e-02f * _Complex_I, -6.051736e-03f + 1.048191e-02f * _Complex_I,
              3.332671e-03f - 5.772355e-03f * _Complex_I, -1.748057e-03f + 3.027723e-03f * _Complex_I,
              7.349896e-04f - 1.273039e-03f * _Complex_I },
            { -5.756945e-04f - 9.971322e-04f * _Complex_I, 1.268614e-03f + 2.197304e-03f * _Complex_I,
              -2.421407e-03f - 4.194000e-03f * _Complex_I, 4.045715e-03f + 7.007384e-03f * _Complex_I,
              -5.527367e-03f - 9.573681e-03f * _Complex_I, 6.837207e-03f + 1.184239e-02f * _Complex_I,
              -7.288212e-03f - 1.262355e-02f * _Complex_I, 6.837207e-03f + 1.184239e-02f * _Complex_I,
              -5.527367e-03f - 9.573681e-03f * _Complex_I, 4.045715e-03f + 7.007384e-03f * _Complex_I,
              -2.421407e-03f - 4.194000e-03f * _Complex_I, 1.268614e-03f + 2.197304e-03f * _Complex_I,
              -5.756945e-04f - 9.971322e-04f * _Complex_I },
            { -1.280260e-03f - 1.097506e-18f * _Complex_I, 1.987108e-03f + 1.460104e-18f * _Complex_I,
              -2.614019e-03f - 1.600625e-18f * _Complex_I, 3.635167e-03f + 1.780718e-18f * _Complex_I,
              -4.954867e-03f - 1.820389e-18f * _Complex_I, 6.653220e-03f + 1.629569e-18f * _Complex_I,
              -7.600546e-03f - 9.307984e-19f * _Complex_I, 6.653220e-03f + 0.000000e+00f * _Complex_I,
              -4.954867e-03f + 6.067962e-19f * _Complex_I, 3.635167e-03f - 8.903590e-19f * _Complex_I,
              -2.614019e-03f + 9.603749e-19f * _Complex_I, 1.987108e-03f - 9.734024e-19f * _Complex_I,
              -1.280260e-03f + 7.839331e-19f * _Complex_I },
            { -7.162255e-04f + 1.240539e-03f * _Complex_I, 8.961176e-04f - 1.552121e-03f * _Complex_I,
              -6.705589e-04f + 1.161442e-03f * _Complex_I, 6.187140e-04f - 1.071644e-03f * _Complex_I,
              -1.165433e-03f + 2.018589e-03f * _Complex_I, 1.948120e-03f - 3.374242e-03f * _Complex_I,
              -2.297663e-03f + 3.979669e-03f * _Complex_I, 1.948120e-03f - 3.374242e-03f * _Complex_I,
              -1.165433e-03f + 2.018589e-03f * _Complex_I, 6.187140e-04f - 1.071644e-03f * _Complex_I,
              -6.705589e-04f + 1.161442e-03f * _Complex_I, 8.961176e-04f - 1.552121e-03f * _Complex_I,
              -7.162255e-04f + 1.240539e-03f * _Complex_I },
            { 7.406884e-04f + 1.282910e-03f * _Complex_I, -1.025411e-03f - 1.776065e-03f * _Complex_I,
              7.186273e-04f + 1.244699e-03f * _Complex_I, -4.025606e-04f - 6.972554e-04f * _Complex_I,
              5.908383e-04f + 1.023362e-03f * _Complex_I, -1.125190e-03f - 1.948886e-03f * _Complex_I,
              1.432695e-03f + 2.481501e-03f * _Complex_I, -1.125190e-03f - 1.948886e-03f * _Complex_I,
              5.908383e-04f + 1.023362e-03f * _Complex_I, -4.025606e-04f - 6.972554e-04f * _Complex_I,
              7.186273e-04f + 1.244699e-03f * _Complex_I, -1.025411e-03f - 1.776065e-03f * _Complex_I,
              7.406884e-04f + 1.282910e-03f * _Complex_I },
            { 1.221201e-03f + 1.196432e-18f * _Complex_I, -1.773498e-03f - 1.520336e-18f * _Complex_I,
              1.246697e-03f + 9.160579e-19f * _Complex_I, -8.215306e-04f - 5.030424e-19f * _Complex_I,
              7.609372e-04f + 3.727517e-19f * _Complex_I, -4.863927e-04f - 1.786978e-19f * _Complex_I,
              4.882100e-04f + 1.195770e-19f * _Complex_I, -4.863927e-04f - 5.956592e-20f * _Complex_I,
              7.609372e-04f + 0.000000e+00f * _Complex_I, -8.215306e-04f + 1.006085e-19f * _Complex_I,
              1.246697e-03f - 3.053526e-19f * _Complex_I, -1.773498e-03f + 6.515727e-19f * _Complex_I,
              1.221201e-03f - 5.982162e-19f * _Complex_I } } };

  const size_t buffer_size = (size_t)TS * TS * (ndir * 4 + 7) * sizeof(float);
  size_t padded_buffer_size;
  char *const all_buffers = (char *)dt_alloc_perthread(buffer_size, sizeof(char), &padded_buffer_size);
  if(!all_buffers)
  {
    dt_print(DT_DEBUG_ALWAYS, "[demosaic] not able to allocate FDC base buffers\n");
    return;
  }

  /* Map a green hexagon around each non-green pixel and vice versa:    */
  for(int row = 0; row < 3; row++)
    for(int col = 0; col < 3; col++)
      for(int ng = 0, d = 0; d < 10; d += 2)
      {
        int g = FCxtrans(row, col, NULL, xtrans) == 1;
        if(FCxtrans(row + orth[d], col + orth[d + 2], NULL, xtrans) == 1)
          ng = 0;
        else
          ng++;
        // if there are four non-green pixels adjacent in cardinal
        // directions, this is the solitary green pixel
        if(ng == 4)
        {
          sgrow = row;
          sgcol = col;
        }
        if(ng == g + 1)
          for(int c = 0; c < 8; c++)
          {
            int v = orth[d] * patt[g][c * 2] + orth[d + 1] * patt[g][c * 2 + 1];
            int h = orth[d + 2] * patt[g][c * 2] + orth[d + 3] * patt[g][c * 2 + 1];
            // offset within TSxTS buffer
            allhex[row][col][c ^ (g * 2 & d)] = h + v * TS;
          }
      }

  // extra passes propagates out errors at edges, hence need more padding
  const int pad_tile = 13;

  // calculate offsets for this roi
  int rowoffset = 0;
  int coloffset = 0;
  for(int row = 0; row < 6; row++)
  {
    if(!((row - sgrow) % 3))
    {
      for(int col = 0; col < 6; col++)
      {
        if(!((col - sgcol) % 3) && (FCxtrans(row, col + 1, roi_in, xtrans) == 0))
        {
          rowoffset = 37 - row - pad_tile; // 1 plus a generous multiple of 6
          coloffset = 37 - col - pad_tile; // to avoid that this value gets negative
          break;
        }
      }
      break;
    }
  }

  // depending on the iso, use either a hybrid approach for chroma, or pure fdc
  float hybrid_fdc[2] = { 1.0f, 0.0f };
  const int xover_iso = dt_conf_get_int("plugins/darkroom/demosaic/fdc_xover_iso");
  int iso = self->dev->image_storage.exif_iso;
  if(iso > xover_iso)
  {
    hybrid_fdc[0] = 0.0f;
    hybrid_fdc[1] = 1.0f;
  }

#ifdef _OPENMP
#pragma omp parallel for default(none)                                                                            \
    dt_omp_firstprivate(ndir, all_buffers, dir, directionality, harr, height, in, Minv, modarr, roi_in, width,    \
                        xtrans, pad_tile, padded_buffer_size)                                                     \
        shared(sgrow, sgcol, allhex, out, rowoffset, coloffset, hybrid_fdc) schedule(static)
#endif
  // step through TSxTS cells of image, each tile overlapping the
  // prior as interpolation needs a substantial border
  for(int top = -pad_tile; top < height - pad_tile; top += TS - (pad_tile * 2))
  {
    char *const buffer = dt_get_perthread(all_buffers, padded_buffer_size);
    // rgb points to ndir TSxTS tiles of 3 channels (R, G, and B)
    float(*rgb)[TS][TS][3] = (float(*)[TS][TS][3])buffer;
    // yuv points to 3 channel (Y, u, and v) TSxTS tiles
    // note that channels come before tiles to allow for a
    // vectorization optimization when building drv[] from yuv[]
    float (*const yuv)[TS][TS] = (float(*)[TS][TS])(buffer + TS * TS * (ndir * 3) * sizeof(float));
    // drv points to ndir TSxTS tiles, each a single channel of derivatives
    float (*const drv)[TS][TS] = (float(*)[TS][TS])(buffer + TS * TS * (ndir * 3 + 3) * sizeof(float));
    // gmin and gmax reuse memory which is used later by yuv buffer;
    // each points to a TSxTS tile of single channel data
    float (*const gmin)[TS] = (float(*)[TS])(buffer + TS * TS * (ndir * 3) * sizeof(float));
    float (*const gmax)[TS] = (float(*)[TS])(buffer + TS * TS * (ndir * 3 + 1) * sizeof(float));
    // homo and homosum reuse memory which is used earlier in the
    // loop; each points to ndir single-channel TSxTS tiles
    uint8_t (*const homo)[TS][TS] = (uint8_t(*)[TS][TS])(buffer + TS * TS * (ndir * 3) * sizeof(float));
    uint8_t (*const homosum)[TS][TS]
        = (uint8_t(*)[TS][TS])(buffer + TS * TS * (ndir * 3) * sizeof(float) + TS * TS * ndir * sizeof(uint8_t));
    // append all fdc related buffers
    float complex *fdc_buf_start = (float complex *)(buffer + TS * TS * (ndir * 4 + 3) * sizeof(float));
    const int fdc_buf_size = TS * TS;
    float(*const i_src) = (float *)fdc_buf_start;
    float complex(*const o_src) = fdc_buf_start + fdc_buf_size;
    // by the time the chroma values are calculated, o_src can be overwritten.
    float(*const fdc_chroma) = (float *)o_src;

    for(int left = -pad_tile; left < width - pad_tile; left += TS - (pad_tile * 2))
    {
      int mrow = MIN(top + TS, height + pad_tile);
      int mcol = MIN(left + TS, width + pad_tile);

      // Copy current tile from in to image buffer. If border goes
      // beyond edges of image, fill with mirrored/interpolated edges.
      // The extra border avoids discontinuities at image edges.
      for(int row = top; row < mrow; row++)
        for(int col = left; col < mcol; col++)
        {
          float(*const pix) = rgb[0][row - top][col - left];
          if((col >= 0) && (row >= 0) && (col < width) && (row < height))
          {
            const int f = FCxtrans(row, col, roi_in, xtrans);
            for(int c = 0; c < 3; c++) pix[c] = (c == f) ? in[roi_in->width * row + col] : 0.f;
            *(i_src + TS * (row - top) + (col - left)) = in[roi_in->width * row + col];
          }
          else
          {
            // mirror a border pixel if beyond image edge
            const int c = FCxtrans(row, col, roi_in, xtrans);
            for(int cc = 0; cc < 3; cc++)
              if(cc != c)
                pix[cc] = 0.0f;
              else
              {
#define TRANSLATE(n, size) ((n >= size) ? (2 * size - n - 2) : abs(n))
                const int cy = TRANSLATE(row, height), cx = TRANSLATE(col, width);
                if(c == FCxtrans(cy, cx, roi_in, xtrans))
                {
                  pix[c] = in[roi_in->width * cy + cx];
                  *(i_src + TS * (row - top) + (col - left)) = in[roi_in->width * cy + cx];
                }
                else
                {
                  // interpolate if mirror pixel is a different color
                  float sum = 0.0f;
                  uint8_t count = 0;
                  for(int y = row - 1; y <= row + 1; y++)
                    for(int x = col - 1; x <= col + 1; x++)
                    {
                      const int yy = TRANSLATE(y, height), xx = TRANSLATE(x, width);
                      const int ff = FCxtrans(yy, xx, roi_in, xtrans);
                      if(ff == c)
                      {
                        sum += in[roi_in->width * yy + xx];
                        count++;
                      }
                    }
                  pix[c] = sum / count;
                  *(i_src + TS * (row - top) + (col - left)) = pix[c];
                }
              }
          }
        }

      // duplicate rgb[0] to rgb[1], rgb[2], and rgb[3]
      for(int c = 1; c <= 3; c++) memcpy(rgb[c], rgb[0], sizeof(*rgb));

      // note that successive calculations are inset within the tile
      // so as to give enough border data, and there needs to be a 6
      // pixel border initially to allow allhex to find neighboring
      // pixels

      /* Set green1 and green3 to the minimum and maximum allowed values:   */
      // Run through each red/blue or blue/red pair, setting their g1
      // and g3 values to the min/max of green pixels surrounding the
      // pair. Use a 3 pixel border as gmin/gmax is used by
      // interpolate green which has a 3 pixel border.
      const int pad_g1_g3 = 3;
      for(int row = top + pad_g1_g3; row < mrow - pad_g1_g3; row++)
      {
        // setting max to 0.0f signifies that this is a new pair, which
        // requires a new min/max calculation of its neighboring greens
        float min = FLT_MAX, max = 0.0f;
        for(int col = left + pad_g1_g3; col < mcol - pad_g1_g3; col++)
        {
          // if in row of horizontal red & blue pairs (or processing
          // vertical red & blue pairs near image bottom), reset min/max
          // between each pair
          if(FCxtrans(row, col, roi_in, xtrans) == 1)
          {
            min = FLT_MAX, max = 0.0f;
            continue;
          }
          // if at start of red & blue pair, calculate min/max of green
          // pixels surrounding it; note that while normally using == to
          // compare floats is suspect, here the check is if 0.0f has
          // explicitly been assigned to max (which signifies a new
          // red/blue pair)
          if(max == 0.0f)
          {
            float (*const pix)[3] = &rgb[0][row - top][col - left];
            const short *const hex = _hexmap(row, col, allhex);
            for(int c = 0; c < 6; c++)
            {
              const float val = pix[hex[c]][1];
              if(min > val) min = val;
              if(max < val) max = val;
            }
          }
          gmin[row - top][col - left] = min;
          gmax[row - top][col - left] = max;
          // handle vertical red/blue pairs
          switch((row - sgrow) % 3)
          {
            // hop down a row to second pixel in vertical pair
            case 1:
              if(row < mrow - 4) row++, col--;
              break;
            // then if not done with the row hop up and right to next
            // vertical red/blue pair, resetting min/max
            case 2:
              min = FLT_MAX, max = 0.0f;
              if((col += 2) < mcol - 4 && row > top + 3) row--;
          }
        }
      }

      /* Interpolate green horizontally, vertically, and along both diagonals: */
      // need a 3 pixel border here as 3*hex[] can have a 3 unit offset
      const int pad_g_interp = 3;
      for(int row = top + pad_g_interp; row < mrow - pad_g_interp; row++)
        for(int col = left + pad_g_interp; col < mcol - pad_g_interp; col++)
        {
          float color[8];
          int f = FCxtrans(row, col, roi_in, xtrans);
          if(f == 1) continue;
          float (*const pix)[3] = &rgb[0][row - top][col - left];
          const short *const hex = _hexmap(row, col, allhex);
          // TODO: these constants come from integer math constants in
          // dcraw -- calculate them instead from interpolation math
          color[0] = 0.6796875f * (pix[hex[1]][1] + pix[hex[0]][1])
                     - 0.1796875f * (pix[2 * hex[1]][1] + pix[2 * hex[0]][1]);
          color[1] = 0.87109375f * pix[hex[3]][1] + pix[hex[2]][1] * 0.13f
                     + 0.359375f * (pix[0][f] - pix[-hex[2]][f]);
          for(int c = 0; c < 2; c++)
            color[2 + c] = 0.640625f * pix[hex[4 + c]][1] + 0.359375f * pix[-2 * hex[4 + c]][1]
                           + 0.12890625f * (2 * pix[0][f] - pix[3 * hex[4 + c]][f] - pix[-3 * hex[4 + c]][f]);
          for(int c = 0; c < 4; c++)
            rgb[c ^ !((row - sgrow) % 3)][row - top][col - left][1]
                = CLAMPS(color[c], gmin[row - top][col - left], gmax[row - top][col - left]);
        }

      /* Interpolate red and blue values for solitary green pixels:   */
      const int pad_rb_g = 6;
      for(int row = (top - sgrow + pad_rb_g + 2) / 3 * 3 + sgrow; row < mrow - pad_rb_g; row += 3)
        for(int col = (left - sgcol + pad_rb_g + 2) / 3 * 3 + sgcol; col < mcol - pad_rb_g; col += 3)
        {
          float(*rfx)[3] = &rgb[0][row - top][col - left];
          int h = FCxtrans(row, col + 1, roi_in, xtrans);
          float diff[6] = { 0.0f };
          float color[3][8];
          for(int i = 1, d = 0; d < 6; d++, i ^= TS ^ 1, h ^= 2)
          {
            for(int c = 0; c < 2; c++, h ^= 2)
            {
              float g = 2 * rfx[0][1] - rfx[i << c][1] - rfx[-(i << c)][1];
              color[h][d] = g + rfx[i << c][h] + rfx[-(i << c)][h];
              if(d > 1)
                diff[d] += sqrf(rfx[i << c][1] - rfx[-(i << c)][1] - rfx[i << c][h] + rfx[-(i << c)][h]) + sqrf(g);
            }
            if(d > 1 && (d & 1))
              if(diff[d - 1] < diff[d])
                for(int c = 0; c < 2; c++) color[c * 2][d] = color[c * 2][d - 1];
            if(d < 2 || (d & 1))
            {
              for(int c = 0; c < 2; c++) rfx[0][c * 2] = color[c * 2][d] / 2.f;
              rfx += TS * TS;
            }
          }
        }

      /* Interpolate red for blue pixels and vice versa:              */
      const int pad_rb_br = 6;
      for(int row = top + pad_rb_br; row < mrow - pad_rb_br; row++)
        for(int col = left + pad_rb_br; col < mcol - pad_rb_br; col++)
        {
          int f = 2 - FCxtrans(row, col, roi_in, xtrans);
          if(f == 1) continue;
          float(*rfx)[3] = &rgb[0][row - top][col - left];
          int c = (row - sgrow) % 3 ? TS : 1;
          int h = 3 * (c ^ TS ^ 1);
          for(int d = 0; d < 4; d++, rfx += TS * TS)
          {
            int i = d > 1 || ((d ^ c) & 1)
                            || ((fabsf(rfx[0][1] - rfx[c][1]) + fabsf(rfx[0][1] - rfx[-c][1]))
                                < 2.f * (fabsf(rfx[0][1] - rfx[h][1]) + fabsf(rfx[0][1] - rfx[-h][1]))) ? c : h;
            rfx[0][f] = (rfx[i][f] + rfx[-i][f] + 2.f * rfx[0][1] - rfx[i][1] - rfx[-i][1]) / 2.f;
          }
        }

      /* Fill in red and blue for 2x2 blocks of green:                */
      const int pad_g22 = 8;
      for(int row = top + pad_g22; row < mrow - pad_g22; row++)
        if((row - sgrow) % 3)
          for(int col = left + pad_g22; col < mcol - pad_g22; col++)
            if((col - sgcol) % 3)
            {
              float redblue[3][3];
              float(*rfx)[3] = &rgb[0][row - top][col - left];
              const short *const hex = _hexmap(row, col, allhex);
              for(int d = 0; d < ndir; d += 2, rfx += TS * TS)
                if(hex[d] + hex[d + 1])
                {
                  float g = 3.f * rfx[0][1] - 2.f * rfx[hex[d]][1] - rfx[hex[d + 1]][1];
                  for(int c = 0; c < 4; c += 2)
                  {
                    rfx[0][c] = (g + 2.f * rfx[hex[d]][c] + rfx[hex[d + 1]][c]) / 3.f;
                    redblue[d][c] = rfx[0][c];
                  }
                }
                else
                {
                  float g = 2.f * rfx[0][1] - rfx[hex[d]][1] - rfx[hex[d + 1]][1];
                  for(int c = 0; c < 4; c += 2)
                  {
                    rfx[0][c] = (g + rfx[hex[d]][c] + rfx[hex[d + 1]][c]) / 2.f;
                    redblue[d][c] = rfx[0][c];
                  }
                }
              // to fill in red and blue also for diagonal directions
              for(int d = 0; d < ndir; d += 2, rfx += TS * TS)
                for(int c = 0; c < 4; c += 2) rfx[0][c] = (redblue[0][c] + redblue[2][c]) * 0.5f;
           }

      // jump back to the first set of rgb buffers (this is a nop
      // unless on the second pass)
      rgb = (float(*)[TS][TS][3])buffer;
      // from here on out, mainly are working within the current tile
      // rather than in reference to the image, so don't offset
      // mrow/mcol by top/left of tile
      mrow -= top;
      mcol -= left;

      /* Convert to perceptual colorspace and differentiate in all directions:  */
      // Original dcraw algorithm uses CIELab as perceptual space
      // (presumably coming from original AHD) and converts taking
      // camera matrix into account. Now use YPbPr which requires much
      // less code and is nearly indistinguishable. It assumes the
      // camera RGB is roughly linear.
      for(int d = 0; d < ndir; d++)
      {
        const int pad_yuv = 8;
        for(int row = pad_yuv; row < mrow - pad_yuv; row++)
          for(int col = pad_yuv; col < mcol - pad_yuv; col++)
          {
            float *rx = rgb[d][row][col];
            // use ITU-R BT.2020 YPbPr, which is great, but could use
            // a better/simpler choice? note that imageop.h provides
            // dt_iop_RGB_to_YCbCr which uses Rec. 601 conversion,
            // which appears less good with specular highlights
            float y = 0.2627f * rx[0] + 0.6780f * rx[1] + 0.0593f * rx[2];
            yuv[0][row][col] = y;
            yuv[1][row][col] = (rx[2] - y) * 0.56433f;
            yuv[2][row][col] = (rx[0] - y) * 0.67815f;
          }
        // Note that f can offset by a column (-1 or +1) and by a row
        // (-TS or TS). The row-wise offsets cause the undefined
        // behavior sanitizer to warn of an out of bounds index, but
        // as yfx is multi-dimensional and there is sufficient
        // padding, that is not actually so.
        const int f = dir[d & 3];
        const int pad_drv = 9;
        for(int row = pad_drv; row < mrow - pad_drv; row++)
          for(int col = pad_drv; col < mcol - pad_drv; col++)
          {
            float(*yfx)[TS][TS] = (float(*)[TS][TS]) & yuv[0][row][col];
            drv[d][row][col] = sqrf(2 * yfx[0][0][0] - yfx[0][0][f] - yfx[0][0][-f])
                               + sqrf(2 * yfx[1][0][0] - yfx[1][0][f] - yfx[1][0][-f])
                               + sqrf(2 * yfx[2][0][0] - yfx[2][0][f] - yfx[2][0][-f]);
          }
      }

      /* Build homogeneity maps from the derivatives:                   */
      memset(homo, 0, sizeof(uint8_t) * ndir * TS * TS);
      const int pad_homo = 10;
      for(int row = pad_homo; row < mrow - pad_homo; row++)
        for(int col = pad_homo; col < mcol - pad_homo; col++)
        {
          float tr = FLT_MAX;
          for(int d = 0; d < ndir; d++)
            if(tr > drv[d][row][col]) tr = drv[d][row][col];
          tr *= 8;
          for(int d = 0; d < ndir; d++)
            for(int v = -1; v <= 1; v++)
              for(int h = -1; h <= 1; h++) homo[d][row][col] += ((drv[d][row + v][col + h] <= tr) ? 1 : 0);
        }

      /* Build 5x5 sum of homogeneity maps for each pixel & direction */
      for(int d = 0; d < ndir; d++)
        for(int row = pad_tile; row < mrow - pad_tile; row++)
        {
          // start before first column where homo[d][row][col+2] != 0,
          // so can know v5sum and homosum[d][row][col] will be 0
          int col = pad_tile - 5;
          uint8_t v5sum[5] = { 0 };
          homosum[d][row][col] = 0;
          // calculate by rolling through column sums
          for(col++; col < mcol - pad_tile; col++)
          {
            uint8_t colsum = 0;
            for(int v = -2; v <= 2; v++) colsum += homo[d][row + v][col + 2];
            homosum[d][row][col] = homosum[d][row][col - 1] - v5sum[col % 5] + colsum;
            v5sum[col % 5] = colsum;
          }
        }

      /* Calculate chroma values in fdc:       */
      const int pad_fdc = 6;
      for(int row = pad_fdc; row < mrow - pad_fdc; row++)
        for(int col = pad_fdc; col < mcol - pad_fdc; col++)
        {
          int myrow, mycol;
          uint8_t hm[8] = { 0 };
          uint8_t maxval = 0;
          for(int d = 0; d < ndir; d++)
          {
            hm[d] = homosum[d][row][col];
            maxval = (maxval < hm[d] ? hm[d] : maxval);
          }
          maxval -= maxval >> 3;
          float dircount = 0;
          float dirsum = 0.f;
          for(int d = 0; d < ndir; d++)
            if(hm[d] >= maxval)
            {
              dircount++;
              dirsum += directionality[d];
            }
          float w = dirsum / (float)dircount;
          int fdc_row, fdc_col;
          float complex C2m, C5m, C7m, C10m;
#define CONV_FILT(VAR, FILT)                                                                                      \
  VAR = 0.0f + 0.0f * _Complex_I;                                                                                 \
  for(fdc_row = 0, myrow = row - 6; fdc_row < 13; fdc_row++, myrow++)                                             \
    for(fdc_col = 0, mycol = col - 6; fdc_col < 13; fdc_col++, mycol++)                                           \
      VAR += FILT[12 - fdc_row][12 - fdc_col] * *(i_src + TS * myrow + mycol);
          CONV_FILT(C2m, harr[0])
          CONV_FILT(C5m, harr[1])
          CONV_FILT(C7m, harr[2])
          CONV_FILT(C10m, harr[3])
#undef CONV_FILT
          // build the q vector components
          myrow = (row + rowoffset) % 6;
          mycol = (col + coloffset) % 6;
          float complex modulator[8];
          for(int c = 0; c < 8; c++) modulator[c] = modarr[myrow][mycol][c];
          float complex qmat[8];
          qmat[4] = w * C10m * modulator[0] - (1.0f - w) * C2m * modulator[1];
          qmat[6] = conjf(qmat[4]);
          qmat[1] = C5m * modulator[6];
          qmat[2] = conjf(-0.5f * qmat[1]);
          qmat[5] = conjf(qmat[2]);
          qmat[3] = C7m * modulator[7];
          qmat[7] = conjf(qmat[1]);
          // get L
          C2m = qmat[4] * (conjf(modulator[0]) - conjf(modulator[1]));
          float complex C3m = qmat[6] * (modulator[2] - modulator[3]);
          float complex C6m = qmat[2] * (conjf(modulator[4]) + conjf(modulator[5]));
          float complex C12m = qmat[5] * (modulator[4] + modulator[5]);
          float complex C18m = qmat[7] * modulator[6];
          qmat[0] = *(i_src + row * TS + col) - C2m - C3m - C5m - C6m - 2.0f * C7m - C12m - C18m;
          // get the rgb components from fdc
          dt_aligned_pixel_t rgbpix = { 0.f, 0.f, 0.f };
          // multiply with the inverse matrix of M
          for(int color = 0; color < 3; color++)
            for(int c = 0; c < 8; c++)
            {
              rgbpix[color] += Minv[color][c] * qmat[c];
            }
          // now separate luma and chroma for
          // frequency domain chroma
          // and store it in fdc_chroma
          float uv[2];
          float y = 0.2627f * rgbpix[0] + 0.6780f * rgbpix[1] + 0.0593f * rgbpix[2];
          uv[0] = (rgbpix[2] - y) * 0.56433f;
          uv[1] = (rgbpix[0] - y) * 0.67815f;
          for(int c = 0; c < 2; c++) *(fdc_chroma + c * TS * TS + row * TS + col) = uv[c];
        }

      /* Average the most homogeneous pixels for the final result:       */
      for(int row = pad_tile; row < mrow - pad_tile; row++)
        for(int col = pad_tile; col < mcol - pad_tile; col++)
        {
          uint8_t hm[8] = { 0 };
          uint8_t maxval = 0;
          for(int d = 0; d < ndir; d++)
          {
            hm[d] = homosum[d][row][col];
            maxval = (maxval < hm[d] ? hm[d] : maxval);
          }
          maxval -= maxval >> 3;
          for(int d = 0; d < ndir - 4; d++)
          {
            if(hm[d] < hm[d + 4])
              hm[d] = 0;
            else if(hm[d] > hm[d + 4])
              hm[d + 4] = 0;
          }

          dt_aligned_pixel_t avg = { 0.f };
          for(int d = 0; d < ndir; d++)
          {
            if(hm[d] >= maxval)
            {
              for(int c = 0; c < 3; c++) avg[c] += rgb[d][row][col][c];
              avg[3]++;
            }
          }
          dt_aligned_pixel_t rgbpix;
          for(int c = 0; c < 3; c++) rgbpix[c] = avg[c] / avg[3];
          // preserve all components of Markesteijn for this pixel
          const float y = 0.2627f * rgbpix[0] + 0.6780f * rgbpix[1] + 0.0593f * rgbpix[2];
          const float um = (rgbpix[2] - y) * 0.56433f;
          const float vm = (rgbpix[0] - y) * 0.67815f;
          float uvf[2];
          // macros for fast meadian filtering
#define PIX_SWAP(a, b)                                                                                            \
  {                                                                                                               \
    tempf = (a);                                                                                                  \
    (a) = (b);                                                                                                    \
    (b) = tempf;                                                                                                  \
  }
#define PIX_SORT(a, b)                                                                                            \
  {                                                                                                               \
    if((a) > (b)) PIX_SWAP((a), (b));                                                                             \
  }
          // instead of merely reading the values, perform 5 pixel median filter
          // one median filter is required to avoid textile artifacts
          for(int chrm = 0; chrm < 2; chrm++)
          {
            float temp[5];
            float tempf;
            // load the window into temp
            memcpy(&temp[0], fdc_chroma + chrm * TS * TS + (row - 1) * TS + (col), sizeof(float) * 1);
            memcpy(&temp[1], fdc_chroma + chrm * TS * TS + (row)*TS + (col - 1),   sizeof(float) * 3);
            memcpy(&temp[4], fdc_chroma + chrm * TS * TS + (row + 1) * TS + (col), sizeof(float) * 1);
            PIX_SORT(temp[0], temp[1]);
            PIX_SORT(temp[3], temp[4]);
            PIX_SORT(temp[0], temp[3]);
            PIX_SORT(temp[1], temp[4]);
            PIX_SORT(temp[1], temp[2]);
            PIX_SORT(temp[2], temp[3]);
            PIX_SORT(temp[1], temp[2]);
            uvf[chrm] = temp[2];
          }
          // use hybrid or pure fdc, depending on what was set above.
          // in case of hybrid, use the chroma that has the smallest
          // absolute value
          float uv[2];
          uv[0] = (((ABS(uvf[0]) < ABS(um)) & (ABS(uvf[1]) < (1.02f * ABS(vm)))) ? uvf[0] : um) * hybrid_fdc[0] + uvf[0] * hybrid_fdc[1];
          uv[1] = (((ABS(uvf[1]) < ABS(vm)) & (ABS(uvf[0]) < (1.02f * ABS(vm)))) ? uvf[1] : vm) * hybrid_fdc[0] + uvf[1] * hybrid_fdc[1];
          // combine the luma from Markesteijn with the chroma from above
          rgbpix[0] = y + 1.474600014746f * uv[1];
          rgbpix[1] = y - 0.15498578286403f * uv[0] - 0.571353132557189f * uv[1];
          rgbpix[2] = y + 1.77201282937288f * uv[0];
          for(int c = 0; c < 3; c++) out[4 * (width * (row + top) + col + left) + c] = rgbpix[c];
        }
    }
  }
  dt_free_align(all_buffers);
}

#undef PIX_SWAP
#undef PIX_SORT
#undef CCLIP
#undef TS

/* taken from dcraw and demosaic_ppg below */

static void lin_interpolate(
        float *out,
        const float *const in,
        const dt_iop_roi_t *const roi_out,
        const dt_iop_roi_t *const roi_in,
        const uint32_t filters,
        const uint8_t (*const xtrans)[6])
{
  const int colors = (filters == 9) ? 3 : 4;

// border interpolate
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(colors, filters, in, roi_in, roi_out, xtrans) \
  shared(out) \
  schedule(static)
#endif
  for(int row = 0; row < roi_out->height; row++)
    for(int col = 0; col < roi_out->width; col++)
    {
      dt_aligned_pixel_t sum = { 0.0f };
      uint8_t count[4] = { 0 };
      if(col == 1 && row >= 1 && row < roi_out->height - 1) col = roi_out->width - 1;
      // average all the adjoining pixels inside image by color
      for(int y = row - 1; y != row + 2; y++)
        for(int x = col - 1; x != col + 2; x++)
          if(y >= 0 && x >= 0 && y < roi_in->height && x < roi_in->width)
          {
            const int f = fcol(y + roi_in->y, x + roi_in->x, filters, xtrans);
            sum[f] += in[y * roi_in->width + x];
            count[f]++;
          }
      const int f = fcol(row + roi_in->y, col + roi_in->x, filters, xtrans);
      // for current cell, copy the current sensor's color data,
      // interpolate the other two colors from surrounding pixels of
      // their color
      for(int c = 0; c < colors; c++)
      {
        if(c != f && count[c] != 0)
          out[4 * (row * roi_out->width + col) + c] = fmaxf(0.0f, sum[c] / count[c]);
        else
          out[4 * (row * roi_out->width + col) + c] = fmaxf(0.0f, in[row * roi_in->width + col]);
      }
    }

  // build interpolation lookup table which for a given offset in the sensor
  // lists neighboring pixels from which to interpolate:
  // NUM_PIXELS                 # of neighboring pixels to read
  // for(1..NUM_PIXELS):
  //   OFFSET                   # in bytes from current pixel
  //   WEIGHT                   # how much weight to give this neighbor
  //   COLOR                    # sensor color
  // # weights of adjoining pixels not of this pixel's color
  // COLORA TOT_WEIGHT
  // COLORB TOT_WEIGHT
  // COLORPIX                   # color of center pixel

  int(*const lookup)[16][32] = malloc(sizeof(int) * 16 * 16 * 32);

  const int size = (filters == 9) ? 6 : 16;
  for(int row = 0; row < size; row++)
    for(int col = 0; col < size; col++)
    {
      int *ip = &(lookup[row][col][1]);
      int sum[4] = { 0 };
      const int f = fcol(row + roi_in->y, col + roi_in->x, filters, xtrans);
      // make list of adjoining pixel offsets by weight & color
      for(int y = -1; y <= 1; y++)
        for(int x = -1; x <= 1; x++)
        {
          const int weight = 1 << ((y == 0) + (x == 0));
          const int color = fcol(row + y + roi_in->y, col + x + roi_in->x, filters, xtrans);
          if(color == f) continue;
          *ip++ = (roi_in->width * y + x);
          *ip++ = weight;
          *ip++ = color;
          sum[color] += weight;
        }
      lookup[row][col][0] = (ip - &(lookup[row][col][0])) / 3; /* # of neighboring pixels found */
      for(int c = 0; c < colors; c++)
        if(c != f)
        {
          *ip++ = c;
          *ip++ = sum[c];
        }
      *ip = f;
    }

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(colors, in, lookup, roi_in, roi_out, size) \
  shared(out) \
  schedule(static)
#endif
  for(int row = 1; row < roi_out->height - 1; row++)
  {
    float *buf = out + 4 * roi_out->width * row + 4;
    const float *buf_in = in + roi_in->width * row + 1;
    for(int col = 1; col < roi_out->width - 1; col++)
    {
      dt_aligned_pixel_t sum = { 0.0f };
      int *ip = &(lookup[row % size][col % size][0]);
      // for each adjoining pixel not of this pixel's color, sum up its weighted values
      for(int i = *ip++; i--; ip += 3) sum[ip[2]] += buf_in[ip[0]] * ip[1];
      // for each interpolated color, load it into the pixel
      for(int i = colors; --i; ip += 2) buf[*ip] = sum[ip[0]] / ip[1];
      buf[*ip] = fmaxf(0.0f, *buf_in);
      buf += 4;
      buf_in++;
    }
  }

  free(lookup);
}


// VNG interpolate adapted from dcraw 9.20

/*
   This algorithm is officially called:

   "Interpolation using a Threshold-based variable number of gradients"

   described in http://scien.stanford.edu/pages/labsite/1999/psych221/projects/99/tingchen/algodep/vargra.html

   I've extended the basic idea to work with non-Bayer filter arrays.
   Gradients are numbered clockwise from NW=0 to W=7.
*/
static inline void _ensure_abovezero(float *to, float *from, const int floats)
{
  for(int i = 0; i < floats; i++)
    to[i] = fmaxf(0.0f, from[i]);
}

static void vng_interpolate(
        float *out,
        const float *const in,
        const dt_iop_roi_t *const roi_out,
        const dt_iop_roi_t *const roi_in,
        const uint32_t filters,
        const uint8_t (*const xtrans)[6],
        const gboolean only_vng_linear)
{
  static const signed char terms[]
      = { -2, -2, +0, -1, 1, 0x01, -2, -2, +0, +0, 2, 0x01, -2, -1, -1, +0, 1, 0x01, -2, -1, +0, -1, 1, 0x02,
          -2, -1, +0, +0, 1, 0x03, -2, -1, +0, +1, 2, 0x01, -2, +0, +0, -1, 1, 0x06, -2, +0, +0, +0, 2, 0x02,
          -2, +0, +0, +1, 1, 0x03, -2, +1, -1, +0, 1, 0x04, -2, +1, +0, -1, 2, 0x04, -2, +1, +0, +0, 1, 0x06,
          -2, +1, +0, +1, 1, 0x02, -2, +2, +0, +0, 2, 0x04, -2, +2, +0, +1, 1, 0x04, -1, -2, -1, +0, 1, 0x80,
          -1, -2, +0, -1, 1, 0x01, -1, -2, +1, -1, 1, 0x01, -1, -2, +1, +0, 2, 0x01, -1, -1, -1, +1, 1, 0x88,
          -1, -1, +1, -2, 1, 0x40, -1, -1, +1, -1, 1, 0x22, -1, -1, +1, +0, 1, 0x33, -1, -1, +1, +1, 2, 0x11,
          -1, +0, -1, +2, 1, 0x08, -1, +0, +0, -1, 1, 0x44, -1, +0, +0, +1, 1, 0x11, -1, +0, +1, -2, 2, 0x40,
          -1, +0, +1, -1, 1, 0x66, -1, +0, +1, +0, 2, 0x22, -1, +0, +1, +1, 1, 0x33, -1, +0, +1, +2, 2, 0x10,
          -1, +1, +1, -1, 2, 0x44, -1, +1, +1, +0, 1, 0x66, -1, +1, +1, +1, 1, 0x22, -1, +1, +1, +2, 1, 0x10,
          -1, +2, +0, +1, 1, 0x04, -1, +2, +1, +0, 2, 0x04, -1, +2, +1, +1, 1, 0x04, +0, -2, +0, +0, 2, 0x80,
          +0, -1, +0, +1, 2, 0x88, +0, -1, +1, -2, 1, 0x40, +0, -1, +1, +0, 1, 0x11, +0, -1, +2, -2, 1, 0x40,
          +0, -1, +2, -1, 1, 0x20, +0, -1, +2, +0, 1, 0x30, +0, -1, +2, +1, 2, 0x10, +0, +0, +0, +2, 2, 0x08,
          +0, +0, +2, -2, 2, 0x40, +0, +0, +2, -1, 1, 0x60, +0, +0, +2, +0, 2, 0x20, +0, +0, +2, +1, 1, 0x30,
          +0, +0, +2, +2, 2, 0x10, +0, +1, +1, +0, 1, 0x44, +0, +1, +1, +2, 1, 0x10, +0, +1, +2, -1, 2, 0x40,
          +0, +1, +2, +0, 1, 0x60, +0, +1, +2, +1, 1, 0x20, +0, +1, +2, +2, 1, 0x10, +1, -2, +1, +0, 1, 0x80,
          +1, -1, +1, +1, 1, 0x88, +1, +0, +1, +2, 1, 0x08, +1, +0, +2, -1, 1, 0x40, +1, +0, +2, +1, 1, 0x10 };
  static const signed char chood[]
    = { -1, -1, -1, 0, -1, +1, 0, +1, +1, +1, +1, 0, +1, -1, 0, -1 };
  int *ip, *code[16][16];
  // ring buffer pointing to three most recent rows processed (brow[3]
  // is only used for rotating the buffer
  float(*brow[4])[4];
  const int width = roi_out->width, height = roi_out->height;
  const int prow = (filters == 9) ? 6 : 8;
  const int pcol = (filters == 9) ? 6 : 2;
  const int colors = (filters == 9) ? 3 : 4;

  // separate out G1 and G2 in RGGB Bayer patterns
  uint32_t filters4 = filters;
  if(filters == 9 || FILTERS_ARE_4BAYER(filters)) // x-trans or CYGM/RGBE
    filters4 = filters;
  else if((filters & 3) == 1)
    filters4 = filters | 0x03030303u;
  else
    filters4 = filters | 0x0c0c0c0cu;

  lin_interpolate(out, in, roi_out, roi_in, filters4, xtrans);

  // if only linear interpolation is requested we can stop it here
  if(only_vng_linear) return;

  char *buffer
      = (char *)dt_alloc_align(64, sizeof(**brow) * width * 3 + sizeof(*ip) * prow * pcol * 320);
  if(!buffer)
  {
    dt_print(DT_DEBUG_ALWAYS, "[demosaic] not able to allocate VNG buffer\n");
    return;
  }
  for(int row = 0; row < 3; row++) brow[row] = (float(*)[4])buffer + row * width;
  ip = (int *)(buffer + sizeof(**brow) * width * 3);

  for(int row = 0; row < prow; row++) /* Precalculate for VNG */
    for(int col = 0; col < pcol; col++)
    {
      code[row][col] = ip;
      const signed char *cp = terms;
      for(int t = 0; t < 64; t++)
      {
        const int y1 = *cp++, x1 = *cp++;
        const int y2 = *cp++, x2 = *cp++;
        const int weight = *cp++;
        const int grads = *cp++;
        const int color = fcol(row + y1, col + x1, filters4, xtrans);
        if(fcol(row + y2, col + x2, filters4, xtrans) != color) continue;
        const int diag
            = (fcol(row, col + 1, filters4, xtrans) == color && fcol(row + 1, col, filters4, xtrans) == color)
                  ? 2
                  : 1;
        if(abs(y1 - y2) == diag && abs(x1 - x2) == diag) continue;
        *ip++ = (y1 * width + x1) * 4 + color;
        *ip++ = (y2 * width + x2) * 4 + color;
        *ip++ = weight;
        for(int g = 0; g < 8; g++)
          if(grads & 1 << g) *ip++ = g;
        *ip++ = -1;
      }
      *ip++ = INT_MAX;
      cp = chood;
      for(int g = 0; g < 8; g++)
      {
        const int y = *cp++, x = *cp++;
        *ip++ = (y * width + x) * 4;
        const int color = fcol(row, col, filters4, xtrans);
        if(fcol(row + y, col + x, filters4, xtrans) != color
           && fcol(row + y * 2, col + x * 2, filters4, xtrans) == color)
          *ip++ = (y * width + x) * 8 + color;
        else
          *ip++ = 0;
      }
    }

  for(int row = 2; row < height - 2; row++) /* Do VNG interpolation */
  {
#ifdef _OPENMP
#pragma omp parallel for default(none) \
    dt_omp_firstprivate(colors, pcol, prow, roi_in, width, xtrans) \
    shared(row, code, brow, out, filters4) \
    private(ip) \
    schedule(static)
#endif
    for(int col = 2; col < width - 2; col++)
    {
      int g;
      float gval[8] = { 0.0f };
      float *pix = out + 4 * (row * width + col);
      ip = code[(row + roi_in->y) % prow][(col + roi_in->x) % pcol];
      while((g = ip[0]) != INT_MAX) /* Calculate gradients */
      {
        float diff = fabsf(pix[g] - pix[ip[1]]) * ip[2];
        gval[ip[3]] += diff;
        ip += 5;
        if((g = ip[-1]) == -1) continue;
        gval[g] += diff;
        while((g = *ip++) != -1) gval[g] += diff;
      }
      ip++;
      float gmin = gval[0], gmax = gval[0]; /* Choose a threshold */
      for(g = 1; g < 8; g++)
      {
        if(gmin > gval[g]) gmin = gval[g];
        if(gmax < gval[g]) gmax = gval[g];
      }
      if(gmax == 0)
      {
        memcpy(brow[2][col], pix, sizeof(*out) * 4);
        continue;
      }
      const float thold = gmin + (gmax * 0.5f);
      dt_aligned_pixel_t sum = { 0.0f };
      const int color = fcol(row + roi_in->y, col + roi_in->x, filters4, xtrans);
      int num = 0;
      for(g = 0; g < 8; g++, ip += 2) /* Average the neighbors */
      {
        if(gval[g] <= thold)
        {
          for(int c = 0; c < colors; c++)
            if(c == color && ip[1])
              sum[c] += (pix[c] + pix[ip[1]]) * 0.5f;
            else
              sum[c] += pix[ip[0] + c];
          num++;
        }
      }
      for(int c = 0; c < colors; c++) /* Save to buffer */
      {
        float tot = pix[color];
        if(c != color) tot += (sum[c] - sum[color]) / num;
        brow[2][col][c] = tot;
      }
    }
    if(row > 3) /* Write buffer to image */
      _ensure_abovezero(out + 4 * ((row - 2) * width + 2), (float *)(brow[0] + 2), 4 * (width - 4));

    // rotate ring buffer
    for(int g = 0; g < 4; g++) brow[(g - 1) & 3] = brow[g];
  }
  // copy the final two rows to the image
  _ensure_abovezero(out + (4 * ((height - 4) * width + 2)), (float *)(brow[0] + 2), 4 * (width - 4));
  _ensure_abovezero(out + (4 * ((height - 3) * width + 2)), (float *)(brow[1] + 2), 4 * (width - 4));
  dt_free_align(buffer);

  if(filters != 9 && !FILTERS_ARE_4BAYER(filters)) // x-trans or CYGM/RGBE
// for Bayer mix the two greens to make VNG4
#ifdef _OPENMP
#pragma omp parallel for default(none) \
    dt_omp_firstprivate(height, width) \
    shared(out) \
    schedule(static)
#endif
    for(int i = 0; i < height * width; i++) out[i * 4 + 1] = (out[i * 4 + 1] + out[i * 4 + 3]) / 2.0f;
}

/** 1:1 demosaic from in to out, in is full buf, out is translated/cropped (scale == 1.0!) */
static void passthrough_monochrome(
        float *out,
        const float *const in,
        dt_iop_roi_t *const roi_out,
        const dt_iop_roi_t *const roi_in)
{
  // we never want to access the input out of bounds though:
  assert(roi_in->width >= roi_out->width);
  assert(roi_in->height >= roi_out->height);

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(in, roi_out, roi_in) \
  shared(out) \
  schedule(static) collapse(2)
#endif
  for(int j = 0; j < roi_out->height; j++)
  {
    for(int i = 0; i < roi_out->width; i++)
    {
      for(int c = 0; c < 3; c++)
      {
        out[(size_t)4 * ((size_t)j * roi_out->width + i) + c]
            = in[(size_t)((size_t)j + roi_out->y) * roi_in->width + i + roi_out->x];
      }
    }
  }
}

static void passthrough_color(
        float *out,
        const float *const in,
        dt_iop_roi_t *const roi_out,
        const dt_iop_roi_t *const roi_in,
        const uint32_t filters,
        const uint8_t (*const xtrans)[6])
{
  assert(roi_in->width >= roi_out->width);
  assert(roi_in->height >= roi_out->height);

  if(filters != 9u)
  {
    #ifdef _OPENMP
      #pragma omp parallel for default(none) \
      dt_omp_firstprivate(in, roi_out, roi_in, filters) \
      shared(out) \
      schedule(static) \
      collapse(2)
    #endif

    for(int row = 0; row < (roi_out->height); row++)
    {
      for(int col = 0; col < (roi_out->width); col++)
      {
        const float val = in[col + roi_out->x + ((row + roi_out->y) * roi_in->width)];
        const uint32_t offset = (size_t)4 * ((size_t)row * roi_out->width + col);
        const uint32_t ch = FC(row + roi_out->y, col + roi_out->x, filters);

        out[offset] = out[offset + 1] = out[offset + 2] = 0.0f;
        out[offset + ch] = val;
      }
    }
  }
  else
  {
    #ifdef _OPENMP
      #pragma omp parallel for default(none) \
      dt_omp_firstprivate(in, roi_out, roi_in, xtrans) \
      shared(out) \
      schedule(static) \
      collapse(2)
    #endif

    for(int row = 0; row < (roi_out->height); row++)
    {
      for(int col = 0; col < (roi_out->width); col++)
      {
        const float val = in[col + roi_out->x + ((row + roi_out->y) * roi_in->width)];
        const uint32_t offset = (size_t)4 * ((size_t)row * roi_out->width + col);
        const uint32_t ch = FCxtrans(row, col, roi_in, xtrans);

        out[offset] = out[offset + 1] = out[offset + 2] = 0.0f;
        out[offset + ch] = val;
      }
    }
  }
}

/** 1:1 demosaic from in to out, in is full buf, out is translated/cropped (scale == 1.0!) */
#ifdef _OPENMP
  #pragma omp declare simd aligned(in, out)
#endif
static void demosaic_ppg(
        float *const out,
        const float *const in,
        const dt_iop_roi_t *const roi_out,
        const dt_iop_roi_t *const roi_in,
        const uint32_t filters,
        const float thrs)
{
  // these may differ a little, if you're unlucky enough to split a bayer block with cropping or similar.
  // we never want to access the input out of bounds though:
  assert(roi_in->width >= roi_out->width);
  assert(roi_in->height >= roi_out->height);
  // border interpolate
  float sum[8];
  for(int j = 0; j < roi_out->height; j++)
    for(int i = 0; i < roi_out->width; i++)
    {
      if(i == 3 && j >= 3 && j < roi_out->height - 3) i = roi_out->width - 3;
      if(i == roi_out->width) break;
      memset(sum, 0, sizeof(float) * 8);
      for(int y = j - 1; y != j + 2; y++)
        for(int x = i - 1; x != i + 2; x++)
        {
          const int yy = y + roi_out->y, xx = x + roi_out->x;
          if((yy >= 0) && (xx >= 0) && (yy < roi_in->height) && (xx < roi_in->width))
          {
            const int f = FC(y, x, filters);
            sum[f] += in[(size_t)yy * roi_in->width + xx];
            sum[f + 4]++;
          }
        }
      const int f = FC(j, i, filters);
      for(int c = 0; c < 3; c++)
      {
        if(c != f && sum[c + 4] > 0.0f)
          out[4 * ((size_t)j * roi_out->width + i) + c] = fmaxf(0.0f, sum[c] / sum[c + 4]);
        else
          out[4 * ((size_t)j * roi_out->width + i) + c]
              = fmaxf(0.0f, in[((size_t)j + roi_out->y) * roi_in->width + i + roi_out->x]);
      }
    }
  const int median = thrs > 0.0f;
  // if(median) fbdd_green(out, in, roi_out, roi_in, filters);
  const float *input = in;
  if(median)
  {
    float *med_in = (float *)dt_alloc_align_float((size_t)roi_in->height * roi_in->width);
    pre_median(med_in, in, roi_in, filters, 1, thrs);
    input = med_in;
  }
// for all pixels except those in the 3 pixel border:
// interpolate green from input into out float array, or copy color.
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(filters, out, roi_in, roi_out) \
  shared(input) \
  schedule(static)
#endif
  for(int j = 3; j < roi_out->height - 3; j++)
  {
    float *buf = out + (size_t)4 * roi_out->width * j + 4 * 3;
    const float *buf_in = input + (size_t)roi_in->width * (j + roi_out->y) + 3 + roi_out->x;
    for(int i = 3; i < roi_out->width - 3; i++)
    {
      const int c = FC(j, i, filters);
      dt_aligned_pixel_t color;
      const float pc = buf_in[0];
      // if(__builtin_expect(c == 0 || c == 2, 1))
      if(c == 0 || c == 2)
      {
        color[c] = pc;
        // get stuff (hopefully from cache)
        const float pym = buf_in[-roi_in->width * 1];
        const float pym2 = buf_in[-roi_in->width * 2];
        const float pym3 = buf_in[-roi_in->width * 3];
        const float pyM = buf_in[+roi_in->width * 1];
        const float pyM2 = buf_in[+roi_in->width * 2];
        const float pyM3 = buf_in[+roi_in->width * 3];
        const float pxm = buf_in[-1];
        const float pxm2 = buf_in[-2];
        const float pxm3 = buf_in[-3];
        const float pxM = buf_in[+1];
        const float pxM2 = buf_in[+2];
        const float pxM3 = buf_in[+3];

        const float guessx = (pxm + pc + pxM) * 2.0f - pxM2 - pxm2;
        const float diffx = (fabsf(pxm2 - pc) + fabsf(pxM2 - pc) + fabsf(pxm - pxM)) * 3.0f
                            + (fabsf(pxM3 - pxM) + fabsf(pxm3 - pxm)) * 2.0f;
        const float guessy = (pym + pc + pyM) * 2.0f - pyM2 - pym2;
        const float diffy = (fabsf(pym2 - pc) + fabsf(pyM2 - pc) + fabsf(pym - pyM)) * 3.0f
                            + (fabsf(pyM3 - pyM) + fabsf(pym3 - pym)) * 2.0f;
        if(diffx > diffy)
        {
          // use guessy
          const float m = fminf(pym, pyM);
          const float M = fmaxf(pym, pyM);
          color[1] = fmaxf(fminf(guessy * .25f, M), m);
        }
        else
        {
          const float m = fminf(pxm, pxM);
          const float M = fmaxf(pxm, pxM);
          color[1] = fmaxf(fminf(guessx * .25f, M), m);
        }
      }
      else
        color[1] = pc;

      color[3] = 0.0f;

      for_each_channel(k,aligned(buf,color:16)
        dt_omp_nontemporal(buf)) buf[k] = fmaxf(0.0f, color[k]);

      buf += 4;
      buf_in++;
    }
  }

// for all pixels except the outermost row/column:
// interpolate colors using out as input into float out array
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(filters, out, roi_out) \
  schedule(static)
#endif
  for(int j = 1; j < roi_out->height - 1; j++)
  {
    float *buf = out + (size_t)4 * roi_out->width * j + 4;
    for(int i = 1; i < roi_out->width - 1; i++)
    {
      // also prefetch direct nbs top/bottom
      const int c = FC(j, i, filters);
      dt_aligned_pixel_t color = { buf[0], buf[1], buf[2], buf[3] };

      // fill all four pixels with correctly interpolated stuff: r/b for green1/2
      // b for r and r for b
      if(__builtin_expect(c & 1, 1)) // c == 1 || c == 3)
      {
        // calculate red and blue for green pixels:
        // need 4-nbhood:
        const float *nt = buf - 4 * roi_out->width;
        const float *nb = buf + 4 * roi_out->width;
        const float *nl = buf - 4;
        const float *nr = buf + 4;
        if(FC(j, i + 1, filters) == 0) // red nb in same row
        {
          color[2] = (nt[2] + nb[2] + 2.0f * color[1] - nt[1] - nb[1]) * .5f;
          color[0] = (nl[0] + nr[0] + 2.0f * color[1] - nl[1] - nr[1]) * .5f;
        }
        else
        {
          // blue nb
          color[0] = (nt[0] + nb[0] + 2.0f * color[1] - nt[1] - nb[1]) * .5f;
          color[2] = (nl[2] + nr[2] + 2.0f * color[1] - nl[1] - nr[1]) * .5f;
        }
      }
      else
      {
        // get 4-star-nbhood:
        const float *ntl = buf - 4 - 4 * roi_out->width;
        const float *ntr = buf + 4 - 4 * roi_out->width;
        const float *nbl = buf - 4 + 4 * roi_out->width;
        const float *nbr = buf + 4 + 4 * roi_out->width;

        if(c == 0)
        {
          // red pixel, fill blue:
          const float diff1 = fabsf(ntl[2] - nbr[2]) + fabsf(ntl[1] - color[1]) + fabsf(nbr[1] - color[1]);
          const float guess1 = ntl[2] + nbr[2] + 2.0f * color[1] - ntl[1] - nbr[1];
          const float diff2 = fabsf(ntr[2] - nbl[2]) + fabsf(ntr[1] - color[1]) + fabsf(nbl[1] - color[1]);
          const float guess2 = ntr[2] + nbl[2] + 2.0f * color[1] - ntr[1] - nbl[1];
          if(diff1 > diff2)
            color[2] = guess2 * .5f;
          else if(diff1 < diff2)
            color[2] = guess1 * .5f;
          else
            color[2] = (guess1 + guess2) * .25f;
        }
        else // c == 2, blue pixel, fill red:
        {
          const float diff1 = fabsf(ntl[0] - nbr[0]) + fabsf(ntl[1] - color[1]) + fabsf(nbr[1] - color[1]);
          const float guess1 = ntl[0] + nbr[0] + 2.0f * color[1] - ntl[1] - nbr[1];
          const float diff2 = fabsf(ntr[0] - nbl[0]) + fabsf(ntr[1] - color[1]) + fabsf(nbl[1] - color[1]);
          const float guess2 = ntr[0] + nbl[0] + 2.0f * color[1] - ntr[1] - nbl[1];
          if(diff1 > diff2)
            color[0] = guess2 * .5f;
          else if(diff1 < diff2)
            color[0] = guess1 * .5f;
          else
            color[0] = (guess1 + guess2) * .25f;
        }
      }

      for_each_channel(k,aligned(buf,color:16)
        dt_omp_nontemporal(buf)) buf[k] = fmaxf(0.0f, color[k]);

      buf += 4;
    }
  }
  // _mm_sfence();
  if(median) dt_free_align((float *)input);
}

void distort_mask(
        struct dt_iop_module_t *self,
        struct dt_dev_pixelpipe_iop_t *piece,
        const float *const in,
        float *const out,
        const dt_iop_roi_t *const roi_in,
        const dt_iop_roi_t *const roi_out)
{
  const struct dt_interpolation *itor = dt_interpolation_new(DT_INTERPOLATION_USERPREF);
  dt_interpolation_resample_roi_1c(itor, out, roi_out, roi_out->width * sizeof(float), in, roi_in,
                                   roi_in->width * sizeof(float));
}

void modify_roi_out(
        struct dt_iop_module_t *self,
        struct dt_dev_pixelpipe_iop_t *piece,
        dt_iop_roi_t *roi_out,
        const dt_iop_roi_t *const roi_in)
{
  *roi_out = *roi_in;

  // snap to start of mosaic block:
  roi_out->x = 0; // MAX(0, roi_out->x & ~1);
  roi_out->y = 0; // MAX(0, roi_out->y & ~1);
}

// which roi input is needed to process to this output?
// roi_out is unchanged, full buffer in is full buffer out.
void modify_roi_in(
        struct dt_iop_module_t *self,
        struct dt_dev_pixelpipe_iop_t *piece,
        const dt_iop_roi_t *roi_out,
        dt_iop_roi_t *roi_in)
{
  // this op is disabled for preview pipe/filters == 0

  *roi_in = *roi_out;
  // need 1:1, demosaic and then sub-sample. or directly sample half-size
  roi_in->x /= roi_out->scale;
  roi_in->y /= roi_out->scale;
  roi_in->width /= roi_out->scale;
  roi_in->height /= roi_out->scale;
  roi_in->scale = 1.0f;

  dt_iop_demosaic_data_t *data = (dt_iop_demosaic_data_t *)piece->data;
  const int method = data->demosaicing_method;
  const gboolean passthrough = (method == DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME) ||
                               (method == DT_IOP_DEMOSAIC_PASSTHR_MONOX);

  // set position to closest sensor pattern snap
  if(!passthrough)
  {
    const int aligner = (piece->pipe->dsc.filters != 9u) ? DT_BAYER_SNAPPER : DT_XTRANS_SNAPPER;
    const int dx = roi_in->x % aligner;
    const int dy = roi_in->y % aligner;
    const int shift_x = (dx > aligner / 2) ? aligner - dx : -dx;
    const int shift_y = (dy > aligner / 2) ? aligner - dy : -dy;

    roi_in->x = MAX(0, roi_in->x + shift_x);
    roi_in->y = MAX(0, roi_in->y + shift_y);
  }

  // clamp numeric inaccuracies to full buffer, to avoid scaling/copying in pixelpipe:
  if(abs(piece->pipe->image.width - roi_in->width) < MAX(ceilf(1.0f / roi_out->scale), 10))
    roi_in->width = piece->pipe->image.width;

  if(abs(piece->pipe->image.height - roi_in->height) < MAX(ceilf(1.0f / roi_out->scale), 10))
    roi_in->height = piece->pipe->image.height;
}

static int get_quality()
{
  int qual = 1;
  const char *quality = dt_conf_get_string("plugins/darkroom/demosaic/quality");
  if(quality)
  {
    if(!strcmp(quality, "always bilinear (fast)"))
      qual = 0;
    else if(!strcmp(quality, "full (possibly slow)"))
      qual = 2;
  }
  return qual;
}

static int get_thumb_quality(int width, int height)
{
  // we check if we need ultra-high quality thumbnail for this size
  const int level = dt_mipmap_cache_get_matching_size(darktable.mipmap_cache, width, height);
  const char *min = dt_conf_get_string_const("plugins/lighttable/thumbnail_hq_min_level");
  const dt_mipmap_size_t min_s = dt_mipmap_cache_get_min_mip_from_pref(min);

  return (level >= min_s);
}

// set flags for demosaic quality based on factors besides demosaic
// method (e.g. config, scale, pixelpipe type)
static int demosaic_qual_flags(
        const dt_dev_pixelpipe_iop_t *const piece,
        const dt_image_t *const img,
        const dt_iop_roi_t *const roi_out)
{
  int flags = 0;
  switch(piece->pipe->type & DT_DEV_PIXELPIPE_ANY)
  {
    case DT_DEV_PIXELPIPE_FULL:
    case DT_DEV_PIXELPIPE_PREVIEW2:
      {
        const int qual = get_quality();
        if(qual > 0) flags |= DT_DEMOSAIC_FULL_SCALE;
        if(qual > 1) flags |= DT_DEMOSAIC_XTRANS_FULL;
        if((qual < 2) && (roi_out->scale <= .99999f))
          flags |= DT_DEMOSAIC_MEDIUM_QUAL;
      }
      break;
    case DT_DEV_PIXELPIPE_EXPORT:
      flags |= DT_DEMOSAIC_FULL_SCALE | DT_DEMOSAIC_XTRANS_FULL;
      break;
    case DT_DEV_PIXELPIPE_THUMBNAIL:
      // we check if we need ultra-high quality thumbnail for this size
      if(get_thumb_quality(roi_out->width, roi_out->height))
      {
        flags |= DT_DEMOSAIC_FULL_SCALE | DT_DEMOSAIC_XTRANS_FULL;
      }
      break;
    default: // make C not complain about missing enum members
      break;
  }

  // For sufficiently small scaling, one or more repetitition of the
  // CFA pattern can be merged into a single pixel, hence it is
  // possible to skip the full demosaic and perform a quick downscale.
  // Note even though the X-Trans CFA is 6x6, for this purposes we can
  // see each 6x6 tile as four fairly similar 3x3 tiles
  if(roi_out->scale > (piece->pipe->dsc.filters == 9u ? 0.333f : 0.5f))
  {
    flags |= DT_DEMOSAIC_FULL_SCALE;
  }
  // half_size_f doesn't support 4bayer images
  if(img->flags & DT_IMAGE_4BAYER) flags |= DT_DEMOSAIC_FULL_SCALE;
  // we use full Markesteijn demosaicing on xtrans sensors if maximum
  // quality is required
  if(roi_out->scale > 0.667f)
  {
    flags |= DT_DEMOSAIC_XTRANS_FULL;
  }

  // we check if we can stop at the linear interpolation step in VNG
  // instead of going the full way
  if((flags & DT_DEMOSAIC_FULL_SCALE) &&
      (roi_out->scale < (piece->pipe->dsc.filters == 9u ? 0.5f : 0.667f)))
  {
    flags |= DT_DEMOSAIC_ONLY_VNG_LINEAR;
  }

  return flags;
}

#include "dual_demosaic.c"

void process(
        struct dt_iop_module_t *self,
        dt_dev_pixelpipe_iop_t *piece,
        const void *const i,
        void *const o,
        const dt_iop_roi_t *const roi_in,
        const dt_iop_roi_t *const roi_out)
{
  const dt_image_t *img = &self->dev->image_storage;
  const float threshold = 0.0001f * img->exif_iso;

  dt_dev_clear_rawdetail_mask(piece->pipe);

  dt_iop_roi_t roi = *roi_in;
  dt_iop_roi_t roo = *roi_out;
  roo.x = roo.y = 0;
  // roi_out->scale = global scale: (iscale == 1.0, always when demosaic is on)
  const gboolean run_fast = piece->pipe->type & DT_DEV_PIXELPIPE_FAST;

  const uint8_t(*const xtrans)[6] = (const uint8_t(*const)[6])piece->pipe->dsc.xtrans;

  dt_iop_demosaic_data_t *data = (dt_iop_demosaic_data_t *)piece->data;
  dt_iop_demosaic_global_data_t *gd = (dt_iop_demosaic_global_data_t *)self->global_data;

  const int qual_flags = demosaic_qual_flags(piece, img, roi_out);
  int demosaicing_method = data->demosaicing_method;

  gboolean showmask = FALSE;
  if(self->dev->gui_attached && (piece->pipe->type & DT_DEV_PIXELPIPE_FULL))
  {
    dt_iop_demosaic_gui_data_t *g = (dt_iop_demosaic_gui_data_t *)self->gui_data;
    showmask = (g->visual_mask);
    // take care of passthru modes
    if(piece->pipe->mask_display == DT_DEV_PIXELPIPE_DISPLAY_PASSTHRU)
      demosaicing_method = (piece->pipe->dsc.filters != 9u) ? DT_IOP_DEMOSAIC_RCD : DT_IOP_DEMOSAIC_MARKESTEIJN;
  }

  if((qual_flags & DT_DEMOSAIC_MEDIUM_QUAL)
  // only overwrite setting if quality << requested and in dr mode and not a special method
  && (demosaicing_method != DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME)
  && (demosaicing_method != DT_IOP_DEMOSAIC_PASSTHROUGH_COLOR)
  // dual demosaicing with the show mask option on is also a special method for ui
  && !((demosaicing_method & DT_DEMOSAIC_DUAL) && showmask))
    demosaicing_method = (piece->pipe->dsc.filters != 9u) ? DT_IOP_DEMOSAIC_RCD : DT_IOP_DEMOSAIC_MARKESTEIJN;

  const float *const pixels = (float *)i;

  if(qual_flags & DT_DEMOSAIC_FULL_SCALE)
  {
    // Full demosaic and then scaling if needed
    const int scaled = (roi_out->width != roi_in->width || roi_out->height != roi_in->height);
    float *tmp = (float *) o;
    if(scaled)
    {
      // demosaic and then clip and zoom
      // we demosaic at 1:1 the size of input roi, so make sure
      // we fit these bounds exactly, to avoid crashes..
      roo.width = roi_in->width;
      roo.height = roi_in->height;
      roo.scale = 1.0f;
      tmp = (float *)dt_alloc_align_float((size_t)4 * roo.width * roo.height);
    }

    if(demosaicing_method == DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME)
    {
      passthrough_monochrome(tmp, pixels, &roo, &roi);
    }
    else if(demosaicing_method == DT_IOP_DEMOSAIC_PASSTHROUGH_COLOR)
    {
      passthrough_color(tmp, pixels, &roo, &roi, piece->pipe->dsc.filters, xtrans);
    }
    else if(piece->pipe->dsc.filters == 9u)
    {
      const int passes = (demosaicing_method == DT_IOP_DEMOSAIC_MARKESTEIJN) ? 1 : 3;
      if(demosaicing_method == DT_IOP_DEMOSAIC_MARKEST3_VNG)
        xtrans_markesteijn_interpolate(tmp, pixels, &roo, &roi, xtrans, passes);
      else if(demosaicing_method == DT_IOP_DEMOSAIC_FDC && (qual_flags & DT_DEMOSAIC_XTRANS_FULL))
        xtrans_fdc_interpolate(self, tmp, pixels, &roo, &roi, xtrans);
      else if(demosaicing_method >= DT_IOP_DEMOSAIC_MARKESTEIJN && (qual_flags & DT_DEMOSAIC_XTRANS_FULL))
        xtrans_markesteijn_interpolate(tmp, pixels, &roo, &roi, xtrans, passes);
      else
        vng_interpolate(tmp, pixels, &roo, &roi, piece->pipe->dsc.filters, xtrans, qual_flags & DT_DEMOSAIC_ONLY_VNG_LINEAR);
    }
    else
    {
      float *in = (float *)pixels;
      float *aux;

      if(!(img->flags & DT_IMAGE_4BAYER) && data->green_eq != DT_IOP_GREEN_EQ_NO)
      {
        in = (float *)dt_alloc_align_float((size_t)roi_in->height * roi_in->width);
        switch(data->green_eq)
        {
          case DT_IOP_GREEN_EQ_FULL:
            green_equilibration_favg(in, pixels, roi_in->width, roi_in->height, piece->pipe->dsc.filters,
                                     roi_in->x, roi_in->y);
            break;
          case DT_IOP_GREEN_EQ_LOCAL:
            green_equilibration_lavg(in, pixels, roi_in->width, roi_in->height, piece->pipe->dsc.filters,
                                     roi_in->x, roi_in->y, threshold);
            break;
          case DT_IOP_GREEN_EQ_BOTH:
            aux = dt_alloc_align_float((size_t)roi_in->height * roi_in->width);
            green_equilibration_favg(aux, pixels, roi_in->width, roi_in->height, piece->pipe->dsc.filters,
                                     roi_in->x, roi_in->y);
            green_equilibration_lavg(in, aux, roi_in->width, roi_in->height, piece->pipe->dsc.filters, roi_in->x,
                                     roi_in->y, threshold);
            dt_free_align(aux);
            break;
        }
      }

      if(demosaicing_method == DT_IOP_DEMOSAIC_VNG4 || (img->flags & DT_IMAGE_4BAYER))
      {
        vng_interpolate(tmp, in, &roo, &roi, piece->pipe->dsc.filters, xtrans, qual_flags & DT_DEMOSAIC_ONLY_VNG_LINEAR);
        if(img->flags & DT_IMAGE_4BAYER)
        {
          dt_colorspaces_cygm_to_rgb(tmp, roo.width*roo.height, data->CAM_to_RGB);
          dt_colorspaces_cygm_to_rgb(piece->pipe->dsc.processed_maximum, 1, data->CAM_to_RGB);
        }
      }
      else if((demosaicing_method & ~DT_DEMOSAIC_DUAL) == DT_IOP_DEMOSAIC_RCD)
      {
        rcd_demosaic(piece, tmp, in, &roo, &roi, piece->pipe->dsc.filters);
      }
      else if(demosaicing_method == DT_IOP_DEMOSAIC_LMMSE)
      {
        if(gd->lmmse_gamma_in == NULL)
        {
          gd->lmmse_gamma_in = dt_alloc_align_float(65536);
          gd->lmmse_gamma_out = dt_alloc_align_float(65536);
#ifdef _OPENMP
    #pragma omp for
#endif
          for(int j = 0; j < 65536; j++)
          {
            const double x = (double)j / 65535.0;
            gd->lmmse_gamma_in[j]  = (x <= 0.001867) ? x * 17.0 : 1.044445 * exp(log(x) / 2.4) - 0.044445;
            gd->lmmse_gamma_out[j] = (x <= 0.031746) ? x / 17.0 : exp(log((x + 0.044445) / 1.044445) * 2.4);
          }
        }
        lmmse_demosaic(piece, tmp, in, &roo, &roi, piece->pipe->dsc.filters, data->lmmse_refine, gd->lmmse_gamma_in, gd->lmmse_gamma_out);
      }
      else if((demosaicing_method & ~DT_DEMOSAIC_DUAL) != DT_IOP_DEMOSAIC_AMAZE)
        demosaic_ppg(tmp, in, &roo, &roi, piece->pipe->dsc.filters,
                     data->median_thrs); // wanted ppg or zoomed out a lot and quality is limited to 1
      else
        amaze_demosaic_RT(piece, in, tmp, &roi, &roo, piece->pipe->dsc.filters);

      if(!(img->flags & DT_IMAGE_4BAYER) && data->green_eq != DT_IOP_GREEN_EQ_NO) dt_free_align(in);
    }

    dt_dev_write_rawdetail_mask(piece, tmp, roi_in, DT_DEV_DETAIL_MASK_DEMOSAIC);

    if((demosaicing_method & DT_DEMOSAIC_DUAL) && !run_fast)
    {
      dual_demosaic(piece, tmp, pixels, &roo, &roi, piece->pipe->dsc.filters, xtrans, showmask, data->dual_thrs);
    }

    if(scaled)
    {
      roi = *roi_out;
      dt_print_pipe(DT_DEBUG_PIPE, "clip_and_zoom_roi", piece->pipe, self->so->op, roi_in, roi_out, "\n");
      dt_iop_clip_and_zoom_roi((float *)o, tmp, &roi, &roo, roi.width, roo.width);
      dt_free_align(tmp);
    }
  }
  else
  {
    if(demosaicing_method == DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME)
      dt_iop_clip_and_zoom_demosaic_passthrough_monochrome_f((float *)o, pixels, &roo, &roi, roo.width, roi.width);
    else if(demosaicing_method == DT_IOP_DEMOSAIC_PASSTHROUGH_COLOR)
       dt_iop_clip_and_zoom_demosaic_passthrough_monochrome_f((float *)o, pixels, &roo, &roi, roo.width, roi.width);
    else if(piece->pipe->dsc.filters == 9u)
      dt_iop_clip_and_zoom_demosaic_third_size_xtrans_f((float *)o, pixels, &roo, &roi, roo.width, roi.width, xtrans);
    else
      dt_iop_clip_and_zoom_demosaic_half_size_f((float *)o, pixels, &roo, &roi, roo.width, roi.width,
                                                piece->pipe->dsc.filters);

    // this is used for preview pipes, currently there is now writing mask implemented
    // we just clear the mask data as we might have changed the preview downsampling
    dt_dev_clear_rawdetail_mask(piece->pipe);
  }
  if(data->color_smoothing)
    color_smoothing(o, roi_out, data->color_smoothing);
}

#ifdef HAVE_OPENCL
// color smoothing step by multiple passes of median filtering
static int color_smoothing_cl(
        struct dt_iop_module_t *self,
        dt_dev_pixelpipe_iop_t *piece,
        cl_mem dev_in,
        cl_mem dev_out,
        const dt_iop_roi_t *const roi_out,
        const int passes)
{
  dt_iop_demosaic_global_data_t *gd = (dt_iop_demosaic_global_data_t *)self->global_data;

  const int devid = piece->pipe->devid;
  const int width = roi_out->width;
  const int height = roi_out->height;

  cl_int err = DT_OPENCL_DEFAULT_ERROR;

  cl_mem dev_tmp = dt_opencl_alloc_device(devid, width, height, sizeof(float) * 4);
  if(dev_tmp == NULL) goto error;

  dt_opencl_local_buffer_t locopt
    = (dt_opencl_local_buffer_t){ .xoffset = 2*1, .xfactor = 1, .yoffset = 2*1, .yfactor = 1,
                                  .cellsize = 4 * sizeof(float), .overhead = 0,
                                  .sizex = 1 << 8, .sizey = 1 << 8 };

  if(!dt_opencl_local_buffer_opt(devid, gd->kernel_color_smoothing, &locopt))
    goto error;

  // two buffer references for our ping-pong
  cl_mem dev_t1 = dev_out;
  cl_mem dev_t2 = dev_tmp;

  for(int pass = 0; pass < passes; pass++)
  {
    size_t sizes[] = { ROUNDUP(width, locopt.sizex), ROUNDUP(height, locopt.sizey), 1 };
    size_t local[] = { locopt.sizex, locopt.sizey, 1 };
    dt_opencl_set_kernel_args(devid, gd->kernel_color_smoothing, 0, CLARG(dev_t1), CLARG(dev_t2), CLARG(width),
      CLARG(height), CLLOCAL(sizeof(float) * 4 * (locopt.sizex + 2) * (locopt.sizey + 2)));
    err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_color_smoothing, sizes, local);
    if(err != CL_SUCCESS) goto error;

    // swap dev_t1 and dev_t2
    cl_mem t = dev_t1;
    dev_t1 = dev_t2;
    dev_t2 = t;
  }

  // after last step we find final output in dev_t1.
  // let's see if this is in dev_tmp1 and needs to be copied to dev_out
  if(dev_t1 == dev_tmp)
  {
    // copy data from dev_tmp -> dev_out
    size_t origin[] = { 0, 0, 0 };
    size_t region[] = { width, height, 1 };
    err = dt_opencl_enqueue_copy_image(devid, dev_tmp, dev_out, origin, origin, region);
    if(err != CL_SUCCESS) goto error;
  }

  dt_opencl_release_mem_object(dev_tmp);
  return TRUE;

error:
  dt_opencl_release_mem_object(dev_tmp);
  dt_print(DT_DEBUG_OPENCL, "[opencl_demosaic_color_smoothing] couldn't enqueue kernel! %s\n", cl_errstr(err));
  return FALSE;
}

static int green_equilibration_cl(
        struct dt_iop_module_t *self,
        dt_dev_pixelpipe_iop_t *piece,
        cl_mem dev_in,
        cl_mem dev_out,
        const dt_iop_roi_t *const roi_in)
{
  dt_iop_demosaic_data_t *data = (dt_iop_demosaic_data_t *)piece->data;
  dt_iop_demosaic_global_data_t *gd = (dt_iop_demosaic_global_data_t *)self->global_data;

  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;

  cl_mem dev_tmp = NULL;
  cl_mem dev_m = NULL;
  cl_mem dev_r = NULL;
  cl_mem dev_in1 = NULL;
  cl_mem dev_out1 = NULL;
  cl_mem dev_in2 = NULL;
  cl_mem dev_out2 = NULL;
  float *sumsum = NULL;

  cl_int err = DT_OPENCL_DEFAULT_ERROR;

  if(data->green_eq == DT_IOP_GREEN_EQ_BOTH)
  {
    dev_tmp = dt_opencl_alloc_device(devid, width, height, sizeof(float));
    if(dev_tmp == NULL) goto error;
  }

  switch(data->green_eq)
  {
    case DT_IOP_GREEN_EQ_FULL:
      dev_in1 = dev_in;
      dev_out1 = dev_out;
      break;
    case DT_IOP_GREEN_EQ_LOCAL:
      dev_in2 = dev_in;
      dev_out2 = dev_out;
      break;
    case DT_IOP_GREEN_EQ_BOTH:
      dev_in1 = dev_in;
      dev_out1 = dev_tmp;
      dev_in2 = dev_tmp;
      dev_out2 = dev_out;
      break;
    case DT_IOP_GREEN_EQ_NO:
    default:
      goto error;
  }

  if(data->green_eq == DT_IOP_GREEN_EQ_FULL || data->green_eq == DT_IOP_GREEN_EQ_BOTH)
  {
    dt_opencl_local_buffer_t flocopt
      = (dt_opencl_local_buffer_t){ .xoffset = 0, .xfactor = 1, .yoffset = 0, .yfactor = 1,
                                    .cellsize = 2 * sizeof(float), .overhead = 0,
                                    .sizex = 1 << 4, .sizey = 1 << 4 };

    if(!dt_opencl_local_buffer_opt(devid, gd->kernel_green_eq_favg_reduce_first, &flocopt))
      goto error;

    const size_t bwidth = ROUNDUP(width, flocopt.sizex);
    const size_t bheight = ROUNDUP(height, flocopt.sizey);

    const int bufsize = (bwidth / flocopt.sizex) * (bheight / flocopt.sizey);

    dev_m = dt_opencl_alloc_device_buffer(devid, sizeof(float) * 2 * bufsize);
    if(dev_m == NULL) goto error;

    size_t fsizes[3] = { bwidth, bheight, 1 };
    size_t flocal[3] = { flocopt.sizex, flocopt.sizey, 1 };
    dt_opencl_set_kernel_args(devid, gd->kernel_green_eq_favg_reduce_first, 0, CLARG(dev_in1), CLARG(width),
      CLARG(height), CLARG(dev_m), CLARG(piece->pipe->dsc.filters), CLARG(roi_in->x), CLARG(roi_in->y),
      CLLOCAL(sizeof(float) * 2 * flocopt.sizex * flocopt.sizey));
    err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_green_eq_favg_reduce_first, fsizes,
                                                 flocal);
    if(err != CL_SUCCESS) goto error;

    dt_opencl_local_buffer_t slocopt
      = (dt_opencl_local_buffer_t){ .xoffset = 0, .xfactor = 1, .yoffset = 0, .yfactor = 1,
                                    .cellsize = sizeof(float) * 2, .overhead = 0,
                                    .sizex = 1 << 16, .sizey = 1 };

    if(!dt_opencl_local_buffer_opt(devid, gd->kernel_green_eq_favg_reduce_second, &slocopt))
      goto error;

    const int reducesize = MIN(DT_REDUCESIZE_MIN, ROUNDUP(bufsize, slocopt.sizex) / slocopt.sizex);

    dev_r = dt_opencl_alloc_device_buffer(devid, sizeof(float) * 2 * reducesize);
    if(dev_r == NULL) goto error;

    size_t ssizes[3] = { (size_t)reducesize * slocopt.sizex, 1, 1 };
    size_t slocal[3] = { slocopt.sizex, 1, 1 };
    dt_opencl_set_kernel_args(devid, gd->kernel_green_eq_favg_reduce_second, 0, CLARG(dev_m), CLARG(dev_r),
      CLARG(bufsize), CLLOCAL(sizeof(float) * 2 * slocopt.sizex));
    err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_green_eq_favg_reduce_second, ssizes,
                                                 slocal);
    if(err != CL_SUCCESS) goto error;

    sumsum = dt_alloc_align_float((size_t)2 * reducesize);
    if(sumsum == NULL) goto error;
    err = dt_opencl_read_buffer_from_device(devid, (void *)sumsum, dev_r, 0,
                                            sizeof(float) * 2 * reducesize, CL_TRUE);
    if(err != CL_SUCCESS) goto error;

    float sum1 = 0.0f, sum2 = 0.0f;
    for(int k = 0; k < reducesize; k++)
    {
      sum1 += sumsum[2 * k];
      sum2 += sumsum[2 * k + 1];
    }

    const float gr_ratio = (sum1 > 0.0f && sum2 > 0.0f) ? sum2 / sum1 : 1.0f;

    err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_green_eq_favg_apply, width, height,
      CLARG(dev_in1), CLARG(dev_out1), CLARG(width), CLARG(height), CLARG(piece->pipe->dsc.filters),
      CLARG(roi_in->x), CLARG(roi_in->y), CLARG(gr_ratio));
    if(err != CL_SUCCESS) goto error;
  }

  if(data->green_eq == DT_IOP_GREEN_EQ_LOCAL || data->green_eq == DT_IOP_GREEN_EQ_BOTH)
  {
    const dt_image_t *img = &self->dev->image_storage;
    const float threshold = 0.0001f * img->exif_iso;

    dt_opencl_local_buffer_t locopt
      = (dt_opencl_local_buffer_t){ .xoffset = 2*2, .xfactor = 1, .yoffset = 2*2, .yfactor = 1,
                                    .cellsize = 1 * sizeof(float), .overhead = 0,
                                    .sizex = 1 << 8, .sizey = 1 << 8 };

    if(!dt_opencl_local_buffer_opt(devid, gd->kernel_green_eq_lavg, &locopt))
      goto error;

    size_t sizes[3] = { ROUNDUP(width, locopt.sizex), ROUNDUP(height, locopt.sizey), 1 };
    size_t local[3] = { locopt.sizex, locopt.sizey, 1 };
    dt_opencl_set_kernel_args(devid, gd->kernel_green_eq_lavg, 0, CLARG(dev_in2), CLARG(dev_out2),
      CLARG(width), CLARG(height), CLARG(piece->pipe->dsc.filters), CLARG(roi_in->x), CLARG(roi_in->y),
      CLARG(threshold), CLLOCAL(sizeof(float) * (locopt.sizex + 4) * (locopt.sizey + 4)));
    err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_green_eq_lavg, sizes, local);
    if(err != CL_SUCCESS) goto error;
  }

  dt_opencl_release_mem_object(dev_tmp);
  dt_opencl_release_mem_object(dev_m);
  dt_opencl_release_mem_object(dev_r);
  dt_free_align(sumsum);
  return TRUE;

error:
  dt_opencl_release_mem_object(dev_tmp);
  dt_opencl_release_mem_object(dev_m);
  dt_opencl_release_mem_object(dev_r);
  dt_free_align(sumsum);
  dt_print(DT_DEBUG_OPENCL, "[opencl_demosaic_green_equilibration] couldn't enqueue kernel! %s\n", cl_errstr(err));
  return FALSE;
}

static int process_rcd_cl(
        struct dt_iop_module_t *self,
        dt_dev_pixelpipe_iop_t *piece,
        cl_mem dev_in,
        cl_mem dev_out,
        const dt_iop_roi_t *const roi_in,
        const dt_iop_roi_t *const roi_out,
        const gboolean smooth)
{
  dt_iop_demosaic_data_t *data = (dt_iop_demosaic_data_t *)piece->data;
  dt_iop_demosaic_global_data_t *gd = (dt_iop_demosaic_global_data_t *)self->global_data;
  const dt_image_t *img = &self->dev->image_storage;

  const int devid = piece->pipe->devid;
  const int qual_flags = demosaic_qual_flags(piece, img, roi_out);

  cl_mem dev_aux = NULL;
  cl_mem dev_tmp = NULL;
  cl_mem dev_green_eq = NULL;
  cl_mem cfa = NULL;
  cl_mem rgb0 = NULL;
  cl_mem rgb1 = NULL;
  cl_mem rgb2 = NULL;
  cl_mem VH_dir = NULL;
  cl_mem PQ_dir = NULL;
  cl_mem VP_diff = NULL;
  cl_mem HQ_diff = NULL;

  cl_int err = DT_OPENCL_DEFAULT_ERROR;

  if(qual_flags & DT_DEMOSAIC_FULL_SCALE)
  {
     // Full demosaic and then scaling if needed
    const int scaled = (roi_out->width != roi_in->width || roi_out->height != roi_in->height);

    int width = roi_out->width;
    int height = roi_out->height;

    // green equilibration
    if(data->green_eq != DT_IOP_GREEN_EQ_NO)
    {
      dev_green_eq = dt_opencl_alloc_device(devid, roi_in->width, roi_in->height, sizeof(float));
      if(dev_green_eq == NULL) goto error;
      if(!green_equilibration_cl(self, piece, dev_in, dev_green_eq, roi_in)) goto error;
      dev_in = dev_green_eq;
    }

    // need to reserve scaled auxiliary buffer or use dev_out
    if(scaled)
    {
      dev_aux = dt_opencl_alloc_device(devid, roi_in->width, roi_in->height, sizeof(float) * 4);
      if(dev_aux == NULL) goto error;
      width = roi_in->width;
      height = roi_in->height;
    }
    else
      dev_aux = dev_out;

    dev_tmp = dt_opencl_alloc_device(devid, roi_in->width, roi_in->height, sizeof(float) * 4);
    if(dev_tmp == NULL) goto error;

    {
      const int myborder = 3;
      err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_border_interpolate, width, height,
        CLARG(dev_in), CLARG(dev_tmp), CLARG(width), CLARG(height), CLARG(piece->pipe->dsc.filters), CLARG(myborder));
      if(err != CL_SUCCESS) goto error;
    }

    {
      dt_opencl_local_buffer_t locopt
        = (dt_opencl_local_buffer_t){ .xoffset = 2*3, .xfactor = 1, .yoffset = 2*3, .yfactor = 1,
                                      .cellsize = sizeof(float) * 1, .overhead = 0,
                                      .sizex = 64, .sizey = 64 };

      if(!dt_opencl_local_buffer_opt(devid, gd->kernel_rcd_border_green, &locopt)) goto error;
      const int myborder = 32;
      size_t sizes[3] = { ROUNDUP(width, locopt.sizex), ROUNDUP(height, locopt.sizey), 1 };
      size_t local[3] = { locopt.sizex, locopt.sizey, 1 };
      dt_opencl_set_kernel_args(devid, gd->kernel_rcd_border_green, 0, CLARG(dev_in), CLARG(dev_tmp),
        CLARG(width), CLARG(height), CLARG(piece->pipe->dsc.filters), CLLOCAL(sizeof(float) * (locopt.sizex + 2*3) * (locopt.sizey + 2*3)),
        CLARG(myborder));
      err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_rcd_border_green, sizes, local);
      if(err != CL_SUCCESS) goto error;
    }

    {
      dt_opencl_local_buffer_t locopt
        = (dt_opencl_local_buffer_t){ .xoffset = 2*1, .xfactor = 1, .yoffset = 2*1, .yfactor = 1,
                                      .cellsize = 4 * sizeof(float), .overhead = 0,
                                      .sizex = 64, .sizey = 64 };

      if(!dt_opencl_local_buffer_opt(devid, gd->kernel_rcd_border_redblue, &locopt)) goto error;
      const int myborder = 16;
      size_t sizes[3] = { ROUNDUP(width, locopt.sizex), ROUNDUP(height, locopt.sizey), 1 };
      size_t local[3] = { locopt.sizex, locopt.sizey, 1 };
      dt_opencl_set_kernel_args(devid, gd->kernel_rcd_border_redblue, 0, CLARG(dev_tmp), CLARG(dev_aux),
        CLARG(width), CLARG(height), CLARG(piece->pipe->dsc.filters), CLLOCAL(sizeof(float) * 4 * (locopt.sizex + 2) * (locopt.sizey + 2)),
        CLARG(myborder));
      err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_rcd_border_redblue, sizes, local);
      if(err != CL_SUCCESS) goto error;
    }
    dt_opencl_release_mem_object(dev_tmp);
    dev_tmp = NULL;

    cfa = dt_opencl_alloc_device_buffer(devid, sizeof(float) * roi_in->width * roi_in->height);
    if(cfa == NULL) goto error;
    VH_dir = dt_opencl_alloc_device_buffer(devid, sizeof(float) * roi_in->width * roi_in->height);
    if(VH_dir == NULL) goto error;
    PQ_dir = dt_opencl_alloc_device_buffer(devid, sizeof(float) * roi_in->width * roi_in->height);
    if(PQ_dir == NULL) goto error;
    VP_diff = dt_opencl_alloc_device_buffer(devid, sizeof(float) * roi_in->width * roi_in->height);
    if(VP_diff == NULL) goto error;
    HQ_diff = dt_opencl_alloc_device_buffer(devid, sizeof(float) * roi_in->width * roi_in->height);
    if(HQ_diff == NULL) goto error;
    rgb0 = dt_opencl_alloc_device_buffer(devid, sizeof(float) * roi_in->width * roi_in->height);
    if(rgb0 == NULL) goto error;
    rgb1 = dt_opencl_alloc_device_buffer(devid, sizeof(float) * roi_in->width * roi_in->height);
    if(rgb1 == NULL) goto error;
    rgb2 = dt_opencl_alloc_device_buffer(devid, sizeof(float) * roi_in->width * roi_in->height);
    if(rgb2 == NULL) goto error;

    {
      // populate data
      const float scaler = 1.0f / fmaxf(piece->pipe->dsc.processed_maximum[0], fmaxf(piece->pipe->dsc.processed_maximum[1], piece->pipe->dsc.processed_maximum[2]));
      err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_rcd_populate, width, height,
        CLARG(dev_in), CLARG(cfa), CLARG(rgb0), CLARG(rgb1), CLARG(rgb2), CLARG(width), CLARG(height),
        CLARG(piece->pipe->dsc.filters), CLARG(scaler));
      if(err != CL_SUCCESS) goto error;
    }

    {
      // Step 1.1: Calculate a squared vertical and horizontal high pass filter on color differences
      err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_rcd_step_1_1, width, height,
        CLARG(cfa), CLARG(VP_diff), CLARG(HQ_diff), CLARG(width), CLARG(height));
      if(err != CL_SUCCESS) goto error;
    }

    {
      // Step 1.2: Calculate vertical and horizontal local discrimination
      err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_rcd_step_1_2, width, height,
        CLARG(VH_dir), CLARG(VP_diff), CLARG(HQ_diff), CLARG(width), CLARG(height));
      if(err != CL_SUCCESS) goto error;
    }

    {
      // Step 2.1: Low pass filter incorporating green, red and blue local samples from the raw data
      err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_rcd_step_2_1, width / 2, height,
        CLARG(PQ_dir), CLARG(cfa), CLARG(width), CLARG(height), CLARG(piece->pipe->dsc.filters));
      if(err != CL_SUCCESS) goto error;
    }

    {
      // Step 3.1: Populate the green channel at blue and red CFA positions
      err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_rcd_step_3_1, width / 2, height,
        CLARG(PQ_dir), CLARG(cfa), CLARG(rgb1), CLARG(VH_dir), CLARG(width), CLARG(height), CLARG(piece->pipe->dsc.filters));
      if(err != CL_SUCCESS) goto error;
    }

    {
      // Step 4.1: Calculate a squared P/Q diagonals high pass filter on color differences
      err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_rcd_step_4_1, width / 2, height,
        CLARG(cfa), CLARG(VP_diff), CLARG(HQ_diff), CLARG(width), CLARG(height), CLARG(piece->pipe->dsc.filters));
      if(err != CL_SUCCESS) goto error;
    }

    {
      // Step 4.2: Calculate P/Q diagonal local discrimination
      err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_rcd_step_4_2, width / 2, height,
        CLARG(PQ_dir), CLARG(VP_diff), CLARG(HQ_diff), CLARG(width), CLARG(height), CLARG(piece->pipe->dsc.filters));
      if(err != CL_SUCCESS) goto error;
    }

    {
      // Step 4.3: Populate the red and blue channels at blue and red CFA positions
      err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_rcd_step_5_1, width / 2, height,
        CLARG(PQ_dir), CLARG(rgb0), CLARG(rgb1), CLARG(rgb2), CLARG(width), CLARG(height), CLARG(piece->pipe->dsc.filters));
      if(err != CL_SUCCESS) goto error;
    }

    {
      // Step 5.2: Populate the red and blue channels at green CFA positions
      err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_rcd_step_5_2, width / 2, height,
        CLARG(VH_dir), CLARG(rgb0), CLARG(rgb1), CLARG(rgb2), CLARG(width), CLARG(height), CLARG(piece->pipe->dsc.filters));
      if(err != CL_SUCCESS) goto error;
    }
    const float scaler = fmaxf(piece->pipe->dsc.processed_maximum[0], fmaxf(piece->pipe->dsc.processed_maximum[1], piece->pipe->dsc.processed_maximum[2]));

    {
      // write output
      const int myborder = 6;
      err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_rcd_write_output, width, height,
        CLARG(dev_aux), CLARG(rgb0), CLARG(rgb1), CLARG(rgb2), CLARG(width), CLARG(height), CLARG(scaler),
        CLARG(myborder));
      if(err != CL_SUCCESS) goto error;
    }

    dt_opencl_release_mem_object(cfa);
    dt_opencl_release_mem_object(rgb0);
    dt_opencl_release_mem_object(rgb1);
    dt_opencl_release_mem_object(rgb2);
    dt_opencl_release_mem_object(VH_dir);
    dt_opencl_release_mem_object(PQ_dir);
    dt_opencl_release_mem_object(VP_diff);
    dt_opencl_release_mem_object(HQ_diff);
    dt_opencl_release_mem_object(dev_green_eq);
    dev_green_eq = cfa = rgb0 = rgb1 = rgb2 = VH_dir = PQ_dir = VP_diff = HQ_diff = NULL;

    dt_dev_write_rawdetail_mask_cl(piece, dev_aux, roi_in, DT_DEV_DETAIL_MASK_DEMOSAIC);

    if(scaled)
    {
      dt_print_pipe(DT_DEBUG_PIPE, "clip_and_zoom_roi_cl", piece->pipe, self->so->op, roi_in, roi_out, "\n");
      // scale aux buffer to output buffer
      err = dt_iop_clip_and_zoom_roi_cl(devid, dev_out, dev_aux, roi_out, roi_in);
      if(err != CL_SUCCESS) goto error;
    }
  }
  else
  {
    // sample half-size image:
    const int zero = 0;
    cl_mem dev_pix = dev_in;
    const int width = roi_out->width;
    const int height = roi_out->height;

    err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_zoom_half_size, width, height,
      CLARG(dev_pix), CLARG(dev_out), CLARG(width), CLARG(height), CLARG(zero), CLARG(zero), CLARG(roi_in->width),
      CLARG(roi_in->height), CLARG(roi_out->scale), CLARG(piece->pipe->dsc.filters));
    if(err != CL_SUCCESS) goto error;
  }

  if(dev_aux != dev_out) dt_opencl_release_mem_object(dev_aux);
  dev_aux = NULL;

  // color smoothing
  if((data->color_smoothing) && smooth)
  {
    if(!color_smoothing_cl(self, piece, dev_out, dev_out, roi_out, data->color_smoothing))
      goto error;
  }

  return TRUE;

error:
  if(dev_aux != dev_out) dt_opencl_release_mem_object(dev_aux);
  dt_opencl_release_mem_object(dev_green_eq);
  dt_opencl_release_mem_object(dev_tmp);
  dt_opencl_release_mem_object(cfa);
  dt_opencl_release_mem_object(rgb0);
  dt_opencl_release_mem_object(rgb1);
  dt_opencl_release_mem_object(rgb2);
  dt_opencl_release_mem_object(VH_dir);
  dt_opencl_release_mem_object(PQ_dir);
  dt_opencl_release_mem_object(VP_diff);
  dt_opencl_release_mem_object(HQ_diff);
  dev_aux = dev_green_eq = dev_tmp = cfa = rgb0 = rgb1 = rgb2 = VH_dir = PQ_dir = VP_diff = HQ_diff = NULL;
  dt_print(DT_DEBUG_OPENCL, "[opencl_demosaic] rcd couldn't enqueue kernel! %s\n", cl_errstr(err));
  return FALSE;
}

static int process_default_cl(
        struct dt_iop_module_t *self,
        dt_dev_pixelpipe_iop_t *piece,
        cl_mem dev_in,
        cl_mem dev_out,
        const dt_iop_roi_t *const roi_in,
        const dt_iop_roi_t *const roi_out,
        const int demosaicing_method)
{
  dt_iop_demosaic_data_t *data = (dt_iop_demosaic_data_t *)piece->data;
  dt_iop_demosaic_global_data_t *gd = (dt_iop_demosaic_global_data_t *)self->global_data;
  const dt_image_t *img = &self->dev->image_storage;

  const int devid = piece->pipe->devid;
  const int qual_flags = demosaic_qual_flags(piece, img, roi_out);

  cl_mem dev_aux = NULL;
  cl_mem dev_tmp = NULL;
  cl_mem dev_med = NULL;
  cl_mem dev_green_eq = NULL;
  cl_int err = DT_OPENCL_DEFAULT_ERROR;

  if(qual_flags & DT_DEMOSAIC_FULL_SCALE)
  {
    // Full demosaic and then scaling if needed
    const int scaled = (roi_out->width != roi_in->width || roi_out->height != roi_in->height);

    int width = roi_out->width;
    int height = roi_out->height;

    // green equilibration
    if(data->green_eq != DT_IOP_GREEN_EQ_NO)
    {
      dev_green_eq = dt_opencl_alloc_device(devid, roi_in->width, roi_in->height, sizeof(float));
      if(dev_green_eq == NULL) goto error;

      if(!green_equilibration_cl(self, piece, dev_in, dev_green_eq, roi_in))
        goto error;

      dev_in = dev_green_eq;
    }

    // need to reserve scaled auxiliary buffer or use dev_out
    if(scaled)
    {
      dev_aux = dt_opencl_alloc_device(devid, roi_in->width, roi_in->height, sizeof(float) * 4);
      if(dev_aux == NULL) goto error;
      width = roi_in->width;
      height = roi_in->height;
    }
    else
      dev_aux = dev_out;

    if(demosaicing_method == DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME)
    {
      err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_passthrough_monochrome, width, height,
        CLARG(dev_in), CLARG(dev_aux), CLARG(width), CLARG(height));
      if(err != CL_SUCCESS) goto error;
    }
    else if(demosaicing_method == DT_IOP_DEMOSAIC_PASSTHROUGH_COLOR)
    {
      cl_mem dev_xtrans = dt_opencl_copy_host_to_device_constant(devid, sizeof(piece->pipe->dsc.xtrans), piece->pipe->dsc.xtrans);
      if(dev_xtrans == NULL) goto error;

      err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_passthrough_color, width, height,
        CLARG(dev_in), CLARG(dev_aux), CLARG(width), CLARG(height), CLARG(roi_in->x), CLARG(roi_in->y),
        CLARG(piece->pipe->dsc.filters), CLARG(dev_xtrans));
      dt_opencl_release_mem_object(dev_xtrans);
      if(err != CL_SUCCESS) goto error;
    }
    else if(demosaicing_method == DT_IOP_DEMOSAIC_PPG)
    {
      dev_tmp = dt_opencl_alloc_device(devid, roi_in->width, roi_in->height, sizeof(float) * 4);
      if(dev_tmp == NULL) goto error;

      {
        const int myborder = 3;
        // manage borders
        err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_border_interpolate, width, height,
          CLARG(dev_in), CLARG(dev_tmp), CLARG(width), CLARG(height), CLARG(piece->pipe->dsc.filters), CLARG(myborder));
        if(err != CL_SUCCESS) goto error;
      }

      if(data->median_thrs > 0.0f)
      {
        dev_med = dt_opencl_alloc_device(devid, roi_in->width, roi_in->height, sizeof(float) * 4);
        if(dev_med == NULL) goto error;

        dt_opencl_local_buffer_t locopt
          = (dt_opencl_local_buffer_t){ .xoffset = 2*2, .xfactor = 1, .yoffset = 2*2, .yfactor = 1,
                                        .cellsize = 1 * sizeof(float), .overhead = 0,
                                        .sizex = 1 << 8, .sizey = 1 << 8 };

        if(!dt_opencl_local_buffer_opt(devid, gd->kernel_pre_median, &locopt))
        goto error;

        size_t sizes[3] = { ROUNDUP(width, locopt.sizex), ROUNDUP(height, locopt.sizey), 1 };
        size_t local[3] = { locopt.sizex, locopt.sizey, 1 };
        dt_opencl_set_kernel_args(devid, gd->kernel_pre_median, 0, CLARG(dev_in), CLARG(dev_med), CLARG(width),
          CLARG(height), CLARG(piece->pipe->dsc.filters), CLARG(data->median_thrs), CLLOCAL(sizeof(float) * (locopt.sizex + 4) * (locopt.sizey + 4)));
        err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_pre_median, sizes, local);
        if(err != CL_SUCCESS) goto error;
        dev_in = dev_aux;
      }
      else dev_med = dev_in;

      {
        dt_opencl_local_buffer_t locopt
          = (dt_opencl_local_buffer_t){ .xoffset = 2*3, .xfactor = 1, .yoffset = 2*3, .yfactor = 1,
                                        .cellsize = sizeof(float) * 1, .overhead = 0,
                                        .sizex = 1 << 8, .sizey = 1 << 8 };

        if(!dt_opencl_local_buffer_opt(devid, gd->kernel_ppg_green, &locopt))
        goto error;

        size_t sizes[3] = { ROUNDUP(width, locopt.sizex), ROUNDUP(height, locopt.sizey), 1 };
        size_t local[3] = { locopt.sizex, locopt.sizey, 1 };
        dt_opencl_set_kernel_args(devid, gd->kernel_ppg_green, 0, CLARG(dev_med), CLARG(dev_tmp), CLARG(width),
          CLARG(height), CLARG(piece->pipe->dsc.filters), CLLOCAL(sizeof(float) * (locopt.sizex + 2*3) * (locopt.sizey + 2*3)));

        err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_ppg_green, sizes, local);
        if(err != CL_SUCCESS) goto error;
      }

      {
        dt_opencl_local_buffer_t locopt
          = (dt_opencl_local_buffer_t){ .xoffset = 2*1, .xfactor = 1, .yoffset = 2*1, .yfactor = 1,
                                        .cellsize = 4 * sizeof(float), .overhead = 0,
                                        .sizex = 1 << 8, .sizey = 1 << 8 };

        if(!dt_opencl_local_buffer_opt(devid, gd->kernel_ppg_redblue, &locopt))
        goto error;

        size_t sizes[3] = { ROUNDUP(width, locopt.sizex), ROUNDUP(height, locopt.sizey), 1 };
        size_t local[3] = { locopt.sizex, locopt.sizey, 1 };
        dt_opencl_set_kernel_args(devid, gd->kernel_ppg_redblue, 0, CLARG(dev_tmp), CLARG(dev_aux), CLARG(width),
          CLARG(height), CLARG(piece->pipe->dsc.filters), CLLOCAL(sizeof(float) * 4 * (locopt.sizex + 2) * (locopt.sizey + 2)));

        err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_ppg_redblue, sizes, local);
        if(err != CL_SUCCESS) goto error;
      }
    }

    dt_dev_write_rawdetail_mask_cl(piece, dev_aux, roi_in, DT_DEV_DETAIL_MASK_DEMOSAIC);

    if(scaled)
    {
      dt_print_pipe(DT_DEBUG_PIPE, "clip_and_zoom_roi_cl", piece->pipe, self->so->op, roi_in, roi_out, "\n");
      // scale aux buffer to output buffer
      err = dt_iop_clip_and_zoom_roi_cl(devid, dev_out, dev_aux, roi_out, roi_in);
      if(err != CL_SUCCESS) goto error;
    }
  }
  else
  {
    if(demosaicing_method == DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME)
    {
      // sample image:
      const int zero = 0;
      cl_mem dev_pix = dev_in;
      const int width = roi_out->width;
      const int height = roi_out->height;

      err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_zoom_passthrough_monochrome, width, height,
        CLARG(dev_pix), CLARG(dev_out), CLARG(width), CLARG(height), CLARG(zero), CLARG(zero), CLARG(roi_in->width),
        CLARG(roi_in->height), CLARG(roi_out->scale), CLARG(piece->pipe->dsc.filters));
      if(err != CL_SUCCESS) goto error;
    }
    else
    {
      // sample half-size image:
      const int zero = 0;
      cl_mem dev_pix = dev_in;
      const int width = roi_out->width;
      const int height = roi_out->height;

      err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_zoom_half_size, width, height,
        CLARG(dev_pix), CLARG(dev_out), CLARG(width), CLARG(height), CLARG(zero), CLARG(zero), CLARG(roi_in->width),
        CLARG(roi_in->height), CLARG(roi_out->scale), CLARG(piece->pipe->dsc.filters));
      if(err != CL_SUCCESS) goto error;
    }
  }

  if(dev_aux != dev_out) dt_opencl_release_mem_object(dev_aux);
  if(dev_med != dev_in) dt_opencl_release_mem_object(dev_med);
  dt_opencl_release_mem_object(dev_green_eq);
  dt_opencl_release_mem_object(dev_tmp);
  dev_aux = dev_green_eq = dev_tmp = dev_med = NULL;

  // color smoothing
  if(data->color_smoothing)
  {
    if(!color_smoothing_cl(self, piece, dev_out, dev_out, roi_out, data->color_smoothing))
      goto error;
  }

  return TRUE;

error:
  if(dev_aux != dev_out) dt_opencl_release_mem_object(dev_aux);
  if(dev_med != dev_in) dt_opencl_release_mem_object(dev_med);
  dt_opencl_release_mem_object(dev_green_eq);
  dt_opencl_release_mem_object(dev_tmp);
  dt_print(DT_DEBUG_OPENCL, "[opencl_demosaic] couldn't enqueue kernel! %s\n", cl_errstr(err));
  return FALSE;
}

static int process_vng_cl(
        struct dt_iop_module_t *self,
        dt_dev_pixelpipe_iop_t *piece,
        cl_mem dev_in,
        cl_mem dev_out,
        const dt_iop_roi_t *const roi_in,
        const dt_iop_roi_t *const roi_out,
        const gboolean smooth,
        const gboolean only_vng_linear)
{
  dt_iop_demosaic_data_t *data = (dt_iop_demosaic_data_t *)piece->data;
  dt_iop_demosaic_global_data_t *gd = (dt_iop_demosaic_global_data_t *)self->global_data;
  const dt_image_t *img = &self->dev->image_storage;

  const uint8_t(*const xtrans)[6] = (const uint8_t(*const)[6])piece->pipe->dsc.xtrans;

  // separate out G1 and G2 in Bayer patterns
  uint32_t filters4;
  if(piece->pipe->dsc.filters == 9u)
    filters4 = piece->pipe->dsc.filters;
  else if((piece->pipe->dsc.filters & 3) == 1)
    filters4 = piece->pipe->dsc.filters | 0x03030303u;
  else
    filters4 = piece->pipe->dsc.filters | 0x0c0c0c0cu;

  const int size = (filters4 == 9u) ? 6 : 16;
  const int colors = (filters4 == 9u) ? 3 : 4;
  const int prow = (filters4 == 9u) ? 6 : 8;
  const int pcol = (filters4 == 9u) ? 6 : 2;
  const int devid = piece->pipe->devid;

  const float processed_maximum[4]
      = { piece->pipe->dsc.processed_maximum[0], piece->pipe->dsc.processed_maximum[1],
          piece->pipe->dsc.processed_maximum[2], 1.0f };

  const int qual_flags = demosaic_qual_flags(piece, img, roi_out);

  int *ips = NULL;

  cl_mem dev_tmp = NULL;
  cl_mem dev_aux = NULL;
  cl_mem dev_xtrans = NULL;
  cl_mem dev_lookup = NULL;
  cl_mem dev_code = NULL;
  cl_mem dev_ips = NULL;
  cl_mem dev_green_eq = NULL;
  cl_int err = DT_OPENCL_DEFAULT_ERROR;

  int32_t(*lookup)[16][32] = NULL;

  if(piece->pipe->dsc.filters == 9u)
  {
    dev_xtrans
        = dt_opencl_copy_host_to_device_constant(devid, sizeof(piece->pipe->dsc.xtrans), piece->pipe->dsc.xtrans);
    if(dev_xtrans == NULL) goto error;
  }

  if(qual_flags & DT_DEMOSAIC_FULL_SCALE)
  {
    // Full demosaic and then scaling if needed
    const int scaled = (roi_out->width != roi_in->width || roi_out->height != roi_in->height);

    // build interpolation lookup table for linear interpolation which for a given offset in the sensor
    // lists neighboring pixels from which to interpolate:
    // NUM_PIXELS                 # of neighboring pixels to read
    // for(1..NUM_PIXELS):
    //   OFFSET                   # in bytes from current pixel
    //   WEIGHT                   # how much weight to give this neighbor
    //   COLOR                    # sensor color
    // # weights of adjoining pixels not of this pixel's color
    // COLORA TOT_WEIGHT
    // COLORB TOT_WEIGHT
    // COLORPIX                   # color of center pixel
    const size_t lookup_size = (size_t)16 * 16 * 32 * sizeof(int32_t);
    lookup = malloc(lookup_size);

    for(int row = 0; row < size; row++)
      for(int col = 0; col < size; col++)
      {
        int32_t *ip = &(lookup[row][col][1]);
        int sum[4] = { 0 };
        const int f = fcol(row + roi_in->y, col + roi_in->x, filters4, xtrans);
        // make list of adjoining pixel offsets by weight & color
        for(int y = -1; y <= 1; y++)
          for(int x = -1; x <= 1; x++)
          {
            const int weight = 1 << ((y == 0) + (x == 0));
            const int color = fcol(row + y + roi_in->y, col + x + roi_in->x, filters4, xtrans);
            if(color == f) continue;
            *ip++ = (y << 16) | (x & 0xffffu);
            *ip++ = weight;
            *ip++ = color;
            sum[color] += weight;
          }
        lookup[row][col][0] = (ip - &(lookup[row][col][0])) / 3; /* # of neighboring pixels found */
        for(int c = 0; c < colors; c++)
          if(c != f)
          {
            *ip++ = c;
            *ip++ = sum[c];
          }
        *ip = f;
      }

    // Precalculate for VNG
    static const signed char terms[]
      = { -2, -2, +0, -1, 1, 0x01, -2, -2, +0, +0, 2, 0x01, -2, -1, -1, +0, 1, 0x01, -2, -1, +0, -1, 1, 0x02,
          -2, -1, +0, +0, 1, 0x03, -2, -1, +0, +1, 2, 0x01, -2, +0, +0, -1, 1, 0x06, -2, +0, +0, +0, 2, 0x02,
          -2, +0, +0, +1, 1, 0x03, -2, +1, -1, +0, 1, 0x04, -2, +1, +0, -1, 2, 0x04, -2, +1, +0, +0, 1, 0x06,
          -2, +1, +0, +1, 1, 0x02, -2, +2, +0, +0, 2, 0x04, -2, +2, +0, +1, 1, 0x04, -1, -2, -1, +0, 1, 0x80,
          -1, -2, +0, -1, 1, 0x01, -1, -2, +1, -1, 1, 0x01, -1, -2, +1, +0, 2, 0x01, -1, -1, -1, +1, 1, 0x88,
          -1, -1, +1, -2, 1, 0x40, -1, -1, +1, -1, 1, 0x22, -1, -1, +1, +0, 1, 0x33, -1, -1, +1, +1, 2, 0x11,
          -1, +0, -1, +2, 1, 0x08, -1, +0, +0, -1, 1, 0x44, -1, +0, +0, +1, 1, 0x11, -1, +0, +1, -2, 2, 0x40,
          -1, +0, +1, -1, 1, 0x66, -1, +0, +1, +0, 2, 0x22, -1, +0, +1, +1, 1, 0x33, -1, +0, +1, +2, 2, 0x10,
          -1, +1, +1, -1, 2, 0x44, -1, +1, +1, +0, 1, 0x66, -1, +1, +1, +1, 1, 0x22, -1, +1, +1, +2, 1, 0x10,
          -1, +2, +0, +1, 1, 0x04, -1, +2, +1, +0, 2, 0x04, -1, +2, +1, +1, 1, 0x04, +0, -2, +0, +0, 2, 0x80,
          +0, -1, +0, +1, 2, 0x88, +0, -1, +1, -2, 1, 0x40, +0, -1, +1, +0, 1, 0x11, +0, -1, +2, -2, 1, 0x40,
          +0, -1, +2, -1, 1, 0x20, +0, -1, +2, +0, 1, 0x30, +0, -1, +2, +1, 2, 0x10, +0, +0, +0, +2, 2, 0x08,
          +0, +0, +2, -2, 2, 0x40, +0, +0, +2, -1, 1, 0x60, +0, +0, +2, +0, 2, 0x20, +0, +0, +2, +1, 1, 0x30,
          +0, +0, +2, +2, 2, 0x10, +0, +1, +1, +0, 1, 0x44, +0, +1, +1, +2, 1, 0x10, +0, +1, +2, -1, 2, 0x40,
          +0, +1, +2, +0, 1, 0x60, +0, +1, +2, +1, 1, 0x20, +0, +1, +2, +2, 1, 0x10, +1, -2, +1, +0, 1, 0x80,
          +1, -1, +1, +1, 1, 0x88, +1, +0, +1, +2, 1, 0x08, +1, +0, +2, -1, 1, 0x40, +1, +0, +2, +1, 1, 0x10 };
    static const signed char chood[]
      = { -1, -1, -1, 0, -1, +1, 0, +1, +1, +1, +1, 0, +1, -1, 0, -1 };

    const size_t ips_size = (size_t)prow * pcol * 352 * sizeof(int);
    ips = malloc(ips_size);

    int *ip = ips;
    int code[16][16];

    for(int row = 0; row < prow; row++)
      for(int col = 0; col < pcol; col++)
      {
        code[row][col] = ip - ips;
        const signed char *cp = terms;
        for(int t = 0; t < 64; t++)
        {
          const int y1 = *cp++, x1 = *cp++;
          const int y2 = *cp++, x2 = *cp++;
          const int weight = *cp++;
          const int grads = *cp++;
          const int color = fcol(row + y1, col + x1, filters4, xtrans);
          if(fcol(row + y2, col + x2, filters4, xtrans) != color) continue;
          const int diag
              = (fcol(row, col + 1, filters4, xtrans) == color && fcol(row + 1, col, filters4, xtrans) == color)
                    ? 2
                    : 1;
          if(abs(y1 - y2) == diag && abs(x1 - x2) == diag) continue;
          *ip++ = (y1 << 16) | (x1 & 0xffffu);
          *ip++ = (y2 << 16) | (x2 & 0xffffu);
          *ip++ = (color << 16) | (weight & 0xffffu);
          for(int g = 0; g < 8; g++)
            if(grads & 1 << g) *ip++ = g;
          *ip++ = -1;
        }
        *ip++ = INT_MAX;
        cp = chood;
        for(int g = 0; g < 8; g++)
        {
          const int y = *cp++, x = *cp++;
          *ip++ = (y << 16) | (x & 0xffffu);
          const int color = fcol(row, col, filters4, xtrans);
          if(fcol(row + y, col + x, filters4, xtrans) != color
             && fcol(row + y * 2, col + x * 2, filters4, xtrans) == color)
          {
            *ip++ = (2*y << 16) | (2*x & 0xffffu);
            *ip++ = color;
          }
          else
          {
            *ip++ = 0;
            *ip++ = 0;
          }
        }
      }


    dev_lookup = dt_opencl_copy_host_to_device_constant(devid, lookup_size, lookup);
    if(dev_lookup == NULL) goto error;

    dev_code = dt_opencl_copy_host_to_device_constant(devid, sizeof(code), code);
    if(dev_code == NULL) goto error;

    dev_ips = dt_opencl_copy_host_to_device_constant(devid, ips_size, ips);
    if(dev_ips == NULL) goto error;

    // green equilibration for Bayer sensors
    if(piece->pipe->dsc.filters != 9u && data->green_eq != DT_IOP_GREEN_EQ_NO)
    {
      dev_green_eq = dt_opencl_alloc_device(devid, roi_in->width, roi_in->height, sizeof(float));
      if(dev_green_eq == NULL) goto error;

      if(!green_equilibration_cl(self, piece, dev_in, dev_green_eq, roi_in))
        goto error;

      dev_in = dev_green_eq;
    }

    int width = roi_out->width;
    int height = roi_out->height;

    // need to reserve scaled auxiliary buffer or use dev_out
    if(scaled)
    {
      dev_aux = dt_opencl_alloc_device(devid, roi_in->width, roi_in->height, sizeof(float) * 4);
      if(dev_aux == NULL) goto error;
      width = roi_in->width;
      height = roi_in->height;
    }
    else
      dev_aux = dev_out;

    dev_tmp = dt_opencl_alloc_device(devid, roi_in->width, roi_in->height, sizeof(float) * 4);
    if(dev_tmp == NULL) goto error;

    {
      // manage borders for linear interpolation part
      const int border = 1;

      err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_vng_border_interpolate, width, height,
        CLARG(dev_in), CLARG(dev_tmp), CLARG(width), CLARG(height), CLARG(border), CLARG(roi_in->x), CLARG(roi_in->y),
        CLARG(filters4), CLARG(dev_xtrans));
      if(err != CL_SUCCESS) goto error;
    }

    {
      // do linear interpolation
      dt_opencl_local_buffer_t locopt
        = (dt_opencl_local_buffer_t){ .xoffset = 2*1, .xfactor = 1, .yoffset = 2*1, .yfactor = 1,
                                      .cellsize = 1 * sizeof(float), .overhead = 0,
                                      .sizex = 1 << 8, .sizey = 1 << 8 };

      if(!dt_opencl_local_buffer_opt(devid, gd->kernel_vng_lin_interpolate, &locopt))
        goto error;

      size_t sizes[3] = { ROUNDUP(width, locopt.sizex), ROUNDUP(height, locopt.sizey), 1 };
      size_t local[3] = { locopt.sizex, locopt.sizey, 1 };
      dt_opencl_set_kernel_args(devid, gd->kernel_vng_lin_interpolate, 0, CLARG(dev_in), CLARG(dev_tmp),
        CLARG(width), CLARG(height), CLARG(filters4), CLARG(dev_lookup), CLLOCAL(sizeof(float) * (locopt.sizex + 2) * (locopt.sizey + 2)));
      err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_vng_lin_interpolate, sizes, local);
      if(err != CL_SUCCESS) goto error;
    }


    if(qual_flags & DT_DEMOSAIC_ONLY_VNG_LINEAR)
    {
      // leave it at linear interpolation and skip VNG
      size_t origin[] = { 0, 0, 0 };
      size_t region[] = { width, height, 1 };
      err = dt_opencl_enqueue_copy_image(devid, dev_tmp, dev_aux, origin, origin, region);
      if(err != CL_SUCCESS) goto error;
    }
    else
    {
      // do full VNG interpolation
      dt_opencl_local_buffer_t locopt
        = (dt_opencl_local_buffer_t){ .xoffset = 2*2, .xfactor = 1, .yoffset = 2*2, .yfactor = 1,
                                      .cellsize = 4 * sizeof(float), .overhead = 0,
                                      .sizex = 1 << 8, .sizey = 1 << 8 };

      if(!dt_opencl_local_buffer_opt(devid, gd->kernel_vng_interpolate, &locopt))
        goto error;

      size_t sizes[3] = { ROUNDUP(width, locopt.sizex), ROUNDUP(height, locopt.sizey), 1 };
      size_t local[3] = { locopt.sizex, locopt.sizey, 1 };
      dt_opencl_set_kernel_args(devid, gd->kernel_vng_interpolate, 0, CLARG(dev_tmp), CLARG(dev_aux),
        CLARG(width), CLARG(height), CLARG(roi_in->x), CLARG(roi_in->y), CLARG(filters4), CLARRAY(4, processed_maximum),
        CLARG(dev_xtrans), CLARG(dev_ips), CLARG(dev_code), CLLOCAL(sizeof(float) * 4 * (locopt.sizex + 4) * (locopt.sizey + 4)));
      err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_vng_interpolate, sizes, local);
      if(err != CL_SUCCESS) goto error;
    }

    {
      // manage borders
      const int border = 2;

      err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_vng_border_interpolate, width, height,
        CLARG(dev_in), CLARG(dev_aux), CLARG(width), CLARG(height), CLARG(border), CLARG(roi_in->x), CLARG(roi_in->y),
        CLARG(filters4), CLARG(dev_xtrans));
      if(err != CL_SUCCESS) goto error;
    }

    if(filters4 != 9)
    {
      // for Bayer sensors mix the two green channels
      size_t origin[] = { 0, 0, 0 };
      size_t region[] = { width, height, 1 };
      err = dt_opencl_enqueue_copy_image(devid, dev_aux, dev_tmp, origin, origin, region);
      if(err != CL_SUCCESS) goto error;

      err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_vng_green_equilibrate, width, height,
        CLARG(dev_tmp), CLARG(dev_aux), CLARG(width), CLARG(height));
      if(err != CL_SUCCESS) goto error;
    }
    dt_dev_write_rawdetail_mask_cl(piece, dev_aux, roi_in, DT_DEV_DETAIL_MASK_DEMOSAIC);

    if(scaled)
    {
      dt_print_pipe(DT_DEBUG_PIPE, "clip_and_zoom_roi_cl", piece->pipe, self->so->op, roi_in, roi_out, "\n");
      // scale temp buffer to output buffer
      err = dt_iop_clip_and_zoom_roi_cl(devid, dev_out, dev_aux, roi_out, roi_in);
      if(err != CL_SUCCESS) goto error;
    }
  }
  else
  {
    // sample half-size or third-size image
    if(piece->pipe->dsc.filters == 9u)
    {
      const int width = roi_out->width;
      const int height = roi_out->height;

      err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_zoom_third_size, width, height,
        CLARG(dev_in), CLARG(dev_out), CLARG(width), CLARG(height), CLARG(roi_in->x), CLARG(roi_in->y),
        CLARG(roi_in->width), CLARG(roi_in->height), CLARG(roi_out->scale), CLARG(dev_xtrans));
      if(err != CL_SUCCESS) goto error;
    }
    else
    {
      const int zero = 0;
      const int width = roi_out->width;
      const int height = roi_out->height;

      err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_zoom_half_size, width, height,
        CLARG(dev_in), CLARG(dev_out), CLARG(width), CLARG(height), CLARG(zero), CLARG(zero), CLARG(roi_in->width),
        CLARG(roi_in->height), CLARG(roi_out->scale), CLARG(piece->pipe->dsc.filters));
      if(err != CL_SUCCESS) goto error;
    }
  }

  if(dev_aux != dev_out) dt_opencl_release_mem_object(dev_aux);
  dev_aux = NULL;

  dt_opencl_release_mem_object(dev_tmp);
  dev_tmp = NULL;

  dt_opencl_release_mem_object(dev_xtrans);
  dev_xtrans = NULL;

  dt_opencl_release_mem_object(dev_lookup);
  dev_lookup = NULL;

  free(lookup);

  dt_opencl_release_mem_object(dev_code);
  dev_code = NULL;

  dt_opencl_release_mem_object(dev_ips);
  dev_ips = NULL;

  dt_opencl_release_mem_object(dev_green_eq);
  dev_green_eq = NULL;

  free(ips);
  ips = NULL;

  // color smoothing
  if((data->color_smoothing) && smooth)
  {
    if(!color_smoothing_cl(self, piece, dev_out, dev_out, roi_out, data->color_smoothing))
      goto error;
  }

  return TRUE;

error:
  if(dev_aux != dev_out) dt_opencl_release_mem_object(dev_aux);
  dt_opencl_release_mem_object(dev_tmp);
  dt_opencl_release_mem_object(dev_xtrans);
  dt_opencl_release_mem_object(dev_lookup);
  free(lookup);
  dt_opencl_release_mem_object(dev_code);
  dt_opencl_release_mem_object(dev_ips);
  dt_opencl_release_mem_object(dev_green_eq);
  free(ips);
  dt_print(DT_DEBUG_OPENCL, "[opencl_demosaic] couldn't enqueue kernel! %s\n", cl_errstr(err));
  return FALSE;
}

static int process_markesteijn_cl(
        struct dt_iop_module_t *self,
        dt_dev_pixelpipe_iop_t *piece,
        cl_mem dev_in,
        cl_mem dev_out,
        const dt_iop_roi_t *const roi_in,
        const dt_iop_roi_t *const roi_out,
        const gboolean smooth)
{
  dt_iop_demosaic_data_t *data = (dt_iop_demosaic_data_t *)piece->data;
  dt_iop_demosaic_global_data_t *gd = (dt_iop_demosaic_global_data_t *)self->global_data;

  const int devid = piece->pipe->devid;
  const uint8_t(*const xtrans)[6] = (const uint8_t(*const)[6])piece->pipe->dsc.xtrans;

  const float processed_maximum[4]
      = { piece->pipe->dsc.processed_maximum[0], piece->pipe->dsc.processed_maximum[1],
          piece->pipe->dsc.processed_maximum[2], 1.0f };

  const int qual_flags = demosaic_qual_flags(piece, &self->dev->image_storage, roi_out);

  cl_mem dev_tmp = NULL;
  cl_mem dev_tmptmp = NULL;
  cl_mem dev_xtrans = NULL;
  cl_mem dev_green_eq = NULL;
  cl_mem dev_rgbv[8] = { NULL };
  cl_mem dev_drv[8] = { NULL };
  cl_mem dev_homo[8] = { NULL };
  cl_mem dev_homosum[8] = { NULL };
  cl_mem dev_gminmax = NULL;
  cl_mem dev_allhex = NULL;
  cl_mem dev_aux = NULL;
  cl_mem dev_edge_in = NULL;
  cl_mem dev_edge_out = NULL;
  cl_int err = DT_OPENCL_DEFAULT_ERROR;

  cl_mem *dev_rgb = dev_rgbv;

  dev_xtrans
      = dt_opencl_copy_host_to_device_constant(devid, sizeof(piece->pipe->dsc.xtrans), piece->pipe->dsc.xtrans);
  if(dev_xtrans == NULL) goto error;

  if(qual_flags & DT_DEMOSAIC_FULL_SCALE)
  {
    // Full demosaic and then scaling if needed
    const int scaled = (roi_out->width != roi_in->width || roi_out->height != roi_in->height);

    int width = roi_in->width;
    int height = roi_in->height;
    const int passes = ((data->demosaicing_method & ~DT_DEMOSAIC_DUAL) == DT_IOP_DEMOSAIC_MARKESTEIJN_3) ? 3 : 1;
    const int ndir = 4 << (passes > 1);
    const int pad_tile = (passes == 1) ? 12 : 17;

    static const short orth[12] = { 1, 0, 0, 1, -1, 0, 0, -1, 1, 0, 0, 1 },
                       patt[2][16] = { { 0, 1, 0, -1, 2, 0, -1, 0, 1, 1, 1, -1, 0, 0, 0, 0 },
                                       { 0, 1, 0, -2, 1, 0, -2, 0, 1, 1, -2, -2, 1, -1, -1, 1 } };

    // allhex contains the offset coordinates (x,y) of a green hexagon around each
    // non-green pixel and vice versa
    char allhex[3][3][8][2];
    // sgreen is the offset in the sensor matrix of the solitary
    // green pixels (initialized here only to avoid compiler warning)
    char sgreen[2] = { 0 };

    // Map a green hexagon around each non-green pixel and vice versa:
    for(int row = 0; row < 3; row++)
      for(int col = 0; col < 3; col++)
        for(int ng = 0, d = 0; d < 10; d += 2)
        {
          const int g = FCxtrans(row, col, NULL, xtrans) == 1;
          if(FCxtrans(row + orth[d] + 6, col + orth[d + 2] + 6, NULL, xtrans) == 1)
            ng = 0;
          else
            ng++;
          // if there are four non-green pixels adjacent in cardinal
          // directions, this is the solitary green pixel
          if(ng == 4)
          {
            sgreen[0] = col;
            sgreen[1] = row;
          }
          if(ng == g + 1)
            for(int c = 0; c < 8; c++)
            {
              const int v = orth[d] * patt[g][c * 2] + orth[d + 1] * patt[g][c * 2 + 1];
              const int h = orth[d + 2] * patt[g][c * 2] + orth[d + 3] * patt[g][c * 2 + 1];

              allhex[row][col][c ^ (g * 2 & d)][0] = h;
              allhex[row][col][c ^ (g * 2 & d)][1] = v;
            }
        }

    dev_allhex = dt_opencl_copy_host_to_device_constant(devid, sizeof(allhex), allhex);
    if(dev_allhex == NULL) goto error;

    for(int n = 0; n < ndir; n++)
    {
      dev_rgbv[n] = dt_opencl_alloc_device_buffer(devid, sizeof(float) * 4 * width * height);
      if(dev_rgbv[n] == NULL) goto error;
    }

    dev_gminmax = dt_opencl_alloc_device_buffer(devid, sizeof(float) * 2 * width * height);
    if(dev_gminmax == NULL) goto error;

    dev_aux = dt_opencl_alloc_device_buffer(devid, sizeof(float) * 4 * width * height);
    if(dev_aux == NULL) goto error;

    if(scaled)
    {
      // need to scale to right res
      dev_tmp = dt_opencl_alloc_device(devid, width, height, sizeof(float) * 4);
      if(dev_tmp == NULL) goto error;
    }
    else
    {
      // scaling factor 1.0 --> we can directly process into the output buffer
      dev_tmp = dev_out;
    }

    {
      // copy from dev_in to first rgb image buffer.
      err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_markesteijn_initial_copy, width, height,
        CLARG(dev_in), CLARG(dev_rgb[0]), CLARG(width), CLARG(height), CLARG(roi_in->x), CLARG(roi_in->y),
        CLARG(dev_xtrans));
      if(err != CL_SUCCESS) goto error;
    }


    // duplicate dev_rgb[0] to dev_rgb[1], dev_rgb[2], and dev_rgb[3]
    for(int c = 1; c <= 3; c++)
    {
      err = dt_opencl_enqueue_copy_buffer_to_buffer(devid, dev_rgb[0], dev_rgb[c], 0, 0,
                                                    sizeof(float) * 4 * width * height);
      if(err != CL_SUCCESS) goto error;
    }

    // find minimum and maximum allowed green values of red/blue pixel pairs
    const int pad_g1_g3 = 3;
    dt_opencl_local_buffer_t locopt_g1_g3
      = (dt_opencl_local_buffer_t){ .xoffset = 2*3, .xfactor = 1, .yoffset = 2*3, .yfactor = 1,
                                    .cellsize = 1 * sizeof(float), .overhead = 0,
                                    .sizex = 1 << 8, .sizey = 1 << 8 };

    if(!dt_opencl_local_buffer_opt(devid, gd->kernel_markesteijn_green_minmax, &locopt_g1_g3))
      goto error;

    {
      size_t sizes[3] = { ROUNDUP(width, locopt_g1_g3.sizex), ROUNDUP(height, locopt_g1_g3.sizey), 1 };
      size_t local[3] = { locopt_g1_g3.sizex, locopt_g1_g3.sizey, 1 };
      dt_opencl_set_kernel_args(devid, gd->kernel_markesteijn_green_minmax, 0, CLARG(dev_rgb[0]), CLARG(dev_gminmax),
        CLARG(width), CLARG(height), CLARG(pad_g1_g3), CLARG(roi_in->x), CLARG(roi_in->y), CLARRAY(2, sgreen),
        CLARG(dev_xtrans), CLARG(dev_allhex), CLLOCAL(sizeof(float) * (locopt_g1_g3.sizex + 2*3) * (locopt_g1_g3.sizey + 2*3)));
      err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_markesteijn_green_minmax, sizes, local);
      if(err != CL_SUCCESS) goto error;
    }

    // interpolate green horizontally, vertically, and along both diagonals
    const int pad_g_interp = 3;
    dt_opencl_local_buffer_t locopt_g_interp
      = (dt_opencl_local_buffer_t){ .xoffset = 2*6, .xfactor = 1, .yoffset = 2*6, .yfactor = 1,
                                    .cellsize = 4 * sizeof(float), .overhead = 0,
                                    .sizex = 1 << 8, .sizey = 1 << 8 };

    if(!dt_opencl_local_buffer_opt(devid, gd->kernel_markesteijn_interpolate_green, &locopt_g_interp))
      goto error;

    {
      size_t sizes[3] = { ROUNDUP(width, locopt_g_interp.sizex), ROUNDUP(height, locopt_g_interp.sizey), 1 };
      size_t local[3] = { locopt_g_interp.sizex, locopt_g_interp.sizey, 1 };
      dt_opencl_set_kernel_args(devid, gd->kernel_markesteijn_interpolate_green, 0, CLARG(dev_rgb[0]),
        CLARG(dev_rgb[1]), CLARG(dev_rgb[2]), CLARG(dev_rgb[3]), CLARG(dev_gminmax), CLARG(width), CLARG(height),
        CLARG(pad_g_interp), CLARG(roi_in->x), CLARG(roi_in->y), CLARRAY(2, sgreen), CLARG(dev_xtrans),
        CLARG(dev_allhex), CLLOCAL(sizeof(float) * 4 * (locopt_g_interp.sizex + 2*6) * (locopt_g_interp.sizey + 2*6)));
      err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_markesteijn_interpolate_green, sizes, local);
      if(err != CL_SUCCESS) goto error;
    }

    // multi-pass loop: one pass for Markesteijn-1 and three passes for Markesteijn-3
    for(int pass = 0; pass < passes; pass++)
    {

      // if on second pass, copy rgb[0] to [3] into rgb[4] to [7] ....
      if(pass == 1)
      {
        for(int c = 0; c < 4; c++)
        {
          err = dt_opencl_enqueue_copy_buffer_to_buffer(devid, dev_rgb[c], dev_rgb[c + 4], 0, 0,
                                                        sizeof(float) * 4 * width * height);
          if(err != CL_SUCCESS) goto error;
        }
        // ... and process that second set of buffers
        dev_rgb += 4;
      }

      // second and third pass (only Markesteijn-3)
      if(pass)
      {
        // recalculate green from interpolated values of closer pixels
        const int pad_g_recalc = 6;
        err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_markesteijn_recalculate_green, width, height,
          CLARG(dev_rgb[0]), CLARG(dev_rgb[1]), CLARG(dev_rgb[2]), CLARG(dev_rgb[3]), CLARG(dev_gminmax),
          CLARG(width), CLARG(height), CLARG(pad_g_recalc), CLARG(roi_in->x), CLARG(roi_in->y), CLARRAY(2, sgreen),
          CLARG(dev_xtrans), CLARG(dev_allhex));
        if(err != CL_SUCCESS) goto error;
      }

      // interpolate red and blue values for solitary green pixels
      const int pad_rb_g = (passes == 1) ? 6 : 5;
      dt_opencl_local_buffer_t locopt_rb_g
        = (dt_opencl_local_buffer_t){ .xoffset = 2*2, .xfactor = 1, .yoffset = 2*2, .yfactor = 1,
                                      .cellsize = 4 * sizeof(float), .overhead = 0,
                                      .sizex = 1 << 8, .sizey = 1 << 8 };

      if(!dt_opencl_local_buffer_opt(devid, gd->kernel_markesteijn_solitary_green, &locopt_rb_g))
      goto error;

      cl_mem *dev_trgb = dev_rgb;
      for(int d = 0, i = 1, h = 0; d < 6; d++, i ^= 1, h ^= 2)
      {
        const char dir[2] = { i, i ^ 1 };

        // we use dev_aux to transport intermediate results from one loop run to the next
        size_t sizes[3] = { ROUNDUP(width, locopt_rb_g.sizex), ROUNDUP(height, locopt_rb_g.sizey), 1 };
        size_t local[3] = { locopt_rb_g.sizex, locopt_rb_g.sizey, 1 };
        dt_opencl_set_kernel_args(devid, gd->kernel_markesteijn_solitary_green, 0, CLARG(dev_trgb[0]),
          CLARG(dev_aux), CLARG(width), CLARG(height), CLARG(pad_rb_g), CLARG(roi_in->x), CLARG(roi_in->y),
          CLARG(d), CLARRAY(2, dir), CLARG(h), CLARRAY(2, sgreen), CLARG(dev_xtrans), CLLOCAL(sizeof(float) * 4 * (locopt_rb_g.sizex + 2*2) * (locopt_rb_g.sizey + 2*2)));
        err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_markesteijn_solitary_green, sizes, local);
        if(err != CL_SUCCESS) goto error;

        if((d < 2) || (d & 1)) dev_trgb++;
      }

      // interpolate red for blue pixels and vice versa
      const int pad_rb_br = (passes == 1) ? 6 : 5;
      dt_opencl_local_buffer_t locopt_rb_br
        = (dt_opencl_local_buffer_t){ .xoffset = 2*3, .xfactor = 1, .yoffset = 2*3, .yfactor = 1,
                                      .cellsize = 4 * sizeof(float), .overhead = 0,
                                      .sizex = 1 << 8, .sizey = 1 << 8 };

      if(!dt_opencl_local_buffer_opt(devid, gd->kernel_markesteijn_red_and_blue, &locopt_rb_br))
      goto error;

      for(int d = 0; d < 4; d++)
      {
        size_t sizes[3] = { ROUNDUP(width, locopt_rb_br.sizex), ROUNDUP(height, locopt_rb_br.sizey), 1 };
        size_t local[3] = { locopt_rb_br.sizex, locopt_rb_br.sizey, 1 };
        dt_opencl_set_kernel_args(devid, gd->kernel_markesteijn_red_and_blue, 0, CLARG(dev_rgb[d]), CLARG(width),
          CLARG(height), CLARG(pad_rb_br), CLARG(roi_in->x), CLARG(roi_in->y), CLARG(d), CLARRAY(2, sgreen),
          CLARG(dev_xtrans), CLLOCAL(sizeof(float) * 4 * (locopt_rb_br.sizex + 2*3) * (locopt_rb_br.sizey + 2*3)));
        err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_markesteijn_red_and_blue, sizes, local);
        if(err != CL_SUCCESS) goto error;
      }

      // interpolate red and blue for 2x2 blocks of green
      const int pad_g22 = (passes == 1) ? 8 : 4;
      dt_opencl_local_buffer_t locopt_g22
        = (dt_opencl_local_buffer_t){ .xoffset = 2*2, .xfactor = 1, .yoffset = 2*2, .yfactor = 1,
                                      .cellsize = 4 * sizeof(float), .overhead = 0,
                                      .sizex = 1 << 8, .sizey = 1 << 8 };

      if(!dt_opencl_local_buffer_opt(devid, gd->kernel_markesteijn_interpolate_twoxtwo, &locopt_g22))
      goto error;

      for(int d = 0, n = 0; d < ndir; d += 2, n++)
      {
        size_t sizes[3] = { ROUNDUP(width, locopt_g22.sizex), ROUNDUP(height, locopt_g22.sizey), 1 };
        size_t local[3] = { locopt_g22.sizex, locopt_g22.sizey, 1 };
        dt_opencl_set_kernel_args(devid, gd->kernel_markesteijn_interpolate_twoxtwo, 0, CLARG(dev_rgb[n]),
          CLARG(width), CLARG(height), CLARG(pad_g22), CLARG(roi_in->x), CLARG(roi_in->y), CLARG(d), CLARRAY(2, sgreen),
          CLARG(dev_xtrans), CLARG(dev_allhex), CLLOCAL(sizeof(float) * 4 * (locopt_g22.sizex + 2*2) * (locopt_g22.sizey + 2*2)));
        err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_markesteijn_interpolate_twoxtwo, sizes, local);
        if(err != CL_SUCCESS) goto error;
      }
    }
    // end of multi pass

    // gminmax data no longer needed
    dt_opencl_release_mem_object(dev_gminmax);
    dev_gminmax = NULL;

    // jump back to the first set of rgb buffers (this is a noop for Markesteijn-1)
    dev_rgb = dev_rgbv;

    // prepare derivatives buffers
    for(int n = 0; n < ndir; n++)
    {
      dev_drv[n] = dt_opencl_alloc_device_buffer(devid, sizeof(float) * width * height);
      if(dev_drv[n] == NULL) goto error;
    }

    // convert to perceptual colorspace and differentiate in all directions
    const int pad_yuv = (passes == 1) ? 8 : 13;
    dt_opencl_local_buffer_t locopt_diff
      = (dt_opencl_local_buffer_t){ .xoffset = 2*1, .xfactor = 1, .yoffset = 2*1, .yfactor = 1,
                                    .cellsize = 4 * sizeof(float), .overhead = 0,
                                    .sizex = 1 << 8, .sizey = 1 << 8 };

    if(!dt_opencl_local_buffer_opt(devid, gd->kernel_markesteijn_differentiate, &locopt_diff))
    goto error;

    for(int d = 0; d < ndir; d++)
    {
      // convert to perceptual YPbPr colorspace
      err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_markesteijn_convert_yuv, width, height,
        CLARG(dev_rgb[d]), CLARG(dev_aux), CLARG(width), CLARG(height), CLARG(pad_yuv));
      if(err != CL_SUCCESS) goto error;


      // differentiate in all directions
      size_t sizes_diff[3] = { ROUNDUP(width, locopt_diff.sizex), ROUNDUP(height, locopt_diff.sizey), 1 };
      size_t local_diff[3] = { locopt_diff.sizex, locopt_diff.sizey, 1 };
      dt_opencl_set_kernel_args(devid, gd->kernel_markesteijn_differentiate, 0, CLARG(dev_aux), CLARG(dev_drv[d]),
        CLARG(width), CLARG(height), CLARG(pad_yuv), CLARG(d), CLLOCAL(sizeof(float) * 4 * (locopt_diff.sizex + 2*1) * (locopt_diff.sizey + 2*1)));
      err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_markesteijn_differentiate, sizes_diff, local_diff);
      if(err != CL_SUCCESS) goto error;
    }

    // reserve buffers for homogeneity maps and sum maps
    for(int n = 0; n < ndir; n++)
    {
      dev_homo[n] = dt_opencl_alloc_device_buffer(devid, sizeof(unsigned char) * width * height);
      if(dev_homo[n] == NULL) goto error;

      dev_homosum[n] = dt_opencl_alloc_device_buffer(devid, sizeof(unsigned char) * width * height);
      if(dev_homosum[n] == NULL) goto error;
    }

    // get thresholds for homogeneity map (store them in dev_aux)
    for(int d = 0; d < ndir; d++)
    {
      const int pad_homo = (passes == 1) ? 10 : 15;
      err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_markesteijn_homo_threshold, width, height,
        CLARG(dev_drv[d]), CLARG(dev_aux), CLARG(width), CLARG(height), CLARG(pad_homo), CLARG(d));
      if(err != CL_SUCCESS) goto error;
    }

    // set homogeneity maps
    const int pad_homo = (passes == 1) ? 10 : 15;
    dt_opencl_local_buffer_t locopt_homo
      = (dt_opencl_local_buffer_t){ .xoffset = 2*1, .xfactor = 1, .yoffset = 2*1, .yfactor = 1,
                                    .cellsize = 1 * sizeof(float), .overhead = 0,
                                    .sizex = 1 << 8, .sizey = 1 << 8 };

    if(!dt_opencl_local_buffer_opt(devid, gd->kernel_markesteijn_homo_set, &locopt_homo))
    goto error;

    for(int d = 0; d < ndir; d++)
    {
      size_t sizes[3] = { ROUNDUP(width, locopt_homo.sizex),ROUNDUP(height, locopt_homo.sizey), 1 };
      size_t local[3] = { locopt_homo.sizex, locopt_homo.sizey, 1 };
      dt_opencl_set_kernel_args(devid, gd->kernel_markesteijn_homo_set, 0, CLARG(dev_drv[d]), CLARG(dev_aux),
        CLARG(dev_homo[d]), CLARG(width), CLARG(height), CLARG(pad_homo), CLLOCAL(sizeof(float) * (locopt_homo.sizex + 2*1) * (locopt_homo.sizey + 2*1)));
      err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_markesteijn_homo_set, sizes, local);
      if(err != CL_SUCCESS) goto error;
    }

    // get rid of dev_drv buffers
    for(int n = 0; n < 8; n++)
    {
      dt_opencl_release_mem_object(dev_drv[n]);
      dev_drv[n] = NULL;
    }

    // build 5x5 sum of homogeneity maps for each pixel and direction
    dt_opencl_local_buffer_t locopt_homo_sum
      = (dt_opencl_local_buffer_t){ .xoffset = 2*2, .xfactor = 1, .yoffset = 2*2, .yfactor = 1,
                                    .cellsize = 1 * sizeof(float), .overhead = 0,
                                    .sizex = 1 << 8, .sizey = 1 << 8 };

    if(!dt_opencl_local_buffer_opt(devid, gd->kernel_markesteijn_homo_sum, &locopt_homo_sum))
    goto error;

    for(int d = 0; d < ndir; d++)
    {
      size_t sizes[3] = { ROUNDUP(width, locopt_homo_sum.sizex), ROUNDUP(height, locopt_homo_sum.sizey), 1 };
      size_t local[3] = { locopt_homo_sum.sizex, locopt_homo_sum.sizey, 1 };
      dt_opencl_set_kernel_args(devid, gd->kernel_markesteijn_homo_sum, 0, CLARG(dev_homo[d]), CLARG(dev_homosum[d]),
        CLARG(width), CLARG(height), CLARG(pad_tile), CLLOCAL(sizeof(char) * (locopt_homo_sum.sizex + 2*2) * (locopt_homo_sum.sizey + 2*2)));
      err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_markesteijn_homo_sum, sizes, local);
      if(err != CL_SUCCESS) goto error;
    }

    // get maximum of homogeneity maps (store in dev_aux)
    for(int d = 0; d < ndir; d++)
    {
      err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_markesteijn_homo_max, width, height,
        CLARG(dev_homosum[d]), CLARG(dev_aux), CLARG(width), CLARG(height), CLARG(pad_tile), CLARG(d));
      if(err != CL_SUCCESS) goto error;
    }

    {
      // adjust maximum value
      err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_markesteijn_homo_max_corr, width, height,
        CLARG(dev_aux), CLARG(width), CLARG(height), CLARG(pad_tile));
      if(err != CL_SUCCESS) goto error;
    }

    // for Markesteijn-3: use only one of two directions if there is a difference in homogeneity
    for(int d = 0; d < ndir - 4; d++)
    {
      err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_markesteijn_homo_quench, width, height,
        CLARG(dev_homosum[d]), CLARG(dev_homosum[d + 4]), CLARG(width), CLARG(height), CLARG(pad_tile));
      if(err != CL_SUCCESS) goto error;
    }

    {
      // initialize output buffer to zero
      err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_markesteijn_zero, width, height,
        CLARG(dev_tmp), CLARG(width), CLARG(height), CLARG(pad_tile));
      if(err != CL_SUCCESS) goto error;
    }

    // need to get another temp buffer for the output image (may use the space of dev_drv[] freed earlier)
    dev_tmptmp = dt_opencl_alloc_device(devid, (size_t)width, height, sizeof(float) * 4);
    if(dev_tmptmp == NULL) goto error;

    cl_mem dev_t1 = dev_tmp;
    cl_mem dev_t2 = dev_tmptmp;

    // accumulate all contributions
    for(int d = 0; d < ndir; d++)
    {
      err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_markesteijn_accu, width, height,
        CLARG(dev_t1), CLARG(dev_t2), CLARG(dev_rgbv[d]), CLARG(dev_homosum[d]), CLARG(dev_aux), CLARG(width),
        CLARG(height), CLARG(pad_tile));
      if(err != CL_SUCCESS) goto error;

      // swap buffers
      cl_mem dev_t = dev_t2;
      dev_t2 = dev_t1;
      dev_t1 = dev_t;
    }

    // copy output to dev_tmptmp (if not already there)
    // note: we need to take swap of buffers into account, so current output lies in dev_t1
    if(dev_t1 != dev_tmptmp)
    {
      size_t origin[] = { 0, 0, 0 };
      size_t region[] = { width, height, 1 };
      err = dt_opencl_enqueue_copy_image(devid, dev_t1, dev_tmptmp, origin, origin, region);
      if(err != CL_SUCCESS) goto error;
    }

    {
      // process the final image
      err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_markesteijn_final, width, height,
        CLARG(dev_tmptmp), CLARG(dev_tmp), CLARG(width), CLARG(height), CLARG(pad_tile), CLARRAY(4, processed_maximum));
      if(err != CL_SUCCESS) goto error;
    }

    // now it's time to get rid of most of the temporary buffers (except of dev_tmp and dev_xtrans)
    for(int n = 0; n < 8; n++)
    {
      dt_opencl_release_mem_object(dev_rgbv[n]);
      dev_rgbv[n] = NULL;
    }

    for(int n = 0; n < 8; n++)
    {
      dt_opencl_release_mem_object(dev_homo[n]);
      dev_homo[n] = NULL;
    }

    for(int n = 0; n < 8; n++)
    {
      dt_opencl_release_mem_object(dev_homosum[n]);
      dev_homosum[n] = NULL;
    }

    dt_opencl_release_mem_object(dev_aux);
    dev_aux = NULL;

    dt_opencl_release_mem_object(dev_xtrans);
    dev_xtrans = NULL;

    dt_opencl_release_mem_object(dev_allhex);
    dev_allhex = NULL;

    dt_opencl_release_mem_object(dev_green_eq);
    dev_green_eq = NULL;

    dt_opencl_release_mem_object(dev_tmptmp);
    dev_tmptmp = NULL;

    // take care of image borders. the algorithm above leaves an unprocessed border of pad_tile pixels.
    // strategy: take the four edges and process them each with process_vng_cl(). as VNG produces
    // an image with a border with only linear interpolation we process edges of pad_tile+3px and
    // drop 3px on the inner side if possible

    // take care of some degenerate cases (which might happen if we are called in a tiling context)
    const int wd = (width > pad_tile+3) ? pad_tile+3 : width;
    const int ht = (height > pad_tile+3) ? pad_tile+3 : height;
    const int wdc = (wd >= pad_tile+3) ? 3 : 0;
    const int htc = (ht >= pad_tile+3) ? 3 : 0;

    // the data of all four edges:
    // total edge: x-offset, y-offset, width, height,
    // after dropping: x-offset adjust, y-offset adjust, width adjust, height adjust
    const int edges[4][8] = { { 0, 0, wd, height, 0, 0, -wdc, 0 },
                              { 0, 0, width, ht, 0, 0, 0, -htc },
                              { width - wd, 0, wd, height, wdc, 0, -wdc, 0 },
                              { 0, height - ht, width, ht, 0, htc, 0, -htc } };

    for(int n = 0; n < 4; n++)
    {
      dt_iop_roi_t roi = { roi_in->x + edges[n][0], roi_in->y + edges[n][1], edges[n][2], edges[n][3], 1.0f };

      size_t iorigin[] = { edges[n][0], edges[n][1], 0 };
      size_t oorigin[] = { 0, 0, 0 };
      size_t region[] = { edges[n][2], edges[n][3], 1 };

      // reserve input buffer for image edge
      dev_edge_in = dt_opencl_alloc_device(devid, edges[n][2], edges[n][3], sizeof(float));
      if(dev_edge_in == NULL) goto error;

      // reserve output buffer for VNG processing of edge
      dev_edge_out = dt_opencl_alloc_device(devid, edges[n][2], edges[n][3], sizeof(float) * 4);
      if(dev_edge_out == NULL) goto error;

      // copy edge to input buffer
      err = dt_opencl_enqueue_copy_image(devid, dev_in, dev_edge_in, iorigin, oorigin, region);
      if(err != CL_SUCCESS) goto error;

      // VNG processing
      if(!process_vng_cl(self, piece, dev_edge_in, dev_edge_out, &roi, &roi, smooth, qual_flags & DT_DEMOSAIC_ONLY_VNG_LINEAR))
        goto error;

      // adjust for "good" part, dropping linear border where possible
      iorigin[0] += edges[n][4];
      iorigin[1] += edges[n][5];
      oorigin[0] += edges[n][4];
      oorigin[1] += edges[n][5];
      region[0] += edges[n][6];
      region[1] += edges[n][7];

      // copy output
      err = dt_opencl_enqueue_copy_image(devid, dev_edge_out, dev_tmp, oorigin, iorigin, region);
      if(err != CL_SUCCESS) goto error;

      // release intermediate buffers
      dt_opencl_release_mem_object(dev_edge_in);
      dt_opencl_release_mem_object(dev_edge_out);
      dev_edge_in = dev_edge_out = NULL;
    }
    dt_dev_write_rawdetail_mask_cl(piece, dev_tmp, roi_in, DT_DEV_DETAIL_MASK_DEMOSAIC);

    if(scaled)
    {
      dt_print_pipe(DT_DEBUG_PIPE, "clip_and_zoom_roi_cl", piece->pipe, self->so->op, roi_in, roi_out, "\n");
      // scale temp buffer to output buffer
      err = dt_iop_clip_and_zoom_roi_cl(devid, dev_out, dev_tmp, roi_out, roi_in);
      if(err != CL_SUCCESS) goto error;
    }
  }
  else
  {
    // sample third-size image
    const int width = roi_out->width;
    const int height = roi_out->height;

    err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_zoom_third_size, width, height,
      CLARG(dev_in), CLARG(dev_out), CLARG(width), CLARG(height), CLARG(roi_in->x), CLARG(roi_in->y),
      CLARG(roi_in->width), CLARG(roi_in->height), CLARG(roi_out->scale), CLARG(dev_xtrans));
    if(err != CL_SUCCESS) goto error;
  }

  // free remaining temporary buffers
  if(dev_tmp != dev_out) dt_opencl_release_mem_object(dev_tmp);
  dev_tmp = NULL;

  dt_opencl_release_mem_object(dev_xtrans);
  dev_xtrans = NULL;


  // color smoothing
  if(data->color_smoothing)
  {
    if(!color_smoothing_cl(self, piece, dev_out, dev_out, roi_out, data->color_smoothing))
      goto error;
  }

  return TRUE;

error:
  if(dev_tmp != dev_out) dt_opencl_release_mem_object(dev_tmp);

  for(int n = 0; n < 8; n++)
    dt_opencl_release_mem_object(dev_rgbv[n]);
  for(int n = 0; n < 8; n++)
    dt_opencl_release_mem_object(dev_drv[n]);
  for(int n = 0; n < 8; n++)
    dt_opencl_release_mem_object(dev_homo[n]);
  for(int n = 0; n < 8; n++)
    dt_opencl_release_mem_object(dev_homosum[n]);
  dt_opencl_release_mem_object(dev_gminmax);
  dt_opencl_release_mem_object(dev_tmptmp);
  dt_opencl_release_mem_object(dev_xtrans);
  dt_opencl_release_mem_object(dev_allhex);
  dt_opencl_release_mem_object(dev_green_eq);
  dt_opencl_release_mem_object(dev_aux);
  dt_opencl_release_mem_object(dev_edge_in);
  dt_opencl_release_mem_object(dev_edge_out);
  dt_print(DT_DEBUG_OPENCL, "[opencl_demosaic] couldn't enqueue process_markesteijn_cl kernel! %s\n", cl_errstr(err));
  return FALSE;
}

int process_cl(
        struct dt_iop_module_t *self,
        dt_dev_pixelpipe_iop_t *piece,
        cl_mem dev_in,
        cl_mem dev_out,
        const dt_iop_roi_t *const roi_in,
        const dt_iop_roi_t *const roi_out)
{
  const gboolean run_fast = piece->pipe->type & DT_DEV_PIXELPIPE_FAST;

  dt_dev_clear_rawdetail_mask(piece->pipe);

  dt_iop_demosaic_data_t *data = (dt_iop_demosaic_data_t *)piece->data;

  int demosaicing_method = data->demosaicing_method;

  gboolean showmask = FALSE;
  if(self->dev->gui_attached && (piece->pipe->type & DT_DEV_PIXELPIPE_FULL))
  {
    dt_iop_demosaic_gui_data_t *g = (dt_iop_demosaic_gui_data_t *)self->gui_data;
    showmask = (g->visual_mask);
    // take care of passthru modes
    if(piece->pipe->mask_display == DT_DEV_PIXELPIPE_DISPLAY_PASSTHRU)
      demosaicing_method = (piece->pipe->dsc.filters != 9u) ? DT_IOP_DEMOSAIC_RCD : DT_IOP_DEMOSAIC_MARKESTEIJN;
  }

  const int qual_flags = demosaic_qual_flags(piece, &self->dev->image_storage, roi_out);
  cl_mem high_image = NULL;
  cl_mem low_image = NULL;
  cl_mem blend = NULL;
  cl_mem details = NULL;
  cl_mem dev_aux = NULL;
  const gboolean dual = ((demosaicing_method & DT_DEMOSAIC_DUAL) && (qual_flags & DT_DEMOSAIC_FULL_SCALE) && (data->dual_thrs > 0.0f) && !run_fast);
  const int devid = piece->pipe->devid;
  gboolean retval = FALSE;

  if(demosaicing_method == DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME ||
     demosaicing_method == DT_IOP_DEMOSAIC_PPG ||
     demosaicing_method == DT_IOP_DEMOSAIC_PASSTHROUGH_COLOR )
  {
    if(!process_default_cl(self, piece, dev_in, dev_out, roi_in, roi_out, demosaicing_method)) return FALSE;
  }
  else if((demosaicing_method & ~DT_DEMOSAIC_DUAL) == DT_IOP_DEMOSAIC_RCD)
  {
    if(dual)
    {
      high_image = dt_opencl_alloc_device(devid, roi_in->width, roi_in->height, sizeof(float) * 4);
      if(high_image == NULL) return FALSE;
      if(!process_rcd_cl(self, piece, dev_in, high_image, roi_in, roi_in, FALSE)) goto finish;
    }
    else
    {
     if(!process_rcd_cl(self, piece, dev_in, dev_out, roi_in, roi_out, TRUE)) return FALSE;
    }
  }
  else if(demosaicing_method ==  DT_IOP_DEMOSAIC_VNG4 || demosaicing_method == DT_IOP_DEMOSAIC_VNG)
  {
    if(!process_vng_cl(self, piece, dev_in, dev_out, roi_in, roi_out, TRUE, FALSE)) return FALSE;
  }
  else if((demosaicing_method == DT_IOP_DEMOSAIC_MARKESTEIJN || demosaicing_method == DT_IOP_DEMOSAIC_MARKESTEIJN_3) &&
    !(qual_flags & DT_DEMOSAIC_XTRANS_FULL))
  {
    if(!process_vng_cl(self, piece, dev_in, dev_out, roi_in, roi_out, TRUE, qual_flags & DT_DEMOSAIC_ONLY_VNG_LINEAR)) return FALSE;
  }
  else if(((demosaicing_method & ~DT_DEMOSAIC_DUAL) == DT_IOP_DEMOSAIC_MARKESTEIJN ) ||
          ((demosaicing_method & ~DT_DEMOSAIC_DUAL) == DT_IOP_DEMOSAIC_MARKESTEIJN_3))
  {
    if(dual)
    {
      high_image = dt_opencl_alloc_device(devid, roi_in->width, roi_in->height, sizeof(float) * 4);
      if(high_image == NULL) return FALSE;
      if(!process_markesteijn_cl(self, piece, dev_in, high_image, roi_in, roi_in, FALSE)) return FALSE;
    }
    else
    {
      if(!process_markesteijn_cl(self, piece, dev_in, dev_out, roi_in, roi_out, TRUE)) return FALSE;
    }
  }
  else
  {
    dt_print(DT_DEBUG_OPENCL, "[opencl_demosaic] demosaicing method %d not yet supported by opencl code\n", demosaicing_method);
    return FALSE;
  }

  if(!dual)
  {
    retval = TRUE;
    goto finish;
  }

  // This is dual demosaicing only stuff
  const int scaled = (roi_out->width != roi_in->width || roi_out->height != roi_in->height);

  int width = roi_out->width;
  int height = roi_out->height;
  // need to reserve scaled auxiliary buffer or use dev_out
  if(scaled)
  {
    dev_aux = dt_opencl_alloc_device(devid, roi_in->width, roi_in->height, sizeof(float) * 4);
    if(dev_aux == NULL) goto finish;
    width = roi_in->width;
    height = roi_in->height;
  }
  else
    dev_aux = dev_out;

  // here we have work to be done only for dual demosaicers
  blend = dt_opencl_alloc_device_buffer(devid, sizeof(float) * width * height);
  details = dt_opencl_alloc_device_buffer(devid, sizeof(float) * width * height);
  low_image = dt_opencl_alloc_device(devid, width, height, sizeof(float) * 4);
  if((blend == NULL) || (low_image == NULL) || (details == NULL)) goto finish;

  if(process_vng_cl(self, piece, dev_in, low_image, roi_in, roi_in, FALSE, FALSE))
  {
    if(!color_smoothing_cl(self, piece, low_image, low_image, roi_in, 2))
    {
      retval = FALSE;
      goto finish;
    }
    retval = dual_demosaic_cl(self, piece, details, blend, high_image, low_image, dev_aux, width, height, showmask);
  }

  if(scaled)
  {
    dt_print_pipe(DT_DEBUG_PIPE, "clip_and_zoom_roi_cl", piece->pipe, self->so->op, roi_in, roi_out, "\n");
    // scale aux buffer to output buffer
    const int err = dt_iop_clip_and_zoom_roi_cl(devid, dev_out, dev_aux, roi_out, roi_in);
    if(err != CL_SUCCESS)
      retval = FALSE;
  }

  finish:
  dt_opencl_release_mem_object(high_image);
  dt_opencl_release_mem_object(low_image);
  dt_opencl_release_mem_object(details);
  dt_opencl_release_mem_object(blend);
  if(dev_aux != dev_out) dt_opencl_release_mem_object(dev_aux);
  if(!retval) dt_control_log(_("[dual demosaic_cl] internal problem"));
  return retval;
}
#endif

void tiling_callback(
        struct dt_iop_module_t *self,
        struct dt_dev_pixelpipe_iop_t *piece,
        const dt_iop_roi_t *roi_in,
        const dt_iop_roi_t *roi_out,
        struct dt_develop_tiling_t *tiling)
{
  dt_iop_demosaic_data_t *data = (dt_iop_demosaic_data_t *)piece->data;

  const float ioratio = (float)roi_out->width * roi_out->height / ((float)roi_in->width * roi_in->height);
  const float smooth = data->color_smoothing ? ioratio : 0.0f;
  const float greeneq
      = ((piece->pipe->dsc.filters != 9u) && (data->green_eq != DT_IOP_GREEN_EQ_NO)) ? 0.25f : 0.0f;
  const dt_iop_demosaic_method_t demosaicing_method = data->demosaicing_method & ~DT_DEMOSAIC_DUAL;

  const int qual_flags = demosaic_qual_flags(piece, &self->dev->image_storage, roi_out);
  const int full_scale_demosaicing = qual_flags & DT_DEMOSAIC_FULL_SCALE;

  // check if output buffer has same dimension as input buffer (thus avoiding one
  // additional temporary buffer)
  const int unscaled = (roi_out->width == roi_in->width && roi_out->height == roi_in->height);

  if((demosaicing_method == DT_IOP_DEMOSAIC_PPG) ||
      (demosaicing_method == DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME) ||
      (demosaicing_method == DT_IOP_DEMOSAIC_PASSTHROUGH_COLOR) ||
      (demosaicing_method == DT_IOP_DEMOSAIC_AMAZE))
  {
    // Bayer pattern with PPG, Passthrough or Amaze
    tiling->factor = 1.0f + ioratio;         // in + out

    if(full_scale_demosaicing && unscaled)
      tiling->factor += fmax(1.0f + greeneq, smooth);  // + tmp + geeneq | + smooth
    else if(full_scale_demosaicing)
      tiling->factor += fmax(2.0f + greeneq, smooth);  // + tmp + aux + greeneq | + smooth
    else
      tiling->factor += smooth;                        // + smooth

    tiling->maxbuf = 1.0f;
    tiling->overhead = 0;
    tiling->xalign = 2;
    tiling->yalign = 2;
    tiling->overlap = 5; // take care of border handling
  }
  else if(((demosaicing_method == DT_IOP_DEMOSAIC_MARKESTEIJN) ||
           (demosaicing_method == DT_IOP_DEMOSAIC_MARKESTEIJN_3) ||
           (demosaicing_method == DT_IOP_DEMOSAIC_FDC)) &&
          (qual_flags & DT_DEMOSAIC_XTRANS_FULL))
  {
    // X-Trans pattern full Markesteijn processing
    const int ndir = (demosaicing_method == DT_IOP_DEMOSAIC_MARKESTEIJN_3) ? 8 : 4;
    const int overlap = (demosaicing_method == DT_IOP_DEMOSAIC_MARKESTEIJN_3) ? 18 : 12;

    tiling->factor = 1.0f + ioratio;
    tiling->factor += ndir * 1.0f      // rgb
                      + ndir * 0.25f   // drv
                      + ndir * 0.125f  // homo + homosum
                      + 1.0f;          // aux

    if(full_scale_demosaicing && unscaled)
      tiling->factor += fmax(1.0f + greeneq, smooth);
    else if(full_scale_demosaicing)
      tiling->factor += fmax(2.0f + greeneq, smooth);
    else
      tiling->factor += smooth;

    tiling->maxbuf = 1.0f;
    tiling->overhead = 0;
    tiling->xalign = DT_XTRANS_SNAPPER;
    tiling->yalign = DT_XTRANS_SNAPPER;
    tiling->overlap = overlap;
  }
  else if(demosaicing_method == DT_IOP_DEMOSAIC_RCD)
  {
    tiling->factor = 1.0f + ioratio;
    if(full_scale_demosaicing && unscaled)
      tiling->factor += fmax(1.0f + greeneq, smooth);  // + tmp + geeneq | + smooth
    else if(full_scale_demosaicing)
      tiling->factor += fmax(2.0f + greeneq, smooth);  // + tmp + aux + greeneq | + smooth
    else
      tiling->factor += smooth;                        // + smooth
    tiling->maxbuf = 1.0f;
    tiling->overhead = sizeof(float) * RCD_TILESIZE * RCD_TILESIZE * 8 * dt_get_num_threads();
    tiling->xalign = 2;
    tiling->yalign = 2;
    tiling->overlap = 10;
    tiling->factor_cl = tiling->factor + 3.0f;
  }
  else if(demosaicing_method == DT_IOP_DEMOSAIC_LMMSE)
  {
    tiling->factor = 1.0f + ioratio;
    if(full_scale_demosaicing && unscaled)
      tiling->factor += fmax(1.0f + greeneq, smooth);  // + tmp + geeneq | + smooth
    else if(full_scale_demosaicing)
      tiling->factor += fmax(2.0f + greeneq, smooth);  // + tmp + aux + greeneq | + smooth
    else
      tiling->factor += smooth;                        // + smooth
    tiling->maxbuf = 1.0f;
    tiling->overhead = sizeof(float) * LMMSE_GRP * LMMSE_GRP * 6 * dt_get_num_threads();
    tiling->xalign = 2;
    tiling->yalign = 2;
    tiling->overlap = 10;
  }
  else
  {
    // VNG
    tiling->factor = 1.0f + ioratio;

    if(full_scale_demosaicing && unscaled)
      tiling->factor += fmax(1.0f + greeneq, smooth);
    else if(full_scale_demosaicing)
      tiling->factor += fmax(2.0f + greeneq, smooth);
    else
      tiling->factor += smooth;

    tiling->maxbuf = 1.0f;
    tiling->overhead = 0;
    tiling->xalign = 6; // covering Bayer pattern for VNG4 as well as xtrans for VNG
    tiling->yalign = 6; // covering Bayer pattern for VNG4 as well as xtrans for VNG
    tiling->overlap = 6;
  }
  if(data->demosaicing_method & DT_DEMOSAIC_DUAL)
  {
    // make sure VNG4 is also possible
    tiling->factor += 1.0f;
    tiling->xalign = MAX(6, tiling->xalign);
    tiling->yalign = MAX(6, tiling->yalign);
    tiling->overlap = MAX(6, tiling->overlap);
  }
  return;
}
#undef RCD_TILESIZE
#undef LMMSE_GRP

void init_global(dt_iop_module_so_t *module)
{
  const int program = 0; // from programs.conf
  dt_iop_demosaic_global_data_t *gd
      = (dt_iop_demosaic_global_data_t *)malloc(sizeof(dt_iop_demosaic_global_data_t));
  module->data = gd;
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
  gd->lmmse_gamma_in = NULL;
  gd->lmmse_gamma_out = NULL;
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_demosaic_global_data_t *gd = (dt_iop_demosaic_global_data_t *)module->data;
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
  dt_free_align(gd->lmmse_gamma_in);
  dt_free_align(gd->lmmse_gamma_out);
  free(module->data);
  module->data = NULL;
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *params, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_demosaic_params_t *p = (dt_iop_demosaic_params_t *)params;
  dt_iop_demosaic_data_t *d = (dt_iop_demosaic_data_t *)piece->data;

  if(!(dt_image_is_raw(&pipe->image))) piece->enabled = 0;
  d->green_eq = p->green_eq;
  d->color_smoothing = p->color_smoothing;
  d->median_thrs = p->median_thrs;
  d->dual_thrs = p->dual_thrs;
  d->lmmse_refine = p->lmmse_refine;
  dt_iop_demosaic_method_t use_method = p->demosaicing_method;
  const gboolean xmethod = use_method & DT_DEMOSAIC_XTRANS;
  const gboolean bayer   = (self->dev->image_storage.buf_dsc.filters != 9u);

  if(bayer && xmethod)   use_method = DT_IOP_DEMOSAIC_RCD;
  if(!bayer && !xmethod) use_method = DT_IOP_DEMOSAIC_MARKESTEIJN;

  if(use_method == DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME || use_method == DT_IOP_DEMOSAIC_PASSTHR_MONOX)
    use_method = DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME;
  if(use_method == DT_IOP_DEMOSAIC_PASSTHROUGH_COLOR || use_method == DT_IOP_DEMOSAIC_PASSTHR_COLORX)
    use_method = DT_IOP_DEMOSAIC_PASSTHROUGH_COLOR;

  const gboolean passing = (use_method == DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME || use_method == DT_IOP_DEMOSAIC_PASSTHROUGH_COLOR);

  if(!(use_method == DT_IOP_DEMOSAIC_PPG))
    d->median_thrs = 0.0f;

  if(passing)
  {
    d->green_eq = DT_IOP_GREEN_EQ_NO;
    d->color_smoothing = 0;
  }

  if(use_method & DT_DEMOSAIC_DUAL)
    d->color_smoothing = 0;

  d->demosaicing_method = use_method;

  // OpenCL only supported by some of the demosaicing methods
  switch(d->demosaicing_method)
  {
    case DT_IOP_DEMOSAIC_PPG:
      piece->process_cl_ready = 1;
      break;
    case DT_IOP_DEMOSAIC_AMAZE:
      piece->process_cl_ready = 0;
      break;
    case DT_IOP_DEMOSAIC_VNG4:
      piece->process_cl_ready = 1;
      break;
    case DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME:
      piece->process_cl_ready = 1;
      break;
    case DT_IOP_DEMOSAIC_PASSTHROUGH_COLOR:
      piece->process_cl_ready = 1;
      break;
    case DT_IOP_DEMOSAIC_RCD:
      piece->process_cl_ready = 1;
      break;
    case DT_IOP_DEMOSAIC_LMMSE:
      piece->process_cl_ready = 0;
      break;
    case DT_IOP_DEMOSAIC_RCD_VNG:
      piece->process_cl_ready = 1;
      break;
    case DT_IOP_DEMOSAIC_AMAZE_VNG:
      piece->process_cl_ready = 0;
      break;
    case DT_IOP_DEMOSAIC_MARKEST3_VNG:
      piece->process_cl_ready = 1;
      break;
    case DT_IOP_DEMOSAIC_VNG:
      piece->process_cl_ready = 1;
      break;
    case DT_IOP_DEMOSAIC_MARKESTEIJN:
      piece->process_cl_ready = 1;
      break;
    case DT_IOP_DEMOSAIC_MARKESTEIJN_3:
      piece->process_cl_ready = 1;
      break;
    case DT_IOP_DEMOSAIC_FDC:
      piece->process_cl_ready = 0;
      break;
    default:
      piece->process_cl_ready = 0;
  }

  // green-equilibrate over full image excludes tiling
  // The details mask is written inside process, this does not allow tiling.
  if((d->green_eq == DT_IOP_GREEN_EQ_FULL
      || d->green_eq == DT_IOP_GREEN_EQ_BOTH)
      || ((use_method & DT_DEMOSAIC_DUAL) && (d->dual_thrs > 0.0f))
      || (piece->pipe->want_detail_mask == (DT_DEV_DETAIL_MASK_REQUIRED | DT_DEV_DETAIL_MASK_DEMOSAIC)))
  {
    piece->process_tiling_ready = 0;
  }

  if(self->dev->image_storage.flags & DT_IMAGE_4BAYER)
  {
    // 4Bayer images not implemented in OpenCL yet
    piece->process_cl_ready = 0;

    // Get and store the matrix to go from camera to RGB for 4Bayer images
    if(!dt_colorspaces_conversion_matrices_rgb(self->dev->image_storage.adobe_XYZ_to_CAM,
                                               NULL, d->CAM_to_RGB,
                                               self->dev->image_storage.d65_color_matrix, NULL))
    {
      const char *camera = self->dev->image_storage.camera_makermodel;
      dt_print(DT_DEBUG_ALWAYS, "[colorspaces] `%s' color matrix not found for 4bayer image!\n", camera);
      dt_control_log(_("`%s' color matrix not found for 4bayer image!"), camera);
    }
  }
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_demosaic_data_t));
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void reload_defaults(dt_iop_module_t *module)
{
  dt_iop_demosaic_params_t *d = (dt_iop_demosaic_params_t *)module->default_params;

  if(dt_image_is_monochrome(&module->dev->image_storage))
    d->demosaicing_method = DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME;
  else if(module->dev->image_storage.buf_dsc.filters == 9u)
    d->demosaicing_method = DT_IOP_DEMOSAIC_MARKESTEIJN;
  else
    d->demosaicing_method = DT_IOP_DEMOSAIC_RCD;

  module->hide_enable_button = 1;

  module->default_enabled = dt_image_is_raw(&module->dev->image_storage);
  if(module->widget)
    gtk_stack_set_visible_child_name(GTK_STACK(module->widget), module->default_enabled ? "raw" : "non_raw");
}

void gui_changed(dt_iop_module_t *self, GtkWidget *w, void *previous)
{
  dt_iop_demosaic_gui_data_t *g = (dt_iop_demosaic_gui_data_t *)self->gui_data;
  dt_iop_demosaic_params_t *p = (dt_iop_demosaic_params_t *)self->params;

  const gboolean bayer = (self->dev->image_storage.buf_dsc.filters != 9u);
  dt_iop_demosaic_method_t use_method = p->demosaicing_method;
  const gboolean xmethod = use_method & DT_DEMOSAIC_XTRANS;

  if(bayer && xmethod)   use_method = DT_IOP_DEMOSAIC_RCD;
  if(!bayer && !xmethod) use_method = DT_IOP_DEMOSAIC_MARKESTEIJN;

  const gboolean isppg = (use_method == DT_IOP_DEMOSAIC_PPG);
  const gboolean isdual = (use_method & DT_DEMOSAIC_DUAL);
  const gboolean islmmse = (use_method == DT_IOP_DEMOSAIC_LMMSE);
  const gboolean passing = ((use_method == DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME) ||
                            (use_method == DT_IOP_DEMOSAIC_PASSTHROUGH_COLOR) ||
                            (use_method == DT_IOP_DEMOSAIC_PASSTHR_MONOX) ||
                            (use_method == DT_IOP_DEMOSAIC_PASSTHR_COLORX));

  gtk_widget_set_visible(g->demosaic_method_bayer, bayer);
  gtk_widget_set_visible(g->demosaic_method_xtrans, !bayer);

  // we might have a wrong method dur to xtrans/bayer - mode mismatch
  if(bayer)
    dt_bauhaus_combobox_set_from_value(g->demosaic_method_bayer, use_method);
  else
    dt_bauhaus_combobox_set_from_value(g->demosaic_method_xtrans, use_method);
  p->demosaicing_method = use_method;

  gtk_widget_set_visible(g->median_thrs, bayer && isppg);
  gtk_widget_set_visible(g->greeneq, !passing);
  gtk_widget_set_visible(g->color_smoothing, !passing && !isdual);
  gtk_widget_set_visible(g->dual_thrs, isdual);
  gtk_widget_set_visible(g->lmmse_refine, islmmse);

  dt_image_t *img = dt_image_cache_get(darktable.image_cache, self->dev->image_storage.id, 'w');
  int mono_changed = img->flags & DT_IMAGE_MONOCHROME_BAYER;
  if((p->demosaicing_method == DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME) ||
     (p->demosaicing_method == DT_IOP_DEMOSAIC_PASSTHR_MONOX))
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
  // as some modes change behaviour for previous pipeline modules we possibly do a reprocess
  if((w == g->demosaic_method_bayer) || (w == g->demosaic_method_xtrans))
    dt_dev_reprocess_center(self->dev);
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_demosaic_gui_data_t *g = (dt_iop_demosaic_gui_data_t *)self->gui_data;
  dt_bauhaus_widget_set_quad_active(g->dual_thrs, FALSE);

  g->visual_mask = FALSE;
  gui_changed(self, NULL, NULL);

  gtk_stack_set_visible_child_name(GTK_STACK(self->widget), self->default_enabled ? "raw" : "non_raw");
}

static void _visualize_callback(GtkWidget *quad, gpointer user_data)
{
  if(darktable.gui->reset) return;
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_demosaic_gui_data_t *g = (dt_iop_demosaic_gui_data_t *)self->gui_data;

  g->visual_mask = dt_bauhaus_widget_get_quad_active(quad);
  dt_dev_reprocess_center(self->dev);
}

void gui_focus(struct dt_iop_module_t *self, gboolean in)
{
  dt_iop_demosaic_gui_data_t *g = (dt_iop_demosaic_gui_data_t *)self->gui_data;
  if(!in)
  {
    const gboolean was_dualmask = g->visual_mask;
    dt_bauhaus_widget_set_quad_active(g->dual_thrs, FALSE);
    g->visual_mask = FALSE;
    if(was_dualmask) dt_dev_reprocess_center(self->dev);
  }
}

void gui_init(struct dt_iop_module_t *self)
{
  dt_iop_demosaic_gui_data_t *g = IOP_GUI_ALLOC(demosaic);

  GtkWidget *box_raw = self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  g->demosaic_method_bayer = dt_bauhaus_combobox_from_params(self, "demosaicing_method");

  const int xtrans = dt_bauhaus_combobox_get_from_value(g->demosaic_method_bayer, DT_DEMOSAIC_XTRANS);
  for(int i=0;i<7;i++) dt_bauhaus_combobox_remove_at(g->demosaic_method_bayer, xtrans);
  gtk_widget_set_tooltip_text(g->demosaic_method_bayer, _("Bayer sensor demosaicing method, PPG and RCD are fast, AMaZE and LMMSE are slow.\nLMMSE is suited best for high ISO images.\ndual demosaicers double processing time."));

  g->demosaic_method_xtrans = dt_bauhaus_combobox_from_params(self, "demosaicing_method");
  for(int i=0;i<xtrans;i++) dt_bauhaus_combobox_remove_at(g->demosaic_method_xtrans, 0);
  gtk_widget_set_tooltip_text(g->demosaic_method_xtrans, _("X-Trans sensor demosaicing method, Markesteijn 3-pass and frequency domain chroma are slow.\ndual demosaicers double processing time."));

  g->median_thrs = dt_bauhaus_slider_from_params(self, "median_thrs");
  dt_bauhaus_slider_set_digits(g->median_thrs, 3);
  gtk_widget_set_tooltip_text(g->median_thrs, _("threshold for edge-aware median.\nset to 0.0 to switch off\n"
                                                "set to 1.0 to ignore edges"));

  g->dual_thrs = dt_bauhaus_slider_from_params(self, "dual_thrs");
  dt_bauhaus_slider_set_digits(g->dual_thrs, 2);
  gtk_widget_set_tooltip_text(g->dual_thrs, _("contrast threshold for dual demosaic.\nset to 0.0 for high frequency content\n"
                                                "set to 1.0 for flat content\ntoggle to visualize the mask"));
  dt_bauhaus_widget_set_quad_paint(g->dual_thrs, dtgtk_cairo_paint_showmask, 0, NULL);
  dt_bauhaus_widget_set_quad_toggle(g->dual_thrs, TRUE);
  dt_bauhaus_widget_set_quad_active(g->dual_thrs, FALSE);
  g_signal_connect(G_OBJECT(g->dual_thrs), "quad-pressed", G_CALLBACK(_visualize_callback), self);

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

