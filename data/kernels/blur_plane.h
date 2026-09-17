/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

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

/* Separable gaussian over a single-channel plane, as OpenCL kernels.
 *
 * The device counterpart of sf_blur_plane1 in common/spektra_core.c, and
 * written to match it exactly: the weights are the dt_gaussian_kernel_1d
 * table the host convolves with, uploaded by the caller, and the loop uses
 * the same clamp-to-edge and the same accumulation order as
 * _sf_gauss_convolve_1d, so a module using both agrees bit-for-bit across
 * its CPU and GPU paths.
 */

#pragma once

__kernel void gauss_row_1c(__global const float *src,
                           __global float *dst,
                           const int w,
                           const int h,
                           __global const float *weights,
                           const int radius)
{
  const int x = get_global_id(0), y = get_global_id(1);
  if(x >= w || y >= h) return;
  float acc = 0.0f;
  for(int k = -radius; k <= radius; k++)
  {
    int xx = x + k;
    xx = xx < 0 ? 0 : (xx >= w ? w - 1 : xx);
    acc += weights[k + radius] * src[(size_t)y * w + xx];
  }
  dst[(size_t)y * w + x] = acc;
}

__kernel void gauss_col_1c(__global const float *src,
                           __global float *dst,
                           const int w,
                           const int h,
                           __global const float *weights,
                           const int radius)
{
  const int x = get_global_id(0), y = get_global_id(1);
  if(x >= w || y >= h) return;
  float acc = 0.0f;
  for(int k = -radius; k <= radius; k++)
  {
    int yy = y + k;
    yy = yy < 0 ? 0 : (yy >= h ? h - 1 : yy);
    acc += weights[k + radius] * src[(size_t)yy * w + x];
  }
  dst[(size_t)y * w + x] = acc;
}
