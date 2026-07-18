/*
    This file is part of darktable,
    Copyright (C) 2011-2026 darktable developers.

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

#include "common/darktable.h"
#include "imageio/imageio_dng.h"
#include "imageio/imageio_jpeg.h"
#include "common/colorspaces_inline_conversions.h"
#include "common/exif.h"
#include "common/image.h"
#include "develop/imageop_math.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <inttypes.h>
#include <math.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tiffio.h>

#include <jpeglib.h>

#ifdef _WIN32
#include <wchar.h>
#endif

// fallback for libtiff < 4.6
#ifndef TIFFTAG_PREVIEWCOLORSPACE
#define TIFFTAG_PREVIEWCOLORSPACE 50970
#endif

// ============================================================================
// Lossless JPEG DNG output — libtiff hijack with libjpeg-turbo encoding
// ============================================================================
//
// Adobe DNG spec lists lossless JPEG (compression code 7) as the canonical
// compression for raw integer (Bayer / Linear) DNG files; the natural
// implementation is `TIFFTAG_COMPRESSION = COMPRESSION_JPEG` + TIFFWriteTile,
// but libtiff's bundled JPEG codec hard-rejects BitsPerSample > 12 in its
// setupencode() — our raw payloads are 16-bit, so that path returns
// "BitsPerSample 16 not allowed for JPEG" and fails before the first write
//
// libjpeg-turbo (which libtiff already links) DOES support 16-bit lossless
// JPEG via jpeg_enable_lossless() + jpeg16_write_scanlines(); the job is to
// route around libtiff's codec without giving up the file-structure
// scaffolding libtiff provides (IFDs, tag bookkeeping, thumbnail SubIFDs)
//
// The trick:
//   1. Tell libtiff the file is COMPRESSION_ADOBE_DEFLATE during setup —
//      libtiff's deflate codec accepts 16-bit, so its setupencode passes
//   2. For each tile, encode our uint16 pixels to a self-contained lossless
//      JPEG datastream via _encode_tile_lossless_jpeg() (libjpeg-turbo)
//   3. Write those bytes via TIFFWriteRawTile() — libtiff stores opaque
//      bytes and records the tile offset / byte-count without invoking its
//      deflate codec at all
//   4. After TIFFClose(), reopen the file and patch the Compression tag
//      from 8 (DEFLATE) to 7 (lossless JPEG) via _patch_compression_tag()
//
// The on-disk tile data is genuine libjpeg-turbo-encoded lossless JPEG; the
// deflate codec is never invoked, it's just a setup-time lie to defeat
// libtiff's 16-bit gate. The file ends up indistinguishable from one Adobe
// DNG Converter would produce — verified by reading it back in darktable
// (rawspeed), Affinity Photo, and Apple Preview (ImageIO)
//
// If libtiff ever re-exposes TIFFRewriteField() (private since 4.0) or adds
// a "raw tile, no codec setup" mode, both the deflate pretense and the
// post-close tag patch can collapse to one libtiff call; until then the
// hijack-and-patch pair is the smallest correct dance
// ============================================================================

// DNG uses SRATIONAL / RATIONAL for matrix and WB tags. libtiff accepts
// these as float/double arrays and handles the conversion; we just pass
// the values as double

// libtiff error/warning handlers — replacing whatever else in the
// process may have installed (e.g. ImageMagick's, which crashes on
// NULL exception context). these just log to stderr and never abort
static void _dt_dng_tiff_warning(const char *module,
                                 const char *fmt, va_list ap)
{
  if(darktable.unmuted & DT_DEBUG_IMAGEIO)
  {
    fprintf(stderr, "%11.4f [imageio_dng] warning: %s: ",
            dt_get_wtime() - darktable.start_wtime,
            module ? module : "(none)");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
  }
}

static void _dt_dng_tiff_error(const char *module,
                               const char *fmt, va_list ap)
{
  fprintf(stderr, "%11.4f [imageio_dng] error: %s: ",
          dt_get_wtime() - darktable.start_wtime,
          module ? module : "(none)");
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
}

static void _install_dng_tiff_handlers(void)
{
  TIFFSetWarningHandler(_dt_dng_tiff_warning);
  TIFFSetErrorHandler(_dt_dng_tiff_error);
}

// register DNG tags that some libtiff builds lack or drop between IFDs.
// PreviewColorSpace is missing from the built-in table; CFA pattern
// tags survive IFD0 but vanish in SubIFD0 on e.g. the libtiff that
// ships with Linux Mint, breaking rawspeed re-import
static void _register_extra_dng_fields(TIFF *tif)
{
  static const TIFFFieldInfo extra[] = {
    { TIFFTAG_PREVIEWCOLORSPACE, 1, 1, TIFF_LONG, FIELD_CUSTOM,
      TRUE, FALSE, (char *)"PreviewColorSpace" },
    { TIFFTAG_CFAREPEATPATTERNDIM, 2, 2, TIFF_SHORT, FIELD_CUSTOM,
      TRUE, FALSE, (char *)"CFARepeatPatternDim" },
    { TIFFTAG_CFAPATTERN, -1, -1, TIFF_BYTE, FIELD_CUSTOM,
      TRUE, TRUE, (char *)"CFAPattern" },
  };
  TIFFMergeFieldInfo(tif, extra, sizeof(extra) / sizeof(extra[0]));
}

/* ---------- post-write COMPRESSION-tag patcher ----------------------
 * libtiff 4.x has no public TIFFRewriteField, and our libtiff hijack
 * (set up with DEFLATE so libtiff accepts 16-bit; write raw JPEG tile
 * bytes that bypass the codec) leaves COMPRESSION=8 in the file. we
 * parse the closed TIFF, locate the SubIFD0 (or IFD0 when no canonical
 * thumbnail), find tag 259 (Compression), and overwrite its 2-byte
 * SHORT value with 7 (lossless JPEG). Stable: TIFF on-disk format is
 * fixed by spec, so this won't bit-rot against libtiff updates
 */
static inline uint16_t _read_u16(const uint8_t *p, gboolean le)
{
  return le ? (uint16_t)(p[0] | (p[1] << 8))
            : (uint16_t)((p[0] << 8) | p[1]);
}

static inline uint32_t _read_u32(const uint8_t *p, gboolean le)
{
  return le
    ? (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24))
    : (uint32_t)((p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]);
}

static inline void _write_u16(uint8_t *p, uint16_t v, gboolean le)
{
  if(le) { p[0] = v & 0xff;        p[1] = (v >> 8) & 0xff; }
  else   { p[0] = (v >> 8) & 0xff; p[1] = v & 0xff; }
}

// scan an IFD at file offset `ifd_off` looking for tag `target_tag`;
// if found, return the absolute file offset of its value-field (the
// last 4 bytes of the 12-byte entry); returns 0 if not found / error
static long _find_tag_value_offset(FILE *f, uint32_t ifd_off,
                                   uint16_t target_tag, gboolean le)
{
  if(fseek(f, (long)ifd_off, SEEK_SET) != 0) return 0;
  uint8_t cnt_buf[2];
  if(fread(cnt_buf, 1, 2, f) != 2) return 0;
  const uint16_t n = _read_u16(cnt_buf, le);
  for(int i = 0; i < n; i++)
  {
    const long entry_pos = ftell(f);
    uint8_t entry[12];
    if(fread(entry, 1, 12, f) != 12) return 0;
    const uint16_t tag = _read_u16(entry, le);
    if(tag == target_tag) return entry_pos + 8;
  }
  return 0;
}

// patch the COMPRESSION tag of the raw-payload IFD to JPEG (7);
// `payload_in_subifd` selects between IFD0 (no thumbnail) and SubIFD0
// (canonical thumbnail layout); returns 0 on success
static int _patch_compression_tag(const char *filename,
                                  gboolean payload_in_subifd)
{
  FILE *f = g_fopen(filename, "r+b");
  if(!f) return 1;

  uint8_t hdr[8];
  if(fread(hdr, 1, 8, f) != 8) { fclose(f); return 1; }
  gboolean le;
  if(hdr[0] == 'I' && hdr[1] == 'I')      le = TRUE;
  else if(hdr[0] == 'M' && hdr[1] == 'M') le = FALSE;
  else { fclose(f); return 1; }
  // hdr[2..3] = magic 42 — skip
  const uint32_t ifd0_off = _read_u32(hdr + 4, le);

  uint32_t payload_ifd_off = ifd0_off;
  if(payload_in_subifd)
  {
    // find SubIFDs tag (330) in IFD0, then read the entry's count
    // field (4 bytes before the value field) and the value itself
    // (4 bytes at the value field). When count == 1, the value IS
    // the inline SubIFD0 offset (TIFF inlining rule for ≤ 4-byte
    // values). When count > 1, the value is an offset to an array
    // of LONG offsets, and we want the first array entry
    const long sub_val_off = _find_tag_value_offset(f, ifd0_off, 330, le);
    if(!sub_val_off) { fclose(f); return 1; }
    if(fseek(f, sub_val_off - 4, SEEK_SET) != 0) { fclose(f); return 1; }
    uint8_t cnt_buf[4], val_buf[4];
    if(fread(cnt_buf, 1, 4, f) != 4) { fclose(f); return 1; }
    if(fread(val_buf, 1, 4, f) != 4) { fclose(f); return 1; }
    const uint32_t sub_count = _read_u32(cnt_buf, le);
    if(sub_count == 0) { fclose(f); return 1; }
    if(sub_count == 1)
    {
      payload_ifd_off = _read_u32(val_buf, le);
    }
    else
    {
      const uint32_t arr_off = _read_u32(val_buf, le);
      if(fseek(f, (long)arr_off, SEEK_SET) != 0)
      {
        fclose(f);
        return 1;
      }
      uint8_t first[4];
      if(fread(first, 1, 4, f) != 4) { fclose(f); return 1; }
      payload_ifd_off = _read_u32(first, le);
    }
  }

  // now patch COMPRESSION in the payload IFD
  const long comp_val_off = _find_tag_value_offset(f, payload_ifd_off, 259, le);
  if(!comp_val_off) { fclose(f); return 1; }

  // confirm libtiff actually wrote DEFLATE (8) here before overwriting;
  // otherwise _find_tag_value_offset landed somewhere we shouldn't patch
  if(fseek(f, comp_val_off, SEEK_SET) != 0) { fclose(f); return 1; }
  uint8_t existing[2];
  if(fread(existing, 1, 2, f) != 2) { fclose(f); return 1; }
  if(_read_u16(existing, le) != COMPRESSION_ADOBE_DEFLATE)
  {
    fclose(f);
    return 1;
  }

  if(fseek(f, comp_val_off, SEEK_SET) != 0) { fclose(f); return 1; }
  uint8_t new_val[2];
  _write_u16(new_val, (uint16_t)7, le);  // 7 = lossless JPEG
  const size_t wrote = fwrite(new_val, 1, 2, f);
  fclose(f);
  return (wrote == 2) ? 0 : 1;
}

/* ---------- libjpeg-turbo lossless JPEG encoder ---------------------
 * libtiff's JPEG codec rejects BitsPerSample > 12, so for compressed
 * raw integer DNG (which is 14- to 16-bit per channel) we encode tiles
 * ourselves using libjpeg-turbo's lossless mode and embed the bytes
 * via the hand-rolled DNG writer (TIFF can store opaque tile data)
 *
 * Each emitted tile is a self-contained JPEG datastream (SOI..EOI),
 * which is what rawspeed's AbstractDngDecompressor::decompressThread<7>
 * expects. Predictor 1 (sample above-left), point_transform 0 (no
 * pre-shift). These match what Adobe DNG Converter emits for raw DNG
 */
struct _ljpeg_err
{
  struct jpeg_error_mgr base;
  jmp_buf jbuf;
};

static void _ljpeg_error_exit(j_common_ptr cinfo)
{
  struct _ljpeg_err *e = (struct _ljpeg_err *)cinfo->err;
  longjmp(e->jbuf, 1);
}

// encode a single tile to a self-contained lossless JPEG datastream;
// returns 0 on success; on success *out_buf is malloc'd (free via
// free(), not g_free — libjpeg-turbo's jpeg_mem_dest uses malloc)
// channels: 1 for Bayer CFA, 3 for Linear RGB
// bit_depth: 16 only today (uses jpeg16_write_scanlines / J16SAMPLE);
// to add 12-bit later, dispatch to jpeg12_write_scanlines / J12SAMPLE
// based on bit_depth — never mix the 16-bit API with data_precision < 13
static int _encode_tile_lossless_jpeg(const uint16_t *tile_data,
                                      int width,
                                      int height,
                                      int channels,
                                      int bit_depth,
                                      uint8_t **out_buf,
                                      size_t *out_len)
{
  *out_buf = NULL;
  *out_len = 0;
  if(width <= 0 || height <= 0
     || (channels != 1 && channels != 3)
     || bit_depth != 16)
    return 1;

  // zero-init cinfo so jpeg_destroy_compress is safe in the setjmp
  // path even if libjpeg longjmps before jpeg_create_compress (it
  // early-returns when cinfo->mem == NULL)
  struct jpeg_compress_struct cinfo;
  memset(&cinfo, 0, sizeof(cinfo));
  struct _ljpeg_err jerr;
  cinfo.err = jpeg_std_error(&jerr.base);
  jerr.base.error_exit = _ljpeg_error_exit;

  unsigned long buflen = 0;
  uint8_t *buf = NULL;

  if(setjmp(jerr.jbuf))
  {
    // libjpeg longjmp'd out of an internal error
    jpeg_destroy_compress(&cinfo);
    free(buf);
    return 1;
  }

  jpeg_create_compress(&cinfo);
  jpeg_mem_dest(&cinfo, &buf, &buflen);

  cinfo.image_width      = (JDIMENSION)width;
  cinfo.image_height     = (JDIMENSION)height;
  cinfo.input_components = channels;
  cinfo.in_color_space   = (channels == 1) ? JCS_GRAYSCALE : JCS_RGB;

  jpeg_set_defaults(&cinfo);
  cinfo.data_precision = bit_depth;
  // suppress the APP0/JFIF marker — JFIF is the JPEG-as-file-format
  // envelope and isn't applicable when JPEG is used purely as a
  // codec inside DNG; Adobe DNG Converter omits it too
  cinfo.write_JFIF_header = FALSE;
  // predictor 1 = use the sample to the left of current
  jpeg_enable_lossless(&cinfo, 1, 0);

  jpeg_start_compress(&cinfo, TRUE);

  // 16-bit scanline API: take J16SAMPROW = uint16_t* per row,
  // pass to jpeg16_write_scanlines one row at a time;
  // const-strip is intentional: libjpeg-turbo doesn't write to the
  // scanline buffer during compress, and the J16SAMPROW typedef
  // doesn't carry const; (void*) makes the strip explicit and
  // silences -Wcast-qual
  const size_t row_stride = (size_t)width * channels;
  while(cinfo.next_scanline < cinfo.image_height)
  {
    J16SAMPROW row =
      (J16SAMPROW)(void *)(tile_data + cinfo.next_scanline * row_stride);
    jpeg16_write_scanlines(&cinfo, &row, 1);
  }

  jpeg_finish_compress(&cinfo);
  jpeg_destroy_compress(&cinfo);

  *out_buf = buf;
  *out_len = (size_t)buflen;
  return 0;
}

// map the dcraw 2x2 CFA filters word to 4 single-byte channel indices
// for the DNG CFAPattern tag: 0=R, 1=G, 2=B, following DNG spec §A.3.1
static void _cfa_bytes_from_filters(uint32_t filters, uint8_t out[4])
{
  out[0] = FC(0, 0, filters);
  out[1] = FC(0, 1, filters);
  out[2] = FC(1, 0, filters);
  out[3] = FC(1, 1, filters);
}

// shared DNG metadata block: written on whichever IFD readers consult
// first for camera/colour information. for single-IFD layouts that's
// the raw IFD; for the canonical preview-leading layout (IFD0 = JPEG
// preview, SubIFD0 = raw) it's IFD0
static void _set_dng_shared_metadata(TIFF *tif, const dt_image_t *img)
{
  TIFFSetField(tif, TIFFTAG_XRESOLUTION, 300.0);
  TIFFSetField(tif, TIFFTAG_YRESOLUTION, 300.0);
  TIFFSetField(tif, TIFFTAG_RESOLUTIONUNIT, RESUNIT_INCH);

  gchar *software = g_strdup_printf("darktable %s", darktable_package_version);
  TIFFSetField(tif, TIFFTAG_SOFTWARE, software);
  g_free(software);

  if(img->camera_maker[0])
    TIFFSetField(tif, TIFFTAG_MAKE, img->camera_maker);
  if(img->camera_model[0])
    TIFFSetField(tif, TIFFTAG_MODEL, img->camera_model);
  if(img->camera_makermodel[0])
    TIFFSetField(tif, TIFFTAG_UNIQUECAMERAMODEL, img->camera_makermodel);

  // OriginalRawFileName: UTF-8 bytes, not zero-terminated ASCII
  if(img->filename[0])
  {
    const size_t fn_len = strlen(img->filename);
    TIFFSetField(tif, TIFFTAG_ORIGINALRAWFILENAME,
                 (uint32_t)fn_len, img->filename);
  }

  const uint8_t dng_version[4]  = { 1, 4, 0, 0 };
  const uint8_t dng_backward[4] = { 1, 2, 0, 0 };
  TIFFSetField(tif, TIFFTAG_DNGVERSION, dng_version);
  TIFFSetField(tif, TIFFTAG_DNGBACKWARDVERSION, dng_backward);

  TIFFSetField(tif, TIFFTAG_BASELINEEXPOSURE, 0.0f);

  // AsShotNeutral: inverse of wb_coeffs, normalized so max=1. omit
  // entirely when missing so the reader derives a default from
  // CalibrationIlluminant1 + ColorMatrix1 instead of a fake daylight
  if(img->wb_coeffs[0] > 0.0f
     && img->wb_coeffs[1] > 0.0f
     && img->wb_coeffs[2] > 0.0f)
  {
    float neutral[3];
    for(int i = 0; i < 3; i++) neutral[i] = 1.0f / img->wb_coeffs[i];
    const float m = fmaxf(neutral[0], fmaxf(neutral[1], neutral[2]));
    if(m > 0.0f) for(int i = 0; i < 3; i++) neutral[i] /= m;
    TIFFSetField(tif, TIFFTAG_ASSHOTNEUTRAL, 3, neutral);
  }

  // ColorMatrix1 (XYZ D65 -> cameraRGB, 3x3, row-major [camRGB][XYZ]).
  // fall back to XYZ-D65->sRGB when source has no matrix so the DNG is
  // still renderable; mirrors dt_imageio_dng_write_float
  float color_matrix[9];
  float non_zero = 0.0f;
  for(int k = 0; k < 3; k++)
    for(int i = 0; i < 3; i++)
      non_zero += fabsf(img->adobe_XYZ_to_CAM[k][i]);
  if(non_zero > 0.0f)
  {
    for(int k = 0; k < 3; k++)
      for(int i = 0; i < 3; i++)
        color_matrix[k * 3 + i] = img->adobe_XYZ_to_CAM[k][i];
  }
  else
  {
    memcpy(color_matrix, xyz_to_srgb_d65, sizeof(color_matrix));
  }
  TIFFSetField(tif, TIFFTAG_COLORMATRIX1, 9, color_matrix);
  // CalibrationIlluminant1 = D65 (21). required alongside ColorMatrix1
  // — without it strict readers (and older dt) ignore the matrix
  TIFFSetField(tif, TIFFTAG_CALIBRATIONILLUMINANT1, (uint16_t)21);
}

// Adobe DNG IFD0 thumbnail convention: ~256 px long-edge JPEG.
// decode supplied preview, downsample, re-encode. returns g_malloc'd
// bytes (caller frees) or NULL on failure
#define DNG_THUMB_MAX_DIM 256
#define DNG_THUMB_JPEG_QUALITY 85
static uint8_t *_make_thumb_jpeg(const uint8_t *src_jpeg,
                                 const int src_len,
                                 int *out_w,
                                 int *out_h,
                                 int *out_len)
{
  dt_imageio_jpeg_t jpg;
  if(dt_imageio_jpeg_decompress_header(src_jpeg, (size_t)src_len, &jpg))
    return NULL;
  if(jpg.width <= 0 || jpg.height <= 0) return NULL;

  uint8_t *rgbx = dt_alloc_align_uint8((size_t)4 * jpg.width * jpg.height);
  if(!rgbx) return NULL;
  if(dt_imageio_jpeg_decompress(&jpg, rgbx))
  {
    dt_free_align(rgbx);
    return NULL;
  }

  const int sw = jpg.width, sh = jpg.height;
  const int long_edge = sw > sh ? sw : sh;
  const int dw = (long_edge <= DNG_THUMB_MAX_DIM)
    ? sw : (sw * DNG_THUMB_MAX_DIM + long_edge / 2) / long_edge;
  const int dh = (long_edge <= DNG_THUMB_MAX_DIM)
    ? sh : (sh * DNG_THUMB_MAX_DIM + long_edge / 2) / long_edge;

  uint8_t *small = dt_alloc_align_uint8((size_t)4 * dw * dh);
  if(!small) { dt_free_align(rgbx); return NULL; }

  for(int y = 0; y < dh; y++)
  {
    const int sy0 = y * sh / dh;
    const int sy1 = (y + 1) * sh / dh;
    for(int x = 0; x < dw; x++)
    {
      const int sx0 = x * sw / dw;
      const int sx1 = (x + 1) * sw / dw;
      uint32_t r = 0, g = 0, b = 0, n = 0;
      for(int sy = sy0; sy < sy1; sy++)
        for(int sx = sx0; sx < sx1; sx++)
        {
          const uint8_t *p = rgbx + 4 * ((size_t)sy * sw + sx);
          r += p[0]; g += p[1]; b += p[2]; n++;
        }
      if(n == 0) n = 1;
      uint8_t *dst = small + 4 * ((size_t)y * dw + x);
      dst[0] = (uint8_t)(r / n);
      dst[1] = (uint8_t)(g / n);
      dst[2] = (uint8_t)(b / n);
      dst[3] = 0;
    }
  }
  dt_free_align(rgbx);

  const size_t max_out = (size_t)4 * dw * dh + 1024;
  uint8_t *jpeg = g_try_malloc(max_out);
  if(!jpeg) { dt_free_align(small); return NULL; }
  const int written = dt_imageio_jpeg_compress(small, jpeg, dw, dh,
                                               DNG_THUMB_JPEG_QUALITY);
  dt_free_align(small);
  if(written <= 0 || written > (int)max_out)
  {
    g_free(jpeg);
    return NULL;
  }
  *out_w = dw;
  *out_h = dh;
  *out_len = written;
  return jpeg;
}

// write canonical Adobe-layout IFD0: JPEG thumbnail + DNG metadata +
// SubIFD pointer table (n_subifds = 1 raw-only, or 2 with full preview)
static int _write_thumb_ifd0(TIFF *tif,
                             const dt_image_t *img,
                             const dt_imageio_dng_preview_t *p,
                             const int n_subifds)
{
  TIFFSetField(tif, TIFFTAG_SUBFILETYPE, FILETYPE_REDUCEDIMAGE);
  TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, (uint32_t)p->width);
  TIFFSetField(tif, TIFFTAG_IMAGELENGTH, (uint32_t)p->height);
  TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, (uint16_t)8);
  TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, (uint16_t)3);
  TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
  TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_YCBCR);
  TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_JPEG);
  TIFFSetField(tif, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
  TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, (uint32_t)p->height);
  TIFFSetField(tif, TIFFTAG_PREVIEWCOLORSPACE, (uint32_t)2);

  _set_dng_shared_metadata(tif, img);

  toff_t sub_offsets[2] = { 0, 0 };
  TIFFSetField(tif, TIFFTAG_SUBIFD, (uint16_t)n_subifds, sub_offsets);

  if(TIFFWriteRawStrip(tif, 0, (void *)p->data, (tmsize_t)p->len) < 0)
  {
    dt_print(DT_DEBUG_ALWAYS,
             "[imageio_dng] TIFFWriteRawStrip failed for JPEG thumbnail "
             "(%d bytes, %dx%d)", p->len, p->width, p->height);
    return 1;
  }
  if(!TIFFWriteDirectory(tif))
  {
    dt_print(DT_DEBUG_ALWAYS,
             "[imageio_dng] TIFFWriteDirectory failed for JPEG thumbnail IFD0");
    return 1;
  }
  return 0;
}

// write SubIFD1: full-res JPEG preview, no DNG tags (they live on IFD0)
static int _write_full_preview_subifd(TIFF *tif,
                                      const dt_imageio_dng_preview_t *p)
{
  TIFFSetField(tif, TIFFTAG_SUBFILETYPE, FILETYPE_REDUCEDIMAGE);
  TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, (uint32_t)p->width);
  TIFFSetField(tif, TIFFTAG_IMAGELENGTH, (uint32_t)p->height);
  TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, (uint16_t)8);
  TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, (uint16_t)3);
  TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
  TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_YCBCR);
  TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_JPEG);
  TIFFSetField(tif, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
  TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, (uint32_t)p->height);
  TIFFSetField(tif, TIFFTAG_PREVIEWCOLORSPACE, (uint32_t)2);

  if(TIFFWriteRawStrip(tif, 0, (void *)p->data, (tmsize_t)p->len) < 0)
  {
    dt_print(DT_DEBUG_ALWAYS,
             "[imageio_dng] TIFFWriteRawStrip failed for full preview "
             "(%d bytes, %dx%d)", p->len, p->width, p->height);
    return 1;
  }
  if(!TIFFWriteDirectory(tif))
  {
    dt_print(DT_DEBUG_ALWAYS,
             "[imageio_dng] TIFFWriteDirectory failed for full preview SubIFD");
    return 1;
  }
  return 0;
}

int dt_imageio_dng_write_cfa_bayer(const char *filename,
                                   const uint16_t *cfa,
                                   int width,
                                   int height,
                                   const dt_image_t *img,
                                   const void *exif_blob,
                                   int exif_len,
                                   const dt_imageio_dng_preview_t *preview,
                                   dt_imageio_dng_compress_t compress)
{
  if(!filename || !cfa || !img || width <= 0 || height <= 0)
    return 1;
  const gboolean use_lossless_jpeg
    = (compress == DT_IMAGEIO_DNG_COMPRESS_LOSSLESS_JPEG);

  _install_dng_tiff_handlers();

#ifdef _WIN32
  wchar_t *wfilename = g_utf8_to_utf16(filename, -1, NULL, NULL, NULL);
  TIFF *tif = TIFFOpenW(wfilename, "wl");
  g_free(wfilename);
#else
  TIFF *tif = TIFFOpen(filename, "wl");
#endif
  if(!tif) return 1;
  _register_extra_dng_fields(tif);

  // canonical Adobe layout: IFD0=thumb, SubIFD0=raw, SubIFD1=full preview.
  // on thumb generation failure fall back to preview-as-IFD0 (old behavior)
  const gboolean canonical = (preview && preview->data && preview->len > 0
                              && preview->width > 0 && preview->height > 0);
  uint8_t *thumb_jpeg = NULL;
  int thumb_w = 0, thumb_h = 0, thumb_len = 0;
  if(canonical)
    thumb_jpeg = _make_thumb_jpeg(preview->data, preview->len,
                                  &thumb_w, &thumb_h, &thumb_len);
  const gboolean have_thumb = (thumb_jpeg != NULL);

  if(canonical)
  {
    dt_imageio_dng_preview_t ifd0;
    if(have_thumb)
      ifd0 = (dt_imageio_dng_preview_t){
        .data = thumb_jpeg, .len = thumb_len,
        .width = thumb_w, .height = thumb_h };
    else
      ifd0 = *preview;

    if(_write_thumb_ifd0(tif, img, &ifd0, have_thumb ? 2 : 1) != 0)
    {
      dt_print(DT_DEBUG_ALWAYS,
               "[imageio_dng] write_cfa_bayer: thumbnail IFD0 failed, aborting");
      g_free(thumb_jpeg);
      TIFFClose(tif);
      g_unlink(filename);
      return 1;
    }
    // some libtiff builds drop merged field info on TIFFCreateDirectory,
    // so re-register or CFA tags are rejected as unknown on the SubIFD
    TIFFCreateDirectory(tif);
    _register_extra_dng_fields(tif);
  }

  // raw payload IFD: single IFD when no preview, otherwise SubIFD0
  TIFFSetField(tif, TIFFTAG_SUBFILETYPE, 0);
  TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, (uint32_t)width);
  TIFFSetField(tif, TIFFTAG_IMAGELENGTH, (uint32_t)height);
  TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, (uint16_t)16);
  TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, (uint16_t)1);
  TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
  TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_CFA);
  TIFFSetField(tif, TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_UINT);
  // see "Lossless JPEG DNG output" notes at top of file for the hijack
  if(use_lossless_jpeg)
  {
    TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_ADOBE_DEFLATE);
    TIFFSetField(tif, TIFFTAG_TILEWIDTH, (uint32_t)256);
    TIFFSetField(tif, TIFFTAG_TILELENGTH, (uint32_t)256);
  }
  else
  {
    TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_NONE);
    TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, TIFFDefaultStripSize(tif, 0));
  }
  TIFFSetField(tif, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
  // shared metadata only on single-IFD layout — canonical has it on IFD0
  if(!canonical)
    _set_dng_shared_metadata(tif, img);

  const int crop_x = (img->crop_x > 0) ? img->crop_x : 0;
  const int crop_y = (img->crop_y > 0) ? img->crop_y : 0;
  const int vis_w  = (img->p_width  > 0 && img->p_width  <= width  - crop_x)
                     ? img->p_width  : (width  - crop_x);
  const int vis_h  = (img->p_height > 0 && img->p_height <= height - crop_y)
                     ? img->p_height : (height - crop_y);

  // CFAPattern and BlackLevel are indexed from ActiveArea[0,0], so
  // rotate per-site arrays when ActiveArea starts on an odd row/col
  const int dy = crop_y & 1;
  const int dx = crop_x & 1;

  const uint16_t cfa_repeat_dim[2] = { 2, 2 };
  TIFFSetField(tif, TIFFTAG_CFAREPEATPATTERNDIM, cfa_repeat_dim);

  uint8_t cfa_pattern[4];
  {
    uint8_t native[4];
    _cfa_bytes_from_filters(img->buf_dsc.filters, native);
    for(int yy = 0; yy < 2; yy++)
      for(int xx = 0; xx < 2; xx++)
        cfa_pattern[yy * 2 + xx] = native[((yy + dy) & 1) * 2 + ((xx + dx) & 1)];
  }
  TIFFSetField(tif, TIFFTAG_CFAPATTERN, 4, cfa_pattern);

  const uint8_t cfa_plane_color[3] = { 0, 1, 2 };   // R, G, B
  TIFFSetField(tif, TIFFTAG_CFAPLANECOLOR, 3, cfa_plane_color);
  TIFFSetField(tif, TIFFTAG_CFALAYOUT, (uint16_t)1); // rectangular

  const uint16_t bl_repeat_dim[2] = { 2, 2 };
  TIFFSetField(tif, TIFFTAG_BLACKLEVELREPEATDIM, bl_repeat_dim);

  float black_level[4];
  const gboolean have_separate
    = (img->raw_black_level_separate[0] != 0
       || img->raw_black_level_separate[1] != 0
       || img->raw_black_level_separate[2] != 0
       || img->raw_black_level_separate[3] != 0);
  if(have_separate)
  {
    for(int yy = 0; yy < 2; yy++)
      for(int xx = 0; xx < 2; xx++)
        black_level[yy * 2 + xx] =
          (float)img->raw_black_level_separate[((yy + dy) & 1) * 2 + ((xx + dx) & 1)];
  }
  else
  {
    for(int i = 0; i < 4; i++)
      black_level[i] = (float)img->raw_black_level;
  }
  TIFFSetField(tif, TIFFTAG_BLACKLEVEL, 4, black_level);

  const uint32_t white = img->raw_white_point
    ? img->raw_white_point : 65535u;
  TIFFSetField(tif, TIFFTAG_WHITELEVEL, 1, &white);

  // advertise the visible region inside the full raw buffer; without
  // these tags the importer renders the optical-black margins too

  const uint32_t active_area[4] = {
    (uint32_t)crop_y, (uint32_t)crop_x,
    (uint32_t)(crop_y + vis_h), (uint32_t)(crop_x + vis_w),
  };
  const float default_scale[2] = { 1.0f, 1.0f };
  const float default_crop_origin[2] = { 0.0f, 0.0f };
  const float default_crop_size[2] = { (float)vis_w, (float)vis_h };
  TIFFSetField(tif, TIFFTAG_ACTIVEAREA, active_area);
  TIFFSetField(tif, TIFFTAG_DEFAULTSCALE, default_scale);
  TIFFSetField(tif, TIFFTAG_DEFAULTCROPORIGIN, default_crop_origin);
  TIFFSetField(tif, TIFFTAG_DEFAULTCROPSIZE, default_crop_size);

  int res = 0;
  if(use_lossless_jpeg)
  {
    // tile-by-tile lossless JPEG; 256×256 is a multiple of 2 (CFA grid)
    // and 16 (JPEG MCU); partial edge tiles are zero-padded; the
    // ActiveArea metadata above keeps the importer inside width×height
    const int TS = 256;
    const int tiles_x = (width + TS - 1) / TS;
    const int tiles_y = (height + TS - 1) / TS;
    uint16_t *tile_buf = g_malloc0((size_t)TS * TS * sizeof(uint16_t));
    if(!tile_buf) res = 1;
    int tile_idx = 0;
    for(int ty = 0; ty < tiles_y && res == 0; ty++)
    {
      for(int tx = 0; tx < tiles_x && res == 0; tx++)
      {
        const int x0 = tx * TS;
        const int y0 = ty * TS;
        const int copy_h = MIN(TS, height - y0);
        const int copy_w = MIN(TS, width - x0);
        memset(tile_buf, 0, (size_t)TS * TS * sizeof(uint16_t));
        for(int yy = 0; yy < copy_h; yy++)
        {
          const uint16_t *src = cfa + (size_t)(y0 + yy) * width + x0;
          memcpy(tile_buf + (size_t)yy * TS, src,
                 (size_t)copy_w * sizeof(uint16_t));
        }
        uint8_t *jpeg_bytes = NULL;
        size_t jpeg_len = 0;
        if(_encode_tile_lossless_jpeg(tile_buf, TS, TS, 1, 16,
                                      &jpeg_bytes, &jpeg_len) != 0)
        {
          res = 1;
          break;
        }
        if(TIFFWriteRawTile(tif, tile_idx, jpeg_bytes,
                            (tmsize_t)jpeg_len) < 0)
          res = 1;
        free(jpeg_bytes);
        tile_idx++;
      }
    }
    g_free(tile_buf);

    // libtiff stored COMPRESSION=8 (DEFLATE) during setup; patch it
    // to 7 (lossless JPEG) post-close via _patch_compression_tag,
    // since libtiff 4.x has no public TIFFRewriteField equivalent
  }
  else
  {
    for(int y = 0; y < height && res == 0; y++)
    {
      const uint16_t *row = cfa + (size_t)y * width;
      if(TIFFWriteScanline(tif, (void *)row, y, 0) < 0)
        res = 1;
    }
  }

  // SubIFD1 = full-res preview when we have both thumb and original
  if(res == 0 && have_thumb)
  {
    if(!TIFFWriteDirectory(tif))
    {
      dt_print(DT_DEBUG_ALWAYS,
               "[imageio_dng] TIFFWriteDirectory failed closing raw SubIFD0");
      res = 1;
    }
    else
    {
      TIFFCreateDirectory(tif);
      _register_extra_dng_fields(tif);
      if(_write_full_preview_subifd(tif, preview) != 0) res = 1;
    }
  }

  TIFFClose(tif);
  g_free(thumb_jpeg);

  // patch Compression tag 8 (DEFLATE) → 7 (lossless JPEG); see notes
  // at top of file
  if(res == 0 && use_lossless_jpeg)
  {
    if(_patch_compression_tag(filename, canonical) != 0)
    {
      dt_print(DT_DEBUG_ALWAYS,
               "[imageio_dng] post-write COMPRESSION patch failed");
      res = 1;
    }
  }

  // embed source EXIF (datetime, ISO, shutter, etc.)
  // dt_exif_write_blob takes a non-const pointer; we don't modify it
  if(res == 0 && exif_blob && exif_len > 0)
    dt_exif_write_blob((uint8_t *)exif_blob, (uint32_t)exif_len,
                       filename, 0);

  if(res != 0)
    g_unlink(filename);

  return res;
}

int dt_imageio_dng_write_linear(const char *filename,
                                const float *rgb,
                                int width,
                                int height,
                                const dt_image_t *img,
                                const void *exif_blob,
                                int exif_len,
                                const dt_imageio_dng_preview_t *preview,
                                dt_imageio_dng_compress_t compress)
{
  if(!filename || !rgb || !img || width <= 0 || height <= 0)
    return 1;
  const gboolean use_lossless_jpeg
    = (compress == DT_IMAGEIO_DNG_COMPRESS_LOSSLESS_JPEG);

  _install_dng_tiff_handlers();

#ifdef _WIN32
  wchar_t *wfilename = g_utf8_to_utf16(filename, -1, NULL, NULL, NULL);
  TIFF *tif = TIFFOpenW(wfilename, "wl");
  g_free(wfilename);
#else
  TIFF *tif = TIFFOpen(filename, "wl");
#endif
  if(!tif) return 1;
  _register_extra_dng_fields(tif);

  // canonical layout when a preview is provided (see write_cfa_bayer)
  const gboolean canonical = (preview && preview->data && preview->len > 0
                              && preview->width > 0 && preview->height > 0);
  uint8_t *thumb_jpeg = NULL;
  int thumb_w = 0, thumb_h = 0, thumb_len = 0;
  if(canonical)
    thumb_jpeg = _make_thumb_jpeg(preview->data, preview->len,
                                  &thumb_w, &thumb_h, &thumb_len);
  const gboolean have_thumb = (thumb_jpeg != NULL);

  if(canonical)
  {
    dt_imageio_dng_preview_t ifd0;
    if(have_thumb)
      ifd0 = (dt_imageio_dng_preview_t){
        .data = thumb_jpeg, .len = thumb_len,
        .width = thumb_w, .height = thumb_h };
    else
      ifd0 = *preview;

    if(_write_thumb_ifd0(tif, img, &ifd0, have_thumb ? 2 : 1) != 0)
    {
      dt_print(DT_DEBUG_ALWAYS,
               "[imageio_dng] write_linear: thumbnail IFD0 failed, aborting");
      g_free(thumb_jpeg);
      TIFFClose(tif);
      g_unlink(filename);
      return 1;
    }
    // see comment in write_cfa_bayer
    TIFFCreateDirectory(tif);
    _register_extra_dng_fields(tif);
  }

  // baseline TIFF tags, 3 samples per pixel (demosaicked)
  TIFFSetField(tif, TIFFTAG_SUBFILETYPE, 0);
  TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, (uint32_t)width);
  TIFFSetField(tif, TIFFTAG_IMAGELENGTH, (uint32_t)height);
  TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, (uint16_t)16);
  TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, (uint16_t)3);
  TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
  TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, 34892);  // LinearRaw
  TIFFSetField(tif, TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_UINT);
  // see "Lossless JPEG DNG output" notes at top of file for the hijack
  if(use_lossless_jpeg)
  {
    TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_ADOBE_DEFLATE);
    TIFFSetField(tif, TIFFTAG_TILEWIDTH, (uint32_t)256);
    TIFFSetField(tif, TIFFTAG_TILELENGTH, (uint32_t)256);
  }
  else
  {
    TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_NONE);
    TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, TIFFDefaultStripSize(tif, 0));
  }
  TIFFSetField(tif, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
  if(!canonical)
    _set_dng_shared_metadata(tif, img);

  // NO CFA tags: this is demosaicked data.
  //     encode as normalized: BlackLevel=0, WhiteLevel=65535. the
  //     pixel data is already un-WB'd camRGB in [0, 1] range (the
  //     raw_restore_linear pipeline does matrix + un-boost + un-WB
  //     before handing off). the consumer applies WB via
  //     AsShotNeutral, reads uint16 as [0, 65535] and normalizes to
  //     [0, 1] via black/white
  const uint32_t white_norm = 65535u;
  const float black3[3] = { 0.0f, 0.0f, 0.0f };
  TIFFSetField(tif, TIFFTAG_BLACKLEVEL, 3, black3);
  TIFFSetField(tif, TIFFTAG_WHITELEVEL, 1, &white_norm);

  // linear DNG: buffer is already at visible dims (post-demosaic);
  // ACTIVEAREA covers the full buffer, no margin to crop
  const uint32_t active_area[4] = {
    0, 0, (uint32_t)height, (uint32_t)width,
  };
  const float default_scale[2] = { 1.0f, 1.0f };
  const float default_crop_origin[2] = { 0.0f, 0.0f };
  const float default_crop_size[2] = { (float)width, (float)height };
  TIFFSetField(tif, TIFFTAG_ACTIVEAREA, active_area);
  TIFFSetField(tif, TIFFTAG_DEFAULTSCALE, default_scale);
  TIFFSetField(tif, TIFFTAG_DEFAULTCROPORIGIN, default_crop_origin);
  TIFFSetField(tif, TIFFTAG_DEFAULTCROPSIZE, default_crop_size);

  // float [0, 1] normalized camRGB -> uint16 [0, 65535]. BlackLevel=0
  // and WhiteLevel=65535 let the re-importer recover the [0, 1] range
  // via the standard raw normalization (val - black) / (white - black)
  const float clip_hi = 65535.0f;
  int res = 0;
  if(use_lossless_jpeg)
  {
    // tile-by-tile lossless JPEG, 256×256, 3-channel RGB
    const int TS = 256;
    const int tiles_x = (width + TS - 1) / TS;
    const int tiles_y = (height + TS - 1) / TS;
    uint16_t *tile_buf
      = g_malloc0((size_t)TS * TS * 3 * sizeof(uint16_t));
    if(!tile_buf) res = 1;
    int tile_idx = 0;
    for(int ty = 0; ty < tiles_y && res == 0; ty++)
    {
      for(int tx = 0; tx < tiles_x && res == 0; tx++)
      {
        const int x0 = tx * TS;
        const int y0 = ty * TS;
        const int copy_h = MIN(TS, height - y0);
        const int copy_w = MIN(TS, width - x0);
        memset(tile_buf, 0,
               (size_t)TS * TS * 3 * sizeof(uint16_t));
        for(int yy = 0; yy < copy_h; yy++)
        {
          const float *src = rgb + (size_t)(y0 + yy) * width * 3
                                 + (size_t)x0 * 3;
          uint16_t *dst = tile_buf + (size_t)yy * TS * 3;
          for(int xx = 0; xx < copy_w; xx++)
          {
            for(int c = 0; c < 3; c++)
            {
              float adc = src[xx * 3 + c] * 65535.0f;
              if(adc < 0.0f) adc = 0.0f;
              if(adc > clip_hi) adc = clip_hi;
              dst[xx * 3 + c] = (uint16_t)(adc + 0.5f);
            }
          }
        }
        uint8_t *jpeg_bytes = NULL;
        size_t jpeg_len = 0;
        if(_encode_tile_lossless_jpeg(tile_buf, TS, TS, 3, 16,
                                      &jpeg_bytes, &jpeg_len) != 0)
        {
          res = 1;
          break;
        }
        if(TIFFWriteRawTile(tif, tile_idx, jpeg_bytes,
                            (tmsize_t)jpeg_len) < 0)
          res = 1;
        free(jpeg_bytes);
        tile_idx++;
      }
    }
    g_free(tile_buf);
  }
  else
  {
    uint16_t *scan = g_malloc((size_t)width * 3 * sizeof(uint16_t));
    if(!scan)
    {
      TIFFClose(tif);
      g_unlink(filename);
      return 1;
    }
    for(int y = 0; y < height && res == 0; y++)
    {
      const float *row = rgb + (size_t)y * width * 3;
      for(int x = 0; x < width; x++)
      {
        for(int c = 0; c < 3; c++)
        {
          float adc = row[x * 3 + c] * 65535.0f;
          if(adc < 0.0f) adc = 0.0f;
          if(adc > clip_hi) adc = clip_hi;
          scan[x * 3 + c] = (uint16_t)(adc + 0.5f);
        }
      }
      if(TIFFWriteScanline(tif, scan, y, 0) < 0) res = 1;
    }
    g_free(scan);
  }

  // SubIFD1 = full-res preview when we have both thumb and original
  if(res == 0 && have_thumb)
  {
    if(!TIFFWriteDirectory(tif))
    {
      dt_print(DT_DEBUG_ALWAYS,
               "[imageio_dng] TIFFWriteDirectory failed closing linear SubIFD0");
      res = 1;
    }
    else
    {
      TIFFCreateDirectory(tif);
      _register_extra_dng_fields(tif);
      if(_write_full_preview_subifd(tif, preview) != 0) res = 1;
    }
  }

  TIFFClose(tif);
  g_free(thumb_jpeg);

  // patch Compression 8 → 7; see notes at top of file
  if(res == 0 && use_lossless_jpeg)
  {
    if(_patch_compression_tag(filename, canonical) != 0)
    {
      dt_print(DT_DEBUG_ALWAYS,
               "[imageio_dng] post-write COMPRESSION patch failed");
      res = 1;
    }
  }

  if(res == 0 && exif_blob && exif_len > 0)
    dt_exif_write_blob((uint8_t *)exif_blob, (uint32_t)exif_len,
                       filename, 0);

  if(res != 0)
    g_unlink(filename);

  return res;
}

// =============================================================================
// dt_imageio_dng_write_float — hand-rolled byte-level TIFF/DNG writer
//
// Used by HDR merge. Writes a 32-bit float CFA DNG (Bayer or X-Trans).
// The helpers and macros below are private to this writer and are not
// shared with the libtiff-based uint16 writers above
// =============================================================================

// TIFF type codes (libtiff knows these natively, so the uint16 writers
// above don't need them)
#define BYTE 1
#define ASCII 2
#define SHORT 3
#define LONG 4
#define RATIONAL 5
#define SRATIONAL 10

#define HEADBUFFSIZE 1024

static inline void _imageio_dng_write_buf(uint8_t *buf, const uint32_t d, const int val)
{
  if(d + 4 >= HEADBUFFSIZE) return;
  buf[d] = val & 0xff;
  buf[d + 1] = (val >> 8) & 0xff;
  buf[d + 2] = (val >> 16) & 0xff;
  buf[d + 3] = val >> 24;
}

static inline int _imageio_dng_make_tag(
    const uint16_t tag,
    const uint16_t type,
    const uint32_t lng,
    const uint32_t fld,
    uint8_t *buf,
    const uint32_t b,
    uint8_t *cnt)
{
  if(b + 12 < HEADBUFFSIZE)
  {
    _imageio_dng_write_buf(buf, b, (type << 16) | tag);
    _imageio_dng_write_buf(buf, b+4, lng);
    _imageio_dng_write_buf(buf, b+8, fld);
    *cnt = *cnt + 1;
  }
  return b + 12;
}

static inline void _imageio_dng_write_tiff_header(
    FILE *fp,
    uint32_t xs,
    uint32_t ys,
    float Tv,
    float Av,
    float f,
    float iso,
    uint32_t filter,
    const uint8_t xtrans[6][6],
    const float whitelevel,
    const dt_aligned_pixel_t wb_coeffs,
    const float adobe_XYZ_to_CAM[4][3])
{
  const uint32_t channels = 1;
  uint8_t buf[HEADBUFFSIZE];
  uint8_t cnt = 0;

  // this matrix is generic for XYZ->sRGB / D65
  int m[9] = { 3240454, -1537138, -498531, -969266, 1876010, 41556, 55643, -204025, 1057225 };
  int den = 1000000;

  memset(buf, 0, sizeof(buf));
  /* TIFF file header, little-endian */
  buf[0] = 0x49;
  buf[1] = 0x49;
  buf[2] = 0x2a;
  buf[4] = 8;

  // If you want to add other tags written to a dng file include the the ID in the enum to
  // keep track of written tags so we don't a) have leaks or b) overwrite anything in data section
  const int first_tag = __LINE__ + 3;
  enum write_tags
  {
    EXIF_TAG_SUBFILE = 254,           /* New subfile type.  */
    EXIF_TAG_IMGWIDTH = 256,          /* Image width.  */
    EXIF_TAG_IMGLENGTH = 257,         /* Image length.  */
    EXIF_TAG_BPS = 258,               /* Bits per sample: 32-bit float */
    EXIF_TAG_COMPRESS = 259,          /* Compression.  */
    EXIF_TAG_PHOTOMINTREP = 262,      /* Photo interp: CFA  */
    EXIF_TAG_STRIP_OFFSET = 273,      /* Strip offset.  */
    EXIF_TAG_ORIENTATION = 274,       /* Orientation. */
    EXIF_TAG_SAMPLES_PER_PIXEL = 277, /* Samples per pixel.  */
    EXIF_TAG_ROWS_PER_STRIP = 278,    /* Rows per strip.  */
    EXIF_TAG_STRIP_BCOUNT = 279,      /* Strip byte count.  */
    EXIF_TAG_PLANAR_CONFIG = 284,     /* Planar configuration.  */
    EXIF_TAG_SAMPLE_FORMAT = 339,     /* SampleFormat = 3 => ieee floating point */
    EXIF_TAG_REPEAT_PATTERN = 33421,  /* pattern repeat */
    EXIF_TAG_SENS_PATTERN = 33422,    /* sensor pattern */
    EXIF_TAG_VERSION = 50706,         /* DNG Version */
    EXIF_TAG_WHITE_LEVEL = 50717,     /* White level */
    EXIF_TAG_COLOR_MATRIX1 = 50721,   /* ColorMatrix1 (XYZ->native cam) */
    EXIF_TAG_SHOT_NEUTRAL = 50728,    /* AsShotNeutral for rawspeed Dngdecoder camera white balance */
    EXIF_TAG_ILLUMINANT1 = 50778,     /* CalibrationIlluminant1 */
  };
  buf[8] = (uint8_t)(__LINE__ - first_tag - 1); /* number of entries */

  uint32_t b = 10;
  uint32_t data = 10 + buf[8] * 12 + 4; // takes care of the header, entries, and termination

  b = _imageio_dng_make_tag(EXIF_TAG_SUBFILE, LONG, 1, 0, buf, b, &cnt);
  b = _imageio_dng_make_tag(EXIF_TAG_IMGWIDTH, LONG, 1, xs, buf, b, &cnt);
  b = _imageio_dng_make_tag(EXIF_TAG_IMGLENGTH, LONG, 1, ys, buf, b, &cnt);
  b = _imageio_dng_make_tag(EXIF_TAG_BPS, SHORT, 1, 32, buf, b, &cnt);
  b = _imageio_dng_make_tag(EXIF_TAG_COMPRESS, SHORT, 1, 1, buf, b, &cnt);
  b = _imageio_dng_make_tag(EXIF_TAG_PHOTOMINTREP, SHORT, 1, 32803, buf, b, &cnt);
  b = _imageio_dng_make_tag(EXIF_TAG_STRIP_OFFSET, LONG, 1, 0, buf, b, &cnt);
  uint32_t ofst = b - 4; /* remember buffer address for updating strip offset later */
  b = _imageio_dng_make_tag(EXIF_TAG_ORIENTATION, SHORT, 1, 1, buf, b, &cnt);
  b = _imageio_dng_make_tag(EXIF_TAG_SAMPLES_PER_PIXEL, SHORT, 1, channels, buf, b, &cnt);
  b = _imageio_dng_make_tag(EXIF_TAG_ROWS_PER_STRIP, LONG, 1, ys, buf, b, &cnt);
  b = _imageio_dng_make_tag(EXIF_TAG_STRIP_BCOUNT, LONG, 1, (ys * xs * channels*4), buf, b, &cnt);
  b = _imageio_dng_make_tag(EXIF_TAG_PLANAR_CONFIG, SHORT, 1, 1, buf, b, &cnt);
  b = _imageio_dng_make_tag(EXIF_TAG_SAMPLE_FORMAT, SHORT, 1, 3, buf, b, &cnt);

  if(filter == 9u) // xtrans
    b = _imageio_dng_make_tag(EXIF_TAG_REPEAT_PATTERN, SHORT, 2, (6 << 16) | 6, buf, b, &cnt);
  else
    b = _imageio_dng_make_tag(EXIF_TAG_REPEAT_PATTERN, SHORT, 2, (2 << 16) | 2, buf, b, &cnt);

  uint32_t cfapattern = 0;
  switch(filter)
  {
    case 0x94949494:
      cfapattern = (2 << 24) | (1 << 16) | (1 << 8) | 0; // rggb
      break;
    case 0x49494949:
      cfapattern = (1 << 24) | (0 << 16) | (2 << 8) | 1; // gbrg
      break;
    case 0x61616161:
      cfapattern = (1 << 24) | (2 << 16) | (0 << 8) | 1; // grbg
      break;
    default:                                             // case 0x16161616:
      cfapattern = (0 << 24) | (1 << 16) | (1 << 8) | 2; // bggr
      break;
  }

  if(filter == 9u) // xtrans
  {
    b = _imageio_dng_make_tag(EXIF_TAG_SENS_PATTERN, BYTE, 36, data, buf, b, &cnt); /* xtrans PATTERN */
    // apparently this doesn't need byteswap:
    memcpy(buf + data, xtrans, sizeof(uint8_t)*36);
    data += 36;
  }
  else // bayer
    b = _imageio_dng_make_tag(EXIF_TAG_SENS_PATTERN, BYTE, 4, cfapattern, buf, b, &cnt); /* bayer PATTERN */

  b = _imageio_dng_make_tag(EXIF_TAG_VERSION, BYTE, 4, 1 | (4 << 8), buf, b, &cnt);

  // WhiteLevel is straight integer even for float DNGs
  b = _imageio_dng_make_tag(EXIF_TAG_WHITE_LEVEL, LONG, 1, (uint32_t)roundf(whitelevel), buf, b, &cnt);

  // ColorMatrix1 try to get camera matrix else m[k] like before
  if(dt_is_valid_colormatrix(adobe_XYZ_to_CAM[0][0]))
  {
    den = 10000;
    for(int k= 0; k < 3; k++)
      for(int i= 0; i < 3; i++)
        m[k*3+i] = roundf(adobe_XYZ_to_CAM[k][i] * den);
  }
  b = _imageio_dng_make_tag(EXIF_TAG_COLOR_MATRIX1, SRATIONAL, 9, data, buf, b, &cnt); /* ColorMatrix1 (XYZ->native cam) */
  for(int k = 0; k < 9; k++)
  {
    _imageio_dng_write_buf(buf, data + k*8, m[k]);
    _imageio_dng_write_buf(buf, data+4 + k*8, den);
  }
  data += 9 * 8;

  b = _imageio_dng_make_tag(EXIF_TAG_SHOT_NEUTRAL, RATIONAL, 3, data, buf, b, &cnt);
  den = 1000000;
  for(int k = 0; k < 3; k++)
  {
    const float coeff = roundf(((float)den * wb_coeffs[1]) / wb_coeffs[k]);
    _imageio_dng_write_buf(buf, data + k*8, (int)coeff);
    _imageio_dng_write_buf(buf, data+4 + k*8, den);
  }
  data += 3 * 8;

  b = _imageio_dng_make_tag(EXIF_TAG_ILLUMINANT1, SHORT, 1, DT_LS_D65, buf, b, &cnt);

  // We have all tags using data now written so we can finally use strip offset
  _imageio_dng_write_buf(buf, ofst, data);

  /* Termination is implicit: next IFD already 0 when buf initialized */

  if(buf[8] != cnt)
  {
    dt_print(DT_DEBUG_ALWAYS, "[dng_write_header] can't write valid header, unexpected number of entries!");
    return;
  }

  if(data >= HEADBUFFSIZE)
  {
    dt_print(DT_DEBUG_ALWAYS, "[dng_write_header] can't write valid header as it exceeds buffer size!");
    return;
  }

  // exif is written later, by exiv2:
  const int written = fwrite(buf, 1, data, fp);
  if(written != data) dt_print(DT_DEBUG_ALWAYS, "[dng_write_header] failed to write image header!");
}


void dt_imageio_dng_write_float(
    const char *filename, const float *const pixel, const int wd,
    const int ht, void *exif, const int exif_len, const uint32_t filter,
    const uint8_t xtrans[6][6],
    const float whitelevel,
    const dt_aligned_pixel_t wb_coeffs,
    const float adobe_XYZ_to_CAM[4][3])
{
  FILE *f = g_fopen(filename, "wb");
  if(f)
  {
    _imageio_dng_write_tiff_header(f, wd, ht, 1.0f / 100.0f, 1.0f / 4.0f, 50.0f, 100.0f,
                                     filter, xtrans, whitelevel, wb_coeffs, adobe_XYZ_to_CAM);
    const int k = fwrite(pixel, sizeof(float), (size_t)wd * ht, f);
    if(k != wd * ht) dt_print(DT_DEBUG_ALWAYS, "[dng_write] Error writing image data to %s", filename);
    fclose(f);
    if(exif) dt_exif_write_blob(exif, exif_len, filename, 0);
  }
}

#undef BYTE
#undef ASCII
#undef SHORT
#undef LONG
#undef RATIONAL
#undef SRATIONAL
#undef HEADBUFFSIZE

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
