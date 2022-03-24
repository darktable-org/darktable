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

#include "chart/tonecurve.h"

int thinplate_match(const tonecurve_t *curve, // tonecurve to apply after this (needed for error estimation)
                    int dim,                  // dimensionality of points
                    int N,                    // number of points
                    const double *point,      // dim-strided points
                    const double **target,    // target values, one pointer per dimension
                    int S,                    // desired sparsity level, actual result will be returned
                    int *permutation, // pointing to original order of points, to identify correct output coeff
                    double **coeff,   // output coefficient arrays for each dimension, ordered according to
                                      // permutation[dim]
                    double *avgerr,           // average error
                    double *maxerr);          // max error

float thinplate_color_pos(float L, float a, float b);

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

