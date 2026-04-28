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

#include "common/ai/segmentation.h"
#include "common/ai_models.h"
#include "ai/backend.h"
#include "common/database.h"
#include "common/darktable.h"
#include "common/file_location.h"
#include "common/grealpath.h"
#include <inttypes.h>
#include <math.h>
#include <string.h>

// SAM encoder expects 1024x1024 input
#define SAM_INPUT_SIZE 1024

// ImageNet normalization constants
static const float IMG_MEAN[3] = {123.675f, 116.28f, 103.53f};
static const float IMG_STD[3] = {58.395f, 57.12f, 57.375f};

// maximum number of dimensions for encoder output tensors
#define MAX_TENSOR_DIMS 8

// maximum number of encoder output tensors
#define MAX_ENCODER_OUTPUTS 4

// maximum number of masks the decoder can produce per decode pass,
// stack buffers (iou_pred[]) are sized to this limit
#define MAX_NUM_MASKS 8

// model architecture type, determines preprocessing, decoder I/O, and refinement
typedef enum dt_seg_model_type_t
{
  DT_SEG_MODEL_SAM,     // SAM/SAM2: multi-mask + IoU + low_res refinement
  DT_SEG_MODEL_SEGNEXT  // SegNext: single mask, full-res prev_mask refinement
} dt_seg_model_type_t;

struct dt_seg_context_t
{
  dt_ai_context_t *encoder;
  dt_ai_context_t *decoder;

  dt_seg_model_type_t model_type;
  gboolean normalize;     // TRUE = apply ImageNet normalization in preprocessing

  // encoder output shapes (queried from model at load time)
  int n_enc_outputs;
  int64_t enc_shapes[MAX_ENCODER_OUTPUTS][MAX_TENSOR_DIMS];
  int enc_ndims[MAX_ENCODER_OUTPUTS];

  // decoder properties
  int num_masks;            // masks per decode (1 = single-mask, 3-4 = multi-mask)
  int dec_mask_h, dec_mask_w; // decoder mask output dims (must be concrete)

  // encoder-to-decoder reorder map: decoder input i uses encoder output enc_order[i],
  // needed because encoder outputs may be in different order than decoder expects
  int enc_order[MAX_ENCODER_OUTPUTS];

  // cached encoder outputs
  float *enc_data[MAX_ENCODER_OUTPUTS];
  size_t enc_sizes[MAX_ENCODER_OUTPUTS];

  // previous mask for iterative refinement
  // SAM: low-res [1][1][prev_mask_dim][prev_mask_dim] (typically 256x256)
  // SegNext: full-res [1][1][prev_mask_dim][prev_mask_dim] (typically 1024x1024)
  float *prev_mask;
  int prev_mask_dim;
  gboolean has_prev_mask;

  // image dimensions that were encoded
  int encoded_width;
  int encoded_height;
  float scale; // SAM_INPUT_SIZE / max(w, h)
  gboolean image_encoded;

  char *model_id;      // model identifier (for cache validation)
  char *model_version; // model version (for cache validation)
};

/* --- preprocessing --- */

// resize RGB image so longest side = SAM_INPUT_SIZE, pad with zeros,
// convert HWC -> CHW.  When normalize=TRUE, applies ImageNet mean/std
// (SAM models).  When FALSE, scales to [0,1] only (SegNext bakes
// normalization into the ONNX encoder graph).
// output: float buffer [1, 3, SAM_INPUT_SIZE, SAM_INPUT_SIZE]
static float *
_preprocess_image(const uint8_t *rgb_data,
                  const int width,
                  const int height,
                  const gboolean normalize,
                  float *out_scale)
{
  const int target = SAM_INPUT_SIZE;
  const float scale = (float)target / (float)(width > height ? width : height);
  const int new_w = MIN((int)(width * scale + 0.5f), target);
  const int new_h = MIN((int)(height * scale + 0.5f), target);

  *out_scale = scale;

  const size_t buf_size = (size_t)3 * target * target;
  float *output = g_try_malloc0(buf_size * sizeof(float));
  if(!output)
    return NULL;

  // bilinear resize + normalize + HWC->CHW in one pass
  for(int y = 0; y < new_h; y++)
  {
    const float src_y = (float)y / scale;
    const int y0 = (int)src_y;
    const int y1 = (y0 + 1 < height) ? y0 + 1 : y0;
    const float fy = src_y - y0;

    for(int x = 0; x < new_w; x++)
    {
      const float src_x = (float)x / scale;
      const int x0 = (int)src_x;
      const int x1 = (x0 + 1 < width) ? x0 + 1 : x0;
      const float fx = src_x - x0;

      for(int c = 0; c < 3; c++)
      {
        // bilinear interpolation
        const float v00 = rgb_data[(y0 * width + x0) * 3 + c];
        const float v01 = rgb_data[(y0 * width + x1) * 3 + c];
        const float v10 = rgb_data[(y1 * width + x0) * 3 + c];
        const float v11 = rgb_data[(y1 * width + x1) * 3 + c];

        const float val = v00 * (1.0f - fx) * (1.0f - fy) + v01 * fx * (1.0f - fy)
          + v10 * (1.0f - fx) * fy + v11 * fx * fy;

        // write in CHW layout: offset = c * H * W + y * W + x
        const float pixel = normalize
          ? (val - IMG_MEAN[c]) / IMG_STD[c]  // ImageNet normalization (SAM)
          : val / 255.0f;                      // scale to [0,1] (SegNext)
        output[c * target * target + y * target + x] = pixel;
      }
    }
  }
  // padded area is already zero from g_try_malloc0

  return output;
}

/* --- bilinear crop+resize helper --- */

// crop the valid (non-padded) region from a SAM-space mask and bilinear-
// resize to the encoded image dimensions
static void _crop_resize_mask(const float *src,
                              const int src_w,
                              const int src_h,
                              float *dst,
                              const int dst_w,
                              const int dst_h,
                              const float scale,
                              const gboolean apply_sigmoid)
{
  const int valid_w = MIN((int)(dst_w * scale + 0.5f), src_w);
  const int valid_h = MIN((int)(dst_h * scale + 0.5f), src_h);

  for(int y = 0; y < dst_h; y++)
  {
    const float sy = (dst_h > 1) ? (float)y * (float)(valid_h - 1) / (float)(dst_h - 1) : 0.0f;
    const int y0 = MIN((int)sy, valid_h - 1);
    const int y1 = MIN(y0 + 1, valid_h - 1);
    const float fy = sy - (float)y0;

    for(int x = 0; x < dst_w; x++)
    {
      const float sx = (dst_w > 1) ? (float)x * (float)(valid_w - 1) / (float)(dst_w - 1) : 0.0f;
      const int x0 = MIN((int)sx, valid_w - 1);
      const int x1 = MIN(x0 + 1, valid_w - 1);
      const float fx = sx - (float)x0;

      const float v00 = src[y0 * src_w + x0];
      const float v01 = src[y0 * src_w + x1];
      const float v10 = src[y1 * src_w + x0];
      const float v11 = src[y1 * src_w + x1];

      float val = v00 * (1.0f - fx) * (1.0f - fy) + v01 * fx * (1.0f - fy)
        + v10 * (1.0f - fx) * fy + v11 * fx * fy;

      if(apply_sigmoid)
        val = 1.0f / (1.0f + expf(-val));

      dst[y * dst_w + x] = val;
    }
  }
}

/* --- public API --- */

dt_seg_context_t *dt_seg_load(dt_ai_environment_t *env, const char *model_id)
{
  if(!env || !model_id)
    return NULL;

  // use the user's configured provider for the encoder (the heavy part)
  dt_ai_context_t *encoder
    = dt_ai_load_model(env, model_id, "encoder.onnx", DT_AI_PROVIDER_CONFIGURED);
  if(!encoder)
  {
    dt_print(DT_DEBUG_AI, "[segmentation] failed to load encoder for %s", model_id);
    return NULL;
  }

  // force CPU for the decoder -- it's lightweight and hardware acceleration
  // adds more overhead than it saves -- also avoids ORT graph optimization
  // issues with some decoder graphs (e.g. SegNext's Concat->Reshape)
  dt_ai_context_t *decoder
    = dt_ai_load_model_ext(env, model_id, "decoder.onnx", DT_AI_PROVIDER_CPU,
                           DT_AI_OPT_DISABLED, NULL, 0, 0);
  if(!decoder)
  {
    dt_print(DT_DEBUG_AI, "[segmentation] failed to load decoder for %s", model_id);
    dt_ai_unload_model(encoder);
    return NULL;
  }

  // check model version compatibility
  const char *version = dt_ai_model_get_version(model_id);
  const char *min_ver = dt_ai_model_get_min_version(model_id);
  if(min_ver)
  {
    int ver_major = 0, min_major = 0;
    sscanf(version, "%d", &ver_major);
    sscanf(min_ver, "%d", &min_major);
    if(ver_major < min_major)
    {
      dt_print(DT_DEBUG_ALWAYS,
               "[segmentation] model %s v%s incompatible "
               "(requires v%s) - please update model",
               model_id, version, min_ver);
      dt_ai_unload_model(encoder);
      dt_ai_unload_model(decoder);
      return NULL;
    }
  }

  dt_seg_context_t *ctx = g_new0(dt_seg_context_t, 1);
  ctx->encoder = encoder;
  ctx->decoder = decoder;
  ctx->model_id = g_strdup(model_id);
  ctx->model_version = g_strdup(version);

  // query encoder output count and shapes from model metadata
  ctx->n_enc_outputs = dt_ai_get_output_count(encoder);
  if(ctx->n_enc_outputs <= 0 || ctx->n_enc_outputs > MAX_ENCODER_OUTPUTS)
  {
    dt_print(DT_DEBUG_AI,
             "[segmentation] unsupported encoder output count %d for %s",
             ctx->n_enc_outputs, model_id);
    dt_seg_free(ctx);
    return NULL;
  }

  for(int i = 0; i < ctx->n_enc_outputs; i++)
  {
    ctx->enc_ndims[i]
      = dt_ai_get_output_shape(encoder, i, ctx->enc_shapes[i], MAX_TENSOR_DIMS);
    if(ctx->enc_ndims[i] <= 0)
    {
      dt_print(DT_DEBUG_AI,
               "[segmentation] failed to query encoder output %d shape for %s",
               i, model_id);
      dt_seg_free(ctx);
      return NULL;
    }
  }

  // build encoder-to-decoder reorder map by matching output/input names,
  // encoder outputs may be in different order than the decoder expects
  // (e.g. encoder: high_res_feats_0, high_res_feats_1, image_embeddings
  //  vs decoder: image_embed, high_res_feats_0, high_res_feats_1)
  for(int i = 0; i < ctx->n_enc_outputs; i++)
    ctx->enc_order[i] = i; // default: same order

  gboolean used[MAX_ENCODER_OUTPUTS] = {FALSE};
  for(int di = 0; di < ctx->n_enc_outputs; di++)
  {
    const char *dec_name = dt_ai_get_input_name(decoder, di);
    if(!dec_name) continue;

    int best = -1;
    for(int ei = 0; ei < ctx->n_enc_outputs; ei++)
    {
      if(used[ei]) continue;
      const char *enc_name = dt_ai_get_output_name(encoder, ei);
      if(!enc_name) continue;

      if(g_strcmp0(dec_name, enc_name) == 0)
      {
        best = ei;
        break; // exact match
      }
      // substring fallback: e.g. decoder "image_embed" matches encoder
      // "image_embeddings" -- safe because exact matches are tried first
      // and used[] prevents double-assignment of the same encoder output
      if(best < 0 && (strstr(enc_name, dec_name) || strstr(dec_name, enc_name)))
        best = ei;
    }

    if(best >= 0)
    {
      ctx->enc_order[di] = best;
      used[best] = TRUE;
    }
  }

  dt_print(DT_DEBUG_AI,
           "[segmentation] encoder-decoder reorder: [%d, %d, %d, %d] (n=%d)",
           ctx->enc_order[0], ctx->enc_order[1], ctx->enc_order[2], ctx->enc_order[3],
           ctx->n_enc_outputs);

  // detect model type from arch field in model registry
  const dt_ai_model_info_t *minfo
    = dt_ai_get_model_info_by_id(env, model_id);
  const char *arch = minfo ? minfo->arch : "";

  if(strcmp(arch, "sam2") == 0)
    ctx->model_type = DT_SEG_MODEL_SAM;
  else if(strcmp(arch, "segnext") == 0)
    ctx->model_type = DT_SEG_MODEL_SEGNEXT;
  else
  {
    dt_print(DT_DEBUG_AI,
             "[segmentation] unknown arch '%s' for %s",
             arch, model_id);
    dt_seg_free(ctx);
    return NULL;
  }

  // SAM requires external ImageNet normalization; SegNext bakes it into the encoder
  ctx->normalize = (ctx->model_type == DT_SEG_MODEL_SAM);

  // query decoder mask output shape: [1, N, H, W] (SAM) or [1, 1, H, W] (SegNext)
  int64_t dec_out_shape[MAX_TENSOR_DIMS];
  const int dec_out_ndim = dt_ai_get_output_shape(decoder, 0, dec_out_shape, MAX_TENSOR_DIMS);

  if(ctx->model_type == DT_SEG_MODEL_SAM)
  {
    // SAM path
    ctx->num_masks = (dec_out_ndim >= 4 && dec_out_shape[1] > 1) ? (int)dec_out_shape[1] : 0;

    if(ctx->num_masks == 0)
    {
      // fallback: check iou_predictions shape [1, N]
      int64_t iou_shape[MAX_TENSOR_DIMS];
      const int iou_ndim = dt_ai_get_output_shape(decoder, 1, iou_shape, MAX_TENSOR_DIMS);
      ctx->num_masks = (iou_ndim >= 2 && iou_shape[1] > 0) ? (int)iou_shape[1] : 1;
    }

    if(ctx->num_masks > MAX_NUM_MASKS)
    {
      dt_print(DT_DEBUG_AI,
               "[segmentation] clamping num_masks from %d to %d",
               ctx->num_masks, MAX_NUM_MASKS);
      ctx->num_masks = MAX_NUM_MASKS;
    }

    // decoder mask output dimensions must be concrete
    ctx->dec_mask_h = (dec_out_ndim >= 4 && dec_out_shape[2] > 0) ? (int)dec_out_shape[2] : -1;
    ctx->dec_mask_w = (dec_out_ndim >= 4 && dec_out_shape[3] > 0) ? (int)dec_out_shape[3] : -1;

    // if decoder has dynamic output dims (e.g. symbolic "num_labels" dim),
    // reload with num_labels=1 override so ORT can resolve concrete shapes
    if(ctx->dec_mask_h <= 0 || ctx->dec_mask_w <= 0)
    {
      dt_print(DT_DEBUG_AI,
               "[segmentation] decoder has dynamic output dims, reloading with dim overrides");
      dt_ai_unload_model(ctx->decoder);
      const dt_ai_dim_override_t overrides[] = {{"num_labels", 1}};
      ctx->decoder = dt_ai_load_model_ext(env, model_id, "decoder.onnx",
                                           DT_AI_PROVIDER_CPU, DT_AI_OPT_BASIC,
                                           overrides, 1, 0);
      if(!ctx->decoder)
      {
        dt_print(DT_DEBUG_AI, "[segmentation] failed to reload decoder for %s", model_id);
        dt_seg_free(ctx);
        return NULL;
      }
      decoder = ctx->decoder;

      // re-query output shapes with concrete dims
      const int new_ndim
        = dt_ai_get_output_shape(decoder, 0, dec_out_shape, MAX_TENSOR_DIMS);
      ctx->dec_mask_h = (new_ndim >= 4 && dec_out_shape[2] > 0) ? (int)dec_out_shape[2] : -1;
      ctx->dec_mask_w = (new_ndim >= 4 && dec_out_shape[3] > 0) ? (int)dec_out_shape[3] : -1;
      if(new_ndim >= 4 && dec_out_shape[1] > 1)
        ctx->num_masks = MIN((int)dec_out_shape[1], MAX_NUM_MASKS);

      // re-query num_masks from iou output if still unresolved
      if(ctx->num_masks <= 1)
      {
        int64_t iou_shape[MAX_TENSOR_DIMS];
        const int iou_ndim
          = dt_ai_get_output_shape(decoder, 1, iou_shape, MAX_TENSOR_DIMS);
        if(iou_ndim >= 2 && iou_shape[1] > 0)
          ctx->num_masks = MIN((int)iou_shape[1], MAX_NUM_MASKS);
      }

      dt_print(DT_DEBUG_AI,
               "[segmentation] after reload: dec_dims=%dx%d, num_masks=%d",
               ctx->dec_mask_h, ctx->dec_mask_w, ctx->num_masks);
    }

    // if dims are still dynamic after override, fall back to SAM_INPUT_SIZE,
    // the backend uses ORT-allocated outputs for dynamic shapes and reports
    // actual dims after inference via the shape array
    if(ctx->dec_mask_h <= 0 || ctx->dec_mask_w <= 0)
    {
      dt_print(DT_DEBUG_AI,
               "[segmentation] using fallback mask dims %dx%d (runtime-resolved)",
               SAM_INPUT_SIZE, SAM_INPUT_SIZE);
      ctx->dec_mask_h = SAM_INPUT_SIZE;
      ctx->dec_mask_w = SAM_INPUT_SIZE;
    }

    // query low_res mask spatial dimensions from decoder output 2
    ctx->prev_mask_dim = 256; // default
    {
      int64_t lr_shape[MAX_TENSOR_DIMS];
      const int lr_ndim = dt_ai_get_output_shape(decoder, 2, lr_shape, MAX_TENSOR_DIMS);
      if(lr_ndim >= 4 && lr_shape[2] > 0 && lr_shape[3] > 0)
        ctx->prev_mask_dim = (int)lr_shape[2];
    }
  }
  else
  {
    // SegNext path
    // single mask output [1, 1, H, W]
    ctx->num_masks = 1;
    ctx->dec_mask_h = (dec_out_ndim >= 4 && dec_out_shape[2] > 0) ? (int)dec_out_shape[2] : SAM_INPUT_SIZE;
    ctx->dec_mask_w = (dec_out_ndim >= 4 && dec_out_shape[3] > 0) ? (int)dec_out_shape[3] : SAM_INPUT_SIZE;

    // SegNext uses full-resolution prev_mask for iterative refinement
    ctx->prev_mask_dim = ctx->dec_mask_h;
  }

  // allocate prev_mask buffer (used as decoder input for iterative refinement)
  const size_t pm_size = (size_t)ctx->prev_mask_dim * ctx->prev_mask_dim;
  ctx->prev_mask = g_try_malloc0(pm_size * sizeof(float));
  if(!ctx->prev_mask)
  {
    dt_seg_free(ctx);
    return NULL;
  }

  const char *type_name = (ctx->model_type == DT_SEG_MODEL_SAM) ? "SAM" : "SegNext";
  dt_print(DT_DEBUG_AI,
           "[segmentation] model loaded: %s [%s] (enc_outputs=%d, num_masks=%d, "
           "dec_dims=%dx%d, prev_mask_dim=%d)",
           model_id, type_name, ctx->n_enc_outputs, ctx->num_masks,
           ctx->dec_mask_h, ctx->dec_mask_w, ctx->prev_mask_dim);
  return ctx;
}

// ONNX Runtime uses a two-phase initialization model:
//   1. CreateSession() -- parses the ONNX graph and builds internal IR,
//      this is what dt_ai_load_model[_ext]() triggers -- relatively fast
//   2. First Run() -- lazily compiles operator kernels, plans memory arenas,
//      and (on GPU providers) compiles shaders -- this can take seconds
//
// the decoder session is created on a background thread inside
// _encode_thread_func (object.c), but the first Run() would otherwise
// happen on the **main GTK thread** when the user clicks to place a point,
// visibly freezing the UI
//
// this warmup forces phase-2 to happen on the background thread by running
// a single dummy decode -- call it after dt_seg_encode_image() so the real
// encoder embeddings are used -- a warmup with zero-filled dummy data only
// partially warms ORT (kernel compilation) but still leaves a significant
// first-run penalty when real data flows through (memory arena resizing,
// CPU cache population) -- using the actual embeddings fully exercises the
// decoder and eliminates the gap between first and subsequent decodes
//
// the output is discarded and no context state is modified (prev_mask
// stays zeroed, has_prev_mask stays FALSE)
void dt_seg_warmup_decoder(dt_seg_context_t *ctx)
{
  if(!ctx || !ctx->decoder) return;

  dt_print(DT_DEBUG_AI, "[segmentation] warming up decoder...");
  const double t0 = dt_get_wtime();
  const gboolean is_sam = (ctx->model_type == DT_SEG_MODEL_SAM);
  const int pm_dim = ctx->prev_mask_dim;
  const int nm = ctx->num_masks;
  const int dec_h = ctx->dec_mask_h;
  const int dec_w = ctx->dec_mask_w;
  const int total_points = is_sam ? 2 : 1;

  // use real encoder outputs when available (after dt_seg_encode_image),
  // fall back to zero-filled dummies (after dt_seg_load only)
  const gboolean use_real = ctx->image_encoded;

  float *dummy_enc[MAX_ENCODER_OUTPUTS] = {NULL};
  float *masks = NULL;
  float *low_res = NULL;

  if(!use_real)
  {
    for(int i = 0; i < ctx->n_enc_outputs; i++)
    {
      size_t n = 1;
      for(int d = 0; d < ctx->enc_ndims[i]; d++)
        n *= (size_t)ctx->enc_shapes[i][d];
      dummy_enc[i] = g_try_malloc0(n * sizeof(float));
      if(!dummy_enc[i]) goto cleanup;
    }
  }

  masks = g_try_malloc((size_t)nm * dec_h * dec_w * sizeof(float));
  if(!masks) goto cleanup;

  if(is_sam)
  {
    low_res = g_try_malloc((size_t)nm * pm_dim * pm_dim * sizeof(float));
    if(!low_res) goto cleanup;
  }

  // single dummy decode: one foreground point at the origin, no previous mask
  {
    float coords[] = {0.0f, 0.0f, 0.0f, 0.0f};
    float labels[] = {1.0f, -1.0f};
    const float has_mask = 0.0f;

    int64_t coords_shape[3] = {1, total_points, 2};
    int64_t labels_shape[2] = {1, total_points};
    int64_t mask_in_shape[4] = {1, 1, pm_dim, pm_dim};
    int64_t has_mask_shape[1] = {1};

    dt_ai_tensor_t inputs[MAX_ENCODER_OUTPUTS + 4];
    int ni = 0;

    for(int i = 0; i < ctx->n_enc_outputs; i++)
    {
      const int ei = ctx->enc_order[i];
      inputs[ni++] = (dt_ai_tensor_t){
        .data = use_real ? ctx->enc_data[ei] : dummy_enc[ei],
        .type = DT_AI_FLOAT,
        .shape = ctx->enc_shapes[ei], .ndim = ctx->enc_ndims[ei]};
    }

    inputs[ni++] = (dt_ai_tensor_t){
      .data = coords, .type = DT_AI_FLOAT, .shape = coords_shape, .ndim = 3};
    inputs[ni++] = (dt_ai_tensor_t){
      .data = labels, .type = DT_AI_FLOAT, .shape = labels_shape, .ndim = 2};
    inputs[ni++] = (dt_ai_tensor_t){
      .data = ctx->prev_mask, .type = DT_AI_FLOAT, .shape = mask_in_shape, .ndim = 4};

    if(is_sam)
      inputs[ni++] = (dt_ai_tensor_t){
        .data = (void *)&has_mask, .type = DT_AI_FLOAT,
        .shape = has_mask_shape, .ndim = 1};

    int64_t masks_shape[4] = {1, nm, dec_h, dec_w};
    // shapes must outlive outputs[] used by dt_ai_run below
    int64_t iou_shape[2] = {1, nm};
    int64_t lr_shape[4] = {1, nm, pm_dim, pm_dim};
    float iou_buf[MAX_NUM_MASKS];

    dt_ai_tensor_t outputs[3];
    int n_out;

    if(is_sam)
    {
      const int dec_outputs = dt_ai_get_output_count(ctx->decoder);

      outputs[0] = (dt_ai_tensor_t){
        .data = masks, .type = DT_AI_FLOAT, .shape = masks_shape, .ndim = 4};
      outputs[1] = (dt_ai_tensor_t){
        .data = iou_buf, .type = DT_AI_FLOAT, .shape = iou_shape, .ndim = 2};
      n_out = 2;
      // low_res_masks output is optional (absent in 256x256 decoders)
      if(dec_outputs >= 3)
      {
        outputs[2] = (dt_ai_tensor_t){
          .data = low_res, .type = DT_AI_FLOAT, .shape = lr_shape, .ndim = 4};
        n_out = 3;
      }
    }
    else
    {
      outputs[0] = (dt_ai_tensor_t){
        .data = masks, .type = DT_AI_FLOAT, .shape = masks_shape, .ndim = 4};
      n_out = 1;
    }

    dt_ai_run(ctx->decoder, inputs, ni, outputs, n_out);
  }

cleanup:
  g_free(low_res);
  g_free(masks);
  if(!use_real)
    for(int i = 0; i < ctx->n_enc_outputs; i++)
      g_free(dummy_enc[i]);

  dt_print(DT_DEBUG_AI, "[segmentation] decoder warmup done in %.3fs%s",
           dt_get_wtime() - t0, use_real ? " (real embeddings)" : " (dummy data)");
}

gboolean
dt_seg_encode_image(dt_seg_context_t *ctx,
                    const uint8_t *rgb_data,
                    const int width,
                    const int height)
{
  if(!ctx || !rgb_data || width <= 0 || height <= 0)
    return FALSE;

  // skip if already encoded for this image
  if(ctx->image_encoded)
    return TRUE;

  float scale;
  float *preprocessed = _preprocess_image(rgb_data, width, height, ctx->normalize, &scale);
  if(!preprocessed)
    return FALSE;

  // run encoder
  int64_t input_shape[4] = {1, 3, SAM_INPUT_SIZE, SAM_INPUT_SIZE};
  dt_ai_tensor_t input
    = { .data = preprocessed,
       .type  = DT_AI_FLOAT,
       .shape = input_shape,
       .ndim  = 4};

  // allocate output buffers for encoder outputs.
  // when shapes have dynamic dims (value <= 0), pass NULL data so
  // dt_ai_run lets ORT allocate and copies back the results
  float *enc_bufs[MAX_ENCODER_OUTPUTS] = {NULL};
  gboolean has_dynamic = FALSE;

  for(int i = 0; i < ctx->n_enc_outputs; i++)
  {
    size_t sz = 1;
    gboolean dynamic = FALSE;
    for(int d = 0; d < ctx->enc_ndims[i]; d++)
    {
      if(ctx->enc_shapes[i][d] <= 0)
      {
        dynamic = TRUE;
        has_dynamic = TRUE;
        break;
      }
      sz *= (size_t)ctx->enc_shapes[i][d];
    }
    if(!dynamic)
    {
      enc_bufs[i] = g_try_malloc(sz * sizeof(float));
      if(!enc_bufs[i])
      {
        for(int j = 0; j < i; j++) g_free(enc_bufs[j]);
        g_free(preprocessed);
        return FALSE;
      }
    }
  }

  if(has_dynamic)
    dt_print(DT_DEBUG_AI,
             "[segmentation] encoder has dynamic output shapes, "
             "using ORT-allocated outputs");

  dt_ai_tensor_t outputs[MAX_ENCODER_OUTPUTS];
  for(int i = 0; i < ctx->n_enc_outputs; i++)
  {
    outputs[i] = (dt_ai_tensor_t){
      .data  = enc_bufs[i], // NULL for dynamic outputs
      .type  = DT_AI_FLOAT,
      .shape = ctx->enc_shapes[i],
      .ndim  = ctx->enc_ndims[i]};
  }

  dt_print(DT_DEBUG_AI,
          "[segmentation] encoding image %dx%d (scale=%.3f)...",
          width, height, scale);

  const double enc_start = dt_get_wtime();
  const int ret = dt_ai_run(ctx->encoder, &input, 1, outputs, ctx->n_enc_outputs);
  const double enc_elapsed = dt_get_wtime() - enc_start;
  g_free(preprocessed);

  if(ret != 0)
  {
    dt_print(DT_DEBUG_AI, "[segmentation] encoder failed: %d (%.1fs)", ret, enc_elapsed);
    for(int i = 0; i < ctx->n_enc_outputs; i++)
    {
      if(outputs[i].data != enc_bufs[i])
        g_free(outputs[i].data);
      g_free(enc_bufs[i]);
    }
    return FALSE;
  }

  // cache results - use data from outputs[] which may have been
  // allocated by the backend for dynamic-shape outputs
  for(int i = 0; i < MAX_ENCODER_OUTPUTS; i++)
  {
    g_free(ctx->enc_data[i]);
    ctx->enc_data[i] = NULL;
    ctx->enc_sizes[i] = 0;
  }
  for(int i = 0; i < ctx->n_enc_outputs; i++)
  {
    if(outputs[i].data != enc_bufs[i])
      g_free(enc_bufs[i]);
    ctx->enc_data[i] = (float *)outputs[i].data;
    // recompute size from actual resolved shape
    size_t sz = 1;
    for(int d = 0; d < outputs[i].ndim; d++)
      sz *= (size_t)outputs[i].shape[d];
    ctx->enc_sizes[i] = sz;
    // update stored shapes with actual values
    ctx->enc_ndims[i] = outputs[i].ndim;
    memcpy(ctx->enc_shapes[i], outputs[i].shape,
           outputs[i].ndim * sizeof(int64_t));
  }

  ctx->encoded_width = width;
  ctx->encoded_height = height;
  ctx->scale = scale;
  ctx->image_encoded = TRUE;
  ctx->has_prev_mask = FALSE;

  dt_print(DT_DEBUG_AI, "[segmentation] image encoded successfully (%.3fs)", enc_elapsed);
  return TRUE;
}

float *dt_seg_compute_mask(dt_seg_context_t *ctx,
                           const dt_seg_point_t *points,
                           const int n_points,
                           int *out_width,
                           int *out_height)
{
  if(!ctx || !ctx->image_encoded || !points || n_points <= 0)
    return NULL;

  const gboolean is_sam = (ctx->model_type == DT_SEG_MODEL_SAM);

  // build point prompts
  // SAM ONNX requires a padding point (0,0) with label -1 appended
  // to every prompt (see SAM official onnx_model_example.ipynb),
  // SegNext does not need a padding point
  const int total_points = is_sam ? n_points + 1 : n_points;
  float *point_coords = g_new(float, total_points * 2);
  float *point_labels = g_new(float, total_points);

  for(int i = 0; i < n_points; i++)
  {
    point_coords[i * 2 + 0] = points[i].x * ctx->scale;
    point_coords[i * 2 + 1] = points[i].y * ctx->scale;
    point_labels[i] = (float)points[i].label;
  }
  if(is_sam)
  {
    // ONNX padding point
    point_coords[n_points * 2 + 0] = 0.0f;
    point_coords[n_points * 2 + 1] = 0.0f;
    point_labels[n_points] = -1.0f;
  }

  const int pm_dim = ctx->prev_mask_dim;

  // debug: log mask feedback state
  if(ctx->has_prev_mask)
  {
    float pm_min = ctx->prev_mask[0], pm_max = ctx->prev_mask[0];
    for(int k = 1; k < pm_dim * pm_dim; k++)
    {
      if(ctx->prev_mask[k] < pm_min) pm_min = ctx->prev_mask[k];
      if(ctx->prev_mask[k] > pm_max) pm_max = ctx->prev_mask[k];
    }
    dt_print(DT_DEBUG_AI,
             "[segmentation] has_prev_mask=1, prev_mask range=[%.3f, %.3f], n_points=%d",
             pm_min, pm_max, n_points);
  }
  else
  {
    dt_print(DT_DEBUG_AI,
             "[segmentation] has_prev_mask=0 (no previous mask), n_points=%d",
             n_points);
  }

  // build decoder inputs: encoder outputs first, then prompt tensors
  int64_t coords_shape[3] = {1, total_points, 2};
  int64_t labels_shape[2] = {1, total_points};
  int64_t mask_in_shape[4] = {1, 1, pm_dim, pm_dim};

  dt_ai_tensor_t inputs[MAX_ENCODER_OUTPUTS + 4];
  int ni = 0;

  // encoder outputs (reordered to match decoder input order)
  for(int i = 0; i < ctx->n_enc_outputs; i++)
  {
    const int ei = ctx->enc_order[i];
    inputs[ni++] = (dt_ai_tensor_t){
      .data = ctx->enc_data[ei], .type = DT_AI_FLOAT,
      .shape = ctx->enc_shapes[ei], .ndim = ctx->enc_ndims[ei]};
  }

  // prompt inputs (shared: coords, labels, prev_mask)
  inputs[ni++] = (dt_ai_tensor_t){
    .data = point_coords, .type = DT_AI_FLOAT, .shape = coords_shape, .ndim = 3};
  inputs[ni++] = (dt_ai_tensor_t){
    .data = point_labels, .type = DT_AI_FLOAT, .shape = labels_shape, .ndim = 2};
  inputs[ni++] = (dt_ai_tensor_t){
    .data = ctx->prev_mask, .type = DT_AI_FLOAT, .shape = mask_in_shape, .ndim = 4};

  // SAM additionally needs has_mask_input scalar
  const float has_mask = ctx->has_prev_mask ? 1.0f : 0.0f;
  int64_t has_mask_shape[1] = {1};
  if(is_sam)
    inputs[ni++] = (dt_ai_tensor_t){
      .data = (void *)&has_mask, .type = DT_AI_FLOAT, .shape = has_mask_shape, .ndim = 1};

  // --- Decoder outputs ---
  const int nm = ctx->num_masks;
  int dec_h = ctx->dec_mask_h;
  int dec_w = ctx->dec_mask_w;
  size_t per_mask = (size_t)dec_h * dec_w;

  float *masks = g_try_malloc((size_t)nm * per_mask * sizeof(float));
  if(!masks)
  {
    g_free(point_coords);
    g_free(point_labels);
    return NULL;
  }

  dt_ai_tensor_t dec_outputs[3];
  int n_dec_out;
  int64_t masks_shape[4] = {1, nm, dec_h, dec_w};
  // shapes must outlive dec_outputs[] used by dt_ai_run below
  int64_t iou_shape[2] = {1, nm};
  int64_t low_res_shape[4] = {1, nm, pm_dim, pm_dim};

  float iou_pred[MAX_NUM_MASKS];
  float *low_res = NULL;

  if(is_sam)
  {
    // SAM: masks [1,N,H,W] + iou [1,N], optionally low_res [1,N,pm,pm]
    const int dec_out_count = dt_ai_get_output_count(ctx->decoder);

    dec_outputs[0] = (dt_ai_tensor_t){
      .data = masks, .type = DT_AI_FLOAT, .shape = masks_shape, .ndim = 4};
    dec_outputs[1] = (dt_ai_tensor_t){
      .data = iou_pred, .type = DT_AI_FLOAT, .shape = iou_shape, .ndim = 2};
    n_dec_out = 2;

    // low_res_masks output is optional (absent in 256x256 decoders)
    if(dec_out_count >= 3)
    {
      const size_t low_res_per = (size_t)pm_dim * pm_dim;
      low_res = g_try_malloc((size_t)nm * low_res_per * sizeof(float));
      if(!low_res)
      {
        g_free(point_coords);
        g_free(point_labels);
        g_free(masks);
        return NULL;
      }
      dec_outputs[2] = (dt_ai_tensor_t){
        .data = low_res, .type = DT_AI_FLOAT, .shape = low_res_shape, .ndim = 4};
      n_dec_out = 3;
    }
  }
  else
  {
    // SegNext: 1 output -- mask [1, 1, H, W]
    dec_outputs[0] = (dt_ai_tensor_t){
      .data = masks, .type = DT_AI_FLOAT, .shape = masks_shape, .ndim = 4};
    n_dec_out = 1;
  }

  const double dec_start = dt_get_wtime();
  const int ret = dt_ai_run(ctx->decoder, inputs, ni, dec_outputs, n_dec_out);
  const double dec_elapsed = dt_get_wtime() - dec_start;

  g_free(point_coords);
  g_free(point_labels);

  if(ret != 0)
  {
    dt_print(DT_DEBUG_AI, "[segmentation] decoder failed: %d (%.3fs)", ret, dec_elapsed);
    g_free(low_res);
    g_free(masks);
    return NULL;
  }

  // re-read actual mask dimensions -- the backend updates the shape array
  // for dynamic-output models after ORT reports the real tensor shape
  if(masks_shape[2] > 0 && masks_shape[3] > 0
     && ((int)masks_shape[2] != dec_h || (int)masks_shape[3] != dec_w))
  {
    dt_print(DT_DEBUG_AI,
             "[segmentation] actual decoder output: %"PRId64"x%"PRId64" (allocated %dx%d)",
             masks_shape[2], masks_shape[3], dec_h, dec_w);
    dec_h = (int)masks_shape[2];
    dec_w = (int)masks_shape[3];
    per_mask = (size_t)dec_h * dec_w;
  }

  // select best mask and cache refinement data
  int best = 0;
  if(is_sam)
  {
    // SAM: select the mask with the highest predicted IoU
    for(int m = 1; m < nm; m++)
    {
      if(iou_pred[m] > iou_pred[best])
        best = m;
    }
    dt_print(DT_DEBUG_AI,
             "[segmentation] mask computed (%.3fs), best=%d/%d IoU=%.3f",
             dec_elapsed, best, nm, iou_pred[best]);

    // cache the best mask for iterative refinement
    if(low_res)
    {
      // use dedicated low_res output (1024x1024 decoder)
      const size_t low_res_per = (size_t)pm_dim * pm_dim;
      memcpy(ctx->prev_mask, low_res + (size_t)best * low_res_per,
             low_res_per * sizeof(float));
      g_free(low_res);
    }
    else
    {
      // masks output is already at prev_mask resolution (256x256 decoder)
      memcpy(ctx->prev_mask, masks + (size_t)best * per_mask,
             per_mask * sizeof(float));
    }
  }
  else
  {
    // SegNext: single mask -- cache full-res output as prev_mask for refinement
    dt_print(DT_DEBUG_AI,
             "[segmentation] mask computed (%.3fs)", dec_elapsed);
    memcpy(ctx->prev_mask, masks, per_mask * sizeof(float));
  }
  ctx->has_prev_mask = TRUE;

  // crop+resize from decoder resolution to encoded image dimensions
  const int final_w = ctx->encoded_width;
  const int final_h = ctx->encoded_height;
  const size_t result_size = (size_t)final_w * final_h;
  float *result = g_try_malloc(result_size * sizeof(float));
  if(!result)
  {
    g_free(masks);
    return NULL;
  }

  const float mask_scale = ctx->scale * (float)dec_h / (float)SAM_INPUT_SIZE;
  _crop_resize_mask(masks + (size_t)best * per_mask,
                    dec_w, dec_h,
                    result, final_w, final_h,
                    mask_scale, is_sam);
  // SegNext decoder already outputs sigmoid probabilities; SAM outputs logits
  dt_print(DT_DEBUG_AI,
           "[segmentation] resized mask (%dx%d -> %dx%d, scale=%.4f)",
           dec_w, dec_h, final_w, final_h, mask_scale);

  g_free(masks);

  if(out_width)
    *out_width = final_w;
  if(out_height)
    *out_height = final_h;

  return result;
}

gboolean dt_seg_is_encoded(dt_seg_context_t *ctx)
{
  return ctx ? ctx->image_encoded : FALSE;
}

gboolean dt_seg_supports_box(dt_seg_context_t *ctx)
{
  return ctx ? (ctx->model_type == DT_SEG_MODEL_SAM) : FALSE;
}

void dt_seg_reset_prev_mask(dt_seg_context_t *ctx)
{
  if(!ctx)
    return;
  ctx->has_prev_mask = FALSE;
  if(ctx->prev_mask)
    memset(ctx->prev_mask, 0,
           (size_t)ctx->prev_mask_dim * ctx->prev_mask_dim * sizeof(float));
}

void dt_seg_reset_encoding(dt_seg_context_t *ctx)
{
  if(!ctx)
    return;

  for(int i = 0; i < MAX_ENCODER_OUTPUTS; i++)
  {
    g_free(ctx->enc_data[i]);
    ctx->enc_data[i] = NULL;
    ctx->enc_sizes[i] = 0;
  }

  ctx->encoded_width = 0;
  ctx->encoded_height = 0;
  ctx->scale = 0.0f;
  ctx->image_encoded = FALSE;
  ctx->has_prev_mask = FALSE;
  if(ctx->prev_mask)
    memset(ctx->prev_mask, 0,
           (size_t)ctx->prev_mask_dim * ctx->prev_mask_dim * sizeof(float));
}

/* --- disk cache for encoder embeddings --- */

// file format: magic + version + metadata + encoder outputs + RGB
#define SEG_CACHE_MAGIC 0x44545347  // "DTSG"
#define SEG_CACHE_VERSION 1
#define SEG_CACHE_SUBDIR "objmasks"

// build the per-database cache directory path.
// returns FALSE when disk caching is unavailable
// (no database or in-memory mode)
static gboolean _get_cache_dir(char *out, size_t size)
{
  out[0] = '\0';

  // skip disk cache when running with --library :memory:
  const gchar *dbpath = darktable.db
    ? dt_database_get_path(darktable.db)
    : NULL;

  if(dbpath && strcmp(dbpath, ":memory:") != 0)
  {
    char cachedir[PATH_MAX] = {0};
    dt_loc_get_user_cache_dir(cachedir, sizeof(cachedir));

    // hash the database path so different --configdir instances
    // get separate cache directories (same pattern as mipmaps)
    gchar *abspath = g_realpath(dbpath);
    if(abspath == NULL) abspath = g_strdup(dbpath);
    GChecksum *chk = g_checksum_new(G_CHECKSUM_SHA1);
    g_checksum_update(chk, (guchar *)abspath, strlen(abspath));
    const gchar *hash = g_checksum_get_string(chk);

    snprintf(out, size, "%s/%s-%s.d", cachedir, SEG_CACHE_SUBDIR, hash);

    g_checksum_free(chk);
    g_free(abspath);
    return TRUE;
  }

  return FALSE;
}

gboolean dt_seg_disk_cache_save(dt_seg_context_t *ctx,
                                const dt_imgid_t imgid,
                                const dt_hash_t distort_hash,
                                const uint8_t *rgb,
                                const int rgb_w,
                                const int rgb_h)
{
  if(!ctx || !ctx->image_encoded)
    return FALSE;

  char dir[PATH_MAX] = {0};
  if(!_get_cache_dir(dir, sizeof(dir)))
    return FALSE;
  g_mkdir_with_parents(dir, 0755);

  char path[PATH_MAX] = {0};
  snprintf(path, sizeof(path), "%s/%d.seg", dir, imgid);

  FILE *fp = g_fopen(path, "wb");
  if(!fp)
  {
    dt_print(DT_DEBUG_AI,
             "[segmentation] disk cache: cannot open %s for writing",
             path);
    return FALSE;
  }

  gboolean ok = TRUE;
  const uint32_t magic = SEG_CACHE_MAGIC;
  const uint32_t version = SEG_CACHE_VERSION;
  const int32_t n_out = ctx->n_enc_outputs;
  // model id and version (length-prefixed strings)
  const uint32_t mid_len = ctx->model_id ? (uint32_t)strlen(ctx->model_id) : 0;
  const uint32_t mver_len = ctx->model_version
    ? (uint32_t)strlen(ctx->model_version) : 0;

  // header
  ok = ok && fwrite(&magic, 4, 1, fp) == 1;
  ok = ok && fwrite(&version, 4, 1, fp) == 1;
  ok = ok && fwrite(&imgid, 4, 1, fp) == 1;
  ok = ok && fwrite(&distort_hash, 8, 1, fp) == 1;
  ok = ok && fwrite(&mid_len, 4, 1, fp) == 1;
  if(mid_len > 0)
    ok = ok && fwrite(ctx->model_id, 1, mid_len, fp) == mid_len;
  ok = ok && fwrite(&mver_len, 4, 1, fp) == 1;
  if(mver_len > 0)
    ok = ok && fwrite(ctx->model_version, 1, mver_len, fp) == mver_len;
  ok = ok && fwrite(&ctx->encoded_width, 4, 1, fp) == 1;
  ok = ok && fwrite(&ctx->encoded_height, 4, 1, fp) == 1;
  ok = ok && fwrite(&ctx->scale, 4, 1, fp) == 1;
  ok = ok && fwrite(&n_out, 4, 1, fp) == 1;

  // encoder outputs
  for(int i = 0; i < n_out && ok; i++)
  {
    ok = ok && fwrite(&ctx->enc_ndims[i], 4, 1, fp) == 1;
    ok = ok && fwrite(ctx->enc_shapes[i],
                      sizeof(int64_t), ctx->enc_ndims[i], fp)
                 == (size_t)ctx->enc_ndims[i];
    const uint64_t data_sz = ctx->enc_sizes[i];
    ok = ok && fwrite(&data_sz, 8, 1, fp) == 1;
    if(data_sz > 0 && ctx->enc_data[i])
      ok = ok && fwrite(ctx->enc_data[i],
                        sizeof(float), data_sz, fp) == data_sz;
  }

  // RGB footer
  const int32_t rw = rgb ? rgb_w : 0;
  const int32_t rh = rgb ? rgb_h : 0;
  ok = ok && fwrite(&rw, 4, 1, fp) == 1;
  ok = ok && fwrite(&rh, 4, 1, fp) == 1;
  if(rw > 0 && rh > 0 && rgb)
  {
    const size_t rgb_sz = (size_t)rw * rh * 3;
    ok = ok && fwrite(rgb, 1, rgb_sz, fp) == rgb_sz;
  }

  fclose(fp);

  if(!ok)
  {
    g_unlink(path);
    dt_print(DT_DEBUG_AI,
             "[segmentation] disk cache: write error for imgid %d",
             imgid);
    return FALSE;
  }

  dt_print(DT_DEBUG_AI,
           "[segmentation] disk cache: saved imgid %d (%dx%d)",
           imgid, ctx->encoded_width, ctx->encoded_height);

  return TRUE;
}

gboolean dt_seg_disk_cache_load(dt_seg_context_t *ctx,
                                const dt_imgid_t imgid,
                                const dt_hash_t distort_hash,
                                uint8_t **out_rgb,
                                int *out_rgb_w,
                                int *out_rgb_h)
{
  if(!ctx) return FALSE;

  char dir[PATH_MAX] = {0};
  if(!_get_cache_dir(dir, sizeof(dir)))
    return FALSE;

  char path[PATH_MAX] = {0};
  snprintf(path, sizeof(path), "%s/%d.seg", dir, imgid);

  FILE *fp = g_fopen(path, "rb");
  if(!fp) return FALSE;

  gboolean ok = TRUE;
  uint32_t magic = 0, version = 0;
  dt_hash_t file_distort_hash = 0;
  int32_t file_imgid = 0, enc_w = 0, enc_h = 0, n_out = 0;
  float scale = 0.0f;

  // read header
  ok = ok && fread(&magic, 4, 1, fp) == 1;
  ok = ok && fread(&version, 4, 1, fp) == 1;
  ok = ok && fread(&file_imgid, 4, 1, fp) == 1;
  ok = ok && fread(&file_distort_hash, 8, 1, fp) == 1;
  // read model id
  uint32_t mid_len = 0;
  ok = ok && fread(&mid_len, 4, 1, fp) == 1;
  char file_model_id[256] = {0};
  if(ok && mid_len > 0 && mid_len < sizeof(file_model_id))
    ok = ok && fread(file_model_id, 1, mid_len, fp) == mid_len;
  else if(mid_len >= sizeof(file_model_id))
    ok = FALSE;
  // read model version
  uint32_t mver_len = 0;
  ok = ok && fread(&mver_len, 4, 1, fp) == 1;
  char file_model_ver[64] = {0};
  if(ok && mver_len > 0 && mver_len < sizeof(file_model_ver))
    ok = ok && fread(file_model_ver, 1, mver_len, fp) == mver_len;
  else if(mver_len >= sizeof(file_model_ver))
    ok = FALSE;
  ok = ok && fread(&enc_w, 4, 1, fp) == 1;
  ok = ok && fread(&enc_h, 4, 1, fp) == 1;
  ok = ok && fread(&scale, 4, 1, fp) == 1;
  ok = ok && fread(&n_out, 4, 1, fp) == 1;

  const char *cur_ver = ctx->model_version ? ctx->model_version : "0.0";
  if(!ok || magic != SEG_CACHE_MAGIC
     || version != SEG_CACHE_VERSION
     || file_imgid != imgid
     || file_distort_hash != distort_hash
     || !ctx->model_id
     || strcmp(file_model_id, ctx->model_id) != 0
     || strcmp(file_model_ver, cur_ver) != 0)
  {
    fclose(fp);
    return FALSE;
  }

  // validate output count matches loaded model
  if(n_out != ctx->n_enc_outputs || n_out <= 0
     || n_out > MAX_ENCODER_OUTPUTS)
  {
    fclose(fp);
    dt_print(DT_DEBUG_AI,
             "[segmentation] disk cache: output count mismatch "
             "(file=%d, model=%d)",
             n_out, ctx->n_enc_outputs);
    return FALSE;
  }

  // read encoder outputs into temporary buffers
  float *tmp_data[MAX_ENCODER_OUTPUTS] = {NULL};
  size_t tmp_sizes[MAX_ENCODER_OUTPUTS] = {0};
  int tmp_ndims[MAX_ENCODER_OUTPUTS] = {0};
  int64_t tmp_shapes[MAX_ENCODER_OUTPUTS][MAX_TENSOR_DIMS];
  memset(tmp_shapes, 0, sizeof(tmp_shapes));

  for(int i = 0; i < n_out && ok; i++)
  {
    ok = ok && fread(&tmp_ndims[i], 4, 1, fp) == 1;
    if(!ok || tmp_ndims[i] <= 0
       || tmp_ndims[i] > MAX_TENSOR_DIMS)
    {
      ok = FALSE;
      break;
    }
    ok = ok && fread(tmp_shapes[i], sizeof(int64_t),
                     tmp_ndims[i], fp) == (size_t)tmp_ndims[i];

    uint64_t data_sz = 0;
    ok = ok && fread(&data_sz, 8, 1, fp) == 1;

    // validate shapes match the model
    if(ok && tmp_ndims[i] != ctx->enc_ndims[i])
    {
      ok = FALSE;
      dt_print(DT_DEBUG_AI,
               "[segmentation] disk cache: ndim mismatch for "
               "output %d (file=%d, model=%d)",
               i, tmp_ndims[i], ctx->enc_ndims[i]);
      break;
    }
    for(int d = 0; d < tmp_ndims[i] && ok; d++)
    {
      // skip validation for dynamic dims - the model may report
      // dynamic shapes at load time that resolve only after inference
      if(ctx->enc_shapes[i][d] <= 0 || tmp_shapes[i][d] <= 0)
        continue;
      if(tmp_shapes[i][d] != ctx->enc_shapes[i][d])
      {
        ok = FALSE;
        dt_print(DT_DEBUG_AI,
                 "[segmentation] disk cache: shape mismatch for "
                 "output %d dim %d (file=%" PRId64 ", model=%" PRId64 ")",
                 i, d, tmp_shapes[i][d], ctx->enc_shapes[i][d]);
      }
    }

    if(ok && data_sz > 0)
    {
      tmp_data[i] = g_try_malloc(data_sz * sizeof(float));
      if(!tmp_data[i])
      {
        ok = FALSE;
        break;
      }
      tmp_sizes[i] = (size_t)data_sz;
      ok = ok && fread(tmp_data[i], sizeof(float),
                       data_sz, fp) == data_sz;
    }
  }

  // read RGB footer
  int32_t rw = 0, rh = 0;
  uint8_t *rgb = NULL;
  if(ok)
  {
    ok = ok && fread(&rw, 4, 1, fp) == 1;
    ok = ok && fread(&rh, 4, 1, fp) == 1;
    if(ok && rw > 0 && rh > 0)
    {
      const size_t rgb_sz = (size_t)rw * rh * 3;
      rgb = g_try_malloc(rgb_sz);
      if(rgb)
        ok = ok && fread(rgb, 1, rgb_sz, fp) == rgb_sz;
      else
        ok = FALSE;
    }
  }

  fclose(fp);

  if(!ok)
  {
    for(int i = 0; i < n_out; i++) g_free(tmp_data[i]);
    g_free(rgb);
    dt_print(DT_DEBUG_AI,
             "[segmentation] disk cache: read error for imgid %d",
             imgid);
    return FALSE;
  }

  // success -- install into context
  for(int i = 0; i < MAX_ENCODER_OUTPUTS; i++)
  {
    g_free(ctx->enc_data[i]);
    ctx->enc_data[i] = NULL;
    ctx->enc_sizes[i] = 0;
  }
  for(int i = 0; i < n_out; i++)
  {
    ctx->enc_data[i] = tmp_data[i];
    ctx->enc_sizes[i] = tmp_sizes[i];
    // update shapes from cache -- the model may have dynamic dims
    // that are only resolved after inference
    ctx->enc_ndims[i] = tmp_ndims[i];
    memcpy(ctx->enc_shapes[i], tmp_shapes[i],
           tmp_ndims[i] * sizeof(int64_t));
  }
  ctx->encoded_width = enc_w;
  ctx->encoded_height = enc_h;
  ctx->scale = scale;
  ctx->image_encoded = TRUE;
  ctx->has_prev_mask = FALSE;
  if(ctx->prev_mask)
    memset(ctx->prev_mask, 0,
           (size_t)ctx->prev_mask_dim * ctx->prev_mask_dim * sizeof(float));

  if(out_rgb) *out_rgb = rgb; else g_free(rgb);
  if(out_rgb_w) *out_rgb_w = rw;
  if(out_rgb_h) *out_rgb_h = rh;

  dt_print(DT_DEBUG_AI,
           "[segmentation] disk cache: loaded imgid %d (%dx%d, rgb=%dx%d)",
           imgid, enc_w, enc_h, rw, rh);
  return TRUE;
}

const char *dt_seg_get_model_id(const dt_seg_context_t *ctx)
{
  return ctx ? ctx->model_id : NULL;
}

void dt_seg_free(dt_seg_context_t *ctx)
{
  if(!ctx)
    return;

  if(ctx->encoder)
    dt_ai_unload_model(ctx->encoder);
  if(ctx->decoder)
    dt_ai_unload_model(ctx->decoder);
  for(int i = 0; i < MAX_ENCODER_OUTPUTS; i++)
    g_free(ctx->enc_data[i]);
  g_free(ctx->prev_mask);
  g_free(ctx->model_id);
  g_free(ctx->model_version);
  g_free(ctx);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
