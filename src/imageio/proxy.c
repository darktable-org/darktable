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

#include "imageio/proxy.h"
#include "common/darktable.h"
#include "common/p2p.h"

gboolean dt_imageio_proxy_path(const char *raw_path, char *buf, size_t buflen)
{
  if(!raw_path || !raw_path[0] || !buf || buflen == 0) return FALSE;
  g_snprintf(buf, buflen, "%s.proxy.avif", raw_path);
  return TRUE;
}

#ifndef HAVE_LIBRAW
gboolean dt_imageio_create_proxy(const char *raw_path, int quality)
{
  dt_print(DT_DEBUG_IMAGEIO,
           "[proxy] cannot create proxy for '%s': darktable was built without libraw",
           raw_path);
  return FALSE;
}
#else // HAVE_LIBRAW

#include <avif/avif.h>
#include <libraw/libraw.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Bayer helpers
// ---------------------------------------------------------------------------

// Return the CFA colour index (0=R 1=G 2=B 3=G2) at sensor position (row,col).
// Uses the same 32-bit filter encoding as darktable / dcraw.
static inline int _bayer_color(uint32_t filters, int row, int col)
{
  return (filters >> ((((row) << 1 & 14) | ((col) & 1)) << 1)) & 3;
}

typedef struct
{
  int r_row, r_col;
  int g0_row, g0_col;
  int g1_row, g1_col;
  int b_row, b_col;
} _bayer_offsets_t;

static _bayer_offsets_t _get_bayer_offsets(uint32_t filters)
{
  _bayer_offsets_t o = { 0 };
  int g_count = 0;
  for(int row = 0; row < 2; row++)
    for(int col = 0; col < 2; col++)
    {
      int c = _bayer_color(filters, row, col);
      if(c == 0)      { o.r_row = row; o.r_col = col; }
      else if(c == 2) { o.b_row = row; o.b_col = col; }
      else
      {
        if(g_count == 0) { o.g0_row = row; o.g0_col = col; }
        else             { o.g1_row = row; o.g1_col = col; }
        g_count++;
      }
    }
  return o;
}

// ---------------------------------------------------------------------------
// Encoder capability probe
// ---------------------------------------------------------------------------

// sRGB OETF: linear [0,1] → gamma-encoded [0,1].
static inline float _srgb_oetf(float v)
{
  return (v <= 0.0031308f) ? (12.92f * v)
                           : (1.055f * powf(v, 1.0f / 2.4f) - 0.055f);
}

// Format/depth capability descriptor.
// Entries are ordered from most-preferred to least-preferred.
typedef struct
{
  avifPixelFormat fmt;
  int             depth;
  gboolean        use_gamma;  // apply sRGB OETF before encoding (for depth < 12)
  gboolean        identity;   // use MC=identity (Y=G Cb=B Cr=R); only valid for YUV444
} _fmt_cap_t;

// Ordered by bit-depth first (12→10→8), then chroma format (444→422→420).
static const _fmt_cap_t _fmt_prefs[] = {
  { AVIF_PIXEL_FORMAT_YUV444, 12, FALSE, TRUE  }, // 12-bit linear identity
  { AVIF_PIXEL_FORMAT_YUV422, 12, FALSE, FALSE }, // 12-bit linear BT.709 4:2:2
  { AVIF_PIXEL_FORMAT_YUV420, 12, FALSE, FALSE }, // 12-bit linear BT.709 4:2:0
  { AVIF_PIXEL_FORMAT_YUV444, 10, TRUE,  TRUE  }, // 10-bit sRGB identity
  { AVIF_PIXEL_FORMAT_YUV422, 10, TRUE,  FALSE }, // 10-bit sRGB BT.709 4:2:2
  { AVIF_PIXEL_FORMAT_YUV420, 10, TRUE,  FALSE }, // 10-bit sRGB BT.709 4:2:0
  { AVIF_PIXEL_FORMAT_YUV444,  8, TRUE,  TRUE  }, // 8-bit  sRGB identity
  { AVIF_PIXEL_FORMAT_YUV422,  8, TRUE,  FALSE }, // 8-bit  sRGB BT.709 4:2:2
  { AVIF_PIXEL_FORMAT_YUV420,  8, TRUE,  FALSE }, // 8-bit  sRGB BT.709 4:2:0
};
#define N_FMT_PREFS ((int)(sizeof(_fmt_prefs) / sizeof(_fmt_prefs[0])))

// Probe whether a codec can encode a 64×64 image at (fmt, depth).
// 64×64 satisfies SVT-AV1's minimum dimension requirement.
static gboolean _probe_codec_fmt(avifCodecChoice codec, avifPixelFormat fmt, int depth)
{
  avifImage *probe = avifImageCreate(64, 64, depth, fmt);
  if(!probe) return FALSE;

  probe->colorPrimaries          = AVIF_COLOR_PRIMARIES_BT709;
  probe->transferCharacteristics = AVIF_TRANSFER_CHARACTERISTICS_LINEAR;
  probe->matrixCoefficients      = (fmt == AVIF_PIXEL_FORMAT_YUV444)
                                     ? AVIF_MATRIX_COEFFICIENTS_IDENTITY
                                     : AVIF_MATRIX_COEFFICIENTS_BT709;
  probe->yuvRange = AVIF_RANGE_FULL;
#if AVIF_VERSION >= 1000000
  if(avifImageAllocatePlanes(probe, AVIF_PLANES_YUV) != AVIF_RESULT_OK)
  { avifImageDestroy(probe); return FALSE; }
#else
  avifImageAllocatePlanes(probe, AVIF_PLANES_YUV);
#endif
  for(int ch = 0; ch < 3; ch++)
  {
    size_t rows = (ch > 0 && fmt == AVIF_PIXEL_FORMAT_YUV420) ? 32 : 64;
    memset(probe->yuvPlanes[ch], 0, probe->yuvRowBytes[ch] * rows);
  }

  avifEncoder *enc = avifEncoderCreate();
  gboolean ok = FALSE;
  if(enc)
  {
    enc->codecChoice = codec;
    enc->speed       = 10;
#if AVIF_VERSION >= 1000000
    enc->quality = 60;
#else
    enc->minQuantizer = 30;
    enc->maxQuantizer = 40;
#endif
    avifRWData out = AVIF_DATA_EMPTY;
    ok = (avifEncoderWrite(enc, probe, &out) == AVIF_RESULT_OK);
    avifRWDataFree(&out);
    avifEncoderDestroy(enc);
  }
  avifImageDestroy(probe);
  return ok;
}

// Per-codec cached best format index into _fmt_prefs[], or -1 if none work.
// Layout: [0]=SVT  [1]=rav1e  [2]=AOM
// Value -2 means "not yet probed".
static gint _codec_cap[3] = { -2, -2, -2 };

static const struct { avifCodecChoice choice; int speed; const char *name; int cap_idx; }
_codec_prefs[] = {
  { AVIF_CODEC_CHOICE_SVT,   6, "SVT-AV1", 0 },
  { AVIF_CODEC_CHOICE_RAV1E, 6, "rav1e",   1 },
  { AVIF_CODEC_CHOICE_AOM,   6, "AOM",     2 },
};
#define N_CODECS ((int)(sizeof(_codec_prefs) / sizeof(_codec_prefs[0])))

// Returns the best _fmt_prefs[] index for the given codec, probing once if needed.
static int _best_fmt_for_codec(int ci)
{
  const int c = g_atomic_int_get(&_codec_cap[ci]);
  if(c > -2) return c;

  if(!avifCodecName(_codec_prefs[ci].choice, AVIF_CODEC_FLAG_CAN_ENCODE))
  {
    g_atomic_int_compare_and_exchange(&_codec_cap[ci], -2, -1);
    return -1;
  }

  int best = -1;
  for(int fi = 0; fi < N_FMT_PREFS; fi++)
  {
    if(_probe_codec_fmt(_codec_prefs[ci].choice, _fmt_prefs[fi].fmt, _fmt_prefs[fi].depth))
    { best = fi; break; }
  }

  g_atomic_int_compare_and_exchange(&_codec_cap[ci], -2, best);
  if(best >= 0)
    dt_print(DT_DEBUG_IMAGEIO, "[proxy] %s best format: YUV%s %d-bit%s",
             _codec_prefs[ci].name,
             _fmt_prefs[best].fmt == AVIF_PIXEL_FORMAT_YUV444 ? "444" :
             _fmt_prefs[best].fmt == AVIF_PIXEL_FORMAT_YUV422 ? "422" : "420",
             _fmt_prefs[best].depth,
             _fmt_prefs[best].use_gamma ? " sRGB" : " linear");
  else
    dt_print(DT_DEBUG_IMAGEIO, "[proxy] %s: no supported format found",
             _codec_prefs[ci].name);
  return g_atomic_int_get(&_codec_cap[ci]);
}

// ---------------------------------------------------------------------------
// RGB → avifImage conversion
// ---------------------------------------------------------------------------

// LUT: 12-bit linear input → gamma-encoded output at target depth.
// Built once on first use.
static uint16_t _gamma_lut[4096];
static gint     _gamma_lut_depth = 0; // depth the LUT was built for

static void _ensure_gamma_lut(int depth)
{
  if(g_atomic_int_get(&_gamma_lut_depth) == depth) return;
  const float dst_max = (float)((1 << depth) - 1);
  for(int i = 0; i < 4096; i++)
    _gamma_lut[i] = (uint16_t)CLAMP((int)(_srgb_oetf(i / 4095.0f) * dst_max + 0.5f),
                                    0, (int)dst_max);
  g_atomic_int_set(&_gamma_lut_depth, depth);
}

// Fill an avifImage from 12-bit linear r/g/b planar buffers.
static void _fill_avif(avifImage *img,
                       const uint16_t *r_buf, const uint16_t *g_buf, const uint16_t *b_buf,
                       int ow, int oh, const _fmt_cap_t *cap)
{
  const float dst_max  = (float)((1 << cap->depth) - 1);
  const float lin_sc   = dst_max / 4095.0f;

  const int ystride  = (int)(img->yuvRowBytes[AVIF_CHAN_Y] / sizeof(uint16_t));
  const int cbstride = (int)(img->yuvRowBytes[AVIF_CHAN_U] / sizeof(uint16_t));
  const int crstride = (int)(img->yuvRowBytes[AVIF_CHAN_V] / sizeof(uint16_t));
  uint16_t *yp  = (uint16_t *)img->yuvPlanes[AVIF_CHAN_Y];
  uint16_t *cbp = (uint16_t *)img->yuvPlanes[AVIF_CHAN_U];
  uint16_t *crp = (uint16_t *)img->yuvPlanes[AVIF_CHAN_V];

  if(cap->use_gamma) _ensure_gamma_lut(cap->depth);

  if(cap->identity)
  {
    // Y=G  Cb=B  Cr=R — all planes full resolution (YUV444 only)
    for(int y = 0; y < oh; y++)
      for(int x = 0; x < ow; x++)
      {
        int i = y * ow + x;
        if(cap->use_gamma)
        {
          yp [y*ystride  + x] = _gamma_lut[g_buf[i]];
          cbp[y*cbstride + x] = _gamma_lut[b_buf[i]];
          crp[y*crstride + x] = _gamma_lut[r_buf[i]];
        }
        else
        {
          yp [y*ystride  + x] = (uint16_t)(g_buf[i] * lin_sc + 0.5f);
          cbp[y*cbstride + x] = (uint16_t)(b_buf[i] * lin_sc + 0.5f);
          crp[y*crstride + x] = (uint16_t)(r_buf[i] * lin_sc + 0.5f);
        }
      }
    return;
  }

  // BT.709 luma + chroma subsampling (YUV422 or YUV420)
  // Chroma block: nx=2 for 422/420 horizontal, ny=2 only for 420 vertical
  const int nx = (cap->fmt == AVIF_PIXEL_FORMAT_YUV444) ? 1 : 2;
  const int ny = (cap->fmt == AVIF_PIXEL_FORMAT_YUV420) ? 2 : 1;
  const int cw = ow / nx;
  const int ch = oh / ny;

  // Y plane — full resolution
  for(int y = 0; y < oh; y++)
    for(int x = 0; x < ow; x++)
    {
      int idx = y * ow + x;
      float r = r_buf[idx] / 4095.0f;
      float g = g_buf[idx] / 4095.0f;
      float b = b_buf[idx] / 4095.0f;
      float luma = 0.2126f*r + 0.7152f*g + 0.0722f*b;
      float out  = cap->use_gamma ? _srgb_oetf(luma) : luma;
      yp[y*ystride + x] = (uint16_t)CLAMP((int)(out * dst_max + 0.5f), 0, (int)dst_max);
    }

  // Cb/Cr planes — subsampled, averaged over nx×ny blocks
  for(int cy = 0; cy < ch; cy++)
    for(int cx = 0; cx < cw; cx++)
    {
      float cb_sum = 0.0f, cr_sum = 0.0f;
      for(int dy = 0; dy < ny; dy++)
        for(int dx = 0; dx < nx; dx++)
        {
          int idx = (cy*ny + dy) * ow + (cx*nx + dx);
          float r = r_buf[idx] / 4095.0f;
          float g = g_buf[idx] / 4095.0f;
          float b = b_buf[idx] / 4095.0f;
          float luma = 0.2126f*r + 0.7152f*g + 0.0722f*b;
          cb_sum += (b - luma) / 1.8556f + 0.5f;
          cr_sum += (r - luma) / 1.5748f + 0.5f;
        }
      float n    = (float)(nx * ny);
      float cb   = cb_sum / n;
      float cr   = cr_sum / n;
      if(cap->use_gamma) { cb = _srgb_oetf(CLAMP(cb, 0.0f, 1.0f));
                           cr = _srgb_oetf(CLAMP(cr, 0.0f, 1.0f)); }
      cbp[cy*cbstride + cx] = (uint16_t)CLAMP((int)(cb * dst_max + 0.5f), 0, (int)dst_max);
      crp[cy*crstride + cx] = (uint16_t)CLAMP((int)(cr * dst_max + 0.5f), 0, (int)dst_max);
    }
}

// ---------------------------------------------------------------------------
// Proxy creation
// ---------------------------------------------------------------------------

gboolean dt_imageio_create_proxy(const char *raw_path, int quality)
{
  gboolean ok = FALSE;
  libraw_data_t *lr = NULL;
  avifImage    *image   = NULL;
  avifEncoder  *encoder = NULL;
  avifRWData    output  = AVIF_DATA_EMPTY;
  uint16_t     *r_buf = NULL, *g_buf = NULL, *b_buf = NULL;
  FILE         *f = NULL;

  // ---- open raw file with libraw ----------------------------------------
  lr = libraw_init(0);
  if(!lr)
  {
    dt_print(DT_DEBUG_IMAGEIO, "[proxy] libraw_init failed for '%s'", raw_path);
    goto out;
  }
  lr->params.output_bps  = 16;
  lr->params.no_auto_bright = 1;
  lr->params.use_camera_wb  = 0;
  lr->params.use_auto_wb    = 0;

  if(libraw_open_file(lr, raw_path) != LIBRAW_SUCCESS)
  {
    dt_print(DT_DEBUG_IMAGEIO, "[proxy] cannot open '%s'", raw_path);
    goto out;
  }
  if(libraw_unpack(lr) != LIBRAW_SUCCESS)
  {
    dt_print(DT_DEBUG_IMAGEIO, "[proxy] cannot unpack '%s'", raw_path);
    goto out;
  }

  const uint16_t *bayer = lr->rawdata.raw_image;
  if(!bayer)
  {
    // floating-point or 4-channel sensor — not supported for proxy creation
    dt_print(DT_DEBUG_IMAGEIO, "[proxy] unsupported raw format in '%s'", raw_path);
    goto out;
  }

  const int rw = lr->rawdata.sizes.raw_width;
  (void)lr->rawdata.sizes.raw_height; // height unused; width needed for stride
  // Honour the active image area
  const int top  = lr->rawdata.sizes.top_margin;
  const int left = lr->rawdata.sizes.left_margin;
  const int iw   = lr->rawdata.sizes.width;
  const int ih   = lr->rawdata.sizes.height;

  // Work in 2×2 Bayer blocks; bw/bh must be even so each block is complete.
  // ow/oh are further rounded down to even so YUV420 encoders (SVT-AV1) accept them.
  const int bw = (iw / 2) * 2;
  const int bh = (ih / 2) * 2;
  const int ow = (bw / 2) & ~1;
  const int oh = (bh / 2) & ~1;

  const float white = (float)lr->rawdata.color.maximum;
  // Prefer per-channel black levels when available; fall back to scalar
  float black_r, black_g, black_b;
  {
    const unsigned int *cb = lr->rawdata.color.cblack;
    const float sb = (float)lr->rawdata.color.black;
    uint32_t filters = lr->rawdata.iparams.filters;
    // cblack order matches the 2×2 Bayer block scan order
    // We use the _bayer_offsets to map CFA positions → cblack indices
    _bayer_offsets_t o = _get_bayer_offsets(filters);
    // cblack indices: position in raster scan (row*2+col)
    int r_idx  = o.r_row  * 2 + o.r_col;
    int g0_idx = o.g0_row * 2 + o.g0_col;
    int g1_idx = o.g1_row * 2 + o.g1_col;
    int b_idx  = o.b_row  * 2 + o.b_col;
    black_r = sb + (float)cb[r_idx];
    black_g = sb + ((float)cb[g0_idx] + (float)cb[g1_idx]) * 0.5f;
    black_b = sb + (float)cb[b_idx];
  }

  const uint32_t filters = lr->rawdata.iparams.filters;
  _bayer_offsets_t o = _get_bayer_offsets(filters);

  // ---- allocate output buffers -------------------------------------------
  r_buf = malloc(ow * oh * sizeof(uint16_t));
  g_buf = malloc(ow * oh * sizeof(uint16_t));
  b_buf = malloc(ow * oh * sizeof(uint16_t));
  if(!r_buf || !g_buf || !b_buf)
  {
    dt_print(DT_DEBUG_IMAGEIO, "[proxy] out of memory for '%s'", raw_path);
    goto out;
  }

  // ---- Bayer averaging ---------------------------------------------------
  // Each output pixel combines one 2×2 Bayer block from the active area.
  // R and B pass through; G is averaged from the two green samples.
  // Values are normalised to 12-bit range [0, 4095].
  const float scale12 = 4095.0f / (white - MIN(MIN(black_r, black_g), black_b));

  for(int oy = 0; oy < oh; oy++)
  {
    const int by = top + oy * 2;  // top-left row of 2×2 block in raw
    for(int ox = 0; ox < ow; ox++)
    {
      const int bx = left + ox * 2;

      // Extract raw values at each CFA position in the block
      float vr  = (float)bayer[(by + o.r_row)  * rw + (bx + o.r_col)];
      float vg0 = (float)bayer[(by + o.g0_row) * rw + (bx + o.g0_col)];
      float vg1 = (float)bayer[(by + o.g1_row) * rw + (bx + o.g1_col)];
      float vb  = (float)bayer[(by + o.b_row)  * rw + (bx + o.b_col)];

      // Normalise and convert to 12-bit
      int r12 = (int)((vr           - black_r) * scale12 + 0.5f);
      int g12 = (int)(((vg0 + vg1)  * 0.5f - black_g) * scale12 + 0.5f);
      int b12 = (int)((vb           - black_b) * scale12 + 0.5f);

      r_buf[oy * ow + ox] = (uint16_t)CLAMP(r12, 0, 4095);
      g_buf[oy * ow + ox] = (uint16_t)CLAMP(g12, 0, 4095);
      b_buf[oy * ow + ox] = (uint16_t)CLAMP(b12, 0, 4095);
    }
  }

  // ---- adaptive quality based on mean image brightness ---------------------
  // Dark images: raise quality toward 99 to avoid shadow banding.
  // Bright images: use the user-supplied floor.
  {
    float g_sum = 0.0f;
    const int n = ow * oh;
    for(int i = 0; i < n; i++)
      g_sum += g_buf[i];
    const float avg_brightness = g_sum / ((float)n * 4095.0f);

    const float dark_thresh   = 0.03f;
    const float bright_thresh = 0.30f;
    const float t = CLAMP((avg_brightness - dark_thresh)
                          / (bright_thresh - dark_thresh), 0.0f, 1.0f);
    const int q_floor = CLAMP(quality, 0, 98);
    quality = (int)(99.0f - t * (99.0f - (float)q_floor) + 0.5f);

    dt_print(DT_DEBUG_IMAGEIO,
             "[proxy] '%s': avg_brightness=%.3f adaptive_quality=%d",
             raw_path, avg_brightness, quality);
  }

  // ---- probe codecs and encode with best available format --------------------
  // For each codec (SVT first), find the highest bit-depth / widest chroma
  // format it supports, build an avifImage for that format, and attempt encode.
  // First success wins; the per-codec best format is cached across calls.
  {
    gboolean encoded = FALSE;
    for(int ci = 0; ci < N_CODECS && !encoded; ci++)
    {
      const int fi = _best_fmt_for_codec(ci);
      if(fi < 0) continue; // codec absent or no supported format

      const _fmt_cap_t *cap = &_fmt_prefs[fi];

      if(image) { avifImageDestroy(image); image = NULL; }
      image = avifImageCreate(ow, oh, cap->depth, cap->fmt);
      if(!image) continue;

      image->colorPrimaries          = AVIF_COLOR_PRIMARIES_BT709;
      image->transferCharacteristics = cap->use_gamma
                                         ? AVIF_TRANSFER_CHARACTERISTICS_SRGB
                                         : AVIF_TRANSFER_CHARACTERISTICS_LINEAR;
      image->matrixCoefficients      = cap->identity
                                         ? AVIF_MATRIX_COEFFICIENTS_IDENTITY
                                         : AVIF_MATRIX_COEFFICIENTS_BT709;
      image->yuvRange = AVIF_RANGE_FULL;

#if AVIF_VERSION >= 1000000
      if(avifImageAllocatePlanes(image, AVIF_PLANES_YUV) != AVIF_RESULT_OK) continue;
#else
      avifImageAllocatePlanes(image, AVIF_PLANES_YUV);
#endif

      _fill_avif(image, r_buf, g_buf, b_buf, ow, oh, cap);

      if(encoder) { avifEncoderDestroy(encoder); encoder = NULL; }
      encoder = avifEncoderCreate();
      if(!encoder) goto out;

      encoder->codecChoice = _codec_prefs[ci].choice;
      encoder->speed       = _codec_prefs[ci].speed;
#if AVIF_VERSION >= 1000000
      encoder->quality = quality;
#else
      {
        const int q = ((100 - quality) * AVIF_QUANTIZER_WORST_QUALITY + 50) / 100;
        encoder->minQuantizer = CLAMP(q - 4, AVIF_QUANTIZER_BEST_QUALITY, AVIF_QUANTIZER_WORST_QUALITY);
        encoder->maxQuantizer = CLAMP(q + 4, AVIF_QUANTIZER_BEST_QUALITY, AVIF_QUANTIZER_WORST_QUALITY);
      }
#endif

      avifRWDataFree(&output);
      if(avifEncoderWrite(encoder, image, &output) == AVIF_RESULT_OK)
      {
        dt_print(DT_DEBUG_IMAGEIO,
                 "[proxy] '%s': %s encoder, YUV%s %d-bit %s",
                 raw_path, _codec_prefs[ci].name,
                 cap->fmt == AVIF_PIXEL_FORMAT_YUV444 ? "444" :
                 cap->fmt == AVIF_PIXEL_FORMAT_YUV422 ? "422" : "420",
                 cap->depth, cap->use_gamma ? "sRGB" : "linear");
        encoded = TRUE;
      }
      else
      {
        dt_print(DT_DEBUG_IMAGEIO, "[proxy] '%s': %s encode failed, trying next",
                 raw_path, _codec_prefs[ci].name);
      }
    }
    if(!encoded)
    {
      dt_print(DT_DEBUG_IMAGEIO, "[proxy] all encoders failed for '%s'", raw_path);
      goto out;
    }
  }

  // ---- write output file ------------------------------------------------
  {
    char proxy_path[PATH_MAX];
    if(!dt_imageio_proxy_path(raw_path, proxy_path, sizeof(proxy_path))) goto out;

    f = g_fopen(proxy_path, "wb");
    if(!f) goto out;
    if(fwrite(output.data, 1, output.size, f) != output.size)
      goto out;

    ok = TRUE;
    dt_print(DT_DEBUG_IMAGEIO, "[proxy] created '%s' (%zu KB, quality %d)",
             proxy_path, output.size / 1024, quality);
    dt_p2p_announce_proxy(raw_path);
  }

out:
  if(f) fclose(f);
  avifRWDataFree(&output);
  if(encoder) avifEncoderDestroy(encoder);
  if(image)   avifImageDestroy(image);
  free(r_buf);
  free(g_buf);
  free(b_buf);
  if(lr) libraw_close(lr);
  return ok;
}

#endif // HAVE_LIBRAW
