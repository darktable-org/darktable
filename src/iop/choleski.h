/*
    This file is part of darktable,
    Copyright (C) 2019-2020 darktable developers.

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

#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "common/darktable.h"
#include "common/imagebuf.h"
#include "develop/imageop_math.h"


/* DOCUMENTATION
 *
 * Choleski decomposition is a fast way to solve linear systems of equations
 * described by a positive definite hermitian (= square and symmetrical) matrix.
 * It is a special case of LU decompositions enabling special optimizations.
 * For matrices not matching this requirement,
 * you need to use the Gauss-Jordan elimination in iop/gaussian_elemination.h,
 * which is about twice as slow but more general.
 *
 * To solve A x = y, for x, with A a positive definite hermitian real matrix :
 *
 *  1) find L such that A = L × L' (Choleski decomposition)
 *  2) Second step : solve L × b = y for b (triangular descent)
 *  3) Third step : solve L' × x = b for x (triangular ascent)
 *
 * L is a lower diagonal matrix such that :
 *
 *     [ l11  0    0   ]
 * L = [ l12  l22  0   ] (if n = 3)
 *     [ l13  l23  l33 ]
 *
 * and L' is its transpose :
 *
 *      [ l11 l12 l13 ]
 * L' = [ 0   l22 l23 ] (if n = 3)
 *      [ 0   0   l33 ]
 *
 * We use the Cholesky-Banachiewicz algorithm for the decomposition because it
 * operates row by row, which is more suitable with C memory layout.
 *
 * The return codes are in sync with /iop/gaussian_elimination.h, in order, maybe one day,
 * to have a general linear solving lib that could for example iterate over methods until
 * one succeed.
 *
 * We didn't bother to parallelize the code nor to make it use double floating point precision
 * because it's already fast enough (2 to 45 ms for 16×16 matrix on Xeon) and used for properly
 * conditioned matrices.
 *
 * Vectorization leads to slow-downs here since we access matrices row-wise and column-wise,
 * in a non-contiguous fashion.
 *
 * References :
 *  "Analyse numérique pour ingénieurs", 4e edition, André Fortin,
 *  Presses Internationales de Polytechnique Montréal, 2011.
 *
 *  Cholesky method,
 *  https://algowiki-project.org/en/Cholesky_method#The_.5Bmath.5DLL.5ET.5B.2Fmath.5D_decomposition
 *  https://en.wikipedia.org/wiki/Cholesky_decomposition
 *  https://rosettacode.org/wiki/Cholesky_decomposition#C
 *
 */


__DT_CLONE_TARGETS__
static inline int choleski_decompose_fast(const float *const restrict A,
                                          float *const restrict L, size_t n)
{
  // A is input n×n matrix, decompose it into L such that A = L × L'
  // fast variant : we don't check values for negatives in sqrt,
  // ensure you know the properties of your matrix.

  if(A[0] <= 0.0f) return 0; // failure : non positive definite matrice

  for(size_t i = 0; i < n; i++)
    for(size_t j = 0; j < (i + 1); j++)
    {
      float sum = 0.0f;

      for(size_t k = 0; k < j; k++)
        sum += L[i * n + k] * L[j * n + k];

      L[i * n + j] = (i == j) ?
                        sqrtf(A[i * n + i] - sum) :
                        (A[i * n + j] - sum) / L[j * n + j];
    }

  return 1; // success
}


__DT_CLONE_TARGETS__
static inline int choleski_decompose_safe(const float *const restrict A,
                                          float *const restrict L, size_t n)
{
  // A is input n×n matrix, decompose it into L such that A = L × L'
  // slow and safe variant : we check values for negatives in sqrt and divisions by 0.

  if(A[0] <= 0.0f) return 0; // failure : non positive definite matrice

  int valid = 1;

  for(size_t i = 0; i < n; i++)
    for(size_t j = 0; j < (i + 1); j++)
    {
      float sum = 0.0f;

      for(size_t k = 0; k < j; k++)
        sum += L[i * n + k] * L[j * n + k];

      if(i == j)
      {
        const float temp = A[i * n + i] - sum;

        if(temp < 0.0f)
        {
          valid = 0;
          L[i * n + j] = NAN;
        }
        else
          L[i * n + j] = sqrtf(A[i * n + i] - sum);
      }
      else
      {
        const float temp = L[j * n + j];

        if(temp == 0.0f)
        {
          valid = 0;
          L[i * n + j] = NAN;
        }
        else
          L[i * n + j] = (A[i * n + j] - sum) / temp;
      }
    }

  return valid; // success ?
}


__DT_CLONE_TARGETS__
static inline int triangular_descent_fast(const float *const restrict L,
                                          const float *const restrict y, float *const restrict b,
                                          const size_t n)
{
  // solve L × b = y for b
  // use the lower triangular part of L from top to bottom

  for(size_t i = 0; i < n; ++i)
  {
    float sum = y[i];
    for(size_t j = 0; j < i; ++j)
      sum -= L[i * n + j] * b[j];

    b[i] = sum / L[i * n + i];
  }

  return 1; // success !
}


__DT_CLONE_TARGETS__
static inline int triangular_descent_safe(const float *const restrict L,
                                          const float *const restrict y, float *const restrict b,
                                          const size_t n)
{
  // solve L × b = y for b
  // use the lower triangular part of L from top to bottom

  int valid = 1;

  for(size_t i = 0; i < n; ++i)
  {
    float sum = y[i];
    for(size_t j = 0; j < i; ++j)
      sum -= L[i * n + j] * b[j];

    const float temp = L[i * n + i];

    if(temp != 0.0f)
      b[i] = sum / temp;
    else
    {
      b[i] = NAN;
      valid = 0;
    }
  }

  return valid; // success ?
}


__DT_CLONE_TARGETS__
static inline int triangular_ascent_fast(const float *const restrict L,
                              const float *const restrict b, float *const restrict x,
                              const size_t n)
{
  // solve L' × x = b for x
  // use the lower triangular part of L transposed from bottom to top

  for(int i = (n - 1); i > -1 ; --i)
  {
    float sum = b[i];
    for(int j = (n - 1); j > i; --j)
      sum -= L[j * n + i] * x[j];

    x[i] = sum / L[i * n + i];
  }

  return 1; // success !
}


__DT_CLONE_TARGETS__
static inline int triangular_ascent_safe(const float *const restrict L,
                              const float *const restrict b, float *const restrict x,
                              const size_t n)
{
  // solve L' × x = b for x
  // use the lower triangular part of L transposed from bottom to top

  int valid = 1;

  for(int i = (n - 1); i > -1 ; --i)
  {
    float sum = b[i];
    for(int j = (n - 1); j > i; --j)
      sum -= L[j * n + i] * x[j];

    const float temp = L[i * n + i];
    if(temp != 0.0f)
      x[i] = sum / temp;
    else
    {
      x[i] = NAN;
      valid = 0;
    }
  }

  return valid; // success ?
}


__DT_CLONE_TARGETS__
static inline int solve_hermitian(const float *const restrict A,
                                  float *const restrict y,
                                  const size_t n, const int checks)
{
  // Solve A x = y where A an hermitian positive definite matrix n × n
  // x and y are n vectors. Output the result in y

  // A and y need to be 64-bits aligned, which is darktable's default memory alignment
  // if you used DT_ALIGNED_ARRAY and dt_alloc_sse_ps(...) to declare arrays and pointers

  // If you are sure about the properties of the matrix A (symmetrical square definite positive)
  // because you built it yourself, set checks == FALSE to branch to the fast track that
  // skips validity checks.

  // If you are unsure about A, because it is user-set, set checks == TRUE to branch
  // to the safe but slower path.

  // clock_t start = clock();

  int valid = 0;
  float *const restrict x = dt_alloc_sse_ps(n);
  float *const restrict L = dt_alloc_sse_ps(n * n);

  if(!x || !L)
  {
    dt_control_log(_("Choleski decomposition failed to allocate memory, check your RAM settings"));
    fprintf(stdout, "Choleski decomposition failed to allocate memory, check your RAM settings\n");
    return 0;
  }

  // LU decomposition
  valid = (checks) ? choleski_decompose_safe(A, L, n) :
                     choleski_decompose_fast(A, L, n) ;
  if(!valid) fprintf(stdout, "Cholesky decomposition returned NaNs\n");

  // Triangular descent
  if(valid)
    valid = (checks) ? triangular_descent_safe(L, y, x, n) :
                       triangular_descent_fast(L, y, x, n) ;
  if(!valid) fprintf(stdout, "Cholesky LU triangular descent returned NaNs\n");

  // Triangular ascent
  if(valid)
    valid = (checks) ? triangular_ascent_safe(L, x, y, n) :
                       triangular_ascent_fast(L, x, y, n);
  if(!valid) fprintf(stdout, "Cholesky LU triangular ascent returned NaNs\n");

  dt_free_align(x);
  dt_free_align(L);

  //clock_t end = clock();
  //fprintf(stdout, "hermitian matrix solving took : %f s\n", ((float) (end - start)) / CLOCKS_PER_SEC);

  return valid;
}


__DT_CLONE_TARGETS__
static inline int transpose_dot_matrix(float *const restrict A, // input
                                       float *const restrict A_square, // output
                                       const size_t m, const size_t n)
{
  // Construct the square symmetrical definite positive matrix A' A,
  // BUT only compute the lower triangle part for performance

  for(size_t i = 0; i < n; ++i)
    for(size_t j = 0; j < (i + 1); ++j)
    {
      float sum = 0.0f;
      for(size_t k = 0; k < m; ++k)
        sum += A[k * n + i] * A[k * n + j];

      A_square[i * n + j] = sum;
    }

  return 1;
}


__DT_CLONE_TARGETS__
static inline int transpose_dot_vector(float *const restrict A, // input
                                       float *const restrict y, // input
                                       float *const restrict y_square, // output
                                       const size_t m, const size_t n)
{
  // Construct the vector A' y

  for(size_t i = 0; i < n; ++i)
  {
    float sum = 0.0f;
    for(size_t k = 0; k < m; ++k)
      sum += A[k * n + i] * y[k];

    y_square[i] = sum;
  }

  return 1;
}


__DT_CLONE_TARGETS__
static inline int pseudo_solve(float *const restrict A,
                               float *const restrict y,
                               const size_t m, const size_t n, const int checks)
{
  // Solve the linear problem A x = y with the over-constrained rectanguler matrice A
  // of dimension m × n (m >= n) by the least squares method

  //clock_t start = clock();

  int valid = 1;
  if(m < n)
  {
    valid = 0;
    fprintf(stdout, "Pseudo solve: cannot cast %zu × %zu matrice\n", m, n);
    return valid;
  }

  float *const restrict A_square = dt_alloc_sse_ps(n * n);
  float *const restrict y_square = dt_alloc_sse_ps(n);

  if(!A_square || !y_square)
  {
    dt_control_log(_("Choleski decomposition failed to allocate memory, check your RAM settings"));
    return 0;
  }

  #ifdef _OPENMP
  #pragma omp parallel sections
  #endif
  {
    #ifdef _OPENMP
    #pragma omp section
    #endif
    {
      // Prepare the least squares matrix = A' A
      valid = transpose_dot_matrix(A, A_square, m, n);
    }

    #ifdef _OPENMP
    #pragma omp section
    #endif
    {
      // Prepare the y square vector = A' y
      valid = transpose_dot_vector(A, y, y_square, m, n);
    }
  }


  // Solve A' A x = A' y for x
  valid = solve_hermitian(A_square, y_square, n, checks);
  dt_simd_memcpy(y_square, y, n);

  dt_free_align(y_square);
  dt_free_align(A_square);

  //clock_t end = clock();
  //fprintf(stdout, "hermitian matrix solving took : %f s\n", ((float) (end - start)) / CLOCKS_PER_SEC);

  return valid;
}


// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

