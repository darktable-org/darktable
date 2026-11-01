/*
    This file is part of darktable,
    copyright (c) 2026 darktable developers.

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
#include "color_conversion.h"

/* the accumulator is a buffer rather than an image because every kernel of
   the bank adds to it, and a kernel cannot both read and write one image */

/* the scatter mask, and the source less the light that leaves it */
kernel void halation_scatter(read_only image2d_t in,
                             __global float *acc,
                             __global float *scatter,
                             const int width,
                             const int height,
                             const float knee2,
                             const float keep,
                             const float4 gain,
                             constant dt_colorspaces_iccprofile_info_cl_t *profile,
                             read_only image2d_t lut)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;

  const size_t k = (size_t)y * width + x;
  const float4 px = read_imagef(in, sampleri, (int2)(x, y));
  const float lum = get_rgb_matrix_luminance(px, profile, profile->matrix_in, lut);
  const float l2 = lum * lum;
  const float s = l2 / (l2 + knee2);

  scatter[k] = s;

  const float4 o = px - (1.0f - keep) * gain * px * s;
  acc[4 * k + 0] = o.x;
  acc[4 * k + 1] = o.y;
  acc[4 * k + 2] = o.z;
  acc[4 * k + 3] = px.w;
}

/* one channel of the plane the blur bank runs on: the channel carries the
   color, the mask carries where the light is */
kernel void halation_plane(read_only image2d_t in,
                           __global const float *scatter,
                           __global float *plane,
                           const int width,
                           const int height,
                           const int channel)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;

  const size_t k = (size_t)y * width + x;
  const float4 px = read_imagef(in, sampleri, (int2)(x, y));
  const float c = (channel == 0) ? px.x : (channel == 1) ? px.y : px.z;
  plane[k] = c * scatter[k];
}

/* add one blurred kernel of the bank into the channel's halo plane. a plane
   rather than the accumulator directly: acc[4k+c] touches one float per
   cache line, and the bank runs twelve times */
kernel void halation_accumulate(__global float *halo,
                                __global const float *blurred,
                                const int width,
                                const int height,
                                const int seeded,
                                const float weight)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;

  const size_t k = (size_t)y * width + x;
  const float v = weight * blurred[k];
  halo[k] = seeded ? halo[k] + v : v;
}

/* fold one finished halo plane into its channel of the accumulator */
kernel void halation_merge(__global float *acc,
                           __global const float *halo,
                           const int width,
                           const int height,
                           const int channel)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;

  const size_t k = (size_t)y * width + x;
  acc[4 * k + channel] += halo[k];
}

kernel void halation_write(__global const float *acc,
                           write_only image2d_t out,
                           const int width,
                           const int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;

  const size_t k = (size_t)y * width + x;
  write_imagef(out, (int2)(x, y),
               (float4)(acc[4 * k + 0], acc[4 * k + 1], acc[4 * k + 2], acc[4 * k + 3]));
}
