/*
 * svdcomp - SVD decomposition routine.
 * Takes an mxn matrix a and decomposes it into udv, where u,v are
 * left and right orthogonal transformation matrices, and d is a
 * diagonal matrix of singular values.
 *
 * This routine is adapted from svdecomp.c in XLISP-STAT 2.1 which is
 * adapted by Luke Tierney and David Betz.
 *
 * the now dead xlisp-stat package seems to have been distributed
 * under some sort of BSD license.
 *
 * Input to dsvd is as follows:
 *   a = mxn matrix to be decomposed, gets overwritten with u
 *   m = row dimension of a
 *   n = column dimension of a
 *   w = returns the vector of singular values of a
 *   v = returns the right orthogonal transformation matrix
*/

#pragma once

#include <math.h>
#include <stdio.h>
#include <stdlib.h>


static inline double SIGN(double a, double b)
{
  return copysign(a, b);
}


static inline double PYTHAG(double a, double b)
{
  const double at = fabs(a), bt = fabs(b);
  if (at > bt)
  {
    const double ct = bt / at;
    return at * sqrt(1.0 + ct * ct);
  }
  if (bt > 0.0)
  {
    const double ct = at / bt;
    return bt * sqrt(1.0 + ct * ct);
  }
  return 0.0;
}


// decompose (m >= n)
//      n             n               n
//   |      |      |     |   n     |     |
// m |  a   |  = m |  u  | diag(w) | v^t | n
//   |      |      |     |         |     |
//
// where the data layout of a (in) and u (out) is strided by str for every row
static inline int dsvd(
    double *a,    // input matrix a[j*str + i] is j-th row and i-th column. will be overwritten by u
    int m,        // number of rows of a and u
    int n,        // number of cols of a and u
    int str,      // row stride of a and u
    double *w,    // output singular values w[n]
    double *v)    // output v matrix v[n*n]
{
  if (m < n)
  {
    fprintf(stderr, "[svd] #rows must be >= #cols \n");
    return 0;
  }

  double c, f, h, s, x, y, z;
  double anorm = 0.0, g = 0.0, scale = 0.0;
  double *rv1 = malloc(n * sizeof(double));
  int l = 0;

  /* Householder reduction to bidiagonal form */
  for (int i = 0; i < n; i++)
  {
    /* left-hand reduction */
    l = i + 1;
    rv1[i] = scale * g;
    g = s = scale = 0.0;
    if (i < m)
    {
      for (int k = i; k < m; k++)
        scale += fabs(a[k*str+i]);
      if (scale != 0.0)
      {
        for (int k = i; k < m; k++)
        {
          a[k*str+i] = a[k*str+i]/scale;
          s += a[k*str+i] * a[k*str+i];
        }
        f = a[i*str+i];
        g = -SIGN(sqrt(s), f);
        h = f * g - s;
        a[i*str+i] = f - g;
        if (i != n - 1)
        {
          for (int j = l; j < n; j++)
          {
            s = 0.0;
            for (int k = i; k < m; k++)
              s += a[k*str+i] * a[k*str+j];
            f = s / h;
            for (int k = i; k < m; k++)
              a[k*str+j] += f * a[k*str+i];
          }
        }
        for (int k = i; k < m; k++)
          a[k*str+i] = a[k*str+i]*scale;
      }
    }
    w[i] = scale * g;

    /* right-hand reduction */
    g = s = scale = 0.0;
    if (i < m && i != n - 1)
    {
      for (int k = l; k < n; k++)
        scale += fabs(a[i*str+k]);
      if (scale != 0.0)
      {
        for (int k = l; k < n; k++)
        {
          a[i*str+k] = a[i*str+k]/scale;
          s += a[i*str+k] * a[i*str+k];
        }
        f = a[i*str+l];
        g = -SIGN(sqrt(s), f);
        h = f * g - s;
        a[i*str+l] = f - g;
        for (int k = l; k < n; k++)
          rv1[k] = a[i*str+k] / h;
        if (i != m - 1)
        {
          for (int j = l; j < m; j++)
          {
            s = 0.0;
            for (int k = l; k < n; k++)
              s += a[j*str+k] * a[i*str+k];
            for (int k = l; k < n; k++)
              a[j*str+k] += s * rv1[k];
          }
        }
        for (int k = l; k < n; k++)
          a[i*str+k] = a[i*str+k]*scale;
      }
    }
    anorm = MAX(anorm, (fabs(w[i]) + fabs(rv1[i])));
  }

  /* accumulate the right-hand transformation */
  for (int i = n - 1; i >= 0; i--)
  {
    if (i < n - 1)
    {
      if (g != 0.0)
      {
        for (int j = l; j < n; j++)
          v[j*n+i] = a[i*str+j] / a[i*str+l] / g;
        /* double division to avoid underflow */
        for (int j = l; j < n; j++)
        {
          s = 0.0;
          for (int k = l; k < n; k++)
            s += a[i*str+k] * v[k*n+j];
          for (int k = l; k < n; k++)
            v[k*n+j] += s * v[k*n+i];
        }
      }
      for (int j = l; j < n; j++)
        v[i*n+j] = v[j*n+i] = 0.0;
    }
    v[i*n+i] = 1.0;
    g = rv1[i];
    l = i;
  }

  /* accumulate the left-hand transformation */
  for (int i = n - 1; i >= 0; i--)
  {
    l = i + 1;
    g = w[i];
    if (i < n - 1)
      for (int j = l; j < n; j++)
        a[i*str+j] = 0.0;
    if (g != 0.0)
    {
      g = 1.0 / g;
      if (i != n - 1)
      {
        for (int j = l; j < n; j++)
        {
          s = 0.0;
          for (int k = l; k < m; k++)
            s += a[k*str+i] * a[k*str+j];
          f = (s / a[i*str+i]) * g;
          for (int k = i; k < m; k++)
            a[k*str+j] += f * a[k*str+i];
        }
      }
      for (int j = i; j < m; j++)
        a[j*str+i] = a[j*str+i]*g;
    }
    else
    {
      for (int j = i; j < m; j++)
        a[j*str+i] = 0.0;
    }
    ++a[i*str+i];
  }

  /* diagonalize the bidiagonal form */
  for (int k = n - 1; k >= 0; k--)
  {                             /* loop over singular values */
    const int max_its = 30;
    for (int its = 0; its <= max_its; its++)
    {                         /* loop over allowed iterations */
      _Bool flag = 1;
      int nm = 0;
      for (l = k; l >= 0; l--)
      {                     /* test for splitting */
        nm = MAX(0, l - 1);
        if (fabs(rv1[l]) + anorm == anorm)
        {
          flag = 0;
          break;
        }
        if (l == 0 || fabs(w[nm]) + anorm == anorm)
          break;
      }
      if (flag)
      {
        s = 1.0;
        for (int i = l; i <= k; i++)
        {
          f = s * rv1[i];
          if (fabs(f) + anorm != anorm)
          {
            g = w[i];
            h = PYTHAG(f, g);
            w[i] = h;
            h = 1.0 / h;
            c = g * h;
            s = (- f * h);
            for (int j = 0; j < m; j++)
            {
              y = a[j*str+nm];
              z = a[j*str+i];
              a[j*str+nm] = y * c + z * s;
              a[j*str+i]  = z * c - y * s;
            }
          }
        }
      }
      z = w[k];
      if (l == k)
      {                  /* convergence */
        if (z < 0.0)
        {              /* make singular value nonnegative */
          w[k] = -z;
          for (int j = 0; j < n; j++)
            v[j*n+k] = -v[j*n+k];
        }
        break;
      }
      if (its >= max_its) {
        fprintf(stderr, "[svd] no convergence after %d iterations\n", its);
        free(rv1);
        return 0;
      }

      /* shift from bottom 2 x 2 minor */
      x = w[l];
      nm = k - 1;
      y = w[nm];
      g = rv1[nm];
      h = rv1[k];
      f = ((y - z) * (y + z) + (g - h) * (g + h)) / (2.0 * h * y);
      g = PYTHAG(f, 1.0);
      f = ((x - z) * (x + z) + h * ((y / (f + SIGN(g, f))) - h)) / x;

      /* next QR transformation */
      c = s = 1.0;
      for (int j = l; j <= nm; j++)
      {
        const int i = j + 1;
        g = rv1[i];
        y = w[i];
        h = s * g;
        g = c * g;
        z = PYTHAG(f, h);
        rv1[j] = z;
        c = f / z;
        s = h / z;
        f = x * c + g * s;
        g = g * c - x * s;
        h = y * s;
        y = y * c;
        for (int jj = 0; jj < n; jj++)
        {
          x = v[jj*n+j];
          z = v[jj*n+i];
          v[jj*n+j] = x * c + z * s;
          v[jj*n+i] = z * c - x * s;
        }
        z = PYTHAG(f, h);
        w[j] = z;
        if (z != 0.0)
        {
          z = 1.0 / z;
          c = f * z;
          s = h * z;
        }
        f = (c * g) + (s * y);
        x = (c * y) - (s * g);
        for (int jj = 0; jj < m; jj++)
        {
          y = a[jj*str+j];
          z = a[jj*str+i];
          a[jj*str+j] = y * c + z * s;
          a[jj*str+i] = z * c - y * s;
        }
      }
      rv1[l] = 0.0;
      rv1[k] = f;
      w[k] = x;
    }
  }
  free(rv1);
  return 1;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
