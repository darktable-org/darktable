/*
    This file is part of darktable,
    copyright (c) 2019 Heiko Bauke.

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

#if defined(__cplusplus)
extern "C" {
#endif

#define CT_SUCCESS 0
#define CT_ERROR 100

#define CUBIC_SPLINE 0
#define CATMULL_ROM 1
#define MONOTONE_HERMITE 2

#define MAX_ANCHORS 20


typedef struct {
  float x;
  float y;
} CurveAnchorPoint;


typedef struct {
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

typedef struct {
  // Number of samples to use for the curve.
  unsigned int m_samplingRes;
  unsigned int m_outputRes;

  // Sampling array
  unsigned short int *m_Samples; // jo: changed to short int to save memory

} CurveSample;

float interpolate_val(int n, CurveAnchorPoint Points[], float x, unsigned int type);
float interpolate_val_periodic(int n, CurveAnchorPoint Points[], float x, unsigned int type, float period);

int CurveDataSample(CurveData *curve, CurveSample *sample);
int CurveDataSamplePeriodic(CurveData *curve, CurveSample *sample);

#if defined(__cplusplus)
}
#endif
