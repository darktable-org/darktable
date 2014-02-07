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

__kernel void
green_equilibration(__read_only image2d_t in, __write_only image2d_t out, const int width, const int height, const unsigned int filters, const float thr)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  const int c = FC(y, x, filters);
  const float maximum = 1.0f;
  const float o = read_imagef(in, sampleri, (int2)(x, y)).x;
  if(c == 1 && (y & 1))
  {
    const float o1_1 = read_imagef(in, sampleri, (int2)(x-1, y-1)).x;
    const float o1_2 = read_imagef(in, sampleri, (int2)(x+1, y-1)).x;
    const float o1_3 = read_imagef(in, sampleri, (int2)(x-1, y+1)).x;
    const float o1_4 = read_imagef(in, sampleri, (int2)(x+1, y+1)).x;
    const float o2_1 = read_imagef(in, sampleri, (int2)(x, y-2)).x;
    const float o2_2 = read_imagef(in, sampleri, (int2)(x, y+2)).x;
    const float o2_3 = read_imagef(in, sampleri, (int2)(x-2, y)).x;
    const float o2_4 = read_imagef(in, sampleri, (int2)(x+2, y)).x;

    const float m1 = (o1_1+o1_2+o1_3+o1_4)/4.0f;
    const float m2 = (o2_1+o2_2+o2_3+o2_4)/4.0f;
    if (m2 > 0.0f && m1/m2<maximum*2.0f)
    {
      const float c1 = (fabs(o1_1-o1_2)+fabs(o1_1-o1_3)+fabs(o1_1-o1_4)+fabs(o1_2-o1_3)+fabs(o1_3-o1_4)+fabs(o1_2-o1_4))/6.0f;
      const float c2 = (fabs(o2_1-o2_2)+fabs(o2_1-o2_3)+fabs(o2_1-o2_4)+fabs(o2_2-o2_3)+fabs(o2_3-o2_4)+fabs(o2_2-o2_4))/6.0f;
      if((o<maximum*0.95f)&&(c1<maximum*thr)&&(c2<maximum*thr))
      {
        write_imagef (out, (int2)(x, y), o * m1/m2);
      }
      else write_imagef (out, (int2)(x, y), o);
    }
    else write_imagef (out, (int2)(x, y), o);
  }
  else write_imagef (out, (int2)(x, y), o);
}

constant int goffx[18] = { -2,  0,  2, -2,  0,  2, -2,  0,  2,   // r, b
                            0, -1,  1, -2,  0,  2, -1,  1,  0};  // green
constant int goffy[18] = { -2, -2, -2,  0,  0,  0,  2,  2,  2,   // r, b
                           -2, -1, -1,  0,  0,  0,  1,  1,  2};  // green

__kernel void
pre_median(__read_only image2d_t in, __write_only image2d_t out, const int width, const int height, 
           const unsigned int filters, const float thrs, const int f4)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  constant int (*offx)[9] = (constant int (*)[9])goffx;
  constant int (*offy)[9] = (constant int (*)[9])goffy;

  const int c = FC(y, x, filters);
  const int c1 = c & 1;
  const float pix = read_imagef(in, sampleri, (int2)(x, y)).x;
  const float n1 = read_imagef(in, sampleri, (int2)(x-1, y)).x;
  const float n2 = read_imagef(in, sampleri, (int2)(x+1, y)).x;
  const float n3 = read_imagef(in, sampleri, (int2)(x, y+1)).x;
  const float n4 = read_imagef(in, sampleri, (int2)(x, y-1)).x;
  const float variation = fabs(n1 - n2) + fabs(n4 - n3)
                        + fabs(n1 - n3) + fabs(n2 - n4)
                        + fabs(n1 - n4) + fabs(n2 - n3);
  const float thrs2 = c1 ? thrs : 2*thrs;
  float med[9];

  // avoid branch divergence, use constant memory to bake mem accesses, use data-based fetches:
  int cnt = 9;
  for(int k=0;k<9;k++) med[k] = read_imagef(in, sampleri, (int2)(x+offx[c1][k], y+offy[c1][k])).x;
  for(int k=0;k<9;k++) if(fabs(med[k] - pix) > thrs)
  {
    med[k] += 64.0f;
    cnt --;
  }

  // sort:
  for (int i=0;i<8;i++) for(int ii=i+1;ii<9;ii++) if(med[i] > med[ii])
  {
    const float tmp = med[i];
    med[i] = med[ii];
    med[ii] = tmp;
  }
  float4 color = (float4)(0.0f);
  // const float cc = (cnt > 1 || variation > 0.06f) ? med[(cnt-1)/2]) : med[4] - 64.0f;
  const float cc = (c1 || cnt > 1 || variation > 0.06f) ? med[(cnt-1)/2] : med[4] - 64.0f;
  if(f4)
    (c == 0) ? (color.x = cc) : ((c & 1) ? (color.y = cc) : (color.z = cc));
  else
     color.x = cc;
  write_imagef (out, (int2)(x, y), color);
}

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
__kernel void
color_smoothing(__read_only image2d_t in, __write_only image2d_t out, const int width, const int height, local float4 *buffer)
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




/**
 * downscale and clip a buffer (in) to the given roi (r_*) and write it to out.
 * output will be linear in memory.
 * operates on float4 -> float4 textures.
 */
__kernel void
clip_and_zoom(__read_only image2d_t in, __write_only image2d_t out, const int width, const int height,
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
  color /= weight;
  write_imagef (out, (int2)(x, y), color);
}


/**
 * fill greens pass of pattern pixel grouping.
 * in (float) -> out (float4)
 */
__kernel void
ppg_demosaic_green (__read_only image2d_t in, __write_only image2d_t out, const int width, const int height,
                    const unsigned int filters)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  // process all non-green pixels
  const int row = y;
  const int col = x;
  const int c = FC(row, col, filters);
  float4 color;// = (float4)(100.0f, 100.0f, 100.0f, 10000.0f); // output color

  const float4 pc   = read_imagef(in, sampleri, (int2)(col, row));

  if     (c == 0) color.x = pc.x; // red
  else if(c == 1) color.y = pc.x; // green1
  else if(c == 2) color.z = pc.x; // blue
  else            color.y = pc.x; // green2

  // fill green layer for red and blue pixels:
  if(c == 0 || c == 2)
  {
    // look up horizontal and vertical neighbours, sharpened weight:
    const float4 pym  = read_imagef(in, sampleri, (int2)(col, row-1));
    const float4 pym2 = read_imagef(in, sampleri, (int2)(col, row-2));
    const float4 pym3 = read_imagef(in, sampleri, (int2)(col, row-3));
    const float4 pyM  = read_imagef(in, sampleri, (int2)(col, row+1));
    const float4 pyM2 = read_imagef(in, sampleri, (int2)(col, row+2));
    const float4 pyM3 = read_imagef(in, sampleri, (int2)(col, row+3));
    const float4 pxm  = read_imagef(in, sampleri, (int2)(col-1, row));
    const float4 pxm2 = read_imagef(in, sampleri, (int2)(col-2, row));
    const float4 pxm3 = read_imagef(in, sampleri, (int2)(col-3, row));
    const float4 pxM  = read_imagef(in, sampleri, (int2)(col+1, row));
    const float4 pxM2 = read_imagef(in, sampleri, (int2)(col+2, row));
    const float4 pxM3 = read_imagef(in, sampleri, (int2)(col+3, row));
    const float guessx = (pxm.x + pc.x + pxM.x) * 2.0f - pxM2.x - pxm2.x;
    const float diffx  = (fabs(pxm2.x - pc.x) +
                          fabs(pxM2.x - pc.x) + 
                          fabs(pxm.x  - pxM.x)) * 3.0f +
                         (fabs(pxM3.x - pxM.x) + fabs(pxm3.x - pxm.x)) * 2.0f;
    const float guessy = (pym.x + pc.x + pyM.x) * 2.0f - pyM2.x - pym2.x;
    const float diffy  = (fabs(pym2.x - pc.x) +
                          fabs(pyM2.x - pc.x) + 
                          fabs(pym.x  - pyM.x)) * 3.0f +
                         (fabs(pyM3.x - pyM.x) + fabs(pym3.x - pym.x)) * 2.0f;
    if(diffx > diffy)
    {
      // use guessy
      const float m = fmin(pym.x, pyM.x);
      const float M = fmax(pym.x, pyM.x);
      color.y = fmax(fmin(guessy*0.25f, M), m);
    }
    else
    {
      const float m = fmin(pxm.x, pxM.x);
      const float M = fmax(pxm.x, pxM.x);
      color.y = fmax(fmin(guessx*0.25f, M), m);
    }
  }
  write_imagef (out, (int2)(x, y), color);
}

__kernel void
ppg_demosaic_green_median (__read_only image2d_t in, __write_only image2d_t out, const int width, const int height,
                           const unsigned int filters)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  // process all non-green pixels
  const int row = y;
  const int col = x;
  const int c = FC(row, col, filters);

  float4 pc = read_imagef(in, sampleri, (int2)(col, row));

  // fill green layer for red and blue pixels:
  if(c == 0 || c == 2)
  {
    // look up horizontal and vertical neighbours, sharpened weight:
    const float4 pym  = read_imagef(in, sampleri, (int2)(col, row-1));  // g
    const float4 pym2 = read_imagef(in, sampleri, (int2)(col, row-2));
    const float4 pym3 = read_imagef(in, sampleri, (int2)(col, row-3));  // g
    const float4 pyM  = read_imagef(in, sampleri, (int2)(col, row+1));  // g
    const float4 pyM2 = read_imagef(in, sampleri, (int2)(col, row+2));
    const float4 pyM3 = read_imagef(in, sampleri, (int2)(col, row+3));  // g
    const float4 pxm  = read_imagef(in, sampleri, (int2)(col-1, row));  // g
    const float4 pxm2 = read_imagef(in, sampleri, (int2)(col-2, row));
    const float4 pxm3 = read_imagef(in, sampleri, (int2)(col-3, row));  // g
    const float4 pxM  = read_imagef(in, sampleri, (int2)(col+1, row));  // g
    const float4 pxM2 = read_imagef(in, sampleri, (int2)(col+2, row));
    const float4 pxM3 = read_imagef(in, sampleri, (int2)(col+3, row));  // g

    const float pc_c = FCV(pc,c);
    float4 px_c  = (float4)0.0f;
    if (c == 0)
      px_c = (float4)(pxm2.z,pxM2.z,pym2.z,pyM2.z);
    else if (c & 1) 
      px_c = (float4)(pxm2.y,pxM2.y,pym2.y,pyM2.y);
    else
      px_c = (float4)(pxm2.x,pxM2.x,pym2.x,pyM2.x);
    #define pxm2_c px_c.x
    #define pxM2_c px_c.y
    #define pym2_c px_c.z
    #define pyM2_c px_c.w

    const float guessx = (pxm.y + pc_c + pxM.y) * 2.0f - pxM2_c - pxm2_c;
    const float diffx  = (fabs(pxm2_c - pc_c) +
                          fabs(pxM2_c - pc_c) + 
                          fabs(pxm.y  - pxM.y)) * 3.0f +
                         (fabs(pxM3.y - pxM.y) + fabs(pxm3.y - pxm.y)) * 2.0f;
    const float guessy = (pym.y + pc_c + pyM.y) * 2.0f - pyM2_c - pym2_c;
    const float diffy  = (fabs(pym2_c - pc_c) +
                          fabs(pyM2_c - pc_c) + 
                          fabs(pym.y  - pyM.y)) * 3.0f +
                         (fabs(pyM3.y - pyM.y) + fabs(pym3.y - pym.y)) * 2.0f;


    if(diffx > diffy)
    {
      // use guessy
      const float m = fmin(pym.y, pyM.y);
      const float M = fmax(pym.y, pyM.y);
      pc.y = fmax(fmin(guessy*0.25f, M), m);
    }
    else
    {
      const float m = fmin(pxm.y, pxM.y);
      const float M = fmax(pxm.y, pxM.y);
      pc.y = fmax(fmin(guessx*0.25f, M), m);
    }
  }
  write_imagef (out, (int2)(x, y), pc);
}

/**
 * fills the reds and blues in the gaps (done after ppg_demosaic_green).
 * in (float4) -> out (float4)
 */
__kernel void
ppg_demosaic_redblue (__read_only image2d_t in, __write_only image2d_t out, const int width, const int height,
                      const unsigned int filters)
{
  // image in contains full green and sparse r b
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  const int row = y;
  const int col = x;
  const int c = FC(row, col, filters);
  float4 color = read_imagef(in, sampleri, (int2)(col, row));

  if(c == 1 || c == 3)
  { // calculate red and blue for green pixels:
    // need 4-nbhood:
    float4 nt = read_imagef(in, sampleri, (int2)(col, row-1));
    float4 nb = read_imagef(in, sampleri, (int2)(col, row+1));
    float4 nl = read_imagef(in, sampleri, (int2)(col-1, row));
    float4 nr = read_imagef(in, sampleri, (int2)(col+1, row));
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
    float4 ntl = read_imagef(in, sampleri, (int2)(col-1, row-1));
    float4 ntr = read_imagef(in, sampleri, (int2)(col+1, row-1));
    float4 nbl = read_imagef(in, sampleri, (int2)(col-1, row+1));
    float4 nbr = read_imagef(in, sampleri, (int2)(col+1, row+1));

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
 * FCN() is a functional equivalent to FC() avoiding right-shift operations. FC() would trigger
 * a compiler error in kernel border_interpolate() for NVIDIA openCL drivers (as of version 270.41).
 * TODO: revise if still needed for newer versions of NVIDIA's driver.
 */
int
FCN(const int row, const int col, const unsigned int filters)
{
  return (filters >> (2*((2*row & 14) + (col & 1)) )) & 3;
}

/**
 * Demosaic image border
 */
__kernel void
border_interpolate(__read_only image2d_t in, __write_only image2d_t out, const int width, const int height, const unsigned int filters)
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
      int f = FCN(j,i,filters);
      sum[f] += read_imagef(in, sampleri, (int2)(i, j)).x;
      count[f]++;
    }
  }

  float i = read_imagef(in, sampleri, (int2)(x, y)).x;
  o.x = count[0] > 0 ? sum[0]/count[0] : i;
  o.y = count[1]+count[3] > 0 ? (sum[1]+sum[3])/(count[1]+count[3]) : i;
  o.z = count[2] > 0 ? sum[2]/count[2] : i;

  int f = FCN(y,x,filters);

  if     (f == 0) o.x = i;
  else if(f == 1) o.y = i;
  else if(f == 2) o.z = i;
  else            o.y = i;

  write_imagef (out, (int2)(x, y), o);
}

