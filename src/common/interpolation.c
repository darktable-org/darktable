/* --------------------------------------------------------------------------
    This file is part of darktable,
    Copyright (C) 2012-2023 darktable developers.

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
* ------------------------------------------------------------------------*/

#include "common/interpolation.h"
#include "common/darktable.h"
#include "common/math.h"
#include "control/conf.h"

#include <assert.h>
#include <glib.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>

/** Border extrapolation modes */
enum border_mode
{
  BORDER_REPLICATE, // aaaa|abcdefg|gggg
  BORDER_WRAP,      // defg|abcdefg|abcd
  BORDER_MIRROR,    // edcb|abcdefg|fedc
  BORDER_CLAMP      // ....|abcdefg|....
};

/* Supporting them all might be overkill, let the compiler trim all
 * unnecessary modes in clip for resampling codepath*/
#define RESAMPLING_BORDER_MODE BORDER_REPLICATE

/* Supporting them all might be overkill, let the compiler trim all
 * unnecessary modes in interpolation codepath */
#define INTERPOLATION_BORDER_MODE BORDER_MIRROR

// Defines minimum alignment requirement for critical SIMD code
#define SSE_ALIGNMENT 64

// Defines the maximum kernel half length
// !! Make sure to sync this with the filter array !!
#define MAX_HALF_FILTER_WIDTH 3

// Add *verbose* (like one msg per pixel out) debug message to stderr
#define DEBUG_PRINT_VERBOSE 0

/* --------------------------------------------------------------------------
 * Debug helpers
 * ------------------------------------------------------------------------*/

static void _show_2_times(const dt_times_t *start,
                          const dt_times_t *mid,
                          const char *prefix)
{
  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_times_t end;
    dt_get_times(&end);
    dt_print(DT_DEBUG_PERF,
             "[%s] plan %.3f secs (%.3f CPU) resample %.3f secs (%.3f CPU)\n",
             prefix, mid->clock - start->clock, mid->user - start->user,
             end.clock - mid->clock, end.user - mid->user);
  }
}

#if DEBUG_PRINT_VERBOSE
#define debug_extra(...)               \
  do                                   \
  {                                    \
    fprintf(stderr, __VA_ARGS__);      \
  } while(0)
#else
#define debug_extra(...)
#endif

/* --------------------------------------------------------------------------
 * Generic helpers
 * ------------------------------------------------------------------------*/

/** Clip into specified range
 * @param idx index to filter
 * @param length length of line
 */
static inline int clip(int i,
                       const int min,
                       const int max,
                       enum border_mode mode)
{
  switch(mode)
  {
    case BORDER_REPLICATE:
      if(i < min)
      {
        i = min;
      }
      else if(i > max)
      {
        i = max;
      }
      break;
    case BORDER_MIRROR:
      if(i < min)
      {
        // i == min - 1  -->  min + 1
        // i == min - 2  -->  min + 2, etc.
        // but as min == 0 in all current cases, this really optimizes to i = -i
        i = min + (min - i);
      }
      else if(i > max)
      {
        // i == max + 1  -->  max - 1
        // i == max + 2  -->  max - 2, etc.
        i = max - (i - max);
      }
      break;
    case BORDER_WRAP:
      if(i < min)
      {
        i = 1 + max - (min - i);
      }
      else if(i > max)
      {
        i = min + (i - max) - 1;
      }
      break;
    case BORDER_CLAMP:
      if(i < min || i > max)
      {
        /* Should not be used as is, we prevent -1 usage, filtering the taps
         * we clip the sample indexes for. So understand this function is
         * specific to its caller. */
        i = -1;
      }
      break;
  }

  return i;
}

static inline void prepare_tap_boundaries(int *tap_first,
                                          int *tap_last,
                                          const enum border_mode mode,
                                          const int filterwidth,
                                          const int t,
                                          const int max)
{
  /* Check lower bound pixel index and skip as many pixels as necessary to
   * fall into range */
  *tap_first = 0;
  if(mode == BORDER_CLAMP && t < 0)
  {
    *tap_first = -t;
  }

  // Same for upper bound pixel
  *tap_last = filterwidth;
  if(mode == BORDER_CLAMP && t + filterwidth >= max)
  {
    *tap_last = max - t;
  }
}

/** Make sure an aligned chunk will not misalign its following chunk
 * proposing an adapted length
 *
 * @param l Length required for current chunk
 * @param align Required alignment for next chunk
 *
 * @return Required length for keeping alignment ok if chaining data chunks
 */
static inline size_t increase_for_alignment(const size_t l,
                                            size_t align)
{
  align -= 1;
  return (l + align) & (~align);
}

/* --------------------------------------------------------------------------
 * Interpolation kernels
 * ------------------------------------------------------------------------*/

/* --------------------------------------------------------------------------
 * Bilinear interpolation
 * ------------------------------------------------------------------------*/

static float maketaps_bilinear(float *taps,
                               size_t num_taps,
                               float  width,
                               float first_tap,
                               float interval)
{
  static const dt_aligned_pixel_t bootstrap = { 0.0f, 1.0f, 2.0f, 3.0f };
  dt_aligned_pixel_t iter;
  dt_aligned_pixel_t vt;
  for_four_channels(c)
    iter[c] = 4.0f * interval;
  for_four_channels(c)
    vt[c] = first_tap + bootstrap[c] * interval;

  const int runs = (num_taps + 3) / 4;

  for(size_t i = 0; i < runs; i++)
  {
    // compute and store the values for the current four taps
    for_four_channels(c)
      taps[4*i + c] = 1.0f - (vt[c] < 0.0f ? -vt[c] : vt[c]);
    // prepare next iteration
    for_four_channels(c)
      vt[c] += iter[c];
  }
  return 1.0f; //kernel norm is 1.0f by construction
}
  
/* --------------------------------------------------------------------------
 * Bicubic interpolation
 * ------------------------------------------------------------------------*/

static float maketaps_bicubic(float *taps,
                              size_t num_taps,
                              float  width,
                              float first_tap,
                              float interval)
{
  static const dt_aligned_pixel_t bootstrap = { 0.0f, 1.0f, 2.0f, 3.0f };
  static const dt_aligned_pixel_t half = { .5f, .5f, .5f, .5f };
  static const dt_aligned_pixel_t two = { 2.f, 2.f, 2.f, 2.f };
  static const dt_aligned_pixel_t three = { 3.f, 3.f, 3.f, 3.f };
  static const dt_aligned_pixel_t four = { 4.f, 4.f, 4.f, 4.f };
  static const dt_aligned_pixel_t five = { 5.f, 5.f, 5.f, 5.f };
  static const dt_aligned_pixel_t eight = { 8.f, 8.f, 8.f, 8.f };
  dt_aligned_pixel_t iter;
  dt_aligned_pixel_t vt;
  for_four_channels(c)
    iter[c] = 4.0f * interval;
  for_four_channels(c)
    vt[c] = first_tap + bootstrap[c] * interval;

  const int runs = (num_taps + 3) / 4;

  for(size_t i = 0; i < runs; i++)
  {
    // compute and store the values for the current four taps
    dt_aligned_pixel_t vt_abs;
    dt_aligned_pixel_t t2;   // tap-squared
    for_four_channels(c)
    {
      vt_abs[c] = vt[c] < 0.0f ? -vt[c] : vt[c];
      t2[c] = vt[c] * vt[c];
    }
    dt_aligned_pixel_t t5;
    dt_aligned_pixel_t mt2_add_t5_sub_8;
    for_four_channels(c)
    {
      t5[c] = five[c] * vt_abs[c];
      mt2_add_t5_sub_8[c] = t5[c] - eight[c] - t2[c];
    }
    dt_aligned_pixel_t b;
    dt_aligned_pixel_t r12;
    for_four_channels(c)
    {
      b[c] = vt_abs[c] * mt2_add_t5_sub_8[c] + four[c];
      r12[c] = b[c] * half[c]; // the value for 1 < t < 2
    }
    dt_aligned_pixel_t t23;
    dt_aligned_pixel_t e;
    dt_aligned_pixel_t r01;
    for_four_channels(c)
    {
      t23[c] = three[c] * t2[c] - t5[c];
      e[c] = t23[c] * vt_abs[c] + two[c];
      r01[c] = e[c] * half[c];
    }
    // combine the values depending on whether abs(tap) is less than one or not
    for_four_channels(c)
    {
      taps[4*i + c] = vt_abs[c] <= 1.0f ? r01[c] : r12[c];
    }
    // prepare next iteration
    for_four_channels(c)
      vt[c] += iter[c];
  }
  return 1.0f; //kernel norm is 1.0f by construction
}
  
/* --------------------------------------------------------------------------
 * Lanczos interpolation
 * ------------------------------------------------------------------------*/

#define DT_LANCZOS_EPSILON (1e-9f)

#if 0
// Reference version left here for ... documentation
static inline float
lanczos(float width, float t)
{
  float r;

  if(t<-width || t>width)
  {
    r = 0.f;
  }
  else if(t>-DT_LANCZOS_EPSILON && t<DT_LANCZOS_EPSILON)
  {
    r = 1.f;
  }
  else
  {
    r = width*sinf(M_PI*t)*sinf(M_PI*t/width)/(M_PI*M_PI*t*t);
  }
  return r;
}
#endif

/* Fast lanczos version, no calls to math.h functions, too accurate, too slow
 *
 * Based on a forum entry at
 * http://devmaster.net/forums/topic/4648-fast-and-accurate-sinecosine/
 *
 * Apart the fast sine function approximation, the only trick is to compute:
 * sin(pi.t) = sin(a.pi + r.pi) where t = a + r = trunc(t) + r
 *           = sin(a.pi).cos(r.pi) + sin(r.pi).cos(a.pi)
 *           =         0*cos(r.pi) + sin(r.pi).cos(a.pi)
 *           = sign.sin(r.pi) where sign =  1 if the a is even
 *                                       = -1 if the a is odd
 *
 * Of course we know that lanczos func will only be called for
 * the range -width < t < width so we can additionally avoid the
 * range check.  */

static float maketaps_lanczos(float *taps,
                              size_t num_taps,
                              float width,
                              float first_tap,
                              float interval)
{
  static const dt_aligned_pixel_t bootstrap = { 0.0f, 1.0f, 2.0f, 3.0f };
  dt_aligned_pixel_t iter;
  dt_aligned_pixel_t vt;
  for_four_channels(c)
    iter[c] = 4.0f * interval;
  for_four_channels(c)
    vt[c] = first_tap + bootstrap[c] * interval;
  dt_aligned_pixel_t vw;
  for_four_channels(c)
    vw[c] = width;

  const int runs = (num_taps + 3) / 4;

  for(size_t i = 0; i < runs; i++)
  {
    // compute and store the values for the current four taps
    static const dt_aligned_pixel_t eps
      = { DT_LANCZOS_EPSILON, DT_LANCZOS_EPSILON, DT_LANCZOS_EPSILON, DT_LANCZOS_EPSILON };
    static const dt_aligned_pixel_t pi = { M_PI_F, M_PI_F, M_PI_F, M_PI_F };
    static const dt_aligned_pixel_t pi2
      = { M_PI_F*M_PI_F, M_PI_F*M_PI_F, M_PI_F*M_PI_F, M_PI_F*M_PI_F };
    dt_aligned_pixel_t r;
    dt_aligned_pixel_t sign;
    for_four_channels(c)
    {
      int a = (int)vt[c];
      r[c] = vt[c] - (float)a;
      sign[c] = (a & 1) ? -1.0f : 1.0f;
    }
    dt_aligned_pixel_t sine_arg1;
    dt_aligned_pixel_t sine_arg2;
    for_four_channels(c)
    {
      sine_arg1[c] = pi[c] * r[c];
      sine_arg2[c] = pi[c] * vt[c] / vw[c];
    }
    dt_aligned_pixel_t sine1;
    dt_aligned_pixel_t sine2;
    dt_vector_sin(sine_arg1, sine1);
    dt_vector_sin(sine_arg2, sine2);
    dt_aligned_pixel_t num;
    dt_aligned_pixel_t denom;
    for_four_channels(c)
    {
      num[c] = (vw[c] * sign[c] * sine1[c] * sine2[c]) + eps[c];
      denom[c] = (pi2[c] * vt[c] * vt[c]) + eps[c];
    }
    for_four_channels(c)
    {
      taps[4*i + c] = num[c] / denom[c];
    }
    // prepare next iteration
    for_four_channels(c)
      vt[c] += iter[c];
  }
  // we need to compute the norm, even though it is very close to 1.0
  // and causes an increase of maxDE on the integration tests only
  // from 1.1 to 1.7, because not doing so generates visible moire
  // banding in smooth gradients.  Unfortunately, this costs an extra
  // 15-20% runtime....
  float norm = 0.0f;
  for(size_t i = 0; i < num_taps; i++)
    norm += taps[i];
  return norm;
}
  
#undef DT_LANCZOS_EPSILON

/* --------------------------------------------------------------------------
 * All our known interpolators
 * ------------------------------------------------------------------------*/

/* !!! !!! !!!
 * Make sure MAX_HALF_FILTER_WIDTH is at least equal to the maximum width
 * of this filter list. Otherwise bad things will happen
 * !!! !!! !!!
 */
static const struct dt_interpolation dt_interpolator[] = {
  {.id = DT_INTERPOLATION_BILINEAR,
   .name = "bilinear",
   .width = 1,
   .maketaps = &maketaps_bilinear,
  },
  {.id = DT_INTERPOLATION_BICUBIC,
   .name = "bicubic",
   .width = 2,
   .maketaps = &maketaps_bicubic,
  },
  {.id = DT_INTERPOLATION_LANCZOS2,
   .name = "lanczos2",
   .width = 2,
   .maketaps = &maketaps_lanczos,
  },
  {.id = DT_INTERPOLATION_LANCZOS3,
   .name = "lanczos3",
   .width = 3,
   .maketaps = &maketaps_lanczos,
  },
};

/* --------------------------------------------------------------------------
 * Kernel utility methods
 * ------------------------------------------------------------------------*/

static inline float compute_upsampling_kernel(const struct dt_interpolation *itor,
                                              float *kernel,
                                              int *first,
                                              float t)
{
  // find first pixel contributing to the filter's kernel.  We need
  // floorf() because a simple cast to int truncates toward zero,
  // yielding an incorrect result for the slightly-negative positions
  // that can occur at the top and left edges when doing perspective
  // correction
  int f = (int)floorf(t) - itor->width + 1;
  if(first)
  {
    *first = f;
  }

  /* Find closest integer position and then offset that to match first
   * filtered sample position */
  t = t - (float)f;

  // compute the taps and return the kernel norm
  return itor->maketaps(kernel, 2*itor->width, itor->width, t, -1.0f);
}


/** Computes a downsampling filtering kernel (vectorized version, four taps
 * per inner loop iteration)
 *
 * @param itor [in] Interpolator used
 * @param kernelsize [out] Number of taps
 * @param kernel [out] resulting taps (at least itor->width/inoout + 4 elements for no overflow)
 * @param norm [out] Kernel norm
 * @param first [out] index of the first sample for which the kernel is to be applied
 * @param outoinratio [in] "out samples" over "in samples" ratio
 * @param xout [in] Output coordinate */
static inline void compute_downsampling_kernel(const struct dt_interpolation *itor,
                                               int *taps,
                                               int *first,
                                               float *kernel,
                                               float *norm,
                                               const float outoinratio,
                                               const int xout)
{
  // Keep this at hand
  const float w = (float)itor->width;

  /* Compute the phase difference between output pixel and its
   * input corresponding input pixel */
  const float xin = ceil_fast(((float)xout - w) / outoinratio);
  if(first)
  {
    *first = (int)xin;
  }

  // Compute first interpolator parameter
  float t = xin * outoinratio - (float)xout;

  // Compute all filter taps
  int num_taps = *taps = (int)((w - t) / outoinratio);
  itor->maketaps(kernel, num_taps, itor->width, t, outoinratio);
  // compute the kernel norm if requested
  if (norm)
  {
    float n  = 0.0f;
    for(size_t i = 0; i < num_taps; i++)
      n += kernel[i];
    *norm = n;
  }
}

/* --------------------------------------------------------------------------
 * Sample interpolation function (see usage in iop/lens.c and iop/clipping.c)
 * ------------------------------------------------------------------------*/

#define MAX_KERNEL_REQ ((2 * (MAX_HALF_FILTER_WIDTH) + 3) & (~3))

float dt_interpolation_compute_sample(const struct dt_interpolation *itor,
                                      const float *in,
                                      const float x,
                                      const float y,
                                      const int width,
                                      const int height,
                                      const int samplestride,
                                      const int linestride)
{
  assert(itor->width < (MAX_HALF_FILTER_WIDTH + 1));

  float kernelh[MAX_KERNEL_REQ] __attribute__((aligned(SSE_ALIGNMENT)));
  float kernelv[MAX_KERNEL_REQ] __attribute__((aligned(SSE_ALIGNMENT)));

  // Compute both horizontal and vertical kernels
  float normh = compute_upsampling_kernel(itor, kernelh, NULL, x);
  float normv = compute_upsampling_kernel(itor, kernelv, NULL, y);

  int ix = (int)x;
  int iy = (int)y;

  /* Now 2 cases, the pixel + filter width goes outside the image
   * in that case we have to use index clipping to keep all reads
   * in the input image (slow path) or we are sure it won't fall
   * outside and can do more simple code */
  float r;
  if(ix >= (itor->width - 1) && iy >= (itor->width - 1) && ix < (width - itor->width)
     && iy < (height - itor->width))
  {
    // Inside image boundary case

    // Go to top left pixel
    in = (float *)in + linestride * iy + ix * samplestride;
    in = in - (itor->width - 1) * (samplestride + linestride);

    // Apply the kernel
    float s = 0.f;
    for(int i = 0; i < 2 * itor->width; i++)
    {
      float h = 0.0f;
      for(int j = 0; j < 2 * itor->width; j++)
      {
        h += kernelh[j] * in[j * samplestride];
      }
      s += kernelv[i] * h;
      in += linestride;
    }
    r = s / (normh * normv);
  }
  else if(ix >= 0 && iy >= 0 && ix < width && iy < height)
  {
    // At least a valid coordinate

    // Point to the upper left pixel index wise
    iy -= itor->width - 1;
    ix -= itor->width - 1;

    static const enum border_mode bordermode = INTERPOLATION_BORDER_MODE;
    assert(bordermode != BORDER_CLAMP); // XXX in clamp mode, norms would be wrong

    int xtap_first;
    int xtap_last;
    prepare_tap_boundaries(&xtap_first, &xtap_last,
                           bordermode, 2 * itor->width, ix, width);

    int ytap_first;
    int ytap_last;
    prepare_tap_boundaries(&ytap_first, &ytap_last,
                           bordermode, 2 * itor->width, iy, height);

    // Apply the kernel
    float s = 0.f;
    for(int i = ytap_first; i < ytap_last; i++)
    {
      const int clip_y = clip(iy + i, 0, height - 1, bordermode);
      float h = 0.0f;
      for(int j = xtap_first; j < xtap_last; j++)
      {
        const int clip_x = clip(ix + j, 0, width - 1, bordermode);
        const float *ipixel = in + clip_y * linestride + clip_x * samplestride;
        h += kernelh[j] * ipixel[0];
      }
      s += kernelv[i] * h;
    }

    r = s / (normh * normv);
  }
  else
  {
    // invalid coordinate
    r = 0.0f;
  }
  return r;
}

/* --------------------------------------------------------------------------
 * Pixel interpolation function (see usage in iop/lens.c and iop/clipping.c)
 * ------------------------------------------------------------------------*/

static void dt_interpolation_compute_pixel4c_plain(const struct dt_interpolation *itor,
                                                   const float *in,
                                                   float *out,
                                                   const float x,
                                                   const float y,
                                                   const int width,
                                                   const int height,
                                                   const int linestride)
{
  assert(itor->width < (MAX_HALF_FILTER_WIDTH + 1));

  // Quite a bit of space for kernels
  float kernelh[MAX_KERNEL_REQ] __attribute__((aligned(SSE_ALIGNMENT)));
  float kernelv[MAX_KERNEL_REQ] __attribute__((aligned(SSE_ALIGNMENT)));

  // Compute both horizontal and vertical kernels
  float normh = compute_upsampling_kernel(itor, kernelh, NULL, x);
  float normv = compute_upsampling_kernel(itor, kernelv, NULL, y);

  // Precompute the inverse of the filter norm for later use
  const float oonorm = (1.f / (normh * normv));

  /* Now 2 cases, the pixel + filter width goes outside the image
   * in that case we have to use index clipping to keep all reads
   * in the input image (slow path) or we are sure it won't fall
   * outside and can do more simple code */
  int ix = (int)x;
  int iy = (int)y;

  if(ix >= (itor->width - 1) && iy >= (itor->width - 1) && ix < (width - itor->width)
     && iy < (height - itor->width))
  {
    // Inside image boundary case

    // Go to top left pixel
    in = (float *)in + linestride * iy + ix * 4;
    in = in - (itor->width - 1) * (4 + linestride);

    // Apply the kernel
    dt_aligned_pixel_t pixel = { 0.0f, 0.0f, 0.0f, 0.0f };
    for(int i = 0; i < 2 * itor->width; i++)
    {
      dt_aligned_pixel_t h = { 0.0f, 0.0f, 0.0f, 0.0f };
      for(int j = 0; j < 2 * itor->width; j++)
      {
        for_each_channel(c)
          h[c] += kernelh[j] * in[j * 4 + c];
      }
      for_each_channel(c)
        pixel[c] += kernelv[i] * h[c];
      in += linestride;
    }

    for_each_channel(c)
      out[c] = oonorm * pixel[c];
  }
  else if(ix >= 0 && iy >= 0 && ix < width && iy < height)
  {
    // At least a valid coordinate

    // Point to the upper left pixel index wise
    iy -= itor->width - 1;
    ix -= itor->width - 1;

    static const enum border_mode bordermode = INTERPOLATION_BORDER_MODE;
    assert(bordermode != BORDER_CLAMP); // XXX in clamp mode, norms would be wrong

    int xtap_first;
    int xtap_last;
    prepare_tap_boundaries(&xtap_first, &xtap_last,
                           bordermode, 2 * itor->width, ix, width);

    int ytap_first;
    int ytap_last;
    prepare_tap_boundaries(&ytap_first, &ytap_last,
                           bordermode, 2 * itor->width, iy, height);

    // Apply the kernel
    dt_aligned_pixel_t pixel = { 0.0f, 0.0f, 0.0f, 0.0f };
    for(int i = ytap_first; i < ytap_last; i++)
    {
      const int clip_y = clip(iy + i, 0, height - 1, bordermode);
      dt_aligned_pixel_t h = { 0.0f, 0.0f, 0.0f, 0.0f };
      for(int j = xtap_first; j < xtap_last; j++)
      {
        const int clip_x = clip(ix + j, 0, width - 1, bordermode);
        const float *ipixel = in + clip_y * linestride + clip_x * 4;
        for_each_channel(c)
          h[c] += kernelh[j] * ipixel[c];
      }
      for_each_channel(c)
        pixel[c] += kernelv[i] * h[c];
    }

    for_each_channel(c)
      out[c] = oonorm * pixel[c];
  }
  else
  {
    for_each_channel(c)
      out[c] = 0.0f;
  }
}

#if defined(__SSE2__)
static void dt_interpolation_compute_pixel4c_sse(const struct dt_interpolation *itor,
                                                 const float *in,
                                                 float *out,
                                                 const float x,
                                                 const float y,
                                                 const int width,
                                                 const int height,
                                                 const int linestride)
{
  assert(itor->width < (MAX_HALF_FILTER_WIDTH + 1));

  // Quite a bit of space for kernels
  float kernelh[MAX_KERNEL_REQ] __attribute__((aligned(SSE_ALIGNMENT)));
  float kernelv[MAX_KERNEL_REQ] __attribute__((aligned(SSE_ALIGNMENT)));

  // Compute both horizontal and vertical kernels
  float normh = compute_upsampling_kernel(itor, kernelh, NULL, x);
  float normv = compute_upsampling_kernel(itor, kernelv, NULL, y);

  // Precompute the inverse of the filter norm for later use
  const __m128 oonorm = _mm_set_ps1(1.f / (normh * normv));

  /* Now 2 cases, the pixel + filter width goes outside the image
   * in that case we have to use index clipping to keep all reads
   * in the input image (slow path) or we are sure it won't fall
   * outside and can do more simple code */
  int ix = (int)x;
  int iy = (int)y;

  if(ix >= (itor->width - 1) && iy >= (itor->width - 1) && ix < (width - itor->width)
     && iy < (height - itor->width))
  {
    // Inside image boundary case

    // Go to top left pixel
    in = (float *)in + linestride * iy + ix * 4;
    in = in - (itor->width - 1) * (4 + linestride);

    // Apply the kernel
    __m128 pixel = _mm_setzero_ps();
    for(int i = 0; i < 2 * itor->width; i++)
    {
      __m128 h = _mm_setzero_ps();
      for(int j = 0; j < 2 * itor->width; j++)
      {
        h = h + _mm_set1_ps(kernelh[j]) * *((__m128 *)&in[j * 4]);
      }
      pixel = pixel + _mm_set1_ps(kernelv[i]) * h;
      in += linestride;
    }

    *(__m128 *)out = pixel * oonorm;
  }
  else if(ix >= 0 && iy >= 0 && ix < width && iy < height)
  {
    // At least a valid coordinate

    // Point to the upper left pixel index wise
    iy -= itor->width - 1;
    ix -= itor->width - 1;

    static const enum border_mode bordermode = INTERPOLATION_BORDER_MODE;
    assert(bordermode != BORDER_CLAMP); // XXX in clamp mode, norms would be wrong

    int xtap_first;
    int xtap_last;
    prepare_tap_boundaries(&xtap_first, &xtap_last,
                           bordermode, 2 * itor->width, ix, width);

    int ytap_first;
    int ytap_last;
    prepare_tap_boundaries(&ytap_first, &ytap_last,
                           bordermode, 2 * itor->width, iy, height);

    // Apply the kernel
    __m128 pixel = _mm_setzero_ps();
    for(int i = ytap_first; i < ytap_last; i++)
    {
      int clip_y = clip(iy + i, 0, height - 1, bordermode);
      __m128 h = _mm_setzero_ps();
      for(int j = xtap_first; j < xtap_last; j++)
      {
        const int clip_x = clip(ix + j, 0, width - 1, bordermode);
        const float *ipixel = in + clip_y * linestride + clip_x * 4;
        h = h + _mm_set1_ps(kernelh[j]) * *((__m128 *)ipixel);
      }
      pixel = pixel + _mm_set1_ps(kernelv[i]) * h;
    }

    *(__m128 *)out = pixel * oonorm;
  }
  else
  {
    *(__m128 *)out = _mm_set_ps1(0.0f);
  }
}
#endif

void dt_interpolation_compute_pixel4c(const struct dt_interpolation *itor,
                                      const float *in,
                                      float *out,
                                      const float x,
                                      const float y,
                                      const int width,
                                      const int height,
                                      const int linestride)
{
#if defined(__SSE2__)
  if(darktable.codepath.SSE2)
    return dt_interpolation_compute_pixel4c_sse(itor, in, out, x, y,
                                                width, height, linestride);
  else
#endif
    return dt_interpolation_compute_pixel4c_plain(itor, in, out, x, y,
                                                  width, height, linestride);
}

static void dt_interpolation_compute_pixel1c_plain(const struct dt_interpolation *itor,
                                                   const float *in,
                                                   float *out,
                                                   const float x,
                                                   const float y,
                                                   const int width,
                                                   const int height,
                                                   const int linestride)
{
  assert(itor->width < (MAX_HALF_FILTER_WIDTH + 1));

  // Quite a bit of space for kernels
  float kernelh[MAX_KERNEL_REQ] __attribute__((aligned(SSE_ALIGNMENT)));
  float kernelv[MAX_KERNEL_REQ] __attribute__((aligned(SSE_ALIGNMENT)));

  // Compute both horizontal and vertical kernels
  float normh = compute_upsampling_kernel(itor, kernelh, NULL, x);
  float normv = compute_upsampling_kernel(itor, kernelv, NULL, y);

  // Precompute the inverse of the filter norm for later use
  const float oonorm = (1.f / (normh * normv));

  /* Now 2 cases, the pixel + filter width goes outside the image
   * in that case we have to use index clipping to keep all reads
   * in the input image (slow path) or we are sure it won't fall
   * outside and can do more simple code */
  int ix = (int)x;
  int iy = (int)y;

  if(ix >= (itor->width - 1) && iy >= (itor->width - 1) && ix < (width - itor->width)
    && iy < (height - itor->width))
  {
    // Inside image boundary case

    // Go to top left pixel
    in = (float *)in + linestride * iy + ix;
    in = in - (itor->width - 1) * (1 + linestride);

    // Apply the kernel
    float pixel = 0.0f;
    for(int i = 0; i < 2 * itor->width; i++)
    {
      float h = 0.0f;
      for(int j = 0; j < 2 * itor->width; j++)
      {
        h += kernelh[j] * in[j];
      }
      pixel += kernelv[i] * h;
      in += linestride;
    }

    *out = oonorm * pixel;
  }
  else if(ix >= 0 && iy >= 0 && ix < width && iy < height)
  {
    // At least a valid coordinate

    // Point to the upper left pixel index wise
    iy -= itor->width - 1;
    ix -= itor->width - 1;

    static const enum border_mode bordermode = INTERPOLATION_BORDER_MODE;
    assert(bordermode != BORDER_CLAMP); // XXX in clamp mode, norms would be wrong

    int xtap_first;
    int xtap_last;
    prepare_tap_boundaries(&xtap_first, &xtap_last, bordermode, 2 * itor->width, ix, width);

    int ytap_first;
    int ytap_last;
    prepare_tap_boundaries(&ytap_first, &ytap_last, bordermode, 2 * itor->width, iy, height);

    // Apply the kernel
    float pixel = 0.0f;
    for(int i = ytap_first; i < ytap_last; i++)
    {
      const int clip_y = clip(iy + i, 0, height - 1, bordermode);
      float h = 0.0f;
      for(int j = xtap_first; j < xtap_last; j++)
      {
        const int clip_x = clip(ix + j, 0, width - 1, bordermode);
        const float *ipixel = in + clip_y * linestride + clip_x;
        h += kernelh[j] * *ipixel;
      }
      pixel += kernelv[i] * h;
    }

    *out = oonorm * pixel;
  }
  else
  {
    *out = 0.0f;
  }
}

void dt_interpolation_compute_pixel1c(const struct dt_interpolation *itor,
                                      const float *in,
                                      float *out,
                                      const float x,
                                      const float y,
                                      const int width,
                                      const int height,
                                      const int linestride)
{
  return dt_interpolation_compute_pixel1c_plain(itor, in, out, x, y,
                                                width, height, linestride);
}

/* --------------------------------------------------------------------------
 * Interpolation factory
 * ------------------------------------------------------------------------*/

const struct dt_interpolation *dt_interpolation_new(enum dt_interpolation_type type)
{
  const struct dt_interpolation *itor = NULL;

  if(type == DT_INTERPOLATION_USERPREF)
  {
    // Find user preferred interpolation method
    const char *uipref =
      dt_conf_get_string_const("plugins/lighttable/export/pixel_interpolator");

    for(int i = DT_INTERPOLATION_FIRST;
        uipref && i < DT_INTERPOLATION_LAST;
        i++)
    {
      if(!strcmp(uipref, dt_interpolator[i].name))
      {
        // Found the one
        itor = &dt_interpolator[i];
        break;
      }
    }

    /* In the case the search failed (!uipref or name not found),
     * prepare later search pass with default fallback */
    type = DT_INTERPOLATION_DEFAULT;
  }
  else if(type == DT_INTERPOLATION_USERPREF_WARP)
  {
    // Find user preferred interpolation method
    const char *uipref =
      dt_conf_get_string_const("plugins/lighttable/export/pixel_interpolator_warp");
    for(int i = DT_INTERPOLATION_FIRST;
        uipref && i < DT_INTERPOLATION_LAST;
        i++)
    {
      if(!strcmp(uipref, dt_interpolator[i].name))
      {
        // Found the one
        itor = &dt_interpolator[i];
        break;
      }
    }

    /* In the case the search failed (!uipref or name not found),
     * prepare later search pass with default fallback */
    type = DT_INTERPOLATION_DEFAULT_WARP;
  }
  if(!itor)
  {
    // Did not find the userpref one or we've been asked for a specific one
    for(int i = DT_INTERPOLATION_FIRST; i < DT_INTERPOLATION_LAST; i++)
    {
      if(dt_interpolator[i].id == type)
      {
        itor = &dt_interpolator[i];
        break;
      }
      if(dt_interpolator[i].id == DT_INTERPOLATION_DEFAULT)
      {
        itor = &dt_interpolator[i];
      }
    }
  }

  return itor;
}

/* --------------------------------------------------------------------------
 * Image resampling
 * ------------------------------------------------------------------------*/

/** Prepares a 1D resampling plan
 *
 * This consists of the following information
 * <ul>
 * <li>A list of lengths that tell how many pixels are relevant for the
 *    next output</li>
 * <li>A list of required filter kernels</li>
 * <li>A list of sample indexes</li>
 * </ul>
 *
 * How to apply the resampling plan:
 * <ol>
 * <li>Pick a length from the length array</li>
 * <li>until length is reached
 *     <ol>
 *     <li>pick a kernel tap></li>
 *     <li>pick the relevant sample according to the picked index</li>
 *     <li>multiply them and accumulate</li>
 *     </ol>
 * </li>
 * <li>here goes a single output sample</li>
 * </ol>
 *
 * This until you reach the number of output pixels
 *
 * @param itor interpolator used to resample
 * @param in [in] Number of input samples
 * @param out [in] Number of output samples
 * @param plength [out] Array of lengths for each pixel filtering (number
 * of taps/indexes to use). This array mus be freed with dt_free_align() when you're
 * done with the plan.
 * @param pkernel [out] Array of filter kernel taps
 * @param pindex [out] Array of sample indexes to be used for applying each kernel tap
 * arrays of information
 * @param pmeta [out] Array of int triplets (length, kernel, index) telling where to
 *        start for an arbitrary
 * out position meta[3*out]
 * @return 0 for success, !0 for failure
 */
static int prepare_resampling_plan(const struct dt_interpolation *itor,
                                   const int in,
                                   const int in_x0,
                                   const int out,
                                   const int out_x0,
                                   const float scale,
                                   int **plength,
                                   float **pkernel,
                                   int **pindex,
                                   int **pmeta)
{
  // Safe return values
  *plength = NULL;
  *pkernel = NULL;
  *pindex = NULL;
  if(pmeta)
  {
    *pmeta = NULL;
  }

  if(scale == 1.f)
  {
    // No resampling required
    return 0;
  }

  // Compute common upsampling/downsampling memory requirements
  int maxtapsapixel;
  if(scale > 1.f)
  {
    // Upscale... the easy one. The values are exact
    maxtapsapixel = 2 * itor->width;
  }
  else
  {
    // Downscale... going for worst case values memory wise
    maxtapsapixel = ceil_fast((float)2 * (float)itor->width / scale);
  }

  int nlengths = out;
  const int nindex = maxtapsapixel * out;
  const int nkernel = maxtapsapixel * out;
  const size_t lengthreq = increase_for_alignment(nlengths * sizeof(int), SSE_ALIGNMENT);
  const size_t indexreq = increase_for_alignment(nindex * sizeof(int), SSE_ALIGNMENT);
  const size_t kernelreq = increase_for_alignment(nkernel * sizeof(float), SSE_ALIGNMENT);
  const size_t scratchreq = maxtapsapixel * sizeof(float) + 4 * sizeof(float);
  // NB: because sse versions compute four taps a time
  const size_t metareq = pmeta ? 3 * sizeof(int) * out : 0;

  void *blob = NULL;
  const size_t totalreq = kernelreq + lengthreq + indexreq + scratchreq + metareq;
  blob = dt_alloc_align(SSE_ALIGNMENT, totalreq);
  if(!blob)
  {
    return 1;
  }

  int *lengths = (int *)blob;
  blob = (char *)blob + lengthreq;
  int *index = (int *)blob;
  blob = (char *)blob + indexreq;
  float *kernel = (float *)blob;
  blob = (char *)blob + kernelreq;
  float *scratchpad = scratchreq ? (float *)blob : NULL;
  blob = (char *)blob + scratchreq;
  int *meta = metareq ? (int *)blob : NULL;
//   blob = (char *)blob + metareq;

  /* setting this as a const should help the compilers trim all unnecessary
   * codepaths */
  const enum border_mode bordermode = RESAMPLING_BORDER_MODE;

  /* Upscale and downscale differ in subtle points, getting rid of code
   * duplication might have been tricky and i prefer keeping the code
   * as straight as possible */
  if(scale > 1.f)
  {
    int kidx = 0;
    int iidx = 0;
    int lidx = 0;
    int midx = 0;
    for(int x = 0; x < out; x++)
    {
      if(meta)
      {
        meta[midx++] = lidx;
        meta[midx++] = kidx;
        meta[midx++] = iidx;
      }

      // Projected position in input samples
      float fx = (float)(out_x0 + x) / scale;

      // Compute the filter kernel at that position
      int first;
      (void)compute_upsampling_kernel(itor, scratchpad, &first, fx);

      /* Check lower and higher bound pixel index and skip as many pixels as
       * necessary to fall into range */
      int tap_first;
      int tap_last;
      prepare_tap_boundaries(&tap_first, &tap_last, bordermode, 2 * itor->width, first, in);

      // Track number of taps that will be used
      lengths[lidx++] = tap_last - tap_first;

      // Precompute the inverse of the norm
      float norm = 0.f;
      for(int tap = tap_first; tap < tap_last; tap++)
      {
        norm += scratchpad[tap];
      }
      norm = 1.f / norm;

      /* Unlike single pixel or single sample code, here it's interesting to
       * precompute the normalized filter kernel as this will avoid dividing
       * by the norm for all processed samples/pixels
       * NB: use the same loop to put in place the index list */
      first += tap_first;
      for(int tap = tap_first; tap < tap_last; tap++)
      {
        kernel[kidx++] = scratchpad[tap] * norm;
        index[iidx++] = clip(first++, 0, in - 1, bordermode);
      }
    }
  }
  else
  {
    int kidx = 0;
    int iidx = 0;
    int lidx = 0;
    int midx = 0;
    for(int x = 0; x < out; x++)
    {
      if(meta)
      {
        meta[midx++] = lidx;
        meta[midx++] = kidx;
        meta[midx++] = iidx;
      }

      // Compute downsampling kernel centered on output position
      int taps;
      int first;
      compute_downsampling_kernel(itor, &taps, &first, scratchpad, NULL, scale, out_x0 + x);

      /* Check lower and higher bound pixel index and skip as many pixels as
       * necessary to fall into range */
      int tap_first;
      int tap_last;
      prepare_tap_boundaries(&tap_first, &tap_last, bordermode, taps, first, in);

      // Track number of taps that will be used
      lengths[lidx++] = tap_last - tap_first;

      // Precompute the inverse of the norm
      float norm = 0.f;
      for(int tap = tap_first; tap < tap_last; tap++)
      {
        norm += scratchpad[tap];
      }
      norm = 1.f / norm;

      /* Unlike single pixel or single sample code, here it's interesting to
       * precompute the normalized filter kernel as this will avoid dividing
       * by the norm for all processed samples/pixels
       * NB: use the same loop to put in place the index list */
      first += tap_first;
      for(int tap = tap_first; tap < tap_last; tap++)
      {
        kernel[kidx++] = scratchpad[tap] * norm;
        index[iidx++] = clip(first++, 0, in - 1, bordermode);
      }
    }
  }

  // Validate plan wrt caller
  *plength = lengths;
  *pindex = index;
  *pkernel = kernel;
  if(pmeta)
  {
    *pmeta = meta;
  }

  return 0;
}

static void dt_interpolation_resample_plain(const struct dt_interpolation *itor,
                                            float *out,
                                            const dt_iop_roi_t *const roi_out,
                                            const int32_t out_stride,
                                            const float *const in,
                                            const dt_iop_roi_t *const roi_in,
                                            const int32_t in_stride)
{
  int *hindex = NULL;
  int *hlength = NULL;
  float *hkernel = NULL;
  int *vindex = NULL;
  int *vlength = NULL;
  float *vkernel = NULL;
  int *vmeta = NULL;

  const int32_t in_stride_floats = in_stride / sizeof(float);
  const int32_t out_stride_floats = out_stride / sizeof(float);
  int r;

  dt_print_pipe(DT_DEBUG_PIPE | DT_DEBUG_VERBOSE,
                "resample_plain", NULL, itor->name, roi_in, roi_out, "\n");
  dt_times_t start = { 0 }, mid = { 0 };
  dt_get_perf_times(&start);

  // Fast code path for 1:1 copy, only cropping area can change
  if(roi_out->scale == 1.f)
  {
    const int x0 = roi_out->x * 4 * sizeof(float);

#ifdef _OPENMP
#pragma omp parallel for default(none) \
    dt_omp_firstprivate(in, in_stride, out_stride, roi_out, x0) \
    shared(out)
#endif
    for(int y = 0; y < roi_out->height; y++)
    {
      memcpy((char *)out + (size_t)out_stride * y,
             (char *)in + (size_t)in_stride * (y + roi_out->y) + x0,
             out_stride);
    }

    dt_show_times_f(&start, "[resample_plain]", "1:1 copy/crop of %dx%d pixels",
                    roi_in->width, roi_in->height);
    // All done, so easy case
    return;
  }

  // Generic non 1:1 case... much more complicated :D

  // Prepare resampling plans once and for all
  r = prepare_resampling_plan(itor, roi_in->width, roi_in->x,
                              roi_out->width, roi_out->x, roi_out->scale,
                              &hlength, &hkernel, &hindex, NULL);
  if(r)
  {
    goto exit;
  }

  r = prepare_resampling_plan(itor, roi_in->height, roi_in->y,
                              roi_out->height, roi_out->y, roi_out->scale,
                              &vlength, &vkernel, &vindex, &vmeta);
  if(r)
  {
    goto exit;
  }

  dt_get_perf_times(&mid);

  const size_t height = roi_out->height;
  const size_t width = roi_out->width;

  // Process each output line
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(in, in_stride_floats, out, out_stride_floats, height, width, \
                      hlength, hindex, hkernel, vlength, vindex, vkernel)       \
  shared(vmeta)
#endif
  for(size_t oy = 0; oy < height; oy++)
  {
    // Initialize column resampling indexes
    int vlidx = vmeta[3 * oy + 0]; // V(ertical) L(ength) I(n)d(e)x
    int vkidx = vmeta[3 * oy + 1]; // V(ertical) K(ernel) I(n)d(e)x
    int viidx = vmeta[3 * oy + 2]; // V(ertical) I(ndex) I(n)d(e)x

    // Initialize row resampling indexes
    int hlidx = 0; // H(orizontal) L(ength) I(n)d(e)x
    int hkidx = 0; // H(orizontal) K(ernel) I(n)d(e)x

    // Number of lines contributing to the output line
    int vl = vlength[vlidx++]; // V(ertical) L(ength)

    // Process each output column
    for(size_t ox = 0; ox < width; ox++)
    {
      debug_extra("output %p [% 4d % 4d]\n", out, ox, oy);

      // This will hold the resulting pixel
      dt_aligned_pixel_t vs = { 0.0f, 0.0f, 0.0f, 0.0f };

      // Number of horizontal samples contributing to the output
      int hl = hlength[hlidx++]; // H(orizontal) L(ength)

      for(size_t iy = 0; iy < vl; iy++)
      {
        // This is our input line
        size_t baseidx_vindex = (size_t)vindex[viidx++] * in_stride_floats;

        dt_aligned_pixel_t vhs = { 0.0f, 0.0f, 0.0f, 0.0f };

        for(size_t ix = 0; ix < hl; ix++)
        {
          // Apply the precomputed filter kernel
          const size_t baseidx = baseidx_vindex + (size_t)hindex[hkidx] * 4;
          const float htap = hkernel[hkidx++];
          dt_aligned_pixel_t tmp;
          copy_pixel(tmp, in + baseidx);
          for_each_channel(c, aligned(tmp,vhs:16))
            vhs[c] += tmp[c] * htap;
        }

        // Accumulate contribution from this line
        const float vtap = vkernel[vkidx++];
        for_each_channel(c, aligned(vhs,vs:16)) vs[c] += vhs[c] * vtap;

        // Reset horizontal resampling context
        hkidx -= hl;
      }

      // Output pixel is ready
      const size_t baseidx = (size_t)oy * out_stride_floats + (size_t)ox * 4;

      // Clip negative RGB that may be produced by Lanczos undershooting
      // Negative RGB are invalid values no matter the RGB space (light is positive)
      dt_aligned_pixel_t pixel;
      for_each_channel(c, aligned(vs:16))
        pixel[c] = MAX(vs[c], 0.f);
      copy_pixel_nontemporal(out + baseidx, pixel);

      // Reset vertical resampling context
      viidx -= vl;
      vkidx -= vl;

      // Progress in horizontal context
      hkidx += hl;
    }
  }
  dt_omploop_sfence();

exit:
  /* Free the resampling plans. It's nasty to optimize allocs like that, but
   * it simplifies the code :-D. The length array is in fact the only memory
   * allocated. */
  dt_free_align(hlength);
  dt_free_align(vlength);
  _show_2_times(&start, &mid, "resample_plain");
}

/** Applies resampling (re-scaling) on *full* input and output buffers.
 *  roi_in and roi_out define the part of the buffers that is affected.
 */
void dt_interpolation_resample(const struct dt_interpolation *itor,
                               float *out,
                               const dt_iop_roi_t *const roi_out,
                               const int32_t out_stride,
                               const float *const in,
                               const dt_iop_roi_t *const roi_in,
                               const int32_t in_stride)
{
  if(out == NULL)
  {
    dt_print(DT_DEBUG_ALWAYS, "[dt_interpolation_resample] no valid output buffer\n");
    return;
  }

  return dt_interpolation_resample_plain(itor, out, roi_out, out_stride, in,
                                         roi_in, in_stride);
}

/** Applies resampling (re-scaling) on a specific region-of-interest
 *  of an image. The input and output buffers hold exactly those
 *  roi's. roi_in and roi_out define the relative positions of the
 *  roi's within the full input and output image, respectively.
 */
void dt_interpolation_resample_roi(const struct dt_interpolation *itor,
                                   float *out,
                                   const dt_iop_roi_t *const roi_out,
                                   const int32_t out_stride,
                                   const float *const in,
                                   const dt_iop_roi_t *const roi_in,
                                   const int32_t in_stride)
{
  dt_iop_roi_t oroi = *roi_out;
  oroi.x = oroi.y = 0;

  dt_iop_roi_t iroi = *roi_in;
  iroi.x = iroi.y = 0;

  dt_interpolation_resample(itor, out, &oroi, out_stride, in, &iroi, in_stride);
}

#ifdef HAVE_OPENCL
dt_interpolation_cl_global_t *dt_interpolation_init_cl_global()
{
  dt_interpolation_cl_global_t *g
      = (dt_interpolation_cl_global_t *)malloc(sizeof(dt_interpolation_cl_global_t));

  const int program = 2; // basic.cl, from programs.conf
  g->kernel_interpolation_resample =
    dt_opencl_create_kernel(program, "interpolation_resample");
  return g;
}

void dt_interpolation_free_cl_global(dt_interpolation_cl_global_t *g)
{
  if(!g) return;
  // destroy kernels
  dt_opencl_free_kernel(g->kernel_interpolation_resample);
  free(g);
}

static uint32_t roundToNextPowerOfTwo(uint32_t x)
{
  x--;
  x |= x >> 1;
  x |= x >> 2;
  x |= x >> 4;
  x |= x >> 8;
  x |= x >> 16;
  x++;
  return x;
}

/** Applies resampling (re-scaling) on *full* input and output buffers.
 *  roi_in and roi_out define the part of the buffers that is affected.
 */
int dt_interpolation_resample_cl(const struct dt_interpolation *itor,
                                 const int devid,
                                 cl_mem dev_out,
                                 const dt_iop_roi_t *const roi_out,
                                 cl_mem dev_in,
                                 const dt_iop_roi_t *const roi_in)
{
  int *hindex = NULL;
  int *hlength = NULL;
  float *hkernel = NULL;
  int *hmeta = NULL;
  int *vindex = NULL;
  int *vlength = NULL;
  float *vkernel = NULL;
  int *vmeta = NULL;

  int r;
  cl_int err = -999;

  cl_mem dev_hindex = NULL;
  cl_mem dev_hlength = NULL;
  cl_mem dev_hkernel = NULL;
  cl_mem dev_hmeta = NULL;
  cl_mem dev_vindex = NULL;
  cl_mem dev_vlength = NULL;
  cl_mem dev_vkernel = NULL;
  cl_mem dev_vmeta = NULL;

  dt_print_pipe(DT_DEBUG_PIPE | DT_DEBUG_VERBOSE,
                "resample_cl", NULL, itor->name, roi_in, roi_out, "\n");
  dt_times_t start = { 0 }, mid = { 0 };
  dt_get_perf_times(&start);

  // Fast code path for 1:1 copy, only cropping area can change
  if(roi_out->scale == 1.f)
  {
    size_t iorigin[] = { roi_out->x, roi_out->y, 0 };
    size_t oorigin[] = { 0, 0, 0 };
    size_t region[] = { roi_out->width, roi_out->height, 1 };

    // copy original input from dev_in -> dev_out as starting point
    err = dt_opencl_enqueue_copy_image(devid, dev_in, dev_out, iorigin, oorigin, region);
    if(err != CL_SUCCESS) goto error;

    dt_show_times_f(&start, "[resample_cl]", "1:1 copy/crop of %dx%d pixels",
                    roi_in->width, roi_in->height);
    // All done, so easy case
    return CL_SUCCESS;
  }

// Generic non 1:1 case... much more complicated :D

  // Prepare resampling plans once and for all
  r = prepare_resampling_plan(itor, roi_in->width, roi_in->x,
                              roi_out->width, roi_out->x, roi_out->scale,
                              &hlength, &hkernel, &hindex, &hmeta);
  if(r)
  {
    goto error;
  }

  r = prepare_resampling_plan(itor, roi_in->height, roi_in->y,
                              roi_out->height, roi_out->y, roi_out->scale,
                              &vlength, &vkernel, &vindex, &vmeta);
  if(r)
  {
    goto error;
  }

  dt_get_perf_times(&mid);

  int hmaxtaps = -1, vmaxtaps = -1;
  for(int k = 0; k < roi_out->width; k++) hmaxtaps = MAX(hmaxtaps, hlength[k]);
  for(int k = 0; k < roi_out->height; k++) vmaxtaps = MAX(vmaxtaps, vlength[k]);

  // strategy: process image column-wise (local[0] = 1). For each row generate
  // a number of parallel work items each taking care of one horizontal convolution,
  // then sum over work items to do the vertical convolution

  const int kernel = darktable.opencl->interpolation->kernel_interpolation_resample;
  const int width = roi_out->width;
  const int height = roi_out->height;

  // make sure blocksize is not too large
  const int taps = roundToNextPowerOfTwo(vmaxtaps);
  // the number of work items per row rounded up to a power of 2
  // (for quick recursive reduction)

  int vblocksize;

  dt_opencl_local_buffer_t locopt
    = (dt_opencl_local_buffer_t)
        { .xoffset = 0,
          .xfactor = 1,
          .yoffset = 0,
          .yfactor = 1,
          .cellsize = 4 * sizeof(float),
          .overhead = hmaxtaps * sizeof(float) + hmaxtaps * sizeof(int),
          .sizex = 1,
          .sizey = (1 << 16) * taps };

  if(dt_opencl_local_buffer_opt(devid, kernel, &locopt))
    vblocksize = locopt.sizey;
  else
    vblocksize = 1;

  if(vblocksize < taps)
  {
    // our strategy does not work: the vertical number of taps exceeds
    // the vertical workgroupsize; there is no point in continuing on
    // the GPU - that would be way too slow; let's delegate the stuff
    // to the CPU then.
    dt_print(DT_DEBUG_OPENCL,
             "[dt_interpolation_resample_cl] resampling plan cannot"
             " efficiently be run on the GPU - fall back to CPU.\n");
    goto error;
  }

  size_t sizes[3] = { ROUNDUPDWD(width, devid), ROUNDUP(height * taps, vblocksize), 1 };
  size_t local[3] = { 1, vblocksize, 1 };

  // store resampling plan to device memory hindex, vindex, hkernel,
  // vkernel: (v|h)maxtaps might be too small, so store a bit more
  // than needed
  dev_hindex = dt_opencl_copy_host_to_device_constant
    (devid, sizeof(int) * width * (hmaxtaps + 1), hindex);
  if(dev_hindex == NULL) goto error;

  dev_hlength = dt_opencl_copy_host_to_device_constant
    (devid, sizeof(int) * width, hlength);
  if(dev_hlength == NULL) goto error;

  dev_hkernel
      = dt_opencl_copy_host_to_device_constant
    (devid, sizeof(float) * width * (hmaxtaps + 1), hkernel);
  if(dev_hkernel == NULL) goto error;

  dev_hmeta = dt_opencl_copy_host_to_device_constant
    (devid, sizeof(int) * width * 3, hmeta);
  if(dev_hmeta == NULL) goto error;

  dev_vindex = dt_opencl_copy_host_to_device_constant
    (devid, sizeof(int) * height * (vmaxtaps + 1), vindex);
  if(dev_vindex == NULL) goto error;

  dev_vlength = dt_opencl_copy_host_to_device_constant
    (devid, sizeof(int) * height, vlength);
  if(dev_vlength == NULL) goto error;

  dev_vkernel
      = dt_opencl_copy_host_to_device_constant
    (devid, sizeof(float) * height * (vmaxtaps + 1), vkernel);
  if(dev_vkernel == NULL) goto error;

  dev_vmeta = dt_opencl_copy_host_to_device_constant
    (devid, sizeof(int) * height * 3, vmeta);
  if(dev_vmeta == NULL) goto error;

  dt_opencl_set_kernel_args(devid, kernel, 0, CLARG(dev_in), CLARG(dev_out),
                            CLARG(width), CLARG(height),
                            CLARG(dev_hmeta), CLARG(dev_vmeta), CLARG(dev_hlength),
                            CLARG(dev_vlength), CLARG(dev_hindex),
                            CLARG(dev_vindex), CLARG(dev_hkernel), CLARG(dev_vkernel),
                            CLARG(hmaxtaps), CLARG(taps), CLLOCAL(hmaxtaps * sizeof(float)),
                            CLLOCAL(hmaxtaps * sizeof(int)),
                            CLLOCAL(vblocksize * 4 * sizeof(float)));
  err = dt_opencl_enqueue_kernel_2d_with_local(devid, kernel, sizes, local);
  if(err != CL_SUCCESS) goto error;

  dt_opencl_release_mem_object(dev_hindex);
  dt_opencl_release_mem_object(dev_hlength);
  dt_opencl_release_mem_object(dev_hkernel);
  dt_opencl_release_mem_object(dev_hmeta);
  dt_opencl_release_mem_object(dev_vindex);
  dt_opencl_release_mem_object(dev_vlength);
  dt_opencl_release_mem_object(dev_vkernel);
  dt_opencl_release_mem_object(dev_vmeta);
  dt_free_align(hlength);
  dt_free_align(vlength);

  _show_2_times(&start, &mid, "resample_cl");
  return CL_SUCCESS;

error:
  dt_opencl_release_mem_object(dev_hindex);
  dt_opencl_release_mem_object(dev_hlength);
  dt_opencl_release_mem_object(dev_hkernel);
  dt_opencl_release_mem_object(dev_hmeta);
  dt_opencl_release_mem_object(dev_vindex);
  dt_opencl_release_mem_object(dev_vlength);
  dt_opencl_release_mem_object(dev_vkernel);
  dt_opencl_release_mem_object(dev_vmeta);
  dt_free_align(hlength);
  dt_free_align(vlength);
  dt_print(DT_DEBUG_ALWAYS,
           "[dt_interpolation_resample_cl] couldn't enqueue kernel! %s\n",
           cl_errstr(err));
  return err;
}

/** Applies resampling (re-scaling) on a specific region-of-interest
 *  of an image. The input and output buffers hold exactly those
 *  roi's. roi_in and roi_out define the relative positions of the
 *  roi's within the full input and output image, respectively.
 */
int dt_interpolation_resample_roi_cl(const struct dt_interpolation *itor,
                                     const int devid,
                                     cl_mem dev_out,
                                     const dt_iop_roi_t *const roi_out,
                                     cl_mem dev_in,
                                     const dt_iop_roi_t *const roi_in)
{
  dt_iop_roi_t oroi = *roi_out;
  oroi.x = oroi.y = 0;

  dt_iop_roi_t iroi = *roi_in;
  iroi.x = iroi.y = 0;

  return dt_interpolation_resample_cl(itor, devid, dev_out, &oroi, dev_in, &iroi);
}
#endif

static void dt_interpolation_resample_1c_plain(const struct dt_interpolation *itor,
                                               float *out,
                                               const dt_iop_roi_t *const roi_out,
                                               const int32_t out_stride,
                                               const float *const in,
                                               const dt_iop_roi_t *const roi_in,
                                               const int32_t in_stride)
{
  int *hindex = NULL;
  int *hlength = NULL;
  float *hkernel = NULL;
  int *vindex = NULL;
  int *vlength = NULL;
  float *vkernel = NULL;
  int *vmeta = NULL;

  int r;

  dt_print_pipe(DT_DEBUG_PIPE | DT_DEBUG_VERBOSE,
                "resample_1c_plain", NULL, itor->name, roi_in, roi_out, "\n");
  dt_times_t start = { 0 }, mid = { 0 };
  dt_get_perf_times(&start);

  // Fast code path for 1:1 copy, only cropping area can change
  if(roi_out->scale == 1.f)
  {
    const int x0 = roi_out->x * sizeof(float);
#ifdef _OPENMP
#pragma omp parallel for default(none) \
    dt_omp_firstprivate(in, in_stride, out_stride, roi_out, x0) \
    shared(out)
#endif
    for(int y = 0; y < roi_out->height; y++)
    {
      float *i = (float *)((char *)in + (size_t)in_stride * (y + roi_out->y) + x0);
      float *o = (float *)((char *)out + (size_t)out_stride * y);
      memcpy(o, i, out_stride);
    }
    dt_show_times_f(&start, "[resample_1c_plain]", "1:1 copy/crop of %dx%d pixels",
                    roi_in->width, roi_in->height);
    // All done, so easy case
    return;
  }

  // Generic non 1:1 case... much more complicated :D

  // Prepare resampling plans once and for all
  r = prepare_resampling_plan(itor, roi_in->width, roi_in->x,
                              roi_out->width, roi_out->x, roi_out->scale,
                              &hlength, &hkernel, &hindex, NULL);
  if(r)
  {
    goto exit;
  }

  r = prepare_resampling_plan(itor, roi_in->height, roi_in->y,
                              roi_out->height, roi_out->y, roi_out->scale,
                              &vlength, &vkernel, &vindex, &vmeta);
  if(r)
  {
    goto exit;
  }

  dt_get_perf_times(&mid);

  // Process each output line
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(in, in_stride, out_stride, roi_out) \
  shared(out, hindex, hlength, hkernel, vindex, vlength, vkernel, vmeta)
#endif
  for(int oy = 0; oy < roi_out->height; oy++)
  {
    // Initialize column resampling indexes
    int vlidx = vmeta[3 * oy + 0]; // V(ertical) L(ength) I(n)d(e)x
    int vkidx = vmeta[3 * oy + 1]; // V(ertical) K(ernel) I(n)d(e)x
    int viidx = vmeta[3 * oy + 2]; // V(ertical) I(ndex) I(n)d(e)x

    // Initialize row resampling indexes
    int hlidx = 0; // H(orizontal) L(ength) I(n)d(e)x
    int hkidx = 0; // H(orizontal) K(ernel) I(n)d(e)x
    int hiidx = 0; // H(orizontal) I(ndex) I(n)d(e)x

    // Number of lines contributing to the output line
    int vl = vlength[vlidx++]; // V(ertical) L(ength)

    // Process each output column
    for(int ox = 0; ox < roi_out->width; ox++)
    {
      debug_extra("output %p [% 4d % 4d]\n", out, ox, oy);

      // This will hold the resulting pixel
      float vs = 0.0f;

      // Number of horizontal samples contributing to the output
      const int hl = hlength[hlidx++]; // H(orizontal) L(ength)

      for(int iy = 0; iy < vl; iy++)
      {
        // This is our input line
        const float *i = (float *)((char *)in + (size_t)in_stride * vindex[viidx++]);

        float vhs = 0.0f;

        for(int ix = 0; ix < hl; ix++)
        {
          // Apply the precomputed filter kernel
          const size_t baseidx = (size_t)hindex[hiidx++];
          const float htap = hkernel[hkidx++];
          vhs += i[baseidx] * htap;
        }

        // Accumulate contribution from this line
        const float vtap = vkernel[vkidx++];
        vs += vhs * vtap;

        // Reset horizontal resampling context
        hkidx -= hl;
        hiidx -= hl;
      }

      // Output pixel is ready
      float *o = (float *)((char *)out + (size_t)oy * out_stride
                           + (size_t)ox * sizeof(float));
      *o = vs;

      // Reset vertical resampling context
      viidx -= vl;
      vkidx -= vl;

      // Progress in horizontal context
      hiidx += hl;
      hkidx += hl;
    }
  }

  exit:
  /* Free the resampling plans. It's nasty to optimize allocs like that, but
   * it simplifies the code :-D. The length array is in fact the only memory
   * allocated. */
  dt_free_align(hlength);
  dt_free_align(vlength);
  _show_2_times(&start, &mid, "resample_1c_plain");
}

/** Applies resampling (re-scaling) on *full* input and output buffers.
 *  roi_in and roi_out define the part of the buffers that is affected.
 */
void dt_interpolation_resample_1c(const struct dt_interpolation *itor,
                                  float *out,
                                  const dt_iop_roi_t *const roi_out,
                                  const int32_t out_stride,
                                  const float *const in,
                                  const dt_iop_roi_t *const roi_in,
                                  const int32_t in_stride)
{
  return dt_interpolation_resample_1c_plain(itor, out, roi_out, out_stride,
                                            in, roi_in, in_stride);
}

/** Applies resampling (re-scaling) on a specific region-of-interest of an image. The input
 *  and output buffers hold exactly those roi's. roi_in and roi_out define the relative
 *  positions of the roi's within the full input and output image, respectively.
 */
void dt_interpolation_resample_roi_1c(const struct dt_interpolation *itor,
                                      float *out,
                                      const dt_iop_roi_t *const roi_out,
                                      const int32_t out_stride,
                                      const float *const in,
                                      const dt_iop_roi_t *const roi_in,
                                      const int32_t in_stride)
{
  dt_iop_roi_t oroi = *roi_out;
  oroi.x = oroi.y = 0;

  dt_iop_roi_t iroi = *roi_in;
  iroi.x = iroi.y = 0;

  dt_interpolation_resample_1c(itor, out, &oroi, out_stride, in, &iroi, in_stride);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
