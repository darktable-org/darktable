/*
  This file is part of darktable,
  Copyright (C) 2026 darktable developers.

  darktable is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  darktable is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "common.h"

// the per-pixel steps of src/iop/cacorrectrgb.c, one kernel per loop there.
// The guide channel is a runtime index, so pixels go through float arrays.

#define CACORRECT_MAX_EV_DIFF 2.0f

static inline void _load4(read_only image2d_t img, const int x, const int y, float v[4])
{
  const float4 p = readpixel(img, x, y);
  v[0] = p.x;
  v[1] = p.y;
  v[2] = p.z;
  v[3] = p.w;
}

static inline void _store4(write_only image2d_t img, const int x, const int y, const float v[4])
{
  write_imagef(img, (int2)(x, y), (float4)(v[0], v[1], v[2], v[3]));
}

// the first loop of get_manifolds()
kernel void
cacorrectrgb_manifolds(read_only image2d_t in,
                       read_only image2d_t blurred_in,
                       write_only image2d_t manifold_higher,
                       write_only image2d_t manifold_lower,
                       const int width,
                       const int height,
                       const int guide)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;

  float pin[4], avg[4];
  _load4(in, x, y, pin);
  _load4(blurred_in, x, y, avg);

  const float pixelg = fmax(pin[guide], 1E-6f);
  float weighth = pixelg >= avg[guide] ? 1.0f : 0.0f;
  float weightl = pixelg <= avg[guide] ? 1.0f : 0.0f;
  float logdiffs[2];
  for(int kc = 0; kc <= 1; kc++)
  {
    const int c = (kc + guide + 1) % 3;
    logdiffs[kc] = dtcl_log2(fmax(pin[c], 1E-6f) / pixelg);
  }
  const float maxlogdiff = fmax(fabs(logdiffs[0]), fabs(logdiffs[1]));
  if(maxlogdiff > CACORRECT_MAX_EV_DIFF)
  {
    const float correction_weight = CACORRECT_MAX_EV_DIFF / maxlogdiff;
    weightl *= correction_weight;
    weighth *= correction_weight;
  }

  float high[4], low[4];
  for(int kc = 0; kc <= 1; kc++)
  {
    const int c = (kc + guide + 1) % 3;
    high[c] = logdiffs[kc] * weighth;
    low[c] = logdiffs[kc] * weightl;
  }
  high[guide] = pixelg * weighth;
  low[guide] = pixelg * weightl;
  high[3] = weighth;
  low[3] = weightl;
  _store4(manifold_higher, x, y, high);
  _store4(manifold_lower, x, y, low);
}

static inline void _normalize(const float blurred[4], float m[4], const int guide)
{
  const float weight = fmax(m[3], 1E-2f);
  const float mg = m[guide] / weight;
  m[guide] = mg;
  for(int kc = 0; kc <= 1; kc++)
  {
    const int c = (kc + guide + 1) % 3;
    m[c] = dtcl_exp2(m[c] / weight) * mg;
  }
  if(weight < 0.05f)
  {
    const float w = (weight - 0.01f) / (0.05f - 0.01f);
    for(int c = 0; c < 4; c++)
      m[c] = w * m[c] + (1.0f - w) * blurred[c];
  }
}

// normalize_manifolds()
kernel void
cacorrectrgb_normalize(read_only image2d_t blurred_in,
                       read_only image2d_t blurred_higher,
                       read_only image2d_t blurred_lower,
                       write_only image2d_t manifold_higher,
                       write_only image2d_t manifold_lower,
                       const int width,
                       const int height,
                       const int guide)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;

  float avg[4], high[4], low[4];
  _load4(blurred_in, x, y, avg);
  _load4(blurred_higher, x, y, high);
  _load4(blurred_lower, x, y, low);
  _normalize(avg, high, guide);
  _normalize(avg, low, guide);
  _store4(manifold_higher, x, y, high);
  _store4(manifold_lower, x, y, low);
}

// the refining loop of get_manifolds()
kernel void
cacorrectrgb_refine(read_only image2d_t in,
                    read_only image2d_t higher,
                    read_only image2d_t lower,
                    read_only image2d_t blurred_in,
                    write_only image2d_t manifold_higher,
                    write_only image2d_t manifold_lower,
                    const int width,
                    const int height,
                    const int guide)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;

  float pin[4], high[4], low[4], avg[4];
  _load4(in, x, y, pin);
  _load4(higher, x, y, high);
  _load4(lower, x, y, low);
  _load4(blurred_in, x, y, avg);

  const float pixelg = dtcl_log2(fmax(pin[guide], 1E-6f));
  const float highg = dtcl_log2(fmax(high[guide], 1E-6f));
  const float lowg = dtcl_log2(fmax(low[guide], 1E-6f));
  const float avgg = dtcl_log2(fmax(avg[guide], 1E-6f));

  float w = 1.0f;
  for(int kc = 0; kc <= 1; kc++)
  {
    const int c = (guide + kc + 1) % 3;
    const float pixel = dtcl_log2(fmax(pin[c], 1E-6f));
    const float highc = dtcl_log2(fmax(high[c], 1E-6f));
    const float lowc = dtcl_log2(fmax(low[c], 1E-6f));

    const float dist_to_ll = fabs(pixelg - lowg - pixel + lowc);
    const float dist_to_hh = fabs(pixelg - highg - pixel + highc);
    const float dist_to_lh = fabs((pixelg - pixel) - (highg - lowc));
    const float dist_to_hl = fabs((pixelg - pixel) - (lowg - highc));

    const int near_low = fabs(pixelg - lowg) < fabs(pixelg - highg);
    const float dist_to_good = near_low ? dist_to_ll : dist_to_hh;
    const float dist_to_bad = near_low ? dist_to_hl : dist_to_lh;

    w *= 1.0f * (0.2f + 1.0f / fmax(dist_to_good, 0.1f)) / (0.2f + 1.0f / fmax(dist_to_bad, 0.1f));
  }

  float logdiffs[2];
  for(int kc = 0; kc <= 1; kc++)
  {
    const int c = (guide + kc + 1) % 3;
    logdiffs[kc] = dtcl_log2(fmax(pin[c], 1E-6f)) - pixelg;
  }
  const float maxlogdiff = fmax(fabs(logdiffs[0]), fabs(logdiffs[1]));
  if(maxlogdiff > CACORRECT_MAX_EV_DIFF)
    w *= CACORRECT_MAX_EV_DIFF / maxlogdiff;

  // the pixel goes to one manifold, the other gets zero
  float m[4];
  for(int kc = 0; kc <= 1; kc++)
  {
    const int c = (kc + guide + 1) % 3;
    m[c] = logdiffs[kc] * w;
  }
  m[guide] = fmax(pin[guide], 0.0f) * w;
  m[3] = w;
  const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
  if(pixelg > avgg)
  {
    _store4(manifold_higher, x, y, m);
    _store4(manifold_lower, x, y, zero);
  }
  else
  {
    _store4(manifold_higher, x, y, zero);
    _store4(manifold_lower, x, y, m);
  }
}

// apply_correction(), with the manifolds upscaled to the full size
kernel void
cacorrectrgb_apply(read_only image2d_t in,
                   read_only image2d_t higher,
                   read_only image2d_t lower,
                   write_only image2d_t out,
                   const int width,
                   const int height,
                   const int guide,
                   const int mode)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;

  float pin[4], high[4], low[4];
  _load4(in, x, y, pin);
  _load4(higher, x, y, high);
  _load4(lower, x, y, low);

  const float high_guide = fmax(high[guide], 1E-6f);
  const float low_guide = fmax(low[guide], 1E-6f);
  const float log_high = dtcl_log2(high_guide);
  const float log_low = dtcl_log2(low_guide);
  const float dist_low_high = log_high - log_low;
  const float pixelg = fmax(pin[guide], 0.0f);
  const float log_pixg = dtcl_log2(fmin(fmax(pixelg, low_guide), high_guide));

  float weight_low = fabs(log_high - log_pixg) / fmax(dist_low_high, 1E-6f);
  const float threshold_dist_low_high = 0.25f;
  if(dist_low_high < threshold_dist_low_high)
  {
    const float weight = dist_low_high / threshold_dist_low_high;
    weight_low = weight_low * weight + 0.5f * (1.0f - weight);
  }
  const float weight_high = fmax(1.0f - weight_low, 0.0f);

  float o[4];
  for(int kc = 0; kc <= 1; kc++)
  {
    const int c = (guide + kc + 1) % 3;
    const float pixelc = fmax(pin[c], 0.0f);
    const float ratio_high_manifolds = high[c] / high_guide;
    const float ratio_low_manifolds = low[c] / low_guide;
    const float ratio = dtcl_pow(ratio_low_manifolds, weight_low) * dtcl_pow(ratio_high_manifolds, weight_high);
    const float outp = pixelg * ratio;
    o[c] = mode == 1 ? fmin(outp, pixelc) : mode == 2 ? fmax(outp, pixelc) : outp;
  }
  o[guide] = pixelg;
  o[3] = pin[3];
  _store4(out, x, y, o);
}

// the first loop of reduce_artifacts(): the two guided channels of in and out
kernel void
cacorrectrgb_artifacts_prepare(read_only image2d_t in,
                               read_only image2d_t corrected,
                               write_only image2d_t in_out,
                               const int width,
                               const int height,
                               const int guide)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;

  float pin[4], pout[4], o[4];
  _load4(in, x, y, pin);
  _load4(corrected, x, y, pout);
  for(int kc = 0; kc <= 1; kc++)
  {
    const int c = (guide + kc + 1) % 3;
    o[kc * 2 + 0] = pin[c];
    o[kc * 2 + 1] = pout[c];
  }
  _store4(in_out, x, y, o);
}

// the second loop of reduce_artifacts()
kernel void
cacorrectrgb_artifacts(read_only image2d_t in,
                       read_only image2d_t corrected,
                       read_only image2d_t blurred_in_out,
                       write_only image2d_t out,
                       const int width,
                       const int height,
                       const int guide,
                       const float safety)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;

  float pin[4], o[4], avg[4];
  _load4(in, x, y, pin);
  _load4(corrected, x, y, o);
  _load4(blurred_in_out, x, y, avg);

  float w = 1.0f;
  for(int kc = 0; kc <= 1; kc++)
  {
    const float avg_in = dtcl_log2(fmax(avg[kc * 2 + 0], 1E-6f));
    const float avg_out = dtcl_log2(fmax(avg[kc * 2 + 1], 1E-6f));
    w *= dtcl_exp(-fmax(fabs(avg_out - avg_in), 0.01f) * safety);
  }
  for(int kc = 0; kc <= 1; kc++)
  {
    const int c = (guide + kc + 1) % 3;
    o[c] = fmax(1.0f - w, 0.0f) * fmax(pin[c], 0.0f) + w * fmax(o[c], 0.0f);
  }
  _store4(out, x, y, o);
}
