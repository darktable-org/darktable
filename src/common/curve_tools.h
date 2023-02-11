/*
    This file is part of darktable,
    Copyright (C) 2011-2020 darktable developers.

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

    part of this file is based on nikon_curve.h from UFraw
    Copyright 2004-2008 by Shawn Freeman, Udi Fuchs
*/

#pragma once

// Curve Types
#define CUBIC_SPLINE 0
#define CATMULL_ROM 1
#define MONOTONE_HERMITE 2

// Maximum resolution allowed due to space considerations.
#define MAX_RESOLUTION 65536
#define MAX_ANCHORS 20

// ERROR CODES
#define CT_SUCCESS 0
#define CT_ERROR 100
#define CT_WARNING 104
#define CT_SET_ERROR 200


//////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////
// DATA STRUCTURES
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
  // Type for this curve
  unsigned int m_spline_type;

  // Box data
  float m_min_x;
  float m_max_x;
  float m_min_y;
  float m_max_y;

  // Number of anchor points
  unsigned char m_numAnchors;

  // contains a list of anchors, 2 floats per each point, x-y format
  // max is 20 points
  CurveAnchorPoint m_anchors[MAX_ANCHORS];

} CurveData;

typedef struct
{
  // Number of samples to use for the curve.
  unsigned int m_samplingRes;
  unsigned int m_outputRes;

  // Sampling array
  unsigned short int *m_Samples; // jo: changed to short int to save memory

} CurveSample;

/*********************************************
CurveDataSample:
    Samples from a spline curve constructed from
    the curve data.

    curve   - Pointer to curve struct to hold the data.
    sample  - Pointer to sample struct to hold the data.
**********************************************/
int CurveDataSample(CurveData *curve, CurveSample *sample);

/***************************************************************
 * interpolate_set:
 *
 * convenience function for calculating the necessary parameters for
 * interpolation.
 *
 * input:
 *      n    - length of data arrays
 *      x    - x axis of the data array
 *      y    - y axis of the data array
 *      type - type of interpolation currently either CUBIC or HERMITE
 * output:
 *      ypp  - pointer to array of parameters
 *******************************************************************/
float *interpolate_set(int n, float x[], float y[], unsigned int type);

/***************************************************************
 * interpolate_val:
 *
 * convenience function for piecewise interpolation
 *
 * input:
 *      n    - length of data arrays
 *      x    - x axis of the data array
 *      xval - point where to interpolate
 *      y    - y axis of the data array
 *      tangents - parameters calculated with interpolate_set
 *      type - type of interpolation currently either CUBIC or HERMITE
 * output:
 *      yval  - interpolated value at xval
 *******************************************************************/
float interpolate_val(int n, float x[], float xval, float y[], float tangents[], unsigned int type);

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
