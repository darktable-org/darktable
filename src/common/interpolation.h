/*
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
*/

#ifndef INTERPOLATION_H
#define INTERPOLATION_H

#include <assert.h>

#define LANCZOS_EPSILON 0.0000001f

static inline float
dt_lanczos(float width, float t)
{
  float r;

  if (t<-width || t>width) {
    r = 0.f;
  } else if (t>-LANCZOS_EPSILON && t<LANCZOS_EPSILON) {
    r = 1.f;
  } else {
    r = width*sinf(M_PI*t)*sinf(M_PI*t/width)/(M_PI*M_PI*t*t);
  }
  return r;
}

static inline float
dt_lanczos_compute_kernel(float* kernel, float width, float t)
{
  t = t - (float)((int)t) + width - 1.f;
  float norm = 0.f;

  // Compute the raw kernel
  for (int i=0; i<2*(int)width; i++) {
    float tap = dt_lanczos(width, t);
    norm += tap;
    kernel[i] = tap;
    t -= 1.f;
  }

  return norm;
}

static inline float
dt_lanczos_apply(const float* in, const float x, const float y, const int width, const int samplestride, const int linestride)
{
  float kernelh[6];
  float kernelv[6];

  assert(width < 4);

  // Compute both horizontal and vertical kernels
  float normh = dt_lanczos_compute_kernel(kernelh, (float)width, x);
  float normv = dt_lanczos_compute_kernel(kernelv, (float)width, y);

  // Go to top left pixel
  in = in - (width-1)*(samplestride + linestride);

  // Apply the kernel
  float s = 0.f;
  for (int i=0; i<2*width; i++) {
    float h = 0.0f;
    for (int j=0; j<2*width; j++) {
      h += kernelh[j]*in[j*samplestride];
    }
    s += kernelv[i]*h;
    in += linestride;
  }
  return  s/(normh*normv);
}

#endif /* INTERPOLATION_H */

