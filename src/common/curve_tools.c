/*
   This file is part of darktable,
   Copyright (C) 2011-2022 darktable developers.

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

   Part of this file is based on nikon_curve.c from UFraw
   Copyright 2004-2008 by Shawn Freeman, Udi Fuchs
*/

#include "curve_tools.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define EPSILON 2 * FLT_MIN

static const int curvedata_anchors_max = 20;

// declare some functions and so I can use the function pointer
float spline_cubic_val(int n, float t[], float tval, float y[], float ypp[]);
float catmull_rom_val(int n, float x[], float xval, float y[], float tangents[]);

float *spline_cubic_set(int n, float t[], float y[]);
float *catmull_rom_set(int n, float x[], float y[]);
float *monotone_hermite_set(int n, float x[], float y[]);

float (*spline_val[])(int, float[], float, float[], float[])
    = { spline_cubic_val, catmull_rom_val, catmull_rom_val };

float *(*spline_set[])(int, float[], float[]) = { spline_cubic_set, catmull_rom_set, monotone_hermite_set };

/**********************************************************************

  Purpose:

    D3_NP_FS factors and solves a D3 system.

  Discussion:

    The D3 storage format is used for a tridiagonal matrix.
    The superdiagonal is stored in entries (1,2:N), the diagonal in
    entries (2,1:N), and the subdiagonal in (3,1:N-1).  Thus, the
    original matrix is "collapsed" vertically into the array.

    This algorithm requires that each diagonal entry be nonzero.
    It does not use pivoting, and so can fail on systems that
    are actually nonsingular.

  Example:

    Here is how a D3 matrix of order 5 would be stored:

       *  A12 A23 A34 A45
      A11 A22 A33 A44 A55
      A21 A32 A43 A54  *

  Modified:

      07 January 2005    Shawn Freeman (pure C modifications)
    15 November 2003    John Burkardt

  Author:

    John Burkardt

  Parameters:

    Input, int N, the order of the linear system.

    Input/output, float A[3*N].
    On input, the nonzero diagonals of the linear system.
    On output, the data in these vectors has been overwritten
    by factorization information.

    Input, float B[N], the right hand side.

    Output, float D3_NP_FS[N], the solution of the linear system.
    This is NULL if there was an error because one of the diagonal
    entries was zero.
**********************************************************************/
float *d3_np_fs(int n, float a[], float b[])

{
  if(n <= 0 || n > curvedata_anchors_max) return NULL;

  //
  //  Check.
  //
  for(int i = 0; i < n; i++)
  {
    if(a[1 + i * 3] == 0.0E+00)
    {
      return NULL;
    }
  }
  float *x = (float *)calloc(n, sizeof(float));
  // nc_merror(x, "d3_np_fs");

  for(int i = 0; i < n; i++)
  {
    x[i] = b[i];
  }

  for(int i = 1; i < n; i++)
  {
    const float xmult = a[2 + (i - 1) * 3] / a[1 + (i - 1) * 3];
    a[1 + i * 3] = a[1 + i * 3] - xmult * a[0 + i * 3];
    x[i] = x[i] - xmult * x[i - 1];
  }

  x[n - 1] = x[n - 1] / a[1 + (n - 1) * 3];
  for(int i = n - 2; 0 <= i; i--)
  {
    x[i] = (x[i] - a[0 + (i + 1) * 3] * x[i + 1]) / a[1 + i * 3];
  }

  return x;
}

/**********************************************************************

  Purpose:

    SPLINE_CUBIC_SET computes the second derivatives of a piecewise cubic spline.

  Discussion:

    For data interpolation, the user must call SPLINE_SET to determine
    the second derivative data, passing in the data to be interpolated,
    and the desired boundary conditions.

    The data to be interpolated, plus the SPLINE_SET output, defines
    the spline.  The user may then call SPLINE_VAL to evaluate the
    spline at any point.

    The cubic spline is a piecewise cubic polynomial.  The intervals
    are determined by the "knots" or abscissas of the data to be
    interpolated.  The cubic spline has continuous first and second
    derivatives over the entire interval of interpolation.

    For any point T in the interval T(IVAL), T(IVAL+1), the form of
    the spline is

      SPL(T) = A(IVAL)
             + B(IVAL) * ( T - T(IVAL) )
             + C(IVAL) * ( T - T(IVAL) )**2
             + D(IVAL) * ( T - T(IVAL) )**3

    If we assume that we know the values Y(*) and YPP(*), which represent
    the values and second derivatives of the spline at each knot, then
    the coefficients can be computed as:

      A(IVAL) = Y(IVAL)
      B(IVAL) = ( Y(IVAL+1) - Y(IVAL) ) / ( T(IVAL+1) - T(IVAL) )
        - ( YPP(IVAL+1) + 2 * YPP(IVAL) ) * ( T(IVAL+1) - T(IVAL) ) / 6
      C(IVAL) = YPP(IVAL) / 2
      D(IVAL) = ( YPP(IVAL+1) - YPP(IVAL) ) / ( 6 * ( T(IVAL+1) - T(IVAL) ) )

    Since the first derivative of the spline is

      SPL'(T) =     B(IVAL)
              + 2 * C(IVAL) * ( T - T(IVAL) )
              + 3 * D(IVAL) * ( T - T(IVAL) )**2,

    the requirement that the first derivative be continuous at interior
    knot I results in a total of N-2 equations, of the form:

      B(IVAL-1) + 2 C(IVAL-1) * (T(IVAL)-T(IVAL-1))
      + 3 * D(IVAL-1) * (T(IVAL) - T(IVAL-1))**2 = B(IVAL)

    or, setting H(IVAL) = T(IVAL+1) - T(IVAL)

      ( Y(IVAL) - Y(IVAL-1) ) / H(IVAL-1)
      - ( YPP(IVAL) + 2 * YPP(IVAL-1) ) * H(IVAL-1) / 6
      + YPP(IVAL-1) * H(IVAL-1)
      + ( YPP(IVAL) - YPP(IVAL-1) ) * H(IVAL-1) / 2
      =
      ( Y(IVAL+1) - Y(IVAL) ) / H(IVAL)
      - ( YPP(IVAL+1) + 2 * YPP(IVAL) ) * H(IVAL) / 6

    or

      YPP(IVAL-1) * H(IVAL-1) + 2 * YPP(IVAL) * ( H(IVAL-1) + H(IVAL) )
      + YPP(IVAL) * H(IVAL)
      =
      6 * ( Y(IVAL+1) - Y(IVAL) ) / H(IVAL)
      - 6 * ( Y(IVAL) - Y(IVAL-1) ) / H(IVAL-1)

    Boundary conditions must be applied at the first and last knots.
    The resulting tridiagonal system can be solved for the YPP values.

  Modified:

      07 January 2005    Shawn Freeman (pure C modifications)
    06 February 2004    John Burkardt


  Author:

    John Burkardt

  Parameters:

    Input, int N, the number of data points.  N must be at least 2.
    In the special case where N = 2 and IBCBEG = IBCEND = 0, the
    spline will actually be linear.

    Input, float T[N], the knot values, that is, the points were data is
    specified.  The knot values should be distinct, and increasing.

    Input, float Y[N], the data values to be interpolated.

    Input, int IBCBEG, left boundary condition flag:
      0: the cubic spline should be a quadratic over the first interval;
      1: the first derivative at the left endpoint should be YBCBEG;
      2: the second derivative at the left endpoint should be YBCBEG.

    Input, float YBCBEG, the values to be used in the boundary
    conditions if IBCBEG is equal to 1 or 2.

    Input, int IBCEND, right boundary condition flag:
      0: the cubic spline should be a quadratic over the last interval;
      1: the first derivative at the right endpoint should be YBCEND;
      2: the second derivative at the right endpoint should be YBCEND.

    Input, float YBCEND, the values to be used in the boundary
    conditions if IBCEND is equal to 1 or 2.

    Output, float SPLINE_CUBIC_SET[N], the second derivatives of the cubic spline.
**********************************************************************/
static float *spline_cubic_set_internal(int n, float t[], float y[], int ibcbeg, float ybcbeg, int ibcend,
                                        float ybcend)
{
  //
  //  Check.
  //
  if(n <= 1)
  {
    // nc_message(NC_SET_ERROR, "spline_cubic_set() error: "
    //    "The number of data points must be at least 2.\n");
    return NULL;
  }

  for(int i = 0; i < n - 1; i++)
  {
    if(t[i + 1] <= t[i])
    {
      // nc_message(NC_SET_ERROR, "spline_cubic_set() error: "
      //   "The knots must be strictly increasing, but "
      //  "T(%u) = %e, T(%u) = %e\n",i,t[i],i+1,t[i+1]);
      return NULL;
    }
  }
  float *a = (float *)calloc(3 * n, sizeof(float));
  // nc_merror(a, "spline_cubic_set");
  float *b = (float *)calloc(n, sizeof(float));
  // nc_merror(b, "spline_cubic_set");
  //
  //  Set up the first equation.
  //
  if(ibcbeg == 0)
  {
    b[0] = 0.0E+00;
    a[1 + 0 * 3] = 1.0E+00;
    a[0 + 1 * 3] = -1.0E+00;
  }
  else if(ibcbeg == 1)
  {
    b[0] = (y[1] - y[0]) / (t[1] - t[0]) - ybcbeg;
    a[1 + 0 * 3] = (t[1] - t[0]) / 3.0E+00;
    a[0 + 1 * 3] = (t[1] - t[0]) / 6.0E+00;
  }
  else if(ibcbeg == 2)
  {
    b[0] = ybcbeg;
    a[1 + 0 * 3] = 1.0E+00;
    a[0 + 1 * 3] = 0.0E+00;
  }
  else
  {
    // nc_message(NC_SET_ERROR, "spline_cubic_set() error: "
    //   "IBCBEG must be 0, 1 or 2. The input value is %u.\n", ibcbeg);
    free(a);
    free(b);
    return NULL;
  }
  //
  //  Set up the intermediate equations.
  //
  for(int i = 1; i < n - 1; i++)
  {
    b[i] = (y[i + 1] - y[i]) / (t[i + 1] - t[i]) - (y[i] - y[i - 1]) / (t[i] - t[i - 1]);
    a[2 + (i - 1) * 3] = (t[i] - t[i - 1]) / 6.0E+00;
    a[1 + i * 3] = (t[i + 1] - t[i - 1]) / 3.0E+00;
    a[0 + (i + 1) * 3] = (t[i + 1] - t[i]) / 6.0E+00;
  }
  //
  //  Set up the last equation.
  //
  if(ibcend == 0)
  {
    b[n - 1] = 0.0E+00;
    a[2 + (n - 2) * 3] = -1.0E+00;
    a[1 + (n - 1) * 3] = 1.0E+00;
  }
  else if(ibcend == 1)
  {
    b[n - 1] = ybcend - (y[n - 1] - y[n - 2]) / (t[n - 1] - t[n - 2]);
    a[2 + (n - 2) * 3] = (t[n - 1] - t[n - 2]) / 6.0E+00;
    a[1 + (n - 1) * 3] = (t[n - 1] - t[n - 2]) / 3.0E+00;
  }
  else if(ibcend == 2)
  {
    b[n - 1] = ybcend;
    a[2 + (n - 2) * 3] = 0.0E+00;
    a[1 + (n - 1) * 3] = 1.0E+00;
  }
  else
  {
    // nc_message(NC_SET_ERROR, "spline_cubic_set() error: "
    //   "IBCEND must be 0, 1 or 2. The input value is %u", ibcend);
    free(a);
    free(b);
    return NULL;
  }
  //
  //  Solve the linear system.
  //
  float *ypp = NULL;

  if(n == 2 && ibcbeg == 0 && ibcend == 0)
  {
    ypp = (float *)calloc(2, sizeof(float));
    // nc_merror(ypp, "spline_cubic_set");

    ypp[0] = 0.0E+00;
    ypp[1] = 0.0E+00;
  }
  else
  {
    ypp = d3_np_fs(n, a, b);

    if(!ypp)
    {
      //  nc_message(NC_SET_ERROR, "spline_cubic_set() error: "
      //     "The linear system could not be solved.\n");
      free(a);
      free(b);
      return NULL;
    }
  }

  free(a);
  free(b);
  return ypp;
}
/************************************************************
 *
 * This is a convenience wrapper function around spline_cubic_set
 *
 ************************************************************/
float *spline_cubic_set(int n, float t[], float y[])
{
  return spline_cubic_set_internal(n, t, y, 2, 0.0, 2, 0.0);
}

/*************************************************************
* monotone_hermite_set:
*      calculates the tangents for a monotonic hermite spline curve.
*      see http://en.wikipedia.org/wiki/Monotone_cubic_interpolation
*
*  input:
*      n = number of control points
*      x = input x array
*      y = input y array
*  output:
*      pointer to array containing the tangents
*************************************************************/
float *monotone_hermite_set(int n, float x[], float y[])
{
  if(n <= 1)
  {
    // nc_message(NC_SET_ERROR, "spline_cubic_set() error: "
    //   "The number of data points must be at least 2.\n");
    return NULL;
  }

  for(int i = 0; i < n - 1; i++)
  {
    if(x[i + 1] <= x[i])
    {
      // nc_message(NC_SET_ERROR, "spline_cubic_set() error: "
      //   "The knots must be strictly increasing, but "
      //  "T(%u) = %e, T(%u) = %e\n",i,x[i],i+1,x[i+1]);
      return NULL;
    }
  }

  float *delta = (float *)calloc(n, sizeof(float));
  // nc_merror(delta, "spline_cubic_set");
  float *m = (float *)calloc(n + 1, sizeof(float));
  // nc_merror(m, "spline_cubic_set");
  // calculate the slopes
  for(int i = 0; i < n - 1; i++)
  {
    delta[i] = (y[i + 1] - y[i]) / (x[i + 1] - x[i]);
  }
  delta[n - 1] = delta[n - 2];

  m[0] = delta[0];
  m[n - 1] = delta[n - 1];

  for(int i = 1; i < n - 1; i++)
  {
    m[i] = (delta[i - 1] + delta[i]) * .5f;
  }
  for(int i = 0; i < n; i++)
  {
    if(fabsf(delta[i]) < EPSILON)
    {
      m[i] = 0.0f;
      m[i + 1] = 0.0f;
    }
    else
    {
      const float alpha = m[i] / delta[i];
      const float beta = m[i + 1] / delta[i];
      const float tau = alpha * alpha + beta * beta;
      if(tau > 9.0f)
      {
        m[i] = 3.0f * alpha * delta[i] / sqrtf(tau);
        m[i + 1] = 3.0f * beta * delta[i] / sqrtf(tau);
      }
    }
  }
  free(delta);
  return m;
}

/*************************************************************
* catmull_rom_set:
*      calculates the tangents for a catmull_rom spline
*      see http://en.wikipedia.org/wiki/Cubic_Hermite_spline
*
*
*  input:
*      n = number of control points
*      x = input x array
*      y = input y array
*  output:
*      pointer to array containing the tangents
*************************************************************/
float *catmull_rom_set(int n, float x[], float y[])
{
  if(n <= 1)
  {
    // nc_message(NC_SET_ERROR, "spline_cubic_set() error: "
    //   "The number of data points must be at least 2.\n");
    return NULL;
  }

  for(int i = 0; i < n - 1; i++)
  {
    if(x[i + 1] <= x[i])
    {
      // nc_message(NC_SET_ERROR, "spline_cubic_set() error: "
      //   "The knots must be strictly increasing, but "
      //  "T(%u) = %e, T(%u) = %e\n",i,x[i],i+1,x[i+1]);
      return NULL;
    }
  }
  // nc_merror(delta, "spline_cubic_set");
  float *m = (float *)calloc(n, sizeof(float));
  // nc_merror(m, "spline_cubic_set");

  // calculate the slopes
  m[0] = (y[1] - y[0]) / (x[1] - x[0]);
  for(int i = 1; i < n - 1; i++)
  {
    m[i] = (y[i + 1] - y[i - 1]) / (x[i + 1] - x[i - 1]);
  }
  m[n - 1] = (y[n - 1] - y[n - 2]) / (x[n - 1] - x[n - 2]);

  return m;
}

float *interpolate_set(int n, float x[], float y[], unsigned int type)
{
  return (*spline_set[type])(n, x, y);
}

float interpolate_val(int n, float x[], float xval, float y[], float tangents[], unsigned int type)
{
  return (*spline_val[type])(n, x, xval, y, tangents);
}

/*************************************************************
 * catmull_rom_val:
 *      piecewise catmull-rom interpolation
 *
 *      n = number of control points
 *      x = input x array
 *      xval = input value where to interpolate the data
 *      y = input y array
 *      tangent = input array of tangents
 *  output:
 *      interpolated value at xval
 *
 *************************************************************/
float catmull_rom_val(int n, float x[], float xval, float y[], float tangents[])
{
  //
  //  Determine the interval [ T(I), T(I+1) ] that contains TVAL.
  //  Values below T[0] or above T[N-1] use extrapolation.
  //
  int ival = n - 2;

  for(int i = 0; i < n - 2; i++)
  {
    if(xval < x[i + 1])
    {
      ival = i;
      break;
    }
  }

  const float m0 = tangents[ival];
  const float m1 = tangents[ival + 1];
  //
  //  In the interval I, the polynomial is in terms of a normalized
  //  coordinate between 0 and 1.
  //
  const float h = x[ival + 1] - x[ival];
  const float dx = (xval - x[ival]) / h;
  const float dx2 = dx * dx;
  const float dx3 = dx * dx2;

  const float h00 = (2.0 * dx3) - (3.0 * dx2) + 1.0;
  const float h10 = (1.0 * dx3) - (2.0 * dx2) + dx;
  const float h01 = (-2.0 * dx3) + (3.0 * dx2);
  const float h11 = (1.0 * dx3) - (1.0 * dx2);

  return (h00 * y[ival]) + (h10 * h * m0) + (h01 * y[ival + 1]) + (h11 * h * m1);
}


/**********************************************************************

  Purpose:

    SPLINE_CUBIC_VAL evaluates a piecewise cubic spline at a point.

  Discussion:

    SPLINE_CUBIC_SET must have already been called to define the values of YPP.

    For any point T in the interval T(IVAL), T(IVAL+1), the form of
    the spline is

      SPL(T) = A
             + B * ( T - T(IVAL) )
             + C * ( T - T(IVAL) )**2
             + D * ( T - T(IVAL) )**3

    Here:
      A = Y(IVAL)
      B = ( Y(IVAL+1) - Y(IVAL) ) / ( T(IVAL+1) - T(IVAL) )
        - ( YPP(IVAL+1) + 2 * YPP(IVAL) ) * ( T(IVAL+1) - T(IVAL) ) / 6
      C = YPP(IVAL) / 2
      D = ( YPP(IVAL+1) - YPP(IVAL) ) / ( 6 * ( T(IVAL+1) - T(IVAL) ) )

  Modified:

      07 January 2005    Shawn Freeman (pure C modifications)
    04 February 1999    John Burkardt

  Author:

    John Burkardt

  Parameters:

    Input, int n, the number of knots.

    Input, float Y[N], the data values at the knots.

    Input, float T[N], the knot values.

    Input, float TVAL, a point, typically between T[0] and T[N-1], at
    which the spline is to be evalulated.  If TVAL lies outside
    this range, extrapolation is used.

    Input, float Y[N], the data values at the knots.

    Input, float YPP[N], the second derivatives of the spline at
    the knots.


    Output, float SPLINE_VAL, the value of the spline at TVAL.

**********************************************************************/
float spline_cubic_val(int n, float t[], float tval, float y[], float ypp[])
{
  int ival = 0;
  //
  //  Determine the interval [ T(I), T(I+1) ] that contains TVAL.
  //  Values below T[0] or above T[N-1] use extrapolation.
  //
  ival = n - 2;

  for(int i = 0; i < n - 1; i++)
  {
    if(tval < t[i + 1])
    {
      ival = i;
      break;
    }
  }
  //
  //  In the interval I, the polynomial is in terms of a normalized
  //  coordinate between 0 and 1.
  //
  const float dt = tval - t[ival];
  const float h = t[ival + 1] - t[ival];

  const float yval = y[ival]
         + dt * ((y[ival + 1] - y[ival]) / h - (ypp[ival + 1] / 6.0E+00 + ypp[ival] / 3.0E+00) * h
                 + dt * (0.5E+00 * ypp[ival] + dt * ((ypp[ival + 1] - ypp[ival]) / (6.0E+00 * h))));

  // we really never need the derivatives so commented this out
  /**ypval = ( y[ival+1] - y[ival] ) / h
    - ( ypp[ival+1] / 6.0E+00 + ypp[ival] / 3.0E+00 ) * h
    + dt * ( ypp[ival]
    + dt * ( 0.5E+00 * ( ypp[ival+1] - ypp[ival] ) / h ) );

  *yppval = ypp[ival] + dt * ( ypp[ival+1] - ypp[ival] ) / h;*/

  return yval;
}


/*********************************************
CurveDataSample:
    Samples from a spline curve constructed from
    the Nikon data.

    curve   - Pointer to curve struct to hold the data.
    sample  - Pointer to sample struct to hold the data.
**********************************************/
int CurveDataSample(CurveData *curve, CurveSample *sample)
{
  int n = 0;

  float x[20] = { 0 };
  float y[20] = { 0 };

  // The box points are what the anchor points are relative
  // to so...

  const float box_width = curve->m_max_x - curve->m_min_x;
  const float box_height = curve->m_max_y - curve->m_min_y;

  // build arrays for processing
  if(curve->m_numAnchors == 0)
  {
    // just a straight line using box coordinates
    x[0] = curve->m_min_x;
    y[0] = curve->m_min_y;
    x[1] = curve->m_max_x;
    y[1] = curve->m_max_y;
    n = 2;
  }
  else
  {
    for(int i = 0; i < curve->m_numAnchors; i++)
    {
      x[i] = curve->m_anchors[i].x * box_width + curve->m_min_x;
      y[i] = curve->m_anchors[i].y * box_height + curve->m_min_y;
    }
    n = curve->m_numAnchors;
  }
  const float res = 1.0 / (float)(sample->m_samplingRes - 1);
  const int firstPointX = x[0] * (sample->m_samplingRes - 1);
  const int firstPointY = y[0] * (sample->m_outputRes - 1);
  const int lastPointX = x[n - 1] * (sample->m_samplingRes - 1);
  const int lastPointY = y[n - 1] * (sample->m_outputRes - 1);
  const int maxY = curve->m_max_y * (sample->m_outputRes - 1);
  const int minY = curve->m_min_y * (sample->m_outputRes - 1);
  // returns an array of second derivatives used to calculate the spline curve.
  // this is a malloc'd array that needs to be freed when done.
  // The settings currently calculate the natural spline, which closely matches
  // camera curve output in raw files.
  float *ypp = interpolate_set(n, x, y, curve->m_spline_type);
  if(ypp == NULL) return CT_ERROR;

  for(int i = 0; i < (int)sample->m_samplingRes; i++)
  {
    // get the value of the curve at a point
    // take into account that curves may not necessarily begin at x = 0.0
    // nor end at x = 1.0

    // Before the first point and after the last point, take a strait line
    if(i < firstPointX)
    {
      sample->m_Samples[i] = firstPointY;
    }
    else if(i > lastPointX)
    {
      sample->m_Samples[i] = lastPointY;
    }
    else
    {
      // within range, we can sample the curve
      int val = interpolate_val(n, x, i * res, y, ypp, curve->m_spline_type) * (sample->m_outputRes - 1) + 0.5;
      if(val > maxY) val = maxY;
      if(val < minY) val = minY;
      sample->m_Samples[i] = val;
    }
  }

  free(ypp);
  return CT_SUCCESS;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
