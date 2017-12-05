/*
    This file is part of darktable,
    copyright (c) 2009--2010 johannes hanika.

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



// FC return values are either 0/1/2/3 = G/M/C/Y or 0/1/2/3 = R/G1/B/G2
#define FCV(val, col) ((col == 0) ? val.x : ((col & 1) ? val.y : val.z) )

int2
backtransformi (float2 p, const int r_x, const int r_y, const int r_wd, const int r_ht, const float r_scale)
{
  return (int2)((p.x + r_x)/r_scale, (p.y + r_y)/r_scale);
}

float2
backtransformf (float2 p, const int r_x, const int r_y, const int r_wd, const int r_ht, const float r_scale)
{
  return (float2)((p.x + r_x)/r_scale, (p.y + r_y)/r_scale);
}

kernel void
green_equilibration_lavg(read_only image2d_t in, write_only image2d_t out, const int width, const int height, const unsigned int filters, 
                         const int r_x, const int r_y, const float thr, local float *buffer)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  const int xlsz = get_local_size(0);
  const int ylsz = get_local_size(1);
  const int xlid = get_local_id(0);
  const int ylid = get_local_id(1);
  const int xgid = get_group_id(0);
  const int ygid = get_group_id(1);
  
  // individual control variable in this work group and the work group size
  const int l = mad24(ylid, xlsz, xlid);
  const int lsz = mul24(xlsz, ylsz);

  // stride and maximum capacity of local buffer
  // cells of 1*float per pixel with a surrounding border of 2 cells
  const int stride = xlsz + 2*2;
  const int maxbuf = mul24(stride, ylsz + 2*2);

  // coordinates of top left pixel of buffer
  // this is 2 pixel left and above of the work group origin
  const int xul = mul24(xgid, xlsz) - 2;
  const int yul = mul24(ygid, ylsz) - 2;

  // populate local memory buffer
  for(int n = 0; n <= maxbuf/lsz; n++)
  {
    const int bufidx = mad24(n, lsz, l);
    if(bufidx >= maxbuf) continue;
    const int xx = xul + bufidx % stride;
    const int yy = yul + bufidx / stride;
    buffer[bufidx] = read_imagef(in, sampleri, (int2)(xx, yy)).x;
  }

  // center buffer around current x,y-Pixel
  buffer += mad24(ylid + 2, stride, xlid + 2);

  barrier(CLK_LOCAL_MEM_FENCE);

  if(x >= width || y >= height) return;

  const int c = FC(y + r_y, x + r_x, filters);
  const float maximum = 1.0f;
  float o = buffer[0];
  
  if(c == 1 && ((y + r_y) & 1))
  {
    const float o1_1 = buffer[-1 * stride - 1];
    const float o1_2 = buffer[-1 * stride + 1];
    const float o1_3 = buffer[ 1 * stride - 1];
    const float o1_4 = buffer[ 1 * stride + 1];
    const float o2_1 = buffer[-2 * stride + 0];
    const float o2_2 = buffer[ 2 * stride + 0];
    const float o2_3 = buffer[-2];
    const float o2_4 = buffer[ 2];

    const float m1 = (o1_1+o1_2+o1_3+o1_4)/4.0f;
    const float m2 = (o2_1+o2_2+o2_3+o2_4)/4.0f;
    
    if (m2 > 0.0f && m1 / m2 < maximum * 2.0f)
    {
      const float c1 = (fabs(o1_1 - o1_2) + fabs(o1_1 - o1_3) + fabs(o1_1 - o1_4) + fabs(o1_2 - o1_3) + fabs(o1_3 - o1_4) + fabs(o1_2 - o1_4)) / 6.0f;
      const float c2 = (fabs(o2_1 - o2_2) + fabs(o2_1 - o2_3) + fabs(o2_1 - o2_4) + fabs(o2_2 - o2_3) + fabs(o2_3 - o2_4) + fabs(o2_2 - o2_4)) / 6.0f;
      
      if((o < maximum * 0.95f) && (c1 < maximum * thr) && (c2 < maximum * thr))
        o *= m1/m2;
    }
  }
  
  write_imagef (out, (int2)(x, y), o);
}


kernel void
green_equilibration_favg_reduce_first(read_only image2d_t in, const int width, const int height, 
                                      global float2 *accu, const unsigned int filters, const int r_x, const int r_y, local float2 *buffer)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  const int xlsz = get_local_size(0);
  const int ylsz = get_local_size(1);
  const int xlid = get_local_id(0);
  const int ylid = get_local_id(1);

  const int l = mad24(ylid, xlsz, xlid);

  const int c = FC(y + r_y, x + r_x, filters);
  
  const int isinimage = (x < 2 * (width / 2) && y < 2 * (height / 2));
  const int isgreen1 = (c == 1 && !((y + r_y) & 1));
  const int isgreen2 = (c == 1 && ((y + r_y) & 1));
  
  float pixel = read_imagef(in, sampleri, (int2)(x, y)).x;

  buffer[l].x = isinimage && isgreen1 ? pixel : 0.0f;
  buffer[l].y = isinimage && isgreen2 ? pixel : 0.0f;

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
green_equilibration_favg_reduce_second(const global float2* input, global float2 *result, const int length, local float2 *buffer)
{
  int x = get_global_id(0);
  float2 sum = (float2)0.0f;

  while(x < length)
  {
    sum += input[x];

    x += get_global_size(0);
  }
  
  int lid = get_local_id(0);
  buffer[lid] = sum;

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


kernel void
green_equilibration_favg_apply(read_only image2d_t in, write_only image2d_t out, const int width, const int height, const unsigned int filters, 
                               const int r_x, const int r_y, const float gr_ratio)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float pixel = read_imagef(in, sampleri, (int2)(x, y)).x;

  const int c = FC(y + r_y, x + r_x, filters);
  
  const int isgreen1 = (c == 1 && !((y + r_y) & 1));
  
  pixel *= (isgreen1 ? gr_ratio : 1.0f);

  write_imagef (out, (int2)(x, y), pixel);
}

#define SWAP(a, b)                \
  {                               \
    const float tmp = (b);        \
    (b) = (a);                    \
    (a) = tmp;                    \
  }

constant int glim[5] = { 0, 1, 2, 1, 0 };
  
kernel void
pre_median(read_only image2d_t in, write_only image2d_t out, const int width, const int height, 
           const unsigned int filters, const float threshold, local float *buffer)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  const int xlsz = get_local_size(0);
  const int ylsz = get_local_size(1);
  const int xlid = get_local_id(0);
  const int ylid = get_local_id(1);
  const int xgid = get_group_id(0);
  const int ygid = get_group_id(1);
  
  // individual control variable in this work group and the work group size
  const int l = mad24(ylid, xlsz, xlid);
  const int lsz = mul24(xlsz, ylsz);

  // stride and maximum capacity of local buffer
  // cells of 1*float per pixel with a surrounding border of 2 cells
  const int stride = xlsz + 2*2;
  const int maxbuf = mul24(stride, ylsz + 2*2);

  // coordinates of top left pixel of buffer
  // this is 2 pixel left and above of the work group origin
  const int xul = mul24(xgid, xlsz) - 2;
  const int yul = mul24(ygid, ylsz) - 2;

  // populate local memory buffer
  for(int n = 0; n <= maxbuf/lsz; n++)
  {
    const int bufidx = mad24(n, lsz, l);
    if(bufidx >= maxbuf) continue;
    const int xx = xul + bufidx % stride;
    const int yy = yul + bufidx / stride;
    buffer[bufidx] = read_imagef(in, sampleri, (int2)(xx, yy)).x;
  }

  // center buffer around current x,y-Pixel
  buffer += mad24(ylid + 2, stride, xlid + 2);

  barrier(CLK_LOCAL_MEM_FENCE);

  if(x >= width || y >= height) return;

  constant int *lim = glim;

  const int c = FC(y, x, filters);

  float med[9];

  int cnt = 0;
  
  for(int k = 0, i = 0; i < 5; i++)
  {
    for(int j = -lim[i]; j <= lim[i]; j += 2)
    {
      if(fabs(buffer[stride * (i - 2) + j] - buffer[0]) < threshold)
      {
        med[k++] = buffer[stride * (i - 2) + j];
        cnt++;
      }
      else
        med[k++] = 64.0f + buffer[stride * (i - 2) + j];
    }
  }
  
  for(int i = 0; i < 8; i++)
    for(int ii = i + 1; ii < 9; ii++)
      if(med[i] > med[ii]) SWAP(med[i], med[ii]);
  
  float color = (c & 1) ? (cnt == 1 ? med[4] - 64.0f : med[(cnt - 1) / 2]) : buffer[0];

  write_imagef (out, (int2)(x, y), color);
}
#undef SWAP

// This median filter is inspired by GPL code from socles, an OpenCL image processing library.

#define cas(a, b)				\
	do {					\
		float x = a;			\
		int c = a > b;			\
		a = c ? b : a;			\
		b = c ? x : b;			\
	} while (0)


// 3x3 median filter
// uses a sorting network to sort entirely in registers with no branches
kernel void
color_smoothing(read_only image2d_t in, write_only image2d_t out, const int width, const int height, local float4 *buffer)
{
  const int lxid = get_local_id(0);
  const int lyid = get_local_id(1);
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  const int lxsz = get_local_size(0);
  const int buffwd = lxsz + 2;
  const int buffsz = (get_local_size(0) + 2) * (get_local_size(1) + 2);
  const int gsz = get_local_size(0) * get_local_size(1);
  const int lidx = lyid * lxsz + lxid;

  const int nchunks = buffsz % gsz == 0 ? buffsz/gsz - 1 : buffsz/gsz;

  for(int n=0; n <= nchunks; n++)
  {
    const int bufidx = (n * gsz) + lidx;
    if(bufidx >= buffsz) break;

    // get position in buffer coordinates and from there translate to position in global coordinates
    const int gx = (bufidx % buffwd) - 1 + x - lxid;
    const int gy = (bufidx / buffwd) - 1 + y - lyid;

    // don't read more than needed
    if(gx >= width + 1 || gy >= height + 1) continue;

    buffer[bufidx] = read_imagef(in, sampleri, (int2)(gx, gy));
  }

  barrier(CLK_LOCAL_MEM_FENCE);

  if(x >= width || y >= height) return;

  // re-position buffer
  buffer += (lyid + 1) * buffwd + lxid + 1;

  float4 o = buffer[0];

  // 3x3 median for R
  float s0 = buffer[-buffwd - 1].x - buffer[-buffwd - 1].y;
  float s1 = buffer[-buffwd].x - buffer[-buffwd].y;
  float s2 = buffer[-buffwd + 1].x - buffer[-buffwd + 1].y;
  float s3 = buffer[-1].x - buffer[-1].y;
  float s4 = buffer[0].x - buffer[0].y;
  float s5 = buffer[1].x - buffer[1].y;
  float s6 = buffer[buffwd - 1].x - buffer[buffwd - 1].y;
  float s7 = buffer[buffwd].x - buffer[buffwd].y;
  float s8 = buffer[buffwd + 1].x - buffer[buffwd + 1].y;

  cas(s1, s2);
  cas(s4, s5);
  cas(s7, s8);
  cas(s0, s1);
  cas(s3, s4);
  cas(s6, s7);
  cas(s1, s2);
  cas(s4, s5);
  cas(s7, s8);
  cas(s0, s3);
  cas(s5, s8);
  cas(s4, s7);
  cas(s3, s6);
  cas(s1, s4);
  cas(s2, s5);
  cas(s4, s7);
  cas(s4, s2);
  cas(s6, s4);
  cas(s4, s2);

  o.x = fmax(s4 + o.y, 0.0f);


  // 3x3 median for B
  s0 = buffer[-buffwd - 1].z - buffer[-buffwd - 1].y;
  s1 = buffer[-buffwd].z - buffer[-buffwd].y;
  s2 = buffer[-buffwd + 1].z - buffer[-buffwd + 1].y;
  s3 = buffer[-1].z - buffer[-1].y;
  s4 = buffer[0].z - buffer[0].y;
  s5 = buffer[1].z - buffer[1].y;
  s6 = buffer[buffwd - 1].z - buffer[buffwd - 1].y;
  s7 = buffer[buffwd].z - buffer[buffwd].y;
  s8 = buffer[buffwd + 1].z - buffer[buffwd + 1].y;

  cas(s1, s2);
  cas(s4, s5);
  cas(s7, s8);
  cas(s0, s1);
  cas(s3, s4);
  cas(s6, s7);
  cas(s1, s2);
  cas(s4, s5);
  cas(s7, s8);
  cas(s0, s3);
  cas(s5, s8);
  cas(s4, s7);
  cas(s3, s6);
  cas(s1, s4);
  cas(s2, s5);
  cas(s4, s7);
  cas(s4, s2);
  cas(s6, s4);
  cas(s4, s2);

  o.z = fmax(s4 + o.y, 0.0f);

  write_imagef(out, (int2) (x, y), o);
}
#undef cas



/**
 * downscale and clip a buffer (in) to the given roi (r_*) and write it to out.
 * output will be linear in memory.
 * operates on float4 -> float4 textures.
 */
kernel void
clip_and_zoom(read_only image2d_t in, write_only image2d_t out, const int width, const int height,
              const int r_x, const int r_y, const int r_wd, const int r_ht, const float r_scale)
{
  // global id is pixel in output image (float4)
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 color = (float4)(0.0f, 0.0f, 0.0f, 0.0f);

  const float px_footprint = 0.5f/r_scale;
  const int samples = ((int)px_footprint);
  float2 p = backtransformf((float2)(x+0.5f, y+0.5f), r_x, r_y, r_wd, r_ht, r_scale);
  for(int j=-samples;j<=samples;j++) for(int i=-samples;i<=samples;i++)
  {
    float4 px = read_imagef(in, samplerf, (float2)(p.x+i, p.y+j));
    color += px;
  }
  color /= (float4)((2*samples+1)*(2*samples+1));
  write_imagef (out, (int2)(x, y), color);
}


/**
 * downscales and clips a mosaiced buffer (in) to the given region of interest (r_*)
 * and writes it to out in float4 format.
 * filters is the dcraw supplied int encoding the bayer pattern.
 * resamping is done via rank-1 lattices and demosaicing using half-size interpolation.
 */
__kernel void
clip_and_zoom_demosaic_half_size(__read_only image2d_t in, __write_only image2d_t out, const int width, const int height,
    const int r_x, const int r_y, const int rin_wd, const int rin_ht, const float r_scale, const unsigned int filters)
{
  // global id is pixel in output image (float4)
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 color = (float4)(0.0f, 0.0f, 0.0f, 0.0f);
  float weight = 0.0f;

  // adjust to pixel region and don't sample more than scale/2 nbs!
  // pixel footprint on input buffer, radius:
  const float px_footprint = 1.0f/r_scale;
  // how many 2x2 blocks can be sampled inside that area
  const int samples = round(px_footprint/2.0f);

  int trggbx = 0, trggby = 0;
  if(FC(trggby, trggbx+1, filters) != 1) trggbx++;
  if(FC(trggby, trggbx,   filters) != 0)
  {
    trggbx = (trggbx + 1)&1;
    trggby++;
  }
  const int2 rggb = (int2)(trggbx, trggby);


  // upper left corner:
  const float2 f = (float2)((x + r_x) * px_footprint, (y + r_y) * px_footprint);
  int2 p = (int2)((int)f.x & ~1, (int)f.y & ~1);
  const float2 d = (float2)((f.x - p.x)/2.0f, (f.y - p.y)/2.0f);

  // now move p to point to an rggb block:
  p += rggb;

  for(int j=0;j<=samples+1;j++) for(int i=0;i<=samples+1;i++)
  {
    const int xx = p.x + 2*i;
    const int yy = p.y + 2*j;

    if(xx + 1 >= rin_wd || yy + 1 >= rin_ht) continue;

    float xfilter = (i == 0) ? 1.0f - d.x : ((i == samples+1) ? d.x : 1.0f);
    float yfilter = (j == 0) ? 1.0f - d.y : ((j == samples+1) ? d.y : 1.0f);

    // get four mosaic pattern uint16:
    float p1 = read_imagef(in, sampleri, (int2)(xx,   yy  )).x;
    float p2 = read_imagef(in, sampleri, (int2)(xx+1, yy  )).x;
    float p3 = read_imagef(in, sampleri, (int2)(xx,   yy+1)).x;
    float p4 = read_imagef(in, sampleri, (int2)(xx+1, yy+1)).x;
    color += yfilter*xfilter*(float4)(p1, (p2+p3)*0.5f, p4, 0.0f);
    weight += yfilter*xfilter;
  }
  color = weight > 0.0f ? color/weight : (float4)0.0f;
  write_imagef (out, (int2)(x, y), color);
}


/**
 * fill greens pass of pattern pixel grouping.
 * in (float) or (float4).x -> out (float4)
 */
kernel void
ppg_demosaic_green (read_only image2d_t in, write_only image2d_t out, const int width, const int height,
                    const unsigned int filters, local float *buffer)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  const int xlsz = get_local_size(0);
  const int ylsz = get_local_size(1);
  const int xlid = get_local_id(0);
  const int ylid = get_local_id(1);
  const int xgid = get_group_id(0);
  const int ygid = get_group_id(1);
  
  // individual control variable in this work group and the work group size
  const int l = mad24(ylid, xlsz, xlid);
  const int lsz = mul24(xlsz, ylsz);

  // stride and maximum capacity of local buffer
  // cells of 1*float per pixel with a surrounding border of 3 cells
  const int stride = xlsz + 2*3;
  const int maxbuf = mul24(stride, ylsz + 2*3);

  // coordinates of top left pixel of buffer
  // this is 3 pixel left and above of the work group origin
  const int xul = mul24(xgid, xlsz) - 3;
  const int yul = mul24(ygid, ylsz) - 3;

  // populate local memory buffer
  for(int n = 0; n <= maxbuf/lsz; n++)
  {
    const int bufidx = mad24(n, lsz, l);
    if(bufidx >= maxbuf) continue;
    const int xx = xul + bufidx % stride;
    const int yy = yul + bufidx / stride;
    buffer[bufidx] = read_imagef(in, sampleri, (int2)(xx, yy)).x;
  }

  // center buffer around current x,y-Pixel
  buffer += mad24(ylid + 3, stride, xlid + 3);

  barrier(CLK_LOCAL_MEM_FENCE);

  if(x >= width || y >= height) return;

  // process all non-green pixels
  const int row = y;
  const int col = x;
  const int c = FC(row, col, filters);
  float4 color; // output color

  const float pc = buffer[0];

  if     (c == 0) color.x = pc; // red
  else if(c == 1) color.y = pc; // green1
  else if(c == 2) color.z = pc; // blue
  else            color.y = pc; // green2

  // fill green layer for red and blue pixels:
  if(c == 0 || c == 2)
  {
    // look up horizontal and vertical neighbours, sharpened weight:
    const float pym  = buffer[-1 * stride];
    const float pym2 = buffer[-2 * stride];
    const float pym3 = buffer[-3 * stride];
    const float pyM  = buffer[ 1 * stride];
    const float pyM2 = buffer[ 2 * stride];
    const float pyM3 = buffer[ 3 * stride];
    const float pxm  = buffer[-1];
    const float pxm2 = buffer[-2];
    const float pxm3 = buffer[-3];
    const float pxM  = buffer[ 1];
    const float pxM2 = buffer[ 2];
    const float pxM3 = buffer[ 3];
    const float guessx = (pxm + pc + pxM) * 2.0f - pxM2 - pxm2;
    const float diffx  = (fabs(pxm2 - pc) +
                          fabs(pxM2 - pc) + 
                          fabs(pxm  - pxM)) * 3.0f +
                         (fabs(pxM3 - pxM) + fabs(pxm3 - pxm)) * 2.0f;
    const float guessy = (pym + pc + pyM) * 2.0f - pyM2 - pym2;
    const float diffy  = (fabs(pym2 - pc) +
                          fabs(pyM2 - pc) + 
                          fabs(pym  - pyM)) * 3.0f +
                         (fabs(pyM3 - pyM) + fabs(pym3 - pym)) * 2.0f;
    if(diffx > diffy)
    {
      // use guessy
      const float m = fmin(pym, pyM);
      const float M = fmax(pym, pyM);
      color.y = fmax(fmin(guessy*0.25f, M), m);
    }
    else
    {
      const float m = fmin(pxm, pxM);
      const float M = fmax(pxm, pxM);
      color.y = fmax(fmin(guessx*0.25f, M), m);
    }
  }
  write_imagef (out, (int2)(x, y), color);
}


/**
 * fills the reds and blues in the gaps (done after ppg_demosaic_green).
 * in (float4) -> out (float4)
 */
kernel void
ppg_demosaic_redblue (read_only image2d_t in, write_only image2d_t out, const int width, const int height,
                      const unsigned int filters, local float4 *buffer)
{
  // image in contains full green and sparse r b
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  const int xlsz = get_local_size(0);
  const int ylsz = get_local_size(1);
  const int xlid = get_local_id(0);
  const int ylid = get_local_id(1);
  const int xgid = get_group_id(0);
  const int ygid = get_group_id(1);
  
  // individual control variable in this work group and the work group size
  const int l = mad24(ylid, xlsz, xlid);
  const int lsz = mul24(xlsz, ylsz);

  // stride and maximum capacity of local buffer
  // cells of float4 per pixel with a surrounding border of 1 cell
  const int stride = xlsz + 2;
  const int maxbuf = mul24(stride, ylsz + 2);

  // coordinates of top left pixel of buffer
  // this is 1 pixel left and above of the work group origin
  const int xul = mul24(xgid, xlsz) - 1;
  const int yul = mul24(ygid, ylsz) - 1;

  // populate local memory buffer
  for(int n = 0; n <= maxbuf/lsz; n++)
  {
    const int bufidx = mad24(n, lsz, l);
    if(bufidx >= maxbuf) continue;
    const int xx = xul + bufidx % stride;
    const int yy = yul + bufidx / stride;
    buffer[bufidx] = read_imagef(in, sampleri, (int2)(xx, yy));
  }

  // center buffer around current x,y-Pixel
  buffer += mad24(ylid + 1, stride, xlid + 1);

  barrier(CLK_LOCAL_MEM_FENCE);

  if(x >= width || y >= height) return;

  const int row = y;
  const int col = x;
  const int c = FC(row, col, filters);
  float4 color = buffer[0];

  if(c == 1 || c == 3)
  { // calculate red and blue for green pixels:
    // need 4-nbhood:
    float4 nt = buffer[-stride];
    float4 nb = buffer[ stride];
    float4 nl = buffer[-1];
    float4 nr = buffer[ 1];
    if(FC(row, col+1, filters) == 0) // red nb in same row
    {
      color.z = (nt.z + nb.z + 2.0f*color.y - nt.y - nb.y)*0.5f;
      color.x = (nl.x + nr.x + 2.0f*color.y - nl.y - nr.y)*0.5f;
    }
    else
    { // blue nb
      color.x = (nt.x + nb.x + 2.0f*color.y - nt.y - nb.y)*0.5f;
      color.z = (nl.z + nr.z + 2.0f*color.y - nl.y - nr.y)*0.5f;
    }
  }
  else
  {
    // get 4-star-nbhood:
    float4 ntl = buffer[-stride - 1];
    float4 ntr = buffer[-stride + 1];
    float4 nbl = buffer[ stride - 1];
    float4 nbr = buffer[ stride + 1];

    if(c == 0)
    { // red pixel, fill blue:
      const float diff1  = fabs(ntl.z - nbr.z) + fabs(ntl.y - color.y) + fabs(nbr.y - color.y);
      const float guess1 = ntl.z + nbr.z + 2.0f*color.y - ntl.y - nbr.y;
      const float diff2  = fabs(ntr.z - nbl.z) + fabs(ntr.y - color.y) + fabs(nbl.y - color.y);
      const float guess2 = ntr.z + nbl.z + 2.0f*color.y - ntr.y - nbl.y;
      if     (diff1 > diff2) color.z = guess2 * 0.5f;
      else if(diff1 < diff2) color.z = guess1 * 0.5f;
      else color.z = (guess1 + guess2)*0.25f;
    }
    else // c == 2, blue pixel, fill red:
    {
      const float diff1  = fabs(ntl.x - nbr.x) + fabs(ntl.y - color.y) + fabs(nbr.y - color.y);
      const float guess1 = ntl.x + nbr.x + 2.0f*color.y - ntl.y - nbr.y;
      const float diff2  = fabs(ntr.x - nbl.x) + fabs(ntr.y - color.y) + fabs(nbl.y - color.y);
      const float guess2 = ntr.x + nbl.x + 2.0f*color.y - ntr.y - nbl.y;
      if     (diff1 > diff2) color.x = guess2 * 0.5f;
      else if(diff1 < diff2) color.x = guess1 * 0.5f;
      else color.x = (guess1 + guess2)*0.25f;
    }
  }
  write_imagef (out, (int2)(x, y), color);
}

/**
 * Demosaic image border
 */
kernel void
border_interpolate(read_only image2d_t in, write_only image2d_t out, const int width, const int height, const unsigned int filters)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  int border = 3;
  int avgwindow = 1;

  if(x>=border && x<width-border && y>=border && y<height-border) return;

  float4 o;
  float sum[4] = { 0.0f };
  int count[4] = { 0 };

  for (int j=y-avgwindow; j<=y+avgwindow; j++) for (int i=x-avgwindow; i<=x+avgwindow; i++)
  {
    if (j>=0 && i>=0 && j<height && i<width)
    {
      int f = FC(j,i,filters);
      sum[f] += read_imagef(in, sampleri, (int2)(i, j)).x;
      count[f]++;
    }
  }

  float i = read_imagef(in, sampleri, (int2)(x, y)).x;
  o.x = count[0] > 0 ? sum[0]/count[0] : i;
  o.y = count[1]+count[3] > 0 ? (sum[1]+sum[3])/(count[1]+count[3]) : i;
  o.z = count[2] > 0 ? sum[2]/count[2] : i;

  int f = FC(y,x,filters);

  if     (f == 0) o.x = i;
  else if(f == 1) o.y = i;
  else if(f == 2) o.z = i;
  else            o.y = i;

  write_imagef (out, (int2)(x, y), o);
}

