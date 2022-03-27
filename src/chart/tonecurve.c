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

#include "chart/tonecurve.h"

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>

// apply and undo a tone curve (L channel only),
// created from the 24 grey input patches from the it8.

void tonecurve_create(tonecurve_t *c, double *Lin, double *Lout, const int32_t num)
{
  c->num = num;
  c->x = Lin;
  c->y = Lout;
}

void tonecurve_delete(tonecurve_t *c)
{
  if(!c) return;

  free(c->y);
  free(c->x);
}

static inline double _tonecurve_apply(const double *x, const double *y, const int32_t num, const double L)
{
  if(L <= 0.0 || L >= 100.0) return L;
  uint32_t min = 0, max = num;
  uint32_t t = max / 2;
  while(t != min)
  {
    if(x[t] <= L)
      min = t;
    else
      max = t;
    t = (min + max) / 2;
  }
  assert(t < num);
  // last step: decide between min and max one more time (min is rounding default),
  // but consider that if max is still out of bounds, it's invalid.
  // (L == 1.0 and a x[0]=1, num=1 would break otherwise)
  if(max < num && x[max] <= L) t = max;
  const double f = (x[t + 1] - x[t] > 1e-6f) ? (L - x[t]) / (x[t + 1] - x[t]) : 1.0f;
  if(t == num - 1) return y[t];
  assert(x[t] <= L);
  assert(x[t + 1] >= L);
  return y[t + 1] * f + y[t] * (1.0f - f);
}

double tonecurve_apply(const tonecurve_t *c, const double L)
{
  return _tonecurve_apply(c->x, c->y, c->num, L);
}

double tonecurve_unapply(const tonecurve_t *c, const double L)
{
  return _tonecurve_apply(c->y, c->x, c->num, L);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

