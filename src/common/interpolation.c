/* --------------------------------------------------------------------------
    This file is part of darktable,
    copyright (c) 2012 Edouard Gomez <ed.gomez@free.fr>

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
#include "control/conf.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <glib.h>
#include <assert.h>

/* --------------------------------------------------------------------------
 * Interpolation kernels
 * ------------------------------------------------------------------------*/

/* --------------------------------------------------------------------------
 * Bilinear interpolation
 * ------------------------------------------------------------------------*/

static inline float
bilinear(float width, float t)
{
  float r;
  t = fabsf(t);
  if (t>1.f) {
    r = 0.f;
  } else {
    r = 1.f - t;
  }
  return r;
}

static inline __m128
_mm_abs_ps(__m128 t)
{
    static const uint32_t signmask[4] = { 0x7fffffff, 0x7fffffff, 0x7fffffff, 0x7fffffff};
    return _mm_and_ps(*(__m128*)signmask, t);
}

static inline __m128
bilinear_sse(__m128 width, __m128 t)
{
    static const __m128 one = { 1.f, 1.f, 1.f, 1.f};
    return _mm_sub_ps(one, _mm_abs_ps(t));
}

/* --------------------------------------------------------------------------
 * Bicubic interpolation
 * ------------------------------------------------------------------------*/

static inline float
bicubic(float width, float t)
{
  float r;
  t = fabsf(t);
  if (t>=2.f) {
    r = 0.f;
  } else if (t>1.f && t<2.f) {
    float t2 = t*t;
    r = 0.5f*(t*(-t2 + 5.f*t - 8.f) + 4.f);
  } else {
    float t2 = t*t;
    r = 0.5f*(t*(3.f*t2 - 5.f*t) + 2.f);
  }
  return r;
}

static inline __m128
bicubic_sse(__m128 width, __m128 t)
{
    static const __m128 half  = { .5f, .5f, .5f, .5f};
    static const __m128 one   = { 1.f, 1.f, 1.f, 1.f};
    static const __m128 two   = { 2.f, 2.f, 2.f, 2.f};
    static const __m128 three = { 3.f, 3.f, 3.f, 3.f};
    static const __m128 four  = { 4.f, 4.f, 4.f, 4.f};
    static const __m128 five  = { 5.f, 5.f, 5.f, 5.f};
    static const __m128 eight = { 8.f, 8.f, 8.f, 8.f};

    t = _mm_abs_ps(t);
    __m128 t2 = _mm_mul_ps(t, t);

    /* Compute 1 < t < 2 case:
     * 0.5f*(t*(-t2 + 5.f*t - 8.f) + 4.f)
     * half*(t*(mt2 + t5 - eight) + four)
     * half*(t*(mt2 + t5_sub_8) + four)
     * half*(t*(mt2_add_t5_sub_8) + four) */
    __m128 t5 = _mm_mul_ps(five, t);
    __m128 t5_sub_8 = _mm_sub_ps(t5, eight);
    __m128 zero = _mm_setzero_ps();
    __m128 mt2 = _mm_sub_ps(zero, t2);
    __m128 mt2_add_t5_sub_8 = _mm_add_ps(mt2, t5_sub_8);
    __m128 a = _mm_mul_ps(t, mt2_add_t5_sub_8);
    __m128 b = _mm_add_ps(a, four);
    __m128 r12 = _mm_mul_ps(b, half);

    /* Compute case < 1
     * 0.5f*(t*(3.f*t2 - 5.f*t) + 2.f) */
    __m128 t23 = _mm_mul_ps(three, t2);
    __m128 c = _mm_sub_ps(t23, t5);
    __m128 d = _mm_mul_ps(t, c);
    __m128 e = _mm_add_ps(d, two);
    __m128 r01 = _mm_mul_ps(half, e);

    // Compute masks fr keeping correct components
    __m128 mask01 = _mm_cmple_ps(t, one);
    __m128 mask12 = _mm_cmpgt_ps(t, one);
    r01 = _mm_and_ps(mask01, r01);
    r12 = _mm_and_ps(mask12, r12);


    return _mm_or_ps(r01, r12);
}

/* --------------------------------------------------------------------------
 * Lanczos interpolation
 * ------------------------------------------------------------------------*/

#define DT_LANCZOS_EPSILON (1e-9f)

#if 0
// Canonic version left here for reference
static inline float
lanczos(float width, float t)
{
  float r;

  if (t<-width || t>width) {
    r = 0.f;
  } else if (t>-DT_LANCZOS_EPSILON && t<DT_LANCZOS_EPSILON) {
    r = 1.f;
  } else {
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

// Valid for [-pi pi] only
static inline float
sinf_fast(float t)
{
    static const float a = 4/(M_PI*M_PI);
    static const float p = 0.225f;

    t = a*t*(M_PI - fabsf(t));

    return t*(p*(fabsf(t) - 1) + 1);
}

static inline __m128
sinf_fast_sse(__m128 t)
{
    static const __m128 a = {4.f/(M_PI*M_PI), 4.f/(M_PI*M_PI), 4.f/(M_PI*M_PI), 4.f/(M_PI*M_PI)};
    static const __m128 p = {0.225f, 0.225f, 0.225f, 0.225f};
    static const __m128 pi = {M_PI, M_PI, M_PI, M_PI};

    // m4 = a*t*(M_PI - fabsf(t));
    __m128 m1 = _mm_abs_ps(t);
    __m128 m2 = _mm_sub_ps(pi, m1);
    __m128 m3 = _mm_mul_ps(t, m2);
    __m128 m4 = _mm_mul_ps(a, m3);

    // p*(m4*fabsf(m4) - m4) + m4;
    __m128 n1 = _mm_abs_ps(m4);
    __m128 n2 = _mm_mul_ps(m4, n1);
    __m128 n3 = _mm_sub_ps(n2, m4);
    __m128 n4 = _mm_mul_ps(p, n3);

    return _mm_add_ps(n4, m4);
}


static inline float
lanczos(float width, float t)
{
  /* Compute a value for sinf(pi.t) in [-pi pi] for which the value will be
   * correct */
  int a = (int)t;
  float r = t - (float)a;

  // Compute the correct sign for sinf(pi.r)
  union { float f; uint32_t i; } sign;
  sign.i = ((a&1)<<31) | 0x3f800000;

  return (DT_LANCZOS_EPSILON + width*sign.f*sinf_fast(M_PI*r)*sinf_fast(M_PI*t/width))/(DT_LANCZOS_EPSILON + M_PI*M_PI*t*t);
}

static inline __m128
lanczos_sse(__m128 width, __m128 t)
{
    /* Compute a value for sinf(pi.t) in [-pi pi] for which the value will be
     * correct */
    __m128i a = _mm_cvtps_epi32(t);
    __m128 r = _mm_sub_ps(t, _mm_cvtepi32_ps(a));

    // Compute the correct sign for sinf(pi.r)
    static const uint32_t fone[] = { 0x3f800000, 0x3f800000, 0x3f800000, 0x3f800000};
    static const uint32_t ione[] = { 1, 1, 1, 1};
    static const __m128 eps = {DT_LANCZOS_EPSILON, DT_LANCZOS_EPSILON, DT_LANCZOS_EPSILON, DT_LANCZOS_EPSILON};
    static const __m128 pi = {M_PI, M_PI, M_PI, M_PI};
    static const __m128 pi2 = {M_PI*M_PI, M_PI*M_PI, M_PI*M_PI, M_PI*M_PI};

    __m128i isign = _mm_and_si128(*(__m128i*)ione, a);
    isign = _mm_slli_epi64(isign, 31);
    isign = _mm_or_si128(*(__m128i*)fone, isign);
    __m128 fsign = _mm_castsi128_ps(isign);

    __m128 num = _mm_mul_ps(width, fsign);
    num = _mm_mul_ps(num, sinf_fast_sse(_mm_mul_ps(pi, r)));
    num = _mm_mul_ps(num, sinf_fast_sse(_mm_div_ps(_mm_mul_ps(pi, t), width)));
    num = _mm_add_ps(eps, num);

    __m128 den = _mm_mul_ps(pi2, _mm_mul_ps(t, t));
    den = _mm_add_ps(eps, den);

    return _mm_div_ps(num, den);
}

#undef DT_LANCZOS_EPSILON

/* --------------------------------------------------------------------------
 * All our known interpolators
 * ------------------------------------------------------------------------*/

static const struct dt_interpolation dt_interpolator[] =
{
  {
    .id = DT_INTERPOLATION_BILINEAR,
    .name = "bilinear",
    .width = 1,
    .func = &bilinear,
    .funcsse = &bilinear_sse
  },
  {
    .id = DT_INTERPOLATION_BICUBIC,
    .name = "bicubic",
    .width = 2,
    .func = &bicubic,
    .funcsse = &bicubic_sse
  },
  {
    .id = DT_INTERPOLATION_LANCZOS2,
    .name = "lanczos2",
    .width = 2,
    .func = &lanczos,
    .funcsse = &lanczos_sse
  },
  {
    .id = DT_INTERPOLATION_LANCZOS3,
    .name = "lanczos3",
    .width = 3,
    .func = &lanczos,
    .funcsse = &lanczos_sse
  },
};

/* --------------------------------------------------------------------------
 * Kernel utility method
 * ------------------------------------------------------------------------*/

static inline float
compute_kernel(
  const struct dt_interpolation* itor,
  float* kernel,
  float t)
{
  /* Find closest integer position and then offset that to match first
   * filtered sample position */
  t = t - (float)((int)t) + (float)itor->width - 1.f;

  // Will hold kernel norm
  float norm = 0.f;

  // Compute the raw kernel
  for (int i=0; i<2*itor->width; i++) {
    float tap = itor->func((float)itor->width, t);
    norm += tap;
    kernel[i] = tap;
    t -= 1.f;
  }

  return norm;
}

static inline float
compute_kernel_sse(
  const struct dt_interpolation* itor,
  float* kernel,
  float t)
{
  /* Find closest integer position and then offset that to match first
   * filtered sample position */
  t = t - (float)((int)t) + (float)itor->width - 1.f;

  // Prepare t vector to compute four values a loop
  static const __m128 first = {  0.f, -1.f, -2.f, -3.f};
  static const __m128 iter  = { -4.f, -4.f, -4.f, -4.f};
  __m128 vt = _mm_add_ps(_mm_set_ps1(t), first);
  __m128 vw = _mm_set_ps1((float)itor->width);

  // Prepare counters (math kept stupid for understanding)
  int i = 0;
  int runs = (2*itor->width + 3)/4;

  while (i<runs) {
    // Compute the values
    __m128 vr = itor->funcsse(vw, vt);

    // Accum norm
    *(__m128*)kernel = vr;

    // Prepare next iter
    vt = _mm_add_ps(vt, iter);
    kernel += 4;
    i++;
  }

  // compute norm now
  float norm = 0.f;
  i = 0;
  kernel -= 4*runs;
  while (i<2*itor->width) {
    norm += *kernel;
    kernel++;
    i++;
  }

  return norm;
}

/* --------------------------------------------------------------------------
 * Sample interpolation function (see usage in iop/lens.c and iop/clipping.c)
 * ------------------------------------------------------------------------*/

float
dt_interpolation_compute_sample(
  const struct dt_interpolation* itor,
  const float* in,
  const float x, const float y,
  const int samplestride, const int linestride)
{
  assert(itor->width < 4);

  float kernelh[8] __attribute__((aligned(16)));
  float kernelv[8] __attribute__((aligned(16)));

  // Compute both horizontal and vertical kernels
  float normh = compute_kernel_sse(itor, kernelh, x);
  float normv = compute_kernel_sse(itor, kernelv, y);

  // Go to top left pixel
  in = in - (itor->width-1)*(samplestride + linestride);

  // Apply the kernel
  float s = 0.f;
  for (int i=0; i<2*itor->width; i++) {
    float h = 0.0f;
    for (int j=0; j<2*itor->width; j++) {
      h += kernelh[j]*in[j*samplestride];
    }
    s += kernelv[i]*h;
    in += linestride;
  }
  return  s/(normh*normv);
}

/* --------------------------------------------------------------------------
 * Pixel interpolation function (see usage in iop/lens.c and iop/clipping.c)
 * ------------------------------------------------------------------------*/

void
dt_interpolation_compute_pixel4c(
  const struct dt_interpolation* itor,
  const float* in,
  const float* out,
  const float x, const float y,
  const int linestride)
{
  assert(itor->width < 4);
  assert(samplestride == 4);

  // Quite a bit of space for kernels
  float kernelh[8] __attribute__((aligned(16)));
  float kernelv[8] __attribute__((aligned(16)));
  __m128 vkernelh[8];
  __m128 vkernelv[8];

  // Compute both horizontal and vertical kernels
  float normh = compute_kernel_sse(itor, kernelh, x);
  float normv = compute_kernel_sse(itor, kernelv, y);

  // We will process four components a time, duplicate the information
  for (int i=0; i<2*itor->width; i++) {
    vkernelh[i] = _mm_set_ps1(kernelh[i]);
    vkernelv[i] = _mm_set_ps1(kernelv[i]);
  }

  // Precompute the inverse of the filter norm for later use
  __m128 oonorm = _mm_set_ps1(1.f/(normh*normv));

  // Go to top left pixel
  in = in - (itor->width-1)*(4 + linestride);

  // Apply the kernel
  __m128 pixel = _mm_setzero_ps();
  for (int i=0; i<2*itor->width; i++) {
    __m128 h = _mm_setzero_ps();
    for (int j=0; j<2*itor->width; j++) {
      h = _mm_add_ps(h, _mm_mul_ps(vkernelh[j], *(__m128*)&in[j*4]));
    }
    pixel = _mm_add_ps(pixel, _mm_mul_ps(vkernelv[i],h));
    in += linestride;
  }

  *(__m128*)out = _mm_mul_ps(pixel, oonorm);
}

/* --------------------------------------------------------------------------
 * Interpolation factory
 * ------------------------------------------------------------------------*/

const struct dt_interpolation*
dt_interpolation_new(
  enum dt_interpolation_type type)
{
  const struct dt_interpolation* itor = NULL;

  if (type == DT_INTERPOLATION_USERPREF) {
    // Find user preferred interpolation method
    gchar* uipref = dt_conf_get_string("plugins/lighttable/export/pixel_interpolator");
    for (int i=DT_INTERPOLATION_FIRST; uipref && i<DT_INTERPOLATION_LAST; i++) {
      if (!strcmp(uipref, dt_interpolator[i].name)) {
        // Found the one
        itor = &dt_interpolator[i];
        break;
      }
    }
    g_free(uipref);

    /* In the case the search failed (!uipref or name not found),
     * prepare later search pass with default fallback */
    type = DT_INTERPOLATION_DEFAULT;
  }
  if (!itor) {
    // Did not find the userpref one or we've been asked for a specific one
    for (int i=DT_INTERPOLATION_FIRST; i<DT_INTERPOLATION_LAST; i++) {
      if (dt_interpolator[i].id == type) {
        itor = &dt_interpolator[i];
        break;
      }
      if (dt_interpolator[i].id == DT_INTERPOLATION_DEFAULT) {
        itor = &dt_interpolator[i];
      }
    }
  }

  return itor;
}
