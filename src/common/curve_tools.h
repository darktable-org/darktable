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
    
    part of this file is based on nikon_curve.h  from UFraw
    Copyright 2004-2008 by Shawn Freeman, Udi Fuchs
*/

//Curve Types
#define TONE_CURVE      0
#define RED_CURVE       1
#define GREEN_CURVE     2
#define BLUE_CURVE      3
#define NUM_CURVE_TYPES 4

//Maximum resoltuion allowed due to space considerations.
#define MAX_RESOLUTION    65536
#define MAX_ANCHORS 20

//ERROR CODES
#define CT_SUCCESS  0
#define CT_ERROR    100
#define CT_WARNING  104
#define CT_SET_ERROR    200
 

//////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////
//DATA STRUCTURES
//////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////
/**********************************************************
CurveData:
    Structure for the curve data inside a NTC/NCV file.
***********************************************************/
typedef struct
{
    float x;
    float y;
} CurveAnchorPoint;

typedef struct
{
    //Type for this curve
    unsigned int m_curveType;

    //Box data
    float m_min_x;
    float m_max_x;
    float m_min_y;
    float m_max_y;

    //Number of anchor points
    unsigned char m_numAnchors;

    //contains a list of anchors, 2 floats per each point, x-y format
    //max is 20 points
    CurveAnchorPoint m_anchors[MAX_ANCHORS];

} CurveData;

typedef struct
{
    //Number of samples to use for the curve.
    unsigned int m_samplingRes;
    unsigned int m_outputRes;

    //Sampling array
    unsigned short int *m_Samples; // jo: changed to short int to save memory

} CurveSample;

/*******************************************************************
 d3_np_fs:
   Helper function for calculating and storing tridiagnol matrices.
   Cubic spline calculations involve these types of matrices.
*********************************************************************/
float *d3_np_fs ( int n, float a[], float b[] );

/*******************************************************************
 spline_cubic_set:
   spline_cubic_set gets the second derivatives for the curve to be used in
   spline construction

    n = number of control points
    t[] = x array
    y[] = y array
    ibcbeg = initial point condition (see function notes).
    ybcbeg = beginning value depending on above flag
    ibcend = end point condition (see function notes).
    ybcend = end value depending on above flag

    returns the y value at the given tval point
*********************************************************************/
float *spline_cubic_set ( int n, float t[], float y[], int ibcbeg,
    float ybcbeg, int ibcend, float ybcend );
/*******************************************************************
 spline_cubic_val:
   spline_cubic_val gets a value from spline curve.

    n = number of control points
    t[] = x array
    tval = x value you're requesting the data for, can be anywhere on the interval.
    y[] = y array
    ypp[] = second derivative array calculated by spline_cubic_set
    ypval = first derivative value of requested point
    yppval = second derivative value of requested point

    returns the y value at the given tval point
*********************************************************************/
float spline_cubic_val ( int n, float t[], float tval, float y[],
    float ypp[], float *ypval, float *yppval );
 /*************************************************************
 * cubic_hermite_set:
 *      calculates the tangents for the hermite spline curve.
 *
 *  input:
 *      n = number of control points
 *      x = input x array
 *      y = input y array
 *  output:
 *      pointer to array containing the tangents
 *************************************************************/
float *cubic_hermite_set ( int n, float x[], float y[]);

/*************************************************************
 * cubic_hermite_val:
 *      calculates the tangents for the hermite spline curve.
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
float cubic_hermite_val ( int n, float x[], float xval, float y[],
        float tangent []);

/*********************************************
CurveDataSample:
    Samples from a spline curve constructed from
    the curve data.

    curve   - Pointer to curve struct to hold the data.
    sample  - Pointer to sample struct to hold the data.
**********************************************/
int CurveDataSample(CurveData *curve, CurveSample *sample);

/************************************************************
 * CurveDataSample_hermite:
 *      Samples from a hermite curve constructed from the curve data.
 *      this is an adjusted CurveDataSample to use the hermite functions
 *
 *      curve = Pointer to the curve struct to hold the data
 *      sample = Pointer to the samples struct to hold the data
 *************************************************************/
int CurveDataSample_hermite (CurveData *curve, CurveSample *sample);
 
