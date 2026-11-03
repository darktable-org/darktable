/*
    This file is part of darktable,
    copyright (c) 2019-2026 darktable developers.

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


kernel void hazeremoval_box_min_x(const int width,
                                  const int height,
                                  read_only image2d_t in,
                                  write_only image2d_t out,
                                  const int w)
{
  const int y = get_global_id(0);
  if(y >= height) return;

  float m = INFINITY;
  for(int i = 0, i_end = min(w + 1, width); i < i_end; ++i) m = min(readsingle(in, i, y), m);
  for(int i = 0; i < width; i++)
  {
    write_imagef(out, (int2)(i, y), m);
    if(i - w >= 0 && readsingle(in, i - w, y) == m)
    {
      m = INFINITY;
      for(int j = max(i - w + 1, 0), j_end = min(i + w + 2, width); j < j_end; ++j)
        m = fmin(readsingle(in, j, y), m);
    }
    if(i + w + 1 < width) m = fmin(readsingle(in, i + w + 1, y), m);
  }
}


kernel void hazeremoval_box_min_y(const int width,
                                  const int height,
                                  read_only image2d_t in,
                                  write_only image2d_t out,
                                  const int w)
{
  const int x = get_global_id(0);
  if(x >= width) return;

  float m = INFINITY;
  for(int i = 0, i_end = min(w + 1, height); i < i_end; ++i)
    m = fmin(readsingle(in, x, i), m);
  for(int i = 0; i < height; i++)
  {
    write_imagef(out, (int2)(x, i), m);
    if(i - w >= 0 && readsingle(in, x, i - w) == m)
    {
      m = INFINITY;
      for(int j = max(i - w + 1, 0), j_end = min(i + w + 2, height); j < j_end; ++j)
        m = fmin(readsingle(in, x, j), m);
    }
    if(i + w + 1 < height) m = fmin(readsingle(in, x, i + w + 1), m);
  }
}


kernel void hazeremoval_box_max_x(const int width,
                                  const int height,
                                  read_only image2d_t in,
                                  write_only image2d_t out,
                                  const int w)
{
  const int y = get_global_id(0);
  if(y >= height) return;

  float m = -(INFINITY);
  for(int i = 0, i_end = min(w + 1, width); i < i_end; ++i)
    m = fmax(readsingle(in, i, y), m);
  for(int i = 0; i < width; i++)
  {
    write_imagef(out, (int2)(i, y), m);
    if(i - w >= 0 && readsingle(in, i - w, y) == m)
    {
      m = -(INFINITY);
      for(int j = max(i - w + 1, 0), j_end = min(i + w + 2, width); j < j_end; ++j)
        m = fmax(readsingle(in, j, y), m);
    }
    if(i + w + 1 < width) m = fmax(readsingle(in, i + w + 1, y), m);
  }
}


kernel void hazeremoval_box_max_y(const int width,
                                  const int height,
                                  read_only image2d_t in,
                                  write_only image2d_t out,
                                  const int w)
{
  const int x = get_global_id(0);
  if(x >= width) return;

  float m = -(INFINITY);
  for(int i = 0, i_end = min(w + 1, height); i < i_end; ++i)
    m = fmax(readsingle(in, x, i), m);
  for(int i = 0; i < height; i++)
  {
    write_imagef(out, (int2)(x, i), m);
    if(i - w >= 0 && readsingle(in, x, i - w) == m)
    {
      m = -(INFINITY);
      for(int j = max(i - w + 1, 0), j_end = min(i + w + 2, height); j < j_end; ++j)
        m = fmax(readsingle(in, x, j), m);
    }
    if(i + w + 1 < height) m = fmax(readsingle(in, x, i + w + 1), m);
  }
}


kernel void hazeremoval_transision_map(const int width,
                                       const int height,
                                       read_only image2d_t in,
                                       write_only image2d_t out,
                                       const float strength,
                                       const float A0_r,
                                       const float A0_g,
                                       const float A0_b)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;

  const float4 pixel = readpixel(in, x, y);
  float m = pixel.x / A0_r;
  m = fmin(pixel.y / A0_g, m);
  m = fmin(pixel.z / A0_b, m);
  write_imagef(out, (int2)(x, y), 1.f - m * strength);
}


kernel void hazeremoval_dehaze(const int width,
                               const int height,
                               read_only image2d_t in,
                               read_only image2d_t trans_map,
                               write_only image2d_t out,
                               const float t_min,
                               const float4 A0)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;

  const float4 pixel = readpixel(in, x, y);
  const float t = fmax(readsingle(trans_map, x, y), t_min);

  write_imagef(out, (int2)(x, y),
      (float4)((pixel.x - A0.x) / t + A0.x, (pixel.y - A0.y) / t + A0.y, (pixel.z - A0.z) / t + A0.z, pixel.w));
}


// the smallest color component of each pixel, before the box minimum of the
// dark channel
kernel void hazeremoval_dark_channel(const int width,
                                     const int height,
                                     read_only image2d_t in,
                                     write_only image2d_t out)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;

  const float4 pixel = readpixel(in, x, y);
  write_imagef(out, (int2)(x, y), fmin(fmin(pixel.x, pixel.y), pixel.z));
}


// an unsigned key that orders like the float, with -0 and +0 as one value
static inline uint _sort_key(const float v)
{
  const uint u = as_uint(v == 0.0f ? 0.0f : v);
  return (u & 0x80000000u) ? ~u : (u | 0x80000000u);
}


// one radix pass of the quantile selection in _ambient_light(): counts the
// next 8 bits of the keys whose higher bits equal prefix. The keys are of
// the dark channel, or with brightness set of the summed colors of the
// pixels whose dark channel is at least crit
kernel void hazeremoval_histogram(const int width,
                                  const int height,
                                  read_only image2d_t dark,
                                  read_only image2d_t in,
                                  const int brightness,
                                  const float crit,
                                  const unsigned int prefix,
                                  const unsigned int prefix_mask,
                                  const int shift,
                                  global unsigned int *hist)
{
  local unsigned int lhist[256];
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  const int l = mad24((int)get_local_id(1), (int)get_local_size(0), (int)get_local_id(0));
  const int lsz = mul24((int)get_local_size(0), (int)get_local_size(1));

  for(int i = l; i < 256; i += lsz) lhist[i] = 0;
  barrier(CLK_LOCAL_MEM_FENCE);

  if(x < width && y < height)
  {
    const float d = readsingle(dark, x, y);
    float v = d;
    int use = 1;
    if(brightness)
    {
      const float4 pixel = readpixel(in, x, y);
      v = pixel.x + pixel.y + pixel.z;
      use = d >= crit;
    }
    const unsigned int key = _sort_key(v);
    if(use && (key & prefix_mask) == prefix)
      atomic_inc(&lhist[(key >> shift) & 255u]);
  }
  barrier(CLK_LOCAL_MEM_FENCE);

  for(int i = l; i < 256; i += lsz)
    if(lhist[i]) atomic_add(&hist[i], lhist[i]);
}


// per work group: the color sums of the brightest of the most hazy pixels,
// and their number in .w. The host adds up the groups in double, see
// _ambient_light_cl()
kernel void hazeremoval_ambient_sums(const int width,
                                     const int height,
                                     read_only image2d_t dark,
                                     read_only image2d_t in,
                                     const float crit_haze,
                                     const float crit_bright,
                                     global float4 *accu,
                                     local float4 *sums)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  const int l = mad24((int)get_local_id(1), (int)get_local_size(0), (int)get_local_id(0));
  const int lsz = mul24((int)get_local_size(0), (int)get_local_size(1));

  float4 s = 0.0f;
  if(x < width && y < height)
  {
    const float4 pixel = readpixel(in, x, y);
    if(readsingle(dark, x, y) >= crit_haze && pixel.x + pixel.y + pixel.z >= crit_bright)
      s = (float4)(pixel.x, pixel.y, pixel.z, 1.0f);
  }
  sums[l] = s;
  barrier(CLK_LOCAL_MEM_FENCE);

  for(int offset = lsz / 2; offset > 0; offset = offset / 2)
  {
    if(l < offset) sums[l] += sums[l + offset];
    barrier(CLK_LOCAL_MEM_FENCE);
  }

  if(l == 0)
    accu[mad24((int)get_group_id(1), (int)get_num_groups(0), (int)get_group_id(0))] = sums[0];
}
