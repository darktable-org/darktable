/*
    This file is part of darktable,
    Copyright (C) 2017-2020 darktable developers.

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
    These routines can be used to solve full-rank linear systems of
    equations by Gaussian elimination with partial pivoting.  The
    functions gauss_make_triangular and gauss_solve have been adopted
    from Fortran routines as presented in the book "Numerik" by
    Helmuth Späth, Vieweg Verlag, 1994, see also
    http://dx.doi.org/10.1007/978-3-322-89220-1

*/


#pragma once

#include <math.h>
#include <stdlib.h>

// Gaussian elimination with partial vivoting
// after function call the square matrix A is triangular
// vector p keeps track of row swaps
// returns 0 if matrix A is singular
// matrix elements are stored in row-major order
static int gauss_make_triangular(double *A, int *p, int n)
{
  p[n - 1] = n - 1; // we never swap from the last row
  for(int k = 0; k < n; ++k)
  {
    // find pivot element for row swap
    int m = k;
    for(int i = k + 1; i < n; ++i)
      if(fabs(A[k + n * i]) > fabs(A[k + n * m])) m = i;
    p[k] = m; // rows k and m are swapped
    // eliminate elements and swap rows
    double t1 = A[k + n * m];
    A[k + n * m] = A[k + n * k];
    A[k + n * k] = t1; // new diagonal elements are (implicitly) one, store scaling factors on diagonal
    if(t1 != 0)
    {
      for(int i = k + 1; i < n; ++i) A[k + n * i] /= -t1;
      // swap rows
      if(k != m)
        for(int i = k + 1; i < n; ++i)
        {
          double t2 = A[i + n * m];
          A[i + n * m] = A[i + n * k];
          A[i + n * k] = t2;
        }
      for(int j = k + 1; j < n; ++j)
        for(int i = k + 1; i < n; ++i) A[i + n * j] += A[k + j * n] * A[i + k * n];
    }
    else
      // the matrix is singular
      return 0;
  }
  return 1;
}

// backward substritution after Gaussian elimination
static void gauss_solve_triangular(const double *A, const int *p, double *b, int n)
{
  // permute and rescale elements of right-hand-side
  for(int k = 0; k < n - 1; ++k)
  {
    int m = p[k];
    double t = b[m];
    b[m] = b[k];
    b[k] = t;
    for(int i = k + 1; i < n; ++i) b[i] += A[k + n * i] * t;
  }
  // perform backward substritution
  for(int k = n - 1; k > 0; --k)
  {
    b[k] /= A[k + n * k];
    double t = b[k];
    for(int i = 0; i < k; ++i) b[i] -= A[k + n * i] * t;
  }
  b[0] /= A[0 + 0 * n];
}

static int gauss_solve(double *A, double *b, int n)
{
  int *p = malloc(n * sizeof(*p));
  int err_code = 1;
  if((err_code = gauss_make_triangular(A, p, n))) gauss_solve_triangular(A, p, b, n);
  free(p);
  return err_code;
}


__DT_CLONE_TARGETS__
static inline int transpose_dot_matrix(double *const restrict A, // input
                                       double *const restrict A_square, // output
                                       const size_t m, const size_t n)
{
  // Construct the square symmetrical definite positive matrix A' A,

  for(size_t i = 0; i < n; ++i)
    for(size_t j = 0; j < n; ++j)
    {
      double sum = 0.0;
      for(size_t k = 0; k < m; ++k)
        sum += A[k * n + i] * A[k * n + j];

      A_square[i * n + j] = sum;
    }

  return 1;
}


__DT_CLONE_TARGETS__
static inline int transpose_dot_vector(double *const restrict A, // input
                                       double *const restrict y, // input
                                       double *const restrict y_square, // output
                                       const size_t m, const size_t n)
{
  // Construct the vector A' y
  for(size_t i = 0; i < n; ++i)
  {
    double sum = 0.0;
    for(size_t k = 0; k < m; ++k)
      sum += A[k * n + i] * y[k];

    y_square[i] = sum;
  }

  return 1;
}


static inline int pseudo_solve_gaussian(double *const restrict A,
                                        double *const restrict y,
                                        const size_t m, const size_t n, const int checks)
{
  // Solve the weighted linear problem w A'A x = w A' y with the over-constrained rectanguler matrix A
  // of dimension m × n (m >= n) and w a vector of weights, by the least squares method
  int valid = 1;

  if(m < n)
  {
    fprintf(stderr, "pseudo solve: cannot cast %zu x %zu matrix\n", m, n);
    return 0;
  }

  double *const restrict A_square = dt_alloc_align(64, n * n * sizeof(double));
  double *const restrict y_square = dt_alloc_align(64, n * sizeof(double));

  #ifdef _OPENMP
  #pragma omp parallel sections
  #endif
  {
    #ifdef _OPENMP
    #pragma omp section
    #endif
    {
      // Prepare the least squares matrix = A' A
      transpose_dot_matrix(A, A_square, m, n);
    }

    #ifdef _OPENMP
    #pragma omp section
    #endif
    {
      // Prepare the y square vector = A' y
      transpose_dot_vector(A, y, y_square, m, n);
    }
  }

  // Solve A' A x = A' y for x
  valid = gauss_solve(A_square, y_square, n);
  for(size_t k = 0; k < n; k++) y[k] = y_square[k];

  dt_free_align(y_square);
  dt_free_align(A_square);

  return valid;
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
