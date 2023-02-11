/*
 *    This file is part of darktable,
 *    Copyright (C) 2016-2020 darktable developers.
 *
 *    darktable is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 *
 *    darktable is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <inttypes.h>

typedef struct tonecurve_t
{
  double *x;   // input L positions, assumed to be strictly monotonic x[i+1] > x[i]
  double *y;   // output L values, assumed to be monotonic y[i+1] >= y[i]
  int32_t num; // number of values
} tonecurve_t;

void tonecurve_create(tonecurve_t *c, double *Lin, double *Lout, const int32_t num);

void tonecurve_delete(tonecurve_t *c);

double tonecurve_apply(const tonecurve_t *c, const double L);


double tonecurve_unapply(const tonecurve_t *c, const double L);

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
