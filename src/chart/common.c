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

#include "chart/common.h"
#include "iop/gaussian_elimination.h"

// using SVD to solve the system with h[8] also being 0 would be better, but this seems to be good enough
int get_homography(const point_t *source, const point_t *target, double *h)
{
  const float x1 = source[0].x;
  const float y1 = source[0].y;
  const float x2 = source[1].x;
  const float y2 = source[1].y;
  const float x3 = source[2].x;
  const float y3 = source[2].y;
  const float x4 = source[3].x;
  const float y4 = source[3].y;

  const float x_1 = target[0].x;
  const float y_1 = target[0].y;
  const float x_2 = target[1].x;
  const float y_2 = target[1].y;
  const float x_3 = target[2].x;
  const float y_3 = target[2].y;
  const float x_4 = target[3].x;
  const float y_4 = target[3].y;

  double P[9*9] = { -x1, -y1, -1.0, 0.0, 0.0,  0.0, x1 * x_1, y1 * x_1, x_1,
                    0.0, 0.0,  0.0, -x1, -y1, -1.0, x1 * y_1, y1 * y_1, y_1,
                    -x2, -y2, -1.0, 0.0, 0.0,  0.0, x2 * x_2, y2 * x_2, x_2,
                    0.0, 0.0,  0.0, -x2, -y2, -1.0, x2 * y_2, y2 * y_2, y_2,
                    -x3, -y3, -1.0, 0.0, 0.0,  0.0, x3 * x_3, y3 * x_3, x_3,
                    0.0, 0.0,  0.0, -x3, -y3, -1.0, x3 * y_3, y3 * y_3, y_3,
                    -x4, -y4, -1.0, 0.0, 0.0,  0.0, x4 * x_4, y4 * x_4, x_4,
                    0.0, 0.0,  0.0, -x4, -y4, -1.0, x4 * y_4, y4 * y_4, y_4,
                    0.0, 0.0,  0.0, 0.0, 0.0,  0.0,      0.0,      0.0, 1.0};

  for(int i = 0; i < 8; i++) h[i] = 0.0;
  h[8] = 1.0;

  return gauss_solve(P, h, 9);
}

point_t apply_homography(point_t p, const double *h)
{
  const double s =  p.x * h[2 * 3 + 0] + p.y * h[2 * 3 + 1] + h[2 * 3 + 2];
  const double x = (p.x * h[0 * 3 + 0] + p.y * h[0 * 3 + 1] + h[0 * 3 + 2]) / s;
  const double y = (p.x * h[1 * 3 + 0] + p.y * h[1 * 3 + 1] + h[1 * 3 + 2]) / s;

  const point_t result = {.x=x, .y=y};

  return result;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces
// modified;
