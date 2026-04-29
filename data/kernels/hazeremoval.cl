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
