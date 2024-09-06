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
typedef enum dt_iop_colorequal_channel_t
{
  HUE = 0,
  SATURATION = 1,
  BRIGHTNESS = 2,
  NUM_CHANNELS = 3,
  GRAD_SWITCH = 4,
  SATURATION_GRAD = SATURATION + GRAD_SWITCH,
  BRIGHTNESS_GRAD = BRIGHTNESS + GRAD_SWITCH
} dt_iop_colorequal_channel_t;

#define NODES 8
#define SAT_EFFECT 2.0f
#define BRIGHT_EFFECT 8.0f

#include "common.h"
#include "colorspace.h"
#include "color_conversion.h"

#define SATSIZE 4096.0f
#define CENORM_MIN 1.52587890625e-05f // norm can't be < to 2^(-16)


static inline float _get_scaling(const float sigma)
{
  return max(1.0f, min(4.0f, floor(sigma - 1.5f)));
}

static inline float _interpolatef(const float a, const float b, const float c)
{
  return a * (b - c) + c;
}

static inline float _get_satweight(const float sat, global float *weights)
{
  const float isat = SATSIZE * (1.0f + clamp(sat, -1.0f, 1.0f - (1.0f / SATSIZE)));
  const float base = floor(isat);
  const int i = (int)base;
  return weights[i] + (isat - base) * (weights[i+1] - weights[i]);
}

static inline float scharr_gradient(__read_only image2d_t in,
                                      const int x,
                                      const int y)
{
  const float gx = 47.0f / 255.0f * (read_imagef(in, samplerA, (int2)(y-1, x-1)).x - read_imagef(in, samplerA, (int2)(y-1, x+1)).x
                                   + read_imagef(in, samplerA, (int2)(y+1, x-1)).x - read_imagef(in, samplerA, (int2)(y+1, x+1)).x)
                + 162.0f / 255.0f * (read_imagef(in, samplerA, (int2)(y,   x-1)).x - read_imagef(in, samplerA, (int2)(y,   x+1)).x);
  const float gy = 47.0f / 255.0f * (read_imagef(in, samplerA, (int2)(y-1, x-1)).x - read_imagef(in, samplerA, (int2)(y+1, x-1)).x
                                   + read_imagef(in, samplerA, (int2)(y-1, x+1)).x - read_imagef(in, samplerA, (int2)(y+1, x+1)).x)
                + 162.0f / 255.0f * (read_imagef(in, samplerA, (int2)(y-1, x)).x   - read_imagef(in, samplerA, (int2)(y+1, x)).x);

  return dt_fast_hypot(gx, gy);
}

static inline float4 gamut_map_HSB(const float4 HSB, global float *gamut_LUT, const float L_white)
{
  const float4 JCH = dt_UCS_HSB_to_JCH(HSB);
  const float max_colorfulness = lookup_gamut(gamut_LUT, JCH.z);
  const float max_chroma = 15.932993652962535f * dtcl_pow(JCH.x * L_white, 0.6523997524738018f) * dtcl_pow(max_colorfulness, 0.6007557017508491f) / L_white;
  const float4 JCH_gamut_boundary = { JCH.x, max_chroma, JCH.z, 0.0f };
  const float4 HSB_gamut_boundary = dt_UCS_JCH_to_HSB(JCH_gamut_boundary);

  // Soft-clip the current pixel saturation at constant brightness
  float4 gHSB = HSB;
  gHSB.y = soft_clip(HSB.y, 0.8f * HSB_gamut_boundary.y, HSB_gamut_boundary.y);
  return gHSB;
}

__kernel void init_covariance(__write_only image2d_t covariance,
                              __read_only image2d_t ds_UV,
                              const int width,
                              const int height)
{
  const int col = get_global_id(0);
  const int row = get_global_id(1);
  if(col >= width || row >= height) return;

  const float2 UV = read_imagef(ds_UV, samplerA, (int2)(col, row)).xy;
  const float4 CV = { UV.x * UV.x, UV.x * UV.y, UV.x * UV.y, UV.y * UV.y };
  write_imagef(covariance, (int2)(col, row), CV);
}

__kernel void finish_covariance(__write_only image2d_t covariance_out,
                                __read_only image2d_t covariance_in,
                                __read_only image2d_t ds_UV,
                                const int width,
                                const int height)
{
  const int col = get_global_id(0);
  const int row = get_global_id(1);
  if(col >= width || row >= height) return;

  const float2 UV = read_imagef(ds_UV, samplerA, (int2)(col, row)).xy;
  const float4 CV = read_imagef(covariance_in, samplerA, (int2)(col, row));
  const float4 CVO = { CV.x - UV.x * UV.x, CV.y - UV.x * UV.y, CV.z - UV.x * UV.y, CV.w - UV.y * UV.y };

  write_imagef(covariance_out, (int2)(col, row), CVO);
}

__kernel void prepare_prefilter(__read_only image2d_t ds_UV,
                                __read_only image2d_t covariance,
                                __write_only image2d_t img_a,
                                __write_only image2d_t img_b,
                                const float epsilon,
                                const int width,
                                const int height)
{
  const int col = get_global_id(0);
  const int row = get_global_id(1);
  if(col >= width || row >= height) return;

  const float2 UV = read_imagef(ds_UV, samplerA, (int2)(col, row)).xy;
  const float4 CV = read_imagef(covariance, samplerA, (int2)(col, row));

  float4 sigma = CV;
  sigma.x += epsilon;
  sigma.w += epsilon;
  const float det = sigma.x * sigma.w - sigma.y * sigma.z;
  const float4 sigma_inv = { sigma.w / det, -sigma.y / det, -sigma.z / det, sigma.x / det };

  const float4 a = (fabs(det) > 4.0f * FLT_EPSILON) ? (float4)( CV.x * sigma_inv.x + CV.y * sigma_inv.y,
                                                                CV.x * sigma_inv.z + CV.y * sigma_inv.w,
                                                                CV.z * sigma_inv.x + CV.w * sigma_inv.y,
                                                                CV.z * sigma_inv.z + CV.w * sigma_inv.w )
                                                    : 0.0f;
  const float4 b = (fabs(det) > 4.0f * FLT_EPSILON) ? (float4)( UV.x - a.x * UV.x - a.y * UV.y, UV.y - a.z * UV.x - a.w * UV.y, 0.0f, 0.0f )
                                                    : (float4)( UV.x, UV.y, 0.0f, 0.0f );

  write_imagef(img_a, (int2)(col, row), a);
  write_imagef(img_b, (int2)(col, row), b);
}

__kernel void apply_prefilter(__read_only image2d_t UV_in,
                              __write_only image2d_t UV_out,
                              __read_only image2d_t saturation,
                              __read_only image2d_t a_full,
                              __read_only image2d_t b_full,
                              global float *weights,
                              const float sat_shift,
                              const int width,
                              const int height)
{
  const int col = get_global_id(0);
  const int row = get_global_id(1);
  if(col >= width || row >= height) return;

  float2 UV = read_imagef(UV_in, samplerA, (int2)(col, row)).xy;
  const float4 a = read_imagef(a_full, samplerA, (int2)(col, row));
  const float2 b = read_imagef(b_full, samplerA, (int2)(col, row)).xy;
  const float2 cv = { a.x * UV.x + a.y * UV.y + b.x,  a.z * UV.x + a.w * UV.y + b.y };

  const float sat = read_imagef(saturation, samplerA, (int2)(col, row)).x;
  const float satweight = _get_satweight(sat - sat_shift, weights);

  UV.x = _interpolatef(satweight, cv.x, UV.x);
  UV.y = _interpolatef(satweight, cv.y, UV.y);
  const float4 UVO = { UV.x, UV.y, 0.0f, 0.0f};
  write_imagef(UV_out, (int2)(col, row), UVO);
}

// also initialize covariance
__kernel void prepare_correlations(__read_only image2d_t ds_corrections,
                                   __read_only image2d_t ds_b_corrections,
                                   __read_only image2d_t ds_UV,
                                   __write_only image2d_t correlations,
                                   __write_only image2d_t covariance,
                                   const int width,
                                   const int height)
{
  const int col = get_global_id(0);
  const int row = get_global_id(1);
  if(col >= width || row >= height) return;

  const float2 UV = read_imagef(ds_UV, samplerA, (int2)(col, row)).xy;
  const float2 CR = read_imagef(ds_corrections, samplerA, (int2)(col, row)).xy;
  const float CRB = read_imagef(ds_b_corrections, samplerA, (int2)(col, row)).x;

  const float4 CL = { UV.x * CR.y, UV.y * CR.y, UV.x * CRB, UV.y * CRB };
  write_imagef(correlations, (int2)(col, row), CL);
  const float4 CV = { UV.x * UV.x, UV.x * UV.y, UV.x * UV.y, UV.y * UV.y };
  write_imagef(covariance, (int2)(col, row), CV);
}

// also write covariance
__kernel void finish_correlations(__read_only image2d_t ds_corrections,
                                  __read_only image2d_t ds_b_corrections,
                                  __read_only image2d_t ds_UV,
                                  __read_only image2d_t correlations_in,
                                  __write_only image2d_t correlations_out,
                                  __read_only image2d_t covariance_in,
                                  __write_only image2d_t covariance_out,
                                  const int width,
                                  const int height)
{
  const int col = get_global_id(0);
  const int row = get_global_id(1);
  if(col >= width || row >= height) return;

  const float2 UV = read_imagef(ds_UV, samplerA, (int2)(col, row)).xy;
  const float2 CR = read_imagef(ds_corrections, samplerA, (int2)(col, row)).xy;
  const float CRB = read_imagef(ds_b_corrections, samplerA, (int2)(col, row)).x;
  const float4 CL = read_imagef(correlations_in, samplerA, (int2)(col, row));

  const float4 CLO = { CL.x - UV.x * CR.y,
                       CL.y - UV.y * CR.y,
                       CL.z - UV.x * CRB,
                       CL.w - UV.y * CRB };

  const float4 CV = read_imagef(covariance_in, samplerA, (int2)(col, row));
  const float4 CVO = { CV.x - UV.x * UV.x, CV.y - UV.x * UV.y, CV.z - UV.x * UV.y, CV.w - UV.y * UV.y };

  write_imagef(correlations_out, (int2)(col, row), CLO);
  write_imagef(covariance_out, (int2)(col, row), CVO);
}

__kernel void final_guide(__read_only image2d_t covariance,
                          __read_only image2d_t correlations,
                          __read_only image2d_t ds_corrections,
                          __read_only image2d_t ds_b_corrections,
                          __read_only image2d_t ds_UV,
                          __write_only image2d_t img_a,
                          __write_only image2d_t img_b,
                          const float epsilon,
                          const int width,
                          const int height)
{
  const int col = get_global_id(0);
  const int row = get_global_id(1);
  if(col >= width || row >= height) return;

  const float2 UV = read_imagef(ds_UV, samplerA, (int2)(col, row)).xy;
  const float4 CL = read_imagef(correlations, samplerA, (int2)(col, row));
  const float2 CR = read_imagef(ds_corrections, samplerA, (int2)(col, row)).xy;
  const float CRB = read_imagef(ds_b_corrections, samplerA, (int2)(col, row)).x;
  float4 sigma = read_imagef(covariance, samplerA, (int2)(col, row));
  sigma.x += epsilon;
  sigma.w += epsilon;

  const float det = sigma.x * sigma.w - sigma.y * sigma.z;

  float4 a = (float4) 0.0f;
  if(fabs(det) > 4.0f * FLT_EPSILON)
  {
    const float4 sigma_inv = { sigma.w / det, -sigma.y / det, -sigma.z / det, sigma.x / det };
    a = (float4)( CL.x * sigma_inv.x + CL.y * sigma_inv.y,
                  CL.x * sigma_inv.z + CL.y * sigma_inv.w,
                  CL.z * sigma_inv.x + CL.w * sigma_inv.y,
                  CL.z * sigma_inv.z + CL.w * sigma_inv.w);
  }

  const float4 b = {  CR.y - a.x * UV.x - a.y * UV.y,
                      CRB  - a.z * UV.x - a.w * UV.y, 0.0f, 0.0f };

  write_imagef(img_a, (int2)(col, row), a);
  write_imagef(img_b, (int2)(col, row), b);
}

__kernel void apply_guided(__read_only image2d_t full_UV,
                           __read_only image2d_t saturation,
                           __read_only image2d_t scharr,
                           __read_only image2d_t img_a,
                           __read_only image2d_t img_b,
                           __read_only image2d_t corrections_in,
                           __write_only image2d_t corrections_out,
                           __write_only image2d_t b_corrections,
                           global float *weights,
                           const float sat_shift,
                           const float bright_shift,
                           const int width,
                           const int height)
{
  const int col = get_global_id(0);
  const int row = get_global_id(1);
  if(col >= width || row >= height) return;

  const float2 UV = read_imagef(full_UV, samplerA, (int2)(col, row)).xy;
  const float4 A = read_imagef(img_a, samplerA, (int2)(col, row));
  const float2 B = read_imagef(img_b, samplerA, (int2)(col, row)).xy;
  float2 CR = read_imagef(corrections_in, samplerA, (int2)(col, row)).xy;
  const float sat = read_imagef(saturation, samplerA, (int2)(col, row)).x;
  const float grad = read_imagef(scharr, samplerA, (int2)(col, row)).x;

  const float2 CV = { A.x * UV.x + A.y * UV.y + B.x,
                      A.z * UV.x + A.w * UV.y + B.y };

  const float satweight = _get_satweight(sat - sat_shift, weights);
  CR.y = _interpolatef(satweight, CV.x, 1.0f);

  const float brightweight = _get_satweight(sat - bright_shift, weights);
  const float BC = _interpolatef(grad * brightweight, CV.y, 0.0f);
  const float4 CRO = { CR.x, CR.y, 0.0f, 0.0f };
  write_imagef(corrections_out, (int2)(col, row), CRO);
  write_imagef(b_corrections, (int2)(col, row), BC);
}

__kernel void sample_input(__read_only image2d_t dev_in,
                           __write_only image2d_t saturation,
                           __write_only image2d_t L,
                           __write_only image2d_t UV,
                           global float4 *mat,
                           const int width,
                           const int height)
{
  const int col = get_global_id(0);
  const int row = get_global_id(1);
  if(col >= width || row >= height) return;

  const float4 pix_in = read_imagef(dev_in, samplerA, (int2)(col, row));

  const float4 M[3] = { mat[0], mat[1], mat[2] };
  const float4 XYZ_D65 = matrix_dot(pix_in, M);
  const float4 xyY = dt_D65_XYZ_to_xyY(XYZ_D65);

  // calc saturation from input data
  const float dmin = fmin(pix_in.x, fmin(pix_in.y, pix_in.z));
  const float dmax = fmax(pix_in.x, fmax(pix_in.y, pix_in.z));
  const float delta = dmax - dmin;
  const float sval = (dmax > CENORM_MIN && delta > CENORM_MIN) ? delta / dmax : 0.0f;
  write_imagef(saturation, (int2)(col, row), sval);

  const float2 UV_star_prime = xyY_to_dt_UCS_UV(xyY);
  const float4 SP = {UV_star_prime.x, UV_star_prime.y, 0.0f, 0.0f};

  write_imagef(UV, (int2)(col, row), SP);

  const float Lval = Y_to_dt_UCS_L_star(xyY.z);
  write_imagef(L, (int2)(col, row), Lval);
}

__kernel void write_output(__write_only image2d_t out,
                            __read_only image2d_t in,
                            __read_only image2d_t dev_in,
                            __read_only image2d_t corrections,
                            __read_only image2d_t b_corrections,
                            global float4 *mat,
                            global float *gamut_LUT,
                            const float white,
                            const int width,
                            const int height)
{
  const int col = get_global_id(0);
  const int row = get_global_id(1);
  if(col >= width || row >= height) return;

  float4 pix_out = read_imagef(in, samplerA, (int2)(col, row));
  const float2 correction = read_imagef(corrections, samplerA, (int2)(col, row)).xy;
  const float b_correction = read_imagef(b_corrections, samplerA, (int2)(col, row)).x;
  const float4 M[3] = { mat[0], mat[1], mat[2] };
  pix_out.x += correction.x;
  pix_out.y = fmax(0.0f, pix_out.y * (1.0f + SAT_EFFECT * (correction.y - 1.0f)));
  pix_out.z = fmax(0.0f, pix_out.z * (1.0f + BRIGHT_EFFECT * b_correction));

  const float4 HSB = gamut_map_HSB(pix_out, gamut_LUT, white);
  const float4 XYZ_D65 = dt_UCS_HSB_to_XYZ(HSB, white);
  float4 pout = matrix_dot(XYZ_D65, M);

  const float alpha = read_imagef(dev_in, samplerA, (int2)(col, row)).w;
  pout.w = alpha;
  write_imagef(out, (int2)(col, row), pout);
}

__kernel void write_visual(__write_only image2d_t out,
                            __read_only image2d_t in,
                            __read_only image2d_t corrections,
                            __read_only image2d_t b_corrections,
                            __read_only image2d_t saturation,
                            __read_only image2d_t scharr,
                            global float *gamut_LUT,
                            global float *weights,
                            const float bright_shift,
                            const float sat_shift,
                            const float white,
                            const int mode,
                            const int width,
                            const int height)
{
  const int col = get_global_id(0);
  const int row = get_global_id(1);
  if(col >= width || row >= height) return;

  const float B = read_imagef(in, samplerA, (int2)(col, row)).z;
  const float2 correction = read_imagef(corrections, samplerA, (int2)(col, row)).xy;
  const float b_correction = read_imagef(b_corrections, samplerA, (int2)(col, row)).x;

  const float sat = read_imagef(saturation, samplerA, (int2)(col, row)).x;
  const float val = dtcl_sqrt(fmax(0.0f, B * white));
  float corr = 0.0f;
  switch(mode)
  {
    case BRIGHTNESS:
        corr = BRIGHT_EFFECT * b_correction;
        break;
    case SATURATION:
        corr = SAT_EFFECT * (correction.y - 1.0f);
        break;
    case BRIGHTNESS_GRAD:
        corr = _get_satweight(sat - bright_shift, weights) - 0.5f;
        break;
    case SATURATION_GRAD:
        corr = _get_satweight(sat - sat_shift, weights) - 0.5f;
        break;
    default:  // HUE
        corr = 0.2f * correction.x;
  }

  const int neg = corr < 0.0f;
  corr = fabs(corr);
  corr = corr < 2e-3f ? 0.0f : corr;

  float4 pout = { fmax(0.0f, neg ? val - corr : val),
                  fmax(0.0f, val - corr),
                  fmax(0.0f, neg ? val : val - corr),
                  0.0f };

  const float gv = 1.0f - read_imagef(scharr, samplerA, (int2)(col, row)).x;
  if(mode == BRIGHTNESS && gv > 0.2f)
  {
    pout.x = pout.z = 0.0f;
    pout.y = gv;
  }
  write_imagef(out, (int2)(col, row), pout);
}

__kernel void draw_weight(__write_only image2d_t out,
                          global float *weights,
                          const float bright_shift,
                          const float sat_shift,
                          const int mode,
                          const int width,
                          const int height)
{
  const int col = get_global_id(0);
  const int y = get_global_id(1);
  if(col >= width || y > 0) return;

  const float eps = 0.5f / (float)height;
  const float weight = _get_satweight((float)col / (float)width
                        - (mode == SATURATION_GRAD ? sat_shift : bright_shift), weights);
  if(weight > eps && weight < 1.0f - eps)
  {
    const int row = (int)((1.0f - weight) * (float)(height-1));
    const float4 mark = { 0.0f, 1.0f, 0.0f, 0.0f};
    write_imagef(out, (int2)(col/16, row), mark);
  }
}

__kernel void process_data(__read_only image2d_t UV,
                           __read_only image2d_t L,
                           __read_only image2d_t saturation,
                           __write_only image2d_t scharr,
                           __write_only image2d_t corrections,
                           __write_only image2d_t b_corrections,
                           __write_only image2d_t out,
                           global float *LUT_saturation,
                           global float *LUT_hue,
                           global float *LUT_brightness,
                           const float white,
                           const float gradient_amp,
                           const int guiding,
                           const int width,
                           const int height)
{
  const int col = get_global_id(0);
  const int row = get_global_id(1);
  if(col >= width || row >= height) return;

  const float lum = read_imagef(L, samplerA, (int2)(col, row)).x;
  const float2 uv = read_imagef(UV, samplerA, (int2)(col, row)).xy;

  const float4 JCH = dt_UCS_LUV_to_JCH(lum, white, uv);
  const float4 pout = dt_UCS_JCH_to_HSB(JCH);

  if(guiding)
  {
    const int icol = clamp(col, 1, width - 2);
    const int irow = clamp(row, 1, height - 2);
    const float kscharr = fmax(0.0f, scharr_gradient(saturation, icol, irow) - 0.02f);
    const float gradient = clamp(1.0f - gradient_amp * kscharr * kscharr, 0.0f, 1.0f);
    write_imagef(scharr, (int2)(col, row), gradient);
  }

  float4 corr = { 0.0f, 1.0f, 0.0f, 0.0f};
  float bcorr = 0.0f;

  if(JCH.y > CENORM_MIN)
  {
    const float hue = pout.x;
    const float sat = pout.y;
    corr.x = lookup_gamut(LUT_hue, hue);
    corr.y = lookup_gamut(LUT_saturation, hue);
    bcorr = sat * (lookup_gamut(LUT_brightness, hue) - 1.0f);
  }

  write_imagef(corrections, (int2)(col, row), corr);
  write_imagef(b_corrections, (int2)(col, row), bcorr);
  write_imagef(out, (int2)(col, row), pout);
}