/*
    This file is part of darktable,
    Copyright (C) 2017-2026 darktable developers.

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

#include "colorspace.h"
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
retouch_copy_alpha(__read_only image2d_t in,
                   global float4 *out,
                   int width,
                   int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float alpha = readalpha(in, x, y);
  const int idx = mad24(y, width, x);
  out[idx].w = alpha;
}

kernel void
retouch_copy_buffer_to_image(global float4 *in,
                             global dt_iop_roi_t *roi_in,
                             __write_only image2d_t out,
                             global dt_iop_roi_t *roi_out,
                             const int xoffs,
                             const int yoffs,
                             const float angle,
                             const float cx,
                             const float cy)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= roi_out->width || y >= roi_out->height) return;

  // skip the rotation math for negligibly small angles (cheaper, and avoids
  // resampling artifacts on a near-zero rotation from repeated UI actions)
  if (fabs(angle) < 0.01f)
  {
    if(x + xoffs >= roi_in->width || y + yoffs >= roi_in->height || x + xoffs < 0 || y + yoffs < 0) return;
    const int idx = mad24(y + yoffs, roi_in->width, x + xoffs);
    write_imagef(out, (int2)(x, y), in[idx]);
  }
  else
  {
    // same rotation as the on-screen source outline, so the overlay marks the
    // region actually copied (see rt_copy_in_to_out in retouch.c)
    const float c = dtcl_cos(angle);
    const float s = dtcl_sin(angle);
    // (cx, cy) is the mask centroid (rotation pivot), in roi_out-local coords
    const float cx_source = cx + xoffs;
    const float cy_source = cy + yoffs;

    const float sx = x + xoffs;
    const float sy = y + yoffs;
    const float rx = sx - cx_source;
    const float ry = sy - cy_source;
    const float ix = cx_source + rx * c - ry * s;
    const float iy = cy_source + rx * s + ry * c;

    // Edge-clamp (replicate) out-of-bounds samples instead of zero-filling:
    // rt_compute_roi_in grows roi_in to the rotation-aware source area, but not
    // past the image border, so a rotated source near the edge can still sample
    // just outside roi_in. Replicating avoids a hard black seam.
    const float ixc = clamp(ix, 0.0f, (float)(roi_in->width - 1));
    const float iyc = clamp(iy, 0.0f, (float)(roi_in->height - 1));

    int x0 = (int)ixc;
    int y0 = (int)iyc;
    x0 = clamp(x0, 0, roi_in->width - 2);
    y0 = clamp(y0, 0, roi_in->height - 2);
    const float dx0 = ixc - x0;
    const float dy0 = iyc - y0;

    float4 in00 = in[mad24(y0, roi_in->width, x0)];
    float4 in10 = in[mad24(y0, roi_in->width, x0 + 1)];
    float4 in01 = in[mad24(y0 + 1, roi_in->width, x0)];
    float4 in11 = in[mad24(y0 + 1, roi_in->width, x0 + 1)];

    float4 val = in00 * (1.0f - dx0) * (1.0f - dy0) +
                 in10 * dx0 * (1.0f - dy0) +
                 in01 * (1.0f - dx0) * dy0 +
                 in11 * dx0 * dy0;
    write_imagef(out, (int2)(x, y), val);
  }
}

kernel void
retouch_copy_buffer_to_buffer(global float4 *in,
                              global dt_iop_roi_t *roi_in,
                              global float4 *out,
                              global dt_iop_roi_t *roi_out,
                              const int xoffs,
                              const int yoffs,
                              const float angle,
                              const float cx,
                              const float cy)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= roi_out->width || y >= roi_out->height) return;

  // skip the rotation math for negligibly small angles (cheaper, and avoids
  // resampling artifacts on a near-zero rotation from repeated UI actions)
  if (fabs(angle) < 0.01f)
  {
    if(x + xoffs >= roi_in->width || y + yoffs >= roi_in->height || x + xoffs < 0 || y + yoffs < 0) return;
    out[mad24(y, roi_out->width, x)] = in[mad24(y + yoffs, roi_in->width, x + xoffs)];
  }
  else
  {
    // same rotation as the on-screen source outline, so the overlay marks the
    // region actually copied (see rt_copy_in_to_out in retouch.c)
    const float c = dtcl_cos(angle);
    const float s = dtcl_sin(angle);
    // (cx, cy) is the mask centroid (rotation pivot), in roi_out-local coords
    const float cx_source = cx + xoffs;
    const float cy_source = cy + yoffs;

    const float sx = x + xoffs;
    const float sy = y + yoffs;
    const float rx = sx - cx_source;
    const float ry = sy - cy_source;
    const float ix = cx_source + rx * c - ry * s;
    const float iy = cy_source + rx * s + ry * c;

    // Edge-clamp (replicate) out-of-bounds samples instead of zero-filling:
    // rt_compute_roi_in grows roi_in to the rotation-aware source area, but not
    // past the image border, so a rotated source near the edge can still sample
    // just outside roi_in. Replicating avoids a hard black seam.
    const float ixc = clamp(ix, 0.0f, (float)(roi_in->width - 1));
    const float iyc = clamp(iy, 0.0f, (float)(roi_in->height - 1));

    int x0 = (int)ixc;
    int y0 = (int)iyc;
    x0 = clamp(x0, 0, roi_in->width - 2);
    y0 = clamp(y0, 0, roi_in->height - 2);
    const float dx0 = ixc - x0;
    const float dy0 = iyc - y0;

    float4 in00 = in[mad24(y0, roi_in->width, x0)];
    float4 in10 = in[mad24(y0, roi_in->width, x0 + 1)];
    float4 in01 = in[mad24(y0 + 1, roi_in->width, x0)];
    float4 in11 = in[mad24(y0 + 1, roi_in->width, x0 + 1)];

    float4 val = in00 * (1.0f - dx0) * (1.0f - dy0) +
                 in10 * dx0 * (1.0f - dy0) +
                 in01 * (1.0f - dx0) * dy0 +
                 in11 * dx0 * dy0;
    out[mad24(y, roi_out->width, x)] = val;
  }
}

kernel void
retouch_copy_mask_to_alpha(global float4 *in,
                           global dt_iop_roi_t *roi_in,
                           global float *mask_scaled,
                           global dt_iop_roi_t *roi_mask_scaled,
                           const float opacity)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= roi_mask_scaled->width || y >= roi_mask_scaled->height) return;

  const int mask_index = mad24(y, roi_mask_scaled->width, x);
  const int dest_index = mad24(y + roi_mask_scaled->y - roi_in->y, roi_in->width, x + roi_mask_scaled->x - roi_in->x);

  const float f = clipf(mask_scaled[mask_index] * opacity);

  if(f > in[dest_index].w) in[dest_index].w = f;
}

kernel void
retouch_fill(global float4 *in,
             global dt_iop_roi_t *roi_in,
             global float *mask_scaled,
             global dt_iop_roi_t *roi_mask_scaled,
             const float opacity,
             const float color_x,
             const float color_y,
             const float color_z)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= roi_mask_scaled->width || y >= roi_mask_scaled->height) return;

  const int mask_index = mad24(y, roi_mask_scaled->width, x);
  const int dest_index = mad24(y + roi_mask_scaled->y - roi_in->y, roi_in->width, x + roi_mask_scaled->x - roi_in->x);

  float4 fill_color = { color_x, color_y, color_z, 0.f };
  const float f = clipf(mask_scaled[mask_index] * opacity);
  const float w = in[dest_index].w;

  in[dest_index] = in[dest_index] * (1.0f - f) + fill_color * f;

  in[dest_index].w = w;
}

kernel void
retouch_copy_buffer_to_buffer_masked(global float4 *buffer_src,
                                     global float4 *buffer_dest,
                                     global dt_iop_roi_t *roi_buffer_dest,
                                     global float *mask_scaled,
                                     global dt_iop_roi_t *roi_mask_scaled,
                                     const float opacity)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= roi_mask_scaled->width || y >= roi_mask_scaled->height) return;

  const int idx_dest = mad24(y + roi_mask_scaled->y - roi_buffer_dest->y, roi_buffer_dest->width,
                             x + roi_mask_scaled->x - roi_buffer_dest->x);
  const int idx_src = mad24(y, roi_mask_scaled->width, x);
  const int idx_mask = mad24(y, roi_mask_scaled->width, x);

  const float f = clipf(mask_scaled[idx_mask] * opacity);
  const float w = buffer_dest[idx_dest].w;

  buffer_dest[idx_dest] = buffer_dest[idx_dest] * (1.0f - f) + buffer_src[idx_src] * f;

  buffer_dest[idx_dest].w = w;
}

kernel void
retouch_copy_image_to_buffer_masked(__read_only image2d_t buffer_src,
                                    global float4 *buffer_dest,
                                    global dt_iop_roi_t *roi_buffer_dest,
                                    global float *mask_scaled,
                                    global dt_iop_roi_t *roi_mask_scaled,
                                    const float opacity)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= roi_mask_scaled->width || y >= roi_mask_scaled->height) return;

  const int idx_dest = mad24(y + roi_mask_scaled->y - roi_buffer_dest->y, roi_buffer_dest->width,
                             x + roi_mask_scaled->x - roi_buffer_dest->x);
  const int idx_mask = mad24(y, roi_mask_scaled->width, x);

  const float f = clipf(mask_scaled[idx_mask] * opacity);
  const float w = buffer_dest[idx_dest].w;

  float4 pix = readpixel(buffer_src, x, y);
  buffer_dest[idx_dest] = buffer_dest[idx_dest] * (1.0f - f) + pix * f;

  buffer_dest[idx_dest].w = w;
}

kernel void
retouch_image_rgb2lab(global float4 *in,
                      int width,
                      int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  const int idx = mad24(y, width, x);

  in[idx] = XYZ_to_Lab(sRGB_to_XYZ(in[idx]));
}

kernel void
retouch_image_lab2rgb(global float4 *in,
                      int width,
                      int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  const int idx = mad24(y, width, x);

  in[idx] = XYZ_to_sRGB(Lab_to_XYZ(in[idx]));
}
