/*
 * HDR transfer functions shared by the CPU and OpenCL color pipelines.
 * Copyright (C) 2026 darktable developers.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#if defined(__OPENCL_VERSION__) || defined(__OPENCL_C_VERSION__)
#define DT_HDR_POW pow
#define DT_HDR_EXP exp
#define DT_HDR_LOG log
#define DT_HDR_SQRT sqrt
#define DT_HDR_MIN fmin
#define DT_HDR_MAX fmax
#else
#include <math.h>
#define DT_HDR_POW powf
#define DT_HDR_EXP expf
#define DT_HDR_LOG logf
#define DT_HDR_SQRT sqrtf
#define DT_HDR_MIN fminf
#define DT_HDR_MAX fmaxf
#endif

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

static inline float dt_hdr_pq_decode(const float signal)
{
  const float e = DT_HDR_MIN(DT_HDR_MAX(signal, 0.0f), 1.0f);
  const float p = DT_HDR_POW(e, 1.0f / DT_HDR_PQ_M2);
  const float n = DT_HDR_MAX(p - DT_HDR_PQ_C1, 0.0f);
  const float d = DT_HDR_PQ_C2 - DT_HDR_PQ_C3 * p;
  return (DT_HDR_PQ_PEAK_NITS / DT_HDR_REFERENCE_WHITE_NITS)
    * DT_HDR_POW(n / d, 1.0f / DT_HDR_PQ_M1);
}

static inline float dt_hdr_pq_encode(const float linear)
{
  const float l = DT_HDR_MIN(DT_HDR_MAX(linear, 0.0f)
    * (DT_HDR_REFERENCE_WHITE_NITS / DT_HDR_PQ_PEAK_NITS), 1.0f);
  const float p = DT_HDR_POW(l, DT_HDR_PQ_M1);
  return DT_HDR_POW((DT_HDR_PQ_C1 + DT_HDR_PQ_C2 * p)
                   / (1.0f + DT_HDR_PQ_C3 * p), DT_HDR_PQ_M2);
}

static inline float dt_hdr_hlg_decode(const float signal)
{
  const float e = DT_HDR_MIN(DT_HDR_MAX(signal, 0.0f), 1.0f);
  return e <= 0.5f ? e * e / 3.0f
    : (DT_HDR_EXP((e - DT_HDR_HLG_C) / DT_HDR_HLG_A) + DT_HDR_HLG_B) / 12.0f;
}

static inline float dt_hdr_hlg_encode(const float scene)
{
  const float e = DT_HDR_MAX(scene, 0.0f);
  return DT_HDR_MIN(e <= 1.0f / 12.0f ? DT_HDR_SQRT(3.0f * e)
    : DT_HDR_HLG_A * DT_HDR_LOG(12.0f * e - DT_HDR_HLG_B) + DT_HDR_HLG_C, 1.0f);
}

static inline float dt_hdr_hlg_luminance(const float rgb[3], const int transfer)
{
  /* D65 luminance coefficients, before chromatic adaptation to D50. */
  return transfer == DT_HDR_HLG_P3
    ? 0.22897456f * rgb[0] + 0.69173852f * rgb[1] + 0.07928691f * rgb[2]
    : 0.2627f * rgb[0] + 0.6780f * rgb[1] + 0.0593f * rgb[2];
}

static inline void dt_hdr_decode(float rgb[3], const int transfer)
{
  if(transfer == DT_HDR_PQ)
  {
    for(int c = 0; c < 3; c++) rgb[c] = dt_hdr_pq_decode(rgb[c]);
  }
  else if(transfer != DT_HDR_NONE)
  {
    for(int c = 0; c < 3; c++) rgb[c] = dt_hdr_hlg_decode(rgb[c]);
    const float y = dt_hdr_hlg_luminance(rgb, transfer);
    const float scale = (DT_HDR_HLG_PEAK_NITS / DT_HDR_REFERENCE_WHITE_NITS)
      * DT_HDR_POW(y, DT_HDR_HLG_SYSTEM_GAMMA - 1.0f);
    for(int c = 0; c < 3; c++) rgb[c] *= scale;
  }
}

static inline void dt_hdr_encode(float rgb[3], const int transfer)
{
  if(transfer == DT_HDR_PQ)
  {
    for(int c = 0; c < 3; c++) rgb[c] = dt_hdr_pq_encode(rgb[c]);
  }
  else if(transfer != DT_HDR_NONE)
  {
    for(int c = 0; c < 3; c++) rgb[c] = DT_HDR_MAX(rgb[c], 0.0f);
    const float y = dt_hdr_hlg_luminance(rgb, transfer);
    const float scale = y > 0.0f
      ? DT_HDR_POW(DT_HDR_REFERENCE_WHITE_NITS / DT_HDR_HLG_PEAK_NITS,
                   1.0f / DT_HDR_HLG_SYSTEM_GAMMA)
        * DT_HDR_POW(y, 1.0f / DT_HDR_HLG_SYSTEM_GAMMA - 1.0f)
      : 0.0f;
    for(int c = 0; c < 3; c++) rgb[c] = dt_hdr_hlg_encode(rgb[c] * scale);
  }
}
