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

#ifndef INTERPOLATION_H
#define INTERPOLATION_H

#include <assert.h>

/* --------------------------------------------------------------------------
 * Types
 * ------------------------------------------------------------------------*/

/** Interpolation function */
typedef float (*dt_interpolation_func)(float width, float t);

/** available interpolations */
enum dt_interpolation
{
  DT_INTERPOLATION_BILINEAR=0,
  DT_INTERPOLATION_LANCZOS2,
  DT_INTERPOLATION_LANCZOS3,
  DT_INTERPOLATOR_MAX
};

/** Interpolation description */
struct dt_interpolation_desc
{
  enum dt_interpolation id;
  const char* name;
  int width;
  dt_interpolation_func func;
};

/* --------------------------------------------------------------------------
 * Functions
 * ------------------------------------------------------------------------*/

static inline float
_dt_interpolation_func_bilinear(float width, float t)
{
  float r;

  if (t>1.f || t<-1.f) {
    r = 0.f;
  } else {
    r = 1.f - fabsf(t);
  }
  return r;
}

#define DT_LANCZOS_EPSILON 0.0000001f

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

/* --------------------------------------------------------------------------
 * Interpolators
 * ------------------------------------------------------------------------*/

static const struct dt_interpolation_desc dt_interpolator[] =
{
    {DT_INTERPOLATION_BILINEAR, "bilinear", 1, &_dt_interpolation_func_bilinear},
    {DT_INTERPOLATION_LANCZOS2, "lanczos2", 2, &_dt_interpolation_func_lanczos},
    {DT_INTERPOLATION_LANCZOS3, "lanczos3", 3, &_dt_interpolation_func_lanczos},
};

/* --------------------------------------------------------------------------
 * Kernel utility method
 * ------------------------------------------------------------------------*/

static inline float
_dt_interpolation_compute_kernel(enum dt_interpolation itype, float* kernel, float t)
{
  const struct dt_interpolation_desc* interpolator = &dt_interpolator[itype];

  /* Find closest integer position and then offset that to match first
   * filtered sample position */
  t = t - (float)((int)t) + (float)interpolator->width - 1.f;

  // Will hold kernel norm
  float norm = 0.f;

  // Compute the raw kernel
  for (int i=0; i<2*interpolator->width; i++) {
    float tap = interpolator->func((float)interpolator->width, t);
    norm += tap;
    kernel[i] = tap;
    t -= 1.f;
  }

  return norm;
}

/* --------------------------------------------------------------------------
 * Interpolation function (see usage in iop/lens.c and iop/clipping.c)
 * ------------------------------------------------------------------------*/

static inline float
dt_interpolation_compute(const float* in, const float x, const float y, enum dt_interpolation itype, const int samplestride, const int linestride)
{
  const struct dt_interpolation_desc* interpolator = &dt_interpolator[itype];
  assert(interpolator->width < 4);

  float kernelh[6];
  float kernelv[6];

  // Compute both horizontal and vertical kernels
  float normh = _dt_interpolation_compute_kernel(itype, kernelh, x);
  float normv = _dt_interpolation_compute_kernel(itype, kernelv, y);

  // Go to top left pixel
  in = in - (interpolator->width-1)*(samplestride + linestride);

  // Apply the kernel
  float s = 0.f;
  for (int i=0; i<2*interpolator->width; i++) {
    float h = 0.0f;
    for (int j=0; j<2*interpolator->width; j++) {
      h += kernelh[j]*in[j*samplestride];
    }
    s += kernelv[i]*h;
    in += linestride;
  }
  return  s/(normh*normv);
}

#endif /* INTERPOLATION_H */

