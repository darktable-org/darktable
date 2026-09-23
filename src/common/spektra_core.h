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
/* Scalar math compiled BOTH here and by data/kernels/spektrafilm.cl, which
   must agree bit-for-bit. Lives in data/kernels/ because that is the only
   directory on the OpenCL compiler's include path, at test-compile time
   (data/kernels/CMakeLists.txt) and at runtime (src/common/opencl.c).
   Any .c that includes this header needs the -ffp-contract=off treatment
   src/CMakeLists.txt applies to the spektra sources. */
#include "grain.h"

#ifndef GRAIN_INLINE
#define GRAIN_INLINE static inline
#endif

/* Spatial effects implemented in spektra_core.c (they need dt_alloc_align_float
   and OpenMP linkage; everything else in this header is inline). */
void sf_blur_plane3(float *buf,
                    int w,
                    int h,
                    float sigma,
                    float *plane);
void sf_blur_plane3_fast(float *buf,
                         int w,
                         int h,
                         float sigma,
                         float *plane);
/* Same exact-kernel blur as sf_blur_plane3, but operating directly on a
   single flat w*h buffer (no 3-channel interleave) -- used for the
   per-sublayer dye-cloud blur inside grain generation, where each
   (channel, sub-layer) has its own sigma and needs its own buffer; one interleaved 3-channel pass
   will not do. No lower sigma cutoff
   (unlike sf_blur_plane3's 0.3px guard for the visible clump blur): the
   dye-cloud sigma is often well under a pixel and still meaningfully
   softens the raw particle draw, matching upstream's plain `> 0` check. */
void sf_blur_plane1(float *buf,
                    int w,
                    int h,
                    float sigma,
                    float *plane,
                    float *trans);
/* Additive unsharp mask on the scanned RGB ([df] apply_unsharp_mask):
   out = D + amount * (D - blur(D)). `orig` and `work` are w*h*3 and w*h
   scratch buffers supplied by the caller. */
void sf_unsharp_mask3(float *buf,
                      int w,
                      int h,
                      float sigma,
                      float amount,
                      float *orig,
                      float *work);
/* Viewing-glare veil ([gl] add_glare): adds a blurred lognormal field of mean
   percent/100 (relative std `roughness`) to all three channels. `field` is a
   w*h scratch buffer; roi_x/roi_y are absolute image coordinates so the veil
   is stable under pan and zoom. */
void sf_glare(float *rgb,
              int w,
              int h,
              float percent,
              float roughness,
              float blur,
              int roi_x,
              int roi_y,
              float *field);

void sf_multiplicative_unsharp_mask3(float *buf,
                                     int w,
                                     int h,
                                     float sigma,
                                     float amount,
                                     const float *floor_d,
                                     float *orig,
                                     float *work);
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
void sf_halation(float *raw,
                 int w,
                 int h,
                 double pixel_um,
                 const double sc_core[3],
                 const double sc_tail[3],
                 const double w_s[3],
                 float scatter_amount,
                 float scatter_scale,
                 float halation_amount,
                 float halation_scale,
                 const double halation_strength[3],
                 double halation_first_sigma_um);
/* Stops of headroom above the protect threshold over which the boost is spread.
   The reference spreads it from the threshold up to the frame's own peak; that
   peak is a whole-image reduction, which a per-ROI/per-tile pixelpipe cannot
   reproduce consistently. A fixed span keeps the curve identical in every tile
   and in both pipes, and puts both controls on the same footing -- protect_ev
   sets where the boost starts, this sets how far above that it reaches full
   strength. 4 EV matches the reference's typical frame peak for a normally
   exposed scene (midgray + 4-6 EV). */
#define SF_BOOST_SPAN_EV 4.0f
void sf_boost_highlights(float *raw,
                         int w,
                         int h,
                         float boost_ev,
                         float boost_range,
                         float protect_ev);
void sf_diffusion_filter(float *raw,
                         int w,
                         int h,
                         double pixel_um,
                         int family,
                         float strength,
                         float spatial_scale,
                         float halo_warmth);

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
int sf_diffusion_build_plan(int family,
                            float strength,
                            float halo_warmth,
                            sf_diffusion_plan_t *plan);


/* Maximum kernel half-width (taps = 2*radius+1) passed to
   dt_gaussian_kernel_1d.
   Caps cost for pathologically large sigma (very high film_format_mm
   combined with very low resolution); every physically-plausible sigma this
   module uses stays far under this. Shared by spektra_core.c's CPU direct
   convolution and spektrafilm.c's GPU host-side weight upload, so both
   dispatch the identical kernel for a given sigma. */
#define SF_GAUSS_MAX_RADIUS 512

/* Sigma at which the direct kernel hands over to the recursive one. This is
   the reference's own crossover (SMALL_SIGMA_MAX in fast_gaussian_filter.py),
   and above it both sides run the same Young-van Vliet filter, so a given
   sigma produces the same blur here, on the GPU, and in the app. */
#define SF_GAUSS_EXACT_MAX_SIGMA 3.0f
/* Below this the blur is skipped outright rather than run with a degenerate
   kernel: dt_gaussian_kernel_1d() floors its radius at 1, so a sigma of 0.1
   still produces a real 3-tap kernel with non-negligible side weights, not
   an identity. sf_blur_plane3/_fast apply this cut themselves; the constant is
   shared so spektrafilm.c's OpenCL path can apply it at the same call sites
   instead of blurring where the CPU does nothing. */
#define SF_GAUSS_MIN_SIGMA 0.3f

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
void sf_gauss_yvv_coeffs(float sigma,
                         float out[4]);

/* Build a normalized, truncated 1D Gaussian kernel. truncate = 3 sigma with
 * radius = int(3*sigma + 0.5), matching the reference's own
 * _gaussian_kernel_1d default, not scipy's truncate = 4 -- the
 * reference never calls scipy for this. `kernel` must have room for
 * 2*max_radius+1 taps; returns the radius actually used. Exported so both
 * the CPU convolution (spektra_core.c) and the GPU host-side weight upload
 * (spektrafilm.c's process_cl) build the identical kernel for a given sigma. */

/* grain_clampf, the grain hash (grain_hash / grain_uniform / grain_normal / grain_pixel_seed), the
   portable exp/log polynomials (grain_exp2i / grain_exp_neg / grain_exp2f /
   grain_log2f) and the grain sampler (grain_poisson / grain_layer_particle, plus
   GRAIN_POISSON_EXACT_MAX) now live in data/kernels/grain.h, included
   at the top of this header: the OpenCL kernel compiles that same file, so
   the two paths cannot drift. Read the rationale for every constant there. */

/* SF_GRAIN_REF_UM: the fixed reference scale (spektrafilm's own
   pixel_size_um=10) the particle model is generated at, independent of the
   live pipe's pixel_um — this keeps grain CHARACTER constant across zoom.
   Callers that turn the generated delta into visible clump STRUCTURE (the
   blur step) must still convert this reference into real pixels via the
   pipe's own pixel_um, or clump SIZE silently stops scaling with output
   resolution — see the grain blur in spektrafilm.c/.cl and
   _max_halo_sigma's ROI padding, all of which must agree. */
#define SF_GRAIN_REF_UM 10.0f
