#ifndef DT_CURVE_TOOL
#define DT_CURVE_TOOL
/*
   This file is part of darktable,
   copyright (c) 2011 Jochen Schroeder

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

#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include "curve_tools.h"

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

    Input/output, double A[3*N].
    On input, the nonzero diagonals of the linear system.
    On output, the data in these vectors has been overwritten
    by factorization information.

    Input, double B[N], the right hand side.

    Output, double D3_NP_FS[N], the solution of the linear system.
    This is NULL if there was an error because one of the diagonal
    entries was zero.
**********************************************************************/
double *d3_np_fs ( int n, double a[], double b[] )

{
  int i;
  double *x;
  double xmult;
//
//  Check.
//
  for ( i = 0; i < n; i++ )
  {
    if ( a[1+i*3] == 0.0E+00 )
    {
      return NULL;
    }
  }
  x = (double *)calloc(n,sizeof(double));
  //nc_merror(x, "d3_np_fs");

  for ( i = 0; i < n; i++ )
  {
    x[i] = b[i];
  }

  for ( i = 1; i < n; i++ )
  {
    xmult = a[2+(i-1)*3] / a[1+(i-1)*3];
    a[1+i*3] = a[1+i*3] - xmult * a[0+i*3];
    x[i] = x[i] - xmult * x[i-1];
  }

  x[n-1] = x[n-1] / a[1+(n-1)*3];
  for ( i = n-2; 0 <= i; i-- )
  {
    x[i] = ( x[i] - a[0+(i+1)*3] * x[i+1] ) / a[1+i*3];
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
    interpolated.  The cubic spline has continous first and second
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

    Input, double T[N], the knot values, that is, the points were data is
    specified.  The knot values should be distinct, and increasing.

    Input, double Y[N], the data values to be interpolated.

    Input, int IBCBEG, left boundary condition flag:
      0: the cubic spline should be a quadratic over the first interval;
      1: the first derivative at the left endpoint should be YBCBEG;
      2: the second derivative at the left endpoint should be YBCBEG.

    Input, double YBCBEG, the values to be used in the boundary
    conditions if IBCBEG is equal to 1 or 2.

    Input, int IBCEND, right boundary condition flag:
      0: the cubic spline should be a quadratic over the last interval;
      1: the first derivative at the right endpoint should be YBCEND;
      2: the second derivative at the right endpoint should be YBCEND.

    Input, double YBCEND, the values to be used in the boundary
    conditions if IBCEND is equal to 1 or 2.

    Output, double SPLINE_CUBIC_SET[N], the second derivatives of the cubic spline.
**********************************************************************/
double *spline_cubic_set ( int n, double t[], double y[], int ibcbeg,
    double ybcbeg, int ibcend, double ybcend )
{
  double *a;
  double *b;
  int i;
  double *ypp;
//
//  Check.
//
  if ( n <= 1 )
  {
   // nc_message(NC_SET_ERROR, "spline_cubic_set() error: "
    //    "The number of data points must be at least 2.\n");
    return NULL;
  }

  for ( i = 0; i < n - 1; i++ )
  {
    if ( t[i+1] <= t[i] )
    {
      //nc_message(NC_SET_ERROR, "spline_cubic_set() error: "
       //   "The knots must be strictly increasing, but "
        //  "T(%u) = %e, T(%u) = %e\n",i,t[i],i+1,t[i+1]);
      return NULL;
    }
  }
  a = (double *)calloc(3*n,sizeof(double));
  //nc_merror(a, "spline_cubic_set");
  b = (double *)calloc(n,sizeof(double));
  //nc_merror(b, "spline_cubic_set");
//
//  Set up the first equation.
//
  if ( ibcbeg == 0 )
  {
    b[0] = 0.0E+00;
    a[1+0*3] = 1.0E+00;
    a[0+1*3] = -1.0E+00;
  }
  else if ( ibcbeg == 1 )
  {
    b[0] = ( y[1] - y[0] ) / ( t[1] - t[0] ) - ybcbeg;
    a[1+0*3] = ( t[1] - t[0] ) / 3.0E+00;
    a[0+1*3] = ( t[1] - t[0] ) / 6.0E+00;
  }
  else if ( ibcbeg == 2 )
  {
    b[0] = ybcbeg;
    a[1+0*3] = 1.0E+00;
    a[0+1*3] = 0.0E+00;
  }
  else
  {
    //nc_message(NC_SET_ERROR, "spline_cubic_set() error: "
     //   "IBCBEG must be 0, 1 or 2. The input value is %u.\n", ibcbeg);
    free(a);
    free(b);
    return NULL;
  }
//
//  Set up the intermediate equations.
//
  for ( i = 1; i < n-1; i++ )
  {
    b[i] = ( y[i+1] - y[i] ) / ( t[i+1] - t[i] )
      - ( y[i] - y[i-1] ) / ( t[i] - t[i-1] );
    a[2+(i-1)*3] = ( t[i] - t[i-1] ) / 6.0E+00;
    a[1+ i   *3] = ( t[i+1] - t[i-1] ) / 3.0E+00;
    a[0+(i+1)*3] = ( t[i+1] - t[i] ) / 6.0E+00;
  }
//
//  Set up the last equation.
//
  if ( ibcend == 0 )
  {
    b[n-1] = 0.0E+00;
    a[2+(n-2)*3] = -1.0E+00;
    a[1+(n-1)*3] = 1.0E+00;
  }
  else if ( ibcend == 1 )
  {
    b[n-1] = ybcend - ( y[n-1] - y[n-2] ) / ( t[n-1] - t[n-2] );
    a[2+(n-2)*3] = ( t[n-1] - t[n-2] ) / 6.0E+00;
    a[1+(n-1)*3] = ( t[n-1] - t[n-2] ) / 3.0E+00;
  }
  else if ( ibcend == 2 )
  {
    b[n-1] = ybcend;
    a[2+(n-2)*3] = 0.0E+00;
    a[1+(n-1)*3] = 1.0E+00;
  }
  else
  {
    //nc_message(NC_SET_ERROR, "spline_cubic_set() error: "
     //   "IBCEND must be 0, 1 or 2. The input value is %u", ibcend);
    free(a);
    free(b);
    return NULL;
  }
//
//  Solve the linear system.
//
  if ( n == 2 && ibcbeg == 0 && ibcend == 0 )
  {
    ypp = (double *)calloc(2,sizeof(double));
    //nc_merror(ypp, "spline_cubic_set");

    ypp[0] = 0.0E+00;
    ypp[1] = 0.0E+00;
  }
  else
  {
    ypp = d3_np_fs ( n, a, b );

    if ( !ypp )
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

double *cubic_hermite_set ( int n, double x[], double y[])
{
  double *delta;
  double *m;
  int i;
  if ( n <= 1 )
  {
    //nc_message(NC_SET_ERROR, "spline_cubic_set() error: "
     //   "The number of data points must be at least 2.\n");
    return NULL;
  }

  for ( i = 0; i < n - 1; i++ )
  {
    if ( x[i+1] <= x[i] )
    {
      //nc_message(NC_SET_ERROR, "spline_cubic_set() error: "
       //   "The knots must be strictly increasing, but "
        //  "T(%u) = %e, T(%u) = %e\n",i,x[i],i+1,x[i+1]);
      return NULL;
    }
  }
  delta = (double *)calloc(n,sizeof(double));
  //nc_merror(delta, "spline_cubic_set");
  m = (double *)calloc(n,sizeof(double));
  //nc_merror(m, "spline_cubic_set");
  //calculate the slopes
  for (i = 0;i<n-1;i++)
  {
      delta[i] = (y[i+1]-y[i])/(x[i+1]-x[i]);
  }

  m[0] = delta[0];
  m[n-1] = delta[n-2];

  for (i=1;i<n-1;i++)
  {
      m[i] = (delta[i]+delta[i+1])/2.0;
  }

  free(delta);
  return m;
}

double cubic_hermite_val ( int n, double x[], double xval, double y[],
    double tangents[])
{
  double dx;
  double h;
  int i;
  int ival;
  double yval;
  double h00,h01,h10,h11;
  double m0, m1;
//
//  Determine the interval [ T(I), T(I+1) ] that contains TVAL.
//  Values below T[0] or above T[N-1] use extrapolation.
//
  ival = n - 2;

  for ( i = 0; i < n-1; i++ )
  {
    if ( xval < x[i+1] )
    {
      ival = i;
      break;
    }
  }

  if (ival==0)
  {
      m0 = y[1]-y[0];
      m1 = (y[2]-y[0])/2.0;
  }
  else if (ival==n-2)
  {
      m0 = (y[ival+1]-y[ival-1])/2.0;
      m1 = y[ival+1]-y[ival];
  }
  else
  {
      m0 = (y[ival+1]-y[ival-1])/2.0;
      m1 = (y[ival+2]-y[ival])/2.0;
  }

//
//  In the interval I, the polynomial is in terms of a normalized
//  coordinate between 0 and 1.
//
  h = x[ival+1] - x[ival];
  dx = (xval-x[ival])/h;

    h00 = ( 2.0 * dx*dx*dx) - (3.0 * dx*dx) + 1.0;
    h10 = ( 1.0 * dx*dx*dx) - (2.0 * dx*dx) + dx;
    h01 = (-2.0 * dx*dx*dx) + (3.0 * dx*dx);
    h11 = ( 1.0 * dx*dx*dx) - (1.0 * dx*dx);

    h = 1;
    yval = (h00 * y[ival]) + (h10 * h * m0) + (h01 * y[ival+1]) + (h11 * h * m1);

  return yval;
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

    Input, double Y[N], the data values at the knots.

    Input, double T[N], the knot values.

    Input, double TVAL, a point, typically between T[0] and T[N-1], at
    which the spline is to be evalulated.  If TVAL lies outside
    this range, extrapolation is used.

    Input, double Y[N], the data values at the knots.

    Input, double YPP[N], the second derivatives of the spline at
    the knots.

    Output, double *YPVAL, the derivative of the spline at TVAL.

    Output, double *YPPVAL, the second derivative of the spline at TVAL.

    Output, double SPLINE_VAL, the value of the spline at TVAL.

**********************************************************************/
double spline_cubic_val ( int n, double t[], double tval, double y[],
    double ypp[], double *ypval, double *yppval )
{
  double dt;
  double h;
  int i;
  int ival;
  double yval;
//
//  Determine the interval [ T(I), T(I+1) ] that contains TVAL.
//  Values below T[0] or above T[N-1] use extrapolation.
//
  ival = n - 2;

  for ( i = 0; i < n-1; i++ )
  {
    if ( tval < t[i+1] )
    {
      ival = i;
      break;
    }
  }
//
//  In the interval I, the polynomial is in terms of a normalized
//  coordinate between 0 and 1.
//
  dt = tval - t[ival];
  h = t[ival+1] - t[ival];

  yval = y[ival]
    + dt * ( ( y[ival+1] - y[ival] ) / h
       - ( ypp[ival+1] / 6.0E+00 + ypp[ival] / 3.0E+00 ) * h
    + dt * ( 0.5E+00 * ypp[ival]
    + dt * ( ( ypp[ival+1] - ypp[ival] ) / ( 6.0E+00 * h ) ) ) );

  *ypval = ( y[ival+1] - y[ival] ) / h
    - ( ypp[ival+1] / 6.0E+00 + ypp[ival] / 3.0E+00 ) * h
    + dt * ( ypp[ival]
    + dt * ( 0.5E+00 * ( ypp[ival+1] - ypp[ival] ) / h ) );

  *yppval = ypp[ival] + dt * ( ypp[ival+1] - ypp[ival] ) / h;

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
    int i = 0, n;

    double x[20];
    double y[20];

    //The box points  are what the anchor points are relative
    //to so...

    double box_width = curve->m_max_x - curve->m_min_x;
    double box_height = curve->m_max_y - curve->m_min_y;

    //build arrays for processing
    if (curve->m_numAnchors == 0)
    {
    //just a straight line using box coordinates
    x[0] = curve->m_min_x;
    y[0] = curve->m_min_y;
    x[1] = curve->m_max_x;
    y[1] = curve->m_max_y;
    n = 2;
    }
    else
    {
    for(i = 0; i < curve->m_numAnchors; i++)
    {
        x[i] = curve->m_anchors[i].x*box_width + curve->m_min_x;
        y[i] = curve->m_anchors[i].y*box_height + curve->m_min_y;
    }
    n = curve->m_numAnchors;
    }
    //returns an array of second derivatives used to calculate the spline curve.
    //this is a malloc'd array that needs to be freed when done.
    //The setings currently calculate the natural spline, which closely matches
    //camera curve output in raw files.
    double *ypp = spline_cubic_set(n, x, y, 2, 0.0, 2, 0.0);
    if (ypp==NULL) return CT_ERROR;

    //first derivative at a point
    double ypval = 0;

    //second derivate at a point
    double yppval = 0;

    //Now build a table
    int val;
    double res = 1.0/(double)(sample->m_samplingRes-1);

    //allocate enough space for the samples
    //DEBUG_PRINT("DEBUG: SAMPLING ALLOCATION: %u bytes\n",
     //       sample->m_samplingRes*sizeof(int));
    //DEBUG_PRINT("DEBUG: SAMPLING OUTPUT RANGE: 0 -> %u\n", sample->m_outputRes);

    // sample->m_Samples = (unsigned short int *)realloc(sample->m_Samples,
        // sample->m_samplingRes * sizeof(short int));
    // nc_merror(sample->m_Samples, "CurveDataSample");

    int firstPointX = x[0] * (sample->m_samplingRes-1);
    int firstPointY = y[0] * (sample->m_outputRes-1);
    int lastPointX = x[n-1] * (sample->m_samplingRes-1);
    int lastPointY = y[n-1] * (sample->m_outputRes-1);
    int maxY = curve->m_max_y * (sample->m_outputRes-1);
    int minY = curve->m_min_y * (sample->m_outputRes-1);

    for(i = 0; i < (int)sample->m_samplingRes; i++)
    {
    //get the value of the curve at a point
    //take into account that curves may not necessarily begin at x = 0.0
    //nor end at x = 1.0

    //Before the first point and after the last point, take a strait line
    if (i < firstPointX) {
        sample->m_Samples[i] = firstPointY;
    } else if (i > lastPointX) {
        sample->m_Samples[i] = lastPointY;
    } else {
        //within range, we can sample the curve
        val = spline_cubic_val( n, x, i*res, y,
            ypp, &ypval, &yppval ) * (sample->m_outputRes-1) + 0.5;
        sample->m_Samples[i] = MIN(MAX(val,minY),maxY);
    }
    }

    free(ypp);
    return CT_SUCCESS;
}

/*********************************************
CurveDataSample_hermite:
    Samples from a hermite spline curve constructed from
    data.

    curve   - Pointer to curve struct to hold the data.
    sample  - Pointer to sample struct to hold the data.
**********************************************/
int CurveDataSample_hermite(CurveData *curve, CurveSample *sample)
{
    int i = 0, n;

    double x[20];
    double y[20];

    //The box points are what the anchor points are relative
    //to so...

    double box_width = curve->m_max_x - curve->m_min_x;
    double box_height = curve->m_max_y - curve->m_min_y;

    //build arrays for processing
    if (curve->m_numAnchors == 0)
    {
    //just a straight line using box coordinates
    x[0] = curve->m_min_x;
    y[0] = curve->m_min_y;
    x[1] = curve->m_max_x;
    y[1] = curve->m_max_y;
    n = 2;
    }
    else
    {
    for(i = 0; i < curve->m_numAnchors; i++)
    {
        x[i] = curve->m_anchors[i].x*box_width + curve->m_min_x;
        y[i] = curve->m_anchors[i].y*box_height + curve->m_min_y;
    }
    n = curve->m_numAnchors;
    }
    //returns an array of second derivatives used to calculate the spline curve.
    //this is a malloc'd array that needs to be freed when done.
    //The setings currently calculate the natural spline, which closely matches
    //camera curve output in raw files.
    double *ypp = cubic_hermite_set(n, x, y);
    if (ypp==NULL) return CT_ERROR;

    //Now build a table
    int val;
    double res = 1.0/(double)(sample->m_samplingRes-1);

    //allocate enough space for the samples
    //DEBUG_PRINT("DEBUG: SAMPLING ALLOCATION: %u bytes\n",
     //       sample->m_samplingRes*sizeof(int));
    //DEBUG_PRINT("DEBUG: SAMPLING OUTPUT RANGE: 0 -> %u\n", sample->m_outputRes);

    // sample->m_Samples = (unsigned short int *)realloc(sample->m_Samples,
        // sample->m_samplingRes * sizeof(short int));
    // nc_merror(sample->m_Samples, "CurveDataSample");

    int firstPointX = x[0] * (sample->m_samplingRes-1);
    int firstPointY = y[0] * (sample->m_outputRes-1);
    int lastPointX = x[n-1] * (sample->m_samplingRes-1);
    int lastPointY = y[n-1] * (sample->m_outputRes-1);
    int maxY = curve->m_max_y * (sample->m_outputRes-1);
    int minY = curve->m_min_y * (sample->m_outputRes-1);

    for(i = 0; i < (int)sample->m_samplingRes; i++)
    {
    //get the value of the curve at a point
    //take into account that curves may not necessarily begin at x = 0.0
    //nor end at x = 1.0

    //Before the first point and after the last point, take a strait line
    if (i < firstPointX) {
        sample->m_Samples[i] = firstPointY;
    } else if (i > lastPointX) {
        sample->m_Samples[i] = lastPointY;
    } else {
        //within range, we can sample the curve
        val = cubic_hermite_val( n, x, i*res, y,
            ypp) * (sample->m_outputRes-1) + 0.5;
        sample->m_Samples[i] = MIN(MAX(val,minY),maxY);
    }
    }

    free(ypp);
    return CT_SUCCESS;
}

#endif 
