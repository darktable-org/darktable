/*
  This file is part of darktable,
  Copyright (C) 2026 darktable developers.

  darktable is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  darktable is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "common.h"

// process() of src/iop/cacorrect.c.
//
// The CPU code works on 128x128 tiles that overlap by 16 sensels, and each
// sensel is counted and corrected by exactly one of them, the one whose inner
// 112x112 part holds it. Inside a tile, every value it reads equals the same
// value computed over the whole image extended by 8 sensels on each side, so
// the kernels work on that extended image (the "E" buffers, 16 sensels wider
// and higher, with image row r at E row r + 8). The extension copies the
// tiles' border fill, including its corners, which are not mirrored the way
// the edges are.
//
// The per-tile sums that the CPU accumulates go to the host, which fits the
// polynomial in double precision as the CPU code does and returns the shift
// of each tile.

#define CA_EPS 1e-5f
#define CA_TILE 112 // ts - 2 * border in cacorrect.c

static inline float _ca_sqrf(const float x)
{
  return x * x;
}

static inline float _ca_interpolatef(const float a, const float b, const float c)
{
  return a * (b - c) + c;
}

kernel void
cacorrect_load(read_only image2d_t in,
               global float *out,
               const int width,
               const int height,
               const float scale)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;

  out[mad24(y, width, x)] = Areadsingle(in, x, y) * scale;
}

// the cropped output, scaled back
kernel void
cacorrect_store(global const float *in,
                write_only image2d_t out,
                const int iwidth,
                const int owidth,
                const int oheight,
                const int dx,
                const int dy,
                const float scale)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= owidth || y >= oheight) return;

  write_imagef(out, (int2)(x, y), in[mad24(y + dy, iwidth, x + dx)] * scale);
}

// the sensel the tiles' border fill puts at image position (row, col), for
// row in [-8, height + 8) and col in [-8, width + 8)
static inline int2 _ca_source(const int row, const int col, const int width, const int height)
{
  const bool top = row < 0;
  const bool bottom = row >= height;
  const bool left = col < 0;
  const bool right = col >= width;
  int r = row;
  int c = col;
  if(bottom) r = 2 * height - 2 - row;
  if(right) c = 2 * width - 2 - col;
  if(top) r = left || right ? 8 - row : -row;
  if(left) c = top || bottom ? 8 - col : -col;
  return (int2)(c, r);
}

// the extended image of src, which is width x height with its first sensel
// at origin and stride sensels per row
kernel void
cacorrect_extend(global const float *src,
                 global float *ext,
                 const int width,
                 const int height,
                 const int origin,
                 const int stride)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  const int ewidth = width + 16;
  if(x >= ewidth || y >= height + 16) return;

  const int2 s = _ca_source(y - 8, x - 8, width, height);
  ext[mad24(y, ewidth, x)] = src[origin + mad24(s.y, stride, s.x)];
}

// green at the red and blue sensels, a directionally weighted average; the
// tiles leave it at 0 within 3 sensels of their edges
kernel void
cacorrect_green(global const float *raw,
                global float *green,
                const int ewidth,
                const int eheight,
                const unsigned int filters)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= ewidth || y >= eheight) return;

  const int i = mad24(y, ewidth, x);
  const int c = FC(y, x, filters);
  if(c == 1)
  {
    green[i] = raw[i];
    return;
  }
  if(x < 3 || y < 3 || x >= ewidth - 3 || y >= eheight - 3)
  {
    green[i] = 0.0f;
    return;
  }
  const int v1 = ewidth;
  const int v2 = 2 * ewidth;
  const int v3 = 3 * ewidth;
  const float wtu = 1.0f / _ca_sqrf(CA_EPS + fabs(raw[i + v1] - raw[i - v1])
                                    + fabs(raw[i] - raw[i - v2])
                                    + fabs(raw[i - v1] - raw[i - v3]));
  const float wtd = 1.0f / _ca_sqrf(CA_EPS + fabs(raw[i - v1] - raw[i + v1])
                                    + fabs(raw[i] - raw[i + v2])
                                    + fabs(raw[i + v1] - raw[i + v3]));
  const float wtl = 1.0f / _ca_sqrf(CA_EPS + fabs(raw[i + 1] - raw[i - 1])
                                    + fabs(raw[i] - raw[i - 2])
                                    + fabs(raw[i - 1] - raw[i - 3]));
  const float wtr = 1.0f / _ca_sqrf(CA_EPS + fabs(raw[i - 1] - raw[i + 1])
                                    + fabs(raw[i] - raw[i + 2])
                                    + fabs(raw[i + 1] - raw[i + 3]));
  green[i] = (wtu * raw[i - v1] + wtd * raw[i + v1] + wtl * raw[i - 1] + wtr * raw[i + 1])
             / (wtu + wtd + wtl + wtr);
}

// the six high and low pass filters at the red and blue sensels, in planes of
// eheight x hwidth with one value per pair of columns:
// 0 rbhpfv, 1 rbhpfh, 2 rblpfv, 3 rblpfh, 4 grblpfv, 5 grblpfh
kernel void
cacorrect_filters(global const float *raw,
                  global const float *green,
                  global float *filt,
                  const int ewidth,
                  const int eheight,
                  const int hwidth,
                  const unsigned int filters)
{
  const int y = get_global_id(1);
  if(y >= eheight) return;
  const int x = 2 * (int)get_global_id(0) + (FC(y, 0, filters) & 1);
  if(x >= ewidth) return;

  const int plane = eheight * hwidth;
  const int h = mad24(y, hwidth, x >> 1);
  if(x < 4 || y < 4 || x >= ewidth - 4 || y >= eheight - 4)
  {
    for(int p = 0; p < 6; p++) filt[p * plane + h] = 0.0f;
    return;
  }
  const int i = mad24(y, ewidth, x);
  const int v2 = 2 * ewidth;
  const int v4 = 4 * ewidth;
  filt[h] = fabs(fabs((green[i] - raw[i]) - (green[i + v4] - raw[i + v4]))
                 + fabs((green[i - v4] - raw[i - v4]) - (green[i] - raw[i]))
                 - fabs((green[i - v4] - raw[i - v4]) - (green[i + v4] - raw[i + v4])));
  filt[plane + h] = fabs(fabs((green[i] - raw[i]) - (green[i + 4] - raw[i + 4]))
                         + fabs((green[i - 4] - raw[i - 4]) - (green[i] - raw[i]))
                         - fabs((green[i - 4] - raw[i - 4]) - (green[i + 4] - raw[i + 4])));
  const float glpfv = 0.25f * (2.0f * green[i] + green[i + v2] + green[i - v2]);
  const float glpfh = 0.25f * (2.0f * green[i] + green[i + 2] + green[i - 2]);
  const float rblv = 0.25f * (2.0f * raw[i] + raw[i + v2] + raw[i - v2]);
  const float rblh = 0.25f * (2.0f * raw[i] + raw[i + 2] + raw[i - 2]);
  filt[2 * plane + h] = CA_EPS + fabs(glpfv - rblv);
  filt[3 * plane + h] = CA_EPS + fabs(glpfh - rblh);
  filt[4 * plane + h] = glpfv + rblv;
  filt[5 * plane + h] = glpfh + rblh;
}

// the color variance coefficients of one tile per 16x16 work group, into
// sums[tile * 12 + dir * 6 + k * 2 + color], as coeff[dir][k][color] in
// cacorrect.c. The host launches it with a local size of 16x16.
kernel void
cacorrect_tile_sums(global const float *raw,
                    global const float *green,
                    global const float *filt,
                    global float *sums,
                    const int width,
                    const int height,
                    const int hwidth,
                    const int htiles,
                    const unsigned int filters,
                    local float *buffer)
{
  const int lx = get_local_id(0);
  const int ly = get_local_id(1);
  const int l = mad24(ly, 16, lx);
  const int ewidth = width + 16;
  const int plane = (height + 16) * hwidth;
  const int row0 = (int)get_group_id(1) * CA_TILE;
  const int col0 = (int)get_group_id(0) * CA_TILE;
  const int row1 = min(row0 + CA_TILE, height);
  const int col1 = min(col0 + CA_TILE, width);

  float s[12];
  for(int k = 0; k < 12; k++) s[k] = 0.0f;

  for(int row = row0 + ly; row < row1; row += 16)
  {
    const int y = row + 8;
    for(int col = col0 + (FC(row, 0, filters) & 1) + 2 * lx; col < col1; col += 32)
    {
      const int x = col + 8;
      const int i = mad24(y, ewidth, x);
      const int h = mad24(y, hwidth, x >> 1);
      const int hv = 2 * hwidth; // two rows up or down in the filter planes
      const int ci = FC(row, col, filters) >> 1;

      // vertical
      float gdiff = 0.3125f * (green[i + ewidth] - green[i - ewidth])
                    + 0.09375f * (green[i + ewidth + 1] - green[i - ewidth + 1]
                                  + green[i + ewidth - 1] - green[i - ewidth - 1]);
      const float deltgrb = raw[i] - green[i];
      float gradwt = fabs(0.25f * filt[h] + 0.125f * (filt[h + 1] + filt[h - 1]))
                     * (filt[4 * plane + h - hv] + filt[4 * plane + h + hv])
                     / (CA_EPS + 0.1f * (filt[4 * plane + h - hv] + filt[4 * plane + h + hv])
                        + filt[2 * plane + h - hv] + filt[2 * plane + h + hv]);
      s[ci] += gradwt * deltgrb * deltgrb;
      s[2 + ci] += gradwt * gdiff * deltgrb;
      s[4 + ci] += gradwt * gdiff * gdiff;

      // horizontal
      gdiff = 0.3125f * (green[i + 1] - green[i - 1])
              + 0.09375f * (green[i + 1 + ewidth] - green[i - 1 + ewidth]
                            + green[i + 1 - ewidth] - green[i - 1 - ewidth]);
      gradwt = fabs(0.25f * filt[plane + h] + 0.125f * (filt[plane + h + hv] + filt[plane + h - hv]))
               * (filt[5 * plane + h - 1] + filt[5 * plane + h + 1])
               / (CA_EPS + 0.1f * (filt[5 * plane + h - 1] + filt[5 * plane + h + 1])
                  + filt[3 * plane + h - 1] + filt[3 * plane + h + 1]);
      s[6 + ci] += gradwt * deltgrb * deltgrb;
      s[8 + ci] += gradwt * gdiff * deltgrb;
      s[10 + ci] += gradwt * gdiff * gdiff;
    }
  }

  for(int k = 0; k < 12; k++) buffer[k * 256 + l] = s[k];
  barrier(CLK_LOCAL_MEM_FENCE);
  for(int offset = 128; offset > 0; offset >>= 1)
  {
    if(l < offset)
      for(int k = 0; k < 12; k++) buffer[k * 256 + l] += buffer[k * 256 + l + offset];
    barrier(CLK_LOCAL_MEM_FENCE);
  }
  if(l < 12) sums[mad24((int)get_group_id(1), htiles, (int)get_group_id(0)) * 12 + l] = buffer[l * 256];
}

typedef struct
{
  int vfloor, vceil, hfloor, hceil;
  float vfrac, hfrac;
} ca_shift_t;

// green at the CA shift point of extended sensel i
static inline float _ca_gint(global const float *green, const int i, const int ewidth, const ca_shift_t s)
{
  const float gfloor = _ca_interpolatef(s.hfrac, green[i + s.vfloor * ewidth + s.hceil],
                                        green[i + s.vfloor * ewidth + s.hfloor]);
  const float gceil = _ca_interpolatef(s.hfrac, green[i + s.vceil * ewidth + s.hceil],
                                       green[i + s.vceil * ewidth + s.hfloor]);
  return _ca_interpolatef(s.vfrac, gceil, gfloor);
}

// the correction pass: red and blue from the color differences at their
// shifted optical positions, green copied. shifts holds the vertical and
// horizontal shift of red and then of blue for each tile.
kernel void
cacorrect_correct(global const float *raw,
                  global const float *green,
                  global const float *shifts,
                  global float *out,
                  const int width,
                  const int height,
                  const int htiles,
                  const unsigned int filters)
{
  const int col = get_global_id(0);
  const int row = get_global_id(1);
  if(col >= width || row >= height) return;

  const int ewidth = width + 16;
  const int i = mad24(row + 8, ewidth, col + 8);
  const int c = FC(row, col, filters);
  if(c == 1)
  {
    out[mad24(row, width, col)] = raw[i];
    return;
  }

  const int tile = (row / CA_TILE) * htiles + col / CA_TILE;
  const float lbv = shifts[tile * 4 + (c >> 1) * 2];
  const float lbh = shifts[tile * 4 + (c >> 1) * 2 + 1];

  ca_shift_t s;
  s.vfloor = (int)floor(lbv);
  s.vceil = (int)ceil(lbv);
  if(lbv < 0.0f)
  {
    const int t = s.vfloor;
    s.vfloor = s.vceil;
    s.vceil = t;
  }
  s.vfrac = fabs(lbv - (float)s.vfloor);
  s.hfloor = (int)floor(lbh);
  s.hceil = (int)ceil(lbh);
  if(lbh < 0.0f)
  {
    const int t = s.hfloor;
    s.hfloor = s.hceil;
    s.hceil = t;
  }
  s.hfrac = fabs(lbh - (float)s.hfloor);
  const int dirv = lbv > 0.0f ? 2 : -2;
  const int dirh = lbh > 0.0f ? 2 : -2;

  // the color differences and shifted greens at this sensel (0), dirh
  // columns to the left (1), dirv rows up (2) and both (3)
  const int k[4] = { i, i - dirh, i - dirv * ewidth, i - dirv * ewidth - dirh };
  float grbdiff[4];
  float gshift[4];
  for(int n = 0; n < 4; n++)
  {
    gshift[n] = _ca_gint(green, k[n], ewidth, s);
    grbdiff[n] = gshift[n] - raw[k[n]];
  }

  const float hfrac = s.hfrac / 2.0f;
  const float vfrac = s.vfrac / 2.0f;
  const float g = green[i];
  float o = raw[i];
  const float grbdiffold = g - o;
  const float grbdiffinthfloor = _ca_interpolatef(hfrac, grbdiff[1], grbdiff[0]);
  const float grbdiffinthceil = _ca_interpolatef(hfrac, grbdiff[3], grbdiff[2]);
  float grbdiffint = _ca_interpolatef(vfrac, grbdiffinthceil, grbdiffinthfloor);

  const float RBint = g - grbdiffint;
  if(fabs(RBint - o) < 0.25f * (RBint + o))
  {
    if(fabs(grbdiffold) > fabs(grbdiffint)) o = RBint;
  }
  else
  {
    const float p0 = 1.0f / (CA_EPS + fabs(g - gshift[0]));
    const float p1 = 1.0f / (CA_EPS + fabs(g - gshift[1]));
    const float p2 = 1.0f / (CA_EPS + fabs(g - gshift[2]));
    const float p3 = 1.0f / (CA_EPS + fabs(g - gshift[3]));
    grbdiffint = (p0 * grbdiff[0] + p1 * grbdiff[1] + p2 * grbdiff[2] + p3 * grbdiff[3])
                 / (p0 + p1 + p2 + p3);
    if(fabs(grbdiffold) > fabs(grbdiffint)) o = g - grbdiffint;
  }

  // if color difference interpolation overshot the correction, just desaturate
  if(grbdiffold * grbdiffint < 0.0f) o = g - 0.5f * (grbdiffold + grbdiffint);
  out[mad24(row, width, col)] = o;
}

// the colorshift avoidance: per red and blue sensel, the ratio of its value
// before and after the correction, in half-size planes. (prow, pcol) is the
// parity of the plane's sensels, and the odd last row and column of the
// planes repeat the ones before them, as in process()
kernel void
cacorrect_factors(global const float *before,
                  global const float *after,
                  write_only image2d_t factors,
                  const int width,
                  const int height,
                  const int hwidth,
                  const int hheight,
                  const int prow,
                  const int pcol,
                  const int fixrow,
                  const int fixcol)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= hwidth || y >= hheight) return;

  const int fx = fixcol && x == hwidth - 1 ? x - 1 : x;
  const int fy = fixrow && y == hheight - 1 ? y - 1 : y;
  const int row = 2 * fy + prow;
  const int col = 2 * fx + pcol;
  float f = 0.0f;
  if(row < height && col < width)
  {
    const int i = mad24(row, width, col);
    // CLAMPF() turns the NaN of 0 / 0 into the lower bound
    const float q = before[i] / after[i];
    f = q >= 0.5f ? fmin(q, 2.0f) : 0.5f;
  }
  write_imagef(factors, (int2)(x, y), f);
}

kernel void
cacorrect_apply_factors(global float *buf,
                        read_only image2d_t red,
                        read_only image2d_t blue,
                        const int width,
                        const int height,
                        const unsigned int filters)
{
  const int col = get_global_id(0);
  const int row = get_global_id(1);
  if(col >= width - 2 || row < 2 || row >= height - 2) return;

  const int c = FC(row, col, filters);
  if(c == 1) return;
  const float f = c == 0 ? Areadsingle(red, col / 2, row / 2) : Areadsingle(blue, col / 2, row / 2);
  buf[mad24(row, width, col)] *= f;
}
