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

#include "imageio/imageio_dng.h"
#include "common/darktable.h"
#include "common/exif.h"
#include "common/image.h"
#include "develop/imageop_math.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tiffio.h>

#ifdef _WIN32
#include <wchar.h>
#endif

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

  const uint8_t dng_version[4]  = { 1, 4, 0, 0 };
  const uint8_t dng_backward[4] = { 1, 2, 0, 0 };
  TIFFSetField(tif, TIFFTAG_DNGVERSION, dng_version);
  TIFFSetField(tif, TIFFTAG_DNGBACKWARDVERSION, dng_backward);

  // AsShotNeutral: inverse of wb_coeffs, normalized so max=1. fallback
  // to neutral [1,1,1] when wb_coeffs missing so the tag is always set
  float neutral[3] = { 1.0f, 1.0f, 1.0f };
  if(img->wb_coeffs[0] > 0.0f
     && img->wb_coeffs[1] > 0.0f
     && img->wb_coeffs[2] > 0.0f)
  {
    for(int i = 0; i < 3; i++) neutral[i] = 1.0f / img->wb_coeffs[i];
    const float m = fmaxf(neutral[0], fmaxf(neutral[1], neutral[2]));
    if(m > 0.0f) for(int i = 0; i < 3; i++) neutral[i] /= m;
  }
  TIFFSetField(tif, TIFFTAG_ASSHOTNEUTRAL, 3, neutral);

  // ColorMatrix1 (XYZ D50 -> cameraRGB, 3x3). row-major [camRGB][XYZ]
  // matches darktable's adobe_XYZ_to_CAM layout exactly
  float non_zero = 0.0f;
  for(int k = 0; k < 3; k++)
    for(int i = 0; i < 3; i++)
      non_zero += fabsf(img->adobe_XYZ_to_CAM[k][i]);
  if(non_zero > 0.0f)
  {
    float color_matrix[9];
    for(int k = 0; k < 3; k++)
      for(int i = 0; i < 3; i++)
        color_matrix[k * 3 + i] = img->adobe_XYZ_to_CAM[k][i];
    TIFFSetField(tif, TIFFTAG_COLORMATRIX1, 9, color_matrix);
  }
}

// write IFD0 as the canonical Adobe-layout JPEG preview: small YCbCr
// thumbnail + shared DNG metadata + SubIFD pointer to the raw payload
// that the caller will write next. caller must follow with
// TIFFCreateDirectory + raw-IFD population + TIFFWriteDirectory
static int _write_jpeg_preview_ifd(TIFF *tif,
                                   const dt_image_t *img,
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

  _set_dng_shared_metadata(tif, img);

  // SubIFD pointer with one slot. libtiff fills the actual offset
  // when the SubIFD is later written via TIFFCreateDirectory + ...
  toff_t sub_offsets[1] = { 0 };
  TIFFSetField(tif, TIFFTAG_SUBIFD, (uint16_t)1, sub_offsets);

  // pre-encoded JPEG written as a single raw strip (libtiff does not
  // re-encode when COMPRESSION_JPEG is paired with TIFFWriteRawStrip)
  if(TIFFWriteRawStrip(tif, 0, (void *)p->data, (tmsize_t)p->len) < 0)
  {
    dt_print(DT_DEBUG_ALWAYS,
             "[imageio_dng] TIFFWriteRawStrip failed for JPEG preview "
             "(%d bytes, %dx%d)", p->len, p->width, p->height);
    return 1;
  }
  if(!TIFFWriteDirectory(tif))
  {
    dt_print(DT_DEBUG_ALWAYS,
             "[imageio_dng] TIFFWriteDirectory failed for JPEG preview IFD0");
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
                                   const dt_imageio_dng_preview_t *preview)
{
  if(!filename || !cfa || !img || width <= 0 || height <= 0)
    return 1;

  _install_dng_tiff_handlers();

#ifdef _WIN32
  wchar_t *wfilename = g_utf8_to_utf16(filename, -1, NULL, NULL, NULL);
  TIFF *tif = TIFFOpenW(wfilename, "wl");
  g_free(wfilename);
#else
  TIFF *tif = TIFFOpen(filename, "wl");
#endif
  if(!tif) return 1;

  // canonical Adobe layout when a preview is provided: IFD0 holds the
  // JPEG thumbnail + DNG identification metadata, the raw payload
  // moves into SubIFD0
  const gboolean canonical = (preview && preview->data && preview->len > 0
                              && preview->width > 0 && preview->height > 0);
  if(canonical)
  {
    if(_write_jpeg_preview_ifd(tif, img, preview) != 0)
    {
      dt_print(DT_DEBUG_ALWAYS,
               "[imageio_dng] write_cfa_bayer: preview IFD0 failed, aborting");
      TIFFClose(tif);
      g_unlink(filename);
      return 1;
    }
    // re-register default tag info on some libtiff builds CFA/DNG
    // extension tags would be lost across the IFD write, breaking
    // CFAREPEATPATTERNDIM / CFAPATTERN. return value differs across
    // libtiff versions; not checked
    TIFFCreateDirectory(tif);
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
  TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_NONE);
  TIFFSetField(tif, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
  TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, TIFFDefaultStripSize(tif, 0));
  // shared metadata only on single-IFD layout — canonical has it on IFD0
  if(!canonical)
    _set_dng_shared_metadata(tif, img);

  // CFA description
  const uint16_t cfa_repeat_dim[2] = { 2, 2 };
  TIFFSetField(tif, TIFFTAG_CFAREPEATPATTERNDIM, cfa_repeat_dim);

  uint8_t cfa_pattern[4];
  _cfa_bytes_from_filters(img->buf_dsc.filters, cfa_pattern);
  TIFFSetField(tif, TIFFTAG_CFAPATTERN, 4, cfa_pattern);

  const uint8_t cfa_plane_color[3] = { 0, 1, 2 };   // R, G, B
  TIFFSetField(tif, TIFFTAG_CFAPLANECOLOR, 3, cfa_plane_color);
  TIFFSetField(tif, TIFFTAG_CFALAYOUT, (uint16_t)1); // rectangular

  // black/white levels
  // BlackLevel is declared as a 2x2 repeat over the CFA pattern. we
  // honor per-channel values when rawspeed provided them, otherwise
  // fall back to the single raw_black_level broadcast to all four
  const uint16_t bl_repeat_dim[2] = { 2, 2 };
  TIFFSetField(tif, TIFFTAG_BLACKLEVELREPEATDIM, bl_repeat_dim);

  float black_level[4];
  const gboolean have_separate
    = (img->raw_black_level_separate[0] != 0
       || img->raw_black_level_separate[1] != 0
       || img->raw_black_level_separate[2] != 0
       || img->raw_black_level_separate[3] != 0);
  for(int i = 0; i < 4; i++)
  {
    black_level[i] = have_separate
      ? (float)img->raw_black_level_separate[i]
      : (float)img->raw_black_level;
  }
  TIFFSetField(tif, TIFFTAG_BLACKLEVEL, 4, black_level);

  const uint32_t white = img->raw_white_point
    ? img->raw_white_point : 65535u;
  TIFFSetField(tif, TIFFTAG_WHITELEVEL, 1, &white);

  // advertise the visible region inside the full raw buffer; without
  // these tags the importer renders the optical-black margins too
  const int crop_x = (img->crop_x > 0) ? img->crop_x : 0;
  const int crop_y = (img->crop_y > 0) ? img->crop_y : 0;
  const int vis_w  = (img->p_width  > 0 && img->p_width  <= width  - crop_x)
                     ? img->p_width  : (width  - crop_x);
  const int vis_h  = (img->p_height > 0 && img->p_height <= height - crop_y)
                     ? img->p_height : (height - crop_y);

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

  // scanline write
  int res = 0;
  for(int y = 0; y < height && res == 0; y++)
  {
    const uint16_t *row = cfa + (size_t)y * width;
    if(TIFFWriteScanline(tif, (void *)row, y, 0) < 0)
      res = 1;
  }

  TIFFClose(tif);

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
                                const dt_imageio_dng_preview_t *preview)
{
  if(!filename || !rgb || !img || width <= 0 || height <= 0)
    return 1;

  _install_dng_tiff_handlers();

#ifdef _WIN32
  wchar_t *wfilename = g_utf8_to_utf16(filename, -1, NULL, NULL, NULL);
  TIFF *tif = TIFFOpenW(wfilename, "wl");
  g_free(wfilename);
#else
  TIFF *tif = TIFFOpen(filename, "wl");
#endif
  if(!tif) return 1;

  // canonical layout when a preview is provided (see write_cfa_bayer)
  const gboolean canonical = (preview && preview->data && preview->len > 0
                              && preview->width > 0 && preview->height > 0);
  if(canonical)
  {
    if(_write_jpeg_preview_ifd(tif, img, preview) != 0)
    {
      dt_print(DT_DEBUG_ALWAYS,
               "[imageio_dng] write_linear: preview IFD0 failed, aborting");
      TIFFClose(tif);
      g_unlink(filename);
      return 1;
    }
    // re-initialize directory state so DNG extension tag info is
    // available on the SubIFD (see comment in write_cfa_bayer)
    TIFFCreateDirectory(tif);
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
  TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_NONE);
  TIFFSetField(tif, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
  TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, TIFFDefaultStripSize(tif, 0));
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

  // scanline write: float [0, 1] normalized camRGB -> uint16
  //     [0, 65535]. BlackLevel=0 and WhiteLevel=65535 let the
  //     re-importer recover the [0, 1] range via the standard raw
  //     normalization (val - black) / (white - black)
  const float clip_hi = 65535.0f;
  uint16_t *scan = g_malloc((size_t)width * 3 * sizeof(uint16_t));
  int res = 0;
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

  TIFFClose(tif);

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
