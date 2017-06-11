/*
    This file is part of darktable,
    copyright (c) 2014 Marcello Perathoner.

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

#include "common.h"

typedef struct dt_iop_roi_t {
  int x, y, width, height;
  float scale;
} dt_iop_roi_t;

typedef struct {
    int x, y;
    int width, height;
} cairo_rectangle_int_t;

typedef struct {
  int size;
  int resolution;
} dt_liquify_kernel_descriptor_t;


float kmix (global const float *k,
	    global const dt_liquify_kernel_descriptor_t* kdesc,
	    float t)
{
  t = fabs (t * kdesc->resolution);
  float flor;
  t = fract (t, &flor);
  int i = (int) flor;
  return mix (k[i], k[i+1], t);
}

/**
 * Image convolution with a discrete kernel.
 *
 * Given a one-dimensional signal with samples \f$s_i\f$, for integer
 * values of \f$i\f$, the value \f$S(x)\f$ interpolated at an
 * arbitrary real argument \f$x\f$ is obtained by the discrete
 * convolution of those samples with the kernel; namely,
 *
 *    \f{S(x) = \sum_{i=\lfloor x \rfloor - a + 1}^{\lfloor x \rfloor + a}
 *       s_{i} K(x - i),\f}
 *
 * where a is the filter size parameter and \f$\lfloor x \rfloor\f$ is
 * the floor function. The bounds of this sum are such that the kernel
 * is zero outside of them.
 *
 * @param in
 * @param out
 * @param roi_in
 * @param roi_out
 * @param map
 * @param map_extent
 * @param kdesc       Kernel description.
 * @param k           Discrete kernel.
 */

kernel void
warp_kernel (read_only image2d_t in,
	     write_only image2d_t out,
	     global dt_iop_roi_t *roi_in,
	     global dt_iop_roi_t *roi_out,
	     global float2 *map,
	     global cairo_rectangle_int_t *map_extent,
	     global dt_liquify_kernel_descriptor_t *kdesc,
	     global float *k)
{
  int2 pos = (int2) (get_global_id (0), get_global_id (1));

  // stop surplus workers in the last workgroup
  if (pos.x >= map_extent->width || pos.y >= map_extent->height)
    return;

  float2 warp = map[pos.y * map_extent->width + pos.x];

  const int2 map_origin = (int2) (map_extent->x, map_extent->y);
  pos += map_origin;

  // roi_in >= roi_out, so we only have to check roi_out
  if (pos.x < roi_out->x || pos.x >= roi_out->x + roi_out->width
      || pos.y < roi_out->y || pos.y >= roi_out->y + roi_out->height)
    return;

  if (warp.x == 0.0f && warp.y == 0.0f)
    return;

  const int2 roi_in_origin  = (int2) (roi_in->x,    roi_in->y);
  const int2 roi_out_origin = (int2) (roi_out->x,   roi_out->y);

  float2 in_pos = convert_float2 (pos - roi_in_origin) + warp;

  const int a = kdesc->size;      // half the kernel width

  float2 lkernel[6];              // 2 * the biggest a
  float2 *lk = lkernel + a - 1;
  float2 norm = (float2) 0.0f;
  for (int i = 1 - a; i <= a; ++i)
  {
    norm.x += (lk[i].x = kmix (k, kdesc, in_pos.x - floor (in_pos.x) - i));
    norm.y += (lk[i].y = kmix (k, kdesc, in_pos.y - floor (in_pos.y) - i));
  }

  in_pos = floor (in_pos);

  int2 sample_pos;
  float4 Sxy = (float4) 0.0f;

  // loop over support region (eg. 6x6 pixels for lanczos3)
  for (sample_pos.y = 1 - a; sample_pos.y <= a; ++sample_pos.y)
    for (sample_pos.x = 1 - a; sample_pos.x <= a; ++sample_pos.x)
      Sxy += (read_imagef (in, sampleri, in_pos + convert_float2 (sample_pos))
	      * lk[sample_pos.x].x * lk[sample_pos.y].y);

  Sxy = Sxy / (norm.x * norm.y);
  Sxy = clamp (Sxy, 0.0f, 1.0f);

  write_imagef (out, pos - roi_out_origin, Sxy);
}
