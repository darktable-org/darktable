/*
    This file is part of darktable,
    copyright (c) 2016 Ulrich Pegelow.

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

#pragma OPENCL EXTENSION cl_amd_printf : enable

#include "common.h"

constant char2 dir[4] = { (char2)(1, 0), (char2)(0, 1), (char2)(1, 1), (char2)(-1, 1) };

// temporary be here to fill in image borders
kernel void
markesteijn_border_interpolate(read_only image2d_t in, write_only image2d_t out, const int width, const int height,
                               const int rin_x, const int rin_y, global const unsigned char (*const xtrans)[6])
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  const int border = 11;
  const int avgwindow = 1;

  if(x >= border && x < width-border && y >= border && y < height-border) return;

  float o[4] = { 0.0f };
  float sum[4] = { 0.0f };
  int count[4] = { 0 };

  for(int j = y-avgwindow; j <= y+avgwindow; j++)
    for(int i = x-avgwindow; i <= x+avgwindow; i++)
    {
      if(j >= 0 && i >= 0 && j < height && i < width)
      {
        int f = FCxtrans(j + rin_y, i + rin_x, xtrans);
        sum[f] += read_imagef(in, sampleri, (int2)(i, j)).x;
        count[f]++;
      }
    }

  float i = read_imagef(in, sampleri, (int2)(x, y)).x;

  int f = FCxtrans(y + rin_y, x + rin_x, xtrans);

  for(int c = 0; c < 3; c++)
  {
    if(c != f && count[c] != 0)
      o[c] = sum[c] / count[c];
    else
      o[c] = i;
  }

  write_imagef (out, (int2)(x, y), (float4)(o[0], o[1], o[2], o[3]));
}


// Copy current tile from in to image buffer.
kernel void
markesteijn_initial_copy(read_only image2d_t in, global float *rgb, const int width, const int height,
                         const int rin_x, const int rin_y, global const unsigned char (*const xtrans)[6])
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  global float *pix = rgb + 4 * mad24(y, width, x);
  
  const int f = FCxtrans(y + rin_y, x + rin_x, xtrans);
  
  float p = read_imagef(in, sampleri, (int2)(x, y)).x;
  
  for(int c = 0; c < 3; c++) 
    pix[c] = (c == f) ? p : 0.0f;
}

int
hexidx1(const char2 hex, const int stride)
{
  return mad24(hex.y, stride, hex.x);
}

int
hexidx4(const char2 hex, const int stride)
{
  return mul24(mad24(hex.y, stride, hex.x), 4);
}


// possible offsets of a neighboring pixel in x/y directions
constant const char2 dir2nd[4] = { (char2)(1, 0), (char2)(0, 1), (char2)(-1, 0), (char2)(0, -1) };

// Set green1 and green3 of red/blud pixel pairs to the minimum and maximum allowed values
kernel void
markesteijn_green_minmax(global float *rgb, global float *gminmax, const int width, const int height, 
                         const int rin_x, const int rin_y, const char2 sgreen, global const unsigned char (*const xtrans)[6], 
                         global const char2 (*const allhex)[3][8], local float *buffer)
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

  // total size of rgb (in units of 1*float)
  const int rgb_max = mul24(width, height);
  
  // we locally buffer rgb_0[1] (i.e. green) to speed-up reading
  for(int n = 0; n <= maxbuf/lsz; n++)
  {
    const int bufidx = mad24(n, lsz, l);
    if(bufidx >= maxbuf) continue;
    const int xx = xul + bufidx % stride;
    const int yy = yul + bufidx / stride;
    const int inidx = mad24(yy, width, xx);
    buffer[bufidx] = (inidx >= 0 && inidx < rgb_max) ? (rgb + 4 * inidx)[1] : 0.0f;
  }
  
  barrier(CLK_LOCAL_MEM_FENCE);

  // center the local buffer around current x,y-Pixel
  local float *buff = buffer +  mad24(ylid + 3, stride, xlid + 3);
  
  // take sufficient border into account
  if(x < 3 || x >= width-3 || y < 3 || y >= height-3) return;
  
  // the color of this pixel
  const int f = FCxtrans(y + rin_y, x + rin_x, xtrans);
  
  // we only work on non-green pixels
  if(f == 1) return;

  // get min/max of *this* pixel
  float gmin = FLT_MAX;
  float gmax = FLT_MIN;
  global const char2 *const hex = allhex[y % 3][x % 3];
  for(int c = 0; c < 6; c++)
  {
    const float val = buff[hexidx1(hex[c], stride)];
    if(gmin > val) gmin = val;
    if(gmax < val) gmax = val;
  }

  // find the *one* neighboring non-green pixel in four directions
  local float *buff2nd = buff;                    // initialized here only to prevent compiler warning
  global const char2 *hex2nd = hex;               // initialized here only to prevent compiler warning
  for (int n = 0; n < 4; n++)
  {
    if(FCxtrans(y + dir2nd[n].y + rin_y, x + dir2nd[n].x + rin_x, xtrans) == 1) continue; // this is a green pixel
    buff2nd = buff + mad24(dir2nd[n].y, stride, dir2nd[n].x);
    hex2nd = allhex[(y + dir2nd[n].y) % 3][(x + dir2nd[n].x) % 3];
  }
  
  // we have found the second pixel: now include it into min/max calculation
  for(int c = 0; c < 6; c++)
  {
    const float val = buff2nd[hexidx1(hex2nd[c], stride)];
    if(gmin > val) gmin = val;
    if(gmax < val) gmax = val;
  }
  
  const int glidx = mad24(y, width, x);
  vstore2((float2)(gmin, gmax), glidx, gminmax);
}
 


// Interpolate green horizontally, vertically, and along both diagonals
kernel void
markesteijn_interpolate_green(global float *rgb_0, global float *rgb_1, global float *rgb_2, global float *rgb_3,
                              global float *gminmax, const int width, const int height, const int rin_x, const int rin_y, 
                              const char2 sgreen, global const unsigned char (*const xtrans)[6], 
                              global const char2 (*const allhex)[3][8], local float *buffer)
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
  // cells of 4*float per pixel with a border of 6 cells
  const int stride = xlsz + 2*6;
  const int maxbuf = mul24(stride, ylsz + 2*6);

  // coordinates of top left pixel of buffer
  // this is 6 pixel left and above of the work group origin
  const int xul = mul24(xgid, xlsz) - 6;
  const int yul = mul24(ygid, ylsz) - 6;

  // total size of rgb_0 (in units of 4*float)
  const int rgb_0_max = mul24(width, height);
  
  // we locally buffer rgb_0 to speed-up reading
  for(int n = 0; n <= maxbuf/lsz; n++)
  {
    const int bufidx = mad24(n, lsz, l);
    if(bufidx >= maxbuf) continue;
    const int xx = xul + bufidx % stride;
    const int yy = yul + bufidx / stride;
    const int inidx = mad24(yy, width, xx);
    float4 pixel = (inidx >= 0 && inidx < rgb_0_max) ? vload4(inidx, rgb_0) : (float4)0.0f;
    vstore4(pixel, bufidx, buffer);
  }
  
  barrier(CLK_LOCAL_MEM_FENCE);

  // center the local buffer around current x,y-Pixel
  local float *buff = buffer + 4 * mad24(ylid + 6, stride, xlid + 6);
  
  // take sufficient border into account
  if(x < 3 || x >= width-3 || y < 3 || y >= height-3) return;
  
  // the color of this pixel
  const int f = FCxtrans(y + rin_y, x + rin_x, xtrans);
  
  // we only work on non-green pixels
  if(f == 1) return;


  // get min/max of this pixel
  float gmin = (gminmax + 2 * mad24(y, width, x))[0];
  float gmax = (gminmax + 2 * mad24(y, width, x))[1];
  
  global const char2 *const hex = allhex[y % 3][x % 3];

  float color[8];
  
  color[0] = 0.6796875f * ((buff + hexidx4(hex[1], stride))[1] + (buff + hexidx4(hex[0], stride))[1])
             - 0.1796875f * ((buff + 2 * hexidx4(hex[1], stride))[1] + (buff + 2 * hexidx4(hex[0], stride))[1]);
                     
  color[1] = 0.87109375f * (buff + hexidx4(hex[3], stride))[1] 
             + 0.13f * (buff + hexidx4(hex[2], stride))[1]
             + 0.359375f * ((buff)[f] - (buff - hexidx4(hex[2], stride))[f]);
                     
  for(int c = 0; c < 2; c++)
  {
    color[2 + c] = 0.640625f * (buff + hexidx4(hex[4 + c], stride))[1] 
                   + 0.359375f * (buff - 2 * hexidx4(hex[4 + c], stride))[1]
                   + 0.12890625f * (2.0f * (buff)[f]
                                    - (buff + 3 * hexidx4(hex[4 + c], stride))[f] 
                                    - (buff - 3 * hexidx4(hex[4 + c], stride))[f]);
  }  
  
  float out[4];
  for(int c = 0; c < 4; c++)
  {
    // note: the original C99 code makes use of the fact that (boolean)TRUE is translates to (int)1.
    //       in OpenCL (boolean)TRUE is represented by (int)-1, though.
    const int d = (!((y - sgreen.y) % 3)) ? 1 : 0;
    out[c ^ d] = clamp(color[c], gmin, gmax);
  }

  global float *rgb[4] = { rgb_0, rgb_1, rgb_2, rgb_3 };
  const int glidx = 4 * mad24(y, width, x);

  for(int c = 0; c < 4; c++)
    (rgb[c] + glidx)[1] = out[c];
}
 
