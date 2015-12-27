/*
    This file is part of darktable,
    copyright (c) 2011-2012 Henrik Andersson, ulrich pegelow.

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
#include <stdlib.h>
#include <math.h>
#include <assert.h>
#include <string.h>
#ifdef HAVE_GEGL
#include <gegl.h>
#endif
#include "bauhaus/bauhaus.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/tiling.h"
#include "control/control.h"
#include "common/opencl.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include <gtk/gtk.h>
#include <inttypes.h>
/* Begin EFH */
#include "common/debug.h"
#include "control/conf.h"
#include "gui/draw.h"
#include "gui/presets.h"
#include "dtgtk/drawingarea.h"
#include <memory.h>
#include <xmmintrin.h>
/* End EFH */

#define MAX_RADIUS 16
#define BOX_ITERATIONS 8
#define BLOCKSIZE                                                                                            \
  2048 /* maximum blocksize. must be a power of 2 and will be automatically reduced if needed */

#define CLIP(x) ((x < 0) ? 0.0 : (x > 1.0) ? 1.0 : x)
#define LCLIP(x) ((x < 0) ? 0.0 : (x > 100.0) ? 100.0 : x)
/* Begin EFH */
#define INSET DT_PIXEL_APPLY_DPI(5)
#define INFL .3f
/* End EFH */

DT_MODULE_INTROSPECTION(1, dt_iop_highpass_params_t)

/* Begin EFH */
#define BANDS 6
#define MAX_NUM_SCALES 8 // 2*2^(i+1) + 1 = 1025px support for i = 8
#define RES 64

#define dt_atrous_show_upper_label(cr, text, ext)                                                            \
  cairo_text_extents(cr, text, &ext);                                                                        \
  cairo_move_to(cr, .5 * (width - ext.width), .08 * height);                                                 \
  cairo_show_text(cr, text);


#define dt_atrous_show_lower_label(cr, text, ext)                                                            \
  cairo_text_extents(cr, text, &ext);                                                                        \
  cairo_move_to(cr, .5 * (width - ext.width), .98 * height);                                                 \
  cairo_show_text(cr, text);


typedef enum atrous_channel_t
{
  atrous_L = 0,  // luminance boost
  atrous_c = 1,  // chrominance boost
  atrous_s = 2,  // edge sharpness
  atrous_Lt = 3, // luminance noise threshold
  atrous_ct = 4, // chrominance noise threshold
  atrous_none = 5
} atrous_channel_t;
/* End EFH */

typedef struct dt_iop_highpass_params_t
{
  float sharpness;
  float contrast;

  int tonecurve_autoscale_ab; // EFH

/* Begin EFH */
  int32_t octaves;
  float x[atrous_none][BANDS], y[atrous_none][BANDS];
/* End EFH */
} dt_iop_highpass_params_t;

typedef struct dt_iop_highpass_gui_data_t
{
  GtkBox *vbox1, *vbox2;
  GtkWidget *label1, *label2; // sharpness,contrast
  GtkWidget *scale1, *scale2; // sharpness,contrast

  GtkWidget *autoscale_ab; // EFH

/* Begin EFH */
  GtkWidget *mix;
  GtkDrawingArea *area;
  GtkNotebook *channel_tabs;
  double mouse_x, mouse_y, mouse_pick;
  float mouse_radius;
  dt_iop_highpass_params_t drag_params;
  int dragging;
  int x_move;
  dt_draw_curve_t *minmax_curve;
  atrous_channel_t channel, channel2;
  float draw_xs[RES], draw_ys[RES];
  float draw_min_xs[RES], draw_min_ys[RES];
  float draw_max_xs[RES], draw_max_ys[RES];
  float band_hist[MAX_NUM_SCALES];
  float band_max;
  float sample[MAX_NUM_SCALES];
  int num_samples;
/* End EFH */
} dt_iop_highpass_gui_data_t;

typedef struct dt_iop_highpass_data_t
{
  float sharpness;
  float contrast;

  int autoscale_ab; // EFH

  /* Begin EFH */
  // demosaic pattern
  int32_t octaves;
  dt_draw_curve_t *curve[atrous_none];
  /* End EFH */
} dt_iop_highpass_data_t;

typedef struct dt_iop_highpass_global_data_t
{
  int kernel_highpass_invert;
  int kernel_highpass_hblur;
  int kernel_highpass_vblur;
  int kernel_highpass_mix;
  /* Begin EFH */
  int kernel_decompose;
  int kernel_synthesize;
  /* End EFH */
} dt_iop_highpass_global_data_t;

/* Begin EFH */
/* End EFH */

/* Begin EFH */
/* End EFH */

const char *name()
{
  return _("highpass");
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING;
}

int groups()
{
  return IOP_GROUP_EFFECT;
}

#if 0 // BAUHAUS doesn't support keyaccels yet...
void init_key_accels(dt_iop_module_so_t *self)
{
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "sharpness"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "contrast boost"));
}

void connect_key_accels(dt_iop_module_t *self)
{
  dt_iop_highpass_gui_data_t *g =
    (dt_iop_highpass_gui_data_t*)self->gui_data;

  dt_accel_connect_slider_iop(self, "sharpness", GTK_WIDGET(g->scale1));
  dt_accel_connect_slider_iop(self, "contrast boost", GTK_WIDGET(g->scale2));
}
#endif

/* Begin EFH */
#define ALIGNED(a) __attribute__((aligned(a)))
#define VEC4(a)                                                                                              \
  {                                                                                                          \
    (a), (a), (a), (a)                                                                                       \
  }

static const __m128 fone ALIGNED(16) = VEC4(0x3f800000u);
static const __m128 femo ALIGNED(16) = VEC4(0x00adf880u);
static const __m128 ooo1 ALIGNED(16) = { 0.f, 0.f, 0.f, 1.f };

/* SSE intrinsics version of dt_fast_expf defined in darktable.h */
static inline __m128 dt_fast_expf_sse(const __m128 x)
{
  __m128 f = _mm_add_ps(fone, _mm_mul_ps(x, femo)); // f(n) = i1 + x(n)*(i2-i1)
  __m128i i = _mm_cvtps_epi32(f);                   // i(n) = int(f(n))
  __m128i mask = _mm_srai_epi32(i, 31);             // mask(n) = 0xffffffff if i(n) < 0
  i = _mm_andnot_si128(mask, i);                    // i(n) = 0 if i(n) < 0
  return _mm_castsi128_ps(i);                       // return *(float*)&i
}

/* Computes the vector
 * (wl, wc, wc, 1)
 *
 * where:
 * wl = exp(-sharpen*SQR(c1[0] - c2[0]))
 *    = exp(-s*d1) (as noted in code comments below)
 * wc = exp(-sharpen*(SQR(c1[1] - c2[1]) + SQR(c1[2] - c2[2]))
 *    = exp(-s*(d2+d3)) (as noted in code comments below)
 */
static inline __m128 weight_sse(const __m128 *c1, const __m128 *c2, const float sharpen)
{
  const __m128 vsharpen = _mm_set1_ps(-sharpen); // (-s, -s, -s, -s)
  __m128 diff = _mm_sub_ps(*c1, *c2);
  __m128 square = _mm_mul_ps(diff, diff);                                   // (?, d3, d2, d1)
  __m128 square2 = _mm_shuffle_ps(square, square, _MM_SHUFFLE(3, 1, 2, 0)); // (?, d2, d3, d1)
  __m128 added = _mm_add_ps(square, square2);                               // (?, d2+d3, d2+d3, 2*d1)
  added = _mm_sub_ss(added, square);                                        // (?, d2+d3, d2+d3, d1)
  __m128 sharpened = _mm_mul_ps(added, vsharpen);                   // (?, -s*(d2+d3), -s*(d2+d3), -s*d1)
  __m128 exp = dt_fast_expf_sse(sharpened);                         // (?, wc, wc, wl)
  exp = _mm_castsi128_ps(_mm_slli_si128(_mm_castps_si128(exp), 4)); // (wc, wc, wl, 0)
  exp = _mm_castsi128_ps(_mm_srli_si128(_mm_castps_si128(exp), 4)); // (0, wc, wc, wl)
  exp = _mm_or_ps(exp, ooo1);                                       // (1, wc, wc, wl)
  return exp;
}

#define SUM_PIXEL_CONTRIBUTION_COMMON(ii, jj)                                                                \
  do                                                                                                         \
  {                                                                                                          \
    const __m128 f = _mm_set1_ps(filter[(ii)] * filter[(jj)]);                                               \
    const __m128 wp = weight_sse(px, px2, sharpen);                                                          \
    const __m128 w = _mm_mul_ps(f, wp);                                                                      \
    const __m128 pd = _mm_mul_ps(w, *px2);                                                                   \
    sum = _mm_add_ps(sum, pd);                                                                               \
    wgt = _mm_add_ps(wgt, w);                                                                                \
  } while(0)

#define SUM_PIXEL_CONTRIBUTION_WITH_TEST(ii, jj)                                                             \
  do                                                                                                         \
  {                                                                                                          \
    const int iii = (ii)-2;                                                                                  \
    const int jjj = (jj)-2;                                                                                  \
    int x = i + mult * iii;                                                                                  \
    int y = j + mult * jjj;                                                                                  \
                                                                                                             \
    if(x < 0) x = 0;                                                                                         \
    if(x >= width) x = width - 1;                                                                            \
    if(y < 0) y = 0;                                                                                         \
    if(y >= height) y = height - 1;                                                                          \
                                                                                                             \
    px2 = ((__m128 *)in) + x + (size_t)y * width;                                                            \
                                                                                                             \
    SUM_PIXEL_CONTRIBUTION_COMMON(ii, jj);                                                                   \
  } while(0)

#define ROW_PROLOGUE                                                                                         \
  const __m128 *px = ((__m128 *)in) + (size_t)j * width;                                                     \
  const __m128 *px2;                                                                                         \
  float *pdetail = detail + (size_t)4 * j * width;                                                           \
  float *pcoarse = out + (size_t)4 * j * width;

#define SUM_PIXEL_PROLOGUE                                                                                   \
  __m128 sum = _mm_setzero_ps();                                                                             \
  __m128 wgt = _mm_setzero_ps();

#define SUM_PIXEL_EPILOGUE                                                                                   \
  sum = _mm_mul_ps(sum, _mm_rcp_ps(wgt));                                                                    \
                                                                                                             \
  _mm_stream_ps(pdetail, _mm_sub_ps(*px, sum));                                                              \
  _mm_stream_ps(pcoarse, sum);                                                                               \
  px++;                                                                                                      \
  pdetail += 4;                                                                                              \
  pcoarse += 4;

static void eaw_decompose(float *const out, const float *const in, float *const detail, const int scale,
                          const float sharpen, const int32_t width, const int32_t height)
{
  const int mult = 1 << scale;
  static const float filter[5] = { 1.0f / 16.0f, 4.0f / 16.0f, 6.0f / 16.0f, 4.0f / 16.0f, 1.0f / 16.0f };

/* The first "2*mult" lines use the macro with tests because the 5x5 kernel
 * requires nearest pixel interpolation for at least a pixel in the sum */
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static)
#endif
  for(int j = 0; j < 2 * mult; j++)
  {
    ROW_PROLOGUE

    for(int i = 0; i < width; i++)
    {
      SUM_PIXEL_PROLOGUE
      for(int jj = 0; jj < 5; jj++)
      {
        for(int ii = 0; ii < 5; ii++)
        {
          SUM_PIXEL_CONTRIBUTION_WITH_TEST(ii, jj);
        }
      }
      SUM_PIXEL_EPILOGUE
    }
  }

#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static)
#endif
  for(int j = 2 * mult; j < height - 2 * mult; j++)
  {
    ROW_PROLOGUE

    /* The first "2*mult" pixels use the macro with tests because the 5x5 kernel
     * requires nearest pixel interpolation for at least a pixel in the sum */
    for(int i = 0; i < 2 * mult; i++)
    {
      SUM_PIXEL_PROLOGUE
      for(int jj = 0; jj < 5; jj++)
      {
        for(int ii = 0; ii < 5; ii++)
        {
          SUM_PIXEL_CONTRIBUTION_WITH_TEST(ii, jj);
        }
      }
      SUM_PIXEL_EPILOGUE
    }

    /* For pixels [2*mult, width-2*mult], we can safely use macro w/o tests
     * to avoid unneeded branching in the inner loops */
    for(int i = 2 * mult; i < width - 2 * mult; i++)
    {
      SUM_PIXEL_PROLOGUE
      px2 = ((__m128 *)in) + i - 2 * mult + (size_t)(j - 2 * mult) * width;
      for(int jj = 0; jj < 5; jj++)
      {
        for(int ii = 0; ii < 5; ii++)
        {
          SUM_PIXEL_CONTRIBUTION_COMMON(ii, jj);
          px2 += mult;
        }
        px2 += (width - 5) * mult;
      }
      SUM_PIXEL_EPILOGUE
    }

    /* Last two pixels in the row require a slow variant... blablabla */
    for(int i = width - 2 * mult; i < width; i++)
    {
      SUM_PIXEL_PROLOGUE
      for(int jj = 0; jj < 5; jj++)
      {
        for(int ii = 0; ii < 5; ii++)
        {
          SUM_PIXEL_CONTRIBUTION_WITH_TEST(ii, jj);
        }
      }
      SUM_PIXEL_EPILOGUE
    }
  }

/* The last "2*mult" lines use the macro with tests because the 5x5 kernel
 * requires nearest pixel interpolation for at least a pixel in the sum */
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static)
#endif
  for(int j = height - 2 * mult; j < height; j++)
  {
    ROW_PROLOGUE

    for(int i = 0; i < width; i++)
    {
      SUM_PIXEL_PROLOGUE
      for(int jj = 0; jj < 5; jj++)
      {
        for(int ii = 0; ii < 5; ii++)
        {
          SUM_PIXEL_CONTRIBUTION_WITH_TEST(ii, jj);
        }
      }
      SUM_PIXEL_EPILOGUE
    }
  }

  _mm_sfence();
}

#undef SUM_PIXEL_CONTRIBUTION_COMMON
#undef SUM_PIXEL_CONTRIBUTION_WITH_TEST
#undef ROW_PROLOGUE
#undef SUM_PIXEL_PROLOGUE
#undef SUM_PIXEL_EPILOGUE

static void eaw_synthesize(float *const out, const float *const in, const float *const detail,
                           const float *thrsf, const float *boostf, const int32_t width, const int32_t height)
{
  const __m128 threshold = _mm_set_ps(thrsf[3], thrsf[2], thrsf[1], thrsf[0]);
  const __m128 boost = _mm_set_ps(boostf[3], boostf[2], boostf[1], boostf[0]);

#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static)
#endif
  for(int j = 0; j < height; j++)
  {
    // TODO: prefetch? _mm_prefetch()
    const __m128 *pin = (__m128 *)in + (size_t)j * width;
    __m128 *pdetail = (__m128 *)detail + (size_t)j * width;
    float *pout = out + (size_t)4 * j * width;
    for(int i = 0; i < width; i++)
    {
      const __m128i maski = _mm_set1_epi32(0x80000000u);
      const __m128 *mask = (__m128 *)&maski;
      const __m128 absamt
          = _mm_max_ps(_mm_setzero_ps(), _mm_sub_ps(_mm_andnot_ps(*mask, *pdetail), threshold));
      const __m128 amount = _mm_or_ps(_mm_and_ps(*pdetail, *mask), absamt);
      _mm_stream_ps(pout, _mm_add_ps(*pin, _mm_mul_ps(boost, amount)));
      pdetail++;
      pin++;
      pout += 4;
    }
  }
  _mm_sfence();
}

static int get_samples(float *t, const dt_iop_highpass_data_t *const d, const dt_iop_roi_t *roi_in,
                       const dt_dev_pixelpipe_iop_t *const piece)
{
  const float scale = roi_in->scale;
  const float supp0
      = MIN(2 * (2 << (MAX_NUM_SCALES - 1)) + 1, MAX(piece->buf_in.height, piece->buf_in.width) * 0.2f);
  const float i0 = dt_log2f((supp0 - 1.0f) * .5f);
  int i = 0;
  for(; i < MAX_NUM_SCALES; i++)
  {
    // actual filter support on scaled buffer
    const float supp = 2 * (2 << i) + 1;
    // approximates this filter size on unscaled input image:
    const float supp_in = supp * (1.0f / scale);
    const float i_in = dt_log2f((supp_in - 1) * .5f) - 1.0f;
    t[i] = 1.0f - (i_in + .5f) / i0;
    if(t[i] < 0.0f) break;
  }
  return i;
}

static int get_scales(float (*thrs)[4], float (*boost)[4], float *sharp, const dt_iop_highpass_data_t *const d,
                      const dt_iop_roi_t *roi_in, const dt_dev_pixelpipe_iop_t *const piece)
{
  // we want coeffs to span max 20% of the image
  // finest is 5x5 filter
  //
  // 1:1 : w=20% buf_in.width                     w=5x5
  //     : ^ ...            ....            ....  ^
  // buf :  17x17  9x9  5x5     2*2^k+1
  // .....
  // . . . . .
  // .   .   .   .   .
  // cut off too fine ones, if image is not detailed enough (due to roi_in->scale)
  const float scale = roi_in->scale / piece->iscale;
  // largest desired filter on input buffer (20% of input dim)
  const float supp0
      = MIN(2 * (2 << (MAX_NUM_SCALES - 1)) + 1,
            MAX(piece->buf_in.height * piece->iscale, piece->buf_in.width * piece->iscale) * 0.2f);
  const float i0 = dt_log2f((supp0 - 1.0f) * .5f);
  int i = 0;
  for(; i < MAX_NUM_SCALES; i++)
  {
    // actual filter support on scaled buffer
    const float supp = 2 * (2 << i) + 1;
    // approximates this filter size on unscaled input image:
    const float supp_in = supp * (1.0f / scale);
    const float i_in = dt_log2f((supp_in - 1) * .5f) - 1.0f;
    // i_in = max_scale .. .. .. 0
    const float t = 1.0f - (i_in + .5f) / i0;
    boost[i][3] = boost[i][0] = 2.0f * dt_draw_curve_calc_value(d->curve[atrous_L], t);
    boost[i][1] = boost[i][2] = 2.0f * dt_draw_curve_calc_value(d->curve[atrous_c], t);
    for(int k = 0; k < 4; k++) boost[i][k] *= boost[i][k];
    thrs[i][0] = thrs[i][3] = powf(2.0f, -7.0f * (1.0f - t)) * 10.0f
                              * dt_draw_curve_calc_value(d->curve[atrous_Lt], t);
    thrs[i][1] = thrs[i][2] = powf(2.0f, -7.0f * (1.0f - t)) * 20.0f
                              * dt_draw_curve_calc_value(d->curve[atrous_ct], t);
    sharp[i] = 0.0025f * dt_draw_curve_calc_value(d->curve[atrous_s], t);
    // printf("scale %d boost %f %f thrs %f %f sharpen %f\n", i, boost[i][0], boost[i][2], thrs[i][0],
    // thrs[i][1], sharp[i]);
    if(t < 0.0f) break;
  }
  return i;
}
/* End EFH */

void tiling_callback(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                     const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out,
                     struct dt_develop_tiling_t *tiling)
{
  dt_iop_highpass_data_t *d = (dt_iop_highpass_data_t *)piece->data;

  int rad = MAX_RADIUS * (fmin(100.0f, d->sharpness + 1) / 100.0f);
  const int radius = MIN(MAX_RADIUS, ceilf(rad * roi_in->scale / piece->iscale));

  const float sigma = sqrt((radius * (radius + 1) * BOX_ITERATIONS + 2) / 3.0f);
  const int wdh = ceilf(3.0f * sigma);

  tiling->factor = 3.0f; // in + out + tmp
  tiling->maxbuf = 1.0f;
  tiling->overhead = 0;
  tiling->overlap = wdh;
  tiling->xalign = 1;
  tiling->yalign = 1;

/* Begin EFH */
/* End EFH */

  return;
}


#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_highpass_data_t *d = (dt_iop_highpass_data_t *)piece->data;
  dt_iop_highpass_global_data_t *gd = (dt_iop_highpass_global_data_t *)self->data;

  cl_int err = -999;
  cl_mem dev_tmp = NULL;
  cl_mem dev_m = NULL;

  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;


  int rad = MAX_RADIUS * (fmin(100.0f, d->sharpness + 1) / 100.0f);
  const int radius = MIN(MAX_RADIUS, ceilf(rad * roi_in->scale / piece->iscale));

  /* sigma-radius correlation to match opencl vs. non-opencl. identified by numerical experiments but
   * unproven. ask me if you need details. ulrich */
  const float sigma = sqrt((radius * (radius + 1) * BOX_ITERATIONS + 2) / 3.0f);
  const int wdh = ceilf(3.0f * sigma);
  const int wd = 2 * wdh + 1;
  float mat[wd];
  float *m = mat + wdh;
  float weight = 0.0f;

  // init gaussian kernel
  for(int l = -wdh; l <= wdh; l++) weight += m[l] = expf(-(l * l) / (2.f * sigma * sigma));
  for(int l = -wdh; l <= wdh; l++) m[l] /= weight;

  // for(int l=-wdh; l<=wdh; l++) printf("%.6f ", (double)m[l]);
  // printf("\n");

  float contrast_scale = ((d->contrast / 100.0f) * 7.5f);

  size_t maxsizes[3] = { 0 };     // the maximum dimensions for a work group
  size_t workgroupsize = 0;       // the maximum number of items in a work group
  unsigned long localmemsize = 0; // the maximum amount of local memory we can use
  size_t kernelworkgroupsize = 0; // the maximum amount of items in work group for this kernel

  // make sure blocksize is not too large
  int blocksize = BLOCKSIZE;
  if(dt_opencl_get_work_group_limits(devid, maxsizes, &workgroupsize, &localmemsize) == CL_SUCCESS
     && dt_opencl_get_kernel_work_group_size(devid, gd->kernel_highpass_hblur, &kernelworkgroupsize)
        == CL_SUCCESS)
  {
    // reduce blocksize step by step until it fits to limits
    while(blocksize > maxsizes[0] || blocksize > maxsizes[1] || blocksize > kernelworkgroupsize
          || blocksize > workgroupsize || (blocksize + 2 * wdh) * sizeof(float) > localmemsize)
    {
      if(blocksize == 1) break;
      blocksize >>= 1;
    }
  }
  else
  {
    blocksize = 1; // slow but safe
  }

  const size_t bwidth = width % blocksize == 0 ? width : (width / blocksize + 1) * blocksize;
  const size_t bheight = height % blocksize == 0 ? height : (height / blocksize + 1) * blocksize;

  size_t sizes[3];
  size_t local[3];

  dev_tmp = dt_opencl_alloc_device(devid, width, height, 4 * sizeof(float));
  if(dev_tmp == NULL) goto error;

  dev_m = dt_opencl_copy_host_to_device_constant(devid, (size_t)sizeof(float) * wd, mat);
  if(dev_m == NULL) goto error;

  /* invert image */
  sizes[0] = ROUNDUPWD(width);
  sizes[1] = ROUNDUPHT(height);
  sizes[2] = 1;
  dt_opencl_set_kernel_arg(devid, gd->kernel_highpass_invert, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highpass_invert, 1, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highpass_invert, 2, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highpass_invert, 3, sizeof(int), (void *)&height);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_highpass_invert, sizes);
  if(err != CL_SUCCESS) goto error;

  if(rad != 0)
  {
    /* horizontal blur */
    sizes[0] = bwidth;
    sizes[1] = ROUNDUPHT(height);
    sizes[2] = 1;
    local[0] = blocksize;
    local[1] = 1;
    local[2] = 1;
    dt_opencl_set_kernel_arg(devid, gd->kernel_highpass_hblur, 0, sizeof(cl_mem), (void *)&dev_out);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highpass_hblur, 1, sizeof(cl_mem), (void *)&dev_tmp);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highpass_hblur, 2, sizeof(cl_mem), (void *)&dev_m);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highpass_hblur, 3, sizeof(int), (void *)&wdh);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highpass_hblur, 4, sizeof(int), (void *)&width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highpass_hblur, 5, sizeof(int), (void *)&height);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highpass_hblur, 6, sizeof(int), (void *)&blocksize);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highpass_hblur, 7, (blocksize + 2 * wdh) * sizeof(float), NULL);
    err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_highpass_hblur, sizes, local);
    if(err != CL_SUCCESS) goto error;


    /* vertical blur */
    sizes[0] = ROUNDUPWD(width);
    sizes[1] = bheight;
    sizes[2] = 1;
    local[0] = 1;
    local[1] = blocksize;
    local[2] = 1;
    dt_opencl_set_kernel_arg(devid, gd->kernel_highpass_vblur, 0, sizeof(cl_mem), (void *)&dev_tmp);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highpass_vblur, 1, sizeof(cl_mem), (void *)&dev_out);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highpass_vblur, 2, sizeof(cl_mem), (void *)&dev_m);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highpass_vblur, 3, sizeof(int), (void *)&wdh);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highpass_vblur, 4, sizeof(int), (void *)&width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highpass_vblur, 5, sizeof(int), (void *)&height);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highpass_vblur, 6, sizeof(int), (void *)&blocksize);
    dt_opencl_set_kernel_arg(devid, gd->kernel_highpass_vblur, 7, (blocksize + 2 * wdh) * sizeof(float), NULL);
    err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_highpass_vblur, sizes, local);
    if(err != CL_SUCCESS) goto error;
  }

  /* mixing out and in -> out */
  sizes[0] = ROUNDUPWD(width);
  sizes[1] = ROUNDUPHT(height);
  sizes[2] = 1;
  dt_opencl_set_kernel_arg(devid, gd->kernel_highpass_mix, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highpass_mix, 1, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highpass_mix, 2, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highpass_mix, 3, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highpass_mix, 4, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_highpass_mix, 5, sizeof(float), (void *)&contrast_scale);

/* Begin EFH */
  {
  float thrs[MAX_NUM_SCALES][4];
  float boost[MAX_NUM_SCALES][4];
  float sharp[MAX_NUM_SCALES];
  const int max_scale = get_scales(thrs, boost, sharp, d, roi_in, piece);

  if(self->dev->gui_attached && piece->pipe->type == DT_DEV_PIXELPIPE_FULL)
  {
    dt_iop_highpass_gui_data_t *g = (dt_iop_highpass_gui_data_t *)self->gui_data;
    g->num_samples = get_samples(g->sample, d, roi_in, piece);
    // dt_control_queue_redraw_widget(GTK_WIDGET(g->area));
    // tries to acquire gdk lock and this prone to deadlock:
    // dt_control_queue_draw(GTK_WIDGET(g->area));
  }

  dt_iop_highpass_global_data_t *gd = (dt_iop_highpass_global_data_t *)self->data;

  const int devid = piece->pipe->devid;
  cl_int err = -999;
  cl_mem dev_filter = NULL;
  cl_mem dev_tmp = NULL;
  cl_mem dev_detail[max_scale];
  for(int k = 0; k < max_scale; k++) dev_detail[k] = NULL;

  float m[] = { 0.0625f, 0.25f, 0.375f, 0.25f, 0.0625f }; // 1/16, 4/16, 6/16, 4/16, 1/16
  float mm[5][5];
  for(int j = 0; j < 5; j++)
    for(int i = 0; i < 5; i++) mm[j][i] = m[i] * m[j];

  dev_filter = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * 25, mm);
  if(dev_filter == NULL) goto error;

  /* allocate space for a temporary buffer. we don't want to use dev_in in the buffer ping-pong below, as we
     need to keep it for blendops */
  dev_tmp = dt_opencl_alloc_device(devid, roi_out->width, roi_out->height, 4 * sizeof(float));
  if(dev_tmp == NULL) goto error;

  /* allocate space to store detail information. Requires a number of additional buffers, each with full image
   * size */
  for(int k = 0; k < max_scale; k++)
  {
    dev_detail[k] = dt_opencl_alloc_device(devid, roi_out->width, roi_out->height, 4 * sizeof(float));
    if(dev_detail[k] == NULL) goto error;
  }

  const int width = roi_out->width;
  const int height = roi_out->height;
  size_t sizes[] = { ROUNDUPWD(width), ROUNDUPHT(height), 1 };
  size_t origin[] = { 0, 0, 0 };
  size_t region[] = { width, height, 1 };

  // copy original input from dev_in -> dev_out as starting point
  err = dt_opencl_enqueue_copy_image(devid, dev_in, dev_out, origin, origin, region);
  if(err != CL_SUCCESS) goto error;

  /* decompose image into detail scales and coarse (the latter is left in dev_tmp or dev_out) */
  for(int s = 0; s < max_scale; s++)
  {
    const int scale = s;

    if(s & 1)
    {
      dt_opencl_set_kernel_arg(devid, gd->kernel_decompose, 0, sizeof(cl_mem), (void *)&dev_tmp);
      dt_opencl_set_kernel_arg(devid, gd->kernel_decompose, 1, sizeof(cl_mem), (void *)&dev_out);
    }
    else
    {
      dt_opencl_set_kernel_arg(devid, gd->kernel_decompose, 0, sizeof(cl_mem), (void *)&dev_out);
      dt_opencl_set_kernel_arg(devid, gd->kernel_decompose, 1, sizeof(cl_mem), (void *)&dev_tmp);
    }
    dt_opencl_set_kernel_arg(devid, gd->kernel_decompose, 2, sizeof(cl_mem), (void *)&dev_detail[s]);
    dt_opencl_set_kernel_arg(devid, gd->kernel_decompose, 3, sizeof(int), (void *)&width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_decompose, 4, sizeof(int), (void *)&height);
    dt_opencl_set_kernel_arg(devid, gd->kernel_decompose, 5, sizeof(unsigned int), (void *)&scale);
    dt_opencl_set_kernel_arg(devid, gd->kernel_decompose, 6, sizeof(float), (void *)&sharp[s]);
    dt_opencl_set_kernel_arg(devid, gd->kernel_decompose, 7, sizeof(cl_mem), (void *)&dev_filter);

    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_decompose, sizes);
    if(err != CL_SUCCESS) goto error;

    // indirectly give gpu some air to breathe (and to do display related stuff)
    dt_iop_nap(darktable.opencl->micro_nap);
  }

  /* now synthesize again */
  for(int scale = max_scale - 1; scale >= 0; scale--)
  {
    if(scale & 1)
    {
      dt_opencl_set_kernel_arg(devid, gd->kernel_synthesize, 0, sizeof(cl_mem), (void *)&dev_tmp);
      dt_opencl_set_kernel_arg(devid, gd->kernel_synthesize, 1, sizeof(cl_mem), (void *)&dev_out);
    }
    else
    {
      dt_opencl_set_kernel_arg(devid, gd->kernel_synthesize, 0, sizeof(cl_mem), (void *)&dev_out);
      dt_opencl_set_kernel_arg(devid, gd->kernel_synthesize, 1, sizeof(cl_mem), (void *)&dev_tmp);
    }

    dt_opencl_set_kernel_arg(devid, gd->kernel_synthesize, 2, sizeof(cl_mem), (void *)&dev_detail[scale]);
    dt_opencl_set_kernel_arg(devid, gd->kernel_synthesize, 3, sizeof(int), (void *)&width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_synthesize, 4, sizeof(int), (void *)&height);
    dt_opencl_set_kernel_arg(devid, gd->kernel_synthesize, 5, sizeof(float), (void *)&thrs[scale][0]);
    dt_opencl_set_kernel_arg(devid, gd->kernel_synthesize, 6, sizeof(float), (void *)&thrs[scale][1]);
    dt_opencl_set_kernel_arg(devid, gd->kernel_synthesize, 7, sizeof(float), (void *)&thrs[scale][2]);
    dt_opencl_set_kernel_arg(devid, gd->kernel_synthesize, 8, sizeof(float), (void *)&thrs[scale][3]);
    dt_opencl_set_kernel_arg(devid, gd->kernel_synthesize, 9, sizeof(float), (void *)&boost[scale][0]);
    dt_opencl_set_kernel_arg(devid, gd->kernel_synthesize, 10, sizeof(float), (void *)&boost[scale][1]);
    dt_opencl_set_kernel_arg(devid, gd->kernel_synthesize, 11, sizeof(float), (void *)&boost[scale][2]);
    dt_opencl_set_kernel_arg(devid, gd->kernel_synthesize, 12, sizeof(float), (void *)&boost[scale][3]);

    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_synthesize, sizes);
    if(err != CL_SUCCESS) goto error;

    // indirectly give gpu some air to breathe (and to do display related stuff)
    dt_iop_nap(darktable.opencl->micro_nap);
  }

/*  if(!darktable.opencl->async_pixelpipe || piece->pipe->type == DT_DEV_PIXELPIPE_EXPORT)
    dt_opencl_finish(devid);

  if(dev_filter != NULL) dt_opencl_release_mem_object(dev_filter);
  if(dev_tmp != NULL) dt_opencl_release_mem_object(dev_tmp);
  for(int k = 0; k < max_scale; k++)
    if(dev_detail[k] != NULL) dt_opencl_release_mem_object(dev_detail[k]);
  return TRUE;*/
  }
/* End EFH */

  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_highpass_mix, sizes);
  if(err != CL_SUCCESS) goto error;

  if(dev_m != NULL) dt_opencl_release_mem_object(dev_m);
  if(dev_tmp != NULL) dt_opencl_release_mem_object(dev_tmp);
  return TRUE;

error:
  if(dev_m != NULL) dt_opencl_release_mem_object(dev_m);
  if(dev_tmp != NULL) dt_opencl_release_mem_object(dev_tmp);
  dt_print(DT_DEBUG_OPENCL, "[opencl_highpass] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif

/* Begin EFH */
/* just process the supplied image buffer, upstream default_process_tiling() does the rest */
void process_equ(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, void *i, void *o,
             const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_highpass_data_t *d = (dt_iop_highpass_data_t *)piece->data;
  float thrs[MAX_NUM_SCALES][4];
  float boost[MAX_NUM_SCALES][4];
  float sharp[MAX_NUM_SCALES];
  const int max_scale = get_scales(thrs, boost, sharp, d, roi_in, piece);

  if(self->dev->gui_attached && piece->pipe->type == DT_DEV_PIXELPIPE_FULL)
  {
    dt_iop_highpass_gui_data_t *g = (dt_iop_highpass_gui_data_t *)self->gui_data;
    g->num_samples = get_samples(g->sample, d, roi_in, piece);
    // tries to acquire gdk lock and this prone to deadlock:
    // dt_control_queue_draw(GTK_WIDGET(g->area));
  }

  float *detail[MAX_NUM_SCALES] = { NULL };
  float *tmp = NULL;
  float *buf2 = NULL;
  float *buf1 = NULL;

  const int width = roi_out->width;
  const int height = roi_out->height;

  tmp = (float *)dt_alloc_align(64, (size_t)sizeof(float) * 4 * width * height);
  if(tmp == NULL)
  {
    fprintf(stderr, "[atrous] failed to allocate coarse buffer!\n");
    goto error;
  }

  for(int k = 0; k < max_scale; k++)
  {
    detail[k] = (float *)dt_alloc_align(64, (size_t)sizeof(float) * 4 * width * height);
    if(detail[k] == NULL)
    {
      fprintf(stderr, "[atrous] failed to allocate one of the detail buffers!\n");
      goto error;
    }
  }

  buf1 = (float *)i;
  buf2 = tmp;

  for(int scale = 0; scale < max_scale; scale++)
  {
    eaw_decompose(buf2, buf1, detail[scale], scale, sharp[scale], width, height);
    if(scale == 0) buf1 = (float *)o; // now switch to (float *)o for buffer ping-pong between buf1 and buf2
    float *buf3 = buf2;
    buf2 = buf1;
    buf1 = buf3;
  }

  for(int scale = max_scale - 1; scale >= 0; scale--)
  {
    eaw_synthesize(buf2, buf1, detail[scale], thrs[scale], boost[scale], width, height);
    float *buf3 = buf2;
    buf2 = buf1;
    buf1 = buf3;
  }
  /* due to symmetric processing, output will be left in (float *)o */

  for(int k = 0; k < max_scale; k++) dt_free_align(detail[k]);
  dt_free_align(tmp);

  if(piece->pipe->mask_display) dt_iop_alpha_copy(i, o, width, height);

  return;

error:
  for(int k = 0; k < max_scale; k++)
    if(detail[k] != NULL) dt_free_align(detail[k]);
  if(tmp != NULL) dt_free_align(tmp);
  return;
}
/* End EFH */

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *ivoid, void *ovoid,
             const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_highpass_data_t *data = (dt_iop_highpass_data_t *)piece->data;
  float *in = (float *)ivoid;
  float *out = (float *)ovoid;
  const int ch = piece->colors;

// create inverted image and then blur
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(in, out, roi_out) schedule(static)
#endif
  for(size_t k = 0; k < (size_t)roi_out->width * roi_out->height; k++)
    out[ch * k] = 100.0f - LCLIP(in[ch * k]); // only L in Lab space


  int rad = MAX_RADIUS * (fmin(100.0, data->sharpness + 1) / 100.0);
  const int radius = MIN(MAX_RADIUS, ceilf(rad * roi_in->scale / piece->iscale));

  // horizontal blur out into out
  const int range = 2 * radius + 1;
  const int hr = range / 2;

  const int size = roi_out->width > roi_out->height ? roi_out->width : roi_out->height;
  float *scanline = calloc(size, sizeof(float));

  for(int iteration = 0; iteration < BOX_ITERATIONS; iteration++)
  {
    for(int y = 0; y < roi_out->height; y++)
    {
      float L = 0;
      int hits = 0;
      size_t index = (size_t)y * roi_out->width;
      for(int x = -hr; x < roi_out->width; x++)
      {
        int op = x - hr - 1;
        int np = x + hr;
        if(op >= 0)
        {
          L -= out[(index + op) * ch];
          hits--;
        }
        if(np < roi_out->width)
        {
          L += out[(index + np) * ch];
          hits++;
        }
        if(x >= 0) scanline[x] = L / hits;
      }

      for(int x = 0; x < roi_out->width; x++) out[(index + x) * ch] = scanline[x];
    }

    // vertical pass on blurlightness
    const int opoffs = -(hr + 1) * roi_out->width;
    const int npoffs = (hr)*roi_out->width;
    for(int x = 0; x < roi_out->width; x++)
    {
      float L = 0;
      int hits = 0;
      size_t index = (size_t)x - hr * roi_out->width;
      for(int y = -hr; y < roi_out->height; y++)
      {
        int op = y - hr - 1;
        int np = y + hr;
        if(op >= 0)
        {
          L -= out[(index + opoffs) * ch];
          hits--;
        }
        if(np < roi_out->height)
        {
          L += out[(index + npoffs) * ch];
          hits++;
        }
        if(y >= 0) scanline[y] = L / hits;
        index += roi_out->width;
      }

      for(int y = 0; y < roi_out->height; y++) out[((size_t)y * roi_out->width + x) * ch] = scanline[y];
    }
  }

  free(scanline);

  const float contrast_scale = ((data->contrast / 100.0) * 7.5);
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(roi_out, in, out, data) schedule(static)
#endif
  for(size_t k = 0; k < (size_t)roi_out->width * roi_out->height; k++)
  {
    size_t index = ch * k;
    // Mix out and in
    out[index] = out[index] * 0.5 + in[index] * 0.5;
    out[index] = LCLIP(50.0f + ((out[index] - 50.0f) * contrast_scale));
    out[index + 1] = out[index + 2] = 0.0f; // desaturate a and b in Lab space
    out[index + 3] = in[index + 3];
  }

  /* Begin EFH */
  if (data->autoscale_ab)
  {
	  void *tmp = NULL;
	  tmp = malloc((size_t)sizeof(float) * roi_out->width * roi_out->height * 4);
	  if (tmp)
	  {
		  memcpy(tmp, ovoid, (size_t)sizeof(float) * roi_out->width * roi_out->height * 4);
		  process_equ(self, piece, tmp, ovoid, roi_in, roi_out);
		  free(tmp);
	  }
  }
  /* End EFH */
}

/* Begin EFH */
/* End EFH */

static void sharpness_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_highpass_params_t *p = (dt_iop_highpass_params_t *)self->params;
  p->sharpness = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void contrast_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_highpass_params_t *p = (dt_iop_highpass_params_t *)self->params;
  p->contrast = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}


void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_highpass_params_t *p = (dt_iop_highpass_params_t *)p1;
#ifdef HAVE_GEGL
  fprintf(stderr, "[highpass] TODO: implement gegl version!\n");
// pull in new params to gegl
#else
  dt_iop_highpass_data_t *d = (dt_iop_highpass_data_t *)piece->data;
  d->sharpness = p->sharpness;
  d->contrast = p->contrast;
  d->autoscale_ab = p->tonecurve_autoscale_ab; // EFH
#endif
/* Begin EFH */
#if 0
  printf("---------- atrous preset begin\n");
  printf("p.octaves = %d;\n", p->octaves);
  for(int ch=0; ch<atrous_none; ch++) for(int k=0; k<BANDS; k++)
    {
      printf("p.x[%d][%d] = %f;\n", ch, k, p->x[ch][k]);
      printf("p.y[%d][%d] = %f;\n", ch, k, p->y[ch][k]);
    }
  printf("---------- atrous preset end\n");
#endif
  d->octaves = p->octaves;
  for(int ch = 0; ch < atrous_none; ch++)
    for(int k = 0; k < BANDS; k++) dt_draw_curve_set_point(d->curve[ch], k, p->x[ch][k], p->y[ch][k]);
  int l = 0;
  for(int k = (int)MIN(pipe->iwidth * pipe->iscale, pipe->iheight * pipe->iscale); k; k >>= 1) l++;
  d->octaves = MIN(BANDS, l);
/* End EFH */
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
#ifdef HAVE_GEGL
  // create part of the gegl pipeline
  piece->data = NULL;
#else
  piece->data = calloc(1, sizeof(dt_iop_highpass_data_t));
  /* Begin EFH */
  dt_iop_highpass_data_t *d = piece->data;
  dt_iop_highpass_params_t *default_params = (dt_iop_highpass_params_t *)self->default_params;
  for(int ch = 0; ch < atrous_none; ch++)
  {
    d->curve[ch] = dt_draw_curve_new(0.0, 1.0, CATMULL_ROM);
    for(int k = 0; k < BANDS; k++)
      (void)dt_draw_curve_add_point(d->curve[ch], default_params->x[ch][k], default_params->y[ch][k]);
  }
  int l = 0;
  for(int k = (int)MIN(pipe->iwidth * pipe->iscale, pipe->iheight * pipe->iscale); k; k >>= 1) l++;
  d->octaves = MIN(BANDS, l);
  /* End EFH */
  self->commit_params(self, self->default_params, pipe, piece);
#endif
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
#ifdef HAVE_GEGL
  // clean up everything again.
  (void)gegl_node_remove_child(pipe->gegl, piece->input);
// no free necessary, no data is alloc'ed
#else
  /* Begin EFH */
  dt_iop_highpass_data_t *d = (dt_iop_highpass_data_t *)(piece->data);
  for(int ch = 0; ch < atrous_none; ch++) dt_draw_curve_destroy(d->curve[ch]);
  /* End EFH */
  free(piece->data);
  piece->data = NULL;
#endif
}

/* Begin EFH */
void init_presets(dt_iop_module_so_t *self)
{
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "begin", NULL, NULL, NULL);
  dt_iop_highpass_params_t p;
  p.octaves = 7;

  for(int k = 0; k < BANDS; k++)
  {
    p.x[atrous_L][k] = k / (BANDS - 1.0);
    p.x[atrous_c][k] = k / (BANDS - 1.0);
    p.x[atrous_s][k] = k / (BANDS - 1.0);
    p.y[atrous_L][k] = fmaxf(.5f, .75f - .5f * k / (BANDS - 1.0));
    p.y[atrous_c][k] = fmaxf(.5f, .55f - .5f * k / (BANDS - 1.0));
    p.y[atrous_s][k] = fminf(.5f, .2f + .35f * k / (BANDS - 1.0));
    p.x[atrous_Lt][k] = k / (BANDS - 1.0);
    p.x[atrous_ct][k] = k / (BANDS - 1.0);
    p.y[atrous_Lt][k] = 0.0f;
    p.y[atrous_ct][k] = 0.0f;
  }
  dt_gui_presets_add_generic(C_("eq_preset", "coarse"), self->op, self->version(), &p, sizeof(p), 1);
  for(int k = 0; k < BANDS; k++)
  {
    p.x[atrous_L][k] = k / (BANDS - 1.0);
    p.x[atrous_c][k] = k / (BANDS - 1.0);
    p.x[atrous_s][k] = k / (BANDS - 1.0);
    p.y[atrous_L][k] = .5f + .25f * k / (float)BANDS;
    p.y[atrous_c][k] = .5f;
    p.y[atrous_s][k] = .5f;
    p.x[atrous_Lt][k] = k / (BANDS - 1.0);
    p.x[atrous_ct][k] = k / (BANDS - 1.0);
    p.y[atrous_Lt][k] = .2f * k / (float)BANDS;
    p.y[atrous_ct][k] = .3f * k / (float)BANDS;
  }
  dt_gui_presets_add_generic(_("denoise & sharpen"), self->op, self->version(), &p, sizeof(p), 1);
  for(int k = 0; k < BANDS; k++)
  {
    p.x[atrous_L][k] = k / (BANDS - 1.0);
    p.x[atrous_c][k] = k / (BANDS - 1.0);
    p.x[atrous_s][k] = k / (BANDS - 1.0);
    p.y[atrous_L][k] = .5f + .25f * k / (float)BANDS;
    p.y[atrous_c][k] = .5f;
    p.y[atrous_s][k] = .5f;
    p.x[atrous_Lt][k] = k / (BANDS - 1.0);
    p.x[atrous_ct][k] = k / (BANDS - 1.0);
    p.y[atrous_Lt][k] = 0.0f;
    p.y[atrous_ct][k] = 0.0f;
  }
  dt_gui_presets_add_generic(C_("atrous", "sharpen"), self->op, self->version(), &p, sizeof(p), 1);
  for(int k = 0; k < BANDS; k++)
  {
    p.x[atrous_L][k] = k / (BANDS - 1.0);
    p.x[atrous_c][k] = k / (BANDS - 1.0);
    p.x[atrous_s][k] = k / (BANDS - 1.0);
    p.y[atrous_L][k] = .5f;
    p.y[atrous_c][k] = .5f;
    p.y[atrous_s][k] = .0f;
    p.x[atrous_Lt][k] = k / (BANDS - 1.0);
    p.x[atrous_ct][k] = k / (BANDS - 1.0);
    p.y[atrous_Lt][k] = .0f;
    p.y[atrous_ct][k] = fmaxf(0.0f, (.60f * k / (float)BANDS) - 0.30f);
  }
  dt_gui_presets_add_generic(_("denoise chroma"), self->op, self->version(), &p, sizeof(p), 1);
  for(int k = 0; k < BANDS; k++)
  {
    p.x[atrous_L][k] = k / (BANDS - 1.0);
    p.x[atrous_c][k] = k / (BANDS - 1.0);
    p.x[atrous_s][k] = k / (BANDS - 1.0);
    p.y[atrous_L][k] = .5f; //-.2f*k/(float)BANDS;
    p.y[atrous_c][k] = .5f; // fmaxf(0.0f, .5f-.3f*k/(float)BANDS);
    p.y[atrous_s][k] = .5f;
    p.x[atrous_Lt][k] = k / (BANDS - 1.0);
    p.x[atrous_ct][k] = k / (BANDS - 1.0);
    p.y[atrous_Lt][k] = .2f * k / (float)BANDS;
    p.y[atrous_ct][k] = .3f * k / (float)BANDS;
  }
  dt_gui_presets_add_generic(_("denoise"), self->op, self->version(), &p, sizeof(p), 1);
  for(int k = 0; k < BANDS; k++)
  {
    p.x[atrous_L][k] = k / (BANDS - 1.0);
    p.x[atrous_c][k] = k / (BANDS - 1.0);
    p.x[atrous_s][k] = k / (BANDS - 1.0);
    p.y[atrous_L][k] = fminf(.5f, .3f + .35f * k / (BANDS - 1.0));
    p.y[atrous_c][k] = .5f;
    p.y[atrous_s][k] = .0f;
    p.x[atrous_Lt][k] = k / (BANDS - 1.0);
    p.x[atrous_ct][k] = k / (BANDS - 1.0);
    p.y[atrous_Lt][k] = 0.0f;
    p.y[atrous_ct][k] = 0.0f;
  }
  p.y[atrous_L][0] = .5f;
  dt_gui_presets_add_generic(_("bloom"), self->op, self->version(), &p, sizeof(p), 1);
  for(int k = 0; k < BANDS; k++)
  {
    p.x[atrous_L][k] = k / (BANDS - 1.0);
    p.x[atrous_c][k] = k / (BANDS - 1.0);
    p.x[atrous_s][k] = k / (BANDS - 1.0);
    p.y[atrous_L][k] = 0.6f;
    p.y[atrous_c][k] = .55f;
    p.y[atrous_s][k] = .0f;
    p.x[atrous_Lt][k] = k / (BANDS - 1.0);
    p.x[atrous_ct][k] = k / (BANDS - 1.0);
    p.y[atrous_Lt][k] = 0.0f;
    p.y[atrous_ct][k] = 0.0f;
  }
  dt_gui_presets_add_generic(_("clarity"), self->op, self->version(), &p, sizeof(p), 1);
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), "commit", NULL, NULL, NULL);
}

static void reset_mix(dt_iop_module_t *self)
{
  dt_iop_highpass_gui_data_t *c = (dt_iop_highpass_gui_data_t *)self->gui_data;
  c->drag_params = *(dt_iop_highpass_params_t *)self->params;
  const int old = self->dt->gui->reset;
  self->dt->gui->reset = 1;
  dt_bauhaus_slider_set(c->mix, 1.0f);
  self->dt->gui->reset = old;
}
/* End EFH */

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_module_t *module = (dt_iop_module_t *)self;
  dt_iop_highpass_gui_data_t *g = (dt_iop_highpass_gui_data_t *)self->gui_data;
  dt_iop_highpass_params_t *p = (dt_iop_highpass_params_t *)module->params;
  dt_bauhaus_slider_set(g->scale1, p->sharpness);
  dt_bauhaus_slider_set(g->scale2, p->contrast);

  dt_bauhaus_combobox_set(g->autoscale_ab, p->tonecurve_autoscale_ab); // EFH

  /* Begin EFH */
  reset_mix(self);
  gtk_widget_queue_draw(self->widget);
  /* End EFH */
}

/* Begin EFH */
// gui stuff:

static gboolean area_enter_notify(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_highpass_gui_data_t *c = (dt_iop_highpass_gui_data_t *)self->gui_data;
  if(!c->dragging) c->mouse_y = fabs(c->mouse_y);
  gtk_widget_queue_draw(widget);
  return TRUE;
}

static gboolean area_leave_notify(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_highpass_gui_data_t *c = (dt_iop_highpass_gui_data_t *)self->gui_data;
  if(!c->dragging) c->mouse_y = -fabs(c->mouse_y);
  gtk_widget_queue_draw(widget);
  return TRUE;
}


// fills in new parameters based on mouse position (in 0,1)
static void get_params(dt_iop_highpass_params_t *p, const int ch, const double mouse_x, const double mouse_y,
                       const float rad)
{
  for(int k = 0; k < BANDS; k++)
  {
    const float f = expf(-(mouse_x - p->x[ch][k]) * (mouse_x - p->x[ch][k]) / (rad * rad));
    p->y[ch][k] = MAX(0.0f, MIN(1.0f, (1 - f) * p->y[ch][k] + f * mouse_y));
  }
}

static gboolean area_draw(GtkWidget *widget, cairo_t *crf, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_highpass_gui_data_t *c = (dt_iop_highpass_gui_data_t *)self->gui_data;
  dt_iop_highpass_params_t p = *(dt_iop_highpass_params_t *)self->params;
  int ch = (int)c->channel;
  int ch2 = (int)c->channel2;
  for(int k = 0; k < BANDS; k++) dt_draw_curve_set_point(c->minmax_curve, k, p.x[ch2][k], p.y[ch2][k]);
  const int inset = INSET;
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  int width = allocation.width, height = allocation.height;
  cairo_surface_t *cst = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);
  // clear bg, match color of the notebook tabs:
  GdkRGBA bright_bg_color, really_dark_bg_color;
  GtkStyleContext *context = gtk_widget_get_style_context(self->expander);
  gboolean color_found = gtk_style_context_lookup_color (context, "selected_bg_color", &bright_bg_color);
  if(!color_found)
  {
    bright_bg_color.red = 1.0;
    bright_bg_color.green = 0.0;
    bright_bg_color.blue = 0.0;
    bright_bg_color.alpha = 1.0;
  }

  color_found = gtk_style_context_lookup_color (context, "really_dark_bg_color", &really_dark_bg_color);
  if(!color_found)
  {
    really_dark_bg_color.red = 1.0;
    really_dark_bg_color.green = 0.0;
    really_dark_bg_color.blue = 0.0;
    really_dark_bg_color.alpha = 1.0;
  }

  gdk_cairo_set_source_rgba(cr, &bright_bg_color);
  cairo_paint(cr);

  cairo_translate(cr, inset, inset);
  width -= 2 * inset;
  height -= 2 * inset;

  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.0));
  gdk_cairo_set_source_rgba(cr, &really_dark_bg_color);
  cairo_rectangle(cr, 0, 0, width, height);
  cairo_stroke(cr);

  gdk_cairo_set_source_rgba(cr, &bright_bg_color);
  cairo_rectangle(cr, 0, 0, width, height);
  cairo_fill(cr);

  if(c->mouse_y > 0 || c->dragging)
  {
    // draw min/max curves:
    get_params(&p, ch2, c->mouse_x, 1., c->mouse_radius);
    for(int k = 0; k < BANDS; k++) dt_draw_curve_set_point(c->minmax_curve, k, p.x[ch2][k], p.y[ch2][k]);
    dt_draw_curve_calc_values(c->minmax_curve, 0.0, 1.0, RES, c->draw_min_xs, c->draw_min_ys);

    p = *(dt_iop_highpass_params_t *)self->params;
    get_params(&p, ch2, c->mouse_x, .0, c->mouse_radius);
    for(int k = 0; k < BANDS; k++) dt_draw_curve_set_point(c->minmax_curve, k, p.x[ch2][k], p.y[ch2][k]);
    dt_draw_curve_calc_values(c->minmax_curve, 0.0, 1.0, RES, c->draw_max_xs, c->draw_max_ys);
  }

  // draw grid
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(.4));
  gdk_cairo_set_source_rgba(cr, &really_dark_bg_color);
  dt_draw_grid(cr, 8, 0, 0, width, height);

  cairo_save(cr);

  // draw selected cursor
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.));
  cairo_translate(cr, 0, height);

// draw frequency histogram in bg.
#if 1
  if(c->num_samples > 0)
  {
    cairo_save(cr);
    for(int k = 1; k < c->num_samples; k += 2)
    {
      cairo_set_source_rgba(cr, really_dark_bg_color.red, really_dark_bg_color.green, really_dark_bg_color.blue, .3);
      cairo_move_to(cr, width * c->sample[k - 1], 0.0f);
      cairo_line_to(cr, width * c->sample[k - 1], -height);
      cairo_line_to(cr, width * c->sample[k], -height);
      cairo_line_to(cr, width * c->sample[k], 0.0f);
      cairo_fill(cr);
    }
    if(c->num_samples & 1)
    {
      cairo_move_to(cr, width * c->sample[c->num_samples - 1], 0.0f);
      cairo_line_to(cr, width * c->sample[c->num_samples - 1], -height);
      cairo_line_to(cr, 0.0f, -height);
      cairo_line_to(cr, 0.0f, 0.0f);
      cairo_fill(cr);
    }
    cairo_restore(cr);
  }
  if(c->band_max > 0)
  {
    cairo_save(cr);
    cairo_scale(cr, width / (BANDS - 1.0), -(height - DT_PIXEL_APPLY_DPI(5)) / c->band_max);
    cairo_set_source_rgba(cr, really_dark_bg_color.red, really_dark_bg_color.green, really_dark_bg_color.blue, .3);
    cairo_move_to(cr, 0, 0);
    for(int k = 0; k < BANDS; k++) cairo_line_to(cr, k, c->band_hist[k]);
    cairo_line_to(cr, BANDS - 1.0, 0.);
    cairo_close_path(cr);
    cairo_fill(cr);
    cairo_restore(cr);
  }
#endif

  // cairo_set_operator(cr, CAIRO_OPERATOR_ADD);
  cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(2.));
  for(int i = 0; i <= atrous_s; i++)
  {
    // draw curves, selected last.
    int ch = ((int)c->channel + i + 1) % (atrous_s + 1);
    int ch2 = -1;
    const float bgmul = i < atrous_s ? 0.5f : 1.0f;
    switch(ch)
    {
      case atrous_L:
        cairo_set_source_rgba(cr, .6, .6, .6, .3 * bgmul);
        ch2 = atrous_Lt;
        break;
      case atrous_c:
        cairo_set_source_rgba(cr, .4, .2, .0, .4 * bgmul);
        ch2 = atrous_ct;
        break;
      default: // case atrous_s:
        cairo_set_source_rgba(cr, .1, .2, .3, .4 * bgmul);
        break;
    }
    p = *(dt_iop_highpass_params_t *)self->params;

    // reverse order if bottom is active (to end up with correct values in minmax_curve):
    if(c->channel2 == ch2)
    {
      ch2 = ch;
      ch = c->channel2;
    }

    if(ch2 >= 0)
    {
      for(int k = 0; k < BANDS; k++) dt_draw_curve_set_point(c->minmax_curve, k, p.x[ch2][k], p.y[ch2][k]);
      dt_draw_curve_calc_values(c->minmax_curve, 0.0, 1.0, RES, c->draw_xs, c->draw_ys);
      cairo_move_to(cr, width, -height * p.y[ch2][BANDS - 1]);
      for(int k = RES - 2; k >= 0; k--)
        cairo_line_to(cr, k * width / (float)(RES - 1), -height * c->draw_ys[k]);
    }
    else
      cairo_move_to(cr, 0, 0);
    for(int k = 0; k < BANDS; k++) dt_draw_curve_set_point(c->minmax_curve, k, p.x[ch][k], p.y[ch][k]);
    dt_draw_curve_calc_values(c->minmax_curve, 0.0, 1.0, RES, c->draw_xs, c->draw_ys);
    for(int k = 0; k < RES; k++) cairo_line_to(cr, k * width / (float)(RES - 1), -height * c->draw_ys[k]);
    if(ch2 < 0) cairo_line_to(cr, width, 0);
    cairo_close_path(cr);
    cairo_stroke_preserve(cr);
    cairo_fill(cr);
  }

  if(c->mouse_y > 0 || c->dragging)
  {
    // draw dots on knots
    cairo_save(cr);
    if(ch != ch2)
      cairo_set_source_rgb(cr, 0.1, 0.1, 0.1);
    else
      cairo_set_source_rgb(cr, 0.7, 0.7, 0.7);
    cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.));
    for(int k = 0; k < BANDS; k++)
    {
      cairo_arc(cr, width * p.x[ch2][k], -height * p.y[ch2][k], DT_PIXEL_APPLY_DPI(3.0), 0.0, 2.0 * M_PI);
      if(c->x_move == k)
        cairo_fill(cr);
      else
        cairo_stroke(cr);
    }
    cairo_restore(cr);
  }

  if(c->mouse_y > 0 || c->dragging)
  {
    // draw min/max, if selected
    // cairo_set_source_rgba(cr, .6, .6, .6, .5);
    cairo_move_to(cr, 0, -height * c->draw_min_ys[0]);
    for(int k = 1; k < RES; k++) cairo_line_to(cr, k * width / (float)(RES - 1), -height * c->draw_min_ys[k]);
    for(int k = RES - 1; k >= 0; k--)
      cairo_line_to(cr, k * width / (float)(RES - 1), -height * c->draw_max_ys[k]);
    cairo_close_path(cr);
    cairo_fill(cr);
    // draw mouse focus circle
    cairo_set_source_rgba(cr, .9, .9, .9, .5);
    const float pos = RES * c->mouse_x;
    int k = (int)pos;
    const float f = k - pos;
    if(k >= RES - 1) k = RES - 2;
    float ht = -height * (f * c->draw_ys[k] + (1 - f) * c->draw_ys[k + 1]);
    cairo_arc(cr, c->mouse_x * width, ht, c->mouse_radius * width, 0, 2. * M_PI);
    cairo_stroke(cr);
  }

  cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);

  // draw x positions
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.));
  cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
  const float arrw = DT_PIXEL_APPLY_DPI(7.0f);
  for(int k = 1; k < BANDS - 1; k++)
  {
    cairo_move_to(cr, width * p.x[ch][k], inset - DT_PIXEL_APPLY_DPI(1));
    cairo_rel_line_to(cr, -arrw * .5f, 0);
    cairo_rel_line_to(cr, arrw * .5f, -arrw);
    cairo_rel_line_to(cr, arrw * .5f, arrw);
    cairo_close_path(cr);
    if(c->x_move == k)
      cairo_fill(cr);
    else
      cairo_stroke(cr);
  }

  cairo_restore(cr);

  if(c->mouse_y > 0 || c->dragging)
  {
    // draw labels:
    cairo_text_extents_t ext;
    gdk_cairo_set_source_rgba(cr, &really_dark_bg_color);
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, .06 * height);
    cairo_text_extents(cr, _("coarse"), &ext);
    cairo_move_to(cr, .02 * width + ext.height, .14 * height + ext.width);
    cairo_save(cr);
    cairo_rotate(cr, -M_PI * .5f);
    cairo_show_text(cr, _("coarse"));
    cairo_restore(cr);
    cairo_text_extents(cr, _("fine"), &ext);
    cairo_move_to(cr, .98 * width, .14 * height + ext.width);
    cairo_save(cr);
    cairo_rotate(cr, -M_PI * .5f);
    cairo_show_text(cr, _("fine"));
    cairo_restore(cr);

    switch(c->channel2)
    {
      case atrous_L:
      case atrous_c:
        dt_atrous_show_upper_label(cr, _("contrasty"), ext);
        dt_atrous_show_lower_label(cr, _("smooth"), ext);
        break;
      case atrous_Lt:
      case atrous_ct:
        dt_atrous_show_upper_label(cr, _("smooth"), ext);
        dt_atrous_show_lower_label(cr, _("noisy"), ext);
        break;
      default: // case atrous_s:
        dt_atrous_show_upper_label(cr, _("bold"), ext);
        dt_atrous_show_lower_label(cr, _("dull"), ext);
        break;
    }
  }


  cairo_destroy(cr);
  cairo_set_source_surface(crf, cst, 0, 0);
  cairo_paint(crf);
  cairo_surface_destroy(cst);
  return TRUE;
}

static gboolean area_motion_notify(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_highpass_gui_data_t *c = (dt_iop_highpass_gui_data_t *)self->gui_data;
  dt_iop_highpass_params_t *p = (dt_iop_highpass_params_t *)self->params;
  const int inset = INSET;
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  int height = allocation.height - 2 * inset, width = allocation.width - 2 * inset;
  if(!c->dragging) c->mouse_x = CLAMP(event->x - inset, 0, width) / (float)width;
  c->mouse_y = 1.0 - CLAMP(event->y - inset, 0, height) / (float)height;
  int ch2 = c->channel;
  if(c->channel == atrous_L) ch2 = atrous_Lt;
  if(c->channel == atrous_c) ch2 = atrous_ct;
  if(c->dragging)
  {
    // drag y-positions
    *p = c->drag_params;
    if(c->x_move >= 0)
    {
      const float mx = CLAMP(event->x - inset, 0, width) / (float)width;
      if(c->x_move > 0 && c->x_move < BANDS - 1)
      {
        const float minx = p->x[c->channel][c->x_move - 1] + 0.001f;
        const float maxx = p->x[c->channel][c->x_move + 1] - 0.001f;
        p->x[ch2][c->x_move] = p->x[c->channel][c->x_move] = fminf(maxx, fmaxf(minx, mx));
      }
    }
    else
    {
      get_params(p, c->channel2, c->mouse_x, c->mouse_y + c->mouse_pick, c->mouse_radius);
    }
    dt_dev_add_history_item(darktable.develop, self, TRUE);
  }
  else if(event->y > height)
  {
    // move x-positions
    c->x_move = 0;
    float dist = fabs(p->x[c->channel][0] - c->mouse_x);
    for(int k = 1; k < BANDS; k++)
    {
      float d2 = fabs(p->x[c->channel][k] - c->mouse_x);
      if(d2 < dist)
      {
        c->x_move = k;
        dist = d2;
      }
    }
  }
  else
  {
    // choose between bottom and top curve:
    int ch = c->channel;
    float dist = 1000000.0f;
    for(int k = 0; k < BANDS; k++)
    {
      float d2 = fabs(p->x[c->channel][k] - c->mouse_x);
      if(d2 < dist)
      {
        if(fabs(c->mouse_y - p->y[ch][k]) < fabs(c->mouse_y - p->y[ch2][k]))
          c->channel2 = ch;
        else
          c->channel2 = ch2;
        dist = d2;
      }
    }
    // don't move x-positions:
    c->x_move = -1;
  }
  gtk_widget_queue_draw(widget);
  gint x, y;
  gdk_window_get_device_position(event->window,
                                 gdk_device_manager_get_client_pointer(
                                     gdk_display_get_device_manager(gdk_window_get_display(event->window))),
                                 &x, &y, NULL);
  return TRUE;
}

static gboolean area_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(event->button == 1 && event->type == GDK_2BUTTON_PRESS)
  {
    // reset current curve
    dt_iop_highpass_params_t *p = (dt_iop_highpass_params_t *)self->params;
    dt_iop_highpass_params_t *d = (dt_iop_highpass_params_t *)self->default_params;
    dt_iop_highpass_gui_data_t *c = (dt_iop_highpass_gui_data_t *)self->gui_data;
    reset_mix(self);
    for(int k = 0; k < BANDS; k++)
    {
      p->x[c->channel2][k] = d->x[c->channel2][k];
      p->y[c->channel2][k] = d->y[c->channel2][k];
    }
    dt_dev_add_history_item(darktable.develop, self, TRUE);
    gtk_widget_queue_draw(self->widget);
  }
  else if(event->button == 1)
  {
    // set active point
    dt_iop_highpass_gui_data_t *c = (dt_iop_highpass_gui_data_t *)self->gui_data;
    reset_mix(self);
    const int inset = INSET;
    GtkAllocation allocation;
    gtk_widget_get_allocation(widget, &allocation);
    int height = allocation.height - 2 * inset, width = allocation.width - 2 * inset;
    c->mouse_pick
        = dt_draw_curve_calc_value(c->minmax_curve, CLAMP(event->x - inset, 0, width) / (float)width);
    c->mouse_pick -= 1.0 - CLAMP(event->y - inset, 0, height) / (float)height;
    c->dragging = 1;
    return TRUE;
  }
  return FALSE;
}

static gboolean area_button_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  if(event->button == 1)
  {
    dt_iop_module_t *self = (dt_iop_module_t *)user_data;
    dt_iop_highpass_gui_data_t *c = (dt_iop_highpass_gui_data_t *)self->gui_data;
    c->dragging = 0;
    reset_mix(self);
    return TRUE;
  }
  return FALSE;
}

static gboolean area_scrolled(GtkWidget *widget, GdkEventScroll *event, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_highpass_gui_data_t *c = (dt_iop_highpass_gui_data_t *)self->gui_data;
  if(event->direction == GDK_SCROLL_UP && c->mouse_radius > 0.25 / BANDS) c->mouse_radius *= 0.9; // 0.7;
  if(event->direction == GDK_SCROLL_DOWN && c->mouse_radius < 1.0) c->mouse_radius *= (1.0 / 0.9); // 1.42;
  gtk_widget_queue_draw(widget);
  return TRUE;
}

static void tab_switch(GtkNotebook *notebook, GtkWidget *page, guint page_num, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_highpass_gui_data_t *c = (dt_iop_highpass_gui_data_t *)self->gui_data;
  if(self->dt->gui->reset) return;
  c->channel = c->channel2 = (atrous_channel_t)page_num;
  gtk_widget_queue_draw(self->widget);
}

static void mix_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_highpass_params_t *p = (dt_iop_highpass_params_t *)self->params;
  dt_iop_highpass_params_t *d = (dt_iop_highpass_params_t *)self->default_params;
  dt_iop_highpass_gui_data_t *c = (dt_iop_highpass_gui_data_t *)self->gui_data;
  const float mix = dt_bauhaus_slider_get(slider);
  for(int ch = 0; ch < atrous_none; ch++)
    for(int k = 0; k < BANDS; k++)
    {
      p->x[ch][k] = fminf(1.0f, fmaxf(0.0f, d->x[ch][k] + mix * (c->drag_params.x[ch][k] - d->x[ch][k])));
      p->y[ch][k] = fminf(1.0f, fmaxf(0.0f, d->y[ch][k] + mix * (c->drag_params.y[ch][k] - d->y[ch][k])));
    }
  dt_dev_add_history_item(darktable.develop, self, TRUE);
  gtk_widget_queue_draw(self->widget);
}
/* End EFH */

static void autoscale_ab_callback(GtkWidget *widget, dt_iop_module_t *self) // EFH
{
  if(darktable.gui->reset) return;
  dt_iop_highpass_gui_data_t *g = (dt_iop_highpass_gui_data_t *)self->gui_data;
  dt_iop_highpass_params_t *p = (dt_iop_highpass_params_t *)self->params;
  p->tonecurve_autoscale_ab = dt_bauhaus_combobox_get(g->autoscale_ab);

  if(p->tonecurve_autoscale_ab)
  {
	  gtk_widget_set_visible(GTK_WIDGET(g->channel_tabs), TRUE);
	  gtk_widget_set_visible(GTK_WIDGET(g->area), TRUE);
	  gtk_widget_set_visible(g->mix, TRUE);
  }
  else
  {
	  gtk_widget_set_visible(GTK_WIDGET(g->channel_tabs), FALSE);
	  gtk_widget_set_visible(GTK_WIDGET(g->area), FALSE);
	  gtk_widget_set_visible(g->mix, FALSE);
  }


  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

void init(dt_iop_module_t *module)
{
  module->params = calloc(1, sizeof(dt_iop_highpass_params_t));
  module->default_params = calloc(1, sizeof(dt_iop_highpass_params_t));
  module->default_enabled = 0;
  module->priority = 766; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_highpass_params_t);
  module->gui_data = NULL;
  dt_iop_highpass_params_t tmp = (dt_iop_highpass_params_t){ 50, 50 };
  /* Begin EFH */
  tmp.octaves = 3;
  for(int k = 0; k < BANDS; k++)
  {
    tmp.y[atrous_L][k] = tmp.y[atrous_s][k] = tmp.y[atrous_c][k] = 0.5f;
    tmp.x[atrous_L][k] = tmp.x[atrous_s][k] = tmp.x[atrous_c][k] = k / (BANDS - 1.0f);
    tmp.y[atrous_Lt][k] = tmp.y[atrous_ct][k] = 0.0f;
    tmp.x[atrous_Lt][k] = tmp.x[atrous_ct][k] = k / (BANDS - 1.0f);
  }
  /* End EFH */
  memcpy(module->params, &tmp, sizeof(dt_iop_highpass_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_highpass_params_t));
}

void init_global(dt_iop_module_so_t *module)
{
  const int program = 4; // highpass.cl, from programs.conf
  dt_iop_highpass_global_data_t *gd
      = (dt_iop_highpass_global_data_t *)malloc(sizeof(dt_iop_highpass_global_data_t));
  module->data = gd;
  gd->kernel_highpass_invert = dt_opencl_create_kernel(program, "highpass_invert");
  gd->kernel_highpass_hblur = dt_opencl_create_kernel(program, "highpass_hblur");
  gd->kernel_highpass_vblur = dt_opencl_create_kernel(program, "highpass_vblur");
  gd->kernel_highpass_mix = dt_opencl_create_kernel(program, "highpass_mix");
/* Begin EFH */
  gd->kernel_decompose = dt_opencl_create_kernel(program, "eaw_decompose");
  gd->kernel_synthesize = dt_opencl_create_kernel(program, "eaw_synthesize");
/* End EFH */
}


void cleanup(dt_iop_module_t *module)
{
  free(module->params);
  module->params = NULL;
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_highpass_global_data_t *gd = (dt_iop_highpass_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_highpass_invert);
  dt_opencl_free_kernel(gd->kernel_highpass_hblur);
  dt_opencl_free_kernel(gd->kernel_highpass_vblur);
  dt_opencl_free_kernel(gd->kernel_highpass_mix);
/* Begin EFH */
  dt_opencl_free_kernel(gd->kernel_decompose);
  dt_opencl_free_kernel(gd->kernel_synthesize);
/* End EFH */
  free(module->data);
  module->data = NULL;
}


void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_highpass_gui_data_t));
  dt_iop_highpass_gui_data_t *g = (dt_iop_highpass_gui_data_t *)self->gui_data;
  dt_iop_highpass_params_t *p = (dt_iop_highpass_params_t *)self->params;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  /* sharpness */
  g->scale1 = dt_bauhaus_slider_new_with_range(self, 0.0, 100.0, 0.5, p->sharpness, 2);
  dt_bauhaus_widget_set_label(g->scale1, NULL, _("sharpness"));
  dt_bauhaus_slider_set_format(g->scale1, "%.0f%%");
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->scale1), TRUE, TRUE, 0);
  g_object_set(g->scale1, "tooltip-text", _("the sharpness of highpass filter"), (char *)NULL);
  g_signal_connect(G_OBJECT(g->scale1), "value-changed", G_CALLBACK(sharpness_callback), self);

  /* contrast boost */
  g->scale2 = dt_bauhaus_slider_new_with_range(self, 0.0, 100.0, 0.5, p->contrast, 2);
  dt_bauhaus_widget_set_label(g->scale2, NULL, _("contrast boost"));
  dt_bauhaus_slider_set_format(g->scale2, "%.0f%%");
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->scale2), TRUE, TRUE, 0);
  g_object_set(g->scale2, "tooltip-text", _("the contrast of highpass filter"), (char *)NULL);
  g_signal_connect(G_OBJECT(g->scale2), "value-changed", G_CALLBACK(contrast_callback), self);

  { // EFH
	  dt_iop_highpass_gui_data_t *c = (dt_iop_highpass_gui_data_t *)self->gui_data;
	  c->autoscale_ab = dt_bauhaus_combobox_new(self);
	  gtk_box_pack_start(GTK_BOX(self->widget), c->autoscale_ab, TRUE, TRUE, 0);
	  dt_bauhaus_widget_set_label(c->autoscale_ab, NULL, _("equalizer"));
	  dt_bauhaus_combobox_add(c->autoscale_ab, _("no"));
	  dt_bauhaus_combobox_add(c->autoscale_ab, _("yes"));
	  g_object_set(G_OBJECT(c->autoscale_ab), "tooltip-text",
				   _("activates equalizer that will be applied after highpass."),
				   (char *)NULL);
	  g_signal_connect(G_OBJECT(c->autoscale_ab), "value-changed", G_CALLBACK(autoscale_ab_callback), self);
  }

/* Begin EFH */
  {
  dt_iop_highpass_gui_data_t *c = (dt_iop_highpass_gui_data_t *)self->gui_data;
  dt_iop_highpass_params_t *p = (dt_iop_highpass_params_t *)self->params;

  c->num_samples = 0;
  c->band_max = 0;
  c->channel = c->channel2 = dt_conf_get_int("plugins/darkroom/atrous/gui_channel");
  int ch = (int)c->channel;
  c->minmax_curve = dt_draw_curve_new(0.0, 1.0, CATMULL_ROM);
  for(int k = 0; k < BANDS; k++) (void)dt_draw_curve_add_point(c->minmax_curve, p->x[ch][k], p->y[ch][k]);
  c->mouse_x = c->mouse_y = c->mouse_pick = -1.0;
  c->dragging = 0;
  c->x_move = -1;
  c->mouse_radius = 1.0 / BANDS;
  // self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE); removed from original code
  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), vbox, FALSE, FALSE, 0);

  c->channel_tabs = GTK_NOTEBOOK(gtk_notebook_new());

  gtk_notebook_append_page(GTK_NOTEBOOK(c->channel_tabs),
                           GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0)), gtk_label_new(_("luma")));
  g_object_set(
      G_OBJECT(gtk_notebook_get_tab_label(c->channel_tabs, gtk_notebook_get_nth_page(c->channel_tabs, -1))),
      "tooltip-text", _("change lightness at each feature size"), NULL);
  gtk_notebook_append_page(GTK_NOTEBOOK(c->channel_tabs),
                           GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0)),
                           gtk_label_new(_("chroma")));
  g_object_set(
      G_OBJECT(gtk_notebook_get_tab_label(c->channel_tabs, gtk_notebook_get_nth_page(c->channel_tabs, -1))),
      "tooltip-text", _("change color saturation at each feature size"), NULL);
  gtk_notebook_append_page(GTK_NOTEBOOK(c->channel_tabs),
                           GTK_WIDGET(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0)), gtk_label_new(_("edges")));
  g_object_set(
      G_OBJECT(gtk_notebook_get_tab_label(c->channel_tabs, gtk_notebook_get_nth_page(c->channel_tabs, -1))),
      "tooltip-text",
      _("change edge halos at each feature size\nonly changes results of luma and chroma tabs"), NULL);

  gtk_widget_show_all(GTK_WIDGET(gtk_notebook_get_nth_page(c->channel_tabs, c->channel)));
  gtk_notebook_set_current_page(GTK_NOTEBOOK(c->channel_tabs), c->channel);

  gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(c->channel_tabs), FALSE, FALSE, 0);

  g_signal_connect(G_OBJECT(c->channel_tabs), "switch_page", G_CALLBACK(tab_switch), self);

  // graph
  c->area = GTK_DRAWING_AREA(dtgtk_drawing_area_new_with_aspect_ratio(0.75));
  gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(c->area), TRUE, TRUE, 0);

  gtk_widget_add_events(GTK_WIDGET(c->area), GDK_POINTER_MOTION_MASK | GDK_POINTER_MOTION_HINT_MASK
                                             | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK
                                             | GDK_LEAVE_NOTIFY_MASK | GDK_SCROLL_MASK);
  g_signal_connect(G_OBJECT(c->area), "draw", G_CALLBACK(area_draw), self);
  g_signal_connect(G_OBJECT(c->area), "button-press-event", G_CALLBACK(area_button_press), self);
  g_signal_connect(G_OBJECT(c->area), "button-release-event", G_CALLBACK(area_button_release), self);
  g_signal_connect(G_OBJECT(c->area), "motion-notify-event", G_CALLBACK(area_motion_notify), self);
  g_signal_connect(G_OBJECT(c->area), "leave-notify-event", G_CALLBACK(area_leave_notify), self);
  g_signal_connect(G_OBJECT(c->area), "enter-notify-event", G_CALLBACK(area_enter_notify), self);
  g_signal_connect(G_OBJECT(c->area), "scroll-event", G_CALLBACK(area_scrolled), self);

  // mix slider
  c->mix = dt_bauhaus_slider_new_with_range(self, -2.0f, 2.0f, 0.1f, 1.0f, 3);
  dt_bauhaus_widget_set_label(c->mix, NULL, _("mix"));
  g_object_set(G_OBJECT(c->mix), "tooltip-text", _("make effect stronger or weaker"), (char *)NULL);
  gtk_box_pack_start(GTK_BOX(self->widget), c->mix, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(c->mix), "value-changed", G_CALLBACK(mix_callback), self);
  }
/* End EFH */

}

void gui_cleanup(struct dt_iop_module_t *self)
{
  /* Begin EFH */
	  dt_iop_highpass_gui_data_t *c = (dt_iop_highpass_gui_data_t *)self->gui_data;
	  dt_conf_set_int("plugins/darkroom/atrous/gui_channel", c->channel);
	  dt_draw_curve_destroy(c->minmax_curve);
  /* End EFH */
  free(self->gui_data);
  self->gui_data = NULL;
}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
