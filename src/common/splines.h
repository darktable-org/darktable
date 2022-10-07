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

#pragma once

#if defined(__cplusplus)
extern "C"
{
#endif

#include "curve_tools.h"

  float interpolate_val_V2(int n, CurveAnchorPoint Points[], float x, unsigned int type);
  float interpolate_val_V2_periodic(int n, CurveAnchorPoint Points[], float x, unsigned int type, float period);

  int CurveDataSampleV2(CurveData *curve, CurveSample *sample);
  int CurveDataSampleV2Periodic(CurveData *curve, CurveSample *sample);

#if defined(__cplusplus)
}
#endif
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

