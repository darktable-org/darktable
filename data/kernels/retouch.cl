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

// the heal solver of src/common/heal.c. The image difference is split into
// 'red' and 'black' checkerboard pixels, each in its own buffer of subwidth
// pixels per row plus a zero padding row at the top and bottom. A pixel of
// split row r (1..height) and column j sits at original row r-1 and column
// 2j + (cs ^ ((r-1) & 1)), with cs 1 for red and 0 for black.

static inline int _heal_col(const int j, const int r, const int cs)
{
  return 2 * j + (cs ^ ((r - 1) & 1));
}

// _heal_sub(): top minus bottom into one color's split buffer
kernel void
retouch_heal_sub(global const float4 *top,
                 global const float4 *bottom,
                 global float4 *split,
                 const int width,
                 const int height,
                 const int subwidth,
                 const int cs)
{
  const int j = get_global_id(0);
  const int r = get_global_id(1);
  if(j >= subwidth || r >= height + 2) return;

  float4 val = (float4)0.0f;
  if(r > 0 && r <= height)
  {
    const int col = _heal_col(j, r, cs);
    if(col < width)
    {
      const int idx = mad24(r - 1, width, col);
      val = top[idx] - bottom[idx];
    }
  }
  split[mad24(r, subwidth, j)] = val;
}

// one half of a Gauss-Seidel iteration with over-relaxation, as
// _heal_laplace_iteration(): updates the masked pixels of one color and
// writes the work group's sum of squared changes. Does nothing once
// retouch_heal_converged() has set the flag.
kernel void
retouch_heal_iterate(global float4 *active,
                     global const float4 *neighbor,
                     global const float *mask,
                     const int width,
                     const int height,
                     const int subwidth,
                     const int cs,
                     const float w,
                     global const int *converged,
                     global float *partial,
                     local float *buffer)
{
  if(converged[0]) return;

  const int j = get_global_id(0);
  const int r = get_global_id(1) + 1;
  const int l = mad24((int)get_local_id(1), (int)get_local_size(0), (int)get_local_id(0));

  float err = 0.0f;
  if(j < subwidth && r <= height)
  {
    const int col = _heal_col(j, r, cs);
    if(col < width && mask[mad24(r - 1, width, col)] != 0.0f)
    {
      const int idx = mad24(r, subwidth, j);
      const int lro = cs ^ ((r - 1) & 1);
      float a = 4.0f;
      if(r == 1) a -= 1.0f;
      if(r == height) a -= 1.0f;
      // the first and last pixel of a row have no left or right neighbor
      float4 left = (float4)0.0f;
      float4 right = (float4)0.0f;
      if(j > 0 || lro)
        left = neighbor[idx - 1 + lro];
      else
        a -= 1.0f;
      if(j + 1 < subwidth || lro == 0)
        right = neighbor[idx + lro];
      else
        a -= 1.0f;

      const float4 act = active[idx];
      const float4 diff = w * ((a * act) - (neighbor[idx - subwidth] + neighbor[idx + subwidth] + left + right));
      active[idx] = act - diff;
      err = diff.x * diff.x + diff.y * diff.y + diff.z * diff.z;
    }
  }

  buffer[l] = err;
  barrier(CLK_LOCAL_MEM_FENCE);
  const int lsz = mul24((int)get_local_size(0), (int)get_local_size(1));
  for(int offset = lsz / 2; offset > 0; offset = offset / 2)
  {
    if(l < offset) buffer[l] += buffer[l + offset];
    barrier(CLK_LOCAL_MEM_FENCE);
  }
  if(l == 0)
    partial[mad24((int)get_group_id(1), (int)get_num_groups(0), (int)get_group_id(0))] = buffer[0];
}

// adds up the partial sums of both colors in one work group and sets the
// flag when the iteration has converged
kernel void
retouch_heal_converged(global const float *partial_black,
                       global const float *partial_red,
                       const int groups,
                       const float err_exit,
                       global int *converged,
                       local float *buffer)
{
  if(converged[0]) return;

  const int l = get_local_id(0);
  const int lsz = get_local_size(0);
  float sum = 0.0f;
  for(int k = l; k < groups; k += lsz)
    sum += partial_black[k] + partial_red[k];

  buffer[l] = sum;
  barrier(CLK_LOCAL_MEM_FENCE);
  for(int offset = lsz / 2; offset > 0; offset = offset / 2)
  {
    if(l < offset) buffer[l] += buffer[l + offset];
    barrier(CLK_LOCAL_MEM_FENCE);
  }
  if(l == 0 && buffer[0] < err_exit) converged[0] = 1;
}

// _heal_add(): the solution plus bottom, back in the original layout
kernel void
retouch_heal_add(global const float4 *red,
                 global const float4 *black,
                 global const float4 *bottom,
                 global float4 *dest,
                 const int width,
                 const int height,
                 const int subwidth)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;

  // the pixel's color: red sits at odd columns of even rows
  const int r = y + 1;
  const int j = x / 2;
  const int is_red = (x & 1) ^ (y & 1);
  const int idx = mad24(r, subwidth, j);
  const int pix = mad24(y, width, x);
  dest[pix] = (is_red ? red[idx] : black[idx]) + bottom[pix];
}
