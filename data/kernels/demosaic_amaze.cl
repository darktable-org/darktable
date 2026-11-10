/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

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

// AMaZE demosaic (src/iop/demosaicing/amaze.cc, RawTherapee code by Emil
// Martinec and Ingo Weyrich) as whole-image passes.
//
// The CPU code works on 160x160 tiles with a 16 pixel border, mirrored at
// the image edges. Here every buffer covers the image plus that border
// (pw x ph, the image at an offset of 16), and each pass computes its whole
// area, so the interior sees the same neighbors as in a tile. Each pass keeps
// the CPU margin to the padded edge (2, 4, 6, ...), where a tile has no data.
//
// Three CPU loops update a buffer in place while reading neighbors that the
// same loop already updated (the color difference bounding, the hvwt and pmwt
// neighbor votes), so their result depends on the scan order inside a tile.
// Those run here one CPU tile per work group, in the CPU order (amaze_cd_bound,
// amaze_vote). The Nyquist test has no per-tile bounding box, which changes
// nothing visible.
//
// The CPU stores the R/B-only arrays at half width, indexed by indx >> 1.
// Here they are full size, at the R/B site of each pair; the one place where
// the half indexing reaches further than the neighboring site is rbint in
// the G correction (indx1 -/+ v1 is two rows away), kept as is.

#include "common.h"

#define eps 1e-5f
#define epssq 1e-10f
#define arthresh 0.75f
#define nyqthresh 0.5f

// gaussian on 5x5 quincunx, sigma=1.2
constant float gaussodd[4] = { 0.14659727707323927f, 0.103592713382435f, 0.0732036125103057f,
                                   0.0365543548389495f };
// gaussian on 5x5, multiplied with nyqthresh
constant float gaussgrad[6] = { 0.5f * 0.07384411893421103f, 0.5f * 0.06207511968171489f,
                                    0.5f * 0.0521818194747806f, 0.5f * 0.03687419286733595f,
                                    0.5f * 0.03099732204057846f, 0.5f * 0.018413194161458882f };
// gaussian on 5x5 alt quincunx, sigma=1.5
constant float gausseven[2] = { 0.13719494435797422f, 0.05640252782101291f };
// gaussian on quincunx grid
constant float gquinc[4] = { 0.169917f, 0.108947f, 0.069855f, 0.0287182f };


bool _area(int col, int row, int m, int pw, int ph)
{
  return col >= m && col < pw - m && row >= m && row < ph - m;
}

// the 16 pixel border mirrors the image without repeating its edge pixel,
// which keeps the CFA phase
int _mirror(int i, int n)
{
  i = i < 0 ? -i : i;
  return i >= n ? 2 * n - 2 - i : i;
}

float _ulim(float x, float y, float z)
{
  return y < z ? clamp(x, y, z) : clamp(x, z, y);
}

float _clampnan(float x)
{
  if(isnan(x)) return 0.5f;
  return isinf(x) ? clamp(x, 0.0f, 1.0f) : x;
}

// the gradient based directional weights, computed where they are read
// instead of stored
float _dirwts0(global const float *cfa, int i, int v1)
{
  return eps + fabs(cfa[i + 2 * v1] - cfa[i]) + fabs(cfa[i] - cfa[i - 2 * v1]) + fabs(cfa[i + v1] - cfa[i - v1]);
}

float _dirwts1(global const float *cfa, int i)
{
  return eps + fabs(cfa[i + 2] - cfa[i]) + fabs(cfa[i] - cfa[i - 2]) + fabs(cfa[i + 1] - cfa[i - 1]);
}

// the CPU's delp/delm at an R/B site and Dgrbsq1p/m at a G site, also
// computed where they are read
float _delp(global const float *cfa, int i, int v1) { return fabs(cfa[i - v1 + 1] - cfa[i + v1 - 1]); }
float _delm(global const float *cfa, int i, int v1) { return fabs(cfa[i + v1 + 1] - cfa[i - v1 - 1]); }
float _sq1p(global const float *cfa, int i, int v1)
{
  return fsquare(cfa[i] - cfa[i + v1 - 1]) + fsquare(cfa[i] - cfa[i - v1 + 1]);
}
float _sq1m(global const float *cfa, int i, int v1)
{
  return fsquare(cfa[i] - cfa[i - v1 - 1]) + fsquare(cfa[i] - cfa[i + v1 + 1]);
}

// the raw data with its mirrored border, and the gradient term of the
// Nyquist test
kernel void amaze_init(
                read_only image2d_t in, global float *cfa,
                const int width, const int height, const int pw, const int ph)
{
  const int col = get_global_id(0);
  const int row = get_global_id(1);
  if(col >= pw || row >= ph) return;
  const int x = col - 16, y = row - 16;
  const bool xout = x < 0 || x >= width, yout = y < 0 || y >= height;
  // the CPU fills the top left, top right and bottom left corners of the
  // border from 16 pixels further in than the edges (amaze.cc:349)
  const int sx = x < 0 && yout ? 16 - x : _mirror(x, width);
  const int sy = y < 0 && xout ? 16 - y : _mirror(y, height);
  cfa[row * pw + col] = Areadsingle(in, sx, sy);
}

kernel void amaze_grad(
                global float *cfa, global float *delhvsqsum, const int pw, const int ph)
{
  const int col = get_global_id(0);
  const int row = get_global_id(1);
  if(!_area(col, row, 2, pw, ph)) return;
  const int i = row * pw + col;
  delhvsqsum[i] = fsquare(fabs(cfa[i + 1] - cfa[i - 1])) + fsquare(fabs(cfa[i + pw] - cfa[i - pw]));
}

// interpolate vertical and horizontal color differences
kernel void amaze_cd(
              global float *cfa, global float *vcd, global float *hcd, global float *vcdalt, global float *hcdalt,
              global float *dgintv, global float *dginth,
              const int pw, const int ph, const unsigned int filters, const float clip_pt8)
{
  const int col = get_global_id(0);
  const int row = get_global_id(1);
  if(!_area(col, row, 4, pw, ph)) return;
  const int v1 = pw, v2 = 2 * pw;
  const int i = row * pw + col;
  const float c0 = cfa[i];

  const float d0m2 = _dirwts0(cfa, i - v2, v1), d0 = _dirwts0(cfa, i, v1), d0p2 = _dirwts0(cfa, i + v2, v1);
  const float d1m2 = _dirwts1(cfa, i - 2), d1 = _dirwts1(cfa, i), d1p2 = _dirwts1(cfa, i + 2);

  // color ratios in each cardinal direction
  const float cru = cfa[i - v1] * (d0m2 + d0) / (d0m2 * (eps + c0) + d0 * (eps + cfa[i - v2]));
  const float crd = cfa[i + v1] * (d0p2 + d0) / (d0p2 * (eps + c0) + d0 * (eps + cfa[i + v2]));
  const float crl = cfa[i - 1] * (d1m2 + d1) / (d1m2 * (eps + c0) + d1 * (eps + cfa[i - 2]));
  const float crr = cfa[i + 1] * (d1p2 + d1) / (d1p2 * (eps + c0) + d1 * (eps + cfa[i + 2]));

  // G interpolated in vert/hor directions using Hamilton-Adams method
  const float guha = cfa[i - v1] + 0.5f * (c0 - cfa[i - v2]);
  const float gdha = cfa[i + v1] + 0.5f * (c0 - cfa[i + v2]);
  const float glha = cfa[i - 1] + 0.5f * (c0 - cfa[i - 2]);
  const float grha = cfa[i + 1] + 0.5f * (c0 - cfa[i + 2]);

  // G interpolated in vert/hor directions using adaptive ratios
  float guar = fabs(1.0f - cru) < arthresh ? c0 * cru : guha;
  float gdar = fabs(1.0f - crd) < arthresh ? c0 * crd : gdha;
  float glar = fabs(1.0f - crl) < arthresh ? c0 * crl : glha;
  float grar = fabs(1.0f - crr) < arthresh ? c0 * crr : grha;

  // adaptive weights for vertical/horizontal directions
  const float hwt = _dirwts1(cfa, i - 1) / (_dirwts1(cfa, i - 1) + _dirwts1(cfa, i + 1));
  const float vwt = _dirwts0(cfa, i - v1, v1) / (_dirwts0(cfa, i + v1, v1) + _dirwts0(cfa, i - v1, v1));

  // interpolated G via adaptive weights of cardinal evaluations
  const float Gintvha = vwt * gdha + (1.0f - vwt) * guha;
  const float Ginthha = hwt * grha + (1.0f - hwt) * glha;

  float v, h, va, ha;
  if((FC(row, col, filters) & 1) != 0)
  {
    v = c0 - (vwt * gdar + (1.0f - vwt) * guar);
    h = c0 - (hwt * grar + (1.0f - hwt) * glar);
    va = c0 - Gintvha;
    ha = c0 - Ginthha;
  }
  else
  {
    v = (vwt * gdar + (1.0f - vwt) * guar) - c0;
    h = (hwt * grar + (1.0f - hwt) * glar) - c0;
    va = Gintvha - c0;
    ha = Ginthha - c0;
  }

  if(c0 > clip_pt8 || Gintvha > clip_pt8 || Ginthha > clip_pt8)
  {
    // use HA if highlights are (nearly) clipped
    guar = guha;
    gdar = gdha;
    glar = glha;
    grar = grha;
    v = va;
    h = ha;
  }

  vcd[i] = v;
  hcd[i] = h;
  vcdalt[i] = va;
  hcdalt[i] = ha;
  // differences of interpolations in opposite directions
  dgintv[i] = fmin(fsquare(guha - gdha), fsquare(guar - gdar));
  dginth[i] = fmin(fsquare(glha - grha), fsquare(glar - grar));
}

// the CPU tile of the work group: it starts every 128 pixels (ts - 32) of
// the padded buffers and is 160 wide, or less at the right and bottom edges.
// Only the pixels a tile shows in the output are written by it, and the
// border of the first and last tiles
typedef struct
{
  int ox, oy;     // origin in the padded buffers
  int rr1, cc1;   // tile height and width
  int r0, r1;     // rows written, tile relative
  int c0, c1;     // columns written
} AmazeTile;

AmazeTile _tile(int pw, int ph)
{
  AmazeTile t;
  const int gx = get_group_id(0);
  const int gy = get_group_id(1);
  t.ox = gx * 128;
  t.oy = gy * 128;
  t.rr1 = min(160, ph - t.oy);
  t.cc1 = min(160, pw - t.ox);
  t.r0 = gy == 0 ? 0 : 16;
  t.r1 = min(144, t.rr1);
  t.c0 = gx == 0 ? 0 : 16;
  t.c1 = min(144, t.cc1);
  return t;
}

// the bounding of one color difference in regions of high saturation, with
// n1 and n2 the raw neighbors on its axis
float _cdbound(float d, float c0, float n1, float n2, bool gsite, float clip_pt)
{
  if(gsite)
  {
    const float Gint = -d + c0; // R or B
    if(d > 0.0f)
    {
      if(3.0f * d > (Gint + c0))
        d = -_ulim(Gint, n1, n2) + c0;
      else
      {
        const float wt = 1.0f - 3.0f * d / (eps + Gint + c0);
        d = wt * d + (1.0f - wt) * (-_ulim(Gint, n1, n2) + c0);
      }
    }
    if(Gint > clip_pt) d = -_ulim(Gint, n1, n2) + c0;
  }
  else
  {
    const float Gint = d + c0; // interpolated G
    if(d < 0.0f)
    {
      if(3.0f * d < -(Gint + c0))
        d = _ulim(Gint, n1, n2) - c0;
      else
      {
        const float wt = 1.0f + 3.0f * d / (eps + Gint + c0);
        d = wt * d + (1.0f - wt) * (_ulim(Gint, n1, n2) - c0);
      }
    }
    if(Gint > clip_pt) d = _ulim(Gint, n1, n2) - c0;
  }
  return d;
}

float _var3v(float a, float b, float c)
{
  return 3.0f * (fsquare(a) + fsquare(b) + fsquare(c)) - fsquare(a + b + c);
}

// choose the smoother color difference and bound it in regions of high
// saturation, one CPU tile per work group. The CPU updates hcd and vcd in
// place, so each value sees the one two pixels before it on its axis
// already done. That makes every row of hcd and every column of vcd two
// chains (one per CFA phase), run here one per thread from the tile edge as
// on the CPU. A whole-image pass that reads only the old values differs by
// up to 36/255 on fine detail.
kernel void amaze_cd_bound(
                    global float *cfa, global float *vcd, global float *hcd, global float *vcdalt, global float *hcdalt,
                    global float *vcd2, global float *hcd2,
                    const int pw, const int ph, const unsigned int filters, const float clip_pt)
{
  const AmazeTile t = _tile(pw, ph);
  const int l = get_local_id(1) * 16 + get_local_id(0);
  const int v1 = pw, v2 = 2 * pw;
  const int nh = 2 * (t.rr1 - 8); // hcd chains: rows 4 .. rr1-4, two phases each
  const int nv = 2 * (t.cc1 - 8); // vcd chains

  for(int job = l; job < nh + nv; job += 256)
  {
    const bool horiz = job < nh;
    const int k = horiz ? job : job - nh;
    const int line = 4 + (k >> 1); // tile row (hcd) or column (vcd)
    const bool own_line = horiz ? (line >= t.r0 && line < t.r1) : (line >= t.c0 && line < t.c1);
    const int lo = horiz ? t.c0 : t.r0;
    const int hi = horiz ? t.c1 : t.r1;
    const int len = horiz ? t.cc1 : t.rr1;
    const int step = horiz ? 2 : v2;
    const int nstep = horiz ? 1 : v1;

    // the CPU computes the color differences from 4 pixels inside the tile
    // edge and reads zeros beyond that
    float prev = 0.0f, aprev = 0.0f;
    int p = 4 + (k & 1);
    int i = horiz ? (t.oy + line) * pw + t.ox + p : (t.oy + p) * pw + t.ox + line;
    float cur = horiz ? hcd[i] : vcd[i];
    float acur = horiz ? hcdalt[i] : vcdalt[i];
    for(; p < len - 4; p += 2, i += step)
    {
      const bool in = p + 2 < len - 4;
      const float next = in ? (horiz ? hcd[i + step] : vcd[i + step]) : 0.0f;
      const float anext = in ? (horiz ? hcdalt[i + step] : vcdalt[i + step]) : 0.0f;
      float x = _var3v(aprev, acur, anext) < _var3v(prev, cur, next) ? acur : cur;
      const int row = horiz ? t.oy + line : t.oy + p;
      const int col = horiz ? t.ox + p : t.ox + line;
      x = _cdbound(x, cfa[i], cfa[i - nstep], cfa[i + nstep], (FC(row, col, filters) & 1) != 0, clip_pt);
      if(own_line && p >= lo && p < hi)
      {
        if(horiz) hcd2[i] = x;
        else vcd2[i] = x;
      }
      prev = x;
      aprev = acur;
      cur = next;
      acur = anext;
    }
  }
}

// the CPU's hvwt and pmwt votes: an R/B site takes the average of its four
// diagonal neighbors when that discriminates the direction better. They
// update in place, so the two neighbors above are already done; here one
// work group walks the rows of one CPU tile, from row m to rr1 - m. A
// whole-image pass that reads only the old values differs by up to 42/255.
kernel void amaze_vote(
                global float *src, global float *dst,
                const int pw, const int ph, const unsigned int filters, const int m)
{
  const AmazeTile t = _tile(pw, ph);
  const int c = get_local_id(1) * 16 + get_local_id(0); // tile column
  local float vote_row[160];
  const bool active = c < t.cc1;
  const bool own_col = c >= t.c0 && c < t.c1;

  if(active) vote_row[c] = src[(t.oy + m - 1) * pw + t.ox + c];
  barrier(CLK_LOCAL_MEM_FENCE);

  for(int r = 0; r < t.rr1; r++)
  {
    const int i = (t.oy + r) * pw + t.ox + c;
    const bool rb = active && (FC(t.oy + r, t.ox + c, filters) & 1) == 0;
    float w = rb ? src[i] : 0.0f;
    if(rb && r >= m && r < t.rr1 - m && c >= m && c < t.cc1 - m)
    {
      const float alt = 0.25f * (vote_row[c - 1] + vote_row[c + 1] + src[i + pw - 1] + src[i + pw + 1]);
      if(fabs(0.5f - w) < fabs(0.5f - alt)) w = alt;
    }
    if(rb && r >= t.r0 && r < t.r1 && own_col) dst[i] = w;
    barrier(CLK_LOCAL_MEM_FENCE);
    // the R/B sites of this row are the other phase of the previous one
    if(rb && r >= m - 1) vote_row[c] = w;
    barrier(CLK_LOCAL_MEM_FENCE);
  }
}
// the square of the color difference, computed where it is read
float _cdd(global const float *vcd, global const float *hcd, int i) { return fsquare(vcd[i] - hcd[i]); }

// R/B sites: the adaptive weight of the vertical against the horizontal
// interpolation, and the Nyquist texture test
kernel void amaze_hvwt(
                global float *cfa, global float *vcd, global float *hcd, global float *dgintv, global float *dginth,
                global float *delhvsqsum, global float *hvwt, global float *nyquist,
                const int pw, const int ph, const unsigned int filters)
{
  const int col = get_global_id(0);
  const int row = get_global_id(1);
  if(!_area(col, row, 6, pw, ph) || (FC(row, col, filters) & 1) != 0) return;
  const int ts = pw;
  const int v1 = pw, v2 = 2 * pw, v3 = 3 * pw;
  const int p1 = -ts + 1, p2 = -2 * ts + 2, m1 = ts + 1, m2 = 2 * ts + 2;
  const int i = row * pw + col;

  // color difference variances in cardinal directions
  const float uave = vcd[i] + vcd[i - v1] + vcd[i - v2] + vcd[i - v3];
  const float dave = vcd[i] + vcd[i + v1] + vcd[i + v2] + vcd[i + v3];
  const float lave = hcd[i] + hcd[i - 1] + hcd[i - 2] + hcd[i - 3];
  const float rave = hcd[i] + hcd[i + 1] + hcd[i + 2] + hcd[i + 3];

  float Dgrbvvaru = fsquare(vcd[i] - uave) + fsquare(vcd[i - v1] - uave) + fsquare(vcd[i - v2] - uave)
                    + fsquare(vcd[i - v3] - uave);
  float Dgrbvvard = fsquare(vcd[i] - dave) + fsquare(vcd[i + v1] - dave) + fsquare(vcd[i + v2] - dave)
                    + fsquare(vcd[i + v3] - dave);
  float Dgrbhvarl = fsquare(hcd[i] - lave) + fsquare(hcd[i - 1] - lave) + fsquare(hcd[i - 2] - lave)
                    + fsquare(hcd[i - 3] - lave);
  float Dgrbhvarr = fsquare(hcd[i] - rave) + fsquare(hcd[i + 1] - rave) + fsquare(hcd[i + 2] - rave)
                    + fsquare(hcd[i + 3] - rave);

  const float hwt = _dirwts1(cfa, i - 1) / (_dirwts1(cfa, i - 1) + _dirwts1(cfa, i + 1));
  const float vwt = _dirwts0(cfa, i - v1, v1) / (_dirwts0(cfa, i + v1, v1) + _dirwts0(cfa, i - v1, v1));

  const float vcdvar = epssq + vwt * Dgrbvvard + (1.0f - vwt) * Dgrbvvaru;
  const float hcdvar = epssq + hwt * Dgrbhvarr + (1.0f - hwt) * Dgrbhvarl;

  // fluctuations in up/down and left/right interpolations of colors
  Dgrbvvaru = dgintv[i] + dgintv[i - v1] + dgintv[i - v2];
  Dgrbvvard = dgintv[i] + dgintv[i + v1] + dgintv[i + v2];
  Dgrbhvarl = dginth[i] + dginth[i - 1] + dginth[i - 2];
  Dgrbhvarr = dginth[i] + dginth[i + 1] + dginth[i + 2];

  const float vcdvar1 = epssq + vwt * Dgrbvvard + (1.0f - vwt) * Dgrbvvaru;
  const float hcdvar1 = epssq + hwt * Dgrbhvarr + (1.0f - hwt) * Dgrbhvarl;

  // adaptive weights for G interpolation
  const float varwt = hcdvar / (vcdvar + hcdvar);
  const float diffwt = hcdvar1 / (vcdvar1 + hcdvar1);

  // if both agree on the direction, choose the one with the strongest
  // discrimination, otherwise the u/d and l/r difference fluctuation weights
  hvwt[i] = ((0.5f - varwt) * (0.5f - diffwt) > 0.0f && fabs(0.5f - diffwt) < fabs(0.5f - varwt)) ? varwt : diffwt;

  const float nyqutest
    = (gaussodd[0] * _cdd(vcd, hcd, i)
       + gaussodd[1] * (_cdd(vcd, hcd, i - m1) + _cdd(vcd, hcd, i + p1) + _cdd(vcd, hcd, i - p1) + _cdd(vcd, hcd, i + m1))
       + gaussodd[2] * (_cdd(vcd, hcd, i - v2) + _cdd(vcd, hcd, i - 2) + _cdd(vcd, hcd, i + 2) + _cdd(vcd, hcd, i + v2))
       + gaussodd[3] * (_cdd(vcd, hcd, i - m2) + _cdd(vcd, hcd, i + p2) + _cdd(vcd, hcd, i - p2) + _cdd(vcd, hcd, i + m2)))
      - (gaussgrad[0] * delhvsqsum[i]
         + gaussgrad[1] * (delhvsqsum[i - v1] + delhvsqsum[i + 1] + delhvsqsum[i - 1] + delhvsqsum[i + v1])
         + gaussgrad[2] * (delhvsqsum[i - m1] + delhvsqsum[i + p1] + delhvsqsum[i - p1] + delhvsqsum[i + m1])
         + gaussgrad[3] * (delhvsqsum[i - v2] + delhvsqsum[i - 2] + delhvsqsum[i + 2] + delhvsqsum[i + v2])
         + gaussgrad[4] * (delhvsqsum[i - v2 - 1] + delhvsqsum[i - v2 + 1] + delhvsqsum[i - ts - 2]
                           + delhvsqsum[i - ts + 2] + delhvsqsum[i + ts - 2] + delhvsqsum[i + ts + 2]
                           + delhvsqsum[i + v2 - 1] + delhvsqsum[i + v2 + 1])
         + gaussgrad[5] * (delhvsqsum[i - m2] + delhvsqsum[i + p2] + delhvsqsum[i - p2] + delhvsqsum[i + m2]));

  nyquist[i] = nyqutest > 0.0f ? 1.0f : 0.0f;
}

// if most of your neighbors are named Nyquist, it's likely that you're one
// too, or not
kernel void amaze_nyquist2(
                    global float *nyquist, global float *nyquist2, const int pw, const int ph, const unsigned int filters)
{
  const int col = get_global_id(0);
  const int row = get_global_id(1);
  if((FC(row, col, filters) & 1) != 0) return;
  if(!_area(col, row, 8, pw, ph))
  {
    // the area interpolation reads these too
    if(col < pw && row < ph) nyquist2[row * pw + col] = 0.0f;
    return;
  }
  const int ts = pw;
  const int v2 = 2 * pw, p1 = -ts + 1, m1 = ts + 1;
  const int i = row * pw + col;
  const float n = nyquist[i - v2] + nyquist[i - m1] + nyquist[i + p1] + nyquist[i - 2] + nyquist[i + 2]
                  + nyquist[i - p1] + nyquist[i + m1] + nyquist[i + v2];
  nyquist2[i] = n > 4.0f ? 1.0f : (n < 4.0f ? 0.0f : nyquist[i]);
}

// in areas of Nyquist texture, do area interpolation
kernel void amaze_area(
                global float *cfa, global float *nyquist2, global float *hvwt,
                const int pw, const int ph, const unsigned int filters)
{
  const int col = get_global_id(0);
  const int row = get_global_id(1);
  if(!_area(col, row, 8, pw, ph) || (FC(row, col, filters) & 1) != 0) return;
  const int v1 = pw;
  const int i = row * pw + col;
  if(nyquist2[i] == 0.0f) return;

  float sumcfa = 0.0f, sumh = 0.0f, sumv = 0.0f, sumsqh = 0.0f, sumsqv = 0.0f, areawt = 0.0f;
  for(int y = -6; y < 7; y += 2)
    for(int x = -6; x < 7; x += 2)
    {
      const int k = i + y * pw + x;
      if(nyquist2[k] != 0.0f)
      {
        const float c = cfa[k];
        sumcfa += c;
        sumh += cfa[k - 1] + cfa[k + 1];
        sumv += cfa[k - v1] + cfa[k + v1];
        sumsqh += fsquare(c - cfa[k - 1]) + fsquare(c - cfa[k + 1]);
        sumsqv += fsquare(c - cfa[k - v1]) + fsquare(c - cfa[k + v1]);
        areawt += 1.0f;
      }
    }

  // horizontal and vertical color differences, and adaptive weight
  sumh = sumcfa - 0.5f * sumh;
  sumv = sumcfa - 0.5f * sumv;
  areawt = 0.5f * areawt;
  const float hcdvar = epssq + fabs(areawt * sumsqh - sumh * sumh);
  const float vcdvar = epssq + fabs(areawt * sumsqv - sumv * sumv);
  hvwt[i] = hcdvar / (vcdvar + hcdvar);
}

// populate G at R/B sites
kernel void amaze_green(
                 global float *cfa, global float *vcd, global float *hcd, global float *hvwt, global float *nyquist2,
                 global float *dgrb, global float *green, global float *dgrb2h, global float *dgrb2v,
                 const int pw, const int ph, const unsigned int filters)
{
  const int col = get_global_id(0);
  const int row = get_global_id(1);
  if(!_area(col, row, 8, pw, ph) || (FC(row, col, filters) & 1) != 0) return;
  const int v1 = pw;
  const int i = row * pw + col;
  const float w = hvwt[i]; // after amaze_vote

  const float d = hcd[i] + w * (vcd[i] - hcd[i]); // evaluate color differences
  const float g = cfa[i] + d;                     // evaluate G (finally!)
  dgrb[i] = d;
  green[i] = g;

  // local curvature in G (preparation for nyquist refinement step); the
  // neighbors are G sites, where G is the raw value
  const bool nyq = nyquist2[i] != 0.0f;
  dgrb2h[i] = nyq ? fsquare(g - 0.5f * (cfa[i - 1] + cfa[i + 1])) : 0.0f;
  dgrb2v[i] = nyq ? fsquare(g - 0.5f * (cfa[i - v1] + cfa[i + v1])) : 0.0f;
}

float _gvar(global const float *b, int i, int ts)
{
  const int v2 = 2 * ts, p1 = -ts + 1, p2 = -2 * ts + 2, m1 = ts + 1, m2 = 2 * ts + 2;
  return epssq + (gquinc[0] * b[i]
                  + gquinc[1] * (b[i - m1] + b[i + p1] + b[i - p1] + b[i + m1])
                  + gquinc[2] * (b[i - v2] + b[i - 2] + b[i + 2] + b[i + v2])
                  + gquinc[3] * (b[i - m2] + b[i + p2] + b[i - p2] + b[i + m2]));
}

// refine Nyquist areas using G curvatures
kernel void amaze_nyquist_refine(
                          global float *cfa, global float *vcd, global float *hcd, global float *nyquist2,
                          global float *dgrb2h, global float *dgrb2v, global float *dgrb, global float *green,
                          const int pw, const int ph, const unsigned int filters)
{
  const int col = get_global_id(0);
  const int row = get_global_id(1);
  if(!_area(col, row, 8, pw, ph) || (FC(row, col, filters) & 1) != 0) return;
  const int i = row * pw + col;
  if(nyquist2[i] == 0.0f) return;

  // local averages (over Nyquist pixels only) of G curvature squared
  const float gvarh = _gvar(dgrb2h, i, pw);
  const float gvarv = _gvar(dgrb2v, i, pw);
  // use the results as weights for refined G interpolation
  const float d = (hcd[i] * gvarv + vcd[i] * gvarh) / (gvarv + gvarh);
  dgrb[i] = d;
  green[i] = cfa[i] + d;
}

float _rbdiag(float c, float cr, float n1, float n2)
{
  return fabs(1.0f - cr) < arthresh ? c * cr : n1 + 0.5f * (c - n2);
}

float _rbvar(global const float *cfa, int i, int ts, bool plus)
{
  const int v1 = ts, v2 = 2 * ts;
  const int k[12] = { i - v1, i - 1, i + 1, i + v1,
                      i - v2 - 1, i - v2 + 1, i - 2 - v1, i + 2 - v1, i - 2 + v1, i + 2 + v1, i + v2 - 1, i + v2 + 1 };
  float s0 = 0.0f, s1 = 0.0f;
  for(int n = 0; n < 4; n++) s0 += plus ? _sq1p(cfa, k[n], ts) : _sq1m(cfa, k[n], ts);
  for(int n = 4; n < 12; n++) s1 += plus ? _sq1p(cfa, k[n], ts) : _sq1m(cfa, k[n], ts);
  return epssq + (gausseven[0] * s0 + gausseven[1] * s1);
}

// diagonal interpolation correction
kernel void amaze_diag(
                global float *cfa, global float *rbm, global float *rbp, global float *pmwt,
                const int pw, const int ph, const unsigned int filters, const float clip_pt)
{
  const int col = get_global_id(0);
  const int row = get_global_id(1);
  if(!_area(col, row, 8, pw, ph) || (FC(row, col, filters) & 1) != 0) return;
  const int ts = pw;
  const int p1 = -ts + 1, p2 = -2 * ts + 2, m1 = ts + 1, m2 = 2 * ts + 2;
  const int i = row * pw + col;
  const float c0 = cfa[i];

  // diagonal color ratios
  const float crse = 2.0f * cfa[i + m1] / (eps + c0 + cfa[i + m2]);
  const float crnw = 2.0f * cfa[i - m1] / (eps + c0 + cfa[i - m2]);
  const float crne = 2.0f * cfa[i + p1] / (eps + c0 + cfa[i + p2]);
  const float crsw = 2.0f * cfa[i - p1] / (eps + c0 + cfa[i - p2]);

  // assign B/R at R/B sites
  const float rbse = _rbdiag(c0, crse, cfa[i + m1], cfa[i + m2]);
  const float rbnw = _rbdiag(c0, crnw, cfa[i - m1], cfa[i - m2]);
  const float rbne = _rbdiag(c0, crne, cfa[i + p1], cfa[i + p2]);
  const float rbsw = _rbdiag(c0, crsw, cfa[i - p1], cfa[i - p2]);

  const float dm = _delm(cfa, i, ts), dp = _delp(cfa, i, ts);
  const float wtse = eps + dm + _delm(cfa, i + m1, ts) + _delm(cfa, i + m2, ts);
  const float wtnw = eps + dm + _delm(cfa, i - m1, ts) + _delm(cfa, i - m2, ts);
  const float wtne = eps + dp + _delp(cfa, i + p1, ts) + _delp(cfa, i + p2, ts);
  const float wtsw = eps + dp + _delp(cfa, i - p1, ts) + _delp(cfa, i - p2, ts);

  float m = (wtse * rbnw + wtnw * rbse) / (wtse + wtnw);
  float p = (wtne * rbsw + wtsw * rbne) / (wtne + wtsw);

  // variance of R-B in plus/minus directions
  const float rbvarm = _rbvar(cfa, i, ts, false);
  pmwt[i] = rbvarm / (_rbvar(cfa, i, ts, true) + rbvarm);

  // bound the interpolation in regions of high saturation
  if(p < c0)
  {
    if(2.0f * p < c0)
      p = _ulim(p, cfa[i - p1], cfa[i + p1]);
    else
    {
      const float pwt = 2.0f * (c0 - p) / (eps + p + c0);
      p = pwt * p + (1.0f - pwt) * _ulim(p, cfa[i - p1], cfa[i + p1]);
    }
  }
  if(m < c0)
  {
    if(2.0f * m < c0)
      m = _ulim(m, cfa[i - m1], cfa[i + m1]);
    else
    {
      const float mwt = 2.0f * (c0 - m) / (eps + m + c0);
      m = mwt * m + (1.0f - mwt) * _ulim(m, cfa[i - m1], cfa[i + m1]);
    }
  }
  if(p > clip_pt) p = _ulim(p, cfa[i - p1], cfa[i + p1]);
  if(m > clip_pt) m = _ulim(m, cfa[i - m1], cfa[i + m1]);

  rbm[i] = m;
  rbp[i] = p;
}

// R+B interpolated
kernel void amaze_rbint(
                 global float *cfa, global float *rbm, global float *rbp, global float *pmwt,
                 global float *rbint, const int pw, const int ph, const unsigned int filters)
{
  const int col = get_global_id(0);
  const int row = get_global_id(1);
  if(!_area(col, row, 10, pw, ph) || (FC(row, col, filters) & 1) != 0) return;
  const int i = row * pw + col;
  const float w = pmwt[i]; // after amaze_vote
  rbint[i] = 0.5f * (cfa[i] + rbm[i] * (1.0f - w) + rbp[i] * w);
}

float _gdir(float rb0, float cr, float c, float rbn)
{
  return fabs(1.0f - cr) < arthresh ? rb0 * cr : c + 0.5f * (rb0 - rbn);
}

// interpolate G vertically/horizontally using R+B values where the
// diagonal discrimination is stronger
kernel void amaze_green_diag(
                      global float *cfa, global float *hvwt, global float *pmwt, global float *rbint,
                      global float *dgrb, global float *green,
                      const int pw, const int ph, const unsigned int filters, const float clip_pt)
{
  const int col = get_global_id(0);
  const int row = get_global_id(1);
  if(!_area(col, row, 12, pw, ph) || (FC(row, col, filters) & 1) != 0) return;
  const int v1 = pw;
  const int i = row * pw + col;
  if(fabs(0.5f - pmwt[i]) < fabs(0.5f - hvwt[i])) return;

  // the CPU's rbint[indx1 -/+ v1] is two rows away in its half width
  // array, and rbint[indx1 -/+ 1] two columns
  const float rb0 = rbint[i];
  const float rbu = rbint[i - 2 * v1], rbd = rbint[i + 2 * v1], rbl = rbint[i - 2], rbr = rbint[i + 2];

  // color ratios for G interpolation
  const float cru = cfa[i - v1] * 2.0f / (eps + rb0 + rbu);
  const float crd = cfa[i + v1] * 2.0f / (eps + rb0 + rbd);
  const float crl = cfa[i - 1] * 2.0f / (eps + rb0 + rbl);
  const float crr = cfa[i + 1] * 2.0f / (eps + rb0 + rbr);

  // interpolated G via adaptive ratios or Hamilton-Adams in each cardinal direction
  const float gu = _gdir(rb0, cru, cfa[i - v1], rbu);
  const float gd = _gdir(rb0, crd, cfa[i + v1], rbd);
  const float gl = _gdir(rb0, crl, cfa[i - 1], rbl);
  const float gr = _gdir(rb0, crr, cfa[i + 1], rbr);

  // interpolated G via adaptive weights of cardinal evaluations
  const float d0u = _dirwts0(cfa, i - v1, v1), d0d = _dirwts0(cfa, i + v1, v1);
  const float d1l = _dirwts1(cfa, i - 1), d1r = _dirwts1(cfa, i + 1);
  float Gintv = (d0u * gd + d0d * gu) / (d0d + d0u);
  float Ginth = (d1l * gr + d1r * gl) / (d1l + d1r);

  // bound the interpolation in regions of high saturation
  if(Gintv < rb0)
  {
    if(2.0f * Gintv < rb0)
      Gintv = _ulim(Gintv, cfa[i - v1], cfa[i + v1]);
    else
    {
      const float vwt = 2.0f * (rb0 - Gintv) / (eps + Gintv + rb0);
      Gintv = vwt * Gintv + (1.0f - vwt) * _ulim(Gintv, cfa[i - v1], cfa[i + v1]);
    }
  }
  if(Ginth < rb0)
  {
    if(2.0f * Ginth < rb0)
      Ginth = _ulim(Ginth, cfa[i - 1], cfa[i + 1]);
    else
    {
      const float hwt = 2.0f * (rb0 - Ginth) / (eps + Ginth + rb0);
      Ginth = hwt * Ginth + (1.0f - hwt) * _ulim(Ginth, cfa[i - 1], cfa[i + 1]);
    }
  }
  if(Ginth > clip_pt) Ginth = _ulim(Ginth, cfa[i - 1], cfa[i + 1]);
  if(Gintv > clip_pt) Gintv = _ulim(Gintv, cfa[i - v1], cfa[i + v1]);

  const float g = Ginth * (1.0f - hvwt[i]) + Gintv * hvwt[i];
  green[i] = g;
  dgrb[i] = g - cfa[i];
}

float _chroma(global const float *d, int i, int ts)
{
  const int v2 = 2 * ts, p1 = -ts + 1, p3 = -3 * ts + 3, m1 = ts + 1, m3 = 3 * ts + 3;
  const float wtnw = 1.0f / (eps + fabs(d[i - m1] - d[i + m1]) + fabs(d[i - m1] - d[i - m3]) + fabs(d[i + m1] - d[i - m3]));
  const float wtne = 1.0f / (eps + fabs(d[i + p1] - d[i - p1]) + fabs(d[i + p1] - d[i + p3]) + fabs(d[i - p1] - d[i + p3]));
  const float wtsw = 1.0f / (eps + fabs(d[i - p1] - d[i + p1]) + fabs(d[i - p1] - d[i + m3]) + fabs(d[i + p1] - d[i - p3]));
  const float wtse = 1.0f / (eps + fabs(d[i + m1] - d[i - m1]) + fabs(d[i + m1] - d[i - p3]) + fabs(d[i - m1] - d[i + m3]));

  return (wtnw * (1.325f * d[i - m1] - 0.175f * d[i - m3] - 0.075f * d[i - m1 - 2] - 0.075f * d[i - m1 - v2])
          + wtne * (1.325f * d[i + p1] - 0.175f * d[i + p3] - 0.075f * d[i + p1 + 2] - 0.075f * d[i + p1 + v2])
          + wtsw * (1.325f * d[i - p1] - 0.175f * d[i - p3] - 0.075f * d[i - p1 - 2] - 0.075f * d[i - p1 - v2])
          + wtse * (1.325f * d[i + m1] - 0.175f * d[i + m3] - 0.075f * d[i + m1 + 2] - 0.075f * d[i + m1 + v2]))
         / (wtnw + wtne + wtsw + wtse);
}

// fancy chrominance interpolation: G-R and G-B at every R/B site, the own
// color difference from dgrb and the other from the diagonal neighbors,
// which are of the other color
kernel void amaze_chroma(
                  global float *dgrb, global float *dgr, global float *dgb,
                  const int pw, const int ph, const unsigned int filters)
{
  const int col = get_global_id(0);
  const int row = get_global_id(1);
  if(!_area(col, row, 14, pw, ph)) return;
  const int c = FC(row, col, filters);
  if((c & 1) != 0) return;
  const int i = row * pw + col;
  const float own = dgrb[i];
  const float other = _chroma(dgrb, i, pw);
  dgr[i] = c == 0 ? own : other;
  dgb[i] = c == 0 ? other : own;
}

kernel void amaze_output(
                  global float *cfa, global float *green, global float *hvwt, global float *dgr, global float *dgb,
                  write_only image2d_t out, const int width, const int height, const int pw, const unsigned int filters)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;
  const int row = y + 16, col = x + 16;
  const int v1 = pw;
  const int i = row * pw + col;

  float r, g, b;
  if((FC(row, col, filters) & 1) == 0)
  { // R/B site
    g = green[i];
    r = g - dgr[i];
    b = g - dgb[i];
  }
  else
  { // G site: the color differences of the four R/B neighbors
    g = cfa[i];
    const float wu = hvwt[i - v1], wd = hvwt[i + v1], wl = 1.0f - hvwt[i - 1], wr = 1.0f - hvwt[i + 1];
    const float temp = 1.0f / (wu + wr + wl + wd);
    r = g - (wu * dgr[i - v1] + wr * dgr[i + 1] + wl * dgr[i - 1] + wd * dgr[i + v1]) * temp;
    b = g - (wu * dgb[i - v1] + wr * dgb[i + 1] + wl * dgb[i - 1] + wd * dgb[i + v1]) * temp;
  }
  write_imagef(out, (int2)(x, y), (float4)(_clampnan(r), _clampnan(g), _clampnan(b), 0.0f));
}
