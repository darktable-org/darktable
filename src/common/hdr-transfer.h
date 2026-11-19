/*
 * HDR transfer functions for the CPU color pipeline. The OpenCL counterpart
 * is data/kernels/hdr_transfer.h; keep both files in sync.
 * Copyright (C) 2026 darktable developers.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include "common/math.h"

#define DT_HDR_NONE 0
#define DT_HDR_PQ 1
#define DT_HDR_HLG_REC2020 2
#define DT_HDR_HLG_P3 3

/* BT.2408 reference white; BT.2100 nominal HLG display, zero black. */
#define DT_HDR_REFERENCE_WHITE_NITS 203.0f
#define DT_HDR_PQ_PEAK_NITS 10000.0f
#define DT_HDR_HLG_PEAK_NITS 1000.0f
#define DT_HDR_HLG_SYSTEM_GAMMA 1.2f
#define DT_HDR_PQ_M1 (2610.0f / 16384.0f)
#define DT_HDR_PQ_M2 (2523.0f / 32.0f)
#define DT_HDR_PQ_C1 (3424.0f / 4096.0f)
#define DT_HDR_PQ_C2 (2413.0f / 128.0f)
#define DT_HDR_PQ_C3 (2392.0f / 128.0f)
#define DT_HDR_HLG_A 0.17883277f
#define DT_HDR_HLG_B 0.28466892f
#define DT_HDR_HLG_C 0.55991073f

/* 1 / C1^M2 as a float pair; C1^M2 = 7.31e-7 is the signal of PQ black */
#define DT_HDR_PQ_BLACK_SCALE 1368071.625f
#define DT_HDR_PQ_BLACK_SCALE_LO -0.040281184f
/* below this signal, 7.1e-7 nits, the decode works relative to black */
#define DT_HDR_PQ_DARK_SIGNAL 1e-4f

static inline float dt_hdr_pq_decode(const float signal)
{
  const float e = CLIP(signal);
  /*
   * p = e^(1/M2) lies in [C1, 1] and a float holds it to 3e-8. Both p - C1
   * (0 at black) and C2 - C3 p (0.16 at signal 1, from C3 p = 18.7) cancel,
   * and the final power 1/M1 = 6.3 amplifies what is left. Take
   * x = p / s - 1 from expm1() instead: s = 1 in general, s = C1 near black,
   * where e / C1^M2 - 1 comes from an fma with the split scale. With
   * C2 - C3 = 1 - C1:
   *   s = 1:   n = (1 - C1) + x   d = (1 - C1) - C3 x
   *   s = C1:  n = C1 x           d = (C2 - C3 C1) - C3 C1 x
   * The -0.5 keeps log1p() finite; signals below half of black decode to 0.
   */
  const int dark = e < DT_HDR_PQ_DARK_SIGNAL;
  const float l = dark
    ? log1pf(fmaxf(fmaf(e, DT_HDR_PQ_BLACK_SCALE, -1.0f) + e * DT_HDR_PQ_BLACK_SCALE_LO, -0.5f))
    : logf(e);
  const float x = expm1f(l * (1.0f / DT_HDR_PQ_M2));
  const float n = fmaxf(dark ? DT_HDR_PQ_C1 * x : (1.0f - DT_HDR_PQ_C1) + x, 0.0f);
  const float d = dark ? (DT_HDR_PQ_C2 - DT_HDR_PQ_C3 * DT_HDR_PQ_C1) - (DT_HDR_PQ_C3 * DT_HDR_PQ_C1) * x
                       : (1.0f - DT_HDR_PQ_C1) - DT_HDR_PQ_C3 * x;
  return (DT_HDR_PQ_PEAK_NITS / DT_HDR_REFERENCE_WHITE_NITS)
    * powf(n / d, 1.0f / DT_HDR_PQ_M1);
}

static inline float dt_hdr_pq_encode(const float linear)
{
  const float l = CLIP(linear * (DT_HDR_REFERENCE_WHITE_NITS / DT_HDR_PQ_PEAK_NITS));
  const float p = powf(l, DT_HDR_PQ_M1);
  return powf((DT_HDR_PQ_C1 + DT_HDR_PQ_C2 * p)
                   / (1.0f + DT_HDR_PQ_C3 * p), DT_HDR_PQ_M2);
}

static inline float dt_hdr_hlg_decode(const float signal)
{
  const float e = CLIP(signal);
  return e <= 0.5f ? e * e / 3.0f
    : (expf((e - DT_HDR_HLG_C) / DT_HDR_HLG_A) + DT_HDR_HLG_B) / 12.0f;
}

static inline float dt_hdr_hlg_encode(const float scene)
{
  const float e = fmaxf(scene, 0.0f);
  return fminf(e <= 1.0f / 12.0f ? sqrtf(3.0f * e)
    : DT_HDR_HLG_A * logf(12.0f * e - DT_HDR_HLG_B) + DT_HDR_HLG_C, 1.0f);
}

static inline float dt_hdr_hlg_luminance(const float rgb[3], const int transfer)
{
  /* D65 luminance coefficients, before chromatic adaptation to D50. */
  return transfer == DT_HDR_HLG_P3
    ? 0.22897456f * rgb[0] + 0.69173852f * rgb[1] + 0.07928691f * rgb[2]
    : 0.2627f * rgb[0] + 0.6780f * rgb[1] + 0.0593f * rgb[2];
}

/* takes display light of luminance y to HLG scene light: the inverse of
   the BT.2100 OOTF */
static inline float dt_hdr_hlg_scene_scale(const float y)
{
  return y > 0.0f
    ? powf(DT_HDR_REFERENCE_WHITE_NITS / DT_HDR_HLG_PEAK_NITS,
                 1.0f / DT_HDR_HLG_SYSTEM_GAMMA)
      * powf(y, 1.0f / DT_HDR_HLG_SYSTEM_GAMMA - 1.0f)
    : 0.0f;
}

static inline void dt_hdr_decode(dt_aligned_pixel_t rgb, const int transfer)
{
  if(transfer == DT_HDR_PQ)
  {
    for_three_channels(c) rgb[c] = dt_hdr_pq_decode(rgb[c]);
  }
  else if(transfer != DT_HDR_NONE)
  {
    for_three_channels(c) rgb[c] = dt_hdr_hlg_decode(rgb[c]);
    const float y = dt_hdr_hlg_luminance(rgb, transfer);
    const float scale = (DT_HDR_HLG_PEAK_NITS / DT_HDR_REFERENCE_WHITE_NITS)
      * powf(y, DT_HDR_HLG_SYSTEM_GAMMA - 1.0f);
    for_three_channels(c) rgb[c] *= scale;
  }
}

static inline void dt_hdr_encode(dt_aligned_pixel_t rgb, const int transfer)
{
  if(transfer == DT_HDR_PQ)
  {
    for_three_channels(c) rgb[c] = dt_hdr_pq_encode(rgb[c]);
  }
  else if(transfer != DT_HDR_NONE)
  {
    dt_vector_clipneg(rgb);
    const float scale = dt_hdr_hlg_scene_scale(dt_hdr_hlg_luminance(rgb, transfer));
    for_three_channels(c) rgb[c] = dt_hdr_hlg_encode(rgb[c] * scale);
  }
}
