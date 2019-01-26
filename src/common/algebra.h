/*
    This file is part of darktable,
    copyright (c) 2009--2010 johannes hanika.
    copyright (c) 2011--2017 tobias ellinghaus.
    copyright (c) 2019 Aurélien Pierre.

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

#pragma once

#include <math.h>

#ifdef __SSE2__
#include "common/sse.h"
#include <xmmintrin.h>
#endif

/**
 * This file contains matrices and vector operations
 **/

// invert 3×3 matrices
int mat3inv_float(float *const dst, const float *const src);
int mat3inv_double(double *const dst, const double *const src);
int mat3inv(float *const dst, const float *const src);

static inline void mat3mul(float *dst, const float *const m1, const float *const m2)
{
  for(int k = 0; k < 3; k++)
  {
    for(int i = 0; i < 3; i++)
    {
      float x = 0.0f;
      for(int j = 0; j < 3; j++) x += m1[3 * k + j] * m2[3 * j + i];
      dst[3 * k + i] = x;
    }
  }
}

// multiply 3x3 matrix with 3x1 vector
// dst needs to be different from v
static inline void mat3mulv(float *dst, const float *const mat, const float *const v)
{
  for(int k = 0; k < 3; k++)
  {
    float x = 0.0f;
    for(int i = 0; i < 3; i++) x += mat[3 * k + i] * v[i];
    dst[k] = x;
  }
}

#ifdef __SSE2__
// Transpose a 3×3 matrice and put the columns into 3 SSE vectors
static inline void mat3_transpose_sse2(const float mat[3][3], __m128 *const dst)
{
  for(int i = 0; i < 3; i++) dst[i] = _mm_setr_ps(mat[0][i], mat[1][i], mat[2][i], 0.0f);
}

static inline void mat3mulv_sse2(__m128 *dst, const float mat[3][3], const __m128 *const v)
{
  __m128 col_vect[3];
  mat3_transpose_sse2(mat, col_vect);

  *dst = col_vect[0] * _mm_shuffle_ps(*v, *v, _MM_SHUFFLE(0, 0, 0, 0)) +
         col_vect[1] * _mm_shuffle_ps(*v, *v, _MM_SHUFFLE(1, 1, 1, 1)) +
         col_vect[2] * _mm_shuffle_ps(*v, *v, _MM_SHUFFLE(2, 2, 2, 2));
}
#endif


// multiply 4x4 matrix with 4x1 vector
// dst needs to be different from v
static inline void mat4mulv(float *dst, float mat[][4], const float *const v)
{
  for(int k = 0; k < 4; k++)
  {
    float x = 0.0f;
    for(int i = 0; i < 4; i++) x += mat[k][i] * v[i];
    dst[k] = x;
  }
}

// normalized product of two 3x1 vectors
// dst needs to be different from v1 and v2
static inline void vec3prodn(float *dst, const float *const v1, const float *const v2)
{
  const float l1 = v1[1] * v2[2] - v1[2] * v2[1];
  const float l2 = v1[2] * v2[0] - v1[0] * v2[2];
  const float l3 = v1[0] * v2[1] - v1[1] * v2[0];

  // normalize so that l1^2 + l2^2 + l3^3 = 1
  const float sq = sqrt(l1 * l1 + l2 * l2 + l3 * l3);

  const float f = sq > 0.0f ? 1.0f / sq : 1.0f;

  dst[0] = l1 * f;
  dst[1] = l2 * f;
  dst[2] = l3 * f;
}

// normalize a 3x1 vector so that x^2 + y^2 + z^2 = 1
// dst and v may be the same
static inline void vec3norm(float *dst, const float *const v)
{
  const float sq = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);

  // special handling for an all-zero vector
  const float f = sq > 0.0f ? 1.0f / sq : 1.0f;

  dst[0] = v[0] * f;
  dst[1] = v[1] * f;
  dst[2] = v[2] * f;
}

// normalize a 3x1 vector so that x^2 + y^2 = 1; a useful normalization for
// lines in homogeneous coordinates
// dst and v may be the same
static inline void vec3lnorm(float *dst, const float *const v)
{
  const float sq = sqrt(v[0] * v[0] + v[1] * v[1]);

  // special handling for a point vector of the image center
  const float f = sq > 0.0f ? 1.0f / sq : 1.0f;

  dst[0] = v[0] * f;
  dst[1] = v[1] * f;
  dst[2] = v[2] * f;
}

// scalar product of two 3x1 vectors
static inline float vec3scalar(const float *const v1, const float *const v2)
{
  return (v1[0] * v2[0] + v1[1] * v2[1] + v1[2] * v2[2]);
}

// check if 3x1 vector is (very close to) null
static inline int vec3isnull(const float *const v)
{
  const float eps = 1e-10f;
  return (fabs(v[0]) < eps && fabs(v[1]) < eps && fabs(v[2]) < eps);
}
