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

#include "common/ai/restore_raw_bayer.h"
#include "common/ai/restore.h"
#include "common/ai/restore_common.h"
#include "common/colorspaces.h"
#include "common/darktable.h"
#include "common/image.h"
#include "common/image_cache.h"
#include "common/iop_order.h"
#include "common/mipmap_cache.h"
#include "control/jobs.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "develop/pixelpipe_hb.h"
#include "imageio/imageio_common.h"

#include <glib.h>
#include <math.h>
#include <string.h>

#define OVERLAP_PACKED 32  // tile overlap in packed (half-res) pixels

// find (y0, x0) in {0,1}^2 such that FC(y0, x0, filters) == 0 (R).
// returns TRUE for standard Bayer patterns; FALSE for non-Bayer (filters
// == 0) or X-Trans (filters == 9u), which this pipeline does not handle
static gboolean _bayer_origin(uint32_t filters, int *y0, int *x0)
{
  if(filters == 0u || filters == 9u) return FALSE;
  for(int y = 0; y < 2; y++)
    for(int x = 0; x < 2; x++)
      if(FC(y, x, filters) == 0)
      {
        *y0 = y;
        *x0 = x;
        return TRUE;
      }
  return FALSE;
}

// shared prep data for Bayer batch + preview
// resolves everything the CFA→packed-input pipeline needs from the
// image metadata: CFA pattern + origin, per-site black/white/range,
// and daylight WB multipliers. both the batch path
// (dt_restore_raw_bayer) and the piped preview use this identical
// pre-processing; keeping it in one helper stops the two copies from
// drifting
typedef struct _bayer_prep_t
{
  uint32_t filters;
  int      y0, x0;
  float    white;
  float    black[4];
  float    range[4];
  float    wb_norm[3];  // daylight WB, G normalised to 1
  float    clip_max;    // = white; kept separate for readability
} _bayer_prep_t;

// populate prep from img metadata. returns 0 on success, 1 when the
// CFA pattern is unsupported (X-Trans or monochrome)
// compute daylight WB (D65 derived from adobe_XYZ_to_CAM). on success
// writes R/B multipliers with G=1 into wb[0..2] and returns TRUE
static gboolean _bayer_wb_daylight(const dt_image_t *img, float wb[3])
{
  const float D65[3] = { 0.9504f, 1.0f, 1.0889f };
  float resp[3] = { 0.0f, 0.0f, 0.0f };
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
    return FALSE;
  wb[0] = resp[1] / resp[0];
  wb[1] = 1.0f;
  wb[2] = resp[1] / resp[2];
  return TRUE;
}

// as-shot WB from img->wb_coeffs normalized to G=1
static gboolean _bayer_wb_as_shot(const dt_image_t *img, float wb[3])
{
  if(img->wb_coeffs[0] <= 0.0f
     || img->wb_coeffs[1] <= 0.0f
     || img->wb_coeffs[2] <= 0.0f)
    return FALSE;
  const float g = img->wb_coeffs[1];
  wb[0] = img->wb_coeffs[0] / g;
  wb[1] = 1.0f;
  wb[2] = img->wb_coeffs[2] / g;
  return TRUE;
}

static int _compute_bayer_prep(const dt_restore_context_t *ctx,
                               const dt_image_t *img, _bayer_prep_t *p)
{
  if(!img || !p) return 1;
  p->filters = img->buf_dsc.filters;
  if(!_bayer_origin(p->filters, &p->y0, &p->x0))
  {
    dt_print(DT_DEBUG_AI,
             "[restore_raw_bayer] unsupported CFA pattern (filters=0x%x)",
             p->filters);
    return 1;
  }

  _compute_cfa_black_range(img, p->black, p->range, &p->white);
  p->clip_max = p->white;

  // WB normalization keyed off ctx->wb_mode. RawNIND's v1 weights were
  // trained on daylight-WB'd data, so the default is DAYLIGHT (derive
  // D65 multipliers from adobe_XYZ_to_CAM) with as-shot as the fallback
  // when the matrix is missing. AS_SHOT flips the order (as-shot first,
  // daylight fallback) for models trained on as-shot distributions.
  // NONE leaves camRGB untouched. The same wb_norm is inverted in
  // postprocess so the round-trip is consistent regardless of mode.
  p->wb_norm[0] = p->wb_norm[1] = p->wb_norm[2] = 1.0f;
  const dt_restore_wb_mode_t mode
    = ctx ? ctx->wb_mode : DT_RESTORE_WB_DAYLIGHT;
  if(mode == DT_RESTORE_WB_DAYLIGHT)
  {
    if(!_bayer_wb_daylight(img, p->wb_norm))
      _bayer_wb_as_shot(img, p->wb_norm);
  }
  else if(mode == DT_RESTORE_WB_AS_SHOT)
  {
    if(!_bayer_wb_as_shot(img, p->wb_norm))
      _bayer_wb_daylight(img, p->wb_norm);
  }
  // DT_RESTORE_WB_NONE: leave at {1, 1, 1}
  return 0;
}

// shared re-mosaic per-pixel math: model camRGB value → raw ADC
// value (reverses WB, normalisation and black-level shift). caller
// supplies (r, c, ch) from its own FC() dispatch and reads model_val
// from the 2T × 2T tile_out; the caller-side blend / clip / store
// differs per path (batch writes uint16 CFA with strength blend,
// preview writes uint16 or float into a patched sensor buffer) so we
// keep just the pure pixel math shared
static inline float _bayer_remosaic_raw(int r, int c, int ch,
                                        float model_val,
                                        const _bayer_prep_t *prep)
{
  const float normalized = model_val / prep->wb_norm[ch];
  const int bl_idx = ((r & 1) << 1) | (c & 1);
  return normalized * prep->range[bl_idx] + prep->black[bl_idx];
}

// shared scalar match_gain: scales 3ch model output (2T × 2T) so
// its mean equals the 4ch input mean. identical algorithm for batch
// and preview; batch uses the returned means/gain to log a per-tile
// diagnostic
static void _bayer_gain_match(const float *tile_in,
                              float *tile_out,
                              int T,
                              double *out_in_mean,
                              double *out_out_mean,
                              float *out_gain)
{
  const size_t tile_in_plane = (size_t)T * T;
  const size_t tile_out_plane = (size_t)(2 * T) * (size_t)(2 * T);
  double in_sum = 0.0, out_sum = 0.0;
  for(int k = 0; k < 4; k++)
  {
    const float *p = tile_in + (size_t)k * tile_in_plane;
    for(size_t i = 0; i < tile_in_plane; i++) in_sum += p[i];
  }
  for(int k = 0; k < 3; k++)
  {
    const float *p = tile_out + (size_t)k * tile_out_plane;
    for(size_t i = 0; i < tile_out_plane; i++) out_sum += p[i];
  }
  const double in_mean = in_sum / (double)(4 * tile_in_plane);
  const double out_mean = out_sum / (double)(3 * tile_out_plane);
  // allow negative gain too: the RawNIND model output scale is
  // arbitrary by design (match_gain post-step during training absorbs
  // it); in some variants the sign is also inverted. guard only
  // against near-zero mean
  const float gain = (fabsf((float)out_mean) > 1e-8f)
    ? (float)(in_mean / out_mean) : 1.0f;
  if(gain != 1.0f)
  {
    const size_t total_out = tile_out_plane * 3;
    for(size_t i = 0; i < total_out; i++) tile_out[i] *= gain;
  }
  if(out_in_mean)  *out_in_mean = in_mean;
  if(out_out_mean) *out_out_mean = out_mean;
  if(out_gain)     *out_gain = gain;
}

// shared 4ch packing: CFA → planar [R, G1, G2, B] at T×T packed
// compute the mirror-reflection bounds + oriented tile origin based on
// the packing policy on ctx. sr0_base / sc0_base are the caller's
// sensor-space base coords for the tile's top-left 2x2 *before* any
// RGGB-forcing shift (batch passes 2*(py_base - O), preview passes
// the user-centred even-snapped inf_y/inf_x)
//   - FORCE_RGGB + MIRROR_CROPPED (bayer_v1 default): origin shifts by
//     (y0, x0) so channel 0 always hits R; mirror reflects within the
//     cropped [y0, H - y0?1:0) x [x0, W - x0?1:0) rectangle — matches
//     training pipelines that physically crop to RGGB before tiling
//   - FORCE_RGGB + MIRROR: same origin shift, but reflections happen
//     against the full buffer (legacy darktable behavior; equivalent
//     to training that doesn't use mirror padding at all)
//   - NATIVE + *: no origin shift; each 4ch slot holds the sensor's
//     native CFA position. mirror is always full-buffer
static void _bayer_tile_geometry(const dt_restore_context_t *ctx,
                                 const _bayer_prep_t *prep,
                                 int sr0_base, int sc0_base,
                                 int width, int height,
                                 int *sr0_origin, int *sc0_origin,
                                 int *mir_y_lo, int *mir_y_hi,
                                 int *mir_x_lo, int *mir_x_hi)
{
  const gboolean force_rggb
    = !ctx || ctx->bayer_orientation == DT_RESTORE_BAYER_FORCE_RGGB;
  const int y0 = force_rggb ? prep->y0 : 0;
  const int x0 = force_rggb ? prep->x0 : 0;
  *sr0_origin = sr0_base + y0;
  *sc0_origin = sc0_base + x0;

  const gboolean cropped_mirror
    = ctx && force_rggb
      && ctx->edge_pad == DT_RESTORE_EDGE_MIRROR_CROPPED;
  *mir_y_lo = cropped_mirror ? y0 : 0;
  *mir_x_lo = cropped_mirror ? x0 : 0;
  *mir_y_hi = cropped_mirror ? (height - (y0 ? 1 : 0)) : height;
  *mir_x_hi = cropped_mirror ? (width  - (x0 ? 1 : 0)) : width;
}

// sr0_origin / sc0_origin are sensor-space top-left coords of the packed
// block's (0, 0); edges are mirror-padded via _mirror_in_range().
// batch and preview paths call this with different origins (tile grid
// vs. a single centred inference tile) but the per-pixel math is identical
// pack a T x T packed-half-res 4-channel tile from the full CFA buffer.
// sr0_origin / sc0_origin is the starting sensor-space (row, col) of the
// tile's top-left 2x2 block; for force_rggb orientation the caller shifts
// by (y0, x0) so channel 0 always hits R.
// [mir_y_lo, mir_y_hi) and [mir_x_lo, mir_x_hi) are the mirror-reflection
// bounds. for EDGE_MIRROR these are [0, height) / [0, width); for
// EDGE_MIRROR_CROPPED they shrink to the effective-RGGB-cropped
// rectangle so reflections match what a crop-then-tile training pipeline
// would see
static void _pack_bayer_tile(const float *cfa,
                             int width, int height,
                             int sr0_origin, int sc0_origin,
                             int mir_y_lo, int mir_y_hi,
                             int mir_x_lo, int mir_x_hi,
                             int T,
                             const _bayer_prep_t *prep,
                             float *tile_in)
{
  const uint32_t filters = prep->filters;
  const float *const black = prep->black;
  const float *const range = prep->range;
  const float *const wb_norm = prep->wb_norm;
  const size_t tile_in_plane = (size_t)T * T;

  for(int dy = 0; dy < T; dy++)
  {
    const int sr0 = sr0_origin + 2 * dy;
    for(int dx = 0; dx < T; dx++)
    {
      const int sc0 = sc0_origin + 2 * dx;
      for(int k = 0; k < 4; k++)
      {
        const int dr = (k >> 1) & 1;
        const int dc = k & 1;
        const int r = _mirror_in_range(sr0 + dr, mir_y_lo, mir_y_hi);
        const int c = _mirror_in_range(sc0 + dc, mir_x_lo, mir_x_hi);
        const float val = cfa[(size_t)r * width + c];
        const int bl_idx = ((r & 1) << 1) | (c & 1);
        const float normalized = (val - black[bl_idx]) / range[bl_idx];
        const int ch = FC(r, c, filters);
        tile_in[k * tile_in_plane + (size_t)dy * T + dx]
          = normalized * wb_norm[ch];
      }
    }
  }
}

int dt_restore_raw_bayer(dt_restore_context_t *ctx,
                         const dt_image_t *img,
                         const float *cfa_in,
                         int width,
                         int height,
                         uint16_t *cfa_out,
                         float strength,
                         struct _dt_job_t *control_job)
{
  if(!ctx || !img || !cfa_in || !cfa_out
     || width <= 0 || height <= 0)
    return 1;

  const float alpha = strength < 0.0f ? 0.0f
                    : (strength > 1.0f ? 1.0f : strength);
  const float inv_alpha = 1.0f - alpha;

  _bayer_prep_t prep;
  if(_compute_bayer_prep(ctx, img, &prep)) return 1;
  const uint32_t filters = prep.filters;
  const int      y0      = prep.y0;
  const int      x0      = prep.x0;
  const float    white   = prep.white;
  const float *const black   = prep.black;
  const float *const wb_norm = prep.wb_norm;
  const float    clip_max = prep.clip_max;

  // initialize output with source CFA (covers margins directly)
  // margins are the 0-2 rows/cols outside the bayer-aligned working
  // region; the model doesn't see them, so we keep original sensor
  // values there
  for(size_t i = 0; i < (size_t)width * height; i++)
  {
    const float v = cfa_in[i];
    const float cv = v < 0.0f ? 0.0f : (v > clip_max ? clip_max : v);
    cfa_out[i] = (uint16_t)(cv + 0.5f);
  }

  // working region in sensor coords: [y0..y0+2*Hh) x [x0..x0+2*Wh)
  const int Hh = (height - y0) / 2;
  const int Wh = (width - x0) / 2;
  if(Hh <= 0 || Wh <= 0) return 0;  // too small; output == input

  // tile setup in packed (half-res) space
  const int O = OVERLAP_PACKED;
  int T = dt_restore_get_tile_size(ctx);
  int n_ladder = 0;
  const int *ladder = dt_restore_get_tile_ladder(ctx, &n_ladder);
  if(T <= 2 * O) T = 256;  // defensive fallback

retry:;
  const int step = T - 2 * O;
  if(step <= 0) return 1;
  const size_t tile_in_plane = (size_t)T * T;
  const size_t tile_out_w = 2 * (size_t)T;
  const size_t tile_out_plane = tile_out_w * tile_out_w;
  const int cols = (Wh + step - 1) / step;
  const int rows = (Hh + step - 1) / step;
  const int total_tiles = cols * rows;

  dt_print(DT_DEBUG_AI,
           "[restore_raw_bayer] %dx%d sensor (CFA origin %d,%d), "
           "working %dx%d packed, tile T=%d, %dx%d grid (%d tiles)",
           width, height, y0, x0, Wh, Hh,
           T, cols, rows, total_tiles);

  // diagnostic: raw CFA range and preprocessing params
  {
    const size_t npix_dbg = (size_t)width * height;
    float in_min = cfa_in[0], in_max = cfa_in[0];
    const size_t step_ = (npix_dbg < 1000000) ? 1 : (npix_dbg / 1000000);
    for(size_t i = 0; i < npix_dbg; i += step_)
    {
      if(cfa_in[i] < in_min) in_min = cfa_in[i];
      if(cfa_in[i] > in_max) in_max = cfa_in[i];
    }
    dt_print(DT_DEBUG_AI,
             "[restore_raw_bayer] raw CFA range [%.1f, %.1f], "
             "black=[%.0f,%.0f,%.0f,%.0f] white=%.0f "
             "wb_coeffs=[%.3f,%.3f,%.3f,%.3f] wb_norm=[%.3f,%.3f,%.3f]",
             in_min, in_max,
             black[0], black[1], black[2], black[3], white,
             img->wb_coeffs[0], img->wb_coeffs[1],
             img->wb_coeffs[2], img->wb_coeffs[3],
             wb_norm[0], wb_norm[1], wb_norm[2]);
  }

  float *tile_in = g_try_malloc(tile_in_plane * 4 * sizeof(float));
  float *tile_out = g_try_malloc(tile_out_plane * 3 * sizeof(float));
  if(!tile_in || !tile_out)
  {
    g_free(tile_in);
    g_free(tile_out);
    return 1;
  }

  int res = 0;
  int tile_count = 0;

  // overlap blending: at each tile boundary 2 (4 at corners) tiles emit
  // ax·ay-weighted contributions whose ramps sum to 1. only the 2*sensor_O-
  // wide seam regions accumulate; pure interior hard-writes. h-strips own
  // corners. memory ~4 MB live, independent of image size
  const int sensor_O = 2 * O;
  const int hstrip_h = 2 * sensor_O;

  // h_strip_top = seam between (ty-1) and ty: built by ty-1 as bot, flushed by ty
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
      h_strip_bot = g_try_malloc0((size_t)width * hstrip_h * sizeof(float));
      if(!h_strip_bot) { res = 1; break; }
    }

    // v_strip_left = seam (tx-1)↔tx: rotated in from tx-1's right, flushed by tx
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

      const int py_base = ty * step;  // core-valid packed start (within working)
      const int px_base = tx * step;
      const int py_end = (py_base + step > Hh) ? Hh : py_base + step;
      const int px_end = (px_base + step > Wh) ? Wh : px_base + step;

      // build 4ch input at packed half-res (T x T). geometry picks
      // the right origin and mirror-reflection bounds based on
      // ctx->bayer_orientation + ctx->edge_pad
      int sr0_origin, sc0_origin;
      int mir_y_lo, mir_y_hi, mir_x_lo, mir_x_hi;
      _bayer_tile_geometry(ctx, &prep,
                           2 * (py_base - O), 2 * (px_base - O),
                           width, height,
                           &sr0_origin, &sc0_origin,
                           &mir_y_lo, &mir_y_hi, &mir_x_lo, &mir_x_hi);
      _pack_bayer_tile(cfa_in, width, height,
                       sr0_origin, sc0_origin,
                       mir_y_lo, mir_y_hi, mir_x_lo, mir_x_hi,
                       T, &prep, tile_in);

      // diagnostic: tile 0 pre-inference (4ch packed input)
      if(tx == 0 && ty == 0)
      {
        float mn[4] = {tile_in[0], tile_in[0], tile_in[0], tile_in[0]};
        float mx[4] = {tile_in[0], tile_in[0], tile_in[0], tile_in[0]};
        for(int k = 0; k < 4; k++)
        {
          const float *p = tile_in + (size_t)k * tile_in_plane;
          mn[k] = mx[k] = p[0];
          for(size_t i = 0; i < tile_in_plane; i++)
          {
            if(p[i] < mn[k]) mn[k] = p[i];
            if(p[i] > mx[k]) mx[k] = p[i];
          }
        }
        dt_print(DT_DEBUG_AI,
                 "[restore_raw_bayer] tile0 model_input range "
                 "R=[%.3f,%.3f] G1=[%.3f,%.3f] G2=[%.3f,%.3f] B=[%.3f,%.3f]",
                 mn[0], mx[0], mn[1], mx[1],
                 mn[2], mx[2], mn[3], mx[3]);
      }

      // inference
      if(dt_restore_run_patch_bayer(ctx, tile_in, T, T, tile_out) != 0)
      {
        // step down the ladder if possible. first tile only so we
        // don't rewrite pixels we've already delivered
        int next_T = 0;
        for(int i = 0; i < n_ladder; i++)
          if(ladder[i] < T) { next_T = ladder[i]; break; }
        if(next_T > 0 && ty == 0 && tx == 0
           && dt_restore_reload_session(ctx, next_T))
        {
          dt_print(DT_DEBUG_AI,
                   "[restore_raw_bayer] inference failed at T=%d, retrying T=%d",
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
                 "[restore_raw_bayer] inference failed at tile %d,%d (T=%d)",
                 tx, ty, T);
        res = 1;
        break;
      }

      // match_gain: scale model output so its mean equals the
      // preprocessed input mean. the RawNIND model output has an
      // arbitrary scale (up to ~10^6) — the Python inference path
      // applies match_gain() after every forward pass. we match
      // per-tile which is stable: the gain factor is a property of
      // the trained weights, approximately constant across tiles of
      // the same image. applied in place in tile_out. skipped for
      // ABSOLUTE-scale models whose output is already calibrated
      double in_mean = 0.0, out_mean = 0.0;
      float gain = 1.0f;
      if(ctx->output_scale == DT_RESTORE_OUT_MATCH_GAIN)
        _bayer_gain_match(tile_in, tile_out, T,
                          &in_mean, &out_mean, &gain);

      // diagnostic: tile 0 post-gain model-output ranges + gain info
      if(tx == 0 && ty == 0)
      {
        float mn[3] = {tile_out[0], tile_out[0], tile_out[0]};
        float mx[3] = {tile_out[0], tile_out[0], tile_out[0]};
        for(int k = 0; k < 3; k++)
        {
          const float *p = tile_out + (size_t)k * tile_out_plane;
          mn[k] = mx[k] = p[0];
          for(size_t i = 0; i < tile_out_plane; i++)
          {
            if(p[i] < mn[k]) mn[k] = p[i];
            if(p[i] > mx[k]) mx[k] = p[i];
          }
        }
        dt_print(DT_DEBUG_AI,
                 "[restore_raw_bayer] tile0 model_output range "
                 "R=[%.3f,%.3f] G=[%.3f,%.3f] B=[%.3f,%.3f] "
                 "in_mean=%.3f out_mean=%.3f gain=%.3e",
                 mn[0], mx[0], mn[1], mx[1], mn[2], mx[2],
                 in_mean, out_mean, (double)gain);
      }

      const gboolean has_left = tx > 0;
      const gboolean has_right = tx < cols - 1;
      const int sensor_py_base = y0 + 2 * py_base;
      const int sensor_py_end  = y0 + 2 * py_end;
      const int sensor_px_base = x0 + 2 * px_base;
      const int sensor_px_end  = x0 + 2 * px_end;

      // cores edge-to-edge in y → one shared h_strip_bot origin per row
      if(has_bot && tx == 0) h_strip_bot_sy0 = sensor_py_end - sensor_O;

      // v-strip excludes top/bot corners (h-strips own them) → y extent = pure interior
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
                                        * v_strip_right_h * sizeof(float));
          if(!v_strip_right) { res = 1; break; }
        }
      }

      // extended extent = core ± seam where a neighbor exists; matches model-output validity
      const int ext_y0 = has_top  ? sensor_py_base - sensor_O : sensor_py_base;
      const int ext_y1 = has_bot  ? sensor_py_end + sensor_O  : sensor_py_end;
      const int ext_x0 = has_left ? sensor_px_base - sensor_O : sensor_px_base;
      const int ext_x1 = has_right? sensor_px_end + sensor_O  : sensor_px_end;

      for(int sr = ext_y0; sr < ext_y1; sr++)
      {
        const int my = 2 * O + (sr - sensor_py_base);
        const float ay = _seam_ay(sr, sensor_py_base, sensor_py_end,
                                  sensor_O, has_top, has_bot);
        const gboolean in_horiz_seam = (ay < 1.0f);
        const size_t mo_row = (size_t)my * tile_out_w;

        float *h_strip = NULL;
        int    h_strip_sy0 = 0;
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
          ? (size_t)(sr - h_strip_sy0) * width : 0;

        for(int sc = ext_x0; sc < ext_x1; sc++)
        {
          const int mx = 2 * O + (sc - sensor_px_base);
          const float ax = _seam_ax(sc, sensor_px_base, sensor_px_end,
                                    sensor_O, has_left, has_right);
          const gboolean in_vert_seam = (ax < 1.0f);

          const int ch = FC(sr, sc, filters);  // 0=R, 1=G, 2=B
          const float model_val
            = tile_out[(size_t)ch * tile_out_plane + mo_row + mx];
          const float raw_val
            = _bayer_remosaic_raw(sr, sc, ch, model_val, &prep);

          const size_t pidx = (size_t)sr * width + sc;
          const float blended
            = alpha * raw_val + inv_alpha * cfa_in[pidx];

          if(in_horiz_seam)
          {
            // h-strip owns corners too; weight ax·ay (other 3 tiles complete the sum)
            if(h_strip)
              h_strip[h_strip_row_off + sc] += ax * ay * blended;
          }
          else if(in_vert_seam)
          {
            float *v_strip = NULL;
            int v_sx0 = 0, v_sy0 = 0;
            if(has_left && sc < sensor_px_base + sensor_O)
            {
              v_strip = v_strip_left;
              v_sx0 = v_strip_left_sx0; v_sy0 = v_strip_left_sy0;
            }
            else if(has_right && sc >= sensor_px_end - sensor_O)
            {
              v_strip = v_strip_right;
              v_sx0 = v_strip_right_sx0; v_sy0 = v_strip_right_sy0;
            }
            if(v_strip)
            {
              const size_t vidx
                = (size_t)(sr - v_sy0) * (2 * sensor_O) + (sc - v_sx0);
              v_strip[vidx] += ax * blended;
            }
          }
          else
          {
            const float clipped
              = blended < 0.0f ? 0.0f
                : (blended > clip_max ? clip_max : blended);
            cfa_out[pidx] = (uint16_t)(clipped + 0.5f);
          }
        }
      }

      // tx-1 + tx ramps now sum to 1; strip = final value, flush + free
      if(v_strip_left)
      {
        for(int sr = v_strip_left_sy0;
            sr < v_strip_left_sy0 + v_strip_left_h; sr++)
        {
          const size_t vrow = (size_t)(sr - v_strip_left_sy0) * (2 * sensor_O);
          for(int dxs = 0; dxs < 2 * sensor_O; dxs++)
          {
            const int sc = v_strip_left_sx0 + dxs;
            const float v = v_strip_left[vrow + dxs];
            const float clipped
              = v < 0.0f ? 0.0f : (v > clip_max ? clip_max : v);
            cfa_out[(size_t)sr * width + sc] = (uint16_t)(clipped + 0.5f);
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

    // defensive: should be NULL after last col, free in case of mid-row break
    g_free(v_strip_left);
    v_strip_left = NULL;

    // ramps sum to 1, flush. clamp sc to working columns — outside cells
    // were never written and would overwrite the cfa_in margin copy
    if(h_strip_top)
    {
      for(int sr = h_strip_top_sy0; sr < h_strip_top_sy0 + hstrip_h; sr++)
      {
        const size_t hrow = (size_t)(sr - h_strip_top_sy0) * width;
        for(int sc = x0; sc < x0 + 2 * Wh; sc++)
        {
          const float v = h_strip_top[hrow + sc];
          const float clipped
            = v < 0.0f ? 0.0f : (v > clip_max ? clip_max : v);
          cfa_out[(size_t)sr * width + sc] = (uint16_t)(clipped + 0.5f);
        }
      }
      g_free(h_strip_top);
    }
    h_strip_top = h_strip_bot;
    h_strip_top_sy0 = h_strip_bot_sy0;
  }

  // last row never allocates a bottom strip — defensive free
  g_free(h_strip_top);

  g_free(tile_in);
  g_free(tile_out);

  if(res == 0)
  {
    // diagnostic: sample cfa_out to confirm values are in a sensible
    // raw-ADC range matching BlackLevel/WhiteLevel the DNG advertises
    const size_t npix_dbg = (size_t)width * height;
    uint16_t omin = cfa_out[0], omax = cfa_out[0];
    uint64_t osum = 0;
    const size_t step_ = (npix_dbg < 1000000) ? 1 : (npix_dbg / 1000000);
    size_t n = 0;
    for(size_t i = 0; i < npix_dbg; i += step_)
    {
      if(cfa_out[i] < omin) omin = cfa_out[i];
      if(cfa_out[i] > omax) omax = cfa_out[i];
      osum += cfa_out[i];
      n++;
    }
    dt_print(DT_DEBUG_AI,
             "[restore_raw_bayer] cfa_out u16 range [%u, %u] mean=%.0f "
             "(DNG will advertise black~%.0f white=%.0f)",
             (unsigned)omin, (unsigned)omax,
             n ? (double)osum / n : 0.0,
             black[0], white);

    dt_restore_persist_tile_size(ctx);
  }

  return res;
}

// preview: single-tile bayer inference + re-mosaic onto a patched CFA,
// then run the user's pipe (via dt_restore_run_user_pipe_roi) twice —
// once on the original mbuf for "before", once on the patched copy for
// "after". the pipe runs at ROI = displayed crop so refreshes stay fast.
// the "after" display-referred output matches what the user would see
// after Process + DNG re-import
int dt_restore_raw_bayer_preview_piped(dt_restore_context_t *ctx,
                                       const dt_image_t *img,
                                       dt_imgid_t imgid,
                                       const float *cfa_full,
                                       int width, int height,
                                       int crop_x, int crop_y,
                                       int crop_w, int crop_h,
                                       float **out_before_rgb,
                                       float **out_denoised_rgb,
                                       int *out_w,
                                       int *out_h)
{
  if(!ctx || !img || !cfa_full || !out_before_rgb || !out_denoised_rgb)
    return 1;
  *out_before_rgb = NULL;
  *out_denoised_rgb = NULL;
  if(out_w) *out_w = 0;
  if(out_h) *out_h = 0;

  if(width <= 0 || height <= 0 || crop_w <= 0 || crop_h <= 0) return 1;

  _bayer_prep_t prep;
  if(_compute_bayer_prep(ctx, img, &prep)) return 1;
  const uint32_t filters = prep.filters;
  const float    clip_max = prep.clip_max;

  const int T = dt_restore_get_tile_size(ctx);
  if(T <= 0) return 1;
  const int sensor_T = 2 * T;
  const int max_disp = sensor_T - 4 * OVERLAP_PACKED;
  if(crop_w > max_disp || crop_h > max_disp) return 1;

  // snap crop to CFA grid
  crop_x = (crop_x / 2) * 2;
  crop_y = (crop_y / 2) * 2;
  crop_w = (crop_w / 2) * 2;
  crop_h = (crop_h / 2) * 2;
  if(crop_w <= 0 || crop_h <= 0) return 1;

  int inf_x = crop_x + crop_w / 2 - sensor_T / 2;
  int inf_y = crop_y + crop_h / 2 - sensor_T / 2;
  inf_x = (inf_x / 2) * 2;
  inf_y = (inf_y / 2) * 2;

  // inference (single tile)
  const size_t tile_in_plane = (size_t)T * T;
  const size_t tile_out_w = 2 * (size_t)T;
  const size_t tile_out_plane = tile_out_w * tile_out_w;

  float *tile_in = g_try_malloc(tile_in_plane * 4 * sizeof(float));
  float *tile_out = g_try_malloc(tile_out_plane * 3 * sizeof(float));
  if(!tile_in || !tile_out)
  {
    g_free(tile_in);
    g_free(tile_out);
    return 1;
  }

  // geometry applies the same orientation + mirror policy as the batch
  // path. sr0_base / sc0_base for the preview is the user-centred,
  // even-snapped inference tile origin in sensor coords
  int pp_sr0, pp_sc0, pp_mir_y_lo, pp_mir_y_hi, pp_mir_x_lo, pp_mir_x_hi;
  _bayer_tile_geometry(ctx, &prep, inf_y, inf_x, width, height,
                       &pp_sr0, &pp_sc0,
                       &pp_mir_y_lo, &pp_mir_y_hi,
                       &pp_mir_x_lo, &pp_mir_x_hi);
  _pack_bayer_tile(cfa_full, width, height,
                   pp_sr0, pp_sc0,
                   pp_mir_y_lo, pp_mir_y_hi, pp_mir_x_lo, pp_mir_x_hi,
                   T, &prep, tile_in);

  if(dt_restore_run_patch_bayer(ctx, tile_in, T, T, tile_out) != 0)
  {
    g_free(tile_in);
    g_free(tile_out);
    return 1;
  }

  // gain-match: same scalar correction as the batch path (gated on
  // output_scale; ABSOLUTE-scale models skip it)
  if(ctx->output_scale == DT_RESTORE_OUT_MATCH_GAIN)
    _bayer_gain_match(tile_in, tile_out, T, NULL, NULL, NULL);
  g_free(tile_in);

  // fetch source sensor buffer in native dtype
  dt_mipmap_buffer_t mbuf;
  dt_mipmap_cache_get(&mbuf, imgid, DT_MIPMAP_FULL,
                      DT_MIPMAP_BLOCKING, 'r');
  if(!mbuf.buf || mbuf.width != width || mbuf.height != height)
  {
    dt_mipmap_cache_release(&mbuf);
    g_free(tile_out);
    return 1;
  }

  const int is_uint16 = (img->buf_dsc.datatype == TYPE_UINT16);
  const int is_float  = (img->buf_dsc.datatype == TYPE_FLOAT);
  if(!is_uint16 && !is_float)
  {
    dt_print(DT_DEBUG_AI,
             "[restore_raw_bayer] preview_piped: unsupported raw datatype %d",
             img->buf_dsc.datatype);
    dt_mipmap_cache_release(&mbuf);
    g_free(tile_out);
    return 1;
  }
  const size_t pixel_sz = is_uint16 ? 2 : 4;
  const size_t total_bytes = (size_t)width * height * pixel_sz;

  // build denoised-patched CFA: copy original, overwrite the
  // entire inference region (2T × 2T sensor pixels) with denoised data.
  // patching beyond the display crop gives the pipe's geometry chain
  // ~64 px of slop on each side — enough to absorb the few-pixel ROI
  // drift that the inscribed-AABB trick alone can't eliminate (pipe
  // sampling slightly outside the quad's interior due to floor/ceil
  // rounding or modules whose distort_transform returns approximations)
  void *patched = g_try_malloc(total_bytes);
  if(!patched)
  {
    dt_mipmap_cache_release(&mbuf);
    g_free(tile_out);
    return 1;
  }
  memcpy(patched, mbuf.buf, total_bytes);

  // patch the full 2T × 2T inference region, clamped to the sensor.
  // pp_sr0 / pp_sc0 is the oriented tile origin (inf_y/x + y0/x0 under
  // FORCE_RGGB, or inf_y/x under NATIVE) — this is where the *output*
  // tile's (0, 0) lives in sensor coords, so the patch rectangle and
  // the tile_out index must use it consistently
  const int patch_x0 = (pp_sc0 < 0) ? 0 : pp_sc0;
  const int patch_y0 = (pp_sr0 < 0) ? 0 : pp_sr0;
  const int patch_x1 = (pp_sc0 + sensor_T > width)  ? width  : pp_sc0 + sensor_T;
  const int patch_y1 = (pp_sr0 + sensor_T > height) ? height : pp_sr0 + sensor_T;

  for(int sr = patch_y0; sr < patch_y1; sr++)
  {
    const size_t mo_row = (size_t)(sr - pp_sr0) * tile_out_w;
    for(int sc = patch_x0; sc < patch_x1; sc++)
    {
      const int ch = FC(sr, sc, filters);
      const float model_val
        = tile_out[(size_t)ch * tile_out_plane + mo_row + (sc - pp_sc0)];
      const float raw_val
        = _bayer_remosaic_raw(sr, sc, ch, model_val, &prep);
      const float clipped = raw_val < 0.0f ? 0.0f
        : (raw_val > clip_max ? clip_max : raw_val);
      const size_t idx = (size_t)sr * width + sc;
      if(is_uint16)
        ((uint16_t *)patched)[idx] = (uint16_t)(clipped + 0.5f);
      else
        ((float *)patched)[idx] = clipped;
    }
  }

  g_free(tile_out);

  // run pipe on patched CFA → out_denoised_rgb
  int dw = 0, dh = 0, bw = 0, bh = 0;
  int err = dt_restore_run_user_pipe_roi(imgid, patched, width, height,
                             crop_x, crop_y, crop_w, crop_h,
                             &dw, &dh, out_denoised_rgb);
  g_free(patched);

  // run pipe on original mbuf → out_before_rgb
  // mbuf.buf is const from our perspective (read-only cache entry) but the
  // pipe set_input API isn't marked const; cast to writable pointer with
  // the understanding that the pipe doesn't mutate its input buffer
  if(err == 0)
  {
    err = dt_restore_run_user_pipe_roi(imgid, (void *)mbuf.buf, width, height,
                           crop_x, crop_y, crop_w, crop_h,
                           &bw, &bh, out_before_rgb);
  }

  dt_mipmap_cache_release(&mbuf);

  if(err || dw != bw || dh != bh)
  {
    // dims must match between the two passes so the caller can blend
    // them; mismatch shouldn't happen (same pipe, same ROI) but guard
    // anyway so we never hand back inconsistent buffers
    if(dw != bw || dh != bh)
      dt_print(DT_DEBUG_AI,
               "[restore_raw_bayer] preview_piped: before/after dim "
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
