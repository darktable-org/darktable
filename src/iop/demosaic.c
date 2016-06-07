/*
    This file is part of darktable,
    copyright (c) 2009--2010 johannes hanika.
    copyright (c) 2016 Ulrich Pegelow.

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
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "develop/tiling.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"

#include <memory.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

// we assume people have -msee support.
#if defined(__SSE__)
#include <xmmintrin.h>
#endif

DT_MODULE_INTROSPECTION(3, dt_iop_demosaic_params_t)

#define DEMOSAIC_XTRANS 1024 // masks for non-Bayer demosaic ops

typedef enum dt_iop_demosaic_method_t
{
  // methods for Bayer images
  DT_IOP_DEMOSAIC_PPG = 0,
  DT_IOP_DEMOSAIC_AMAZE = 1,
  DT_IOP_DEMOSAIC_VNG4 = 2,
  DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME = 3,
  // methods for x-trans images
  DT_IOP_DEMOSAIC_VNG = DEMOSAIC_XTRANS | 0,
  DT_IOP_DEMOSAIC_MARKESTEIJN = DEMOSAIC_XTRANS | 1,
  DT_IOP_DEMOSAIC_MARKESTEIJN_3 = DEMOSAIC_XTRANS | 2
} dt_iop_demosaic_method_t;

typedef enum dt_iop_demosaic_greeneq_t
{
  DT_IOP_GREEN_EQ_NO = 0,
  DT_IOP_GREEN_EQ_LOCAL = 1,
  DT_IOP_GREEN_EQ_FULL = 2,
  DT_IOP_GREEN_EQ_BOTH = 3
} dt_iop_demosaic_greeneq_t;

typedef struct dt_iop_demosaic_params_t
{
  dt_iop_demosaic_greeneq_t green_eq;
  float median_thrs;
  uint32_t color_smoothing;
  dt_iop_demosaic_method_t demosaicing_method;
  uint32_t yet_unused_data_specific_to_demosaicing_method;
} dt_iop_demosaic_params_t;

typedef struct dt_iop_demosaic_gui_data_t
{
  GtkWidget *box_raw;
  GtkWidget *median_thrs;
  GtkWidget *greeneq;
  GtkWidget *color_smoothing;
  GtkWidget *demosaic_method_bayer;
  GtkWidget *demosaic_method_xtrans;
  GtkWidget *label_non_raw;
} dt_iop_demosaic_gui_data_t;

typedef struct dt_iop_demosaic_global_data_t
{
  // demosaic pattern
  int kernel_green_eq;
  int kernel_pre_median;
  int kernel_passthrough_monochrome;
  int kernel_ppg_green;
  int kernel_ppg_green_median;
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
} dt_iop_demosaic_global_data_t;

typedef struct dt_iop_demosaic_data_t
{
  uint32_t green_eq;
  uint32_t color_smoothing;
  uint32_t demosaicing_method;
  uint32_t yet_unused_data_specific_to_demosaicing_method;
  float median_thrs;
  double CAM_to_RGB[3][4];
} dt_iop_demosaic_data_t;

void amaze_demosaic_RT(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const float *const in,
                       float *out, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out,
                       const uint32_t filters);

const char *name()
{
  return _("demosaic");
}

int groups()
{
  return IOP_GROUP_BASIC;
}

int flags()
{
  return IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_ONE_INSTANCE;
}

void init_key_accels(dt_iop_module_so_t *self)
{
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "edge threshold"));
}

void connect_key_accels(dt_iop_module_t *self)
{
  dt_accel_connect_slider_iop(self, "edge threshold",
                              GTK_WIDGET(((dt_iop_demosaic_gui_data_t *)self->gui_data)->median_thrs));
}

int legacy_params(dt_iop_module_t *self, const void *const old_params, const int old_version,
                  void *new_params, const int new_version)
{
  if(old_version == 2 && new_version == 3)
  {
    dt_iop_demosaic_params_t *o = (dt_iop_demosaic_params_t *)old_params;
    dt_iop_demosaic_params_t *n = (dt_iop_demosaic_params_t *)new_params;
    n->green_eq = o->green_eq;
    n->median_thrs = o->median_thrs;
    n->color_smoothing = 0;
    n->demosaicing_method = DT_IOP_DEMOSAIC_PPG;
    n->yet_unused_data_specific_to_demosaicing_method = 0;
    return 0;
  }
  return 1;
}

#ifdef HAVE_OPENCL
static const char* method2string(dt_iop_demosaic_method_t method)
{
  const char *string;

  switch(method)
  {
    case DT_IOP_DEMOSAIC_PPG:
      string = "PPG";
      break;
    case DT_IOP_DEMOSAIC_AMAZE:
      string = "AMaZE";
      break;
    case DT_IOP_DEMOSAIC_VNG4:
      string = "VNG4";
      break;
    case DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME:
      string = "passthrough monochrome";
      break;
    case DT_IOP_DEMOSAIC_VNG:
      string = "VNG (xtrans)";
      break;
    case DT_IOP_DEMOSAIC_MARKESTEIJN:
      string = "Markesteijn-1 (xtrans)";
      break;
    case DT_IOP_DEMOSAIC_MARKESTEIJN_3:
      string = "Markesteijn-3 (xtrans)";
      break;
    default:
      string = "(unknown method)";
  }
  return string;
}
#endif

#define SWAP(a, b)                                                                                           \
  {                                                                                                          \
    const float tmp = (b);                                                                                   \
    (b) = (a);                                                                                               \
    (a) = tmp;                                                                                               \
  }

static void pre_median_b(float *out, const float *const in, const dt_iop_roi_t *const roi, const uint32_t filters,
                         const int num_passes, const float threshold)
{
#if 1
  memcpy(out, in, (size_t)roi->width * roi->height * sizeof(float));
#else
  // colors:
  const float thrsc = 2 * threshold;
  for(int pass = 0; pass < num_passes; pass++)
  {
    for(int c = 0; c < 3; c += 2)
    {
      int rows = 3;
      if(FC(rows, 3, filters) != c && FC(rows, 4, filters) != c) rows++;
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(rows, c, out) schedule(static)
#endif
      for(int row = rows; row < roi->height - 3; row += 2)
      {
        float med[9];
        int col = 3;
        if(FC(row, col, filters) != c) col++;
        float *pixo = out + (size_t)roi->width * row + col;
        const float *pixi = in + (size_t)roi->width * row + col;
        for(; col < roi->width - 3; col += 2)
        {
          int cnt = 0;
          for(int k = 0, i = -2 * roi->width; i <= 2 * roi->width; i += 2 * roi->width)
          {
            for(int j = i - 2; j <= i + 2; j += 2)
            {
              if(fabsf(pixi[j] - pixi[0]) < thrsc)
              {
                med[k++] = pixi[j];
                cnt++;
              }
              else
                med[k++] = 64.0f + pixi[j];
            }
          }
          for(int i = 0; i < 8; i++)
            for(int ii = i + 1; ii < 9; ii++)
              if(med[i] > med[ii]) SWAP(med[i], med[ii]);
#if 0
          // cnt == 1 and no small edge in greens.
          if(fabsf(pixi[-roi->width] - pixi[+roi->width]) + fabsf(pixi[-1] - pixi[+1])
              + fabsf(pixi[-roi->width] - pixi[+1]) + fabsf(pixi[-1] - pixi[+roi->width])
              + fabsf(pixi[+roi->width] - pixi[+1]) + fabsf(pixi[-1] - pixi[-roi->width])
              > 0.06)
            pixo[0] = med[(cnt-1)/2];
          else
#endif
          pixo[0] = (cnt == 1 ? med[4] - 64.0f : med[(cnt - 1) / 2]);
          pixo += 2;
          pixi += 2;
        }
      }
    }
  }
#endif

  // now green:
  const int lim[5] = { 0, 1, 2, 1, 0 };
  for(int pass = 0; pass < num_passes; pass++)
  {
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(out) schedule(static)
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
        pixo[0] = (cnt == 1 ? med[4] - 64.0f : med[(cnt - 1) / 2]);
        // pixo[0] = med[(cnt-1)/2];
        pixo += 2;
        pixi += 2;
      }
    }
  }
}

static void pre_median(float *out, const float *const in, const dt_iop_roi_t *const roi, const uint32_t filters,
                       const int num_passes, const float threshold)
{
  pre_median_b(out, in, roi, filters, num_passes, threshold);
}

#define SWAPmed(I, J)                                                                                        \
  if(med[I] > med[J]) SWAP(med[I], med[J])

static void color_smoothing(float *out, const dt_iop_roi_t *const roi_out, const int num_passes)
{
  const int width4 = 4 * roi_out->width;

  for(int pass = 0; pass < num_passes; pass++)
  {
    for(int c = 0; c < 3; c += 2)
    {
      float *outp = out;
      for(int j = 0; j < roi_out->height; j++)
        for(int i = 0; i < roi_out->width; i++, outp += 4) outp[3] = outp[c];
#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none) shared(out, c)
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

static void green_equilibration_lavg(float *out, const float *const in, const int width, const int height,
                                     const uint32_t filters, const int x, const int y, const int in_place,
                                     const float thr)
{
  const float maximum = 1.0f;

  int oj = 2, oi = 2;
  if(FC(oj + y, oi + x, filters) != 1) oj++;
  if(FC(oj + y, oi + x, filters) != 1) oi++;
  if(FC(oj + y, oi + x, filters) != 1) oj--;

  if(!in_place) memcpy(out, in, height * width * sizeof(float));

#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none) shared(out, oi, oj)
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
      if(m2 > 0.0f && m1 / m2 < maximum * 2.0f)
      {
        const float c1 = (fabsf(o1_1 - o1_2) + fabsf(o1_1 - o1_3) + fabsf(o1_1 - o1_4) + fabsf(o1_2 - o1_3)
                          + fabsf(o1_3 - o1_4) + fabsf(o1_2 - o1_4)) / 6.0f;
        const float c2 = (fabsf(o2_1 - o2_2) + fabsf(o2_1 - o2_3) + fabsf(o2_1 - o2_4) + fabsf(o2_2 - o2_3)
                          + fabsf(o2_3 - o2_4) + fabsf(o2_2 - o2_4)) / 6.0f;
        if((in[j * width + i] < maximum * 0.95f) && (c1 < maximum * thr) && (c2 < maximum * thr))
        {
          out[j * width + i] = in[j * width + i] * m1 / m2;
        }
      }
    }
  }
}

static void green_equilibration_favg(float *out, const float *const in, const int width, const int height,
                                     const uint32_t filters, const int x, const int y)
{
  int oj = 0, oi = 0;
  // const float ratio_max = 1.1f;
  double sum1 = 0.0, sum2 = 0.0, gr_ratio;

  if((FC(oj + y, oi + x, filters) & 1) != 1) oi++;
  int g2_offset = oi ? -1 : 1;
  memcpy(out, in, (size_t)height * width * sizeof(float));
#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none) reduction(+ : sum1, sum2) shared(oi, oj, g2_offset)
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
    gr_ratio = sum1 / sum2;
  else
    return;

#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none) shared(out, oi, oj, gr_ratio, g2_offset)
#endif
  for(int j = oj; j < (height - 1); j += 2)
  {
    for(int i = oi; i < (width - 1 - g2_offset); i += 2)
    {
      out[(size_t)j * width + i] = in[(size_t)j * width + i] / gr_ratio;
    }
  }
}


//
// x-trans specific demosaicing algorithms
//

// xtrans_interpolate adapted from dcraw 9.20

#define SQR(x) ((x) * (x))
// tile size, optimized to keep data in L2 cache
#define TS 122

/** Lookup for allhex[], making sure that row/col aren't negative **/
static inline const short *const hexmap(const int row, const int col,
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
static void xtrans_markesteijn_interpolate(float *out, const float *const in,
                                           const dt_iop_roi_t *const roi_out,
                                           const dt_iop_roi_t *const roi_in,
                                           const uint8_t (*const xtrans)[6], const int passes)
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
  char *const all_buffers = (char *)dt_alloc_align(16, dt_get_num_threads() * buffer_size);
  if(!all_buffers)
  {
    printf("[demosaic] not able to allocate Markesteijn buffers\n");
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
  const int pad_tile = (passes == 1) ? 12 : 17;
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(sgrow, sgcol, allhex, out) schedule(dynamic)
#endif
  // step through TSxTS cells of image, each tile overlapping the
  // prior as interpolation needs a substantial border
  for(int top = -pad_tile; top < height - pad_tile; top += TS - (pad_tile*2))
  {
    char *const buffer = all_buffers + dt_get_thread_num() * buffer_size;
    // rgb points to ndir TSxTS tiles of 3 channels (R, G, and B)
    float(*rgb)[TS][TS][3] = (float(*)[TS][TS][3])buffer;
    // yuv points to 3 channel (Y, u, and v) TSxTS tiles
    // note that channels come before tiles to allow for a
    // vectorization optimization when building drv[] from yuv[]
    float (*const yuv)[TS][TS] = (float(*)[TS][TS])(buffer + TS * TS * (ndir * 3) * sizeof(float));
    // drv points to ndir TSxTS tiles, each a single chanel of derivatives
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
              if(cc != c)
                pix[cc] = 0.0f;
              else
              {
#define TRANSLATE(n, size) ((n >= size) ? (2 * size - n - 1) : abs(n))
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
            const short *const hex = hexmap(row,col,allhex);
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
          const short *const hex = hexmap(row,col,allhex);
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
          memcpy(rgb + 4, rgb, (size_t)4 * sizeof(*rgb));
          rgb += 4;
        }

        /* Recalculate green from interpolated values of closer pixels: */
        if(pass)
        {
          const int pad_g_recalc = 6;
          for(int row = top + pad_g_recalc; row < mrow - pad_g_recalc; row++)
            for(int col = left + pad_g_recalc; col < mcol - pad_g_recalc; col++)
            {
              int f = FCxtrans(row, col, roi_in, xtrans);
              if(f == 1) continue;
              const short *const hex = hexmap(row,col,allhex);
              for(int d = 3; d < 6; d++)
              {
                float(*rfx)[3] = &rgb[(d - 2) ^ !((row - sgrow) % 3)][row - top][col - left];
                float val = rfx[-2 * hex[d]][1] + 2 * rfx[hex[d]][1] - rfx[-2 * hex[d]][f]
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
            float color[3][8];
            for(int i = 1, d = 0; d < 6; d++, i ^= TS ^ 1, h ^= 2)
            {
              for(int c = 0; c < 2; c++, h ^= 2)
              {
                float g = 2 * rfx[0][1] - rfx[i << c][1] - rfx[-i << c][1];
                color[h][d] = g + rfx[i << c][h] + rfx[-i << c][h];
                if(d > 1)
                  diff[d] += SQR(rfx[i << c][1] - rfx[-i << c][1] - rfx[i << c][h] + rfx[-i << c][h])
                             + SQR(g);
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
        const int pad_rb_br = (passes == 1) ? 6 : 5;
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
              int i = d > 1 || ((d ^ c) & 1) ||
                ((fabsf(rfx[0][1]-rfx[c][1]) + fabsf(rfx[0][1]-rfx[-c][1])) <
                 2.f*(fabsf(rfx[0][1]-rfx[h][1]) + fabsf(rfx[0][1]-rfx[-h][1]))) ? c:h;
              rfx[0][f] = (rfx[i][f] + rfx[-i][f] + 2.f * rfx[0][1] - rfx[i][1] - rfx[-i][1]) / 2.f;
            }
          }

        /* Fill in red and blue for 2x2 blocks of green:                */
        const int pad_g22 = (passes == 1) ? 8 : 4;
        for(int row = top + pad_g22; row < mrow - pad_g22; row++)
          if((row - sgrow) % 3)
            for(int col = left + pad_g22; col < mcol - pad_g22; col++)
              if((col - sgcol) % 3)
              {
                float(*rfx)[3] = &rgb[0][row - top][col - left];
                const short *const hex = hexmap(row,col,allhex);
                for(int d = 0; d < ndir; d += 2, rfx += TS * TS)
                  if(hex[d] + hex[d + 1])
                  {
                    float g = 3.f * rfx[0][1] - 2.f * rfx[hex[d]][1] - rfx[hex[d + 1]][1];
                    for(int c = 0; c < 4; c += 2)
                      rfx[0][c] = (g + 2.f * rfx[hex[d]][c] + rfx[hex[d + 1]][c]) / 3.f;
                  }
                  else
                  {
                    float g = 2.f * rfx[0][1] - rfx[hex[d]][1] - rfx[hex[d + 1]][1];
                    for(int c = 0; c < 4; c += 2)
                      rfx[0][c] = (g + rfx[hex[d]][c] + rfx[hex[d + 1]][c]) / 2.f;
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
        const int f = dir[d & 3];
        const int pad_drv = (passes == 1) ? 9 : 14;
        for(int row = pad_drv; row < mrow - pad_drv; row++)
          for(int col = pad_drv; col < mcol - pad_drv; col++)
          {
            float(*yfx)[TS][TS] = (float(*)[TS][TS]) & yuv[0][row][col];
            drv[d][row][col] = SQR(2 * yfx[0][0][0] - yfx[0][0][f] - yfx[0][0][-f])
                               + SQR(2 * yfx[1][0][0] - yfx[1][0][f] - yfx[1][0][-f])
                               + SQR(2 * yfx[2][0][0] - yfx[2][0][f] - yfx[2][0][-f]);
          }
      }

      /* Build homogeneity maps from the derivatives:                   */
      memset(homo, 0, (size_t)ndir * TS * TS * sizeof(uint8_t));
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
              for(int h = -1; h <= 1; h++) homo[d][row][col] += ((drv[d][row + v][col + h] <= tr) ? 1 : 0);
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

      /* Average the most homogenous pixels for the final result:       */
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
            if(hm[d] < hm[d + 4])
              hm[d] = 0;
            else if(hm[d] > hm[d + 4])
              hm[d + 4] = 0;
          float avg[4] = { 0.0f };
          for(int d = 0; d < ndir; d++)
            if(hm[d] >= maxval)
            {
              for(int c = 0; c < 3; c++) avg[c] += rgb[d][row][col][c];
              avg[3]++;
            }
          for(int c = 0; c < 3; c++)
            out[4 * (width * (row + top) + col + left) + c] =
              avg[c]/avg[3];
        }
    }
  }
  free(all_buffers);
}

#undef TS

/* taken from dcraw and demosaic_ppg below */

static void lin_interpolate(float *out, const float *const in, const dt_iop_roi_t *const roi_out,
                            const dt_iop_roi_t *const roi_in, const uint32_t filters,
                            const uint8_t (*const xtrans)[6])
{
  const int colors = (filters == 9) ? 3 : 4;

// border interpolate
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(out) schedule(static)
#endif
  for(int row = 0; row < roi_out->height; row++)
    for(int col = 0; col < roi_out->width; col++)
    {
      float sum[4] = { 0.0f };
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
          out[4 * (row * roi_out->width + col) + c] = sum[c] / count[c];
        else
          out[4 * (row * roi_out->width + col) + c] = in[row * roi_in->width + col];
      }
    }

  // build interpolation lookup table which for a given offset in the sensor
  // lists neighboring pixels from which to interpolate:
  // NUM_PIXELS                 # of neighboring pixels to read
  // for (1..NUM_PIXELS):
  //   OFFSET                   # in bytes from current pixel
  //   WEIGHT                   # how much weight to give this neighbor
  //   COLOR                    # sensor color
  // # weights of adjoining pixels not of this pixel's color
  // COLORA TOT_WEIGHT
  // COLORB TOT_WEIGHT
  // COLORPIX                   # color of center pixel

  int lookup[16][16][32];
  const int size = (filters == 9) ? 6 : 16;
  for(int row = 0; row < size; row++)
    for(int col = 0; col < size; col++)
    {
      int *ip = lookup[row][col] + 1;
      int sum[4] = { 0 };
      const int f = fcol(row + roi_in->y, col + roi_in->x, filters, xtrans);
      // make list of adjoining pixel offsets by weight & color
      for(int y = -1; y <= 1; y++)
        for(int x = -1; x <= 1; x++)
        {
          int weight = 1 << ((y == 0) + (x == 0));
          const int color = fcol(row + y + roi_in->y, col + x + roi_in->x, filters, xtrans);
          if(color == f) continue;
          *ip++ = (roi_in->width * y + x);
          *ip++ = weight;
          *ip++ = color;
          sum[color] += weight;
        }
      lookup[row][col][0] = (ip - lookup[row][col]) / 3; /* # of neighboring pixels found */
      for(int c = 0; c < colors; c++)
        if(c != f)
        {
          *ip++ = c;
          *ip++ = sum[c];
        }
      *ip = f;
    }

#ifdef _OPENMP
#pragma omp parallel for default(none) shared(lookup, out) schedule(static)
#endif
  for(int row = 1; row < roi_out->height - 1; row++)
  {
    float *buf = out + 4 * roi_out->width * row + 4;
    const float *buf_in = in + roi_in->width * row + 1;
    for(int col = 1; col < roi_out->width - 1; col++)
    {
      float sum[4] = { 0.0f };
      int *ip = lookup[row % size][col % size];
      // for each adjoining pixel not of this pixel's color, sum up its weighted values
      for(int i = *ip++; i--; ip += 3) sum[ip[2]] += buf_in[ip[0]] * ip[1];
      // for each interpolated color, load it into the pixel
      for(int i = colors; --i; ip += 2) buf[*ip] = sum[ip[0]] / ip[1];
      buf[*ip] = *buf_in;
      buf += 4;
      buf_in++;
    }
  }
}


// VNG interpolate adapted from dcraw 9.20

/*
   This algorithm is officially called:

   "Interpolation using a Threshold-based variable number of gradients"

   described in http://scien.stanford.edu/pages/labsite/1999/psych221/projects/99/tingchen/algodep/vargra.html

   I've extended the basic idea to work with non-Bayer filter arrays.
   Gradients are numbered clockwise from NW=0 to W=7.
 */
static void vng_interpolate(float *out, const float *const in,
                            const dt_iop_roi_t *const roi_out, const dt_iop_roi_t *const roi_in,
                            const uint32_t filters, const uint8_t (*const xtrans)[6], const int only_vng_linear)
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
          +1, -1, +1, +1, 1, 0x88, +1, +0, +1, +2, 1, 0x08, +1, +0, +2, -1, 1, 0x40, +1, +0, +2, +1, 1, 0x10 },
      chood[] = { -1, -1, -1, 0, -1, +1, 0, +1, +1, +1, +1, 0, +1, -1, 0, -1 };
  int *ip, *code[16][16];
  // ring buffer pointing to three most recent rows procesed (brow[3]
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
      = (char *)dt_alloc_align(16, (size_t)sizeof(**brow) * width * 3 + sizeof(*ip) * prow * pcol * 320);
  if(!buffer)
  {
    fprintf(stderr, "[demosaic] not able to allocate VNG buffer\n");
    return;
  }
  for(int row = 0; row < 3; row++) brow[row] = (float(*)[4])buffer + row * width;
  ip = (int *)(buffer + (size_t)sizeof(**brow) * width * 3);

  for(int row = 0; row < prow; row++) /* Precalculate for VNG */
    for(int col = 0; col < pcol; col++)
    {
      code[row][col] = ip;
      const signed char *cp = terms;
      for(int t = 0; t < 64; t++)
      {
        int y1 = *cp++, x1 = *cp++;
        int y2 = *cp++, x2 = *cp++;
        int weight = *cp++;
        int grads = *cp++;
        int color = fcol(row + y1, col + x1, filters4, xtrans);
        if(fcol(row + y2, col + x2, filters4, xtrans) != color) continue;
        int diag
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
        int y = *cp++, x = *cp++;
        *ip++ = (y * width + x) * 4;
        int color = fcol(row, col, filters4, xtrans);
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
#pragma omp parallel for default(none) shared(row, code, brow, out, filters4) private(ip) schedule(static)
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
        memcpy(brow[2][col], pix, (size_t)4 * sizeof(*out));
        continue;
      }
      float thold = gmin + (gmax * 0.5f);
      float sum[4] = { 0.0f };
      int color = fcol(row + roi_in->y, col + roi_in->x, filters4, xtrans);
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
      memcpy(out + 4 * ((row - 2) * width + 2), brow[0] + 2, (size_t)(width - 4) * 4 * sizeof(*out));
    // rotate ring buffer
    for(int g = 0; g < 4; g++) brow[(g - 1) & 3] = brow[g];
  }
  // copy the final two rows to the image
  memcpy(out + (4 * ((height - 4) * width + 2)), brow[0] + 2, (size_t)(width - 4) * 4 * sizeof(*out));
  memcpy(out + (4 * ((height - 3) * width + 2)), brow[1] + 2, (size_t)(width - 4) * 4 * sizeof(*out));
  free(buffer);

  if(filters != 9 && !FILTERS_ARE_4BAYER(filters)) // x-trans or CYGM/RGBE
// for Bayer mix the two greens to make VNG4
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(out) schedule(static)
#endif
    for(int i = 0; i < height * width; i++) out[i * 4 + 1] = (out[i * 4 + 1] + out[i * 4 + 3]) / 2.0f;
}

/** 1:1 demosaic from in to out, in is full buf, out is translated/cropped (scale == 1.0!) */
static void passthrough_monochrome(float *out, const float *const in, dt_iop_roi_t *const roi_out,
                                   const dt_iop_roi_t *const roi_in)
{
  // we never want to access the input out of bounds though:
  assert(roi_in->width >= roi_out->width);
  assert(roi_in->height >= roi_out->height);

#ifdef _OPENMP
#pragma omp parallel for default(none) shared(out) schedule(static)
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

/** 1:1 demosaic from in to out, in is full buf, out is translated/cropped (scale == 1.0!) */
static void demosaic_ppg(float *const out, const float *const in, const dt_iop_roi_t *const roi_out,
                         const dt_iop_roi_t *const roi_in, const uint32_t filters, const float thrs)
{
  // offsets only where the buffer ends:
  const int offx = 3; // MAX(0, 3 - roi_out->x);
  const int offy = 3; // MAX(0, 3 - roi_out->y);
  const int offX = 3; // MAX(0, 3 - (roi_in->width  - (roi_out->x + roi_out->width)));
  const int offY = 3; // MAX(0, 3 - (roi_in->height - (roi_out->y + roi_out->height)));

  // these may differ a little, if you're unlucky enough to split a bayer block with cropping or similar.
  // we never want to access the input out of bounds though:
  assert(roi_in->width >= roi_out->width);
  assert(roi_in->height >= roi_out->height);
  // border interpolate
  float sum[8];
  for(int j = 0; j < roi_out->height; j++)
    for(int i = 0; i < roi_out->width; i++)
    {
      if(i == offx && j >= offy && j < roi_out->height - offY) i = roi_out->width - offX;
      if(i == roi_out->width) break;
      memset(sum, 0, sizeof(float) * 8);
      for(int y = j - 1; y != j + 2; y++)
        for(int x = i - 1; x != i + 2; x++)
        {
          const int yy = y + roi_out->y, xx = x + roi_out->x;
          if(yy >= 0 && xx >= 0 && yy < roi_in->height && xx < roi_in->width)
          {
            int f = FC(y, x, filters);
            sum[f] += in[(size_t)yy * roi_in->width + xx];
            sum[f + 4]++;
          }
        }
      int f = FC(j, i, filters);
      for(int c = 0; c < 3; c++)
      {
        if(c != f && sum[c + 4] > 0.0f)
          out[4 * ((size_t)j * roi_out->width + i) + c] = sum[c] / sum[c + 4];
        else
          out[4 * ((size_t)j * roi_out->width + i) + c]
              = in[((size_t)j + roi_out->y) * roi_in->width + i + roi_out->x];
      }
    }
  const int median = thrs > 0.0f;
  // if(median) fbdd_green(out, in, roi_out, roi_in, filters);
  const float *input = in;
  if(median)
  {
    float *med_in = (float *)dt_alloc_align(16, (size_t)roi_in->height * roi_in->width * sizeof(float));
    pre_median(med_in, in, roi_in, filters, 1, thrs);
    input = med_in;
  }
// for all pixels: interpolate green into float array, or copy color.
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(input) schedule(static)
#endif
  for(int j = offy; j < roi_out->height - offY; j++)
  {
    float *buf = out + (size_t)4 * roi_out->width * j + 4 * offx;
    const float *buf_in = input + (size_t)roi_in->width * (j + roi_out->y) + offx + roi_out->x;
    for(int i = offx; i < roi_out->width - offX; i++)
    {
      const int c = FC(j, i, filters);
#if defined(__SSE__)
      // prefetch what we need soon (load to cpu caches)
      _mm_prefetch((char *)buf_in + 256, _MM_HINT_NTA); // TODO: try HINT_T0-3
      _mm_prefetch((char *)buf_in + roi_in->width + 256, _MM_HINT_NTA);
      _mm_prefetch((char *)buf_in + 2 * roi_in->width + 256, _MM_HINT_NTA);
      _mm_prefetch((char *)buf_in + 3 * roi_in->width + 256, _MM_HINT_NTA);
      _mm_prefetch((char *)buf_in - roi_in->width + 256, _MM_HINT_NTA);
      _mm_prefetch((char *)buf_in - 2 * roi_in->width + 256, _MM_HINT_NTA);
      _mm_prefetch((char *)buf_in - 3 * roi_in->width + 256, _MM_HINT_NTA);
#endif

#if defined(__SSE__)
      __m128 col = _mm_load_ps(buf);
      float *color = (float *)&col;
#else
      float color[4] = { buf[0], buf[1], buf[2], buf[3] };
#endif
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

      // write using MOVNTPS (write combine omitting caches)
      // _mm_stream_ps(buf, col);
      memcpy(buf, color, 4 * sizeof(float));
      buf += 4;
      buf_in++;
    }
  }
// SFENCE (make sure stuff is stored now)
// _mm_sfence();

// for all pixels: interpolate colors into float array
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static)
#endif
  for(int j = 1; j < roi_out->height - 1; j++)
  {
    float *buf = out + (size_t)4 * roi_out->width * j + 4;
    for(int i = 1; i < roi_out->width - 1; i++)
    {
      // also prefetch direct nbs top/bottom
#if defined(__SSE__)
      _mm_prefetch((char *)buf + 256, _MM_HINT_NTA);
      _mm_prefetch((char *)buf - roi_out->width * 4 * sizeof(float) + 256, _MM_HINT_NTA);
      _mm_prefetch((char *)buf + roi_out->width * 4 * sizeof(float) + 256, _MM_HINT_NTA);
#endif

      const int c = FC(j, i, filters);
#if defined(__SSE__)
      __m128 col = _mm_load_ps(buf);
      float *color = (float *)&col;
#else
      float color[4] = { buf[0], buf[1], buf[2], buf[3] };
#endif
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
      // _mm_stream_ps(buf, col);
      memcpy(buf, color, 4 * sizeof(float));
      buf += 4;
    }
  }
  // _mm_sfence();
  if(median) dt_free_align((float *)input);
}

void modify_roi_out(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, dt_iop_roi_t *roi_out,
                    const dt_iop_roi_t *const roi_in)
{
  *roi_out = *roi_in;

  // snap to start of mosaic block:
  roi_out->x = 0; // MAX(0, roi_out->x & ~1);
  roi_out->y = 0; // MAX(0, roi_out->y & ~1);
}

// which roi input is needed to process to this output?
// roi_out is unchanged, full buffer in is full buffer out.
void modify_roi_in(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                   const dt_iop_roi_t *roi_out, dt_iop_roi_t *roi_in)
{
  // this op is disabled for preview pipe/filters == 0

  *roi_in = *roi_out;
  // need 1:1, demosaic and then sub-sample. or directly sample half-size
  roi_in->x /= roi_out->scale;
  roi_in->y /= roi_out->scale;
  roi_in->width /= roi_out->scale;
  roi_in->height /= roi_out->scale;
  roi_in->scale = 1.0f;
  // clamp to even x/y, to make demosaic pattern still hold..
  if(piece->pipe->filters != 9u)
  {
    roi_in->x = MAX(0, roi_in->x & ~1);
    roi_in->y = MAX(0, roi_in->y & ~1);
  }
  else
  {
    // Markesteijn needs factors of 3
    roi_in->x = MAX(0, roi_in->x - (roi_in->x % 3));
    roi_in->y = MAX(0, roi_in->y - (roi_in->y % 3));
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
  gchar *quality = dt_conf_get_string("plugins/darkroom/demosaic/quality");
  if(quality)
  {
    if(!strcmp(quality, "always bilinear (fast)"))
      qual = 0;
    else if(!strcmp(quality, "full (possibly slow)"))
      qual = 2;
    g_free(quality);
  }
  return qual;
}

static int get_thumb_quality(int width, int height)
{
  // we check if we need ultra-high quality thumbnail for this size
  char *min = dt_conf_get_string("plugins/lighttable/thumbnail_hq_min_level");

  int level = dt_mipmap_cache_get_matching_size(darktable.mipmap_cache, width, height);
  int res = 0;
  if (strcmp(min, "always")==0) res = 1;
  else if (strcmp(min, "small")==0) res = ( level >= 1 );
  else if (strcmp(min, "VGA")==0) res = ( level >= 2 );
  else if (strcmp(min, "720p")==0) res = ( level >= 3 );
  else if (strcmp(min, "1080p")==0) res = ( level >= 4 );
  else if (strcmp(min, "WQXGA")==0) res = ( level >= 5 );
  else if (strcmp(min, "4k")==0) res = ( level >= 6 );
  else if (strcmp(min, "5K")==0) res = ( level >= 7 );

  g_free(min);
  return res;
}

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const i, void *const o,
             const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_image_t *img = &self->dev->image_storage;
  const float threshold = 0.0001f * img->exif_iso;

  dt_iop_roi_t roi = *roi_in;
  dt_iop_roi_t roo = *roi_out;
  roo.x = roo.y = 0;
  // roi_out->scale = global scale: (iscale == 1.0, always when demosaic is on)

  const uint8_t(*const xtrans)[6] = (const uint8_t(*const)[6])piece->pipe->xtrans;

  dt_iop_demosaic_data_t *data = (dt_iop_demosaic_data_t *)piece->data;

  const int qual = get_quality();
  int demosaicing_method = data->demosaicing_method;
  if(piece->pipe->type == DT_DEV_PIXELPIPE_FULL && qual < 2 && roi_out->scale <= .99999f
     && // only overwrite setting if quality << requested and in dr mode
     ((piece->pipe->filters != 9u) && (demosaicing_method != DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME)))
    demosaicing_method = (piece->pipe->filters != 9u) ? DT_IOP_DEMOSAIC_PPG : DT_IOP_DEMOSAIC_MARKESTEIJN;

  // we check if we need ultra-high quality thumbnail for this size
  int uhq_thumb = 0;
  if (piece->pipe->type == DT_DEV_PIXELPIPE_THUMBNAIL)
    uhq_thumb = get_thumb_quality(roi_out->width, roi_out->height);

  // we check if we can avoid full scale demosaicing and chose simple
  // half scale or third scale interpolation instead
  const int full_scale_demosaicing
      = (piece->pipe->type == DT_DEV_PIXELPIPE_FULL && qual > 0) || piece->pipe->type == DT_DEV_PIXELPIPE_EXPORT
        || uhq_thumb || roi_out->scale > (piece->pipe->filters == 9u ? 0.333f : 0.5f)
        || (img->flags & DT_IMAGE_4BAYER); // half_size_f doesn't support 4bayer images

  // we check if we can stop at the linear interpolation step in VNG
  // instead of going the full way
  const int only_vng_linear
      = full_scale_demosaicing && roi_out->scale < (piece->pipe->filters == 9u ? 0.5f : 0.667f);

  // we use full Markesteijn demosaicing on xtrans sensors only if
  // maximum quality is required
  const int xtrans_full_markesteijn_demosaicing =
      (piece->pipe->type == DT_DEV_PIXELPIPE_FULL && qual > 1) ||
      piece->pipe->type == DT_DEV_PIXELPIPE_EXPORT ||
      uhq_thumb ||
      roi_out->scale > 0.667f;

  const float *const pixels = (float *)i;

  if(full_scale_demosaicing)
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
      tmp = (float *)dt_alloc_align(16, (size_t)roo.width * roo.height * 4 * sizeof(float));
    }

    if(piece->pipe->filters == 9u)
    {
      if(demosaicing_method >= DT_IOP_DEMOSAIC_MARKESTEIJN && xtrans_full_markesteijn_demosaicing)
        xtrans_markesteijn_interpolate(tmp, pixels, &roo, &roi, xtrans,
                                       1 + (demosaicing_method - DT_IOP_DEMOSAIC_MARKESTEIJN) * 2);
      else
        vng_interpolate(tmp, pixels, &roo, &roi, piece->pipe->filters, xtrans,
                        only_vng_linear);
    }
    else
    {
      float *in = (float *)pixels;

      if(!(img->flags & DT_IMAGE_4BAYER) && data->green_eq != DT_IOP_GREEN_EQ_NO)
      {
        in = (float *)dt_alloc_align(16, (size_t)roi_in->height * roi_in->width * sizeof(float));
        switch(data->green_eq)
        {
          case DT_IOP_GREEN_EQ_FULL:
            green_equilibration_favg(in, pixels, roi_in->width, roi_in->height, piece->pipe->filters, roi_in->x,
                                     roi_in->y);
            break;
          case DT_IOP_GREEN_EQ_LOCAL:
            green_equilibration_lavg(in, pixels, roi_in->width, roi_in->height, piece->pipe->filters, roi_in->x,
                                     roi_in->y, 0, threshold);
            break;
          case DT_IOP_GREEN_EQ_BOTH:
            green_equilibration_favg(in, pixels, roi_in->width, roi_in->height, piece->pipe->filters, roi_in->x,
                                     roi_in->y);
            green_equilibration_lavg(in, in, roi_in->width, roi_in->height, piece->pipe->filters, roi_in->x,
                                     roi_in->y, 1, threshold);
            break;
        }
      }

      if(demosaicing_method == DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME)
        passthrough_monochrome(tmp, in, &roo, &roi);
      else if(demosaicing_method == DT_IOP_DEMOSAIC_VNG4 || (img->flags & DT_IMAGE_4BAYER))
      {
        vng_interpolate(tmp, in, &roo, &roi, piece->pipe->filters, xtrans,
                        only_vng_linear);
        if (img->flags & DT_IMAGE_4BAYER)
        {
          dt_colorspaces_cygm_to_rgb(tmp, roo.width*roo.height, data->CAM_to_RGB);
          dt_colorspaces_cygm_to_rgb(piece->pipe->processed_maximum, 1, data->CAM_to_RGB);
        }
      }
      else if(demosaicing_method != DT_IOP_DEMOSAIC_AMAZE)
        demosaic_ppg(tmp, in, &roo, &roi, piece->pipe->filters,
                     data->median_thrs); // wanted ppg or zoomed out a lot and quality is limited to 1
      else
        amaze_demosaic_RT(self, piece, in, tmp, &roi, &roo, piece->pipe->filters);

      if(!(img->flags & DT_IMAGE_4BAYER) && data->green_eq != DT_IOP_GREEN_EQ_NO) dt_free_align(in);
    }

    if(scaled)
    {
      roi = *roi_out;
      dt_iop_clip_and_zoom_roi((float *)o, tmp, &roi, &roo, roi.width, roo.width);
      dt_free_align(tmp);
    }
  }
  else
  {
    // sample half-size raw (Bayer) or 1/3-size raw (X-Trans)
    const float clip = fminf(piece->pipe->processed_maximum[0],
                             fminf(piece->pipe->processed_maximum[1], piece->pipe->processed_maximum[2]));
    if(piece->pipe->filters == 9u)
      dt_iop_clip_and_zoom_demosaic_third_size_xtrans_f((float *)o, pixels, &roo, &roi, roo.width, roi.width,
                                                        xtrans);
    else
    {
      if(demosaicing_method == DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME)
        dt_iop_clip_and_zoom_demosaic_passthrough_monochrome_f((float *)o, pixels, &roo, &roi, roo.width,
                                                               roi.width);
      else
        dt_iop_clip_and_zoom_demosaic_half_size_f((float *)o, pixels, &roo, &roi, roo.width, roi.width,
                                                  piece->pipe->filters, clip);
    }
  }
  if(data->color_smoothing) color_smoothing(o, roi_out, data->color_smoothing);
}

#ifdef HAVE_OPENCL
typedef struct dt_iop_demosaic_cl_buffer_t
{
  // description of memory requirements of local buffer
  // local buffer size will be calculated as:
  // (xoffset + xfactor * x) * (yoffset + yfactor * y) * cellsize + overhead;
  int xoffset;
  int xfactor;
  int yoffset;
  int yfactor;
  size_t cellsize;
  size_t overhead;
} dt_iop_demosaic_cl_buffer_t;


static int
nextpow2(int n)
{
  int k = 1;
  while (k < n)
    k <<= 1;
  return k;
}

// utility function to calculate optimal work group dimensions for a given kernel
// taking device specific restrictions and local memory limitations into account
static int
blocksizeopt(const int devid, const int kernel, int *blocksizex, int *blocksizey,
             const dt_iop_demosaic_cl_buffer_t *factors)
{
   size_t maxsizes[3] = { 0 };     // the maximum dimensions for a work group
   size_t workgroupsize = 0;       // the maximum number of items in a work group
   unsigned long localmemsize = 0; // the maximum amount of local memory we can use
   size_t kernelworkgroupsize = 0; // the maximum amount of items in work group for this kernel

   // first starting values must be supplied in *blocksizex and *blocksizey.
   // we make sure that these are a power of 2 and lie within reasonable limits.
   // note: *blocksizex and *blocksizey may point to the same location in memory
   *blocksizex = CLAMP(nextpow2(*blocksizex), 2, 2 << 8);
   *blocksizey = CLAMP(nextpow2(*blocksizey), 2, 2 << 8);

   if(dt_opencl_get_work_group_limits(devid, maxsizes, &workgroupsize, &localmemsize) == CL_SUCCESS
      && dt_opencl_get_kernel_work_group_size(devid, kernel, &kernelworkgroupsize) == CL_SUCCESS)
      {
        while(maxsizes[0] < *blocksizex || maxsizes[1] < *blocksizey
              || localmemsize < ((factors->xfactor * (*blocksizex) + factors->xoffset) *
                                (factors->yfactor * (*blocksizey) + factors->yoffset)) * factors->cellsize + factors->overhead
              || workgroupsize < (*blocksizex) * (*blocksizey) || kernelworkgroupsize < (*blocksizex) * (*blocksizey))
        {
          if(*blocksizex == 1 || *blocksizey == 1) break;

          if(*blocksizex > *blocksizey)
            *blocksizex >>= 1;
          else
            *blocksizey >>= 1;
        }
      }
      else
      {
        dt_print(DT_DEBUG_OPENCL,
             "[opencl_demosaic] can not identify resource limits for device %d\n", devid);
        return FALSE;
      }

  return TRUE;
}

// color smoothing step by multiple passes of median filtering
static int color_smoothing_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in,
                              cl_mem dev_out, const dt_iop_roi_t *const roi_out)
{
  dt_iop_demosaic_data_t *data = (dt_iop_demosaic_data_t *)piece->data;
  dt_iop_demosaic_global_data_t *gd = (dt_iop_demosaic_global_data_t *)self->data;

  const int devid = piece->pipe->devid;
  const int width = roi_out->width;
  const int height = roi_out->height;

  cl_int err = -999;

  cl_mem dev_tmp = dt_opencl_alloc_device(devid, width, height, 4 * sizeof(float));
  if(dev_tmp == NULL) goto error;

  dt_iop_demosaic_cl_buffer_t factors;
  int blocksizex;
  int blocksizey;

  factors.xoffset = 2*1;
  factors.xfactor = 1;
  factors.yoffset = 2*1;
  factors.yfactor = 1;
  factors.cellsize = 4 * sizeof(float);
  factors.overhead = 0;
  blocksizex = 64;
  blocksizey = 64;
  if(!blocksizeopt(devid, gd->kernel_color_smoothing, &blocksizex, &blocksizey, &factors))
    goto error;

  // speed optimized limits for my NVIDIA GTS450
  // TODO: find out if this is good for other systems as well
  blocksizex = blocksizex > 16 ? 16 : blocksizex;
  blocksizey = blocksizey > 8 ? 8 : blocksizey;

  size_t sizes[] = { ROUNDUP(width, blocksizex), ROUNDUP(height, blocksizey), 1 };
  size_t local[] = { blocksizex, blocksizey, 1 };
  size_t origin[] = { 0, 0, 0 };
  size_t region[] = { width, height, 1 };

  // two buffer references for our ping-pong
  cl_mem dev_t1 = dev_out;
  cl_mem dev_t2 = dev_tmp;

  for(int pass = 0; pass < data->color_smoothing; pass++)
  {

    dt_opencl_set_kernel_arg(devid, gd->kernel_color_smoothing, 0, sizeof(cl_mem), &dev_t1);
    dt_opencl_set_kernel_arg(devid, gd->kernel_color_smoothing, 1, sizeof(cl_mem), &dev_t2);
    dt_opencl_set_kernel_arg(devid, gd->kernel_color_smoothing, 2, sizeof(int), &width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_color_smoothing, 3, sizeof(int), &height);
    dt_opencl_set_kernel_arg(devid, gd->kernel_color_smoothing, 4,
                               (blocksizex + 2) * (blocksizey + 2) * 4 * sizeof(float), NULL);
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
    err = dt_opencl_enqueue_copy_image(devid, dev_tmp, dev_out, origin, origin, region);
    if(err != CL_SUCCESS) goto error;
  }

  if(dev_tmp != NULL) dt_opencl_release_mem_object(dev_tmp);
  return TRUE;

error:
  if(dev_tmp != NULL) dt_opencl_release_mem_object(dev_tmp);
  dt_print(DT_DEBUG_OPENCL, "[opencl_demosaic_color_smoothing] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}

static int process_default_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in,
                              cl_mem dev_out, const dt_iop_roi_t *const roi_in,
                              const dt_iop_roi_t *const roi_out)
{
  dt_iop_demosaic_data_t *data = (dt_iop_demosaic_data_t *)piece->data;
  dt_iop_demosaic_global_data_t *gd = (dt_iop_demosaic_global_data_t *)self->data;
  const dt_image_t *img = &self->dev->image_storage;

  const float threshold = 0.0001f * img->exif_iso;
  const int devid = piece->pipe->devid;
  const int qual = get_quality();
  const int demosaicing_method = data->demosaicing_method;

  // we check if we need ultra-high quality thumbnail for this size
  int uhq_thumb = 0;
  if (piece->pipe->type == DT_DEV_PIXELPIPE_THUMBNAIL)
    uhq_thumb = get_thumb_quality(roi_out->width, roi_out->height);

  cl_mem dev_tmp = NULL;
  cl_mem dev_green_eq = NULL;
  cl_int err = -999;

  if((piece->pipe->type == DT_DEV_PIXELPIPE_FULL && qual > 0) ||
      piece->pipe->type == DT_DEV_PIXELPIPE_EXPORT || (uhq_thumb) ||
      roi_out->scale > (piece->pipe->filters == 9u ? 0.333f : .5f))
  {
    // Full demosaic and then scaling if needed
    const int scaled = (roi_out->width != roi_in->width || roi_out->height != roi_in->height);

    int width = roi_out->width;
    int height = roi_out->height;
    dev_tmp = dev_out;

    if(scaled)
    {
      // need to scale to right res
      dev_tmp = dt_opencl_alloc_device(devid, roi_in->width, roi_in->height, 4 * sizeof(float));
      if(dev_tmp == NULL) goto error;
      width = roi_in->width;
      height = roi_in->height;
    }

    size_t sizes[2] = { ROUNDUPWD(width), ROUNDUPHT(height) };

    // 1:1 demosaic
    dev_green_eq = NULL;
    if(data->green_eq != DT_IOP_GREEN_EQ_NO)
    {
      // green equilibration
      dev_green_eq = dt_opencl_alloc_device(devid, roi_in->width, roi_in->height, sizeof(float));
      if(dev_green_eq == NULL) goto error;
      dt_opencl_set_kernel_arg(devid, gd->kernel_green_eq, 0, sizeof(cl_mem), &dev_in);
      dt_opencl_set_kernel_arg(devid, gd->kernel_green_eq, 1, sizeof(cl_mem), &dev_green_eq);
      dt_opencl_set_kernel_arg(devid, gd->kernel_green_eq, 2, sizeof(int), &width);
      dt_opencl_set_kernel_arg(devid, gd->kernel_green_eq, 3, sizeof(int), &height);
      dt_opencl_set_kernel_arg(devid, gd->kernel_green_eq, 4, sizeof(uint32_t), (void *)&piece->pipe->filters);
      dt_opencl_set_kernel_arg(devid, gd->kernel_green_eq, 5, sizeof(float), (void *)&threshold);
      err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_green_eq, sizes);
      if(err != CL_SUCCESS) goto error;
      dev_in = dev_green_eq;
    }

    if(demosaicing_method == DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME)
    {
      dt_opencl_set_kernel_arg(devid, gd->kernel_passthrough_monochrome, 0, sizeof(cl_mem), &dev_in);
      dt_opencl_set_kernel_arg(devid, gd->kernel_passthrough_monochrome, 1, sizeof(cl_mem), &dev_tmp);
      dt_opencl_set_kernel_arg(devid, gd->kernel_passthrough_monochrome, 2, sizeof(int), &width);
      dt_opencl_set_kernel_arg(devid, gd->kernel_passthrough_monochrome, 3, sizeof(int), &height);
      err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_passthrough_monochrome, sizes);
      if(err != CL_SUCCESS) goto error;
    }
    else if(demosaicing_method == DT_IOP_DEMOSAIC_PPG)
    {
      if(data->median_thrs > 0.0f)
      {
        const int one = 1;
        dt_opencl_set_kernel_arg(devid, gd->kernel_pre_median, 0, sizeof(cl_mem), &dev_in);
        dt_opencl_set_kernel_arg(devid, gd->kernel_pre_median, 1, sizeof(cl_mem), &dev_tmp);
        dt_opencl_set_kernel_arg(devid, gd->kernel_pre_median, 2, sizeof(int), &width);
        dt_opencl_set_kernel_arg(devid, gd->kernel_pre_median, 3, sizeof(int), &height);
        dt_opencl_set_kernel_arg(devid, gd->kernel_pre_median, 4, sizeof(uint32_t), (void *)&piece->pipe->filters);
        dt_opencl_set_kernel_arg(devid, gd->kernel_pre_median, 5, sizeof(float), (void *)&data->median_thrs);
        dt_opencl_set_kernel_arg(devid, gd->kernel_pre_median, 6, sizeof(int), (void *)&one);
        err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_pre_median, sizes);
        if(err != CL_SUCCESS) goto error;

        dt_opencl_set_kernel_arg(devid, gd->kernel_ppg_green_median, 0, sizeof(cl_mem), &dev_tmp);
        dt_opencl_set_kernel_arg(devid, gd->kernel_ppg_green_median, 1, sizeof(cl_mem), &dev_tmp);
        dt_opencl_set_kernel_arg(devid, gd->kernel_ppg_green_median, 2, sizeof(int), &width);
        dt_opencl_set_kernel_arg(devid, gd->kernel_ppg_green_median, 3, sizeof(int), &height);
        dt_opencl_set_kernel_arg(devid, gd->kernel_ppg_green_median, 4, sizeof(uint32_t),
                                 (void *)&piece->pipe->filters);
        err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_ppg_green_median, sizes);
        if(err != CL_SUCCESS) goto error;
      }
      else
      {
        dt_opencl_set_kernel_arg(devid, gd->kernel_ppg_green, 0, sizeof(cl_mem), &dev_in);
        dt_opencl_set_kernel_arg(devid, gd->kernel_ppg_green, 1, sizeof(cl_mem), &dev_tmp);
        dt_opencl_set_kernel_arg(devid, gd->kernel_ppg_green, 2, sizeof(int), &width);
        dt_opencl_set_kernel_arg(devid, gd->kernel_ppg_green, 3, sizeof(int), &height);
        dt_opencl_set_kernel_arg(devid, gd->kernel_ppg_green, 4, sizeof(uint32_t), (void *)&piece->pipe->filters);
        err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_ppg_green, sizes);
        if(err != CL_SUCCESS) goto error;
      }

      dt_opencl_set_kernel_arg(devid, gd->kernel_ppg_redblue, 0, sizeof(cl_mem), &dev_tmp);
      dt_opencl_set_kernel_arg(devid, gd->kernel_ppg_redblue, 1, sizeof(cl_mem), &dev_tmp);
      dt_opencl_set_kernel_arg(devid, gd->kernel_ppg_redblue, 2, sizeof(int), &width);
      dt_opencl_set_kernel_arg(devid, gd->kernel_ppg_redblue, 3, sizeof(int), &height);
      dt_opencl_set_kernel_arg(devid, gd->kernel_ppg_redblue, 4, sizeof(uint32_t), (void *)&piece->pipe->filters);
      err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_ppg_redblue, sizes);
      if(err != CL_SUCCESS) goto error;

      // manage borders
      dt_opencl_set_kernel_arg(devid, gd->kernel_border_interpolate, 0, sizeof(cl_mem), &dev_in);
      dt_opencl_set_kernel_arg(devid, gd->kernel_border_interpolate, 1, sizeof(cl_mem), &dev_tmp);
      dt_opencl_set_kernel_arg(devid, gd->kernel_border_interpolate, 2, sizeof(int), (void *)&width);
      dt_opencl_set_kernel_arg(devid, gd->kernel_border_interpolate, 3, sizeof(int), (void *)&height);
      dt_opencl_set_kernel_arg(devid, gd->kernel_border_interpolate, 4, sizeof(uint32_t),
                               (void *)&piece->pipe->filters);
      err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_border_interpolate, sizes);
      if(err != CL_SUCCESS) goto error;
    }

    if(scaled)
    {
      // scale temp buffer to output buffer
      err = dt_iop_clip_and_zoom_roi_cl(devid, dev_out, dev_tmp, roi_out, roi_in);
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
      size_t sizes[2] = { ROUNDUPWD(width), ROUNDUPHT(height) };

      dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_passthrough_monochrome, 0, sizeof(cl_mem), &dev_pix);
      dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_passthrough_monochrome, 1, sizeof(cl_mem), &dev_out);
      dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_passthrough_monochrome, 2, sizeof(int), &width);
      dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_passthrough_monochrome, 3, sizeof(int), &height);
      dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_passthrough_monochrome, 4, sizeof(int), (void *)&zero);
      dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_passthrough_monochrome, 5, sizeof(int), (void *)&zero);
      dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_passthrough_monochrome, 6, sizeof(int),
                               (void *)&roi_in->width);
      dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_passthrough_monochrome, 7, sizeof(int),
                               (void *)&roi_in->height);
      dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_passthrough_monochrome, 8, sizeof(float),
                               (void *)&roi_out->scale);
      dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_passthrough_monochrome, 9, sizeof(uint32_t),
                               (void *)&piece->pipe->filters);
      err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_zoom_passthrough_monochrome, sizes);
      if(err != CL_SUCCESS) goto error;
    }
    else
    {
      // sample half-size image:
      const int zero = 0;
      cl_mem dev_pix = dev_in;
      const int width = roi_out->width;
      const int height = roi_out->height;
      size_t sizes[2] = { ROUNDUPWD(width), ROUNDUPHT(height) };

      dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_half_size, 0, sizeof(cl_mem), &dev_pix);
      dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_half_size, 1, sizeof(cl_mem), &dev_out);
      dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_half_size, 2, sizeof(int), &width);
      dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_half_size, 3, sizeof(int), &height);
      dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_half_size, 4, sizeof(int), (void *)&zero);
      dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_half_size, 5, sizeof(int), (void *)&zero);
      dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_half_size, 6, sizeof(int), (void *)&roi_in->width);
      dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_half_size, 7, sizeof(int), (void *)&roi_in->height);
      dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_half_size, 8, sizeof(float), (void *)&roi_out->scale);
      dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_half_size, 9, sizeof(uint32_t),
                               (void *)&piece->pipe->filters);
      err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_zoom_half_size, sizes);
      if(err != CL_SUCCESS) goto error;
    }
  }

  if(dev_tmp != NULL && dev_tmp != dev_out) dt_opencl_release_mem_object(dev_tmp);
  if(dev_green_eq != NULL) dt_opencl_release_mem_object(dev_green_eq);
  dev_tmp = dev_green_eq = NULL;

  // color smoothing
  if(data->color_smoothing)
  {
    if(!color_smoothing_cl(self, piece, dev_out, dev_out, roi_out))
      goto error;
  }

  return TRUE;

error:
  if(dev_tmp != NULL && dev_tmp != dev_out) dt_opencl_release_mem_object(dev_tmp);
  if(dev_green_eq != NULL) dt_opencl_release_mem_object(dev_green_eq);
  dt_print(DT_DEBUG_OPENCL, "[opencl_demosaic] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}

static int process_vng_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in,
                          cl_mem dev_out, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_demosaic_data_t *data = (dt_iop_demosaic_data_t *)piece->data;
  dt_iop_demosaic_global_data_t *gd = (dt_iop_demosaic_global_data_t *)self->data;

  const dt_image_t *img = &self->dev->image_storage;
  const float threshold = 0.0001f * img->exif_iso;

  const uint8_t(*const xtrans)[6] = (const uint8_t(*const)[6])piece->pipe->xtrans;

  // separate out G1 and G2 in Bayer patterns
  uint32_t filters4;
  if(piece->pipe->filters == 9u)
    filters4 = piece->pipe->filters;
  else if((piece->pipe->filters & 3) == 1)
    filters4 = piece->pipe->filters | 0x03030303u;
  else
    filters4 = piece->pipe->filters | 0x0c0c0c0cu;

  const int size = (filters4 == 9u) ? 6 : 16;
  const int colors = (filters4 == 9u) ? 3 : 4;
  const int prow = (filters4 == 9u) ? 6 : 8;
  const int pcol = (filters4 == 9u) ? 6 : 2;
  const int devid = piece->pipe->devid;
  const int qual = get_quality();

  const float processed_maximum[4] = { piece->pipe->processed_maximum[0],
                                       piece->pipe->processed_maximum[1],
                                       piece->pipe->processed_maximum[2],
                                       1.0f };

  // we check if we need ultra-high quality thumbnail for this size
  int uhq_thumb = 0;
  if (piece->pipe->type == DT_DEV_PIXELPIPE_THUMBNAIL)
    uhq_thumb = get_thumb_quality(roi_out->width, roi_out->height);

  // check if we can avoid full scale demosaicing and chose simple
  // half scale or third scale interpolation instead
  const int full_scale_demosaicing = (piece->pipe->type == DT_DEV_PIXELPIPE_FULL && qual > 0)
                                     || piece->pipe->type == DT_DEV_PIXELPIPE_EXPORT || uhq_thumb
                                     || roi_out->scale > (piece->pipe->filters == 9u ? 0.333f : 0.5f);

  // check if we can stop at the linear interpolation step and
  // avoid full VNG
  const int only_vng_linear
      = full_scale_demosaicing && roi_out->scale < (piece->pipe->filters == 9u ? 0.5f : 0.667f);

  cl_mem dev_tmp1 = NULL;
  cl_mem dev_tmp2 = NULL;
  cl_mem dev_xtrans = NULL;
  cl_mem dev_lookup = NULL;
  cl_mem dev_code = NULL;
  cl_mem dev_ips = NULL;
  cl_mem dev_green_eq = NULL;
  cl_int err = -999;

  if(piece->pipe->filters == 9u)
  {
    dev_xtrans = dt_opencl_copy_host_to_device_constant(devid, sizeof(piece->pipe->xtrans), piece->pipe->xtrans);
    if(dev_xtrans == NULL) goto error;
  }

  if(full_scale_demosaicing)
  {
    // Full demosaic and then scaling if needed
    const int scaled = (roi_out->width != roi_in->width || roi_out->height != roi_in->height);

    int width = roi_out->width;
    int height = roi_out->height;

    size_t sizes[3];
    size_t local[3];
    dt_iop_demosaic_cl_buffer_t factors;
    int blocksizex;
    int blocksizey;

    dev_tmp1 = dt_opencl_alloc_device(devid, roi_in->width, roi_in->height, 4 * sizeof(float));
    if(dev_tmp1 == NULL) goto error;

    dev_tmp2 = dev_out;

    if(scaled)
    {
      // need to scale to right res
      dev_tmp2 = dt_opencl_alloc_device(devid, roi_in->width, roi_in->height, 4 * sizeof(float));
      if(dev_tmp2 == NULL) goto error;
      width = roi_in->width;
      height = roi_in->height;
    }

    // build interpolation lookup table for linear interpolation which for a given offset in the sensor
    // lists neighboring pixels from which to interpolate:
    // NUM_PIXELS                 # of neighboring pixels to read
    // for (1..NUM_PIXELS):
    //   OFFSET                   # in bytes from current pixel
    //   WEIGHT                   # how much weight to give this neighbor
    //   COLOR                    # sensor color
    // # weights of adjoining pixels not of this pixel's color
    // COLORA TOT_WEIGHT
    // COLORB TOT_WEIGHT
    // COLORPIX                   # color of center pixel
    int32_t lookup[16][16][32];

    for(int row = 0; row < size; row++)
      for(int col = 0; col < size; col++)
      {
        int32_t *ip = lookup[row][col] + 1;
        int sum[4] = { 0 };
        const int f = fcol(row + roi_in->y, col + roi_in->x, filters4, xtrans);
        // make list of adjoining pixel offsets by weight & color
        for(int y = -1; y <= 1; y++)
          for(int x = -1; x <= 1; x++)
          {
            int weight = 1 << ((y == 0) + (x == 0));
            const int color = fcol(row + y + roi_in->y, col + x + roi_in->x, filters4, xtrans);
            if(color == f) continue;
            *ip++ = (y << 16) | (x & 0xffffu);
            *ip++ = weight;
            *ip++ = color;
            sum[color] += weight;
          }
        lookup[row][col][0] = (ip - lookup[row][col]) / 3; /* # of neighboring pixels found */
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
    int ips[prow * pcol * 352];
    int *ip = ips;
    int code[16][16];

    for(int row = 0; row < prow; row++)
      for(int col = 0; col < pcol; col++)
      {
        code[row][col] = ip - ips;
        const signed char *cp = terms;
        for(int t = 0; t < 64; t++)
        {
          int y1 = *cp++, x1 = *cp++;
          int y2 = *cp++, x2 = *cp++;
          int weight = *cp++;
          int grads = *cp++;
          int color = fcol(row + y1, col + x1, filters4, xtrans);
          if(fcol(row + y2, col + x2, filters4, xtrans) != color) continue;
          int diag
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
          int y = *cp++, x = *cp++;
          *ip++ = (y << 16) | (x & 0xffffu);
          int color = fcol(row, col, filters4, xtrans);
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


    dev_lookup = dt_opencl_copy_host_to_device_constant(devid, sizeof(lookup), lookup);
    if(dev_lookup == NULL) goto error;

    dev_code = dt_opencl_copy_host_to_device_constant(devid, sizeof(code), code);
    if(dev_code == NULL) goto error;

    dev_ips = dt_opencl_copy_host_to_device_constant(devid, sizeof(ips), ips);
    if(dev_ips == NULL) goto error;

    if(piece->pipe->filters != 9u && data->green_eq != DT_IOP_GREEN_EQ_NO)
    {
      // green equilibration for Bayer sensors
      dev_green_eq = dt_opencl_alloc_device(devid, roi_in->width, roi_in->height, sizeof(float));
      if(dev_green_eq == NULL) goto error;

      sizes[0] = ROUNDUPWD(width);
      sizes[1] = ROUNDUPHT(height);
      sizes[2] = 1;
      dt_opencl_set_kernel_arg(devid, gd->kernel_green_eq, 0, sizeof(cl_mem), (void *)&dev_in);
      dt_opencl_set_kernel_arg(devid, gd->kernel_green_eq, 1, sizeof(cl_mem), (void *)&dev_green_eq);
      dt_opencl_set_kernel_arg(devid, gd->kernel_green_eq, 2, sizeof(int), (void *)&width);
      dt_opencl_set_kernel_arg(devid, gd->kernel_green_eq, 3, sizeof(int), (void *)&height);
      dt_opencl_set_kernel_arg(devid, gd->kernel_green_eq, 4, sizeof(uint32_t), (void *)&piece->pipe->filters);
      dt_opencl_set_kernel_arg(devid, gd->kernel_green_eq, 5, sizeof(float), (void *)&threshold);
      err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_green_eq, sizes);
      if(err != CL_SUCCESS) goto error;
      dev_in = dev_green_eq;
    }

    // do linear interpolation
    factors.xoffset = 2*1;
    factors.xfactor = 1;
    factors.yoffset = 2*1;
    factors.yfactor = 1;
    factors.cellsize = 1 * sizeof(float);
    factors.overhead = 0;
    blocksizex = 64;
    blocksizey = 64;
    if(!blocksizeopt(devid, gd->kernel_vng_lin_interpolate, &blocksizex, &blocksizey, &factors))
      goto error;

    sizes[0] = ROUNDUP(width, blocksizex);
    sizes[1] = ROUNDUP(height, blocksizey);
    sizes[2] = 1;
    local[0] = blocksizex;
    local[1] = blocksizey;
    local[2] = 1;
    dt_opencl_set_kernel_arg(devid, gd->kernel_vng_lin_interpolate, 0, sizeof(cl_mem), (void *)&dev_in);
    dt_opencl_set_kernel_arg(devid, gd->kernel_vng_lin_interpolate, 1, sizeof(cl_mem), (void *)&dev_tmp1);
    dt_opencl_set_kernel_arg(devid, gd->kernel_vng_lin_interpolate, 2, sizeof(int), (void *)&width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_vng_lin_interpolate, 3, sizeof(int), (void *)&height);
    dt_opencl_set_kernel_arg(devid, gd->kernel_vng_lin_interpolate, 4, sizeof(uint32_t), (void *)&filters4);
    dt_opencl_set_kernel_arg(devid, gd->kernel_vng_lin_interpolate, 5, sizeof(cl_mem), (void *)&dev_lookup);
    dt_opencl_set_kernel_arg(devid, gd->kernel_vng_lin_interpolate, 6,
                             (blocksizex + 2) * (blocksizey + 2) * sizeof(float), NULL);
    err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_vng_lin_interpolate, sizes, local);
    if(err != CL_SUCCESS) goto error;


    if(only_vng_linear)
    {
      // leave it at linear interpolation and skip VNG
      size_t origin[] = { 0, 0, 0 };
      size_t region[] = { width, height, 1 };
      err = dt_opencl_enqueue_copy_image(devid, dev_tmp1, dev_tmp2, origin, origin, region);
      if(err != CL_SUCCESS) goto error;
    }
    else
    {
      // do full VNG interpolation
      factors.xoffset = 2*2;
      factors.xfactor = 1;
      factors.yoffset = 2*2;
      factors.yfactor = 1;
      factors.cellsize = 4 * sizeof(float);
      factors.overhead = 0;
      blocksizex = 64;
      blocksizey = 64;
      if(!blocksizeopt(devid, gd->kernel_vng_interpolate, &blocksizex, &blocksizey, &factors))
        goto error;

      sizes[0] = ROUNDUP(width, blocksizex);
      sizes[1] = ROUNDUP(height, blocksizey);
      sizes[2] = 1;
      local[0] = blocksizex;
      local[1] = blocksizey;
      local[2] = 1;
      dt_opencl_set_kernel_arg(devid, gd->kernel_vng_interpolate, 0, sizeof(cl_mem), (void *)&dev_tmp1);
      dt_opencl_set_kernel_arg(devid, gd->kernel_vng_interpolate, 1, sizeof(cl_mem), (void *)&dev_tmp2);
      dt_opencl_set_kernel_arg(devid, gd->kernel_vng_interpolate, 2, sizeof(int), (void *)&width);
      dt_opencl_set_kernel_arg(devid, gd->kernel_vng_interpolate, 3, sizeof(int), (void *)&height);
      dt_opencl_set_kernel_arg(devid, gd->kernel_vng_interpolate, 4, sizeof(int), (void *)&roi_in->x);
      dt_opencl_set_kernel_arg(devid, gd->kernel_vng_interpolate, 5, sizeof(int), (void *)&roi_in->y);
      dt_opencl_set_kernel_arg(devid, gd->kernel_vng_interpolate, 6, sizeof(uint32_t), (void *)&filters4);
      dt_opencl_set_kernel_arg(devid, gd->kernel_vng_interpolate, 7, 4*sizeof(float), (void *)processed_maximum);
      dt_opencl_set_kernel_arg(devid, gd->kernel_vng_interpolate, 8, sizeof(cl_mem), (void *)&dev_xtrans);
      dt_opencl_set_kernel_arg(devid, gd->kernel_vng_interpolate, 9, sizeof(cl_mem), (void *)&dev_ips);
      dt_opencl_set_kernel_arg(devid, gd->kernel_vng_interpolate, 10, sizeof(cl_mem), (void *)&dev_code);
      dt_opencl_set_kernel_arg(devid, gd->kernel_vng_interpolate, 11, (blocksizex + 4) * (blocksizey + 4) * 4 * sizeof(float), NULL);
      err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_vng_interpolate, sizes, local);
      if(err != CL_SUCCESS) goto error;
    }

    // manage borders
    sizes[0] = ROUNDUPWD(width);
    sizes[1] = ROUNDUPHT(height);
    sizes[2] = 1;
    dt_opencl_set_kernel_arg(devid, gd->kernel_vng_border_interpolate, 0, sizeof(cl_mem), (void *)&dev_in);
    dt_opencl_set_kernel_arg(devid, gd->kernel_vng_border_interpolate, 1, sizeof(cl_mem), (void *)&dev_tmp2);
    dt_opencl_set_kernel_arg(devid, gd->kernel_vng_border_interpolate, 2, sizeof(int), (void *)&width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_vng_border_interpolate, 3, sizeof(int), (void *)&height);
    dt_opencl_set_kernel_arg(devid, gd->kernel_vng_border_interpolate, 4, sizeof(int), (void *)&roi_in->x);
    dt_opencl_set_kernel_arg(devid, gd->kernel_vng_border_interpolate, 5, sizeof(int), (void *)&roi_in->y);
    dt_opencl_set_kernel_arg(devid, gd->kernel_vng_border_interpolate, 6, sizeof(uint32_t), (void *)&filters4);
    dt_opencl_set_kernel_arg(devid, gd->kernel_vng_border_interpolate, 7, sizeof(cl_mem), (void *)&dev_xtrans);
    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_vng_border_interpolate, sizes);
    if(err != CL_SUCCESS) goto error;

    if(filters4 != 9)
    {
      // for Bayer sensors mix the two green channels
      sizes[0] = ROUNDUPWD(width);
      sizes[1] = ROUNDUPHT(height);
      sizes[2] = 1;
      dt_opencl_set_kernel_arg(devid, gd->kernel_vng_green_equilibrate, 0, sizeof(cl_mem), (void *)&dev_tmp2);
      dt_opencl_set_kernel_arg(devid, gd->kernel_vng_green_equilibrate, 1, sizeof(cl_mem), (void *)&dev_tmp2);
      dt_opencl_set_kernel_arg(devid, gd->kernel_vng_green_equilibrate, 2, sizeof(int), (void *)&width);
      dt_opencl_set_kernel_arg(devid, gd->kernel_vng_green_equilibrate, 3, sizeof(int), (void *)&height);
      err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_vng_green_equilibrate, sizes);
      if(err != CL_SUCCESS) goto error;
    }

    if(scaled)
    {
      // scale temp buffer to output buffer
      err = dt_iop_clip_and_zoom_roi_cl(devid, dev_out, dev_tmp2, roi_out, roi_in);
      if(err != CL_SUCCESS) goto error;
    }
  }
  else
  {
    // sample half-size or third-size image
    if(piece->pipe->filters == 9u)
    {
      const int width = roi_out->width;
      const int height = roi_out->height;
      size_t sizes[2] = { ROUNDUPWD(width), ROUNDUPHT(height) };

      dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_third_size, 0, sizeof(cl_mem), (void *)&dev_in);
      dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_third_size, 1, sizeof(cl_mem), (void *)&dev_out);
      dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_third_size, 2, sizeof(int), (void *)&width);
      dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_third_size, 3, sizeof(int), (void *)&height);
      dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_third_size, 4, sizeof(int), (void *)&roi_in->x);
      dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_third_size, 5, sizeof(int), (void *)&roi_in->y);
      dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_third_size, 6, sizeof(int), (void *)&roi_in->width);
      dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_third_size, 7, sizeof(int), (void *)&roi_in->height);
      dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_third_size, 8, sizeof(float), (void *)&roi_out->scale);
      dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_third_size, 9, sizeof(cl_mem), (void *)&dev_xtrans);
      err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_zoom_third_size, sizes);
      if(err != CL_SUCCESS) goto error;
    }
    else
    {
      const int zero = 0;
      const int width = roi_out->width;
      const int height = roi_out->height;
      size_t sizes[2] = { ROUNDUPWD(width), ROUNDUPHT(height) };

      dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_half_size, 0, sizeof(cl_mem), (void *)&dev_in);
      dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_half_size, 1, sizeof(cl_mem), (void *)&dev_out);
      dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_half_size, 2, sizeof(int), (void *)&width);
      dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_half_size, 3, sizeof(int), (void *)&height);
      dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_half_size, 4, sizeof(int), (void *)&zero);
      dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_half_size, 5, sizeof(int), (void *)&zero);
      dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_half_size, 6, sizeof(int), (void *)&roi_in->width);
      dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_half_size, 7, sizeof(int), (void *)&roi_in->height);
      dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_half_size, 8, sizeof(float), (void *)&roi_out->scale);
      dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_half_size, 9, sizeof(uint32_t),
                               (void *)&piece->pipe->filters);
      err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_zoom_half_size, sizes);
      if(err != CL_SUCCESS) goto error;
    }
  }

  if(dev_tmp1 != NULL && dev_tmp1 != dev_out) dt_opencl_release_mem_object(dev_tmp1);
  dev_tmp1 = NULL;

  if(dev_tmp2 != NULL && dev_tmp2 != dev_out) dt_opencl_release_mem_object(dev_tmp2);
  dev_tmp2 = NULL;

  if(dev_xtrans != NULL) dt_opencl_release_mem_object(dev_xtrans);
  dev_xtrans = NULL;

  if(dev_lookup != NULL) dt_opencl_release_mem_object(dev_lookup);
  dev_lookup = NULL;

  if(dev_code != NULL) dt_opencl_release_mem_object(dev_code);
  dev_code = NULL;

  if(dev_ips != NULL) dt_opencl_release_mem_object(dev_ips);
  dev_ips = NULL;

  if(dev_green_eq != NULL) dt_opencl_release_mem_object(dev_green_eq);
  dev_green_eq = NULL;

  // color smoothing
  if(data->color_smoothing)
  {
    if(!color_smoothing_cl(self, piece, dev_out, dev_out, roi_out))
      goto error;
  }

  return TRUE;

error:
  if(dev_tmp1 != NULL && dev_tmp1 != dev_out) dt_opencl_release_mem_object(dev_tmp1);
  if(dev_tmp2 != NULL && dev_tmp2 != dev_out) dt_opencl_release_mem_object(dev_tmp2);
  if(dev_xtrans != NULL) dt_opencl_release_mem_object(dev_xtrans);
  if(dev_lookup != NULL) dt_opencl_release_mem_object(dev_lookup);
  if(dev_code != NULL) dt_opencl_release_mem_object(dev_code);
  if(dev_ips != NULL) dt_opencl_release_mem_object(dev_ips);
  if(dev_green_eq != NULL) dt_opencl_release_mem_object(dev_green_eq);
  dt_print(DT_DEBUG_OPENCL, "[opencl_demosaic] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}

static int process_markesteijn_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in,
                                  cl_mem dev_out, const dt_iop_roi_t *const roi_in,
                                  const dt_iop_roi_t *const roi_out)
{
  dt_iop_demosaic_data_t *data = (dt_iop_demosaic_data_t *)piece->data;
  dt_iop_demosaic_global_data_t *gd = (dt_iop_demosaic_global_data_t *)self->data;

  const int devid = piece->pipe->devid;
  const int qual = get_quality();
  const uint8_t(*const xtrans)[6] = (const uint8_t(*const)[6])piece->pipe->xtrans;

  const float processed_maximum[4] = { piece->pipe->processed_maximum[0],
                                       piece->pipe->processed_maximum[1],
                                       piece->pipe->processed_maximum[2],
                                       1.0f };

  // we check if we need ultra-high quality thumbnail for this size
  int uhq_thumb = 0;
  if (piece->pipe->type == DT_DEV_PIXELPIPE_THUMBNAIL)
    uhq_thumb = get_thumb_quality(roi_out->width, roi_out->height);

  // check if we can avoid full scale demosaicing and chose simple
  // half scale or third scale interpolation instead
  const int full_scale_demosaicing = (piece->pipe->type == DT_DEV_PIXELPIPE_FULL && qual > 0)
                                     || piece->pipe->type == DT_DEV_PIXELPIPE_EXPORT || uhq_thumb
                                     || roi_out->scale > (piece->pipe->filters == 9u ? 0.333f : 0.5f);

  cl_mem dev_tmp = NULL;
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
  cl_int err = -999;

  cl_mem *dev_rgb = dev_rgbv;

  dev_xtrans = dt_opencl_copy_host_to_device_constant(devid, sizeof(piece->pipe->xtrans), piece->pipe->xtrans);
  if(dev_xtrans == NULL) goto error;

  if(full_scale_demosaicing)
  {
    // Full demosaic and then scaling if needed
    const int scaled = (roi_out->width != roi_in->width || roi_out->height != roi_in->height);

    int width = roi_in->width;
    int height = roi_in->height;
    const int passes = (data->demosaicing_method == DT_IOP_DEMOSAIC_MARKESTEIJN_3) ? 3 : 1;
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
          int g = FCxtrans(row, col, NULL, xtrans) == 1;
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
              int v = orth[d] * patt[g][c * 2] + orth[d + 1] * patt[g][c * 2 + 1];
              int h = orth[d + 2] * patt[g][c * 2] + orth[d + 3] * patt[g][c * 2 + 1];

              allhex[row][col][c ^ (g * 2 & d)][0] = h;
              allhex[row][col][c ^ (g * 2 & d)][1] = v;
            }
        }

    dev_allhex = dt_opencl_copy_host_to_device_constant(devid, sizeof(allhex), allhex);
    if(dev_allhex == NULL) goto error;

    for(int n = 0; n < ndir; n++)
    {
      dev_rgbv[n] = dt_opencl_alloc_device_buffer(devid, (size_t)width * height * 4 * sizeof(float));
      if(dev_rgbv[n] == NULL) goto error;
    }

    dev_gminmax = dt_opencl_alloc_device_buffer(devid, (size_t)width * height * 2 * sizeof(float));
    if(dev_gminmax == NULL) goto error;

    dev_aux = dt_opencl_alloc_device_buffer(devid, (size_t)width * height * 4 * sizeof(float));
    if(dev_aux == NULL) goto error;

    if(scaled)
    {
      // need to scale to right res
      dev_tmp = dt_opencl_alloc_device(devid, (size_t)width, height, 4 * sizeof(float));
      if(dev_tmp == NULL) goto error;
    }
    else
    {
      // scaling factor 1.0 --> we can directly process into the output buffer
      dev_tmp = dev_out;
    }

    size_t sizes[3];
    size_t local[3];
    dt_iop_demosaic_cl_buffer_t factors;
    int blocksizex;
    int blocksizey;

    // copy from dev_in to first rgb image buffer.
    sizes[0] = ROUNDUPWD(width);
    sizes[1] = ROUNDUPHT(height);
    sizes[2] = 1;
    dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_initial_copy, 0, sizeof(cl_mem), (void *)&dev_in);
    dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_initial_copy, 1, sizeof(cl_mem), (void *)&dev_rgb[0]);
    dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_initial_copy, 2, sizeof(int), (void *)&width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_initial_copy, 3, sizeof(int), (void *)&height);
    dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_initial_copy, 4, sizeof(int), (void *)&roi_in->x);
    dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_initial_copy, 5, sizeof(int), (void *)&roi_in->y);
    dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_initial_copy, 6, sizeof(cl_mem), (void *)&dev_xtrans);
    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_markesteijn_initial_copy, sizes);
    if(err != CL_SUCCESS) goto error;


    // duplicate dev_rgb[0] to dev_rgb[1], dev_rgb[2], and dev_rgb[3]
    for(int c = 1; c <= 3; c++)
    {
      err = dt_opencl_enqueue_copy_buffer_to_buffer(devid, dev_rgb[0], dev_rgb[c], 0, 0,
                                                    (size_t)width * height * 4 * sizeof(float));
      if(err != CL_SUCCESS) goto error;
    }

    // find minimum and maximum allowed green values of red/blue pixel pairs
    const int pad_g1_g3 = 3;
    factors.xoffset = 2*3;
    factors.xfactor = 1;
    factors.yoffset = 2*3;
    factors.yfactor = 1;
    factors.cellsize = sizeof(float);
    factors.overhead = 0;
    blocksizex = 64;
    blocksizey = 64;
    if(!blocksizeopt(devid, gd->kernel_markesteijn_green_minmax, &blocksizex, &blocksizey, &factors))
      goto error;

    sizes[0] = ROUNDUP(width, blocksizex);
    sizes[1] = ROUNDUP(height, blocksizey);
    sizes[2] = 1;
    local[0] = blocksizex;
    local[1] = blocksizey;
    local[2] = 1;
    dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_green_minmax, 0, sizeof(cl_mem), (void *)&dev_rgb[0]);
    dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_green_minmax, 1, sizeof(cl_mem), (void *)&dev_gminmax);
    dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_green_minmax, 2, sizeof(int), (void *)&width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_green_minmax, 3, sizeof(int), (void *)&height);
    dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_green_minmax, 4, sizeof(int), (void *)&pad_g1_g3);
    dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_green_minmax, 5, sizeof(int), (void *)&roi_in->x);
    dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_green_minmax, 6, sizeof(int), (void *)&roi_in->y);
    dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_green_minmax, 7, 2 * sizeof(char), (void *)sgreen);
    dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_green_minmax, 8, sizeof(cl_mem), (void *)&dev_xtrans);
    dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_green_minmax, 9, sizeof(cl_mem), (void *)&dev_allhex);
    dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_green_minmax, 10,
                             (blocksizex + 2*3) * (blocksizey + 2*3) * sizeof(float), NULL);
    err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_markesteijn_green_minmax, sizes, local);
    if(err != CL_SUCCESS) goto error;

    // interpolate green horizontally, vertically, and along both diagonals
    const int pad_g_interp = 3;
    factors.xoffset = 2*6;
    factors.xfactor = 1;
    factors.yoffset = 2*6;
    factors.yfactor = 1;
    factors.cellsize = 4 * sizeof(float);
    factors.overhead = 0;
    blocksizex = 64;
    blocksizey = 64;
    if(!blocksizeopt(devid, gd->kernel_markesteijn_interpolate_green, &blocksizex, &blocksizey, &factors))
      goto error;

    sizes[0] = ROUNDUP(width, blocksizex);
    sizes[1] = ROUNDUP(height, blocksizey);
    sizes[2] = 1;
    local[0] = blocksizex;
    local[1] = blocksizey;
    local[2] = 1;
    dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_interpolate_green, 0, sizeof(cl_mem), (void *)&dev_rgb[0]);
    dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_interpolate_green, 1, sizeof(cl_mem), (void *)&dev_rgb[1]);
    dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_interpolate_green, 2, sizeof(cl_mem), (void *)&dev_rgb[2]);
    dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_interpolate_green, 3, sizeof(cl_mem), (void *)&dev_rgb[3]);
    dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_interpolate_green, 4, sizeof(cl_mem), (void *)&dev_gminmax);
    dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_interpolate_green, 5, sizeof(int), (void *)&width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_interpolate_green, 6, sizeof(int), (void *)&height);
    dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_interpolate_green, 7, sizeof(int), (void *)&pad_g_interp);
    dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_interpolate_green, 8, sizeof(int), (void *)&roi_in->x);
    dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_interpolate_green, 9, sizeof(int), (void *)&roi_in->y);
    dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_interpolate_green, 10, 2 * sizeof(char), (void *)sgreen);
    dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_interpolate_green, 11, sizeof(cl_mem), (void *)&dev_xtrans);
    dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_interpolate_green, 12, sizeof(cl_mem), (void *)&dev_allhex);
    dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_interpolate_green, 13,
                             (blocksizex + 2*6) * (blocksizey + 2*6) * 4 * sizeof(float), NULL);
    err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_markesteijn_interpolate_green, sizes, local);
    if(err != CL_SUCCESS) goto error;

    // multi-pass loop: one pass for Markesteijn-1 and three passes for Markesteijn-3
    for(int pass = 0; pass < passes; pass++)
    {

      // if on second pass, copy rgb[0] to [3] into rgb[4] to [7] ....
      if(pass == 1)
      {
        for(int c = 0; c < 4; c++)
        {
          err = dt_opencl_enqueue_copy_buffer_to_buffer(devid, dev_rgb[c], dev_rgb[c + 4], 0, 0,
                                                        (size_t)width * height * 4 * sizeof(float));
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
        sizes[0] = ROUNDUPWD(width);
        sizes[1] = ROUNDUPHT(height);
        sizes[2] = 1;
        dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_recalculate_green, 0, sizeof(cl_mem), (void *)&dev_rgb[0]);
        dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_recalculate_green, 1, sizeof(cl_mem), (void *)&dev_rgb[1]);
        dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_recalculate_green, 2, sizeof(cl_mem), (void *)&dev_rgb[2]);
        dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_recalculate_green, 3, sizeof(cl_mem), (void *)&dev_rgb[3]);
        dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_recalculate_green, 4, sizeof(cl_mem), (void *)&dev_gminmax);
        dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_recalculate_green, 5, sizeof(int), (void *)&width);
        dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_recalculate_green, 6, sizeof(int), (void *)&height);
        dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_recalculate_green, 7, sizeof(int), (void *)&pad_g_recalc);
        dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_recalculate_green, 8, sizeof(int), (void *)&roi_in->x);
        dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_recalculate_green, 9, sizeof(int), (void *)&roi_in->y);
        dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_recalculate_green, 10, 2 * sizeof(char), (void *)sgreen);
        dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_recalculate_green, 11, sizeof(cl_mem), (void *)&dev_xtrans);
        dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_recalculate_green, 12, sizeof(cl_mem), (void *)&dev_allhex);
        err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_markesteijn_recalculate_green, sizes);
        if(err != CL_SUCCESS) goto error;
      }

      // interpolate red and blue values for solitary green pixels
      const int pad_rb_g = (passes == 1) ? 6 : 5;
      factors.xoffset = 2*2;
      factors.xfactor = 1;
      factors.yoffset = 2*2;
      factors.yfactor = 1;
      factors.cellsize = 4 * sizeof(float);
      factors.overhead = 0;
      blocksizex = 64;
      blocksizey = 64;
      if(!blocksizeopt(devid, gd->kernel_markesteijn_solitary_green, &blocksizex, &blocksizey, &factors))
      goto error;

      sizes[0] = ROUNDUP(width, blocksizex);
      sizes[1] = ROUNDUP(height, blocksizey);
      sizes[2] = 1;
      local[0] = blocksizex;
      local[1] = blocksizey;
      local[2] = 1;

      cl_mem *dev_trgb = dev_rgb;
      for(int d = 0, i = 1, h = 0; d < 6; d++, i ^= 1, h ^= 2)
      {
        const char dir[2] = { i, i ^ 1 };

        // we use dev_aux to transport intermediate results from one loop run to the next
        dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_solitary_green, 0, sizeof(cl_mem), (void *)&dev_trgb[0]);
        dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_solitary_green, 1, sizeof(cl_mem), (void *)&dev_aux);
        dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_solitary_green, 2, sizeof(int), (void *)&width);
        dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_solitary_green, 3, sizeof(int), (void *)&height);
        dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_solitary_green, 4, sizeof(int), (void *)&pad_rb_g);
        dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_solitary_green, 5, sizeof(int), (void *)&roi_in->x);
        dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_solitary_green, 6, sizeof(int), (void *)&roi_in->y);
        dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_solitary_green, 7, sizeof(int), (void *)&d);
        dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_solitary_green, 8, 2 * sizeof(char), (void *)dir);
        dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_solitary_green, 9, sizeof(int), (void *)&h);
        dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_solitary_green, 10, 2 * sizeof(char), (void *)sgreen);
        dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_solitary_green, 11, sizeof(cl_mem), (void *)&dev_xtrans);
        dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_solitary_green, 12,
                                (blocksizex + 2*2) * (blocksizey + 2*2) * 4 * sizeof(float), NULL);
        err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_markesteijn_solitary_green, sizes, local);
        if(err != CL_SUCCESS) goto error;

        if((d < 2) || (d & 1)) dev_trgb++;
      }

      // interpolate red for blue pixels and vice versa
      const int pad_rb_br = (passes == 1) ? 6 : 5;
      factors.xoffset = 2*3;
      factors.xfactor = 1;
      factors.yoffset = 2*3;
      factors.yfactor = 1;
      factors.cellsize = 4 * sizeof(float);
      factors.overhead = 0;
      blocksizex = 64;
      blocksizey = 64;
      if(!blocksizeopt(devid, gd->kernel_markesteijn_red_and_blue, &blocksizex, &blocksizey, &factors))
      goto error;

      sizes[0] = ROUNDUP(width, blocksizex);
      sizes[1] = ROUNDUP(height, blocksizey);
      sizes[2] = 1;
      local[0] = blocksizex;
      local[1] = blocksizey;
      local[2] = 1;

      for(int d = 0; d < 4; d++)
      {
        dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_red_and_blue, 0, sizeof(cl_mem), (void *)&dev_rgb[d]);
        dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_red_and_blue, 1, sizeof(int), (void *)&width);
        dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_red_and_blue, 2, sizeof(int), (void *)&height);
        dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_red_and_blue, 3, sizeof(int), (void *)&pad_rb_br);
        dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_red_and_blue, 4, sizeof(int), (void *)&roi_in->x);
        dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_red_and_blue, 5, sizeof(int), (void *)&roi_in->y);
        dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_red_and_blue, 6, sizeof(int), (void *)&d);
        dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_red_and_blue, 7, 2 * sizeof(char), (void *)sgreen);
        dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_red_and_blue, 8, sizeof(cl_mem), (void *)&dev_xtrans);
        dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_red_and_blue, 9,
                                (blocksizex + 2*3) * (blocksizey + 2*3) * 4 * sizeof(float), NULL);
        err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_markesteijn_red_and_blue, sizes, local);
        if(err != CL_SUCCESS) goto error;
      }

      // interpolate red and blue for 2x2 blocks of green
      const int pad_g22 = (passes == 1) ? 8 : 4;
      factors.xoffset = 2*2;
      factors.xfactor = 1;
      factors.yoffset = 2*2;
      factors.yfactor = 1;
      factors.cellsize = 4 * sizeof(float);
      factors.overhead = 0;
      blocksizex = 64;
      blocksizey = 64;
      if(!blocksizeopt(devid, gd->kernel_markesteijn_interpolate_twoxtwo, &blocksizex, &blocksizey, &factors))
      goto error;

      sizes[0] = ROUNDUP(width, blocksizex);
      sizes[1] = ROUNDUP(height, blocksizey);
      sizes[2] = 1;
      local[0] = blocksizex;
      local[1] = blocksizey;
      local[2] = 1;

      for(int d = 0, n = 0; d < ndir; d += 2, n++)
      {
        dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_interpolate_twoxtwo, 0, sizeof(cl_mem), (void *)&dev_rgb[n]);
        dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_interpolate_twoxtwo, 1, sizeof(int), (void *)&width);
        dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_interpolate_twoxtwo, 2, sizeof(int), (void *)&height);
        dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_interpolate_twoxtwo, 3, sizeof(int), (void *)&pad_g22);
        dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_interpolate_twoxtwo, 4, sizeof(int), (void *)&roi_in->x);
        dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_interpolate_twoxtwo, 5, sizeof(int), (void *)&roi_in->y);
        dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_interpolate_twoxtwo, 6, sizeof(int), (void *)&d);
        dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_interpolate_twoxtwo, 7, 2 * sizeof(char), (void *)sgreen);
        dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_interpolate_twoxtwo, 8, sizeof(cl_mem), (void *)&dev_xtrans);
        dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_interpolate_twoxtwo, 9, sizeof(cl_mem), (void *)&dev_allhex);
        dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_interpolate_twoxtwo, 10,
                                (blocksizex + 2*2) * (blocksizey + 2*2) * 4 * sizeof(float), NULL);
        err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_markesteijn_interpolate_twoxtwo, sizes, local);
        if(err != CL_SUCCESS) goto error;
      }
    }
    // end of multi pass

    // gminmax data no longer needed
    if(dev_gminmax != NULL) dt_opencl_release_mem_object(dev_gminmax);
    dev_gminmax = NULL;

    // jump back to the first set of rgb buffers (this is a noop for Markesteijn-1)
    dev_rgb = dev_rgbv;

    // prepare derivatives buffers
    for(int n = 0; n < ndir; n++)
    {
      dev_drv[n] = dt_opencl_alloc_device_buffer(devid, (size_t)width * height * sizeof(float));
      if(dev_drv[n] == NULL) goto error;
    }

    // convert to perceptual colorspace and differentiate in all directions
    const int pad_yuv = (passes == 1) ? 8 : 13;
    factors.xoffset = 2*1;
    factors.xfactor = 1;
    factors.yoffset = 2*1;
    factors.yfactor = 1;
    factors.cellsize = 4 * sizeof(float);
    factors.overhead = 0;
    blocksizex = 64;
    blocksizey = 64;
    if(!blocksizeopt(devid, gd->kernel_markesteijn_differentiate, &blocksizex, &blocksizey, &factors))
    goto error;

    for(int d = 0; d < ndir; d++)
    {
      // convert to perceptual YPbPr colorspace
      sizes[0] = ROUNDUPWD(width);
      sizes[1] = ROUNDUPHT(height);
      sizes[2] = 1;
      dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_convert_yuv, 0, sizeof(cl_mem), (void *)&dev_rgb[d]);
      dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_convert_yuv, 1, sizeof(cl_mem), (void *)&dev_aux);
      dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_convert_yuv, 2, sizeof(int), (void *)&width);
      dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_convert_yuv, 3, sizeof(int), (void *)&height);
      dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_convert_yuv, 4, sizeof(int), (void *)&pad_yuv);
      err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_markesteijn_convert_yuv, sizes);
      if(err != CL_SUCCESS) goto error;


      // differentiate in all directions
      sizes[0] = ROUNDUP(width, blocksizex);
      sizes[1] = ROUNDUP(height, blocksizey);
      sizes[2] = 1;
      local[0] = blocksizex;
      local[1] = blocksizey;
      local[2] = 1;
      dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_differentiate, 0, sizeof(cl_mem), (void *)&dev_aux);
      dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_differentiate, 1, sizeof(cl_mem), (void *)&dev_drv[d]);
      dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_differentiate, 2, sizeof(int), (void *)&width);
      dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_differentiate, 3, sizeof(int), (void *)&height);
      dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_differentiate, 4, sizeof(int), (void *)&pad_yuv);
      dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_differentiate, 5, sizeof(int), (void *)&d);
      dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_differentiate, 6,
                              (blocksizex + 2*1) * (blocksizey + 2*1) * 4 * sizeof(float), NULL);
      err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_markesteijn_differentiate, sizes, local);
      if(err != CL_SUCCESS) goto error;
    }

    // reserve buffers for homogeneity maps and sum maps
    for(int n = 0; n < ndir; n++)
    {
      dev_homo[n] = dt_opencl_alloc_device_buffer(devid, (size_t)width * height * sizeof(unsigned char));
      if(dev_homo[n] == NULL) goto error;

      dev_homosum[n] = dt_opencl_alloc_device_buffer(devid, (size_t)width * height * sizeof(unsigned char));
      if(dev_homosum[n] == NULL) goto error;
    }

    // get thresholds for homogeneity map (store them in dev_aux)
    for(int d = 0; d < ndir; d++)
    {
      const int pad_homo = (passes == 1) ? 10 : 15;
      sizes[0] = ROUNDUPWD(width);
      sizes[1] = ROUNDUPHT(height);
      sizes[2] = 1;
      dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_homo_threshold, 0, sizeof(cl_mem), (void *)&dev_drv[d]);
      dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_homo_threshold, 1, sizeof(cl_mem), (void *)&dev_aux);
      dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_homo_threshold, 2, sizeof(int), (void *)&width);
      dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_homo_threshold, 3, sizeof(int), (void *)&height);
      dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_homo_threshold, 4, sizeof(int), (void *)&pad_homo);
      dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_homo_threshold, 5, sizeof(int), (void *)&d);
      err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_markesteijn_homo_threshold, sizes);
      if(err != CL_SUCCESS) goto error;
    }

    // set homogeneity maps
    const int pad_homo = (passes == 1) ? 10 : 15;
    factors.xoffset = 2*1;
    factors.xfactor = 1;
    factors.yoffset = 2*1;
    factors.yfactor = 1;
    factors.cellsize = 1 * sizeof(float);
    factors.overhead = 0;
    blocksizex = 64;
    blocksizey = 64;
    if(!blocksizeopt(devid, gd->kernel_markesteijn_homo_set, &blocksizex, &blocksizey, &factors))
    goto error;

    for(int d = 0; d < ndir; d++)
    {
      sizes[0] = ROUNDUP(width, blocksizex);
      sizes[1] = ROUNDUP(height, blocksizey);
      sizes[2] = 1;
      local[0] = blocksizex;
      local[1] = blocksizey;
      local[2] = 1;
      dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_homo_set, 0, sizeof(cl_mem), (void *)&dev_drv[d]);
      dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_homo_set, 1, sizeof(cl_mem), (void *)&dev_aux);
      dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_homo_set, 2, sizeof(cl_mem), (void *)&dev_homo[d]);
      dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_homo_set, 3, sizeof(int), (void *)&width);
      dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_homo_set, 4, sizeof(int), (void *)&height);
      dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_homo_set, 5, sizeof(int), (void *)&pad_homo);
      dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_homo_set, 6,
                              (blocksizex + 2*1) * (blocksizey + 2*1) * sizeof(float), NULL);
      err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_markesteijn_homo_set, sizes, local);
      if(err != CL_SUCCESS) goto error;
    }

    // build 5x5 sum of homogeneity maps for each pixel and direction
    factors.xoffset = 2*2;
    factors.xfactor = 1;
    factors.yoffset = 2*2;
    factors.yfactor = 1;
    factors.cellsize = 1 * sizeof(char);
    factors.overhead = 0;
    blocksizex = 64;
    blocksizey = 64;
    if(!blocksizeopt(devid, gd->kernel_markesteijn_homo_sum, &blocksizex, &blocksizey, &factors))
    goto error;

    for(int d = 0; d < ndir; d++)
    {
      sizes[0] = ROUNDUP(width, blocksizex);
      sizes[1] = ROUNDUP(height, blocksizey);
      sizes[2] = 1;
      local[0] = blocksizex;
      local[1] = blocksizey;
      local[2] = 1;
      dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_homo_sum, 0, sizeof(cl_mem), (void *)&dev_homo[d]);
      dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_homo_sum, 1, sizeof(cl_mem), (void *)&dev_homosum[d]);
      dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_homo_sum, 2, sizeof(int), (void *)&width);
      dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_homo_sum, 3, sizeof(int), (void *)&height);
      dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_homo_sum, 4, sizeof(int), (void *)&pad_tile);
      dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_homo_sum, 5,
                              (blocksizex + 2*2) * (blocksizey + 2*2) * sizeof(char), NULL);
      err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_markesteijn_homo_sum, sizes, local);
      if(err != CL_SUCCESS) goto error;
    }

    // get maximum of homogeneity maps (store in dev_aux)
    for(int d = 0; d < ndir; d++)
    {
      sizes[0] = ROUNDUPWD(width);
      sizes[1] = ROUNDUPHT(height);
      sizes[2] = 1;
      dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_homo_max, 0, sizeof(cl_mem), (void *)&dev_homosum[d]);
      dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_homo_max, 1, sizeof(cl_mem), (void *)&dev_aux);
      dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_homo_max, 2, sizeof(int), (void *)&width);
      dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_homo_max, 3, sizeof(int), (void *)&height);
      dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_homo_max, 4, sizeof(int), (void *)&pad_tile);
      dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_homo_max, 5, sizeof(int), (void *)&d);
      err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_markesteijn_homo_max, sizes);
      if(err != CL_SUCCESS) goto error;
    }

    // adjust maximum value
    sizes[0] = ROUNDUPWD(width);
    sizes[1] = ROUNDUPHT(height);
    sizes[2] = 1;
    dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_homo_max_corr, 0, sizeof(cl_mem), (void *)&dev_aux);
    dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_homo_max_corr, 1, sizeof(int), (void *)&width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_homo_max_corr, 2, sizeof(int), (void *)&height);
    dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_homo_max_corr, 3, sizeof(int), (void *)&pad_tile);
    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_markesteijn_homo_max_corr, sizes);
    if(err != CL_SUCCESS) goto error;

    // for Markesteijn-3: use only one of two directions if there is a difference in homogeneity
    for(int d = 0; d < ndir - 4; d++)
    {
      sizes[0] = ROUNDUPWD(width);
      sizes[1] = ROUNDUPHT(height);
      sizes[2] = 1;
      dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_homo_quench, 0, sizeof(cl_mem), (void *)&dev_homosum[d]);
      dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_homo_quench, 1, sizeof(cl_mem), (void *)&dev_homosum[d + 4]);
      dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_homo_quench, 2, sizeof(int), (void *)&width);
      dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_homo_quench, 3, sizeof(int), (void *)&height);
      dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_homo_quench, 4, sizeof(int), (void *)&pad_tile);
      err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_markesteijn_homo_quench, sizes);
      if(err != CL_SUCCESS) goto error;
    }

    // initialize output buffer to zero
    sizes[0] = ROUNDUPWD(width);
    sizes[1] = ROUNDUPHT(height);
    sizes[2] = 1;
    dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_zero, 0, sizeof(cl_mem), (void *)&dev_tmp);
    dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_zero, 1, sizeof(int), (void *)&width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_zero, 2, sizeof(int), (void *)&height);
    dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_zero, 3, sizeof(int), (void *)&pad_tile);
    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_markesteijn_zero, sizes);
    if(err != CL_SUCCESS) goto error;


    // accumulate all contributions
    for(int d = 0; d < ndir; d++)
    {
      sizes[0] = ROUNDUPWD(width);
      sizes[1] = ROUNDUPHT(height);
      sizes[2] = 1;
      dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_accu, 0, sizeof(cl_mem), (void *)&dev_tmp);
      dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_accu, 1, sizeof(cl_mem), (void *)&dev_tmp);
      dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_accu, 2, sizeof(cl_mem), (void *)&dev_rgbv[d]);
      dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_accu, 3, sizeof(cl_mem), (void *)&dev_homosum[d]);
      dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_accu, 4, sizeof(cl_mem), (void *)&dev_aux);
      dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_accu, 5, sizeof(int), (void *)&width);
      dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_accu, 6, sizeof(int), (void *)&height);
      dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_accu, 7, sizeof(int), (void *)&pad_tile);
      err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_markesteijn_accu, sizes);
      if(err != CL_SUCCESS) goto error;
    }

    // process the final image
    sizes[0] = ROUNDUPWD(width);
    sizes[1] = ROUNDUPHT(height);
    sizes[2] = 1;
    dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_final, 0, sizeof(cl_mem), (void *)&dev_tmp);
    dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_final, 1, sizeof(cl_mem), (void *)&dev_tmp);
    dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_final, 2, sizeof(int), (void *)&width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_final, 3, sizeof(int), (void *)&height);
    dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_final, 4, sizeof(int), (void *)&pad_tile);
    dt_opencl_set_kernel_arg(devid, gd->kernel_markesteijn_final, 5, 4*sizeof(float), (void *)processed_maximum);
    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_markesteijn_final, sizes);
    if(err != CL_SUCCESS) goto error;

    // now it's time to get rid of most of the temporary buffers (except of dev_tmp and dev_xtrans)
    for(int n = 0; n < 8; n++)
    {
      if(dev_rgbv[n] != NULL) dt_opencl_release_mem_object(dev_rgbv[n]);
      dev_rgbv[n] = NULL;
    }

    for(int n = 0; n < 8; n++)
    {
      if(dev_drv[n] != NULL) dt_opencl_release_mem_object(dev_drv[n]);
      dev_drv[n] = NULL;
    }

    for(int n = 0; n < 8; n++)
    {
      if(dev_homo[n] != NULL) dt_opencl_release_mem_object(dev_homo[n]);
      dev_homo[n] = NULL;
    }

    for(int n = 0; n < 8; n++)
    {
      if(dev_homosum[n] != NULL) dt_opencl_release_mem_object(dev_homosum[n]);
      dev_homosum[n] = NULL;
    }

    if(dev_aux != NULL) dt_opencl_release_mem_object(dev_aux);
    dev_aux = NULL;

    if(dev_xtrans != NULL) dt_opencl_release_mem_object(dev_xtrans);
    dev_xtrans = NULL;

    if(dev_allhex != NULL) dt_opencl_release_mem_object(dev_allhex);
    dev_allhex = NULL;

    if(dev_green_eq != NULL) dt_opencl_release_mem_object(dev_green_eq);
    dev_green_eq = NULL;


    // take care of image borders. the algorihm above leaves an unprocessed border of pad_tile pixels.
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
      cl_mem dev_edge_in = dt_opencl_alloc_device(devid, edges[n][2], edges[n][3], sizeof(float));
      if(dev_edge_in == NULL) goto error;

      // reserve output buffer for VNG processing of edge
      cl_mem dev_edge_out = dt_opencl_alloc_device(devid, edges[n][2], edges[n][3], 4 * sizeof(float));
      if(dev_edge_out == NULL) goto error;

      // copy edge to input buffer
      err = dt_opencl_enqueue_copy_image(devid, dev_in, dev_edge_in, iorigin, oorigin, region);
      if(err != CL_SUCCESS) goto error;

      // VNG processing
      if(!process_vng_cl(self, piece, dev_edge_in, dev_edge_out, &roi, &roi))
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


    if(scaled)
    {
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
    size_t sizes[2] = { ROUNDUPWD(width), ROUNDUPHT(height) };

    dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_third_size, 0, sizeof(cl_mem), &dev_in);
    dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_third_size, 1, sizeof(cl_mem), &dev_out);
    dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_third_size, 2, sizeof(int), &width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_third_size, 3, sizeof(int), &height);
    dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_third_size, 4, sizeof(int), (void *)&roi_in->x);
    dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_third_size, 5, sizeof(int), (void *)&roi_in->y);
    dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_third_size, 6, sizeof(int), (void *)&roi_in->width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_third_size, 7, sizeof(int), (void *)&roi_in->height);
    dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_third_size, 8, sizeof(float), (void *)&roi_out->scale);
    dt_opencl_set_kernel_arg(devid, gd->kernel_zoom_third_size, 9, sizeof(cl_mem), (void *)&dev_xtrans);
    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_zoom_third_size, sizes);
    if(err != CL_SUCCESS) goto error;
  }

  // free remaining temporary buffers
  if(dev_tmp != NULL && dev_tmp != dev_out) dt_opencl_release_mem_object(dev_tmp);
  dev_tmp = NULL;

  if(dev_xtrans != NULL) dt_opencl_release_mem_object(dev_xtrans);
  dev_xtrans = NULL;


  // color smoothing
  if(data->color_smoothing)
  {
    if(!color_smoothing_cl(self, piece, dev_out, dev_out, roi_out))
      goto error;
  }

  return TRUE;

error:
  for(int n = 0; n < 8; n++)
    if(dev_rgbv[n] != NULL) dt_opencl_release_mem_object(dev_rgbv[n]);
  for(int n = 0; n < 8; n++)
    if(dev_drv[n] != NULL) dt_opencl_release_mem_object(dev_drv[n]);
  for(int n = 0; n < 8; n++)
    if(dev_homo[n] != NULL) dt_opencl_release_mem_object(dev_homo[n]);
  for(int n = 0; n < 8; n++)
    if(dev_homosum[n] != NULL) dt_opencl_release_mem_object(dev_homosum[n]);
  if(dev_gminmax != NULL) dt_opencl_release_mem_object(dev_gminmax);
  if(dev_tmp != NULL && dev_tmp != dev_out) dt_opencl_release_mem_object(dev_tmp);
  if(dev_xtrans != NULL) dt_opencl_release_mem_object(dev_xtrans);
  if(dev_allhex != NULL) dt_opencl_release_mem_object(dev_allhex);
  if(dev_green_eq != NULL) dt_opencl_release_mem_object(dev_green_eq);
  if(dev_aux != NULL) dt_opencl_release_mem_object(dev_aux);
  if(dev_edge_in != NULL) dt_opencl_release_mem_object(dev_edge_in);
  if(dev_edge_out != NULL) dt_opencl_release_mem_object(dev_edge_out);
  dt_print(DT_DEBUG_OPENCL, "[opencl_demosaic] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}

int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_demosaic_data_t *data = (dt_iop_demosaic_data_t *)piece->data;
  const int demosaicing_method = data->demosaicing_method;
  const int qual = get_quality();

  // we check if we need ultra-high quality thumbnail for this size
  int uhq_thumb = 0;
  if (piece->pipe->type == DT_DEV_PIXELPIPE_THUMBNAIL)
    uhq_thumb = get_thumb_quality(roi_out->width, roi_out->height);

  // we use full Markesteijn demosaicing on xtrans sensors only if
  // maximum quality is required
  const int xtrans_full_markesteijn_demosaicing =
      (piece->pipe->type == DT_DEV_PIXELPIPE_FULL && qual > 1) ||
      piece->pipe->type == DT_DEV_PIXELPIPE_EXPORT ||
      uhq_thumb ||
      roi_out->scale > 0.667f;

  if(demosaicing_method ==  DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME || demosaicing_method == DT_IOP_DEMOSAIC_PPG)
  {
    return process_default_cl(self, piece, dev_in, dev_out, roi_in, roi_out);
  }
  else if(demosaicing_method ==  DT_IOP_DEMOSAIC_VNG4 || demosaicing_method == DT_IOP_DEMOSAIC_VNG)
  {
    return process_vng_cl(self, piece, dev_in, dev_out, roi_in, roi_out);
  }
  else if((demosaicing_method == DT_IOP_DEMOSAIC_MARKESTEIJN || demosaicing_method == DT_IOP_DEMOSAIC_MARKESTEIJN_3) &&
    !xtrans_full_markesteijn_demosaicing)
  {
    return process_vng_cl(self, piece, dev_in, dev_out, roi_in, roi_out);
  }
  else if(demosaicing_method == DT_IOP_DEMOSAIC_MARKESTEIJN || demosaicing_method == DT_IOP_DEMOSAIC_MARKESTEIJN_3)
  {
    // only use markesteijn demosaicing on GPU if the corresponding config option is enabled.
    if(darktable.opencl->enable_markesteijn)
      return process_markesteijn_cl(self, piece, dev_in, dev_out, roi_in, roi_out);
    else
    {
      dt_print(DT_DEBUG_OPENCL, "[opencl_demosaic] Markesteijn demosaicing with OpenCL not enabled (see 'opencl_enable_markesteijn')\n");
      return FALSE;
    }
  }
  else
  {
    dt_print(DT_DEBUG_OPENCL, "[opencl_demosaic] demosaicing method '%s' not yet supported by opencl code\n", method2string(demosaicing_method));
    return FALSE;
  }
}
#endif

void tiling_callback(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                     const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out,
                     struct dt_develop_tiling_t *tiling)
{
  dt_iop_demosaic_data_t *data = (dt_iop_demosaic_data_t *)piece->data;

  const int qual = get_quality();
  const float ioratio = (float)roi_out->width * roi_out->height / ((float)roi_in->width * roi_in->height);
  const float smooth = data->color_smoothing ? ioratio : 0.0f;
  const float greeneq = ((piece->pipe->filters != 9u) && (data->green_eq != DT_IOP_GREEN_EQ_NO)) ? 0.25f : 0.0f;
  const dt_iop_demosaic_method_t demosaicing_method = data->demosaicing_method;

  // we check if we need ultra-high quality thumbnail for this size
  const int uhq_thumb = (piece->pipe->type == DT_DEV_PIXELPIPE_THUMBNAIL) ?
                          get_thumb_quality(roi_out->width, roi_out->height) : 0;

  // check if we will do full scale demosaicing or chose simple
  // half scale or third scale interpolation instead
  const int full_scale_demosaicing = (piece->pipe->type == DT_DEV_PIXELPIPE_FULL && qual > 0)
                                     || piece->pipe->type == DT_DEV_PIXELPIPE_EXPORT || uhq_thumb
                                     || roi_out->scale > (piece->pipe->filters == 9u ? 0.333f : 0.5f);

  // we use full Markesteijn demosaicing on xtrans sensors only if
  // maximum quality is required
  const int xtrans_full_markesteijn_demosaicing =
      (piece->pipe->type == DT_DEV_PIXELPIPE_FULL && qual > 1) ||
      piece->pipe->type == DT_DEV_PIXELPIPE_EXPORT ||
      uhq_thumb ||
      roi_out->scale > 0.8f;

  // check if output buffer has same dimension as input buffer (thus avoiding one
  // additional temporary buffer)
  const int unscaled = (roi_out->width == roi_in->width && roi_out->height == roi_in->height);

  if((demosaicing_method == DT_IOP_DEMOSAIC_PPG) ||
      (demosaicing_method == DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME) ||
      (demosaicing_method == DT_IOP_DEMOSAIC_AMAZE))
  {
    // Bayer pattern with PPG, Monochrome and Amaze
    tiling->factor = 1.0f + ioratio;

    if(full_scale_demosaicing && unscaled)
      tiling->factor += fmax(greeneq, smooth);
    else if(full_scale_demosaicing)
      tiling->factor += fmax(1.0f + greeneq, smooth);
    else
      tiling->factor += smooth;

    tiling->maxbuf = 1.0f;
    tiling->overhead = 0;
    tiling->xalign = 2;
    tiling->yalign = 2;
    tiling->overlap = 5; // take care of border handling
  }
  else if(((demosaicing_method ==  DT_IOP_DEMOSAIC_MARKESTEIJN) ||
           (demosaicing_method ==  DT_IOP_DEMOSAIC_MARKESTEIJN_3)) &&
           xtrans_full_markesteijn_demosaicing)
  {
    // X-Trans pattern full Markesteijn processing
    const int ndir = (demosaicing_method == DT_IOP_DEMOSAIC_MARKESTEIJN_3) ? 8 : 4;
    const int overlap = (demosaicing_method == DT_IOP_DEMOSAIC_MARKESTEIJN_3) ? 17 : 12;

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
    tiling->xalign = 3;
    tiling->yalign = 3;
    tiling->overlap = overlap;
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
  return;
}



void init(dt_iop_module_t *module)
{
  module->params = calloc(1, sizeof(dt_iop_demosaic_params_t));
  module->default_params = calloc(1, sizeof(dt_iop_demosaic_params_t));
  module->default_enabled = 1;
  module->priority = 123; // module order created by iop_dependencies.py, do not edit!
  module->hide_enable_button = 1;
  module->params_size = sizeof(dt_iop_demosaic_params_t);
  module->gui_data = NULL;
}

void init_global(dt_iop_module_so_t *module)
{
  const int program = 0; // from programs.conf
  dt_iop_demosaic_global_data_t *gd
      = (dt_iop_demosaic_global_data_t *)malloc(sizeof(dt_iop_demosaic_global_data_t));
  module->data = gd;
  gd->kernel_zoom_half_size = dt_opencl_create_kernel(program, "clip_and_zoom_demosaic_half_size");
  gd->kernel_ppg_green = dt_opencl_create_kernel(program, "ppg_demosaic_green");
  gd->kernel_green_eq = dt_opencl_create_kernel(program, "green_equilibration");
  gd->kernel_pre_median = dt_opencl_create_kernel(program, "pre_median");
  gd->kernel_ppg_green_median = dt_opencl_create_kernel(program, "ppg_demosaic_green_median");
  gd->kernel_ppg_redblue = dt_opencl_create_kernel(program, "ppg_demosaic_redblue");
  gd->kernel_downsample = dt_opencl_create_kernel(program, "clip_and_zoom");
  gd->kernel_border_interpolate = dt_opencl_create_kernel(program, "border_interpolate");
  gd->kernel_color_smoothing = dt_opencl_create_kernel(program, "color_smoothing");

  const int other = 14; // from programs.conf
  gd->kernel_passthrough_monochrome = dt_opencl_create_kernel(other, "passthrough_monochrome");
  gd->kernel_zoom_passthrough_monochrome
      = dt_opencl_create_kernel(other, "clip_and_zoom_demosaic_passthrough_monochrome");

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
}

void cleanup(dt_iop_module_t *module)
{
  free(module->params);
  module->params = NULL;
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_demosaic_global_data_t *gd = (dt_iop_demosaic_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_zoom_half_size);
  dt_opencl_free_kernel(gd->kernel_ppg_green);
  dt_opencl_free_kernel(gd->kernel_pre_median);
  dt_opencl_free_kernel(gd->kernel_green_eq);
  dt_opencl_free_kernel(gd->kernel_ppg_green_median);
  dt_opencl_free_kernel(gd->kernel_ppg_redblue);
  dt_opencl_free_kernel(gd->kernel_downsample);
  dt_opencl_free_kernel(gd->kernel_border_interpolate);
  dt_opencl_free_kernel(gd->kernel_color_smoothing);
  dt_opencl_free_kernel(gd->kernel_passthrough_monochrome);
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
  free(module->data);
  module->data = NULL;
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *params, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_demosaic_params_t *p = (dt_iop_demosaic_params_t *)params;
  dt_iop_demosaic_data_t *d = (dt_iop_demosaic_data_t *)piece->data;
  if(!(pipe->image.flags & DT_IMAGE_RAW) || dt_dev_pixelpipe_uses_downsampled_input(pipe)) piece->enabled = 0;
  d->green_eq = p->green_eq;
  d->color_smoothing = p->color_smoothing;
  d->median_thrs = p->median_thrs;
  d->demosaicing_method = p->demosaicing_method;

  if(d->demosaicing_method == DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME)
  {
    d->green_eq = DT_IOP_GREEN_EQ_NO;
    d->color_smoothing = 0;
    d->median_thrs = 0.0f;
  }

  if(d->demosaicing_method == DT_IOP_DEMOSAIC_AMAZE)
  {
    d->median_thrs = 0.0f;
  }

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
    case DT_IOP_DEMOSAIC_VNG:
      piece->process_cl_ready = 1;
      break;
    case DT_IOP_DEMOSAIC_MARKESTEIJN:
      piece->process_cl_ready = 1;
      break;
    case DT_IOP_DEMOSAIC_MARKESTEIJN_3:
      piece->process_cl_ready = 1;
      break;
    default:
      piece->process_cl_ready = 0;
  }

  // OpenCL can not green-equilibrate over full image.
  if(d->green_eq == DT_IOP_GREEN_EQ_FULL || d->green_eq == DT_IOP_GREEN_EQ_BOTH) piece->process_cl_ready = 0;

  if (self->dev->image_storage.flags & DT_IMAGE_4BAYER)
  {
    // 4Bayer images not implemented in OpenCL yet
    piece->process_cl_ready = 0;

    // Get and store the matrix to go from camera to RGB for 4Bayer images
    char *camera = self->dev->image_storage.camera_makermodel;
    if (!dt_colorspaces_conversion_matrices_rgb(camera, NULL, d->CAM_to_RGB, NULL))
    {
      fprintf(stderr, "[colorspaces] `%s' color matrix not found for 4bayer image!\n", camera);
      dt_control_log(_("`%s' color matrix not found for 4bayer image!"), camera);
    }
  }
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_demosaic_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_demosaic_gui_data_t *g = (dt_iop_demosaic_gui_data_t *)self->gui_data;
  dt_iop_demosaic_params_t *p = (dt_iop_demosaic_params_t *)self->params;

  if(self->dev->image_storage.filters != 9u)
  {
    gtk_widget_show(g->demosaic_method_bayer);
    gtk_widget_hide(g->demosaic_method_xtrans);
    gtk_widget_show(g->median_thrs);
    gtk_widget_show(g->greeneq);
    dt_bauhaus_combobox_set(g->demosaic_method_bayer, p->demosaicing_method);
  }
  else
  {
    gtk_widget_show(g->demosaic_method_xtrans);
    gtk_widget_hide(g->demosaic_method_bayer);
    gtk_widget_hide(g->median_thrs);
    gtk_widget_hide(g->greeneq);
    dt_bauhaus_combobox_set(g->demosaic_method_xtrans, p->demosaicing_method & ~DEMOSAIC_XTRANS);
  }

  if(p->demosaicing_method == DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME)
  {
    gtk_widget_hide(g->median_thrs);
    gtk_widget_hide(g->color_smoothing);
    gtk_widget_hide(g->greeneq);
  }

  if(p->demosaicing_method == DT_IOP_DEMOSAIC_AMAZE || p->demosaicing_method == DT_IOP_DEMOSAIC_VNG4)
  {
    gtk_widget_hide(g->median_thrs);
  }

  dt_bauhaus_slider_set(g->median_thrs, p->median_thrs);
  dt_bauhaus_combobox_set(g->color_smoothing, p->color_smoothing);
  dt_bauhaus_combobox_set(g->greeneq, p->green_eq);

  if(self->default_enabled)
  {
    gtk_widget_show(g->box_raw);
    gtk_widget_hide(g->label_non_raw);
  }
  else
  {
    gtk_widget_hide(g->box_raw);
    gtk_widget_show(g->label_non_raw);
  }
}

void reload_defaults(dt_iop_module_t *module)
{
  dt_iop_demosaic_params_t tmp
      = (dt_iop_demosaic_params_t){ .green_eq = DT_IOP_GREEN_EQ_NO,
                                    .median_thrs = 0.0f,
                                    .color_smoothing = 0,
                                    .demosaicing_method = DT_IOP_DEMOSAIC_PPG,
                                    .yet_unused_data_specific_to_demosaicing_method = 0 };

  // we might be called from presets update infrastructure => there is no image
  if(!module->dev) goto end;

  // only on for raw images:
  if(dt_image_is_raw(&module->dev->image_storage))
    module->default_enabled = 1;
  else
    module->default_enabled = 0;

  if(module->dev->image_storage.filters == 9u) tmp.demosaicing_method = DT_IOP_DEMOSAIC_MARKESTEIJN;

end:
  memcpy(module->params, &tmp, sizeof(dt_iop_demosaic_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_demosaic_params_t));
}

static void median_thrs_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(darktable.gui->reset) return;
  dt_iop_demosaic_params_t *p = (dt_iop_demosaic_params_t *)self->params;
  p->median_thrs = dt_bauhaus_slider_get(slider);
  if(p->median_thrs < 0.001f) p->median_thrs = 0.0f;
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void color_smoothing_callback(GtkWidget *button, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(darktable.gui->reset) return;
  dt_iop_demosaic_params_t *p = (dt_iop_demosaic_params_t *)self->params;
  p->color_smoothing = dt_bauhaus_combobox_get(button);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void greeneq_callback(GtkWidget *combo, dt_iop_module_t *self)
{
  dt_iop_demosaic_params_t *p = (dt_iop_demosaic_params_t *)self->params;
  int active = dt_bauhaus_combobox_get(combo);
  switch(active)
  {
    case DT_IOP_GREEN_EQ_FULL:
      p->green_eq = DT_IOP_GREEN_EQ_FULL;
      break;
    case DT_IOP_GREEN_EQ_LOCAL:
      p->green_eq = DT_IOP_GREEN_EQ_LOCAL;
      break;
    case DT_IOP_GREEN_EQ_BOTH:
      p->green_eq = DT_IOP_GREEN_EQ_BOTH;
      break;
    default:
    case DT_IOP_GREEN_EQ_NO:
      p->green_eq = DT_IOP_GREEN_EQ_NO;
      break;
  }
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void demosaic_method_bayer_callback(GtkWidget *combo, dt_iop_module_t *self)
{
  dt_iop_demosaic_gui_data_t *g = (dt_iop_demosaic_gui_data_t *)self->gui_data;
  dt_iop_demosaic_params_t *p = (dt_iop_demosaic_params_t *)self->params;
  int active = dt_bauhaus_combobox_get(combo);

  switch(active)
  {
    case DT_IOP_DEMOSAIC_AMAZE:
      p->demosaicing_method = DT_IOP_DEMOSAIC_AMAZE;
      break;
    case DT_IOP_DEMOSAIC_VNG4:
      p->demosaicing_method = DT_IOP_DEMOSAIC_VNG4;
      break;
    case DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME:
      p->demosaicing_method = DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME;
      break;
    default:
    case DT_IOP_DEMOSAIC_PPG:
      p->demosaicing_method = DT_IOP_DEMOSAIC_PPG;
      break;
  }

  if(p->demosaicing_method == DT_IOP_DEMOSAIC_PASSTHROUGH_MONOCHROME)
  {
    gtk_widget_hide(g->median_thrs);
    gtk_widget_hide(g->color_smoothing);
    gtk_widget_hide(g->greeneq);
  }
  else if(p->demosaicing_method == DT_IOP_DEMOSAIC_AMAZE || p->demosaicing_method == DT_IOP_DEMOSAIC_VNG4)
  {
    gtk_widget_hide(g->median_thrs);
    gtk_widget_show(g->color_smoothing);
    gtk_widget_show(g->greeneq);
  }
  else
  {
    gtk_widget_show(g->median_thrs);
    gtk_widget_show(g->color_smoothing);
    gtk_widget_show(g->greeneq);
  }

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void demosaic_method_xtrans_callback(GtkWidget *combo, dt_iop_module_t *self)
{
  dt_iop_demosaic_params_t *p = (dt_iop_demosaic_params_t *)self->params;
  p->demosaicing_method = dt_bauhaus_combobox_get(combo) | DEMOSAIC_XTRANS;
  if((p->demosaicing_method > DT_IOP_DEMOSAIC_MARKESTEIJN_3) || (p->demosaicing_method < DT_IOP_DEMOSAIC_VNG))
    p->demosaicing_method = DT_IOP_DEMOSAIC_MARKESTEIJN;
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_demosaic_gui_data_t));
  dt_iop_demosaic_gui_data_t *g = (dt_iop_demosaic_gui_data_t *)self->gui_data;
  dt_iop_demosaic_params_t *p = (dt_iop_demosaic_params_t *)self->params;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  g->box_raw = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  g->demosaic_method_bayer = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->demosaic_method_bayer, NULL, _("method"));
  gtk_box_pack_start(GTK_BOX(g->box_raw), g->demosaic_method_bayer, TRUE, TRUE, 0);
  dt_bauhaus_combobox_add(g->demosaic_method_bayer, _("PPG (fast)"));
  dt_bauhaus_combobox_add(g->demosaic_method_bayer, _("AMaZE (slow)"));
  dt_bauhaus_combobox_add(g->demosaic_method_bayer, _("VNG4"));
  dt_bauhaus_combobox_add(g->demosaic_method_bayer, _("passthrough (monochrome) (experimental)"));
  gtk_widget_set_tooltip_text(g->demosaic_method_bayer, _("demosaicing raw data method"));

  g->demosaic_method_xtrans = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->demosaic_method_xtrans, NULL, _("method"));
  gtk_box_pack_start(GTK_BOX(g->box_raw), g->demosaic_method_xtrans, TRUE, TRUE, 0);
  dt_bauhaus_combobox_add(g->demosaic_method_xtrans, _("VNG"));
  dt_bauhaus_combobox_add(g->demosaic_method_xtrans, _("Markesteijn 1-pass"));
  dt_bauhaus_combobox_add(g->demosaic_method_xtrans, _("Markesteijn 3-pass (slow)"));
  gtk_widget_set_tooltip_text(g->demosaic_method_xtrans, _("demosaicing raw data method"));

  g->median_thrs = dt_bauhaus_slider_new_with_range(self, 0.0, 1.0, 0.001, p->median_thrs, 3);
  gtk_widget_set_tooltip_text(g->median_thrs, _("threshold for edge-aware median.\nset to 0.0 to switch off.\n"
                                                "set to 1.0 to ignore edges."));
  dt_bauhaus_widget_set_label(g->median_thrs, NULL, _("edge threshold"));
  gtk_box_pack_start(GTK_BOX(g->box_raw), g->median_thrs, TRUE, TRUE, 0);

  g->color_smoothing = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->color_smoothing, NULL, _("color smoothing"));
  gtk_box_pack_start(GTK_BOX(g->box_raw), g->color_smoothing, TRUE, TRUE, 0);
  dt_bauhaus_combobox_add(g->color_smoothing, _("off"));
  dt_bauhaus_combobox_add(g->color_smoothing, _("one time"));
  dt_bauhaus_combobox_add(g->color_smoothing, _("two times"));
  dt_bauhaus_combobox_add(g->color_smoothing, _("three times"));
  dt_bauhaus_combobox_add(g->color_smoothing, _("four times"));
  dt_bauhaus_combobox_add(g->color_smoothing, _("five times"));
  gtk_widget_set_tooltip_text(g->color_smoothing, _("how many color smoothing median steps after demosaicing"));

  g->greeneq = dt_bauhaus_combobox_new(self);
  gtk_box_pack_start(GTK_BOX(g->box_raw), g->greeneq, TRUE, TRUE, 0);
  dt_bauhaus_widget_set_label(g->greeneq, NULL, _("match greens"));
  dt_bauhaus_combobox_add(g->greeneq, _("disabled"));
  dt_bauhaus_combobox_add(g->greeneq, _("local average"));
  dt_bauhaus_combobox_add(g->greeneq, _("full average"));
  dt_bauhaus_combobox_add(g->greeneq, _("full and local average"));
  gtk_widget_set_tooltip_text(g->greeneq, _("green channels matching method"));

  g_signal_connect(G_OBJECT(g->median_thrs), "value-changed", G_CALLBACK(median_thrs_callback), self);
  g_signal_connect(G_OBJECT(g->color_smoothing), "value-changed", G_CALLBACK(color_smoothing_callback), self);
  g_signal_connect(G_OBJECT(g->greeneq), "value-changed", G_CALLBACK(greeneq_callback), self);
  g_signal_connect(G_OBJECT(g->demosaic_method_bayer), "value-changed",
                   G_CALLBACK(demosaic_method_bayer_callback), self);
  g_signal_connect(G_OBJECT(g->demosaic_method_xtrans), "value-changed",
                   G_CALLBACK(demosaic_method_xtrans_callback), self);

  gtk_box_pack_start(GTK_BOX(self->widget), g->box_raw, FALSE, FALSE, 0);

  g->label_non_raw = gtk_label_new(_("demosaicing\nonly needed for raw images."));
  gtk_widget_set_halign(g->label_non_raw, GTK_ALIGN_START);
  gtk_box_pack_start(GTK_BOX(self->widget), g->label_non_raw, FALSE, FALSE, 0);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
