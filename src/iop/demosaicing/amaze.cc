/*
    This file is part of darktable,
    Copyright (C) 2011-2023 darktable developers.

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

#define __STDC_FORMAT_MACROS

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "common/math.h"

extern "C" {

static inline float _clampnan(const float x, const float m, const float M)
{
  float r;

  // clamp to [m, M] if x is infinite; return average of m and M if x is NaN; else just return x

  if(std::isinf(x))
    r = (std::isless(x, m) ? m : (std::isgreater(x, M) ? M : x));
  else if(std::isnan(x))
    r = (m + M) / 2.0f;
  else // normal number
    r = x;

  return r;
}

static inline float _xmul2f(float d)
{
  union {
      float f;
      uint32_t u;
  } x;
  x.f = d;
  if(x.u & 0x7FFFFFFF) // if f==0 do nothing
  {
    x.u += 1 << 23; // add 1 to the exponent
  }
  return x.f;
}

static inline float _xdiv2f(float d)
{
  union {
      float f;
      uint32_t u;
  } x;
  x.f = d;
  if(x.u & 0x7FFFFFFF) // if f==0 do nothing
  {
    x.u -= 1 << 23; // sub 1 from the exponent
  }
  return x.f;
}

static inline float _xdivf(float d, int n)
{
  union {
      float f;
      uint32_t u;
  } x;
  x.f = d;
  if(x.u & 0x7FFFFFFF) // if f==0 do nothing
  {
    x.u -= n << 23; // add n to the exponent
  }
  return x.f;
}


/*==================================================================================
 * begin raw therapee code, hg checkout of march 03, 2016 branch master.
 *==================================================================================*/

#define LIM(x, min, max) MAX(min, MIN(x, max))
#define ULIM(x, y, z) ((y) < (z) ? LIM(x, y, z) : LIM(x, z, y))


////////////////////////////////////////////////////////////////
//
//          AMaZE demosaic algorithm
// (Aliasing Minimization and Zipper Elimination)
//
//  copyright (c) 2008-2010  Emil Martinec <ejmartin@uchicago.edu>
//  optimized for speed by Ingo Weyrich
//
// incorporating ideas of Luis Sanz Rodrigues and Paul Lee
//
// code dated: May 27, 2010
// latest modification: Ingo Weyrich, January 25, 2016
//
//  amaze_interpolate_RT.cc is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation, either version 3 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
////////////////////////////////////////////////////////////////


void amaze_demosaic(dt_dev_pixelpipe_iop_t *piece,
                       const float *const in,
                       float *out,
                       const dt_iop_roi_t *const roi_in,
                       const dt_iop_roi_t *const roi_out,
                       const uint32_t filters)
{
  int winx = roi_out->x;
  int winy = roi_out->y;
  int winw = roi_in->width;
  int winh = roi_in->height;

  const int width = winw, height = winh;
  const float clip_pt = fminf(piece->pipe->dsc.processed_maximum[0],
                              fminf(piece->pipe->dsc.processed_maximum[1], piece->pipe->dsc.processed_maximum[2]));
  const float clip_pt8 = 0.8f * clip_pt;

// this allows to pass AMAZETS to the code. On some machines larger AMAZETS is faster
// If AMAZETS is undefined it will be set to 160, which is the fastest on modern x86/64 machines
#ifndef AMAZETS
#define AMAZETS 160
#endif
  // Tile size; the image is processed in square tiles to lower memory requirements and facilitate
  // multi-threading
  // We assure that Tile size is a multiple of 32 in the range [96;992]
  constexpr int ts = (AMAZETS & 992) < 96 ? 96 : (AMAZETS & 992);
  constexpr int tsh = ts / 2; // half of Tile size

  // offset of R pixel within a Bayer quartet
  int ex, ey;

  // determine GRBG coset; (ey,ex) is the offset of the R subarray
  if(FC(0, 0, filters) == 1)
  { // first pixel is G
    if(FC(0, 1, filters) == 0)
    {
      ey = 0;
      ex = 1;
    }
    else
    {
      ey = 1;
      ex = 0;
    }
  }
  else
  { // first pixel is R or B
    if(FC(0, 0, filters) == 0)
    {
      ey = 0;
      ex = 0;
    }
    else
    {
      ey = 1;
      ex = 1;
    }
  }

  // shifts of pointer value to access pixels in vertical and diagonal directions
  constexpr int v1 = ts, v2 = 2 * ts, v3 = 3 * ts, p1 = -ts + 1, p2 = -2 * ts + 2, p3 = -3 * ts + 3,
                m1 = ts + 1, m2 = 2 * ts + 2, m3 = 3 * ts + 3;

  // tolerance to avoid dividing by zero
  constexpr float eps = 1e-5, epssq = 1e-10; // tolerance to avoid dividing by zero

  // adaptive ratios threshold
  constexpr float arthresh = 0.75;

  // gaussian on 5x5 quincunx, sigma=1.2
  constexpr float gaussodd[4]
      = { 0.14659727707323927f, 0.103592713382435f, 0.0732036125103057f, 0.0365543548389495f };
  // nyquist texture test threshold
  constexpr float nyqthresh = 0.5;
  // gaussian on 5x5, sigma=1.2, multiplied with nyqthresh to save some time later in loop
  // Is this really sigma=1.2????, seems more like sigma = 1.672
  constexpr float gaussgrad[6] = { nyqthresh * 0.07384411893421103f, nyqthresh * 0.06207511968171489f,
                                   nyqthresh * 0.0521818194747806f,  nyqthresh * 0.03687419286733595f,
                                   nyqthresh * 0.03099732204057846f, nyqthresh * 0.018413194161458882f };
  // gaussian on 5x5 alt quincunx, sigma=1.5
  constexpr float gausseven[2] = { 0.13719494435797422f, 0.05640252782101291f };
  // gaussian on quincunx grid
  constexpr float gquinc[4] = { 0.169917f, 0.108947f, 0.069855f, 0.0287182f };

  typedef struct
  {
    float h;
    float v;
  } s_hv;

#ifdef _OPENMP
#pragma omp parallel
#endif
  {
    constexpr int cldf = 2; // factor to multiply cache line distance. 1 = 64 bytes, 2 = 128 bytes ...
    // assign working space
    char *buffer
        = (char *)calloc(sizeof(float) * 14 * ts * ts + sizeof(char) * ts * tsh + 18 * cldf * 64 + 63, 1);
    // aligned to 64 byte boundary
    char *data = (char *)((uintptr_t(buffer) + uintptr_t(63)) / 64 * 64);

    // green values
    float *rgbgreen = (float(*))data;
    // sum of square of horizontal gradient and square of vertical gradient
    float *delhvsqsum = (float(*))((char *)rgbgreen + sizeof(float) * ts * ts + cldf * 64); // 1
    // gradient based directional weights for interpolation
    float *dirwts0 = (float(*))((char *)delhvsqsum + sizeof(float) * ts * ts + cldf * 64); // 1
    float *dirwts1 = (float(*))((char *)dirwts0 + sizeof(float) * ts * ts + cldf * 64);    // 1
    // vertically interpolated colour differences G-R, G-B
    float *vcd = (float(*))((char *)dirwts1 + sizeof(float) * ts * ts + cldf * 64); // 1
    // horizontally interpolated colour differences
    float *hcd = (float(*))((char *)vcd + sizeof(float) * ts * ts + cldf * 64); // 1
    // alternative vertical interpolation
    float *vcdalt = (float(*))((char *)hcd + sizeof(float) * ts * ts + cldf * 64); // 1
    // alternative horizontal interpolation
    float *hcdalt = (float(*))((char *)vcdalt + sizeof(float) * ts * ts + cldf * 64); // 1
    // square of average colour difference
    float *cddiffsq = (float(*))((char *)hcdalt + sizeof(float) * ts * ts + cldf * 64); // 1
    // weight to give horizontal vs vertical interpolation
    float *hvwt = (float(*))((char *)cddiffsq + sizeof(float) * ts * ts + 2 * cldf * 64); // 1
    // final interpolated colour difference
    float(*Dgrb)[ts * tsh] = (float(*)[ts * tsh])vcdalt; // there is no overlap in buffer usage => share
    // gradient in plus (NE/SW) direction
    float *delp = (float(*))cddiffsq; // there is no overlap in buffer usage => share
    // gradient in minus (NW/SE) direction
    float *delm = (float(*))((char *)delp + sizeof(float) * ts * tsh + cldf * 64);
    // diagonal interpolation of R+B
    float *rbint = (float(*))delm; // there is no overlap in buffer usage => share
    // horizontal and vertical curvature of interpolated G (used to refine interpolation in Nyquist texture
    // regions)
    s_hv *Dgrb2 = (s_hv(*))((char *)hvwt + sizeof(float) * ts * tsh + cldf * 64); // 1
    // difference between up/down interpolations of G
    float *dgintv = (float(*))Dgrb2; // there is no overlap in buffer usage => share
    // difference between left/right interpolations of G
    float *dginth = (float(*))((char *)dgintv + sizeof(float) * ts * ts + cldf * 64); // 1
    // square of diagonal colour differences
    float *Dgrbsq1m = (float(*))((char *)dginth + sizeof(float) * ts * ts + cldf * 64);    // 1
    float *Dgrbsq1p = (float(*))((char *)Dgrbsq1m + sizeof(float) * ts * tsh + cldf * 64); // 1
    // tile raw data
    float *cfa = (float(*))((char *)Dgrbsq1p + sizeof(float) * ts * tsh + cldf * 64); // 1
    // relative weight for combining plus and minus diagonal interpolations
    float *pmwt = (float(*))delhvsqsum; // there is no overlap in buffer usage => share
    // interpolated colour difference R-B in minus and plus direction
    float *rbm = (float(*))vcd; // there is no overlap in buffer usage => share
    float *rbp = (float(*))((char *)rbm + sizeof(float) * ts * tsh + cldf * 64);
    // nyquist texture flags 1=nyquist, 0=not nyquist
    unsigned char *nyquist = (unsigned char(*))((char *)cfa + sizeof(float) * ts * ts + cldf * 64); // 1
    unsigned char *nyquist2 = (unsigned char(*))cddiffsq;
    float *nyqutest = (float(*))((char *)nyquist + sizeof(unsigned char) * ts * tsh + cldf * 64); // 1

// Main algorithm: Tile loop
// use collapse(2) to collapse the 2 loops to one large loop, so there is better scaling
#ifdef _OPENMP
#pragma omp for SIMD() schedule(static) collapse(2) nowait
#endif

    for(int top = winy - 16; top < winy + height; top += ts - 32)
    {
      for(int left = winx - 16; left < winx + width; left += ts - 32)
      {
        memset(&nyquist[3 * tsh], 0, sizeof(unsigned char) * (ts - 6) * tsh);
        // location of tile bottom edge
        const int bottom = MIN(top + ts, winy + height + 16);
        // location of tile right edge
        const int right = MIN(left + ts, winx + width + 16);
        // tile width  (=ts except for right edge of image)
        const int rr1 = bottom - top;
        // tile height (=ts except for bottom edge of image)
        const int cc1 = right - left;
        // bookkeeping for borders
        // min and max row/column in the tile
        const int rrmin = top < winy ? 16 : 0;
        const int ccmin = left < winx ? 16 : 0;
        const int rrmax = bottom > (winy + height) ? winy + height - top : rr1;
        const int ccmax = right > (winx + width) ? winx + width - left : cc1;

// rgb from input CFA data
// rgb values should be floating point number between 0 and 1
// after white balance multipliers are applied
// a 16 pixel border is added to each side of the image

// begin of tile initialization
        // fill upper border
        if(rrmin > 0)
        {
          for(int rr = 0; rr < 16; rr++)
            for(int cc = ccmin, row = 32 - rr + top; cc < ccmax; cc++)
            {
              cfa[rr * ts + cc] = (in[row * width + (cc + left)]);
              rgbgreen[rr * ts + cc] = cfa[rr * ts + cc];
            }
        }

        // fill inner part
        for(int rr = rrmin; rr < rrmax; rr++)
        {
          const int row = rr + top;

          for(int cc = ccmin; cc < ccmax; cc++)
          {
            const int indx1 = rr * ts + cc;
            cfa[indx1] = (in[row * width + (cc + left)]);
            rgbgreen[indx1] = cfa[indx1];
          }
        }

        // fill lower border
        if(rrmax < rr1)
        {
          for(int rr = 0; rr < 16; rr++)
            for(int cc = ccmin; cc < ccmax; cc++)
            {
              cfa[(rrmax + rr) * ts + cc] = (in[(winy + height - rr - 2) * width + (left + cc)]);
              rgbgreen[(rrmax + rr) * ts + cc] = cfa[(rrmax + rr) * ts + cc];
            }
        }
        // fill left border
        if(ccmin > 0)
        {
          for(int rr = rrmin; rr < rrmax; rr++)
            for(int cc = 0, row = rr + top; cc < 16; cc++)
            {
              cfa[rr * ts + cc] = (in[row * width + (32 - cc + left)]);
              rgbgreen[rr * ts + cc] = cfa[rr * ts + cc];
            }
        }

        // fill right border
        if(ccmax < cc1)
        {
          for(int rr = rrmin; rr < rrmax; rr++)
            for(int cc = 0; cc < 16; cc++)
            {
              cfa[rr * ts + ccmax + cc] = (in[(top + rr) * width + ((winx + width - cc - 2))]);
              rgbgreen[rr * ts + ccmax + cc] = cfa[rr * ts + ccmax + cc];
            }
        }

        // also, fill the image corners
        if(rrmin > 0 && ccmin > 0)
        {
          for(int rr = 0; rr < 16; rr++)
            for(int cc = 0; cc < 16; cc++)
            {
              cfa[(rr)*ts + cc] = (in[(winy + 32 - rr) * width + (winx + 32 - cc)]);
              rgbgreen[(rr)*ts + cc] = cfa[(rr)*ts + cc];
            }
        }

        if(rrmax < rr1 && ccmax < cc1)
        {
          for(int rr = 0; rr < 16; rr++)
            for(int cc = 0; cc < 16; cc++)
            {
              cfa[(rrmax + rr) * ts + ccmax + cc]
                  = (in[(winy + height - rr - 2) * width + ((winx + width - cc - 2))]);
              rgbgreen[(rrmax + rr) * ts + ccmax + cc] = cfa[(rrmax + rr) * ts + ccmax + cc];
            }
        }

        if(rrmin > 0 && ccmax < cc1)
        {
          for(int rr = 0; rr < 16; rr++)
            for(int cc = 0; cc < 16; cc++)
            {
              cfa[(rr)*ts + ccmax + cc] = (in[(winy + 32 - rr) * width + ((winx + width - cc - 2))]);
              rgbgreen[(rr)*ts + ccmax + cc] = cfa[(rr)*ts + ccmax + cc];
            }
        }

        if(rrmax < rr1 && ccmin > 0)
        {
          for(int rr = 0; rr < 16; rr++)
            for(int cc = 0; cc < 16; cc++)
            {
              cfa[(rrmax + rr) * ts + cc] = (in[(winy + height - rr - 2) * width + ((winx + 32 - cc))]);
              rgbgreen[(rrmax + rr) * ts + cc] = cfa[(rrmax + rr) * ts + cc];
            }
        }

// end of tile initialization

// horizontal and vertical gradients
        for(int rr = 2; rr < rr1 - 2; rr++)
          for(int cc = 2, indx = (rr)*ts + cc; cc < cc1 - 2; cc++, indx++)
          {
            const float delh = fabsf(cfa[indx + 1] - cfa[indx - 1]);
            const float delv = fabsf(cfa[indx + v1] - cfa[indx - v1]);
            dirwts0[indx]
                = eps + fabsf(cfa[indx + v2] - cfa[indx]) + fabsf(cfa[indx] - cfa[indx - v2]) + delv;
            dirwts1[indx] = eps + fabsf(cfa[indx + 2] - cfa[indx]) + fabsf(cfa[indx] - cfa[indx - 2]) + delh;
            delhvsqsum[indx] = sqrf(delh) + sqrf(delv);
          }

// interpolate vertical and horizontal colour differences

        for(int rr = 4; rr < rr1 - 4; rr++)
        {
          bool fcswitch = FC(rr, 4, filters) & 1;

          for(int cc = 4, indx = rr * ts + cc; cc < cc1 - 4; cc++, indx++)
          {

            // colour ratios in each cardinal direction
            const float cru = cfa[indx - v1] * (dirwts0[indx - v2] + dirwts0[indx])
                        / (dirwts0[indx - v2] * (eps + cfa[indx]) + dirwts0[indx] * (eps + cfa[indx - v2]));
            const float crd = cfa[indx + v1] * (dirwts0[indx + v2] + dirwts0[indx])
                        / (dirwts0[indx + v2] * (eps + cfa[indx]) + dirwts0[indx] * (eps + cfa[indx + v2]));
            const float crl = cfa[indx - 1] * (dirwts1[indx - 2] + dirwts1[indx])
                        / (dirwts1[indx - 2] * (eps + cfa[indx]) + dirwts1[indx] * (eps + cfa[indx - 2]));
            const float crr = cfa[indx + 1] * (dirwts1[indx + 2] + dirwts1[indx])
                        / (dirwts1[indx + 2] * (eps + cfa[indx]) + dirwts1[indx] * (eps + cfa[indx + 2]));

            // G interpolated in vert/hor directions using Hamilton-Adams method
            const float guha = cfa[indx - v1] + _xdiv2f(cfa[indx] - cfa[indx - v2]);
            const float gdha = cfa[indx + v1] + _xdiv2f(cfa[indx] - cfa[indx + v2]);
            const float glha = cfa[indx - 1] + _xdiv2f(cfa[indx] - cfa[indx - 2]);
            const float grha = cfa[indx + 1] + _xdiv2f(cfa[indx] - cfa[indx + 2]);

            // G interpolated in vert/hor directions using adaptive ratios
            float guar, gdar, glar, grar;

            if(fabsf(1.f - cru) < arthresh)
            {
              guar = cfa[indx] * cru;
            }
            else
            {
              guar = guha;
            }

            if(fabsf(1.f - crd) < arthresh)
            {
              gdar = cfa[indx] * crd;
            }
            else
            {
              gdar = gdha;
            }

            if(fabsf(1.f - crl) < arthresh)
            {
              glar = cfa[indx] * crl;
            }
            else
            {
              glar = glha;
            }

            if(fabsf(1.f - crr) < arthresh)
            {
              grar = cfa[indx] * crr;
            }
            else
            {
              grar = grha;
            }

            // adaptive weights for vertical/horizontal directions
            const float hwt = dirwts1[indx - 1] / (dirwts1[indx - 1] + dirwts1[indx + 1]);
            const float vwt = dirwts0[indx - v1] / (dirwts0[indx + v1] + dirwts0[indx - v1]);

            // interpolated G via adaptive weights of cardinal evaluations
            const float Gintvha = vwt * gdha + (1.f - vwt) * guha;
            const float Ginthha = hwt * grha + (1.f - hwt) * glha;

            // interpolated colour differences
            if(fcswitch)
            {
              vcd[indx] = cfa[indx] - (vwt * gdar + (1.f - vwt) * guar);
              hcd[indx] = cfa[indx] - (hwt * grar + (1.f - hwt) * glar);
              vcdalt[indx] = cfa[indx] - Gintvha;
              hcdalt[indx] = cfa[indx] - Ginthha;
            }
            else
            {
              // interpolated colour differences
              vcd[indx] = (vwt * gdar + (1.f - vwt) * guar) - cfa[indx];
              hcd[indx] = (hwt * grar + (1.f - hwt) * glar) - cfa[indx];
              vcdalt[indx] = Gintvha - cfa[indx];
              hcdalt[indx] = Ginthha - cfa[indx];
            }

            fcswitch = !fcswitch;

            if(cfa[indx] > clip_pt8 || Gintvha > clip_pt8 || Ginthha > clip_pt8)
            {
              // use HA if highlights are (nearly) clipped
              guar = guha;
              gdar = gdha;
              glar = glha;
              grar = grha;
              vcd[indx] = vcdalt[indx];
              hcd[indx] = hcdalt[indx];
            }

            // differences of interpolations in opposite directions
            dgintv[indx] = MIN(sqrf(guha - gdha), sqrf(guar - gdar));
            dginth[indx] = MIN(sqrf(glha - grha), sqrf(glar - grar));
          }
        }

        for(int rr = 4; rr < rr1 - 4; rr++)
        {
          for(int cc = 4, indx = rr * ts + cc, c = FC(rr, cc, filters) & 1; cc < cc1 - 4; cc++, indx++)
          {
            const float hcdvar = 3.f * (sqrf(hcd[indx - 2]) + sqrf(hcd[indx]) + sqrf(hcd[indx + 2]))
                           - sqrf(hcd[indx - 2] + hcd[indx] + hcd[indx + 2]);
            const float hcdaltvar = 3.f * (sqrf(hcdalt[indx - 2]) + sqrf(hcdalt[indx]) + sqrf(hcdalt[indx + 2]))
                              - sqrf(hcdalt[indx - 2] + hcdalt[indx] + hcdalt[indx + 2]);
            const float vcdvar = 3.f * (sqrf(vcd[indx - v2]) + sqrf(vcd[indx]) + sqrf(vcd[indx + v2]))
                           - sqrf(vcd[indx - v2] + vcd[indx] + vcd[indx + v2]);
            const float vcdaltvar = 3.f * (sqrf(vcdalt[indx - v2]) + sqrf(vcdalt[indx]) + sqrf(vcdalt[indx + v2]))
                              - sqrf(vcdalt[indx - v2] + vcdalt[indx] + vcdalt[indx + v2]);

            // choose the smallest variance; this yields a smoother interpolation
            if(hcdaltvar < hcdvar)
            {
              hcd[indx] = hcdalt[indx];
            }

            if(vcdaltvar < vcdvar)
            {
              vcd[indx] = vcdalt[indx];
            }

            // bound the interpolation in regions of high saturation
            // vertical and horizontal G interpolations
            float Gintv, Ginth;

            if(c)
            {                                 // G site
              Ginth = -hcd[indx] + cfa[indx]; // R or B
              Gintv = -vcd[indx] + cfa[indx]; // B or R

              if(hcd[indx] > 0)
              {
                if(3.f * hcd[indx] > (Ginth + cfa[indx]))
                {
                  hcd[indx] = -ULIM(Ginth, cfa[indx - 1], cfa[indx + 1]) + cfa[indx];
                }
                else
                {
                  const float hwt = 1.f - 3.f * hcd[indx] / (eps + Ginth + cfa[indx]);
                  hcd[indx] = hwt * hcd[indx]
                              + (1.f - hwt) * (-ULIM(Ginth, cfa[indx - 1], cfa[indx + 1]) + cfa[indx]);
                }
              }

              if(vcd[indx] > 0)
              {
                if(3.f * vcd[indx] > (Gintv + cfa[indx]))
                {
                  vcd[indx] = -ULIM(Gintv, cfa[indx - v1], cfa[indx + v1]) + cfa[indx];
                }
                else
                {
                  const float vwt = 1.f - 3.f * vcd[indx] / (eps + Gintv + cfa[indx]);
                  vcd[indx] = vwt * vcd[indx]
                              + (1.f - vwt) * (-ULIM(Gintv, cfa[indx - v1], cfa[indx + v1]) + cfa[indx]);
                }
              }

              if(Ginth > clip_pt)
              {
                hcd[indx] = -ULIM(Ginth, cfa[indx - 1], cfa[indx + 1]) + cfa[indx];
              }

              if(Gintv > clip_pt)
              {
                vcd[indx] = -ULIM(Gintv, cfa[indx - v1], cfa[indx + v1]) + cfa[indx];
              }
            }
            else
            { // R or B site

              Ginth = hcd[indx] + cfa[indx]; // interpolated G
              Gintv = vcd[indx] + cfa[indx];

              if(hcd[indx] < 0)
              {
                if(3.f * hcd[indx] < -(Ginth + cfa[indx]))
                {
                  hcd[indx] = ULIM(Ginth, cfa[indx - 1], cfa[indx + 1]) - cfa[indx];
                }
                else
                {
                  float hwt = 1.f + 3.f * hcd[indx] / (eps + Ginth + cfa[indx]);
                  hcd[indx] = hwt * hcd[indx]
                              + (1.f - hwt) * (ULIM(Ginth, cfa[indx - 1], cfa[indx + 1]) - cfa[indx]);
                }
              }

              if(vcd[indx] < 0)
              {
                if(3.f * vcd[indx] < -(Gintv + cfa[indx]))
                {
                  vcd[indx] = ULIM(Gintv, cfa[indx - v1], cfa[indx + v1]) - cfa[indx];
                }
                else
                {
                  const float vwt = 1.f + 3.f * vcd[indx] / (eps + Gintv + cfa[indx]);
                  vcd[indx] = vwt * vcd[indx]
                              + (1.f - vwt) * (ULIM(Gintv, cfa[indx - v1], cfa[indx + v1]) - cfa[indx]);
                }
              }

              if(Ginth > clip_pt)
              {
                hcd[indx] = ULIM(Ginth, cfa[indx - 1], cfa[indx + 1]) - cfa[indx];
              }

              if(Gintv > clip_pt)
              {
                vcd[indx] = ULIM(Gintv, cfa[indx - v1], cfa[indx + v1]) - cfa[indx];
              }

              cddiffsq[indx] = sqrf(vcd[indx] - hcd[indx]);
            }

            c = !c;
          }
        }


        for(int rr = 6; rr < rr1 - 6; rr++)
        {
          for(int cc = 6 + (FC(rr, 2, filters) & 1), indx = rr * ts + cc; cc < cc1 - 6; cc += 2, indx += 2)
          {

            // compute colour difference variances in cardinal directions

            const float uave = vcd[indx] + vcd[indx - v1] + vcd[indx - v2] + vcd[indx - v3];
            const float dave = vcd[indx] + vcd[indx + v1] + vcd[indx + v2] + vcd[indx + v3];
            const float lave = hcd[indx] + hcd[indx - 1] + hcd[indx - 2] + hcd[indx - 3];
            const float rave = hcd[indx] + hcd[indx + 1] + hcd[indx + 2] + hcd[indx + 3];

            // colour difference (G-R or G-B) variance in up/down/left/right directions
            float Dgrbvvaru = sqrf(vcd[indx] - uave) + sqrf(vcd[indx - v1] - uave) + sqrf(vcd[indx - v2] - uave)
                              + sqrf(vcd[indx - v3] - uave);
            float Dgrbvvard = sqrf(vcd[indx] - dave) + sqrf(vcd[indx + v1] - dave) + sqrf(vcd[indx + v2] - dave)
                              + sqrf(vcd[indx + v3] - dave);
            float Dgrbhvarl = sqrf(hcd[indx] - lave) + sqrf(hcd[indx - 1] - lave) + sqrf(hcd[indx - 2] - lave)
                              + sqrf(hcd[indx - 3] - lave);
            float Dgrbhvarr = sqrf(hcd[indx] - rave) + sqrf(hcd[indx + 1] - rave) + sqrf(hcd[indx + 2] - rave)
                              + sqrf(hcd[indx + 3] - rave);

            const float hwt = dirwts1[indx - 1] / (dirwts1[indx - 1] + dirwts1[indx + 1]);
            const float vwt = dirwts0[indx - v1] / (dirwts0[indx + v1] + dirwts0[indx - v1]);

            const float vcdvar = epssq + vwt * Dgrbvvard + (1.f - vwt) * Dgrbvvaru;
            const float hcdvar = epssq + hwt * Dgrbhvarr + (1.f - hwt) * Dgrbhvarl;

            // compute fluctuations in up/down and left/right interpolations of colours
            Dgrbvvaru = (dgintv[indx]) + (dgintv[indx - v1]) + (dgintv[indx - v2]);
            Dgrbvvard = (dgintv[indx]) + (dgintv[indx + v1]) + (dgintv[indx + v2]);
            Dgrbhvarl = (dginth[indx]) + (dginth[indx - 1]) + (dginth[indx - 2]);
            Dgrbhvarr = (dginth[indx]) + (dginth[indx + 1]) + (dginth[indx + 2]);

            float vcdvar1 = epssq + vwt * Dgrbvvard + (1.f - vwt) * Dgrbvvaru;
            float hcdvar1 = epssq + hwt * Dgrbhvarr + (1.f - hwt) * Dgrbhvarl;

            // determine adaptive weights for G interpolation
            const float varwt = hcdvar / (vcdvar + hcdvar);
            const float diffwt = hcdvar1 / (vcdvar1 + hcdvar1);

            // if both agree on interpolation direction, choose the one with strongest directional
            // discrimination;
            // otherwise, choose the u/d and l/r difference fluctuation weights
            if((0.5 - varwt) * (0.5 - diffwt) > 0 && fabsf(0.5f - diffwt) < fabsf(0.5f - varwt))
            {
              hvwt[indx >> 1] = varwt;
            }
            else
            {
              hvwt[indx >> 1] = diffwt;
            }
          }
        }

        // precompute nyquist
        for(int rr = 6; rr < rr1 - 6; rr++)
        {
          int cc = 6 + (FC(rr, 2, filters) & 1);
          int indx = rr * ts + cc;

          for(; cc < cc1 - 6; cc += 2, indx += 2)
          {
            nyqutest[indx >> 1]
                = (gaussodd[0] * cddiffsq[indx]
                   + gaussodd[1] * (cddiffsq[(indx - m1)] + cddiffsq[(indx + p1)] + cddiffsq[(indx - p1)]
                                    + cddiffsq[(indx + m1)])
                   + gaussodd[2] * (cddiffsq[(indx - v2)] + cddiffsq[(indx - 2)] + cddiffsq[(indx + 2)]
                                    + cddiffsq[(indx + v2)])
                   + gaussodd[3] * (cddiffsq[(indx - m2)] + cddiffsq[(indx + p2)] + cddiffsq[(indx - p2)]
                                    + cddiffsq[(indx + m2)]))
                  - (gaussgrad[0] * delhvsqsum[indx]
                     + gaussgrad[1] * (delhvsqsum[indx - v1] + delhvsqsum[indx + 1] + delhvsqsum[indx - 1]
                                       + delhvsqsum[indx + v1])
                     + gaussgrad[2] * (delhvsqsum[indx - m1] + delhvsqsum[indx + p1] + delhvsqsum[indx - p1]
                                       + delhvsqsum[indx + m1])
                     + gaussgrad[3] * (delhvsqsum[indx - v2] + delhvsqsum[indx - 2] + delhvsqsum[indx + 2]
                                       + delhvsqsum[indx + v2])
                     + gaussgrad[4] * (delhvsqsum[indx - v2 - 1] + delhvsqsum[indx - v2 + 1]
                                       + delhvsqsum[indx - ts - 2] + delhvsqsum[indx - ts + 2]
                                       + delhvsqsum[indx + ts - 2] + delhvsqsum[indx + ts + 2]
                                       + delhvsqsum[indx + v2 - 1] + delhvsqsum[indx + v2 + 1])
                     + gaussgrad[5] * (delhvsqsum[indx - m2] + delhvsqsum[indx + p2] + delhvsqsum[indx - p2]
                                       + delhvsqsum[indx + m2]));
          }
        }

        // Nyquist test
        int nystartrow = 0;
        int nyendrow = 0;
        int nystartcol = ts + 1;
        int nyendcol = 0;

        for(int rr = 6; rr < rr1 - 6; rr++)
        {
          for(int cc = 6 + (FC(rr, 2, filters) & 1), indx = rr * ts + cc; cc < cc1 - 6; cc += 2, indx += 2)
          {

            // nyquist texture test: ask if difference of vcd compared to hcd is larger or smaller than RGGB
            // gradients
            if(nyqutest[indx >> 1] > 0.f)
            {
              nyquist[indx >> 1] = 1; // nyquist=1 for nyquist region
              nystartrow = nystartrow ? nystartrow : rr;
              nyendrow = rr;
              nystartcol = nystartcol > cc ? cc : nystartcol;
              nyendcol = nyendcol < cc ? cc : nyendcol;
            }
          }
        }


        bool doNyquist = nystartrow != nyendrow && nystartcol != nyendcol;

        if(doNyquist)
        {
          nyendrow++; // because of < condition
          nyendcol++; // because of < condition
          nystartcol -= (nystartcol & 1);
          nystartrow = std::max(8, nystartrow);
          nyendrow = std::min(rr1 - 8, nyendrow);
          nystartcol = std::max(8, nystartcol);
          nyendcol = std::min(cc1 - 8, nyendcol);
          memset(&nyquist2[4 * tsh], 0, sizeof(char) * (ts - 8) * tsh);

          for(int rr = nystartrow; rr < nyendrow; rr++)
          {
            for(int indx = rr * ts + nystartcol + (FC(rr, 2, filters) & 1); indx < rr * ts + nyendcol;
                indx += 2)
            {
              unsigned int nyquisttemp
                  = (nyquist[(indx - v2) >> 1] + nyquist[(indx - m1) >> 1] + nyquist[(indx + p1) >> 1]
                     + nyquist[(indx - 2) >> 1] + nyquist[(indx + 2) >> 1] + nyquist[(indx - p1) >> 1]
                     + nyquist[(indx + m1) >> 1] + nyquist[(indx + v2) >> 1]);
              // if most of your neighbours are named Nyquist, it's likely that you're one too, or not
              nyquist2[indx >> 1] = nyquisttemp > 4 ? 1 : (nyquisttemp < 4 ? 0 : nyquist[indx >> 1]);
            }
          }

          // end of Nyquist test

          // in areas of Nyquist texture, do area interpolation
          for(int rr = nystartrow; rr < nyendrow; rr++)
            for(int indx = rr * ts + nystartcol + (FC(rr, 2, filters) & 1); indx < rr * ts + nyendcol;
                indx += 2)
            {

              if(nyquist2[indx >> 1])
              {
                // area interpolation

                float sumcfa = 0.f, sumh = 0.f, sumv = 0.f, sumsqh = 0.f, sumsqv = 0.f, areawt = 0.f;

                for(int i = -6; i < 7; i += 2)
                {
                  int indx1 = indx + (i * ts) - 6;

                  for(int j = -6; j < 7; j += 2, indx1 += 2)
                  {
                    if(nyquist2[indx1 >> 1])
                    {
                      float cfatemp = cfa[indx1];
                      sumcfa += cfatemp;
                      sumh += (cfa[indx1 - 1] + cfa[indx1 + 1]);
                      sumv += (cfa[indx1 - v1] + cfa[indx1 + v1]);
                      sumsqh += sqrf(cfatemp - cfa[indx1 - 1]) + sqrf(cfatemp - cfa[indx1 + 1]);
                      sumsqv += sqrf(cfatemp - cfa[indx1 - v1]) + sqrf(cfatemp - cfa[indx1 + v1]);
                      areawt += 1;
                    }
                  }
                }

                // horizontal and vertical colour differences, and adaptive weight
                sumh = sumcfa - _xdiv2f(sumh);
                sumv = sumcfa - _xdiv2f(sumv);
                areawt = _xdiv2f(areawt);
                const float hcdvar = epssq + fabsf(areawt * sumsqh - sumh * sumh);
                const float vcdvar = epssq + fabsf(areawt * sumsqv - sumv * sumv);
                hvwt[indx >> 1] = hcdvar / (vcdvar + hcdvar);

                // end of area interpolation
              }
            }
        }


        // populate G at R/B sites
        for(int rr = 8; rr < rr1 - 8; rr++)
          for(int indx = rr * ts + 8 + (FC(rr, 2, filters) & 1); indx < rr * ts + cc1 - 8; indx += 2)
          {

            // first ask if one gets more directional discrimination from nearby B/R sites
            const float hvwtalt = _xdivf(hvwt[(indx - m1) >> 1] + hvwt[(indx + p1) >> 1] + hvwt[(indx - p1) >> 1]
                                      + hvwt[(indx + m1) >> 1],
                                  2);

            hvwt[indx >> 1]
                = fabsf(0.5f - hvwt[indx >> 1]) < fabsf(0.5f - hvwtalt) ? hvwtalt : hvwt[indx >> 1];
            // a better result was obtained from the neighbours

            Dgrb[0][indx >> 1] = interpolatef(hvwt[indx >> 1], vcd[indx], hcd[indx]); // evaluate colour differences

            rgbgreen[indx] = cfa[indx] + Dgrb[0][indx >> 1]; // evaluate G (finally!)

            // local curvature in G (preparation for nyquist refinement step)
            Dgrb2[indx >> 1].h = nyquist2[indx >> 1]
                                     ? sqrf(rgbgreen[indx] - _xdiv2f(rgbgreen[indx - 1] + rgbgreen[indx + 1]))
                                     : 0.f;
            Dgrb2[indx >> 1].v = nyquist2[indx >> 1]
                                     ? sqrf(rgbgreen[indx] - _xdiv2f(rgbgreen[indx - v1] + rgbgreen[indx + v1]))
                                     : 0.f;
          }


        // end of standard interpolation

        // refine Nyquist areas using G curvatures
        if(doNyquist)
        {
          for(int rr = nystartrow; rr < nyendrow; rr++)
            for(int indx = rr * ts + nystartcol + (FC(rr, 2, filters) & 1); indx < rr * ts + nyendcol;
                indx += 2)
            {

              if(nyquist2[indx >> 1])
              {
                // local averages (over Nyquist pixels only) of G curvature squared
                const float gvarh
                    = epssq + (gquinc[0] * Dgrb2[indx >> 1].h
                               + gquinc[1] * (Dgrb2[(indx - m1) >> 1].h + Dgrb2[(indx + p1) >> 1].h
                                              + Dgrb2[(indx - p1) >> 1].h + Dgrb2[(indx + m1) >> 1].h)
                               + gquinc[2] * (Dgrb2[(indx - v2) >> 1].h + Dgrb2[(indx - 2) >> 1].h
                                              + Dgrb2[(indx + 2) >> 1].h + Dgrb2[(indx + v2) >> 1].h)
                               + gquinc[3] * (Dgrb2[(indx - m2) >> 1].h + Dgrb2[(indx + p2) >> 1].h
                                              + Dgrb2[(indx - p2) >> 1].h + Dgrb2[(indx + m2) >> 1].h));
                const float gvarv
                    = epssq + (gquinc[0] * Dgrb2[indx >> 1].v
                               + gquinc[1] * (Dgrb2[(indx - m1) >> 1].v + Dgrb2[(indx + p1) >> 1].v
                                              + Dgrb2[(indx - p1) >> 1].v + Dgrb2[(indx + m1) >> 1].v)
                               + gquinc[2] * (Dgrb2[(indx - v2) >> 1].v + Dgrb2[(indx - 2) >> 1].v
                                              + Dgrb2[(indx + 2) >> 1].v + Dgrb2[(indx + v2) >> 1].v)
                               + gquinc[3] * (Dgrb2[(indx - m2) >> 1].v + Dgrb2[(indx + p2) >> 1].v
                                              + Dgrb2[(indx - p2) >> 1].v + Dgrb2[(indx + m2) >> 1].v));
                // use the results as weights for refined G interpolation
                Dgrb[0][indx >> 1] = (hcd[indx] * gvarv + vcd[indx] * gvarh) / (gvarv + gvarh);
                rgbgreen[indx] = cfa[indx] + Dgrb[0][indx >> 1];
              }
            }
        }

        for(int rr = 6; rr < rr1 - 6; rr++)
        {
          if((FC(rr, 2, filters) & 1) == 0)
          {
            for(int cc = 6, indx = rr * ts + cc; cc < cc1 - 6; cc += 2, indx += 2)
            {
              delp[indx >> 1] = fabsf(cfa[indx + p1] - cfa[indx - p1]);
              delm[indx >> 1] = fabsf(cfa[indx + m1] - cfa[indx - m1]);
              Dgrbsq1p[indx >> 1]
                  = (sqrf(cfa[indx + 1] - cfa[indx + 1 - p1]) + sqrf(cfa[indx + 1] - cfa[indx + 1 + p1]));
              Dgrbsq1m[indx >> 1]
                  = (sqrf(cfa[indx + 1] - cfa[indx + 1 - m1]) + sqrf(cfa[indx + 1] - cfa[indx + 1 + m1]));
            }
          }
          else
          {
            for(int cc = 6, indx = rr * ts + cc; cc < cc1 - 6; cc += 2, indx += 2)
            {
              Dgrbsq1p[indx >> 1] = (sqrf(cfa[indx] - cfa[indx - p1]) + sqrf(cfa[indx] - cfa[indx + p1]));
              Dgrbsq1m[indx >> 1] = (sqrf(cfa[indx] - cfa[indx - m1]) + sqrf(cfa[indx] - cfa[indx + m1]));
              delp[indx >> 1] = fabsf(cfa[indx + 1 + p1] - cfa[indx + 1 - p1]);
              delm[indx >> 1] = fabsf(cfa[indx + 1 + m1] - cfa[indx + 1 - m1]);
            }
          }
        }

// diagonal interpolation correction
        for(int rr = 8; rr < rr1 - 8; rr++)
        {
          for(int cc = 8 + (FC(rr, 2, filters) & 1), indx = rr * ts + cc, indx1 = indx >> 1; cc < cc1 - 8;
              cc += 2, indx += 2, indx1++)
          {

            // diagonal colour ratios
            float crse = _xmul2f(cfa[indx + m1]) / (eps + cfa[indx] + (cfa[indx + m2]));
            float crnw = _xmul2f(cfa[indx - m1]) / (eps + cfa[indx] + (cfa[indx - m2]));
            float crne = _xmul2f(cfa[indx + p1]) / (eps + cfa[indx] + (cfa[indx + p2]));
            float crsw = _xmul2f(cfa[indx - p1]) / (eps + cfa[indx] + (cfa[indx - p2]));
            // colour differences in diagonal directions
            float rbse, rbnw, rbne, rbsw;

            // assign B/R at R/B sites
            if(fabsf(1.f - crse) < arthresh)
            {
              rbse = cfa[indx] * crse; // use this if more precise diag interp is necessary
            }
            else
            {
              rbse = (cfa[indx + m1]) + _xdiv2f(cfa[indx] - cfa[indx + m2]);
            }

            if(fabsf(1.f - crnw) < arthresh)
            {
              rbnw = cfa[indx] * crnw;
            }
            else
            {
              rbnw = (cfa[indx - m1]) + _xdiv2f(cfa[indx] - cfa[indx - m2]);
            }

            if(fabsf(1.f - crne) < arthresh)
            {
              rbne = cfa[indx] * crne;
            }
            else
            {
              rbne = (cfa[indx + p1]) + _xdiv2f(cfa[indx] - cfa[indx + p2]);
            }

            if(fabsf(1.f - crsw) < arthresh)
            {
              rbsw = cfa[indx] * crsw;
            }
            else
            {
              rbsw = (cfa[indx - p1]) + _xdiv2f(cfa[indx] - cfa[indx - p2]);
            }

            const float wtse = eps + delm[indx1] + delm[(indx + m1) >> 1]
                         + delm[(indx + m2) >> 1]; // same as for wtu,wtd,wtl,wtr
            const float wtnw = eps + delm[indx1] + delm[(indx - m1) >> 1] + delm[(indx - m2) >> 1];
            const float wtne = eps + delp[indx1] + delp[(indx + p1) >> 1] + delp[(indx + p2) >> 1];
            const float wtsw = eps + delp[indx1] + delp[(indx - p1) >> 1] + delp[(indx - p2) >> 1];


            rbm[indx1] = (wtse * rbnw + wtnw * rbse) / (wtse + wtnw);
            rbp[indx1] = (wtne * rbsw + wtsw * rbne) / (wtne + wtsw);

            // variance of R-B in plus/minus directions
            const float rbvarm = epssq
                  + (gausseven[0] * (Dgrbsq1m[(indx - v1) >> 1] + Dgrbsq1m[(indx - 1) >> 1]
                                     + Dgrbsq1m[(indx + 1) >> 1] + Dgrbsq1m[(indx + v1) >> 1])
                     + gausseven[1] * (Dgrbsq1m[(indx - v2 - 1) >> 1] + Dgrbsq1m[(indx - v2 + 1) >> 1]
                                       + Dgrbsq1m[(indx - 2 - v1) >> 1] + Dgrbsq1m[(indx + 2 - v1) >> 1]
                                       + Dgrbsq1m[(indx - 2 + v1) >> 1] + Dgrbsq1m[(indx + 2 + v1) >> 1]
                                       + Dgrbsq1m[(indx + v2 - 1) >> 1] + Dgrbsq1m[(indx + v2 + 1) >> 1]));
            pmwt[indx1] = rbvarm
                  / ((epssq + (gausseven[0] * (Dgrbsq1p[(indx - v1) >> 1] + Dgrbsq1p[(indx - 1) >> 1]
                                               + Dgrbsq1p[(indx + 1) >> 1] + Dgrbsq1p[(indx + v1) >> 1])
                               + gausseven[1]
                                     * (Dgrbsq1p[(indx - v2 - 1) >> 1] + Dgrbsq1p[(indx - v2 + 1) >> 1]
                                        + Dgrbsq1p[(indx - 2 - v1) >> 1] + Dgrbsq1p[(indx + 2 - v1) >> 1]
                                        + Dgrbsq1p[(indx - 2 + v1) >> 1] + Dgrbsq1p[(indx + 2 + v1) >> 1]
                                        + Dgrbsq1p[(indx + v2 - 1) >> 1] + Dgrbsq1p[(indx + v2 + 1) >> 1])))
                     + rbvarm);

            // bound the interpolation in regions of high saturation

            if(rbp[indx1] < cfa[indx])
            {
              if(_xmul2f(rbp[indx1]) < cfa[indx])
              {
                rbp[indx1] = ULIM(rbp[indx1], cfa[indx - p1], cfa[indx + p1]);
              }
              else
              {
                const float pwt = _xmul2f(cfa[indx] - rbp[indx1]) / (eps + rbp[indx1] + cfa[indx]);
                rbp[indx1] = pwt * rbp[indx1] + (1.f - pwt) * ULIM(rbp[indx1], cfa[indx - p1], cfa[indx + p1]);
              }
            }

            if(rbm[indx1] < cfa[indx])
            {
              if(_xmul2f(rbm[indx1]) < cfa[indx])
              {
                rbm[indx1] = ULIM(rbm[indx1], cfa[indx - m1], cfa[indx + m1]);
              }
              else
              {
                const float mwt = _xmul2f(cfa[indx] - rbm[indx1]) / (eps + rbm[indx1] + cfa[indx]);
                rbm[indx1] = mwt * rbm[indx1] + (1.f - mwt) * ULIM(rbm[indx1], cfa[indx - m1], cfa[indx + m1]);
              }
            }

            if(rbp[indx1] > clip_pt)
            {
              rbp[indx1] = ULIM(rbp[indx1], cfa[indx - p1], cfa[indx + p1]);
            }

            if(rbm[indx1] > clip_pt)
            {
              rbm[indx1] = ULIM(rbm[indx1], cfa[indx - m1], cfa[indx + m1]);
            }
          }
        }

        for(int rr = 10; rr < rr1 - 10; rr++)
          for(int cc = 10 + (FC(rr, 2, filters) & 1), indx = rr * ts + cc, indx1 = indx >> 1; cc < cc1 - 10;
              cc += 2, indx += 2, indx1++)
          {

            // first ask if one gets more directional discrimination from nearby B/R sites
            const float pmwtalt = _xdivf(pmwt[(indx - m1) >> 1] + pmwt[(indx + p1) >> 1] + pmwt[(indx - p1) >> 1]
                                      + pmwt[(indx + m1) >> 1],
                                  2);

            if(fabsf(0.5f - pmwt[indx1]) < fabsf(0.5f - pmwtalt))
            {
              pmwt[indx1] = pmwtalt; // a better result was obtained from the neighbours
            }

            rbint[indx1] = _xdiv2f(cfa[indx] + rbm[indx1] * (1.f - pmwt[indx1])
                                  + rbp[indx1] * pmwt[indx1]); // this is R+B, interpolated
          }

        for(int rr = 12; rr < rr1 - 12; rr++)
          for(int cc = 12 + (FC(rr, 2, filters) & 1), indx = rr * ts + cc, indx1 = indx >> 1; cc < cc1 - 12;
              cc += 2, indx += 2, indx1++)
          {

            if(fabsf(0.5f - pmwt[indx >> 1]) < fabsf(0.5f - hvwt[indx >> 1]))
            {
              continue;
            }

            // now interpolate G vertically/horizontally using R+B values
            // unfortunately, since G interpolation cannot be done diagonally this may lead to colour shifts

            // colour ratios for G interpolation
            const float cru = cfa[indx - v1] * 2.0 / (eps + rbint[indx1] + rbint[(indx1 - v1)]);
            const float crd = cfa[indx + v1] * 2.0 / (eps + rbint[indx1] + rbint[(indx1 + v1)]);
            const float crl = cfa[indx - 1] * 2.0 / (eps + rbint[indx1] + rbint[(indx1 - 1)]);
            const float crr = cfa[indx + 1] * 2.0 / (eps + rbint[indx1] + rbint[(indx1 + 1)]);

            // interpolation of G in four directions
            float gu, gd, gl, gr;

            // interpolated G via adaptive ratios or Hamilton-Adams in each cardinal direction
            if(fabsf(1.f - cru) < arthresh)
            {
              gu = rbint[indx1] * cru;
            }
            else
            {
              gu = cfa[indx - v1] + _xdiv2f(rbint[indx1] - rbint[(indx1 - v1)]);
            }

            if(fabsf(1.f - crd) < arthresh)
            {
              gd = rbint[indx1] * crd;
            }
            else
            {
              gd = cfa[indx + v1] + _xdiv2f(rbint[indx1] - rbint[(indx1 + v1)]);
            }

            if(fabsf(1.f - crl) < arthresh)
            {
              gl = rbint[indx1] * crl;
            }
            else
            {
              gl = cfa[indx - 1] + _xdiv2f(rbint[indx1] - rbint[(indx1 - 1)]);
            }

            if(fabsf(1.f - crr) < arthresh)
            {
              gr = rbint[indx1] * crr;
            }
            else
            {
              gr = cfa[indx + 1] + _xdiv2f(rbint[indx1] - rbint[(indx1 + 1)]);
            }

            // interpolated G via adaptive weights of cardinal evaluations
            float Gintv = (dirwts0[indx - v1] * gd + dirwts0[indx + v1] * gu)
                          / (dirwts0[indx + v1] + dirwts0[indx - v1]);
            float Ginth = (dirwts1[indx - 1] * gr + dirwts1[indx + 1] * gl)
                          / (dirwts1[indx - 1] + dirwts1[indx + 1]);

            // bound the interpolation in regions of high saturation
            if(Gintv < rbint[indx1])
            {
              if(2 * Gintv < rbint[indx1])
              {
                Gintv = ULIM(Gintv, cfa[indx - v1], cfa[indx + v1]);
              }
              else
              {
                float vwt = 2.0 * (rbint[indx1] - Gintv) / (eps + Gintv + rbint[indx1]);
                Gintv = vwt * Gintv + (1.f - vwt) * ULIM(Gintv, cfa[indx - v1], cfa[indx + v1]);
              }
            }

            if(Ginth < rbint[indx1])
            {
              if(2 * Ginth < rbint[indx1])
              {
                Ginth = ULIM(Ginth, cfa[indx - 1], cfa[indx + 1]);
              }
              else
              {
                const float hwt = 2.0 * (rbint[indx1] - Ginth) / (eps + Ginth + rbint[indx1]);
                Ginth = hwt * Ginth + (1.f - hwt) * ULIM(Ginth, cfa[indx - 1], cfa[indx + 1]);
              }
            }

            if(Ginth > clip_pt)
            {
              Ginth = ULIM(Ginth, cfa[indx - 1], cfa[indx + 1]);
            }

            if(Gintv > clip_pt)
            {
              Gintv = ULIM(Gintv, cfa[indx - v1], cfa[indx + v1]);
            }

            rgbgreen[indx] = Ginth * (1.f - hvwt[indx1]) + Gintv * hvwt[indx1];
            Dgrb[0][indx >> 1] = rgbgreen[indx] - cfa[indx];
          }

        // end of diagonal interpolation correction

        // fancy chrominance interpolation
        //(ey,ex) is location of R site
        for(int rr = 13 - ey; rr < rr1 - 12; rr += 2)
          for(int indx1 = (rr * ts + 13 - ex) >> 1; indx1<(rr * ts + cc1 - 12)>> 1; indx1++)
          {                                  // B coset
            Dgrb[1][indx1] = Dgrb[0][indx1]; // split out G-B from G-R
            Dgrb[0][indx1] = 0;
          }

        for(int rr = 14; rr < rr1 - 14; rr++)

          for(int cc = 14 + (FC(rr, 2, filters) & 1), indx = rr * ts + cc, c = 1 - FC(rr, cc, filters) / 2;
              cc < cc1 - 14; cc += 2, indx += 2)
          {
            const float wtnw = 1.f / (eps + fabsf(Dgrb[c][(indx - m1) >> 1] - Dgrb[c][(indx + m1) >> 1])
                                + fabsf(Dgrb[c][(indx - m1) >> 1] - Dgrb[c][(indx - m3) >> 1])
                                + fabsf(Dgrb[c][(indx + m1) >> 1] - Dgrb[c][(indx - m3) >> 1]));
            const float wtne = 1.f / (eps + fabsf(Dgrb[c][(indx + p1) >> 1] - Dgrb[c][(indx - p1) >> 1])
                                + fabsf(Dgrb[c][(indx + p1) >> 1] - Dgrb[c][(indx + p3) >> 1])
                                + fabsf(Dgrb[c][(indx - p1) >> 1] - Dgrb[c][(indx + p3) >> 1]));
            const float wtsw = 1.f / (eps + fabsf(Dgrb[c][(indx - p1) >> 1] - Dgrb[c][(indx + p1) >> 1])
                                + fabsf(Dgrb[c][(indx - p1) >> 1] - Dgrb[c][(indx + m3) >> 1])
                                + fabsf(Dgrb[c][(indx + p1) >> 1] - Dgrb[c][(indx - p3) >> 1]));
            const float wtse = 1.f / (eps + fabsf(Dgrb[c][(indx + m1) >> 1] - Dgrb[c][(indx - m1) >> 1])
                                + fabsf(Dgrb[c][(indx + m1) >> 1] - Dgrb[c][(indx - p3) >> 1])
                                + fabsf(Dgrb[c][(indx - m1) >> 1] - Dgrb[c][(indx + m3) >> 1]));

            Dgrb[c][indx >> 1]
                = (wtnw * (1.325f * Dgrb[c][(indx - m1) >> 1] - 0.175f * Dgrb[c][(indx - m3) >> 1]
                           - 0.075f * Dgrb[c][(indx - m1 - 2) >> 1] - 0.075f * Dgrb[c][(indx - m1 - v2) >> 1])
                   + wtne * (1.325f * Dgrb[c][(indx + p1) >> 1] - 0.175f * Dgrb[c][(indx + p3) >> 1]
                             - 0.075f * Dgrb[c][(indx + p1 + 2) >> 1]
                             - 0.075f * Dgrb[c][(indx + p1 + v2) >> 1])
                   + wtsw * (1.325f * Dgrb[c][(indx - p1) >> 1] - 0.175f * Dgrb[c][(indx - p3) >> 1]
                             - 0.075f * Dgrb[c][(indx - p1 - 2) >> 1]
                             - 0.075f * Dgrb[c][(indx - p1 - v2) >> 1])
                   + wtse * (1.325f * Dgrb[c][(indx + m1) >> 1] - 0.175f * Dgrb[c][(indx + m3) >> 1]
                             - 0.075f * Dgrb[c][(indx + m1 + 2) >> 1]
                             - 0.075f * Dgrb[c][(indx + m1 + v2) >> 1]))
                  / (wtnw + wtne + wtsw + wtse);
          }

        for(int rr = 16; rr < rr1 - 16; rr++)
        {
          int row = rr + top;
          int col = left + 16;
          int indx = rr * ts + 16;

          if((FC(rr, 2, filters) & 1) == 1)
          {
            for(; indx < rr * ts + cc1 - 16 - (cc1 & 1); indx++, col++)
            {
              if(col < roi_out->width && row < roi_out->height)
              {
                const float temp = 1.f / (hvwt[(indx - v1) >> 1] + 2.f - hvwt[(indx + 1) >> 1]
                                    - hvwt[(indx - 1) >> 1] + hvwt[(indx + v1) >> 1]);
                out[(row * roi_out->width + col) * 4]
                    = _clampnan(rgbgreen[indx]
                                   - ((hvwt[(indx - v1) >> 1]) * Dgrb[0][(indx - v1) >> 1]
                                      + (1.f - hvwt[(indx + 1) >> 1]) * Dgrb[0][(indx + 1) >> 1]
                                      + (1.f - hvwt[(indx - 1) >> 1]) * Dgrb[0][(indx - 1) >> 1]
                                      + (hvwt[(indx + v1) >> 1]) * Dgrb[0][(indx + v1) >> 1])
                                         * temp,
                               0.0, 1.0);
 
                out[(row * roi_out->width + col) * 4 + 2]
                    = _clampnan(rgbgreen[indx]
                                   - ((hvwt[(indx - v1) >> 1]) * Dgrb[1][(indx - v1) >> 1]
                                      + (1.f - hvwt[(indx + 1) >> 1]) * Dgrb[1][(indx + 1) >> 1]
                                      + (1.f - hvwt[(indx - 1) >> 1]) * Dgrb[1][(indx - 1) >> 1]
                                      + (hvwt[(indx + v1) >> 1]) * Dgrb[1][(indx + v1) >> 1])
                                         * temp,
                               0.0, 1.0);
              }

              indx++;
              col++;
              if(col < roi_out->width && row < roi_out->height)
              {
                out[(row * roi_out->width + col) * 4]
                    = _clampnan(rgbgreen[indx] - Dgrb[0][indx >> 1], 0.0, 1.0);
                out[(row * roi_out->width + col) * 4 + 2]
                    = _clampnan(rgbgreen[indx] - Dgrb[1][indx >> 1], 0.0, 1.0);
              }
            }

            if(cc1 & 1)
            { // width of tile is odd
              if(col < roi_out->width && row < roi_out->height)
              {
                const float temp = 1.f / (hvwt[(indx - v1) >> 1] + 2.f - hvwt[(indx + 1) >> 1]
                                    - hvwt[(indx - 1) >> 1] + hvwt[(indx + v1) >> 1]);
                out[(row * roi_out->width + col) * 4]
                    = _clampnan(rgbgreen[indx]
                                   - ((hvwt[(indx - v1) >> 1]) * Dgrb[0][(indx - v1) >> 1]
                                      + (1.f - hvwt[(indx + 1) >> 1]) * Dgrb[0][(indx + 1) >> 1]
                                      + (1.f - hvwt[(indx - 1) >> 1]) * Dgrb[0][(indx - 1) >> 1]
                                      + (hvwt[(indx + v1) >> 1]) * Dgrb[0][(indx + v1) >> 1])
                                         * temp,
                               0.0, 1.0);
                out[(row * roi_out->width + col) * 4 + 2]
                    = _clampnan(rgbgreen[indx]
                                   - ((hvwt[(indx - v1) >> 1]) * Dgrb[1][(indx - v1) >> 1]
                                      + (1.f - hvwt[(indx + 1) >> 1]) * Dgrb[1][(indx + 1) >> 1]
                                      + (1.f - hvwt[(indx - 1) >> 1]) * Dgrb[1][(indx - 1) >> 1]
                                      + (hvwt[(indx + v1) >> 1]) * Dgrb[1][(indx + v1) >> 1])
                                         * temp,
                               0.0, 1.0);
              }
            }
          }
          else
          {
            for(; indx < rr * ts + cc1 - 16 - (cc1 & 1); indx++, col++)
            {
              if(col < roi_out->width && row < roi_out->height)
              {
                out[(row * roi_out->width + col) * 4]
                    = _clampnan(rgbgreen[indx] - Dgrb[0][indx >> 1], 0.0f, 1.0f);
                out[(row * roi_out->width + col) * 4 + 2]
                    = _clampnan(rgbgreen[indx] - Dgrb[1][indx >> 1], 0.0f, 1.0f);
              }

              indx++;
              col++;
              if(col < roi_out->width && row < roi_out->height)
              {
                const float temp = 1.f / (hvwt[(indx - v1) >> 1] + 2.f - hvwt[(indx + 1) >> 1]
                                    - hvwt[(indx - 1) >> 1] + hvwt[(indx + v1) >> 1]);

                out[(row * roi_out->width + col) * 4]
                    = _clampnan(rgbgreen[indx]
                                   - ((hvwt[(indx - v1) >> 1]) * Dgrb[0][(indx - v1) >> 1]
                                      + (1.0f - hvwt[(indx + 1) >> 1]) * Dgrb[0][(indx + 1) >> 1]
                                      + (1.0f - hvwt[(indx - 1) >> 1]) * Dgrb[0][(indx - 1) >> 1]
                                      + (hvwt[(indx + v1) >> 1]) * Dgrb[0][(indx + v1) >> 1])
                                         * temp,
                               0.0f, 1.0f);

                out[(row * roi_out->width + col) * 4 + 2]
                    = _clampnan(rgbgreen[indx]
                                   - ((hvwt[(indx - v1) >> 1]) * Dgrb[1][(indx - v1) >> 1]
                                      + (1.0f - hvwt[(indx + 1) >> 1]) * Dgrb[1][(indx + 1) >> 1]
                                      + (1.0f - hvwt[(indx - 1) >> 1]) * Dgrb[1][(indx - 1) >> 1]
                                      + (hvwt[(indx + v1) >> 1]) * Dgrb[1][(indx + v1) >> 1])
                                         * temp,
                               0.0f, 1.0f);
              }
            }

            if(cc1 & 1)
            { // width of tile is odd
              if(col < roi_out->width && row < roi_out->height)
              {
                out[(row * roi_out->width + col) * 4]
                    = _clampnan(rgbgreen[indx] - Dgrb[0][indx >> 1], 0.0f, 1.0f);
                out[(row * roi_out->width + col) * 4 + 2]
                    = _clampnan(rgbgreen[indx] - Dgrb[1][indx >> 1], 0.0f, 1.0f);
              }
            }
          }
        }

        // copy smoothed results back to image matrix
        for(int rr = 16; rr < rr1 - 16; rr++)
        {
          const int row = rr + top;
          for(int cc = 16; cc < cc1 - 16; cc++)
          {
            const int col = cc + left;
            const int indx = rr * ts + cc;
            if(col < roi_out->width && row < roi_out->height)
              out[(row * roi_out->width + col) * 4 + 1] = _clampnan(rgbgreen[indx], 0.0f, 1.0f);
          }
        }
      }
    } // end of main loop

    // clean up
    free(buffer);
  }
}

/*==================================================================================
 * end of raw therapee code
 *==================================================================================*/
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

