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

#include "common.h"
#include "colorspace.h"

static inline float wrap_hue(float h)
{
  h = fmod(h, 1.0f);
  if(h < 0.0f) h += 1.0f;
  return h;
}

// Pull the pixel hue toward the nearest harmony node, scaled by Gaussian proximity,
// and return the matching saturation delta. Mirrors get_weighted_hue_shift() in
// src/iop/colorharmonizer.c, which explains the boundary band.
//
// The shift equals (nearest_node - px_hue) * max_w, so:
//   - A pixel already at a node produces zero shift.
//   - A pixel far from all nodes gets near-zero shift (max_w ≈ 0).
#define COLORHARMONIZER_MAX_NODES 4
#define COLORHARMONIZER_BOUNDARY_BAND 0.01f

static inline float2 get_weighted_hue_shift(const float px_hue,
                                            constant const float *const nodes,
                                            constant const float *const node_saturation,
                                            const int num_nodes,
                                            const float zone_width_factor)
{
  if(num_nodes <= 0) return (float2)(0.0f, 0.0f);

  const float sigma = zone_width_factor * 0.5f / (float)num_nodes;
  const float inv_2sigma2 = 1.0f / (2.0f * sigma * sigma);

  float diff[2 * COLORHARMONIZER_MAX_NODES];
  int n = 0;
  float d_min = 1.0f;
  for(int i = 0; i < num_nodes; i++)
  {
    float s = nodes[i] - px_hue;
    if(s > 0.5f)       s -= 1.0f;
    else if(s < -0.5f) s += 1.0f;
    diff[n++] = s;
    diff[n++] = s - copysign(1.0f, s);
    d_min = fmin(d_min, fabs(s));
  }

  float sum_a = 0.0f, shift = 0.0f, sat = 0.0f;
  for(int k = 0; k < n; k++)
  {
    const float d = fabs(diff[k]);
    const float a = 1.0f - (d - d_min) / COLORHARMONIZER_BOUNDARY_BAND;
    if(a <= 0.0f) continue;
    const float w = exp(-d * d * inv_2sigma2);
    sum_a += a;
    shift += a * diff[k] * w;
    sat   += a * (node_saturation[k / 2] - 1.0f) * w;
  }

  return (float2)(shift / sum_a, sat / sum_a);
}

// Kernel 1: compute per-pixel correction maps (hue_delta, sat_delta) and
// cache the forward JCH conversion so the apply kernel can skip it.
// jch_out stores (J, chroma, normalized_hue, alpha) per pixel.
kernel void colorharmonizer_map(read_only  image2d_t  in,
                                global     float2    *p_out,
                                global     float4    *jch_out,
                                const int   width,
                                const int   height,
                                constant const float *const matrix_in,
                                constant const float *const nodes,
                                const int   num_nodes,
                                const float zone_width,
                                constant const float *const node_saturation,
                                const float L_white)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;

  const float4 pix_in = readpixel(in, x, y);
  float4 XYZ_D65 = matrix_product_float4(fmax(0.0f, pix_in), matrix_in);
  float4 xyY = dt_D65_XYZ_to_xyY(XYZ_D65);
  float4 JCH = xyY_to_dt_UCS_JCH(xyY, L_white);

  const float hue = (JCH.z + M_PI_F) / DT_2PI_F;

  const int idx = y * width + x;
  jch_out[idx] = (float4)(JCH.x, JCH.y, hue, pix_in.w);

  p_out[idx] = get_weighted_hue_shift(hue, nodes, node_saturation, num_nodes, zone_width);
}

// Kernel 2: apply Gaussian-smoothed corrections using cached JCH to produce output.
// Reads from the JCH cache written by the map kernel, avoiding a redundant
// forward RGB → JCH conversion.
kernel void colorharmonizer_apply(write_only image2d_t  out,
                                  const int   width,
                                  const int   height,
                                  constant const float *const matrix_out,
                                  global const float4 *jch_in,
                                  global const float2 *corrections,
                                  const float effect_strength,
                                  const float protect_neutral,
                                  const float L_white)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;

  const int    k      = y * width + x;
  const float4 cached = jch_in[k];
  const float  J      = cached.x;
  const float  chroma = cached.y;
  const float  hue    = cached.z;

  const float2 corr = corrections[k];

  const float t             = protect_neutral;
  const float cutoff        = t * t * t * 0.03f;
  const float chroma_weight = chroma / (chroma + cutoff + 1.0e-5f);

  float4 JCH;
  JCH.x = J;
  JCH.y = fmax(chroma * (1.0f + corr.y * chroma_weight), 0.0f);
  JCH.z = wrap_hue(hue + corr.x * effect_strength * chroma_weight) * DT_2PI_F - M_PI_F;

  float4 xyY = dt_UCS_JCH_to_xyY(JCH, L_white);
  float4 XYZ_D65 = dt_xyY_to_XYZ(xyY);

  float4 pix_out = matrix_product_float4(XYZ_D65, matrix_out);
  pix_out.w = cached.w;
  write_imagef(out, (int2)(x, y), pix_out);
}
