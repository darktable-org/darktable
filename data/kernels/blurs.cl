/*
    This file is part of darktable,
    copyright (c) 2021-2026 darktable developers.

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

// Dense 2D convolution for lens/motion blurs.
// kern is a flat float buffer (read-only; small enough for GPU L2 cache at typical radii).
kernel void convolve(read_only image2d_t in,
                     __global const float *kern,
                     write_only image2d_t out,
                     const int width,
                     const int height,
                     const int radius,
                     const int kernel_width)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;

  float4 pix_in = read_imagef(in, samplerA, (int2)(x, y));
  float4 acc = 0.f;

  for(int l = -radius; l <= radius; l++)
    for(int m = -radius; m <= radius; m++)
    {
      const int ii = clamp(y + l, 0, height - 1);
      const int jj = clamp(x + m, 0, width - 1);
      const int ik = l + radius;
      const int jk = m + radius;
      acc += kern[ik * kernel_width + jk] * read_imagef(in, samplerA, (int2)(jj, ii));
    }

  acc.w = pix_in.w;
  write_imagef(out, (int2)(x, y), acc);
}

// Sparse 2D convolution: only the non-negligible kernel entries are applied.
// Dramatically faster for motion blur (thin arc kernel, ~1-5% fill at large radii)
// and moderate improvement for lens blur (circular aperture, ~78% fill).
// offsets_x / offsets_y are pixel coordinate deltas; values are the kernel weights.
kernel void convolve_sparse(read_only image2d_t in,
                             __global const int *offsets_x,
                             __global const int *offsets_y,
                             __global const float *values,
                             write_only image2d_t out,
                             const int width,
                             const int height,
                             const int n_entries)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;

  float4 pix_in = read_imagef(in, samplerA, (int2)(x, y));
  float4 acc = 0.f;

  for(int i = 0; i < n_entries; i++)
  {
    const int2 coord = (int2)(clamp(x + offsets_x[i], 0, width - 1),
                              clamp(y + offsets_y[i], 0, height - 1));
    acc += values[i] * read_imagef(in, samplerA, coord);
  }

  acc.w = pix_in.w;
  write_imagef(out, (int2)(x, y), acc);
}

// Copies RGB from blurred and alpha from original into out.
// Used after dt_gaussian_blur_cl to restore the pipeline mask channel.
kernel void restore_alpha(read_only image2d_t original,
                          read_only image2d_t blurred,
                          write_only image2d_t out,
                          const int width,
                          const int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;

  float4 px_blurred = read_imagef(blurred, samplerA, (int2)(x, y));
  float4 px_orig    = read_imagef(original, samplerA, (int2)(x, y));
  px_blurred.w = px_orig.w;
  write_imagef(out, (int2)(x, y), px_blurred);
}
