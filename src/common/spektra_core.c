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
 * for a proper direct Gaussian convolution (see _sf_gauss_kernel_1d below): a
 * truncated, normalized kernel applied separably (row pass, transpose, row
 * pass, transpose back), matching what the reference spektrafilm's own
 * Gaussian blur actually does, rather than a recursive IIR approximation whose
 * effective blur radius measurably departs from the requested sigma. Boundary
 * handling is clamp-to-edge.
 */

#include "common/darktable.h"
#include "common/imagebuf.h"

#include <math.h>
#include <string.h>

/* The bundle-loader half of spektra_core.h needs file-IO/locale helpers. darktable
   poisons bare libc fopen, so map them to glib here (this .c does not itself read
   bundles, but the shared header must compile in this translation unit). */
#include <glib/gstdio.h>
/* whole-file slurp via glib (g_file_get_contents returns a NUL-terminated,
   g_free-owned buffer); maps the header's SF_READ_FILE/SF_FREE_FILE. */
#define SF_READ_FILE(path, out, len) \
  (g_file_get_contents((path), (out), (len), NULL) ? 0 : -1)
#define SF_FREE_FILE(buf) g_free(buf)
#define SF_STRTOD(s, end) g_ascii_strtod((s), (end))
#include "spektra_core.h"

/* ------------------------------------------------------------------------ */
/* Row-major Gaussian blur with transpose                                  */
/* ------------------------------------------------------------------------ */
/* A naive column pass has stride = width*4 bytes per pixel access, striding
   beyond cache-line reach at any realistic resolution. Instead:
   1. Row-pass (direct convolution) — good stride, stays in cache
   2. Cache-blocked transpose — temp → scratch, sequential access both ways
   3. Row-pass again on transposed data — second dimension, also good stride
   4. Transpose back
   The transpose itself is tiled so it stays in L2. */

/* Maximum kernel half-width: see SF_GAUSS_MAX_RADIUS in spektra_core.h. */

/* Build a normalized, truncated 1D Gaussian kernel -- truncate=4 sigma,
   matching scipy.ndimage.gaussian_filter's own default (what the reference
   spektrafilm actually blurs with) -- so this blur is exact to kernel-
   truncation precision (~1e-7 relative error at 4 sigma) for ANY sigma.
   This replaces a recursive (Young-van Vliet IIR) approximation, whose
   actual blur radius measurably departs from the sigma it's asked for --
   confirmed by simulating its impulse response and comparing the resulting
   second moment to the requested sigma: the discrepancy is a stable ~18%
   too wide for sigma above ~1.5px, but the error changes character (and can
   flip to too NARROW) below that, exactly the small-sigma range scatter's
   core/tail and grain's clump blur typically fall into. A direct kernel has
   no such regime-dependent error to chase. `kernel` must have room for
   2*max_radius+1 taps; returns the radius actually used. */
int sf_gauss_kernel_1d(const float sigma, float *const kernel, const int max_radius)
{
  int radius = (int)ceilf(4.0f * sigma);
  if(radius < 1) radius = 1;
  if(radius > max_radius) radius = max_radius;
  double sum = 0.0;
  const double inv2s2 = 1.0 / (2.0 * (double)sigma * (double)sigma);
  for(int i = -radius; i <= radius; i++)
  {
    const double v = exp(-(double)(i * i) * inv2s2);
    kernel[i + radius] = (float)v;
    sum += v;
  }
  const float invsum = (float)(1.0 / sum);
  for(int i = 0; i < 2 * radius + 1; i++) kernel[i] *= invsum;
  return radius;
}

/* Exact grain-clump renormalization: blurring the grain-delta buffer
   necessarily reduces its variance (that's what turns per-pixel noise into
   soft "clumps"), so the result needs scaling back up to keep the grain at
   its intended RMS after clumping. For a normalized kernel k, convolving
   white noise reduces variance by sum(k^2) along that axis; since the clump
   blur is separable (the same kernel applied along both axes), the full 2D
   variance factor is sum(k^2)^2, and the amplitude factor that undoes it is
   1/sum(k^2). Computed directly from the same kernel actually used for the
   blur, so this is exact rather than an assumed closed-form approximation. */
float sf_gauss_grain_renorm(const float sigma)
{
  if(sigma < 1e-6f) return 1.0f;
  float kernel[2 * SF_GAUSS_MAX_RADIUS + 1];
  const int radius = sf_gauss_kernel_1d(sigma, kernel, SF_GAUSS_MAX_RADIUS);
  double ss = 0.0;
  for(int i = 0; i < 2 * radius + 1; i++) ss += (double)kernel[i] * kernel[i];
  return (float)(1.0 / ss);
}

/* Direct 1D convolution along stride-1 elements, clamp-to-edge boundary (a
   plain "hold the edge value" extension -- the exact boundary rule matters
   far less than the kernel itself once the kernel is exact). `in` and
   `out` must NOT alias. */
static void _sf_gauss_convolve_1d(const float *const in, float *const out, const int len,
                                  const float *const kernel, const int radius)
{
  for(int x = 0; x < len; x++)
  {
    float acc = 0.0f;
    for(int k = -radius; k <= radius; k++)
    {
      int xx = x + k;
      xx = xx < 0 ? 0 : (xx >= len ? len - 1 : xx);
      acc += kernel[k + radius] * in[xx];
    }
    out[x] = acc;
  }
}

/* Cache-blocked transpose of a width×height float buffer to height×width.
   Using BLOCK=64 keeps read/write working sets in L1/L2 during the pass. */
#define SF_TRANSPOSE_BLOCK 64
static void _sf_transpose(const float *const src, float *const dst,
                          const int w, const int h)
{
  DT_OMP_FOR()
  for(int j0 = 0; j0 < h; j0 += SF_TRANSPOSE_BLOCK)
  {
    const int jlim = (j0 + SF_TRANSPOSE_BLOCK < h) ? j0 + SF_TRANSPOSE_BLOCK : h;
    for(int i0 = 0; i0 < w; i0 += SF_TRANSPOSE_BLOCK)
    {
      const int ilim = (i0 + SF_TRANSPOSE_BLOCK < w) ? i0 + SF_TRANSPOSE_BLOCK : w;
      for(int j = j0; j < jlim; j++)
        for(int i = i0; i < ilim; i++)
          dst[(size_t)i * h + j] = src[(size_t)j * w + i];
    }
  }
}

/* Single-channel exact Gaussian blur with cache-blocked transpose. `trans`
   is a w*h intermediate buffer for the transpose (may be NULL: the row/col
   passes then operate directly without the transpose, which is fine for
   small buffers where cache locality matters less). */
static void _blur_channel(float *const buf, const int w, const int h, const int c,
                          const float sigma, float *const plane, float *const trans)
{
  if(sigma < 1e-6f) return;
  const size_t npix = (size_t)w * h;
  for(size_t i = 0; i < npix; i++) plane[i] = buf[i * 3 + c];

  float kernel[2 * SF_GAUSS_MAX_RADIUS + 1];
  const int radius = sf_gauss_kernel_1d(sigma, kernel, SF_GAUSS_MAX_RADIUS);

  if(trans && w >= 16 && h >= 16)
  {
    float *const temp = trans;
    /* Pass 1: row-major on each row -> temp */
    DT_OMP_FOR()
    for(int j = 0; j < h; j++)
    {
      const size_t off = (size_t)j * w;
      _sf_gauss_convolve_1d(plane + off, temp + off, w, kernel, radius);
    }
    /* Transpose temp (w×h) -> plane (h×w) */
    _sf_transpose(temp, plane, w, h);
    /* Pass 2: row-major on transposed data -> temp */
    DT_OMP_FOR()
    for(int j = 0; j < w; j++)
    {
      const size_t off = (size_t)j * h;
      _sf_gauss_convolve_1d(plane + off, temp + off, h, kernel, radius);
    }
    /* Transpose back temp (h×w) -> plane (w×h) */
    _sf_transpose(temp, plane, h, w);
  }
  else
  {
    /* small buffer: skip the cache-blocking transpose, convolve directly */
    float *const row_tmp = dt_alloc_align_float((size_t)MAX(w, h));
    if(row_tmp)
    {
      for(int j = 0; j < h; j++)
      {
        _sf_gauss_convolve_1d(plane + (size_t)j * w, row_tmp, w, kernel, radius);
        memcpy(plane + (size_t)j * w, row_tmp, sizeof(float) * w);
      }
      float *const col_in = dt_alloc_align_float((size_t)h);
      float *const col_out = dt_alloc_align_float((size_t)h);
      if(col_in && col_out)
      {
        for(int i = 0; i < w; i++)
        {
          for(int j = 0; j < h; j++) col_in[j] = plane[(size_t)j * w + i];
          _sf_gauss_convolve_1d(col_in, col_out, h, kernel, radius);
          for(int j = 0; j < h; j++) plane[(size_t)j * w + i] = col_out[j];
        }
      }
      dt_free_align(col_in);
      dt_free_align(col_out);
    }
    dt_free_align(row_tmp);
  }
  for(size_t i = 0; i < npix; i++) buf[i * 3 + c] = plane[i];
}

/* Blur all three channels with the same sigma (grain). Allocates trans buffer. */
void sf_blur_plane3(float *const buf, const int w, const int h, const float sigma, float *const plane)
{
  if(sigma < 0.3f) return;
  float *const trans = dt_alloc_align_float((size_t)w * h);
  for(int c = 0; c < 3; c++) _blur_channel(buf, w, h, c, sigma, plane, trans);
  dt_free_align(trans);
}

/* Blur packed buffer with per-channel sigma. `trans` is a w*h intermediate. */
static void _blur_per_channel(float *const buf, const int w, const int h, const float sigma[3],
                               float *const plane, float *const trans)
{
  for(int c = 0; c < 3; c++) _blur_channel(buf, w, h, c, sigma[c], plane, trans);
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
   Operates in place on a linear w*h*3 plane; max is the plane's peak value. */
void sf_boost_highlights(float *const raw, const int w, const int h, const float boost_ev,
                         const float boost_range, const float protect_ev)
{
  if(boost_ev <= 0.0f) return;
  const size_t nn = (size_t)w * h * 3;
  float maxv = 0.0f;
  for(size_t i = 0; i < nn; i++) maxv = fmaxf(maxv, raw[i]);
  if(maxv <= 0.0f) return;

  const float midgray = 0.184f;
  const float rng = fminf(fmaxf(boost_range, 0.0f), 1.0f);
  float raw_x0 = midgray * exp2f(fmaxf(protect_ev, 0.0f));
  if(raw_x0 > maxv) return; /* threshold above peak: nothing to boost */
  const float a = powf(28.0f, 1.0f - rng);
  const float x0 = raw_x0 / maxv;
  const float denom = expf(a * (1.0f - x0)) - a * (1.0f - x0) - 1.0f;
  if(denom <= 0.0f) return;
  const float k = (exp2f(boost_ev) - 1.0f) / denom;
  const float inv_max = 1.0f / maxv, boost_scale = k * maxv;

  for(size_t i = 0; i < nn; i++)
  {
    const float x = raw[i];
    if(x > raw_x0)
    {
      const float dx = (x - raw_x0) * inv_max;
      raw[i] = x + boost_scale * (expf(a * dx) - a * dx - 1.0f);
    }
  }
}

void sf_halation(float *const raw, const int w, const int h, const double pixel_um,
                 const float scatter_amount, const float scatter_scale,
                 const float halation_amount, const float halation_scale,
                 const double halation_strength[3], const double halation_first_sigma_um)
{
  if(scatter_amount <= 0.0f && halation_amount <= 0.0f) return;

  /* per-channel scatter radii (um on film) and core/tail mix weights. These
     are the reference model's schema defaults (upstream HalationParams) and,
     unlike halation_strength/halation_first_sigma_um below, are NOT varied
     per film stock upstream (every (use, antihalation) preset leaves them
     alone), so they stay fixed constants here too. */
  static const double sc_core[3] = { 2.2, 2.0, 1.6 };
  static const double sc_tail[3] = { 9.3, 9.7, 9.1 };
  static const double w_s[3]     = { 0.78, 0.65, 0.67 };
  /* tail = sum of three Gaussians (amplitude, radius multiplier) */
  static const double tail_amp[3] = { 0.1633, 0.6496, 0.1870 };
  static const double tail_rat[3] = { 0.5360, 1.5236, 2.7684 };
  /* stage 1 (scatter): s_amount is the (1-s)*raw + s*scattered blend weight,
     matching upstream's scatter_amount 1:1 (no extra curve). scl is the
     shared core/tail spatial-scale multiplier. */
  const double s_amount = (double)scatter_amount;
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
  const int n_bounces = 3;
  const double rho = 0.5;             /* bounce decay */

  const size_t npix = (size_t)w * h;
  const size_t nn = npix * 3;
  float *const plane = dt_alloc_align_float(npix);
  float *const trans = dt_alloc_align_float(npix);
  if(!plane) { dt_free_align(trans); return; }

  /* --- stage 1: scatter PSF (core + 3-component tail) --- */
  if(s_amount > 0.0)
  {
    float *const core = dt_alloc_align_float(nn);
    float *const tail = dt_alloc_align_float(nn);
    float *const comp = dt_alloc_align_float(nn);
    if(core && tail && comp)
    {
      dt_iop_image_copy(core, raw, nn);
      float sc[3];
      for(int c = 0; c < 3; c++) sc[c] = fmaxf((float)(sc_core[c] * scl / pixel_um), 1e-6f);
      _blur_per_channel(core, w, h, sc, plane, trans);

      memset(tail, 0, sizeof(float) * nn);
      for(int g = 0; g < 3; g++)
      {
        dt_iop_image_copy(comp, raw, nn);
        float lt[3];
        for(int c = 0; c < 3; c++)
          lt[c] = fmaxf((float)(tail_rat[g] * (sc_tail[c] * scl / pixel_um)), 1e-6f);
        _blur_per_channel(comp, w, h, lt, plane, trans);
        for(size_t i = 0; i < nn; i++) tail[i] += (float)tail_amp[g] * comp[i];
      }
      for(size_t i = 0; i < nn; i++)
      {
        const int c = i % 3;
        const double scattered = (1.0 - w_s[c]) * core[i] + w_s[c] * tail[i];
        raw[i] = (float)((1.0 - s_amount) * (double)raw[i] + s_amount * scattered);
      }
    }
    dt_free_align(core);
    dt_free_align(tail);
    dt_free_align(comp);
  }

  /* --- stage 2: multi-bounce halation --- */
  if(halation_amount > 0.0f && (a_tot[0] > 0.0 || a_tot[1] > 0.0 || a_tot[2] > 0.0))
  {
    double decay[8], dsum = 0.0;
    for(int k = 1; k <= n_bounces; k++)
    {
      decay[k - 1] = pow(rho, k - 1);
      dsum += decay[k - 1];
    }
    for(int k = 0; k < n_bounces; k++) decay[k] /= dsum;

    float *const blur = dt_alloc_align_float(nn);
    float *const comp = dt_alloc_align_float(nn);
    if(blur && comp)
    {
      memset(blur, 0, sizeof(float) * nn);
      for(int k = 1; k <= n_bounces; k++)
      {
        dt_iop_image_copy(comp, raw, nn);
        const float sk = fmaxf((float)((first_sigma_um * hscl / pixel_um) * sqrt((double)k)), 1e-6f);
        const float sig3[3] = { sk, sk, sk };
        _blur_per_channel(comp, w, h, sig3, plane, trans);
        const float wk = (float)decay[k - 1];
        for(size_t i = 0; i < nn; i++) blur[i] += wk * comp[i];
      }
      for(size_t i = 0; i < nn; i++)
      {
        const int c = i % 3;
        raw[i] = (float)((raw[i] + a_tot[c] * blur[i]) / (1.0 + a_tot[c]));
      }
    }
    dt_free_align(blur);
    dt_free_align(comp);
  }

  dt_free_align(plane);
  dt_free_align(trans);
}

/* ---------------- diffusion filter (Black Pro-Mist family) ----------------
 *
 * spektrafilm's diffusion filter is an energy-conserving scatter:
 *   E_out = (1 - p_s) * E_in + p_s * (K_s * E_in)
 * where the per-channel PSF K_s is a sum of radial exponentials grouped into
 * core / halo / bloom. Each exponential exp(-r/lambda)/(2*pi*lambda^2) has
 * radial RMS lambda*sqrt(2); we approximate each as a Gaussian of that sigma so
 * the whole PSF becomes a weighted bank of Gaussian blurs (_blur_channel, this
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
static double sf_diff_strength_to_ps(double strength, const sf_diff_family_t *fam)
{
  if(strength <= 0.0) return 0.0;
  const double ls = log2(fmax(strength, 1e-6));
  double base;
  if(ls <= log2(SF_DIFF_BREAKS[0])) base = SF_DIFF_FRAC[0];
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
static int sf_diff_expand(const sf_diff_group_t *g, const char is_bloom, double lam[SF_DIFFUSION_MAX_COMP],
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
static void sf_diff_halo_warmth(const double *wgt, int n, double warmth, double out[3][SF_DIFFUSION_MAX_COMP])
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
int sf_diffusion_build_plan(int family, float strength, float halo_warmth, sf_diffusion_plan_t *plan)
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

  const double L2 = 1.4142135623730951; /* exp(-r/lambda) ~ Gaussian sigma=lambda*sqrt(2) */
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

/* Apply the diffusion filter in place on a linear w*h*3 plane. */
void sf_diffusion_filter(float *const raw, const int w, const int h, const double pixel_um,
                         const int family, const float strength, const float spatial_scale,
                         const float halo_warmth)
{
  if(strength <= 0.0f || spatial_scale <= 0.0f) return;
  sf_diffusion_plan_t plan;
  if(!sf_diffusion_build_plan(family, strength, halo_warmth, &plan) || plan.p_s <= 0.0f) return;

  const double sc = fmax((double)spatial_scale, 1e-6);
  const size_t npix = (size_t)w * h, nn = npix * 3;

  float *const acc = dt_alloc_align_float(nn);
  float *const comp = dt_alloc_align_float(nn);
  float *const plane1 = dt_alloc_align_float(npix);
  float *const trans = dt_alloc_align_float(npix);
  if(!acc || !comp || !plane1)
  {
    dt_free_align(acc);
    dt_free_align(comp);
    dt_free_align(plane1);
    dt_free_align(trans);
    return;
  }
  memset(acc, 0, sizeof(float) * nn);

  for(int j = 0; j < plan.n; j++)
  {
    const float sigma = (float)(plan.sigma_um[j] * sc / fmax(pixel_um, 1e-3));
    dt_iop_image_copy(comp, raw, nn);
    for(int c = 0; c < 3; c++) _blur_channel(comp, w, h, c, sigma, plane1, trans);
    const float wr = plan.wr[j], wg = plan.wg[j], wb = plan.wb[j];
    for(size_t i = 0; i < npix; i++)
    {
      acc[i * 3 + 0] += wr * comp[i * 3 + 0];
      acc[i * 3 + 1] += wg * comp[i * 3 + 1];
      acc[i * 3 + 2] += wb * comp[i * 3 + 2];
    }
  }

  const float ps = plan.p_s;
  for(size_t i = 0; i < nn; i++) raw[i] = (1.0f - ps) * raw[i] + ps * acc[i];

  dt_free_align(acc);
  dt_free_align(comp);
  dt_free_align(plane1);
  dt_free_align(trans);
}
