/*
    This file is part of darktable,
    Copyright (C) 2024 darktable developers.

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

// Pull the pixel hue toward the nearest harmony node, scaled by Gaussian proximity.
// Also returns the winning node index and its max Gaussian weight via output pointers.
//
// The shift equals (nearest_node - px_hue) * max_w, so:
//   - A pixel already at a node produces zero shift.
//   - A pixel far from all nodes gets near-zero shift (max_w ≈ 0).
static inline float get_weighted_hue_shift(const float px_hue,
                                           constant const float *const nodes,
                                           const int num_nodes,
                                           const float zone_width_factor,
                                           int *out_winning_idx,
                                           float *out_max_weight)
{
  if(num_nodes <= 0)
  {
    *out_winning_idx = 0;
    *out_max_weight = 0.0f;
    return 0.0f;
  }
  const float sigma = zone_width_factor * 0.5f / (float)num_nodes;
  const float inv_2sigma2 = 1.0f / (2.0f * sigma * sigma);

  float max_w        = 0.0f;
  int   winning_idx  = 0;
  float diff_winning = 0.0f;

  for(int i = 0; i < num_nodes; i++)
  {
    float d = fabs(px_hue - nodes[i]);
    if(d > 0.5f) d = 1.0f - d;

    const float w = exp(-d * d * inv_2sigma2);
    float diff = nodes[i] - px_hue;
    if(diff > 0.5f)       diff -= 1.0f;
    else if(diff < -0.5f) diff += 1.0f;

    if(w > max_w)
    {
      max_w        = w;
      winning_idx  = i;
      diff_winning = diff;
    }
  }

  *out_winning_idx = winning_idx;
  *out_max_weight  = max_w;
  return diff_winning * max_w;
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

  const float4 pix_in = read_imagef(in, sampleri, (int2)(x, y));
  float4 XYZ_D65 = matrix_product_float4(fmax(0.0f, pix_in), matrix_in);
  float4 xyY = dt_D65_XYZ_to_xyY(XYZ_D65);
  float4 JCH = xyY_to_dt_UCS_JCH(xyY, L_white);

  const float hue = (JCH.z + M_PI_F) / (2.0f * M_PI_F);

  const int idx = y * width + x;
  jch_out[idx] = (float4)(JCH.x, JCH.y, hue, pix_in.w);

  int   winning_idx = 0;
  float max_weight  = 0.0f;
  const float hue_shift = get_weighted_hue_shift(hue, nodes, num_nodes, zone_width,
                                                 &winning_idx, &max_weight);

  const float sd = (node_saturation[winning_idx] - 1.0f) * max_weight;

  p_out[idx] = (float2)(hue_shift, sd);
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
  JCH.z = wrap_hue(hue + corr.x * effect_strength * chroma_weight) * 2.0f * M_PI_F - M_PI_F;

  float4 xyY = dt_UCS_JCH_to_xyY(JCH, L_white);
  float4 XYZ_D65 = dt_xyY_to_XYZ(xyY);

  float4 pix_out = matrix_product_float4(XYZ_D65, matrix_out);
  pix_out.w = cached.w;
  write_imagef(out, (int2)(x, y), pix_out);
}
