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

#include "common/ai/restore.h"
#include "ai/backend.h"
#include "common/darktable.h"
#include "common/ai_models.h"
#include "common/imagebuf.h"
#include "control/jobs.h"

// forward-declare to avoid pulling in dwt.h (which
// includes OpenCL types when HAVE_OPENCL is defined)
extern void dwt_denoise(float *buf, int width, int height,
                        int bands, const float *noise);
#include <math.h>
#include <string.h>

#define OVERLAP_DENOISE 64
#define OVERLAP_UPSCALE 16
#define MAX_MODEL_INPUTS 4
#define DWT_DETAIL_BANDS 5

/* --- opaque struct definitions --- */

struct dt_restore_env_t
{
  dt_ai_environment_t *ai_env;
};

struct dt_restore_context_t
{
  dt_ai_context_t *ai_ctx;
  dt_restore_env_t *env;
  char *model_id;
  char *model_file;
  char *task;
};

static const float _dwt_detail_noise[DWT_DETAIL_BANDS] = {
  0.04f,  // band 0 (finest) — strong noise suppression
  0.03f,  // band 1
  0.02f,  // band 2
  0.01f,  // band 3
  0.005f  // band 4 (coarsest) — preserve most detail
};

/* --- environment lifecycle --- */

dt_restore_env_t *dt_restore_env_init(void)
{
  dt_ai_environment_t *ai = dt_ai_env_init(NULL);
  if(!ai) return NULL;

  dt_restore_env_t *env = g_new0(dt_restore_env_t, 1);
  env->ai_env = ai;
  return env;
}

void dt_restore_env_refresh(dt_restore_env_t *env)
{
  if(env && env->ai_env)
    dt_ai_env_refresh(env->ai_env);
}

void dt_restore_env_destroy(dt_restore_env_t *env)
{
  if(!env) return;
  if(env->ai_env)
    dt_ai_env_destroy(env->ai_env);
  g_free(env);
}

/* --- model lifecycle --- */

#define TASK_DENOISE "denoise"
#define TASK_UPSCALE "upscale"

// internal: resolve task -> model_id -> load
static dt_restore_context_t *_load(
  dt_restore_env_t *env,
  const char *task,
  const char *model_file)
{
  if(!env) return NULL;

  char *model_id
    = dt_ai_models_get_active_for_task(task);
  if(!model_id || !model_id[0])
  {
    g_free(model_id);
    return NULL;
  }

  dt_ai_context_t *ai_ctx = dt_ai_load_model(
    env->ai_env, model_id, model_file,
    DT_AI_PROVIDER_AUTO);
  if(!ai_ctx)
  {
    g_free(model_id);
    return NULL;
  }

  dt_restore_context_t *ctx
    = g_new0(dt_restore_context_t, 1);
  ctx->ai_ctx = ai_ctx;
  ctx->env = env;
  ctx->task = g_strdup(task);
  ctx->model_id = model_id;
  ctx->model_file = g_strdup(model_file);
  return ctx;
}

dt_restore_context_t *dt_restore_load_denoise(
  dt_restore_env_t *env)
{
  return _load(env, TASK_DENOISE, NULL);
}

dt_restore_context_t *dt_restore_load_upscale_x2(
  dt_restore_env_t *env)
{
  return _load(env, TASK_UPSCALE, "model_x2.onnx");
}

dt_restore_context_t *dt_restore_load_upscale_x4(
  dt_restore_env_t *env)
{
  return _load(env, TASK_UPSCALE, "model_x4.onnx");
}

void dt_restore_unload(dt_restore_context_t *ctx)
{
  if(!ctx || !ctx->ai_ctx) return;
  dt_ai_unload_model(ctx->ai_ctx);
  ctx->ai_ctx = NULL;
}

int dt_restore_reload(dt_restore_context_t *ctx)
{
  if(!ctx || !ctx->env) return 1;
  if(ctx->ai_ctx) return 0; // already loaded

  ctx->ai_ctx = dt_ai_load_model(
    ctx->env->ai_env, ctx->model_id,
    ctx->model_file, DT_AI_PROVIDER_AUTO);
  return ctx->ai_ctx ? 0 : 1;
}

void dt_restore_free(dt_restore_context_t *ctx)
{
  if(!ctx) return;
  dt_restore_unload(ctx);
  g_free(ctx->task);
  g_free(ctx->model_id);
  g_free(ctx->model_file);
  g_free(ctx);
}

static gboolean _model_available(
  dt_restore_env_t *env,
  const char *task)
{
  if(!env || !env->ai_env) return FALSE;
  char *model_id
    = dt_ai_models_get_active_for_task(task);
  if(!model_id || !model_id[0])
  {
    g_free(model_id);
    return FALSE;
  }
  const dt_ai_model_info_t *info
    = dt_ai_get_model_info_by_id(env->ai_env,
                                 model_id);
  g_free(model_id);
  return (info != NULL);
}

gboolean dt_restore_denoise_available(
  dt_restore_env_t *env)
{
  return _model_available(env, TASK_DENOISE);
}

gboolean dt_restore_upscale_available(
  dt_restore_env_t *env)
{
  return _model_available(env, TASK_UPSCALE);
}

/* --- color conversion --- */

static inline float _linear_to_srgb(float v)
{
  if(v <= 0.0f) return 0.0f;
  if(v >= 1.0f) return 1.0f;
  return (v <= 0.0031308f)
    ? 12.92f * v
    : 1.055f * powf(v, 1.0f / 2.4f) - 0.055f;
}

static inline float _srgb_to_linear(float v)
{
  if(v <= 0.0f) return 0.0f;
  if(v >= 1.0f) return 1.0f;
  return (v <= 0.04045f)
    ? v / 12.92f
    : powf((v + 0.055f) / 1.055f, 2.4f);
}

/* --- helpers --- */

static inline int _mirror(int v, int max)
{
  if(v < 0) v = -v;
  if(v >= max) v = 2 * max - 2 - v;
  if(v < 0) return 0;
  if(v >= max) return max - 1;
  return v;
}

/* --- public API --- */

int dt_restore_get_overlap(int scale)
{
  return (scale > 1) ? OVERLAP_UPSCALE : OVERLAP_DENOISE;
}

int dt_restore_select_tile_size(int scale)
{
  static const int candidates_1x[] =
    {2048, 1536, 1024, 768, 512, 384, 256};
  static const int n_1x = 7;
  static const int candidates_sr[] =
    {512, 384, 256, 192};
  static const int n_sr = 4;

  const int *candidates = (scale > 1)
    ? candidates_sr : candidates_1x;
  const int n_candidates = (scale > 1) ? n_sr : n_1x;

  const size_t avail = dt_get_available_mem();
  const size_t budget = avail / 4;

  for(int i = 0; i < n_candidates; i++)
  {
    const size_t T = (size_t)candidates[i];
    const size_t T_out = T * scale;
    const size_t tile_in = T * T * 3 * sizeof(float);
    const size_t tile_out
      = T_out * T_out * 3 * sizeof(float);
    const size_t ort_factor = (scale > 1) ? 50 : 100;
    const size_t ort_overhead
      = T_out * T_out * 3 * sizeof(float) * ort_factor;
    const size_t total = tile_in + tile_out + ort_overhead;

    if(total <= budget)
    {
      dt_print(DT_DEBUG_AI,
               "[restore] tile size %d (scale=%d, need %zuMB, budget %zuMB)",
               candidates[i], scale,
               total / (1024 * 1024),
               budget / (1024 * 1024));
      return candidates[i];
    }
  }

  dt_print(DT_DEBUG_AI,
           "[restore] using minimum tile size %d (budget %zuMB)",
           candidates[n_candidates - 1],
           budget / (1024 * 1024));
  return candidates[n_candidates - 1];
}

int dt_restore_run_patch(dt_restore_context_t *ctx,
                         const float *in_patch,
                         int w, int h,
                         float *out_patch,
                         int scale)
{
  if(!ctx || !ctx->ai_ctx) return 1;
  const int in_pixels = w * h * 3;
  const int out_w = w * scale;
  const int out_h = h * scale;
  const int out_pixels = out_w * out_h * 3;

  // convert to sRGB in scratch buffer
  float *srgb_in = g_try_malloc(in_pixels * sizeof(float));
  if(!srgb_in) return 1;

  for(int i = 0; i < in_pixels; i++)
  {
    float v = in_patch[i];
    if(v < 0.0f) v = 0.0f;
    if(v > 1.0f) v = 1.0f;
    srgb_in[i] = _linear_to_srgb(v);
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
  if(ret != 0) return ret;

  // sRGB -> linear
  for(int i = 0; i < out_pixels; i++)
    out_patch[i] = _srgb_to_linear(out_patch[i]);

  return 0;
}

int dt_restore_process_tiled(dt_restore_context_t *ctx,
                             const float *in_data,
                             int width, int height,
                             int scale,
                             dt_restore_row_writer_t row_writer,
                             void *writer_data,
                             struct _dt_job_t *control_job,
                             int tile_size)
{
  if(!ctx || !in_data || !row_writer)
    return 1;

  const int T = tile_size;
  const int O = (scale > 1) ? OVERLAP_UPSCALE : OVERLAP_DENOISE;
  const int step = T - 2 * O;
  const int S = scale;
  const int T_out = T * S;
  const int O_out = O * S;
  const int step_out = step * S;
  const int out_w = width * S;
  const size_t in_plane = (size_t)T * T;
  const size_t out_plane = (size_t)T_out * T_out;
  const int cols = (width + step - 1) / step;
  const int rows = (height + step - 1) / step;
  const int total_tiles = cols * rows;

  dt_print(DT_DEBUG_AI,
           "[restore] tiling %dx%d (scale=%d)"
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
        dt_print(DT_DEBUG_AI,
                 "[restore] inference failed at"
                 " tile %d,%d", x, y);
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

  dwt_denoise(lum_residual, width, height,
              DWT_DETAIL_BANDS, _dwt_detail_noise);

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

  dwt_denoise(lum_residual, width, height,
              DWT_DETAIL_BANDS, _dwt_detail_noise);

  return lum_residual;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
