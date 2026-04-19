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

    Adaptive Residual Interpolation (ARI) demosaicing, following:
      Y. Monno, D. Kiku, M. Tanaka, M. Okutomi,
      "Adaptive Residual Interpolation for Color and Multispectral Image
      Demosaicking", IEEE ICIP 2015 (and the extended Sensors 2017 paper).
    The implementation is a direct C port of the authors' MATLAB reference
    code (ok.sc.e.titech.ac.jp/res/DM/ARI.zip), keeping the same 11-iteration
    per-pixel argmin selection and the error-weighted a/b smoothing inside
    the guided filters that is the paper's key contribution.
*/

#define ARI_K_MAX 11   /* paper default iteration count */
#define ARI_EPS   1e-10f

/* ---------- rectangular box filter via 2-pass separable rolling sum ----------
 * Replaces integral-image approach for better cache behavior on large images.
 *
 * Pass 1 horizontal: for each row, sliding-window sum over [x-rh, x+rh] with
 *                    replicate-boundary. Output to `tmp` (float).
 * Pass 2 vertical:   for each column, sliding sum over [y-rv, y+rv] reading
 *                    tmp and writing dst (float). Column strides are handled
 *                    per-column; the acc is a double register for precision.
 *
 * Memory BW per call: src read + tmp write + tmp read + dst write
 *                   ~ 4 passes × npix × 4 B (vs integral's ~15 passes × 8 B).
 * Working set per tile naturally fits L2/L3 since we only need 2 float buffers.
 *
 * `integral` is the scratch buffer previously used for the double integral
 * image; we reinterpret it as a float temp big enough for npix floats
 * (it's allocated as (W+1)*(H+1) doubles, i.e. >> W*H floats). */
static void _ari_box_sum_rect(const float *const restrict src,
                              float *const restrict dst,
                              const int width,
                              const int height,
                              const int rh,
                              const int rv,
                              double *const restrict integral)
{
  float *const restrict tmp = (float *)integral;  /* enough space */
  const int W1 = width - 1;
  const int H1 = height - 1;

  /* Pass 1: horizontal rolling sum, src -> tmp. */
  DT_OMP_FOR()
  for(int y = 0; y < height; y++)
  {
    const float *const restrict sr = &src[(size_t)y * width];
    float *const restrict dr = &tmp[(size_t)y * width];
    double acc = 0.0;
    for(int k = -rh; k <= rh; k++)
    {
      const int xx = k < 0 ? 0 : (k > W1 ? W1 : k);
      acc += (double)sr[xx];
    }
    dr[0] = (float)acc;
    for(int x = 1; x < width; x++)
    {
      const int drop = x - 1 - rh;
      const int add  = x + rh;
      const float v_drop = sr[drop < 0 ? 0 : drop];
      const float v_add  = sr[add > W1 ? W1 : add];
      acc += (double)v_add - (double)v_drop;
      dr[x] = (float)acc;
    }
  }

  /* Pass 2: vertical rolling sum, tmp -> dst.
   * Cache-friendly variant: within each thread's column strip, process
   * row-by-row (row-major inner loop) so we read/write contiguous memory.
   * Previous column-major variant had 21KB stride per element and paid full
   * cache-line cost per read. Thread-local `acc` array carries rolling state
   * across rows for each of the thread's assigned columns. */
  #pragma omp parallel
  {
    const int tid = omp_get_thread_num();
    const int nt  = omp_get_num_threads();
    const int x_start = (int)(((long long)width * tid) / nt);
    const int x_end   = (int)(((long long)width * (tid + 1)) / nt);
    const int ncols   = x_end - x_start;

    if(ncols > 0)
    {
      double *const acc = (double *)malloc((size_t)ncols * sizeof(double));
      if(acc)
      {
        for(int i = 0; i < ncols; i++) acc[i] = 0.0;

        /* Initial row sum = sum over k=-rv..rv of tmp[clamp(k,0,H1), x]. */
        for(int k = -rv; k <= rv; k++)
        {
          const int yy = k < 0 ? 0 : (k > H1 ? H1 : k);
          const float *const restrict t_row = &tmp[(size_t)yy * width];
          for(int x = x_start; x < x_end; x++)
            acc[x - x_start] += (double)t_row[x];
        }
        {
          float *const restrict d_row = &dst[0];
          for(int x = x_start; x < x_end; x++)
            d_row[x] = (float)acc[x - x_start];
        }

        /* Rolling slide for rows y=1..H-1. */
        for(int y = 1; y < height; y++)
        {
          const int drop = y - 1 - rv;
          const int add  = y + rv;
          const int drop_y = drop < 0 ? 0 : drop;
          const int add_y  = add  > H1 ? H1 : add;
          const float *const restrict t_add  = &tmp[(size_t)add_y  * width];
          const float *const restrict t_drop = &tmp[(size_t)drop_y * width];
          float       *const restrict d_row  = &dst[(size_t)y * width];
          for(int x = x_start; x < x_end; x++)
          {
            const double a = acc[x - x_start] + (double)t_add[x] - (double)t_drop[x];
            acc[x - x_start] = a;
            d_row[x] = (float)a;
          }
        }
        free(acc);
      }
    }
  }
}

/* ---------- utility: apply 1D horizontal kernel ---------- */
static void _ari_imfilter_h1d(const float *const restrict src,
                              float *const restrict dst,
                              const int width,
                              const int height,
                              const float *const restrict kernel,
                              const int taps)
{
  const int half = taps / 2;
  DT_OMP_FOR()
  for(int y = 0; y < height; y++)
    for(int x = 0; x < width; x++)
    {
      float s = 0.0f;
      for(int k = 0; k < taps; k++)
      {
        int xx = x + k - half;
        if(xx < 0) xx = -xx;
        else if(xx >= width) xx = 2 * width - 2 - xx;
        s += kernel[k] * src[(size_t)y * width + xx];
      }
      dst[(size_t)y * width + x] = s;
    }
}

static void _ari_imfilter_v1d(const float *const restrict src,
                              float *const restrict dst,
                              const int width,
                              const int height,
                              const float *const restrict kernel,
                              const int taps)
{
  const int half = taps / 2;
  DT_OMP_FOR()
  for(int y = 0; y < height; y++)
  {
    for(int x = 0; x < width; x++)
    {
      float s = 0.0f;
      for(int k = 0; k < taps; k++)
      {
        int yy = y + k - half;
        if(yy < 0) yy = -yy;
        else if(yy >= height) yy = 2 * height - 2 - yy;
        s += kernel[k] * src[(size_t)yy * width + x];
      }
      dst[(size_t)y * width + x] = s;
    }
  }
}

/* ---------- 5x5 cross Laplacian (red/blue interpolation) ---------- */
static void _ari_laplacian_5x5_cross(const float *const restrict src,
                                     float *const restrict dst,
                                     const int width,
                                     const int height)
{
  DT_OMP_FOR()
  for(int y = 0; y < height; y++)
    for(int x = 0; x < width; x++)
    {
      const int xl = (x >= 2) ? x - 2 : x + 2;
      const int xr = (x < width - 2) ? x + 2 : x - 2;
      const int yu = (y >= 2) ? y - 2 : y + 2;
      const int yd = (y < height - 2) ? y + 2 : y - 2;
      dst[(size_t)y * width + x] =
          4.0f * src[(size_t)y * width + x]
        - src[(size_t)y * width + xl]
        - src[(size_t)y * width + xr]
        - src[(size_t)yu * width + x]
        - src[(size_t)yd * width + x];
    }
}

/* ---------- 2D convolution with an arbitrary kernel (mirror boundary) ---------- */
__attribute__((unused)) static void _ari_conv2d(const float *const restrict src,
                        float *const restrict dst,
                        const int width,
                        const int height,
                        const float *const restrict kernel,
                        const int kh,
                        const int kw)
{
  const int half_h = kh / 2;
  const int half_w = kw / 2;
  DT_OMP_FOR()
  for(int y = 0; y < height; y++)
    for(int x = 0; x < width; x++)
    {
      float s = 0.0f;
      for(int ky = 0; ky < kh; ky++)
      {
        int yy = y + ky - half_h;
        if(yy < 0) yy = -yy;
        else if(yy >= height) yy = 2 * height - 2 - yy;
        for(int kx = 0; kx < kw; kx++)
        {
          int xx = x + kx - half_w;
          if(xx < 0) xx = -xx;
          else if(xx >= width) xx = 2 * width - 2 - xx;
          s += kernel[ky * kw + kx] * src[(size_t)yy * width + xx];
        }
      }
      dst[(size_t)y * width + x] = s;
    }
}

/* ---------- ARI's guided filter (Eq. 2) =====================================
 * Port of ARI.zip/guidedfilter.m. Signature mirrors the MATLAB one:
 *   guidedfilter(I, p, M, rh, rv, eps)
 * with I = guide, p = input, M = sample-validity mask for the regression,
 * and (rh, rv) the rectangular radius of the support window.
 * Differences vs the textbook He et al. guided filter:
 *   - masked statistics: mean_I = box(I*M)/N where N = box(M);
 *   - output-phase a,b smoothing is weighted by the per-pixel inverse
 *     regression residual (the `dif` block below), which is what the
 *     paper calls "weighted averaging" and is the crucial piece for
 *     reaching the paper's CPSNR numbers.
 */
__attribute__((unused)) static void _ari_guidedfilter(const float *const restrict gI,
                              const float *const restrict p,
                              const float *const restrict M,
                              float *const restrict out,
                              const int width,
                              const int height,
                              const int rh,
                              const int rv,
                              const float eps,
                              /* workspace buffers, caller-managed */
                              float *const restrict work0,
                              float *const restrict work1,
                              float *const restrict work2,
                              float *const restrict work3,
                              float *const restrict work4,
                              float *const restrict work5,
                              float *const restrict work6,
                              float *const restrict work7,
                              float *const restrict work8,
                              float *const restrict work9,
                              float *const restrict work10,
                              double *const restrict integral)
{
  const size_t npix = (size_t)width * height;

  /* Cache the pre-division box sums so the dif computation can reuse them
   * instead of boxing the same products a second time. */
  float *N   = work0;   /* box(M) */
  float *sI  = work1;   /* box(I*M), reused as mean_a late */
  float *sp  = work2;   /* box(p*M), reused as mean_b late */
  float *sIp = work3;   /* box(I*p*M) */
  float *sII = work4;   /* box(I*I*M) */
  float *spp = work5;   /* box(p*p*M) */
  float *aa   = work6;
  float *bb   = work7;
  float *dif  = work8;
  float *wdif = work9;
  float *tmp  = work10;

  _ari_box_sum_rect(M, N, width, height, rh, rv, integral);
  DT_OMP_FOR()
  for(size_t i = 0; i < npix; i++) if(N[i] == 0.0f) N[i] = 1.0f;

  DT_OMP_FOR()
  for(size_t i = 0; i < npix; i++) tmp[i] = gI[i] * M[i];
  _ari_box_sum_rect(tmp, sI, width, height, rh, rv, integral);

  DT_OMP_FOR()
  for(size_t i = 0; i < npix; i++) tmp[i] = p[i] * M[i];
  _ari_box_sum_rect(tmp, sp, width, height, rh, rv, integral);

  DT_OMP_FOR()
  for(size_t i = 0; i < npix; i++) tmp[i] = gI[i] * p[i] * M[i];
  _ari_box_sum_rect(tmp, sIp, width, height, rh, rv, integral);

  DT_OMP_FOR()
  for(size_t i = 0; i < npix; i++) tmp[i] = gI[i] * gI[i] * M[i];
  _ari_box_sum_rect(tmp, sII, width, height, rh, rv, integral);

  DT_OMP_FOR()
  for(size_t i = 0; i < npix; i++) tmp[i] = p[i] * p[i] * M[i];
  _ari_box_sum_rect(tmp, spp, width, height, rh, rv, integral);

  /* a, b from cached sums (fused). */
  DT_OMP_FOR()
  for(size_t i = 0; i < npix; i++)
  {
    const float n = N[i];
    const float mI = sI[i] / n;
    const float mp = sp[i] / n;
    const float cov = sIp[i] / n - mI * mp;
    const float var = sII[i] / n - mI * mI;
    aa[i] = cov / (var + eps);
    bb[i] = mp - aa[i] * mI;
  }

  /* dif = E[(aI+b-p)^2 | mask] via cached sums, fused into a single pass. */
  DT_OMP_FOR()
  for(size_t i = 0; i < npix; i++)
  {
    const float a = aa[i], b = bb[i];
    float d = a*a*sII[i] + b*b*N[i] + spp[i]
            + 2.0f*a*b*sI[i] - 2.0f*b*sp[i] - 2.0f*a*sIp[i];
    d /= N[i];
    if(d < 0.0f) d = 0.0f;
    d = sqrtf(d);
    if(d < 1e-3f) d = 1e-3f;
    dif[i] = 1.0f / d;
  }

  _ari_box_sum_rect(dif, wdif, width, height, rh, rv, integral);

  /* mean_a, mean_b via weighted average. Reuse sI/sp buffers. */
  DT_OMP_FOR()
  for(size_t i = 0; i < npix; i++) tmp[i] = aa[i] * dif[i];
  _ari_box_sum_rect(tmp, sI /*= mean_a*/, width, height, rh, rv, integral);
  DT_OMP_FOR()
  for(size_t i = 0; i < npix; i++) tmp[i] = bb[i] * dif[i];
  _ari_box_sum_rect(tmp, sp /*= mean_b*/, width, height, rh, rv, integral);

  DT_OMP_FOR()
  for(size_t i = 0; i < npix; i++)
  {
    const float denom = wdif[i] + 1e-4f;
    const float ma = sI[i] / denom;
    const float mb = sp[i] / denom;
    out[i] = ma * gI[i] + mb;
  }
}

/* ---------- Paired RI guided filter (shares the 6 pre-aa box sums between
 *            GF(U,V,M) and GF(V,U,M) since those compute identical box sums
 *            modulo pointer relabelling). Used for iteration-loop calls where
 *            (riGrH, riRH) and (riGbH, riBH) etc. form natural swap pairs. */
static void _ari_gf_pair(const float *const restrict U,
                         const float *const restrict V,
                         const float *const restrict M,
                         float *const restrict out_UV,   /* GF(I=U, p=V, M) */
                         float *const restrict out_VU,   /* GF(I=V, p=U, M) */
                         const int width,
                         const int height,
                         const int rh,
                         const int rv,
                         const float eps,
                         /* workspace: 13 npix buffers */
                         float *const restrict N,        /* box(M) */
                         float *const restrict sU,       /* box(U*M) */
                         float *const restrict sV,       /* box(V*M) */
                         float *const restrict sUV,      /* box(U*V*M) */
                         float *const restrict sUU,      /* box(U*U*M) */
                         float *const restrict sVV,      /* box(V*V*M) */
                         float *const restrict aa,
                         float *const restrict bb,
                         float *const restrict dif,
                         float *const restrict wdif,
                         float *const restrict tmp,
                         float *const restrict mean_a,
                         float *const restrict mean_b,
                         double *const restrict integral)
{
  const size_t npix = (size_t)width * height;

  /* --- Shared box sums (6): computed once for both calls --- */
  _ari_box_sum_rect(M, N, width, height, rh, rv, integral);
  DT_OMP_FOR()
  for(size_t i = 0; i < npix; i++) if(N[i] == 0.0f) N[i] = 1.0f;

  DT_OMP_FOR()
  for(size_t i = 0; i < npix; i++) tmp[i] = U[i] * M[i];
  _ari_box_sum_rect(tmp, sU, width, height, rh, rv, integral);

  DT_OMP_FOR()
  for(size_t i = 0; i < npix; i++) tmp[i] = V[i] * M[i];
  _ari_box_sum_rect(tmp, sV, width, height, rh, rv, integral);

  DT_OMP_FOR()
  for(size_t i = 0; i < npix; i++) tmp[i] = U[i] * V[i] * M[i];
  _ari_box_sum_rect(tmp, sUV, width, height, rh, rv, integral);

  DT_OMP_FOR()
  for(size_t i = 0; i < npix; i++) tmp[i] = U[i] * U[i] * M[i];
  _ari_box_sum_rect(tmp, sUU, width, height, rh, rv, integral);

  DT_OMP_FOR()
  for(size_t i = 0; i < npix; i++) tmp[i] = V[i] * V[i] * M[i];
  _ari_box_sum_rect(tmp, sVV, width, height, rh, rv, integral);

  /* --- Call 1: I=U, p=V  (sI=sU, sp=sV, sII=sUU, spp=sVV; sIp=sUV) --- */
  DT_OMP_FOR()
  for(size_t i = 0; i < npix; i++)
  {
    const float n = N[i];
    const float mI = sU[i] / n;
    const float mp = sV[i] / n;
    const float cov = sUV[i] / n - mI * mp;
    const float var = sUU[i] / n - mI * mI;
    aa[i] = cov / (var + eps);
    bb[i] = mp - aa[i] * mI;
  }

  DT_OMP_FOR()
  for(size_t i = 0; i < npix; i++)
  {
    const float a = aa[i], b = bb[i];
    float d = a*a*sUU[i] + b*b*N[i] + sVV[i]
            + 2.0f*a*b*sU[i] - 2.0f*b*sV[i] - 2.0f*a*sUV[i];
    d /= N[i];
    if(d < 0.0f) d = 0.0f;
    d = sqrtf(d);
    if(d < 1e-3f) d = 1e-3f;
    dif[i] = 1.0f / d;
  }

  _ari_box_sum_rect(dif, wdif, width, height, rh, rv, integral);

  DT_OMP_FOR()
  for(size_t i = 0; i < npix; i++) tmp[i] = aa[i] * dif[i];
  _ari_box_sum_rect(tmp, mean_a, width, height, rh, rv, integral);
  DT_OMP_FOR()
  for(size_t i = 0; i < npix; i++) tmp[i] = bb[i] * dif[i];
  _ari_box_sum_rect(tmp, mean_b, width, height, rh, rv, integral);

  DT_OMP_FOR()
  for(size_t i = 0; i < npix; i++)
  {
    const float denom = wdif[i] + 1e-4f;
    out_UV[i] = mean_a[i] / denom * U[i] + mean_b[i] / denom;
  }

  /* --- Call 2: I=V, p=U  (swap: sI=sV, sp=sU, sII=sVV, spp=sUU; sIp=sUV) --- */
  DT_OMP_FOR()
  for(size_t i = 0; i < npix; i++)
  {
    const float n = N[i];
    const float mI = sV[i] / n;
    const float mp = sU[i] / n;
    const float cov = sUV[i] / n - mI * mp;
    const float var = sVV[i] / n - mI * mI;
    aa[i] = cov / (var + eps);
    bb[i] = mp - aa[i] * mI;
  }

  DT_OMP_FOR()
  for(size_t i = 0; i < npix; i++)
  {
    const float a = aa[i], b = bb[i];
    float d = a*a*sVV[i] + b*b*N[i] + sUU[i]
            + 2.0f*a*b*sV[i] - 2.0f*b*sU[i] - 2.0f*a*sUV[i];
    d /= N[i];
    if(d < 0.0f) d = 0.0f;
    d = sqrtf(d);
    if(d < 1e-3f) d = 1e-3f;
    dif[i] = 1.0f / d;
  }

  _ari_box_sum_rect(dif, wdif, width, height, rh, rv, integral);

  /* mean_a, mean_b for call 2 reuse sU, sV buffers (no longer needed). */
  DT_OMP_FOR()
  for(size_t i = 0; i < npix; i++) tmp[i] = aa[i] * dif[i];
  _ari_box_sum_rect(tmp, sU /*= mean_a2*/, width, height, rh, rv, integral);
  DT_OMP_FOR()
  for(size_t i = 0; i < npix; i++) tmp[i] = bb[i] * dif[i];
  _ari_box_sum_rect(tmp, sV /*= mean_b2*/, width, height, rh, rv, integral);

  DT_OMP_FOR()
  for(size_t i = 0; i < npix; i++)
  {
    const float denom = wdif[i] + 1e-4f;
    out_VU[i] = sU[i] / denom * V[i] + sV[i] / denom;
  }
}

/* ---------- ARI's MLRI guided filter (Kiku 2014) ============================
 * Port of ARI.zip/guidedfilter_MLRI.m. Signature:
 *   guidedfilter_MLRI(G, R, mask_intensity, I, p, mask_laplace, rh, rv, eps)
 * where G, R are the intensity-domain guide and input (used for the b
 * offset and the dif weighting), and I, p are the Laplacian-filtered
 * guide and input (used to solve the origin-through slope
 *   a = box(I*p*Mdif) / (box(I*I*Mdif) + eps) ).
 */
static void _ari_guidedfilter_mlri(const float *const restrict G,
                                   const float *const restrict R,
                                   const float *const restrict mask_int,
                                   const float *const restrict gI,
                                   const float *const restrict p,
                                   const float *const restrict mask_lap,
                                   float *const restrict out,
                                   const int width,
                                   const int height,
                                   const int rh,
                                   const int rv,
                                   const float eps,
                                   float *const restrict work0,
                                   float *const restrict work1,
                                   float *const restrict work2,
                                   float *const restrict work3,
                                   float *const restrict work4,
                                   float *const restrict work5,
                                   float *const restrict work6,
                                   float *const restrict work7,
                                   float *const restrict work8,
                                   float *const restrict work9,
                                   float *const restrict work10,
                                   double *const restrict integral)
{
  const size_t npix = (size_t)width * height;

  float *N3   = work0;   /* box(mask_int) */
  float *N    = work1;   /* box(mask_lap) */
  float *aa   = work2;
  float *bb   = work3;
  float *sG   = work4;   /* box(G*mask_int) - kept for dif and mean_G */
  float *sR   = work5;   /* box(R*mask_int) - kept for dif and mean_R */
  float *sGG  = work6;   /* box(G*G*mask_int) */
  float *dif  = work7;
  float *wdif = work8;
  float *tmp  = work9;
  float *aux  = work10;

  _ari_box_sum_rect(mask_int, N3, width, height, rh, rv, integral);
  _ari_box_sum_rect(mask_lap, N,  width, height, rh, rv, integral);
  DT_OMP_FOR()
  for(size_t i = 0; i < npix; i++)
  {
    if(N3[i] == 0.0f) N3[i] = 1.0f;
    if(N[i]  == 0.0f) N[i]  = 1.0f;
  }

  /* a = box(I*p*Mdif) / (box(I*I*Mdif) + eps)   (origin-through) */
  DT_OMP_FOR()
  for(size_t i = 0; i < npix; i++) tmp[i] = gI[i] * p[i] * mask_lap[i];
  _ari_box_sum_rect(tmp, aa, width, height, rh, rv, integral);
  DT_OMP_FOR()
  for(size_t i = 0; i < npix; i++) tmp[i] = gI[i] * gI[i] * mask_lap[i];
  _ari_box_sum_rect(tmp, aux, width, height, rh, rv, integral);
  DT_OMP_FOR()
  for(size_t i = 0; i < npix; i++)
  {
    const float num = aa[i] / N[i];
    const float den = aux[i] / N[i];
    aa[i] = num / (den + eps);
  }

  /* Cached sums over mask_int: sG, sR, sGG computed once and reused in dif. */
  DT_OMP_FOR()
  for(size_t i = 0; i < npix; i++) tmp[i] = G[i] * mask_int[i];
  _ari_box_sum_rect(tmp, sG, width, height, rh, rv, integral);
  DT_OMP_FOR()
  for(size_t i = 0; i < npix; i++) tmp[i] = R[i] * mask_int[i];
  _ari_box_sum_rect(tmp, sR, width, height, rh, rv, integral);
  DT_OMP_FOR()
  for(size_t i = 0; i < npix; i++) tmp[i] = G[i] * G[i] * mask_int[i];
  _ari_box_sum_rect(tmp, sGG, width, height, rh, rv, integral);

  /* bb = mean_R - a*mean_G. */
  DT_OMP_FOR()
  for(size_t i = 0; i < npix; i++)
  {
    const float n = N3[i];
    bb[i] = sR[i] / n - aa[i] * sG[i] / n;
  }

  /* dif accumulator. The aux buffer is reused for sRR and sRG one-shot sums. */
  DT_OMP_FOR()
  for(size_t i = 0; i < npix; i++) tmp[i] = R[i] * R[i] * mask_int[i];
  _ari_box_sum_rect(tmp, aux /* = sRR */, width, height, rh, rv, integral);
  DT_OMP_FOR()
  for(size_t i = 0; i < npix; i++)
  {
    const float a = aa[i], b = bb[i];
    dif[i] = a*a*sGG[i] + b*b*N3[i] + aux[i] + 2.0f*a*b*sG[i] - 2.0f*b*sR[i];
  }
  DT_OMP_FOR()
  for(size_t i = 0; i < npix; i++) tmp[i] = R[i] * G[i] * mask_int[i];
  _ari_box_sum_rect(tmp, aux /* = sRG */, width, height, rh, rv, integral);
  DT_OMP_FOR()
  for(size_t i = 0; i < npix; i++)
  {
    float d = (dif[i] - 2.0f * aa[i] * aux[i]) / N3[i];
    if(d < 0.0f) d = 0.0f;
    d = sqrtf(d);
    if(d < 1e-3f) d = 1e-3f;
    dif[i] = 1.0f / d;
  }

  _ari_box_sum_rect(dif, wdif, width, height, rh, rv, integral);

  /* Weighted mean_a, mean_b. Reuse sG as mean_a, sR as mean_b (no longer needed). */
  DT_OMP_FOR()
  for(size_t i = 0; i < npix; i++) tmp[i] = aa[i] * dif[i];
  _ari_box_sum_rect(tmp, sG /*= mean_a*/, width, height, rh, rv, integral);
  DT_OMP_FOR()
  for(size_t i = 0; i < npix; i++) tmp[i] = bb[i] * dif[i];
  _ari_box_sum_rect(tmp, sR /*= mean_b*/, width, height, rh, rv, integral);

  DT_OMP_FOR()
  for(size_t i = 0; i < npix; i++)
  {
    const float denom = wdif[i] + 1e-4f;
    const float ma = sG[i] / denom;
    const float mb = sR[i] / denom;
    out[i] = ma * G[i] + mb;
  }
}

/* ---------- Paired MLRI guided filter (shares the 6 intensity-mask box sums
 *            between GF_MLRI(U,V,...) and GF_MLRI(V,U,...). The Laplacian-
 *            domain sums differ per call (mask_lap, I, p change) and are
 *            computed separately. Used for (mlRH, mlGrH) etc. pairs. */
static void _ari_gfmlri_pair(const float *const restrict U,    /* intensity guide, call 1 */
                             const float *const restrict V,    /* intensity input, call 1 */
                             const float *const restrict mask_int,
                             const float *const restrict I1,   /* Laplacian guide, call 1 */
                             const float *const restrict p1,   /* Laplacian input, call 1 */
                             const float *const restrict mask_lap1,
                             const float *const restrict I2,   /* Laplacian guide, call 2 (swap) */
                             const float *const restrict p2,   /* Laplacian input, call 2 */
                             const float *const restrict mask_lap2,
                             float *const restrict out_UV,     /* MLRI(G=U, R=V, I=I1, p=p1, mask_lap=mask_lap1) */
                             float *const restrict out_VU,     /* MLRI(G=V, R=U, I=I2, p=p2, mask_lap=mask_lap2) */
                             const int width,
                             const int height,
                             const int rh,
                             const int rv,
                             const float eps,
                             /* workspace: 14 npix buffers */
                             float *const restrict N3,    /* box(mask_int), shared */
                             float *const restrict sU,    /* box(U*mask_int), shared */
                             float *const restrict sV,    /* box(V*mask_int), shared */
                             float *const restrict sUV,   /* box(U*V*mask_int), shared */
                             float *const restrict sUU,   /* box(U*U*mask_int), shared */
                             float *const restrict sVV,   /* box(V*V*mask_int), shared */
                             float *const restrict N,     /* box(mask_lap), per-call */
                             float *const restrict aa,
                             float *const restrict bb,
                             float *const restrict dif,
                             float *const restrict wdif,
                             float *const restrict tmp,
                             float *const restrict mean_a,
                             float *const restrict mean_b,
                             double *const restrict integral)
{
  const size_t npix = (size_t)width * height;

  /* --- Shared intensity-mask box sums (6): computed once for both calls --- */
  _ari_box_sum_rect(mask_int, N3, width, height, rh, rv, integral);
  DT_OMP_FOR()
  for(size_t i = 0; i < npix; i++) if(N3[i] == 0.0f) N3[i] = 1.0f;

  DT_OMP_FOR()
  for(size_t i = 0; i < npix; i++) tmp[i] = U[i] * mask_int[i];
  _ari_box_sum_rect(tmp, sU, width, height, rh, rv, integral);

  DT_OMP_FOR()
  for(size_t i = 0; i < npix; i++) tmp[i] = V[i] * mask_int[i];
  _ari_box_sum_rect(tmp, sV, width, height, rh, rv, integral);

  DT_OMP_FOR()
  for(size_t i = 0; i < npix; i++) tmp[i] = U[i] * V[i] * mask_int[i];
  _ari_box_sum_rect(tmp, sUV, width, height, rh, rv, integral);

  DT_OMP_FOR()
  for(size_t i = 0; i < npix; i++) tmp[i] = U[i] * U[i] * mask_int[i];
  _ari_box_sum_rect(tmp, sUU, width, height, rh, rv, integral);

  DT_OMP_FOR()
  for(size_t i = 0; i < npix; i++) tmp[i] = V[i] * V[i] * mask_int[i];
  _ari_box_sum_rect(tmp, sVV, width, height, rh, rv, integral);

  /* --- Call 1: G=U, R=V, I=I1, p=p1, mask_lap=mask_lap1 --- */
  _ari_box_sum_rect(mask_lap1, N, width, height, rh, rv, integral);
  DT_OMP_FOR()
  for(size_t i = 0; i < npix; i++) if(N[i] == 0.0f) N[i] = 1.0f;

  /* aa = box(I1*p1*mask_lap1) / (box(I1*I1*mask_lap1) + eps*N) */
  DT_OMP_FOR()
  for(size_t i = 0; i < npix; i++) tmp[i] = I1[i] * p1[i] * mask_lap1[i];
  _ari_box_sum_rect(tmp, aa, width, height, rh, rv, integral);
  DT_OMP_FOR()
  for(size_t i = 0; i < npix; i++) tmp[i] = I1[i] * I1[i] * mask_lap1[i];
  _ari_box_sum_rect(tmp, bb /* = aa_denom temp */, width, height, rh, rv, integral);
  DT_OMP_FOR()
  for(size_t i = 0; i < npix; i++)
  {
    const float num = aa[i] / N[i];
    const float den = bb[i] / N[i];
    aa[i] = num / (den + eps);
    bb[i] = sV[i] / N3[i] - aa[i] * sU[i] / N3[i];
  }

  /* dif = a²*sUU + b²*N3 + sVV + 2ab*sU - 2b*sV - 2a*sUV   (G=U, R=V) */
  DT_OMP_FOR()
  for(size_t i = 0; i < npix; i++)
  {
    const float a = aa[i], b = bb[i];
    float d = a*a*sUU[i] + b*b*N3[i] + sVV[i]
            + 2.0f*a*b*sU[i] - 2.0f*b*sV[i] - 2.0f*a*sUV[i];
    d /= N3[i];
    if(d < 0.0f) d = 0.0f;
    d = sqrtf(d);
    if(d < 1e-3f) d = 1e-3f;
    dif[i] = 1.0f / d;
  }

  _ari_box_sum_rect(dif, wdif, width, height, rh, rv, integral);
  DT_OMP_FOR()
  for(size_t i = 0; i < npix; i++) tmp[i] = aa[i] * dif[i];
  _ari_box_sum_rect(tmp, mean_a, width, height, rh, rv, integral);
  DT_OMP_FOR()
  for(size_t i = 0; i < npix; i++) tmp[i] = bb[i] * dif[i];
  _ari_box_sum_rect(tmp, mean_b, width, height, rh, rv, integral);
  DT_OMP_FOR()
  for(size_t i = 0; i < npix; i++)
  {
    const float denom = wdif[i] + 1e-4f;
    out_UV[i] = mean_a[i] / denom * U[i] + mean_b[i] / denom;
  }

  /* --- Call 2: G=V, R=U, I=I2, p=p2, mask_lap=mask_lap2 --- */
  _ari_box_sum_rect(mask_lap2, N, width, height, rh, rv, integral);
  DT_OMP_FOR()
  for(size_t i = 0; i < npix; i++) if(N[i] == 0.0f) N[i] = 1.0f;

  DT_OMP_FOR()
  for(size_t i = 0; i < npix; i++) tmp[i] = I2[i] * p2[i] * mask_lap2[i];
  _ari_box_sum_rect(tmp, aa, width, height, rh, rv, integral);
  DT_OMP_FOR()
  for(size_t i = 0; i < npix; i++) tmp[i] = I2[i] * I2[i] * mask_lap2[i];
  _ari_box_sum_rect(tmp, bb, width, height, rh, rv, integral);
  DT_OMP_FOR()
  for(size_t i = 0; i < npix; i++)
  {
    const float num = aa[i] / N[i];
    const float den = bb[i] / N[i];
    aa[i] = num / (den + eps);
    /* swap: G=V, R=U */
    bb[i] = sU[i] / N3[i] - aa[i] * sV[i] / N3[i];
  }

  /* dif = a²*sVV + b²*N3 + sUU + 2ab*sV - 2b*sU - 2a*sUV   (G=V, R=U) */
  DT_OMP_FOR()
  for(size_t i = 0; i < npix; i++)
  {
    const float a = aa[i], b = bb[i];
    float d = a*a*sVV[i] + b*b*N3[i] + sUU[i]
            + 2.0f*a*b*sV[i] - 2.0f*b*sU[i] - 2.0f*a*sUV[i];
    d /= N3[i];
    if(d < 0.0f) d = 0.0f;
    d = sqrtf(d);
    if(d < 1e-3f) d = 1e-3f;
    dif[i] = 1.0f / d;
  }

  _ari_box_sum_rect(dif, wdif, width, height, rh, rv, integral);
  /* mean_a, mean_b for call 2 reuse sU, sV (shared sums no longer needed). */
  DT_OMP_FOR()
  for(size_t i = 0; i < npix; i++) tmp[i] = aa[i] * dif[i];
  _ari_box_sum_rect(tmp, sU /*= mean_a2*/, width, height, rh, rv, integral);
  DT_OMP_FOR()
  for(size_t i = 0; i < npix; i++) tmp[i] = bb[i] * dif[i];
  _ari_box_sum_rect(tmp, sV /*= mean_b2*/, width, height, rh, rv, integral);

  DT_OMP_FOR()
  for(size_t i = 0; i < npix; i++)
  {
    const float denom = wdif[i] + 1e-4f;
    out_VU[i] = sU[i] / denom * V[i] + sV[i] / denom;
  }
}

/* ---------- Gaussian 5x5 sigma=2 ==========================================
 * fspecial('gaussian', [5,5], 2) from MATLAB. Used for smoothing the
 * per-iteration criterion maps before the argmin selection. */
static const float ARI_GAUSS5_SIGMA2[25] = {
  0.02324684f, 0.03382395f, 0.03832756f, 0.03382395f, 0.02324684f,
  0.03382395f, 0.04921356f, 0.05576627f, 0.04921356f, 0.03382395f,
  0.03832756f, 0.05576627f, 0.06319146f, 0.05576627f, 0.03832756f,
  0.03382395f, 0.04921356f, 0.05576627f, 0.04921356f, 0.03382395f,
  0.02324684f, 0.03382395f, 0.03832756f, 0.03382395f, 0.02324684f,
};

/* 1D factorization of the above 5x5 Gaussian (outer product). Using this
 * with _ari_imfilter_h1d + _ari_imfilter_v1d yields the exact same result
 * with 10 MAC/pix instead of 25. */
static const float ARI_GAUSS5_SIGMA2_1D[5] = {
  0.15246898f, 0.22184129f, 0.25137911f, 0.22184129f, 0.15246898f
};

/* Wrapper: separable Gaussian 5x5 sigma=2. Uses `scratch` as an intermediate
 * npix buffer. Callers may pass src == dst (overwrite-in-place); the scratch
 * buffer mediates. So src/dst are not restrict-qualified here. */
static inline void _ari_gauss5_separable(const float *const src,
                                         float *const dst,
                                         float *const restrict scratch,
                                         const int width,
                                         const int height)
{
  _ari_imfilter_h1d(src,     scratch, width, height, ARI_GAUSS5_SIGMA2_1D, 5);
  _ari_imfilter_v1d(scratch, dst,     width, height, ARI_GAUSS5_SIGMA2_1D, 5);
}

/* ---------- Bayer masks ==================================================== */
static void _ari_build_masks(float *const restrict mR,
                             float *const restrict mG,
                             float *const restrict mB,
                             float *const restrict mGr,
                             float *const restrict mGb,
                             const int width,
                             const int height,
                             const uint32_t filters)
{
  DT_OMP_FOR()
  for(int y = 0; y < height; y++)
  {
    /* A row is a "red row" if either of its first two pixels is R. */
    const gboolean red_row =
        (FC(y, 0, filters) == 0) || (FC(y, 1, filters) == 0);
    for(int x = 0; x < width; x++)
    {
      const size_t p = (size_t)y * width + x;
      const int fc = FC(y, x, filters);
      mR[p]  = (fc == 0) ? 1.0f : 0.0f;
      mG[p]  = (fc == 1) ? 1.0f : 0.0f;
      mB[p]  = (fc == 2) ? 1.0f : 0.0f;
      mGr[p] = (fc == 1 &&  red_row) ? 1.0f : 0.0f;
      mGb[p] = (fc == 1 && !red_row) ? 1.0f : 0.0f;
    }
  }
}

/* ---------- Color-channel split (sparse R, G, B) =========================== */
static void _ari_split_channels(const float *const restrict raw,
                                const float *const restrict mR,
                                const float *const restrict mG,
                                const float *const restrict mB,
                                float *const restrict R,
                                float *const restrict G,
                                float *const restrict B,
                                const size_t npix)
{
  DT_OMP_FOR()
  for(size_t i = 0; i < npix; i++)
  {
    R[i] = raw[i] * mR[i];
    G[i] = raw[i] * mG[i];
    B[i] = raw[i] * mB[i];
  }
}

/* ---------- Compose green at every pixel from the 4 best candidates ========
 * Final ARI green = weighted average of RI_Gh, RI_Gv, MLRI_Gh, MLRI_Gv
 * with weights 1/(best_criterion + 1e-10). Measured G values are passed
 * through unchanged. */
static void _ari_combine_green(const float *const restrict raw,
                               const float *const restrict mG,
                               const float *const restrict RI_Gh,
                               const float *const restrict RI_Gv,
                               const float *const restrict MLRI_Gh,
                               const float *const restrict MLRI_Gv,
                               const float *const restrict RI_w2h,
                               const float *const restrict RI_w2v,
                               const float *const restrict MLRI_w2h,
                               const float *const restrict MLRI_w2v,
                               float *const restrict green,
                               const size_t npix)
{
  DT_OMP_FOR()
  for(size_t i = 0; i < npix; i++)
  {
    const float wr_h = 1.0f / (RI_w2h[i]   + 1e-10f);
    const float wr_v = 1.0f / (RI_w2v[i]   + 1e-10f);
    const float wm_h = 1.0f / (MLRI_w2h[i] + 1e-10f);
    const float wm_v = 1.0f / (MLRI_w2v[i] + 1e-10f);
    const float wsum = wr_h + wr_v + wm_h + wm_v + 1e-32f;
    const float gmix = (wr_h * RI_Gh[i]   + wr_v * RI_Gv[i]
                      + wm_h * MLRI_Gh[i] + wm_v * MLRI_Gv[i]) / wsum;
    green[i] = (mG[i] > 0.0f) ? raw[i] : gmix;
  }
}

/* ============================================================================
 * Paper-exact ARI green interpolation (port of green_interpolation.m).
 *
 * Runs 11 iterations of joint RI + MLRI directional tentative estimates
 * (8 guided-filter calls per method per direction = 16 per iteration = 32
 * per iteration total). Each iteration:
 *   (1) Tentative estimates for Gr, Gb, R, B (both methods, both dirs)
 *   (2) Residuals at the observed sample sites, upsampled with a 3-tap
 *       [1/2, 1, 1/2] kernel (paper's Eq. 5)
 *   (3) Criterion map = (|guide - tentative|)^2 * |gradient(guide - tentative)|
 *       Summed across {Gr+R, Gb+B} pairs, h and v, Gaussian-smoothed (sigma=2)
 *   (4) Per-pixel argmin across iterations: if the current criterion is
 *       smaller, replace the best-green-so-far and best-criterion-so-far
 *   (5) Guide update: the next iteration uses the refined estimates
 *   (6) Window grows: RI (h+=2, v+=1), MLRI (h2+=2, v2+=1)
 * Final output combines the 4 best-greens (RI h/v, MLRI h/v) by
 * inverse-criterion weighted average.
 * ============================================================================ */
static void _ari_green_interpolation(float *const restrict green,
                                     const float *const restrict raw,
                                     const int width,
                                     const int height,
                                     const uint32_t filters,
                                     const float eps,
                                     const int itnum,
                                     double *const restrict integral)
{
  const size_t npix = (size_t)width * height;

  /* --- mask channels & split raw per colour (sparse) --- */
  float *mR  = dt_alloc_align_float(npix);
  float *mG  = dt_alloc_align_float(npix);
  float *mB  = dt_alloc_align_float(npix);
  float *mGr = dt_alloc_align_float(npix);
  float *mGb = dt_alloc_align_float(npix);
  float *R_ch = dt_alloc_align_float(npix);
  float *G_ch = dt_alloc_align_float(npix);
  float *B_ch = dt_alloc_align_float(npix);

  /* --- regression masks: Mrh = R+Gr, Mbh = B+Gb, etc. --- */
  float *Mrh = dt_alloc_align_float(npix);
  float *Mbh = dt_alloc_align_float(npix);
  float *Mrv = dt_alloc_align_float(npix);
  float *Mbv = dt_alloc_align_float(npix);

  /* --- initial 1D bilinear rawH, rawV (for guide construction) --- */
  float *rawH = dt_alloc_align_float(npix);
  float *rawV = dt_alloc_align_float(npix);

  /* --- per-direction, per-method guides (8 per method = 16 total) --- */
  float *RI_Guidegrh = dt_alloc_align_float(npix);
  float *RI_Guidegbh = dt_alloc_align_float(npix);
  float *RI_Guiderh  = dt_alloc_align_float(npix);
  float *RI_Guidebh  = dt_alloc_align_float(npix);
  float *RI_Guidegrv = dt_alloc_align_float(npix);
  float *RI_Guidegbv = dt_alloc_align_float(npix);
  float *RI_Guiderv  = dt_alloc_align_float(npix);
  float *RI_Guidebv  = dt_alloc_align_float(npix);
  float *ML_Guidegrh = dt_alloc_align_float(npix);
  float *ML_Guidegbh = dt_alloc_align_float(npix);
  float *ML_Guiderh  = dt_alloc_align_float(npix);
  float *ML_Guidebh  = dt_alloc_align_float(npix);
  float *ML_Guidegrv = dt_alloc_align_float(npix);
  float *ML_Guidegbv = dt_alloc_align_float(npix);
  float *ML_Guiderv  = dt_alloc_align_float(npix);
  float *ML_Guidebv  = dt_alloc_align_float(npix);

  /* --- best (per-pixel argmin) state --- */
  float *RI_Gh  = dt_alloc_align_float(npix);
  float *RI_Gv  = dt_alloc_align_float(npix);
  float *ML_Gh  = dt_alloc_align_float(npix);
  float *ML_Gv  = dt_alloc_align_float(npix);
  float *RI_w2h = dt_alloc_align_float(npix);
  float *RI_w2v = dt_alloc_align_float(npix);
  float *ML_w2h = dt_alloc_align_float(npix);
  float *ML_w2v = dt_alloc_align_float(npix);

  /* --- transient per-iteration buffers (tentative, residual, refined,
   *     criterion, difcri, sums) --- reused across 4 channels, 2 dirs,
   *     2 methods. We allocate a pool of 16 npix-sized floats and reuse. */
  float *t0 = dt_alloc_align_float(npix);
  float *t1 = dt_alloc_align_float(npix);
  float *t2 = dt_alloc_align_float(npix);
  float *t3 = dt_alloc_align_float(npix);
  float *t4 = dt_alloc_align_float(npix);
  float *t5 = dt_alloc_align_float(npix);
  float *t6 = dt_alloc_align_float(npix);
  float *t7 = dt_alloc_align_float(npix);
  /* guided-filter internal workspace (14 buffers: 11 legacy GF wrapper work
   * slots + 3 extra for the shared-sums pair variants). */
  float *w0 = dt_alloc_align_float(npix);
  float *w1 = dt_alloc_align_float(npix);
  float *w2 = dt_alloc_align_float(npix);
  float *w3 = dt_alloc_align_float(npix);
  float *w4 = dt_alloc_align_float(npix);
  float *w5 = dt_alloc_align_float(npix);
  float *w6 = dt_alloc_align_float(npix);
  float *w7 = dt_alloc_align_float(npix);
  float *w8 = dt_alloc_align_float(npix);
  float *w9 = dt_alloc_align_float(npix);
  float *w10 = dt_alloc_align_float(npix);
  float *w11 = dt_alloc_align_float(npix);
  float *w12 = dt_alloc_align_float(npix);
  float *w13 = dt_alloc_align_float(npix);
  /* direction-sum and per-iter scratch */
  float *crih   = dt_alloc_align_float(npix);
  float *criv   = dt_alloc_align_float(npix);
  float *dcrih  = dt_alloc_align_float(npix);
  float *dcriv  = dt_alloc_align_float(npix);
  float *whMap  = dt_alloc_align_float(npix);
  float *wvMap  = dt_alloc_align_float(npix);

  /* Group pointers so OOM handling can release them in bulk */
  float *const alloc_list[] = {
    mR, mG, mB, mGr, mGb, R_ch, G_ch, B_ch, Mrh, Mbh, Mrv, Mbv, rawH, rawV,
    RI_Guidegrh, RI_Guidegbh, RI_Guiderh, RI_Guidebh,
    RI_Guidegrv, RI_Guidegbv, RI_Guiderv, RI_Guidebv,
    ML_Guidegrh, ML_Guidegbh, ML_Guiderh, ML_Guidebh,
    ML_Guidegrv, ML_Guidegbv, ML_Guiderv, ML_Guidebv,
    RI_Gh, RI_Gv, ML_Gh, ML_Gv,
    RI_w2h, RI_w2v, ML_w2h, ML_w2v,
    t0, t1, t2, t3, t4, t5, t6, t7,
    w0, w1, w2, w3, w4, w5, w6, w7, w8, w9, w10, w11, w12, w13,
    crih, criv, dcrih, dcriv, whMap, wvMap,
  };
  const int n_alloc = (int)(sizeof(alloc_list) / sizeof(alloc_list[0]));
  for(int i = 0; i < n_alloc; i++)
    if(!alloc_list[i])
    {
      /* fallback: copy G at G sites, zero elsewhere */
      DT_OMP_FOR()
      for(int y = 0; y < height; y++)
        for(int x = 0; x < width; x++)
        {
          const size_t p = (size_t)y * width + x;
          green[p] = (FC(y, x, filters) == 1) ? raw[p] : 0.0f;
        }
      for(int j = 0; j < n_alloc; j++) dt_free_align(alloc_list[j]);
      return;
    }

  /* ---- Step 0: masks, split, initial guides ---- */
  _ari_build_masks(mR, mG, mB, mGr, mGb, width, height, filters);
  _ari_split_channels(raw, mR, mG, mB, R_ch, G_ch, B_ch, npix);

  DT_OMP_FOR()
  for(size_t i = 0; i < npix; i++)
  {
    Mrh[i] = mR[i] + mGr[i];
    Mbh[i] = mB[i] + mGb[i];
    Mrv[i] = mR[i] + mGb[i];
    Mbv[i] = mB[i] + mGr[i];
  }

  const float kH_half01_half[3] = { 0.5f, 0.0f, 0.5f };
  _ari_imfilter_h1d(raw, rawH, width, height, kH_half01_half, 3);
  _ari_imfilter_v1d(raw, rawV, width, height, kH_half01_half, 3);

  /* Initial guides (paper eq. 1 style) */
  DT_OMP_FOR()
  for(size_t i = 0; i < npix; i++)
  {
    RI_Guidegrh[i] = G_ch[i] * mGr[i] + rawH[i] * mR[i];
    RI_Guidegbh[i] = G_ch[i] * mGb[i] + rawH[i] * mB[i];
    RI_Guiderh[i]  = R_ch[i]          + rawH[i] * mGr[i];
    RI_Guidebh[i]  = B_ch[i]          + rawH[i] * mGb[i];
    RI_Guidegrv[i] = G_ch[i] * mGb[i] + rawV[i] * mR[i];
    RI_Guidegbv[i] = G_ch[i] * mGr[i] + rawV[i] * mB[i];
    RI_Guiderv[i]  = R_ch[i]          + rawV[i] * mGb[i];
    RI_Guidebv[i]  = B_ch[i]          + rawV[i] * mGr[i];

    ML_Guidegrh[i] = RI_Guidegrh[i];
    ML_Guidegbh[i] = RI_Guidegbh[i];
    ML_Guiderh[i]  = RI_Guiderh[i];
    ML_Guidebh[i]  = RI_Guidebh[i];
    ML_Guidegrv[i] = RI_Guidegrv[i];
    ML_Guidegbv[i] = RI_Guidegbv[i];
    ML_Guiderv[i]  = RI_Guiderv[i];
    ML_Guidebv[i]  = RI_Guidebv[i];

    RI_Gh[i] = RI_Guidegrh[i] + RI_Guidegbh[i];
    RI_Gv[i] = RI_Guidegrv[i] + RI_Guidegbv[i];
    ML_Gh[i] = ML_Guidegrh[i] + ML_Guidegbh[i];
    ML_Gv[i] = ML_Guidegrv[i] + ML_Guidegbv[i];

    RI_w2h[i] = 1e32f;
    RI_w2v[i] = 1e32f;
    ML_w2h[i] = 1e32f;
    ML_w2v[i] = 1e32f;
  }

  /* Window sizes (paper) */
  int h_ri = 2, v_ri = 1;   /* RI window */
  int h_ml = 4, v_ml = 0;   /* MLRI window */

  const float kRes3[3]  = { 0.5f, 1.0f, 0.5f };  /* Eq. 5 residual upsample */
  const float kLap5[5]  = { -1.0f, 0.0f, 2.0f, 0.0f, -1.0f };  /* Fh for MLRI */
  const float kGrad3[3] = { -1.0f, 0.0f, 1.0f };

  for(int iter = 0; iter < itnum; iter++)
  {
    /* =========================================================
     * RI branch (h, v) — 8 GF calls per iter for R, B, Gr, Gb
     * ========================================================= */

    /* H direction: window (h_ri, v_ri) */
    /* We compute tentatives sequentially; need storage for Gr, Gb, R, B */
    float *riGrH = t0, *riGbH = t1, *riRH = t2, *riBH = t3;
    float *riGrV = t4, *riGbV = t5, *riRV = t6, *riBV = t7;

    /* RI H: pair (riGrH, riRH) shares sums over Mrh; pair (riGbH, riBH) over Mbh */
    _ari_gf_pair(RI_Guiderh, RI_Guidegrh, Mrh, riGrH /*I=Guiderh*/, riRH /*I=Guidegrh*/,
                 width, height, h_ri, v_ri, eps,
                 w0, w1, w2, w3, w4, w5, w6, w7, w8, w9, w10, w11, w12, integral);
    _ari_gf_pair(RI_Guidebh, RI_Guidegbh, Mbh, riGbH, riBH,
                 width, height, h_ri, v_ri, eps,
                 w0, w1, w2, w3, w4, w5, w6, w7, w8, w9, w10, w11, w12, integral);

    /* RI V: pair (riGrV, riRV) / (riGbV, riBV) with swapped window */
    _ari_gf_pair(RI_Guiderv, RI_Guidegrv, Mrv, riGrV, riRV,
                 width, height, v_ri, h_ri, eps,
                 w0, w1, w2, w3, w4, w5, w6, w7, w8, w9, w10, w11, w12, integral);
    _ari_gf_pair(RI_Guidebv, RI_Guidegbv, Mbv, riGbV, riBV,
                 width, height, v_ri, h_ri, eps,
                 w0, w1, w2, w3, w4, w5, w6, w7, w8, w9, w10, w11, w12, integral);

    /* Process H RI channel: residual + bilinear up + final refined + criterion.
     * Use crih/criv/dcrih/dcriv buffers to accumulate Gr+R / Gb+B (and h+v) sums.
     * Local scratch reused through w0..w9. */

    /* === RI Horizontal: Gr (cri/difcri/refined), R, Gb, B === */
    {
      /* For Gr: res = (G - tentGrH) * mGr, upsampled, refined (tent+res)*mR
       * cri = (Guide - tent) * Mrh, difcri = grad(cri, h), then abs. */
      float *res   = w0;
      float *resUp = w1;
      float *refined_Grh = w2;   /* kept for guide update below */
      float *cri   = w3;
      float *grad  = w4;
      float *refined_Rh  = w5;   /* kept for guide update below */

      /* ---- Gr ---- */
      DT_OMP_FOR()
      for(size_t i = 0; i < npix; i++) res[i] = (G_ch[i] - riGrH[i]) * mGr[i];
      _ari_imfilter_h1d(res, resUp, width, height, kRes3, 3);
      DT_OMP_FOR()
      for(size_t i = 0; i < npix; i++)
      {
        const float t = riGrH[i];
        refined_Grh[i] = (t + resUp[i]) * mR[i]; /* RI_Grh */
        cri[i] = (RI_Guidegrh[i] - t) * Mrh[i];
      }
      _ari_imfilter_h1d(cri, grad, width, height, kGrad3, 3);
      /* crih/dcrih accumulate: crih = (|cri_Gr| + |cri_R|)*Mrh summed with Gb+B later */
      DT_OMP_FOR()
      for(size_t i = 0; i < npix; i++)
      {
        crih[i]  = fabsf(cri[i]);           /* start with Gr contribution */
        dcrih[i] = fabsf(grad[i]);
      }

      /* ---- R (uses Mrh as mask, maskGr on refined) ---- */
      DT_OMP_FOR()
      for(size_t i = 0; i < npix; i++) res[i] = (R_ch[i] - riRH[i]) * mR[i];
      _ari_imfilter_h1d(res, resUp, width, height, kRes3, 3);
      DT_OMP_FOR()
      for(size_t i = 0; i < npix; i++)
      {
        const float t = riRH[i];
        refined_Rh[i] = (t + resUp[i]) * mGr[i];       /* RI_Rh */
        cri[i] = (RI_Guiderh[i] - t) * Mrh[i];
      }
      _ari_imfilter_h1d(cri, grad, width, height, kGrad3, 3);
      DT_OMP_FOR()
      for(size_t i = 0; i < npix; i++)
      {
        crih[i]  = (crih[i] + fabsf(cri[i])) * Mrh[i]; /* (|cri_Gr|+|cri_R|)*Mrh */
        dcrih[i] = (dcrih[i] + fabsf(grad[i])) * Mrh[i];
      }

      /* ---- Gb ---- */
      DT_OMP_FOR()
      for(size_t i = 0; i < npix; i++) res[i] = (G_ch[i] - riGbH[i]) * mGb[i];
      _ari_imfilter_h1d(res, resUp, width, height, kRes3, 3);
      float *refined_Gbh = w6;
      DT_OMP_FOR()
      for(size_t i = 0; i < npix; i++)
      {
        const float t = riGbH[i];
        refined_Gbh[i] = (t + resUp[i]) * mB[i];
        cri[i] = (RI_Guidegbh[i] - t) * Mbh[i];
      }
      _ari_imfilter_h1d(cri, grad, width, height, kGrad3, 3);
      float *gbh_cri  = w7;
      float *gbh_dcri = w8;
      DT_OMP_FOR()
      for(size_t i = 0; i < npix; i++)
      {
        gbh_cri[i]  = fabsf(cri[i]);
        gbh_dcri[i] = fabsf(grad[i]);
      }

      /* ---- B ---- */
      DT_OMP_FOR()
      for(size_t i = 0; i < npix; i++) res[i] = (B_ch[i] - riBH[i]) * mB[i];
      _ari_imfilter_h1d(res, resUp, width, height, kRes3, 3);
      float *refined_Bh = w9;
      DT_OMP_FOR()
      for(size_t i = 0; i < npix; i++)
      {
        const float t = riBH[i];
        refined_Bh[i] = (t + resUp[i]) * mGb[i];
        cri[i] = (RI_Guidebh[i] - t) * Mbh[i];
      }
      _ari_imfilter_h1d(cri, grad, width, height, kGrad3, 3);
      DT_OMP_FOR()
      for(size_t i = 0; i < npix; i++)
      {
        gbh_cri[i]  = (gbh_cri[i] + fabsf(cri[i])) * Mbh[i];
        gbh_dcri[i] = (gbh_dcri[i] + fabsf(grad[i])) * Mbh[i];
      }
      /* Sum horizontal criteria across Gr+R and Gb+B */
      DT_OMP_FOR()
      for(size_t i = 0; i < npix; i++)
      {
        crih[i]  += gbh_cri[i];
        dcrih[i] += gbh_dcri[i];
      }

      /* Gaussian sigma=2 smoothing of crih and dcrih (separable, cri as scratch) */
      _ari_gauss5_separable(crih,  crih,  cri, width, height);
      _ari_gauss5_separable(dcrih, dcrih, cri, width, height);

      /* RI_wh = crih^2 * dcrih */
      DT_OMP_FOR()
      for(size_t i = 0; i < npix; i++) whMap[i] = crih[i] * crih[i] * dcrih[i];

      /* Update guides using stored refined estimates (no recomputation). */
      DT_OMP_FOR()
      for(size_t i = 0; i < npix; i++)
      {
        RI_Guidegrh[i] = G_ch[i] * mGr[i] + refined_Grh[i];
        RI_Guiderh[i]  = R_ch[i]          + refined_Rh[i];
        RI_Guidegbh[i] = G_ch[i] * mGb[i] + refined_Gbh[i];
        RI_Guidebh[i]  = B_ch[i]          + refined_Bh[i];
      }

      /* New green estimate = RI_Guidegrh + RI_Guidegbh (dense green) */
      /* Per-pixel argmin: if whMap < RI_w2h, update RI_Gh and RI_w2h */
      DT_OMP_FOR()
      for(size_t i = 0; i < npix; i++)
      {
        const float new_g = RI_Guidegrh[i] + RI_Guidegbh[i];
        if(whMap[i] < RI_w2h[i])
        {
          RI_Gh[i]  = new_g;
          RI_w2h[i] = whMap[i];
        }
      }
    }

    /* === RI Vertical: same as H but with V kernels === */
    {
      float *res   = w0;
      float *resUp = w1;
      float *cri   = w3;
      float *grad  = w4;

      /* Accumulate criv / dcriv in the same manner as crih / dcrih */
      /* Gr vertical */
      DT_OMP_FOR()
      for(size_t i = 0; i < npix; i++) res[i] = (G_ch[i] - riGrV[i]) * mGb[i];
      _ari_imfilter_v1d(res, resUp, width, height, kRes3, 3);
      float *ri_Grv = w2;
      DT_OMP_FOR()
      for(size_t i = 0; i < npix; i++)
      {
        const float t = riGrV[i];
        ri_Grv[i] = (t + resUp[i]) * mR[i];
        cri[i] = (RI_Guidegrv[i] - t) * Mrv[i];
      }
      _ari_imfilter_v1d(cri, grad, width, height, kGrad3, 3);
      DT_OMP_FOR()
      for(size_t i = 0; i < npix; i++)
      {
        criv[i]  = fabsf(cri[i]);
        dcriv[i] = fabsf(grad[i]);
      }

      /* R vertical */
      DT_OMP_FOR()
      for(size_t i = 0; i < npix; i++) res[i] = (R_ch[i] - riRV[i]) * mR[i];
      _ari_imfilter_v1d(res, resUp, width, height, kRes3, 3);
      float *ri_Rv = w5;
      DT_OMP_FOR()
      for(size_t i = 0; i < npix; i++)
      {
        const float t = riRV[i];
        ri_Rv[i] = (t + resUp[i]) * mGb[i];
        cri[i] = (RI_Guiderv[i] - t) * Mrv[i];
      }
      _ari_imfilter_v1d(cri, grad, width, height, kGrad3, 3);
      DT_OMP_FOR()
      for(size_t i = 0; i < npix; i++)
      {
        criv[i]  = (criv[i] + fabsf(cri[i])) * Mrv[i];
        dcriv[i] = (dcriv[i] + fabsf(grad[i])) * Mrv[i];
      }

      /* Gb vertical */
      DT_OMP_FOR()
      for(size_t i = 0; i < npix; i++) res[i] = (G_ch[i] - riGbV[i]) * mGr[i];
      _ari_imfilter_v1d(res, resUp, width, height, kRes3, 3);
      float *ri_Gbv = w6;
      DT_OMP_FOR()
      for(size_t i = 0; i < npix; i++)
      {
        const float t = riGbV[i];
        ri_Gbv[i] = (t + resUp[i]) * mB[i];
        cri[i] = (RI_Guidegbv[i] - t) * Mbv[i];
      }
      _ari_imfilter_v1d(cri, grad, width, height, kGrad3, 3);
      float *gbv_cri = w7;
      float *gbv_dcri = w8;
      DT_OMP_FOR()
      for(size_t i = 0; i < npix; i++)
      {
        gbv_cri[i]  = fabsf(cri[i]);
        gbv_dcri[i] = fabsf(grad[i]);
      }

      /* B vertical */
      DT_OMP_FOR()
      for(size_t i = 0; i < npix; i++) res[i] = (B_ch[i] - riBV[i]) * mB[i];
      _ari_imfilter_v1d(res, resUp, width, height, kRes3, 3);
      float *ri_Bv = w9;
      DT_OMP_FOR()
      for(size_t i = 0; i < npix; i++)
      {
        const float t = riBV[i];
        ri_Bv[i] = (t + resUp[i]) * mGr[i];
        cri[i] = (RI_Guidebv[i] - t) * Mbv[i];
      }
      _ari_imfilter_v1d(cri, grad, width, height, kGrad3, 3);
      DT_OMP_FOR()
      for(size_t i = 0; i < npix; i++)
      {
        gbv_cri[i]  = (gbv_cri[i] + fabsf(cri[i])) * Mbv[i];
        gbv_dcri[i] = (gbv_dcri[i] + fabsf(grad[i])) * Mbv[i];
      }
      DT_OMP_FOR()
      for(size_t i = 0; i < npix; i++)
      {
        criv[i]  += gbv_cri[i];
        dcriv[i] += gbv_dcri[i];
      }

      /* Gaussian sigma=2 smoothing of criv and dcriv (separable) */
      _ari_gauss5_separable(criv,  criv,  cri, width, height);
      _ari_gauss5_separable(dcriv, dcriv, cri, width, height);

      DT_OMP_FOR()
      for(size_t i = 0; i < npix; i++) wvMap[i] = criv[i] * criv[i] * dcriv[i];

      /* Update V guides */
      DT_OMP_FOR()
      for(size_t i = 0; i < npix; i++)
      {
        RI_Guidegrv[i] = G_ch[i] * mGb[i] + ri_Grv[i];
        RI_Guidegbv[i] = G_ch[i] * mGr[i] + ri_Gbv[i];
        RI_Guiderv[i]  = R_ch[i]          + ri_Rv[i];
        RI_Guidebv[i]  = B_ch[i]          + ri_Bv[i];
      }
      DT_OMP_FOR()
      for(size_t i = 0; i < npix; i++)
      {
        const float new_g = RI_Guidegrv[i] + RI_Guidegbv[i];
        if(wvMap[i] < RI_w2v[i])
        {
          RI_Gv[i]  = new_g;
          RI_w2v[i] = wvMap[i];
        }
      }
    }

    /* =========================================================
     * MLRI branch (h, v) — same structure but Laplacian GF
     * ========================================================= */
    /* H direction: need difR, difGr, difB, difGb via 1D Laplacian, then GF_MLRI */
    float *difR_ = t0, *difGr_ = t1, *difB_ = t2, *difGb_ = t3;
    _ari_imfilter_h1d(ML_Guiderh,  difR_,  width, height, kLap5, 5);
    _ari_imfilter_h1d(ML_Guidegrh, difGr_, width, height, kLap5, 5);
    _ari_imfilter_h1d(ML_Guidebh,  difB_,  width, height, kLap5, 5);
    _ari_imfilter_h1d(ML_Guidegbh, difGb_, width, height, kLap5, 5);

    float *mlRH = t4, *mlBH = t5, *mlGrH = t6, *mlGbH = t7;
    /* MLRI-H: pair (mlRH, mlGrH) shares intensity sums over Mrh; (mlBH, mlGbH) over Mbh */
    _ari_gfmlri_pair(ML_Guidegrh /*U*/, ML_Guiderh /*V*/, Mrh,
                     difGr_, difR_, mR,     /* call 1: G=U, R=V, I=difGr, p=difR, mask_lap=mR */
                     difR_, difGr_, mGr,    /* call 2: G=V, R=U, I=difR, p=difGr, mask_lap=mGr */
                     mlRH, mlGrH, width, height, h_ml, v_ml, eps,
                     w0, w1, w2, w3, w4, w5, w6, w7, w8, w9, w10, w11, w12, w13, integral);
    _ari_gfmlri_pair(ML_Guidegbh /*U*/, ML_Guidebh /*V*/, Mbh,
                     difGb_, difB_, mB,
                     difB_, difGb_, mGb,
                     mlBH, mlGbH, width, height, h_ml, v_ml, eps,
                     w0, w1, w2, w3, w4, w5, w6, w7, w8, w9, w10, w11, w12, w13, integral);

    /* Process MLRI-H: compute residuals, refined, criterion, update best */
    {
      float *res   = w0;
      float *resUp = w1;
      float *cri   = w3;
      float *grad  = w4;

      /* Gr */
      DT_OMP_FOR()
      for(size_t i = 0; i < npix; i++) res[i] = (G_ch[i] - mlGrH[i]) * mGr[i];
      _ari_imfilter_h1d(res, resUp, width, height, kRes3, 3);
      float *ml_Grh = w2;
      DT_OMP_FOR()
      for(size_t i = 0; i < npix; i++)
      {
        const float t = mlGrH[i];
        ml_Grh[i] = (t + resUp[i]) * mR[i];
        cri[i] = (ML_Guidegrh[i] - t) * Mrh[i];
      }
      _ari_imfilter_h1d(cri, grad, width, height, kGrad3, 3);
      DT_OMP_FOR()
      for(size_t i = 0; i < npix; i++)
      {
        crih[i]  = fabsf(cri[i]);
        dcrih[i] = fabsf(grad[i]);
      }

      /* R */
      DT_OMP_FOR()
      for(size_t i = 0; i < npix; i++) res[i] = (R_ch[i] - mlRH[i]) * mR[i];
      _ari_imfilter_h1d(res, resUp, width, height, kRes3, 3);
      float *ml_Rh = w5;
      DT_OMP_FOR()
      for(size_t i = 0; i < npix; i++)
      {
        const float t = mlRH[i];
        ml_Rh[i] = (t + resUp[i]) * mGr[i];
        cri[i] = (ML_Guiderh[i] - t) * Mrh[i];
      }
      _ari_imfilter_h1d(cri, grad, width, height, kGrad3, 3);
      DT_OMP_FOR()
      for(size_t i = 0; i < npix; i++)
      {
        crih[i]  = (crih[i] + fabsf(cri[i])) * Mrh[i];
        dcrih[i] = (dcrih[i] + fabsf(grad[i])) * Mrh[i];
      }

      /* Gb */
      DT_OMP_FOR()
      for(size_t i = 0; i < npix; i++) res[i] = (G_ch[i] - mlGbH[i]) * mGb[i];
      _ari_imfilter_h1d(res, resUp, width, height, kRes3, 3);
      float *ml_Gbh = w6;
      DT_OMP_FOR()
      for(size_t i = 0; i < npix; i++)
      {
        const float t = mlGbH[i];
        ml_Gbh[i] = (t + resUp[i]) * mB[i];
        cri[i] = (ML_Guidegbh[i] - t) * Mbh[i];
      }
      _ari_imfilter_h1d(cri, grad, width, height, kGrad3, 3);
      float *gbh_cri  = w7;
      float *gbh_dcri = w8;
      DT_OMP_FOR()
      for(size_t i = 0; i < npix; i++)
      {
        gbh_cri[i]  = fabsf(cri[i]);
        gbh_dcri[i] = fabsf(grad[i]);
      }

      /* B */
      DT_OMP_FOR()
      for(size_t i = 0; i < npix; i++) res[i] = (B_ch[i] - mlBH[i]) * mB[i];
      _ari_imfilter_h1d(res, resUp, width, height, kRes3, 3);
      float *ml_Bh = w9;
      DT_OMP_FOR()
      for(size_t i = 0; i < npix; i++)
      {
        const float t = mlBH[i];
        ml_Bh[i] = (t + resUp[i]) * mGb[i];
        cri[i] = (ML_Guidebh[i] - t) * Mbh[i];
      }
      _ari_imfilter_h1d(cri, grad, width, height, kGrad3, 3);
      DT_OMP_FOR()
      for(size_t i = 0; i < npix; i++)
      {
        gbh_cri[i]  = (gbh_cri[i] + fabsf(cri[i])) * Mbh[i];
        gbh_dcri[i] = (gbh_dcri[i] + fabsf(grad[i])) * Mbh[i];
      }
      DT_OMP_FOR()
      for(size_t i = 0; i < npix; i++)
      {
        crih[i]  += gbh_cri[i];
        dcrih[i] += gbh_dcri[i];
      }
      /* Gaussian sigma=2 smoothing (separable) */
      _ari_gauss5_separable(crih,  crih,  cri, width, height);
      _ari_gauss5_separable(dcrih, dcrih, cri, width, height);
      DT_OMP_FOR()
      for(size_t i = 0; i < npix; i++) whMap[i] = crih[i] * crih[i] * dcrih[i];

      /* Update MLRI H guides and argmin */
      DT_OMP_FOR()
      for(size_t i = 0; i < npix; i++)
      {
        ML_Guidegrh[i] = G_ch[i] * mGr[i] + ml_Grh[i];
        ML_Guidegbh[i] = G_ch[i] * mGb[i] + ml_Gbh[i];
        ML_Guiderh[i]  = R_ch[i]          + ml_Rh[i];
        ML_Guidebh[i]  = B_ch[i]          + ml_Bh[i];
      }
      DT_OMP_FOR()
      for(size_t i = 0; i < npix; i++)
      {
        const float new_g = ML_Guidegrh[i] + ML_Guidegbh[i];
        if(whMap[i] < ML_w2h[i])
        {
          ML_Gh[i]  = new_g;
          ML_w2h[i] = whMap[i];
        }
      }
    }

    /* MLRI V: Laplacian of V guides */
    float *difR_v = t0, *difGr_v = t1, *difB_v = t2, *difGb_v = t3;
    _ari_imfilter_v1d(ML_Guiderv,  difR_v,  width, height, kLap5, 5);
    _ari_imfilter_v1d(ML_Guidegrv, difGr_v, width, height, kLap5, 5);
    _ari_imfilter_v1d(ML_Guidebv,  difB_v,  width, height, kLap5, 5);
    _ari_imfilter_v1d(ML_Guidegbv, difGb_v, width, height, kLap5, 5);

    float *mlRV = t4, *mlBV = t5, *mlGrV = t6, *mlGbV = t7;
    /* MLRI-V: pair (mlRV, mlGrV) over Mrv; (mlBV, mlGbV) over Mbv. Note the V
     * direction uses swapped (mGb, mGr) masks for the Gr/Gb-flavored calls. */
    _ari_gfmlri_pair(ML_Guidegrv /*U*/, ML_Guiderv /*V*/, Mrv,
                     difGr_v, difR_v, mR,     /* call 1: G=U, R=V, mask_lap=mR */
                     difR_v, difGr_v, mGb,    /* call 2: G=V, R=U, mask_lap=mGb */
                     mlRV, mlGrV, width, height, v_ml, h_ml, eps,
                     w0, w1, w2, w3, w4, w5, w6, w7, w8, w9, w10, w11, w12, w13, integral);
    _ari_gfmlri_pair(ML_Guidegbv /*U*/, ML_Guidebv /*V*/, Mbv,
                     difGb_v, difB_v, mB,
                     difB_v, difGb_v, mGr,
                     mlBV, mlGbV, width, height, v_ml, h_ml, eps,
                     w0, w1, w2, w3, w4, w5, w6, w7, w8, w9, w10, w11, w12, w13, integral);

    {
      float *res   = w0;
      float *resUp = w1;
      float *cri   = w3;
      float *grad  = w4;

      DT_OMP_FOR()
      for(size_t i = 0; i < npix; i++) res[i] = (G_ch[i] - mlGrV[i]) * mGb[i];
      _ari_imfilter_v1d(res, resUp, width, height, kRes3, 3);
      float *ml_Grv = w2;
      DT_OMP_FOR()
      for(size_t i = 0; i < npix; i++)
      {
        const float t = mlGrV[i];
        ml_Grv[i] = (t + resUp[i]) * mR[i];
        cri[i] = (ML_Guidegrv[i] - t) * Mrv[i];
      }
      _ari_imfilter_v1d(cri, grad, width, height, kGrad3, 3);
      DT_OMP_FOR()
      for(size_t i = 0; i < npix; i++)
      {
        criv[i]  = fabsf(cri[i]);
        dcriv[i] = fabsf(grad[i]);
      }

      DT_OMP_FOR()
      for(size_t i = 0; i < npix; i++) res[i] = (R_ch[i] - mlRV[i]) * mR[i];
      _ari_imfilter_v1d(res, resUp, width, height, kRes3, 3);
      float *ml_Rv = w5;
      DT_OMP_FOR()
      for(size_t i = 0; i < npix; i++)
      {
        const float t = mlRV[i];
        ml_Rv[i] = (t + resUp[i]) * mGb[i];
        cri[i] = (ML_Guiderv[i] - t) * Mrv[i];
      }
      _ari_imfilter_v1d(cri, grad, width, height, kGrad3, 3);
      DT_OMP_FOR()
      for(size_t i = 0; i < npix; i++)
      {
        criv[i]  = (criv[i] + fabsf(cri[i])) * Mrv[i];
        dcriv[i] = (dcriv[i] + fabsf(grad[i])) * Mrv[i];
      }

      DT_OMP_FOR()
      for(size_t i = 0; i < npix; i++) res[i] = (G_ch[i] - mlGbV[i]) * mGr[i];
      _ari_imfilter_v1d(res, resUp, width, height, kRes3, 3);
      float *ml_Gbv = w6;
      DT_OMP_FOR()
      for(size_t i = 0; i < npix; i++)
      {
        const float t = mlGbV[i];
        ml_Gbv[i] = (t + resUp[i]) * mB[i];
        cri[i] = (ML_Guidegbv[i] - t) * Mbv[i];
      }
      _ari_imfilter_v1d(cri, grad, width, height, kGrad3, 3);
      float *gbv_cri = w7;
      float *gbv_dcri = w8;
      DT_OMP_FOR()
      for(size_t i = 0; i < npix; i++)
      {
        gbv_cri[i]  = fabsf(cri[i]);
        gbv_dcri[i] = fabsf(grad[i]);
      }

      DT_OMP_FOR()
      for(size_t i = 0; i < npix; i++) res[i] = (B_ch[i] - mlBV[i]) * mB[i];
      _ari_imfilter_v1d(res, resUp, width, height, kRes3, 3);
      float *ml_Bv = w9;
      DT_OMP_FOR()
      for(size_t i = 0; i < npix; i++)
      {
        const float t = mlBV[i];
        ml_Bv[i] = (t + resUp[i]) * mGr[i];
        cri[i] = (ML_Guidebv[i] - t) * Mbv[i];
      }
      _ari_imfilter_v1d(cri, grad, width, height, kGrad3, 3);
      DT_OMP_FOR()
      for(size_t i = 0; i < npix; i++)
      {
        gbv_cri[i]  = (gbv_cri[i] + fabsf(cri[i])) * Mbv[i];
        gbv_dcri[i] = (gbv_dcri[i] + fabsf(grad[i])) * Mbv[i];
      }
      DT_OMP_FOR()
      for(size_t i = 0; i < npix; i++)
      {
        criv[i]  += gbv_cri[i];
        dcriv[i] += gbv_dcri[i];
      }
      /* Gaussian sigma=2 smoothing of criv and dcriv (separable) */
      _ari_gauss5_separable(criv,  criv,  cri, width, height);
      _ari_gauss5_separable(dcriv, dcriv, cri, width, height);
      DT_OMP_FOR()
      for(size_t i = 0; i < npix; i++) wvMap[i] = criv[i] * criv[i] * dcriv[i];

      DT_OMP_FOR()
      for(size_t i = 0; i < npix; i++)
      {
        ML_Guidegrv[i] = G_ch[i] * mGb[i] + ml_Grv[i];
        ML_Guidegbv[i] = G_ch[i] * mGr[i] + ml_Gbv[i];
        ML_Guiderv[i]  = R_ch[i]          + ml_Rv[i];
        ML_Guidebv[i]  = B_ch[i]          + ml_Bv[i];
      }
      DT_OMP_FOR()
      for(size_t i = 0; i < npix; i++)
      {
        const float new_g = ML_Guidegrv[i] + ML_Guidegbv[i];
        if(wvMap[i] < ML_w2v[i])
        {
          ML_Gv[i]  = new_g;
          ML_w2v[i] = wvMap[i];
        }
      }
    }

    /* Grow window sizes for next iteration */
    h_ri += 2; v_ri += 1;
    h_ml += 2; v_ml += 1;
  }

  /* Final combine by inverse-criterion weighted average */
  _ari_combine_green(raw, mG,
                     RI_Gh, RI_Gv, ML_Gh, ML_Gv,
                     RI_w2h, RI_w2v, ML_w2h, ML_w2v,
                     green, npix);

  /* Clamp to valid range */
  DT_OMP_FOR()
  for(size_t i = 0; i < npix; i++)
    green[i] = fmaxf(fminf(green[i], 1.0f), 0.0f);

  for(int j = 0; j < n_alloc; j++) dt_free_align(alloc_list[j]);
}

/* ---------- 7x7 bicubic residual kernel (red/blue_interpolation.m, a=-0.5) */
static inline float _ari_cubic_s(const float x)
{
  const float a = -0.5f;
  const float ax = fabsf(x);
  if(ax < 1.0f) return (a + 2.0f) * ax * ax * ax - (a + 3.0f) * ax * ax + 1.0f;
  if(ax < 2.0f) return a * ax * ax * ax - 5.0f * a * ax * ax + 8.0f * a * ax - 4.0f * a;
  return 0.0f;
}

__attribute__((unused)) static void _ari_build_bicubic7x7(float kernel[49])
{
  const float s32 = _ari_cubic_s(1.5f);
  const float s12 = _ari_cubic_s(0.5f);
  const float s0  = _ari_cubic_s(0.0f);
  const float s1  = _ari_cubic_s(1.0f);
  const float A = s32 * s32;
  const float B = s32 * s12;
  const float C = s12 * s12;
  const float D = s0  * s12;
  const float E = s0  * s32;
  const float F = s1  * s12;
  const float G = s1  * s32;
  const float H[49] = {
    A, G, B, E, B, G, A,
    G, 0.0f, F, 0.0f, F, 0.0f, G,
    B, F, C, D, C, F, B,
    E, 0.0f, D, 1.0f, D, 0.0f, E,
    B, F, C, D, C, F, B,
    G, 0.0f, F, 0.0f, F, 0.0f, G,
    A, G, B, E, B, G, A,
  };
  memcpy(kernel, H, sizeof(H));
}

/* 1D separable factorization: the above 7x7 is the outer product of
 * k1d = [s32, 0, s12, s0, s12, 0, s32]  (s1=0, s0=1). */
static void _ari_build_bicubic1d(float kernel[7])
{
  kernel[0] = _ari_cubic_s(1.5f);
  kernel[1] = 0.0f;  /* s_fn(1) = 0 */
  kernel[2] = _ari_cubic_s(0.5f);
  kernel[3] = _ari_cubic_s(0.0f);  /* = 1 */
  kernel[4] = kernel[2];
  kernel[5] = 0.0f;
  kernel[6] = kernel[0];
}

/* ---------- R/B interpolation (port of red/blue_interpolation.m) ========== */
static void _ari_rb_interpolation(float *const restrict out_channel,
                                  const float *const restrict green,
                                  const float *const restrict mosaic_c,
                                  const float *const restrict mask_c,
                                  const int width,
                                  const int height,
                                  const int rh,
                                  const int rv,
                                  const float eps,
                                  float *const restrict work_buffers[17],
                                  double *const restrict integral)
{
  const size_t npix = (size_t)width * height;
  float *lap_c = work_buffers[0];
  float *lap_g = work_buffers[1];
  float *green_masked = work_buffers[2];
  float *tentative = work_buffers[3];
  float *residual = work_buffers[4];
  float *kernel_buf = work_buffers[5]; (void)kernel_buf;

  /* Laplacian of mosaic_c (Bayer sparse color) and green*mask_c */
  _ari_laplacian_5x5_cross(mosaic_c, lap_c, width, height);
  DT_OMP_FOR()
  for(size_t i = 0; i < npix; i++) green_masked[i] = green[i] * mask_c[i];
  _ari_laplacian_5x5_cross(green_masked, lap_g, width, height);

  /* Tentative via MLRI guided filter: G guide, C input, Laplacians for slope */
  _ari_guidedfilter_mlri(green, mosaic_c, mask_c, lap_g, lap_c, mask_c, tentative,
                         width, height, rh, rv, eps,
                         work_buffers[6], work_buffers[7], work_buffers[8],
                         work_buffers[9], work_buffers[10], work_buffers[11],
                         work_buffers[12], work_buffers[13], work_buffers[14],
                         work_buffers[15], work_buffers[16], integral);
  DT_OMP_FOR()
  for(size_t i = 0; i < npix; i++) tentative[i] = fmaxf(fminf(tentative[i], 1.0f), 0.0f);

  /* Residual at known C sites */
  DT_OMP_FOR()
  for(size_t i = 0; i < npix; i++) residual[i] = mask_c[i] * (mosaic_c[i] - tentative[i]);

  /* 7x7 bicubic residual upsample via separable 1D (tensor product) */
  float bicubic1d[7];
  _ari_build_bicubic1d(bicubic1d);
  _ari_imfilter_h1d(residual, work_buffers[5] /*scratch*/, width, height, bicubic1d, 7);
  _ari_imfilter_v1d(work_buffers[5], work_buffers[6], width, height, bicubic1d, 7);
  DT_OMP_FOR()
  for(size_t i = 0; i < npix; i++)
    out_channel[i] = fmaxf(fminf(work_buffers[6][i] + tentative[i], 1.0f), 0.0f);
}

/* ---------- ari_demosaic entry point ======================================
 * q1 = q2 = q3 all invoke the same paper-exact pipeline here. The quality
 * parameter is retained for backward-compat with the existing enum but has
 * no effect -- the paper's adaptive selection subsumes the tiered "fast /
 * balanced / full" distinction the original simplified variant had. */
static void ari_demosaic(float *const restrict out,
                         const float *const restrict in,
                         const int width,
                         const int height,
                         const uint32_t filters,
                         const int quality)
{
  /* Interpret `quality` as the iteration count when >= 3 (experimental
   * knob). Paper default is 11; this lets benchmark scripts sweep iter
   * counts without rebuilding. */
  const int itnum = (quality >= 3 && quality <= 99) ? quality : ARI_K_MAX;
  const size_t npix = (size_t)width * height;

  /* Shared integral-image buffer reused across every box_sum_rect call. */
  double *integral = (double *)dt_alloc_aligned((size_t)(width + 1) * (height + 1) * sizeof(double));
  if(!integral) { memset(out, 0, sizeof(float) * npix * 4); return; }

  /* Clamp negative inputs (rawprepare can produce them) */
  float *cfa = dt_alloc_align_float(npix);
  if(!cfa) { dt_free_align(integral); memset(out, 0, sizeof(float) * npix * 4); return; }
  DT_OMP_FOR()
  for(size_t i = 0; i < npix; i++) cfa[i] = fmaxf(in[i], 0.0f);

  /* Interpolate green via full paper ARI */
  float *green = dt_alloc_align_float(npix);
  if(!green) { dt_free_align(cfa); dt_free_align(integral); memset(out, 0, sizeof(float) * npix * 4); return; }
  _ari_green_interpolation(green, cfa, width, height, filters, ARI_EPS, itnum, integral);

  /* Prepare masks for R and B interpolation */
  float *mR = dt_alloc_align_float(npix);
  float *mG = dt_alloc_align_float(npix);
  float *mB = dt_alloc_align_float(npix);
  float *mGr = dt_alloc_align_float(npix);
  float *mGb = dt_alloc_align_float(npix);
  float *R_ch = dt_alloc_align_float(npix);
  float *B_ch = dt_alloc_align_float(npix);
  float *red   = dt_alloc_align_float(npix);
  float *blue  = dt_alloc_align_float(npix);
  /* scratch pool for rb_interpolation (17 buffers: +1 for guidedfilter_mlri work10) */
  float *sb[17];
  for(int i = 0; i < 17; i++) sb[i] = dt_alloc_align_float(npix);

  if(!mR || !mG || !mB || !mGr || !mGb || !R_ch || !B_ch || !red || !blue)
  {
    memset(out, 0, sizeof(float) * npix * 4);
    goto cleanup;
  }
  for(int i = 0; i < 17; i++) if(!sb[i]) { memset(out, 0, sizeof(float) * npix * 4); goto cleanup; }

  _ari_build_masks(mR, mG, mB, mGr, mGb, width, height, filters);
  DT_OMP_FOR()
  for(size_t i = 0; i < npix; i++)
  {
    R_ch[i] = cfa[i] * mR[i];
    B_ch[i] = cfa[i] * mB[i];
  }

  _ari_rb_interpolation(red,  green, R_ch, mR, width, height, 5, 5, ARI_EPS, sb, integral);
  _ari_rb_interpolation(blue, green, B_ch, mB, width, height, 5, 5, ARI_EPS, sb, integral);

  /* Write planar RGBX */
  DT_OMP_FOR()
  for(size_t i = 0; i < npix; i++)
  {
    const size_t o = 4 * i;
    out[o + 0] = red[i];
    out[o + 1] = green[i];
    out[o + 2] = blue[i];
    out[o + 3] = 0.0f;
  }

cleanup:
  dt_free_align(cfa);  dt_free_align(green);
  dt_free_align(mR);   dt_free_align(mG);   dt_free_align(mB);
  dt_free_align(mGr);  dt_free_align(mGb);
  dt_free_align(R_ch); dt_free_align(B_ch);
  dt_free_align(red);  dt_free_align(blue);
  for(int i = 0; i < 17; i++) dt_free_align(sb[i]);
  dt_free_align(integral);
}
