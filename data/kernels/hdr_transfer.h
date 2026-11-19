/*
    This file is part of darktable,
    copyright (c) 2026 darktable developers.

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

/* OpenCL HDR transfer functions, the counterpart of src/common/hdr-transfer.h.
   Keep both files in sync. The math here avoids native functions and uses
   fp32_div() for every division by a runtime value: the PQ curve amplifies
   relative errors by 1/m1 (about 6.3), so the relaxed OpenCL division and
   native functions give visible CPU vs OpenCL differences. Divisions of two
   constants are folded by the compiler and stay plain. */

#pragma once

#include "common.h"

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

#define DT_HDR_PQ_BLACK_SCALE 1368071.625f
#define DT_HDR_PQ_BLACK_SCALE_LO -0.040281184f
#define DT_HDR_PQ_DARK_SIGNAL 1e-4f

/* see the CPU header for the derivation */
static inline float dt_hdr_pq_decode(const float signal)
{
  const float e = clipf(signal);
  const int dark = e < DT_HDR_PQ_DARK_SIGNAL;
  const float l = dark
    ? log1p(fmax(fma(e, DT_HDR_PQ_BLACK_SCALE, -1.0f) + e * DT_HDR_PQ_BLACK_SCALE_LO, -0.5f))
    : log(e);
  const float x = expm1(l * (1.0f / DT_HDR_PQ_M2));
  const float n = fmax(dark ? DT_HDR_PQ_C1 * x : (1.0f - DT_HDR_PQ_C1) + x, 0.0f);
  const float d = dark ? (DT_HDR_PQ_C2 - DT_HDR_PQ_C3 * DT_HDR_PQ_C1) - (DT_HDR_PQ_C3 * DT_HDR_PQ_C1) * x
                       : (1.0f - DT_HDR_PQ_C1) - DT_HDR_PQ_C3 * x;
  return (DT_HDR_PQ_PEAK_NITS / DT_HDR_REFERENCE_WHITE_NITS)
    * pow(fp32_div(n, d), 1.0f / DT_HDR_PQ_M1);
}

static inline float dt_hdr_pq_encode(const float linear)
{
  const float l = clipf(linear * (DT_HDR_REFERENCE_WHITE_NITS / DT_HDR_PQ_PEAK_NITS));
  const float p = pow(l, DT_HDR_PQ_M1);
  return pow(fp32_div(DT_HDR_PQ_C1 + DT_HDR_PQ_C2 * p, 1.0f + DT_HDR_PQ_C3 * p),
             DT_HDR_PQ_M2);
}

static inline float dt_hdr_hlg_decode(const float signal)
{
  const float e = clipf(signal);
  return e <= 0.5f ? fp32_div(e * e, 3.0f)
    : fp32_div(exp(fp32_div(e - DT_HDR_HLG_C, DT_HDR_HLG_A)) + DT_HDR_HLG_B, 12.0f);
}

static inline float dt_hdr_hlg_encode(const float scene)
{
  const float e = fmax(scene, 0.0f);
  return fmin(e <= 1.0f / 12.0f ? sqrt(3.0f * e)
    : DT_HDR_HLG_A * log(12.0f * e - DT_HDR_HLG_B) + DT_HDR_HLG_C, 1.0f);
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
  const float inv_gamma = 1.0f / DT_HDR_HLG_SYSTEM_GAMMA;
  return y > 0.0f
    ? pow(DT_HDR_REFERENCE_WHITE_NITS / DT_HDR_HLG_PEAK_NITS, inv_gamma)
      * pow(y, inv_gamma - 1.0f)
    : 0.0f;
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
      * pow(y, DT_HDR_HLG_SYSTEM_GAMMA - 1.0f);
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
    for(int c = 0; c < 3; c++) rgb[c] = fmax(rgb[c], 0.0f);
    const float scale = dt_hdr_hlg_scene_scale(dt_hdr_hlg_luminance(rgb, transfer));
    for(int c = 0; c < 3; c++) rgb[c] = dt_hdr_hlg_encode(rgb[c] * scale);
  }
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode c; remove-trailing-spaces on;
// clang-format on
