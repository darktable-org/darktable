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

void
mul_mat_vec_2(const float4 m, const float2 *p, float2 *o)
{
  (*o).x = (*p).x*m.x + (*p).y*m.y;
  (*o).y = (*p).x*m.z + (*p).y*m.w;
}

void
backtransform(float2 *p, float2 *o, const float4 m, const float2 t)
{
  (*p).y /= (1.0f + (*p).x*t.x);
  (*p).x /= (1.0f + (*p).y*t.y);
  mul_mat_vec_2(m, p, o);
}

void
keystone_backtransform(float2 *i, const float4 k_space, const float2 ka, const float4 ma, const float2 mb)
{
  float xx = (*i).x - k_space.x;
  float yy = (*i).y - k_space.y;

  /*float u = ka.x-kb.x+kc.x-kd.x;
  float v = ka.x-kb.x;
  float w = ka.x-kd.x;
  float z = ka.x;
  //(*i).x = (xx/k_space.z)*(yy/k_space.w)*(ka.x-kb.x+kc.x-kd.x) - (xx/k_space.z)*(ka.x-kb.x) - (yy/k_space.w)*(ka.x-kd.x) + ka.x + k_space.x;
  (*i).x = (xx/k_space.z)*(yy/k_space.w)*u - (xx/k_space.z)*v - (yy/k_space.w)*w + z + k_space.x;
  u = ka.y-kb.y+kc.y-kd.y;
  v = ka.y-kb.y;
  w = ka.y-kd.y;
  z = ka.y;
  //(*i).y = (xx/k_space.z)*(yy/k_space.w)*(ka.y-kb.y+kc.y-kd.y) - (xx/k_space.z)*(ka.y-kb.y) - (yy/k_space.w)*(ka.y-kd.y) + ka.y + k_space.y;
  (*i).y = (xx/k_space.z)*(yy/k_space.w)*u - (xx/k_space.z)*v - (yy/k_space.w)*w + z + k_space.y;*/
  float div = ((ma.z*xx-ma.x*yy)*mb.y+(ma.y*yy-ma.w*xx)*mb.x+ma.x*ma.w-ma.y*ma.z);

  (*i).x = (ma.w*xx-ma.y*yy)/div + ka.x;
  (*i).y =-(ma.z*xx-ma.x*yy)/div + ka.y;
}

/* kernel for clip&rotate: bilinear interpolation */
__kernel void
clip_rotate_bilinear(read_only image2d_t in, write_only image2d_t out, const int width, const int height,
            const int in_width, const int in_height,
            const int2 roi_in, const float2 roi_out, const float scale_in, const float scale_out,
            const int flip, const float2 t, const float2 k, const float4 mat,
            const float4 k_space, const float2 ka, const float4 ma, const float2 mb)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float2 pi, po;

  pi.x = roi_out.x + x ;
  pi.y = roi_out.y + y ;

  pi.x -= flip ? t.y * scale_out : t.x * scale_out;
  pi.y -= flip ? t.x * scale_out : t.y * scale_out;

  pi /= scale_out;
  backtransform(&pi, &po, mat, k);
  po *= scale_in;

  po.x += t.x * scale_in;
  po.y += t.y * scale_in;

  if (k_space.z > 0.0f) keystone_backtransform(&po,k_space,ka,ma,mb);

  po.x -= roi_in.x;
  po.y -= roi_in.y;

  const int ii = (int)po.x;
  const int jj = (int)po.y;

  float4 o = (ii >=0 && jj >= 0 && ii <= in_width-2 && jj <= in_height-2) ? read_imagef(in, samplerf, po) : (float4)0.0f;

  write_imagef (out, (int2)(x, y), o);
}

/* kernel for clip&rotate: bicubic interpolation */
__kernel void
clip_rotate_bicubic(read_only image2d_t in, write_only image2d_t out, const int width, const int height,
            const int in_width, const int in_height,
            const int2 roi_in, const float2 roi_out, const float scale_in, const float scale_out,
            const int flip, const float2 t, const float2 k, const float4 mat,
            const float4 k_space, const float2 ka, const float4 ma, const float2 mb)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  const int kwidth = 2;

  if(x >= width || y >= height) return;

  float2 pi, po;

  pi.x = roi_out.x + x ;
  pi.y = roi_out.y + y ;

  pi.x -= flip ? t.y * scale_out : t.x * scale_out;
  pi.y -= flip ? t.x * scale_out : t.y * scale_out;

  pi /= scale_out;
  backtransform(&pi, &po, mat, k);
  po *= scale_in;

  po.x += t.x * scale_in;
  po.y += t.y * scale_in;

  if (k_space.z > 0.0f) keystone_backtransform(&po,k_space,ka,ma,mb);

  po.x -= roi_in.x;
  po.y -= roi_in.y;

  int tx = po.x;
  int ty = po.y;

  float4 pixel = (float4)0.0f;
  float weight = 0.0f;

  for(int jj = 1 - kwidth; jj <= kwidth; jj++)
    for(int ii= 1 - kwidth; ii <= kwidth; ii++)
  {
    const int i = tx + ii;
    const int j = ty + jj;

    float wx = interpolation_func_bicubic((float)i - po.x);
    float wy = interpolation_func_bicubic((float)j - po.y);
    float w = (i < 0 || j < 0 || i >= in_width || j >= in_height) ? 0.0f : wx * wy;

    pixel += read_imagef(in, sampleri, (int2)(i, j)) * w;
    weight += w;
  }

  pixel = weight > 0.0f ? pixel / weight : (float4)0.0f;

  write_imagef (out, (int2)(x, y), pixel);
}

/* kernel for clip&rotate: lanczos2 interpolation */
__kernel void
clip_rotate_lanczos2(read_only image2d_t in, write_only image2d_t out, const int width, const int height,
            const int in_width, const int in_height,
            const int2 roi_in, const float2 roi_out, const float scale_in, const float scale_out,
            const int flip, const float2 t, const float2 k, const float4 mat,
            const float4 k_space, const float2 ka, const float4 ma, const float2 mb)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  const int kwidth = 2;

  if(x >= width || y >= height) return;

  float2 pi, po;

  pi.x = roi_out.x + x ;
  pi.y = roi_out.y + y ;

  pi.x -= flip ? t.y * scale_out : t.x * scale_out;
  pi.y -= flip ? t.x * scale_out : t.y * scale_out;

  pi /= scale_out;
  backtransform(&pi, &po, mat, k);
  po *= scale_in;

  po.x += t.x * scale_in;
  po.y += t.y * scale_in;

  if (k_space.z > 0.0f) keystone_backtransform(&po,k_space,ka,ma,mb);

  po.x -= roi_in.x;
  po.y -= roi_in.y;

  int tx = po.x;
  int ty = po.y;

  float4 pixel = (float4)0.0f;
  float weight = 0.0f;

  for(int jj = 1 - kwidth; jj <= kwidth; jj++)
    for(int ii= 1 - kwidth; ii <= kwidth; ii++)
  {
    const int i = tx + ii;
    const int j = ty + jj;

    float wx = interpolation_func_lanczos(2, (float)i - po.x);
    float wy = interpolation_func_lanczos(2, (float)j - po.y);
    float w = (i < 0 || j < 0 || i >= in_width || j >= in_height) ? 0.0f : wx * wy;

    pixel += read_imagef(in, sampleri, (int2)(i, j)) * w;
    weight += w;
  }

  pixel = weight > 0.0f ? pixel / weight : (float4)0.0f;

  write_imagef (out, (int2)(x, y), pixel);
}

/* kernel for clip&rotate: lanczos3 interpolation */
__kernel void
clip_rotate_lanczos3(read_only image2d_t in, write_only image2d_t out, const int width, const int height,
            const int in_width, const int in_height,
            const int2 roi_in, const float2 roi_out, const float scale_in, const float scale_out,
            const int flip, const float2 t, const float2 k, const float4 mat,
            const float4 k_space, const float2 ka, const float4 ma, const float2 mb)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  const int kwidth = 3;

  if(x >= width || y >= height) return;

  float2 pi, po;

  pi.x = roi_out.x + x ;
  pi.y = roi_out.y + y ;

  pi.x -= flip ? t.y * scale_out : t.x * scale_out;
  pi.y -= flip ? t.x * scale_out : t.y * scale_out;

  pi /= scale_out;
  backtransform(&pi, &po, mat, k);
  po *= scale_in;

  po.x += t.x * scale_in;
  po.y += t.y * scale_in;

  if (k_space.z > 0.0f) keystone_backtransform(&po,k_space,ka,ma,mb);

  po.x -= roi_in.x ;
  po.y -= roi_in.y ;

  int tx = po.x;
  int ty = po.y;

  float4 pixel = (float4)0.0f;
  float weight = 0.0f;

  for(int jj = 1 - kwidth; jj <= kwidth; jj++)
    for(int ii= 1 - kwidth; ii <= kwidth; ii++)
  {
    const int i = tx + ii;
    const int j = ty + jj;

    float wx = interpolation_func_lanczos(3, (float)i - po.x);
    float wy = interpolation_func_lanczos(3, (float)j - po.y);
    float w = (i < 0 || j < 0 || i >= in_width || j >= in_height) ? 0.0f : wx * wy;

    pixel += read_imagef(in, sampleri, (int2)(i, j)) * w;
    weight += w;
  }

  pixel = weight > 0.0f ? pixel / weight : (float4)0.0f;

  write_imagef (out, (int2)(x, y), pixel);
}
