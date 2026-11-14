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

/* spektrafilm — spatial effects (grain blur and halation).
 *
 * These two operations are the only parts of the film simulation that a static
 * LUT cannot carry, because they are neighbour-dependent. They live here (rather
 * than in the inline-only spektra_core.h) so they can allocate scratch buffers
 * for a proper direct Gaussian convolution (see dt_gaussian_kernel_1d): a
 * truncated, normalized kernel applied separably (row pass, transpose, row
 * pass, transpose back), matching what the reference spektrafilm's own
 * Gaussian blur actually does, not a recursive IIR approximation whose
 * effective blur radius measurably departs from the requested sigma. Boundary
 * handling is clamp-to-edge.
 */

/* -fno-math-errno, as a source pragma, not a CMake compile option: the
   option is GCC and Clang spelling and would have to be guarded by compiler id
   in CMakeLists, and a guard there silently drops the optimisation for any
   compiler the condition does not name. __GNUC__ answers the same question at
   the point where it matters, and matches how the rest of the tree does it
   (see common/box_filters.cc).

   The gain is around the powf/exp2f/log10f calls in the per-pixel paths: told
   that they never set errno, the compiler is free to inline and vectorise them
   instead of treating each as a call with side effects. */
#ifdef __GNUC__
#pragma GCC optimize("no-math-errno")
#endif

#include "common/darktable.h"
#include "common/imagebuf.h"

#include <math.h>
#include <string.h>

#include "common/gaussian.h"
#include "spektra_core.h"

/* ------------------------------------------------------------------------ */
/* Separable Gaussian blur                                                  */
/* ------------------------------------------------------------------------ */
/* Rows first, then columns, on one channel or on three interleaved channels
   at once. Neither pass strides through memory: a row pass walks its rows,
   and a column pass takes SF_BLUR_CHUNK neighbouring floats of a row together,
   so its inner loop runs along the row and keeps one accumulator or one filter
   state per column.

   A blur reads its source in the row pass only, which writes a scratch
   buffer, so the source and the destination may be the same buffer or
   different ones. The column pass hands every result to _sf_blur_emit(),
   which stores it, adds it to an accumulator or applies an unsharp mask with
   it; a caller that combines the blurred image with something else does not
   get it written out first. */

/* Maximum kernel half-width: see SF_GAUSS_MAX_RADIUS in spektra_core.h. */

/* Direct kernel for the small-sigma path, matching the reference's own
   _gaussian_kernel_1d: truncate = 3, radius = int(3*sigma + 0.5). `kernel` must
   have room for 2*max_radius+1 taps; returns the radius actually used. */

/* SF_GAUSS_EXACT_MAX_SIGMA is defined in spektra_core.h, shared with
   spektrafilm.c's GPU macros so both dispatch at the same sigma. */

/* Young-van Vliet order-3 recursive Gaussian -- the same filter and the same
 * coefficients the reference runs above its own crossover (_yvv_coeffs /
 * _iir_horizontal in fast_gaussian_filter.py).
 *
 * Matching the reference matters more here than being independently correct.
 * Its filter runs 8-11% wide over the range that counts: a requested sigma of 5
 * gives an effective 5.56, a requested 30 gives 32.28. Every spatial radius in
 * this module was chosen by eye against renders made through it: halation's
 * 65 um first bounce, the scatter core and tail, the diffusion bank. A filter
 * that delivered exactly the sigma it was asked for would make every one of
 * those halos about 10% too tight. */
void sf_gauss_yvv_coeffs(const float sigma_req,
                         float out[4])
{
  /* The clamp is this module's, not the recursion's: the CPU dispatch in
     _sf_blur and _sf_yvv_blur_cl on the GPU both come through here,
     so neither can end up filtering at a different sigma than the other. See
     SF_GAUSS_MAX_IIR_SIGMA. */
  dt_gaussian_yvv_coeffs(fminf(sigma_req, SF_GAUSS_MAX_IIR_SIGMA), out);
}

int sf_gauss_iir_passes(const float sigma,
                        float *const pass_sigma)
{
  if(!(sigma > SF_GAUSS_MAX_IIR_SIGMA))
  {
    *pass_sigma = sigma;
    return 1;
  }
  const float r = sigma / SF_GAUSS_MAX_IIR_SIGMA;
  const int n = (int)ceilf(r * r);
  *pass_sigma = sigma / sqrtf((float)n);
  return n;
}

/* One step of the recursion: the output for input sample `x`, given the three
 * previous outputs w1, w2, w3 (most recent first). A line is filtered by a
 * forward and then a backward sweep of this step, and both sweeps seed their
 * state by replicating the edge sample, as the reference does.
 *
 * The recursion v = B*x + B1*w1 + B2*w2 + B3*w3 is evaluated as an increment
 * on the previous output, using B1 = 1 - B - B2 - B3:
 *
 *   v = w1 + (B*(x - w1) + B2*(w2 - w1) + B3*(w3 - w1))
 *
 * so B1 is not taken. Two properties follow, both of which the direct form
 * lacks in float:
 *
 *  - the DC gain is exactly 1 for any rounding of B, B2 and B3. In the direct
 *    form it is B / (1 - B1 - B2 - B3), and the four float coefficients do not
 *    sum to 1 exactly; with B around 2e-6 at sigma 126 that rounding alone
 *    puts the gain about 10% off;
 *
 *  - no large terms cancel. At large sigma B1 and B2 are about +2.9 and -2.8
 *    against an output of order 1, so the direct form loses several ulp to
 *    cancellation every step, and the near-unit poles carry that error along
 *    the line: a 1-ulp change of the input moves the output by about 2e-3 at
 *    sigma 126. Here the terms are differences of neighbouring states, and the
 *    same input change moves the output by about 2e-4.
 *
 * Explicit fmaf(), in this exact nesting, and the same in spektrafilm.cl's
 * four spektrafilm_yvv_* kernels: the result feeds straight back in as w1, so
 * a compiler fusing or reassociating differently on the two sides would give a
 * different filter, not a one-ulp difference. #pragma STDC FP_CONTRACT and
 * #pragma OPENCL FP_CONTRACT do not pin this (GCC does not implement the C
 * one, and not every OpenCL driver honours its own); an explicit fma is
 * correctly rounded on both sides. The differences and the final add are
 * single correctly rounded operations, and B3 * (w3 - w1) is a bare multiply,
 * so every operation in the step is fully specified. */
static inline float _sf_gauss_iir_step(const float x,
                                       const float w1,
                                       const float w2,
                                       const float w3,
                                       const float B,
                                       const float B2,
                                       const float B3)
{
  return w1 + fmaf(B, x - w1, fmaf(B2, w2 - w1, B3 * (w3 - w1)));
}

/* Floats of one row a column pass takes together. A multiple of 3, so a chunk
   of an interleaved 3-channel row holds whole pixels. */
#define SF_BLUR_CHUNK 1536

/* What the column pass does with a blurred value v and the destination d. */
typedef enum sf_blur_emit_t
{
  SF_EMIT_STORE = 0, /* d = v */
  SF_EMIT_ACCUM,     /* d = fmaf(wt, v, d), or fmaf(wt, v, 0) with `reset` */
  SF_EMIT_USM,       /* d = fmaf(amount, d - v, d), see sf_unsharp_mask3() */
  SF_EMIT_MUSM       /* see sf_multiplicative_unsharp_mask3() */
} sf_blur_emit_t;

typedef struct sf_blur_out_t
{
  sf_blur_emit_t mode;
  float *dst;   /* w*h pixels of `ds` floats; the result goes to the channels
                   from `doff` on */
  int ds, doff;
  float wt[3];  /* per channel. ACCUM: the weight; MUSM: the density floor */
  int reset;    /* ACCUM */
  float amount; /* USM, MUSM */
} sf_blur_out_t;

/* Hands `len` blurred floats of row `y` to the destination. They are `n` per
   pixel and start at float i0 of the row, a multiple of `n`. `v` may be the
   destination itself. */
static inline void _sf_blur_emit(const sf_blur_out_t *const o,
                                 const int w,
                                 const int n,
                                 const int y,
                                 const int i0,
                                 const float *const v,
                                 const int len)
{
  const int npx = len / n;
  const int ds = o->ds;
  float *const d = o->dst + ((size_t)y * w + i0 / n) * ds + o->doff;
  switch(o->mode)
  {
    case SF_EMIT_STORE:
      if(ds == n)
      {
        if(d != v) memcpy(d, v, sizeof(float) * len);
      }
      else
        for(int x = 0; x < npx; x++)
          for(int c = 0; c < n; c++) d[x * ds + c] = v[x * n + c];
      break;
    case SF_EMIT_ACCUM:
      /* fma-pinned, matching spektrafilm_diffusion_accum */
      for(int x = 0; x < npx; x++)
        for(int c = 0; c < n; c++)
        {
          const float a = o->reset ? 0.0f : d[x * ds + c];
          d[x * ds + c] = fmaf(o->wt[c], v[x * n + c], a);
        }
      break;
    case SF_EMIT_USM:
      /* fma-pinned, matching spektrafilm_scan_usm */
      for(int x = 0; x < npx; x++)
        for(int c = 0; c < n; c++)
        {
          const float D = d[x * ds + c];
          d[x * ds + c] = fmaf(o->amount, D - v[x * n + c], D);
        }
      break;
    case SF_EMIT_MUSM:
    {
      const float eps = 1e-6f;
      const float ratio_max = 4.0f;
      for(int x = 0; x < npx; x++)
        for(int c = 0; c < n; c++)
        {
          const float d0 = o->wt[c];
          const float D = fmaxf(d[x * ds + c] + d0, 0.0f);
          const float blur = fmaxf(v[x * n + c] + d0, eps);
          const float ratio = fmaxf(fminf(D / blur, ratio_max), 1.0f / ratio_max);
          /* fma-pinned, matching spektrafilm_grain_usm. D >= 0 and the power
             term is positive, so the result is bounded below by -d0: the
             absolute density stays non-negative while the density relative to
             the floor may go negative, as in the reference. Clamping at 0 here
             would rectify the grain below the floor and add density wherever
             grain is strong relative to the local density. */
          d[x * ds + c] = fmaf(D, powf(ratio, o->amount), -d0);
        }
      break;
    }
  }
}

/* The unblurred source through _sf_blur_emit(): what a blur at a sigma below
   its cut delivers. Source layout as for _sf_blur(). */
static void _sf_blur_emit_source(const float *const src,
                                 const int ss,
                                 const int so,
                                 const int n,
                                 const int w,
                                 const int h,
                                 const sf_blur_out_t *const out)
{
  const int W = w * n;
  DT_OMP_FOR()
  for(int y = 0; y < h; y++)
  {
    const float *const s = src + (size_t)y * w * ss + so;
    if(ss == n)
      _sf_blur_emit(out, w, n, y, 0, s, W);
    else
    {
      float v[SF_BLUR_CHUNK];
      for(int i0 = 0; i0 < W; i0 += SF_BLUR_CHUNK)
      {
        const int len = MIN(SF_BLUR_CHUNK, W - i0);
        for(int i = 0; i < len; i++) v[i] = s[(size_t)(i0 + i) * ss];
        _sf_blur_emit(out, w, n, y, i0, v, len);
      }
    }
  }
}

/* Direct convolution of one row, clamp-to-edge boundary (a plain "hold the
   edge value" extension -- the exact boundary rule matters far less than the
   kernel itself once the kernel is exact). `s` has `ss` floats per pixel and
   `d` has `n`, the channels that are convolved; ss is n, or n is 1.

   Every output takes its taps in the same order, first to last, into an
   accumulator that starts at zero. Away from the two edges the tap loop is
   the outer one, so the inner loop runs over the outputs and vectorises. */
static void _sf_gauss_convolve_row(const float *const s,
                                   const int ss,
                                   float *const d,
                                   const int n,
                                   const int w,
                                   const float *const kernel,
                                   const int radius)
{
  const int xa = MIN(radius, w), xb = MAX(xa, w - radius);
  for(int x = 0; x < w; x++)
  {
    if(x == xa) x = xb;
    if(x >= w) break;
    for(int c = 0; c < n; c++)
    {
      float acc = 0.0f;
      for(int k = -radius; k <= radius; k++)
      {
        int xx = x + k;
        xx = xx < 0 ? 0 : (xx >= w ? w - 1 : xx);
        /* fma-pinned, matching spektrafilm_gauss_row/col_*c */
        acc = fmaf(kernel[k + radius], s[(size_t)xx * ss + c], acc);
      }
      d[x * n + c] = acc;
    }
  }
  for(int i0 = xa * n; i0 < xb * n; i0 += SF_BLUR_CHUNK)
  {
    const int len = MIN(SF_BLUR_CHUNK, xb * n - i0);
    float *const dd = d + i0;
    for(int i = 0; i < len; i++) dd[i] = 0.0f;
    for(int k = 0; k <= 2 * radius; k++)
    {
      const float kk = kernel[k];
      if(ss == n)
      {
        const float *const p = s + i0 + (k - radius) * n;
        DT_OMP_SIMD()
        for(int i = 0; i < len; i++) dd[i] = fmaf(kk, p[i], dd[i]);
      }
      else
      {
        const float *const p = s + (ptrdiff_t)(i0 + k - radius) * ss;
        DT_OMP_SIMD()
        for(int i = 0; i < len; i++) dd[i] = fmaf(kk, p[(size_t)i * ss], dd[i]);
      }
    }
  }
}

/* Direct convolution along the columns of `tmp`, h rows of W floats, each
   result handed to `out`. Same tap order and boundary as the row pass. */
static void _sf_gauss_convolve_cols(const float *const tmp,
                                    const int w,
                                    const int h,
                                    const int n,
                                    const float *const kernel,
                                    const int radius,
                                    const sf_blur_out_t *const out)
{
  const int W = w * n;
  DT_OMP_FOR()
  for(int y = 0; y < h; y++)
  {
    float v[SF_BLUR_CHUNK];
    for(int i0 = 0; i0 < W; i0 += SF_BLUR_CHUNK)
    {
      const int len = MIN(SF_BLUR_CHUNK, W - i0);
      for(int i = 0; i < len; i++) v[i] = 0.0f;
      for(int k = -radius; k <= radius; k++)
      {
        const int yy = CLAMP(y + k, 0, h - 1);
        const float *const p = tmp + (size_t)yy * W + i0;
        const float kk = kernel[k + radius];
        DT_OMP_SIMD()
        for(int i = 0; i < len; i++) v[i] = fmaf(kk, p[i], v[i]);
      }
      _sf_blur_emit(out, w, n, y, i0, v, len);
    }
  }
}

/* Lanes of the recursive row pass: `rows` neighbouring rows of `n` channels
   each run through the recursion side by side. A row is one serial chain of
   steps, each waiting for the one before; the lanes are independent chains, so
   they share the time of one. */
#define SF_IIR_MAX_LANES 12

/* Forward and backward sweep along `rows` neighbouring rows. `in` has `ss`
   floats per pixel and `out` has `n`; they may be the same buffer when ss is
   n. Inlined into _sf_gauss_iir_rows() with `n` and `rows` constant. */
static inline void _sf_gauss_iir_rows_lanes(const float *const in,
                                            const int ss,
                                            float *const out,
                                            const int n,
                                            const int rows,
                                            const int w,
                                            const float B,
                                            const float B2,
                                            const float B3)
{
  const int lanes = rows * n;
  const size_t ip = (size_t)w * ss, op = (size_t)w * n;
  float w1[SF_IIR_MAX_LANES], w2[SF_IIR_MAX_LANES], w3[SF_IIR_MAX_LANES], x[SF_IIR_MAX_LANES];
  for(int r = 0; r < rows; r++)
    for(int c = 0; c < n; c++) w1[r * n + c] = w2[r * n + c] = w3[r * n + c] = in[r * ip + c];
  for(int i = 0; i < w; i++)
  {
    for(int r = 0; r < rows; r++)
      for(int c = 0; c < n; c++) x[r * n + c] = in[r * ip + (size_t)i * ss + c];
    for(int l = 0; l < lanes; l++)
    {
      const float v = _sf_gauss_iir_step(x[l], w1[l], w2[l], w3[l], B, B2, B3);
      w3[l] = w2[l];
      w2[l] = w1[l];
      w1[l] = v;
    }
    for(int r = 0; r < rows; r++)
      for(int c = 0; c < n; c++) out[r * op + (size_t)i * n + c] = w1[r * n + c];
  }
  for(int r = 0; r < rows; r++)
    for(int c = 0; c < n; c++)
      w1[r * n + c] = w2[r * n + c] = w3[r * n + c] = out[r * op + (size_t)(w - 1) * n + c];
  for(int i = w - 1; i >= 0; i--)
  {
    for(int r = 0; r < rows; r++)
      for(int c = 0; c < n; c++) x[r * n + c] = out[r * op + (size_t)i * n + c];
    for(int l = 0; l < lanes; l++)
    {
      const float v = _sf_gauss_iir_step(x[l], w1[l], w2[l], w3[l], B, B2, B3);
      w3[l] = w2[l];
      w2[l] = w1[l];
      w1[l] = v;
    }
    for(int r = 0; r < rows; r++)
      for(int c = 0; c < n; c++) out[r * op + (size_t)i * n + c] = w1[r * n + c];
  }
}

/* Recursive row pass over all rows: 4 rows of 3 channels or 8 rows of one
   channel at a time, and the rows left over one by one. `src` as for
   _sf_blur(); `tmp` has `n` floats per pixel. */
static void _sf_gauss_iir_rows(const float *const src,
                               const int ss,
                               const int so,
                               float *const tmp,
                               const int n,
                               const int w,
                               const int h,
                               const float B,
                               const float B2,
                               const float B3)
{
  const int rows = (n == 3) ? 4 : 8;
  const int nblocks = (h + rows - 1) / rows;
  DT_OMP_FOR()
  for(int b = 0; b < nblocks; b++)
  {
    const int y0 = b * rows;
    const float *const in = src + (size_t)y0 * w * ss + so;
    float *const out = tmp + (size_t)y0 * w * n;
    if(y0 + rows <= h)
    {
      if(n == 3) _sf_gauss_iir_rows_lanes(in, ss, out, 3, 4, w, B, B2, B3);
      else _sf_gauss_iir_rows_lanes(in, ss, out, 1, 8, w, B, B2, B3);
    }
    else
      for(int y = y0; y < h; y++)
      {
        const float *const in1 = src + (size_t)y * w * ss + so;
        float *const out1 = tmp + (size_t)y * w * n;
        if(n == 3) _sf_gauss_iir_rows_lanes(in1, ss, out1, 3, 1, w, B, B2, B3);
        else _sf_gauss_iir_rows_lanes(in1, ss, out1, 1, 1, w, B, B2, B3);
      }
  }
}

/* Recursive column pass on `tmp`, h rows of W floats. The forward sweep runs
   in place. The backward sweep hands its results to `out`, or with out NULL
   leaves them in `tmp`. The three state rows of a chunk rotate, the oldest
   taking the new outputs. */
static void _sf_gauss_iir_cols(float *const tmp,
                               const int w,
                               const int h,
                               const int n,
                               const float B,
                               const float B2,
                               const float B3,
                               const sf_blur_out_t *const out)
{
  const int W = w * n;
  DT_OMP_FOR()
  for(int i0 = 0; i0 < W; i0 += SF_BLUR_CHUNK)
  {
    const int len = MIN(SF_BLUR_CHUNK, W - i0);
    float sa[SF_BLUR_CHUNK], sb[SF_BLUR_CHUNK], sc[SF_BLUR_CHUNK];
    float *a1 = sa, *a2 = sb, *a3 = sc;
    const float *const first = tmp + i0;
    for(int i = 0; i < len; i++) a1[i] = a2[i] = a3[i] = first[i];
    for(int y = 0; y < h; y++)
    {
      float *const row = tmp + (size_t)y * W + i0;
      DT_OMP_SIMD()
      for(int i = 0; i < len; i++)
      {
        const float v = _sf_gauss_iir_step(row[i], a1[i], a2[i], a3[i], B, B2, B3);
        a3[i] = v;
        row[i] = v;
      }
      float *const t = a3;
      a3 = a2;
      a2 = a1;
      a1 = t;
    }
    const float *const last = tmp + (size_t)(h - 1) * W + i0;
    for(int i = 0; i < len; i++) a1[i] = a2[i] = a3[i] = last[i];
    for(int y = h - 1; y >= 0; y--)
    {
      float *const row = tmp + (size_t)y * W + i0;
      DT_OMP_SIMD()
      for(int i = 0; i < len; i++)
        a3[i] = _sf_gauss_iir_step(row[i], a1[i], a2[i], a3[i], B, B2, B3);
      if(out) _sf_blur_emit(out, w, n, y, i0, a3, len);
      else memcpy(row, a3, sizeof(float) * len);
      float *const t = a3;
      a3 = a2;
      a2 = a1;
      a1 = t;
    }
  }
}

/* Gaussian blur of `n` channels of `src` at `sigma`, the result handed to
   `out`. `src` holds w*h pixels of `ss` floats and the channels from `so` on
   are blurred: n = ss = 3 for an interleaved buffer, n = 1 for one channel of
   it or for a flat buffer (ss = 1). `tmp` is a scratch of w*h*n floats, none
   of the other buffers.

   Below `min_sigma` the source goes to `out` unblurred. Below
   SF_GAUSS_EXACT_MAX_SIGMA, or at any sigma with `exact_only`, the kernel is
   the direct one; above, the Young-van Vliet recursion, cascaded past
   SF_GAUSS_MAX_IIR_SIGMA. */
static void _sf_blur(const float *const src,
                     const int ss,
                     const int so,
                     const int n,
                     float *const tmp,
                     const int w,
                     const int h,
                     const float sigma,
                     const float min_sigma,
                     const int exact_only,
                     const sf_blur_out_t *const out)
{
  if(sigma < min_sigma)
  {
    _sf_blur_emit_source(src, ss, so, n, w, h, out);
    return;
  }
  if(exact_only || sigma < SF_GAUSS_EXACT_MAX_SIGMA)
  {
    float kernel[2 * SF_GAUSS_MAX_RADIUS + 1];
    const float *const kw = kernel;
    const int radius = dt_gaussian_kernel_1d(sigma, kernel, SF_GAUSS_MAX_RADIUS);
    DT_OMP_FOR()
    for(int y = 0; y < h; y++)
      _sf_gauss_convolve_row(src + (size_t)y * w * ss + so, ss, tmp + (size_t)y * w * n, n, w,
                             kw, radius);
    _sf_gauss_convolve_cols(tmp, w, h, n, kw, radius, out);
    return;
  }
  float pass_sigma = sigma;
  const int passes = sf_gauss_iir_passes(sigma, &pass_sigma);
  float yvv[4];
  sf_gauss_yvv_coeffs(pass_sigma, yvv);
  for(int pass = 0; pass < passes; pass++)
  {
    if(pass == 0) _sf_gauss_iir_rows(src, ss, so, tmp, n, w, h, yvv[0], yvv[2], yvv[3]);
    else _sf_gauss_iir_rows(tmp, n, 0, tmp, n, w, h, yvv[0], yvv[2], yvv[3]);
    _sf_gauss_iir_cols(tmp, w, h, n, yvv[0], yvv[2], yvv[3], pass == passes - 1 ? out : NULL);
  }
}

/* Blur all three channels with the same sigma (grain clumps), in place. Always
   uses the exact kernel regardless of sigma: the recursive path has a known
   ~18% effective-width error at the small sigmas grain's clump blur
   typically uses, which would make grain visibly the wrong size relative
   to upstream's own (exact-shape) Gaussian blur. `tmp` is a w*h*3 scratch. */
void sf_blur_plane3(float *const buf,
                    const int w,
                    const int h,
                    const float sigma,
                    float *const tmp)
{
  if(sigma < SF_GAUSS_MIN_SIGMA) return;
  const sf_blur_out_t out = { .mode = SF_EMIT_STORE, .dst = buf, .ds = 3 };
  _sf_blur(buf, 3, 0, 3, tmp, w, h, sigma, 0.0f, /*exact_only=*/1, &out);
}

/* Same exact-kernel blur as sf_blur_plane3, but for one flat w*h buffer --
   used for grain's per-sublayer dye-cloud blur, where each (channel,
   sub-layer) has its own sigma. No 0.3px floor (unlike sf_blur_plane3's guard
   for the visible clump blur): the dye-cloud sigma is often well under a
   pixel and still meaningfully softens the raw particle draw, matching
   upstream's plain `> 0` check. `tmp` is a w*h scratch. */
void sf_blur_plane1(float *const buf,
                    const int w,
                    const int h,
                    const float sigma,
                    float *const tmp)
{
  if(sigma < 1e-6f) return;
  const sf_blur_out_t out = { .mode = SF_EMIT_STORE, .dst = buf, .ds = 1 };
  _sf_blur(buf, 1, 0, 1, tmp, w, h, sigma, 0.0f, /*exact_only=*/1, &out);
}

/* Same as sf_blur_plane3, but takes the recursive path at large sigma: for
   callers with no downstream dependency on the exact kernel's shape (DIR
   coupler correction-field diffusion, unlike grain, isn't renormalizing
   against it -- it's just smoothing a density correction, not restoring a
   noise buffer's variance). */
void sf_blur_plane3_fast(float *const buf,
                         const int w,
                         const int h,
                         const float sigma,
                         float *const tmp)
{
  if(sigma < SF_GAUSS_MIN_SIGMA) return;
  const sf_blur_out_t out = { .mode = SF_EMIT_STORE, .dst = buf, .ds = 3 };
  _sf_blur(buf, 3, 0, 3, tmp, w, h, sigma, 0.0f, /*exact_only=*/0, &out);
}

/* Variance of the reference's Young-van Vliet filter at `sigma`, forward and
   backward sweep together, from the same coefficients dt_gaussian_yvv_coeffs()
   rounds to float, kept in double. For a causal B / (1 - b1 z - b2 z^2 - b3 z^3)
   with S = 1 - b1 - b2 - b3, P = b1 + 2 b2 + 3 b3 and Q = b1 + 4 b2 + 9 b3 the
   impulse response has variance P^2 / S^2 + Q / S; the backward sweep doubles
   it. Matches the measured response of the double-precision recursion to the
   second decimal (5 -> 5.56, 30 -> 32.28, 150 -> 137.88). */
static double _yvv_ref_variance(const double s)
{
  const double q = (s >= 2.5) ? (0.98711 * s - 0.96330) : (3.97156 - 4.14554 * sqrt(1.0 - 0.26891 * s));
  const double q2 = q * q, q3 = q2 * q;
  const double b0 = 1.57825 + 2.44413 * q + 1.4281 * q2 + 0.422205 * q3;
  const double b1 = (2.44413 * q + 2.85619 * q2 + 1.26661 * q3) / b0;
  const double b2 = -(1.4281 * q2 + 1.26661 * q3) / b0;
  const double b3 = 0.422205 * q3 / b0;
  const double S = 1.0 - b1 - b2 - b3;
  const double P = b1 + 2.0 * b2 + 3.0 * b3;
  const double Q = b1 + 4.0 * b2 + 9.0 * b3;
  return 2.0 * (P * P / (S * S) + Q / S);
}

int sf_wide_blur_plan(const float sigma,
                      const int w,
                      const int h,
                      const int roi_x,
                      const int roi_y,
                      int *const k,
                      float *const sigma_low,
                      int *const lw,
                      int *const lh,
                      int *const ox,
                      int *const oy)
{
  if(!(sigma >= SF_WIDE_BLUR_MIN_SIGMA) || w < 1 || h < 1) return 0;
  const int kk = (int)(sigma / SF_WIDE_BLUR_LOW_SIGMA);
  /* the width the recursive path delivers: n cascaded passes of pass_sigma,
     variances adding */
  float pass_sigma = sigma;
  const int passes = sf_gauss_iir_passes(sigma, &pass_sigma);
  const double target = (double)passes * _yvv_ref_variance((double)pass_sigma);
  const double var = target - ((double)kk * kk - 1.0) / 12.0 - (double)kk * kk / 6.0;
  *k = kk;
  *sigma_low = (float)(sqrt(var) / kk);
  /* offset of the buffer's first pixel inside its block: the grid sits on
     multiples of k in absolute coordinates */
  *ox = ((roi_x % kk) + kk) % kk;
  *oy = ((roi_y % kk) + kk) % kk;
  *lw = (*ox + w - 1) / kk + 1;
  *lh = (*oy + h - 1) / kk + 1;
  return 1;
}

void sf_wide_blur_inv_table(const int k,
                            float *const inv)
{
  inv[0] = 0.0f;
  for(int i = 1; i <= k; i++) inv[i] = 1.0f / (float)i;
}

/* Box average of each k x k block (clipped at the buffer edge) into `low`.
   Must match spektrafilm_wide_rows and spektrafilm_wide_down in spektrafilm.cl
   operation for operation:
   each block row is summed left to right, its mean is folded in with fmaf in
   top-to-bottom row order, and the sum of row means is scaled by inv[rows].
   The x loop runs over the whole buffer row with the block index computed per
   pixel, so a vectorising compiler cannot split one block's sum into partial
   sums and reorder it. */
static void _wide_down3(const float *const in,
                        float *const low,
                        const int w,
                        const int h,
                        const int k,
                        const int lw,
                        const int lh,
                        const int ox,
                        const int oy,
                        const float *const inv)
{
  DT_OMP_FOR()
  for(int Y = 0; Y < lh; Y++)
  {
    const int y0 = MAX(Y * k - oy, 0), y1 = MIN((Y + 1) * k - oy, h);
    float *const rs = dt_alloc_align_float((size_t)lw * 3);
    float *const lo = low + (size_t)Y * lw * 3;
    for(int i = 0; i < lw * 3; i++) lo[i] = 0.0f;
    if(rs)
    {
      for(int y = y0; y < y1; y++)
      {
        for(int i = 0; i < lw * 3; i++) rs[i] = 0.0f;
        const float *const row = in + (size_t)y * w * 3;
        for(int x = 0; x < w; x++)
        {
          const int X = (x + ox) / k;
          rs[X * 3 + 0] += row[x * 3 + 0];
          rs[X * 3 + 1] += row[x * 3 + 1];
          rs[X * 3 + 2] += row[x * 3 + 2];
        }
        for(int X = 0; X < lw; X++)
        {
          const int cx = MIN((X + 1) * k - ox, w) - MAX(X * k - ox, 0);
          for(int c = 0; c < 3; c++) lo[X * 3 + c] = fmaf(rs[X * 3 + c], inv[cx], lo[X * 3 + c]);
        }
      }
      dt_free_align(rs);
    }
    const float ic = inv[y1 - y0];
    for(int i = 0; i < lw * 3; i++) lo[i] = lo[i] * ic;
  }
}

/* Bilinear interpolation of `low` back to w x h, sampling block centres, each
   row handed to `out` in pieces of SF_BLUR_CHUNK floats. Must match
   spektrafilm_wide_up in spektrafilm.cl operation for operation. */
static void _wide_up3(const float *const low,
                      const int w,
                      const int h,
                      const int k,
                      const int lw,
                      const int lh,
                      const int ox,
                      const int oy,
                      const float inv2k,
                      const sf_blur_out_t *const out)
{
  DT_OMP_FOR()
  for(int y = 0; y < h; y++)
  {
    const float v = (float)(2 * (y + oy) + 1 - k) * inv2k;
    int j0 = (int)floorf(v), j1;
    float fy;
    if(j0 < 0) { j0 = j1 = 0; fy = 0.0f; }
    else if(j0 >= lh - 1) { j0 = j1 = lh - 1; fy = 0.0f; }
    else { j1 = j0 + 1; fy = v - (float)j0; }
    const float *const r0 = low + (size_t)j0 * lw * 3;
    const float *const r1 = low + (size_t)j1 * lw * 3;
    float o[SF_BLUR_CHUNK];
    for(int x0 = 0; x0 < w; x0 += SF_BLUR_CHUNK / 3)
    {
      const int npx = MIN(SF_BLUR_CHUNK / 3, w - x0);
      for(int i = 0; i < npx; i++)
      {
        const int x = x0 + i;
        const float u = (float)(2 * (x + ox) + 1 - k) * inv2k;
        int i0 = (int)floorf(u), i1;
        float fx;
        if(i0 < 0) { i0 = i1 = 0; fx = 0.0f; }
        else if(i0 >= lw - 1) { i0 = i1 = lw - 1; fx = 0.0f; }
        else { i1 = i0 + 1; fx = u - (float)i0; }
        for(int c = 0; c < 3; c++)
        {
          const float p00 = r0[i0 * 3 + c], p01 = r0[i1 * 3 + c];
          const float p10 = r1[i0 * 3 + c], p11 = r1[i1 * 3 + c];
          const float top = fmaf(fx, p01 - p00, p00);
          const float bot = fmaf(fx, p11 - p10, p10);
          o[i * 3 + c] = fmaf(fy, bot - top, top);
        }
      }
      _sf_blur_emit(out, w, 3, y, x0 * 3, o, npx * 3);
    }
  }
}

/* The decimated blur of an interleaved 3-channel `src`, the result handed to
   `out`; _sf_blur() on the recursive path where sf_wide_blur_plan() does not
   decimate. `tmp` is a w*h*3 scratch for that case. */
static void _sf_blur3_wide(const float *const src,
                           float *const tmp,
                           const int w,
                           const int h,
                           const int roi_x,
                           const int roi_y,
                           const float sigma,
                           const sf_blur_out_t *const out)
{
  int k, lw, lh, ox, oy;
  float sigma_low;
  if(!sf_wide_blur_plan(sigma, w, h, roi_x, roi_y, &k, &sigma_low, &lw, &lh, &ox, &oy))
  {
    _sf_blur(src, 3, 0, 3, tmp, w, h, sigma, SF_GAUSS_MIN_SIGMA, /*exact_only=*/0, out);
    return;
  }
  float *const low = dt_alloc_align_float((size_t)lw * lh * 3);
  float *const lowtmp = dt_alloc_align_float((size_t)lw * lh * 3);
  float *const inv = dt_alloc_align_float((size_t)k + 1);
  if(!low || !lowtmp || !inv)
  {
    dt_free_align(low);
    dt_free_align(lowtmp);
    dt_free_align(inv);
    _sf_blur(src, 3, 0, 3, tmp, w, h, sigma, SF_GAUSS_MIN_SIGMA, /*exact_only=*/0, out);
    return;
  }
  sf_wide_blur_inv_table(k, inv);
  _wide_down3(src, low, w, h, k, lw, lh, ox, oy, inv);
  sf_blur_plane3(low, lw, lh, sigma_low, lowtmp);
  _wide_up3(low, w, h, k, lw, lh, ox, oy, 1.0f / (float)(2 * k), out);
  dt_free_align(low);
  dt_free_align(lowtmp);
  dt_free_align(inv);
}

void sf_blur_plane3_wide(float *const buf,
                         const int w,
                         const int h,
                         const int roi_x,
                         const int roi_y,
                         const float sigma,
                         float *const tmp)
{
  const sf_blur_out_t out = { .mode = SF_EMIT_STORE, .dst = buf, .ds = 3 };
  _sf_blur3_wide(buf, tmp, w, h, roi_x, roi_y, sigma, &out);
}

void sf_blur_plane3_wide_accum(const float *const src,
                               float *const acc,
                               const int w,
                               const int h,
                               const int roi_x,
                               const int roi_y,
                               const float sigma,
                               const float wt[3],
                               const int reset,
                               float *const tmp)
{
  sf_blur_out_t out = { .mode = SF_EMIT_STORE, .dst = acc, .ds = 3 };
  if(wt)
  {
    out.mode = SF_EMIT_ACCUM;
    for(int c = 0; c < 3; c++) out.wt[c] = wt[c];
    out.reset = reset;
  }
  _sf_blur3_wide(src, tmp, w, h, roi_x, roi_y, sigma, &out);
}

/* Multiplicative unsharp mask on density (study b80): out = D * (D / blur(D))^amount.
   The reference (apply_multiplicative_unsharp_mask, diffusion.py) follows this
   with a per-channel scalar renormalisation that restores each channel's total
   density mass. That renormalisation is deliberately NOT reproduced here: it is
   a whole-image reduction, and this function runs on whatever ROI or tile the
   pixelpipe hands it, so the scale factor would differ between the preview pipe,
   the export pipe and every tile of a tiled export -- the same pixel would come
   out at a different density depending on how the image was cut up. The
   correction it applies is in any case tiny: measured over synthetic CMY density
   fields with grain from sigma_D = 0.01 to 0.08, the factor stays inside
   [0.9945, 1.0], i.e. under 0.03 dB. Dropping it also makes this path agree with
   spektrafilm_grain_usm in the .cl, which never had the renormalisation. */
void sf_multiplicative_unsharp_mask3(float *const buf,
                                     const int w,
                                     const int h,
                                     const float sigma,
                                     const float amount,
                                     const float *const floor_d,
                                     float *const tmp)
{
  if(sigma <= 0.0f || amount <= 0.0f) return;
  /* the mask is applied as the blur's column pass delivers each value, see
     SF_EMIT_MUSM; below SF_GAUSS_MIN_SIGMA the blurred value is the density
     itself */
  const sf_blur_out_t out = { .mode = SF_EMIT_MUSM, .dst = buf, .ds = 3,
                              .wt = { floor_d[0], floor_d[1], floor_d[2] }, .amount = amount };
  _sf_blur(buf, 3, 0, 3, tmp, w, h, sigma, SF_GAUSS_MIN_SIGMA, /*exact_only=*/1, &out);
}

/* Additive unsharp mask ([df] apply_unsharp_mask, the scanner's own sharpening
   pass): out = D + amount * (D - blur(D)). Distinct from the multiplicative one
   above, which is the grain-recovery pass in the density domain -- this runs on
   the scanned RGB, can legitimately overshoot below zero at an edge, and is
   left unclamped exactly as the reference leaves it. */
void sf_unsharp_mask3(float *const buf,
                      const int w,
                      const int h,
                      const float sigma,
                      const float amount,
                      float *const tmp)
{
  if(sigma <= 0.0f || amount <= 0.0f) return;
  const sf_blur_out_t out = { .mode = SF_EMIT_USM, .dst = buf, .ds = 3, .amount = amount };
  _sf_blur(buf, 3, 0, 3, tmp, w, h, sigma, SF_GAUSS_MIN_SIGMA, /*exact_only=*/1, &out);
}

/* Viewing glare field ([gl] compute_random_glare_amount): a lognormal field of
   linear-space mean `percent`/100 and relative standard deviation `roughness`,
   blurred by `blur` pixels. sf_sim_scan adds it in XYZ, scaled by the viewing
   illuminant, before the output matrix and the gamut compression, as
   add_glare does.

   Lognormal with linear-space mean m and std s: sigma2 = ln(1 + (s/m)^2),
   mu = ln(m) - sigma2/2, so with s/m = roughness the shape parameter does not
   depend on the amount at all. */
void sf_glare_field(float *const field,
                    const int w,
                    const int h,
                    const float percent,
                    const float roughness,
                    const float blur,
                    const int roi_x,
                    const int roi_y,
                    const uint32_t salt,
                    float *const tmp)
{
  const float mean = percent * 0.01f;
  const float sigma2 = logf(1.0f + roughness * roughness);
  const float s = sqrtf(sigma2), bias = -0.5f * sigma2;

  DT_OMP_FOR()
  for(int y = 0; y < h; y++)
    for(int x = 0; x < w; x++)
    {
      const uint32_t seed = grain_pixel_seed((uint32_t)(x + roi_x), (uint32_t)(y + roi_y), 0x5eedu, salt);
      field[(size_t)y * w + x] = mean * expf(fmaf(s, grain_normal(seed), bias));
    }
  sf_blur_plane1(field, w, h, blur, tmp);
}

/* Apply halation + scatter to a w*h*3 LINEAR plane, in place.
 *
 * Two stages, both physically motivated and run on linear irradiance:
 *   1. Scatter (the emulsion point-spread function): a narrow core Gaussian plus
 *      a wide three-Gaussian tail, mixed per channel.
 *   2. Multi-bounce halation: N reflections off the film base, each a wider
 *      Gaussian, weighted by a decaying series, mixed back per channel.
 *
 * `amount` scales the halation strength with a mild non-linearity so that 1.0 is
 * the film-accurate value (red 0.05 / green 0.015 / blue 0.0) while higher values
 * ramp up faster. `pixel_um` converts the micrometre-on-film radii to pixels. */
/* Highlight boost (spektrafilm's pre-halation highlight reconstruction). On real
   film the brightest highlights are clipped before they can scatter; this bows the
   response upward above a threshold so blown highlights carry extra energy into the
   halation/scatter that follows. Ported from spektrafilm's boost_highlights:
     raw_x0 = midgray * 2^protect_ev            (threshold; below it, unchanged)
     a      = 28^(1 - boost_range)              (curve sharpness)
     k      = (2^boost_ev - 1) / (e^(a(1-x0)) - a(1-x0) - 1)   (normaliser)
     above x0:  y = x + k*max * (e^(a*dx) - a*dx - 1),  dx=(x-x0)/max
   max is midgray * 2^(protect_ev + SF_BOOST_SPAN_EV), and above it the curve
   continues along its tangent (see sf_boost_plan_t). */
int sf_boost_build_plan(const float boost_ev,
                        const float boost_range,
                        const float protect_ev,
                        sf_boost_plan_t *plan)
{
  if(boost_ev <= 0.0f) return 0;

  /* The reference normalises this curve by max(raw) over the whole frame, so its
     brightest pixel lands exactly boost_ev stops higher. That is a whole-image
     reduction, and this function only ever sees one ROI or one tile: the preview
     pipe's downscale averages specular highlights down, so it measured a lower
     peak and applied a different curve than the export, and a tiled export got a
     different curve in every tile (tiling_callback sets overlap and factor, so
     large images do get tiled).

     Anchor the ceiling to the exposure scale instead. raw_x0 is already defined
     as a number of stops above the film's calibrated middle grey, so defining
     the ceiling the same way makes the whole curve scene-referred and identical
     everywhere. Anchoring it to the film's own shoulder was the other candidate
     and is a trap: the log exposure at 95% of curve excursion ranges from 2.5
     (Velvia) to 272 (Vision3 250D) in raw units across the shipped stocks, which
     would leave the slider nearly inert on negatives and violent on slides.

     The reference's normalisation also bounds dx to 1 - x0, which a fixed
     ceiling does not: anything above xmax would follow the bare exponential,
     overflow float within a few stops and reach the recursive blurs as inf,
     where inf - inf turns whole rows and columns into NaN. Above xmax the curve
     therefore continues along its tangent. */
  const double midgray = 0.184;
  const double rng = fmin(fmax((double)boost_range, 0.0), 1.0);
  const double prot = fmax((double)protect_ev, 0.0);
  const double raw_x0 = midgray * exp2(prot);
  const double maxv = midgray * exp2(prot + SF_BOOST_SPAN_EV);
  const double a = pow(28.0, 1.0 - rng);
  const double dxm = 1.0 - raw_x0 / maxv;
  const double e_m = exp(a * dxm);
  const double denom = e_m - a * dxm - 1.0;
  if(denom <= 0.0) return 0;
  const double gain = exp2((double)boost_ev) - 1.0;
  const double k = gain / denom;
  plan->raw_x0 = (float)raw_x0;
  plan->xmax = (float)maxv;
  plan->inv_max = (float)(1.0 / maxv);
  plan->a = (float)a;
  plan->a_log2e = (float)(a * M_LOG2E);
  plan->scale = (float)(k * maxv);
  plan->ext = (float)(gain * maxv);
  plan->slope = (float)(k * a * (e_m - 1.0));
  return 1;
}

void sf_boost_highlights(float *const raw,
                         const int w,
                         const int h,
                         const float boost_ev,
                         const float boost_range,
                         const float protect_ev)
{
  sf_boost_plan_t bp;
  if(!sf_boost_build_plan(boost_ev, boost_range, protect_ev, &bp)) return;
  const size_t nn = (size_t)w * h * 3;

  /* same expression, in the same association, as spektrafilm_boost */
  DT_OMP_FOR()
  for(size_t i = 0; i < nn; i++)
  {
    const float x = raw[i];
    if(x <= bp.raw_x0) continue;
    if(x <= bp.xmax)
    {
      const float dx = (x - bp.raw_x0) * bp.inv_max;
      const float t = bp.a * dx;
      raw[i] = fmaf(bp.scale, (grain_exp2f(bp.a_log2e * dx) - t) - 1.0f, x);
    }
    else
      raw[i] = fmaf(bp.slope, x - bp.xmax, x + bp.ext);
  }
}

void sf_halation(float *const raw,
                 const int w,
                 const int h,
                 const int roi_x,
                 const int roi_y,
                 const double pixel_um,
                 const double sc_core[3],
                 const double sc_tail[3],
                 const double w_s[3],
                 const float scatter_amount,
                 const float scatter_scale,
                 const float halation_amount,
                 const float halation_scale,
                 const double halation_strength[3],
                 const double halation_first_sigma_um,
                 float *const buf_a,
                 float *const buf_b,
                 float *const plane)
{
  if(scatter_amount <= 0.0f && halation_amount <= 0.0f) return;

  /* tail = sum of three Gaussians (amplitude, radius multiplier) */
  static const double tail_amp[3] = { 0.1633, 0.6496, 0.1870 };
  static const double tail_rat[3] = { 0.5360, 1.5236, 2.7684 };
  /* stage 1 (scatter): s_amount is the (1-s)*raw + s*scattered blend weight,
     matching upstream's scatter_amount 1:1 (no extra curve). scl is the
     shared core/tail spatial-scale multiplier.

     Clamped to [0, 1] because the blend is CONVEX: s is the fraction of photons
     that scatter, so s = 1 (upstream's own default and maximum) already means
     "all of them". Past 1 the weight on the unscattered term goes negative and
     the stage stops being a blur and starts subtracting the sharp image --
     s = 2 gives 2*scattered - raw, an inverted ghost of the subject with a dark
     halo around it, clipping to black wherever it drives raw below zero. */
  const double s_amount = fmin(fmax((double)scatter_amount, 0.0), 1.0);
  const double scl = fmax((double)scatter_scale, 1e-3);
  /* stage 2 (halation): per-channel strength at halation_amount==1.0, and the
     first-bounce radius, both per-film (sf_sim_halation_params()) since
     upstream keys these off the profile's use/antihalation tags -- e.g. a
     modern strong-AH stock scatters far less red/green back than a
     rem-jet-removed one. hscl is halation's OWN spatial-scale multiplier,
     independent from the scatter stage's scl above. halation_amount is a
     direct linear multiplier on strength here, matching upstream's
     a_tot = halation_strength * halation_amount exactly (no extra curve). */
  const double a_tot[3] = { halation_strength[0] * (double)halation_amount,
                            halation_strength[1] * (double)halation_amount,
                            halation_strength[2] * (double)halation_amount };
  const double first_sigma_um = halation_first_sigma_um; /* base bounce radius */
  const double hscl = fmax((double)halation_scale, 1e-3);
  const int n_bounces = 3; /* bounce weights decay by 0.5 each, see decay[] below */

  const size_t nn = (size_t)w * h * 3;

  /* --- stage 1: scatter PSF (core + 3-component tail) --- */
  if(s_amount > 0.0)
  {
    /* every channel has its own sigma, so each is blurred on its own, read
       straight from `raw`, which stays untouched until the combine */
    float *const tail = buf_a;
    float *const core = buf_b;
    for(int g = 0; g < 3; g++)
      for(int c = 0; c < 3; c++)
      {
        const float lt = fmaxf((float)(tail_rat[g] * (sc_tail[c] * scl / pixel_um)), 1e-6f);
        const sf_blur_out_t out = { .mode = SF_EMIT_ACCUM, .dst = tail, .ds = 3, .doff = c,
                                    .wt = { (float)tail_amp[g] }, .reset = (g == 0) };
        _sf_blur(raw, 3, c, 1, plane, w, h, lt, 1e-6f, /*exact_only=*/0, &out);
      }
    for(int c = 0; c < 3; c++)
    {
      const float sc = fmaxf((float)(sc_core[c] * scl / pixel_um), 1e-6f);
      const sf_blur_out_t out = { .mode = SF_EMIT_STORE, .dst = core, .ds = 3, .doff = c };
      _sf_blur(raw, 3, c, 1, plane, w, h, sc, 1e-6f, /*exact_only=*/0, &out);
    }
    /* float and fma-pinned, matching spektrafilm_scatter_combine */
    const float s_f = (float)s_amount;
    const float ws[3] = { (float)w_s[0], (float)w_s[1], (float)w_s[2] };
    DT_OMP_FOR()
    for(size_t i = 0; i < nn; i++)
    {
      const int c = i % 3;
      const float scattered = fmaf(ws[c], tail[i], (1.0f - ws[c]) * core[i]);
      raw[i] = fmaf(s_f, scattered - raw[i], raw[i]);
    }
  }

  /* --- stage 2: multi-bounce halation --- */
  if(halation_amount > 0.0f && (a_tot[0] > 0.0 || a_tot[1] > 0.0 || a_tot[2] > 0.0))
  {
    /* 0.5^(k-1) normalised by their sum 1.75, as float literals identical to
       spektrafilm.c's GPU host so both paths weight the bounces alike */
    static const float decay[3] = { 1.0f / 1.75f, 0.5f / 1.75f, 0.25f / 1.75f };
    const float a_f[3] = { (float)a_tot[0], (float)a_tot[1], (float)a_tot[2] };

    float *const blur = buf_a;
    for(int k = 1; k <= n_bounces; k++)
    {
      const float sk = fmaxf((float)((first_sigma_um * hscl / pixel_um) * sqrt((double)k)), 1e-6f);
      const float wk = decay[k - 1];
      const sf_blur_out_t out = { .mode = SF_EMIT_ACCUM, .dst = blur, .ds = 3,
                                  .wt = { wk, wk, wk }, .reset = (k == 1) };
      /* one sigma for all three channels: wide bounces decimate, as
         spektrafilm.c's GPU bounce loop does from the same threshold */
      if(sk >= SF_WIDE_BLUR_EFFECT_SIGMA)
        _sf_blur3_wide(raw, buf_b, w, h, roi_x, roi_y, sk, &out);
      else
        _sf_blur(raw, 3, 0, 3, buf_b, w, h, sk, 1e-6f, /*exact_only=*/0, &out);
    }
    /* float and fma-pinned, matching spektrafilm_halation_apply */
    DT_OMP_FOR()
    for(size_t i = 0; i < nn; i++)
    {
      const int c = i % 3;
      raw[i] = fmaf(a_f[c], blur[i], raw[i]) / (1.0f + a_f[c]);
    }
  }
}

/* ---------------- diffusion filter (Black Pro-Mist family) ----------------
 *
 * spektrafilm's diffusion filter is an energy-conserving scatter:
 *   E_out = (1 - p_s) * E_in + p_s * (K_s * E_in)
 * where the per-channel PSF K_s is a sum of radial exponentials grouped into
 * core / halo / bloom. Each exponential exp(-r/lambda)/(2*pi*lambda^2) has
 * per-axis sigma lambda*sqrt(3); we approximate each as a Gaussian of that sigma so
 * the whole PSF becomes a weighted bank of Gaussian blurs (_sf_blur, this
 * file's own exact direct convolution), summed per channel. The strength->p_s
 * table, geometric lambda progressions, group weights and warmth
 * redistribution are ported exactly from spektrafilm; only
 * the exponential->Gaussian per-component shape is an approximation (a soft
 * diffusion halo is dominated by scale, not tail shape). */

#define SF_DIFFUSION_MAX_COMP 4

typedef struct sf_diff_group_t
{
  double lambda_um;
  double spread;
  int n;
  double alpha; /* bloom only; <=0 = uniform weights */
} sf_diff_group_t;

typedef struct sf_diff_family_t
{
  sf_diff_group_t core, halo, bloom;
  double w_c, w_h, w_b;
  double total_gain; /* family scatter gain in strength->p_s */
  double halo_warmth_base; /* per-family halo warmth bias, added to the
                               user's own warmth slider before redistribution
                               (spektrafilm's DIFFUSION_FILTER_SHAPES
                               halo_warmth_base) */
} sf_diff_family_t;

/* All four families spektrafilm ships, values ported exactly from
   model/diffusion.py's _DIFFUSION_FILTER_SHAPES / _DIFFUSION_FAMILY_TOTAL_GAIN. */
static const sf_diff_family_t SF_FAMILY_GLIMMERGLASS = {
  { 10.0, 1.5, 2, 0.0 }, { 50.0, 2.0, 3, 0.0 }, { 260.0, 2.5, 4, 3.2 },
  0.60, 0.30, 0.10, 0.65, 0.0
};
/* Black Pro-Mist (the app default family). */
static const sf_diff_family_t SF_FAMILY_BPM = {
  { 16.0, 1.5, 2, 0.0 }, { 95.0, 2.0, 3, 0.0 }, { 380.0, 2.5, 4, 3.5 },
  0.40, 0.47, 0.13, 0.75, 0.65
};
/* Classic Pro-Mist. */
static const sf_diff_family_t SF_FAMILY_PRO_MIST = {
  { 14.0, 1.5, 2, 0.0 }, { 150.0, 2.0, 3, 0.0 }, { 650.0, 2.5, 4, 2.9 },
  0.28, 0.42, 0.30, 1.05, 0.40
};
static const sf_diff_family_t SF_FAMILY_CINEBLOOM = {
  { 20.0, 1.5, 2, 0.0 }, { 200.0, 2.0, 3, 0.0 }, { 1000.0, 2.5, 4, 2.5 },
  0.22, 0.30, 0.48, 1.00, 0.85
};
/* Index order must match dt_iop_spektrafilm_diffusion_family_t in spektrafilm.c. */
static const sf_diff_family_t *const SF_DIFF_FAMILIES[4] = {
  &SF_FAMILY_BPM, &SF_FAMILY_GLIMMERGLASS, &SF_FAMILY_PRO_MIST, &SF_FAMILY_CINEBLOOM
};

static const double SF_DIFF_BREAKS[5] = { 0.125, 0.25, 0.5, 1.0, 2.0 };
static const double SF_DIFF_FRAC[5] = { 0.10, 0.20, 0.35, 0.55, 0.75 };
static const double SF_HALO_WARMTH_AXIS[3] = { 1.30, 0.15, -1.45 };

/* strength -> deflected fraction p_s (log2-interpolated table * family gain) */
static double sf_diff_strength_to_ps(double strength,
                                     const sf_diff_family_t *fam)
{
  if(strength <= 0.0) return 0.0;
  const double ls = log2(fmax(strength, 1e-6));
  double base;
  /* Below the first tabulated stop there is nothing to interpolate towards, so
     ramp linearly in strength down to zero instead of holding the table's first
     value. Holding it makes every strength under 1/8 deflect the same tenth of
     the light, and makes the filter jump from off to that tenth the moment the
     value leaves zero -- most visible at large spatial scales, where the tenth
     is spread widest. The ramp meets the table exactly at 1/8, so every
     tabulated stop is unchanged. */
  if(ls <= log2(SF_DIFF_BREAKS[0]))
    base = SF_DIFF_FRAC[0] * strength / SF_DIFF_BREAKS[0];
  else if(ls >= log2(SF_DIFF_BREAKS[4])) base = SF_DIFF_FRAC[4];
  else
  {
    base = SF_DIFF_FRAC[4];
    for(int i = 0; i < 4; i++)
    {
      const double lo = log2(SF_DIFF_BREAKS[i]), hi = log2(SF_DIFF_BREAKS[i + 1]);
      if(ls >= lo && ls <= hi)
      {
        const double t = (ls - lo) / (hi - lo);
        base = SF_DIFF_FRAC[i] + t * (SF_DIFF_FRAC[i + 1] - SF_DIFF_FRAC[i]);
        break;
      }
    }
  }
  return fmin(fmax(base * fam->total_gain, 0.0), 0.99);
}

/* expand a group into (lambda_um[], weight[]) summing to 1; returns count */
static int sf_diff_expand(const sf_diff_group_t *g,
                          const char is_bloom,
                          double lam[SF_DIFFUSION_MAX_COMP],
                          double wgt[SF_DIFFUSION_MAX_COMP])
{
  int n = g->n < 1 ? 1 : (g->n > SF_DIFFUSION_MAX_COMP ? SF_DIFFUSION_MAX_COMP : g->n);
  if(n == 1 || g->spread <= 1.0)
  {
    lam[0] = g->lambda_um;
    wgt[0] = 1.0;
    return 1;
  }
  const double llo = log(g->lambda_um / g->spread), lhi = log(g->lambda_um * g->spread);
  double wsum = 0.0;
  for(int k = 0; k < n; k++)
  {
    lam[k] = exp(llo + (lhi - llo) * k / (n - 1));
    wgt[k] = is_bloom ? pow(lam[k], 2.0 - g->alpha) : 1.0;
    wsum += wgt[k];
  }
  for(int k = 0; k < n; k++) wgt[k] /= wsum;
  return n;
}

/* per-channel halo weights after energy-conserving warmth redistribution */
static void sf_diff_halo_warmth(const double *wgt,
                                int n,
                                double warmth,
                                double out[3][SF_DIFFUSION_MAX_COMP])
{
  if(n < 2)
  {
    for(int c = 0; c < 3; c++)
      for(int k = 0; k < n; k++) out[c][k] = wgt[k];
    return;
  }
  warmth = fmin(fmax(warmth, -1.5), 1.5);
  double g[SF_DIFFUSION_MAX_COMP], gmean = 0.0, tt = 0.0;
  for(int k = 0; k < n; k++)
  {
    g[k] = -1.0 + 2.0 * k / (n - 1);
    gmean += wgt[k] * g[k];
    tt += wgt[k];
  }
  gmean /= tt; /* weighted mean, to re-centre */
  for(int k = 0; k < n; k++) g[k] -= gmean;
  for(int c = 0; c < 3; c++)
  {
    double s = 0.0, raw[SF_DIFFUSION_MAX_COMP];
    for(int k = 0; k < n; k++)
    {
      raw[k] = wgt[k] * (1.0 + warmth * SF_HALO_WARMTH_AXIS[c] * g[k]);
      if(raw[k] < 0.0) raw[k] = 0.0;
      s += raw[k];
    }
    for(int k = 0; k < n; k++) out[c][k] = (s > 0.0) ? raw[k] * (tt / s) : wgt[k];
  }
}

/* Build the shared Gaussian bank (used by both CPU and GPU). */
int sf_diffusion_build_plan(int family,
                            float strength,
                            float halo_warmth,
                            sf_diffusion_plan_t *plan)
{
  plan->n = 0;
  plan->p_s = 0.0f;
  const int nfam = (int)(sizeof(SF_DIFF_FAMILIES) / sizeof(SF_DIFF_FAMILIES[0]));
  const sf_diff_family_t *fam = SF_DIFF_FAMILIES[(family >= 0 && family < nfam) ? family : 0];
  const double p_s = sf_diff_strength_to_ps((double)strength, fam);
  if(p_s <= 0.0) return 0;

  double clam[SF_DIFFUSION_MAX_COMP], cw[SF_DIFFUSION_MAX_COMP];
  double hlam[SF_DIFFUSION_MAX_COMP], hw[SF_DIFFUSION_MAX_COMP];
  double blam[SF_DIFFUSION_MAX_COMP], bw[SF_DIFFUSION_MAX_COMP];
  const int nc = sf_diff_expand(&fam->core, 0, clam, cw);
  const int nh = sf_diff_expand(&fam->halo, 0, hlam, hw);
  const int nb = sf_diff_expand(&fam->bloom, 1, blam, bw);
  double hch[3][SF_DIFFUSION_MAX_COMP];
  /* effective_warmth = family base + user knob, matching
     diffusion_filter_radial_profile()'s own "cfg base + halo_warmth". */
  sf_diff_halo_warmth(hw, nh, fam->halo_warmth_base + (double)halo_warmth, hch);

  /* Moment-matched Gaussian surrogate for one 2D isotropic exponential
     exp(-r/lambda) / (2*pi*lambda^2). That kernel has E[r^2] = 6*lambda^2, and a
     2D Gaussian of per-axis sigma has E[r^2] = 2*sigma^2, so the second moments
     match at sigma = lambda*sqrt(3). (The reference's own exponential surrogate
     agrees: the SF_EXPTAIL_* mixture satisfies sum_k a_k * r_k^2 = 2.988 ~ 3.) */
  const double L2 = 1.7320508075688772; /* sqrt(3) */
  int idx = 0;
  for(int k = 0; k < nc; k++) /* core: channel-independent */
  {
    plan->sigma_um[idx] = (float)(clam[k] * L2);
    plan->wr[idx] = plan->wg[idx] = plan->wb[idx] = (float)(fam->w_c * cw[k]);
    idx++;
  }
  for(int k = 0; k < nh; k++) /* halo: per channel (warmth) */
  {
    plan->sigma_um[idx] = (float)(hlam[k] * L2);
    plan->wr[idx] = (float)(fam->w_h * hch[0][k]);
    plan->wg[idx] = (float)(fam->w_h * hch[1][k]);
    plan->wb[idx] = (float)(fam->w_h * hch[2][k]);
    idx++;
  }
  for(int k = 0; k < nb; k++) /* bloom: channel-independent */
  {
    plan->sigma_um[idx] = (float)(blam[k] * L2);
    plan->wr[idx] = plan->wg[idx] = plan->wb[idx] = (float)(fam->w_b * bw[k]);
    idx++;
  }
  plan->n = idx;
  plan->p_s = (float)p_s;
  return 1;
}

/* Apply the diffusion filter in place on a linear w*h*3 plane. `acc` and
   `tmp` are w*h*3 scratch buffers. */
void sf_diffusion_filter(float *const raw,
                         const int w,
                         const int h,
                         const int roi_x,
                         const int roi_y,
                         const double pixel_um,
                         const int family,
                         const float strength,
                         const float spatial_scale,
                         const float halo_warmth,
                         float *const acc,
                         float *const tmp)
{
  if(strength <= 0.0f || spatial_scale <= 0.0f) return;
  sf_diffusion_plan_t plan;
  if(!sf_diffusion_build_plan(family, strength, halo_warmth, &plan) || plan.p_s <= 0.0f) return;

  const double sc = fmax((double)spatial_scale, 1e-6);
  const size_t nn = (size_t)w * h * 3;

  for(int j = 0; j < plan.n; j++)
  {
    const float sigma = (float)(plan.sigma_um[j] * sc / fmax(pixel_um, 1e-3));
    /* fma-pinned in the blur's column pass, matching
       spektrafilm_diffusion_accum */
    const sf_blur_out_t out = { .mode = SF_EMIT_ACCUM, .dst = acc, .ds = 3,
                                .wt = { plan.wr[j], plan.wg[j], plan.wb[j] },
                                .reset = (j == 0) };
    /* wide components decimate, as spektrafilm.c's GPU bank loop does from
       the same threshold */
    if(sigma >= SF_WIDE_BLUR_EFFECT_SIGMA)
      _sf_blur3_wide(raw, tmp, w, h, roi_x, roi_y, sigma, &out);
    else
      _sf_blur(raw, 3, 0, 3, tmp, w, h, sigma, 1e-6f, /*exact_only=*/0, &out);
  }

  /* fma-pinned, matching spektrafilm_diffusion_mix */
  const float ps = plan.p_s;
  DT_OMP_FOR()
  for(size_t i = 0; i < nn; i++) raw[i] = fmaf(ps, acc[i], (1.0f - ps) * raw[i]);
}
