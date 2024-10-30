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


float ddirac(const int2 q)
{
  return ((q.x || q.y) ? 1.0f : 0.0f);
}


kernel void
denoiseprofile_precondition(read_only image2d_t in, write_only image2d_t out, const int width, const int height,
                             const float4 a, const float4 sigma2)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 pixel = read_imagef(in, sampleri, (int2)(x, y));
  const float alpha = pixel.w;

  float4 t = fmax(pixel / a, 0.f);
  float4 d = fmax((float4)0.0f, t + (float4)0.375f + sigma2);
  float4 s = 2.0f*sqrt(d);

  s.w = alpha;

  write_imagef (out, (int2)(x, y), s);
}


kernel void
denoiseprofile_precondition_v2(read_only image2d_t in, write_only image2d_t out, const int width, const int height,
                             const float4 a, const float4 p, const float4 b, const float4 wb)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 pixel = read_imagef(in, sampleri, (int2)(x, y));
  const float alpha = pixel.w;

  float4 t = fmax(2.0f * dtcl_pow(fmax((float4)0.0f, pixel / wb + b), 1.0f - p / 2.0f) / ((-p + 2.0f) * sqrt(a)), 0.f);

  t.w = alpha;

  write_imagef (out, (int2)(x, y), t);
}

kernel void
denoiseprofile_precondition_Y0U0V0(read_only image2d_t in, write_only image2d_t out, const int width, const int height,
                             const float4 a, const float4 p, const float4 b, global float *toY0U0V0)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 pixel = read_imagef(in, sampleri, (int2)(x, y));
  const float alpha = pixel.w;

  const float4 t = fmax(2.0f * dtcl_pow(fmax((float4)0.0f, pixel + b), 1.0f - p / 2.0f) / ((-p + 2.0f) * sqrt(a)), 0.f);

  float4 outpx = (float4)0.0f;
  outpx.x += toY0U0V0[0] * t.x;
  outpx.x += toY0U0V0[1] * t.y;
  outpx.x += toY0U0V0[2] * t.z;
  outpx.y += toY0U0V0[3] * t.x;
  outpx.y += toY0U0V0[4] * t.y;
  outpx.y += toY0U0V0[5] * t.z;
  outpx.z += toY0U0V0[6] * t.x;
  outpx.z += toY0U0V0[7] * t.y;
  outpx.z += toY0U0V0[8] * t.z;

  outpx.w = alpha;

  write_imagef (out, (int2)(x, y), outpx);
}


kernel void
denoiseprofile_init(global float4* out, const int width, const int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  const int gidx = mad24(y, width, x);

  if(x >= width || y >= height) return;

  out[gidx] = (float4)0.0f;
}


kernel void
denoiseprofile_dist(read_only image2d_t in, global float* U4, const int width, const int height,
             const int2 q)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  const int gidx = mad24(y, width, x);

  if(x >= width || y >= height) return;

  int xpq = x + q.x;
  int ypq = y + q.y;
  // Convert out of bounds indexes to 0
  // Reminder: q.x and q.y can be negative
  xpq *= (x+q.x < width && x+q.x >= 0) ? 1 : 0;
  ypq *= (y+q.y < height && y+q.y >= 0) ? 1 : 0;

  float4 p1 = read_imagef(in, sampleri, (int2)(x, y));
  float4 p2 = read_imagef(in, sampleri, (int2)(xpq, ypq));
  float4 tmp = (p1 - p2)*(p1 - p2);
  float dist = tmp.x + tmp.y + tmp.z;

  // make dist equal to 0 in case xpq or ypq is out of bounds
  dist *= (x+q.x < width && x+q.x >= 0 && y+q.y < height && y+q.y >= 0)  ? 1.0f : 0.0f;

  U4[gidx] = dist;
}

kernel void
denoiseprofile_horiz(global float* U4_in, global float* U4_out, const int width, const int height,
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
denoiseprofile_vert(global float* U4_in, global float* U4_out, const int width, const int height,
              const int2 q, const int P, const float norm, local float *buffer,
              const float central_pixel_weight, global float* U4_single_pixel)
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

  distacc += U4_single_pixel[gidx] * (2 * P + 1) * (2 * P + 1) * central_pixel_weight;
  distacc /= (1.0f + central_pixel_weight);

  distacc = fast_mexp2f(fmax(0.0f, distacc*norm - 2.0f));

  U4_out[gidx] = distacc;
}


kernel void
denoiseprofile_accu(read_only image2d_t in, global float4* U2, global float* U4,
             const int width, const int height, const int2 q)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  const int gidx = mad24(y, width, x);

  if(x >= width || y >= height) return;

  // wpq and wmq are weights for the image read of
  // indexes (int2)(x, y) + q and (int2)(x, y) - q)
  // respectively
  // we want wpq and wmq equal to 1 only if
  // their associated index is in bounds
  int wpq = 1;
  int wmq = 1;

  // handle bounds for x
  // Reminder: q.x can be negative
  wpq *= (x+q.x < width) ? 1 : 0;
  wmq *= (x-q.x < width) ? 1 : 0;
  wpq *= (x+q.x >= 0) ? 1 : 0;
  wmq *= (x-q.x >= 0) ? 1 : 0;

  // handle bounds for y
  // Reminder: q.y can be negative
  wpq *= (y+q.y >= 0) ? 1 : 0;
  wmq *= (y-q.y >= 0) ? 1 : 0;
  wpq *= (y+q.y < height) ? 1 : 0;
  wmq *= (y-q.y < height) ? 1 : 0;

  float4 u1_pq = wpq ? read_imagef(in, sampleri, (int2)(x, y) + q) : (float4)0.0f;
  float4 u1_mq = wmq ? read_imagef(in, sampleri, (int2)(x, y) - q) : (float4)0.0f;

  float  u4    = U4[gidx];
  float  u4_mq = U4[mad24(clamp(y-q.y, 0, height-1), width, clamp(x-q.x, 0, width-1))];

  float u4_mq_dd = u4_mq * ddirac(q);

  float4 accu = (u4 * u1_pq) + (u4_mq_dd * u1_mq);
  accu.w = (wpq * u4 + wmq * u4_mq_dd);

  U2[gidx] += accu;
}


kernel void
denoiseprofile_finish(read_only image2d_t in, global float4* U2, write_only image2d_t out, const int width, const int height,
                             const float4 a, const float4 sigma2)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  const int gidx = mad24(y, width, x);

  if(x >= width || y >= height) return;

  float4 u2   = U2[gidx];
  const float alpha = read_imagef(in, sampleri, (int2)(x, y)).w;

  float4 px = ((float4)u2.w > (float4)0.0f ? u2/u2.w : (float4)0.0f);

  px = (px < (float4)0.5f ? (float4)0.0f :
    0.25f*px*px + 0.25f*sqrt(1.5f)/px - 1.375f/(px*px) + 0.625f*sqrt(1.5f)/(px*px*px) - 0.125f - sigma2);

  px *= a;
  px.w = alpha;

  write_imagef (out, (int2)(x, y), px);
}


kernel void
denoiseprofile_finish_v2(read_only image2d_t in, global float4* U2, write_only image2d_t out, const int width, const int height,
                             const float4 a, const float4 p, const float4 b, const float bias, const float4 wb)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  const int gidx = mad24(y, width, x);

  if(x >= width || y >= height) return;

  float4 u2   = U2[gidx];
  const float alpha = read_imagef(in, sampleri, (int2)(x, y)).w;

  float4 px = ((float4)u2.w > (float4)0.0f ? u2/u2.w : (float4)0.0f);

  float4 delta = px * px + (float4)bias;
  float4 denominator = 4.0f / (sqrt(a) * (2.0f - p));
  float4 z1 = (px + sqrt(fmax((float4)0.0f, delta))) / denominator;
  px = fmax(dtcl_pow(z1, 1.0f / (1.0f - p / 2.0f)) - b, 0.f);
  px = px * wb;
  px.w = alpha;

  write_imagef (out, (int2)(x, y), px);
}



kernel void
denoiseprofile_backtransform(read_only image2d_t in, write_only image2d_t out, const int width, const int height,
                             const float4 a, const float4 sigma2)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  const int gidx = mad24(y, width, x);

  if(x >= width || y >= height) return;

  float4 px = read_imagef(in, sampleri, (int2)(x, y));
  const float alpha = px.w;

  px = (px < (float4)0.5f ? (float4)0.0f :
    0.25f*px*px + 0.25f*sqrt(1.5f)/px - 1.375f/(px*px) + 0.625f*sqrt(1.5f)/(px*px*px) - 0.125f - sigma2);

  px *= a;
  px.w = alpha;

  write_imagef (out, (int2)(x, y), px);
}


kernel void
denoiseprofile_backtransform_v2(read_only image2d_t in, write_only image2d_t out, const int width, const int height,
                             const float4 a, const float4 p, const float4 b, const float bias, const float4 wb)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  const int gidx = mad24(y, width, x);

  if(x >= width || y >= height) return;

  float4 px = read_imagef(in, sampleri, (int2)(x, y));
  const float alpha = px.w;

  px = fmax((float4)0.0f, px);
  const float4 delta = px * px + (float4)bias;
  const float4 denominator = 4.0f / (sqrt(a) * (2.0f - p));
  const float4 z1 = (px + sqrt(fmax((float4)0.0f, delta))) / denominator;
  px = fmax(dtcl_pow(z1, 1.0f / (1.0f - p / 2.0f)) - b, 0.f);
  px = px * wb;
  px.w = alpha;

  write_imagef (out, (int2)(x, y), px);
}

kernel void
denoiseprofile_backtransform_Y0U0V0(read_only image2d_t in, write_only image2d_t out, const int width, const int height,
                             const float4 a, const float4 p, const float4 b, const float bias, const float4 wb, global float *toRGB)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  const int gidx = mad24(y, width, x);

  if(x >= width || y >= height) return;

  const float4 t = read_imagef(in, sampleri, (int2)(x, y));
  const float alpha = t.w;

  float4 px = (float4)0.0f;
  px.x += toRGB[0] * t.x;
  px.x += toRGB[1] * t.y;
  px.x += toRGB[2] * t.z;
  px.y += toRGB[3] * t.x;
  px.y += toRGB[4] * t.y;
  px.y += toRGB[5] * t.z;
  px.z += toRGB[6] * t.x;
  px.z += toRGB[7] * t.y;
  px.z += toRGB[8] * t.z;

  px = fmax((float4)0.0f, px);
  const float4 delta = px * px + (float4)bias * wb;
  const float4 denominator = 4.0f / (sqrt(a) * (2.0f - p));
  const float4 z1 = (px + sqrt(fmax((float4)0.0f, delta))) / denominator;
  px = fmax(dtcl_pow(z1, 1.0f / (1.0f - p / 2.0f)) - b, 0.f);
  px.w = alpha;

  write_imagef (out, (int2)(x, y), px);
}


float4
weight(const float4 c1, const float4 c2, const float inv_sigma2)
{
  const float4 sqr = (c1 - c2)*(c1 - c2);
  const float dt = (sqr.x + sqr.y + sqr.z)*inv_sigma2;
  const float var = 0.02f;
  const float off2 = 9.0f;
  const float r = fast_mexp2f(fmax(0.0f, dt*var - off2));

  return (float4)r;
}


kernel void
denoiseprofile_decompose(read_only image2d_t in, write_only image2d_t coarse, write_only image2d_t detail,
     const int width, const int height, const unsigned int scale, const float inv_sigma2, global const float *filter)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  const int mult = 1<<scale;

  float4 pixel = read_imagef(in, sampleri, (int2)(x, y));
  float4 sum = (float4)(0.0f);
  float4 wgt = (float4)(0.0f);

  for(int j=0;j<5;j++) for(int i=0;i<5;i++)
  {
    int xx = mad24(mult, i - 2, x);
    int yy = mad24(mult, j - 2, y);
    int k  = mad24(j, 5, i);

    float4 px = read_imagef(in, sampleri, (int2)(xx, yy));
    float4 w = filter[k]*weight(pixel, px, inv_sigma2);

    sum += w*px;
    wgt += w;
  }

  sum /= wgt;
  sum.w = pixel.w;

  write_imagef (detail, (int2)(x, y), pixel - sum);
  write_imagef (coarse, (int2)(x, y), sum);
}


kernel void
denoiseprofile_synthesize(read_only image2d_t coarse, read_only image2d_t detail, write_only image2d_t out,
     const int width, const int height,
     const float t0, const float t1, const float t2, const float t3,
     const float b0, const float b1, const float b2, const float b3)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  const float4 threshold = (float4)(t0, t1, t2, t3);
  const float4 boost     = (float4)(b0, b1, b2, b3);
  float4 c = read_imagef(coarse, sampleri, (int2)(x, y));
  float4 d = read_imagef(detail, sampleri, (int2)(x, y));
  float4 amount = copysign(max((float4)(0.0f), fabs(d) - threshold), d);
  float4 sum = c + boost*amount;
  sum.w = c.w;
  write_imagef (out, (int2)(x, y), sum);
}


kernel void
denoiseprofile_reduce_first(read_only image2d_t in, const int width, const int height,
                            global float4 *accu, local float4 *buffer)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  const int xlsz = get_local_size(0);
  const int ylsz = get_local_size(1);
  const int xlid = get_local_id(0);
  const int ylid = get_local_id(1);

  const int l = mad24(ylid, xlsz, xlid);

  const int isinimage = (x < width && y < height);
  float4 pixel = read_imagef(in, sampleri, (int2)(x, y));

  buffer[l] = isinimage ? pixel*pixel : (float4)0.0f;

  barrier(CLK_LOCAL_MEM_FENCE);

  const int lsz = mul24(xlsz, ylsz);

  for(int offset = lsz / 2; offset > 0; offset = offset / 2)
  {
    if(l < offset)
    {
      buffer[l] += buffer[l + offset];
    }
    barrier(CLK_LOCAL_MEM_FENCE);
  }

  const int xgid = get_group_id(0);
  const int ygid = get_group_id(1);
  const int xgsz = get_num_groups(0);

  const int m = mad24(ygid, xgsz, xgid);
  accu[m]   = buffer[0];
}


kernel void
denoiseprofile_reduce_second(const global float4* input, global float4 *result, const int length, local float4 *buffer)
{
  int x = get_global_id(0);
  float4 sum_y2 = (float4)0.0f;

  while(x < length)
  {
    sum_y2 += input[x];

    x += get_global_size(0);
  }

  int lid = get_local_id(0);
  buffer[lid] = sum_y2;

  barrier(CLK_LOCAL_MEM_FENCE);

  for(int offset = get_local_size(0) / 2; offset > 0; offset = offset / 2)
  {
    if(lid < offset)
    {
      buffer[lid] += buffer[lid + offset];
    }
    barrier(CLK_LOCAL_MEM_FENCE);
  }

  if(lid == 0)
  {
    const int gid = get_group_id(0);

    result[gid]   = buffer[0];
  }
}
