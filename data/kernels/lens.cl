/*
 *  This file is part of darktable,
 *  copyright (c) 2009--2013 johannes hanika.
 *  copyright (c) 2014 Ulrich Pegelow.
 *  copyright (c) 2014 LebedevRI.
 *
 *  darktable is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  darktable is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with darktable.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "common.h"

#include "interpolation.h"

/* kernels for the lens plugin: bilinear interpolation */
kernel void
lens_distort_bilinear (read_only image2d_t in, write_only image2d_t out, const int width, const int height,
               const int iwidth, const int iheight, const int roi_in_x, const int roi_in_y, global float *pi)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 pixel;

  float rx, ry;
  const int piwidth = 2*3*width;
  global float *ppi = pi + mad24(y, piwidth, 2*3*x);

  rx = ppi[0] - roi_in_x;
  ry = ppi[1] - roi_in_y;
  pixel.x = (rx >= 0 && ry >= 0 && rx <= iwidth - 1 && ry <= iheight - 1) ? read_imagef(in, samplerf, (float2)(rx, ry)).x : NAN;

  rx = ppi[2] - roi_in_x;
  ry = ppi[3] - roi_in_y;
  pixel.yw = (rx >= 0 && ry >= 0 && rx <= iwidth - 1 && ry <= iheight - 1) ? read_imagef(in, samplerf, (float2)(rx, ry)).yw : (float2)NAN;

  rx = ppi[4] - roi_in_x;
  ry = ppi[5] - roi_in_y;
  pixel.z = (rx >= 0 && ry >= 0 && rx <= iwidth - 1 && ry <= iheight - 1) ? read_imagef(in, samplerf, (float2)(rx, ry)).z : NAN;

  pixel = (isnormal(pixel.x) && isnormal(pixel.y) && isnormal(pixel.z)) ? pixel : (float4)0.0f;

  write_imagef (out, (int2)(x, y), pixel);
}

/* kernels for the lens plugin: bicubic interpolation */
kernel void
lens_distort_bicubic (read_only image2d_t in, write_only image2d_t out, const int width, const int height,
                      const int iwidth, const int iheight, const int roi_in_x, const int roi_in_y, global float *pi)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  const int kwidth = 2;

  if(x >= width || y >= height) return;

  float4 pixel = (float4)0.0f;

  float rx, ry;
  int tx, ty;
  float sum, weight;
  float2 sum2;
  const int piwidth = 2*3*width;
  global float *ppi = pi + mad24(y, piwidth, 2*3*x);

  rx = ppi[0] - (float)roi_in_x;
  ry = ppi[1] - (float)roi_in_y;

  tx = rx;
  ty = ry;

  sum = 0.0f;
  weight = 0.0f;
  for(int jj = 1 - kwidth; jj <= kwidth; jj++)
    for(int ii= 1 - kwidth; ii <= kwidth; ii++)
  {
    const int i = tx + ii;
    const int j = ty + jj;

    float wx = interpolation_func_bicubic((float)i - rx);
    float wy = interpolation_func_bicubic((float)j - ry);
    float w = (i < 0 || j < 0 || i >= iwidth || j >= iheight) ? 0.0f : wx * wy;

    sum += read_imagef(in, sampleri, (int2)(i, j)).x * w;
    weight += w;
  }
  pixel.x = sum/weight;


  rx = ppi[2] - (float)roi_in_x;
  ry = ppi[3] - (float)roi_in_y;

  tx = rx;
  ty = ry;

  sum2 = (float2)0.0f;
  weight = 0.0f;
  for(int jj = 1 - kwidth; jj <= kwidth; jj++)
    for(int ii= 1 - kwidth; ii <= kwidth; ii++)
  {
    const int i = tx + ii;
    const int j = ty + jj;

    float wx = interpolation_func_bicubic((float)i - rx);
    float wy = interpolation_func_bicubic((float)j - ry);
    float w = (i < 0 || j < 0 || i >= iwidth || j >= iheight) ? 0.0f : wx * wy;

    sum2 += read_imagef(in, sampleri, (int2)(i, j)).yw * w;
    weight += w;
  }
  pixel.yw = sum2/weight;


  rx = ppi[4] - (float)roi_in_x;
  ry = ppi[5] - (float)roi_in_y;

  tx = rx;
  ty = ry;

  sum = 0.0f;
  weight = 0.0f;
  for(int jj = 1 - kwidth; jj <= kwidth; jj++)
    for(int ii= 1 - kwidth; ii <= kwidth; ii++)
  {
    const int i = tx + ii;
    const int j = ty + jj;

    float wx = interpolation_func_bicubic((float)i - rx);
    float wy = interpolation_func_bicubic((float)j - ry);
    float w = (i < 0 || j < 0 || i >= iwidth || j >= iheight) ? 0.0f : wx * wy;

    sum += read_imagef(in, sampleri, (int2)(i, j)).z * w;
    weight += w;
  }
  pixel.z = sum/weight;

  pixel = (isnormal(pixel.x) && isnormal(pixel.y) && isnormal(pixel.z)) ? pixel : (float4)0.0f;

  write_imagef (out, (int2)(x, y), pixel);
}

/* kernels for the lens plugin: lanczos2 interpolation */
kernel void
lens_distort_lanczos2 (read_only image2d_t in, write_only image2d_t out, const int width, const int height,
                      const int iwidth, const int iheight, const int roi_in_x, const int roi_in_y, global float *pi)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  const int kwidth = 2;

  if(x >= width || y >= height) return;

  float4 pixel = (float4)0.0f;

  float rx, ry;
  int tx, ty;
  float sum, weight;
  float2 sum2;
  const int piwidth = 2*3*width;
  global float *ppi = pi + mad24(y, piwidth, 2*3*x);

  rx = ppi[0] - (float)roi_in_x;
  ry = ppi[1] - (float)roi_in_y;

  tx = rx;
  ty = ry;

  sum = 0.0f;
  weight = 0.0f;
  for(int jj = 1 - kwidth; jj <= kwidth; jj++)
    for(int ii= 1 - kwidth; ii <= kwidth; ii++)
  {
    const int i = tx + ii;
    const int j = ty + jj;

    float wx = interpolation_func_lanczos(2, (float)i - rx);
    float wy = interpolation_func_lanczos(2, (float)j - ry);
    float w = (i < 0 || j < 0 || i >= iwidth || j >= iheight) ? 0.0f : wx * wy;

    sum += read_imagef(in, sampleri, (int2)(i, j)).x * w;
    weight += w;
  }
  pixel.x = sum/weight;


  rx = ppi[2] - (float)roi_in_x;
  ry = ppi[3] - (float)roi_in_y;

  tx = rx;
  ty = ry;

  sum2 = (float2)0.0f;
  weight = 0.0f;
  for(int jj = 1 - kwidth; jj <= kwidth; jj++)
    for(int ii= 1 - kwidth; ii <= kwidth; ii++)
  {
    const int i = tx + ii;
    const int j = ty + jj;

    float wx = interpolation_func_lanczos(2, (float)i - rx);
    float wy = interpolation_func_lanczos(2, (float)j - ry);
    float w = (i < 0 || j < 0 || i >= iwidth || j >= iheight) ? 0.0f : wx * wy;

    sum2 += read_imagef(in, sampleri, (int2)(i, j)).yw * w;
    weight += w;
  }
  pixel.yw = sum2/weight;


  rx = ppi[4] - (float)roi_in_x;
  ry = ppi[5] - (float)roi_in_y;

  tx = rx;
  ty = ry;

  sum = 0.0f;
  weight = 0.0f;
  for(int jj = 1 - kwidth; jj <= kwidth; jj++)
    for(int ii= 1 - kwidth; ii <= kwidth; ii++)
  {
    const int i = tx + ii;
    const int j = ty + jj;

    float wx = interpolation_func_lanczos(2, (float)i - rx);
    float wy = interpolation_func_lanczos(2, (float)j - ry);
    float w = (i < 0 || j < 0 || i >= iwidth || j >= iheight) ? 0.0f : wx * wy;

    sum += read_imagef(in, sampleri, (int2)(i, j)).z * w;
    weight += w;
  }
  pixel.z = sum/weight;

  pixel = (isnormal(pixel.x) && isnormal(pixel.y) && isnormal(pixel.z)) ? pixel : (float4)0.0f;

  write_imagef (out, (int2)(x, y), pixel);
}

/* kernels for the lens plugin: lanczos3 interpolation */
kernel void
lens_distort_lanczos3 (read_only image2d_t in, write_only image2d_t out, const int width, const int height,
                      const int iwidth, const int iheight, const int roi_in_x, const int roi_in_y, global float *pi)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  const int kwidth = 3;

  if(x >= width || y >= height) return;

  float4 pixel = (float4)0.0f;

  float rx, ry;
  int tx, ty;
  float sum, weight;
  float2 sum2;
  const int piwidth = 2*3*width;
  global float *ppi = pi + mad24(y, piwidth, 2*3*x);

  rx = ppi[0] - (float)roi_in_x;
  ry = ppi[1] - (float)roi_in_y;

  tx = rx;
  ty = ry;

  sum = 0.0f;
  weight = 0.0f;
  for(int jj = 1 - kwidth; jj <= kwidth; jj++)
    for(int ii= 1 - kwidth; ii <= kwidth; ii++)
  {
    const int i = tx + ii;
    const int j = ty + jj;

    float wx = interpolation_func_lanczos(3, (float)i - rx);
    float wy = interpolation_func_lanczos(3, (float)j - ry);
    float w = (i < 0 || j < 0 || i >= iwidth || j >= iheight) ? 0.0f : wx * wy;

    sum += read_imagef(in, sampleri, (int2)(i, j)).x * w;
    weight += w;
  }
  pixel.x = sum/weight;


  rx = ppi[2] - (float)roi_in_x;
  ry = ppi[3] - (float)roi_in_y;

  tx = rx;
  ty = ry;

  sum2 = (float2)0.0f;
  weight = 0.0f;
  for(int jj = 1 - kwidth; jj <= kwidth; jj++)
    for(int ii= 1 - kwidth; ii <= kwidth; ii++)
  {
    const int i = tx + ii;
    const int j = ty + jj;

    float wx = interpolation_func_lanczos(3, (float)i - rx);
    float wy = interpolation_func_lanczos(3, (float)j - ry);
    float w = (i < 0 || j < 0 || i >= iwidth || j >= iheight) ? 0.0f : wx * wy;

    sum2 += read_imagef(in, sampleri, (int2)(i, j)).yw * w;
    weight += w;
  }
  pixel.yw = sum2/weight;


  rx = ppi[4] - (float)roi_in_x;
  ry = ppi[5] - (float)roi_in_y;

  tx = rx;
  ty = ry;

  sum = 0.0f;
  weight = 0.0f;
  for(int jj = 1 - kwidth; jj <= kwidth; jj++)
    for(int ii= 1 - kwidth; ii <= kwidth; ii++)
  {
    const int i = tx + ii;
    const int j = ty + jj;

    float wx = interpolation_func_lanczos(3, (float)i - rx);
    float wy = interpolation_func_lanczos(3, (float)j - ry);
    float w = (i < 0 || j < 0 || i >= iwidth || j >= iheight) ? 0.0f : wx * wy;

    sum += read_imagef(in, sampleri, (int2)(i, j)).z * w;
    weight += w;
  }
  pixel.z = sum/weight;

  pixel = (isnormal(pixel.x) && isnormal(pixel.y) && isnormal(pixel.z)) ? pixel : (float4)0.0f;

  write_imagef (out, (int2)(x, y), pixel);
}

kernel void
lens_vignette (read_only image2d_t in, write_only image2d_t out, const int width, const int height, global float4 *pi)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 pixel = read_imagef(in, sampleri, (int2)(x, y));
  float4 scale = pi[mad24(y, width, x)]/(float4)0.5f;

  pixel.xyz *= scale.xyz;

  write_imagef (out, (int2)(x, y), pixel);
}
