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

/* Device side of the film grain module's spektrafilm method. The host path
   in src/iop/filmgrain.c is the reference and these kernels reproduce it
   bit-for-bit: the per-pixel math comes from filmgrain.h and grain.h, which
   both sides compile, and the gaussian from blur_plane.h, which matches
   sf_blur_plane1.

   The pixelpipe hands a module its input and output as image2d objects, so
   those two are only ever touched through read_imagef / write_imagef.
   Everything in between lives in plain buffers the module allocates. */


#include "common.h"

/* After common.h, not before: under -cl-fast-relaxed-math (what the "fast"
   OpenCL preference compiles with) common.h issues "#pragma OPENCL
   FP_CONTRACT ON", and the last pragma at file scope is the one that counts.
   Ahead of the include this setting is silently undone and every a*b+c below
   fuses into a single rounding the host does not perform. */
#pragma OPENCL FP_CONTRACT OFF
#define GRAIN_CL 1
#include "grain.h"
#include "filmgrain.h"
#include "blur_plane.h"

/* Film density of each colour channel of the input, into a float4 buffer
   (xyz = R, G, B densities). Done once, ahead of the nine sub-layer draws
   that read it. */
kernel void filmgrain_to_density(read_only image2d_t in,
                                 global float4 *dens,
                                 const int w,
                                 const int h)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= w || y >= h) return;

  const float4 pix = read_imagef(in, samplerA, (int2)(x, y));
  float4 d = (float4)0.0f;
  d.x = filmgrain_density(pix.x);
  d.y = filmgrain_density(pix.y);
  d.z = filmgrain_density(pix.z);
  dens[(size_t)y * w + x] = d;
}

/* One sub-layer of one dye layer, as its deviation from the density it was
   drawn against. roi_x / roi_y place the buffer in the full image so the
   pattern holds still while panning and across tiles; the seed is keyed on
   the dye layer and sub-layer exactly as on the host. */
kernel void filmgrain_deviation(global const float4 *dens,
                                global float *dev,
                                const int w,
                                const int h,
                                const int roi_x,
                                const int roi_y,
                                const int channel,
                                const int sublayer,
                                const float share,
                                const float dmin,
                                const float dmax,
                                const float npart,
                                const float unif)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= w || y >= h) return;
  const size_t k = (size_t)y * w + x;

  const float4 d = dens[k];
  const float density = channel == 0 ? d.x : (channel == 1 ? d.y : d.z);
  const uint seed = grain_pixel_seed((uint)(x + roi_x), (uint)(y + roi_y),
                                     (uint)(channel + sublayer * 10));
  dev[k] = filmgrain_sublayer_deviation(density, share, dmin, dmax, npart, unif, seed);
}

/* Running sum of sub-layer deviations. reset stores the first sub-layer
   directly, which saves a separate zero fill. */
kernel void filmgrain_accumulate(global float *acc,
                                 global const float *src,
                                 const int w,
                                 const int h,
                                 const int reset)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= w || y >= h) return;
  const size_t k = (size_t)y * w + x;
  acc[k] = reset ? src[k] : acc[k] + src[k];
}

/* Acutance recovery after the clump blur: g + amount * (g - blur(g)).
   Additive because g is a zero-mean deviation, where a ratio would be
   ill-conditioned at every zero crossing. */
kernel void filmgrain_recover(global float *g,
                              global const float *blurred,
                              const int w,
                              const int h,
                              const float amount)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= w || y >= h) return;
  const size_t k = (size_t)y * w + x;
  g[k] = g[k] + amount * (g[k] - blurred[k]);
}

/* Turn the three dye layers' grain into an exposure change per channel and
   apply it to the output window. The grain buffers and `in` cover roi_in,
   `out` covers roi_out, which sits (dx, dy) inside it. Each pixel's share
   of the grain comes from the tone curve at that pixel's luminance.
   Multiplying keeps black at black and never changes a value's sign. */
kernel void filmgrain_apply(read_only image2d_t in,
                            write_only image2d_t out,
                            global const float *g0,
                            global const float *g1,
                            global const float *g2,
                            global const float *tone_lut,
                            const int ow,
                            const int oh,
                            const int iw,
                            const int dx,
                            const int dy,
                            const float k_lum,
                            const float k_chroma,
                            const float lum_r,
                            const float lum_g,
                            const float lum_b)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= ow || y >= oh) return;
  const size_t k = (size_t)(y + dy) * iw + (x + dx);

  const float a0 = g0[k];
  const float a1 = g1[k];
  const float a2 = g2[k];
  float4 pix = read_imagef(in, samplerA, (int2)(x + dx, y + dy));
  const float lum = filmgrain_luminance(pix.x, pix.y, pix.z, lum_r, lum_g, lum_b);
  const float tone = filmgrain_tone_factor(tone_lut, filmgrain_density(lum) / FILMGRAIN_DMAX);
  pix.x *= grain_exp2f(tone * filmgrain_channel_ev(a0, a1, a2, a0, k_lum, k_chroma));
  pix.y *= grain_exp2f(tone * filmgrain_channel_ev(a0, a1, a2, a1, k_lum, k_chroma));
  pix.z *= grain_exp2f(tone * filmgrain_channel_ev(a0, a1, a2, a2, k_lum, k_chroma));
  write_imagef(out, (int2)(x, y), pix);
}
