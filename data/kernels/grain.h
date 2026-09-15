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

/* Film grain sampling, and the scalar float math it rests on, compiled by
 * both a module's CPU path and its OpenCL kernels. spektrafilm is the first
 * caller; nothing here is specific to it.
 *
 * This header is compiled TWICE, by two different compilers, and the two
 * results must agree BIT-FOR-BIT: the grain sampler below runs an
 * accept/reject loop whose exit depends on exact float comparisons, so a
 * one-ULP disagreement between host and device becomes a whole-integer
 * grain-count difference on isolated pixels. It used to live as two
 * hand-synchronised copies (common/spektra_core.h and spektrafilm.cl);
 * keeping those in lockstep by hand is exactly the failure mode the bit
 * exactness requirement cannot tolerate, hence this single copy.
 *
 * Consequences of being dual-dialect, all of which constrain edits here:
 *
 *  - Plain scalar C only: no pointers, no structs, no arrays, no library
 *    calls beyond the four shimmed below, nothing from <math.h> that the
 *    OpenCL side would resolve to a different implementation.
 *
 *  - Only correctly-rounded operations (+, -, *, /, sqrt, floor, and exact
 *    bit manipulation). NOT exp/log/exp2/log2/pow: OpenCL specs those to
 *    <=3 ULP and most GPUs implement them in hardware, while glibc rounds
 *    correctly, and that slack reaches the grain draw. The polynomials
 *    below exist for that reason and are not to be "simplified" back into
 *    library calls, nor into native_* variants, which have no accuracy
 *    guarantee at all.
 *
 *  - No multiply-add may be contracted into an FMA on one side and not the
 *    other. The host side gets -ffp-contract=off from src/CMakeLists.txt
 *    (GCC has never implemented "#pragma STDC FP_CONTRACT OFF"), the device
 *    side gets "#pragma OPENCL FP_CONTRACT OFF" at the top of the .cl.
 *    ANY .c file that includes this header must be added to the
 *    set_source_files_properties() list in src/CMakeLists.txt that applies
 *    SPEKTRA_FP_CONTRACT_FLAGS, or its copy of this math will silently
 *    diverge from the kernel's.
 *
 *  - Changing anything here changes rendered output. The file is listed in
 *    clincludes[] in src/common/opencl.c so that editing it invalidates the
 *    cached device binaries; without that entry you would keep running a
 *    stale kernel against a rebuilt host.
 */

#pragma once

/* ---- dialect shim ------------------------------------------------------ */
/* OpenCL C has no <stdint.h> and no f-suffixed math functions; its fabs/
   fmax/floor/sqrt are overloaded and already resolve to the single-precision
   versions for float arguments. The macros are #undef'd at the end of this
   header so they cannot leak into the including translation unit. */
/* Which dialect this is being compiled as is DECLARED by the includer, not
   sniffed from predefined macros: a .cl does "#define GRAIN_CL 1"
   before including this, and the host includes it with GRAIN_CL unset.
   Sniffing is not reliable enough to decide something this load-bearing --
   "clang -cc1 -cl-std=CL1.2", which the testcompile_opencl_kernels CMake
   target uses, defines only __OPENCL_C_VERSION__, while real device
   compilers define __OPENCL_VERSION__ as well. Keying on the wrong one of
   those would have the test-compile take the host branch and pass while the
   GPU took the other, which is the one failure this file exists to prevent.

   The predefines are still worth something as an ASSERTION, though: if
   either is set we know we are in an OpenCL translation unit, so a .cl that
   forgets the #define gets a clear diagnostic here rather than a confusing
   "math.h not found" fifteen lines down. */
#if (defined(__OPENCL_VERSION__) || defined(__OPENCL_C_VERSION__)) && !defined(GRAIN_CL)
#error "grain.h: OpenCL translation unit must '#define GRAIN_CL 1' before including this header"
#endif
#if defined(GRAIN_CL) && !defined(__OPENCL_VERSION__) && !defined(__OPENCL_C_VERSION__)
#error "grain.h: GRAIN_CL is set but this is not an OpenCL translation unit"
#endif

#ifdef GRAIN_CL
typedef uint uint32_t;
#define floorf floor
#define sqrtf sqrt
#define fmaxf fmax
#define fminf fmin
#else
#include <math.h>
#include <stdint.h>
#pragma STDC FP_CONTRACT OFF
#endif

#ifndef GRAIN_INLINE
#define GRAIN_INLINE static inline
#endif

/* fmin(fmax(...)), NOT a ternary chain: IEEE fmax/fmin return the non-NaN
   operand, so a NaN x collapses to lo rather than propagating. That is the
   behaviour grain_layer_particle's 0/0 comment below relies on. The host copy
   this replaces used "x < lo ? lo : (x > hi ? hi : x)", which propagates
   NaN instead -- the two agreed on every finite input and disagreed on
   exactly the case the guard was written for. */
GRAIN_INLINE float grain_clampf(float x,
                               float lo,
                               float hi)
{
  return fminf(fmaxf(x, lo), hi);
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

/* grain_hash: Chris Wellons' "lowbias32" integer hash finalizer. The multipliers and
   shift sequence are the published, bias-minimised constants of that algorithm. */
GRAIN_INLINE uint32_t grain_hash(uint32_t x)
{
  x ^= x >> 16;
  x *= 0x7feb352dU;
  x ^= x >> 15;
  x *= 0x846ca68bU;
  x ^= x >> 16;
  return x;
}

/* grain_uniform: hash -> uniform float in [0,1) using the top 24 bits (float mantissa). */
GRAIN_INLINE float grain_uniform(uint32_t s)
{
  return (grain_hash(s) & 0xffffff) / (float)0x1000000;
}

/* grain_normal: one hash seed -> one approximate standard-normal sample via a
   sum-of-4-uniforms (Irwin-Hall) approximation instead of Box-Muller's
   sqrt+log+cos transcendental chain. Var[uniform(0,1)] = 1/12, so a sum of 4
   has variance 4/12 = 1/3 and mean 2; rescaling by sqrt(3) and centering
   gives unit variance, zero mean -- the two moments grain_layer_particle's
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
GRAIN_INLINE float grain_normal(uint32_t s)
{
  const float u = grain_uniform(s) + grain_uniform(s * 2654435761u + 1u) + grain_uniform(s * 2246822519u + 2u)
                  + grain_uniform(s * 3266489917u + 3u);
  return (u - 2.0f) * 1.7320508f; /* sqrt(3) */
}

/* grain_pixel_seed: combine pixel coordinates and a channel/sub-layer index into one
   seed for the grain hash. The three large primes are Teschner et al.'s published
   spatial-hash constants; XOR-mixing distinct primes per axis keeps neighbouring
   pixels and channels from sharing a seed (which would correlate their grain).
   Uses ABSOLUTE image coordinates so grain is stable while panning. */
GRAIN_INLINE uint32_t grain_pixel_seed(uint32_t xi,
                                      uint32_t yi,
                                      uint32_t chan)
{
  return xi * 73856093u ^ yi * 19349663u ^ chan * 83492791u;
}

/* grain_poisson: one Poisson(lam) draw from a stateless seed.

   Below GRAIN_POISSON_EXACT_MAX the draw is EXACT (Knuth's product-of-uniforms).
   That threshold is not a quality/speed compromise, it is where the normal
   approximation stops being safe: grain_normal is bounded at +-sqrt(12) (Irwin-Hall
   over four uniforms), so lam + sqrt(lam)*grain_normal() can only go negative when
   lam < 12. Above the threshold no clamp is ever needed and the approximation
   is mean- and variance-exact; below it, a normal clamped at zero would bias
   the draw upward in the shadows, which is the whole reason for the exact
   branch. Cost: the exact branch averages lam+1 hashes (<= 13), the fast
   branch 4, against 8 for a pair of grain_normal draws. */
#define GRAIN_POISSON_EXACT_MAX 12.0f

/* grain_exp2i: 2^k for integer k, produced by writing k into the binary32
   exponent field directly instead of calling ldexpf/exp2f. This is
   bit manipulation, not arithmetic -- no rounding happens, so it is exact
   and identical everywhere by construction. Only valid for k that keeps the
   result normal (roughly -125..127); grain_exp_neg below never asks for
   anything close to those limits over its intended domain. */
GRAIN_INLINE float grain_exp2i(int k)
{
#ifdef GRAIN_CL
  return as_float((uint)(k + 127) << 23);
#else
  union { uint32_t u; float f; } v;
  v.u = (uint32_t)(k + 127) << 23;
  return v.f;
#endif
}

/* grain_exp_neg: exp(-lam) for lam in (0, GRAIN_POISSON_EXACT_MAX), built only from
   +, -, * and the exact floor()/exponent-injection above -- deliberately NOT
   a call to expf()/exp(). The platform exp() is only spec'd to within a few
   ULP (OpenCL C requires just <=3 ULP for exp(), versus basic +,-,* which
   IEEE-754 and the OpenCL spec both require to be correctly rounded), and
   this value feeds an accept/reject loop in grain_poisson: prod *= grain_uniform(...)
   until prod <= limit. A few-ULP disagreement between the CPU's expf() and
   the GPU's exp() only rarely lands close enough to prod to matter, but
   when it does, the loop exits one iteration earlier or later and the
   sampled grain count is off by a whole integer -- a real, visible
   per-pixel difference from an invisible input difference. Every op used
   here (+, -, *, floor, and the bit-exact 2^k above) is required to be
   exact/correctly-rounded on both sides, so this reproduces bit-for-bit
   given the same lam, unlike the library call it replaces.

   Implementation: standard exp(x) = 2^k * exp(r) range reduction, k =
   round(x/ln2), r in [-ln2/2, ln2/2], exp(r) via a degree-6 Taylor
   polynomial (evaluated with Horner's method). Max relative error over the
   full (0,12) domain is ~1.1e-6 -- far tighter than grain needs, chosen
   for auditability over a tighter minimax fit. */
GRAIN_INLINE float grain_exp_neg(float lam)
{
  const float t = -lam;
  const int k = (int)floorf(t * 1.4426950216293335f + 0.5f); /* log2(e) */
  const float r = t - (float)k * 0.6931471824645996f;        /* ln(2) */
  float p = 0.00138888892f;                                  /* 1/720 */
  p = p * r + 0.00833333377f;                                /* 1/120 */
  p = p * r + 0.0416666679f;                                 /* 1/24 */
  p = p * r + 0.166666672f;                                  /* 1/6 */
  p = p * r + 0.5f;
  p = p * r + 1.0f;
  p = p * r + 1.0f;
  return p * grain_exp2i(k);
}

/* grain_exp2f / grain_log2f: 2^x and log2(x) built from +, -, *, / and the exact
   floor/exponent manipulation above, for the reason grain_exp_neg is -- and
   this pair is what actually reaches the grain sampler. SF_POW10F and
   SF_LOG10F (spektra_sim.c) and sf_pow10f / sf_log10f (spektrafilm.cl) are
   defined in terms of these rather than the platform exp2f/log2f, which
   OpenCL specifies only to <=3 ULP while glibc rounds correctly: that slack
   lands in the film density arriving at grain_layer_particle, and grain_poisson's
   accept/reject loop turns a one-ULP density difference into a
   whole-integer grain count difference wherever a partial product happens
   to sit near limit. The result is isolated pixels, scattered evenly and
   uncorrelated with image structure, each off by a full grain quantum
   rather than by a rounding.

   Both are ~1 ULP against glibc over the domains this module uses, and are
   not general-purpose replacements outside them: grain_exp2f assumes the result
   stays normal, grain_log2f assumes x is positive and normal. SF_LOG10F floors
   its argument at SF_LOG_EPS, which keeps it there. */
GRAIN_INLINE float grain_exp2f(float x)
{
  /* x = k + r, k integer and |r| <= 0.5, so 2^x = 2^k * e^(r ln2) with the
     exponential taken over |t| <= 0.347 by a degree-7 Taylor polynomial in
     Horner form and 2^k injected exactly. */
  const int k = (int)floorf(x + 0.5f);
  const float t = (x - (float)k) * 0.6931471824645996f; /* ln(2) */
  float p = 0.000198412700f;                            /* 1/5040 */
  p = p * t + 0.00138888892f;                           /* 1/720 */
  p = p * t + 0.00833333377f;                           /* 1/120 */
  p = p * t + 0.0416666679f;                            /* 1/24 */
  p = p * t + 0.166666672f;                             /* 1/6 */
  p = p * t + 0.5f;
  p = p * t + 1.0f;
  p = p * t + 1.0f;
  return p * grain_exp2i(k);
}

GRAIN_INLINE float grain_log2f(float x)
{
  /* Take the binary exponent off by hand, then fold the mantissa into
     [1/sqrt2, sqrt2] so that s = (m-1)/(m+1) stays inside +-0.1716, where
     log(m) = 2(s + s^3/3 + s^5/5 + s^7/7 + s^9/9) is good to well under an
     ULP. Both steps of the fold are exact. */
#ifdef GRAIN_CL
  const uint32_t xu = as_uint(x);
  int e = (int)((xu >> 23) & 0xffu) - 127;
  float m = as_float((xu & 0x007fffffu) | 0x3f800000u);
#else
  union { float f; uint32_t u; } v;
  v.f = x;
  int e = (int)((v.u >> 23) & 0xffu) - 127;
  v.u = (v.u & 0x007fffffu) | 0x3f800000u;
  float m = v.f;
#endif
  if(m > 1.41421356f) { m *= 0.5f; e += 1; }
  const float s = (m - 1.0f) / (m + 1.0f);
  const float s2 = s * s;
  float p = 0.222222224f;    /* 2/9 */
  p = p * s2 + 0.285714298f; /* 2/7 */
  p = p * s2 + 0.400000006f; /* 2/5 */
  p = p * s2 + 0.666666687f; /* 2/3 */
  p = p * s2 + 2.0f;
  return (float)e + p * s * 1.4426950216293335f; /* log2(e) */
}

GRAIN_INLINE float grain_poisson(float lam,
                                uint32_t seed)
{
  if(lam <= 0.0f) return 0.0f;
  if(lam < GRAIN_POISSON_EXACT_MAX)
  {
    const float limit = grain_exp_neg(lam);
    float prod = 1.0f;
    int k = 0;
    do
    {
      prod *= grain_uniform(seed + (uint32_t)k * 0x9e3779b9u);
      k++;
    } while(prod > limit && k < 64);
    return (float)(k - 1);
  }
  /* Plain sqrt(), never native_sqrt(): this branch must reproduce the host's
     sqrtf(lam) bit-for-bit (see the grain_exp_neg comment above on why
     exactness here matters) -- native_sqrt has no accuracy guarantee at all
     and commonly maps to a low-precision hardware rsqrt, which was
     decorrelating the two renders' grain for every pixel landing in this
     branch (lam >= GRAIN_POISSON_EXACT_MAX). */
  return lam + sqrtf(lam) * grain_normal(seed);
}

/* grain_layer_particle: draw the developed density of one emulsion layer.

   The reference model (layer_particle_model, grain.py) draws N_s ~ Poisson(lam)
   sensitised grains and develops each with probability p, i.e.
   Binomial(Poisson(lam), p). Poisson thinning makes that composition EXACTLY
   Poisson(lam * p), so the two-stage draw collapses to a single Poisson and the
   intermediate grain count -- along with the two clamps that went with it --
   disappears.

   The mean is then exactly lam*p * od * sat = density, and the variance exactly
   p * dmax^2 * sat / npart = D (Dmax - u D) / N, the target grain.py derives.
   The 0x9e3779b9 offset is a standard hash-mixing constant (golden ratio)
   that simply decorrelates this draw's seed from the caller's. */
GRAIN_INLINE float grain_layer_particle(float density,
                                       float dmax,
                                       float npart,
                                       float unif,
                                       uint32_t seed)
{
  const float p = grain_clampf(density / dmax, 1e-6f, 1.0f - 1e-6f);
  /* A sub-layer that carries no density has dmax and npart both zero, making
     this 0/0. The clamp above already absorbs a non-finite ratio (fmaxf
     returns the non-NaN operand), but this divide has no such guard and its
     NaN would reach the returned sample and from there the density buffer. */
  const float od = dmax / fmaxf(npart, 1e-9f);
  const float sat = 1.0f - p * unif * (1.0f - 1e-6f);
  return grain_poisson(npart * p / sat, seed * 0x9e3779b9u + 1u) * od * sat;
}

#ifdef GRAIN_CL
#undef floorf
#undef sqrtf
#undef fmaxf
#undef fminf
#endif
