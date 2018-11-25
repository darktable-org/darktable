/*
    This file is part of darktable,
    copyright (c) 2011 johannes hanika.
    copyright (c) 2012--2013 Ulrich Pegelow.

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



/* 
    To speed up processing we use an algorithm proposed by B. Goossens, H.Q. Luong, J. Aelterman, A. Pizurica,  and W. Philips, 
    "A GPU-Accelerated Real-Time NLMeans Algorithm for Denoising Color Video Sequences", in Proc. ACIVS (2), 2010, pp.46-57. 
*/

float fast_mexp2f(const float x)
{
  const float i1 = (float)0x3f800000u; // 2^0
  const float i2 = (float)0x3f000000u; // 2^-1
  const float k0 = i1 + x * (i2 - i1);
  union { float f; unsigned int i; } k;
  k.i = (k0 >= (float)0x800000u) ? k0 : 0;
  return k.f;
}

float gh(const float f, const float sharpness)
{
  // make sharpness bigger: less smoothing
  return fast_mexp2f(f*sharpness);
}

float ddirac(const int2 q)
{
  return ((q.x || q.y) ? 1.0f : 0.0f);
}


kernel void
nlmeans_init(global float4* out, const int width, const int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  const int gidx = mad24(y, width, x);

  if(x >= width || y >= height) return;

  out[gidx] = (float4)0.0f;
}
              

kernel void
nlmeans_dist(read_only image2d_t in, global float *U4, const int width, const int height, 
             const int2 q, const float nL2, const float nC2)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  const int gidx = mad24(y, width, x);
  const float4 norm2 = (float4)(nL2, nC2, nC2, 1.0f);

  // Reminder: q.x and q.y can be negative
  if(x >= width || y >= height) return;
  if(x+q.x >= width || y+q.y >= height) return;
  if(x+q.x < 0 || y+q.y < 0) return;

  float4 p1 = read_imagef(in, sampleri, (int2)(x, y));
  float4 p2 = read_imagef(in, sampleri, (int2)(x, y) + q);
  float4 tmp = (p1 - p2)*(p1 - p2)*norm2;
  float dist = tmp.x + tmp.y + tmp.z;
  
  U4[gidx] = dist;
}

kernel void
nlmeans_horiz(global float *U4_in, global float *U4_out, const int width, const int height, 
              const int2 q, const int P, local float *buffer)
{
  const int lid = get_local_id(0);
  const int lsz = get_local_size(0);
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  const int gidx = mad24(min(y, height-1), width, min(x, width-1));

  if(y < height)
  {
    /* fill center part of buffer */
    buffer[P + lid] = U4_in[gidx];

    /* left wing of buffer */
    for(int n=0; n <= P/lsz; n++)
    {
      const int l = mad24(n, lsz, lid + 1);
      if(l > P) continue;
      int xx = mad24((int)get_group_id(0), lsz, -l);
      xx = max(xx, 0);
      buffer[P - l] = U4_in[mad24(y, width, xx)];
    }
    
    /* right wing of buffer */
    for(int n=0; n <= P/lsz; n++)
    {
      const int r = mad24(n, lsz, lsz - lid);
      if(r > P) continue;
      int xx = mad24((int)get_group_id(0), lsz, lsz - 1 + r);
      xx = min(xx, width-1);
      buffer[P + lsz - 1 + r] = U4_in[mad24(y, width, xx)];
    }
  }

  barrier(CLK_LOCAL_MEM_FENCE);

  if(x >= width || y >= height) return;

  buffer += lid + P;

  float distacc = 0.0f;
  for(int pi = -P; pi <= P; pi++)
  {
    distacc += buffer[pi];
  }

  U4_out[gidx] = distacc;
}


kernel void
nlmeans_vert(global float* U4_in, global float* U4_out, const int width, const int height, 
              const int2 q, const int P, const float sharpness, local float *buffer)
{
  const int lid = get_local_id(1);
  const int lsz = get_local_size(1);
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  const int gidx = mad24(min(y, height-1), width, min(x, width-1));

  if(x < width)
  {
    /* fill center part of buffer */
    buffer[P + lid] = U4_in[gidx];

    /* left wing of buffer */
    for(int n=0; n <= P/lsz; n++)
    {
      const int l = mad24(n, lsz, lid + 1);
      if(l > P) continue;
      int yy = mad24((int)get_group_id(1), lsz, -l);
      yy = max(yy, 0);
      buffer[P - l] = U4_in[mad24(yy, width, x)];
    }

    /* right wing of buffer */
    for(int n=0; n <= P/lsz; n++)
    {
      const int r = mad24(n, lsz, lsz - lid);
      if(r > P) continue;
      int yy = mad24((int)get_group_id(1), lsz, lsz - 1 + r);
      yy = min(yy, height-1);
      buffer[P + lsz - 1 + r] = U4_in[mad24(yy, width, x)];
    }
  }

  barrier(CLK_LOCAL_MEM_FENCE);

  if(x >= width || y >= height) return;

  buffer += lid + P;

  float distacc = 0.0f;
  for(int pj = -P; pj <= P; pj++)
  {
    distacc += buffer[pj];
  }

  distacc = gh(distacc, sharpness);

  U4_out[gidx] = distacc;
}



kernel void
nlmeans_accu(read_only image2d_t in, global float4* U2, global float* U4,
             const int width, const int height, const int2 q)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  const int gidx = mad24(y, width, x);

  if(q.x<0)
  {
    if(x-q.x >= width || x<-q.x) return;
  }
  else
  {
    if(x+q.x >= width || x<q.x) return;
  }
  if(q.y<0)
  {
    if(y-q.y >= height || y<-q.y) return;
  }
  else
  {
    if(y+q.y >= height || y<q.y) return;
  }

  float4 u1_pq = read_imagef(in, sampleri, (int2)(x, y) + q);
  float4 u1_mq = read_imagef(in, sampleri, (int2)(x, y) - q);

  float  u4    = U4[gidx];
  float  u4_mq = U4[mad24(clamp(y-q.y, 0, height-1), width, clamp(x-q.x, 0, width-1))];

  float u4_mq_dd = u4_mq * ddirac(q);

  float4 accu = (u4 * u1_pq) + (u4_mq_dd * u1_mq);
  accu.w = (u4 + u4_mq_dd);

  U2[gidx] += accu;
}


kernel void
nlmeans_finish(read_only image2d_t in, global float4* U2, write_only image2d_t out, 
               const int width, const int height, const float4 weight)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  const int gidx = mad24(y, width, x);

  if(x >= width || y >= height) return;

  float4 i  = read_imagef(in, sampleri, (int2)(x, y));
  float4 u2 = U2[gidx];
  float  u3 = u2.w;

  float4 o = i * ((float4)1.0f - weight) + u2/u3 * weight;
  o.w = i.w;

  write_imagef(out, (int2)(x, y), o);
}



