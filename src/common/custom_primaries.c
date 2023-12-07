/*
 *    This file is part of darktable,
 *    Copyright (C) 2023 darktable developers.
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

#include "common/custom_primaries.h"
#include <float.h>


static inline float _determinant(const float a,
                                 const float b,
                                 const float c,
                                 const float d)
{
  return a * d - b * c;
}

static inline float _intersect_line_segments(const float x1,
                                             const float y1,
                                             const float x2,
                                             const float y2,
                                             const float x3,
                                             const float y3,
                                             const float x4,
                                             const float y4)
{
  const float denominator = _determinant(x1 - x2, x3 - x4, y1 - y2, y3 - y4);
  if(denominator == 0.f)
    return FLT_MAX; // lines don't intersect

  const float t = _determinant(x1 - x3, x3 - x4, y1 - y3, y3 - y4) / denominator;
  if(t >= 0.f)
    return t;
  return FLT_MAX; // intersection is in the wrong direction
}

static inline float _find_distance_to_edge
  (const dt_iop_order_iccprofile_info_t *const profile,
   const float cos_angle,
   const float sin_angle)
{
  const float x1 = profile->whitepoint[0];
  const float y1 = profile->whitepoint[1];
  const float x2 = x1 + cos_angle;
  const float y2 = y1 + sin_angle;

  float distance_to_edge = FLT_MAX;
  for(size_t i = 0; i < 3; i++)
  {
    const size_t next_i = i == 2 ? 0 : i + 1;
    const float x3 = profile->primaries[i][0];
    const float y3 = profile->primaries[i][1];
    const float x4 = profile->primaries[next_i][0];
    const float y4 = profile->primaries[next_i][1];
    const float distance = _intersect_line_segments(x1, y1, x2, y2, x3, y3, x4, y4);
    if(distance < distance_to_edge)
      distance_to_edge = distance;
  }

  return distance_to_edge;
}

void dt_rotate_and_scale_primary(const dt_iop_order_iccprofile_info_t *const profile,
                                 const float scaling,
                                 const float rotation,
                                 const size_t primary_index,
                                 float new_xy[2])
{
  // Generate a custom set of tone mapping primaries by scaling
  // and rotating the primaries of the given profile.
  const float dx = profile->primaries[primary_index][0] - profile->whitepoint[0];
  const float dy = profile->primaries[primary_index][1] - profile->whitepoint[1];
  const float angle = atan2f(dy, dx) + rotation;
  const float cos_angle = cosf(angle);
  const float sin_angle = sinf(angle);
  const float distance_to_edge = _find_distance_to_edge(profile, cos_angle, sin_angle);
  const float dx_new = scaling * distance_to_edge * cos_angle;
  const float dy_new = scaling * distance_to_edge * sin_angle;
  new_xy[0] = dx_new + profile->whitepoint[0];
  new_xy[1] = dy_new + profile->whitepoint[1];
}
