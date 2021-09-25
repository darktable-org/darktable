/*
    This file is part of darktable,
    Copyright (C) 2019-2021 darktable developers.

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


/*
  eucledian distance transform for darktable Hanno Schwalm (hanno@schwalm-bremen.de) 2021/09  
   - adopted to C
   - omp support
   - reduced alloc/free using dt_alloc_align variants for better debug support 
   - tuned for performance in collaboration with Ingo Weyrich (heckflosse67@gmx.de) from rawtherapee
  
  The original code is from:

  *** Original copyright note ***
  Implementation of the distance transform algorithm described in:

  Distance Transforms of Sampled Functions
  Pedro F. Felzenszwalb and Daniel P. Huttenlocher
  Cornell Computing and Information Science TR2004-1963 
  Copyright (C) 2006 Pedro Felzenszwalb

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.
*/

/* Howto
  dt_image_distance_transform(float *const restrict src, float *const restrict out, const size_t width, const size_t height, const float clip, const dt_distance_transform_t mode)
    writes data to an 1-ch image at 'out' with dimensions given. 'out' must be aligned as by dt_alloc_align_float.
    You may either
      - prepare the 'out' image before calling the distance transform, in this case use DT_DISTANCE_TRANSFORM_NONE as mode.
        you should have filled 'out' with either 0.0f or DT_DISTANCE_TRANSFORM_MAX marking the positions as on/off
      - use DT_DISTANCE_TRANSFORM_MASK, in this case data found in src is checked vs clip, dt_image_distance_transform
        will fill in the zeros / DT_DISTANCE_TRANSFORM_MAX
*/


#include "develop/openmp_maths.h"

typedef enum dt_distance_transform_t
{
  DT_DISTANCE_TRANSFORM_NONE = 0,
  DT_DISTANCE_TRANSFORM_MASK = 1
} dt_distance_transform_t;

#define DT_DISTANCE_TRANSFORM_MAX (1e20)

static void dt(float *f, float *z, float *d, int *v, int n)
{
  int k = 0;
  v[0] = 0;
  z[0] = -DT_DISTANCE_TRANSFORM_MAX;
  z[1] = DT_DISTANCE_TRANSFORM_MAX;
  for(int q = 1; q <= n-1; q++)
  {
    float s  = ((f[q] + sqf((float)q)) - (f[v[k]] + sqf((float)v[k]))) / (float)(2*q - 2*v[k]);
    while(s <= z[k])
    {
      k--;
      s = ((f[q] + sqf((float)q)) - (f[v[k]] + sqf((float)v[k]))) / (float)(2*q - 2*v[k]);
    }
    k++;
    v[k] = q;
    z[k] = s;
    z[k+1] = DT_DISTANCE_TRANSFORM_MAX;
  }

  k = 0;
  for(int q = 0; q <= n-1; q++)
  {
    while(z[k+1] < (float)q)
      k++;
    d[q] = sqf((float)(q-v[k])) + f[v[k]];
  }
}

void dt_image_distance_transform(float *const restrict src, float *const restrict out, const size_t width, const size_t height, const float clip, const dt_distance_transform_t mode)
{
  switch(mode)
  {
    case DT_DISTANCE_TRANSFORM_NONE:
      break;
    case DT_DISTANCE_TRANSFORM_MASK:
#ifdef _OPENMP
  #pragma omp parallel for simd default(none) \
  dt_omp_firstprivate(src, out) \
  dt_omp_sharedconst(clip, width, height) \
  schedule(static) aligned(src, out : 64)
#endif
      for(size_t i = 0; i < width * height; i++)
        out[i] = (src[i] < clip) ? 0.0f : DT_DISTANCE_TRANSFORM_MAX;
      break;
    default:
      dt_iop_image_fill(out, 0.0f, width, height, 1);
      fprintf(stderr,"[dt_image_distance_transform] called with unsupported mode %i\n", mode);
      return;
  }

  const size_t maxdim = MAX(width, height);

#ifdef _OPENMP
  #pragma omp parallel \
  dt_omp_firstprivate(out) \
  dt_omp_sharedconst(maxdim, width, height)
#endif
  {
    float *f = dt_alloc_align_float(maxdim);
    float *z = dt_alloc_align_float(maxdim + 1); 
    float *d = dt_alloc_align_float(maxdim);
    int *v = dt_alloc_align(64, maxdim * sizeof (int));

    // transform along columns
#ifdef _OPENMP
  #pragma omp for schedule(simd:static)
#endif
    for(size_t x = 0; x < width; x++)
    {
      for(size_t y = 0; y < height; y++)
        f[y] = out[y*width + x];
      dt(f, z, d, v, height);
      for(size_t y = 0; y < height; y++)
        out[y*width + x] = d[y];
    }
    // implicit barrier :-)
    // transform along rows
#ifdef _OPENMP
  #pragma omp for schedule(simd:static) nowait
#endif
    for(size_t y = 0; y < height; y++)
    {
      for(size_t x = 0; x < width; x++)
        f[x] = out[y*width + x];
      dt(f, z, d, v, width);
      for(size_t x = 0; x < width; x++)
        out[y*width + x] = sqrtf(d[x]);
    }
    dt_free_align(f);
    dt_free_align(d);
    dt_free_align(z);
    dt_free_align(v);
  }
}

