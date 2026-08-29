/*
    This file is part of darktable,
    Copyright (C) 2025 darktable developers.

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

/* Bilinear interpolators, GPU counterpart of interpolate_bilinear() in
   common/fast_guided_filter.h.

   bilinear1/2/4 take global buffers and are used to down- and upscale the grey
   working buffers of the guided filters, see dt_interpolate_bilinear_cl().
   bilinear_image does the same for image2d_t, see
   dt_interpolate_bilinear_image_cl(). Both are in common/bilinear.h */

#define DT_BILINEAR_KERNEL(SUFFIX, TYPE)                                 \
kernel void bilinear##SUFFIX(global const TYPE *const in,                \
                             const int width_in,                         \
                             const int height_in,                        \
                             global TYPE *const out,                     \
                             const int width_out,                        \
                             const int height_out)                       \
{                                                                        \
  const int x = get_global_id(0);                                        \
  const int y = get_global_id(1);                                        \
  if(x >= width_out || y >= height_out) return;                          \
                                                                         \
  /* Relative coordinates of the pixel in output space */                \
  const float x_out = (float)x / (float)width_out;                       \
  const float y_out = (float)y / (float)height_out;                      \
                                                                         \
  /* Corresponding absolute coordinates of the pixel in input space */   \
  const float x_in = x_out * (float)width_in;                            \
  const float y_in = y_out * (float)height_in;                           \
                                                                         \
  /* Nearest neighbours coordinates in input space */                    \
  int x_prev = (int)floor(x_in);                                         \
  int x_next = x_prev + 1;                                               \
  int y_prev = (int)floor(y_in);                                         \
  int y_next = y_prev + 1;                                               \
                                                                         \
  x_prev = (x_prev < width_in) ? x_prev : width_in - 1;                  \
  x_next = (x_next < width_in) ? x_next : width_in - 1;                  \
  y_prev = (y_prev < height_in) ? y_prev : height_in - 1;                \
  y_next = (y_next < height_in) ? y_next : height_in - 1;                \
                                                                         \
  /* Nearest pixels in input array (nodes in grid) */                    \
  const TYPE Q_NW = in[mad24(y_prev, width_in, x_prev)];                 \
  const TYPE Q_NE = in[mad24(y_prev, width_in, x_next)];                 \
  const TYPE Q_SE = in[mad24(y_next, width_in, x_next)];                 \
  const TYPE Q_SW = in[mad24(y_next, width_in, x_prev)];                 \
                                                                         \
  /* Spatial differences between nodes */                                \
  const float Dy_next = (float)y_next - y_in;                            \
  const float Dy_prev = 1.0f - Dy_next; /* because next - prev = 1 */    \
  const float Dx_next = (float)x_next - x_in;                            \
  const float Dx_prev = 1.0f - Dx_next; /* because next - prev = 1 */    \
                                                                         \
  out[mad24(y, width_out, x)] =                                          \
      Dy_prev * (Q_SW * Dx_next + Q_SE * Dx_prev)                        \
    + Dy_next * (Q_NW * Dx_next + Q_NE * Dx_prev);                       \
}

DT_BILINEAR_KERNEL(1, float)
DT_BILINEAR_KERNEL(2, float2)
DT_BILINEAR_KERNEL(4, float4)

// works correctly with 1-4 channel float images
kernel void bilinear_image(read_only image2d_t in,
                           const int width_in,
                           const int height_in,
                           write_only image2d_t out,
                           const int width_out,
                           const int height_out)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width_out || y >= height_out) return;

  // Relative coordinates of the pixel in output space
  const float x_out = (float)x /(float)width_out;
  const float y_out = (float)y /(float)height_out;

  // Corresponding absolute coordinates of the pixel in input space
  const float x_in = x_out * (float)width_in;
  const float y_in = y_out * (float)height_in;

  // Nearest neighbours coordinates in input space
  int x_prev = clamp((int)floor(x_in), 0, width_in - 1);
  int x_next = clamp(x_prev + 1, 0, width_in - 1);
  int y_prev = clamp((int)floor(y_in), 0, height_in - 1);
  int y_next = clamp(y_prev + 1, 0, height_in - 1);

  // Nearest pixels in input array (nodes in grid)
  const float4 Q_NW = Areadpixel(in, x_prev, y_prev);
  const float4 Q_NE = Areadpixel(in, x_next, y_prev);
  const float4 Q_SE = Areadpixel(in, x_next, y_next);
  const float4 Q_SW = Areadpixel(in, x_prev, y_next);

  // Spatial differences between nodes
  const float Dy_next = (float)y_next - y_in;
  const float Dy_prev = 1.f - Dy_next; // because next - prev = 1
  const float Dx_next = (float)x_next - x_in;
  const float Dx_prev = 1.f - Dx_next; // because next - prev = 1

  // Interpolate
  const float4 pix_out = Dy_prev * (Q_SW * Dx_next + Q_SE * Dx_prev) +
                         Dy_next * (Q_NW * Dx_next + Q_NE * Dx_prev);

  // Full RGBa copy - 4 channels
  write_imagef(out, (int2)(x, y), pix_out);
}
