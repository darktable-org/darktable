/*
    This file is part of darktable,
    Copyright (C) 2016-2026 darktable developers.

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

kernel void
vng_lin_interpolate(read_only image2d_t in,
                    write_only image2d_t out,
                    const int width,
                    const int height,
                    const int border,
                    const unsigned int filters,
                    global const unsigned char (*const xtrans)[6],
                    global const int (*const lookup)[16][32],
                    local float *buffer,
                    const int equil)
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

  // stride and maximum capacity of buffer (note: 1 cell per pixel)
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
    buffer[bufidx] = fmax(0.0f, read_imagef(in, sampleri, (int2)(xx, yy)).x);
  }

  // center buffer around current x,y-Pixel
  buffer += mad24(ylid + 1, stride, xlid + 1);

  barrier(CLK_LOCAL_MEM_FENCE);
  if(x >= width || y >= height) return;

  if(border > 0 && x > border && y > border && x <= width - border && y <= height - border)
    return;

  const bool is_xtrans = filters == 9; 
  const int colors = is_xtrans ? 3 : 4;
  const int av = (filters == 9) ? 2 : 1;
  const int size = is_xtrans ? 6 : 16;

  float sum[4] = { 0.0f };
  float o[4] = { 0.0f };

  if(x < 1 || x >= width-1 || y < 1 || y >= height-1)
  {
    int count[4] = { 0 };
    for(int j = y-av; j <= y+av; j++)
    {
      for(int i = x-av; i <= x+av; i++)
      {
        if(j >= 0 && i >= 0 && j < height && i < width)
        {
          const int f = fcol(j, i, filters, xtrans);
          sum[f] += fmax(0.0f, read_imagef(in, sampleri, (int2)(i, j)).x);
          count[f]++;
        }
      }
    }
    for(int c = 0; c < colors; c++)
      o[c] = sum[c] / fmax(1.0f, (float)count[c]);
  }
  else
  {
    global const int *ip = lookup[y % size][x % size];
    const int num_pixels = ip[0];
    ip++;

    // for each adjoining pixel not of this pixel's color, sum up its weighted values
    for(int i = 0; i < num_pixels; i++, ip += 3)
    {
      const int offset = ip[0];
      const int xx = (short)(offset & 0xffffu);
      const int yy = (short)(offset >> 16);
      const int idx = mad24(yy, stride, xx);
      sum[ip[2]] += buffer[idx] * ip[1];
    }
    // for each interpolated color, load it into the pixel
    for(int i = 0; i < colors - 1; i++, ip += 2)
    {
      o[ip[0]] = sum[ip[0]] / ip[1];
    }
    o[ip[0]] = buffer[0];
  }

  if(!is_xtrans && equil)
  {
    o[1] = 0.5f * (o[1] + o[3]);
    o[3] = 0.0f;
  }
  write_imagef(out, (int2)(x, y), (float4)(o[0], o[1], o[2], o[3]));
}

kernel void
vng_interpolate(read_only image2d_t input,
                read_only image2d_t in,
                write_only image2d_t out,
                const int width,
                const int height,
                const unsigned int filters,
                global const unsigned char (*const xtrans)[6],
                global const int (*const ips),
                global const int (*const code)[16],
                local float *buffer)
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

  // stride and maximum capacity of buffer (note: 4 cells per pixel)
  const int stride = xlsz + 4;
  const int maxbuf = mul24(stride, ylsz + 4);

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
    const float4 pixel = fmax(0.0f, read_imagef(in, sampleri, (int2)(xx, yy)));
    vstore4(pixel, bufidx, buffer);
  }

  // center buffer around current x,y-Pixel
  buffer += 4 * mad24(ylid + 2, stride, xlid + 2);

  barrier(CLK_LOCAL_MEM_FENCE);
  if(x >= width || y >= height) return;

  const bool is_xtrans = filters == 9; 
  const int colors = is_xtrans ? 3 : 4;
  const int av = is_xtrans ? 2 : 1;

  const int prow = is_xtrans ? 6 : 8;
  const int pcol = is_xtrans ? 6 : 2;

  float gval[8] = { 0.0f };
  int g;

  // get byte code
  global const int *ip = ips + code[y % prow][x % pcol];

  // calculate gradients
  while((g = ip[0]) != INT_MAX)
  {
    const int offset0 = g;
    const int x0 = (short)(offset0 & 0xffffu);
    const int y0 = (short)(offset0 >> 16);
    const int idx0 = 4 * mad24(y0, stride, x0);

    const int offset1 = ip[1];
    const int x1 = (short)(offset1 & 0xffffu);
    const int y1 = (short)(offset1 >> 16);
    const int idx1 = 4 * mad24(y1, stride, x1);

    const int weight = (short)(ip[2] & 0xffffu);
    const int color = (short)(ip[2] >> 16);

    const float diff = fabs(buffer[idx0 + color] - buffer[idx1 + color]) * weight;

    gval[ip[3]] += diff;
    ip += 5;
    if((g = ip[-1]) == -1) continue;
    gval[g] += diff;
    while((g = *ip++) != -1) gval[g] += diff;
  }
  ip++;

  // chose a threshold
  float gmin = gval[0];
  float gmax = gval[0];
  for(g = 1; g < 8; g++)
  {
    if(gmin > gval[g]) gmin = gval[g];
    if(gmax < gval[g]) gmax = gval[g];
  }

  const bool border = x < 2 || x >= width-2 || y < 2 || y >= height-2;
  float b[4] = { 0.0f };
  // Ok lets calculate the border pixel, we'll need it soon
  if(border)
  {
    int count[4] = { 0 };
    for(int j = y-av; j <= y+av; j++)
    {
      for(int i = x-av; i <= x+av; i++)
      {
        if(j >= 0 && i >= 0 && j < height && i < width)
        {
          const int f = fcol(j, i, filters, xtrans);
          b[f] += fmax(0.0f, read_imagef(input, sampleri, (int2)(i, j)).x);
          count[f]++;
        }
      }
    }
    for(int c = 0; c < colors; c++)
      b[c] /= fmax(1.0f, (float)count[c]);
  }

  if(gmax == 0.0f)
  {
    if(!border)
    {
      for(int c = 0; c < colors; c++)
        b[c] = buffer[c];
    }
    if(!is_xtrans)
    {
      b[1] = 0.5f * (b[1] + b[3]);
      b[3] = 0.0f;
    }
    write_imagef(out, (int2)(x, y), (float4)(b[0], b[1], b[2], b[3]));
    return;
  }

  const float thold = gmin + (gmax * 0.5f);
  float sum[4] = { 0.0f };
  const int color = fcol(y, x, filters, xtrans);
  int num = 0;

  // average the neighbors
  for(g = 0; g < 8; g++, ip += 3)
  {
    if(gval[g] <= thold)
    {
      const int offset0 = ip[0];
      const int x0 = (short)(offset0 & 0xffffu);
      const int y0 = (short)(offset0 >> 16);
      const int idx0 = 4 * mad24(y0, stride, x0);

      const int offset1 = ip[1];
      const int x1 = (short)(offset1 & 0xffffu);
      const int y1 = (short)(offset1 >> 16);
      const int idx1 = 4 * mad24(y1, stride, x1);

      const int c1 = ip[2];

      for(int c = 0; c < colors; c++)
      {
        if(c == color && (idx1 + c1))
        {
          sum[c] += (buffer[c] + buffer[idx1 + c1]) * 0.5f;
        }
        else
        {
          sum[c] += buffer[idx0 + c];
        }
      }
      num++;
    }
  }

  // save to output
  float o[4] = { 0.0f };
  for(int c = 0; c < colors; c++)
  {
    float tot = buffer[color];
    if(c != color) tot += (sum[c] - sum[color]) / num;
    o[c] = fmax(0.0f, tot);
  }

  if(border)
  {
    for(int c = 0; c < colors; c++)
      o[c] = b[c];
  }

  if(!is_xtrans)
  {
    o[1] = 0.5f * (o[1] + o[3]);
    o[3] = 0.0f;
  }
  write_imagef(out, (int2)(x, y), (float4)(o[0], o[1], o[2], 0.0f));
}

kernel void
clip_and_zoom_demosaic_third_size_xtrans(read_only image2d_t in, write_only image2d_t out, const int width, const int height,
                                         const int rin_wd, const int rin_ht, const float r_scale,
                                         global const unsigned char (*const xtrans)[6])
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  // A slightly different algorithm than
  // clip_and_zoom_demosaic_half_size() which aligns to 2x2
  // Bayer grid and hence most pull additional data from all edges
  // which don't align with CFA. Instead align to a 3x3 pattern (which
  // is semi-regular in X-Trans CFA). If instead had aligned the
  // samples to the full 6x6 X-Trans CFA, wouldn't need to perform a
  // CFA lookup, but then would only work at 1/6 scale or less. This
  // code doesn't worry about fractional pixel offset of top/left of
  // pattern nor oversampling by non-integer number of samples.

  float col[3] = { 0.0f };
  int num = 0;

  const float px_footprint = 1.0f/r_scale;
  const int samples = max(1, (int)floor(px_footprint / 3.0f));

  const int px = clamp((int)round((x - 0.5f) * px_footprint), 0, rin_wd - 3);
  const int py = clamp((int)round((y - 0.5f) * px_footprint), 0, rin_ht - 3);

  const int xmax = min(rin_wd - 3, px + 3 * samples);
  const int ymax = min(rin_ht - 3, py + 3 * samples);

  for(int yy = py; yy <= ymax; yy += 3)
    for(int xx = px; xx <= xmax; xx += 3)
    {
      for(int j = 0; j < 3; j++)
        for(int i = 0; i < 3; i++)
          col[FCxtrans(yy + j, xx + i, xtrans)] += fmax(0.0f, read_imagef(in, sampleri, (int2)(xx + i, yy + j)).x);
      num++;
    }

  // X-Trans RGB weighting averages to 2:5:2 for each 3x3 cell
  col[0] = col[0] / (num * 2);
  col[1] = col[1] / (num * 5);
  col[2] = col[2] / (num * 2);

  write_imagef(out, (int2)(x, y), (float4)(col[0], col[1], col[2], 0.0f));
}

