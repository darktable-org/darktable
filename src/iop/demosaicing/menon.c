/*
    This file is part of darktable,
    Copyright (C) 2024-2025 darktable developers.

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

    Menon (2007) DDFAPD demosaicing algorithm.
    Ported from the colour-demosaicing Python library (BSD-3-Clause).
    Reference: D. Menon, S. Andriani and G. Calvagno,
    "Demosaicing With Directional Filtering and a posteriori Decision",
    IEEE Trans. Image Processing, vol. 16, no. 1, pp. 132-141, Jan. 2007.
*/

#define MENON_BORDER 6

/* Helper: mirror-boundary 1D horizontal convolution with a 5-tap kernel.
   kernel[] is indexed [-2..+2] stored as kernel[0..4]. */
static inline float _menon_cnv_h5(const float *const restrict buf,
                                  const int row, const int col,
                                  const int width, const int height,
                                  const float *const restrict kernel)
{
  float sum = 0.0f;
  for(int k = -2; k <= 2; k++)
  {
    int c = col + k;
    if(c < 0) c = -c;
    if(c >= width) c = 2 * (width - 1) - c;
    sum += buf[(size_t)row * width + c] * kernel[k + 2];
  }
  return sum;
}

/* Helper: mirror-boundary 1D vertical convolution with a 5-tap kernel. */
static inline float _menon_cnv_v5(const float *const restrict buf,
                                  const int row, const int col,
                                  const int width, const int height,
                                  const float *const restrict kernel)
{
  float sum = 0.0f;
  for(int k = -2; k <= 2; k++)
  {
    int r = row + k;
    if(r < 0) r = -r;
    if(r >= height) r = 2 * (height - 1) - r;
    sum += buf[(size_t)r * width + col] * kernel[k + 2];
  }
  return sum;
}

/* Helper: mirror-boundary 1D horizontal convolution with a 3-tap kernel.
   kernel[] is indexed [-1..+1] stored as kernel[0..2]. */
static inline float _menon_cnv_h3(const float *const restrict buf,
                                  const int row, const int col,
                                  const int width, const int height,
                                  const float *const restrict kernel)
{
  float sum = 0.0f;
  for(int k = -1; k <= 1; k++)
  {
    int c = col + k;
    if(c < 0) c = -c;
    if(c >= width) c = 2 * (width - 1) - c;
    sum += buf[(size_t)row * width + c] * kernel[k + 1];
  }
  return sum;
}

/* Helper: mirror-boundary 1D vertical convolution with a 3-tap kernel. */
static inline float _menon_cnv_v3(const float *const restrict buf,
                                  const int row, const int col,
                                  const int width, const int height,
                                  const float *const restrict kernel)
{
  float sum = 0.0f;
  for(int k = -1; k <= 1; k++)
  {
    int r = row + k;
    if(r < 0) r = -r;
    if(r >= height) r = 2 * (height - 1) - r;
    sum += buf[(size_t)r * width + col] * kernel[k + 1];
  }
  return sum;
}

/* Helper: 5x5 2D convolution with constant (zero-pad) boundary for the classifier kernel. */
static inline float _menon_cnv_2d(const float *const restrict buf,
                                  const int row, const int col,
                                  const int width, const int height,
                                  const float kernel[5][5])
{
  float sum = 0.0f;
  for(int dr = -2; dr <= 2; dr++)
  {
    const int r = row + dr;
    if(r < 0 || r >= height) continue;
    for(int dc = -2; dc <= 2; dc++)
    {
      const int c = col + dc;
      if(c < 0 || c >= width) continue;
      sum += buf[(size_t)r * width + c] * kernel[dr + 2][dc + 2];
    }
  }
  return sum;
}

DT_OMP_DECLARE_SIMD(aligned(in, out : 64))
static void menon_demosaic(float *const restrict out,
                           const float *const restrict in,
                           const int width,
                           const int height,
                           const uint32_t filters)
{
  const size_t numpix = (size_t)width * height;

  /* ---- Allocate working buffers ---- */
  float *const restrict CFA = dt_alloc_align_float(numpix);
  float *const restrict R   = dt_alloc_align_float(numpix);
  float *const restrict G   = dt_alloc_align_float(numpix);
  float *const restrict B   = dt_alloc_align_float(numpix);
  float *const restrict G_H = dt_alloc_align_float(numpix);
  float *const restrict G_V = dt_alloc_align_float(numpix);
  float *const restrict C_H = dt_alloc_align_float(numpix);
  float *const restrict C_V = dt_alloc_align_float(numpix);
  float *const restrict D_H = dt_alloc_align_float(numpix);
  float *const restrict D_V = dt_alloc_align_float(numpix);
  uint8_t *const restrict M = dt_alloc_aligned(numpix); // direction mask: 1=horizontal, 0=vertical

  if(!CFA || !R || !G || !B || !G_H || !G_V || !C_H || !C_V || !D_H || !D_V || !M)
  {
    dt_free_align(CFA); dt_free_align(R); dt_free_align(G); dt_free_align(B);
    dt_free_align(G_H); dt_free_align(G_V);
    dt_free_align(C_H); dt_free_align(C_V);
    dt_free_align(D_H); dt_free_align(D_V);
    dt_free_align(M);
    // fallback: zero output
    memset(out, 0, sizeof(float) * numpix * 4);
    return;
  }

  /* ---- Filter kernels ---- */
  const float h_0[5] = { 0.0f, 0.5f, 0.0f, 0.5f, 0.0f };
  const float h_1[5] = { -0.25f, 0.0f, 0.5f, 0.0f, -0.25f };
  const float k_b[3] = { 0.5f, 0.0f, 0.5f };
  const float FIR[3] = { 1.0f/3.0f, 1.0f/3.0f, 1.0f/3.0f };

  /* Directional classifier kernel (5x5) for horizontal.
     scipy.ndimage.convolve flips the kernel (true convolution), but our
     _menon_cnv_2d does correlation, so we store the 180-degree-rotated
     version of the original kernel k from the paper / reference code. */
  const float clf_h[5][5] = {
    { 1, 0, 1, 0, 0 },
    { 0, 1, 0, 0, 0 },
    { 3, 0, 3, 0, 0 },
    { 0, 1, 0, 0, 0 },
    { 1, 0, 1, 0, 0 }
  };
  /* Transposed kernel for vertical (also rotated 180) */
  const float clf_v[5][5] = {
    { 1, 0, 3, 0, 1 },
    { 0, 1, 0, 1, 0 },
    { 1, 0, 3, 0, 1 },
    { 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0 }
  };

  /* ---- Step 1: Extract CFA and initial R, G, B channels ---- */
  DT_OMP_PRAGMA(parallel for schedule(static) default(none)
    dt_omp_firstprivate(in, CFA, R, G, B, width, height, filters, numpix))
  for(size_t idx = 0; idx < numpix; idx++)
  {
    const int row = idx / width;
    const int col = idx % width;
    const float val = in[idx]; // raw data is 1 channel per pixel for Bayer
    CFA[idx] = val;
    const int fc = FC(row, col, filters);
    R[idx] = (fc == 0) ? val : 0.0f;
    G[idx] = (fc == 1) ? val : 0.0f;
    B[idx] = (fc == 2) ? val : 0.0f;
  }

  /* ---- Step 2: Tentative horizontal and vertical green estimates ---- */
  DT_OMP_PRAGMA(parallel for schedule(static) default(none)
    dt_omp_firstprivate(CFA, G, G_H, G_V, width, height, filters, h_0, h_1, numpix))
  for(size_t idx = 0; idx < numpix; idx++)
  {
    const int row = idx / width;
    const int col = idx % width;
    const int fc = FC(row, col, filters);
    if(fc == 1)
    {
      // Green pixel: keep original
      G_H[idx] = G[idx];
      G_V[idx] = G[idx];
    }
    else
    {
      // Non-green pixel: interpolate green directionally
      G_H[idx] = _menon_cnv_h5(CFA, row, col, width, height, h_0)
               + _menon_cnv_h5(CFA, row, col, width, height, h_1);
      G_V[idx] = _menon_cnv_v5(CFA, row, col, width, height, h_0)
               + _menon_cnv_v5(CFA, row, col, width, height, h_1);
    }
  }

  /* ---- Step 3: Compute color differences for classification ---- */
  DT_OMP_PRAGMA(parallel for schedule(static) default(none)
    dt_omp_firstprivate(R, B, G_H, G_V, C_H, C_V, width, height, filters, numpix))
  for(size_t idx = 0; idx < numpix; idx++)
  {
    const int row = idx / width;
    const int col = idx % width;
    const int fc = FC(row, col, filters);
    if(fc == 0) // Red pixel
    {
      C_H[idx] = R[idx] - G_H[idx];
      C_V[idx] = R[idx] - G_V[idx];
    }
    else if(fc == 2) // Blue pixel
    {
      C_H[idx] = B[idx] - G_H[idx];
      C_V[idx] = B[idx] - G_V[idx];
    }
    else
    {
      C_H[idx] = 0.0f;
      C_V[idx] = 0.0f;
    }
  }

  /* ---- Step 4: Compute directional gradients D_H, D_V ---- */
  /* D_H[row][col] = |C_H[row][col] - C_H[row][col+2]|  (forward shift by 2)
     D_V[row][col] = |C_V[row][col] - C_V[row+2][col]|  (forward shift by 2)
     Boundary: reflect (numpy pad mode='reflect') */
  DT_OMP_PRAGMA(parallel for schedule(static) default(none)
    dt_omp_firstprivate(C_H, C_V, D_H, D_V, width, height, numpix))
  for(size_t idx = 0; idx < numpix; idx++)
  {
    const int row = idx / width;
    const int col = idx % width;

    // Horizontal gradient with reflect padding
    {
      int c2 = col + 2;
      if(c2 >= width) c2 = 2 * (width - 1) - c2; // reflect
      D_H[idx] = fabsf(C_H[idx] - C_H[(size_t)row * width + c2]);
    }
    // Vertical gradient with reflect padding
    {
      int r2 = row + 2;
      if(r2 >= height) r2 = 2 * (height - 1) - r2; // reflect
      D_V[idx] = fabsf(C_V[idx] - C_V[(size_t)r2 * width + col]);
    }
  }

  /* ---- Step 5: Classify direction using 5x5 weighted sum ---- */
  /* d_H = convolve2d(D_H, clf_h), d_V = convolve2d(D_V, clf_v) */
  /* Step 6: Choose direction: if d_V >= d_H => horizontal (M=1), else vertical (M=0) */
  DT_OMP_PRAGMA(parallel for schedule(static) default(none)
    dt_omp_firstprivate(D_H, D_V, G_H, G_V, G, M, width, height, clf_h, clf_v, numpix))
  for(size_t idx = 0; idx < numpix; idx++)
  {
    const int row = idx / width;
    const int col = idx % width;
    const float d_h = _menon_cnv_2d(D_H, row, col, width, height, clf_h);
    const float d_v = _menon_cnv_2d(D_V, row, col, width, height, clf_v);
    if(d_v >= d_h)
    {
      M[idx] = 1; // choose horizontal
      G[idx] = G_H[idx];
    }
    else
    {
      M[idx] = 0; // choose vertical
      G[idx] = G_V[idx];
    }
  }

  /* ---- Step 7: Red/Blue channel interpolation ---- */
  /* We need row masks: R_r (row contains red), B_r (row contains blue).
     For standard RGGB: row 0 has R (even rows), row 1 has B (odd rows).
     We use FC to determine this per-row. */

  /* 7a: At green pixels on red rows, interpolate R horizontally */
  /* 7b: At green pixels on blue rows, interpolate R vertically */
  /* 7c: At green pixels on blue rows, interpolate B horizontally */
  /* 7d: At green pixels on red rows, interpolate B vertically */
  DT_OMP_PRAGMA(parallel for schedule(static) default(none)
    dt_omp_firstprivate(R, G, B, width, height, filters, k_b, numpix))
  for(size_t idx = 0; idx < numpix; idx++)
  {
    const int row = idx / width;
    const int col = idx % width;
    const int fc = FC(row, col, filters);

    if(fc != 1) continue; // only process green pixels

    // Determine if this row is a "red row" or "blue row"
    // A "red row" has red pixels on it; a "blue row" has blue pixels
    const gboolean red_row = (FC(row, 0, filters) == 0) || (FC(row, 1, filters) == 0);

    if(red_row)
    {
      // R at green pixel on red row: interpolate horizontally
      R[idx] = G[idx]
             + _menon_cnv_h3(R, row, col, width, height, k_b)
             - _menon_cnv_h3(G, row, col, width, height, k_b);
      // B at green pixel on red row: interpolate vertically
      B[idx] = G[idx]
             + _menon_cnv_v3(B, row, col, width, height, k_b)
             - _menon_cnv_v3(G, row, col, width, height, k_b);
    }
    else // blue_row
    {
      // R at green pixel on blue row: interpolate vertically
      R[idx] = G[idx]
             + _menon_cnv_v3(R, row, col, width, height, k_b)
             - _menon_cnv_v3(G, row, col, width, height, k_b);
      // B at green pixel on blue row: interpolate horizontally
      B[idx] = G[idx]
             + _menon_cnv_h3(B, row, col, width, height, k_b)
             - _menon_cnv_h3(G, row, col, width, height, k_b);
    }
  }

  /* 7e: At blue pixels, interpolate R using direction mask M
     7f: At red pixels, interpolate B using direction mask M */
  DT_OMP_PRAGMA(parallel for schedule(static) default(none)
    dt_omp_firstprivate(R, G, B, M, width, height, filters, k_b, numpix))
  for(size_t idx = 0; idx < numpix; idx++)
  {
    const int row = idx / width;
    const int col = idx % width;
    const int fc = FC(row, col, filters);

    if(fc == 2) // Blue pixel: interpolate R
    {
      if(M[idx])
        R[idx] = B[idx]
               + _menon_cnv_h3(R, row, col, width, height, k_b)
               - _menon_cnv_h3(B, row, col, width, height, k_b);
      else
        R[idx] = B[idx]
               + _menon_cnv_v3(R, row, col, width, height, k_b)
               - _menon_cnv_v3(B, row, col, width, height, k_b);
    }
    else if(fc == 0) // Red pixel: interpolate B
    {
      if(M[idx])
        B[idx] = R[idx]
               + _menon_cnv_h3(B, row, col, width, height, k_b)
               - _menon_cnv_h3(R, row, col, width, height, k_b);
      else
        B[idx] = R[idx]
               + _menon_cnv_v3(B, row, col, width, height, k_b)
               - _menon_cnv_v3(R, row, col, width, height, k_b);
    }
  }

  /* ---- Step 8: Refinement ---- */

  /* 8a: Update green at R/B locations using FIR filter on color differences */
  {
    /* Recompute R-G and B-G into temporary buffers (reuse C_H for R_G, C_V for B_G) */
    float *const restrict R_G = C_H;
    float *const restrict B_G = C_V;

    DT_OMP_PRAGMA(parallel for schedule(static) default(none)
      dt_omp_firstprivate(R, G, B, R_G, B_G, numpix))
    for(size_t idx = 0; idx < numpix; idx++)
    {
      R_G[idx] = R[idx] - G[idx];
      B_G[idx] = B[idx] - G[idx];
    }

    /* At R pixels: G = R - directional_FIR(R_G)
       At B pixels: G = B - directional_FIR(B_G) */
    DT_OMP_PRAGMA(parallel for schedule(static) default(none)
      dt_omp_firstprivate(R, G, B, R_G, B_G, M, width, height, filters, FIR, numpix))
    for(size_t idx = 0; idx < numpix; idx++)
    {
      const int row = idx / width;
      const int col = idx % width;
      const int fc = FC(row, col, filters);
      if(fc == 0) // Red pixel
      {
        const float r_g_m = M[idx]
          ? _menon_cnv_h3(R_G, row, col, width, height, FIR)
          : _menon_cnv_v3(R_G, row, col, width, height, FIR);
        G[idx] = R[idx] - r_g_m;
      }
      else if(fc == 2) // Blue pixel
      {
        const float b_g_m = M[idx]
          ? _menon_cnv_h3(B_G, row, col, width, height, FIR)
          : _menon_cnv_v3(B_G, row, col, width, height, FIR);
        G[idx] = B[idx] - b_g_m;
      }
    }

    /* Recompute R_G after green update */
    DT_OMP_PRAGMA(parallel for schedule(static) default(none)
      dt_omp_firstprivate(R, G, R_G, numpix))
    for(size_t idx = 0; idx < numpix; idx++)
      R_G[idx] = R[idx] - G[idx];

    /* Recompute B_G after green update */
    DT_OMP_PRAGMA(parallel for schedule(static) default(none)
      dt_omp_firstprivate(B, G, B_G, numpix))
    for(size_t idx = 0; idx < numpix; idx++)
      B_G[idx] = B[idx] - G[idx];

    /* 8b: Update R at green pixels on blue rows (vertical averaging of R-G)
           Update R at green pixels on blue columns (horizontal averaging of R-G)
           Update B at green pixels on red rows (vertical averaging of B-G)
           Update B at green pixels on red columns (horizontal averaging of B-G) */
    DT_OMP_PRAGMA(parallel for schedule(static) default(none)
      dt_omp_firstprivate(R, G, B, R_G, B_G, width, height, filters, k_b, numpix))
    for(size_t idx = 0; idx < numpix; idx++)
    {
      const int row = idx / width;
      const int col = idx % width;
      const int fc = FC(row, col, filters);
      if(fc != 1) continue; // only green pixels

      const gboolean red_row = (FC(row, 0, filters) == 0) || (FC(row, 1, filters) == 0);
      const gboolean red_col = (FC(0, col, filters) == 0) || (FC(1, col, filters) == 0);

      if(!red_row) // blue row: update R vertically
        R[idx] = G[idx] + _menon_cnv_v3(R_G, row, col, width, height, k_b);
      if(!red_col) // blue column: update R horizontally
        R[idx] = G[idx] + _menon_cnv_h3(R_G, row, col, width, height, k_b);

      if(red_row) // red row: update B vertically
        B[idx] = G[idx] + _menon_cnv_v3(B_G, row, col, width, height, k_b);
      if(red_col) // red column: update B horizontally
        B[idx] = G[idx] + _menon_cnv_h3(B_G, row, col, width, height, k_b);
    }

    /* 8c: Update R at blue pixels and B at red pixels using R-B differences */
    /* Compute R-B into D_H (reuse buffer) */
    float *const restrict R_B = D_H;
    DT_OMP_PRAGMA(parallel for schedule(static) default(none)
      dt_omp_firstprivate(R, B, R_B, numpix))
    for(size_t idx = 0; idx < numpix; idx++)
      R_B[idx] = R[idx] - B[idx];

    DT_OMP_PRAGMA(parallel for schedule(static) default(none)
      dt_omp_firstprivate(R, B, R_B, M, width, height, filters, FIR, numpix))
    for(size_t idx = 0; idx < numpix; idx++)
    {
      const int row = idx / width;
      const int col = idx % width;
      const int fc = FC(row, col, filters);
      if(fc == 2) // Blue pixel: update R from R-B
      {
        const float r_b_m = M[idx]
          ? _menon_cnv_h3(R_B, row, col, width, height, FIR)
          : _menon_cnv_v3(R_B, row, col, width, height, FIR);
        R[idx] = B[idx] + r_b_m;
      }
      else if(fc == 0) // Red pixel: update B from R-B
      {
        const float r_b_m = M[idx]
          ? _menon_cnv_h3(R_B, row, col, width, height, FIR)
          : _menon_cnv_v3(R_B, row, col, width, height, FIR);
        B[idx] = R[idx] - r_b_m;
      }
    }
  }

  /* ---- Step 9: Write output in planar RGBX format ---- */
  DT_OMP_PRAGMA(parallel for schedule(static) default(none)
    dt_omp_firstprivate(out, R, G, B, numpix))
  for(size_t idx = 0; idx < numpix; idx++)
  {
    out[idx * 4 + 0] = R[idx];
    out[idx * 4 + 1] = G[idx];
    out[idx * 4 + 2] = B[idx];
    out[idx * 4 + 3] = 0.0f;
  }

  /* ---- Border handling: simple bilinear for border pixels ---- */
  for(int row = 0; row < height; row++)
    for(int col = 0; col < width; col++)
    {
      if(row == MENON_BORDER && col >= MENON_BORDER && col < width - MENON_BORDER)
      {
        col = width - MENON_BORDER - 1;
        continue;
      }
      if(row >= MENON_BORDER && row < height - MENON_BORDER
         && col == MENON_BORDER)
      {
        col = width - MENON_BORDER - 1;
        continue;
      }

      float sum[8] = { 0.0f };
      for(int y = row - 1; y <= row + 1; y++)
        for(int x = col - 1; x <= col + 1; x++)
        {
          if(y >= 0 && x >= 0 && y < height && x < width)
          {
            const int f = FC(y, x, filters);
            sum[f] += in[(size_t)y * width + x];
            sum[f + 4]++;
          }
        }
      const int f = FC(row, col, filters);
      const size_t oidx = ((size_t)row * width + col) * 4;
      for_each_channel(c)
      {
        if(c == (size_t)f)
          out[oidx + c] = in[(size_t)row * width + col];
        else
          out[oidx + c] = sum[c] / MAX(1.0f, sum[c + 4]);
      }
      out[oidx + 3] = 0.0f;
    }

  /* ---- Cleanup ---- */
  dt_free_align(CFA);
  dt_free_align(R);
  dt_free_align(G);
  dt_free_align(B);
  dt_free_align(G_H);
  dt_free_align(G_V);
  dt_free_align(C_H);
  dt_free_align(C_V);
  dt_free_align(D_H);
  dt_free_align(D_V);
  dt_free_align(M);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
