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
  if (x <= knee)
    return x;
  const float range = maximum - knee;
  if (range <= 0.f)
    return maximum;
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

  // Use explicit row-major arrays matching the CPU transposed matrix representation
  const float AI_trans_1[3] = {0.1386050432715393f, -0.1386050432715393f, -0.0960192420263190f};
  const float AI_trans_2[3] = {0.0580473161561189f, -0.0580473161561189f, -0.8118918960560390f};

  const float4 AI[3] = {
      {1.0f, AI_trans_1[0], AI_trans_2[0], 0.0f},
      {1.0f, AI_trans_1[1], AI_trans_2[1], 0.0f},
      {1.0f, AI_trans_1[2], AI_trans_2[2], 0.0f}};

  const float4 izab = {Iz, Cz * ch, Cz * sh, 0.f};
  const float lms0 = dot(AI[0], izab);
  const float lms1 = dot(AI[1], izab);
  const float lms2 = dot(AI[2], izab);

  float max_c = Cz;
  if (lms0 < 0.f)
    max_c = fmin(max_c, -Iz / (AI_trans_1[0] * ch + AI_trans_2[0] * sh));
  if (lms1 < 0.f)
    max_c = fmin(max_c, -Iz / (AI_trans_1[1] * ch + AI_trans_2[1] * sh));
  if (lms2 < 0.f)
    max_c = fmin(max_c, -Iz / (AI_trans_1[2] * ch + AI_trans_2[2] * sh));

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
// mirrors satcurve_ucs_gamut_saturation() on CPU
static inline float satcurve_ucs_gamut_saturation_cl(const float J, const float h,
                                                     const float L_white,
                                                     global const float *const gamut_lut)
{
  const float max_colorfulness = fmax(satcurve_lookup_gamut_cl(gamut_lut, h), FLT_MIN);
  const float max_chroma = 15.932993652962535f * dtcl_pow(J / L_white, 0.6523997524738018f) * dtcl_pow(max_colorfulness, 0.6007557017508491f) / L_white;

  const float4 JCH_gamut_boundary = {J, max_chroma, h, 0.f};
  const float4 HSB_gamut_boundary = dt_UCS_JCH_to_HSB(JCH_gamut_boundary);

  return fmax(HSB_gamut_boundary.y, FLT_MIN);
}

// s_in_norm only (no full pixel transform) -- used by the histogram reduction kernel
static inline float satcurve_s_in_norm_cl(const float4 rgb_in,
                                          constant const float *const matrix_in,
                                          global const float *const gamut_lut,
                                          const int formula,
                                          const float L_white)
{
  const float4 rgb = fmax(rgb_in, 0.f);
  const float4 xyz = matrix_product_float4(rgb, matrix_in);

  if (formula == DT_IOP_SATCURVE_DTUCS)
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
  if (x >= width || y >= height)
    return;

  const float4 rgb_in = Areadpixel(in, x, y);
  const float s_in_norm = satcurve_s_in_norm_cl(rgb_in, matrix_in, gamut_lut, formula, L_white);

  const int bin = clamp((int)(s_in_norm * (DT_IOP_SATCURVE_HIST_RES - 1)), 0, DT_IOP_SATCURVE_HIST_RES - 1);
  atomic_inc(hist_bins + bin);
}

kernel void
satcurvergb(read_only image2d_t in, write_only image2d_t out,
            read_only image2d_t sat_mask_in, read_only image2d_t bri_mask_in,
            read_only image2d_t noise_confidence, const int use_mask,
            const int width, const int height,
            constant const float *const matrix_in, constant const float *const matrix_out,
            global const float *const sat_lut, global const float *const bri_lut,
            global const float *const gamut_lut,
            const int formula, const float L_white, const float noise_protection)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if (x >= width || y >= height)
    return;

  float4 pix_in = Areadpixel(in, x, y);
  float4 rgb = fmax(pix_in, 0.f);

  float4 xyz = matrix_product_float4(rgb, matrix_in);
  float4 pixout;

  if (formula == DT_IOP_SATCURVE_DTUCS)
  {
    float4 xyY = dt_D65_XYZ_to_xyY(xyz);
    float4 JCH = xyY_to_dt_UCS_JCH(xyY, L_white);
    float4 HCB = dt_UCS_JCH_to_HCB(JCH);

    float4 HSB = {HCB.x, HCB.z > 0.f ? HCB.y / HCB.z : 0.f, HCB.z, 0.f};

    const float gamut_s = satcurve_ucs_gamut_saturation_cl(JCH.x, JCH.z, L_white, gamut_lut);
    const float raw_s_in = HSB.y / gamut_s;

    // Determine input saturation per channel: either from the filtered/blended
    // masks (guided filter or noise-aware blend, both use the same code path
    // here) or directly from the pixel for both channels.
    const float s_in_sat = use_mask ? clamp(Areadsingle(sat_mask_in, x, y), 0.f, 1.f) : raw_s_in;
    const float s_in_bri = use_mask ? clamp(Areadsingle(bri_mask_in, x, y), 0.f, 1.f) : raw_s_in;

    const float sat_c = clamp(satcurve_lookup_lut_cl(sat_lut, s_in_sat), 0.f, 1.f);
    const float bri_c = clamp(satcurve_lookup_lut_cl(bri_lut, s_in_bri), 0.f, 1.f);
    const float noise_conf = use_mask ? clamp(Areadsingle(noise_confidence, x, y), 0.f, 1.f) : 0.f;
    const float effect = 1.f - clamp(noise_protection * noise_conf, 0.f, 1.f);
    const float sat_factor = 1.f + effect * (curve_to_factor_cl(sat_c) - 1.f);
    const float bri_factor = 1.f + effect * (curve_to_factor_cl(bri_c) - 1.f);

    HSB.y = fmax(HSB.y * sat_factor, 0.f);
    HSB.y = satcurve_soft_clip_cl(HSB.y, 0.8f * gamut_s, gamut_s);

    JCH = dt_UCS_HSB_to_JCH(HSB);
    HCB = dt_UCS_JCH_to_HCB(JCH);

    HCB.y = fmax(HCB.y * bri_factor, 0.f);
    HCB.z = fmax(HCB.z * bri_factor, 0.f);

    JCH = dt_UCS_HCB_to_JCH(HCB);

    const float gamut_s_out = satcurve_ucs_gamut_saturation_cl(JCH.x, JCH.z, L_white, gamut_lut);

    float4 HSB_out = {HCB.x, HCB.z > 0.f ? HCB.y / HCB.z : 0.f, HCB.z, 0.f};
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
    const float raw_s_in = (Jz > 0.f ? Cz / Jz : 0.f) / gamut;

    // Determine input saturation per channel: either from the filtered/blended
    // masks (guided filter or noise-aware blend, both use the same code path
    // here) or directly from the pixel for both channels.
    const float s_in_sat = use_mask ? clamp(Areadsingle(sat_mask_in, x, y), 0.f, 1.f) : raw_s_in;
    const float s_in_bri = use_mask ? clamp(Areadsingle(bri_mask_in, x, y), 0.f, 1.f) : raw_s_in;

    const float sat_c = clamp(satcurve_lookup_lut_cl(sat_lut, s_in_sat), 0.f, 1.f);
    const float bri_c = clamp(satcurve_lookup_lut_cl(bri_lut, s_in_bri), 0.f, 1.f);
    const float noise_conf = use_mask ? clamp(Areadsingle(noise_confidence, x, y), 0.f, 1.f) : 0.f;
    const float effect = 1.f - clamp(noise_protection * noise_conf, 0.f, 1.f);
    const float sat_factor = 1.f + effect * (curve_to_factor_cl(sat_c) - 1.f);
    const float bri_factor = 1.f + effect * (curve_to_factor_cl(bri_c) - 1.f);

    const float s_out = satcurve_soft_clip_cl(fmax(s_in_sat * sat_factor, 0.f), 0.8f, 1.f) * gamut;

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

// Visualization of the saturation mask for GUI display. Zero saturation is
// shown as white, full saturation as purple/magenta {0.5, 0.0, 0.5}, matching
// the chroma gradient end color used in blend_gui.c.
kernel void
satcurve_mask(read_only image2d_t in, write_only image2d_t out,
              const int width, const int height,
              constant const float *const matrix_in,
              global const float *const gamut_lut,
              const int formula, const float L_white)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if (x >= width || y >= height)
    return;

  const float4 pix_in = Areadpixel(in, x, y);
  const float s_in_norm = satcurve_s_in_norm_cl(pix_in, matrix_in, gamut_lut, formula, L_white);

  // white at t = 0, magenta {0.5, 0.0, 0.5} at t = 1
  const float t = sqrt(clamp(s_in_norm, 0.f, 1.f));
  float4 pixout = {1.0f - 0.5f * t, 1.0f - t, 1.0f - 0.5f * t, pix_in.w};

  write_imagef(out, (int2)(x, y), pixout);
}

// scalar (single-channel) saturation mask, feeding the guided filter pipeline
// in fast_guided_filter_scalar.cl. Mirrors compute_saturation_mask() on the
// CPU side: MAX(s_in_norm, 0.f), written into a single-channel buffer instead
// of the 3-channel visualisation that satcurve_mask above produces.
kernel void
satcurve_prepare_scalar_mask(read_only image2d_t in, write_only image2d_t out,
                            const int width, const int height,
                            constant const float *const matrix_in,
                            global const float *const gamut_lut,
                            const int formula, const float L_white)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if (x >= width || y >= height)
    return;

  const float4 pix_in = Areadpixel(in, x, y);
  const float s_in_norm = satcurve_s_in_norm_cl(pix_in, matrix_in, gamut_lut, formula, L_white);

  write_imagef(out, (int2)(x, y), fmax(s_in_norm, 0.f));
}

kernel void
satcurve_prepare_perceptual_guide(read_only image2d_t in, write_only image2d_t out,
                                 const int width, const int height,
                                 constant const float *const matrix_in,
                                 const float L_white)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if (x >= width || y >= height) return;

  const float4 rgb = fmax(Areadpixel(in, x, y), 0.f);
  const float4 xyz = matrix_product_float4(rgb, matrix_in);
  const float4 xyY = dt_D65_XYZ_to_xyY(xyz);
  const float4 JCH = xyY_to_dt_UCS_JCH(xyY, L_white);
  const float4 guide = (float4)(JCH.x / fmax(L_white, 1e-6f),
                                JCH.y * dtcl_cos(JCH.z) / fmax(L_white, 1e-6f),
                                JCH.y * dtcl_sin(JCH.z) / fmax(L_white, 1e-6f), 0.f);
  write_imagef(out, (int2)(x, y), guide);
}

static inline float smoothstep01_cl(const float edge0, const float edge1, const float x)
{
  const float t = clamp((x - edge0) / fmax(edge1 - edge0, 1e-6f), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

kernel void
satcurve_prepare_filter_confidence(read_only image2d_t raw, read_only image2d_t filtered,
                                    read_only image2d_t guide, write_only image2d_t control,
                                    write_only image2d_t confidence,
                                    const int width, const int height,
                                    const float protect_from, const float protect_to)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if (x >= width || y >= height) return;

  const float raw_center = Areadsingle(raw, x, y);
  const float filtered_center = Areadsingle(filtered, x, y);
  const float4 guide_center = Areadpixel(guide, x, y);
  float residual_energy = 0.f, edge_energy = 0.f;
  for (int dy = -1; dy <= 1; dy++)
    for (int dx = -1; dx <= 1; dx++)
    {
      const int nx = clamp(x + dx, 0, width - 1);
      const int ny = clamp(y + dy, 0, height - 1);
      const float residual = Areadsingle(raw, nx, ny) - Areadsingle(filtered, nx, ny);
      const float4 delta = Areadpixel(guide, nx, ny) - guide_center;
      residual_energy += residual * residual;
      edge_energy += dot(delta, delta);
    }
  residual_energy /= 9.f;
  edge_energy /= 27.f;
  const float noisy = smoothstep(0.0005f, 0.01f, residual_energy);
  const float edge_safe = 1.f - smoothstep(0.0005f, 0.01f, edge_energy);
  const float conf = noisy * edge_safe;
  write_imagef(confidence, (int2)(x, y), conf);

  // Mirrors the CPU main loop: protection_weight() called with
  // noise_protection = 0.0f -> only the neutral-region weight applies here.
  const float neutral_w = smoothstep01_cl(protect_from, protect_to, raw_center);
  write_imagef(control, (int2)(x, y), clamp(raw_center + neutral_w * (filtered_center - raw_center), 0.f, 1.f));
}

kernel void
satcurve_mask_from_control(read_only image2d_t raw, read_only image2d_t filtered,
                            read_only image2d_t confidence, read_only image2d_t in,
                            write_only image2d_t out,
                            const int width, const int height,
                            const float protect_from, const float protect_to,
                            const float noise_protection)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);
  if (x >= width || y >= height) return;

  const float raw_value = Areadsingle(raw, x, y);
  const float filtered_value = Areadsingle(filtered, x, y);
  const float conf = clamp(Areadsingle(confidence, x, y), 0.f, 1.f);

  const float neutral_w = smoothstep01_cl(protect_from, protect_to, raw_value);
  const float noise_w = 1.f - clamp(noise_protection * conf, 0.f, 1.f);
  const float weight = neutral_w * noise_w;

  const float value = clamp(raw_value + weight * (filtered_value - raw_value), 0.f, 1.f);
  const float t = sqrt(value);
  const float4 pixel = Areadpixel(in, x, y);
  write_imagef(out, (int2)(x, y), (float4)(1.f - .5f * t, 1.f - t, 1.f - .5f * t, pixel.w));
}