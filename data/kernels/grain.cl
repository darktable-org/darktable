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

/* Device side of the grain module's particle method. The host path in
   src/iop/grain.c is the reference: these kernels must reproduce it
   bit-for-bit, or a preview rendered on the GPU shows different grain from
   the export rendered on the CPU.

   Everything that decides a grain count comes from the shared headers below.
   What remains is buffer walking and the gaussian convolution, and that
   convolution matches the host's _sf_gauss_convolve_1d exactly: same weights
   (built host-side by dt_gaussian_kernel_1d and uploaded), same
   clamp-to-edge, same accumulation order. */

#pragma OPENCL FP_CONTRACT OFF

#include "common.h"
#define GRAIN_CL 1
#include "grain.h"
#include "grain_curve.h"
#include "blur_plane.h"

/* One sub-layer of one dye layer, reduced to its deviation from the density
   it was drawn against. Mirrors the inner loop of process() in grain.c: the
   sampler's mean is exactly that density, so what is left carries no image
   content and the blurs downstream act on grain alone. */
__kernel void grain_gen_deviation(__global const float4 *in,
                                  __global float *dev,
                                  const int w,
                                  const int h,
                                  const int roi_x,
                                  const int roi_y,
                                  const int channel_idx,
                                  const int sl_idx,
                                  const int nle,
                                  const int max_sub,
                                  const float dmax_full,
                                  const float unif_ch,
                                  const float npart_scale,
                                  __global const float *layer_dmax,
                                  __global const float *layer_npart,
                                  __global const float *layer_dmin,
                                  __global const float *layer_curve_total,
                                  __global const float *layer_curve)
{
  const int x = get_global_id(0), y = get_global_id(1);
  if(x >= w || y >= h) return;
  const size_t k = (size_t)y * w + x;

  const float L = in[k].x;
  const float dens = dmax_full * clamp(L, 0.0f, 100.0f) / 100.0f;

  const int lstride = max_sub * 3;
  const int idx = sl_idx * 3 + channel_idx;

  const float pos = grain_curve_inverse(layer_curve_total + channel_idx, nle, 3, dens);
  const float raw = grain_curve_sample(layer_curve + idx, nle, lstride, pos);
  const float dmin = layer_dmin[idx];
  const float d_abs = raw + dmin;

  /* Absolute buffer coordinates, so the pattern does not crawl when panning
     and a tile draws the same grain as the untiled render. Seed channel is
     the dye layer, as on the host. */
  const uint seed = grain_pixel_seed((uint)(x + roi_x), (uint)(y + roi_y),
                                  (uint)(channel_idx + sl_idx * 10));
  const float sample = grain_layer_particle(d_abs, layer_dmax[idx],
                                         layer_npart[idx] * npart_scale, unif_ch, seed);

  const float wgt = layer_dmax[idx];
  const float expected = dens * (wgt - dmin) / dmax_full + dmin;
  dev[k] = sample - expected;
}

/* reset=1 stores the first sub-layer instead of needing a separate zero fill */
__kernel void grain_accumulate(__global float *acc,
                               __global const float *src,
                               const int w,
                               const int h,
                               const int reset)
{
  const int x = get_global_id(0), y = get_global_id(1);
  if(x >= w || y >= h) return;
  const size_t k = (size_t)y * w + x;
  acc[k] = reset ? src[k] : acc[k] + src[k];
}



/* Acutance recovery: g + amount * (g - blur(g)), with the blur supplied
   separately. Additive rather than multiplicative because g is the zero-mean
   grain deviation; see _particle_recover in grain.c. */
__kernel void grain_recover(__global float *g,
                            __global const float *blurred,
                            const int w,
                            const int h,
                            const float amount)
{
  const int x = get_global_id(0), y = get_global_id(1);
  if(x >= w || y >= h) return;
  const size_t k = (size_t)y * w + x;
  g[k] = g[k] + amount * (g[k] - blurred[k]);
}

/* Combine the three dye layers onto Lab's opponent axes and add them to the
   output window. in/out are addressed independently: in is the padded roi_in
   buffer, out the roi_out one. */
__kernel void grain_combine(__global const float4 *in,
                            __global float4 *out,
                            __global const float *g0,
                            __global const float *g1,
                            __global const float *g2,
                            const int ow,
                            const int oh,
                            const int iw,
                            const int dx,
                            const int dy,
                            const float k_l,
                            const float k_c)
{
  const int x = get_global_id(0), y = get_global_id(1);
  if(x >= ow || y >= oh) return;
  const size_t ki = (size_t)(y + dy) * iw + (dx + x);
  const size_t ko = (size_t)y * ow + x;

  const float a0 = g0[ki], a1 = g1[ki], a2 = g2[ki];
  float4 pix = in[ki];
  /* Left unclamped, as the host path leaves it: a clamp to [0,100] truncates
     the upper half of the draw wherever L is already 100. */
  pix.x += (a0 + a1 + a2) * k_l;
  pix.y += (a0 - a1) * k_c;
  pix.z += (a1 - a2) * k_c;
  out[ko] = pix;
}
