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
#include "common/ai/restore_common.h"
#include "ai/backend.h"
#include "common/darktable.h"
#include "common/ai_models.h"
#include "common/colorspaces.h"
#include "common/iop_order.h"
#include "control/control.h"
#include "control/jobs.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/pixelpipe_hb.h"
#include "imageio/imageio_common.h"

#include <math.h>
#include <string.h>

#define OVERLAP_DENOISE 64
#define OVERLAP_UPSCALE 16

// candidate tile sizes from largest to smallest, used by both the
// startup memory-budget selector and the runtime OOM-retry fallback.
// the memory-budget check gates which entry is chosen at startup;
// the tile size cache persists the result so JIT-compiling EPs
// (MIGraphX, CoreML, TensorRT) only pay the compile cost once
#define DT_RESTORE_TILE_LADDER_1X {2048, 1536, 1024, 768, 512, 384, 256}
#define DT_RESTORE_TILE_LADDER_SR {768, 512, 384, 256, 192}

// --- environment lifecycle ---

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

// --- model lifecycle ---

#define TASK_DENOISE    "denoise"
#define TASK_RAWDENOISE "rawdenoise"
#define TASK_UPSCALE    "upscale"

// --- manifest policy parsers ---
//
// parse a variant's string attribute into the matching enum. unknown
// values return UNKNOWN (for input_kind — caller validates), or the
// supplied default (for the other three — caller has already decided
// the default matches today's RawNIND behavior)

static dt_restore_input_kind_t _parse_input_kind(const char *s)
{
  if(!s) return DT_RESTORE_INPUT_KIND_UNKNOWN;
  if(!g_strcmp0(s, "bayer_v1"))  return DT_RESTORE_INPUT_KIND_BAYER_V1;
  if(!g_strcmp0(s, "xtrans_v1")) return DT_RESTORE_INPUT_KIND_XTRANS_V1;
  if(!g_strcmp0(s, "linear_v1")) return DT_RESTORE_INPUT_KIND_LINEAR_V1;
  return DT_RESTORE_INPUT_KIND_UNKNOWN;
}

static const char *_input_kind_name(dt_restore_input_kind_t k)
{
  switch(k)
  {
    case DT_RESTORE_INPUT_KIND_BAYER_V1:  return "bayer_v1";
    case DT_RESTORE_INPUT_KIND_XTRANS_V1: return "xtrans_v1";
    case DT_RESTORE_INPUT_KIND_LINEAR_V1: return "linear_v1";
    default:                              return "unknown";
  }
}

static dt_restore_colorspace_t _parse_colorspace(const char *s,
                                                 dt_restore_colorspace_t dflt)
{
  if(!s) return dflt;
  if(!g_strcmp0(s, "lin_rec2020")) return DT_RESTORE_CS_LIN_REC2020;
  if(!g_strcmp0(s, "camRGB"))      return DT_RESTORE_CS_CAMRGB;
  if(!g_strcmp0(s, "srgb_linear")) return DT_RESTORE_CS_SRGB_LINEAR;
  dt_print(DT_DEBUG_AI,
           "[restore] unknown input_colorspace '%s', using default", s);
  return dflt;
}

static dt_restore_wb_mode_t _parse_wb_mode(const char *s,
                                           dt_restore_wb_mode_t dflt)
{
  if(!s) return dflt;
  if(!g_strcmp0(s, "daylight")) return DT_RESTORE_WB_DAYLIGHT;
  if(!g_strcmp0(s, "as_shot"))  return DT_RESTORE_WB_AS_SHOT;
  if(!g_strcmp0(s, "none"))     return DT_RESTORE_WB_NONE;
  dt_print(DT_DEBUG_AI,
           "[restore] unknown wb_norm '%s', using default", s);
  return dflt;
}

static dt_restore_output_scale_t _parse_output_scale(const char *s,
                                                     dt_restore_output_scale_t dflt)
{
  if(!s) return dflt;
  if(!g_strcmp0(s, "match_gain")) return DT_RESTORE_OUT_MATCH_GAIN;
  if(!g_strcmp0(s, "absolute"))   return DT_RESTORE_OUT_ABSOLUTE;
  dt_print(DT_DEBUG_AI,
           "[restore] unknown output_scale '%s', using default", s);
  return dflt;
}

static dt_restore_bayer_orientation_t _parse_bayer_orientation(
    const char *s, dt_restore_bayer_orientation_t dflt)
{
  if(!s) return dflt;
  if(!g_strcmp0(s, "force_rggb")) return DT_RESTORE_BAYER_FORCE_RGGB;
  if(!g_strcmp0(s, "native"))     return DT_RESTORE_BAYER_NATIVE;
  dt_print(DT_DEBUG_AI,
           "[restore] unknown bayer_orientation '%s', using default", s);
  return dflt;
}

static dt_restore_edge_pad_t _parse_edge_pad(const char *s,
                                             dt_restore_edge_pad_t dflt)
{
  if(!s) return dflt;
  if(!g_strcmp0(s, "mirror_cropped")) return DT_RESTORE_EDGE_MIRROR_CROPPED;
  if(!g_strcmp0(s, "mirror"))         return DT_RESTORE_EDGE_MIRROR;
  dt_print(DT_DEBUG_AI,
           "[restore] unknown edge_pad '%s', using default", s);
  return dflt;
}

// target_mean accepts "null" as an explicit disable; missing key falls
// back to the per-variant default (NAN for bayer, 0.3 for linear).
// a numeric string parses via g_ascii_strtod
static float _parse_target_mean(const dt_ai_model_info_t *info,
                                const char *key, float dflt)
{
  char *s = dt_ai_model_attribute_string(info, key);
  if(!s) return dflt;
  if(!g_strcmp0(s, "null") || !g_strcmp0(s, "none"))
  {
    g_free(s);
    return NAN;
  }
  char *endp = NULL;
  const double v = g_ascii_strtod(s, &endp);
  if(endp == s || !endp || *endp != '\0')
  {
    dt_print(DT_DEBUG_AI,
             "[restore] target_mean '%s' not parseable, using default", s);
    g_free(s);
    return dflt;
  }
  g_free(s);
  return (float)v;
}

static int _select_tile_size(const int *ladder, int n_ladder, int scale);
// resolve the tile ladder for a model: prefer the "input_sizes" JSON
// array attribute from config.json when present, otherwise fall back
// to the built-in ladder for the model's scale. always returns a
// freshly-allocated int[] + count that the caller owns and g_free()s
static void _resolve_tile_ladder(const dt_ai_model_info_t *info,
                                 int scale,
                                 int **out_sizes,
                                 int *out_count);

// returns the cached tile size for model_id+scale+provider combo, or 0 if not set
static int _get_cached_tile_size(const char *model_id, int scale)
{
  char *prov = dt_conf_get_string(DT_AI_CONF_PROVIDER);
  char *key = g_strdup_printf("plugins/ai/tile_cache/%s/%d/%s",
                               model_id, scale, prov ? prov : "auto");
  g_free(prov);
  const int cached = dt_conf_get_int(key);
  g_free(key);
  return cached;
}

// persist a successful tile size to darktablerc so the next run skips OOM retry
static void _set_cached_tile_size(const char *model_id, int scale, int tile_size)
{
  char *prov = dt_conf_get_string(DT_AI_CONF_PROVIDER);
  char *key = g_strdup_printf("plugins/ai/tile_cache/%s/%d/%s",
                               model_id, scale, prov ? prov : "auto");
  g_free(prov);
  dt_conf_set_int(key, tile_size);
  g_free(key);
}

// internal: create an ORT session for model_id/model_file with spatial dims
// fixed to tile_size. returns a new ai_ctx, or NULL on failure
static dt_ai_context_t *_create_session(dt_ai_environment_t *ai_env,
                                        const char *model_id,
                                        const char *model_file,
                                        const char *dim_h,
                                        const char *dim_w,
                                        int tile_size,
                                        uint32_t ep_flags)
{
  const dt_ai_dim_override_t overrides[] = {
    { "batch_size", 1 },
    { "batch",      1 },
    { dim_h,        tile_size },
    { dim_w,        tile_size },
  };
  return dt_ai_load_model_ext(
    ai_env, model_id, model_file,
    DT_AI_PROVIDER_CONFIGURED, DT_AI_OPT_ALL,
    overrides, (int)G_N_ELEMENTS(overrides), ep_flags);
}

// internal: resolve task -> model_id -> load with tile size dim overrides
static dt_restore_context_t *_load(dt_restore_env_t *env,
                                   const char *task,
                                   const char *variant,
                                   const char *default_file,
                                   int scale)
{
  if(!env) return NULL;

  char *model_id = dt_ai_models_get_active_for_task(task);
  if(!model_id || !model_id[0])
  {
    g_free(model_id);
    return NULL;
  }

  // look up spatial dimension names for this model
  const char *dim_h, *dim_w;
  dt_ai_models_get_spatial_dims(darktable.ai_registry, model_id,
                                &dim_h, &dim_w);

  const dt_ai_model_info_t *info
    = dt_ai_get_model_info_by_id(env->ai_env, model_id);

  // variant-aware config lookup: variant models must declare their ONNX
  // filename under variants.<variant>.onnx. input_kind is stashed on ctx
  // so raw paths can sanity-check they're pointing at the right model.
  // non-variant models (denoise, upscale) pass variant=NULL and supply
  // the filename directly via default_file
  char *variant_file = NULL;
  char *input_kind = NULL;
  // policy strings (all optional; NULL falls through to defaults)
  char *cs_str = NULL, *wb_str = NULL, *scale_str = NULL;
  char *bo_str = NULL, *edge_str = NULL;
  // expected input_kind for this variant slot. raw variants MUST match
  // one of the declared v1 contracts; non-variant tasks pass UNKNOWN
  // and skip the contract check entirely
  dt_restore_input_kind_t expected_kind = DT_RESTORE_INPUT_KIND_UNKNOWN;
  if(variant)
  {
    char *k_file  = g_strdup_printf("variants.%s.onnx", variant);
    char *k_kind  = g_strdup_printf("variants.%s.input_kind", variant);
    char *k_cs    = g_strdup_printf("variants.%s.input_colorspace", variant);
    char *k_wb    = g_strdup_printf("variants.%s.wb_norm", variant);
    char *k_scale = g_strdup_printf("variants.%s.output_scale", variant);
    char *k_bo    = g_strdup_printf("variants.%s.bayer_orientation", variant);
    char *k_edge  = g_strdup_printf("variants.%s.edge_pad", variant);
    variant_file = dt_ai_model_attribute_string(info, k_file);
    input_kind   = dt_ai_model_attribute_string(info, k_kind);
    cs_str       = dt_ai_model_attribute_string(info, k_cs);
    wb_str       = dt_ai_model_attribute_string(info, k_wb);
    scale_str    = dt_ai_model_attribute_string(info, k_scale);
    bo_str       = dt_ai_model_attribute_string(info, k_bo);
    edge_str     = dt_ai_model_attribute_string(info, k_edge);
    g_free(k_file);
    g_free(k_kind);
    g_free(k_cs);
    g_free(k_wb);
    g_free(k_scale);
    g_free(k_bo);
    g_free(k_edge);

    if(!variant_file)
    {
      dt_print(DT_DEBUG_AI,
               "[restore] model %s declares no variants.%s.onnx — "
               "cannot load variant",
               model_id, variant);
      g_free(input_kind);
      g_free(cs_str);
      g_free(wb_str);
      g_free(scale_str);
      g_free(bo_str);
      g_free(edge_str);
      g_free(model_id);
      return NULL;
    }

    // contract check: the variant slot name pins which input_kind we
    // expect. older manifests predate the label; if unset, assume the
    // expected one (back-compat). a declared-but-wrong label is a hard
    // error — refusing to load keeps mis-packaged ONNX from crashing
    // at inference with a confusing shape-mismatch
    if(!g_strcmp0(task, TASK_RAWDENOISE))
    {
      if(!g_strcmp0(variant, "bayer"))
        expected_kind = DT_RESTORE_INPUT_KIND_BAYER_V1;
      else if(!g_strcmp0(variant, "xtrans"))
        expected_kind = DT_RESTORE_INPUT_KIND_XTRANS_V1;
      else if(!g_strcmp0(variant, "linear"))
        expected_kind = DT_RESTORE_INPUT_KIND_LINEAR_V1;
    }
    if(expected_kind != DT_RESTORE_INPUT_KIND_UNKNOWN)
    {
      const dt_restore_input_kind_t declared = _parse_input_kind(input_kind);
      const gboolean missing = (input_kind == NULL);
      const gboolean mismatch
        = !missing && declared != expected_kind;
      if(mismatch || (!missing && declared == DT_RESTORE_INPUT_KIND_UNKNOWN))
      {
        dt_print(DT_DEBUG_AI,
                 "[restore] model %s variant '%s': input_kind '%s' "
                 "does not match expected '%s' — refusing to load",
                 model_id, variant, input_kind,
                 _input_kind_name(expected_kind));
        dt_control_log(_("raw denoise model %s: incompatible input_kind"),
                       model_id);
        g_free(input_kind);
        g_free(cs_str);
        g_free(wb_str);
        g_free(scale_str);
        g_free(bo_str);
        g_free(edge_str);
        g_free(variant_file);
        g_free(model_id);
        return NULL;
      }
    }

    dt_print(DT_DEBUG_AI,
             "[restore] variant '%s': file=%s input_kind=%s",
             variant, variant_file,
             input_kind ? input_kind : "(none)");
  }
  const char *model_file = variant ? variant_file : default_file;

  // resolve the tile ladder: model-declared input_sizes if present,
  // otherwise a copy of the built-in ladder for this scale
  int *tile_ladder = NULL;
  int n_tile_ladder = 0;
  _resolve_tile_ladder(info, scale, &tile_ladder, &n_tile_ladder);

  // select tile size from cache, but only if the cached value is still
  // a member of the ladder — otherwise a model upgrade that narrowed
  // its supported input_sizes would load with a stale size and fail
  // at graph shape inference (U-Nets are strict about spatial dims)
  int tile_size = _get_cached_tile_size(model_id, scale);
  gboolean cached_ok = FALSE;
  for(int i = 0; i < n_tile_ladder && !cached_ok; i++)
    if(tile_ladder[i] == tile_size) cached_ok = TRUE;
  if(!cached_ok)
  {
    if(tile_size > 0)
      dt_print(DT_DEBUG_AI,
               "[restore] cached tile size %d not in ladder, re-selecting",
               tile_size);
    tile_size = _select_tile_size(tile_ladder, n_tile_ladder, scale);
  }

  // CoreML CPU-only flag: models whose intermediate activations
  // overflow FP16 (e.g. raw denoise) declare this in config.json
  // to force CoreML's CPU path which runs FP32
  const uint32_t ep_flags
    = dt_ai_model_attribute_bool(info, "coreml_cpu_only") ? 1 : 0;
  if(ep_flags)
    dt_print(DT_DEBUG_AI,
             "[restore] model %s: coreml_cpu_only=true (ep_flags=%u)",
             model_id, ep_flags);

  dt_ai_context_t *ai_ctx = _create_session(
    env->ai_env, model_id, model_file, dim_h, dim_w, tile_size,
    ep_flags);
  if(!ai_ctx)
  {
    g_free(model_id);
    g_free(tile_ladder);
    g_free(variant_file);
    g_free(input_kind);
    g_free(cs_str);
    g_free(wb_str);
    g_free(scale_str);
    g_free(bo_str);
    g_free(edge_str);
    return NULL;
  }

  dt_restore_context_t *ctx = g_new0(dt_restore_context_t, 1);
  ctx->ref_count           = 1;
  ctx->ai_ctx              = ai_ctx;
  ctx->env                 = env;
  ctx->task                = g_strdup(task);
  ctx->input_kind          = input_kind;   // take ownership
  ctx->scale               = scale;
  ctx->model_id            = model_id;
  ctx->model_file          = g_strdup(model_file);
  ctx->tile_size           = tile_size;
  ctx->tile_ladder         = tile_ladder;
  ctx->n_tile_ladder       = n_tile_ladder;
  ctx->ep_flags            = ep_flags;
  ctx->dim_h               = g_strdup(dim_h);
  ctx->dim_w               = g_strdup(dim_w);
  ctx->preserve_wide_gamut = TRUE;

  // resolve policy enums: per-variant defaults reproduce today's
  // RawNIND behavior exactly, so manifests that declare none of these
  // keys keep working unchanged. bayer path defaults to daylight WB
  // (training distribution); linear path defaults to as-shot WB (its
  // re-imported DNG benefits from matching the source tonemap — see
  // the rationale in dt_restore_raw_linear). output_scale defaults to
  // match_gain for both. linear gets a 0.30 exposure target; bayer
  // doesn't use one (NAN = disabled). input_colorspace only applies
  // to the linear path
  ctx->input_kind_enum = expected_kind;
  {
    // linear_v1 and xtrans_v1 share the demosaic-based pipeline
    // defaults (as-shot WB, lin_rec2020 colorspace, 0.30 training-
    // brightness exposure target). When a dedicated xtrans model
    // ships these defaults may need to diverge — override in the
    // manifest if so
    const gboolean demosaic_pipeline
      = (expected_kind == DT_RESTORE_INPUT_KIND_LINEAR_V1)
        || (expected_kind == DT_RESTORE_INPUT_KIND_XTRANS_V1);
    const dt_restore_wb_mode_t default_wb
      = demosaic_pipeline ? DT_RESTORE_WB_AS_SHOT : DT_RESTORE_WB_DAYLIGHT;
    const dt_restore_colorspace_t default_cs
      = demosaic_pipeline ? DT_RESTORE_CS_LIN_REC2020 : DT_RESTORE_CS_CAMRGB;
    const float default_tm = demosaic_pipeline ? 0.30f : NAN;
    ctx->wb_mode          = _parse_wb_mode(wb_str, default_wb);
    ctx->output_scale     = _parse_output_scale(scale_str, DT_RESTORE_OUT_MATCH_GAIN);
    ctx->input_colorspace = _parse_colorspace(cs_str, default_cs);
    char *k_tm = variant
      ? g_strdup_printf("variants.%s.target_mean", variant) : NULL;
    ctx->target_mean = k_tm
      ? _parse_target_mean(info, k_tm, default_tm) : default_tm;
    g_free(k_tm);

    // bayer-only packing knobs. bayer_v1's contract pairs with
    // force_rggb + mirror_cropped (matches RawNIND training which
    // physically crops to RGGB before tiling — so corner-tile mirror
    // reflections must happen in the cropped frame). a future
    // 'native' orientation would let a model see non-RGGB sensors
    // without any origin shift; paired default is mirror_absolute
    // since there's no cropped frame to reflect within
    const dt_restore_bayer_orientation_t default_bo
      = (expected_kind == DT_RESTORE_INPUT_KIND_BAYER_V1)
          ? DT_RESTORE_BAYER_FORCE_RGGB
          : DT_RESTORE_BAYER_NATIVE;
    ctx->bayer_orientation = _parse_bayer_orientation(bo_str, default_bo);
    const dt_restore_edge_pad_t default_edge
      = (ctx->bayer_orientation == DT_RESTORE_BAYER_FORCE_RGGB)
          ? DT_RESTORE_EDGE_MIRROR_CROPPED
          : DT_RESTORE_EDGE_MIRROR;
    ctx->edge_pad = _parse_edge_pad(edge_str, default_edge);
  }
  g_free(cs_str);
  g_free(wb_str);
  g_free(scale_str);
  g_free(bo_str);
  g_free(edge_str);
  // shadow boost capability is declared per-model via the
  // "attributes": { "shadow_boost": true } object in config.json;
  // models that hallucinate in dark patches opt in this way;
  // other models run as-is
  ctx->shadow_boost_capable
    = dt_ai_model_attribute_bool(info, "shadow_boost");
  ctx->shadow_boost = ctx->shadow_boost_capable;
  if(ctx->shadow_boost_capable)
    dt_print(DT_DEBUG_AI,
             "[restore] model %s declares shadow_boost attribute",
             model_id);
  g_free(variant_file);
  return ctx;
}

// internal: recreate the ORT session with a smaller tile size after OOM.
// updates ctx->ai_ctx and ctx->tile_size in place.
// returns TRUE on success, FALSE if the reload also fails.
//
// unload the old session BEFORE creating the new one: after a GPU OOM
// the old session is still holding VRAM, and trying to allocate even
// a tiny new session on top triggers a cascade of init failures in
// ORT's provider-fallback retry path. freeing first lets the new
// session fit without the retries
static gboolean _reload_session(dt_restore_context_t *ctx, int new_tile_size)
{
  dt_ai_unload_model(ctx->ai_ctx);
  ctx->ai_ctx = NULL;

  dt_ai_context_t *new_ctx = _create_session(
    ctx->env->ai_env, ctx->model_id, ctx->model_file,
    ctx->dim_h, ctx->dim_w, new_tile_size, ctx->ep_flags);
  if(!new_ctx) return FALSE;
  ctx->ai_ctx    = new_ctx;
  ctx->tile_size = new_tile_size;
  return TRUE;
}

dt_restore_context_t *dt_restore_load_denoise(dt_restore_env_t *env)
{
  return _load(env, TASK_DENOISE, NULL, NULL, 1);
}

dt_restore_sensor_class_t dt_restore_classify_sensor(const dt_image_t *img)
{
  if(!img || !(img->flags & DT_IMAGE_RAW))
    return DT_RESTORE_SENSOR_CLASS_UNSUPPORTED;
  if(img->flags & (DT_IMAGE_MONOCHROME | DT_IMAGE_MONOCHROME_BAYER))
    return DT_RESTORE_SENSOR_CLASS_UNSUPPORTED;
  const uint32_t filters = img->buf_dsc.filters;
  if(filters == 9u) return DT_RESTORE_SENSOR_CLASS_XTRANS;
  if(filters != 0u) return DT_RESTORE_SENSOR_CLASS_BAYER;
  return DT_RESTORE_SENSOR_CLASS_LINEAR;
}

dt_restore_context_t *dt_restore_load_rawdenoise_bayer(dt_restore_env_t *env)
{
  // scale 1x, same pipeline as denoise; filename comes from the model's
  // variants.bayer.onnx attribute. loading fails if the YAML doesn't
  // declare it — no silent fallback for broken model packages
  return _load(env, TASK_RAWDENOISE, "bayer", NULL, 1);
}

dt_restore_context_t *dt_restore_load_rawdenoise_linear(dt_restore_env_t *env)
{
  // generic-demosaic fallback: Foveon, monochrome-with-pattern, and
  // currently also X-Trans (until dt_restore_load_rawdenoise_xtrans
  // gets a dedicated variant to load)
  return _load(env, TASK_RAWDENOISE, "linear", NULL, 1);
}

dt_restore_context_t *dt_restore_load_rawdenoise_xtrans(dt_restore_env_t *env)
{
  // prefer a dedicated xtrans variant when the manifest declares one;
  // fall back to the linear pipeline otherwise. this lets a future
  // RawNIND release ship a dedicated X-Trans model via just a manifest
  // update — no code changes in darktable (assuming the dedicated model
  // shares the linear pipeline; a structurally different X-Trans input
  // format would still need its own preprocessing code)
  dt_restore_context_t *ctx = _load(env, TASK_RAWDENOISE, "xtrans", NULL, 1);
  if(!ctx)
  {
    dt_print(DT_DEBUG_AI,
             "[restore] no dedicated xtrans variant; using linear as fallback");
    ctx = _load(env, TASK_RAWDENOISE, "linear", NULL, 1);
  }
  return ctx;
}

dt_restore_context_t *dt_restore_load_upscale_x2(dt_restore_env_t *env)
{
  return _load(env, TASK_UPSCALE, NULL, "model_x2.onnx", 2);
}

dt_restore_context_t *dt_restore_load_upscale_x4(dt_restore_env_t *env)
{
  return _load(env, TASK_UPSCALE, NULL, "model_x4.onnx", 4);
}

dt_restore_context_t *dt_restore_ref(dt_restore_context_t *ctx)
{
  if(ctx)
    g_atomic_int_inc(&ctx->ref_count);
  return ctx;
}

void dt_restore_unref(dt_restore_context_t *ctx)
{
  if(ctx && g_atomic_int_dec_and_test(&ctx->ref_count))
  {
    dt_ai_unload_model(ctx->ai_ctx);
    g_free(ctx->task);
    g_free(ctx->input_kind);
    g_free(ctx->model_id);
    g_free(ctx->model_file);
    g_free(ctx->dim_h);
    g_free(ctx->dim_w);
    g_free(ctx->tile_ladder);
    g_free(ctx);
  }
}

static gboolean _model_available(dt_restore_env_t *env,
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

gboolean dt_restore_denoise_available(dt_restore_env_t *env)
{
  return _model_available(env, TASK_DENOISE);
}

gboolean dt_restore_rawdenoise_available(dt_restore_env_t *env)
{
  return _model_available(env, TASK_RAWDENOISE);
}

gboolean dt_restore_upscale_available(dt_restore_env_t *env)
{
  return _model_available(env, TASK_UPSCALE);
}

// --- public API ---

int dt_restore_get_overlap(int scale)
{
  return (scale > 1) ? OVERLAP_UPSCALE : OVERLAP_DENOISE;
}

static int _select_tile_size(const int *ladder, int n_ladder, int scale)
{
  const size_t avail = dt_get_available_mem();
  const size_t budget = avail / 4;

  for(int i = 0; i < n_ladder; i++)
  {
    const size_t T = (size_t)ladder[i];
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
               ladder[i], scale,
               total / (1024 * 1024),
               budget / (1024 * 1024));
      return ladder[i];
    }
  }

  dt_print(DT_DEBUG_AI,
           "[restore] using minimum tile size %d (budget %zuMB)",
           ladder[n_ladder - 1],
           budget / (1024 * 1024));
  return ladder[n_ladder - 1];
}

static void _resolve_tile_ladder(const dt_ai_model_info_t *info,
                                 int scale,
                                 int **out_sizes,
                                 int *out_count)
{
  // prefer the model's declared input_sizes if present: some exports
  // ship with a fixed set of supported tile sizes (e.g. the model was
  // compiled for specific spatial dims) and using anything outside
  // that list will either refuse to run or produce garbage
  int n = 0;
  int *sizes = dt_ai_model_attribute_int_array(info, "input_sizes", &n);
  if(sizes && n > 0)
  {
    *out_sizes = sizes;
    *out_count = n;
    return;
  }
  g_free(sizes);

  // fall back to the built-in ladder for the model's scale
  static const int ladder_1x[] = DT_RESTORE_TILE_LADDER_1X;
  static const int ladder_sr[] = DT_RESTORE_TILE_LADDER_SR;
  const int *src = (scale > 1) ? ladder_sr : ladder_1x;
  const int src_n = (scale > 1)
    ? (int)(sizeof(ladder_sr) / sizeof(int))
    : (int)(sizeof(ladder_1x) / sizeof(int));
  int *copy = g_new(int, src_n);
  memcpy(copy, src, src_n * sizeof(int));
  *out_sizes = copy;
  *out_count = src_n;
}

int dt_restore_run_patch_bayer(dt_restore_context_t *ctx,
                               const float *in_4ch,
                               int w, int h,
                               float *out_3ch)
{
  if(!ctx || !ctx->ai_ctx) return 1;

  int64_t in_shape[]  = { 1, 4, h, w };
  int64_t out_shape[] = { 1, 3, 2 * h, 2 * w };
  dt_ai_tensor_t input = {
    .data  = (void *)in_4ch,
    .shape = in_shape,
    .ndim  = 4,
    .type  = DT_AI_FLOAT,
  };
  dt_ai_tensor_t output = {
    .data  = out_3ch,
    .shape = out_shape,
    .ndim  = 4,
    .type  = DT_AI_FLOAT,
  };
  return dt_ai_run(ctx->ai_ctx, &input, 1, &output, 1);
}

int dt_restore_run_patch_3ch_raw(dt_restore_context_t *ctx,
                                 const float *in_3ch,
                                 int w, int h,
                                 float *out_3ch)
{
  if(!ctx || !ctx->ai_ctx) return 1;

  int64_t in_shape[]  = { 1, 3, h, w };
  int64_t out_shape[] = { 1, 3, h, w };
  dt_ai_tensor_t input = {
    .data  = (void *)in_3ch,
    .shape = in_shape,
    .ndim  = 4,
    .type  = DT_AI_FLOAT,
  };
  dt_ai_tensor_t output = {
    .data  = out_3ch,
    .shape = out_shape,
    .ndim  = 4,
    .type  = DT_AI_FLOAT,
  };
  return dt_ai_run(ctx->ai_ctx, &input, 1, &output, 1);
}

const int *dt_restore_get_tile_ladder(const dt_restore_context_t *ctx,
                                      int *out_count)
{
  if(!ctx)
  {
    if(out_count) *out_count = 0;
    return NULL;
  }
  if(out_count) *out_count = ctx->n_tile_ladder;
  return ctx->tile_ladder;
}

int dt_restore_get_tile_size(const dt_restore_context_t *ctx)
{
  return ctx ? ctx->tile_size : 0;
}

gboolean dt_restore_reload_session(dt_restore_context_t *ctx,
                                   int new_tile_size)
{
  if(!ctx) return FALSE;
  return _reload_session(ctx, new_tile_size);
}

void dt_restore_persist_tile_size(const dt_restore_context_t *ctx)
{
  if(ctx && ctx->model_id)
    _set_cached_tile_size(ctx->model_id, ctx->scale, ctx->tile_size);
}

// shared bridge: run the user's darktable pixelpipe on an arbitrary sensor
// buffer, capture the display-referred RGB at an ROI. used by both raw-
// denoise preview paths (Bayer CFA after re-mosaic, X-Trans CFA after
// re-mosaic) so the preview before/after match what the user sees in
// darkroom after Process + DNG re-import
int dt_restore_run_user_pipe_roi(dt_imgid_t imgid,
                                 void *input_native,
                                 int iw,
                                 int ih,
                                 int roi_x, int roi_y,
                                 int roi_w, int roi_h,
                                 int *out_w, int *out_h,
                                 float **out_rgb)
{
  if(out_rgb) *out_rgb = NULL;
  if(out_w) *out_w = 0;
  if(out_h) *out_h = 0;
  if(!input_native || iw <= 0 || ih <= 0
     || roi_w <= 0 || roi_h <= 0)
    return 1;

  dt_develop_t dev;
  dt_dev_init(&dev, FALSE);
  dt_dev_load_image(&dev, imgid);

  dt_dev_pixelpipe_t pipe;
  if(!dt_dev_pixelpipe_init_export(&pipe, iw, ih, IMAGEIO_FLOAT, FALSE))
  {
    dt_dev_cleanup(&dev);
    return 1;
  }

  // force output to linear Rec.709 (sRGB primaries, linear transfer)
  // so the widget's sRGB-gamma encoder displays the right colours.
  // MUST be called before create_nodes / synch_all: colorout reads
  // pipe->icc_type during commit_params at synch_all time. setting it
  // afterwards leaves colorout committed with the user's working
  // profile (often Rec.2020 / ProPhoto) → the cairo path then
  // applies sRGB gamma to wrong-primaries numbers → preview comes
  // out noticeably brighter / wrong colours vs. the batch DNG that
  // re-imports through the normal pipe
  dt_dev_pixelpipe_set_icc(&pipe, DT_COLORSPACE_LIN_REC709, NULL,
                           DT_INTENT_PERCEPTUAL);

  dt_ioppr_resync_modules_order(&dev);
  dt_dev_pixelpipe_set_input(&pipe, &dev, (float *)input_native,
                             iw, ih, 1.0f);
  dt_dev_pixelpipe_create_nodes(&pipe, &dev);
  dt_dev_pixelpipe_synch_all(&pipe, &dev);

  // skip rawdenoise — neural denoise already happened upstream.
  // safe to do this after synch_all: this only flips piece->enabled,
  // which the per-iop process loop checks at run time
  for(GList *n = pipe.nodes; n; n = g_list_next(n))
  {
    dt_dev_pixelpipe_iop_t *piece = n->data;
    if(dt_iop_module_is(piece->module->so, "rawdenoise"))
      piece->enabled = FALSE;
  }

  int pw = 0, ph = 0;
  dt_dev_pixelpipe_get_dimensions(&pipe, &dev, iw, ih, &pw, &ph);
  if(pw <= 0 || ph <= 0)
  {
    dt_dev_pixelpipe_cleanup(&pipe);
    dt_dev_cleanup(&dev);
    return 1;
  }
  pipe.processed_width = pw;
  pipe.processed_height = ph;

  // the ROI passed to process_no_gamma is in POST-pipe (final output)
  // coords, but the caller hands us sensor (input) coords so the ROI
  // lines up with the denoised CFA patch it built. forward-transform
  // the crop rectangle's 4 corners through the user's full geometry
  // chain (rawprepare + clipping + ashift + lens + rotatepixels + ...)
  // and use the INSCRIBED axis-aligned rectangle of the transformed
  // quad as the pipe ROI. the circumscribed AABB would include corner
  // triangles that back-project to sensor positions OUTSIDE the
  // denoised region — they'd render as noisy strips at the edges of
  // the preview. the inscribed rect is strictly inside the quad so
  // every sample back-projects within the patched region
  float corners[8] = {
    (float)roi_x,             (float)roi_y,
    (float)(roi_x + roi_w),   (float)roi_y,
    (float)roi_x,             (float)(roi_y + roi_h),
    (float)(roi_x + roi_w),   (float)(roi_y + roi_h),
  };
  dt_dev_distort_transform_plus(&dev, &pipe, 0.0,
                                DT_DEV_TRANSFORM_DIR_ALL_GEOMETRY,
                                corners, 4);

  // inscribed AABB: second-smallest x/y and second-largest x/y of the
  // 4 transformed corners. for a parallelogram these are the innermost
  // of each pair; for small lens distortions they're still safe (i.e.
  // lie inside the quad) because the quad stays nearly rectangular
  float xs[4] = { corners[0], corners[2], corners[4], corners[6] };
  float ys[4] = { corners[1], corners[3], corners[5], corners[7] };
  for(int i = 0; i < 3; i++)
    for(int j = i + 1; j < 4; j++)
    {
      if(xs[i] > xs[j]) { float t = xs[i]; xs[i] = xs[j]; xs[j] = t; }
      if(ys[i] > ys[j]) { float t = ys[i]; ys[i] = ys[j]; ys[j] = t; }
    }
  // round inward (ceil for inner min, floor for inner max) so the
  // chosen rect stays strictly inside the transformed quad
  int pipe_roi_x = (int)ceilf(xs[1]);
  int pipe_roi_y = (int)ceilf(ys[1]);
  int pipe_roi_w = (int)floorf(xs[2]) - pipe_roi_x;
  int pipe_roi_h = (int)floorf(ys[2]) - pipe_roi_y;

  // clamp to the pipe's actual processed extent; a sensor ROI near
  // the edge may transform to a post-pipe ROI that spills past pw/ph
  if(pipe_roi_x < 0) { pipe_roi_w += pipe_roi_x; pipe_roi_x = 0; }
  if(pipe_roi_y < 0) { pipe_roi_h += pipe_roi_y; pipe_roi_y = 0; }
  if(pipe_roi_x + pipe_roi_w > pw) pipe_roi_w = pw - pipe_roi_x;
  if(pipe_roi_y + pipe_roi_h > ph) pipe_roi_h = ph - pipe_roi_y;
  if(pipe_roi_w <= 0 || pipe_roi_h <= 0)
  {
    dt_dev_pixelpipe_cleanup(&pipe);
    dt_dev_cleanup(&dev);
    return 1;
  }

  // NB: process_no_gamma's return value signals "pipe altered
  // mid-flight", NOT success — check backbuf instead
  dt_dev_pixelpipe_process_no_gamma(&pipe, &dev,
                                    pipe_roi_x, pipe_roi_y,
                                    pipe_roi_w, pipe_roi_h, 1.0f);

  const int bw = pipe.backbuf_width;
  const int bh = pipe.backbuf_height;
  if(!pipe.backbuf || bw <= 0 || bh <= 0)
  {
    dt_dev_pixelpipe_cleanup(&pipe);
    dt_dev_cleanup(&dev);
    return 1;
  }

  // actual rendered dims may differ from the geometry-transformed
  // pipe ROI if the pipe is trimmed mid-chain (rare but possible).
  // callers must read *out_w / *out_h instead of assuming anything
  if(bw != pipe_roi_w || bh != pipe_roi_h)
    dt_print(DT_DEBUG_AI,
             "[restore] pipe ROI %dx%d -> backbuf %dx%d",
             pipe_roi_w, pipe_roi_h, bw, bh);

  // pipe.backbuf is 4ch interleaved RGBA; repack to 3ch for the
  // preview blend / display path
  float *rgb = g_try_malloc((size_t)bw * bh * 3 * sizeof(float));
  if(rgb)
  {
    const float *src = (const float *)pipe.backbuf;
    for(size_t i = 0; i < (size_t)bw * bh; i++)
    {
      rgb[i * 3 + 0] = src[i * 4 + 0];
      rgb[i * 3 + 1] = src[i * 4 + 1];
      rgb[i * 3 + 2] = src[i * 4 + 2];
    }
  }

  dt_dev_pixelpipe_cleanup(&pipe);
  dt_dev_cleanup(&dev);

  if(!rgb) return 1;
  *out_rgb = rgb;
  if(out_w) *out_w = bw;
  if(out_h) *out_h = bh;
  return 0;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
