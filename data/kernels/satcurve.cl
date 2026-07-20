/*
  This file is part of darktable,
  Copyright (C) 2009-2026 darktable developers.

  darktable is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  darktable is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
  GNU General Public License for more details.
*/

#include "common.h"
#include "colorspace.h"
#include "color_conversion.h"

#define DT_IOP_SATCURVE_RES 256

// periodic lookup in the hue-indexed gamut LUT; mirrors satcurve_lookup_gamut() on CPU
static inline float satcurve_lookup_gamut_cl(global const float *const gamut_lut, const float h)
{
  const float position = (h + M_PI_F) * (float)LUT_ELEM / (2.0f * M_PI_F);
  const float position_floor = floor(position);
  const int bin0 = ((int)position_floor) % LUT_ELEM;
  const int bin1 = (bin0 + 1) % LUT_ELEM;
  const float f = position - position_floor;
  return gamut_lut[bin0] * (1.f - f) + gamut_lut[bin1] * f;
}

// smoothly map x from knee onwards towards maximum; mirrors satcurve_soft_clip() on CPU
static inline float satcurve_soft_clip_cl(const float x, const float knee, const float maximum)
{
  if(x <= knee) return x;
  const float range = maximum - knee;
  if(range <= 0.f) return maximum;
  return knee + range * (1.f - dtcl_exp(-(x - knee) / range));
}

// linear interpolation into the 256-entry curve LUT
static inline float satcurve_lookup_lut_cl(global const float *const lut, const float x)
{
  const float position = clamp(x, 0.f, 1.f) * (DT_IOP_SATCURVE_RES - 1);
  const int i = min((int)position, DT_IOP_SATCURVE_RES - 2);
  return lut[i] + (position - i) * (lut[i + 1] - lut[i]);
}

// mirrors clip_jz_chroma() on CPU : test-converts to L'M'S' and clips Cz so
// the JzAzBz -> XYZ back-transform never produces negative LMS values
static inline float clip_jz_chroma_cl(const float Jz, const float Cz, const float ch, const float sh)
{
  const float d0 = 1.6295499532821566e-11f;
  const float dd = -0.56f;
  float Iz = (Jz + d0) / (1.f + dd - dd * (Jz + d0));
  Iz = fmax(Iz, 0.f);

  const float4 AI[3] = {
    { 1.0f,  0.1386050432715393f,  0.0580473161561189f, 0.0f },
    { 1.0f, -0.1386050432715393f, -0.0580473161561189f, 0.0f },
    { 1.0f, -0.0960192420263190f, -0.8118918960560390f, 0.0f }
  };

  const float4 izab = { Iz, Cz * ch, Cz * sh, 0.f };
  const float lms0 = dot(AI[0], izab);
  const float lms1 = dot(AI[1], izab);
  const float lms2 = dot(AI[2], izab);

  float max_c = Cz;
  if(lms0 < 0.f) max_c = fmin(max_c, -Iz / (AI[0].y * ch + AI[0].z * sh));
  if(lms1 < 0.f) max_c = fmin(max_c, -Iz / (AI[1].y * ch + AI[1].z * sh));
  if(lms2 < 0.f) max_c = fmin(max_c, -Iz / (AI[2].y * ch + AI[2].z * sh));
  return fmax(max_c, 0.f);
}

typedef enum dt_iop_satcurve_formula_t
{
  DT_IOP_SATCURVE_JZAZBZ = 0,
  DT_IOP_SATCURVE_DTUCS = 1
} dt_iop_satcurve_formula_t;

static inline float curve_to_factor_cl(const float c)
{
  return fmax(2.f * c, 0.f);
}

// maximum HSB saturation reachable at the gamut boundary for given J, h in dt UCS;
// mirrors satcurve_ucs_gamut_saturation() on CPU -- single source of truth for the
// gamut-boundary fit, do not re-derive it inline in the kernel.
static inline float satcurve_ucs_gamut_saturation_cl(const float J, const float h,
                                                      const float L_white,
                                                      global const float *const gamut_lut)
{
  const float max_colorfulness = fmax(satcurve_lookup_gamut_cl(gamut_lut, h), FLT_MIN);
  const float max_chroma = 15.932993652962535f
                         * dtcl_pow(J / L_white, 0.6523997524738018f)
                         * dtcl_pow(max_colorfulness, 0.6007557017508491f)
                         / L_white;

  const float4 JCH_gamut_boundary = { J, max_chroma, h, 0.f };
  const float4 HSB_gamut_boundary = dt_UCS_JCH_to_HSB(JCH_gamut_boundary);

  return fmax(HSB_gamut_boundary.y, FLT_MIN);
}

// s_in_norm only (no full pixel transform) -- used by the histogram
// reduction kernel below. Mirrors the s_in_norm computation embedded in each
// branch of satcurvergb() below and pixel_s_in_norm() on the CPU side.
static inline float satcurve_s_in_norm_cl(const float4 rgb_in,
                                          constant const float *const matrix_in,
                                          global const float *const gamut_lut,
                                          const int formula,
                                          const float L_white)
{
  const float4 rgb = fmax(rgb_in, 0.f);
  const float4 xyz = matrix_product_float4(rgb, matrix_in);

  if(formula == DT_IOP_SATCURVE_DTUCS)
  {
    const float4 xyY = dt_D65_XYZ_to_xyY(xyz);
    const float4 JCH = xyY_to_dt_UCS_JCH(xyY, L_white);
    const float4 HCB = dt_UCS_JCH_to_HCB(JCH);
    const float gamut_s = satcurve_ucs_gamut_saturation_cl(JCH.x, JCH.z, L_white, gamut_lut);
    const float saturation = HCB.z > 0.f ? HCB.y / HCB.z : 0.f;
    return saturation / gamut_s;
  }
  else
  {
    const float4 jab = XYZ_to_JzAzBz(xyz);
    const float Jz = fmax(jab.x, 0.f);
    const float Cz = hypot(jab.y, jab.z);
    const float h = atan2(jab.z, jab.y);
    const float gamut = fmax(satcurve_lookup_gamut_cl(gamut_lut, h), FLT_MIN);
    return (Jz > 0.f ? Cz / Jz : 0.f) / gamut;
  }
}

#define DT_IOP_SATCURVE_HIST_RES 256

// on-device histogram reduction: accumulates per-pixel s_in_norm into
// hist_bins so only DT_IOP_SATCURVE_HIST_RES ints need to cross the PCIe
// bus, instead of copying the entire image back to the host. hist_bins must
// be zeroed by the caller before this kernel runs.
kernel void
satcurve_histogram(read_only image2d_t in, const int width, const int height,
                   constant const float *const matrix_in,
                   global const float *const gamut_lut,
                   const int formula, const float L_white,
                   global int *const hist_bins)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;

  const float4 rgb_in = Areadpixel(in, x, y);
  const float s_in_norm = satcurve_s_in_norm_cl(rgb_in, matrix_in, gamut_lut, formula, L_white);

  const int bin = clamp((int)(s_in_norm * (DT_IOP_SATCURVE_HIST_RES - 1)), 0, DT_IOP_SATCURVE_HIST_RES - 1);
  atomic_inc(hist_bins + bin);
}

kernel void
satcurvergb(read_only image2d_t in, write_only image2d_t out,
            const int width, const int height,
            constant const float *const matrix_in, constant const float *const matrix_out,
            global const float *const sat_lut, global const float *const bri_lut,
            global const float *const gamut_lut,
            const int formula, const float L_white)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if(x >= width || y >= height) return;

  float4 pix_in = Areadpixel(in, x, y);
  float4 rgb = fmax(pix_in, 0.f);

  float4 xyz = matrix_product_float4(rgb, matrix_in);
  float4 pixout;

  if(formula == DT_IOP_SATCURVE_DTUCS)
  {
    float4 xyY = dt_D65_XYZ_to_xyY(xyz);
    float4 JCH = xyY_to_dt_UCS_JCH(xyY, L_white);
    float4 HCB = dt_UCS_JCH_to_HCB(JCH);

    float4 HSB = { HCB.x, HCB.z > 0.f ? HCB.y / HCB.z : 0.f, HCB.z, 0.f };

    const float gamut_s = satcurve_ucs_gamut_saturation_cl(JCH.x, JCH.z, L_white, gamut_lut);
    const float s_in_norm = HSB.y / gamut_s;

    const float sat_c = clamp(satcurve_lookup_lut_cl(sat_lut, s_in_norm), 0.f, 1.f);
    const float bri_c = clamp(satcurve_lookup_lut_cl(bri_lut, s_in_norm), 0.f, 1.f);
    const float sat_factor = curve_to_factor_cl(sat_c);
    const float bri_factor = curve_to_factor_cl(bri_c);

    HSB.y = fmax(HSB.y * sat_factor, 0.f);
    HSB.y = satcurve_soft_clip_cl(HSB.y, 0.8f * gamut_s, gamut_s);

    JCH = dt_UCS_HSB_to_JCH(HSB);
    HCB = dt_UCS_JCH_to_HCB(JCH);

    HCB.y = fmax(HCB.y * bri_factor, 0.f);
    HCB.z = fmax(HCB.z * bri_factor, 0.f);

    JCH = dt_UCS_HCB_to_JCH(HCB);

    const float gamut_s_out = satcurve_ucs_gamut_saturation_cl(JCH.x, JCH.z, L_white, gamut_lut);

    float4 HSB_out = { HCB.x, HCB.z > 0.f ? HCB.y / HCB.z : 0.f, HCB.z, 0.f };
    HSB_out.y = satcurve_soft_clip_cl(HSB_out.y, 0.8f * gamut_s_out, gamut_s_out);

    JCH = dt_UCS_HSB_to_JCH(HSB_out);
    xyY = dt_UCS_JCH_to_xyY(JCH, L_white);
    xyz = dt_xyY_to_XYZ(xyY);
  }
  else
  {
    float4 jab = XYZ_to_JzAzBz(xyz);

    const float Jz = fmax(jab.x, 0.f);
    const float Cz = hypot(jab.y, jab.z);
    const float h = atan2(jab.z, jab.y);
    const float ch = dtcl_cos(h), sh = dtcl_sin(h);
    const float gamut = fmax(satcurve_lookup_gamut_cl(gamut_lut, h), FLT_MIN);

    const float s_in_norm = (Jz > 0.f ? Cz / Jz : 0.f) / gamut;

    const float sat_c = clamp(satcurve_lookup_lut_cl(sat_lut, s_in_norm), 0.f, 1.f);
    const float bri_c = clamp(satcurve_lookup_lut_cl(bri_lut, s_in_norm), 0.f, 1.f);
    const float sat_factor = curve_to_factor_cl(sat_c);
    const float bri_factor = curve_to_factor_cl(bri_c);

    const float s_out = satcurve_soft_clip_cl(fmax(s_in_norm * sat_factor, 0.f), 0.8f, 1.f) * gamut;

    const float r = hypot(Jz, Cz);
    const float inv_norm = 1.f / sqrt(1.f + s_out * s_out);

    float Jz_tmp = r * inv_norm;
    float Cz_tmp = clip_jz_chroma_cl(Jz_tmp, r * s_out * inv_norm, ch, sh);

    Jz_tmp *= bri_factor;
    Cz_tmp = clip_jz_chroma_cl(Jz_tmp, Cz_tmp * bri_factor, ch, sh);

    jab.x = Jz_tmp;
    jab.y = Cz_tmp * ch;
    jab.z = Cz_tmp * sh;
    xyz = JzAzBz_2_XYZ(jab);
  }

  pixout = matrix_product_float4(xyz, matrix_out);
  pixout = fmax(pixout, 0.f);
  pixout.w = pix_in.w;

  write_imagef(out, (int2)(x, y), pixout);
}