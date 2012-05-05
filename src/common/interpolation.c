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
 * Functions
 * ------------------------------------------------------------------------*/

static inline float
_dt_interpolation_func_bilinear(float width, float t)
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

static inline float
_dt_interpolation_func_bicubic(float width, float t)
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

#define DT_LANCZOS_EPSILON (1e-9f)

#if 0
// Canonic version
static inline float
_dt_interpolation_func_lanczos(float width, float t)
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
#else
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
_dt_sinf_fast(float t)
{
    static const float a = 4/(M_PI*M_PI);
    static const float p = 0.225f;

    t = a*t*(M_PI - fabsf(t));

    return t*(p*(fabsf(t) - 1) + 1);
}

static inline float
_dt_interpolation_func_lanczos(float width, float t)
{
  /* Compute a value for sinf(pi.t) in [-pi pi] for which the value will be
   * correct */
  int a = (int)t;
  float r = t - (float)a;

  // Compute the correct sign for sinf(pi.r)
  union { float f; uint32_t i; } sign;
  sign.i = ((a&1)<<31) | 0x3f800000;

  return (DT_LANCZOS_EPSILON + width*sign.f*_dt_sinf_fast(M_PI*r)*_dt_sinf_fast(M_PI*t/width))/(DT_LANCZOS_EPSILON + M_PI*M_PI*t*t);
}

#endif

#undef DT_LANCZOS_EPSILON

/* --------------------------------------------------------------------------
 * Interpolators
 * ------------------------------------------------------------------------*/

static const struct dt_interpolation dt_interpolator[] =
{
    {DT_INTERPOLATION_BILINEAR, "bilinear", 1, &_dt_interpolation_func_bilinear},
    {DT_INTERPOLATION_BICUBIC,  "bicubic",  2, &_dt_interpolation_func_bicubic},
    {DT_INTERPOLATION_LANCZOS2, "lanczos2", 2, &_dt_interpolation_func_lanczos},
    {DT_INTERPOLATION_LANCZOS3, "lanczos3", 3, &_dt_interpolation_func_lanczos},
};

/* --------------------------------------------------------------------------
 * Kernel utility method
 * ------------------------------------------------------------------------*/

static inline float
_dt_interpolation_compute_kernel(
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

/* --------------------------------------------------------------------------
 * Interpolation function (see usage in iop/lens.c and iop/clipping.c)
 * ------------------------------------------------------------------------*/

float
dt_interpolation_compute_sample(
  const struct dt_interpolation* itor,
  const float* in,
  const float x, const float y,
  const int samplestride, const int linestride)
{
  assert(itor->width < 4);

  float kernelh[6];
  float kernelv[6];

  // Compute both horizontal and vertical kernels
  float normh = _dt_interpolation_compute_kernel(itor, kernelh, x);
  float normv = _dt_interpolation_compute_kernel(itor, kernelv, y);

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
