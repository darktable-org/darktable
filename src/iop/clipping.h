/*
    This file is part of darktable,
    copyright (c) 2011--2012 henrik andersson.
    copyright (c) 2012 ulrich pegelow.

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

#ifndef DT_CLIPPING_H
#define DT_CLIPPING_H

typedef struct dt_iop_clipping_params_t
{
  float angle, cx, cy, cw, ch, k_h, k_v;
  float kxa, kya, kxb, kyb, kxc, kyc, kxd, kyd;
  int k_type, k_sym;
  int k_apply, crop_auto;
}
dt_iop_clipping_params_t;

#endif
