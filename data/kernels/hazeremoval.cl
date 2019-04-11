/*
    This file is part of darktable,
    copyright (c) 2019 Heiko Bauke

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


kernel void hazeremoval_box_min_x(const int width, const int height, read_only image2d_t in,
                                  write_only image2d_t out, const int w)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= 1 || y >= height) return;

  float m = INFINITY;
  for(int i = 0, i_end = min(w + 1, width); i < i_end; ++i) m = min(read_imagef(in, sampleri, (int2)(i, y)).x, m);
  for(int i = 0; i < width; i++)
  {
    write_imagef(out, (int2)(i, y), (float4)(m, 0.f, 0.f, 0.f));
    if(i - w >= 0 && read_imagef(in, sampleri, (int2)(i - w, y)).x == m)
    {
      m = INFINITY;
      for(int j = max(i - w + 1, 0), j_end = min(i + w + 2, width); j < j_end; ++j)
        m = min(read_imagef(in, sampleri, (int2)(j, y)).x, m);
    }
    if(i + w + 1 < width) m = min(read_imagef(in, sampleri, (int2)(i + w + 1, y)).x, m);
  }
}


kernel void hazeremoval_box_min_y(const int width, const int height, read_only image2d_t in,
                                  write_only image2d_t out, const int w)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= 1) return;

  float m = INFINITY;
  for(int i = 0, i_end = min(w + 1, height); i < i_end; ++i) m = min(read_imagef(in, sampleri, (int2)(x, i)).x, m);
  for(int i = 0; i < height; i++)
  {
    write_imagef(out, (int2)(x, i), (float4)(m, 0.f, 0.f, 0.f));
    if(i - w >= 0 && read_imagef(in, sampleri, (int2)(x, i - w)).x == m)
    {
      m = INFINITY;
      for(int j = max(i - w + 1, 0), j_end = min(i + w + 2, height); j < j_end; ++j)
        m = min(read_imagef(in, sampleri, (int2)(x, j)).x, m);
    }
    if(i + w + 1 < height) m = min(read_imagef(in, sampleri, (int2)(x, i + w + 1)).x, m);
  }
}


kernel void hazeremoval_box_max_x(const int width, const int height, read_only image2d_t in,
                                  write_only image2d_t out, const int w)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= 1 || y >= height) return;

  float m = -(INFINITY);
  for(int i = 0, i_end = min(w + 1, width); i < i_end; ++i) m = max(read_imagef(in, sampleri, (int2)(i, y)).x, m);
  for(int i = 0; i < width; i++)
  {
    write_imagef(out, (int2)(i, y), (float4)(m, 0.f, 0.f, 0.f));
    if(i - w >= 0 && read_imagef(in, sampleri, (int2)(i - w, y)).x == m)
    {
      m = -(INFINITY);
      for(int j = max(i - w + 1, 0), j_end = min(i + w + 2, width); j < j_end; ++j)
        m = max(read_imagef(in, sampleri, (int2)(j, y)).x, m);
    }
    if(i + w + 1 < width) m = max(read_imagef(in, sampleri, (int2)(i + w + 1, y)).x, m);
  }
}


kernel void hazeremoval_box_max_y(const int width, const int height, read_only image2d_t in,
                                  write_only image2d_t out, const int w)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= 1) return;

  float m = -(INFINITY);
  for(int i = 0, i_end = min(w + 1, height); i < i_end; ++i) m = max(read_imagef(in, sampleri, (int2)(x, i)).x, m);
  for(int i = 0; i < height; i++)
  {
    write_imagef(out, (int2)(x, i), (float4)(m, 0.f, 0.f, 0.f));
    if(i - w >= 0 && read_imagef(in, sampleri, (int2)(x, i - w)).x == m)
    {
      m = -(INFINITY);
      for(int j = max(i - w + 1, 0), j_end = min(i + w + 2, height); j < j_end; ++j)
        m = max(read_imagef(in, sampleri, (int2)(x, j)).x, m);
    }
    if(i + w + 1 < height) m = max(read_imagef(in, sampleri, (int2)(x, i + w + 1)).x, m);
  }
}


kernel void hazeremoval_transision_map(const int width, const int height, read_only image2d_t in,
                                       write_only image2d_t out, const float strength, const float A0_r,
                                       const float A0_g, const float A0_b)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;

  const float4 pixel = read_imagef(in, sampleri, (int2)(x, y));
  float m = pixel.x / A0_r;
  m = min(pixel.y / A0_g, m);
  m = min(pixel.z / A0_b, m);
  write_imagef(out, (int2)(x, y), (float4)(1.f - m * strength, 0.f, 0.f, 0.f));
}


kernel void hazeremoval_dehaze(const int width, const int height, read_only image2d_t in,
                               read_only image2d_t trans_map, write_only image2d_t out, const float t_min,
                               const float A0_r, const float A0_g, const float A0_b)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;

  const float4 pixel = read_imagef(in, sampleri, (int2)(x, y));
  const float t = max(read_imagef(trans_map, sampleri, (int2)(x, y)).x, t_min);
  write_imagef(
      out, (int2)(x, y),
      (float4)((pixel.x - A0_r) / t + A0_r, (pixel.y - A0_g) / t + A0_g, (pixel.z - A0_b) / t + A0_b, pixel.w));
}
