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

  // Work in 2×2 blocks; ensure even dimensions
  const int bw = (iw / 2) * 2;
  const int bh = (ih / 2) * 2;
  const int ow = bw / 2;
  const int oh = bh / 2;

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

  // ---- encode AVIF with identity matrix ----------------------------------
  // YUV 4:4:4 + AVIF_MATRIX_COEFFICIENTS_IDENTITY (MC=0): Y=G, Cb=B, Cr=R.
  // Unspecified primaries + linear transfer → camera-native linear light.
  image = avifImageCreate(ow, oh, 12, AVIF_PIXEL_FORMAT_YUV444);
  if(!image) goto out;

  image->colorPrimaries          = AVIF_COLOR_PRIMARIES_UNSPECIFIED;
  image->transferCharacteristics = AVIF_TRANSFER_CHARACTERISTICS_LINEAR;
  image->matrixCoefficients      = AVIF_MATRIX_COEFFICIENTS_IDENTITY;
  image->yuvRange                = AVIF_RANGE_FULL;

#if AVIF_VERSION >= 1000000
  if(avifImageAllocatePlanes(image, AVIF_PLANES_YUV) != AVIF_RESULT_OK) goto out;
#else
  avifImageAllocatePlanes(image, AVIF_PLANES_YUV);
#endif

  {
    // libavif identity matrix (ITU-T H.273 MC=0): Y=G, Cb=B, Cr=R.
    // avifImageYUVToRGB with identity gives R=Cr, G=Y, B=Cb.
    const int ystride = (int)(image->yuvRowBytes[AVIF_CHAN_Y]  / sizeof(uint16_t));
    const int ustride = (int)(image->yuvRowBytes[AVIF_CHAN_U]  / sizeof(uint16_t));
    const int vstride = (int)(image->yuvRowBytes[AVIF_CHAN_V]  / sizeof(uint16_t));
    uint16_t *yp = (uint16_t *)image->yuvPlanes[AVIF_CHAN_Y];
    uint16_t *up = (uint16_t *)image->yuvPlanes[AVIF_CHAN_U];
    uint16_t *vp = (uint16_t *)image->yuvPlanes[AVIF_CHAN_V];
    for(int y = 0; y < oh; y++)
    {
      memcpy(yp + y * ystride, g_buf + y * ow, ow * sizeof(uint16_t));  // Y  ← G
      memcpy(up + y * ustride, b_buf + y * ow, ow * sizeof(uint16_t));  // Cb ← B
      memcpy(vp + y * vstride, r_buf + y * ow, ow * sizeof(uint16_t));  // Cr ← R
    }
  }

  // ---- adaptive quality based on mean image brightness ---------------------
  // Dark images have low signal-to-quantization-noise ratio: a coarse quantizer
  // produces visible banding in the shadows. We raise quality toward 99 as the
  // scene gets darker.  The user's setting acts as the floor for bright images.
  //
  // Calibration (mean G brightness normalised to [0,1]):
  //   ≤ 0.03  (very dark, e.g. night/indoor)  → quality 99
  //   ≥ 0.30  (typical outdoor daylight)       → quality = user setting
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

  encoder = avifEncoderCreate();
  if(!encoder) goto out;

  // Pick the fastest encoder the installed libavif supports.
  // SVT-AV1 is 5-10x faster than libaom at equivalent quality; prefer it when
  // available.  Fall back to AOM (always present) otherwise.
  if(avifCodecName(AVIF_CODEC_CHOICE_SVT, AVIF_CODEC_FLAG_CAN_ENCODE))
  {
    encoder->codecChoice = AVIF_CODEC_CHOICE_SVT;
    // SVT preset: 0=slowest … 13=fastest.  Preset 6 is a good quality/speed
    // balance (same numeric value as our AOM setting, different meaning).
    encoder->speed = 6;
    dt_print(DT_DEBUG_IMAGEIO, "[proxy] using SVT-AV1 encoder (speed 6)");
  }
  else
  {
    encoder->codecChoice = AVIF_CODEC_CHOICE_AOM;
    // AOM speed 6 is the "good" preset: proper RD decisions and adaptive block
    // partitioning.  Speed 10 (FASTEST) produces poor shadow quality.
    encoder->speed = 6;
    dt_print(DT_DEBUG_IMAGEIO, "[proxy] using AOM encoder (speed 6)");
  }

#if AVIF_VERSION >= 1000000
  encoder->quality = quality;
#else
  {
    const int q = ((100 - quality) * AVIF_QUANTIZER_WORST_QUALITY + 50) / 100;
    encoder->minQuantizer = CLAMP(q - 4, AVIF_QUANTIZER_BEST_QUALITY, AVIF_QUANTIZER_WORST_QUALITY);
    encoder->maxQuantizer = CLAMP(q + 4, AVIF_QUANTIZER_BEST_QUALITY, AVIF_QUANTIZER_WORST_QUALITY);
  }
#endif

  if(avifEncoderWrite(encoder, image, &output) != AVIF_RESULT_OK)
  {
    dt_print(DT_DEBUG_IMAGEIO, "[proxy] avifEncoderWrite failed for '%s'", raw_path);
    goto out;
  }

  // ---- write output file ------------------------------------------------
  {
    char *proxy_path = g_strdup_printf("%s.proxy.avif", raw_path);
    f = g_fopen(proxy_path, "wb");
    g_free(proxy_path);
    if(!f) goto out;
    if(fwrite(output.data, 1, output.size, f) != output.size)
      goto out;
  }

  ok = TRUE;
  dt_print(DT_DEBUG_IMAGEIO,
           "[proxy] created proxy for '%s' (%zu KB, quality %d)",
           raw_path, output.size / 1024, quality);

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
