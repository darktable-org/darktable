/*
    This file is part of darktable,
    copyright (c) 2017 edgardo hoszowski.

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

#include "colorspace.cl"
#include "common.h"

typedef struct dt_iop_roi_t
{
  int x, y, width, height;
  float scale;
} dt_iop_roi_t;

kernel void
retouch_clear_alpha(global float4 *in, int width, int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  const int idx = mad24(y, width, x);
  in[idx].w = 0.f;
}

kernel void
retouch_copy_alpha(__read_only image2d_t in, global float4 *out, int width, int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 pixel = read_imagef(in, sampleri, (int2)(x, y));
  const int idx = mad24(y, width, x);
  out[idx].w = pixel.w;
}

kernel void
retouch_copy_buffer_to_image(global float4 *in, global dt_iop_roi_t *roi_in,
                                         __write_only image2d_t out, global dt_iop_roi_t *roi_out, const int xoffs,
                                         const int yoffs)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= roi_out->width || y >= roi_out->height) return;
  if(x + xoffs >= roi_in->width || y + yoffs >= roi_in->height) return;

  const int idx = mad24(y + yoffs, roi_in->width, x + xoffs);
  write_imagef(out, (int2)(x, y), in[idx]);
}

kernel void
retouch_copy_buffer_to_buffer(global float4 *in, global dt_iop_roi_t *roi_in, global float4 *out,
                                          global dt_iop_roi_t *roi_out, const int xoffs, const int yoffs)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= roi_out->width || y >= roi_out->height) return;
  if(x + xoffs >= roi_in->width || y + yoffs >= roi_in->height) return;

  out[mad24(y, roi_out->width, x)] = in[mad24(y + yoffs, roi_in->width, x + xoffs)];
}

kernel void
retouch_copy_mask_to_alpha(global float4 *in, global dt_iop_roi_t *roi_in, global float *mask_scaled,
                                       global dt_iop_roi_t *roi_mask_scaled, const float opacity)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= roi_mask_scaled->width || y >= roi_mask_scaled->height) return;

  const int mask_index = mad24(y, roi_mask_scaled->width, x);
  const int dest_index
      = mad24(y + roi_mask_scaled->y - roi_in->y, roi_in->width, x + roi_mask_scaled->x - roi_in->x);

  const float f = mask_scaled[mask_index] * opacity;

  if(f > in[dest_index].w) in[dest_index].w = f;
}

kernel void
retouch_fill(global float4 *in, global dt_iop_roi_t *roi_in, global float *mask_scaled,
                         global dt_iop_roi_t *roi_mask_scaled, const float opacity, const float color_x,
                         const float color_y, const float color_z)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= roi_mask_scaled->width || y >= roi_mask_scaled->height) return;

  const int mask_index = mad24(y, roi_mask_scaled->width, x);
  const int dest_index
      = mad24(y + roi_mask_scaled->y - roi_in->y, roi_in->width, x + roi_mask_scaled->x - roi_in->x);

  float4 fill_color = { color_x, color_y, color_z, 0.f };
  const float f = mask_scaled[mask_index] * opacity;
  const float w = in[dest_index].w;

  in[dest_index] = in[dest_index] * (1.0f - f) + fill_color * f;

  in[dest_index].w = w;
}

kernel void
retouch_copy_buffer_to_buffer_masked(global float4 *buffer_src, global float4 *buffer_dest,
                                                 global dt_iop_roi_t *roi_buffer_dest, global float *mask_scaled,
                                                 global dt_iop_roi_t *roi_mask_scaled, const float opacity)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= roi_mask_scaled->width || y >= roi_mask_scaled->height) return;

  const int idx_dest = mad24(y + roi_mask_scaled->y - roi_buffer_dest->y, roi_buffer_dest->width,
                             x + roi_mask_scaled->x - roi_buffer_dest->x);
  const int idx_src = mad24(y, roi_mask_scaled->width, x);
  const int idx_mask = mad24(y, roi_mask_scaled->width, x);

  const float f = mask_scaled[idx_mask] * opacity;
  const float w = buffer_dest[idx_dest].w;

  buffer_dest[idx_dest] = buffer_dest[idx_dest] * (1.0f - f) + buffer_src[idx_src] * f;

  buffer_dest[idx_dest].w = w;
}

kernel void
retouch_copy_image_to_buffer_masked(__read_only image2d_t buffer_src, global float4 *buffer_dest,
                                                global dt_iop_roi_t *roi_buffer_dest, global float *mask_scaled,
                                                global dt_iop_roi_t *roi_mask_scaled, const float opacity)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= roi_mask_scaled->width || y >= roi_mask_scaled->height) return;

  const int idx_dest = mad24(y + roi_mask_scaled->y - roi_buffer_dest->y, roi_buffer_dest->width,
                             x + roi_mask_scaled->x - roi_buffer_dest->x);
  const int idx_mask = mad24(y, roi_mask_scaled->width, x);

  const float f = mask_scaled[idx_mask] * opacity;
  const float w = buffer_dest[idx_dest].w;

  float4 pix = read_imagef(buffer_src, (int2)(x, y));
  buffer_dest[idx_dest] = buffer_dest[idx_dest] * (1.0f - f) + pix * f;

  buffer_dest[idx_dest].w = w;
}

kernel void
retouch_image_rgb2lab(global float4 *in, int width, int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  const int idx = mad24(y, width, x);

  in[idx] = XYZ_to_Lab(sRGB_to_XYZ(in[idx]));
}

kernel void
retouch_image_lab2rgb(global float4 *in, int width, int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  const int idx = mad24(y, width, x);

  in[idx] = XYZ_to_sRGB(Lab_to_XYZ(in[idx]));
}
