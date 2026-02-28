/*
    This file is part of darktable,
    copyright (c) 2016-2026 darktable developers.

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

#include "color_conversion.h"
#include "rgb_norms.h"

/* 
These coefficients are the Narkowicz ACES approximation. 
They are widely used for their good balance between performance and visual accuracy. 
You can find the reference here: https://knarkowicz.wordpress.com/2016/01/06/aces-filmic-tone-mapping-curve/" 
*/
inline float _aces_tone_map(const float x)
{
  const float a = 2.51f;
  const float b = 0.03f;
  const float c = 2.43f;
  const float d = 0.59f;
  const float e = 0.14f;

  return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0f, 1.0f);
}

/*
hese coefficients refer to the Fitted ACES (RRT+ODT) approximation, 
which is more precise than the basic Narkowicz fit. 
It is based on the curve fitting of the ACES 1.0/2.0 RRT/ODT transform.
Reference: https://github.com/TheRealMJP/BakingLab/blob/master/BakingLab/ACES.hlsl 
(or similar ACES fitted shaders used in cinematic rendering)."
*/
inline float _aces_20_tonemap(const float x)
{
  const float a = 0.0245786f;
  const float b = 0.000090537f;
  const float c = 0.983729f;
  const float d = 0.4329510f;
  const float e = 0.238081f;

  return clamp((x * (x + a) - b) / (x * (c * x + d) + e), 0.0f, 1.0f);
}

/*
  Primary LUT lookup.  Measures the luminance of a given pixel using a selectable function, looks up that
  luminance in the configured basecurve, and then scales each channel by the result.

  Doing it this way avoids the color shifts documented as being possible in the legacy basecurve approach.

  Also applies a multiplier prior to lookup in order to support fusion.  The idea of doing this here is to
  emulate the original use case of enfuse, which was to fuse multiple JPEGs from a camera that was set up
  for exposure bracketing, and which may have had a camera-specific base curve applied.
*/
kernel void
basecurve_lut(read_only image2d_t in, 
              write_only image2d_t out, 
              const int width, const int height,
              const float mul, 
              read_only image2d_t table, 
              constant float *a, 
              const int preserve_colors,
              constant dt_colorspaces_iccprofile_info_cl_t *profile_info, 
              read_only image2d_t lut,
              const int use_work_profile)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 pixel = read_imagef(in, sampleri, (int2)(x, y));

  float ratio = 1.f;
  const float lum = mul * dt_rgb_norm(pixel, preserve_colors, use_work_profile, profile_info, lut);
  if(lum > 0.f)
  {
    const float curve_lum = lookup_unbounded(table, lum, a);
    ratio = mul * curve_lum / lum;
  }
  pixel.xyz *= ratio;
  pixel = fmax(pixel, 0.f);

  write_imagef (out, (int2)(x, y), pixel);
}


kernel void
basecurve_zero(write_only image2d_t out, const int width, const int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  write_imagef (out, (int2)(x, y), (float4)0.0f);
}

/*
  Original basecurve implementation.  Applies a LUT on a per-channel basis which can cause color shifts.

  These can be undesirable (skin tone shifts), or sometimes may be desired (fiery sunset).  Continue to allow
  the "old" method but don't make it the default, both for backwards compatibility and for those who are willing
  to take the risks of "artistic" impacts on their image.
*/
kernel void
basecurve_legacy_lut(read_only image2d_t in, 
                    write_only image2d_t out, 
                    const int width, const int height,
                    const float mul, read_only image2d_t table, 
                    constant float *a)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 pixel = read_imagef(in, sampleri, (int2)(x, y));

  // apply ev multiplier and use lut or extrapolation:
  float3 f = pixel.xyz * mul;

  pixel.x = lookup_unbounded(table, f.x, a);
  pixel.y = lookup_unbounded(table, f.y, a);
  pixel.z = lookup_unbounded(table, f.z, a);
  pixel = fmax(pixel, 0.f);
  write_imagef (out, (int2)(x, y), pixel);
}

kernel void
basecurve_compute_features(read_only image2d_t in, write_only image2d_t out, const int width, const int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 value = read_imagef(in, sampleri, (int2)(x, y));

  const float ma = fmax(value.x, fmax(value.y, value.z));
  const float mi = fmin(value.x, fmin(value.y, value.z));

  const float sat = 0.1f + 0.1f * (ma - mi) / fmax(1.0e-4f, ma);
  value.w = sat;

  const float c = 0.54f;

  float v = fabs(value.x - c);
  v = fmax(fabs(value.y - c), v);
  v = fmax(fabs(value.z - c), v);

  const float var = 0.5f;
  const float e = 0.2f + dt_fast_expf(-v * v / (var * var));

  value.w *= e;

  write_imagef (out, (int2)(x, y), value);
}

constant float gw[5] = { 1.0f/16.0f, 4.0f/16.0f, 6.0f/16.0f, 4.0f/16.0f, 1.0f/16.0f };

kernel void
basecurve_blur_h(read_only image2d_t in, write_only image2d_t out,
                 const int width, const int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  const int rad = 2;
  constant float *w = gw + rad;

  float4 sum = (float4)0.0f;

  for (int i = -rad; i <= rad; i++)
  {
    const int xx = min(max(-x - i, x + i), width - (x + i - width + 1));
    sum += read_imagef(in, sampleri, (int2)(xx, y)) * w[i];
  }

  write_imagef (out, (int2)(x, y), sum);
}


kernel void
basecurve_blur_v(read_only image2d_t in, write_only image2d_t out,
                 const int width, const int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);


  if(x >= width || y >= height) return;

  const int rad = 2;
  constant float *w = gw + rad;

  float4 sum = (float4)0.0f;

  for (int i = -rad; i <= rad; i++)
  {
    const int yy = min(max(-y - i, y + i), height - (y + i - height + 1));
    sum += read_imagef(in, sampleri, (int2)(x, yy)) * w[i];
  }

  write_imagef (out, (int2)(x, y), sum);
}

kernel void
basecurve_expand(read_only image2d_t in, write_only image2d_t out, const int width, const int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  // fill numbers in even pixels, zero odd ones
  float4 pixel = (x % 2 == 0 && y % 2 == 0) ? 4.0f * read_imagef(in, sampleri, (int2)(x / 2, y / 2)) : (float4)0.0f;

  write_imagef (out, (int2)(x, y), pixel);
}

kernel void
basecurve_reduce(read_only image2d_t in, write_only image2d_t out, const int width, const int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 pixel = read_imagef(in, sampleri, (int2)(2 * x, 2 * y));

  write_imagef (out, (int2)(x, y), pixel);
}

kernel void
basecurve_detail(read_only image2d_t in, read_only image2d_t det, write_only image2d_t out, const int width, const int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 input = read_imagef(in, sampleri, (int2)(x, y));
  float4 detail = read_imagef(det, sampleri, (int2)(x, y));

  write_imagef (out, (int2)(x, y), input - detail);
}

kernel void
basecurve_adjust_features(read_only image2d_t in, read_only image2d_t det, write_only image2d_t out, const int width, const int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 input = read_imagef(in, sampleri, (int2)(x, y));
  float4 detail = read_imagef(det, sampleri, (int2)(x, y));

  input.w *= 0.1f + sqrt(detail.x * detail.x + detail.y * detail.y + detail.z * detail.z);

  write_imagef (out, (int2)(x, y), input);
}

kernel void
basecurve_blend_gaussian(read_only image2d_t in, read_only image2d_t col, write_only image2d_t out, const int width, const int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 comb = read_imagef(in, sampleri, (int2)(x, y));
  float4 collect = read_imagef(col, sampleri, (int2)(x, y));

  comb.xyz += collect.xyz * collect.w;
  comb.w += collect.w;

  write_imagef (out, (int2)(x, y), comb);
}

kernel void
basecurve_blend_laplacian(read_only image2d_t in, read_only image2d_t col, read_only image2d_t tmp, write_only image2d_t out,
                          const int width, const int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 comb = read_imagef(in, sampleri, (int2)(x, y));
  float4 collect = read_imagef(col, sampleri, (int2)(x, y));
  float4 temp = read_imagef(tmp, sampleri, (int2)(x, y));

  comb.xyz += (collect.xyz - temp.xyz) * collect.w;
  comb.w += collect.w;

  write_imagef (out, (int2)(x, y), comb);
}

kernel void
basecurve_normalize(read_only image2d_t in, write_only image2d_t out, const int width, const int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 comb = read_imagef(in, sampleri, (int2)(x, y));

  comb.xyz /= (comb.w > 1.0e-8f) ? comb.w : 1.0f;

  write_imagef (out, (int2)(x, y), comb);
}

kernel void
basecurve_reconstruct(read_only image2d_t in, read_only image2d_t tmp, write_only image2d_t out, const int width, const int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 comb = read_imagef(in, sampleri, (int2)(x, y));
  float4 temp = read_imagef(tmp, sampleri, (int2)(x, y));

  comb += temp;

  write_imagef (out, (int2)(x, y), comb);
}

kernel void
basecurve_finalize(read_only image2d_t in,
                   read_only image2d_t comb, 
                   write_only image2d_t out, 
                   const int width,
                   const int height, const int workflow_mode, 
                   const float shadow_lift, 
                   const float highlight_gain,
                   const float ucs_saturation_balance, 
                   const float gamut_strength, const float highlight_corr, 
                   const int target_gamut, 
                   const float look_opacity, 
                   const float16 look_mat, 
                   const float alpha)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 pixel = read_imagef(comb, sampleri, (int2)(x, y));

  // Sanitize to avoid Inf/NaN propagation
  pixel.xyz = clamp(pixel.xyz, -1e6f, 1e6f);

  if(workflow_mode > 0)
  {
    float3 pixel_in = pixel.xyz;
    float3 look_transformed;
    look_transformed.x = dot(pixel_in, (float3)(look_mat.s0, look_mat.s1, look_mat.s2));
    look_transformed.y = dot(pixel_in, (float3)(look_mat.s3, look_mat.s4, look_mat.s5));
    look_transformed.z = dot(pixel_in, (float3)(look_mat.s6, look_mat.s7, look_mat.s8));

    // Mix between original and transformed
    pixel.xyz = mix(pixel_in, look_transformed, look_opacity);
    pixel.xyz = fmax(pixel.xyz, 0.0f); // Anti-black artifacts

    if(highlight_gain != 1.0f)
      pixel.xyz *= highlight_gain;

    if(shadow_lift != 1.0f)
    {
      pixel.x = (pixel.x > 0.0f) ? native_powr(pixel.x, shadow_lift) : pixel.x;
      pixel.y = (pixel.y > 0.0f) ? native_powr(pixel.y, shadow_lift) : pixel.y;
      pixel.z = (pixel.z > 0.0f) ? native_powr(pixel.z, shadow_lift) : pixel.z;
    }

    const float r_coeff = 0.2627f;
    const float g_coeff = 0.6780f;
    const float b_coeff = 0.0593f;
    
    float y_in = pixel.x * r_coeff + pixel.y * g_coeff + pixel.z * b_coeff;
    float y_out = y_in;

    /* Scene-referred: luminance-adaptive shoulder extension for ACES-like
       tonemapping using perceptual luminance Jz. */
    if(workflow_mode == 1 || workflow_mode == 2)
    {
      float3 xyz;
      xyz.x = 0.636958f * pixel.x + 0.144617f * pixel.y + 0.168881f * pixel.z;
      xyz.y = 0.262700f * pixel.x + 0.677998f * pixel.y + 0.059302f * pixel.z;
      xyz.z = 0.000000f * pixel.x + 0.028073f * pixel.y + 1.060985f * pixel.z;

      xyz = fmax(xyz, (float3)(0.0f));

      float4 xyz_scaled = (float4)(xyz.x * 400.0f, xyz.y * 400.0f, xyz.z * 400.0f, 0.0f);
      float4 jab = XYZ_to_JzAzBz(xyz_scaled);

      const float L = clamp(jab.x, 0.0f, 1.0f);
      const float k = 1.0f + alpha * L * L;

      const float x_scaled = y_in / k;
      if(workflow_mode == 1)
        y_out = _aces_tone_map(x_scaled) * k;
      else
        y_out = _aces_20_tonemap(x_scaled * 1.257f) * k;
    }

    float gain = y_out / fmax(y_in, 1e-6f);
    pixel.xyz *= gain;

    const float threshold = 0.80f;
    if(y_out > threshold)
    {
      float factor = (y_out - threshold) / (1.0f - threshold);
      factor = clamp(factor, 0.0f, 1.0f);
      pixel.xyz = mix(pixel.xyz, (float3)y_out, factor);
    }

    float4 jab = (float4)(0.0f);
    if(ucs_saturation_balance != 0.0f || gamut_strength > 0.0f || highlight_corr != 0.0f)
    {
      // RGB Rec2020 to XYZ D65
      float3 xyz;
      xyz.x = 0.636958f * pixel.x + 0.144617f * pixel.y + 0.168881f * pixel.z;
      xyz.y = 0.262700f * pixel.x + 0.677998f * pixel.y + 0.059302f * pixel.z;
      xyz.z = 0.000000f * pixel.x + 0.028073f * pixel.y + 1.060985f * pixel.z;

      xyz = fmax(xyz, 0.0f);

      // XYZ to JzAzBz
      float4 xyz_scaled = (float4)(xyz.x * 400.0f, xyz.y * 400.0f, xyz.z * 400.0f, 0.0f);
      jab = XYZ_to_JzAzBz(xyz_scaled);

      int modified = 0;

      if(ucs_saturation_balance != 0.0f)
      {
        // Chroma-based modulation for saturation balance
        const float chroma = fmax(fmax(pixel.x, pixel.y), pixel.z) - fmin(fmin(pixel.x, pixel.y), pixel.z);
        const float effective_saturation = ucs_saturation_balance * fmin(chroma * 2.0f, 1.0f);

        // Apply saturation balance
        const float Y = xyz.y;
        const float L = native_sqrt(fmax(Y, 0.0f));
        const float fulcrum = 0.5f;
        const float n = (L - fulcrum) / fulcrum;
        const float mask_shadow = 1.0f / (1.0f + dtcl_exp(n * 4.0f));
        
        float sat_adjust = effective_saturation * (2.0f * mask_shadow - 1.0f);
        sat_adjust *= fmin(L * 4.0f, 1.0f);
        const float sat_factor = 1.0f + sat_adjust;
        jab.y *= sat_factor;
        jab.z *= sat_factor;
        modified = 1;
      }

      if(gamut_strength > 0.0f)
      {
        const float Y = xyz.y;
        const float L = native_sqrt(fmax(Y, 0.0f));
        const float chroma_factor = 1.0f - gamut_strength * (0.2f + 0.2f * L);
        jab.y *= chroma_factor;
        jab.z *= chroma_factor;
        modified = 1;
      }

      // HIGH SENSITIVITY CORRECTION
      // Start effect at 0.20 up to 0.90. Linear transition.
      float hl_mask = clamp((jab.x - 0.20f) / 0.70f, 0.0f, 1.0f);

      if(hl_mask > 0.0f && highlight_corr != 0.0f)
      {
        // 1. Soft symmetric desaturation (0.75 factor)
        const float desat = 1.0f - (fabs(highlight_corr) * hl_mask * 0.75f);
        jab.y *= desat;
        jab.z *= desat;

        // 2. Controlled Hue Rotation (2.0 factor)
        const float angle = highlight_corr * hl_mask * 2.0f;
        const float ca = native_cos(angle);
        const float sa = native_sin(angle);
        const float az = jab.y;
        const float bz = jab.z;

        jab.y = az * ca - bz * sa;
        jab.z = az * sa + bz * ca;
        modified = 1;
      }

      if(jab.x > 0.95f)
      {
        const float desat = clamp((1.0f - jab.x) * 20.0f, 0.0f, 1.0f);
        jab.y *= desat;
        jab.z *= desat;
        modified = 1;
      }

      if(modified)
      {
        // JzAzBz to XYZ
        xyz = JzAzBz_2_XYZ(jab).xyz / 400.0f;

        // XYZ D65 to RGB Rec2020
        pixel.x =  1.716651f * xyz.x - 0.355671f * xyz.y - 0.253366f * xyz.z;
        pixel.y = -0.666684f * xyz.x + 1.616481f * xyz.y + 0.015768f * xyz.z;
        pixel.z =  0.017640f * xyz.x - 0.042771f * xyz.y + 0.942103f * xyz.z;
        
        const float min_val = fmin(pixel.x, fmin(pixel.y, pixel.z));
        if(min_val < 0.0f)
        {
          const float lum = 0.2627f * pixel.x + 0.6780f * pixel.y + 0.0593f * pixel.z;
          if(lum > 0.0f)
          {
           const float factor = lum / (lum - min_val);
            pixel.xyz = lum + factor * (pixel.xyz - lum);
          }
        }
        pixel.xyz = clamp(pixel.xyz, 0.0f, 1.0f);
      }
    }

    if(gamut_strength > 0.0f)
    {
      float4 orig = pixel;

      float Y = 0.2126f * pixel.x + 0.7152f * pixel.y + 0.0722f * pixel.z;
      float lum_weight = clamp((Y - 0.3f) / (0.8f - 0.3f), 0.0f, 1.0f);
      lum_weight = lum_weight * lum_weight * (3.0f - 2.0f * lum_weight);
      float effective_strength = gamut_strength * lum_weight;

      float limit = 0.90f;
      if(target_gamut == 1) limit = 0.95f;
      else if(target_gamut == 2) limit = 1.00f;

      float threshold = limit * (1.0f - (effective_strength * 0.25f));
      float max_val = fmax(pixel.x, fmax(pixel.y, pixel.z));

      if(max_val > threshold)
      {
        const float range = limit - threshold;
        const float delta = max_val - threshold;
        const float compressed = threshold + range * delta / (delta + range);
        const float factor = compressed / max_val;

        const float range_blue = 1.1f * range;
        const float compressed_blue = threshold + range * delta / (delta + range_blue);
        const float factor_blue = compressed_blue / max_val;

        pixel.x *= factor;
        pixel.y *= factor;
        pixel.z *= factor_blue;
      }
      pixel = mix(orig, pixel, effective_strength);
    }

    // Final gamut check to preserve hue
    if(pixel.x < 0.0f || pixel.x > 1.0f || pixel.y < 0.0f || pixel.y > 1.0f || pixel.z < 0.0f || pixel.z > 1.0f)
    {
      const float luma = 0.2627f * pixel.x + 0.6780f * pixel.y + 0.0593f * pixel.z;
      const float target_luma = clamp(luma, 0.0f, 1.0f);
      float t = 1.0f;
      if(pixel.x < 0.0f) t = fmin(t, target_luma / (target_luma - pixel.x));
      if(pixel.y < 0.0f) t = fmin(t, target_luma / (target_luma - pixel.y));
      if(pixel.z < 0.0f) t = fmin(t, target_luma / (target_luma - pixel.z));
      if(pixel.x > 1.0f) t = fmin(t, (1.0f - target_luma) / (pixel.x - target_luma));
      if(pixel.y > 1.0f) t = fmin(t, (1.0f - target_luma) / (pixel.y - target_luma));
      if(pixel.z > 1.0f) t = fmin(t, (1.0f - target_luma) / (pixel.z - target_luma));
      t = fmax(0.0f, t);
      pixel.xyz = target_luma + t * (pixel.xyz - target_luma);
    }
  }

  pixel.w = read_imagef(in, sampleri, (int2)(x, y)).w;

  write_imagef (out, (int2)(x, y), pixel);
}
