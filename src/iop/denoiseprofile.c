/*
    This file is part of darktable,
    copyright (c) 2011--2013 johannes hanika.

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
#include "common/noiseprofiles.h"
#include "common/opencl.h"
#include "common/opencl.h"
#include "control/control.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "develop/tiling.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "iop/iop_api.h"

#include <gtk/gtk.h>
#include <math.h>
#include <stdlib.h>
#if defined(__SSE__)
#include <xmmintrin.h>
#endif

#define BLOCKSIZE                                                                                            \
  2048 /* maximum blocksize. must be a power of 2 and will be automatically reduced if needed */
#define REDUCESIZE 64
#define MAX_PROFILES 30
#define NUM_BUCKETS 4

typedef enum dt_iop_denoiseprofile_mode_t
{
  MODE_NLMEANS = 0,
  MODE_WAVELETS = 1
} dt_iop_denoiseprofile_mode_t;

// this is the version of the modules parameters,
// and includes version information about compile-time dt
DT_MODULE_INTROSPECTION(3, dt_iop_denoiseprofile_params_t)

typedef struct dt_iop_denoiseprofile_params_t
{
  float radius;     // search radius
  float strength;   // noise level after equalization
  float a[3], b[3]; // fit for poissonian-gaussian noise per color channel.
  dt_iop_denoiseprofile_mode_t mode; // switch between nlmeans and wavelets
} dt_iop_denoiseprofile_params_t;

typedef struct dt_iop_denoiseprofile_gui_data_t
{
  GtkWidget *profile;
  GtkWidget *mode;
  GtkWidget *radius;
  GtkWidget *strength;
  dt_noiseprofile_t interpolated; // don't use name, maker or model, they may point to garbage
  GList *profiles;
} dt_iop_denoiseprofile_gui_data_t;

typedef dt_iop_denoiseprofile_params_t dt_iop_denoiseprofile_data_t;

typedef struct dt_iop_denoiseprofile_global_data_t
{
  int kernel_denoiseprofile_precondition;
  int kernel_denoiseprofile_init;
  int kernel_denoiseprofile_dist;
  int kernel_denoiseprofile_horiz;
  int kernel_denoiseprofile_vert;
  int kernel_denoiseprofile_accu;
  int kernel_denoiseprofile_finish;
  int kernel_denoiseprofile_backtransform;
  int kernel_denoiseprofile_decompose;
  int kernel_denoiseprofile_synthesize;
  int kernel_denoiseprofile_reduce_first;
  int kernel_denoiseprofile_reduce_second;
} dt_iop_denoiseprofile_global_data_t;

static dt_noiseprofile_t dt_iop_denoiseprofile_get_auto_profile(dt_iop_module_t *self);

int legacy_params(dt_iop_module_t *self, const void *const old_params, const int old_version,
                  void *new_params, const int new_version)
{
  if((old_version == 1 || old_version == 2) && new_version == 3)
  {
    dt_iop_denoiseprofile_params_t *o = (dt_iop_denoiseprofile_params_t *)old_params;
    dt_iop_denoiseprofile_params_t *n = (dt_iop_denoiseprofile_params_t *)new_params;
    if(old_version == 1)
    {
      // one more parameter and changed parameters in case we autodetected a profile
      memcpy(n, o, sizeof(dt_iop_denoiseprofile_params_t) - sizeof(uint32_t));
      n->mode = MODE_NLMEANS;
    }
    else
    {
      memcpy(n, o, sizeof(dt_iop_denoiseprofile_params_t));
    }
    // autodetect current profile:
    if(!self->dev)
    {
      // we are probably handling a style or preset, do nothing for them, we cant do anything to detect if
      // autodetection was used or not
      return 0;
    }
    dt_noiseprofile_t interpolated = dt_iop_denoiseprofile_get_auto_profile(self);
    // if the profile in old_version is an autodetected one (this would mean a+b params match the interpolated
    // one, AND
    // the profile is actually the first selected one - however we can only detect the params, but most people
    // did probably
    // not set the exact ISO on purpose instead of the "found match" - they probably still want
    // autodetection!)
    if(!memcmp(interpolated.a, o->a, sizeof(float) * 3) && !memcmp(interpolated.b, o->b, sizeof(float) * 3))
    {
      // set the param a[0] to -1.0 to signal the autodetection
      n->a[0] = -1.0;
    }
    return 0;
  }
  return 1;
}

const char *name()
{
  return _("denoise (profiled)");
}

int groups()
{
  return IOP_GROUP_CORRECT;
}

int flags()
{
  return IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING;
}

typedef union floatint_t
{
  float f;
  uint32_t i;
} floatint_t;

// very fast approximation for 2^-x (returns 0 for x > 126)
static inline float fast_mexp2f(const float x)
{
  const float i1 = (float)0x3f800000u; // 2^0
  const float i2 = (float)0x3f000000u; // 2^-1
  const float k0 = i1 + x * (i2 - i1);
  floatint_t k;
  k.i = k0 >= (float)0x800000u ? k0 : 0;
  return k.f;
}

void tiling_callback(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece,
                     const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out,
                     struct dt_develop_tiling_t *tiling)
{
  dt_iop_denoiseprofile_params_t *d = (dt_iop_denoiseprofile_params_t *)piece->data;

  if(d->mode == MODE_NLMEANS)
  {
    const int P
        = ceilf(d->radius * fmin(roi_in->scale, 2.0f) / fmax(piece->iscale, 1.0f)); // pixel filter size
    const int K = ceilf(7 * fmin(roi_in->scale, 2.0f) / fmax(piece->iscale, 1.0f)); // nbhood

    tiling->factor = 4.0f + 0.25f * NUM_BUCKETS; // in + out + (2 + NUM_BUCKETS * 0.25) tmp
    tiling->maxbuf = 1.0f;
    tiling->overhead = 0;
    tiling->overlap = P + K;
    tiling->xalign = 1;
    tiling->yalign = 1;
  }
  else
  {
    const int max_max_scale = 5; // hard limit
    int max_scale = 0;
    const float scale = roi_in->scale / piece->iscale;
    // largest desired filter on input buffer (20% of input dim)
    const float supp0
        = MIN(2 * (2 << (max_max_scale - 1)) + 1,
              MAX(piece->buf_in.height * piece->iscale, piece->buf_in.width * piece->iscale) * 0.2f);
    const float i0 = dt_log2f((supp0 - 1.0f) * .5f);
    for(; max_scale < max_max_scale; max_scale++)
    {
      // actual filter support on scaled buffer
      const float supp = 2 * (2 << max_scale) + 1;
      // approximates this filter size on unscaled input image:
      const float supp_in = supp * (1.0f / scale);
      const float i_in = dt_log2f((supp_in - 1) * .5f) - 1.0f;
      // i_in = max_scale .. .. .. 0
      const float t = 1.0f - (i_in + .5f) / i0;
      if(t < 0.0f) break;
    }

    const int max_filter_radius = (1 << max_scale); // 2 * 2^max_scale

    tiling->factor = 3.5f + max_scale; // in + out + tmp + reducebuffer + scale buffers
    tiling->maxbuf = 1.0f;
    tiling->overhead = 0;
    tiling->overlap = max_filter_radius;
    tiling->xalign = 1;
    tiling->yalign = 1;
  }

  return;
}

static inline void precondition(const float *const in, float *const buf, const int wd, const int ht,
                                const float a[3], const float b[3])
{
  const float sigma2[3]
      = { (b[0] / a[0]) * (b[0] / a[0]), (b[1] / a[1]) * (b[1] / a[1]), (b[2] / a[1]) * (b[2] / a[1]) };

#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none) shared(a)
#endif
  for(int j = 0; j < ht; j++)
  {
    float *buf2 = buf + (size_t)4 * j * wd;
    const float *in2 = in + (size_t)4 * j * wd;
    for(int i = 0; i < wd; i++)
    {
      for(int c = 0; c < 3; c++)
      {
        buf2[c] = in2[c] / a[c];
        const float d = fmaxf(0.0f, buf2[c] + 3. / 8. + sigma2[c]);
        buf2[c] = 2.0f * sqrtf(d);
      }
      buf2 += 4;
      in2 += 4;
    }
  }
}

static inline void backtransform(float *const buf, const int wd, const int ht, const float a[3],
                                 const float b[3])
{
  const float sigma2[3]
      = { (b[0] / a[0]) * (b[0] / a[0]), (b[1] / a[1]) * (b[1] / a[1]), (b[2] / a[1]) * (b[2] / a[1]) };

#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none) shared(a)
#endif
  for(int j = 0; j < ht; j++)
  {
    float *buf2 = buf + (size_t)4 * j * wd;
    for(int i = 0; i < wd; i++)
    {
      for(int c = 0; c < 3; c++)
      {
        const float x = buf2[c];
        // closed form approximation to unbiased inverse (input range was 0..200 for fit, not 0..1)
        if(x < .5f)
          buf2[c] = 0.0f;
        else
          buf2[c] = 1. / 4. * x * x + 1. / 4. * sqrtf(3. / 2.) / x - 11. / 8. * 1.0 / (x * x)
                    + 5. / 8. * sqrtf(3. / 2.) * 1.0 / (x * x * x) - 1. / 8. - sigma2[c];
        // asymptotic form:
        // buf2[c] = fmaxf(0.0f, 1./4.*x*x - 1./8. - sigma2[c]);
        buf2[c] *= a[c];
      }
      buf2 += 4;
    }
  }
}

// =====================================================================================
// begin wavelet code:
// =====================================================================================

static inline float weight(const float *c1, const float *c2, const float inv_sigma2)
{
// return _mm_set1_ps(1.0f);
#if 1
  // 3d distance based on color
  float diff[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
  for(int c = 0; c < 4; c++) diff[c] = c1[c] - c2[c];

  float sqr[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
  for(int c = 0; c < 4; c++) sqr[c] = diff[c] * diff[c];

  const float dot = (sqr[0] + sqr[1] + sqr[2]) * inv_sigma2;
  const float var
      = 0.02f; // FIXME: this should ideally depend on the image before noise stabilizing transforms!
  const float off2 = 9.0f; // (3 sigma)^2
  return fast_mexp2f(MAX(0, dot * var - off2));
#endif
}

#if defined(__SSE__)
static inline __m128 weight_sse(const __m128 *c1, const __m128 *c2, const float inv_sigma2)
{
// return _mm_set1_ps(1.0f);
#if 1
  // 3d distance based on color
  __m128 diff = _mm_sub_ps(*c1, *c2);
  __m128 sqr = _mm_mul_ps(diff, diff);
  float *fsqr = (float *)&sqr;
  const float dot = (fsqr[0] + fsqr[1] + fsqr[2]) * inv_sigma2;
  const float var
      = 0.02f; // FIXME: this should ideally depend on the image before noise stabilizing transforms!
  const float off2 = 9.0f; // (3 sigma)^2
  return _mm_set1_ps(fast_mexp2f(MAX(0, dot * var - off2)));
#endif
}
#endif

#define SUM_PIXEL_CONTRIBUTION_COMMON(ii, jj)                                                                \
  do                                                                                                         \
  {                                                                                                          \
    const float f = filter[(ii)] * filter[(jj)];                                                             \
    const float wp = weight(px, px2, inv_sigma2);                                                            \
    const float w = f * wp;                                                                                  \
    float pd[4] = { 0.0f, 0.0f, 0.0f, 0.0f };                                                                \
    for(int c = 0; c < 4; c++) pd[c] = w * px2[c];                                                           \
    for(int c = 0; c < 4; c++) sum[c] += pd[c];                                                              \
    for(int c = 0; c < 4; c++) wgt[c] += w;                                                                  \
  } while(0)

#if defined(__SSE__)
#define SUM_PIXEL_CONTRIBUTION_COMMON_SSE(ii, jj)                                                            \
  do                                                                                                         \
  {                                                                                                          \
    const __m128 f = _mm_set1_ps(filter[(ii)] * filter[(jj)]);                                               \
    const __m128 wp = weight_sse(px, px2, inv_sigma2);                                                       \
    const __m128 w = _mm_mul_ps(f, wp);                                                                      \
    const __m128 pd = _mm_mul_ps(w, *px2);                                                                   \
    sum = _mm_add_ps(sum, pd);                                                                               \
    wgt = _mm_add_ps(wgt, w);                                                                                \
  } while(0)
#endif

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
    px2 = ((float *)in) + 4 * x + (size_t)4 * y * width;                                                     \
                                                                                                             \
    SUM_PIXEL_CONTRIBUTION_COMMON(ii, jj);                                                                   \
  } while(0)

#if defined(__SSE__)
#define SUM_PIXEL_CONTRIBUTION_WITH_TEST_SSE(ii, jj)                                                         \
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
    SUM_PIXEL_CONTRIBUTION_COMMON_SSE(ii, jj);                                                               \
  } while(0)
#endif

#define ROW_PROLOGUE                                                                                         \
  const float *px = ((float *)in) + (size_t)4 * j * width;                                                   \
  const float *px2;                                                                                          \
  float *pdetail = detail + (size_t)4 * j * width;                                                           \
  float *pcoarse = out + (size_t)4 * j * width;

#if defined(__SSE__)
#define ROW_PROLOGUE_SSE                                                                                     \
  const __m128 *px = ((__m128 *)in) + (size_t)j * width;                                                     \
  const __m128 *px2;                                                                                         \
  float *pdetail = detail + (size_t)4 * j * width;                                                           \
  float *pcoarse = out + (size_t)4 * j * width;
#endif

#define SUM_PIXEL_PROLOGUE                                                                                   \
  float sum[4] = { 0.0f, 0.0f, 0.0f, 0.0f };                                                                 \
  float wgt[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

#if defined(__SSE__)
#define SUM_PIXEL_PROLOGUE_SSE                                                                               \
  __m128 sum = _mm_setzero_ps();                                                                             \
  __m128 wgt = _mm_setzero_ps();
#endif

#define SUM_PIXEL_EPILOGUE                                                                                   \
  for(int c = 0; c < 4; c++) sum[c] /= wgt[c];                                                               \
                                                                                                             \
  for(int c = 0; c < 4; c++) pdetail[c] = (px[c] - sum[c]);                                                  \
  for(int c = 0; c < 4; c++) pcoarse[c] = sum[c];                                                            \
  px += 4;                                                                                                   \
  pdetail += 4;                                                                                              \
  pcoarse += 4;

#if defined(__SSE__)
#define SUM_PIXEL_EPILOGUE_SSE                                                                               \
  sum = _mm_div_ps(sum, wgt);                                                                                \
                                                                                                             \
  _mm_stream_ps(pdetail, _mm_sub_ps(*px, sum));                                                              \
  _mm_stream_ps(pcoarse, sum);                                                                               \
  px++;                                                                                                      \
  pdetail += 4;                                                                                              \
  pcoarse += 4;
#endif

typedef void((*eaw_decompose_t)(float *const out, const float *const in, float *const detail, const int scale,
                                const float inv_sigma2, const int32_t width, const int32_t height));

static void eaw_decompose(float *const out, const float *const in, float *const detail, const int scale,
                          const float inv_sigma2, const int32_t width, const int32_t height)
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
      px2 = ((float *)in) + (size_t)4 * (i - 2 * mult + (size_t)(j - 2 * mult) * width);
      for(int jj = 0; jj < 5; jj++)
      {
        for(int ii = 0; ii < 5; ii++)
        {
          SUM_PIXEL_CONTRIBUTION_COMMON(ii, jj);
          px2 += (size_t)4 * mult;
        }
        px2 += (size_t)4 * (width - 5) * mult;
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
}

#undef SUM_PIXEL_CONTRIBUTION_COMMON
#undef SUM_PIXEL_CONTRIBUTION_WITH_TEST
#undef ROW_PROLOGUE
#undef SUM_PIXEL_PROLOGUE
#undef SUM_PIXEL_EPILOGUE

#if defined(__SSE2__)
static void eaw_decompose_sse(float *const out, const float *const in, float *const detail, const int scale,
                              const float inv_sigma2, const int32_t width, const int32_t height)
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
    ROW_PROLOGUE_SSE

    for(int i = 0; i < width; i++)
    {
      SUM_PIXEL_PROLOGUE_SSE
      for(int jj = 0; jj < 5; jj++)
      {
        for(int ii = 0; ii < 5; ii++)
        {
          SUM_PIXEL_CONTRIBUTION_WITH_TEST_SSE(ii, jj);
        }
      }
      SUM_PIXEL_EPILOGUE_SSE
    }
  }

#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static)
#endif
  for(int j = 2 * mult; j < height - 2 * mult; j++)
  {
    ROW_PROLOGUE_SSE

    /* The first "2*mult" pixels use the macro with tests because the 5x5 kernel
     * requires nearest pixel interpolation for at least a pixel in the sum */
    for(int i = 0; i < 2 * mult; i++)
    {
      SUM_PIXEL_PROLOGUE_SSE
      for(int jj = 0; jj < 5; jj++)
      {
        for(int ii = 0; ii < 5; ii++)
        {
          SUM_PIXEL_CONTRIBUTION_WITH_TEST_SSE(ii, jj);
        }
      }
      SUM_PIXEL_EPILOGUE_SSE
    }

    /* For pixels [2*mult, width-2*mult], we can safely use macro w/o tests
     * to avoid unneeded branching in the inner loops */
    for(int i = 2 * mult; i < width - 2 * mult; i++)
    {
      SUM_PIXEL_PROLOGUE_SSE
      px2 = ((__m128 *)in) + i - 2 * mult + (size_t)(j - 2 * mult) * width;
      for(int jj = 0; jj < 5; jj++)
      {
        for(int ii = 0; ii < 5; ii++)
        {
          SUM_PIXEL_CONTRIBUTION_COMMON_SSE(ii, jj);
          px2 += mult;
        }
        px2 += (width - 5) * mult;
      }
      SUM_PIXEL_EPILOGUE_SSE
    }

    /* Last two pixels in the row require a slow variant... blablabla */
    for(int i = width - 2 * mult; i < width; i++)
    {
      SUM_PIXEL_PROLOGUE_SSE
      for(int jj = 0; jj < 5; jj++)
      {
        for(int ii = 0; ii < 5; ii++)
        {
          SUM_PIXEL_CONTRIBUTION_WITH_TEST_SSE(ii, jj);
        }
      }
      SUM_PIXEL_EPILOGUE_SSE
    }
  }

/* The last "2*mult" lines use the macro with tests because the 5x5 kernel
 * requires nearest pixel interpolation for at least a pixel in the sum */
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static)
#endif
  for(int j = height - 2 * mult; j < height; j++)
  {
    ROW_PROLOGUE_SSE

    for(int i = 0; i < width; i++)
    {
      SUM_PIXEL_PROLOGUE_SSE
      for(int jj = 0; jj < 5; jj++)
      {
        for(int ii = 0; ii < 5; ii++)
        {
          SUM_PIXEL_CONTRIBUTION_WITH_TEST_SSE(ii, jj);
        }
      }
      SUM_PIXEL_EPILOGUE_SSE
    }
  }

  _mm_sfence();
}

#undef SUM_PIXEL_CONTRIBUTION_COMMON_SSE
#undef SUM_PIXEL_CONTRIBUTION_WITH_TEST_SSE
#undef ROW_PROLOGUE_SSE
#undef SUM_PIXEL_PROLOGUE_SSE
#undef SUM_PIXEL_EPILOGUE_SSE
#endif

typedef void((*eaw_synthesize_t)(float *const out, const float *const in, const float *const detail,
                                 const float *thrsf, const float *boostf, const int32_t width,
                                 const int32_t height));

static void eaw_synthesize(float *const out, const float *const in, const float *const detail,
                           const float *thrsf, const float *boostf, const int32_t width, const int32_t height)
{
  const float threshold[4] = { thrsf[0], thrsf[1], thrsf[2], thrsf[3] };
  const float boost[4] = { boostf[0], boostf[1], boostf[2], boostf[3] };

#ifdef _OPENMP
#pragma omp parallel for SIMD() default(none) schedule(static) collapse(2)
#endif
  for(size_t k = 0; k < (size_t)4 * width * height; k += 4)
  {
    for(size_t c = 0; c < 4; c++)
    {
      const float absamt = MAX(0.0f, (fabsf(detail[k + c]) - threshold[c]));
      const float amount = copysignf(absamt, detail[k + c]);
      out[k + c] = in[k + c] + (boost[c] * amount);
    }
  }
}

#if defined(__SSE2__)
static void eaw_synthesize_sse2(float *const out, const float *const in, const float *const detail,
                                const float *thrsf, const float *boostf, const int32_t width,
                                const int32_t height)
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
#if 1
      const __m128i maski = _mm_set1_epi32(0x80000000u);
      const __m128 *mask = (__m128 *)&maski;
      const __m128 absamt
          = _mm_max_ps(_mm_setzero_ps(), _mm_sub_ps(_mm_andnot_ps(*mask, *pdetail), threshold));
      const __m128 amount = _mm_or_ps(_mm_and_ps(*pdetail, *mask), absamt);
      _mm_stream_ps(pout, _mm_add_ps(*pin, _mm_mul_ps(boost, amount)));
#endif
      // _mm_stream_ps(pout, _mm_add_ps(*pin, *pdetail));
      pdetail++;
      pin++;
      pout += 4;
    }
  }
  _mm_sfence();
}
#endif

// =====================================================================================

static void process_wavelets(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece,
                             const void *const ivoid, void *const ovoid, const dt_iop_roi_t *const roi_in,
                             const dt_iop_roi_t *const roi_out, const eaw_decompose_t decompose,
                             const eaw_synthesize_t synthesize)
{
  // this is called for preview and full pipe separately, each with its own pixelpipe piece.
  // get our data struct:
  dt_iop_denoiseprofile_params_t *d = (dt_iop_denoiseprofile_params_t *)piece->data;

  const int max_max_scale = 5; // hard limit
  int max_scale = 0;
  const float scale = roi_in->scale / piece->iscale;
  // largest desired filter on input buffer (20% of input dim)
  const float supp0
      = MIN(2 * (2 << (max_max_scale - 1)) + 1,
            MAX(piece->buf_in.height * piece->iscale, piece->buf_in.width * piece->iscale) * 0.2f);
  const float i0 = dt_log2f((supp0 - 1.0f) * .5f);
  for(; max_scale < max_max_scale; max_scale++)
  {
    // actual filter support on scaled buffer
    const float supp = 2 * (2 << max_scale) + 1;
    // approximates this filter size on unscaled input image:
    const float supp_in = supp * (1.0f / scale);
    const float i_in = dt_log2f((supp_in - 1) * .5f) - 1.0f;
    // i_in = max_scale .. .. .. 0
    const float t = 1.0f - (i_in + .5f) / i0;
    if(t < 0.0f) break;
  }

  const int width = roi_in->width, height = roi_in->height;
  const size_t npixels = (size_t)width*height;

  // corner case of extremely small image. this is not really likely to happen but would cause issues later
  // when we divide by (n-1). so let's be prepared
  if(npixels < 2)
  {
    memcpy(ovoid, ivoid, npixels * 4 * sizeof(float));
    return;
  }

  float *buf[max_max_scale];
  float *tmp = NULL;
  float *buf1 = NULL, *buf2 = NULL;
  for(int k = 0; k < max_scale; k++)
    buf[k] = dt_alloc_align(64, (size_t)4 * sizeof(float) * npixels);
  tmp = dt_alloc_align(64, (size_t)4 * sizeof(float) * npixels);

  const float wb[3] = { // twice as many samples in green channel:
                        2.0f * piece->pipe->processed_maximum[0] * d->strength * (scale * scale),
                        piece->pipe->processed_maximum[1] * d->strength * (scale * scale),
                        2.0f * piece->pipe->processed_maximum[2] * d->strength * (scale * scale)
  };
  // only use green channel + wb for now:
  const float aa[3] = { d->a[1] * wb[0], d->a[1] * wb[1], d->a[1] * wb[2] };
  const float bb[3] = { d->b[1] * wb[0], d->b[1] * wb[1], d->b[1] * wb[2] };


  precondition((float *)ivoid, (float *)ovoid, width, height, aa, bb);

#if 0 // DEBUG: see what variance we have after transform
  if(piece->pipe->type != DT_DEV_PIXELPIPE_PREVIEW)
  {
    const int n = width*height;
    FILE *f = fopen("/tmp/transformed.pfm", "wb");
    fprintf(f, "PF\n%d %d\n-1.0\n", width, height);
    for(int k=0; k<n; k++)
      fwrite(((float*)ovoid)+4*k, sizeof(float), 3, f);
    fclose(f);
  }
#endif

  buf1 = (float *)ovoid;
  buf2 = tmp;

  for(int scale = 0; scale < max_scale; scale++)
  {
    const float sigma = 1.0f;
    const float varf = sqrtf(2.0f + 2.0f * 4.0f * 4.0f + 6.0f * 6.0f) / 16.0f; // about 0.5
    const float sigma_band = powf(varf, scale) * sigma;
    decompose(buf2, buf1, buf[scale], scale, 1.0f / (sigma_band * sigma_band), width, height);
// DEBUG: clean out temporary memory:
// memset(buf1, 0, sizeof(float)*4*width*height);
#if 0 // DEBUG: print wavelet scales:
    if(piece->pipe->type != DT_DEV_PIXELPIPE_PREVIEW)
    {
      char filename[512];
      snprintf(filename, sizeof(filename), "/tmp/coarse_%d.pfm", scale);
      FILE *f = fopen(filename, "wb");
      fprintf(f, "PF\n%d %d\n-1.0\n", width, height);
      for(size_t k = 0; k < npixels; k++)
        fwrite(buf2+4*k, sizeof(float), 3, f);
      fclose(f);
      snprintf(filename, sizeof(filename), "/tmp/detail_%d.pfm", scale);
      f = fopen(filename, "wb");
      fprintf(f, "PF\n%d %d\n-1.0\n", width, height);
      for(size_t k = 0; k < npixels; k++)
        fwrite(buf[scale]+4*k, sizeof(float), 3, f);
      fclose(f);
    }
#endif
    float *buf3 = buf2;
    buf2 = buf1;
    buf1 = buf3;
  }

  // now do everything backwards, so the result will end up in *ovoid
  for(int scale = max_scale - 1; scale >= 0; scale--)
  {
#if 1
    // variance stabilizing transform maps sigma to unity.
    const float sigma = 1.0f;
    // it is then transformed by wavelet scales via the 5 tap a-trous filter:
    const float varf = sqrtf(2.0f + 2.0f * 4.0f * 4.0f + 6.0f * 6.0f) / 16.0f; // about 0.5
    const float sigma_band = powf(varf, scale) * sigma;
    // determine thrs as bayesshrink
    // TODO: parallelize!
    float sum_y2[3] = { 0.0f };
    for(size_t k = 0; k < npixels; k++)
      for(int c = 0; c < 3; c++) sum_y2[c] += buf[scale][4 * k + c] * buf[scale][4 * k + c];

    const float sb2 = sigma_band * sigma_band;
    const float var_y[3] = { sum_y2[0] / (npixels - 1.0f), sum_y2[1] / (npixels - 1.0f), sum_y2[2] / (npixels - 1.0f) };
    const float std_x[3] = { sqrtf(MAX(1e-6f, var_y[0] - sb2)), sqrtf(MAX(1e-6f, var_y[1] - sb2)),
                             sqrtf(MAX(1e-6f, var_y[2] - sb2)) };
    // add 8.0 here because it seemed a little weak
    const float adjt = 8.0f;
    const float thrs[4] = { adjt * sb2 / std_x[0], adjt * sb2 / std_x[1], adjt * sb2 / std_x[2], 0.0f };
// const float std = (std_x[0] + std_x[1] + std_x[2])/3.0f;
// const float thrs[4] = { adjt*sigma*sigma/std, adjt*sigma*sigma/std, adjt*sigma*sigma/std, 0.0f};
// fprintf(stderr, "scale %d thrs %f %f %f = %f / %f %f %f \n", scale, thrs[0], thrs[1], thrs[2], sb2,
// std_x[0], std_x[1], std_x[2]);
#endif
    const float boost[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    // const float thrs[4] = { 0.0, 0.0, 0.0, 0.0 };
    synthesize(buf2, buf1, buf[scale], thrs, boost, width, height);
    // DEBUG: clean out temporary memory:
    // memset(buf1, 0, sizeof(float)*4*width*height);

    float *buf3 = buf2;
    buf2 = buf1;
    buf1 = buf3;
  }

  backtransform((float *)ovoid, width, height, aa, bb);

  for(int k = 0; k < max_scale; k++) dt_free_align(buf[k]);
  dt_free_align(tmp);

  if(piece->pipe->mask_display) dt_iop_alpha_copy(ivoid, ovoid, width, height);
}

static void process_nlmeans(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece,
                            const void *const ivoid, void *const ovoid, const dt_iop_roi_t *const roi_in,
                            const dt_iop_roi_t *const roi_out)
{
  // this is called for preview and full pipe separately, each with its own pixelpipe piece.
  // get our data struct:
  const dt_iop_denoiseprofile_params_t *const d = (const dt_iop_denoiseprofile_params_t *const)piece->data;

  const int ch = piece->colors;

  // TODO: fixed K to use adaptive size trading variance and bias!
  // adjust to zoom size:
  const float scale = fmin(roi_in->scale, 2.0f) / fmax(piece->iscale, 1.0f);
  const int P = ceilf(d->radius * scale); // pixel filter size
  const int K = ceilf(7 * scale);         // nbhood

  // P == 0 : this will degenerate to a (fast) bilateral filter.

  float *Sa = dt_alloc_align(64, (size_t)sizeof(float) * roi_out->width * dt_get_num_threads());
  // we want to sum up weights in col[3], so need to init to 0:
  memset(ovoid, 0x0, (size_t)sizeof(float) * roi_out->width * roi_out->height * 4);
  float *in = dt_alloc_align(64, (size_t)4 * sizeof(float) * roi_in->width * roi_in->height);

  const float wb[3] = { piece->pipe->processed_maximum[0] * d->strength * (scale * scale),
                        piece->pipe->processed_maximum[1] * d->strength * (scale * scale),
                        piece->pipe->processed_maximum[2] * d->strength * (scale * scale) };
  const float aa[3] = { d->a[1] * wb[0], d->a[1] * wb[1], d->a[1] * wb[2] };
  const float bb[3] = { d->b[1] * wb[0], d->b[1] * wb[1], d->b[1] * wb[2] };
  precondition((float *)ivoid, in, roi_in->width, roi_in->height, aa, bb);

  // for each shift vector
  for(int kj = -K; kj <= K; kj++)
  {
    for(int ki = -K; ki <= K; ki++)
    {
      // TODO: adaptive K tests here!
      // TODO: expf eval for real bilateral experience :)

      int inited_slide = 0;
// don't construct summed area tables but use sliding window! (applies to cpu version res < 1k only, or else
// we will add up errors)
// do this in parallel with a little threading overhead. could parallelize the outer loops with a bit more
// memory
#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none) firstprivate(inited_slide) shared(kj, ki, in, Sa)
#endif
      for(int j = 0; j < roi_out->height; j++)
      {
        if(j + kj < 0 || j + kj >= roi_out->height) continue;
        float *S = Sa + dt_get_thread_num() * roi_out->width;
        const float *ins = in + 4l * ((size_t)roi_in->width * (j + kj) + ki);
        float *out = ((float *)ovoid) + (size_t)4 * roi_out->width * j;

        const int Pm = MIN(MIN(P, j + kj), j);
        const int PM = MIN(MIN(P, roi_out->height - 1 - j - kj), roi_out->height - 1 - j);
        // first line of every thread
        // TODO: also every once in a while to assert numerical precision!
        if(!inited_slide)
        {
          // sum up a line
          memset(S, 0x0, sizeof(float) * roi_out->width);
          for(int jj = -Pm; jj <= PM; jj++)
          {
            int i = MAX(0, -ki);
            float *s = S + i;
            const float *inp = in + 4 * i + (size_t)4 * roi_in->width * (j + jj);
            const float *inps = in + 4 * i + 4l * ((size_t)roi_in->width * (j + jj + kj) + ki);
            const int last = roi_out->width + MIN(0, -ki);
            for(; i < last; i++, inp += 4, inps += 4, s++)
            {
              for(int k = 0; k < 3; k++) s[0] += (inp[k] - inps[k]) * (inp[k] - inps[k]);
            }
          }
          // only reuse this if we had a full stripe
          if(Pm == P && PM == P) inited_slide = 1;
        }

        // sliding window for this line:
        float *s = S;
        float slide = 0.0f;
        // sum up the first -P..P
        for(int i = 0; i < 2 * P + 1; i++) slide += s[i];
        for(int i = 0; i < roi_out->width; i++, s++, ins += 4, out += 4)
        {
          // FIXME: the comment above is actually relevant even for 1000 px width already.
          // XXX    numerical precision will not forgive us:
          if(i - P > 0 && i + P < roi_out->width) slide += s[P] - s[-P - 1];
          if(i + ki >= 0 && i + ki < roi_out->width)
          {
            // TODO: could put that outside the loop.
            // DEBUG XXX bring back to computable range:
            const float norm = .015f / (2 * P + 1);
            const float iv[4] = { ins[0], ins[1], ins[2], 1.0f };
#if defined(_OPENMP) && defined(OPENMP_SIMD_)
#pragma omp SIMD()
#endif
            for(size_t c = 0; c < 4; c++)
            {
              out[c] += iv[c] * fast_mexp2f(fmaxf(0.0f, slide * norm - 2.0f));
            }
          }
        }
        if(inited_slide && j + P + 1 + MAX(0, kj) < roi_out->height)
        {
          // sliding window in j direction:
          int i = MAX(0, -ki);
          float *s = S + i;
          const float *inp = in + 4 * i + 4l * (size_t)roi_in->width * (j + P + 1);
          const float *inps = in + 4 * i + 4l * ((size_t)roi_in->width * (j + P + 1 + kj) + ki);
          const float *inm = in + 4 * i + 4l * (size_t)roi_in->width * (j - P);
          const float *inms = in + 4 * i + 4l * ((size_t)roi_in->width * (j - P + kj) + ki);
          const int last = roi_out->width + MIN(0, -ki);
          for(; i < last; i++, inp += 4, inps += 4, inm += 4, inms += 4, s++)
          {
            float stmp = s[0];
            for(int k = 0; k < 3; k++)
              stmp += ((inp[k] - inps[k]) * (inp[k] - inps[k]) - (inm[k] - inms[k]) * (inm[k] - inms[k]));
            s[0] = stmp;
          }
        }
        else
          inited_slide = 0;
      }
    }
  }

  float *const out = ((float *const)ovoid);

// normalize
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static)
#endif
  for(size_t k = 0; k < (size_t)ch * roi_out->width * roi_out->height; k += ch)
  {
    if(out[k + 3] <= 0.0f) continue;
    for(size_t c = 0; c < 4; c++)
    {
      out[k + c] *= (1.0f / out[k + 3]);
    }
  }

  // free shared tmp memory:
  dt_free_align(Sa);
  dt_free_align(in);
  backtransform((float *)ovoid, roi_in->width, roi_in->height, aa, bb);

  if(piece->pipe->mask_display) dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);
}

#if defined(__SSE2__)
static void process_nlmeans_sse(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece,
                                const void *const ivoid, void *const ovoid, const dt_iop_roi_t *const roi_in,
                                const dt_iop_roi_t *const roi_out)
{
  // this is called for preview and full pipe separately, each with its own pixelpipe piece.
  // get our data struct:
  dt_iop_denoiseprofile_params_t *d = (dt_iop_denoiseprofile_params_t *)piece->data;

  // TODO: fixed K to use adaptive size trading variance and bias!
  // adjust to zoom size:
  const float scale = fmin(roi_in->scale, 2.0f) / fmax(piece->iscale, 1.0f);
  const int P = ceilf(d->radius * scale); // pixel filter size
  const int K = ceilf(7 * scale);         // nbhood

  // P == 0 : this will degenerate to a (fast) bilateral filter.

  float *Sa = dt_alloc_align(64, (size_t)sizeof(float) * roi_out->width * dt_get_num_threads());
  // we want to sum up weights in col[3], so need to init to 0:
  memset(ovoid, 0x0, (size_t)sizeof(float) * roi_out->width * roi_out->height * 4);
  float *in = dt_alloc_align(64, (size_t)4 * sizeof(float) * roi_in->width * roi_in->height);

  const float wb[3] = { piece->pipe->processed_maximum[0] * d->strength * (scale * scale),
                        piece->pipe->processed_maximum[1] * d->strength * (scale * scale),
                        piece->pipe->processed_maximum[2] * d->strength * (scale * scale) };
  const float aa[3] = { d->a[1] * wb[0], d->a[1] * wb[1], d->a[1] * wb[2] };
  const float bb[3] = { d->b[1] * wb[0], d->b[1] * wb[1], d->b[1] * wb[2] };
  precondition((float *)ivoid, in, roi_in->width, roi_in->height, aa, bb);

  // for each shift vector
  for(int kj = -K; kj <= K; kj++)
  {
    for(int ki = -K; ki <= K; ki++)
    {
      // TODO: adaptive K tests here!
      // TODO: expf eval for real bilateral experience :)

      int inited_slide = 0;
// don't construct summed area tables but use sliding window! (applies to cpu version res < 1k only, or else
// we will add up errors)
// do this in parallel with a little threading overhead. could parallelize the outer loops with a bit more
// memory
#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none) firstprivate(inited_slide) shared(kj, ki, in, Sa)
#endif
      for(int j = 0; j < roi_out->height; j++)
      {
        if(j + kj < 0 || j + kj >= roi_out->height) continue;
        float *S = Sa + dt_get_thread_num() * roi_out->width;
        const float *ins = in + 4l * ((size_t)roi_in->width * (j + kj) + ki);
        float *out = ((float *)ovoid) + (size_t)4 * roi_out->width * j;

        const int Pm = MIN(MIN(P, j + kj), j);
        const int PM = MIN(MIN(P, roi_out->height - 1 - j - kj), roi_out->height - 1 - j);
        // first line of every thread
        // TODO: also every once in a while to assert numerical precision!
        if(!inited_slide)
        {
          // sum up a line
          memset(S, 0x0, sizeof(float) * roi_out->width);
          for(int jj = -Pm; jj <= PM; jj++)
          {
            int i = MAX(0, -ki);
            float *s = S + i;
            const float *inp = in + 4 * i + (size_t)4 * roi_in->width * (j + jj);
            const float *inps = in + 4 * i + 4l * ((size_t)roi_in->width * (j + jj + kj) + ki);
            const int last = roi_out->width + MIN(0, -ki);
            for(; i < last; i++, inp += 4, inps += 4, s++)
            {
              for(int k = 0; k < 3; k++) s[0] += (inp[k] - inps[k]) * (inp[k] - inps[k]);
            }
          }
          // only reuse this if we had a full stripe
          if(Pm == P && PM == P) inited_slide = 1;
        }

        // sliding window for this line:
        float *s = S;
        float slide = 0.0f;
        // sum up the first -P..P
        for(int i = 0; i < 2 * P + 1; i++) slide += s[i];
        for(int i = 0; i < roi_out->width; i++)
        {
          // FIXME: the comment above is actually relevant even for 1000 px width already.
          // XXX    numerical precision will not forgive us:
          if(i - P > 0 && i + P < roi_out->width) slide += s[P] - s[-P - 1];
          if(i + ki >= 0 && i + ki < roi_out->width)
          {
            // TODO: could put that outside the loop.
            // DEBUG XXX bring back to computable range:
            const float norm = .015f / (2 * P + 1);
            const __m128 iv = { ins[0], ins[1], ins[2], 1.0f };
            _mm_store_ps(out,
                         _mm_load_ps(out) + iv * _mm_set1_ps(fast_mexp2f(fmaxf(0.0f, slide * norm - 2.0f))));
            // _mm_store_ps(out, _mm_load_ps(out) + iv * _mm_set1_ps(fast_mexp2f(fmaxf(0.0f, slide*norm))));
          }
          s++;
          ins += 4;
          out += 4;
        }
        if(inited_slide && j + P + 1 + MAX(0, kj) < roi_out->height)
        {
          // sliding window in j direction:
          int i = MAX(0, -ki);
          float *s = S + i;
          const float *inp = in + 4 * i + 4l * (size_t)roi_in->width * (j + P + 1);
          const float *inps = in + 4 * i + 4l * ((size_t)roi_in->width * (j + P + 1 + kj) + ki);
          const float *inm = in + 4 * i + 4l * (size_t)roi_in->width * (j - P);
          const float *inms = in + 4 * i + 4l * ((size_t)roi_in->width * (j - P + kj) + ki);
          const int last = roi_out->width + MIN(0, -ki);
          for(; ((intptr_t)s & 0xf) != 0 && i < last; i++, inp += 4, inps += 4, inm += 4, inms += 4, s++)
          {
            float stmp = s[0];
            for(int k = 0; k < 3; k++)
              stmp += ((inp[k] - inps[k]) * (inp[k] - inps[k]) - (inm[k] - inms[k]) * (inm[k] - inms[k]));
            s[0] = stmp;
          }
          /* Process most of the line 4 pixels at a time */
          for(; i < last - 4; i += 4, inp += 16, inps += 16, inm += 16, inms += 16, s += 4)
          {
            __m128 sv = _mm_load_ps(s);
            const __m128 inp1 = _mm_sub_ps(_mm_load_ps(inp), _mm_load_ps(inps));
            const __m128 inp2 = _mm_sub_ps(_mm_load_ps(inp + 4), _mm_load_ps(inps + 4));
            const __m128 inp3 = _mm_sub_ps(_mm_load_ps(inp + 8), _mm_load_ps(inps + 8));
            const __m128 inp4 = _mm_sub_ps(_mm_load_ps(inp + 12), _mm_load_ps(inps + 12));

            const __m128 inp12lo = _mm_unpacklo_ps(inp1, inp2);
            const __m128 inp34lo = _mm_unpacklo_ps(inp3, inp4);
            const __m128 inp12hi = _mm_unpackhi_ps(inp1, inp2);
            const __m128 inp34hi = _mm_unpackhi_ps(inp3, inp4);

            const __m128 inpv0 = _mm_movelh_ps(inp12lo, inp34lo);
            sv += inpv0 * inpv0;

            const __m128 inpv1 = _mm_movehl_ps(inp34lo, inp12lo);
            sv += inpv1 * inpv1;

            const __m128 inpv2 = _mm_movelh_ps(inp12hi, inp34hi);
            sv += inpv2 * inpv2;

            const __m128 inm1 = _mm_sub_ps(_mm_load_ps(inm), _mm_load_ps(inms));
            const __m128 inm2 = _mm_sub_ps(_mm_load_ps(inm + 4), _mm_load_ps(inms + 4));
            const __m128 inm3 = _mm_sub_ps(_mm_load_ps(inm + 8), _mm_load_ps(inms + 8));
            const __m128 inm4 = _mm_sub_ps(_mm_load_ps(inm + 12), _mm_load_ps(inms + 12));

            const __m128 inm12lo = _mm_unpacklo_ps(inm1, inm2);
            const __m128 inm34lo = _mm_unpacklo_ps(inm3, inm4);
            const __m128 inm12hi = _mm_unpackhi_ps(inm1, inm2);
            const __m128 inm34hi = _mm_unpackhi_ps(inm3, inm4);

            const __m128 inmv0 = _mm_movelh_ps(inm12lo, inm34lo);
            sv -= inmv0 * inmv0;

            const __m128 inmv1 = _mm_movehl_ps(inm34lo, inm12lo);
            sv -= inmv1 * inmv1;

            const __m128 inmv2 = _mm_movelh_ps(inm12hi, inm34hi);
            sv -= inmv2 * inmv2;

            _mm_store_ps(s, sv);
          }
          for(; i < last; i++, inp += 4, inps += 4, inm += 4, inms += 4, s++)
          {
            float stmp = s[0];
            for(int k = 0; k < 3; k++)
              stmp += ((inp[k] - inps[k]) * (inp[k] - inps[k]) - (inm[k] - inms[k]) * (inm[k] - inms[k]));
            s[0] = stmp;
          }
        }
        else
          inited_slide = 0;
      }
    }
  }
// normalize
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static) shared(d)
#endif
  for(int j = 0; j < roi_out->height; j++)
  {
    float *out = ((float *)ovoid) + (size_t)4 * roi_out->width * j;
    for(int i = 0; i < roi_out->width; i++)
    {
      if(out[3] > 0.0f) _mm_store_ps(out, _mm_mul_ps(_mm_load_ps(out), _mm_set1_ps(1.0f / out[3])));
      // DEBUG show weights
      // _mm_store_ps(out, _mm_set1_ps(1.0f/out[3]));
      out += 4;
    }
  }
  // free shared tmp memory:
  dt_free_align(Sa);
  dt_free_align(in);
  backtransform((float *)ovoid, roi_in->width, roi_in->height, aa, bb);

  if(piece->pipe->mask_display) dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);
}
#endif

#ifdef HAVE_OPENCL
static int bucket_next(unsigned int *state, unsigned int max)
{
  unsigned int current = *state;
  unsigned int next = (current >= max - 1 ? 0 : current + 1);

  *state = next;

  return next;
}

static int process_nlmeans_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in,
                              cl_mem dev_out, const dt_iop_roi_t *const roi_in,
                              const dt_iop_roi_t *const roi_out)
{
  dt_iop_denoiseprofile_params_t *d = (dt_iop_denoiseprofile_params_t *)piece->data;
  dt_iop_denoiseprofile_global_data_t *gd = (dt_iop_denoiseprofile_global_data_t *)self->data;

  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;

  cl_mem dev_tmp = NULL;
  cl_mem dev_U2 = NULL;
  cl_mem dev_U4 = NULL;
  cl_mem dev_U4_t = NULL;
  cl_mem dev_U4_tt = NULL;

  unsigned int state = 0;
  cl_mem buckets[NUM_BUCKETS] = { NULL };


  cl_int err = -999;

  const float scale = fmin(roi_in->scale, 2.0f) / fmax(piece->iscale, 1.0f);
  const int P = ceilf(d->radius * scale); // pixel filter size
  const int K = ceilf(7 * scale);         // nbhood
  const float norm = 0.015f / (2 * P + 1);


  const float wb[4]
      = { piece->pipe->processed_maximum[0] * d->strength * (scale * scale),
          piece->pipe->processed_maximum[1] * d->strength * (scale * scale),
          piece->pipe->processed_maximum[2] * d->strength * (scale * scale),
          0.0f };
  const float aa[4] = { d->a[1] * wb[0], d->a[1] * wb[1], d->a[1] * wb[2], 1.0f };
  const float bb[4] = { d->b[1] * wb[0], d->b[1] * wb[1], d->b[1] * wb[2], 1.0f };
  const float sigma2[4] = { (bb[0] / aa[0]) * (bb[0] / aa[0]), (bb[1] / aa[1]) * (bb[1] / aa[1]),
                            (bb[2] / aa[1]) * (bb[2] / aa[1]), 0.0f };

  dev_tmp = dt_opencl_alloc_device(devid, width, height, 4 * sizeof(float));
  if(dev_tmp == NULL) goto error;

  dev_U2 = dt_opencl_alloc_device_buffer(devid, (size_t)width * height * 4 * sizeof(float));
  if(dev_U2 == NULL) goto error;

  for(int k = 0; k < NUM_BUCKETS; k++)
  {
    buckets[k] = dt_opencl_alloc_device_buffer(devid, (size_t)width * height * sizeof(float));
    if(buckets[k] == NULL) goto error;
  }

  // prepare local work group
  size_t maxsizes[3] = { 0 };     // the maximum dimensions for a work group
  size_t workgroupsize = 0;       // the maximum number of items in a work group
  unsigned long localmemsize = 0; // the maximum amount of local memory we can use
  size_t kernelworkgroupsize = 0; // the maximum amount of items in work group of the kernel
  // assuming this is the same for denoiseprofile_horiz and denoiseprofile_vert

  // make sure blocksize is not too large
  int blocksize = BLOCKSIZE;
  if(dt_opencl_get_work_group_limits(devid, maxsizes, &workgroupsize, &localmemsize) == CL_SUCCESS
     && dt_opencl_get_kernel_work_group_size(devid, gd->kernel_denoiseprofile_horiz, &kernelworkgroupsize)
        == CL_SUCCESS)
  {
    // reduce blocksize step by step until it fits to limits
    while(blocksize > maxsizes[0] || blocksize > maxsizes[1] || blocksize > kernelworkgroupsize
          || blocksize > workgroupsize || (blocksize + 2 * P) * sizeof(float) > localmemsize)
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

  const size_t sizes[] = { ROUNDUPWD(width), ROUNDUPHT(height), 1 };
  size_t sizesl[3];
  size_t local[3];

  dt_opencl_set_kernel_arg(devid, gd->kernel_denoiseprofile_precondition, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_denoiseprofile_precondition, 1, sizeof(cl_mem), (void *)&dev_tmp);
  dt_opencl_set_kernel_arg(devid, gd->kernel_denoiseprofile_precondition, 2, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_denoiseprofile_precondition, 3, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_denoiseprofile_precondition, 4, 4 * sizeof(float), (void *)&aa);
  dt_opencl_set_kernel_arg(devid, gd->kernel_denoiseprofile_precondition, 5, 4 * sizeof(float),
                           (void *)&sigma2);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_denoiseprofile_precondition, sizes);
  if(err != CL_SUCCESS) goto error;


  dt_opencl_set_kernel_arg(devid, gd->kernel_denoiseprofile_init, 0, sizeof(cl_mem), (void *)&dev_U2);
  dt_opencl_set_kernel_arg(devid, gd->kernel_denoiseprofile_init, 1, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_denoiseprofile_init, 2, sizeof(int), (void *)&height);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_denoiseprofile_init, sizes);
  if(err != CL_SUCCESS) goto error;


  for(int j = -K; j <= 0; j++)
    for(int i = -K; i <= K; i++)
    {
      int q[2] = { i, j };

      dev_U4 = buckets[bucket_next(&state, NUM_BUCKETS)];
      dt_opencl_set_kernel_arg(devid, gd->kernel_denoiseprofile_dist, 0, sizeof(cl_mem), (void *)&dev_tmp);
      dt_opencl_set_kernel_arg(devid, gd->kernel_denoiseprofile_dist, 1, sizeof(cl_mem), (void *)&dev_U4);
      dt_opencl_set_kernel_arg(devid, gd->kernel_denoiseprofile_dist, 2, sizeof(int), (void *)&width);
      dt_opencl_set_kernel_arg(devid, gd->kernel_denoiseprofile_dist, 3, sizeof(int), (void *)&height);
      dt_opencl_set_kernel_arg(devid, gd->kernel_denoiseprofile_dist, 4, 2 * sizeof(int), (void *)&q);
      err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_denoiseprofile_dist, sizes);
      if(err != CL_SUCCESS) goto error;

      sizesl[0] = bwidth;
      sizesl[1] = ROUNDUPHT(height);
      sizesl[2] = 1;
      local[0] = blocksize;
      local[1] = 1;
      local[2] = 1;
      dev_U4_t = buckets[bucket_next(&state, NUM_BUCKETS)];
      dt_opencl_set_kernel_arg(devid, gd->kernel_denoiseprofile_horiz, 0, sizeof(cl_mem), (void *)&dev_U4);
      dt_opencl_set_kernel_arg(devid, gd->kernel_denoiseprofile_horiz, 1, sizeof(cl_mem), (void *)&dev_U4_t);
      dt_opencl_set_kernel_arg(devid, gd->kernel_denoiseprofile_horiz, 2, sizeof(int), (void *)&width);
      dt_opencl_set_kernel_arg(devid, gd->kernel_denoiseprofile_horiz, 3, sizeof(int), (void *)&height);
      dt_opencl_set_kernel_arg(devid, gd->kernel_denoiseprofile_horiz, 4, 2 * sizeof(int), (void *)&q);
      dt_opencl_set_kernel_arg(devid, gd->kernel_denoiseprofile_horiz, 5, sizeof(int), (void *)&P);
      dt_opencl_set_kernel_arg(devid, gd->kernel_denoiseprofile_horiz, 6, (blocksize + 2 * P) * sizeof(float),
                               NULL);
      err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_denoiseprofile_horiz, sizesl, local);
      if(err != CL_SUCCESS) goto error;


      sizesl[0] = ROUNDUPWD(width);
      sizesl[1] = bheight;
      sizesl[2] = 1;
      local[0] = 1;
      local[1] = blocksize;
      local[2] = 1;
      dev_U4_tt = buckets[bucket_next(&state, NUM_BUCKETS)];
      dt_opencl_set_kernel_arg(devid, gd->kernel_denoiseprofile_vert, 0, sizeof(cl_mem), (void *)&dev_U4_t);
      dt_opencl_set_kernel_arg(devid, gd->kernel_denoiseprofile_vert, 1, sizeof(cl_mem), (void *)&dev_U4_tt);
      dt_opencl_set_kernel_arg(devid, gd->kernel_denoiseprofile_vert, 2, sizeof(int), (void *)&width);
      dt_opencl_set_kernel_arg(devid, gd->kernel_denoiseprofile_vert, 3, sizeof(int), (void *)&height);
      dt_opencl_set_kernel_arg(devid, gd->kernel_denoiseprofile_vert, 4, 2 * sizeof(int), (void *)&q);
      dt_opencl_set_kernel_arg(devid, gd->kernel_denoiseprofile_vert, 5, sizeof(int), (void *)&P);
      dt_opencl_set_kernel_arg(devid, gd->kernel_denoiseprofile_vert, 6, sizeof(float), (void *)&norm);
      dt_opencl_set_kernel_arg(devid, gd->kernel_denoiseprofile_vert, 7, (blocksize + 2 * P) * sizeof(float),
                               NULL);
      err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_denoiseprofile_vert, sizesl, local);
      if(err != CL_SUCCESS) goto error;


      dt_opencl_set_kernel_arg(devid, gd->kernel_denoiseprofile_accu, 0, sizeof(cl_mem), (void *)&dev_tmp);
      dt_opencl_set_kernel_arg(devid, gd->kernel_denoiseprofile_accu, 1, sizeof(cl_mem), (void *)&dev_U2);
      dt_opencl_set_kernel_arg(devid, gd->kernel_denoiseprofile_accu, 2, sizeof(cl_mem), (void *)&dev_U4_tt);
      dt_opencl_set_kernel_arg(devid, gd->kernel_denoiseprofile_accu, 3, sizeof(int), (void *)&width);
      dt_opencl_set_kernel_arg(devid, gd->kernel_denoiseprofile_accu, 4, sizeof(int), (void *)&height);
      dt_opencl_set_kernel_arg(devid, gd->kernel_denoiseprofile_accu, 5, 2 * sizeof(int), (void *)&q);
      err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_denoiseprofile_accu, sizes);
      if(err != CL_SUCCESS) goto error;

      if(!darktable.opencl->async_pixelpipe || piece->pipe->type == DT_DEV_PIXELPIPE_EXPORT)
        dt_opencl_finish(devid);

      // indirectly give gpu some air to breathe (and to do display related stuff)
      dt_iop_nap(darktable.opencl->micro_nap);
    }

  dt_opencl_set_kernel_arg(devid, gd->kernel_denoiseprofile_finish, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_denoiseprofile_finish, 1, sizeof(cl_mem), (void *)&dev_U2);
  dt_opencl_set_kernel_arg(devid, gd->kernel_denoiseprofile_finish, 2, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(devid, gd->kernel_denoiseprofile_finish, 3, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_denoiseprofile_finish, 4, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_denoiseprofile_finish, 5, 4 * sizeof(float), (void *)&aa);
  dt_opencl_set_kernel_arg(devid, gd->kernel_denoiseprofile_finish, 6, 4 * sizeof(float), (void *)&sigma2);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_denoiseprofile_finish, sizes);
  if(err != CL_SUCCESS) goto error;

  for(int k = 0; k < NUM_BUCKETS; k++)
  {
    if(buckets[k] != NULL) dt_opencl_release_mem_object(buckets[k]);
  }
  if(dev_U2 != NULL) dt_opencl_release_mem_object(dev_U2);
  if(dev_tmp != NULL) dt_opencl_release_mem_object(dev_tmp);
  return TRUE;

error:
  for(int k = 0; k < NUM_BUCKETS; k++)
  {
    if(buckets[k] != NULL) dt_opencl_release_mem_object(buckets[k]);
  }
  if(dev_U2 != NULL) dt_opencl_release_mem_object(dev_U2);
  if(dev_tmp != NULL) dt_opencl_release_mem_object(dev_tmp);
  dt_print(DT_DEBUG_OPENCL, "[opencl_denoiseprofile] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}


static int process_wavelets_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in,
                               cl_mem dev_out, const dt_iop_roi_t *const roi_in,
                               const dt_iop_roi_t *const roi_out)
{
  dt_iop_denoiseprofile_data_t *d = (dt_iop_denoiseprofile_data_t *)piece->data;
  dt_iop_denoiseprofile_global_data_t *gd = (dt_iop_denoiseprofile_global_data_t *)self->data;

  const int max_max_scale = 5; // hard limit
  int max_scale = 0;
  const float scale = roi_in->scale / piece->iscale;
  // largest desired filter on input buffer (20% of input dim)
  const float supp0
      = MIN(2 * (2 << (max_max_scale - 1)) + 1,
            MAX(piece->buf_in.height * piece->iscale, piece->buf_in.width * piece->iscale) * 0.2f);
  const float i0 = dt_log2f((supp0 - 1.0f) * .5f);
  for(; max_scale < max_max_scale; max_scale++)
  {
    // actual filter support on scaled buffer
    const float supp = 2 * (2 << max_scale) + 1;
    // approximates this filter size on unscaled input image:
    const float supp_in = supp * (1.0f / scale);
    const float i_in = dt_log2f((supp_in - 1) * .5f) - 1.0f;
    // i_in = max_scale .. .. .. 0
    const float t = 1.0f - (i_in + .5f) / i0;
    if(t < 0.0f) break;
  }

  const int devid = piece->pipe->devid;
  cl_int err = -999;
  const int width = roi_in->width;
  const int height = roi_in->height;
  const size_t npixels = (size_t)width * height;

  cl_mem dev_tmp = NULL;
  cl_mem dev_buf1 = NULL;
  cl_mem dev_buf2 = NULL;
  cl_mem dev_m = NULL;
  cl_mem dev_r = NULL;
  cl_mem dev_filter = NULL;
  cl_mem dev_detail[max_max_scale];
  for(int k = 0; k < max_scale; k++) dev_detail[k] = NULL;

  // corner case of extremely small image. this is not really likely to happen but would cause issues later
  // when we divide by (n-1). so let's be prepared
  if(npixels < 2)
  {
    // copy original input from dev_in -> dev_out
    size_t origin[] = { 0, 0, 0 };
    size_t region[] = { width, height, 1 };
    err = dt_opencl_enqueue_copy_image(devid, dev_in, dev_out, origin, origin, region);
    if(err != CL_SUCCESS) goto error;
    return TRUE;
  }

  // prepare local work group
  size_t maxsizes[3] = { 0 };     // the maximum dimensions for a work group
  size_t workgroupsize = 0;       // the maximum number of items in a work group
  unsigned long localmemsize = 0; // the maximum amount of local memory we can use
  size_t kernelworkgroupsize = 0; // the maximum amount of items in work group for this kernel

  // make sure blocksize is not too large
  int blocksize = BLOCKSIZE;
  if(dt_opencl_get_work_group_limits(devid, maxsizes, &workgroupsize, &localmemsize) == CL_SUCCESS
     && dt_opencl_get_kernel_work_group_size(devid, gd->kernel_denoiseprofile_reduce_first,
                                             &kernelworkgroupsize) == CL_SUCCESS)
  {
    // reduce blocksize step by step until it fits to limits
    while(blocksize > maxsizes[0] || blocksize > maxsizes[1] || blocksize * blocksize > kernelworkgroupsize
          || blocksize * blocksize > workgroupsize
          || blocksize * blocksize * 4 * sizeof(float) > localmemsize)
    {
      if(blocksize == 1) break;
      blocksize >>= 1;
    }
  }
  else
  {
    blocksize = 1;
  }

  if(blocksize < 2)
  {
    // very small blocksize. this is really unlikely to happen, but let's be prepared: give up on opencl in
    // that case
    return FALSE;
  }

  const size_t bwidth = ROUNDUP(width, blocksize);
  const size_t bheight = ROUNDUP(height, blocksize);

  const int bufsize = (bwidth / blocksize) * (bheight / blocksize);
  const int lsize = maxsizes[0];
  const int reducesize = MIN(REDUCESIZE, ROUNDUP(bufsize, lsize) / lsize);

  dev_m = dt_opencl_alloc_device_buffer(devid, (size_t)bufsize * 4 * sizeof(float));
  if(dev_m == NULL) goto error;

  dev_r = dt_opencl_alloc_device_buffer(devid, (size_t)reducesize * 4 * sizeof(float));
  if(dev_r == NULL) goto error;

  dev_tmp = dt_opencl_alloc_device(devid, width, height, 4 * sizeof(float));
  if(dev_tmp == NULL) goto error;

  float m[] = { 0.0625f, 0.25f, 0.375f, 0.25f, 0.0625f }; // 1/16, 4/16, 6/16, 4/16, 1/16
  float mm[5][5];
  for(int j = 0; j < 5; j++)
    for(int i = 0; i < 5; i++) mm[j][i] = m[i] * m[j];

  dev_filter = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * 25, mm);
  if(dev_filter == NULL) goto error;

  for(int k = 0; k < max_scale; k++)
  {
    dev_detail[k] = dt_opencl_alloc_device(devid, width, height, 4 * sizeof(float));
    if(dev_detail[k] == NULL) goto error;
  }

  const float wb[4] = { 2.0f * piece->pipe->processed_maximum[0] * d->strength * (scale * scale),
                        piece->pipe->processed_maximum[1] * d->strength * (scale * scale),
                        2.0f * piece->pipe->processed_maximum[2] * d->strength * (scale * scale), 0.0f };
  const float aa[4] = { d->a[1] * wb[0], d->a[1] * wb[1], d->a[1] * wb[2], 1.0f };
  const float bb[4] = { d->b[1] * wb[0], d->b[1] * wb[1], d->b[1] * wb[2], 1.0f };
  const float sigma2[4] = { (bb[0] / aa[0]) * (bb[0] / aa[0]), (bb[1] / aa[1]) * (bb[1] / aa[1]),
                            (bb[2] / aa[1]) * (bb[2] / aa[1]), 0.0f };

  size_t sizes[] = { ROUNDUPWD(width), ROUNDUPHT(height), 1 };

  dt_opencl_set_kernel_arg(devid, gd->kernel_denoiseprofile_precondition, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_denoiseprofile_precondition, 1, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(devid, gd->kernel_denoiseprofile_precondition, 2, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_denoiseprofile_precondition, 3, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_denoiseprofile_precondition, 4, 4 * sizeof(float), (void *)&aa);
  dt_opencl_set_kernel_arg(devid, gd->kernel_denoiseprofile_precondition, 5, 4 * sizeof(float),
                           (void *)&sigma2);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_denoiseprofile_precondition, sizes);
  if(err != CL_SUCCESS) goto error;

  dev_buf1 = dev_out;
  dev_buf2 = dev_tmp;

  /* decompose image into detail scales and coarse */
  for(int s = 0; s < max_scale; s++)
  {
    const float sigma = 1.0f;
    const float varf = sqrtf(2.0f + 2.0f * 4.0f * 4.0f + 6.0f * 6.0f) / 16.0f; // about 0.5
    const float sigma_band = powf(varf, s) * sigma;
    const float inv_sigma2 = 1.0f / (sigma_band * sigma_band);

    dt_opencl_set_kernel_arg(devid, gd->kernel_denoiseprofile_decompose, 0, sizeof(cl_mem), (void *)&dev_buf1);
    dt_opencl_set_kernel_arg(devid, gd->kernel_denoiseprofile_decompose, 1, sizeof(cl_mem), (void *)&dev_buf2);
    dt_opencl_set_kernel_arg(devid, gd->kernel_denoiseprofile_decompose, 2, sizeof(cl_mem),
                             (void *)&dev_detail[s]);
    dt_opencl_set_kernel_arg(devid, gd->kernel_denoiseprofile_decompose, 3, sizeof(int), (void *)&width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_denoiseprofile_decompose, 4, sizeof(int), (void *)&height);
    dt_opencl_set_kernel_arg(devid, gd->kernel_denoiseprofile_decompose, 5, sizeof(unsigned int),
                             (void *)&s);
    dt_opencl_set_kernel_arg(devid, gd->kernel_denoiseprofile_decompose, 6, sizeof(float),
                             (void *)&inv_sigma2);
    dt_opencl_set_kernel_arg(devid, gd->kernel_denoiseprofile_decompose, 7, sizeof(cl_mem),
                             (void *)&dev_filter);
    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_denoiseprofile_decompose, sizes);
    if(err != CL_SUCCESS) goto error;

    // indirectly give gpu some air to breathe (and to do display related stuff)
    dt_iop_nap(darktable.opencl->micro_nap);

    // swap buffers
    cl_mem dev_buf3 = dev_buf2;
    dev_buf2 = dev_buf1;
    dev_buf1 = dev_buf3;
  }

  /* now synthesize again */
  for(int s = max_scale - 1; s >= 0; s--)
  {
    // variance stabilizing transform maps sigma to unity.
    const float sigma = 1.0f;
    // it is then transformed by wavelet scales via the 5 tap a-trous filter:
    const float varf = sqrtf(2.0f + 2.0f * 4.0f * 4.0f + 6.0f * 6.0f) / 16.0f; // about 0.5
    const float sigma_band = powf(varf, scale) * sigma;

    // determine thrs as bayesshrink
    float sum_y2[3] = { 0.0f };

    size_t lsizes[3];
    size_t llocal[3];

    lsizes[0] = bwidth;
    lsizes[1] = bheight;
    lsizes[2] = 1;
    llocal[0] = blocksize;
    llocal[1] = blocksize;
    llocal[2] = 1;
    dt_opencl_set_kernel_arg(devid, gd->kernel_denoiseprofile_reduce_first, 0, sizeof(cl_mem),
                             &(dev_detail[s]));
    dt_opencl_set_kernel_arg(devid, gd->kernel_denoiseprofile_reduce_first, 1, sizeof(int), &width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_denoiseprofile_reduce_first, 2, sizeof(int), &height);
    dt_opencl_set_kernel_arg(devid, gd->kernel_denoiseprofile_reduce_first, 3, sizeof(cl_mem), &dev_m);
    dt_opencl_set_kernel_arg(devid, gd->kernel_denoiseprofile_reduce_first, 4,
                             blocksize * blocksize * 4 * sizeof(float), NULL);
    err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_denoiseprofile_reduce_first, lsizes,
                                                 llocal);
    if(err != CL_SUCCESS) goto error;


    lsizes[0] = reducesize * lsize;
    lsizes[1] = 1;
    lsizes[2] = 1;
    llocal[0] = lsize;
    llocal[1] = 1;
    llocal[2] = 1;
    dt_opencl_set_kernel_arg(devid, gd->kernel_denoiseprofile_reduce_second, 0, sizeof(cl_mem), &dev_m);
    dt_opencl_set_kernel_arg(devid, gd->kernel_denoiseprofile_reduce_second, 1, sizeof(cl_mem), &dev_r);
    dt_opencl_set_kernel_arg(devid, gd->kernel_denoiseprofile_reduce_second, 2, sizeof(int), &bufsize);
    dt_opencl_set_kernel_arg(devid, gd->kernel_denoiseprofile_reduce_second, 3, lsize * 4 * sizeof(float),
                             NULL);
    err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_denoiseprofile_reduce_second, lsizes,
                                                 llocal);
    if(err != CL_SUCCESS) goto error;


    float sumsum[4 * reducesize];
    err = dt_opencl_read_buffer_from_device(devid, (void *)sumsum, dev_r, 0,
                                            (size_t)reducesize * 4 * sizeof(float), CL_TRUE);
    if(err != CL_SUCCESS) goto error;

    for(int k = 0; k < reducesize; k++)
    {
      for(int c = 0; c < 3; c++)
      {
        sum_y2[c] += sumsum[4 * k + c];
      }
    }

    const float sb2 = sigma_band * sigma_band;
    const float var_y[3] = { sum_y2[0] / (npixels - 1.0f), sum_y2[1] / (npixels - 1.0f), sum_y2[2] / (npixels - 1.0f) };
    const float std_x[3] = { sqrtf(MAX(1e-6f, var_y[0] - sb2)), sqrtf(MAX(1e-6f, var_y[1] - sb2)),
                             sqrtf(MAX(1e-6f, var_y[2] - sb2)) };
    // add 8.0 here because it seemed a little weak
    const float adjt = 8.0f;

    const float thrs[4] = { adjt * sb2 / std_x[0], adjt * sb2 / std_x[1], adjt * sb2 / std_x[2], 0.0f };
    // fprintf(stderr, "scale %d thrs %f %f %f\n", s, thrs[0], thrs[1], thrs[2]);

    const float boost[4] = { 1.0f, 1.0f, 1.0f, 1.0f };

    dt_opencl_set_kernel_arg(devid, gd->kernel_denoiseprofile_synthesize, 0, sizeof(cl_mem),
                             (void *)&dev_buf1);
    dt_opencl_set_kernel_arg(devid, gd->kernel_denoiseprofile_synthesize, 1, sizeof(cl_mem),
                             (void *)&dev_detail[s]);
    dt_opencl_set_kernel_arg(devid, gd->kernel_denoiseprofile_synthesize, 2, sizeof(cl_mem),
                             (void *)&dev_buf2);
    dt_opencl_set_kernel_arg(devid, gd->kernel_denoiseprofile_synthesize, 3, sizeof(int), (void *)&width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_denoiseprofile_synthesize, 4, sizeof(int), (void *)&height);
    dt_opencl_set_kernel_arg(devid, gd->kernel_denoiseprofile_synthesize, 5, sizeof(float), (void *)&thrs[0]);
    dt_opencl_set_kernel_arg(devid, gd->kernel_denoiseprofile_synthesize, 6, sizeof(float), (void *)&thrs[1]);
    dt_opencl_set_kernel_arg(devid, gd->kernel_denoiseprofile_synthesize, 7, sizeof(float), (void *)&thrs[2]);
    dt_opencl_set_kernel_arg(devid, gd->kernel_denoiseprofile_synthesize, 8, sizeof(float), (void *)&thrs[3]);
    dt_opencl_set_kernel_arg(devid, gd->kernel_denoiseprofile_synthesize, 9, sizeof(float), (void *)&boost[0]);
    dt_opencl_set_kernel_arg(devid, gd->kernel_denoiseprofile_synthesize, 10, sizeof(float),
                             (void *)&boost[1]);
    dt_opencl_set_kernel_arg(devid, gd->kernel_denoiseprofile_synthesize, 11, sizeof(float),
                             (void *)&boost[2]);
    dt_opencl_set_kernel_arg(devid, gd->kernel_denoiseprofile_synthesize, 12, sizeof(float),
                             (void *)&boost[3]);
    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_denoiseprofile_synthesize, sizes);
    if(err != CL_SUCCESS) goto error;

    // indirectly give gpu some air to breathe (and to do display related stuff)
    dt_iop_nap(darktable.opencl->micro_nap);

    // swap buffers
    cl_mem dev_buf3 = dev_buf2;
    dev_buf2 = dev_buf1;
    dev_buf1 = dev_buf3;
  }

  // copy output of last run of synthesize kernel to dev_tmp (if not already there)
  // note: we need to take swap of buffers into account, so current output lies in dev_buf1
  if(dev_buf1 != dev_tmp)
  {
    size_t origin[] = { 0, 0, 0 };
    size_t region[] = { width, height, 1 };
    err = dt_opencl_enqueue_copy_image(devid, dev_buf1, dev_tmp, origin, origin, region);
    if(err != CL_SUCCESS) goto error;
  }

  dt_opencl_set_kernel_arg(devid, gd->kernel_denoiseprofile_backtransform, 0, sizeof(cl_mem),
                           (void *)&dev_tmp);
  dt_opencl_set_kernel_arg(devid, gd->kernel_denoiseprofile_backtransform, 1, sizeof(cl_mem),
                           (void *)&dev_out);
  dt_opencl_set_kernel_arg(devid, gd->kernel_denoiseprofile_backtransform, 2, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_denoiseprofile_backtransform, 3, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_denoiseprofile_backtransform, 4, 4 * sizeof(float), (void *)&aa);
  dt_opencl_set_kernel_arg(devid, gd->kernel_denoiseprofile_backtransform, 5, 4 * sizeof(float),
                           (void *)&sigma2);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_denoiseprofile_backtransform, sizes);
  if(err != CL_SUCCESS) goto error;

  if(!darktable.opencl->async_pixelpipe || piece->pipe->type == DT_DEV_PIXELPIPE_EXPORT)
    dt_opencl_finish(devid);


  if(dev_r != NULL) dt_opencl_release_mem_object(dev_r);
  if(dev_m != NULL) dt_opencl_release_mem_object(dev_m);
  if(dev_tmp != NULL) dt_opencl_release_mem_object(dev_tmp);
  if(dev_filter != NULL) dt_opencl_release_mem_object(dev_filter);
  for(int k = 0; k < max_scale; k++)
    if(dev_detail[k] != NULL) dt_opencl_release_mem_object(dev_detail[k]);
  return TRUE;

error:
  if(dev_r != NULL) dt_opencl_release_mem_object(dev_r);
  if(dev_m != NULL) dt_opencl_release_mem_object(dev_m);
  if(dev_tmp != NULL) dt_opencl_release_mem_object(dev_tmp);
  if(dev_filter != NULL) dt_opencl_release_mem_object(dev_filter);
  for(int k = 0; k < max_scale; k++)
    if(dev_detail[k] != NULL) dt_opencl_release_mem_object(dev_detail[k]);
  dt_print(DT_DEBUG_OPENCL, "[opencl_denoiseprofile] couldn't enqueue kernel! %d, devid %d\n", err, devid);
  return FALSE;
}


int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_denoiseprofile_params_t *d = (dt_iop_denoiseprofile_params_t *)piece->data;

  if(d->mode == MODE_NLMEANS)
  {
    return process_nlmeans_cl(self, piece, dev_in, dev_out, roi_in, roi_out);
  }
  else
  {
    return process_wavelets_cl(self, piece, dev_in, dev_out, roi_in, roi_out);
  }
}
#endif // HAVE_OPENCL

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_denoiseprofile_params_t *d = (dt_iop_denoiseprofile_params_t *)piece->data;
  if(d->mode == MODE_NLMEANS)
    process_nlmeans(self, piece, ivoid, ovoid, roi_in, roi_out);
  else
    process_wavelets(self, piece, ivoid, ovoid, roi_in, roi_out, eaw_decompose, eaw_synthesize);
}

#if defined(__SSE2__)
void process_sse2(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
                  void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_denoiseprofile_params_t *d = (dt_iop_denoiseprofile_params_t *)piece->data;
  if(d->mode == MODE_NLMEANS)
    process_nlmeans_sse(self, piece, ivoid, ovoid, roi_in, roi_out);
  else
    process_wavelets(self, piece, ivoid, ovoid, roi_in, roi_out, eaw_decompose_sse, eaw_synthesize_sse2);
}
#endif

/** this will be called to init new defaults if a new image is loaded from film strip mode. */
void reload_defaults(dt_iop_module_t *module)
{
  // our module is disabled by default
  module->default_enabled = 0;
  dt_iop_denoiseprofile_gui_data_t *g = (dt_iop_denoiseprofile_gui_data_t *)module->gui_data;
  if(g)
  {
    dt_bauhaus_combobox_clear(g->profile);

    // get matching profiles:
    char name[512];
    if(g->profiles) g_list_free_full(g->profiles, dt_noiseprofile_free);
    g->profiles = dt_noiseprofile_get_matching(&module->dev->image_storage);
    g->interpolated = dt_noiseprofile_generic; // default to generic poissonian
    g_strlcpy(name, _(g->interpolated.name), sizeof(name));

    const int iso = module->dev->image_storage.exif_iso;
    dt_noiseprofile_t *last = NULL;
    for(GList *iter = g->profiles; iter; iter = g_list_next(iter))
    {
      dt_noiseprofile_t *current = (dt_noiseprofile_t *)iter->data;

      if(current->iso == iso)
      {
        g->interpolated = *current;
        // signal later autodetection in commit_params:
        g->interpolated.a[0] = -1.0;
        snprintf(name, sizeof(name), _("found match for ISO %d"), iso);
        break;
      }
      if(last && last->iso < iso && current->iso > iso)
      {
        dt_noiseprofile_interpolate(last, current, &g->interpolated);
        // signal later autodetection in commit_params:
        g->interpolated.a[0] = -1.0;
        snprintf(name, sizeof(name), _("interpolated from ISO %d and %d"), last->iso, current->iso);
        break;
      }
      last = current;
    }

    dt_bauhaus_combobox_add(g->profile, name);
    for(GList *iter = g->profiles; iter; iter = g_list_next(iter))
    {
      dt_noiseprofile_t *profile = (dt_noiseprofile_t *)iter->data;
      dt_bauhaus_combobox_add(g->profile, profile->name);
    }

    ((dt_iop_denoiseprofile_params_t *)module->default_params)->radius = 1.0f;
    ((dt_iop_denoiseprofile_params_t *)module->default_params)->strength = 1.0f;
    ((dt_iop_denoiseprofile_params_t *)module->default_params)->mode = MODE_NLMEANS;
    for(int k = 0; k < 3; k++)
    {
      ((dt_iop_denoiseprofile_params_t *)module->default_params)->a[k] = g->interpolated.a[k];
      ((dt_iop_denoiseprofile_params_t *)module->default_params)->b[k] = g->interpolated.b[k];
    }
    memcpy(module->params, module->default_params, sizeof(dt_iop_denoiseprofile_params_t));
  }
}

/** init, cleanup, commit to pipeline */
void init(dt_iop_module_t *module)
{
  module->params = calloc(1, sizeof(dt_iop_denoiseprofile_params_t));
  module->default_params = calloc(1, sizeof(dt_iop_denoiseprofile_params_t));
  module->priority = 138; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_denoiseprofile_params_t);
  module->gui_data = NULL;
  module->data = NULL;
}

void cleanup(dt_iop_module_t *module)
{
  free(module->params);
  module->params = NULL;
}

void init_global(dt_iop_module_so_t *module)
{
  const int program = 11; // denoiseprofile.cl, from programs.conf
  dt_iop_denoiseprofile_global_data_t *gd
      = (dt_iop_denoiseprofile_global_data_t *)malloc(sizeof(dt_iop_denoiseprofile_global_data_t));
  module->data = gd;
  gd->kernel_denoiseprofile_precondition = dt_opencl_create_kernel(program, "denoiseprofile_precondition");
  gd->kernel_denoiseprofile_init = dt_opencl_create_kernel(program, "denoiseprofile_init");
  gd->kernel_denoiseprofile_dist = dt_opencl_create_kernel(program, "denoiseprofile_dist");
  gd->kernel_denoiseprofile_horiz = dt_opencl_create_kernel(program, "denoiseprofile_horiz");
  gd->kernel_denoiseprofile_vert = dt_opencl_create_kernel(program, "denoiseprofile_vert");
  gd->kernel_denoiseprofile_accu = dt_opencl_create_kernel(program, "denoiseprofile_accu");
  gd->kernel_denoiseprofile_finish = dt_opencl_create_kernel(program, "denoiseprofile_finish");
  gd->kernel_denoiseprofile_backtransform = dt_opencl_create_kernel(program, "denoiseprofile_backtransform");
  gd->kernel_denoiseprofile_decompose = dt_opencl_create_kernel(program, "denoiseprofile_decompose");
  gd->kernel_denoiseprofile_synthesize = dt_opencl_create_kernel(program, "denoiseprofile_synthesize");
  gd->kernel_denoiseprofile_reduce_first = dt_opencl_create_kernel(program, "denoiseprofile_reduce_first");
  gd->kernel_denoiseprofile_reduce_second = dt_opencl_create_kernel(program, "denoiseprofile_reduce_second");
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_denoiseprofile_global_data_t *gd = (dt_iop_denoiseprofile_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_denoiseprofile_precondition);
  dt_opencl_free_kernel(gd->kernel_denoiseprofile_init);
  dt_opencl_free_kernel(gd->kernel_denoiseprofile_dist);
  dt_opencl_free_kernel(gd->kernel_denoiseprofile_horiz);
  dt_opencl_free_kernel(gd->kernel_denoiseprofile_vert);
  dt_opencl_free_kernel(gd->kernel_denoiseprofile_accu);
  dt_opencl_free_kernel(gd->kernel_denoiseprofile_finish);
  dt_opencl_free_kernel(gd->kernel_denoiseprofile_backtransform);
  dt_opencl_free_kernel(gd->kernel_denoiseprofile_decompose);
  dt_opencl_free_kernel(gd->kernel_denoiseprofile_synthesize);
  dt_opencl_free_kernel(gd->kernel_denoiseprofile_reduce_first);
  dt_opencl_free_kernel(gd->kernel_denoiseprofile_reduce_second);
  free(module->data);
  module->data = NULL;
}

static dt_noiseprofile_t dt_iop_denoiseprofile_get_auto_profile(dt_iop_module_t *self)
{
  GList *profiles = dt_noiseprofile_get_matching(&self->dev->image_storage);
  dt_noiseprofile_t interpolated = dt_noiseprofile_generic; // default to generic poissonian

  const int iso = self->dev->image_storage.exif_iso;
  dt_noiseprofile_t *last = NULL;
  for(GList *iter = profiles; iter; iter = g_list_next(iter))
  {
    dt_noiseprofile_t *current = (dt_noiseprofile_t *)iter->data;
    if(current->iso == iso)
    {
      interpolated = *current;
      break;
    }
    if(last && last->iso < iso && current->iso > iso)
    {
      dt_noiseprofile_interpolate(last, current, &interpolated);
      break;
    }
    last = current;
  }
  g_list_free_full(profiles, dt_noiseprofile_free);
  return interpolated;
}

/** commit is the synch point between core and gui, so it copies params to pipe data. */
void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *params, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_denoiseprofile_params_t *p = (dt_iop_denoiseprofile_params_t *)params;
  dt_iop_denoiseprofile_data_t *d = (dt_iop_denoiseprofile_data_t *)piece->data;

  // copy everything first and make some changes later
  memcpy(d, p, sizeof(*d));

  // compare if a[0] in params is set to "magic value" -1.0 for autodetection
  if(p->a[0] == -1.0)
  {
    // autodetect matching profile again, the same way as detecting their names,
    // this is partially duplicated code and data because we are not allowed to access
    // gui_data here ..
    dt_noiseprofile_t interpolated = dt_iop_denoiseprofile_get_auto_profile(self);
    for(int k = 0; k < 3; k++)
    {
      d->a[k] = interpolated.a[k];
      d->b[k] = interpolated.b[k];
    }
  }
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_denoiseprofile_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

static void profile_callback(GtkWidget *w, dt_iop_module_t *self)
{
  int i = dt_bauhaus_combobox_get(w);
  dt_iop_denoiseprofile_params_t *p = (dt_iop_denoiseprofile_params_t *)self->params;
  dt_iop_denoiseprofile_gui_data_t *g = (dt_iop_denoiseprofile_gui_data_t *)self->gui_data;
  const dt_noiseprofile_t *profile = &(g->interpolated);
  if(i > 0) profile = (dt_noiseprofile_t *)g_list_nth_data(g->profiles, i - 1);
  for(int k = 0; k < 3; k++)
  {
    p->a[k] = profile->a[k];
    p->b[k] = profile->b[k];
  }
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void mode_callback(GtkWidget *w, dt_iop_module_t *self)
{
  dt_iop_denoiseprofile_params_t *p = (dt_iop_denoiseprofile_params_t *)self->params;
  dt_iop_denoiseprofile_gui_data_t *g = (dt_iop_denoiseprofile_gui_data_t *)self->gui_data;
  p->mode = dt_bauhaus_combobox_get(w);
  if(p->mode == MODE_WAVELETS)
    gtk_widget_set_visible(g->radius, FALSE);
  else
    gtk_widget_set_visible(g->radius, TRUE);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void radius_callback(GtkWidget *w, dt_iop_module_t *self)
{
  dt_iop_denoiseprofile_params_t *p = (dt_iop_denoiseprofile_params_t *)self->params;
  p->radius = (int)dt_bauhaus_slider_get(w);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void strength_callback(GtkWidget *w, dt_iop_module_t *self)
{
  dt_iop_denoiseprofile_params_t *p = (dt_iop_denoiseprofile_params_t *)self->params;
  p->strength = dt_bauhaus_slider_get(w);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

void gui_update(dt_iop_module_t *self)
{
  // let gui slider match current parameters:
  dt_iop_denoiseprofile_gui_data_t *g = (dt_iop_denoiseprofile_gui_data_t *)self->gui_data;
  dt_iop_denoiseprofile_params_t *p = (dt_iop_denoiseprofile_params_t *)self->params;
  dt_bauhaus_slider_set(g->radius, p->radius);
  dt_bauhaus_slider_set(g->strength, p->strength);
  dt_bauhaus_combobox_set(g->mode, p->mode);
  dt_bauhaus_combobox_set(g->profile, -1);
  if(p->mode == MODE_WAVELETS)
    gtk_widget_set_visible(g->radius, FALSE);
  else
    gtk_widget_set_visible(g->radius, TRUE);
  if(p->a[0] == -1.0)
  {
    dt_bauhaus_combobox_set(g->profile, 0);
  }
  else
  {
    int i = 1;
    for(GList *iter = g->profiles; iter; iter = g_list_next(iter), i++)
    {
      dt_noiseprofile_t *profile = (dt_noiseprofile_t *)iter->data;
      if(!memcmp(profile->a, p->a, sizeof(float) * 3)
         && !memcmp(profile->b, p->b, sizeof(float) * 3))
      {
        dt_bauhaus_combobox_set(g->profile, i);
        break;
      }
    }
  }
}

void gui_init(dt_iop_module_t *self)
{
  // init the slider (more sophisticated layouts are possible with gtk tables and boxes):
  self->gui_data = malloc(sizeof(dt_iop_denoiseprofile_gui_data_t));
  dt_iop_denoiseprofile_gui_data_t *g = (dt_iop_denoiseprofile_gui_data_t *)self->gui_data;
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);
  g->profiles = NULL;
  g->profile = dt_bauhaus_combobox_new(self);
  g->mode = dt_bauhaus_combobox_new(self);
  g->radius = dt_bauhaus_slider_new_with_range(self, 0.0f, 4.0f, 1., 1.f, 0);
  g->strength = dt_bauhaus_slider_new_with_range(self, 0.001f, 4.0f, .05, 1.f, 3);
  gtk_box_pack_start(GTK_BOX(self->widget), g->profile, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->mode, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->radius, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->strength, TRUE, TRUE, 0);
  dt_bauhaus_widget_set_label(g->profile, NULL, _("profile"));
  dt_bauhaus_widget_set_label(g->mode, NULL, _("mode"));
  dt_bauhaus_widget_set_label(g->radius, NULL, _("patch size"));
  dt_bauhaus_slider_set_format(g->radius, "%.0f");
  dt_bauhaus_widget_set_label(g->strength, NULL, _("strength"));
  dt_bauhaus_combobox_add(g->mode, _("non-local means"));
  dt_bauhaus_combobox_add(g->mode, _("wavelets"));
  gtk_widget_set_tooltip_text(g->profile, _("profile used for variance stabilization"));
  gtk_widget_set_tooltip_text(g->mode, _("method used in the denoising core. "
                                         "non-local means works best for `lightness' blending, "
                                         "wavelets work best for `color' blending"));
  gtk_widget_set_tooltip_text(g->radius, _("radius of the patches to match. increase for more sharpness"));
  gtk_widget_set_tooltip_text(g->strength, _("finetune denoising strength"));
  g_signal_connect(G_OBJECT(g->profile), "value-changed", G_CALLBACK(profile_callback), self);
  g_signal_connect(G_OBJECT(g->mode), "value-changed", G_CALLBACK(mode_callback), self);
  g_signal_connect(G_OBJECT(g->radius), "value-changed", G_CALLBACK(radius_callback), self);
  g_signal_connect(G_OBJECT(g->strength), "value-changed", G_CALLBACK(strength_callback), self);
}

void gui_cleanup(dt_iop_module_t *self)
{
  dt_iop_denoiseprofile_gui_data_t *g = (dt_iop_denoiseprofile_gui_data_t *)self->gui_data;
  g_list_free_full(g->profiles, dt_noiseprofile_free);
  // nothing else necessary, gtk will clean up the slider.
  free(self->gui_data);
  self->gui_data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
