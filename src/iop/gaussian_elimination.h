/*
    This file is part of darktable,
    copyright (c) 2017 Heiko Bauke.

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
    // eleminate elements and swap rows
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
