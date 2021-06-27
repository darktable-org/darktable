/*
    This file is part of darktable,
    Copyright (C) 2010-2021 darktable developers.

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

/*
    The lmmse code base used for the darktable port has been taken from rawtherapee derived librtprocess.
    Adaption for dt and tiling - hanno schwalm 06/2021

    LSMME demosaicing algorithm
    L. Zhang and X. Wu,
    Color demozaicing via directional Linear Minimum Mean Square-error Estimation,
    IEEE Trans. on Image Processing, vol. 14, pp. 2167-2178, Dec. 2005.

    Adapted to RawTherapee by Jacques Desmis 3/2013
    Improved speed and reduced memory consumption by Ingo Weyrich 2/2015
*/

/* about performance tested with 45mpix images (AMaZE is 0.45 here)
                      untiled           tiled
   basic:             0.5               0.15
   median:            0.6 (default)     0.25
   3xmedian:          0.8               0.42
   3xmed + 2xrefine:  1.2               0.80
*/

/* Why tiling?
   The internal tiling vastly reduces memory footprint and allows data processing to be done mostly
   with in-cache data thus increasing performance.

   As the borders are calculated with reduced precision due to the algo there is an overlapping
   area which is not used for the internal tiles. 
*/

#ifdef __GNUC__
  #pragma GCC push_options
  #pragma GCC optimize ("fast-math", "fp-contract=fast", "finite-math-only", "no-math-errno")
#endif

#define LMMSE_TILESIZE 128
#define LMMSE_OVERLAP 8
#define BORDER_AROUND 4
#define LMMSE_TILEVALID (LMMSE_TILESIZE - 2 * LMMSE_OVERLAP)
#define LMMSE_GRP (LMMSE_TILESIZE + 2 * BORDER_AROUND)
#define w1 (LMMSE_GRP)
#define w2 (LMMSE_GRP * 2)
#define w3 (LMMSE_GRP * 3)
#define w4 (LMMSE_GRP * 4)

static INLINE float limf(float x, float min, float max)
{
  return fmaxf(min, fminf(x, max));
}

static INLINE float median3f(float x0, float x1, float x2)
{
  return fmaxf(fminf(x0,x1), fminf(x2, fmaxf(x0,x1))); 
}

static INLINE float median9f(float a0, float a1, float a2, float a3, float a4, float a5, float a6, float a7, float a8)
{
  float tmp;
  tmp = fminf(a1, a2);
  a2  = fmaxf(a1, a2);
  a1  = tmp;
  tmp = fminf(a4, a5);
  a5  = fmaxf(a4, a5);
  a4  = tmp;
  tmp = fminf(a7, a8);
  a8  = fmaxf(a7, a8);
  a7  = tmp;
  tmp = fminf(a0, a1);
  a1  = fmaxf(a0, a1);
  a0  = tmp;
  tmp = fminf(a3, a4);
  a4  = fmaxf(a3, a4);
  a3  = tmp;
  tmp = fminf(a6, a7);
  a7  = fmaxf(a6, a7);
  a6  = tmp;
  tmp = fminf(a1, a2);
  a2  = fmaxf(a1, a2);
  a1  = tmp;
  tmp = fminf(a4, a5);
  a5  = fminf(a4, a5);
  a4  = tmp;
  tmp = fminf(a7, a8);
  a8  = fmaxf(a7, a8);
  a3  = fmaxf(a0, a3);
  a5  = fminf(a5, a8);
  a7  = fmaxf(a4, tmp);
  tmp = fminf(a4, tmp);
  a6  = fmaxf(a3, a6);
  a4  = fmaxf(a1, tmp);
  a2  = fminf(a2, a5);
  a4  = fminf(a4, a7);
  tmp = fminf(a4, a2);
  a2  = fmaxf(a4, a2);
  a4  = fmaxf(a6, tmp);
  return fminf(a4, a2);
}

static INLINE float calc_gamma(float val, float *table)
{
  const float index = val * 65535.0f;
  if(index < 0.0f)      return 0.0f;
  if(index > 65534.99f) return 1.0f;
  const int idx = (int)index;

  const float diff = index - (float)idx;
  const float p1 = table[idx];
  const float p2 = table[idx+1] - p1;
  return (p1 + p2 * diff);
}

/*
   Refinement based on EECI demosaicing algorithm by L. Chang and Y.P. Tan
   Paul Lee
   Adapted for RawTherapee - Jacques Desmis 04/2013
*/

#define c1 4
#define c2 8

#ifdef _OPENMP
  #pragma omp declare simd aligned(out)
#endif
static void refinement(const int width, const int height, float *const restrict out, const uint32_t filters, const int refine)
{
  const int r1 = 4 * width;
  const int r2 = 8 * width;
  for(int b = 0; b < refine; b++)
  {
    // Reinforce interpolated green pixels on RED/BLUE pixel locations
#ifdef _OPENMP
  #pragma omp parallel for simd default(none) \
  dt_omp_sharedconst(out, width, height, filters, r1, r2) \
  schedule(simd:static) aligned(out : 64) 
#endif
    for(int row = 2; row < height - 2; row++)
    {
      for(int col = 2 + (FC(row, 2, filters) & 1), c = FC(row, col, filters); col < width - 2; col += 2)
      {
        float *rgb = &out[4 * (width * row + col)];
        const float dL = 1.0f / (1.0f + fabsf(rgb[c - c2] - rgb[c]) + fabsf(rgb[1 + c1] - rgb[1 - c1]));
        const float dR = 1.0f / (1.0f + fabsf(rgb[c + c2] - rgb[c]) + fabsf(rgb[1 + c1] - rgb[1 - c1]));
        const float dU = 1.0f / (1.0f + fabsf(rgb[c - r2] - rgb[c]) + fabsf(rgb[1 + r1] - rgb[1 - r1]));
        const float dD = 1.0f / (1.0f + fabsf(rgb[c + r2] - rgb[c]) + fabsf(rgb[1 + r1] - rgb[1 - r1]));
        const float v0 = (rgb[c] + ((rgb[1 - c1] - rgb[c - c1]) * dL + (rgb[1 + c1] - rgb[c + c1]) * dR + (rgb[1 - r1] - rgb[c - r1]) * dU + (rgb[1 + r1] - rgb[c + r1]) * dD ) / (dL + dR + dU + dD));
        rgb[1] = fmaxf(0.0f, v0);
      }
    }

    // Reinforce interpolated red/blue pixels on GREEN pixel locations
#ifdef _OPENMP
  #pragma omp parallel for simd default(none) \
  dt_omp_sharedconst(out, width, height, filters, r1, r2) \
  schedule(simd:static) aligned(out : 64) 
#endif
    for(int row = 2; row < height - 2; row++)
    {
      for(int col = 2 + (FC(row, 3, filters) & 1), c = FC(row, col + 1, filters); col < width - 2; col += 2)
      {
        float *rgb = &out[4 * (width * row + col)];
        for(int i = 0; i < 2; c = 2 - c, i++)
        {
          const float dL = 1.0f / (1.0f + fabsf(rgb[1 - c2] - rgb[1]) + fabsf(rgb[c + c1] - rgb[c - c1]));
          const float dR = 1.0f / (1.0f + fabsf(rgb[1 + c2] - rgb[1]) + fabsf(rgb[c + c1] - rgb[c - c1]));
          const float dU = 1.0f / (1.0f + fabsf(rgb[1 - r2] - rgb[1]) + fabsf(rgb[c + r1] - rgb[c - r1]));
          const float dD = 1.0f / (1.0f + fabsf(rgb[1 + r2] - rgb[1]) + fabsf(rgb[c + r1] - rgb[c - r1]));
          const float v0 = (rgb[1] - ((rgb[1 - c1] - rgb[c - c1]) * dL + (rgb[1 + c1] - rgb[c + c1]) * dR + (rgb[1 - r1] - rgb[c - r1]) * dU + (rgb[1 + r1] - rgb[c + r1]) * dD ) / (dL + dR + dU + dD));
          rgb[c] = fmaxf(0.0f, v0);
        }
      }
    }
    // Reinforce integrated red/blue pixels on BLUE/RED pixel locations
#ifdef _OPENMP
  #pragma omp parallel for simd default(none) \
  dt_omp_sharedconst(out, width, height, filters, r1, r2) \
  schedule(simd:static) aligned(out : 64) 
#endif
    for(int row = 2; row < height - 2; row++)
    {
      for(int col = 2 + (FC(row, 2, filters) & 1), c = 2 - FC(row, col, filters); col < width - 2; col += 2)
      {
        float *rgb = &out[4 * (width * row + col)];
        const int d = 2 - c;
        const float dL = 1.0f / (1.0f + fabsf(rgb[d - c2] - rgb[d]) + fabsf(rgb[1 + c1] - rgb[1 - c1]));
        const float dR = 1.0f / (1.0f + fabsf(rgb[d + c2] - rgb[d]) + fabsf(rgb[1 + c1] - rgb[1 - c1]));
        const float dU = 1.0f / (1.0f + fabsf(rgb[d - r2] - rgb[d]) + fabsf(rgb[1 + r1] - rgb[1 - r1]));
        const float dD = 1.0f / (1.0f + fabsf(rgb[d + r2] - rgb[d]) + fabsf(rgb[1 + r1] - rgb[1 - r1]));
        const float v0 = (rgb[1] - ((rgb[1 - c1] - rgb[c - c1]) * dL + (rgb[1 + c1] - rgb[c + c1]) * dR + (rgb[1 - r1] - rgb[c - r1]) * dU + (rgb[1 + r1] - rgb[c + r1]) * dD ) / (dL + dR + dU + dD));
        rgb[c] = fmaxf(0.0f, v0);
      }
    }
  }
}
#undef c1
#undef c2

#ifdef _OPENMP
  #pragma omp declare simd aligned(in, out, gamma_in, gamma_out)
#endif
static void lmmse_demosaic(dt_dev_pixelpipe_iop_t *piece, float *const restrict out, const float *const restrict in, dt_iop_roi_t *const roi_out,
                                   const dt_iop_roi_t *const roi_in, const uint32_t filters, const uint32_t mode, float *const restrict gamma_in, float *const restrict gamma_out)
{
  const int width = roi_in->width;
  const int height = roi_in->height;

  if((width < 16) || (height < 16))
  {
    dt_control_log(_("[lmmse_demosaic] too small area"));
    return;
  }

  float h0 = 1.0f;
  float h1 = expf( -1.0f / 8.0f);
  float h2 = expf( -4.0f / 8.0f);
  float h3 = expf( -9.0f / 8.0f);
  float h4 = expf(-16.0f / 8.0f);
  float hs = h0 + 2.0f * (h1 + h2 + h3 + h4);
  h0 /= hs;
  h1 /= hs;
  h2 /= hs;
  h3 /= hs;
  h4 /= hs;

  // median filter iterations
  const int medians = (mode < 2) ? mode : 3;
  // refinement steps
  const int refine = (mode > 2) ? mode - 2 : 0;

  const float scaler = fmaxf(piece->pipe->dsc.processed_maximum[0], fmaxf(piece->pipe->dsc.processed_maximum[1], piece->pipe->dsc.processed_maximum[2]));
  const float revscaler = 1.0f / scaler;

  const int num_vertical =   1 + (height - 2 * LMMSE_OVERLAP -1) / LMMSE_TILEVALID;
  const int num_horizontal = 1 + (width  - 2 * LMMSE_OVERLAP -1) / LMMSE_TILEVALID;
#ifdef _OPENMP
  #pragma omp parallel \
  dt_omp_firstprivate(width, height, out, in, scaler, revscaler, filters)
#endif
  {
    float *rix[6];
    float *qix[6];
    float *buffer = dt_alloc_align_float(LMMSE_GRP * LMMSE_GRP * 6);

    qix[0] = buffer;
    for(int i = 1; i < 6; i++)
    {
      qix[i] = qix[i - 1] + LMMSE_GRP * LMMSE_GRP;
    }
    memset(buffer, 0, sizeof(float) * LMMSE_GRP * LMMSE_GRP * 6);

#ifdef _OPENMP
  #pragma omp for schedule(simd:dynamic, 6) collapse(2)
#endif
    for(int tile_vertical = 0; tile_vertical < num_vertical; tile_vertical++)
    {
      for(int tile_horizontal = 0; tile_horizontal < num_horizontal; tile_horizontal++)
      {
        const int rowStart = tile_vertical * LMMSE_TILEVALID;
        const int rowEnd = MIN(rowStart + LMMSE_TILESIZE, height);

        const int colStart = tile_horizontal * LMMSE_TILEVALID;
        const int colEnd = MIN(colStart + LMMSE_TILESIZE, width);

        const int tileRows = MIN(rowEnd - rowStart, LMMSE_TILESIZE);
        const int tileCols = MIN(colEnd - colStart, LMMSE_TILESIZE);

        const int last_rr = tileRows + 2 * BORDER_AROUND;
        const int last_cc = tileCols + 2 * BORDER_AROUND;

        for(int rrr = BORDER_AROUND; rrr < tileRows + BORDER_AROUND; rrr++)
        {
          for(int ccc = BORDER_AROUND; ccc < tileCols + BORDER_AROUND; ccc++)
          {
            const int row = rrr - BORDER_AROUND + rowStart;
            const int col = ccc - BORDER_AROUND + colStart;

            float *cfa = qix[5] + rrr * LMMSE_GRP + ccc;
            cfa[0] = calc_gamma(revscaler * in[row * width + col], gamma_in);
          }
        }

        // G-R(B)
        for(int rr = 2; rr < last_rr - 2; rr++)
        {
          // G-R(B) at R(B) location
          for(int cc = 2 + (FC(rr, 2, filters) & 1); cc < last_cc - 2; cc += 2)
          {
            float *cfa = qix[5] + rr * LMMSE_GRP + cc;
            const float v0 = 0.0625f * (cfa[-w1 - 1] + cfa[-w1 + 1] + cfa[w1 - 1] + cfa[w1 + 1]) + 0.25f * cfa[0];
            // horizontal
            rix[0] = qix[0] + rr * LMMSE_GRP + cc;
            rix[0][0] = -0.25f * (cfa[ -2] + cfa[ 2]) + 0.5f * (cfa[ -1] + cfa[0] + cfa[ 1]);
            const float Y0 = v0 + 0.5f * rix[0][0];
    
            rix[0][0] = (cfa[0] > 1.75f * Y0) ? median3f(rix[0][0], cfa[ -1], cfa[ 1]) : limf(rix[0][0], 0.0f, 1.0f);
            rix[0][0] -= cfa[0];
            // vertical
            rix[1] = qix[1] + rr * LMMSE_GRP + cc;
            rix[1][0] = -0.25f * (cfa[-w2] + cfa[w2]) + 0.5f * (cfa[-w1] + cfa[0] + cfa[w1]);
            const float Y1 = v0 + 0.5f * rix[1][0];
    
            rix[1][0] = (cfa[0] > 1.75f * Y1) ? median3f(rix[1][0], cfa[-w1], cfa[w1]) : limf(rix[1][0], 0.0f, 1.0f);
            rix[1][0] -= cfa[0];
          }
    
          // G-R(B) at G location
          for(int ccc = 2 + (FC(rr, 3, filters) & 1); ccc < LMMSE_GRP - 2; ccc += 2)
          {
            float *cfa = qix[5] + rr * LMMSE_GRP + ccc;
            rix[0] = qix[0] + rr * LMMSE_GRP + ccc;
            rix[1] = qix[1] + rr * LMMSE_GRP + ccc;
            rix[0][0] = 0.25f * (cfa[ -2] + cfa[ 2]) - 0.5f * (cfa[ -1] + cfa[0] + cfa[ 1]);
            rix[1][0] = 0.25f * (cfa[-w2] + cfa[w2]) - 0.5f * (cfa[-w1] + cfa[0] + cfa[w1]);
            rix[0][0] = limf(rix[0][0], -1.0f, 0.0f) + cfa[0];
            rix[1][0] = limf(rix[1][0], -1.0f, 0.0f) + cfa[0];
          }
        }

        // apply low pass filter on differential colors
        for (int rr = 4; rr < last_rr - 4; rr++)
        {
          for(int cc = 4; cc < last_cc - 4; cc++)
          {
            rix[0] = qix[0] + rr * LMMSE_GRP + cc;
            rix[2] = qix[2] + rr * LMMSE_GRP + cc;
            rix[2][0] = h0 * rix[0][0] + h1 * (rix[0][ -1] + rix[0][ 1]) + h2 * (rix[0][ -2] + rix[0][ 2]) + h3 * (rix[0][ -3] + rix[0][ 3]) + h4 * (rix[0][ -4] + rix[0][ 4]);
            rix[1] = qix[1] + rr * LMMSE_GRP + cc;
            rix[3] = qix[3] + rr * LMMSE_GRP + cc;
            rix[3][0] = h0 * rix[1][0] + h1 * (rix[1][-w1] + rix[1][w1]) + h2 * (rix[1][-w2] + rix[1][w2]) + h3 * (rix[1][-w3] + rix[1][w3]) + h4 * (rix[1][-w4] + rix[1][w4]);
          }
        }

        for(int rr = 4; rr < last_rr - 4; rr++)
        {
          for(int cc = 4 + (FC(rr, 4, filters) & 1); cc < last_cc - 4; cc += 2)
          {
            rix[0] = qix[0] + rr * LMMSE_GRP + cc;
            rix[1] = qix[1] + rr * LMMSE_GRP + cc;
            rix[2] = qix[2] + rr * LMMSE_GRP + cc;
            rix[3] = qix[3] + rr * LMMSE_GRP + cc;
            float *interp = qix[4] + rr * LMMSE_GRP + cc;
            // horizontal
            float p1 = rix[2][-4];
            float p2 = rix[2][-3];
            float p3 = rix[2][-2];
            float p4 = rix[2][-1];
            float p5 = rix[2][ 0];
            float p6 = rix[2][ 1];
            float p7 = rix[2][ 2];
            float p8 = rix[2][ 3];
            float p9 = rix[2][ 4];
            float mu = (p1 + p2 + p3 + p4 + p5 + p6 + p7 + p8 + p9) / 9.0f;
            float vx = 1e-7f + sqrf(p1 - mu) + sqrf(p2 - mu) + sqrf(p3 - mu) + sqrf(p4 - mu) + sqrf(p5 - mu) + sqrf(p6 - mu) + sqrf(p7 - mu) + sqrf(p8 - mu) + sqrf(p9 - mu);
            p1 -= rix[0][-4];
            p2 -= rix[0][-3];
            p3 -= rix[0][-2];
            p4 -= rix[0][-1];
            p5 -= rix[0][ 0];
            p6 -= rix[0][ 1];
            p7 -= rix[0][ 2];
            p8 -= rix[0][ 3];
            p9 -= rix[0][ 4];
            float vn = 1e-7f + sqrf(p1) + sqrf(p2) + sqrf(p3) + sqrf(p4) + sqrf(p5) + sqrf(p6) + sqrf(p7) + sqrf(p8) + sqrf(p9);
            float xh = (rix[0][0] * vx + rix[2][0] * vn) / (vx + vn);
            float vh = vx * vn / (vx + vn);
    
            // vertical
            p1 = rix[3][-w4];
            p2 = rix[3][-w3];
            p3 = rix[3][-w2];
            p4 = rix[3][-w1];
            p5 = rix[3][  0];
            p6 = rix[3][ w1];
            p7 = rix[3][ w2];
            p8 = rix[3][ w3];
            p9 = rix[3][ w4];
            mu = (p1 + p2 + p3 + p4 + p5 + p6 + p7 + p8 + p9) / 9.0f;
            vx = 1e-7f + sqrf(p1 - mu) + sqrf(p2 - mu) + sqrf(p3 - mu) + sqrf(p4 - mu) + sqrf(p5 - mu) + sqrf(p6 - mu) + sqrf(p7 - mu) + sqrf(p8 - mu) + sqrf(p9 - mu);
            p1 -= rix[1][-w4];
            p2 -= rix[1][-w3];
            p3 -= rix[1][-w2];
            p4 -= rix[1][-w1];
            p5 -= rix[1][  0];
            p6 -= rix[1][ w1];
            p7 -= rix[1][ w2];
            p8 -= rix[1][ w3];
            p9 -= rix[1][ w4];
            vn = 1e-7f + sqrf(p1) + sqrf(p2) + sqrf(p3) + sqrf(p4) + sqrf(p5) + sqrf(p6) + sqrf(p7) + sqrf(p8) + sqrf(p9);
            float xv = (rix[1][0] * vx + rix[3][0] * vn) / (vx + vn);
            float vv = vx * vn / (vx + vn);
            // interpolated G-R(B)
            interp[0] = (xh * vv + xv * vh) / (vh + vv);
          }
        }

        // copy CFA values
        for(int rr = 0; rr < last_rr; rr++)
        {
          for(int cc = 0; cc < last_cc; cc++)
          {
            const int row_in = rr - BORDER_AROUND + rowStart;
            const int col_in = cc - BORDER_AROUND + colStart;

            const int c = FC(rr, cc, filters);
            rix[c] = qix[c] + rr * LMMSE_GRP + cc;
            rix[c][0] = ((row_in >= 0) && (row_in < height) && (col_in >= 0) && (col_in < width)) ? qix[5][rr * LMMSE_GRP + cc] : 0.0f;
    
            if(c != 1)
            {
              rix[1] = qix[1] + rr * LMMSE_GRP + cc;
              float *interp = qix[4] + rr * LMMSE_GRP + cc;
              rix[1][0] = rix[c][0] + interp[0];
            }
          }
        }

        // bilinear interpolation for R/B
        // interpolate R/B at G location
        for(int rr = 1; rr < last_rr - 1; rr++)
        {
          for(int cc = 1 + (FC(rr, 2, filters) & 1), c = FC(rr, cc + 1, filters); cc < last_cc - 1; cc += 2)
          {
            rix[c] = qix[c] + rr * LMMSE_GRP + cc;
            rix[1] = qix[1] + rr * LMMSE_GRP + cc;
            rix[c][0] = rix[1][0] + 0.5f * (rix[c][ -1] - rix[1][ -1] + rix[c][ 1] - rix[1][ 1]);
            c = 2 - c;
            rix[c] = qix[c] + rr * LMMSE_GRP + cc;
            rix[c][0] = rix[1][0] + 0.5f * (rix[c][-w1] - rix[1][-w1] + rix[c][w1] - rix[1][w1]);
            c = 2 - c;
          }
        }
    
        // interpolate R/B at B/R location
        for(int rr = 1; rr < last_rr - 1; rr++)
        {
          for(int cc = 1 + (FC(rr, 1, filters) & 1), c = 2 - FC(rr, cc, filters); cc < last_cc - 1; cc += 2)
          {
            rix[c] = qix[c] + rr * LMMSE_GRP + cc;
            rix[1] = qix[1] + rr * LMMSE_GRP + cc;
            rix[c][0] = rix[1][0] + 0.25f * (rix[c][-w1] - rix[1][-w1] + rix[c][ -1] - rix[1][ -1] + rix[c][  1] - rix[1][  1] + rix[c][ w1] - rix[1][ w1]);
          }
        }

        // median filter/
        for(int pass = 0; pass < medians; pass++)
        {
          // Apply 3x3 median filter
          // Compute median(R-G) and median(B-G)
          for(int rr = 1; rr < last_rr - 1; rr++)
          {
            for(int c = 0; c < 3; c += 2)
            {
              const int d = c + 3 - (c == 0 ? 0 : 1);
              for(int cc = 1; cc < last_cc - 1; cc++)
              {
                rix[d] = qix[d] + rr * LMMSE_GRP + cc;
                rix[c] = qix[c] + rr * LMMSE_GRP + cc;
                rix[1] = qix[1] + rr * LMMSE_GRP + cc;
                // Assign 3x3 differential color values
                rix[d][0] = median9f(rix[c][-w1-1] - rix[1][-w1-1],
                                     rix[c][-w1  ] - rix[1][-w1  ],
                                     rix[c][-w1+1] - rix[1][-w1+1],
                                     rix[c][   -1] - rix[1][   -1],
                                     rix[c][    0] - rix[1][    0],
                                     rix[c][    1] - rix[1][    1],
                                     rix[c][ w1-1] - rix[1][ w1-1],
                                     rix[c][ w1  ] - rix[1][ w1  ],
                                     rix[c][ w1+1] - rix[1][ w1+1]);
              }
            }
          }

          // red/blue at GREEN pixel locations & red/blue and green at BLUE/RED pixel locations
          for(int rr = 0; rr < last_rr - 1; rr++)
          {
            rix[0] = qix[0] + rr * LMMSE_GRP;
            rix[1] = qix[1] + rr * LMMSE_GRP;
            rix[2] = qix[2] + rr * LMMSE_GRP;
            rix[3] = qix[3] + rr * LMMSE_GRP;
            rix[4] = qix[4] + rr * LMMSE_GRP;
            int c0 = FC(rr, 0, filters);
            int c1 = FC(rr, 1, filters);
      
            if(c0 == 1)
            {
              c1 = 2 - c1;
              const int d = c1 + 3 - (c1 == 0 ? 0 : 1);
              int cc;
              for(cc = 0; cc < last_cc - 1; cc += 2)
              {
                rix[0][0] = rix[1][0] + rix[3][0];
                rix[2][0] = rix[1][0] + rix[4][0];
                rix[0]++;
                rix[1]++;
                rix[2]++;
                rix[3]++;
                rix[4]++;
                rix[c1][0] = rix[1][0] + rix[d][0];
                rix[1][0] = 0.5f * (rix[0][0] - rix[3][0] + rix[2][0] - rix[4][0]);
                rix[0]++;
                rix[1]++;
                rix[2]++;
                rix[3]++;
                rix[4]++;
              }
      
              if(cc < last_cc)
              { // remaining pixel, only if width is odd
                rix[0][0] = rix[1][0] + rix[3][0];
                rix[2][0] = rix[1][0] + rix[4][0];
              }
            }
            else
            {
              c0 = 2 - c0;
              const int d = c0 + 3 - (c0 == 0 ? 0 : 1);
              int cc;
              for(cc = 0; cc < last_cc - 1; cc += 2)
              {
                rix[c0][0] = rix[1][0] + rix[d][0];
                rix[1][0] = 0.5f * (rix[0][0] - rix[3][0] + rix[2][0] - rix[4][0]);
                rix[0]++;
                rix[1]++;
                rix[2]++;
                rix[3]++;
                rix[4]++;
                rix[0][0] = rix[1][0] + rix[3][0];
                rix[2][0] = rix[1][0] + rix[4][0];
                rix[0]++;
                rix[1]++;
                rix[2]++;
                rix[3]++;
                rix[4]++;
              }
      
              if(cc < last_cc)
              { // remaining pixel, only if width is odd
                rix[c0][0] = rix[1][0] + rix[d][0];
                rix[1][0] = 0.5f * (rix[0][0] - rix[3][0] + rix[2][0] - rix[4][0]);
              }
            }
          }
        }

        // write result to out
        // For the outermost tiles in all directions we also write the otherwise overlapped area
        const int first_vertical =   rowStart + ((tile_vertical == 0)                    ? 0 : LMMSE_OVERLAP);
        const int last_vertical =    rowEnd   - ((tile_vertical == num_vertical - 1)     ? 0 : LMMSE_OVERLAP);
        const int first_horizontal = colStart + ((tile_horizontal == 0)                  ? 0 : LMMSE_OVERLAP);
        const int last_horizontal =  colEnd   - ((tile_horizontal == num_horizontal - 1) ? 0 : LMMSE_OVERLAP);
        for(int row = first_vertical; row < last_vertical; row++)
        {
          for(int col = first_horizontal; col < last_horizontal; col++)
          {

            const int rr = row - rowStart + BORDER_AROUND;
            const int cc = col - colStart + BORDER_AROUND;
            const int c = FC(row, col, filters);
            const int oidx = 4 * (row * width + col);

            for(int i = 0; i < 3; i++)
            {
              const float val = (i == c) ? in[row * width + col] : scaler * calc_gamma(qix[i][rr * LMMSE_GRP + cc], gamma_out); 
              out[oidx + i] = fmaxf(0.0f, val);
            }
            out[oidx + 3] = 0.0f;
          }
        }
      }
    }
    dt_free_align(buffer);
  }
  if(refine)
  {
    refinement(width, height, out, filters, refine);
  }
}

// revert specific aggressive optimizing
#ifdef __GNUC__
  #pragma GCC pop_options
#endif

#undef LMMSE_TILESIZE
#undef LMMSE_OVERLAP
#undef BORDER_AROUND
#undef LMMSE_TILEVALID
#undef LMMSE_GRP
#undef w1
#undef w2
#undef w3
#undef w4

