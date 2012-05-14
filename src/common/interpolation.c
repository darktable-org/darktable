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

#define SSE_ALIGNMENT 16

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

/** Computes an upsampling filtering kernel
 *
 * @param itor [in] Interpolator used
 * @param kernel [out] resulting itor->width*2 filter taps
 * @param first [out] first input sample index used
 * @param t [in] Interpolated coordinate
 *
 * @return kernel norm
 */
static inline float
compute_upsampling_kernel(
  const struct dt_interpolation* itor,
  float* kernel,
  int* first,
  float t)
{
  int f = (int)t - itor->width + 1;
  if (first) {
    *first = f;
  }

  /* Find closest integer position and then offset that to match first
   * filtered sample position */
  t = t - (float)f;

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

/** Computes an upsampling filtering kernel (SSE version, four taps per inner loop)
 *
 * @param itor [in] Interpolator used
 * @param kernel [out] resulting itor->width*2 filter taps (array must be at least (itor->width*2+3)/4*4 floats long)
 * @param first [out] first input sample index used
 * @param t [in] Interpolated coordinate
 *
 * @return kernel norm
 */
static inline float
compute_upsampling_kernel_sse(
  const struct dt_interpolation* itor,
  float* kernel,
  int* first,
  float t)
{
  int f = (int)t - itor->width + 1;
  if (first) {
    *first = f;
  }

  /* Find closest integer position and then offset that to match first
   * filtered sample position */
  t = t - (float)f;

  // Prepare t vector to compute four values a loop
  static const __m128 bootstrap = {  0.f, -1.f, -2.f, -3.f};
  static const __m128 iter  = { -4.f, -4.f, -4.f, -4.f};
  __m128 vt = _mm_add_ps(_mm_set_ps1(t), bootstrap);
  __m128 vw = _mm_set_ps1((float)itor->width);

  // Prepare counters (math kept stupid for understanding)
  int i = 0;
  int runs = (2*itor->width + 3)/4;

  while (i<runs) {
    // Compute the values
    __m128 vr = itor->funcsse(vw, vt);

    // Save result
    *(__m128*)kernel = vr;

    // Prepare next iteration
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

// Avoid libc ceil for now. Maybe we'll revert to libc later
static inline float
ceil_fast(
  float x)
{
  if (x <= 0.f) {
    return (float)(int)x;
  } else {
    return -((float)(int)-x) + 1.f;
  }
}

/** Computes a downsampling filtering kernel
 *
 * @param itor [in] Interpolator used
 * @param kernelsize [out] Number of taps
 * @param kernel [out] resulting taps (at least itor->width/inoout elements for no overflow)
 * @param first [out] index of the first sample for which the kernel is to be applied
 * @param outoinratio [in] "out samples" over "in samples" ratio
 * @param xout [in] Output coordinate
 *
 * @return kernel norm
 */
static inline float
compute_downsampling_kernel(
  const struct dt_interpolation* itor,
  int* taps,
  int* first,
  float* kernel,
  float outoinratio,
  int xout)
{
  // Keep this at hand
  float w = (float)itor->width;

  /* Compute the phase difference between output pixel and its
   * input corresponding input pixel */
  float xin = ceil_fast(((float)xout-w)/outoinratio);
  if (first) {
    *first = (int)xin;
  }

  // Compute first interpolator parameter
  float t = xin*outoinratio - (float)xout;

  // Will hold kernel norm
  float norm = 0.f;

  // Compute all filter taps
  *taps = (int)((w-t)/outoinratio);
  for (int i=0; i<*taps; i++) {
    *kernel = itor->func(w, t);
    norm += *kernel;
    t += outoinratio;
    kernel++;
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

  float kernelh[8] __attribute__((aligned(SSE_ALIGNMENT)));
  float kernelv[8] __attribute__((aligned(SSE_ALIGNMENT)));

  // Compute both horizontal and vertical kernels
  float normh = compute_upsampling_kernel_sse(itor, kernelh, NULL, x);
  float normv = compute_upsampling_kernel_sse(itor, kernelv, NULL, y);

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

  // Quite a bit of space for kernels
  float kernelh[8] __attribute__((aligned(SSE_ALIGNMENT)));
  float kernelv[8] __attribute__((aligned(SSE_ALIGNMENT)));
  __m128 vkernelh[8];
  __m128 vkernelv[8];

  // Compute both horizontal and vertical kernels
  float normh = compute_upsampling_kernel_sse(itor, kernelh, NULL, x);
  float normv = compute_upsampling_kernel_sse(itor, kernelv, NULL, y);

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

/* --------------------------------------------------------------------------
 * Image resampling
 * ------------------------------------------------------------------------*/

/** Clip index into range
 * @param idx index to filter
 * @param length length of line
 */
static inline int
clip_index(
  int idx,
  int min,
  int max)
{
  if (idx < min) {
    idx = min;
  } else if (idx > max) {
    idx = max;
  }
  return idx;
}

/** Make sure length will keep alignment is base pointer is aligned too
 *
 * @param l Length required
 * @param align Required alignment for next chained chunk
 *
 * @return Required length for keeping alignment ok if chaining data chunks
 */
static inline size_t
increase_for_alignment(
  size_t l,
  size_t align)
{
  align -= 1;
  return (l + align) & (~align);
}

/** Prepares the resampling plan
 *
 * This consists of the following informations
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
 * of taps/indexes to use). This array mus be freed with fre() when you're
 * done with the plan.
 * @param pkernel [out] Array of filter kernel taps
 * @param pindex [out] Array of sample indexes to be used for applying each kernel tap
 * arrays of informations
 * @return 0 for success, !0 for failure
 */
static int
prepare_resampling_plan(
  const struct dt_interpolation* itor,
  int in,
  const int in_x0,
  int out,
  const int out_x0,
  float scale,
  int** plength,
  float** pkernel,
  int** pindex)
{
  // Safe return values
  *plength = NULL;
  *pkernel = NULL;
  *pindex = NULL;

  if (scale == 1.f) {
    // No resampling required
    return 0;
  }

  // Compute common upsampling/downsampling memory requirements
  int nlengths = out;
  size_t lengthreq = increase_for_alignment(nlengths*sizeof(int), SSE_ALIGNMENT);

  // Left these as they depend on sampling case
  int nkernel;
  int nindex;
  size_t kernelreq = 0;
  size_t indexreq = 0;

  if (scale > 1.f) {
    // Upscale... the easy one. The values are exact
    nindex = 2*itor->width*out;
    nkernel = 2*itor->width*out;
    indexreq = increase_for_alignment(nindex*sizeof(int), SSE_ALIGNMENT);
    kernelreq = increase_for_alignment(nkernel*sizeof(float), SSE_ALIGNMENT);
  } else {
    // Downscale... going for worst case values memory wise
    nindex = 2*itor->width*(int)(ceil_fast((float)out/scale));
    nkernel = 2*itor->width*(int)(ceil_fast((float)out/scale));
    indexreq = increase_for_alignment(nindex*sizeof(int), SSE_ALIGNMENT);
    kernelreq = increase_for_alignment(nkernel*sizeof(float), SSE_ALIGNMENT);
  }

  void *blob = NULL;
  posix_memalign(&blob, SSE_ALIGNMENT, kernelreq + lengthreq + indexreq);
  if (!blob) {
    return 1;
  }

  int* lengths = blob;
  int* index = (int*)((char*)lengths + lengthreq);
  float* kernel = (float*)((char*)index + indexreq);
  if (scale > 1.f) {
    int kidx = 0;
    int iidx = 0;
    int lidx = 0;
    for (int x=0; x<out; x++) {
      // For upsampling the number of taps is always the width of the filter
      lengths[lidx] = 2*itor->width;
      lidx++;

      // Projected position in input samples
      float fx = (float)(out_x0 + x)*scale;

      // Compute the filter kernel at that position
      int first;
      float norm = compute_upsampling_kernel(itor, &kernel[kidx], &first, fx);

      // Precompute the inverse of the norm
      norm = 1.f/norm;

      /* Unlike single pixel or single sample code, here it's interesting to
       * precompute the normalized filter kernel as this will avoid dividing
       * by the norm for all processed samples/pixels
       * NB: use the same loop to put in place the index list */
      for (int tap=0; tap<2*itor->width; tap++) {
        kernel[kidx++] *= norm;
        index[iidx++] = clip_index(first++, 0, in-1);
      }
    }
  } else {
    int kidx = 0;
    int iidx = 0;
    int lidx = 0;
    for (int x=0; x<out; x++) {
      // Compute downsampling kernel centered on output position
      int taps;
      int first;
      float norm = compute_downsampling_kernel(itor, &taps, &first, &kernel[kidx], scale, out_x0 + x);

      // Now we know how many samples will be used for this output pixel
      lengths[lidx] = taps;
      lidx++;

      // Precompute inverse of the norm
      norm = 1.f/norm;

      // Precomputed normalized filter kernel and index list
      for (int tap=0; tap<taps; tap++) {
        kernel[kidx++] *= norm;
        index[iidx++] = clip_index(first++, 0, in-1);
      }
    }
  }

  // Validate plan wrt caller
  *plength = lengths;
  *pindex = index;
  *pkernel = kernel;

  return 0;
}

#if 0
#define debug_info(...) do { fprintf(stderr, __VA_ARGS__); } while (0)
#else
#define debug_info(...)
#endif

#if 0
#define debug_extra(...) do { fprintf(stderr, __VA_ARGS__); } while (0)
#else
#define debug_extra(...)
#endif


void
dt_interpolation_resample(
  const struct dt_interpolation* itor,
  float *out,
  const dt_iop_roi_t* const roi_out,
  const int32_t out_stride,
  const float* const in,
  const dt_iop_roi_t* const roi_in,
  const int32_t in_stride)
{
  int* hindex = NULL;
  int* hlength = NULL;
  float* hkernel = NULL;
  int* vindex = NULL;
  int* vlength = NULL;
  float* vkernel = NULL;

  int r;

  debug_info(
    "resampling %p (%dx%d@%dx%d scale %f) -> %p (%dx%d@%dx%d scale %f)\n",
    in,
    roi_in->width, roi_in->height, roi_in->x, roi_in->y, roi_in->scale,
    out,
    roi_out->width, roi_out->height, roi_out->x, roi_out->y, roi_out->scale);

  // Fast code path for 1:1 copy, only cropping area can change
  if (roi_out->scale == 1.f) {
    const int x0 = roi_out->x*4*sizeof(float);
    const int l = roi_out->width*4*sizeof(float);
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(out)
#endif
    for (int y=0; y<roi_out->height; y++) {
      float* i = (float*)((char*)in + in_stride*(y + roi_out->y) + x0);
      float* o = (float*)((char*)out + out_stride*y);
      memcpy(o, i, l);
    }

    // All done, so easy case
    return;
  }

  // Generic non 1:1 case... much more complicated :D

  // Prepare resampling plans once and for all
  r = prepare_resampling_plan(itor, roi_in->width, roi_in->x, roi_out->width, roi_out->x, roi_out->scale, &hlength, &hkernel, &hindex);
  if (r) {
    goto exit;
  }

  r = prepare_resampling_plan(itor, roi_in->height, roi_in->y, roi_out->height, roi_out->y, roi_out->scale, &vlength, &vkernel, &vindex);
  if (r) {
    goto exit;
  }

  // Initialize column resampling indexes
  int vlidx = 0; // V(ertical) L(ength) I(n)d(e)x
  int vkidx = 0; // V(ertical) K(ernel) I(n)d(e)x
  int viidx = 0; // V(ertical) I(ndex) I(n)d(e)x

  /* XXX: add a touch of OpenMP here, make sure the spanwed job do use
   * correct indexes in the resampling plans (probably needs indexing
   * kernels and index array with line instead of linear progress of
   * individual pixel lengths); Do this later, validate first the
   * correct working of the resampling plans*/
  // Process each output line
  for (int oy=0; oy<roi_out->height; oy++) {
    // Initialize row resampling indexes
    int hlidx = 0; // H(orizontal) L(ength) I(n)d(e)x
    int hkidx = 0; // H(orizontal) K(ernel) I(n)d(e)x
    int hiidx = 0; // H(orizontal) I(ndex) I(n)d(e)x

    // Number of lines contributing to the output line
    int vl = vlength[vlidx++]; // V(ertical) L(ength)

    // Process each output column
    for (int ox=0; ox < roi_out->width; ox++) {
      debug_extra("output %p [% 4d % 4d]\n", out, ox, oy);

      // This will hold the resulting pixel
      float s[3] = {0.f, 0.f, 0.f};

      // Number of horizontal samples contributing to the output
      int hl = hlength[hlidx++]; // H(orizontal) L(ength)

      for (int iy=0; iy < vl; iy++) {
        // This is our input line
        const float* i = (float*)((char*)in + in_stride*vindex[viidx++]);

        float hs[3] = {0.f, 0.f, 0.f};

        for (int ix=0; ix< hl; ix++) {
          // Apply the precomputed filter kernel
          int baseidx = hindex[hiidx++]*4;
          float htap = hkernel[hkidx++];
          hs[0] += i[baseidx + 0]*htap;
          hs[1] += i[baseidx + 1]*htap;
          hs[2] += i[baseidx + 2]*htap;
        }

        // Accumulate contribution from this line
        float vtap = vkernel[vkidx++];
        s[0] += hs[0]*vtap;
        s[1] += hs[1]*vtap;
        s[2] += hs[2]*vtap;

        // Reset horizontal resampling context
        hkidx -= hl;
        hiidx -= hl;
      }

      // Output pixel is ready
      float* o = (float*)((char*)out + oy*out_stride + ox*4*sizeof(float));
      o[0] = s[0];
      o[1] = s[1];
      o[2] = s[2];

      // Reset vertical resampling context
      viidx -= vl;
      vkidx -= vl;

      // Progress in horizontal context
      hiidx += hl;
      hkidx += hl;
    }

    // Progress in vertical context
    viidx += vl;
    vkidx += vl;
  }

exit:
  free(hlength);
  free(vlength);
}
