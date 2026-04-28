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

// restore_rgb — RGB-path glue for the AI denoise + upscale tasks.
//
// inputs here are linear-working-profile float4 RGBA (from darktable
// export). this file owns:
//   - color management: working-profile → sRGB before inference and
//     back after, with optional wide-gamut preservation mask
//   - shadow boost: per-image luminance curve to protect deep shadows
//     during sRGB round-trip (opt-in via model attribute)
//   - wavelet (DWT) detail recovery: preserve high-frequency texture
//     in the luminance residual after denoise
//   - dt_restore_process_tiled driver that ties together tiling,
//     gamut masking, shadow boost and the per-patch inference call
//     (dt_restore_run_patch). the low-level inference helpers live in
//     restore.c; this file composes them for RGB.
//
// the raw denoise variants (Bayer / X-Trans) do their own pre/post-
// processing (per-CFA-site black / WB / re-mosaic) and live in
// restore_raw_bayer.c / restore_raw_linear.c. they share the generic
// pipeline-bridge dt_restore_run_user_pipe_roi() in restore.c

#include "common/ai/restore_rgb.h"
#include "common/ai/restore_common.h"
#include "ai/backend.h"
#include "common/darktable.h"
#include "common/colorspaces.h"
#include "common/colorspaces_inline_conversions.h"
#include "common/imagebuf.h"
#include "common/math.h"
#include "common/matrices.h"
#include "control/conf.h"
#include "control/jobs.h"

#include <glib.h>
#include <math.h>
#include <string.h>

// forward-declare to avoid pulling in dwt.h (which includes OpenCL
// types when HAVE_OPENCL is defined — and the AI shared library
// is built without OpenCL)
extern void dwt_denoise(float *buf, int width, int height,
                        int bands, const float *noise);

#define MAX_MODEL_INPUTS 4

// default multipliers of residual sigma for each wavelet band.
// band 0 (finest) gets the strongest suppression since fine-scale
// features are hardest to distinguish from noise. coarser bands
// preserve more because they capture real texture.
// tunable via darktablerc: plugins/lighttable/neural_restore/detail_recovery_bands
static const float _dwt_sigma_mul_default[DWT_DETAIL_BANDS] = {
  0.25f,  // band 0 (finest) — suppress fine luminance noise
  0.15f,  // band 1
  0.05f,  // band 2
  0.02f,  // band 3
  0.01f   // band 4 (coarsest) — keep almost everything
};

// sRGB transfer function (gamma curve only, no primaries change).
// values > 1.0 are allowed to preserve wide-gamut colors
static inline float _linear_to_srgb(const float v)
{
  if(v <= 0.0f) return 0.0f;
  return (v <= 0.0031308f)
    ? 12.92f * v
    : 1.055f * powf(v, 1.0f / 2.4f) - 0.055f;
}

static inline float _srgb_to_linear(const float v)
{
  if(v <= 0.0f) return 0.0f;
  return (v <= 0.04045f)
    ? v / 12.92f
    : powf((v + 0.055f) / 1.055f, 2.4f);
}

// Rec.709 / sRGB luminance weights (Y row of sRGB->XYZ D65);
// applied to working-profile-linear pixels in the pass-through
// blending below; exact only when the working profile is
// sRGB/Rec.709, but correct enough for luminance deltas
static inline float _luma_rec709(float r, float g, float b)
{
  return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}

// compute adaptive noise thresholds from residual standard deviation
static void _compute_adaptive_noise(const float *const restrict buf,
                                    const size_t npix,
                                    float noise[DWT_DETAIL_BANDS])
{
  // read band multipliers from config (comma-separated list).
  // e.g. "0.5,0.3,0.1,0.05,0.02" in darktablerc
  float sigma_mul[DWT_DETAIL_BANDS];
  memcpy(sigma_mul, _dwt_sigma_mul_default, sizeof(sigma_mul));
  gchar *val = dt_conf_get_string("plugins/lighttable/neural_restore/detail_recovery_bands");
  if(val && val[0])
  {
    gchar **parts = g_strsplit(val, ",", DWT_DETAIL_BANDS);
    for(int b = 0; parts[b] && b < DWT_DETAIL_BANDS; b++)
      sigma_mul[b] = g_ascii_strtod(g_strstrip(parts[b]), NULL);
    g_strfreev(parts);
  }
  g_free(val);

  double sum = 0.0, sum2 = 0.0;
  for(size_t i = 0; i < npix; i++)
  {
    sum += (double)buf[i];
    sum2 += (double)buf[i] * (double)buf[i];
  }
  const double mean = sum / (double)npix;
  const float sigma = (float)sqrt(sum2 / (double)npix - mean * mean);

  for(int b = 0; b < DWT_DETAIL_BANDS; b++)
    noise[b] = sigma * sigma_mul[b];
}

void dt_restore_set_profile(dt_restore_context_t *ctx, void *profile)
{
  if(!ctx) return;
  if(!profile)
  {
    ctx->has_profile = FALSE;
    return;
  }

  float primaries[3][2], whitepoint[2];
  if(!dt_colorspaces_get_primaries_and_whitepoint_from_profile(
       (cmsHPROFILE)profile, primaries, whitepoint))
  {
    dt_print(DT_DEBUG_AI,
             "[restore_rgb] could not read primaries from working profile, "
             "falling back to gamma-only conversion");
    ctx->has_profile = FALSE;
    return;
  }

  // build WP -> XYZ (stored transposed by dt, convert to row-major)
  dt_colormatrix_t wp_to_xyz_T;
  dt_make_transposed_matrices_from_primaries_and_whitepoint(primaries,
                                                            whitepoint,
                                                            wp_to_xyz_T);
  float wp_to_xyz[9];
  for(int i = 0; i < 3; i++)
    for(int j = 0; j < 3; j++)
      wp_to_xyz[3 * i + j] = wp_to_xyz_T[j][i];

  // transpose dt's sRGB<->XYZ matrices (Bradford D50) to row-major
  float xyz_to_srgb[9], srgb_to_xyz[9];
  for(int i = 0; i < 3; i++)
    for(int j = 0; j < 3; j++)
    {
      xyz_to_srgb[3 * i + j] = xyz_to_srgb_transposed[j][i];
      srgb_to_xyz[3 * i + j] = sRGB_to_xyz_transposed[j][i];
    }

  // WP -> sRGB = (XYZ -> sRGB) * (WP -> XYZ)
  mat3mul(ctx->wp_to_srgb, xyz_to_srgb, wp_to_xyz);

  // invert WP -> XYZ to get XYZ -> WP, then compose sRGB -> WP
  float xyz_to_wp[9];
  if(mat3inv(xyz_to_wp, wp_to_xyz) != 0)
  {
    dt_print(DT_DEBUG_AI,
             "[restore_rgb] singular WP->XYZ matrix, falling back to gamma-only");
    ctx->has_profile = FALSE;
    return;
  }
  mat3mul(ctx->srgb_to_wp, xyz_to_wp, srgb_to_xyz);

  ctx->has_profile = TRUE;
  dt_print(DT_DEBUG_AI, "[restore_rgb] working profile color matrices ready");
}

void dt_restore_set_preserve_wide_gamut(dt_restore_context_t *ctx, gboolean preserve)
{
  if(ctx) ctx->preserve_wide_gamut = preserve;
}

int dt_restore_run_patch(dt_restore_context_t *ctx,
                         const float *in_patch,
                         int w, int h,
                         float *out_patch,
                         int scale)
{
  if(!ctx || !ctx->ai_ctx) return 1;
  const size_t in_pixels = (size_t)w * h * 3;
  const int out_w = w * scale;
  const int out_h = h * scale;
  const size_t out_pixels = (size_t)out_w * out_h * 3;
  const size_t plane = (size_t)w * h;

  // convert to sRGB gamma-encoded. If a working profile is set,
  // first convert primaries (working profile -> sRGB linear) so the
  // model sees the image as if it were native sRGB. Otherwise only
  // apply the gamma curve (legacy path, shifts hues for wide-gamut).
  // input layout is planar NCHW: R plane, then G plane, then B plane.
  // in_gamut_mask records which pixels were in sRGB gamut (scale==1
  // only) so the output pass can skip recomputing WP->sRGB
  float *srgb_in = g_try_malloc(in_pixels * sizeof(float));
  uint8_t *in_gamut_mask = NULL;
  if(!srgb_in) return 1;
  // only allocate the gamut mask when denoise pass-through is requested
  const gboolean need_gamut_mask
    = ctx->has_profile && scale == 1 && ctx->preserve_wide_gamut;
  if(need_gamut_mask)
  {
    in_gamut_mask = g_try_malloc(plane);
    if(!in_gamut_mask)
    {
      g_free(srgb_in);
      return 1;
    }
  }

  if(ctx->has_profile)
  {
    const float *M = ctx->wp_to_srgb;
    const gboolean boost = ctx->shadow_boost;
    for(size_t p = 0; p < plane; p++)
    {
      const float r = in_patch[p];
      const float g = in_patch[p + plane];
      const float b = in_patch[p + 2 * plane];
      float sr = M[0] * r + M[1] * g + M[2] * b;
      float sg = M[3] * r + M[4] * g + M[5] * b;
      float sb = M[6] * r + M[7] * g + M[8] * b;
      // gamut check uses pre-boost values so pass-through decisions
      // reflect the original color
      if(in_gamut_mask)
      {
        const float m = 0.01f;  // ~1% margin beyond [0, 1]
        in_gamut_mask[p] = (sr >= -m && sr <= 1.0f + m
                           && sg >= -m && sg <= 1.0f + m
                           && sb >= -m && sb <= 1.0f + m) ? 1 : 0;
      }
      if(boost)
      {
        sr = sr > 0.0f ? sqrtf(sr) : 0.0f;
        sg = sg > 0.0f ? sqrtf(sg) : 0.0f;
        sb = sb > 0.0f ? sqrtf(sb) : 0.0f;
      }
      srgb_in[p]             = _linear_to_srgb(sr);
      srgb_in[p + plane]     = _linear_to_srgb(sg);
      srgb_in[p + 2 * plane] = _linear_to_srgb(sb);
    }
  }
  else if(ctx->shadow_boost)
  {
    // no profile: still boost shadows so the model stays within its
    // comfort zone, even though we treat WP values as sRGB
    for(size_t i = 0; i < in_pixels; i++)
    {
      const float v = in_patch[i];
      const float boosted = v > 0.0f ? sqrtf(v) : 0.0f;
      srgb_in[i] = _linear_to_srgb(boosted);
    }
  }
  else
  {
    for(size_t i = 0; i < in_pixels; i++)
      srgb_in[i] = _linear_to_srgb(in_patch[i]);
  }

  const int num_inputs = dt_ai_get_input_count(ctx->ai_ctx);
  if(num_inputs > MAX_MODEL_INPUTS)
  {
    g_free(srgb_in);
    return 1;
  }

  int64_t input_shape[] = {1, 3, h, w};
  dt_ai_tensor_t inputs[MAX_MODEL_INPUTS];
  memset(inputs, 0, sizeof(inputs));
  inputs[0] = (dt_ai_tensor_t){
    .data = (void *)srgb_in,
    .shape = input_shape,
    .ndim = 4,
    .type = DT_AI_FLOAT};

  // noise level map for multi-input models
  float *noise_map = NULL;
  int64_t noise_shape[] = {1, 1, h, w};
  if(num_inputs >= 2)
  {
    const size_t map_size = (size_t)w * h;
    noise_map = g_try_malloc(map_size * sizeof(float));
    if(!noise_map)
    {
      g_free(srgb_in);
      return 1;
    }
    const float sigma_norm = 25.0f / 255.0f;
    for(size_t i = 0; i < map_size; i++)
      noise_map[i] = sigma_norm;
    inputs[1] = (dt_ai_tensor_t){
      .data = (void *)noise_map,
      .shape = noise_shape,
      .ndim = 4,
      .type = DT_AI_FLOAT};
  }

  int64_t output_shape[] = {1, 3, out_h, out_w};
  dt_ai_tensor_t output = {
    .data = (void *)out_patch,
    .shape = output_shape,
    .ndim = 4,
    .type = DT_AI_FLOAT};

  int ret = dt_ai_run(ctx->ai_ctx, inputs, num_inputs,
                      &output, 1);
  g_free(srgb_in);
  g_free(noise_map);
  if(ret != 0)
  {
    g_free(in_gamut_mask);
    return ret;
  }

  // convert model output back to the working profile
  //
  // with profile: apply inverse sRGB gamma, then check if the ORIGINAL
  // input pixel (converted to sRGB linear) is representable in sRGB
  // gamut. if yes, use model output converted back to working profile.
  // if no, pass through the original pixel (wide-gamut colors preserved,
  // no denoising on those pixels). upscale has no pixel-to-pixel
  // correspondence so pass-through is not possible — always use the
  // model output
  //
  // without profile: fall back to per-channel pass-through in the
  // original (working-profile-as-sRGB) space
  const gboolean boost = ctx->shadow_boost;
  if(ctx->has_profile && scale == 1 && ctx->preserve_wide_gamut)
  {
    const size_t out_plane = (size_t)out_w * out_h;
    const float *Mi = ctx->srgb_to_wp;
    // pass 1: write denoised values for in-gamut pixels; out-of-gamut
    // pixels get plain pass-through as a fallback (used only when no
    // in-gamut neighbors are found in pass 2)
    for(size_t p = 0; p < out_plane; p++)
    {
      if(in_gamut_mask[p])
      {
        float sr = _srgb_to_linear(out_patch[p]);
        float sg = _srgb_to_linear(out_patch[p + out_plane]);
        float sb = _srgb_to_linear(out_patch[p + 2 * out_plane]);
        if(boost) { sr *= sr; sg *= sg; sb *= sb; }
        out_patch[p]                 = Mi[0] * sr + Mi[1] * sg + Mi[2] * sb;
        out_patch[p + out_plane]     = Mi[3] * sr + Mi[4] * sg + Mi[5] * sb;
        out_patch[p + 2 * out_plane] = Mi[6] * sr + Mi[7] * sg + Mi[8] * sb;
      }
      else
      {
        out_patch[p]                 = in_patch[p];
        out_patch[p + out_plane]     = in_patch[p + plane];
        out_patch[p + 2 * out_plane] = in_patch[p + 2 * plane];
      }
    }
    // pass 2: luminance-only smoothing for out-of-gamut pixels. the
    // original pixel keeps its chroma (wide-gamut color preserved
    // exactly) but its brightness is shifted to match the local
    // average luminance of denoised in-gamut neighbors; this kills
    // the single-pixel speckles that pass-through would otherwise
    // leave visible against the denoised background
    const int radius = 2;  // 5x5 window
    for(int y = 0; y < out_h; y++)
    {
      for(int x = 0; x < out_w; x++)
      {
        const size_t p = (size_t)y * out_w + x;
        if(in_gamut_mask[p]) continue;
        const float r0 = in_patch[p];
        const float g0 = in_patch[p + plane];
        const float b0 = in_patch[p + 2 * plane];
        const float Y_orig = _luma_rec709(r0, g0, b0);
        float sumY = 0.0f;
        int count = 0;
        const int y0 = y - radius < 0 ? 0 : y - radius;
        const int y1 = y + radius >= out_h ? out_h - 1 : y + radius;
        const int x0 = x - radius < 0 ? 0 : x - radius;
        const int x1 = x + radius >= out_w ? out_w - 1 : x + radius;
        for(int yy = y0; yy <= y1; yy++)
        {
          for(int xx = x0; xx <= x1; xx++)
          {
            const size_t q = (size_t)yy * out_w + xx;
            if(!in_gamut_mask[q]) continue;
            const float rq = out_patch[q];
            const float gq = out_patch[q + out_plane];
            const float bq = out_patch[q + 2 * out_plane];
            sumY += _luma_rec709(rq, gq, bq);
            count++;
          }
        }
        if(count > 0)
        {
          const float dY = sumY / (float)count - Y_orig;
          out_patch[p]                 = r0 + dY;
          out_patch[p + out_plane]     = g0 + dY;
          out_patch[p + 2 * out_plane] = b0 + dY;
        }
      }
    }
  }
  else if(ctx->has_profile && scale == 1)
  {
    // denoise with profile but NO pass-through: apply the inverse
    // matrix to every pixel. wide-gamut inputs will have been clipped
    // by the model, but we get denoising everywhere
    const size_t out_plane = (size_t)out_w * out_h;
    const float *Mi = ctx->srgb_to_wp;
    for(size_t p = 0; p < out_plane; p++)
    {
      float sr = _srgb_to_linear(out_patch[p]);
      float sg = _srgb_to_linear(out_patch[p + out_plane]);
      float sb = _srgb_to_linear(out_patch[p + 2 * out_plane]);
      if(boost) { sr *= sr; sg *= sg; sb *= sb; }
      out_patch[p]                 = Mi[0] * sr + Mi[1] * sg + Mi[2] * sb;
      out_patch[p + out_plane]     = Mi[3] * sr + Mi[4] * sg + Mi[5] * sb;
      out_patch[p + 2 * out_plane] = Mi[6] * sr + Mi[7] * sg + Mi[8] * sb;
    }
  }
  else if(scale == 1)
  {
    // no profile set: per-channel pass-through, treats working-profile
    // numbers as if they were sRGB. colors will be slightly shifted
    // for wide-gamut working profiles — rely on the profile path above
    // when possible. pass-through still honored via preserve_wide_gamut
    for(size_t i = 0; i < out_pixels; i++)
    {
      const float in = in_patch[i];
      if(ctx->preserve_wide_gamut && (in < 0.0f || in > 1.0f))
      {
        out_patch[i] = in;
      }
      else
      {
        float v = _srgb_to_linear(out_patch[i]);
        if(boost) v *= v;
        out_patch[i] = v;
      }
    }
  }
  else
  {
    // upscale: no pixel-to-pixel correspondence, use model output as-is
    if(ctx->has_profile)
    {
      const size_t out_plane = (size_t)out_w * out_h;
      const float *Mi = ctx->srgb_to_wp;
      for(size_t p = 0; p < out_plane; p++)
      {
        float sr = _srgb_to_linear(out_patch[p]);
        float sg = _srgb_to_linear(out_patch[p + out_plane]);
        float sb = _srgb_to_linear(out_patch[p + 2 * out_plane]);
        if(boost) { sr *= sr; sg *= sg; sb *= sb; }
        out_patch[p]                 = Mi[0] * sr + Mi[1] * sg + Mi[2] * sb;
        out_patch[p + out_plane]     = Mi[3] * sr + Mi[4] * sg + Mi[5] * sb;
        out_patch[p + 2 * out_plane] = Mi[6] * sr + Mi[7] * sg + Mi[8] * sb;
      }
    }
    else
    {
      for(size_t i = 0; i < out_pixels; i++)
      {
        float v = _srgb_to_linear(out_patch[i]);
        if(boost) v *= v;
        out_patch[i] = v;
      }
    }
  }

  g_free(in_gamut_mask);
  return 0;
}

// per-image gate for the shadow-boost curve; enable only when the image
// has substantial near-black area to protect — bright images would only
// pay the curve cost (minor highlight compression) for no gain;
// thresholds tuned so localized very-dark features (a tree hollow, a
// silhouette) do NOT trigger; only broad noisy shadow regions do
//
// in_data is interleaved float4 RGBA
#define _SHADOW_BOOST_THRESHOLD 0.005f  // 0.5% linear luminance
#define _SHADOW_BOOST_FRACTION  0.10f   // 10% of sampled pixels
static gboolean _image_has_deep_shadows(const float *in_data, int w, int h)
{
  const size_t stride = 16;  // sample 1/256 of pixels for speed
  size_t dark = 0, total = 0;
  for(size_t y = 0; y < (size_t)h; y += stride)
    for(size_t x = 0; x < (size_t)w; x += stride)
    {
      const size_t p = ((size_t)y * w + x) * 4;
      const float luma = 0.2126f * in_data[p]
                       + 0.7152f * in_data[p + 1]
                       + 0.0722f * in_data[p + 2];
      if(luma < _SHADOW_BOOST_THRESHOLD) dark++;
      total++;
    }
  return total > 0 && (float)dark / total >= _SHADOW_BOOST_FRACTION;
}

int dt_restore_process_tiled(dt_restore_context_t *ctx,
                             const float *in_data,
                             int width, int height,
                             int scale,
                             dt_restore_row_writer_t row_writer,
                             void *writer_data,
                             struct _dt_job_t *control_job)
{
  if(!ctx || !ctx->ai_ctx || !in_data || !row_writer)
    return 1;

  // for shadow-boost-capable models, decide per-image whether the
  // curve is worth applying; one analysis per call, before tiling,
  // so all tiles see the same flag (avoids per-tile seams)
  if(ctx->shadow_boost_capable)
  {
    const gboolean dark = _image_has_deep_shadows(in_data, width, height);
    ctx->shadow_boost = dark;
    dt_print(DT_DEBUG_AI, "[restore_rgb] shadow boost %s",
             dark ? "enabled" : "disabled");
  }

  const int O = dt_restore_get_overlap(scale);
  const int S = scale;
  const int out_w = width * S;
  // ladder was resolved at load time (either model's input_sizes or
  // the built-in default for this scale) and travels with the context
  const int *ladder = ctx->tile_ladder;
  const int n_ladder = ctx->n_tile_ladder;
  int T = ctx->tile_size;

  // outer retry loop: on inference failure (e.g. GPU OOM) drop to the
  // next smaller candidate in the shared ladder and try again
retry:;
  int step = T - 2 * O;
  int T_out = T * S;
  int O_out = O * S;
  int step_out = step * S;
  size_t in_plane = (size_t)T * T;
  size_t out_plane = (size_t)T_out * T_out;
  int cols = (width + step - 1) / step;
  int rows = (height + step - 1) / step;
  int total_tiles = cols * rows;

  dt_print(DT_DEBUG_AI,
           "[restore_rgb] tiling %dx%d (scale=%d)"
           " -> %dx%d, %dx%d grid (%d tiles, T=%d)",
           width, height, S, out_w, height * S,
           cols, rows, total_tiles, T);

  float *tile_in = g_try_malloc(
    in_plane * 3 * sizeof(float));
  float *tile_out = g_try_malloc(
    out_plane * 3 * sizeof(float));
  float *row_buf = g_try_malloc(
    (size_t)out_w * step_out * 3 * sizeof(float));
  if(!tile_in || !tile_out || !row_buf)
  {
    g_free(tile_in);
    g_free(tile_out);
    g_free(row_buf);
    return 1;
  }

  int res = 0;
  int tile_count = 0;

  for(int ty = 0; ty < rows; ty++)
  {
    const int y = ty * step;
    const int valid_h = (y + step > height)
      ? height - y : step;
    const int valid_h_out = valid_h * S;

    memset(row_buf, 0,
           (size_t)out_w * valid_h_out * 3
           * sizeof(float));

    for(int tx = 0; tx < cols; tx++)
    {
      if(control_job
         && dt_control_job_get_state(control_job)
              == DT_JOB_STATE_CANCELLED)
      {
        res = 1;
        goto cleanup;
      }

      const int x = tx * step;
      const int in_x = x - O;
      const int in_y = y - O;
      const int needs_mirror
        = (in_x < 0 || in_y < 0
           || in_x + T > width
           || in_y + T > height);

      // interleaved RGBx -> planar RGB
      if(needs_mirror)
      {
        for(int dy = 0; dy < T; ++dy)
        {
          const int sy = _mirror(in_y + dy, height);
          for(int dx = 0; dx < T; ++dx)
          {
            const int sx
              = _mirror(in_x + dx, width);
            const size_t po = (size_t)dy * T + dx;
            const size_t si
              = ((size_t)sy * width + sx) * 4;
            tile_in[po] = in_data[si + 0];
            tile_in[po + in_plane]
              = in_data[si + 1];
            tile_in[po + 2 * in_plane]
              = in_data[si + 2];
          }
        }
      }
      else
      {
        for(int dy = 0; dy < T; ++dy)
        {
          const float *row
            = in_data
              + ((size_t)(in_y + dy) * width
                 + in_x) * 4;
          const size_t ro = (size_t)dy * T;
          for(int dx = 0; dx < T; ++dx)
          {
            tile_in[ro + dx] = row[dx * 4 + 0];
            tile_in[ro + dx + in_plane]
              = row[dx * 4 + 1];
            tile_in[ro + dx + 2 * in_plane]
              = row[dx * 4 + 2];
          }
        }
      }

      if(dt_restore_run_patch(
           ctx, tile_in, T, T, tile_out, S) != 0)
      {
        // retry with the next smaller ladder entry if no rows have
        // been delivered yet (safe to restart). once rows are written
        // we can't rewind the row_writer (e.g. TIFF is sequential).
        // _reload_session() recreates the ORT session for the smaller
        // tile size (dim overrides are shape-specific).
        int next_T = 0;
        for(int i = 0; i < n_ladder; i++)
          if(ladder[i] < T) { next_T = ladder[i]; break; }
        if(next_T > 0 && ty == 0
           && dt_restore_reload_session(ctx, next_T))
        {
          dt_print(DT_DEBUG_AI,
                   "[restore_rgb] inference failed at tile %d,%d "
                   "(T=%d), retrying with T=%d",
                   x, y, T, next_T);
          g_free(tile_in);
          g_free(tile_out);
          g_free(row_buf);
          T = next_T;
          goto retry;
        }
        dt_print(DT_DEBUG_AI,
                 "[restore_rgb] inference failed at"
                 " tile %d,%d (T=%d, minimum reached)", x, y, T);
        res = 1;
        goto cleanup;
      }

      // valid region -> row buffer
      const int valid_w = (x + step > width)
        ? width - x : step;
      const int valid_w_out = valid_w * S;

      for(int dy = 0; dy < valid_h_out; ++dy)
      {
        const size_t src_row
          = (size_t)(O_out + dy) * T_out + O_out;
        const size_t dst_row
          = ((size_t)dy * out_w + x * S) * 3;
        for(int dx = 0; dx < valid_w_out; ++dx)
        {
          row_buf[dst_row + dx * 3 + 0]
            = tile_out[src_row + dx];
          row_buf[dst_row + dx * 3 + 1]
            = tile_out[src_row + dx + out_plane];
          row_buf[dst_row + dx * 3 + 2]
            = tile_out[src_row + dx
                       + 2 * out_plane];
        }
      }

      tile_count++;
      if(control_job)
        dt_control_job_set_progress(control_job,
                                    (double)tile_count / total_tiles);
    }

    // deliver completed scanlines via callback
    for(int dy = 0; dy < valid_h_out; dy++)
    {
      const float *src = row_buf + (size_t)dy * out_w * 3;
      if(row_writer(src, out_w, y * S + dy,
                    writer_data) != 0)
      {
        res = 1;
        goto cleanup;
      }
    }
  }

  // persist tile size on first full success so subsequent runs skip OOM retry
  if(res == 0)
    dt_restore_persist_tile_size(ctx);

cleanup:
  g_free(tile_in);
  g_free(tile_out);
  g_free(row_buf);
  return res;
}

void dt_restore_apply_detail_recovery(const float *original_4ch,
                                      float *denoised_4ch,
                                      int width, int height,
                                      float alpha)
{
  const size_t npix = (size_t)width * height;

  float *const restrict lum_residual
    = dt_alloc_align_float(npix);
  if(!lum_residual) return;

#ifdef _OPENMP
#pragma omp parallel for simd default(none)           \
  dt_omp_firstprivate(original_4ch, denoised_4ch,     \
                      lum_residual, npix)             \
  schedule(simd:static)                               \
  aligned(original_4ch, denoised_4ch, lum_residual:64)
#endif
  for(size_t i = 0; i < npix; i++)
  {
    const size_t p = i * 4;
    const float lum_orig
      = 0.2126f * original_4ch[p + 0]
        + 0.7152f * original_4ch[p + 1]
        + 0.0722f * original_4ch[p + 2];
    const float lum_den
      = 0.2126f * denoised_4ch[p + 0]
        + 0.7152f * denoised_4ch[p + 1]
        + 0.0722f * denoised_4ch[p + 2];
    lum_residual[i] = lum_orig - lum_den;
  }

  float noise[DWT_DETAIL_BANDS];
  _compute_adaptive_noise(lum_residual, npix, noise);
  dwt_denoise(lum_residual, width, height,
              DWT_DETAIL_BANDS, noise);

#ifdef _OPENMP
#pragma omp parallel for simd default(none)       \
  dt_omp_firstprivate(denoised_4ch, lum_residual, \
                      npix, alpha)                \
  schedule(simd:static)                           \
  aligned(denoised_4ch, lum_residual:64)
#endif
  for(size_t i = 0; i < npix; i++)
  {
    const size_t p = i * 4;
    const float d = alpha * lum_residual[i];
    denoised_4ch[p + 0] += d;
    denoised_4ch[p + 1] += d;
    denoised_4ch[p + 2] += d;
  }

  dt_free_align(lum_residual);
}


float *dt_restore_compute_dwt_detail(const float *before_3ch,
                                     const float *after_3ch,
                                     int width, int height)
{
  const size_t npix = (size_t)width * height;
  float *lum_residual = dt_alloc_align_float(npix);
  if(!lum_residual) return NULL;

  for(size_t i = 0; i < npix; i++)
  {
    const size_t si = i * 3;
    const float lum_orig
      = 0.2126f * before_3ch[si + 0]
        + 0.7152f * before_3ch[si + 1]
        + 0.0722f * before_3ch[si + 2];
    const float lum_den
      = 0.2126f * after_3ch[si + 0]
        + 0.7152f * after_3ch[si + 1]
        + 0.0722f * after_3ch[si + 2];
    lum_residual[i] = lum_orig - lum_den;
  }

  float noise[DWT_DETAIL_BANDS];
  _compute_adaptive_noise(lum_residual, npix, noise);
  dwt_denoise(lum_residual, width, height,
              DWT_DETAIL_BANDS, noise);

  return lum_residual;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
