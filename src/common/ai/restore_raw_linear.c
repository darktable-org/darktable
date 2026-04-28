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

#include "common/ai/restore_raw_linear.h"
#include "common/ai/restore.h"
#include "common/ai/restore_common.h"
#include "common/darktable.h"
#include "common/image.h"
#include "common/image_cache.h"
#include "common/iop_order.h"
#include "common/math.h"
#include "common/matrices.h"
#include "common/mipmap_cache.h"
#include "control/jobs.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "develop/pixelpipe_hb.h"
#include "imageio/imageio_common.h"

#include <float.h>
#include <glib.h>
#include <math.h>
#include <string.h>

#define OVERLAP_LINEAR 32  // sensor pixels; same scale as input

// derive daylight WB multipliers from the camera's XYZ->CAM matrix:
// at D65 white, the camera response per channel is
//   resp[c] = sum_i M[c][i] * D65[i]
// and wb_norm[c] = resp[G] / resp[c] normalizes green to 1.
// returns TRUE when a usable matrix is available
static gboolean _daylight_wb(const dt_image_t *img, float wb_norm[3])
{
  const float D65[3] = { 0.9504f, 1.0f, 1.0889f };
  float resp[3];
  float mag = 0.0f;
  for(int c = 0; c < 3; c++)
  {
    resp[c] = img->adobe_XYZ_to_CAM[c][0] * D65[0]
            + img->adobe_XYZ_to_CAM[c][1] * D65[1]
            + img->adobe_XYZ_to_CAM[c][2] * D65[2];
    mag += fabsf(img->adobe_XYZ_to_CAM[c][0])
         + fabsf(img->adobe_XYZ_to_CAM[c][1])
         + fabsf(img->adobe_XYZ_to_CAM[c][2]);
  }
  if(mag <= 0.0f || resp[0] <= 0.0f || resp[1] <= 0.0f || resp[2] <= 0.0f)
  {
    wb_norm[0] = wb_norm[1] = wb_norm[2] = 1.0f;
    return FALSE;
  }
  wb_norm[0] = resp[1] / resp[0];
  wb_norm[1] = 1.0f;
  wb_norm[2] = resp[1] / resp[2];
  return TRUE;
}

// build the combined "input-space → camRGB + undo exposure boost + undo
// WB" 3×3 used in the final un-matrix pass. folds three linear ops into
// one per-pixel multiplication for speed. caller provides input_to_cam
// (built by _build_cam_matrices for the ctx's input_colorspace),
// inv_boost (= 1 / exposure_boost) and wb_norm
static void _linear_build_M_boosted(const float input_to_cam[9],
                                    float inv_boost,
                                    const float wb_norm[3],
                                    float M[9])
{
  for(int k = 0; k < 3; k++)
    for(int i = 0; i < 3; i++)
      M[k * 3 + i] = input_to_cam[k * 3 + i] * inv_boost / wb_norm[k];
}

// scalar match_gain: tile_out *= in_mean / out_mean, where both means
// are taken over all 3 channels and all spatial positions. mirrors the
// upstream Python rawproc.match_gain (mean over (-1, -2, -3) dims),
// which the model was trained against. applied in place. out_gain
// optional (batch uses it for a tile0 diagnostic)
static void _linear_gain_match(const float *tile_in,
                               float *tile_out,
                               size_t per_ch,
                               float *out_gain)
{
  const size_t total = per_ch * 3;
  double in_sum = 0.0, out_sum = 0.0;
  for(size_t i = 0; i < total; i++)
  {
    in_sum += tile_in[i];
    out_sum += tile_out[i];
  }
  const double im = in_sum / (double)total;
  const double om = out_sum / (double)total;
  const float g = (fabs(om) > 1e-8) ? (float)(im / om) : 1.0f;
  if(g != 1.0f)
    for(size_t i = 0; i < total; i++) tile_out[i] *= g;
  if(out_gain) *out_gain = g;
}

// derive + apply an exposure boost to a planar 3ch lin_rec2020 buffer.
// RawNIND training data was exposed at editorial brightness (mean ~0.3
// in lin_rec2020); low-light raws land near ~0.02, which is >10× darker
// than the training distribution. the UtNet2 weights diverge on such
// OOD input (observed: model output range ±1e10 with negative mean,
// breaking match_gain). we boost to the training mean pre-inference
// and un-boost at the very end; the multiplication commutes with the
// linear un-matrix and un-WB steps so correctness holds.
// target_mean = NAN disables the boost entirely for models that don't
// need a brightness-normalized input. otherwise boost is capped at
// [1, 100] (never dim bright scenes). returned mean / boost are filled
// for optional diagnostics (boost=1 when disabled)
static void _linear_exposure_boost(const dt_restore_context_t *ctx,
                                   float *rgb_planar,
                                   size_t plane,
                                   float *out_mean,
                                   float *out_boost)
{
  const size_t total = plane * 3;
  double sum = 0.0;
  for(size_t i = 0; i < total; i++) sum += rgb_planar[i];
  const float scene_mean = (float)(sum / (double)total);
  const float target = ctx ? ctx->target_mean : 0.30f;
  float boost = 1.0f;
  if(!isnan(target) && target > 0.0f && scene_mean > 1e-4f)
  {
    boost = target / scene_mean;
    if(boost < 1.0f) boost = 1.0f;
    if(boost > 100.0f) boost = 100.0f;
  }
  if(boost != 1.0f)
    for(size_t i = 0; i < total; i++) rgb_planar[i] *= boost;
  if(out_mean)  *out_mean = scene_mean;
  if(out_boost) *out_boost = boost;
}

// as-shot WB from img->wb_coeffs normalized to G=1
static gboolean _as_shot_wb(const dt_image_t *img, float wb_norm[3])
{
  if(img->wb_coeffs[0] <= 0.0f
     || img->wb_coeffs[1] <= 0.0f
     || img->wb_coeffs[2] <= 0.0f)
    return FALSE;
  const float g = img->wb_coeffs[1];
  wb_norm[0] = img->wb_coeffs[0] / g;
  wb_norm[1] = 1.0f;
  wb_norm[2] = img->wb_coeffs[2] / g;
  return TRUE;
}

// resolve WB for the linear path keyed off ctx->wb_mode. Default for
// this path is AS_SHOT (as-shot beats daylight for re-imported DNGs
// because the denoised output's tonal character then matches the
// source — see the long rationale in dt_restore_raw_linear). Fallback
// order swaps per mode; NONE skips normalization entirely
static void _resolve_linear_wb(const dt_restore_context_t *ctx,
                               const dt_image_t *img, float wb_norm[3])
{
  wb_norm[0] = wb_norm[1] = wb_norm[2] = 1.0f;
  const dt_restore_wb_mode_t mode
    = ctx ? ctx->wb_mode : DT_RESTORE_WB_AS_SHOT;
  if(mode == DT_RESTORE_WB_AS_SHOT)
  {
    if(!_as_shot_wb(img, wb_norm))
      _daylight_wb(img, wb_norm);
  }
  else if(mode == DT_RESTORE_WB_DAYLIGHT)
  {
    if(!_daylight_wb(img, wb_norm))
      _as_shot_wb(img, wb_norm);
  }
  // DT_RESTORE_WB_NONE: leave at {1, 1, 1}
}

// D65 XYZ -> linear Rec.2020 (ITU-R BT.2020), row-major 3x3.
// matches the lin_rec2020 color profile the RawNIND linear variant
// was trained on
static const float _xyz_to_rec2020[9] = {
   1.7166511880f, -0.3556707838f, -0.2533662814f,
  -0.6666843518f,  1.6164812366f,  0.0157685458f,
   0.0176398574f, -0.0427706133f,  0.9421031212f,
};

static const float _rec2020_to_xyz[9] = {
  0.6369580483f, 0.1446169036f, 0.1688809752f,
  0.2627002120f, 0.6779980715f, 0.0593017165f,
  0.0000000000f, 0.0280726930f, 1.0609850577f,
};

// D65 XYZ -> linear sRGB / Rec.709 (IEC 61966-2-1), row-major 3x3.
// used when a variant declares input_colorspace: srgb_linear
static const float _xyz_to_srgb[9] = {
   3.2404542f, -1.5371385f, -0.4985314f,
  -0.9692660f,  1.8760108f,  0.0415560f,
   0.0556434f, -0.2040259f,  1.0572252f,
};

static const float _srgb_to_xyz[9] = {
  0.4124564f, 0.3575761f, 0.1804375f,
  0.2126729f, 0.7151522f, 0.0721750f,
  0.0193339f, 0.1191920f, 0.9503041f,
};

// build the per-image camRGB<->input-space matrices, where input-space
// is chosen by ctx->input_colorspace:
//   LIN_REC2020 (default): xyz_to_rec2020 · inverse(adobe_XYZ_to_CAM)
//   SRGB_LINEAR:           xyz_to_srgb · inverse(adobe_XYZ_to_CAM)
//   CAMRGB:                identity (model runs directly on camRGB)
// returns TRUE when the input-space transform could be built; FALSE
// when the camera's color matrix is absent or singular (CAMRGB always
// succeeds since it skips the matrix entirely). on FALSE the caller
// falls back to identity (color cast but at least no garbage)
static gboolean _build_cam_matrices(const dt_restore_context_t *ctx,
                                    const dt_image_t *img,
                                    float cam_to_input[9],
                                    float input_to_cam[9])
{
  const dt_restore_colorspace_t cs
    = ctx ? ctx->input_colorspace : DT_RESTORE_CS_LIN_REC2020;

  if(cs == DT_RESTORE_CS_CAMRGB)
  {
    for(int i = 0; i < 9; i++)
      cam_to_input[i] = input_to_cam[i] = (i % 4 == 0) ? 1.0f : 0.0f;
    return TRUE;
  }

  float cam_from_xyz[9];
  float mag = 0.0f;
  for(int k = 0; k < 3; k++)
    for(int i = 0; i < 3; i++)
    {
      const float v = img->adobe_XYZ_to_CAM[k][i];
      cam_from_xyz[k * 3 + i] = v;
      mag += fabsf(v);
    }
  if(mag <= 0.0f) return FALSE;

  float xyz_from_cam[9];
  if(mat3inv(xyz_from_cam, cam_from_xyz) != 0)
    return FALSE;

  const float *xyz_to_input = (cs == DT_RESTORE_CS_SRGB_LINEAR)
    ? _xyz_to_srgb : _xyz_to_rec2020;
  const float *input_to_xyz = (cs == DT_RESTORE_CS_SRGB_LINEAR)
    ? _srgb_to_xyz : _rec2020_to_xyz;

  mat3mul(cam_to_input, xyz_to_input, xyz_from_cam);
  mat3mul(input_to_cam, cam_from_xyz, input_to_xyz);
  return TRUE;
}

// run the minimal darktable pixelpipe: rawprepare + highlights +
// demosaic, nothing after, no temperature (so output is raw-native
// camRGB without WB applied). output is a newly-allocated 4ch float
// RGBA buffer at the pipeline's processed_{width,height}; caller frees
// with dt_free_align(). returns 0 on success
static int _run_demosaic_pipe(const dt_imgid_t imgid,
                              float **out_buf,
                              int *out_w,
                              int *out_h)
{
  dt_develop_t dev;
  dt_dev_init(&dev, FALSE);
  dt_dev_load_image(&dev, imgid);

  dt_mipmap_buffer_t mbuf;
  dt_mipmap_cache_get(&mbuf, imgid, DT_MIPMAP_FULL,
                      DT_MIPMAP_BLOCKING, 'r');
  if(!mbuf.buf || !mbuf.width || !mbuf.height)
  {
    dt_print(DT_DEBUG_AI,
             "[restore_raw_linear] could not load raw for imgid %d",
             imgid);
    dt_mipmap_cache_release(&mbuf);
    dt_dev_cleanup(&dev);
    return 1;
  }

  const int iw = mbuf.width;
  const int ih = mbuf.height;

  dt_dev_pixelpipe_t pipe;
  if(!dt_dev_pixelpipe_init_export(&pipe, iw, ih,
                                   IMAGEIO_FLOAT, FALSE))
  {
    dt_print(DT_DEBUG_AI,
             "[restore_raw_linear] pipe init_export failed (%dx%d)", iw, ih);
    dt_mipmap_cache_release(&mbuf);
    dt_dev_cleanup(&dev);
    return 1;
  }

  // the export code sequences this as: resync_modules_order -> set_input
  // -> create_nodes -> synch_all. resync builds the iop-order table
  // from the loaded image's history; without it, create_nodes sees an
  // empty/misaligned list and leaves pipe->nodes NULL, which then
  // crashes dt_dev_pixelpipe_disable_after when it dereferences
  // g_list_last(pipe->nodes)
  dt_ioppr_resync_modules_order(&dev);
  dt_dev_pixelpipe_set_input(&pipe, &dev, (float *)mbuf.buf,
                             iw, ih, mbuf.iscale);
  dt_dev_pixelpipe_create_nodes(&pipe, &dev);
  dt_dev_pixelpipe_synch_all(&pipe, &dev);

  if(!pipe.nodes)
  {
    dt_print(DT_DEBUG_AI,
             "[restore_raw_linear] pipe has no nodes — aborting");
    dt_dev_pixelpipe_cleanup(&pipe);
    dt_mipmap_cache_release(&mbuf);
    dt_dev_cleanup(&dev);
    return 1;
  }

  // keep rawprepare + highlights (clip) + demosaic; skip temperature
  // (we apply our own daylight WB later) and everything after demosaic
  dt_dev_pixelpipe_disable_after(&pipe, "demosaic");
  for(GList *n = pipe.nodes; n; n = g_list_next(n))
  {
    dt_dev_pixelpipe_iop_t *piece = n->data;
    if(dt_iop_module_is(piece->module->so, "temperature")
       || dt_iop_module_is(piece->module->so, "rawdenoise"))
      piece->enabled = FALSE;
  }

  dt_dev_pixelpipe_get_dimensions(&pipe, &dev, iw, ih,
                                  &pipe.processed_width,
                                  &pipe.processed_height);
  const int pw = pipe.processed_width;
  const int ph = pipe.processed_height;

  // process CPU-side at full scale. no_gamma keeps float output
  dt_dev_pixelpipe_process_no_gamma(&pipe, &dev, 0, 0, pw, ph, 1.0f);

  if(!pipe.backbuf || !pipe.backbuf_width || !pipe.backbuf_height)
  {
    dt_dev_pixelpipe_cleanup(&pipe);
    dt_mipmap_cache_release(&mbuf);
    dt_dev_cleanup(&dev);
    dt_print(DT_DEBUG_AI,
             "[restore_raw_linear] pipe produced no backbuffer");
    return 1;
  }

  const int bw = pipe.backbuf_width;
  const int bh = pipe.backbuf_height;
  float *copy = dt_alloc_align_float((size_t)bw * bh * 4);
  if(!copy)
  {
    dt_dev_pixelpipe_cleanup(&pipe);
    dt_mipmap_cache_release(&mbuf);
    dt_dev_cleanup(&dev);
    return 1;
  }

  memcpy(copy, pipe.backbuf, (size_t)bw * bh * 4 * sizeof(float));

  *out_buf = copy;
  *out_w = bw;
  *out_h = bh;

  dt_dev_pixelpipe_cleanup(&pipe);
  dt_mipmap_cache_release(&mbuf);
  dt_dev_cleanup(&dev);
  return 0;
}

int dt_restore_raw_linear(dt_restore_context_t *ctx,
                          const dt_imgid_t imgid,
                          float **out_rgb,
                          int *out_w,
                          int *out_h,
                          float strength,
                          struct _dt_job_t *control_job)
{
  if(!ctx || !out_rgb || !out_w || !out_h) return 1;
  *out_rgb = NULL;

  const float alpha = strength < 0.0f ? 0.0f
                    : (strength > 1.0f ? 1.0f : strength);
  const float inv_alpha = 1.0f - alpha;

  // --- 1. produce demosaicked 4ch RGBA via minimal pipeline ---
  float *rgba = NULL;
  int w = 0, h = 0;
  if(_run_demosaic_pipe(imgid, &rgba, &w, &h)) return 1;

  // snapshot image metadata for WB derivation (plain data members;
  // don't touch heap pointers like profile/dng_gain_maps)
  const dt_image_t *cached = dt_image_cache_get(imgid, 'r');
  if(!cached)
  {
    dt_free_align(rgba);
    return 1;
  }
  dt_image_t img_meta = *cached;
  dt_image_cache_read_release(cached);

  // WB normalization per ctx->wb_mode (default AS_SHOT; see
  // _resolve_linear_wb). AS_SHOT beats DAYLIGHT for this path because
  // match_gain + the negative-gain hack absorb the training-distribution
  // mismatch, so the WB choice mostly shapes the final DNG's tonal look
  // and we want the re-imported DNG to render with the same tone/contrast
  // as the source
  float wb_norm[3];
  _resolve_linear_wb(ctx, &img_meta, wb_norm);

  // feed the model in ctx->input_colorspace (default lin_rec2020,
  // matches RawNIND training preprocessing). identity fallback when
  // the camera's color matrix is absent (rare); CAMRGB always succeeds
  float cam_to_input[9];
  float input_to_cam[9];
  const gboolean matrix_ok =
    _build_cam_matrices(ctx, &img_meta, cam_to_input, input_to_cam);
  if(!matrix_ok)
  {
    for(int i = 0; i < 9; i++)
      cam_to_input[i] = input_to_cam[i] = (i % 4 == 0) ? 1.0f : 0.0f;
  }
  dt_print(DT_DEBUG_AI,
           "[restore_raw_linear] wb_norm=[%.3f,%.3f,%.3f], "
           "colorspace matrix: %s",
           wb_norm[0], wb_norm[1], wb_norm[2],
           matrix_ok ? "cam->input from adobe_XYZ_to_CAM"
                     : "identity (no color matrix)");

  const size_t npix = (size_t)w * h;

  // allocate planar 3ch buffers for tile I/O + the preserved
  // pre-inference source for the strength blend
  float *rgb_src = dt_alloc_align_float(npix * 3);  // planar R,G,B
  if(!rgb_src)
  {
    dt_free_align(rgba);
    return 1;
  }

  // interleaved RGBA -> planar RGB. apply daylight WB first (matches
  // RawNIND training: WB in camRGB space, then camRGB->lin_rec2020),
  // then the matrix transform so the model sees lin_rec2020 directly
  const size_t plane = npix;
  for(size_t i = 0; i < npix; i++)
  {
    const float cam[3] = {
      rgba[i * 4 + 0] * wb_norm[0],
      rgba[i * 4 + 1] * wb_norm[1],
      rgba[i * 4 + 2] * wb_norm[2],
    };
    float input_rgb[3];
    mat3mulv(input_rgb, cam_to_input, cam);
    rgb_src[i]             = input_rgb[0];
    rgb_src[i + plane]     = input_rgb[1];
    rgb_src[i + 2 * plane] = input_rgb[2];
  }

  // diagnostic min/max sweep + exposure boost. sweep is separate from
  // the shared boost helper because only the batch diagnostic needs the
  // per-channel min/max; the helper just computes the mean
  float dbg_min[3], dbg_max[3];
  for(int k = 0; k < 3; k++)
  {
    const float *p = rgb_src + (size_t)k * plane;
    dbg_min[k] = dbg_max[k] = p[0];
    for(size_t i = 0; i < plane; i++)
    {
      if(p[i] < dbg_min[k]) dbg_min[k] = p[i];
      if(p[i] > dbg_max[k]) dbg_max[k] = p[i];
    }
  }
  float scene_mean = 0.0f, exposure_boost = 1.0f;
  _linear_exposure_boost(ctx, rgb_src, plane, &scene_mean, &exposure_boost);
  dt_print(DT_DEBUG_AI,
           "[restore_raw_linear] %dx%d, lin_rec2020 input range "
           "R=[%.3f,%.3f] G=[%.3f,%.3f] B=[%.3f,%.3f] "
           "mean=%.4f boost=%.2fx",
           w, h,
           dbg_min[0], dbg_max[0], dbg_min[1], dbg_max[1],
           dbg_min[2], dbg_max[2],
           scene_mean, exposure_boost);

  // allocate planar output buffer that tiles blend into
  float *rgb_out = dt_alloc_align_float(npix * 3);
  if(!rgb_out)
  {
    dt_free_align(rgb_src);
    dt_free_align(rgba);
    return 1;
  }

  // initialize output with WB'd source so strength = 0 is exact
  // pass-through and tile-edge gaps don't leave uninitialized data
  memcpy(rgb_out, rgb_src, npix * 3 * sizeof(float));

  // tile setup
  const int O = OVERLAP_LINEAR;
  int T = dt_restore_get_tile_size(ctx);
  int n_ladder = 0;
  const int *ladder = dt_restore_get_tile_ladder(ctx, &n_ladder);
  if(T <= 2 * O) T = 256;

retry:;
  const int step = T - 2 * O;
  if(step <= 0)
  {
    dt_free_align(rgb_src);
    dt_free_align(rgb_out);
    dt_free_align(rgba);
    return 1;
  }
  const size_t tile_plane = (size_t)T * T;
  const int cols = (w + step - 1) / step;
  const int rows = (h + step - 1) / step;
  const int total_tiles = cols * rows;

  dt_print(DT_DEBUG_AI,
           "[restore_raw_linear] tile T=%d step=%d, grid %dx%d (%d tiles)",
           T, step, cols, rows, total_tiles);

  float *tile_in = g_try_malloc(tile_plane * 3 * sizeof(float));
  float *tile_out = g_try_malloc(tile_plane * 3 * sizeof(float));
  if(!tile_in || !tile_out)
  {
    g_free(tile_in);
    g_free(tile_out);
    dt_free_align(rgb_src);
    dt_free_align(rgb_out);
    dt_free_align(rgba);
    return 1;
  }

  int res = 0;
  int tile_count = 0;

  // overlap blending — see restore_raw_bayer.c for the scheme.
  // sensor_O = O (1:1 with input), strips are 3-ch planar matching rgb_out
  const int sensor_O = O;
  const int hstrip_h = 2 * sensor_O;
  const size_t hstrip_chan = (size_t)w * hstrip_h;  // floats per channel

  float *h_strip_top = NULL;
  int h_strip_top_sy0 = 0;

  for(int ty = 0; ty < rows && res == 0; ty++)
  {
    const gboolean has_top = ty > 0;
    const gboolean has_bot = ty < rows - 1;

    float *h_strip_bot = NULL;
    int h_strip_bot_sy0 = 0;
    if(has_bot)
    {
      h_strip_bot = g_try_malloc0(hstrip_chan * 3 * sizeof(float));
      if(!h_strip_bot) { res = 1; break; }
    }

    float *v_strip_left = NULL;
    int v_strip_left_sx0 = 0, v_strip_left_sy0 = 0, v_strip_left_h = 0;

    for(int tx = 0; tx < cols && res == 0; tx++)
    {
      if(control_job
         && dt_control_job_get_state(control_job)
              == DT_JOB_STATE_CANCELLED)
      {
        res = 1;
        break;
      }

      const int y_base = ty * step;
      const int x_base = tx * step;
      const int y_end  = (y_base + step > h) ? h : y_base + step;
      const int x_end  = (x_base + step > w) ? w : x_base + step;

      // extract T x T tile with mirror-pad at boundaries, planar
      for(int dy = 0; dy < T; dy++)
      {
        const int sy = _mirror(y_base - O + dy, h);
        for(int dx = 0; dx < T; dx++)
        {
          const int sx = _mirror(x_base - O + dx, w);
          const size_t src = (size_t)sy * w + sx;
          const size_t dst = (size_t)dy * T + dx;
          tile_in[dst]                  = rgb_src[src];
          tile_in[dst + tile_plane]     = rgb_src[src + plane];
          tile_in[dst + 2 * tile_plane] = rgb_src[src + 2 * plane];
        }
      }

      // inference
      if(dt_restore_run_patch_3ch_raw(ctx, tile_in, T, T, tile_out) != 0)
      {
        int next_T = 0;
        for(int i = 0; i < n_ladder; i++)
          if(ladder[i] < T) { next_T = ladder[i]; break; }
        if(next_T > 0 && ty == 0 && tx == 0
           && dt_restore_reload_session(ctx, next_T))
        {
          dt_print(DT_DEBUG_AI,
                   "[restore_raw_linear] inference failed at T=%d, retry T=%d",
                   T, next_T);
          g_free(tile_in);
          g_free(tile_out);
          g_free(h_strip_top);
          g_free(h_strip_bot);
          g_free(v_strip_left);
          T = next_T;
          goto retry;
        }
        dt_print(DT_DEBUG_AI,
                 "[restore_raw_linear] inference failed at tile %d,%d "
                 "(T=%d)", tx, ty, T);
        res = 1;
        break;
      }

      // scalar match_gain: tile_out *= in_mean / out_mean (applied in
      // place by the helper). skipped for ABSOLUTE-scale models whose
      // output is already calibrated
      const size_t per_ch = tile_plane;
      float gain = 1.0f;
      if(ctx->output_scale == DT_RESTORE_OUT_MATCH_GAIN)
        _linear_gain_match(tile_in, tile_out, per_ch, &gain);
      if(tx == 0 && ty == 0)
        dt_print(DT_DEBUG_AI,
                 "[restore_raw_linear] tile0 match_gain=%.3e",
                 (double)gain);

      const gboolean has_left = tx > 0;
      const gboolean has_right = tx < cols - 1;
      const int sensor_py_base = y_base;
      const int sensor_py_end  = y_end;
      const int sensor_px_base = x_base;
      const int sensor_px_end  = x_end;

      if(has_bot && tx == 0) h_strip_bot_sy0 = sensor_py_end - sensor_O;

      float *v_strip_right = NULL;
      int v_strip_right_sx0 = 0, v_strip_right_sy0 = 0, v_strip_right_h = 0;
      if(has_right)
      {
        v_strip_right_sx0 = sensor_px_end - sensor_O;
        v_strip_right_sy0 = sensor_py_base + (has_top ? sensor_O : 0);
        const int v_y_end = sensor_py_end - (has_bot ? sensor_O : 0);
        v_strip_right_h = v_y_end - v_strip_right_sy0;
        if(v_strip_right_h > 0)
        {
          v_strip_right = g_try_malloc0((size_t)(2 * sensor_O)
                                        * v_strip_right_h * 3 * sizeof(float));
          if(!v_strip_right) { res = 1; break; }
        }
      }

      const int ext_y0 = has_top  ? sensor_py_base - sensor_O : sensor_py_base;
      const int ext_y1 = has_bot  ? sensor_py_end + sensor_O  : sensor_py_end;
      const int ext_x0 = has_left ? sensor_px_base - sensor_O : sensor_px_base;
      const int ext_x1 = has_right? sensor_px_end + sensor_O  : sensor_px_end;

      for(int sr = ext_y0; sr < ext_y1; sr++)
      {
        const int my = O + (sr - sensor_py_base);
        const float ay = _seam_ay(sr, sensor_py_base, sensor_py_end,
                                  sensor_O, has_top, has_bot);
        const gboolean in_horiz_seam = (ay < 1.0f);

        float *h_strip = NULL;
        int h_strip_sy0 = 0;
        if(in_horiz_seam)
        {
          if(has_top && sr < sensor_py_base + sensor_O)
          {
            h_strip = h_strip_top;
            h_strip_sy0 = h_strip_top_sy0;
          }
          else if(has_bot && sr >= sensor_py_end - sensor_O)
          {
            h_strip = h_strip_bot;
            h_strip_sy0 = h_strip_bot_sy0;
          }
        }
        const size_t h_strip_row_off = h_strip
          ? (size_t)(sr - h_strip_sy0) * w : 0;

        for(int sc = ext_x0; sc < ext_x1; sc++)
        {
          const int mx = O + (sc - sensor_px_base);
          const float ax = _seam_ax(sc, sensor_px_base, sensor_px_end,
                                    sensor_O, has_left, has_right);
          const gboolean in_vert_seam = (ax < 1.0f);

          const size_t tloc = (size_t)my * T + mx;
          const size_t dst = (size_t)sr * w + sc;

          if(in_horiz_seam)
          {
            if(h_strip)
            {
              const float wgt = ax * ay;
              for(int k = 0; k < 3; k++)
              {
                const float model_v = tile_out[tloc + (size_t)k * per_ch];
                const float src_v   = rgb_src[dst + (size_t)k * plane];
                const float blended = alpha * model_v + inv_alpha * src_v;
                h_strip[h_strip_row_off + sc + (size_t)k * hstrip_chan]
                  += wgt * blended;
              }
            }
          }
          else if(in_vert_seam)
          {
            float *v_strip = NULL;
            int v_sx0 = 0, v_sy0 = 0, v_h = 0;
            if(has_left && sc < sensor_px_base + sensor_O)
            {
              v_strip = v_strip_left;
              v_sx0 = v_strip_left_sx0; v_sy0 = v_strip_left_sy0;
              v_h = v_strip_left_h;
            }
            else if(has_right && sc >= sensor_px_end - sensor_O)
            {
              v_strip = v_strip_right;
              v_sx0 = v_strip_right_sx0; v_sy0 = v_strip_right_sy0;
              v_h = v_strip_right_h;
            }
            if(v_strip)
            {
              const size_t vchan = (size_t)(2 * sensor_O) * v_h;
              const size_t vidx
                = (size_t)(sr - v_sy0) * (2 * sensor_O) + (sc - v_sx0);
              for(int k = 0; k < 3; k++)
              {
                const float model_v = tile_out[tloc + (size_t)k * per_ch];
                const float src_v   = rgb_src[dst + (size_t)k * plane];
                const float blended = alpha * model_v + inv_alpha * src_v;
                v_strip[vidx + (size_t)k * vchan] += ax * blended;
              }
            }
          }
          else
          {
            for(int k = 0; k < 3; k++)
            {
              const float model_v = tile_out[tloc + (size_t)k * per_ch];
              const float src_v   = rgb_src[dst + (size_t)k * plane];
              rgb_out[dst + (size_t)k * plane]
                = alpha * model_v + inv_alpha * src_v;
            }
          }
        }
      }

      // tx-1 + tx ramps sum to 1; flush + free
      if(v_strip_left)
      {
        const size_t vchan = (size_t)(2 * sensor_O) * v_strip_left_h;
        for(int sr = v_strip_left_sy0;
            sr < v_strip_left_sy0 + v_strip_left_h; sr++)
        {
          const size_t vrow = (size_t)(sr - v_strip_left_sy0) * (2 * sensor_O);
          for(int dxs = 0; dxs < 2 * sensor_O; dxs++)
          {
            const int sc = v_strip_left_sx0 + dxs;
            const size_t dst = (size_t)sr * w + sc;
            for(int k = 0; k < 3; k++)
              rgb_out[dst + (size_t)k * plane]
                = v_strip_left[vrow + dxs + (size_t)k * vchan];
          }
        }
        g_free(v_strip_left);
      }
      v_strip_left = v_strip_right;
      v_strip_left_sx0 = v_strip_right_sx0;
      v_strip_left_sy0 = v_strip_right_sy0;
      v_strip_left_h   = v_strip_right_h;

      tile_count++;
      if(control_job)
        dt_control_job_set_progress(control_job,
                                    (double)tile_count / total_tiles);
    }

    g_free(v_strip_left);
    v_strip_left = NULL;

    // ramps sum to 1, flush. no column clamp needed (no working-region offset)
    if(h_strip_top)
    {
      for(int sr = h_strip_top_sy0; sr < h_strip_top_sy0 + hstrip_h; sr++)
      {
        const size_t hrow = (size_t)(sr - h_strip_top_sy0) * w;
        for(int sc = 0; sc < w; sc++)
        {
          const size_t dst = (size_t)sr * w + sc;
          for(int k = 0; k < 3; k++)
            rgb_out[dst + (size_t)k * plane]
              = h_strip_top[hrow + sc + (size_t)k * hstrip_chan];
        }
      }
      g_free(h_strip_top);
    }
    h_strip_top = h_strip_bot;
    h_strip_top_sy0 = h_strip_bot_sy0;
  }

  g_free(h_strip_top);

  g_free(tile_in);
  g_free(tile_out);

  if(res == 0)
  {
    // final undo pass: input-space -> camRGB (matrix), divide by
    // exposure boost, divide by WB. the DNG writer expects un-WB'd
    // normalized camRGB in [0, 1] — AsShotNeutral tells the consumer
    // what WB to apply
    // out = (input_to_cam · in) / (boost · wb_norm[k])
    // all ops are linear, fold into a single per-pixel 3x3 mul
    const float inv_boost = 1.0f / exposure_boost;
    float M[9];
    _linear_build_M_boosted(input_to_cam, inv_boost, wb_norm, M);

    for(size_t i = 0; i < npix; i++)
    {
      const float input_rgb[3] = {
        rgb_out[i],
        rgb_out[i + plane],
        rgb_out[i + 2 * plane],
      };
      float cam[3];
      mat3mulv(cam, M, input_rgb);
      rgb_out[i]             = cam[0];
      rgb_out[i + plane]     = cam[1];
      rgb_out[i + 2 * plane] = cam[2];
    }

    dt_restore_persist_tile_size(ctx);
  }

  dt_free_align(rgb_src);
  dt_free_align(rgba);

  if(res != 0)
  {
    dt_free_align(rgb_out);
    return res;
  }

  // convert planar RGB back to interleaved for caller convenience
  float *interleaved = dt_alloc_align_float(npix * 3);
  if(!interleaved)
  {
    dt_free_align(rgb_out);
    return 1;
  }
  for(size_t i = 0; i < npix; i++)
  {
    interleaved[i * 3 + 0] = rgb_out[i];
    interleaved[i * 3 + 1] = rgb_out[i + plane];
    interleaved[i * 3 + 2] = rgb_out[i + 2 * plane];
  }
  dt_free_align(rgb_out);

  *out_rgb = interleaved;
  *out_w = w;
  *out_h = h;
  return 0;
}

// preview prep: demosaic-once per image
//
// dt_restore_raw_linear_prepare runs the full per-image demosaic +
// WB + camRGB->lin_rec2020 once and returns a 3ch interleaved buffer at
// sensor resolution; neural_restore.c caches it across previews of the
// same image
int dt_restore_raw_linear_prepare(const dt_restore_context_t *ctx,
                                  const dt_imgid_t imgid,
                                  float **out_rgb,
                                  int *out_w,
                                  int *out_h)
{
  if(!out_rgb || !out_w || !out_h) return 1;
  *out_rgb = NULL;

  // 1. demosaic via minimal darktable pipe (rawprepare + highlights +
  //    demosaic; no temperature, no post-demosaic modules)
  float *rgba = NULL;
  int w = 0, h = 0;
  if(_run_demosaic_pipe(imgid, &rgba, &w, &h)) return 1;

  // 2. snapshot image metadata for WB + matrix derivation
  const dt_image_t *cached = dt_image_cache_get(imgid, 'r');
  if(!cached) { dt_free_align(rgba); return 1; }
  dt_image_t img_meta = *cached;
  dt_image_cache_read_release(cached);

  // WB + matrix derived from ctx so the cached buffer matches what the
  // inference + undo paths will assume. without this, a NONE-mode model
  // would see a buffer with WB baked in by the prepare default and the
  // undo step (which honours ctx) would not strip it back out — magenta
  // cast on re-mosaic
  float wb_norm[3];
  _resolve_linear_wb(ctx, &img_meta, wb_norm);

  float cam_to_input[9];
  float input_to_cam[9];
  if(!_build_cam_matrices(ctx, &img_meta, cam_to_input, input_to_cam))
  {
    for(int i = 0; i < 9; i++)
      cam_to_input[i] = (i % 4 == 0) ? 1.0f : 0.0f;
  }

  // 4. interleaved RGBA -> interleaved RGB in input-space + WB
  const size_t npix = (size_t)w * h;
  float *interleaved = dt_alloc_align_float(npix * 3);
  if(!interleaved) { dt_free_align(rgba); return 1; }

  for(size_t i = 0; i < npix; i++)
  {
    const float cam[3] = {
      rgba[i * 4 + 0] * wb_norm[0],
      rgba[i * 4 + 1] * wb_norm[1],
      rgba[i * 4 + 2] * wb_norm[2],
    };
    float input_rgb[3];
    mat3mulv(input_rgb, cam_to_input, cam);
    interleaved[i * 3 + 0] = input_rgb[0];
    interleaved[i * 3 + 1] = input_rgb[1];
    interleaved[i * 3 + 2] = input_rgb[2];
  }
  dt_free_align(rgba);

  *out_rgb = interleaved;
  *out_w = w;
  *out_h = h;
  return 0;
}

// preview: single-tile X-Trans/linear inference, un-matrix + un-WB +
// un-boost back to raw-ADC, re-mosaic onto the X-Trans CFA, then run
// the user's pipe twice (via dt_restore_run_user_pipe_roi) on the
// patched vs. original CFA to produce display-referred before/after
// crops matching the darkroom render
int dt_restore_raw_linear_preview_piped(dt_restore_context_t *ctx,
                                        const dt_image_t *img,
                                        dt_imgid_t imgid,
                                        const float *full_rgb,
                                        int width, int height,
                                        int crop_x, int crop_y,
                                        int crop_w, int crop_h,
                                        float **out_before_rgb,
                                        float **out_denoised_rgb,
                                        int *out_w,
                                        int *out_h)
{
  if(!ctx || !img || !full_rgb || !out_before_rgb || !out_denoised_rgb)
    return 1;
  *out_before_rgb = NULL;
  *out_denoised_rgb = NULL;
  if(out_w) *out_w = 0;
  if(out_h) *out_h = 0;

  if(width <= 0 || height <= 0 || crop_w <= 0 || crop_h <= 0) return 1;

  const int T = dt_restore_get_tile_size(ctx);
  if(T <= 0) return 1;
  const int max_disp = T - 2 * OVERLAP_LINEAR;
  if(crop_w > max_disp || crop_h > max_disp) return 1;

  int inf_x = crop_x + crop_w / 2 - T / 2;
  int inf_y = crop_y + crop_h / 2 - T / 2;

  // WB + matrix prep (same as dt_restore_raw_linear_prepare /
  // dt_restore_raw_linear_preview — but we also need the REVERSE
  // transforms to go back to camRGB raw for pipe input)
  float wb_norm[3];
  _resolve_linear_wb(ctx, img, wb_norm);

  // NOTE: full_rgb comes from dt_restore_raw_linear_prepare, which
  // always caches in LIN_REC2020. if ctx->input_colorspace is something
  // else, the reverse-matrix below won't undo what _prepare did and
  // output will be wrong. until the cache keys on colorspace, this
  // branch is only correct for LIN_REC2020. we still thread ctx so
  // the invocation shape is right for future work
  float cam_to_input[9];
  float input_to_cam[9];
  if(!_build_cam_matrices(ctx, img, cam_to_input, input_to_cam))
  {
    for(int i = 0; i < 9; i++)
      cam_to_input[i] = input_to_cam[i] = (i % 4 == 0) ? 1.0f : 0.0f;
  }

  // extract crop + overlap from cached full lin_rec2020 -> tile_in
  // apply exposure boost (same as preview), run inference
  const size_t tile_plane = (size_t)T * T;
  float *tile_in = g_try_malloc(tile_plane * 3 * sizeof(float));
  float *tile_out = g_try_malloc(tile_plane * 3 * sizeof(float));
  if(!tile_in || !tile_out)
  {
    g_free(tile_in);
    g_free(tile_out);
    return 1;
  }

  for(int dy = 0; dy < T; dy++)
  {
    const int sy = _mirror(inf_y + dy, height);
    for(int dx = 0; dx < T; dx++)
    {
      const int sx = _mirror(inf_x + dx, width);
      const size_t src = ((size_t)sy * width + sx) * 3;
      const size_t dst = (size_t)dy * T + dx;
      tile_in[dst]                  = full_rgb[src + 0];
      tile_in[dst + tile_plane]     = full_rgb[src + 1];
      tile_in[dst + 2 * tile_plane] = full_rgb[src + 2];
    }
  }

  float exposure_boost = 1.0f;
  _linear_exposure_boost(ctx, tile_in, tile_plane, NULL, &exposure_boost);

  if(dt_restore_run_patch_3ch_raw(ctx, tile_in, T, T, tile_out) != 0)
  {
    g_free(tile_in);
    g_free(tile_out);
    return 1;
  }

  if(ctx->output_scale == DT_RESTORE_OUT_MATCH_GAIN)
    _linear_gain_match(tile_in, tile_out, tile_plane, NULL);
  g_free(tile_in);

  // build matrix to reverse matrix + WB + boost + normalise
  // tile_out came from a boosted tile_in (gain_match matches boosted
  // magnitudes). to write it back to the native CFA, we reverse the
  // whole prepare chain:
  //   input-space → (input_to_cam) → cam[k] = sum_i M[k][i] * in[i]
  //   → /wb_norm[k] → un-WB'd raw scale (normalised)
  //   → *range[?]+black[?] → raw ADC range
  //   → rounded uint16 CFA value
  // folding WB undo and boost undo into one matrix applied per-pixel
  const float inv_boost = 1.0f / exposure_boost;
  float M_boosted[9];
  _linear_build_M_boosted(input_to_cam, inv_boost, wb_norm, M_boosted);

  // tile_in was already freed after the gain-match loop above

  // fetch native raw buffer + rawprepare params for un-normalise
  // mbuf is at RAW sensor dims (e.g. 6336x4182), which are larger than
  // the post-rawprepare dims the caller passed (e.g. 6240x4160). the
  // rawprepare crop offset lives in img->crop_x / img->crop_y
  dt_mipmap_buffer_t mbuf;
  dt_mipmap_cache_get(&mbuf, imgid, DT_MIPMAP_FULL,
                      DT_MIPMAP_BLOCKING, 'r');
  if(!mbuf.buf || mbuf.width <= 0 || mbuf.height <= 0)
  {
    dt_mipmap_cache_release(&mbuf);
    g_free(tile_out);
    return 1;
  }
  const int raw_w = mbuf.width;
  const int raw_h = mbuf.height;
  const int raw_off_x = img->crop_x;
  const int raw_off_y = img->crop_y;
  const int is_uint16 = (img->buf_dsc.datatype == TYPE_UINT16);
  const int is_float  = (img->buf_dsc.datatype == TYPE_FLOAT);
  if(!is_uint16 && !is_float)
  {
    dt_mipmap_cache_release(&mbuf);
    g_free(tile_out);
    return 1;
  }
  const size_t pixel_sz = is_uint16 ? 2 : 4;
  const size_t total_bytes = (size_t)raw_w * raw_h * pixel_sz;

  // rawprepare's normalisation: pipe will do (value - sub) / div where
  // sub is per-CFA-site black level and div is (white - black). to get
  // back to raw ADC space we compute per-site (value * range[idx]) + black[idx]
  // NOTE: raw_black_level_separate is indexed by CFA position k in 0..3
  // (even if X-Trans has 6 colours, darktable's per-sensel black is 4-
  // entry; typical cameras use one value for all positions anyway)
  float black[4], range[4], white;
  _compute_cfa_black_range(img, black, range, &white);

  // build patched CFA: copy original, overwrite crop with re-mosaiced denoised
  void *patched = g_try_malloc(total_bytes);
  if(!patched)
  {
    dt_mipmap_cache_release(&mbuf);
    g_free(tile_out);
    return 1;
  }
  memcpy(patched, mbuf.buf, total_bytes);

  // patch the full T × T inference region (clamped to the post-rawprepare
  // buffer extent) rather than just the display crop. this gives the
  // pipe's geometry chain ~tile-size/2 pixels of slop on each side so
  // any residual coordinate drift falls inside denoised data instead of
  // showing original CFA at the preview edge
  const int patch_x0 = (inf_x < 0) ? 0 : inf_x;
  const int patch_y0 = (inf_y < 0) ? 0 : inf_y;
  const int patch_x1 = (inf_x + T > width)  ? width  : inf_x + T;
  const int patch_y1 = (inf_y + T > height) ? height : inf_y + T;

  for(int py = patch_y0; py < patch_y1; py++)
  {
    const int sr_raw = raw_off_y + py;
    const size_t mo_row = (size_t)(py - inf_y) * T;
    for(int px = patch_x0; px < patch_x1; px++)
    {
      const int sc_raw = raw_off_x + px;
      const size_t mx = (size_t)(px - inf_x);
      const float rec[3] = {
        tile_out[0 * tile_plane + mo_row + mx],
        tile_out[1 * tile_plane + mo_row + mx],
        tile_out[2 * tile_plane + mo_row + mx],
      };
      // rec → cam (un-matrix + un-WB + un-boost); clamp to [0, 1]
      float cam[3];
      mat3mulv(cam, M_boosted, rec);
      for(int c = 0; c < 3; c++)
      {
        if(cam[c] < 0.0f) cam[c] = 0.0f;
        if(cam[c] > 1.0f) cam[c] = 1.0f;
      }
      // re-mosaic: pick the single colour that the X-Trans pattern
      // wants at this sensor position, scaled back to raw ADC range.
      // FCxtrans uses raw-sensor parity (since xtrans[6][6] is aligned
      // with the raw, not the post-crop buffer)
      const int ch = FCxtrans(sr_raw, sc_raw, NULL, img->buf_dsc.xtrans);
      const int bl_idx = ((sr_raw & 1) << 1) | (sc_raw & 1);
      const float adc = cam[ch] * range[bl_idx] + black[bl_idx];
      const float clipped
        = adc < 0.0f ? 0.0f : (adc > white ? white : adc);
      const size_t idx = (size_t)sr_raw * raw_w + sc_raw;
      if(is_uint16)
        ((uint16_t *)patched)[idx] = (uint16_t)(clipped + 0.5f);
      else
        ((float *)patched)[idx] = clipped;
    }
  }

  g_free(tile_out);

  // run pipe twice on raw-sensor-sized buffers
  // ROI is in sensor coords (matching the patched region we built
  // above); dt_restore_run_user_pipe_roi forward-transforms it
  // through the user's geometry chain before handing to the pipe
  int dw = 0, dh = 0, bw = 0, bh = 0;
  int err = dt_restore_run_user_pipe_roi(imgid, patched, raw_w, raw_h,
                                 crop_x, crop_y, crop_w, crop_h,
                                 &dw, &dh, out_denoised_rgb);
  g_free(patched);

  if(err == 0)
  {
    err = dt_restore_run_user_pipe_roi(imgid, (void *)mbuf.buf, raw_w, raw_h,
                               crop_x, crop_y, crop_w, crop_h,
                               &bw, &bh, out_before_rgb);
  }
  dt_mipmap_cache_release(&mbuf);

  if(err || dw != bw || dh != bh)
  {
    if(dw != bw || dh != bh)
      dt_print(DT_DEBUG_AI,
               "[restore_raw_linear] preview_piped: before/after dim "
               "mismatch (%dx%d vs %dx%d) — aborting",
               bw, bh, dw, dh);
    g_free(*out_before_rgb);   *out_before_rgb = NULL;
    g_free(*out_denoised_rgb); *out_denoised_rgb = NULL;
    return 1;
  }
  if(out_w) *out_w = dw;
  if(out_h) *out_h = dh;
  return 0;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
