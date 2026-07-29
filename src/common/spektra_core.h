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

#pragma once
#include <math.h>
#include <stdint.h>

#ifndef SPEKTRA_INLINE
#define SPEKTRA_INLINE static inline
#endif

/* Spatial effects implemented in spektra_core.c (they need dt_alloc_align_float
   and OpenMP linkage; everything else in this header is inline). */
void sf_blur_plane3(float *buf, int w, int h, float sigma, float *plane);
void sf_blur_plane3_fast(float *buf, int w, int h, float sigma, float *plane);
/* Same exact-kernel blur as sf_blur_plane3, but operating directly on a
   single flat w*h buffer (no 3-channel interleave) -- used for the
   per-sublayer dye-cloud blur inside grain generation, where each
   (channel, sub-layer) has its own sigma and needs its own buffer rather
   than sharing one interleaved 3-channel pass. No lower sigma cutoff
   (unlike sf_blur_plane3's 0.3px guard for the visible clump blur): the
   dye-cloud sigma is often well under a pixel and still meaningfully
   softens the raw particle draw, matching upstream's plain `> 0` check. */
void sf_blur_plane1(float *buf, int w, int h, float sigma, float *plane, float *trans);
/* Additive unsharp mask on the scanned RGB ([df] apply_unsharp_mask):
   out = D + amount * (D - blur(D)). `orig` and `work` are w*h*3 and w*h
   scratch buffers supplied by the caller. */
void sf_unsharp_mask3(float *buf, int w, int h, float sigma, float amount,
                      float *orig, float *work);
/* Viewing-glare veil ([gl] add_glare): adds a blurred lognormal field of mean
   percent/100 (relative std `roughness`) to all three channels. `field` is a
   w*h scratch buffer; roi_x/roi_y are absolute image coordinates so the veil
   is stable under pan and zoom. */
void sf_glare(float *rgb, int w, int h, float percent, float roughness, float blur,
              int roi_x, int roi_y, float *field);

void sf_multiplicative_unsharp_mask3(float *buf, int w, int h, float sigma, float amount,
                                     float *orig, float *work);
/* Two independently-controllable stages, matching upstream's HalationParams:
 *   scatter_amount / scatter_scale   -- stage 1, in-emulsion core+tail scatter
 *   halation_amount / halation_scale -- stage 2, back-reflection multi-bounce
 * halation_strength: per-channel (R,G,B) back-reflection strength at
 * halation_amount==1.0; halation_first_sigma_um: first-bounce Gaussian radius
 * in micrometres. Both come from sf_sim_halation_params() — per-film when the
 * pack provides film_render_defaults[stock].halation, otherwise the generic
 * still/strong-antihalation baseline. */
/* `sc_core` / `sc_tail` / `w_s` are the per-channel in-emulsion scatter PSF from
   sf_sim_scatter_params(): Gaussian core radius and exponential tail decay in
   micrometres on film, and the core/tail mix weight. Already collapsed to one
   value per channel for a single-emulsion stock, and already clamped by the
   caller to what the ROI padding covers. */
void sf_halation(float *raw, int w, int h, double pixel_um, const double sc_core[3],
                 const double sc_tail[3], const double w_s[3], float scatter_amount,
                 float scatter_scale, float halation_amount, float halation_scale,
                 const double halation_strength[3], double halation_first_sigma_um);
/* Stops of headroom above the protect threshold over which the boost is spread.
   The reference spreads it from the threshold up to the frame's own peak; that
   peak is a whole-image reduction, which a per-ROI/per-tile pixelpipe cannot
   reproduce consistently. A fixed span keeps the curve identical in every tile
   and in both pipes, and puts both controls on the same footing -- protect_ev
   sets where the boost starts, this sets how far above that it reaches full
   strength. 4 EV matches the reference's typical frame peak for a normally
   exposed scene (midgray + 4-6 EV). */
#define SF_BOOST_SPAN_EV 4.0f
void sf_boost_highlights(float *raw, int w, int h, float boost_ev, float boost_range,
                         float protect_ev);
void sf_diffusion_filter(float *raw, int w, int h, double pixel_um, int family, float strength,
                         float spatial_scale, float halo_warmth);

/* Diffusion-filter Gaussian bank, built host-side and consumed by the GPU path
   (the CPU path builds it internally). Each entry is one Gaussian blur of the
   linear plane, with a per-channel weight; the scattered image is their sum, and
   the final mix is (1-p_s)*in + p_s*scatter. */
#define SF_DIFFUSION_MAX_BANK 11  /* core(2) + halo(3) + bloom(4) + margin */
typedef struct sf_diffusion_plan_t
{
  int n;                              /* number of Gaussian components */
  float sigma_um[SF_DIFFUSION_MAX_BANK];   /* blur sigma in micrometres (×scale/pixel = px) */
  float wr[SF_DIFFUSION_MAX_BANK];    /* per-channel weight (already ×group weight) */
  float wg[SF_DIFFUSION_MAX_BANK];
  float wb[SF_DIFFUSION_MAX_BANK];
  float p_s;                          /* scatter fraction */
} sf_diffusion_plan_t;

/* Fill `plan` for the given strength/warmth. Returns 0 and sets plan->p_s=0 when
   the filter is a no-op. spatial_scale/pixel are applied by the caller (sigma_px
   = sigma_um * spatial_scale / pixel_um). */
int sf_diffusion_build_plan(int family, float strength, float halo_warmth, sf_diffusion_plan_t *plan);


SPEKTRA_INLINE float sf_clampf(float x, float lo, float hi)
{
  return x < lo ? lo : (x > hi ? hi : x);
}

/* ---------------- grain (validated) ----------------
 *
 * Grain must be random per pixel yet perfectly reproducible (stable under
 * re-render, pan and zoom, and identical on CPU and GPU). So instead of a
 * stateful PRNG we use a stateless integer HASH keyed on the pixel coordinates:
 * hash(x, y, channel) -> a random-looking value for that exact pixel. The hash
 * constants below are published, well-tested values, NOT tunable parameters;
 * any good integer hash would do, and changing them only reshuffles the noise.
 */

/* sf_h: Chris Wellons' "lowbias32" integer hash finalizer. The multipliers and
   shift sequence are the published, bias-minimised constants of that algorithm. */
SPEKTRA_INLINE uint32_t sf_h(uint32_t x)
{
  x ^= x >> 16;
  x *= 0x7feb352dU;
  x ^= x >> 15;
  x *= 0x846ca68bU;
  x ^= x >> 16;
  return x;
}
/* sf_u01: hash -> uniform float in [0,1) using the top 24 bits (float mantissa). */
SPEKTRA_INLINE float sf_u01(uint32_t s)
{
  return (sf_h(s) & 0xffffff) / (float)0x1000000;
}
/* sf_nrm: one hash seed -> one approximate standard-normal sample via a
   sum-of-4-uniforms (Irwin-Hall) approximation instead of Box-Muller's
   sqrt+log+cos transcendental chain. Var[uniform(0,1)] = 1/12, so a sum of 4
   has variance 4/12 = 1/3 and mean 2; rescaling by sqrt(3) and centering
   gives unit variance, zero mean -- the two moments sf_layer_particle's
   normal approximations actually rely on. The finite (not truly Gaussian)
   tails this leaves behind aren't visually meaningful for film grain: real
   emulsions don't have famously heavy statistical tails either, and the
   difference from a true Gaussian only shows up several standard
   deviations out, well past where grain is visible at all. Called twice per
   particle draw, per sub-layer (up to SF_GRAIN_MAX_SUBLAYERS times for a
   multi-sublayer film) -- worth being cheap. The four multipliers are
   distinct, well-known odd hash constants (murmur3's c1/c2, Knuth's golden-
   ratio multiplier, and one more), used only to decorrelate the four
   uniform draws from each other. */
SPEKTRA_INLINE float sf_nrm(uint32_t s)
{
  const float u = sf_u01(s) + sf_u01(s * 2654435761u + 1u) + sf_u01(s * 2246822519u + 2u)
                  + sf_u01(s * 3266489917u + 3u);
  return (u - 2.0f) * 1.7320508f; /* sqrt(3) */
}
/* sf_layer_particle: draw the developed density of one emulsion layer as a
   doubly-stochastic process. First the number of developed grains in this pixel
   (mean lam, Poisson -> normal approximation), then the fraction that record
   signal (binomial -> normal approximation). The 0x9e3779b9 / 0x85ebca6b offsets
   are standard hash-mixing constants (golden ratio; murmurhash) that simply give
   the two normal draws independent seeds. */
/* sf_pixel_seed: combine pixel coordinates and a channel/sub-layer index into one
   seed for the grain hash. The three large primes are Teschner et al.'s published
   spatial-hash constants; XOR-mixing distinct primes per axis keeps neighbouring
   pixels and channels from sharing a seed (which would correlate their grain).
   Uses ABSOLUTE image coordinates so grain is stable while panning. */
SPEKTRA_INLINE uint32_t sf_pixel_seed(uint32_t xi, uint32_t yi, uint32_t chan)
{
  return xi * 73856093u ^ yi * 19349663u ^ chan * 83492791u;
}

/* Maximum kernel half-width (taps = 2*radius+1) for sf_gauss_kernel_1d below.
   Caps cost for pathologically large sigma (very high film_format_mm
   combined with very low resolution); every physically-plausible sigma this
   module uses stays far under this. Shared by spektra_core.c's CPU direct
   convolution and spektrafilm.c's GPU host-side weight upload, so both
   dispatch the identical kernel for a given sigma. */
#define SF_GAUSS_MAX_RADIUS 512

/* Sigma at which the direct kernel hands over to the recursive one. This is
   the reference's own crossover (SMALL_SIGMA_MAX in fast_gaussian_filter.py),
   and above it both sides now run the same Young-van Vliet filter, so a given
   sigma produces the same blur here, on the GPU, and in the app. */
#define SF_GAUSS_EXACT_MAX_SIGMA 3.0f

/* Young-van Vliet order-3 recursive Gaussian coefficients (B, B1, B2, B3),
   identical to the reference's _yvv_coeffs. Exported so the GPU host side can
   build the same filter the CPU runs. */
/* Widest sigma the recursive filter is asked for. Above this its float32
   coefficients stop describing the filter we want: B falls to ~1e-7 while
   B1..B3 stay near 3, and the poles walk out to the unit circle. Measured
   effective vs requested sigma, single pass, float32:

       requested   100    150    200    400    700
       effective   102    155    254    875  105395

   -- so it tracks to ~150, is unusable by 200, and diverges outright past ~700,
   which is where cinebloom and pro-mist land at export resolution (their bloom
   reaches 2500 um and 1625 um, ~1000 px on a 6000 px frame at 26 mm). The
   divergence shows as full-height coloured striping: the column pass runs after
   the row pass, so each column blows up on its own.

   Clamping keeps the filter inside the range where it is a Gaussian at the cost
   of a narrower halo than asked for at extreme diffusion settings. That is a
   stopgap, not the answer -- a large-sigma blur wants downsample/blur/upsample,
   which is also faster. This just stops it producing garbage in the meantime. */
#define SF_GAUSS_MAX_IIR_SIGMA 150.0f
void sf_gauss_yvv_coeffs(float sigma, float out[4]);

/* Build a normalized, truncated 1D Gaussian kernel. truncate = 3 sigma with
 * radius = int(3*sigma + 0.5), matching the reference's own
 * _gaussian_kernel_1d default rather than scipy's truncate = 4 -- the
 * reference never calls scipy for this. `kernel` must have room for
 * 2*max_radius+1 taps; returns the radius actually used. Exported so both
 * the CPU convolution (spektra_core.c) and the GPU host-side weight upload
 * (spektrafilm.c's process_cl) build the identical kernel for a given sigma. */
int sf_gauss_kernel_1d(float sigma, float *kernel, int max_radius);

/* sf_poisson: one Poisson(lam) draw from a stateless seed.

   Below SF_POISSON_EXACT_MAX the draw is EXACT (Knuth's product-of-uniforms).
   That threshold is not a quality/speed compromise, it is where the normal
   approximation stops being safe: sf_nrm is bounded at +-sqrt(12) (Irwin-Hall
   over four uniforms), so lam + sqrt(lam)*sf_nrm() can only go negative when
   lam < 12. Above the threshold no clamp is ever needed and the approximation is
   mean- and variance-exact; below it, clamping a normal at zero is exactly what
   biased the old sampler upward in the shadows. Cost: the exact branch averages
   lam+1 hashes (<= 13), the fast branch 4 -- against 8 for the two sf_nrm draws
   this replaces. */
#define SF_POISSON_EXACT_MAX 12.0f
SPEKTRA_INLINE float sf_poisson(float lam, uint32_t seed)
{
  if(lam <= 0.0f) return 0.0f;
  if(lam < SF_POISSON_EXACT_MAX)
  {
    const float limit = expf(-lam);
    float prod = 1.0f;
    int k = 0;
    do
    {
      prod *= sf_u01(seed + (uint32_t)k * 0x9e3779b9u);
      k++;
    } while(prod > limit && k < 64);
    return (float)(k - 1);
  }
  return lam + sqrtf(lam) * sf_nrm(seed);
}

/* sf_layer_particle: draw the developed density of one emulsion layer.

   The reference model (layer_particle_model, grain.py) draws N_s ~ Poisson(lam)
   sensitised grains and develops each with probability p, i.e.
   Binomial(Poisson(lam), p). Poisson thinning makes that composition EXACTLY
   Poisson(lam * p), so the two-stage draw collapses to a single Poisson and the
   intermediate grain count -- along with the two clamps that went with it --
   disappears.

   The mean is then exactly lam*p * od * sat = density, and the variance exactly
   p * dmax^2 * sat / npart = D (Dmax - u D) / N, the target grain.py derives. */
SPEKTRA_INLINE float sf_layer_particle(float density, float dmax, float npart, float unif,
                                       uint32_t seed)
{
  const float p = sf_clampf(density / dmax, 1e-6f, 1 - 1e-6f);
  const float od = dmax / npart;
  const float sat = 1.f - p * unif * (1 - 1e-6f);
  return sf_poisson(npart * p / sat, seed * 0x9e3779b9u + 1u) * od * sat;
}
/* SF_GRAIN_REF_UM: the fixed reference scale (spektrafilm's own
   pixel_size_um=10) the particle model is generated at, independent of the
   live pipe's pixel_um — this keeps grain CHARACTER constant across zoom.
   Callers that turn the generated delta into visible clump STRUCTURE (the
   blur step) must still convert this reference into real pixels via the
   pipe's own pixel_um, or clump SIZE silently stops scaling with output
   resolution — see the grain blur in spektrafilm.c/.cl and
   _max_halo_sigma's ROI padding, all of which must agree. */
#define SF_GRAIN_REF_UM 10.0f
